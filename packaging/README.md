# UFTA-VMM — Packaging & Release

This directory contains everything needed to build distributable
packages of UFTA-VMM for Linux and Windows.

## Quick Start

```bash
# Build the self-extracting installer (recommended for end users)
make installer

# Build all packages (installer + portable tar.gz + .deb)
make package

# Or run the individual scripts directly
./packaging/linux/build-installer.sh --version 1.0.0
./packaging/linux/build-portable.sh --version 1.0.0
./packaging/linux/build-deb.sh --version 1.0.0
```

## Package Types

### 1. Self-Extracting Installer (`ufta-vmm-<ver>-linux-x86_64.run`) ⭐ RECOMENDADO

Um **instalador único e autossuficiente** que contém os binários
pré-compilados e as bibliotecas SDL2/OpenGL embarcadas. O usuário
final **não precisa de código-fonte nem compilar nada** — apenas
executa o arquivo.

```
./ufta-vmm-1.0.0-linux-x86_64.run
```

**O que o instalador faz:**
- Verifica a arquitetura e dependências do sistema
- Extrai os binários pré-compilados (`uvm`, `uvm-gui`)
- Instala as bibliotecas SDL2/OpenGL embarcadas em `lib/ufta-vmm/`
- Cria o launcher `ufta-vmm` que usa as libs embarcadas
- Cria atalho de desktop (quando instalado em `/usr/local`)
- Instala a documentação

**Opções:**
```bash
./ufta-vmm-1.0.0-linux-x86_64.run                    # instala em /usr/local
./ufta-vmm-1.0.0-linux-x86_64.run --prefix ~/ufta    # instala em outro lugar
./ufta-vmm-1.0.0-linux-x86_64.run --uninstall        # remove a instalação
./ufta-vmm-1.0.0-linux-x86_64.run --help             # mostra ajuda
```

**Após instalar:**
```bash
ufta-vmm gui        # Dashboard em tempo real (SDL2+OpenGL)
ufta-vmm validate   # Validação de migração (CPU)
ufta-vmm help       # Lista todos os comandos
```

### 2. Portable Linux (`ufta-vmm-<ver>-linux-x86_64.tar.gz`)

Uma alternativa ao instalador — um arquivo `.tar.gz` que também
embute as bibliotecas SDL2/OpenGL, mas não requer instalação.

```
ufta-vmm-1.0.0-linux-x86_64/
├── bin/
│   ├── uvm          # CLI tool (no external deps)
│   ├── uvm-gui      # GUI dashboard
│   └── ufta-vmm     # launcher (sets LD_LIBRARY_PATH to bundled libs)
├── lib/
│   ├── libSDL2-2.0.so.0   # embedded SDL2 runtime
│   └── libGL.so.1         # embedded OpenGL runtime
└── share/
    ├── applications/ufta-vmm.desktop
    └── doc/ufta-vmm/README.md
```

**Install:**
```bash
tar -xzf ufta-vmm-1.0.0-linux-x86_64.tar.gz
cd ufta-vmm-1.0.0-linux-x86_64
./bin/ufta-vmm gui     # launch GUI
./bin/ufta-vmm validate # run CLI validation
```

### 3. Debian/Ubuntu (`ufta-vmm_<ver>_amd64.deb`)

Um instalador `.deb` nativo com integração de sistema (atalho de
desktop, resolução de dependências).

**Install:**
```bash
sudo dpkg -i ufta-vmm_1.0.0_amd64.deb
sudo apt-get install -f   # resolves libsdl2-2.0-0 if missing
```

**Run:**
```bash
uvm-gui gui    # GUI dashboard
uvm validate   # CLI validation
```

### 4. Windows (`ufta-vmm-<ver>-windows-x86_64.zip`)

Um `.zip` autossuficiente com a DLL SDL2 embarcada. Compilado com
MinGW-w64.

**Prerequisites (on Windows):**
1. Install [MinGW-w64](https://www.mingw-w64.org/)
2. Download [SDL2-devel-2.x.x-mingw.zip](https://github.com/libsdl-org/SDL/releases)
3. Extract SDL2 to `third_party/SDL2` (or set `SDL2DIR`)

**Build:**
```bat
packaging\windows\build-windows.bat 1.0.0
```

**Install:**
```bat
unzip ufta-vmm-1.0.0-windows-x86_64.zip
bin\ufta-vmm.bat gui
```

### 5. itch.io (`ufta-vmm-<ver>-linux-x86_64.zip` + Windows ZIP)

Pacotes **portáteis** prontos para upload no [itch.io](https://itch.io).
O comprador baixa, extrai e roda — sem instalar nada.

**Linux (gerado aqui):**
```bash
bash packaging/itch/build-itch.sh 1.0.0
# → dist/ufta-vmm-1.0.0-linux-x86_64.zip
```

**Windows (gerado em uma máquina Windows):**
```bat
packaging\windows\build-windows.bat 1.0.0
# → dist\ufta-vmm-1.0.0-windows-x86_64.zip
```

**OU automaticamente via GitHub Actions (recomendado):**
```bash
git tag v1.0.0
git push origin v1.0.0
```
O workflow `.github/workflows/build-release.yml` compila Linux + Windows
na nuvem e cria um Release com os dois ZIPs. Veja `.github/workflows/README.md`.

**Estrutura do pacote Linux:**
```
ufta-vmm/
├── ufta-vmm.sh          # launcher (usa SDL2 embutido)
├── uvm                  # CLI tool
├── uvm-gui              # GUI dashboard
├── README.txt
└── lib/
    └── libSDL2-2.0.so.0 # SDL2 embutido
```

**No itch.io:**
- Upload `ufta-vmm-1.0.0-linux-x86_64.zip` como **Linux**
- Upload `ufta-vmm-1.0.0-windows-x86_64.zip` como **Windows**
- O itch.io detecta automaticamente a plataforma do comprador

## Release Checklist

1. Bump version in `Makefile` / `packaging/*`
2. `make clean && make && make gui && make cuda`
3. Run `./uvm validate` and `./uvm-cuda validate-cuda`
4. `make installer` to build the self-extracting installer
5. Test the installer on a clean system (install + uninstall)
6. `bash packaging/itch/build-itch.sh <ver>` for itch.io Linux ZIP
7. Build Windows ZIP on a Windows machine (`build-windows.bat`)
8. Tag the release in git
