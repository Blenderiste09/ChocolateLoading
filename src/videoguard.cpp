#define _CRT_SECURE_NO_WARNINGS
#include "videoguard.h"
#include "pinset.h"
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
PinSet<256> g_videos;

void close_video(void* b) { o_close(b); }

void* __stdcall H_BinkOpen(const char* name, uint32_t flags) {
    void* b = o_open(name, flags);
    if (b) g_videos.Created(b);
    return b;
}

void __stdcall H_BinkClose(void* b) {
    if (b && g_videos.Destroyed(b)) o_close(b);
}

} // namespace

bool Hook(uintptr_t base) {
    if (g_hooked) return true;
    static bool init = false;
    if (!init) { g_videos.Init(); init = true; }
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

bool Hooked() { return g_hooked && !g_videos.Overflowed(); }

void OnSave(int slot) {
    if (!g_hooked) return;
    int n = g_videos.Pin(slot);
    if (n) util::Log("videoguard: %d open Bink videos pinned", n);
}

void BeforeRestore(int slot) {
    if (!g_hooked) return;
    int n = g_videos.Discard(slot, close_video);
    if (n) util::Log("videoguard: %d videos closed", n);
}

void AfterRestore(int slot) {
    if (!g_hooked) return;
    int n = g_videos.Revive(slot);
    if (n) util::Log("videoguard: %d videos closed since the save are open again", n);
}

void OnDrop(int slot) {
    if (!g_hooked) return;
    int n = g_videos.Unpin(slot, close_video);
    if (n) util::Log("videoguard: %d deferred video closes done", n);
}

} // namespace videoguard
