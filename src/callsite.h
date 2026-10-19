// Tells whether the function being hooked was called DIRECTLY by game code.
//
// The NT-level hooks (vatrack, fileguard) see every reservation / file open / close of the process:
// heap growth in ntdll, the GPU driver, Steam, the overlay, our own code... Only the calls made by the
// game's modules concern a snapshot. Walking the call stack outward from the hook, our own frames and
// the thin API layers (kernelbase/kernel32 and the compatibility shims that wrap them) are skipped, and
// the first other frame decides: it must lie in one of the listed game modules.
//
// Classification is lock-free (VirtualQuery + the module's PE export name, cached), so it is safe in
// hooks that run while other threads are suspended.
#pragma once
#include <stddef.h>

namespace callsite {

// Sets the space separated list of game modules (e.g. "engine.dll client.dll"). Safe to call again;
// the list is only rewritten when it changes, because hook threads read it concurrently.
void SetGameModules(const char* module_list);

// True if the hooked function was called directly by a game module; `owner` (optional) receives the
// module name. Must be called from the hook itself (frames of chocolateloading.dll at the top of the
// stack are skipped; one further down means the call came from our own code).
bool DirectGameCaller(char* owner = nullptr, size_t owner_len = 0);

} // namespace callsite
