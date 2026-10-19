#define _CRT_SECURE_NO_WARNINGS
#include "soundguard.h"
#include "pinset.h"
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
PinSet<512> g_decoders;

void destroy(void* d) { o_destroy(g_vaudio, d); }

void* __fastcall H_Create(void* self, void*, void* events) {
    g_vaudio = self;
    void* d = o_create(self, events);
    if (d) g_decoders.Created(d);
    return d;
}

void __fastcall H_Destroy(void* self, void*, void* d) {
    g_vaudio = self;
    if (d && g_decoders.Destroyed(d)) o_destroy(self, d);
}

} // namespace

bool Hook(uintptr_t base, size_t size) {
    if (g_hooked) return true;
    static bool init = false;
    if (!init) { g_decoders.Init(); init = true; }
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

bool Hooked() { return g_hooked && !g_decoders.Overflowed(); }

void OnSave(int slot) {
    if (g_hooked) g_decoders.Pin(slot);
}

void BeforeRestore(int slot) {
    if (!g_hooked) return;
    int n = g_decoders.Discard(slot, destroy);
    if (n) util::Log("soundguard: destroyed %d MP3 decoder(s)", n);
}

void AfterRestore(int slot) {
    if (g_hooked) g_decoders.Revive(slot);
}

void OnDrop(int slot) {
    if (!g_hooked) return;
    int n = g_decoders.Unpin(slot, destroy);
    if (n) util::Log("soundguard: destroyed %d MP3 decoder(s)", n);
}

} // namespace soundguard
