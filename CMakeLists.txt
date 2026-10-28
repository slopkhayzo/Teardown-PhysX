cmake_minimum_required(VERSION 3.24)
project(TeardownPhysX LANGUAGES C CXX ASM_MASM RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: the plugin does not depend on the game's runtime DLLs.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(minhook STATIC
    third_party/minhook/src/buffer.c
    third_party/minhook/src/hook.c
    third_party/minhook/src/trampoline.c
    third_party/minhook/src/hde/hde64.c)
target_include_directories(minhook PUBLIC third_party/minhook/include)

# PhysX 5.12 static libraries (/MT, PhysX "release" config), built separately: with GPU support by
# tools\build_physx_gpu.cmd (plus PhysXGpu.dll, which the plugin loads at run time when [PhysX] Gpu=1), or CPU only by
# tools\build_physx.cmd (TDPX_PHYSX_GPU=OFF).
option(TDPX_PHYSX_GPU "Link the GPU-enabled PhysX build" ON)
set(PHYSX_ROOT ${CMAKE_SOURCE_DIR}/third_party/physx-sdk/physx)
add_library(physx_static INTERFACE)
target_include_directories(physx_static SYSTEM INTERFACE ${PHYSX_ROOT}/include)
if(TDPX_PHYSX_GPU)
    set(PHYSX_LIBDIR ${CMAKE_SOURCE_DIR}/third_party/physx-build/release-gpu/lib/bin/win.x86_64.vc143.mt/release)
    target_compile_definitions(physx_static INTERFACE PX_PHYSX_STATIC_LIB)
else()
    set(PHYSX_LIBDIR ${CMAKE_SOURCE_DIR}/third_party/physx-build/release/lib/bin/win.x86_64.vc143.mt/release)
    # built without CUDA: PX_SUPPORT_GPU_PHYSX 0 changes public structs
    target_compile_definitions(physx_static INTERFACE DISABLE_CUDA_PHYSX PX_PHYSX_STATIC_LIB)
endif()
target_link_libraries(physx_static INTERFACE
    ${PHYSX_LIBDIR}/PhysXExtensions_static_64.lib
    ${PHYSX_LIBDIR}/PhysX_static_64.lib
    ${PHYSX_LIBDIR}/PhysXPvdSDK_static_64.lib
    ${PHYSX_LIBDIR}/PhysXCommon_static_64.lib
    ${PHYSX_LIBDIR}/PhysXFoundation_static_64.lib)

add_library(TeardownPhysX SHARED
    src/dllmain.cpp
    src/core/log.cpp
    src/core/config.cpp
    src/core/crash.cpp
    src/td/addresses.cpp
    src/td/shape_events.cpp
    src/trace/trace.cpp
    src/trace/census.cpp
    src/trace/sampler.cpp
    src/trace/threads.cpp
    src/trace/gpu_kernels.cpp
    src/hooks/hooks.cpp
    src/hooks/timing_thunk.asm
    src/bench/bench.cpp
    src/fix/overrides.cpp
    src/fix/touch_cache.cpp
    src/fix/visible_early_out.cpp
    src/fix/keep_debris.cpp
    src/fix/split_jobs.cpp
    src/fix/static_body_update.cpp
    src/fix/serial_jobs.cpp
    src/input/recorder.cpp
    src/px/colliders.cpp
    src/px/world.cpp
    src/px/shadow.cpp
    src/version.rc)
target_include_directories(TeardownPhysX PRIVATE src)
# CUPTI (CUDA toolkit) for the GPU kernel timer ([Trace] GpuKernels): headers only, the DLL is loaded at run time
set(TDPX_CUDA_DIR "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v12.8" CACHE PATH "CUDA toolkit with CUPTI")
if(EXISTS "${TDPX_CUDA_DIR}/extras/CUPTI/include/cupti_activity.h")
    target_include_directories(TeardownPhysX SYSTEM PRIVATE "${TDPX_CUDA_DIR}/include" "${TDPX_CUDA_DIR}/extras/CUPTI/include")
    target_compile_definitions(TeardownPhysX PRIVATE TDPX_CUPTI=1)
endif()
target_compile_definitions(TeardownPhysX PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(TeardownPhysX PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /permissive- /Zi>)
target_link_options(TeardownPhysX PRIVATE /DEBUG /OPT:REF /OPT:ICF /MAP)
target_link_libraries(TeardownPhysX PRIVATE minhook physx_static psapi)
# Ultimate ASI Loader loads *.asi files.
set_target_properties(TeardownPhysX PROPERTIES SUFFIX ".asi")
if(TDPX_PHYSX_GPU)
    # under the name PhysX looks for (PX_PHYSX_GPU_SHARED_LIB_NAME); the install scripts put it next to the plugin
    add_custom_command(TARGET TeardownPhysX POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${PHYSX_LIBDIR}/PhysXGpu.dll
                $<TARGET_FILE_DIR:TeardownPhysX>/PhysXGpu_64.dll)
endif()
