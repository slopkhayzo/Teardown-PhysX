@echo off
rem Configure + build TeardownPhysX.asi (RelWithDebInfo) into build\. CPU-heavy: run it through the shared queue.
setlocal
set "VS=C:\Program Files\Microsoft Visual Studio\18\Community"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "CMAKE=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "PATH=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
cd /d "%~dp0.."
if not exist build\build.ninja (
    "%CMAKE%" -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo || exit /b 1
)
"%CMAKE%" --build build || exit /b 1
echo built %CD%\build\TeardownPhysX.asi
