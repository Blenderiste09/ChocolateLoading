#include "copypool.h"
#include "util.h"
#include <string.h>

namespace copypool {

namespace {

const int MAX_WORKERS = 8;
HANDLE g_go[MAX_WORKERS];         // auto-reset: one job for that worker
DWORD  g_ids[MAX_WORKERS];
int    g_workers = 0;

// The current job. Parts are handed out with an atomic counter; each worker reports when it ran out.
PartFn        g_fn = nullptr;
void*         g_ctx = nullptr;
int           g_parts = 0;
volatile LONG g_next = 0;
volatile LONG g_idle = 0;         // workers done with the current job
volatile LONG g_started = 0;      // workers that got to their loop

void run_parts() {
    for (LONG i; (i = InterlockedIncrement(&g_next) - 1) < g_parts;) g_fn(g_ctx, (int)i);
}

DWORD WINAPI worker(void* arg) {
    HANDLE go = (HANDLE)arg;
    InterlockedIncrement(&g_started);
    for (;;) {
        WaitForSingleObject(go, INFINITE);
        run_parts();
        InterlockedIncrement(&g_idle);
    }
}

} // namespace

bool Start(int workers) {
    if (g_workers) return true;
    if (workers > MAX_WORKERS) workers = MAX_WORKERS;
    for (int i = 0; i < workers; i++) {
        g_go[i] = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        HANDLE t = g_go[i] ? CreateThread(nullptr, 64 * 1024, worker, g_go[i], 0, &g_ids[i]) : nullptr;
        if (!t) {
            if (g_go[i]) CloseHandle(g_go[i]);
            break;
        }
        SetThreadPriority(t, THREAD_PRIORITY_HIGHEST);
        CloseHandle(t);
        g_workers++;
    }
    // A new thread first runs every DLL's thread attach (loader lock, tier0's own lock...). The first save
    // creates the pool right before it suspends every other thread: a worker still in its attach would
    // wait forever for a lock held by a suspended thread, and Run() would spin waiting for it (seen: the
    // game frozen in Run, two workers blocked in LdrInitializeThunk / tier0_s). Wait for them to start.
    for (int ms = 0; g_started < g_workers && ms < 5000; ms++) Sleep(1);
    if (g_started < g_workers) {
        // which ones are late is unknown: hand jobs to none (everything runs on the calling thread)
        util::Log("copypool: only %d of %d copy workers started, not using them", (int)g_started, g_workers);
        g_workers = 0;
    }
    util::Log("copypool: %d copy workers", g_workers);
    return g_workers == workers;
}

int Workers() { return g_workers; }
const DWORD* ThreadIds() { return g_ids; }

void Run(PartFn fn, void* ctx, int parts) {
    g_fn = fn;
    g_ctx = ctx;
    g_parts = parts;
    g_next = 0;
    g_idle = 0;
    MemoryBarrier();
    int woken = parts - 1 < g_workers ? parts - 1 : g_workers;   // the calling thread takes a part too
    for (int i = 0; i < woken; i++) SetEvent(g_go[i]);
    run_parts();
    while (g_idle < woken) YieldProcessor();
}

} // namespace copypool
