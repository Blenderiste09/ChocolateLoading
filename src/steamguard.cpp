#define _CRT_SECURE_NO_WARNINGS
#include "steamguard.h"
#include "callsite.h"
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
bool         g_snapshot = false;     // a snapshot exists
CRITICAL_SECTION g_cs;

struct Cb { void* obj; int id; };
struct Cr { void* obj; uint64_t call; };
const int MAX_CB = 4096, MAX_CR = 1024;
Cb  g_cb[MAX_CB];       int g_ncb = 0;      // callbacks the game has registered now
Cb  g_saved[MAX_CB];    int g_nsaved = 0;   // ... and at save time
Cr  g_cr[MAX_CR];       int g_ncr = 0;      // call results the game issued since the save
Cb  g_work[MAX_CB];

int find_cb(const Cb* v, int n, void* obj, int id) {
    for (int i = 0; i < n; i++) if (v[i].obj == obj && v[i].id == id) return i;
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
    if (!obj || !g_snapshot || !callsite::DirectGameCaller()) return;
    EnterCriticalSection(&g_cs);
    if (g_ncr < MAX_CR) g_cr[g_ncr++] = Cr{ obj, call };
    else g_overflow = true;
    LeaveCriticalSection(&g_cs);
}

void __cdecl H_UnregisterCallResult(void* obj, uint64_t call) {
    if (obj && g_snapshot && callsite::DirectGameCaller()) {
        EnterCriticalSection(&g_cs);
        for (int i = 0; i < g_ncr;) {
            if (g_cr[i].obj == obj && g_cr[i].call == call) g_cr[i] = g_cr[--g_ncr];
            else i++;
        }
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

void OnSave() {
    if (!g_hooked) return;
    EnterCriticalSection(&g_cs);
    memcpy(g_saved, g_cb, g_ncb * sizeof(Cb));
    g_nsaved = g_ncb;
    g_ncr = 0;
    g_snapshot = true;
    LeaveCriticalSection(&g_cs);
    util::Log("steamguard: %d Steam callbacks registered by the game", g_nsaved);
}

void BeforeRestore() {
    if (!g_hooked || !g_snapshot) return;
    static Cr crs[MAX_CR];
    EnterCriticalSection(&g_cs);
    int ncr = g_ncr, ncb = 0;
    memcpy(crs, g_cr, ncr * sizeof(Cr));
    g_ncr = 0;
    for (int i = 0; i < g_ncb;) {   // registered since the save: drop them from the registry
        if (find_cb(g_saved, g_nsaved, g_cb[i].obj, g_cb[i].id) < 0) { g_work[ncb++] = g_cb[i]; g_cb[i] = g_cb[--g_ncb]; }
        else i++;
    }
    LeaveCriticalSection(&g_cs);
    // The objects still hold their current state, which steam_api reads to find them.
    for (int i = 0; i < ncr; i++) o_UnregisterCallResult(crs[i].obj, crs[i].call);
    for (int i = 0; i < ncb; i++) o_UnregisterCallback(g_work[i].obj);
    if (ncr || ncb) util::Log("steamguard: restore: %d call results and %d callbacks registered since the save withdrawn", ncr, ncb);
}

void AfterRestore() {
    if (!g_hooked || !g_snapshot) return;
    EnterCriticalSection(&g_cs);
    int n = 0;
    for (int i = 0; i < g_nsaved; i++)
        if (find_cb(g_cb, g_ncb, g_saved[i].obj, g_saved[i].id) < 0) g_work[n++] = g_saved[i];
    memcpy(g_cb, g_saved, g_nsaved * sizeof(Cb));
    g_ncb = g_nsaved;
    LeaveCriticalSection(&g_cs);
    // Unregistered since the save; the restored objects are registered ones again.
    for (int i = 0; i < n; i++) o_RegisterCallback(g_work[i].obj, g_work[i].id);
    if (n) util::Log("steamguard: restore: %d callbacks unregistered since the save registered again", n);
}

void OnDrop() {
    if (!g_hooked) return;
    EnterCriticalSection(&g_cs);
    g_snapshot = false;
    g_nsaved = 0;
    g_ncr = 0;
    LeaveCriticalSection(&g_cs);
}

} // namespace steamguard
