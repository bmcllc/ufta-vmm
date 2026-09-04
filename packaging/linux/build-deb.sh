#!/usr/bin/env bash
# ═══════════════════════════════════════════════════════════════
#  UFTA-VMM — Debian/Ubuntu .deb Package Builder
#  Creates a native .deb installer with proper system integration.
#
#  Usage:  ./packaging/linux/build-deb.sh [--version X.Y.Z]
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
ARCH="amd64"
PKG_NAME="ufta-vmm_${VERSION}_${ARCH}"
STAGE="$ROOT/build/deb/$PKG_NAME"
OUT="$ROOT/dist/$PKG_NAME.deb"

echo "═══════════════════════════════════════════════════════════"
echo "  UFTA-VMM Debian Package Builder"
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
mkdir -p "$STAGE/DEBIAN"
mkdir -p "$STAGE/usr/bin"
mkdir -p "$STAGE/usr/share/doc/ufta-vmm"
mkdir -p "$STAGE/usr/share/applications"
mkdir -p "$STAGE/usr/share/icons/hicolor/128x128/apps"

# ── 3. Copy binaries ───────────────────────────────────────────
echo "[3/5] Copying binaries..."
cp "$ROOT/uvm"        "$STAGE/usr/bin/uvm"
cp "$ROOT/uvm-gui"    "$STAGE/usr/bin/uvm-gui"

# ── 4. Create control file ─────────────────────────────────────
echo "[4/5] Creating control file..."
cat > "$STAGE/DEBIAN/control" <<CONTROL
Package: ufta-vmm
Version: $VERSION
Section: utils
Priority: optional
Architecture: $ARCH
Depends: libc6 (>= 2.17), libsdl2-2.0-0 (>= 2.0.0), libgl1
Maintainer: UFTA-VMM Team <dev@ufta-vmm.org>
Description: Universal Field Transformation Architecture — Virtual Memory Manager
 A field-driven virtual memory manager implementing hierarchical memory
 tier migration using mathematical state vectors, adaptive prediction,
 and cost/gain analysis.
 .
 Features:
  * 7-step pipeline (Observe→Represent→Predict→Prioritize→Throttle→Migrate→Commit)
  * LMS predictor with rotation matrices
  * Tier-based memory hierarchy (RAM/VRAM/NVMe/USB/FILE)
  * Transparent page fault migration via SIGSEGV
  * Real-time SDL2 + OpenGL dashboard
CONTROL

# ── 5. Copy docs & desktop integration ─────────────────────────
echo "[5/5] Copying documentation and desktop integration..."

cp "$ROOT/README.md" "$STAGE/usr/share/doc/ufta-vmm/README.md"
gzip -9 -c "$ROOT/README.md" > "$STAGE/usr/share/doc/ufta-vmm/README.md.gz" 2>/dev/null || true
rm -f "$STAGE/usr/share/doc/ufta-vmm/README.md"

cat > "$STAGE/usr/share/applications/ufta-vmm.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=UFTA-VMM
Comment=Universal Field Transformation Architecture — Virtual Memory Manager
Exec=uvm-gui gui
Icon=ufta-vmm
Terminal=false
Categories=System;Utility;
Keywords=memory;virtual;manager;gpu;
DESKTOP

# Create a simple icon (SVG placeholder converted to PNG if possible)
cat > "$STAGE/usr/share/icons/hicolor/128x128/apps/ufta-vmm.png" <<'EOF'
iVBORw0KGgoAAAANSUhEUgAAAIAAAACACAYAAADDPmHLAAAA
EOF
# The above is a placeholder; generate a real icon if ImageMagick is available
if command -v convert >/dev/null 2>&1; then
    convert -size 128x128 xc:none \
        -fill '#1a1a2e' -draw 'roundrectangle 8,8 120,120 16,16' \
        -fill '#00d4ff' -draw 'circle 64,64 64,20' \
        -fill '#ffffff' -draw 'circle 64,64 64,44' \
        -fill '#1a1a2e' -draw 'circle 64,64 64,56' \
        "$STAGE/usr/share/icons/hicolor/128x128/apps/ufta-vmm.png" 2>/dev/null || true
fi

# ── Build .deb ─────────────────────────────────────────────────
echo ""
echo "Building .deb package..."
chmod 755 "$STAGE/DEBIAN"
chmod 755 "$STAGE/usr/bin/uvm" "$STAGE/usr/bin/uvm-gui"

mkdir -p "$ROOT/dist"
dpkg-deb --build --root-owner-group "$STAGE" "$OUT"

echo ""
echo "═══════════════════════════════════════════════════════════"
echo "  ✓ Package created: $OUT"
echo ""
echo "  Install:"
echo "    sudo dpkg -i $PKG_NAME.deb"
echo "    sudo apt-get install -f   # (if deps need resolving)"
echo ""
echo "  Run:"
echo "    uvm-gui gui    # GUI dashboard"
echo "    uvm validate   # CLI validation"
echo "═══════════════════════════════════════════════════════════"
