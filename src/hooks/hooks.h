#pragma once

namespace hooks {

bool InstallApiHooks();     // right away from InitializeASI (the AppData redirect must precede the game's init)
bool InstallEngineHooks();  // once .text is decrypted: timed hooks + profiler/state hooks

}  // namespace hooks
