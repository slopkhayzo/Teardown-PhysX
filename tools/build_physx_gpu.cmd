@echo off
rem Builds PhysX 5.12 (third_party\physx-sdk) with GPU support for the plugin: static CPU libraries plus
rem PhysXGpu_64.dll (the SDK's supported pairing), static CRT (/MT), PhysX's "release" configuration, with Ninja.
rem CUDA 12.8's nvcc accepts host compilers up to Visual Studio 2022, so this whole build uses the v143 toolset
rem (14.44) installed in Visual Studio 2026; the plugin, built with v145, links the libraries (no /GL in them).
rem %1: the GPU architectures to compile machine code for, in quotes (default "70,75,86,89,120": every GPU that
rem PhysX's GPU code supports, Volta and Turing (GTX 16, RTX 20) to Blackwell (RTX 50); machine code also runs on later
rem GPUs of its major version, 7.0 on 7.5 for instance). %2: the one also compiled to PTX, which the driver compiles
rem for GPUs newer than all of them (default 120). For this machine only: build_physx_gpu.cmd 89 89.
rem Output: third_party\physx-build\release-gpu\. CPU-heavy: run it through the shared queue (a few minutes per
rem architecture the first time, then only what changed).
setlocal
set "SASS=%~1"
if "%SASS%"=="" set "SASS=70,75,86,89,120"
set "PTX=%~2"
if "%PTX%"=="" set "PTX=120"
rem CMake's own architecture list (policy CMP0104) gets the same machine code ("-real": no PTX of its own)
set "REAL=%SASS:,=-real;%-real"
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
if not exist "%OUT%\build.ninja" (
    "%CMAKE%" -S third_party\physx-sdk\physx -B "%OUT%" -G Ninja -DCMAKE_BUILD_TYPE=release ^
        -DPX_GENERATE_GPU_PROJECTS=ON -DPX_GENERATE_STATIC_LIBRARIES=ON -DPX_GENERATE_GPU_STATIC_LIBRARIES=OFF ^
        -DCMAKE_CUDA_COMPILER="%CUDA%\bin\nvcc.exe" -DCUDAToolkit_ROOT="%CUDA%" ^
        "-DPX_GPU_SASS_ARCHS=%SASS%" "-DPX_GPU_PTX_ARCH=%PTX%" "-DCMAKE_CUDA_ARCHITECTURES=%REAL%" ^
        -DNV_USE_STATIC_WINCRT=ON -DNV_USE_DEBUG_WINCRT=OFF ^
        -DPX_BUILDSNIPPETS=OFF -DPX_BUILDPVDRUNTIME=OFF -DPX_FLOAT_POINT_PRECISE_MATH=OFF ^
        -DPX_OUTPUT_LIB_DIR="%OUT%\lib" -DPX_OUTPUT_BIN_DIR="%OUT%\bin" -DPX_OUTPUT_DLL_DIR="%OUT%\bin" ^
        -DCMAKE_INSTALL_PREFIX="%OUT%\install" || exit /b 1
) else (
    rem an existing build: only the architectures may have changed (then the CUDA files are compiled again)
    "%CMAKE%" -S third_party\physx-sdk\physx -B "%OUT%" "-DPX_GPU_SASS_ARCHS=%SASS%" "-DPX_GPU_PTX_ARCH=%PTX%" "-DCMAKE_CUDA_ARCHITECTURES=%REAL%" >nul || exit /b 1
)
echo GPU architectures: machine code %SASS%, PTX %PTX%
"%CMAKE%" --build "%OUT%" || exit /b 1
echo built PhysX with GPU support into %OUT%
dir /s /b "%OUT%\*.lib" "%OUT%\*.dll"
