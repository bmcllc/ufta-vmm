/*
 * ═══════════════════════════════════════════════════════════════
 *  pack-payload — Empacota arquivos em formato binário
 *
 *  Utilitário de build que gera o payload para o instalador.
 *  Formato: para cada arquivo:
 *    [4 bytes: tamanho do nome][nome][4 bytes: tamanho][dados]
 *  Finalizado por: [4 bytes: 0][4 bytes: 0]
 *
 *  Uso:
 *    pack-payload.exe output.bin file1 file2 ...
 *
 *  Compilação (MinGW-w64):
 *    gcc -O2 -o pack-payload.exe pack-payload.c
 * ═══════════════════════════════════════════════════════════════
 */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma pack(push, 1)
typedef struct {
    unsigned int name_len;
    unsigned int data_len;
} payload_entry_t;
#pragma pack(pop)

int main(int argc, char **argv)
{
    FILE *out;
    int i;

    if (argc < 3) {
        fprintf(stderr, "Uso: %s <output.bin> <file1> [file2] ...\n", argv[0]);
        return 1;
    }

    out = fopen(argv[1], "wb");
    if (!out) {
        fprintf(stderr, "ERRO: não foi possível criar %s\n", argv[1]);
        return 1;
    }

    for (i = 2; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        long size;
        char *data;
        const char *name;
        payload_entry_t entry;

        if (!f) {
            fprintf(stderr, "ERRO: não foi possível abrir %s\n", argv[i]);
            fclose(out);
            return 1;
        }

        /* Extrai apenas o nome do arquivo (sem caminho) */
        name = strrchr(argv[i], '\\');
        if (!name) name = strrchr(argv[i], '/');
        if (name) name++; else name = argv[i];

        /* Lê o arquivo */
        fseek(f, 0, SEEK_END);
        size = ftell(f);
        fseek(f, 0, SEEK_SET);

        data = (char *)malloc(size);
        if (!data && size > 0) {
            fprintf(stderr, "ERRO: sem memória para %s\n", argv[i]);
            fclose(f);
            fclose(out);
            return 1;
        }
        if (fread(data, 1, size, f) != (size_t)size) {
            fprintf(stderr, "ERRO: falha ao ler %s\n", argv[i]);
            free(data);
            fclose(f);
            fclose(out);
            return 1;
        }
        fclose(f);

        /* Escreve a entrada no payload */
        entry.name_len = (unsigned int)strlen(name);
        entry.data_len = (unsigned int)size;

        fwrite(&entry, sizeof(entry), 1, out);
        fwrite(name, 1, entry.name_len, out);
        fwrite(data, 1, entry.data_len, out);

        printf("  + %-30s (%ld bytes)\n", name, size);
        free(data);
    }

    /* Marca de fim */
    {
        payload_entry_t end = { 0, 0 };
        fwrite(&end, sizeof(end), 1, out);
    }

    fclose(out);
    printf("\n  Payload: %s (%d arquivos)\n", argv[1], argc - 2);
    return 0;
}
