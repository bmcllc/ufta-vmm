#!/usr/bin/env bash
# ═══════════════════════════════════════════════════════════════
#  UFTA-VMM — Instalador
#  Universal Field Transformation Architecture — Virtual Memory Manager
#
#  Este instalador contém os binários pré-compilados e as
#  bibliotecas SDL2/OpenGL embarcadas. NÃO requer compilação.
#
#  Uso:
#    ./ufta-vmm-installer.run              # instala em /usr/local
#    ./ufta-vmm-installer.run --prefix ~/ufta-vmm   # instala em outro lugar
#    ./ufta-vmm-installer.run --uninstall   # remove a instalação
#    ./ufta-vmm-installer.run --help        # mostra ajuda
# ═══════════════════════════════════════════════════════════════
set -euo pipefail

# ── Versão embutida (substituída pelo build script) ────────────
INSTALLER_VERSION="__VERSION__"
ARCH="x86_64"

# ── Cores para output ──────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

# ── Banner ─────────────────────────────────────────────────────
banner() {
    echo -e "${CYAN}"
    echo "  ╔══════════════════════════════════════════════════╗"
    echo "  ║   UFTA-VMM  —  Virtual Memory Manager           ║"
    echo "  ║   Universal Field Transformation Architecture   ║"
    echo "  ╚══════════════════════════════════════════════════╝"
    echo -e "${NC}"
    echo -e "  ${BOLD}Versão:${NC} $INSTALLER_VERSION  |  Arquitetura: $ARCH"
    echo ""
}

# ── Ajuda ──────────────────────────────────────────────────────
show_help() {
    banner
    echo -e "  ${BOLD}Uso:${NC}"
    echo "    ./ufta-vmm-installer.run [opções]"
    echo ""
    echo -e "  ${BOLD}Opções:${NC}"
    echo "    --prefix <dir>     Diretório de instalação (padrão: /usr/local)"
    echo "    --uninstall        Remove a instalação"
    echo "    --version          Mostra a versão do instalador"
    echo "    --help             Mostra esta ajuda"
    echo ""
    echo -e "  ${BOLD}Exemplos:${NC}"
    echo "    ./ufta-vmm-installer.run"
    echo "    ./ufta-vmm-installer.run --prefix ~/ufta-vmm"
    echo "    ./ufta-vmm-installer.run --uninstall"
    echo ""
}

# ── Verifica arquitetura ───────────────────────────────────────
check_arch() {
    local host_arch
    host_arch="$(uname -m)"
    if [ "$host_arch" != "x86_64" ] && [ "$host_arch" != "amd64" ]; then
        echo -e "  ${RED}✗ Arquitetura incompatível.${NC}"
        echo -e "    Este instalador é para x86_64, mas o sistema é: $host_arch"
        exit 1
    fi
}

# ── Verifica dependências do sistema ───────────────────────────
check_deps() {
    echo -e "  ${BOLD}Verificando dependências do sistema...${NC}"

    # libc é sempre presente, mas verificamos libm
    if ! ldconfig -p 2>/dev/null | grep -q 'libm.so.6'; then
        if ! ls /lib/x86_64-linux-gnu/libm.so.6 >/dev/null 2>&1 && \
           ! ls /usr/lib/x86_64-linux-gnu/libm.so.6 >/dev/null 2>&1; then
            echo -e "    ${YELLOW}⚠ libm.so.6 não encontrada (raro)${NC}"
        fi
    fi

    # Verifica se há display para o GUI (não é bloqueante)
    if [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
        echo -e "    ${YELLOW}⚠ Nenhum display detectado. O GUI precisará de um display gráfico.${NC}"
    fi

    echo -e "    ${GREEN}✓ Dependências OK${NC}"
    return 0
}

# ── Extrai o payload (binários embarcados) ─────────────────────
extract_payload() {
    local dest="$1"
    local line

    # Encontra a linha de marcador do payload (trata como texto: -a)
    line=$(grep -an '^__PAYLOAD_BELOW__$' "$0" | tail -1 | cut -d: -f1)
    if [ -z "$line" ]; then
        echo -e "  ${RED}✗ Instalador corrompido (payload não encontrado).${NC}"
        exit 1
    fi

    # Extrai o tar.gz a partir da linha seguinte ao marcador
    tail -n +$((line + 1)) "$0" | tar -xzf - -C "$dest"
}

# ── Instala ────────────────────────────────────────────────────
TMPDIR_CLEANUP=""
cleanup_tmp() {
    [ -n "$TMPDIR_CLEANUP" ] && rm -rf "$TMPDIR_CLEANUP"
}

do_install() {
    local prefix="$1"
    local bindir="$prefix/bin"
    local libdir="$prefix/lib/ufta-vmm"
    local sharedir="$prefix/share/ufta-vmm"
    local tmpdir

    banner
    check_arch
    check_deps

    echo ""
    echo -e "  ${BOLD}Instalando UFTA-VMM $INSTALLER_VERSION...${NC}"
    echo -e "    Prefixo: ${CYAN}$prefix${NC}"

    # Cria diretórios
    mkdir -p "$bindir" "$libdir" "$sharedir"

    # Extrai payload para diretório temporário
    tmpdir="$(mktemp -d)"
    TMPDIR_CLEANUP="$tmpdir"
    trap cleanup_tmp EXIT

    echo -e "  ${BOLD}[1/4]${NC} Extraindo arquivos..."
    extract_payload "$tmpdir"

    # Copia binários
    echo -e "  ${BOLD}[2/4]${NC} Instalando binários..."
    cp "$tmpdir/bin/uvm"      "$bindir/uvm"
    cp "$tmpdir/bin/uvm-gui"  "$bindir/uvm-gui"
    chmod 755 "$bindir/uvm" "$bindir/uvm-gui"

    # Copia bibliotecas embarcadas (SDL2/OpenGL)
    echo -e "  ${BOLD}[3/4]${NC} Instalando bibliotecas embarcadas..."
    if [ -d "$tmpdir/lib" ] && [ -n "$(ls -A "$tmpdir/lib" 2>/dev/null)" ]; then
        cp -r "$tmpdir/lib/." "$libdir/"
        echo -e "    ${GREEN}✓ SDL2/OpenGL embarcadas em $libdir${NC}"
    else
        echo -e "    ${YELLOW}⚠ Nenhuma biblioteca embarcada. O GUI usará as do sistema.${NC}"
    fi

    # Copia documentação
    echo -e "  ${BOLD}[4/4]${NC} Instalando documentação..."
    if [ -f "$tmpdir/share/doc/ufta-vmm/README.md" ]; then
        cp "$tmpdir/share/doc/ufta-vmm/README.md" "$sharedir/README.md"
    fi

    # Cria o launcher (usa as libs embarcadas)
    cat > "$bindir/ufta-vmm" <<LAUNCHER
#!/usr/bin/env bash
# UFTA-VMM launcher — usa bibliotecas embarcadas se presentes
DIR="\$(cd "\$(dirname "\${BASH_SOURCE[0]}")" && pwd)"
export LD_LIBRARY_PATH="$libdir:\${LD_LIBRARY_PATH:-}"

if [ "\$1" = "gui" ]; then
    exec "\$DIR/uvm-gui" "\$@"
else
    exec "\$DIR/uvm" "\$@"
fi
LAUNCHER
    chmod 755 "$bindir/ufta-vmm"

    # Cria atalho de desktop se estiver em /usr/local ou /usr
    if [ "$prefix" = "/usr/local" ] || [ "$prefix" = "/usr" ]; then
        local appsdir="/usr/share/applications"
        if [ -d "$appsdir" ] && [ -w "$appsdir" ]; then
            cat > "$appsdir/ufta-vmm.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=UFTA-VMM
Comment=Universal Field Transformation Architecture — Virtual Memory Manager
Exec=$bindir/ufta-vmm gui
Icon=ufta-vmm
Terminal=false
Categories=System;Utility;
Keywords=memory;virtual;manager;gpu;
DESKTOP
            echo -e "    ${GREEN}✓ Atalho de desktop criado${NC}"
        fi
    fi

    # Atualiza cache de comandos
    if command -v hash >/dev/null 2>&1; then
        hash -r 2>/dev/null || true
    fi

    echo ""
    echo -e "  ${GREEN}══════════════════════════════════════════════════${NC}"
    echo -e "  ${GREEN}  ✓ UFTA-VMM instalado com sucesso!${NC}"
    echo -e "  ${GREEN}══════════════════════════════════════════════════${NC}"
    echo ""
    echo -e "  ${BOLD}Para usar:${NC}"
    echo -e "    ${CYAN}ufta-vmm gui${NC}        → Dashboard em tempo real (SDL2+OpenGL)"
    echo -e "    ${CYAN}ufta-vmm validate${NC}   → Validação de migração (CPU)"
    echo -e "    ${CYAN}ufta-vmm help${NC}       → Lista todos os comandos"
    echo ""
    echo -e "  ${BOLD}Para remover:${NC}"
    echo -e "    ${CYAN}./ufta-vmm-installer.run --uninstall${NC}"
    echo ""
}

# ── Desinstala ─────────────────────────────────────────────────
do_uninstall() {
    local prefix="$1"
    local bindir="$prefix/bin"
    local libdir="$prefix/lib/ufta-vmm"
    local sharedir="$prefix/share/ufta-vmm"

    banner
    echo -e "  ${BOLD}Removendo UFTA-VMM...${NC}"

    local removed=0
    for f in "$bindir/uvm" "$bindir/uvm-gui" "$bindir/ufta-vmm"; do
        if [ -f "$f" ]; then
            rm -f "$f"
            echo -e "    ${GREEN}✓ Removido: $f${NC}"
            removed=1
        fi
    done

    if [ -d "$libdir" ]; then
        rm -rf "$libdir"
        echo -e "    ${GREEN}✓ Removido: $libdir${NC}"
        removed=1
    fi

    if [ -d "$sharedir" ]; then
        rm -rf "$sharedir"
        echo -e "    ${GREEN}✓ Removido: $sharedir${NC}"
        removed=1
    fi

    # Remove atalho de desktop
    if [ -f "/usr/share/applications/ufta-vmm.desktop" ]; then
        rm -f "/usr/share/applications/ufta-vmm.desktop"
        echo -e "    ${GREEN}✓ Removido: atalho de desktop${NC}"
        removed=1
    fi

    if [ "$removed" = "0" ]; then
        echo -e "    ${YELLOW}⚠ Nenhuma instalação encontrada em $prefix${NC}"
    else
        echo ""
        echo -e "  ${GREEN}✓ UFTA-VMM removido com sucesso.${NC}"
    fi
}

# ── Main ───────────────────────────────────────────────────────
PREFIX="/usr/local"
ACTION="install"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --prefix)
            PREFIX="$2"
            shift 2
            ;;
        --uninstall)
            ACTION="uninstall"
            shift
            ;;
        --version)
            echo "UFTA-VMM $INSTALLER_VERSION ($ARCH)"
            exit 0
            ;;
        --help|-h)
            show_help
            exit 0
            ;;
        *)
            echo -e "  ${RED}✗ Opção desconhecida: $1${NC}"
            show_help
            exit 1
            ;;
    esac
done

case "$ACTION" in
    install)   do_install "$PREFIX" ;;
    uninstall) do_uninstall "$PREFIX" ;;
esac

exit 0

# ═══════════════════════════════════════════════════════════════
#  PAYLOAD — arquivos binários pré-compilados (tar.gz)
#  Tudo abaixo desta linha é o payload embarcado.
# ═══════════════════════════════════════════════════════════════
__PAYLOAD_BELOW__
