// Mod file overrides ([Mods] Overrides=<folder>): when the game opens a mod's file for reading, a file at
// <folder>\<mod folder name>\<path inside the mod> is opened instead, e.g. <folder>\2972672998\main.lua for the
// workshop mod 2972672998. A file the mod doesn't have can be added the same way (it exists for the game too). The
// mod's own files and the mod list are never touched; an empty setting turns it off.
// Used to run an optimized copy of a mod's script (the Gluon Plasma Diffuser's, research/mods).
#pragma once

namespace fix {

bool InstallOverrides();  // hooks on CreateFileW (the game's own opens and the C runtime's fopen) and on the
                          // existence checks GetFileAttributes(Ex)A/W and FindFirstFileA, all in KERNELBASE

}  // namespace fix
