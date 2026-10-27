# Teardown-PhysX

An ASI plugin for Teardown (Steam, 2.1.0) that hands free debris to NVIDIA PhysX 5 (on the GPU or the CPU) and
fixes stutters in the game's destruction code. Ultimate ASI Loader loads it as the game's `dxgi.dll`.

Work in progress: game addresses are for one build of `teardown.exe` (`tools/gen_addresses.py` regenerates them
from a decrypted executable, which is not part of this repository). The research notes, recordings and benchmark
runs are kept out of the repository for now.

## Layout

- `src/`: the plugin (`px/` PhysX offload, `fix/` engine fixes, `hooks/`, `td/` game structures, `trace/` tracing).
  Settings and their defaults: `src/core/config.h` (read from `TeardownPhysX.ini`).
- `tools/`: build, install, benchmark and recording scripts.
- `third_party/`: MinHook and the PhysX SDK as git submodules; `patches/` holds the local change to the SDK.

## Building

Needs Visual Studio with the v143 (14.44) toolset, CMake and Ninja (both ship with Visual Studio), and the CUDA
12.8 toolkit for the GPU build.

1. `git submodule update --init`
2. `git -C third_party/physx-sdk apply ../patches/physx-sdk-gpu-archs.patch` (lets `build_physx_gpu.cmd` pick the
   GPU architectures to compile)
3. `tools\build_physx_gpu.cmd` (PhysX with GPU support; the first build takes hours) or `tools\build_physx.cmd`
   (CPU only)
4. `tools\build.cmd`: `build\TeardownPhysX.asi`
5. Ultimate ASI Loader v9.7.4, x64 (`dinput8.dll` from its release) in `third_party\uasi\v9.7.4\`
6. `tools\install.ps1 -GameDir <Teardown folder>` installs the loader, the plugin and the GPU module;
   `tools\uninstall.ps1` removes exactly what it installed.

The scripts default to the paths of the machine they were written on (Visual Studio, CUDA, Python, the game);
pass yours as parameters or edit the defaults.
