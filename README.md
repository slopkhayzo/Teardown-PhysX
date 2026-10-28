# Teardown-PhysX

> [!WARNING]
> This mod has pretty much entirely been generated using AI;
> I won't and will never claim to have enough knowledge or expertise to do this
> kind of reverse-engineering by myself; I've tested it in a few campaign missions
> and levels on an RTX 4090 and so far spotted no major issues (on my machine ofc,
> if you have issues feel free to open an Issue).
> It only works with the Steam version of Teardown 2.1.0: with any other version it
> stays off and the game runs unmodified.

An ASI plugin for Teardown (Steam, 2.1.0) that hands free debris to NVIDIA PhysX 5 (on the GPU or the CPU) and
fixes stutters in the game's destruction code. Ultimate ASI Loader loads it as the game's `dxgi.dll`.

Current release: **v1.0.0**

## Install

Each release has two archives with the same plugin:

- `Teardown-PhysX-v<version>-with-Ultimate-ASI-Loader-v9.7.4.zip`: for a game without an ASI loader (if you don't
  know, take this one). Extract it into the game folder (in Steam: right-click Teardown > Manage > Browse local
  files) and start the game. Its `dxgi.dll` is Ultimate ASI Loader v9.7.4 x64, which loads the plugin.
- `Teardown-PhysX-v<version>-plugin-only.zip`: for a game that already has an x64 ASI loader. Extract it into the
  game folder too, or put `TeardownPhysX.asi`, `TeardownPhysX.ini` and `PhysXGpu_64.dll` together where your loader
  loads plugins from. Keep your loader.

PhysX runs on the GPU with an NVIDIA GeForce GTX 16 / RTX 20 series or newer (driver 570 or newer), otherwise on the
CPU. The archives' README.txt (the same text as [tools/package/README.txt](tools/package/README.txt)) has the
requirements, the settings and how to uninstall.
[Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) is MIT-licensed; the archive with it
includes its licence.

Game addresses are for one build of `teardown.exe`: with any other the plugin stays off and the game runs unmodified
(`tools/gen_addresses.py` regenerates them from a decrypted executable, which is not part of this repository). The
research notes, recordings and benchmark runs are kept out of the repository for now.

## Layout

- `src/`: the plugin (`px/` PhysX offload, `fix/` engine fixes, `hooks/`, `td/` game structures, `trace/` tracing).
  Settings and their defaults: `src/core/config.h` (read from `TeardownPhysX.ini`). The defaults are what players
  get; the test scripts write `[Session] Defaults=dev`, which starts from the development defaults instead (tracing
  on, PhysX off).
- `tools/`: build, install, benchmark, recording and packaging scripts; `tools/package/` holds the release archives'
  settings file, README and the loader's notice.
- `third_party/`: MinHook and the PhysX SDK as git submodules; `patches/` holds the local changes to the SDK.

## Building

Needs Visual Studio with the v143 (14.44) toolset, CMake and Ninja (both ship with Visual Studio), and the CUDA
12.8 toolkit for the GPU build.

1. `git submodule update --init`
2. `git -C third_party/physx-sdk apply ../patches/physx-sdk-gpu-archs.patch ../patches/physx-sdk-sleep.patch`
   (the first lets `build_physx_gpu.cmd` pick the GPU architectures to compile; the second keeps bodies the plugin
   puts to sleep asleep, reports those its solver moves anyway, and adds the island diagnostics behind
   `[PhysX] DebugIslands`)
3. `tools\build_physx_gpu.cmd` (PhysX with GPU support, for every GPU generation it supports: Volta and Turing to
   Blackwell, plus PTX for newer ones; `tools\build_physx_gpu.cmd 89 89` builds for one, here RTX 40) or
   `tools\build_physx.cmd` (CPU only)
4. `tools\build.cmd`: `build\TeardownPhysX.asi`
5. Ultimate ASI Loader v9.7.4, x64: the `dinput8.dll` of its release's `Ultimate-ASI-Loader-NoPDB_x64.zip`, in
   `third_party\uasi\v9.7.4\` (`tools\package.ps1` checks its SHA-256)
6. `tools\install.ps1 -GameDir <Teardown folder>` installs the loader, the plugin and the GPU module;
   `tools\uninstall.ps1` removes exactly what it installed.
7. `tools\package.ps1`: the two release archives in `dist\` (the version is in `src\core\version.h`), each with a
   `SHA256SUMS.txt` of its files and a `.zip.sha256` beside it. It checks that the GPU module has every
   architecture and that the loader is the tested build, and it never overwrites an archive (delete one to rebuild
   it).

The scripts default to the paths of the machine they were written on (Visual Studio, CUDA, Python, the game);
pass yours as parameters or edit the defaults.
