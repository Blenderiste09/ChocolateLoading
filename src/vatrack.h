// Tracks memory the engine reserves with a direct VirtualAlloc instead of through tier0's allocator.
// Above all this is the Source "hunk": a tier1 CMemoryStack (statically linked into engine.dll) that
// does VirtualAlloc(MEM_RESERVE, PAGE_NOACCESS) once and commits PAGE_READWRITE pages as it grows. It
// holds the BSP world data, e.g. every surface's head handle into CShadowMgr::m_ShadowDecals. That
// memory is game state, so it must be snapshotted and reverted together with the arena and the module
// .data: otherwise lists that span both (heads in the hunk, nodes in the arena) become inconsistent
// after a restore and the engine spins forever or dies with "CUtlLinkedList overflow".
//
// We inline-hook ntdll!NtAllocateVirtualMemory / NtFreeVirtualMemory (the single choke point under
// VirtualAlloc) and record a reservation only when it is a DIRECT VirtualAlloc by game code (see
// callsite.h). Heap growth (ntdll frames), the GPU driver, Steam and our own allocations are therefore
// never tracked. Caller classification is lock-free, so the hook is safe while other threads are
// suspended.
// snapshot.cpp reads the registry and captures/reverts the committed writable pages.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace vatrack {

// Installs the hooks (once) and sets the space separated list of game modules whose direct
// reservations are tracked. Must run before the engine creates the hunk (early loader: DllMain).
void Install(const char* module_list);
bool Installed();

struct Region { uintptr_t base; size_t size; };
// Copies up to `max` tracked regions (base + reserved size). Safe while other threads are suspended.
size_t GetRegions(Region* out, size_t max);
// True if a reservation could not be recorded because the registry was full: snapshots taken
// afterwards may be incomplete.
bool Overflowed();

// Logs every tracked region (owner module, base, reserved and committed size). It logs, so never
// call it while other threads are suspended.
void LogRegions();

} // namespace vatrack
