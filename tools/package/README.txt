Teardown-PhysX
==============

Version @VERSION@

NVIDIA PhysX 5 simulates Teardown's loose debris, on the GPU or the CPU, instead of the game's own physics. Far
more debris can fall, collide and pile up before the game slows down, and piles that come to rest go to sleep and
cost next to nothing. The rest stays the game's: destruction, the player, vehicles, tools, scripts and mods.

Requirements
------------
- Teardown 2.1.0, the Steam version. At start the plugin checks the game's code: with any other version it stays
  off and the game runs unmodified, until an update of the plugin.
- Windows 10 or 11, 64-bit.
- PhysX on the GPU: an NVIDIA GeForce GTX 16 / RTX 20 series or newer (or a Volta or newer workstation card), with
  a driver that supports CUDA 12.8 (driver 570 or newer). Without one, PhysX runs on the CPU.
- Single player. In a multiplayer session PhysX stays off; remove the mod to play multiplayer.

Choose an archive
-----------------
Each version comes in two archives with the same plugin:

- Teardown-PhysX-v@VERSION@-with-Ultimate-ASI-Loader-v9.7.4.zip: for a game without an ASI loader (if you don't
  know, it's this one). Its dxgi.dll is Ultimate ASI Loader x64 v9.7.4, which loads the plugin when the game
  starts.
- Teardown-PhysX-v@VERSION@-plugin-only.zip: for a game that already has an x64 ASI loader. Keep that loader.

Both hold this README, the licences (the licenses folder) and SHA256SUMS.txt, the SHA-256 of each of their files.

Install
-------
1. Open the game's folder: in Steam, right-click Teardown > Manage > Browse local files (the folder holding
   teardown.exe).
2. Extract the archive into it.
3. Start the game as usual.

If the folder already has a dxgi.dll that isn't an ASI loader (ReShade, for instance), rename it to dxgiHooked.dll
before you extract the archive with the loader: the loader then loads it in turn.

With the plugin-only archive, TeardownPhysX.asi, TeardownPhysX.ini and PhysXGpu_64.dll can go into the folder your
loader loads plugins from instead (scripts or plugins, for Ultimate ASI Loader). Keep the three together: the
plugin reads its settings and loads PhysX's GPU module from its own folder.

Uninstall
---------
Delete what the archive put into the game's folder (TeardownPhysX.asi, TeardownPhysX.ini, PhysXGpu_64.dll,
README.txt, SHA256SUMS.txt and the licenses folder) and the TeardownPhysX_logs folder next to the plugin. Delete
dxgi.dll only if it came with this archive and no other mod needs it, then rename a dxgiHooked.dll back to dxgi.dll.

Settings
--------
TeardownPhysX.ini, next to the plugin; each setting is explained there. Restart the game after a change. Without
the file the plugin uses its defaults.

What changes in the game
------------------------
- Loose debris moves under PhysX: each piece is one or a few convex shapes that follow its voxels, so pieces slide,
  tumble and stack a little differently than in the game.
- Small pieces collide with each other and pile up (the game lets them fall through other debris).
  [PhysX] SmallDebrisCollide=0 brings back the game's behaviour.
- The game's cleanup no longer deletes small debris out of view or far away. [Fix] KeepDebris=0 brings it back.
- PhysX's collisions make the game's impact sounds and break voxels as the game's own collisions do.

Good to know
------------
- A pile with thousands of pieces moving at once still costs frame rate (while you cut a building apart, for
  instance). Once it settles, it sleeps.
- When a big pile wakes up (you walk onto it) or falls asleep, the game can stutter once.
- The debris tool ([Tool] Enabled=1) cuts whatever you aim at into pieces while you hold the middle mouse button:
  a quick way to make piles. It is off by default.

Logs and bug reports
--------------------
The plugin writes TeardownPhysX_logs\plugin.log next to it (plugin.prev.log is the session before), and crash.dmp
there if it crashes. Please attach them to a bug report, with what you were doing.

Source code and licences
------------------------
https://github.com/slopkhayzo/Teardown-PhysX (MIT License, see licenses\Teardown-PhysX.txt). The archives contain:
- NVIDIA PhysX 5.12 (Apache License 2.0, licenses\PhysX.txt), built from source with changes of this project's
  (third_party\patches in the repository).
- MinHook (BSD 2-Clause License, licenses\MinHook.txt).
- Only in the archive with the loader: Ultimate ASI Loader v9.7.4 by ThirteenAG (MIT License,
  licenses\Ultimate-ASI-Loader.txt).

Teardown is a game by Tuxedo Labs. NVIDIA and PhysX are trademarks of NVIDIA Corporation. This mod is not affiliated
with or endorsed by either.

Misc
----
For more game fixes, visit:
https://slop-blog.enkhayzomachines.net/fixes :)
