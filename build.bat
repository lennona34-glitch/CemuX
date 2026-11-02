@echo off
setlocal enabledelayedexpansion

title CemuX Build System

echo ============================================================
echo                     CemuX Build System
echo ============================================================
echo.

set "ROOT_DIR=%~dp0"
if "%ROOT_DIR:~-1%"=="\" set "ROOT_DIR=%ROOT_DIR:~0,-1%"

:: 1. Locate Visual Studio 2022 / Build Tools
echo [1/4] Detecting Visual Studio 2022...
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VCVARS="

if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" (
            set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
        )
    )
)

if "%VCVARS%"=="" (
    if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" (
        set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
    ) else if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" (
        set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
    ) else if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat" (
        set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
    ) else if exist "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" (
        set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
    ) else if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" (
        set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
    )
)

if "%VCVARS%"=="" (
    where cl.exe >nul 2>nul
    if %ERRORLEVEL% equ 0 (
        echo   MSVC compiler found in active PATH.
    ) else (
        echo [ERROR] Visual Studio 2022 with C++ x64 tools was not found!
        echo Please install Visual Studio 2022 or C++ Build Tools with x64 support.
        echo.
        if not "%1"=="--no-pause" pause
        exit /b 1
    )
) else (
    echo   Found: %VCVARS%
    echo   Initializing x64 developer environment...
    call "%VCVARS%" >nul
)

:: 2. Check Git Submodules
echo.
echo [2/4] Checking Git submodules...
if not exist "%ROOT_DIR%\dependencies\imgui\imgui.cpp" (
    echo   Initializing Git submodules: imgui, cubeb, ZArchive, Vulkan-Headers...
    where git >nul 2>nul
    if !ERRORLEVEL! equ 0 (
        git submodule update --init --recursive
    ) else (
        echo [WARNING] Git not found in PATH; skipping submodule update.
    )
) else (
    echo   Submodules already initialized.
)

:: 3. Prepare Dependencies
echo.
echo [3/4] Checking dependencies...
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT_DIR%\prepare_deps.ps1" "%ROOT_DIR%"
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Dependency preparation failed.
    if not "%1"=="--no-pause" pause
    exit /b %ERRORLEVEL%
)

:: 4. Build CemuX
echo.
echo [4/4] Building CemuX executable...
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT_DIR%\build_direct.ps1" "%ROOT_DIR%"
if %ERRORLEVEL% neq 0 (
    echo.
    echo [ERROR] Build failed! Check build\direct_build.log for details.
    if not "%1"=="--no-pause" pause
    exit /b %ERRORLEVEL%
)

echo.
echo ============================================================
echo                  CemuX Build Succeeded!
echo   Executable: bin\CemuX.exe
echo ============================================================
echo.

if not "%1"=="--no-pause" (
    pause
)
