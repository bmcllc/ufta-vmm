#!/bin/bash
# ═══════════════════════════════════════════════════════════════
#  UFTA-VMM — itch.io Package Builder
#
#  Gera os pacotes prontos para upload no itch.io:
#    - Linux:   dist/ufta-vmm-<ver>-linux-x86_64.zip  (portátil, SDL2 embutido)
#    - Windows: dist/ufta-vmm-<ver>-windows-x86_64.zip (gerado no Windows)
#
#  O pacote Windows deve ser gerado em uma máquina Windows rodando:
#    packaging/windows/build-windows.bat
#
#  Uso:  bash packaging/itch/build-itch.sh [versão]
# ═══════════════════════════════════════════════════════════════
set -e

VERSION="${1:-1.0.0}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
STAGE="$ROOT/build/itch-linux/ufta-vmm"
OUT="$ROOT/dist/ufta-vmm-$VERSION-linux-x86_64.zip"

echo "═══════════════════════════════════════════════════════"
echo "  UFTA-VMM — itch.io Package Builder"
echo "  Versão: $VERSION"
echo "═══════════════════════════════════════════════════════"
echo

# ── 1. Compila os binários ──────────────────────────────────
echo "[1/4] Compilando binários..."
make -C "$ROOT" clean >/dev/null 2>&1 || true
make -C "$ROOT" >/dev/null
make -C "$ROOT" gui >/dev/null
echo "  ✓ uvm e uvm-gui compilados"

# ── 2. Monta o stage ────────────────────────────────────────
echo "[2/4] Montando pacote..."
rm -rf "$STAGE"
mkdir -p "$STAGE/lib"

cp "$ROOT/uvm" "$STAGE/uvm"
cp "$ROOT/uvm-gui" "$STAGE/uvm-gui"
chmod +x "$STAGE/uvm" "$STAGE/uvm-gui"

# SDL2 embutido
SDL2_LIB="$(find /usr/lib -name 'libSDL2-2.0.so.0.*' 2>/dev/null | head -1)"
if [ -n "$SDL2_LIB" ]; then
    cp "$SDL2_LIB" "$STAGE/lib/libSDL2-2.0.so.0"
    echo "  ✓ SDL2 embutido: $(basename "$SDL2_LIB")"
else
    echo "  ⚠ SDL2 não encontrado — o GUI precisará do SDL2 do sistema"
fi

# Launcher
cat > "$STAGE/ufta-vmm.sh" <<'EOF'
#!/bin/bash
DIR="$(cd "$(dirname "$0")" && pwd)"
export LD_LIBRARY_PATH="$DIR/lib:$LD_LIBRARY_PATH"
if [ "$1" = "gui" ]; then
    shift
    exec "$DIR/uvm-gui" gui "$@"
else
    exec "$DIR/uvm" "$@"
fi
EOF
chmod +x "$STAGE/ufta-vmm.sh"

# README
cp "$ROOT/packaging/itch/README-itch.txt" "$STAGE/README.txt" 2>/dev/null || true

# ── 3. Empacota ZIP ─────────────────────────────────────────
echo "[3/4] Gerando ZIP..."
mkdir -p "$ROOT/dist"
rm -f "$OUT"
cd "$ROOT/build/itch-linux"
zip -r "$OUT" ufta-vmm/ >/dev/null
echo "  ✓ $OUT"

# ── 4. Resumo ───────────────────────────────────────────────
echo
echo "[4/4] Concluído!"
echo
echo "  ═══════════════════════════════════════════════════════"
echo "   Linux:  $OUT"
echo "           ($(du -h "$OUT" | cut -f1))"
echo
echo "   Windows: gere em uma máquina Windows:"
echo "     packaging/windows/build-windows.bat $VERSION"
echo "     → dist/ufta-vmm-$VERSION-windows-x86_64.zip"
echo "  ═══════════════════════════════════════════════════════"
echo
echo "  Upload no itch.io:"
echo "    - Linux:   ufta-vmm-$VERSION-linux-x86_64.zip"
echo "    - Windows: ufta-vmm-$VERSION-windows-x86_64.zip"
echo
