// Finds the arena blocks that threads reference from their own storage (TLS and FLS slots).
//
// Each game DLL links its own static CRT, whose per-thread data (_tiddata: errno, strtok state, locale
// references...) is allocated through tier0, hence in the arena, and is referenced only from the
// thread's FLS slot - per-thread kernel/ntdll state that is outside the snapshot. When a thread gets
// that block after a save (a new thread, or one that first enters a DLL's CRT then), a restore used to
// free it as a block "allocated after the snapshot"; the memory got reused, and the CRT read garbage
// when that thread exited or when the DLL was unloaded at "quit": an access violation in __freefls /
// __removelocaleref under the loader lock, which froze the game in its crash handlers.
//
// So a restore keeps the blocks allocated since the save that a live thread still references from
// its TLS slots (read from its TEB) or FLS slots (recorded by a FlsSetValue hook), plus the blocks those
// reference in turn (see snapshot.cpp).
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace tlsguard {

bool Install();   // hooks kernelbase!FlsSetValue; early loader, before the engine starts threads
bool Installed();

// Appends the TLS and FLS slot values of the given live threads (thread handles with
// THREAD_QUERY_LIMITED_INFORMATION access) and of the calling thread to `out`; returns how many were
// written. Lock-free: safe while the other threads are suspended.
size_t CollectRoots(const void* const* threads, int thread_count, uintptr_t* out, size_t max);

} // namespace tlsguard
