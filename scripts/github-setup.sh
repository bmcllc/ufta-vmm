#!/bin/bash
# ═══════════════════════════════════════════════════════════════
#  UFTA-VMM — GitHub Setup
#
#  Conecta o repositório local ao GitHub e cria o primeiro release.
#
#  Uso:
#    bash scripts/github-setup.sh            # cria repositório e faz push
#    bash scripts/github-setup.sh --release  # também cria tag v1.0.0
#
#  Pré-requisito: gh CLI autenticado
#    gh auth login
# ═══════════════════════════════════════════════════════════════
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

REPO_NAME="ufta-vmm"
VERSION="1.0.0"
DO_RELEASE=0

# Parse args
for arg in "$@"; do
    case "$arg" in
        --release) DO_RELEASE=1 ;;
        --version=*) VERSION="${arg#--version=}" ;;
    esac
done

echo "═══════════════════════════════════════════════════════"
echo "  UFTA-VMM — GitHub Setup"
echo "═══════════════════════════════════════════════════════"
echo

# ── 1. Verifica gh autenticado ─────────────────────────────
echo "[1/4] Verificando GitHub CLI..."
if ! gh auth status >/dev/null 2>&1; then
    echo "  ⚠ gh CLI não autenticado."
    echo "  Rode:  gh auth login"
    echo "  Depois execute este script novamente."
    exit 1
fi
echo "  ✓ Autenticado como: $(gh api user -q .login)"

# ── 2. Cria repositório no GitHub ──────────────────────────
echo "[2/4] Criando repositório no GitHub..."
if gh repo view "$(gh api user -q .login)/$REPO_NAME" >/dev/null 2>&1; then
    echo "  ✓ Repositório já existe"
else
    gh repo create "$REPO_NAME" --public --source=. --remote=origin --push
    echo "  ✓ Repositório criado e código enviado"
fi

# ── 3. Garante que o remote está configurado ───────────────
echo "[3/4] Verificando remote..."
if ! git remote | grep -q origin; then
    git remote add origin "https://github.com/$(gh api user -q .login)/$REPO_NAME.git"
fi
git push -u origin main
echo "  ✓ Código enviado para main"

# ── 4. Cria release (opcional) ─────────────────────────────
if [ "$DO_RELEASE" = "1" ]; then
    echo "[4/4] Criando release v$VERSION..."
    if git tag | grep -q "v$VERSION"; then
        echo "  ✓ Tag v$VERSION já existe"
    else
        git tag "v$VERSION"
        git push origin "v$VERSION"
        echo "  ✓ Tag v$VERSION criada — GitHub Actions vai compilar os pacotes"
    fi
else
    echo "[4/4] (pule release — use --release para criar a tag)"
fi

echo
echo "═══════════════════════════════════════════════════════"
echo "  ✓ Setup concluído!"
echo
echo "  Repositório: https://github.com/$(gh api user -q .login)/$REPO_NAME"
echo
if [ "$DO_RELEASE" = "1" ]; then
    echo "  O GitHub Actions está compilando Linux + Windows..."
    echo "  Acompanhe em: Actions → Build & Release"
    echo "  Quando terminar, baixe os ZIPs do Release e suba no itch.io"
else
    echo "  Para gerar os pacotes (Linux + Windows):"
    echo "    bash scripts/github-setup.sh --release"
    echo "  Ou crie uma tag manualmente:"
    echo "    git tag v1.0.0 && git push origin v1.0.0"
fi
echo "═══════════════════════════════════════════════════════"
echo
