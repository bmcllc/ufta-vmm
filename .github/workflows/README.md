# GitHub Actions — Build & Release Automático

Este workflow compila automaticamente o UFTA-VMM para **Linux** e **Windows**
e gera os pacotes prontos para o **itch.io** — sem precisar de máquina Windows.

## Como funciona

Quando você cria uma **tag** `v*` no GitHub (ex: `v1.0.0`), o workflow:

1. **Linux** (ubuntu-latest):
   - Instala SDL2 + OpenGL
   - Compila `uvm` e `uvm-gui`
   - Roda `./uvm validate` (teste)
   - Gera `ufta-vmm-<ver>-linux-x86_64.zip` (SDL2 embutido)

2. **Windows** (windows-latest):
   - Instala MinGW-w64
   - Baixa SDL2-devel-mingw
   - Compila `uvm.exe` e `uvm-gui.exe`
   - Gera `ufta-vmm-<ver>-windows-x86_64.zip` (SDL2.dll embutido)

3. **Release**:
   - Cria um GitHub Release com os dois ZIPs anexados

## Como usar

### Opção 1 — Criar uma tag (recomendado)

```bash
git tag v1.0.0
git push origin v1.0.0
```

O GitHub Actions roda automaticamente e cria o Release com os pacotes.

### Opção 2 — Rodar manualmente

1. Vá em **Actions** → **Build & Release**
2. Clique em **Run workflow**
3. Informe a versão (ex: `1.0.0`)
4. Os artefatos ficam disponíveis em **Artifacts** (sem criar Release)

## Onde baixar os pacotes

- **Com Release:** Abra o Release criado → baixe os ZIPs anexados
- **Sem Release:** Actions → Build & Release → Artifacts → baixe os ZIPs

## Upload no itch.io

1. Baixe os dois ZIPs do Release
2. No itch.io, crie/abra o projeto
3. **Linux:** upload do `ufta-vmm-<ver>-linux-x86_64.zip` (marcar como Linux)
4. **Windows:** upload do `ufta-vmm-<ver>-windows-x86_64.zip` (marcar como Windows)
5. O itch.io entrega o pacote certo automaticamente

## Requisitos

- Repositório no GitHub (público ou privado)
- O código-fonte commitado (sem `build/`, `dist/`, `third_party/` — estão no `.gitignore`)

## Notas

- O SDL2 é baixado automaticamente pelo CI (versão 2.32.4)
- O workflow usa `workflow_dispatch` para rodar manualmente
- Os binários são compilados com `-O2` (otimizado para release)
