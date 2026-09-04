#!/bin/bash
# ═══════════════════════════════════════════════════════════════
#  UFTA-VMM — Windows Package Builder (bash)
#
#  Compila os binários Windows com MinGW-w64 e gera o ZIP portátil
#  para o itch.io. Funciona tanto no GitHub Actions quanto localmente
#  (com MinGW-w64 instalado).
#
#  Uso:  bash packaging/windows/build-windows.sh [versão]
# ═══════════════════════════════════════════════════════════════
set -e

VERSION="${1:-1.0.0}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PKG_NAME="ufta-vmm-${VERSION}-windows-x86_64"
STAGE="$ROOT/build/package/${PKG_NAME}"
OUT="$ROOT/dist/${PKG_NAME}.zip"

echo "═══════════════════════════════════════════════════════"
echo "  UFTA-VMM — Windows Package Builder"
echo "  Versão: $VERSION"
echo "═══════════════════════════════════════════════════════"
echo

# ── 1. Verifica MinGW ──────────────────────────────────────
echo "[1/5] Verificando MinGW-w64..."
if ! command -v gcc >/dev/null 2>&1; then
    echo "  ERRO: gcc (MinGW-w64) não encontrado no PATH."
    exit 1
fi
echo "  ✓ gcc: $(gcc --version | head -1)"

# ── 2. Localiza SDL2 ───────────────────────────────────────
echo "[2/5] Localizando SDL2..."
SDL2_DIR=""
if [ -d "$ROOT/third_party/SDL2/include/SDL2" ]; then
    SDL2_DIR="$ROOT/third_party/SDL2"
elif [ -n "$SDL2DIR" ] && [ -d "$SDL2DIR/include/SDL2" ]; then
    SDL2_DIR="$SDL2DIR"
fi

if [ -z "$SDL2_DIR" ]; then
    echo "  ERRO: SDL2 não encontrado."
    echo "  Baixe SDL2-devel-mingw.zip de:"
    echo "    https://github.com/libsdl-org/SDL/releases"
    echo "  Extraia para: $ROOT/third_party/SDL2"
    exit 1
fi
echo "  ✓ SDL2 em: $SDL2_DIR"

# ── 3. Compila binários ────────────────────────────────────
echo "[3/5] Compilando binários..."

# uvm.exe (CLI)
echo "  Compilando uvm.exe..."
gcc -Wall -Wextra -O2 -std=c11 -D_GNU_SOURCE -Iinclude \
    src/backend.c src/field.c src/main.c src/migrate.c \
    src/page.c src/pagefault.c src/pipeline.c src/predictor.c \
    src/state.c src/tier.c src/validate.c \
    -o uvm.exe -lm

# uvm-gui.exe (GUI com SDL2)
echo "  Compilando uvm-gui.exe..."
gcc -Wall -Wextra -O2 -std=c11 -D_GNU_SOURCE -Iinclude \
    -I"$SDL2_DIR/include" \
    src/backend.c src/field.c src/migrate.c src/page.c \
    src/pagefault.c src/pipeline.c src/predictor.c src/state.c \
    src/tier.c src/validate.c src/gui.c \
    -o uvm-gui.exe -lm \
    -L"$SDL2_DIR/lib/x64" -lmingw32 -lSDL2main -lSDL2 -lopengl32 -mwindows

echo "  ✓ uvm.exe e uvm-gui.exe compilados"

# ── 4. Monta o pacote ──────────────────────────────────────
echo "[4/5] Montando pacote..."
rm -rf "$STAGE"
mkdir -p "$STAGE/bin" "$STAGE/lib" "$STAGE/docs"

cp "$ROOT/uvm.exe" "$STAGE/bin/"
cp "$ROOT/uvm-gui.exe" "$STAGE/bin/"

# SDL2.dll
if [ -f "$SDL2_DIR/lib/x64/SDL2.dll" ]; then
    cp "$SDL2_DIR/lib/x64/SDL2.dll" "$STAGE/lib/"
    echo "  ✓ SDL2.dll (x64) copiada"
elif [ -f "$SDL2_DIR/lib/SDL2.dll" ]; then
    cp "$SDL2_DIR/lib/SDL2.dll" "$STAGE/lib/"
    echo "  ✓ SDL2.dll copiada"
fi

# Launcher
cat > "$STAGE/bin/ufta-vmm.bat" <<'EOF'
@echo off
REM UFTA-VMM launcher — usa DLLs SDL2 embarcadas
set "DIR=%~dp0"
set "PATH=%DIR%..\lib;%PATH%"
if "%1"=="gui" (
    "%DIR%uvm-gui.exe" %*
) else (
    "%DIR%uvm.exe" %*
)
EOF

# README
if [ -f "$ROOT/packaging/itch/README-itch.txt" ]; then
    cp "$ROOT/packaging/itch/README-itch.txt" "$STAGE/docs/README.txt"
elif [ -f "$ROOT/README.md" ]; then
    cp "$ROOT/README.md" "$STAGE/docs/README.md"
fi

# ── 5. Empacota ZIP ────────────────────────────────────────
echo "[5/5] Gerando ZIP..."
mkdir -p "$ROOT/dist"
rm -f "$OUT"

# Usa PowerShell no Windows, zip no Linux
if command -v powershell >/dev/null 2>&1; then
    (cd "$ROOT/build/package" && powershell -Command \
        "Compress-Archive -Path '${PKG_NAME}' -DestinationPath '${OUT}' -Force")
else
    (cd "$ROOT/build/package" && zip -r "$OUT" "$PKG_NAME" >/dev/null)
fi

echo
echo "  ✓ Pacote gerado: $OUT"
echo "    ($(du -h "$OUT" | cut -f1))"
echo
echo "  Conteúdo:"
echo "    bin/uvm.exe          — CLI"
echo "    bin/uvm-gui.exe      — GUI dashboard"
echo "    bin/ufta-vmm.bat     — launcher"
echo "    lib/SDL2.dll         — SDL2 embutido"
echo "    docs/README.txt      — documentação"
echo

# Limpa binários temporários
rm -f "$ROOT/uvm.exe" "$ROOT/uvm-gui.exe"
