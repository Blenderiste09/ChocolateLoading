#define _CRT_SECURE_NO_WARNINGS
#include "soundguard.h"
#include "util.h"
#include <windows.h>
#include <string.h>

namespace soundguard {

namespace {

// CVAudio vtable slots in this build (vaudio_miles!0x1000c374 in the RE copy): 0 deleting destructor,
// 1 CreateMP3StreamDecoder(IAudioStreamEvent*) -> IAudioStream*, 2 DestroyMP3StreamDecoder(IAudioStream*).
// Both are __thiscall with one stack argument ("ret 4"), which Hook() checks.
const int SLOT_CREATE = 1, SLOT_DESTROY = 2;
typedef void* (__thiscall* CreateFn)(void* self, void* events);
typedef void  (__thiscall* DestroyFn)(void* self, void* decoder);
CreateFn  o_create = nullptr;
DestroyFn o_destroy = nullptr;
void*     g_vaudio = nullptr;       // the engine's CVAudio instance (learnt from the hook calls)
bool      g_hooked = false;

struct Decoder {
    void* p;
    bool  pinned;      // referenced by the current snapshot
    bool  destroyed;   // destroyed by the game after the snapshot, destruction deferred
};
const int MAX_DECODERS = 512;
Decoder          g_dec[MAX_DECODERS];
int              g_count = 0;
CRITICAL_SECTION g_cs;

int find(void* p) {
    for (int i = 0; i < g_count; i++) if (g_dec[i].p == p) return i;
    return -1;
}

void remove_at(int i) { g_dec[i] = g_dec[--g_count]; }

void* __fastcall H_Create(void* self, void*, void* events) {
    g_vaudio = self;
    void* d = o_create(self, events);
    if (d) {
        EnterCriticalSection(&g_cs);
        if (g_count < MAX_DECODERS) g_dec[g_count++] = Decoder{ d, false, false };
        else util::Log("soundguard: too many live MP3 decoders, %p is not tracked", d);
        LeaveCriticalSection(&g_cs);
    }
    return d;
}

void __fastcall H_Destroy(void* self, void*, void* d) {
    g_vaudio = self;
    if (!d) return;
    EnterCriticalSection(&g_cs);
    int i = find(d);
    if (i >= 0 && g_dec[i].pinned) {   // the snapshot still needs it: keep it alive
        g_dec[i].destroyed = true;
        LeaveCriticalSection(&g_cs);
        return;
    }
    if (i >= 0) remove_at(i);
    LeaveCriticalSection(&g_cs);
    o_destroy(self, d);
}

// Removes the entries matching `pred` and destroys their decoders (outside the lock).
template <class Pred> void destroy_where(Pred pred) {
    void* doomed[MAX_DECODERS];
    int n = 0;
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < g_count;)
        if (pred(g_dec[i])) { doomed[n++] = g_dec[i].p; remove_at(i); }
        else i++;
    LeaveCriticalSection(&g_cs);
    for (int i = 0; i < n; i++) o_destroy(g_vaudio, doomed[i]);
    if (n) util::Log("soundguard: destroyed %d MP3 decoder(s)", n);
}

} // namespace

bool Hook(uintptr_t base, size_t size) {
    static bool cs_ready = false;
    if (g_hooked) return true;
    if (!cs_ready) { InitializeCriticalSection(&g_cs); cs_ready = true; }
    util::Module m;
    memset(&m, 0, sizeof(m));
    m.base = base;
    m.end = base + size;
    void** vt = (void**)util::FindVTableByRTTI(m, ".?AVCVAudio@@");
    if (!vt) { util::Log("soundguard: CVAudio vtable not found"); return false; }
    if (util::FirstRetImm(vt[SLOT_CREATE]) != 4 || util::FirstRetImm(vt[SLOT_DESTROY]) != 4) {
        util::Log("soundguard: unexpected CVAudio layout, not hooked");
        return false;
    }
    o_create = (CreateFn)vt[SLOT_CREATE];
    o_destroy = (DestroyFn)vt[SLOT_DESTROY];
    // Destroy first: it must handle decoders before Create starts tracking them.
    if (!util::WritePointer(&vt[SLOT_DESTROY], (void*)H_Destroy) || !util::WritePointer(&vt[SLOT_CREATE], (void*)H_Create)) {
        util::Log("soundguard: vtable patch failed");
        return false;
    }
    g_hooked = true;
    util::Log("soundguard: MP3 decoder lifetime hooked (CVAudio vtable %p)", vt);
    return true;
}

bool Hooked() { return g_hooked; }

void OnSave() {
    if (!g_hooked) return;
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < g_count; i++) g_dec[i].pinned = true;
    LeaveCriticalSection(&g_cs);
}

void BeforeRestore() {
    if (g_hooked) destroy_where([](const Decoder& d) { return !d.pinned; });
}

void AfterRestore() {
    if (!g_hooked) return;
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < g_count; i++) g_dec[i].destroyed = false;
    LeaveCriticalSection(&g_cs);
}

void OnDrop() {
    if (!g_hooked) return;
    destroy_where([](const Decoder& d) { return d.destroyed; });
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < g_count; i++) g_dec[i].pinned = false;
    LeaveCriticalSection(&g_cs);
}

} // namespace soundguard
