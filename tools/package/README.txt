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

Install
-------
1. Open the game's folder: in Steam, right-click Teardown > Manage > Browse local files (the folder holding
   teardown.exe).
2. Copy dxgi.dll, TeardownPhysX.asi, TeardownPhysX.ini and PhysXGpu_64.dll into it.
3. Start the game as usual.

dxgi.dll is Ultimate ASI Loader, which loads TeardownPhysX.asi when the game starts. If the folder already has a
dxgi.dll (ReShade, for instance), rename that one to dxgiHooked.dll first: the loader then loads it in turn.

Uninstall
---------
Delete dxgi.dll, TeardownPhysX.asi, TeardownPhysX.ini, PhysXGpu_64.dll and the TeardownPhysX_logs folder from the
game's folder (and rename a dxgiHooked.dll back to dxgi.dll).

Settings
--------
TeardownPhysX.ini in the game's folder; each setting is explained there. Restart the game after a change.
Without the file the plugin uses its defaults.

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
The plugin writes TeardownPhysX_logs\plugin.log in the game's folder (plugin.prev.log is the session before), and
crash.dmp there if it crashes. Please attach them to a bug report, with what you were doing.

Source code and licences
------------------------
https://github.com/slopkhayzo/Teardown-PhysX (MIT License, see licenses\Teardown-PhysX.txt). The package contains:
- NVIDIA PhysX 5.12 (Apache License 2.0, licenses\PhysX.txt), built from source with changes of this project's
  (third_party\patches in the repository).
- MinHook (BSD 2-Clause License, licenses\MinHook.txt).
- Ultimate ASI Loader by ThirteenAG (MIT License, licenses\Ultimate-ASI-Loader.txt).

Teardown is a game by Tuxedo Labs. NVIDIA and PhysX are trademarks of NVIDIA Corporation. This mod is not affiliated
with or endorsed by either.
