// A few worker threads that share the large memory copies of a save with the main thread. Every other
// thread of the game is suspended at that point, so the cores are free; the workers are created once,
// are left running by util::SuspendOtherThreads (ThreadIds), and never allocate, lock or log while they
// work. A restore uses them for the module sections, the VirtualAlloc segments and whole arena images;
// the dirty pages of the arena are copied by the calling thread alone (they are resident and scattered,
// that copy is bound by memory bandwidth).
#pragma once
#include <windows.h>

namespace copypool {

bool Start(int workers);        // creates the idle workers (0: everything runs on the calling thread)
int  Workers();
const DWORD* ThreadIds();       // the workers' thread ids (Workers() of them)

// Calls fn(ctx, part) for every part in [0, parts), spread over the workers and the calling thread, and
// returns once all of them are done. fn must be safe to run concurrently on different parts.
typedef void (*PartFn)(void* ctx, int part);
void Run(PartFn fn, void* ctx, int parts);

} // namespace copypool
