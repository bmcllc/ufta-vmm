@echo off
REM ═══════════════════════════════════════════════════════════════
REM  UFTA-VMM — Windows Package Builder
REM  Creates a self-contained .zip with embedded SDL2 DLLs.
REM
REM  Prerequisites:
REM    - MinGW-w64 (gcc) or MSVC in PATH
REM    - SDL2 development libraries (SDL2-devel-2.x.x-mingw.zip)
REM      from https://github.com/libsdl-org/SDL/releases
REM
REM  Usage:  build-windows.bat [version]
REM ═══════════════════════════════════════════════════════════════
@setlocal enabledelayedexpansion

set VERSION=%~1
if "%VERSION%"=="" set VERSION=1.0.0

set ROOT=%~dp0..\..
set PKG_NAME=ufta-vmm-%VERSION%-windows-x86_64
set STAGE=%ROOT%\build\package\%PKG_NAME%
set OUT=%ROOT%\dist\%PKG_NAME%.zip

echo ═══════════════════════════════════════════════════════════
echo   UFTA-VMM Windows Package Builder
echo   Version: %VERSION%
echo ═══════════════════════════════════════════════════════════
echo.

REM ── 1. Locate SDL2 ───────────────────────────────────────────
echo [1/5] Locating SDL2...
set SDL2_DIR=
if exist "%SDL2DIR%\include\SDL2\SDL.h" set SDL2_DIR=%SDL2DIR%
if "%SDL2_DIR%"=="" (
    if exist "%ROOT%\third_party\SDL2\include\SDL2\SDL.h" (
        set SDL2_DIR=%ROOT%\third_party\SDL2
    )
)
if "%SDL2_DIR%"=="" (
    echo   ERROR: SDL2 not found.
    echo   Download SDL2-devel-2.x.x-mingw.zip and extract to:
    echo     %ROOT%\third_party\SDL2
    echo   OR set the SDL2DIR environment variable.
    exit /b 1
)
echo   ✓ SDL2 found at: %SDL2_DIR%

REM ── 2. Build binaries ────────────────────────────────────────
echo [2/5] Building binaries...
pushd "%ROOT%"
if exist "Makefile.windows" (
    mingw32-make -f Makefile.windows clean >nul 2>&1
    mingw32-make -f Makefile.windows >nul 2>&1
    if errorlevel 1 (
        echo   ERROR: Build failed. Check MinGW-w64 is in PATH.
        popd
        exit /b 1
    )
) else (
    echo   WARNING: Makefile.windows not found. Using pre-built binaries.
    if not exist "uvm.exe" (
        echo   ERROR: uvm.exe not found. Build it first.
        popd
        exit /b 1
    )
)
popd

REM ── 3. Create staging directory ──────────────────────────────
echo [3/5] Creating staging directory...
if exist "%STAGE%" rmdir /s /q "%STAGE%"
if exist "%ROOT%\dist" rmdir /s /q "%ROOT%\dist"
mkdir "%STAGE%\bin"
mkdir "%STAGE%\lib"
mkdir "%STAGE%\docs"

REM ── 4. Copy binaries and SDL2 DLLs ───────────────────────────
echo [4/5] Copying binaries and SDL2 DLLs...
copy /y "%ROOT%\uvm.exe" "%STAGE%\bin\" >nul
copy /y "%ROOT%\uvm-gui.exe" "%STAGE%\bin\" >nul

REM Copy SDL2 DLLs (both x86 and x64 variants if present)
if exist "%SDL2_DIR%\lib\x64\SDL2.dll" (
    copy /y "%SDL2_DIR%\lib\x64\SDL2.dll" "%STAGE%\lib\" >nul
    echo   ✓ SDL2.dll (x64) copied
)
if exist "%SDL2_DIR%\lib\x86\SDL2.dll" (
    copy /y "%SDL2_DIR%\lib\x86\SDL2.dll" "%STAGE%\lib\" >nul
    echo   ✓ SDL2.dll (x86) copied
)
if exist "%SDL2_DIR%\lib\SDL2.dll" (
    copy /y "%SDL2_DIR%\lib\SDL2.dll" "%STAGE%\lib\" >nul
    echo   ✓ SDL2.dll copied
)

REM ── 5. Copy docs & launcher ──────────────────────────────────
echo [5/5] Copying documentation and launcher...
copy /y "%ROOT%\README.md" "%STAGE%\docs\README.md" >nul

REM Create launcher batch file
(
echo @echo off
echo REM UFTA-VMM launcher — uses bundled SDL2 DLLs
echo set "DIR=%%~dp0"
echo set "PATH=%%DIR%%..\lib;%%PATH%%"
echo.
echo if "%%1"=="gui" ^(
echo     "%%DIR%%uvm-gui.exe" %%*
echo ^) else ^(
echo     "%%DIR%%uvm.exe" %%*
echo ^)
) > "%STAGE%\bin\ufta-vmm.bat"

REM ── Package ──────────────────────────────────────────────────
echo.
echo Packaging...
if exist "%ROOT%\dist" rmdir /s /q "%ROOT%\dist"
mkdir "%ROOT%\dist"
powershell -Command "Compress-Archive -Path '%STAGE%' -DestinationPath '%OUT%' -Force"

echo.
echo ═══════════════════════════════════════════════════════════
echo   ✓ Package created: %OUT%
echo.
echo   Contents:
echo     bin\uvm.exe          — CLI tool
echo     bin\uvm-gui.exe      — GUI dashboard
echo     bin\ufta-vmm.bat     — launcher (uses bundled DLLs)
echo     lib\SDL2.dll         — embedded SDL2 runtime
echo     docs\README.md       — documentation
echo.
echo   Install:
echo     Unzip %PKG_NAME%.zip
echo     Run bin\ufta-vmm.bat gui
echo ═══════════════════════════════════════════════════════════
@endlocal
