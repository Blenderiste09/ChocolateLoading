// Redirects tier0's allocator (g_pMemAlloc, the IMemAlloc every Source module uses) and the
// KeyValuesSystem allocator into the snapshot arena.
//
// Two modes:
//  * route-all (early loader): installed by InstallEarly() the moment tier0.dll is mapped, before any
//    engine allocation exists, and EVERY allocation goes to the arena - the whole engine heap.
//  * domain (plugin_load fallback): installed late by Install(); only allocations whose return address
//    lies in a "domain" module (SetDomain) go to the arena, the rest stays on tier0's heap.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace memhook {

// Early-loader path: patches tier0's CStdMemAlloc vtable (found through RTTI) right after tier0.dll is
// mapped and before its static initialisation, reserves the arena and turns route-all on. Call it
// from a DLL-load notification. Install() completes the setup later (KeyValues hook).
bool InstallEarly(uintptr_t tier0_base, size_t tier0_size, size_t arena_reserve_bytes, char* err, size_t errlen);

// Hooks g_pMemAlloc (unless InstallEarly already did) and the KeyValuesSystem allocator; tier0 and
// vstdlib must be loaded and initialised.
bool Install(size_t arena_reserve_bytes, char* err, size_t errlen);
bool Installed();

// Route every allocation to the arena regardless of the caller (see the modes above).
void SetRouteAll(bool on);
bool RouteAll();

// Domain mode: rebuilds the routing table from a space separated module list
// (e.g. "engine.dll client.dll server.dll"). Modules that are not loaded yet are ignored.
void SetDomain(const char* module_list);
const char* DomainDescription();      // the routed modules that were found

// Diagnostic: put the CRTs' per-thread data in the arena like everything else (see memhook.cpp). It
// reproduces the "freeze on quit" bug; never use it otherwise.
void SetCrtThreadDataInArena(bool on);

// True once the arena could not satisfy a routed allocation (it then went to tier0's heap):
// snapshots taken after that point may be incomplete.
bool Overflowed();

} // namespace memhook
