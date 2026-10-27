@echo off
rem Builds PhysX 5.12 (third_party\physx-sdk) as static libraries for the plugin: CPU only, static CRT (/MT),
rem PhysX's own "release" configuration (or the one given as %1: release | profile | checked), with Ninja.
rem Output: third_party\physx-build\<config>\ (libs in lib\, PxConfig.h in include\). CPU-heavy: run it through the
rem shared queue. Re-running only rebuilds what changed.
setlocal
set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=release"
set "VS=C:\Program Files\Microsoft Visual Studio\18\Community"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "CMAKE=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "PATH=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
cd /d "%~dp0.."
set "OUT=%CD%\third_party\physx-build\%CONFIG%"
if not exist "%OUT%\build.ninja" (
    "%CMAKE%" -S third_party\physx-sdk\physx -B "%OUT%" -G Ninja -DCMAKE_BUILD_TYPE=%CONFIG% ^
        -DPX_GENERATE_GPU_PROJECTS=OFF -DPX_GENERATE_STATIC_LIBRARIES=ON ^
        -DNV_USE_STATIC_WINCRT=ON -DNV_USE_DEBUG_WINCRT=OFF ^
        -DPX_BUILDSNIPPETS=OFF -DPX_BUILDPVDRUNTIME=OFF -DPX_FLOAT_POINT_PRECISE_MATH=OFF ^
        -DPX_OUTPUT_LIB_DIR="%OUT%\lib" -DPX_OUTPUT_BIN_DIR="%OUT%\bin" -DPX_OUTPUT_DLL_DIR="%OUT%\bin" ^
        -DCMAKE_INSTALL_PREFIX="%OUT%\install" || exit /b 1
)
"%CMAKE%" --build "%OUT%" || exit /b 1
echo built PhysX %CONFIG% into %OUT%
dir /s /b "%OUT%\*.lib"
