// Snapshot-aware heap ("arena"), a TLSF allocator living in one large reserved address range.
//
// memhook routes the engine's allocations here (all of them with the early loader). All allocator
// metadata lives inside the range, so copying [Base(), image end) captures the complete heap state and
// copying it back restores it. The range is reserved with MEM_WRITE_WATCH, so a restore only needs to
// copy back the pages written since the snapshot.
//
// Snapshot semantics (single active snapshot S):
//  * At S every allocated block gets the FROZEN flag.
//  * After S, frozen blocks are never reused: Free() of a frozen block is deferred,
//    Realloc() of a frozen block never grows in place. New blocks are carved only from space that
//    was free at S (or from heap growth past the S top), so the image of S is never overwritten by
//    live data that a restore would need to keep.
//  * Restore: blocks allocated after S that are still alive ("zombies") are either freed (default
//    with the early loader: nothing outside the snapshot can reference them) or kept allocated and
//    left untouched (plugin_load mode, where code outside the snapshot may still reference them).
//    Everything else is copied back from the image and the free lists are rebuilt around the kept
//    zombies.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace arena {

struct Range { uintptr_t start, end; };   // [start, end)

// --- lifetime -----------------------------------------------------------------------------
bool        Init(size_t reserve_bytes);    // reserve the address range (MEM_WRITE_WATCH)
uintptr_t   Base();
uintptr_t   ReservedEnd();
inline bool Contains(const void* p);

// --- allocation (thread safe) ---------------------------------------------------------------
void*  Alloc(size_t size);                 // nullptr when the arena is exhausted
void   Free(void* p);
void*  Realloc(void* p, size_t size);      // nullptr when exhausted (p left untouched)
size_t UsableSize(const void* p);

// --- statistics -----------------------------------------------------------------------------
struct Stats {
    size_t heap_bytes;      // heap span (start..top)
    size_t committed;       // committed bytes
    size_t used_bytes;      // bytes in allocated blocks (headers included)
    size_t used_blocks;
    size_t free_blocks;
    size_t deferred;        // pending deferred frees
    size_t frozen_blocks;
};
Stats  GetStats();                         // walks the heap (slow-ish, takes the lock)
bool   Check(char* err, size_t errlen);    // full consistency check (takes the lock)
uint64_t ChecksumAllocated();              // hash over every allocated block (addr+payload); lock held by caller

// --- snapshot support -----------------------------------------------------------------------
// All functions below require the caller to hold the arena lock (Lock/Unlock) and to guarantee
// that no other thread touches arena memory.
void   Lock();
void   Unlock();
bool   SnapshotActive();

// Prepare a new snapshot: apply deferred frees, freeze every allocated block, record the free
// ranges of S. Returns the end of the image range [Base(), image_end) that must be saved, or 0 if the
// bookkeeping could not be allocated (no snapshot is active then).
uintptr_t BeginSnapshot();
// Called after the image was saved: resets write tracking.
void   EndSnapshot();
// Drop the snapshot: apply deferred frees, unfreeze nothing (frozen flags are harmless).
void   DropSnapshot();

// Restore step 1: list post-snapshot live blocks (sorted by address, header included).
// The caller may compact the array to the blocks it wants to KEEP (e.g. blocks still referenced
// from outside the domain); every block not kept is freed by the restore.
// Returns nullptr and *count == (size_t)-1 when the heap is inconsistent.
Range* CollectZombies(size_t* count);
// Restore step 2 (caller): copy the image back, skipping the KEPT zombie ranges.
// Pages modified since the last snapshot/restore can be queried with DirtyPages().
size_t DirtyPages(uintptr_t start, uintptr_t end, uintptr_t* out, size_t max_out, bool* overflow);
// Restore step 3: rebuild allocator metadata around the kept zombies (the first `kept` entries
// of the CollectZombies array), clear deferred frees, reset write tracking.
bool   FinishRestore(size_t kept, char* err, size_t errlen);
// Address range that may contain post-snapshot blocks (for scans): [lo, hi) of the heap.
void   HeapSpan(uintptr_t* lo, uintptr_t* hi);

// Old-heap (non-arena) blocks freed by routed code while a snapshot exists are parked here.
// BeginSnapshot/DropSnapshot move them to a "pending" list; FlushPendingOld() really frees them
// with the original allocator (call it only when no thread is suspended).
typedef void (*OldFreeFn)(void* p);
void   DeferOldFree(void* p);              // thread safe
void   SetOldFreeFn(OldFreeFn fn);
size_t DeferredOldCount();
void   FlushPendingOld();

// internal fast-path data
extern uintptr_t g_lo, g_hi;
inline bool Contains(const void* p) { return (uintptr_t)p - g_lo < g_hi - g_lo; }

} // namespace arena
