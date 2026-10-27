@echo off
rem Builds PhysX 5.12 (third_party\physx-sdk) with GPU support for the plugin: static CPU libraries plus
rem PhysXGpu_64.dll (the SDK's supported pairing), static CRT (/MT), PhysX's "release" configuration, with Ninja.
rem CUDA 12.8's nvcc accepts host compilers up to Visual Studio 2022, so this whole build uses the v143 toolset
rem (14.44) installed in Visual Studio 2026; the plugin, built with v145, links the libraries (no /GL in them).
rem %1: GPU architectures to compile for (default 89 = RTX 40xx, with PTX 89 for newer GPUs to JIT);
rem "sdk" = the SDK's own list (70 to 120, much slower). Output: third_party\physx-build\release-gpu\.
rem CPU-heavy and long: run it through the shared queue.
setlocal
set "ARCHS=%~1"
if "%ARCHS%"=="" set "ARCHS=89"
set "VS=C:\Program Files\Microsoft Visual Studio\18\Community"
set "CUDA=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.8"
if not exist "%CUDA%\bin\nvcc.exe" (
    echo CUDA 12.8 not found at %CUDA%
    exit /b 1
)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" -vcvars_ver=14.44 >nul || exit /b 1
set "CMAKE=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "PATH=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%CUDA%\bin;%PATH%"
set "CUDA_PATH=%CUDA%"
cd /d "%~dp0.."
set "OUT=%CD%\third_party\physx-build\release-gpu"
set "ARCHDEFS=-DPX_GPU_SASS_ARCHS=%ARCHS% -DPX_GPU_PTX_ARCH=%ARCHS% -DCMAKE_CUDA_ARCHITECTURES=%ARCHS%"
if "%ARCHS%"=="sdk" set "ARCHDEFS=-DCMAKE_CUDA_ARCHITECTURES=70"
if not exist "%OUT%\build.ninja" (
    "%CMAKE%" -S third_party\physx-sdk\physx -B "%OUT%" -G Ninja -DCMAKE_BUILD_TYPE=release ^
        -DPX_GENERATE_GPU_PROJECTS=ON -DPX_GENERATE_STATIC_LIBRARIES=ON -DPX_GENERATE_GPU_STATIC_LIBRARIES=OFF ^
        -DCMAKE_CUDA_COMPILER="%CUDA%\bin\nvcc.exe" -DCUDAToolkit_ROOT="%CUDA%" %ARCHDEFS% ^
        -DNV_USE_STATIC_WINCRT=ON -DNV_USE_DEBUG_WINCRT=OFF ^
        -DPX_BUILDSNIPPETS=OFF -DPX_BUILDPVDRUNTIME=OFF -DPX_FLOAT_POINT_PRECISE_MATH=OFF ^
        -DPX_OUTPUT_LIB_DIR="%OUT%\lib" -DPX_OUTPUT_BIN_DIR="%OUT%\bin" -DPX_OUTPUT_DLL_DIR="%OUT%\bin" ^
        -DCMAKE_INSTALL_PREFIX="%OUT%\install" || exit /b 1
)
"%CMAKE%" --build "%OUT%" || exit /b 1
echo built PhysX with GPU support into %OUT%
dir /s /b "%OUT%\*.lib" "%OUT%\*.dll"
