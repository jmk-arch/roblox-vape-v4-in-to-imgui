@echo off
REM Vape V4 ImGui port - build script
REM
REM Usage:  build.bat        (build and run)
REM         build.bat build  (build only)

setlocal
chcp 65001 >nul
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSPATH=%%i
if "%VSPATH%"=="" (
    echo [ERROR] Visual Studio C++ tools not found.
    exit /b 1
)
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

if not exist "imgui\imgui.h" (
    echo [ERROR] Bundled Dear ImGui source not found.
    exit /b 1
)

if not exist "build" mkdir "build"

echo [BUILD] Vape V4 ImGui ...
cl /nologo /EHsc /std:c++17 /W3 /utf-8 /O2 /MT ^
   /I"imgui" /I"imgui\backends" /I"." ^
   vape_gui.cpp vape_assets.cpp example_main.cpp app_main.cpp ^
   imgui\imgui.cpp imgui\imgui_draw.cpp ^
   imgui\imgui_widgets.cpp imgui\imgui_tables.cpp ^
   imgui\backends\imgui_impl_dx11.cpp ^
   imgui\backends\imgui_impl_win32.cpp ^
   /Fo"build\\" /Fe"build\VapeV4.exe" ^
   /link d3d11.lib dxgi.lib d3dcompiler.lib user32.lib gdi32.lib shell32.lib

if errorlevel 1 (
    echo [FAILED] build error
    exit /b 1
)

echo [OK] build\VapeV4.exe

if /i "%1"=="build" goto :done

echo [RUN] RIGHT SHIFT = toggle GUI, ESC = quit
"build\VapeV4.exe"

:done
endlocal
