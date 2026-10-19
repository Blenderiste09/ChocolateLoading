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

void run_parts() {
    for (LONG i; (i = InterlockedIncrement(&g_next) - 1) < g_parts;) g_fn(g_ctx, (int)i);
}

DWORD WINAPI worker(void* arg) {
    HANDLE go = (HANDLE)arg;
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

namespace {
const size_t PIECE = 1u << 20;
const int    MAX_PIECES = 8192;
Copy         g_pieces[MAX_PIECES];

void copy_piece(void*, int part) {
    const Copy& p = g_pieces[part];
    memcpy(p.dst, p.src, p.len);
}
} // namespace

void CopyAll(const Copy* list, int count) {
    int n = 0;
    for (int i = 0; i < count; i++)
        for (size_t off = 0; off < list[i].len; off += PIECE) {
            size_t len = list[i].len - off < PIECE ? list[i].len - off : PIECE;
            if (n == MAX_PIECES) { Run(copy_piece, nullptr, n); n = 0; }
            g_pieces[n++] = Copy{ (uint8_t*)list[i].dst + off, (const uint8_t*)list[i].src + off, len };
        }
    if (n) Run(copy_piece, nullptr, n);
}

} // namespace copypool
