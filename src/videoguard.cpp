#define _CRT_SECURE_NO_WARNINGS
#include "videoguard.h"
#include "util.h"
#include <windows.h>

namespace videoguard {

namespace {

// binkw32.dll exports (__stdcall): HBINK BinkOpen(const char* name, U32 flags), void BinkClose(HBINK).
typedef void* (__stdcall* OpenFn)(const char* name, uint32_t flags);
typedef void  (__stdcall* CloseFn)(void* bink);
OpenFn  o_open = nullptr;
CloseFn o_close = nullptr;
bool    g_hooked = false;
bool    g_overflow = false;
CRITICAL_SECTION g_cs;

struct Video {
    void* bink;
    bool  pinned;      // open at save time: the snapshot refers to it
    bool  closed;      // closed by the game since the save, close deferred
};
const int MAX_VIDEOS = 256;
Video g_vid[MAX_VIDEOS];
int   g_count = 0;

int find(void* b) {
    for (int i = 0; i < g_count; i++) if (g_vid[i].bink == b) return i;
    return -1;
}

void* __stdcall H_BinkOpen(const char* name, uint32_t flags) {
    void* b = o_open(name, flags);
    if (b) {
        EnterCriticalSection(&g_cs);
        int i = find(b);   // an address reused after a close we did not see: a new video
        if (i < 0 && g_count < MAX_VIDEOS) i = g_count++;
        if (i >= 0) g_vid[i] = Video{ b, false, false };
        else g_overflow = true;
        LeaveCriticalSection(&g_cs);
    }
    return b;
}

void __stdcall H_BinkClose(void* b) {
    if (!b) return;
    EnterCriticalSection(&g_cs);
    int i = find(b);
    if (i >= 0 && g_vid[i].pinned) {   // the snapshot still needs it: keep it open
        g_vid[i].closed = true;
        LeaveCriticalSection(&g_cs);
        return;
    }
    if (i >= 0) g_vid[i] = g_vid[--g_count];
    LeaveCriticalSection(&g_cs);
    o_close(b);
}

// Removes the entries matching `pred` and closes their videos (outside the lock).
template <class Pred> int close_where(Pred pred) {
    void* doomed[MAX_VIDEOS];
    int n = 0;
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < g_count;)
        if (pred(g_vid[i])) { doomed[n++] = g_vid[i].bink; g_vid[i] = g_vid[--g_count]; }
        else i++;
    LeaveCriticalSection(&g_cs);
    for (int i = 0; i < n; i++) o_close(doomed[i]);
    return n;
}

} // namespace

bool Hook(uintptr_t base) {
    if (g_hooked) return true;
    InitializeCriticalSection(&g_cs);
    void* open = util::FindExport(base, "_BinkOpen@8");
    void* close = util::FindExport(base, "_BinkClose@4");
    // BinkClose first: it must handle videos before BinkOpen starts tracking them.
    if (!open || !close || !util::InlineHookAt(close, (void*)H_BinkClose, (void**)&o_close) ||
        !util::InlineHookAt(open, (void*)H_BinkOpen, (void**)&o_open)) {
        util::Log("videoguard: could not hook binkw32.dll BinkOpen/BinkClose: videos NOT guarded");
        return false;
    }
    g_hooked = true;
    util::Log("videoguard: Bink video lifetime hooked");
    return true;
}

bool Hooked() { return g_hooked && !g_overflow; }

void OnSave() {
    if (!g_hooked) return;
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < g_count; i++) g_vid[i].pinned = true;
    int n = g_count;
    LeaveCriticalSection(&g_cs);
    if (n) util::Log("videoguard: %d open Bink videos pinned", n);
}

void BeforeRestore() {
    if (!g_hooked) return;
    int n = close_where([](const Video& v) { return !v.pinned; });
    if (n) util::Log("videoguard: %d videos opened since the save closed", n);
}

void AfterRestore() {
    if (!g_hooked) return;
    EnterCriticalSection(&g_cs);
    int revived = 0;
    for (int i = 0; i < g_count; i++) if (g_vid[i].closed) { g_vid[i].closed = false; revived++; }
    LeaveCriticalSection(&g_cs);
    if (revived) util::Log("videoguard: %d videos closed since the save are open again", revived);
}

void OnDrop() {
    if (!g_hooked) return;
    int n = close_where([](const Video& v) { return v.pinned && v.closed; });
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < g_count; i++) g_vid[i].pinned = false;
    LeaveCriticalSection(&g_cs);
    if (n) util::Log("videoguard: %d deferred video closes done", n);
}

} // namespace videoguard
