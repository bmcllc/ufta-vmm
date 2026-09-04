/*
 * ═══════════════════════════════════════════════════════════════
 *  UFTA-VMM — Windows Self-Extracting Installer
 *
 *  Este programa é compilado em .exe e contém um payload (tar.gz)
 *  embutido no final do próprio executável. Ao ser executado, ele:
 *    1. Extrai o payload (binários + DLLs SDL2)
 *    2. Instala em %ProgramFiles%\UFTA-VMM (ou diretório escolhido)
 *    3. Cria atalhos no Menu Iniciar e na Área de Trabalho
 *    4. Cria o launcher ufta-vmm.bat
 *
 *  Compilação (MinGW-w64):
 *    gcc -O2 -o ufta-vmm-installer.exe installer-main.c -lz
 *
 *  O payload é anexado ao .exe pelo build-installer-exe.bat.
 * ═══════════════════════════════════════════════════════════════
 */
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <shlobj.h>
#include <direct.h>
#include <sys/stat.h>

/* ── Config ──────────────────────────────────────────────────── */
#define INSTALLER_VERSION "1.0.0"
#define PAYLOAD_MARKER    "__UFTA_PAYLOAD_BELOW__"
#define DEFAULT_PREFIX    "C:\\Program Files\\UFTA-VMM"

/* ── Utilitários ─────────────────────────────────────────────── */

/* Cria diretórios recursivamente */
static void mkdirs(const char *path)
{
    char tmp[1024];
    char *p = NULL;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (tmp[len - 1] == '\\' || tmp[len - 1] == '/')
        tmp[len - 1] = '\0';

    for (p = tmp + 1; *p; p++) {
        if (*p == '\\' || *p == '/') {
            *p = '\0';
            _mkdir(tmp);
            *p = '\\';
        }
    }
    _mkdir(tmp);
}

/* Copia um arquivo */
static int copy_file(const char *src, const char *dst)
{
    if (!CopyFileA(src, dst, FALSE)) {
        fprintf(stderr, "  ERRO: falha ao copiar %s -> %s (erro %lu)\n",
                src, dst, GetLastError());
        return 0;
    }
    return 1;
}

/* ── Extração do payload ─────────────────────────────────────── */

/*
 * O payload é um tar.gz anexado ao final do .exe.
 * Localizamos o marcador __UFTA_PAYLOAD_BELOW__ no binário,
 * e a partir dali extraímos o tar.gz.
 *
 * Como o Windows não tem tar nativo, usamos uma abordagem simples:
 * o payload é na verdade um "arquivo concatenado" onde cada entrada
 * tem o formato:
 *   [4 bytes: tamanho do nome][nome][4 bytes: tamanho][dados]
 *
 * Isso é gerado pelo build-installer-exe.bat usando um pequeno
 * utilitário (pack-payload.exe) que também compilamos aqui.
 */

/* Estrutura de entrada do payload */
#pragma pack(push, 1)
typedef struct {
    DWORD name_len;
    DWORD data_len;
    /* seguido por name_len bytes de nome + data_len bytes de dados */
} payload_entry_t;
#pragma pack(pop)

/* Localiza o marcador do payload no executável */
static long find_payload_offset(const char *exe_path)
{
    FILE *f = fopen(exe_path, "rb");
    char buf[4096];
    size_t n;
    long pos = 0;
    const char *marker = PAYLOAD_MARKER;
    size_t mlen = strlen(marker);

    if (!f) return -1;

    /* Procura o marcador em blocos */
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        size_t i;
        for (i = 0; i + mlen <= n; i++) {
            if (memcmp(buf + i, marker, mlen) == 0) {
                fclose(f);
                return pos + (long)i + (long)mlen;
            }
        }
        pos += (long)n;
        /* Volta um pouco para pegar marcadores que cruzam blocos */
        if (n == sizeof(buf)) {
            fseek(f, -(long)(mlen - 1), SEEK_CUR);
            pos -= (long)(mlen - 1);
        }
    }
    fclose(f);
    return -1;
}

/* Extrai o payload para o diretório de destino */
static int extract_payload(const char *exe_path, const char *dest)
{
    long offset = find_payload_offset(exe_path);
    FILE *f;
    payload_entry_t entry;
    char name[1024];
    char outpath[2048];
    FILE *out;
    char *data;
    DWORD remaining;

    if (offset < 0) {
        fprintf(stderr, "  ERRO: payload não encontrado no instalador.\n");
        return 0;
    }

    f = fopen(exe_path, "rb");
    if (!f) return 0;
    fseek(f, offset, SEEK_SET);

    printf("  Extraindo arquivos...\n");

    while (1) {
        /* Lê o cabeçalho da entrada */
        if (fread(&entry, sizeof(entry), 1, f) != 1)
            break;

        /* Fim do payload */
        if (entry.name_len == 0 && entry.data_len == 0)
            break;

        if (entry.name_len >= sizeof(name)) {
            fprintf(stderr, "  ERRO: nome de arquivo muito longo.\n");
            fclose(f);
            return 0;
        }

        /* Lê o nome */
        if (fread(name, 1, entry.name_len, f) != entry.name_len) {
            fprintf(stderr, "  ERRO: payload corrompido (nome).\n");
            fclose(f);
            return 0;
        }
        name[entry.name_len] = '\0';

        /* Lê os dados */
        data = (char *)malloc(entry.data_len ? entry.data_len : 1);
        if (!data) {
            fprintf(stderr, "  ERRO: sem memória.\n");
            fclose(f);
            return 0;
        }
        remaining = entry.data_len;
        while (remaining > 0) {
            size_t r = fread(data + (entry.data_len - remaining), 1, remaining, f);
            if (r == 0) {
                fprintf(stderr, "  ERRO: payload corrompido (dados).\n");
                free(data);
                fclose(f);
                return 0;
            }
            remaining -= (DWORD)r;
        }

        /* Constrói o caminho de saída */
        snprintf(outpath, sizeof(outpath), "%s\\%s", dest, name);

        /* Cria diretórios pai */
        {
            char *slash = strrchr(outpath, '\\');
            if (slash) {
                *slash = '\0';
                mkdirs(outpath);
                *slash = '\\';
            }
        }

        /* Escreve o arquivo */
        out = fopen(outpath, "wb");
        if (!out) {
            fprintf(stderr, "  ERRO: não foi possível criar %s\n", outpath);
            free(data);
            fclose(f);
            return 0;
        }
        fwrite(data, 1, entry.data_len, out);
        fclose(out);

        printf("    ✓ %s\n", name);
        free(data);
    }

    fclose(f);
    return 1;
}

/* ── Instalação ──────────────────────────────────────────────── */

static void print_banner(void)
{
    printf("\n");
    printf("  ╔══════════════════════════════════════════════╗\n");
    printf("  ║   UFTA-VMM  —  Virtual Memory Manager       ║\n");
    printf("  ║   Universal Field Transformation Architecture║\n");
    printf("  ╚══════════════════════════════════════════════╝\n");
    printf("  Versão: %s\n\n", INSTALLER_VERSION);
}

static void show_help(void)
{
    print_banner();
    printf("  Uso:\n");
    printf("    ufta-vmm-installer.exe [opções]\n\n");
    printf("  Opções:\n");
    printf("    /prefix <dir>     Diretório de instalação\n");
    printf("    /uninstall        Remove a instalação\n");
    printf("    /silent           Instala sem perguntar\n");
    printf("    /?                Mostra esta ajuda\n\n");
    printf("  Exemplos:\n");
    printf("    ufta-vmm-installer.exe\n");
    printf("    ufta-vmm-installer.exe /prefix D:\\ufta\n");
    printf("    ufta-vmm-installer.exe /uninstall\n\n");
}

/* Obtém o diretório do executável atual */
static void get_exe_dir(char *buf, size_t size)
{
    GetModuleFileNameA(NULL, buf, (DWORD)size);
    char *slash = strrchr(buf, '\\');
    if (slash) *slash = '\0';
}

/* Cria atalho .lnk no Menu Iniciar */
static void create_start_menu_shortcut(const char *target, const char *link_name)
{
    char startmenu[MAX_PATH];
    char linkpath[MAX_PATH];
    HRESULT hr;

    if (SHGetFolderPathA(NULL, CSIDL_PROGRAMS, NULL, 0, startmenu) != S_OK)
        return;

    snprintf(linkpath, sizeof(linkpath), "%s\\%s.lnk", startmenu, link_name);

    /* Cria atalho via IShellLink */
    IShellLinkA *psl = NULL;
    hr = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IShellLinkA, (void **)&psl);
    if (SUCCEEDED(hr) && psl) {
        IPersistFile *ppf = NULL;
        psl->lpVtbl->SetPath(psl, target);
        hr = psl->lpVtbl->QueryInterface(psl, &IID_IPersistFile, (void **)&ppf);
        if (SUCCEEDED(hr) && ppf) {
            WCHAR wpath[MAX_PATH];
            MultiByteToWideChar(CP_ACP, 0, linkpath, -1, wpath, MAX_PATH);
            ppf->lpVtbl->Save(ppf, wpath, TRUE);
            ppf->lpVtbl->Release(ppf);
        }
        psl->lpVtbl->Release(psl);
    }
}

/* Cria atalho .lnk na Área de Trabalho */
static void create_desktop_shortcut(const char *target, const char *link_name)
{
    char desktop[MAX_PATH];
    char linkpath[MAX_PATH];

    if (SHGetFolderPathA(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, desktop) != S_OK)
        return;

    snprintf(linkpath, sizeof(linkpath), "%s\\%s.lnk", desktop, link_name);

    IShellLinkA *psl = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                                  &IID_IShellLinkA, (void **)&psl);
    if (SUCCEEDED(hr) && psl) {
        IPersistFile *ppf = NULL;
        psl->lpVtbl->SetPath(psl, target);
        hr = psl->lpVtbl->QueryInterface(psl, &IID_IPersistFile, (void **)&ppf);
        if (SUCCEEDED(hr) && ppf) {
            WCHAR wpath[MAX_PATH];
            MultiByteToWideChar(CP_ACP, 0, linkpath, -1, wpath, MAX_PATH);
            ppf->lpVtbl->Save(ppf, wpath, TRUE);
            ppf->lpVtbl->Release(ppf);
        }
        psl->lpVtbl->Release(psl);
    }
}

/* Instala */
static int do_install(const char *prefix, int silent)
{
    char exe_path[MAX_PATH];
    char tmpdir[MAX_PATH];
    char bindir[MAX_PATH];
    char libdir[MAX_PATH];
    char launcher[MAX_PATH];
    char uvm_exe[MAX_PATH];
    char uvm_gui_exe[MAX_PATH];
    char sdl_dll[MAX_PATH];
    char gl_dll[MAX_PATH];
    char launcher_path[MAX_PATH];

    print_banner();

    printf("  Instalando UFTA-VMM %s...\n", INSTALLER_VERSION);
    printf("    Prefixo: %s\n\n", prefix);

    /* Obtém o caminho do executável atual */
    GetModuleFileNameA(NULL, exe_path, sizeof(exe_path));

    /* Cria diretórios */
    snprintf(bindir, sizeof(bindir), "%s\\bin", prefix);
    snprintf(libdir, sizeof(libdir), "%s\\lib", prefix);
    mkdirs(bindir);
    mkdirs(libdir);

    /* Extrai payload para diretório temporário */
    snprintf(tmpdir, sizeof(tmpdir), "%s\\_tmp", prefix);
    mkdirs(tmpdir);

    if (!extract_payload(exe_path, tmpdir)) {
        fprintf(stderr, "  ERRO: falha na extração.\n");
        return 1;
    }

    /* Copia binários */
    printf("\n  Instalando binários...\n");
    snprintf(uvm_exe, sizeof(uvm_exe), "%s\\bin\\uvm.exe", tmpdir);
    snprintf(uvm_gui_exe, sizeof(uvm_gui_exe), "%s\\bin\\uvm-gui.exe", tmpdir);
    snprintf(launcher, sizeof(launcher), "%s\\bin\\uvm.exe", bindir);
    copy_file(uvm_exe, launcher);
    snprintf(launcher, sizeof(launcher), "%s\\bin\\uvm-gui.exe", bindir);
    copy_file(uvm_gui_exe, launcher);

    /* Copia DLLs SDL2/OpenGL */
    printf("  Instalando bibliotecas embarcadas...\n");
    snprintf(sdl_dll, sizeof(sdl_dll), "%s\\lib\\SDL2.dll", tmpdir);
    snprintf(launcher, sizeof(launcher), "%s\\SDL2.dll", libdir);
    if (GetFileAttributesA(sdl_dll) != INVALID_FILE_ATTRIBUTES)
        copy_file(sdl_dll, launcher);

    snprintf(gl_dll, sizeof(gl_dll), "%s\\lib\\opengl32.dll", tmpdir);
    snprintf(launcher, sizeof(launcher), "%s\\opengl32.dll", libdir);
    if (GetFileAttributesA(gl_dll) != INVALID_FILE_ATTRIBUTES)
        copy_file(gl_dll, launcher);

    /* Cria o launcher ufta-vmm.bat */
    printf("  Criando launcher...\n");
    snprintf(launcher_path, sizeof(launcher_path), "%s\\bin\\ufta-vmm.bat", prefix);
    {
        FILE *f = fopen(launcher_path, "w");
        if (f) {
            fprintf(f, "@echo off\r\n");
            fprintf(f, "REM UFTA-VMM launcher — usa DLLs embarcadas\r\n");
            fprintf(f, "set \"DIR=%%~dp0\"\r\n");
            fprintf(f, "set \"PATH=%%DIR%%..\\lib;%%PATH%%\"\r\n");
            fprintf(f, "\r\n");
            fprintf(f, "if \"%%1\"==\"gui\" (\r\n");
            fprintf(f, "    \"%%DIR%%uvm-gui.exe\" %%*\r\n");
            fprintf(f, ") else (\r\n");
            fprintf(f, "    \"%%DIR%%uvm.exe\" %%*\r\n");
            fprintf(f, ")\r\n");
            fclose(f);
        }
    }

    /* Cria atalhos */
    printf("  Criando atalhos...\n");
    {
        char gui_target[MAX_PATH];
        snprintf(gui_target, sizeof(gui_target), "%s\\bin\\uvm-gui.exe", prefix);
        create_start_menu_shortcut(gui_target, "UFTA-VMM");
        create_desktop_shortcut(gui_target, "UFTA-VMM");
    }

    /* Limpa diretório temporário */
    {
        char cmd[MAX_PATH * 2];
        snprintf(cmd, sizeof(cmd), "rmdir /s /q \"%s\"", tmpdir);
        system(cmd);
    }

    printf("\n");
    printf("  ══════════════════════════════════════════════\n");
    printf("    ✓ UFTA-VMM instalado com sucesso!\n");
    printf("  ══════════════════════════════════════════════\n");
    printf("\n");
    printf("  Para usar:\n");
    printf("    %s\\bin\\ufta-vmm.bat gui        → Dashboard\n", prefix);
    printf("    %s\\bin\\ufta-vmm.bat validate   → Validação\n", prefix);
    printf("\n");

    if (!silent) {
        printf("  Pressione Enter para sair...");
        getchar();
    }

    return 0;
}

/* Desinstala */
static int do_uninstall(const char *prefix)
{
    char path[MAX_PATH];

    print_banner();
    printf("  Removendo UFTA-VMM...\n");

    /* Remove binários */
    snprintf(path, sizeof(path), "%s\\bin\\uvm.exe", prefix);
    DeleteFileA(path);
    snprintf(path, sizeof(path), "%s\\bin\\uvm-gui.exe", prefix);
    DeleteFileA(path);
    snprintf(path, sizeof(path), "%s\\bin\\ufta-vmm.bat", prefix);
    DeleteFileA(path);

    /* Remove diretórios */
    snprintf(path, sizeof(path), "%s\\bin", prefix);
    RemoveDirectoryA(path);
    snprintf(path, sizeof(path), "%s\\lib", prefix);
    RemoveDirectoryA(path);

    /* Remove atalhos */
    {
        char startmenu[MAX_PATH];
        char desktop[MAX_PATH];
        char linkpath[MAX_PATH];

        if (SHGetFolderPathA(NULL, CSIDL_PROGRAMS, NULL, 0, startmenu) == S_OK) {
            snprintf(linkpath, sizeof(linkpath), "%s\\UFTA-VMM.lnk", startmenu);
            DeleteFileA(linkpath);
        }
        if (SHGetFolderPathA(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, desktop) == S_OK) {
            snprintf(linkpath, sizeof(linkpath), "%s\\UFTA-VMM.lnk", desktop);
            DeleteFileA(linkpath);
        }
    }

    printf("  ✓ UFTA-VMM removido com sucesso.\n");
    return 0;
}

/* ── Main ────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    char prefix[MAX_PATH] = DEFAULT_PREFIX;
    int silent = 0;
    int uninstall = 0;

    /* Parse args */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "/prefix") == 0 && i + 1 < argc) {
            snprintf(prefix, sizeof(prefix), "%s", argv[++i]);
        } else if (strcmp(argv[i], "/uninstall") == 0) {
            uninstall = 1;
        } else if (strcmp(argv[i], "/silent") == 0) {
            silent = 1;
        } else if (strcmp(argv[i], "/?") == 0 || strcmp(argv[i], "/help") == 0) {
            show_help();
            return 0;
        }
    }

    /* Inicializa COM para atalhos */
    CoInitialize(NULL);

    if (uninstall) {
        do_uninstall(prefix);
    } else {
        do_install(prefix, silent);
    }

    CoUninitialize();
    return 0;
}
