#define _CRT_SECURE_NO_WARNINGS
#include "steamguard.h"
#include "callsite.h"
#include "slotmask.h"
#include "util.h"
#include <windows.h>
#include <string.h>

namespace steamguard {

namespace {

// steam_api.dll exports (S_API = extern "C" __cdecl). SteamAPICall_t is a uint64 passed by value.
typedef void (__cdecl* RegCbFn)(void* obj, int id);
typedef void (__cdecl* UnregCbFn)(void* obj);
typedef void (__cdecl* CallResultFn)(void* obj, uint64_t call);
RegCbFn      o_RegisterCallback = nullptr;
UnregCbFn    o_UnregisterCallback = nullptr;
CallResultFn o_RegisterCallResult = nullptr;
CallResultFn o_UnregisterCallResult = nullptr;
bool         g_hooked = false;
bool         g_overflow = false;
CRITICAL_SECTION g_cs;

struct Cb { void* obj; int id; };
struct Cr { void* obj; uint64_t call; };
const int MAX_CB = 4096, MAX_CR = 1024, MAX_SLOTS = MAX_SNAPSHOT_SLOTS;

// What the game has registered in the current timeline. Call results are never removed when steam_api
// delivers them (that happens inside steam_api), so the oldest are forgotten when the list is full: by
// then they have long been delivered, and withdrawing a delivered one is a no-op anyway.
Cb  g_cb[MAX_CB];  int g_ncb = 0;
Cr  g_cr[MAX_CR];  int g_ncr = 0;

// The same, as each snapshot saw it.
struct Slot { Cb* cb; int ncb; Cr* cr; int ncr; };
Slot g_slot[MAX_SLOTS];
Cb   g_work_cb[MAX_CB];
Cr   g_work_cr[MAX_CR];

int find_cb(const Cb* v, int n, void* obj, int id) {
    for (int i = 0; i < n; i++) if (v[i].obj == obj && v[i].id == id) return i;
    return -1;
}

int find_cr(const Cr* v, int n, void* obj, uint64_t call) {
    for (int i = 0; i < n; i++) if (v[i].obj == obj && v[i].call == call) return i;
    return -1;
}

void __cdecl H_RegisterCallback(void* obj, int id) {
    o_RegisterCallback(obj, id);
    if (!obj || !callsite::DirectGameCaller()) return;   // steam_api registers its own objects too
    EnterCriticalSection(&g_cs);
    if (find_cb(g_cb, g_ncb, obj, id) < 0) {
        if (g_ncb < MAX_CB) g_cb[g_ncb++] = Cb{ obj, id };
        else g_overflow = true;
    }
    LeaveCriticalSection(&g_cs);
}

void __cdecl H_UnregisterCallback(void* obj) {
    if (obj && callsite::DirectGameCaller()) {
        EnterCriticalSection(&g_cs);
        for (int i = 0; i < g_ncb;) {
            if (g_cb[i].obj == obj) g_cb[i] = g_cb[--g_ncb];
            else i++;
        }
        LeaveCriticalSection(&g_cs);
    }
    o_UnregisterCallback(obj);
}

void __cdecl H_RegisterCallResult(void* obj, uint64_t call) {
    o_RegisterCallResult(obj, call);
    if (!obj || !callsite::DirectGameCaller()) return;
    EnterCriticalSection(&g_cs);
    if (g_ncr == MAX_CR) memmove(g_cr, g_cr + 1, --g_ncr * sizeof(Cr));   // forget the oldest
    g_cr[g_ncr++] = Cr{ obj, call };
    LeaveCriticalSection(&g_cs);
}

void __cdecl H_UnregisterCallResult(void* obj, uint64_t call) {
    if (obj && callsite::DirectGameCaller()) {
        EnterCriticalSection(&g_cs);
        int i = find_cr(g_cr, g_ncr, obj, call);
        if (i >= 0) memmove(g_cr + i, g_cr + i + 1, (--g_ncr - i) * sizeof(Cr));
        LeaveCriticalSection(&g_cs);
    }
    o_UnregisterCallResult(obj, call);
}

struct Hook { const char* name; void* fn; void** orig; };
const Hook kHooks[] = {
    { "SteamAPI_RegisterCallback",     (void*)H_RegisterCallback,     (void**)&o_RegisterCallback },
    { "SteamAPI_UnregisterCallback",   (void*)H_UnregisterCallback,   (void**)&o_UnregisterCallback },
    { "SteamAPI_RegisterCallResult",   (void*)H_RegisterCallResult,   (void**)&o_RegisterCallResult },
    { "SteamAPI_UnregisterCallResult", (void*)H_UnregisterCallResult, (void**)&o_UnregisterCallResult },
};

bool valid_slot(int slot) { return g_hooked && slot >= 0 && slot < MAX_SLOTS; }

} // namespace

bool Hook(uintptr_t base) {
    if (g_hooked) return true;
    InitializeCriticalSection(&g_cs);
    void* fns[4];
    for (int i = 0; i < 4; i++) {
        fns[i] = util::FindExport(base, kHooks[i].name);
        if (!fns[i]) { util::Log("steamguard: steam_api.dll!%s not found, Steam callbacks NOT guarded", kHooks[i].name); return false; }
    }
    for (int i = 0; i < 4; i++)
        if (!util::InlineHookAt(fns[i], kHooks[i].fn, kHooks[i].orig)) {
            // The ones hooked so far only track: without g_hooked nothing is acted upon.
            util::Log("steamguard: could not hook steam_api.dll!%s, Steam callbacks NOT guarded", kHooks[i].name);
            return false;
        }
    g_hooked = true;
    util::Log("steamguard: steam_api callback registration hooked");
    return true;
}

bool Hooked() { return g_hooked && !g_overflow; }

void OnSave(int slot) {
    if (!valid_slot(slot)) return;
    Slot& s = g_slot[slot];
    if (!s.cb) {
        s.cb = (Cb*)VirtualAlloc(nullptr, MAX_CB * sizeof(Cb) + MAX_CR * sizeof(Cr), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!s.cb) { g_overflow = true; return; }
        s.cr = (Cr*)(s.cb + MAX_CB);
    }
    EnterCriticalSection(&g_cs);
    memcpy(s.cb, g_cb, g_ncb * sizeof(Cb));
    s.ncb = g_ncb;
    memcpy(s.cr, g_cr, g_ncr * sizeof(Cr));
    s.ncr = g_ncr;
    LeaveCriticalSection(&g_cs);
    util::Log("steamguard: %d Steam callbacks registered by the game", s.ncb);
}

void BeforeRestore(int slot) {
    if (!valid_slot(slot) || !g_slot[slot].cb) return;
    const Slot& s = g_slot[slot];
    int ncr = 0, ncb = 0;
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < g_ncr; i++)   // issued in the current timeline only
        if (find_cr(s.cr, s.ncr, g_cr[i].obj, g_cr[i].call) < 0) g_work_cr[ncr++] = g_cr[i];
    for (int i = 0; i < g_ncb; i++)   // registered in the current timeline only
        if (find_cb(s.cb, s.ncb, g_cb[i].obj, g_cb[i].id) < 0) g_work_cb[ncb++] = g_cb[i];
    LeaveCriticalSection(&g_cs);
    // The objects still hold their current state, which steam_api reads to find them.
    for (int i = 0; i < ncr; i++) o_UnregisterCallResult(g_work_cr[i].obj, g_work_cr[i].call);
    for (int i = 0; i < ncb; i++) o_UnregisterCallback(g_work_cb[i].obj);
    if (ncr || ncb) util::Log("steamguard: restore: %d call results and %d callbacks of the current timeline withdrawn", ncr, ncb);
}

void AfterRestore(int slot) {
    if (!valid_slot(slot) || !g_slot[slot].cb) return;
    const Slot& s = g_slot[slot];
    int n = 0;
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < s.ncb; i++)
        if (find_cb(g_cb, g_ncb, s.cb[i].obj, s.cb[i].id) < 0) g_work_cb[n++] = s.cb[i];
    memcpy(g_cb, s.cb, s.ncb * sizeof(Cb));
    g_ncb = s.ncb;
    memcpy(g_cr, s.cr, s.ncr * sizeof(Cr));
    g_ncr = s.ncr;
    LeaveCriticalSection(&g_cs);
    // Registered in the restored timeline but not any more: the restored objects are registered ones.
    for (int i = 0; i < n; i++) o_RegisterCallback(g_work_cb[i].obj, g_work_cb[i].id);
    if (n) util::Log("steamguard: restore: %d callbacks registered again", n);
}

void OnDrop(int slot) {
    if (!valid_slot(slot)) return;
    Slot& s = g_slot[slot];
    if (s.cb) VirtualFree(s.cb, 0, MEM_RELEASE);   // cr shares that allocation
    s = Slot{};
}

} // namespace steamguard
