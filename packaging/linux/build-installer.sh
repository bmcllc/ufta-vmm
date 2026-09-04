#!/usr/bin/env bash
# ═══════════════════════════════════════════════════════════════
#  UFTA-VMM — Self-Extracting Installer Builder
#  Gera um instalador .run autossuficiente que contém os binários
#  pré-compilados e as bibliotecas SDL2/OpenGL embarcadas.
#
#  O instalador resultante NÃO requer compilação nem código-fonte.
#  O usuário final apenas executa:
#      ./ufta-vmm-<ver>-linux-x86_64.run
#
#  Uso:  ./packaging/linux/build-installer.sh [--version X.Y.Z]
# ═══════════════════════════════════════════════════════════════
set -euo pipefail

# ── Config ──────────────────────────────────────────────────────
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
VERSION="1.0.0"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --version) VERSION="$2"; shift 2 ;;
        *) VERSION="$1"; shift ;;
    esac
done

ARCH="x86_64"
TEMPLATE="$ROOT/packaging/linux/installer-template.sh"
STAGE="$ROOT/build/installer-stage"
PAYLOAD="$ROOT/build/installer-payload.tar.gz"
OUT="$ROOT/dist/ufta-vmm-${VERSION}-linux-${ARCH}.run"

echo "═══════════════════════════════════════════════════════════"
echo "  UFTA-VMM Self-Extracting Installer Builder"
echo "  Version: $VERSION | Arch: $ARCH"
echo "═══════════════════════════════════════════════════════════"

# ── 1. Build binaries ──────────────────────────────────────────
echo ""
echo "[1/5] Building binaries..."
make -C "$ROOT" clean >/dev/null 2>&1 || true
make -C "$ROOT" >/dev/null
make -C "$ROOT" gui >/dev/null

# ── 2. Create staging directory ────────────────────────────────
echo "[2/5] Creating staging directory..."
rm -rf "$STAGE" "$ROOT/dist"
mkdir -p "$STAGE/bin"
mkdir -p "$STAGE/lib"
mkdir -p "$STAGE/share/doc/ufta-vmm"
mkdir -p "$ROOT/dist"

# ── 3. Copy binaries ───────────────────────────────────────────
echo "[3/5] Copying binaries..."
cp "$ROOT/uvm"        "$STAGE/bin/uvm"
cp "$ROOT/uvm-gui"    "$STAGE/bin/uvm-gui"

# ── 4. Embed SDL2/OpenGL dynamic libraries ─────────────────────
echo "[4/5] Embedding SDL2/OpenGL dynamic libraries..."

# Função para localizar uma biblioteca
find_lib() {
    local name="$1"
    local lib
    lib="$(ldconfig -p 2>/dev/null | grep "$name" | awk '{print $NF}' | head -1)"
    if [ -z "$lib" ]; then
        for p in /usr/lib/x86_64-linux-gnu /usr/lib /lib/x86_64-linux-gnu /lib /usr/local/lib; do
            [ -f "$p/$name" ] && lib="$p/$name" && break
        done
    fi
    echo "$lib"
}

# SDL2
SDL2_LIB="$(find_lib 'libSDL2-2.0.so.0')"
if [ -n "$SDL2_LIB" ] && [ -f "$SDL2_LIB" ]; then
    cp -L "$SDL2_LIB" "$STAGE/lib/"
    echo "  ✓ libSDL2-2.0.so.0 → $STAGE/lib/"
else
    echo "  ⚠  SDL2 não encontrada. O GUI usará a do sistema."
fi

# OpenGL
GL_LIB="$(find_lib 'libGL.so.1')"
if [ -n "$GL_LIB" ] && [ -f "$GL_LIB" ]; then
    cp -L "$GL_LIB" "$STAGE/lib/" 2>/dev/null || true
    echo "  ✓ libGL.so.1 → $STAGE/lib/"
fi

# ── 5. Copy docs & build payload ───────────────────────────────
echo "[5/5] Copying docs and building payload..."

cp "$ROOT/README.md" "$STAGE/share/doc/ufta-vmm/README.md"

# Cria o payload tar.gz
tar -czf "$PAYLOAD" -C "$STAGE" .

# ── Gera o instalador .run ─────────────────────────────────────
echo ""
echo "Gerando instalador self-extracting..."

# Substitui a versão no template
sed "s/__VERSION__/$VERSION/g" "$TEMPLATE" > "$OUT.tmp"

# Anexa o payload ao instalador
cat "$PAYLOAD" >> "$OUT.tmp"

# Torna executável
chmod +x "$OUT.tmp"
mv "$OUT.tmp" "$OUT"

# Limpa
rm -rf "$STAGE" "$PAYLOAD"

# ── Verifica o instalador ──────────────────────────────────────
echo ""
echo "Verificando instalador..."
if head -1 "$OUT" | grep -q '#!/usr/bin/env bash'; then
    echo "  ✓ Shebang OK"
else
    echo "  ✗ Shebang inválido"
    exit 1
fi

if grep -q '__PAYLOAD_BELOW__' "$OUT"; then
    echo "  ✓ Payload marker OK"
else
    echo "  ✗ Payload marker ausente"
    exit 1
fi

SIZE="$(du -h "$OUT" | cut -f1)"
echo "  ✓ Tamanho: $SIZE"

echo ""
echo "═══════════════════════════════════════════════════════════"
echo "  ✓ Instalador criado: $OUT"
echo ""
echo "  Distribua este arquivo. O usuário final apenas faz:"
echo ""
echo "    chmod +x ufta-vmm-${VERSION}-linux-${ARCH}.run"
echo "    ./ufta-vmm-${VERSION}-linux-${ARCH}.run"
echo ""
echo "  Opções do instalador:"
echo "    --prefix <dir>     instala em outro diretório"
echo "    --uninstall        remove a instalação"
echo "    --help             mostra ajuda"
echo "═══════════════════════════════════════════════════════════"
