#!/usr/bin/env bash
# ═══════════════════════════════════════════════════════════════
#  UFTA-VMM — Portable Linux Package Builder
#  Creates a self-contained .tar.gz with embedded SDL2/OpenGL
#  dynamic libraries so the GUI runs on systems without SDL2.
#
#  Usage:  ./packaging/linux/build-portable.sh [--version X.Y.Z]
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
PKG_NAME="ufta-vmm-${VERSION}-linux-x86_64"
STAGE="$ROOT/build/package/$PKG_NAME"
OUT="$ROOT/dist/$PKG_NAME.tar.gz"

echo "═══════════════════════════════════════════════════════════"
echo "  UFTA-VMM Portable Package Builder"
echo "  Version: $VERSION"
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
mkdir -p "$STAGE/share/applications"

# ── 3. Copy binaries ───────────────────────────────────────────
echo "[3/5] Copying binaries..."
cp "$ROOT/uvm"        "$STAGE/bin/uvm"
cp "$ROOT/uvm-gui"    "$STAGE/bin/uvm-gui"

# ── 4. Embed SDL2 dynamic libraries ────────────────────────────
echo "[4/5] Embedding SDL2 dynamic libraries..."

# Find SDL2 shared library
SDL2_LIB="$(ldconfig -p 2>/dev/null | grep 'libSDL2-2.0.so.0' | awk '{print $NF}' | head -1)"
if [ -z "$SDL2_LIB" ]; then
    echo "  ⚠  libSDL2-2.0.so.0 not found via ldconfig, searching common paths..."
    for p in /usr/lib/x86_64-linux-gnu /usr/lib /lib/x86_64-linux-gnu /lib /usr/local/lib; do
        [ -f "$p/libSDL2-2.0.so.0" ] && SDL2_LIB="$p/libSDL2-2.0.so.0" && break
    done
fi

if [ -n "$SDL2_LIB" ] && [ -f "$SDL2_LIB" ]; then
    cp -L "$SDL2_LIB" "$STAGE/lib/"
    echo "  ✓ libSDL2-2.0.so.0 → $STAGE/lib/"
else
    echo "  ⚠  SDL2 library not found. The GUI will require system SDL2."
fi

# Also embed libGL if present (some minimal systems lack it)
GL_LIB="$(ldconfig -p 2>/dev/null | grep 'libGL.so.1' | awk '{print $NF}' | head -1)"
if [ -z "$GL_LIB" ]; then
    for p in /usr/lib/x86_64-linux-gnu /usr/lib /lib/x86_64-linux-gnu /lib /usr/local/lib; do
        [ -f "$p/libGL.so.1" ] && GL_LIB="$p/libGL.so.1" && break
    done
fi
if [ -n "$GL_LIB" ] && [ -f "$GL_LIB" ]; then
    cp -L "$GL_LIB" "$STAGE/lib/" 2>/dev/null || true
    echo "  ✓ libGL.so.1 → $STAGE/lib/"
fi

# ── 5. Copy docs & launcher ────────────────────────────────────
echo "[5/5] Copying documentation and launcher..."

cp "$ROOT/README.md" "$STAGE/share/doc/ufta-vmm/README.md"

# Create the launcher script (sets LD_LIBRARY_PATH to bundled libs)
cat > "$STAGE/bin/ufta-vmm" <<'LAUNCHER'
#!/usr/bin/env bash
# UFTA-VMM launcher — uses bundled SDL2/OpenGL libs if present
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export LD_LIBRARY_PATH="$DIR/../lib:${LD_LIBRARY_PATH:-}"

if [ "$1" = "gui" ]; then
    exec "$DIR/uvm-gui" "$@"
else
    exec "$DIR/uvm" "$@"
fi
LAUNCHER
chmod +x "$STAGE/bin/ufta-vmm"

# Create .desktop file for Linux desktop integration
cat > "$STAGE/share/applications/ufta-vmm.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=UFTA-VMM
Comment=Universal Field Transformation Architecture — Virtual Memory Manager
Exec=ufta-vmm gui
Icon=ufta-vmm
Terminal=false
Categories=System;Utility;
Keywords=memory;virtual;manager;gpu;
DESKTOP

# ── Package ────────────────────────────────────────────────────
echo ""
echo "Packaging..."
mkdir -p "$ROOT/dist"
tar -czf "$OUT" -C "$ROOT/build/package" "$PKG_NAME"

echo ""
echo "═══════════════════════════════════════════════════════════"
echo "  ✓ Package created: $OUT"
echo ""
echo "  Contents:"
echo "    bin/uvm          — CLI tool (no deps)"
echo "    bin/uvm-gui      — GUI dashboard"
echo "    bin/ufta-vmm     — launcher (uses bundled libs)"
echo "    lib/             — embedded SDL2/OpenGL .so files"
echo "    share/           — docs & desktop integration"
echo ""
echo "  Install:"
echo "    tar -xzf $PKG_NAME.tar.gz"
echo "    cd $PKG_NAME && ./bin/ufta-vmm gui"
echo "═══════════════════════════════════════════════════════════"
