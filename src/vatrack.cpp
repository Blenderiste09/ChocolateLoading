#define _CRT_SECURE_NO_WARNINGS
#include "vatrack.h"
#include "callsite.h"
#include "util.h"
#include <windows.h>
#include <string.h>

namespace vatrack {

namespace {

// ---- registry of tracked reservations ---------------------------------------------------------
const size_t MAX_REGIONS = 512;
const size_t MIN_RESERVE = 0x10000;   // ignore sub-64 KB reservations (allocation granularity)

struct Reg { uintptr_t base; size_t size; char owner[24]; };
Reg              g_regs[MAX_REGIONS];
volatile LONG    g_reg_count = 0;
volatile LONG    g_overflow = 0;
CRITICAL_SECTION g_cs;                // serialises writers; readers never block (see GetRegions)
bool             g_cs_init = false;
bool             g_installed = false;

void ensure_cs() {
    if (!g_cs_init) { InitializeCriticalSection(&g_cs); g_cs_init = true; }
}

void add_region(uintptr_t base, size_t size, const char* owner) {
    EnterCriticalSection(&g_cs);
    LONG n = g_reg_count;
    for (LONG i = 0; i < n; i++)
        if (g_regs[i].base == base) { g_regs[i].size = size; LeaveCriticalSection(&g_cs); return; }
    if ((size_t)n < MAX_REGIONS) {
        g_regs[n].base = base;
        g_regs[n].size = size;
        strncpy(g_regs[n].owner, owner, sizeof(g_regs[n].owner) - 1);
        g_regs[n].owner[sizeof(g_regs[n].owner) - 1] = 0;
        MemoryBarrier();      // the entry is complete before it becomes visible to readers
        g_reg_count = n + 1;
    } else {
        InterlockedExchange(&g_overflow, 1);
    }
    LeaveCriticalSection(&g_cs);
}

void remove_region(uintptr_t base) {
    // Lock-free pre-check: almost every free in the process is for memory we do not track, and we
    // must not take the lock for those (a suspended thread may own it during a snapshot).
    LONG n = g_reg_count;
    bool found = false;
    for (LONG i = 0; i < n && !found; i++) found = g_regs[i].base == base;
    if (!found) return;
    EnterCriticalSection(&g_cs);
    n = g_reg_count;
    for (LONG i = 0; i < n; i++)
        if (g_regs[i].base == base) { g_regs[i] = g_regs[n - 1]; g_reg_count = n - 1; break; }
    LeaveCriticalSection(&g_cs);
}

// ---- ntdll hooks -------------------------------------------------------------------------------
typedef LONG (NTAPI* NtAllocFn)(HANDLE, PVOID*, ULONG_PTR, PSIZE_T, ULONG, ULONG);
typedef LONG (NTAPI* NtFreeFn)(HANDLE, PVOID*, PSIZE_T, ULONG);
NtAllocFn o_NtAlloc = nullptr;
NtFreeFn  o_NtFree  = nullptr;
const HANDLE CURRENT_PROCESS = (HANDLE)-1;

LONG NTAPI H_NtAllocateVirtualMemory(HANDLE proc, PVOID* base, ULONG_PTR zero_bits, PSIZE_T size, ULONG type, ULONG protect) {
    LONG status = o_NtAlloc(proc, base, zero_bits, size, type, protect);
    // Any protection: CMemoryStack reserves PAGE_NOACCESS and commits PAGE_READWRITE later.
    if (status >= 0 && (type & MEM_RESERVE) && proc == CURRENT_PROCESS && base && *base && size && *size >= MIN_RESERVE) {
        char owner[24];
        if (callsite::DirectGameCaller(owner, sizeof(owner))) add_region((uintptr_t)*base, *size, owner);
    }
    return status;
}

LONG NTAPI H_NtFreeVirtualMemory(HANDLE proc, PVOID* base, PSIZE_T size, ULONG type) {
    if ((type & MEM_RELEASE) && proc == CURRENT_PROCESS && base && *base) remove_region((uintptr_t)*base);
    return o_NtFree(proc, base, size, type);
}

size_t committed_bytes(uintptr_t base, size_t size) {
    size_t n = 0;
    uintptr_t a = base, end = base + size;
    MEMORY_BASIC_INFORMATION mbi;
    while (a < end && VirtualQuery((void*)a, &mbi, sizeof(mbi))) {
        uintptr_t e = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (e > end) e = end;
        if (mbi.State == MEM_COMMIT) n += e - a;
        a = e;
        if (!mbi.RegionSize) break;
    }
    return n;
}

} // namespace

void Install(const char* module_list) {
    ensure_cs();
    callsite::SetGameModules(module_list);
    if (g_installed) return;
    if (util::InlineHook("ntdll.dll", "NtAllocateVirtualMemory", (void*)H_NtAllocateVirtualMemory, (void**)&o_NtAlloc) &&
        util::InlineHook("ntdll.dll", "NtFreeVirtualMemory", (void*)H_NtFreeVirtualMemory, (void**)&o_NtFree)) {
        g_installed = true;
        util::Log("vatrack: hooked ntdll!NtAllocateVirtualMemory/NtFreeVirtualMemory (direct game reservations)");
    } else {
        // InlineHook refuses an already-patched function, so a retry never double-hooks.
        util::Log("vatrack: could not hook ntdll (alloc %s, free %s) - engine VirtualAlloc regions will NOT be captured",
                  o_NtAlloc ? "ok" : "failed", o_NtFree ? "ok" : "failed");
    }
}

bool Installed() { return g_installed; }
bool Overflowed() { return g_overflow != 0; }

size_t GetRegions(Region* out, size_t max) {
    // May run while other threads are suspended: never block on the lock (a suspended thread could
    // own it). If it stays busy, read without it - the writers are frozen, so the table is stable.
    bool locked = false;
    for (int i = 0; i < 200 && !(locked = TryEnterCriticalSection(&g_cs) != 0); i++) Sleep(0);
    LONG n = g_reg_count;
    size_t count = (size_t)n < max ? (size_t)n : max;
    for (size_t i = 0; i < count; i++) { out[i].base = g_regs[i].base; out[i].size = g_regs[i].size; }
    if (locked) LeaveCriticalSection(&g_cs);
    return count;
}

void LogRegions() {
    ensure_cs();
    EnterCriticalSection(&g_cs);
    LONG n = g_reg_count;
    size_t total = 0;
    for (LONG i = 0; i < n; i++) {
        size_t c = committed_bytes(g_regs[i].base, g_regs[i].size);
        total += c;
        util::Log("vatrack:   %-20s %08x  reserved %7u KB  committed %7u KB", g_regs[i].owner,
                  (unsigned)g_regs[i].base, (unsigned)(g_regs[i].size >> 10), (unsigned)(c >> 10));
    }
    LeaveCriticalSection(&g_cs);
    util::Log("vatrack: %d regions, %.1f MB committed%s", (int)n, total / 1048576.0,
              g_overflow ? " - REGISTRY OVERFLOWED, some regions are missing" : "");
}

} // namespace vatrack
