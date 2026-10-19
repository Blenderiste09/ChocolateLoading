// A few worker threads that share the large memory copies of a save with the main thread. Every other
// thread of the game is suspended at that point, so the cores are free; the workers are created once,
// are left running by util::SuspendOtherThreads (ThreadIds), and never allocate, lock or log while they
// work. (A restore does not use them: its pages are resident and its copies are bandwidth bound.)
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

// memcpy of every range of a list, cut into pieces of at most 1 MB spread over the workers.
struct Copy { void* dst; const void* src; size_t len; };
void CopyAll(const Copy* list, int count);

} // namespace copypool
