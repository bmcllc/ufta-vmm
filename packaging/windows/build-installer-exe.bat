@echo off
REM ═══════════════════════════════════════════════════════════════
REM  UFTA-VMM — Windows .exe Installer Builder
REM  Compila o instalador self-extracting (.exe) para Windows.
REM
REM  O instalador é um binário compilado em C (installer-main.c)
REM  que contém o payload (binários + DLLs SDL2) embutidos.
REM  Ao executar, ele instala em %ProgramFiles%\UFTA-VMM.
REM
REM  Prerequisites:
REM    - MinGW-w64 (gcc) in PATH
REM    - SDL2-devel-2.x.x-mingw.zip em third_party/SDL2
REM
REM  Usage:  build-installer-exe.bat [version]
REM ═══════════════════════════════════════════════════════════════
@setlocal enabledelayedexpansion

set VERSION=%~1
if "%VERSION%"=="" set VERSION=1.0.0

set ROOT=%~dp0..\..
set PKG_NAME=ufta-vmm-%VERSION%-windows-x86_64
set STAGE=%ROOT%\build\installer-stage
set OUTDIR=%ROOT%\dist
set OUT=%OUTDIR%\%PKG_NAME%-installer.exe
set PACK=%ROOT%\build\pack-payload.exe

echo.
echo  ═══════════════════════════════════════════════════════
echo   UFTA-VMM Windows .exe Installer Builder
echo   Version: %VERSION%
echo  ═══════════════════════════════════════════════════════
echo.

REM ── 1. Verifica MinGW ────────────────────────────────────────
echo [1/7] Verifying MinGW-w64...
gcc --version >nul 2>&1
if errorlevel 1 (
    echo   ERRO: MinGW-w64 (gcc) não encontrado no PATH.
    echo   Instale de: https://github.com/niXman/mingw-builds-binaries
    exit /b 1
)
echo   ✓ gcc encontrado

REM ── 2. Localiza SDL2 ─────────────────────────────────────────
echo [2/7] Locating SDL2...
set SDL2_DIR=
if exist "%ROOT%\third_party\SDL2\include\SDL2\SDL.h" (
    set SDL2_DIR=%ROOT%\third_party\SDL2
)
if "%SDL2_DIR%"=="" (
    if defined SDL2DIR (
        if exist "%SDL2DIR%\include\SDL2\SDL.h" set SDL2_DIR=%SDL2DIR%
    )
)
if "%SDL2_DIR%"=="" (
    echo   ERRO: SDL2 não encontrado.
    echo   Baixe SDL2-devel-mingw.zip de:
    echo     https://github.com/libsdl-org/SDL/releases
    echo   Extraia para: %ROOT%\third_party\SDL2
    exit /b 1
)
echo   ✓ SDL2 encontrado em: %SDL2_DIR%

REM ── 3. Compila utilitário pack-payload ────────────────────────
echo [3/7] Building pack-payload utility...
gcc -O2 -o "%PACK%" "%~dp0pack-payload.c"
if errorlevel 1 (
    echo   ERRO: falha ao compilar pack-payload.c
    exit /b 1
)
echo   ✓ pack-payload.exe compilado

REM ── 4. Compila o instalador ──────────────────────────────────
echo [4/7] Building installer executable...
gcc -O2 -o "%OUTDIR%\ufta-vmm-installer.exe" "%~dp0installer-main.c" -lz
if errorlevel 1 (
    echo   ERRO: falha ao compilar installer-main.c
    echo   Certifique-se de que o zlib está disponível ou remova -lz
    REM Tenta sem zlib
    gcc -O2 -o "%OUTDIR%\ufta-vmm-installer.exe" "%~dp0installer-main.c"
    if errorlevel 1 (
        echo   ERRO: falha ao compilar installer-main.c
        exit /b 1
    )
    echo   ✓ compilado (sem compressão)
) else (
    echo   ✓ ufta-vmm-installer.exe compilado
)

REM ── 5. Compila os binários UFTA-VMM ──────────────────────────
echo [5/7] Building UFTA-VMM binaries...
pushd "%ROOT%"
if exist "Makefile.windows" (
    mingw32-make -f Makefile.windows clean >nul 2>&1
    mingw32-make -f Makefile.windows >nul 2>&1
    if errorlevel 1 (
        echo   AVISO: build via Makefile falhou, tentando gcc direto...
        REM Build manual como fallback
        gcc -Wall -Wextra -O2 -std=c11 -D_GNU_SOURCE -Iinclude ^
            src/backend.c src/field.c src/main.c src/migrate.c ^
            src/page.c src/pagefault.c src/pipeline.c src/predictor.c ^
            src/state.c src/tier.c src/validate.c ^
            -o uvm.exe -lm
        if errorlevel 1 (
            echo   ERRO: build dos binários falhou
            popd
            exit /b 1
        )
    )
)
popd
if not exist "%ROOT%\uvm.exe" (
    echo   ERRO: uvm.exe não encontrado após build
    exit /b 1
)
echo   ✓ uvm.exe compilado

REM ── 6. Compila o GUI ─────────────────────────────────────────
echo [6/7] Building GUI binary...
pushd "%ROOT%"
gcc -Wall -Wextra -O2 -std=c11 -D_GNU_SOURCE -Iinclude ^
    -I"%SDL2_DIR%\include" ^
    src/backend.c src/field.c src/migrate.c src/page.c ^
    src/pagefault.c src/pipeline.c src/predictor.c src/state.c ^
    src/tier.c src/validate.c src/gui.c ^
    -o uvm-gui.exe -lm ^
    -L"%SDL2_DIR%\lib\x64" -lmingw32 -lSDL2main -lSDL2 -lopengl32 -mwindows
popd

REM ── 7. Empacota o payload e gera o .exe ──────────────────────
echo [7/7] Packaging payload into installer...

REM Limpa e cria diretório temporário
if exist "%STAGE%" rmdir /s /q "%STAGE%"
mkdir "%STAGE%"

REM Copia binários para o stage
copy /y "%ROOT%\uvm.exe" "%STAGE%\" >nul
copy /y "%ROOT%\uvm-gui.exe" "%STAGE%\" >nul

REM Copia SDL2 DLL
if exist "%SDL2_DIR%\lib\x64\SDL2.dll" (
    copy /y "%SDL2_DIR%\lib\x64\SDL2.dll" "%STAGE%\SDL2.dll" >nul
) else if exist "%SDL2_DIR%\lib\SDL2.dll" (
    copy /y "%SDL2_DIR%\lib\SDL2.dll" "%STAGE%\SDL2.dll" >nul
)

REM Cria payload binário usando pack-payload
cd /d "%ROOT%"
"%PACK%" "%STAGE%\payload.bin" ^
    "%STAGE%\uvm.exe" ^
    "%STAGE%\uvm-gui.exe" "%STAGE%\SDL2.dll"

REM Anexa o payload ao instalador
echo. >> "%OUT%"
echo __UFTA_PAYLOAD_BELOW__ >> "%OUT%"
copy /b "%OUT%" + "%STAGE%\payload.bin" "%STAGE%\final.exe" >nul
move /y "%STAGE%\final.exe" "%OUT%" >nul

REM ── Limpeza ──────────────────────────────────────────────────
rmdir /s /q "%STAGE%" 2>nul
del /q "%STAGE%\payload.bin" 2>nul
del /q "%PACK%" 2>nul
if exist "%ROOT%\uvm.exe" del /q "%ROOT%\uvm.exe"
if exist "%ROOT%\uvm-gui.exe" del /q "%ROOT%\uvm-gui.exe"

REM ── Verificação final ────────────────────────────────────────
echo.
echo  ═══════════════════════════════════════════════════════
if exist "%OUT%" (
    echo   ✓ Instalador gerado com sucesso!
    echo.
    echo   Arquivo:  %OUT%
    echo   Tamanho:
    for %%F in ("%OUT%") do echo     %%~zF bytes
    echo.
    echo   Distribua este arquivo. Para instalar:
    echo     %PKG_NAME%-installer.exe
    echo.
    echo   Opções:
    echo     /prefix <dir>     Diretório de instalação
    echo     /silent           Instala sem perguntar
    echo     /uninstall        Remove a instalação
    echo  ═══════════════════════════════════════════════════════
) else (
    echo   ERRO: falha ao gerar o instalador.
    echo  ═══════════════════════════════════════════════════════
)
echo.
