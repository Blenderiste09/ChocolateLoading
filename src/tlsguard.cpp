#include "tlsguard.h"
#include "util.h"
#include <windows.h>
#include <intrin.h>

namespace tlsguard {

namespace {

// ---- FLS: every FlsSetValue of the process, as (thread, index) -> value -------------------------------
// Writers are serialised by a spin lock; CollectRoots reads without it (it runs while the writers are
// suspended), so an entry is published (its thread id written) only once complete. tid 0 = free entry.
struct FlsEntry { volatile DWORD tid; DWORD index; void* volatile value; };
const LONG MAX_FLS = 8192;
FlsEntry      g_fls[MAX_FLS];
volatile LONG g_fls_count = 0;   // high-water mark of the entries used
volatile LONG g_lock = 0;
volatile LONG g_lost = 0;        // values that could not be recorded

typedef BOOL (WINAPI* FlsSetValueFn)(DWORD, PVOID);
FlsSetValueFn o_FlsSetValue = nullptr;
bool g_installed = false;

bool thread_alive(DWORD tid) {
    HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
    if (!h) return false;
    DWORD code = 0;
    bool alive = GetExitCodeThread(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
}

void record(DWORD tid, DWORD index, void* value) {
    // Bounded spin: never wait for a writer that may be suspended (the value is lost instead).
    int spins = 0;
    while (InterlockedCompareExchange(&g_lock, 1, 0) != 0) {
        if (++spins > 1000000) { InterlockedIncrement(&g_lost); return; }
        YieldProcessor();
    }
    LONG n = g_fls_count, free_at = -1;
    for (LONG i = 0; i < n; i++) {
        if (g_fls[i].tid == tid && g_fls[i].index == index) { g_fls[i].value = value; InterlockedExchange(&g_lock, 0); return; }
        if (!g_fls[i].tid && free_at < 0) free_at = i;
    }
    if (value) {
        if (free_at < 0 && n == MAX_FLS) {   // full: forget the threads that are gone
            for (LONG i = 0; i < n; i++)
                if (g_fls[i].tid && !thread_alive(g_fls[i].tid)) g_fls[i].tid = 0;
            for (LONG i = 0; i < n && free_at < 0; i++) if (!g_fls[i].tid) free_at = i;
        }
        if (free_at < 0 && n < MAX_FLS) free_at = n;
        if (free_at < 0) {
            InterlockedIncrement(&g_lost);
        } else {
            g_fls[free_at].index = index;
            g_fls[free_at].value = value;
            MemoryBarrier();
            g_fls[free_at].tid = tid;
            if (free_at == n) g_fls_count = n + 1;
        }
    }
    InterlockedExchange(&g_lock, 0);
}

BOOL WINAPI H_FlsSetValue(DWORD index, PVOID value) {
    BOOL ok = o_FlsSetValue(index, value);
    if (ok) record(GetCurrentThreadId(), index, value);
    return ok;
}

// ---- TLS: read straight from each thread's TEB ----------------------------------------------------------
// x86 TEB: TlsSlots[64] at +0xE10, TlsExpansionSlots (1024 more) at +0xF94. Both offsets are checked
// against TlsSetValue at install time.
const size_t TEB_TLS_SLOTS = 0xE10, TEB_TLS_EXPANSION = 0xF94;
bool     g_tls_ok = false;
intptr_t g_teb_delta = 0;   // what to add to NtQueryInformationThread's TEB to get the 32-bit TEB

struct ThreadBasicInfo { LONG exit_status; void* teb; void* pid; void* tid; ULONG_PTR affinity; LONG prio, base_prio; };
typedef LONG (NTAPI* NtQueryInformationThreadFn)(HANDLE, int, void*, ULONG, ULONG*);
NtQueryInformationThreadFn g_query_thread = nullptr;

uintptr_t thread_teb(HANDLE h) {
    ThreadBasicInfo tbi = {};
    if (!g_query_thread || g_query_thread(h, 0 /* ThreadBasicInformation */, &tbi, sizeof(tbi), nullptr) < 0) return 0;
    return tbi.teb ? (uintptr_t)tbi.teb + g_teb_delta : 0;
}

void check_tls_layout() {
    g_query_thread = (NtQueryInformationThreadFn)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
    uintptr_t own = (uintptr_t)__readfsdword(0x18);   // NT_TIB.Self
    uintptr_t queried = thread_teb(GetCurrentThread());
    if (!queried) { util::Log("tlsguard: NtQueryInformationThread failed: TLS slots not read"); return; }
    g_teb_delta = (intptr_t)(own - queried);
    if (g_teb_delta != 0 && g_teb_delta != 0x2000 && g_teb_delta != -0x2000) {
        util::Log("tlsguard: unexpected TEB address (%p vs %p): TLS slots not read", (void*)queried, (void*)own);
        g_teb_delta = 0;
        return;
    }
    DWORD idx = TlsAlloc();
    if (idx == TLS_OUT_OF_INDEXES) return;
    void* marker = (void*)0x5EEDC0DE;
    TlsSetValue(idx, marker);
    void* seen = nullptr;
    if (idx < 64) seen = *(void**)(own + TEB_TLS_SLOTS + idx * 4);
    else if (void** ext = *(void***)(own + TEB_TLS_EXPANSION)) seen = ext[idx - 64];
    TlsFree(idx);
    g_tls_ok = seen == marker;
    if (!g_tls_ok) util::Log("tlsguard: TEB TLS layout check failed: TLS slots not read");
}

size_t add_tls_roots(uintptr_t teb, uintptr_t* out, size_t n, size_t max) {
    __try {
        void** slots = (void**)(teb + TEB_TLS_SLOTS);
        for (int i = 0; i < 64 && n < max; i++) if (slots[i]) out[n++] = (uintptr_t)slots[i];
        if (void** ext = *(void***)(teb + TEB_TLS_EXPANSION))
            for (int i = 0; i < 1024 && n < max; i++) if (ext[i]) out[n++] = (uintptr_t)ext[i];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

} // namespace

bool Install() {
    if (g_installed) return true;
    check_tls_layout();
    HMODULE kb = GetModuleHandleA("kernelbase.dll");
    void* fn = kb ? util::FindExport((uintptr_t)kb, "FlsSetValue") : nullptr;
    if (!fn || !util::InlineHookAt(fn, (void*)H_FlsSetValue, (void**)&o_FlsSetValue)) {
        util::Log("tlsguard: could not hook kernelbase!FlsSetValue: CRT per-thread data is NOT protected");
        return false;
    }
    g_installed = true;
    util::Log("tlsguard: hooked kernelbase!FlsSetValue, TEB TLS slots %s", g_tls_ok ? "readable" : "NOT readable");
    return true;
}

bool Installed() { return g_installed; }

size_t CollectRoots(const void* const* threads, int count, uintptr_t* out, size_t max) {
    size_t n = 0;
    DWORD tids[1025];
    int nt = 0;
    for (int i = -1; i < count && nt < 1025; i++) {
        HANDLE h = i < 0 ? GetCurrentThread() : (HANDLE)threads[i];
        tids[nt++] = i < 0 ? GetCurrentThreadId() : GetThreadId(h);
        if (g_tls_ok) {
            uintptr_t teb = i < 0 ? (uintptr_t)__readfsdword(0x18) : thread_teb(h);
            if (teb) n = add_tls_roots(teb, out, n, max);
        }
    }
    LONG fc = g_fls_count;
    for (LONG i = 0; i < fc && n < max; i++) {
        DWORD tid = g_fls[i].tid;
        void* v = g_fls[i].value;
        if (!tid || !v) continue;
        for (int k = 0; k < nt; k++)
            if (tids[k] == tid) { out[n++] = (uintptr_t)v; break; }
    }
    return n;
}

} // namespace tlsguard
