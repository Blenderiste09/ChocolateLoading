#define _CRT_SECURE_NO_WARNINGS
#include "d3dguard.h"
#include "slotmask.h"
#include "util.h"
#include <windows.h>
#include <intrin.h>
#include <stdlib.h>

#pragma intrinsic(_ReturnAddress)

namespace d3dguard {

namespace {

// IUnknown / IDirect3D9 / IDirect3DDevice9 vtable slots: the COM order of d3d9.h (DXVK implements the
// same interfaces). Every method is __stdcall with `this` first.
const int SLOT_ADDREF = 1, SLOT_RELEASE = 2;
const int D3D_CREATEDEVICE = 16;
enum {
    DEV_Reset = 16, DEV_CreateTexture = 23, DEV_CreateVolumeTexture = 24, DEV_CreateCubeTexture = 25,
    DEV_CreateVertexBuffer = 26, DEV_CreateIndexBuffer = 27, DEV_CreateRenderTarget = 28,
    DEV_CreateDepthStencilSurface = 29, DEV_CreateOffscreenPlainSurface = 36, DEV_CreateStateBlock = 59,
    DEV_EndStateBlock = 61, DEV_CreateVertexDeclaration = 86, DEV_CreateVertexShader = 91,
    DEV_CreatePixelShader = 106, DEV_CreateQuery = 118,
};

enum Kind { K_TEXTURE, K_VOLUME, K_CUBE, K_VB, K_IB, K_SURFACE, K_STATEBLOCK, K_DECL, K_VS, K_PS, K_QUERY, K_COUNT };

typedef ULONG (__stdcall* RefFn)(void* obj);
inline ULONG add_ref(void* o) { return (*(RefFn**)o)[SLOT_ADDREF](o); }
inline ULONG release(void* o) { return (*(RefFn**)o)[SLOT_RELEASE](o); }   // through H_Release

bool      g_active = false;      // the engine's device is hooked
void*     g_device = nullptr;    // the engine's IDirect3DDevice9
uintptr_t g_api_lo = 0, g_api_hi = 0;   // image of the shader API (shaderapidx9.dll / shaderapivk.dll)
CRITICAL_SECTION g_cs;           // guards the registry and the class table writers

// ---- registry of the engine's live objects: open addressing (linear probing), under g_cs -----------
struct Obj { void* p; SlotMask mask; uint8_t kind; uint8_t pad[3]; };   // mask: one bit per snapshot pinning it
const size_t CAP = 1u << 17;
const size_t MAX_LIVE = CAP / 4 * 3;
Obj*   g_tab = nullptr;
Obj*   g_work = nullptr;         // scratch copy used while adjusting reference counts
size_t g_live = 0;
bool   g_overflow = false;

inline size_t home(void* p) { return (size_t)((((uintptr_t)p >> 3) * 2654435761u) & (CAP - 1)); }

void insert(void* p, int kind) {
    size_t i = home(p);
    while (g_tab[i].p && g_tab[i].p != p) i = (i + 1) & (CAP - 1);
    if (!g_tab[i].p) {
        if (g_live >= MAX_LIVE) { g_overflow = true; return; }
        g_live++;
    }
    g_tab[i] = Obj{ p, 0, (uint8_t)kind, { 0, 0, 0 } };   // a reused address is a new object
}

void erase(void* p) {
    size_t i = home(p);
    for (;; i = (i + 1) & (CAP - 1)) {
        if (!g_tab[i].p) return;
        if (g_tab[i].p == p) break;
    }
    // Backward-shift deletion: move up the following entries whose home slot is not in (i, j].
    for (size_t j = i;;) {
        j = (j + 1) & (CAP - 1);
        if (!g_tab[j].p) break;
        size_t k = home(g_tab[j].p);
        bool move = i <= j ? (k <= i || k > j) : (k <= i && k > j);
        if (move) { g_tab[i] = g_tab[j]; i = j; }
    }
    g_tab[i].p = nullptr;
    g_live--;
}

Obj* find(void* p) {   // under g_cs
    for (size_t i = home(p);; i = (i + 1) & (CAP - 1)) {
        if (!g_tab[i].p) return nullptr;
        if (g_tab[i].p == p) return &g_tab[i];
    }
}

// Copies the live entries (those with a bit of `mask_filter` set, or all when 0) to g_work, clearing the
// bits of `unpin` from them, so that AddRef/Release can then be called without the lock held.
size_t snapshot_entries(SlotMask mask_filter, SlotMask unpin) {
    size_t n = 0;
    EnterCriticalSection(&g_cs);
    for (size_t i = 0; i < CAP; i++) {
        if (!g_tab[i].p || (mask_filter && !(g_tab[i].mask & mask_filter))) continue;
        g_work[n++] = g_tab[i];
        g_tab[i].mask &= ~unpin;
    }
    LeaveCriticalSection(&g_cs);
    return n;
}

// ---- per snapshot: the engine's reference count of every object it pins, sorted by address -------------
const int MAX_SLOTS = MAX_SNAPSHOT_SLOTS;
struct Saved { void* p; LONG refs; };
struct SlotObjs { Saved* v; size_t n, cap; bool intact; };
SlotObjs g_slot[MAX_SLOTS];
SlotMask g_pinned_slots = 0;     // slots that hold pins

LONG saved_refs(int slot, void* p) {
    const SlotObjs& s = g_slot[slot];
    size_t lo = 0, hi = s.n;
    while (lo < hi) { size_t mid = (lo + hi) / 2; if (s.v[mid].p < p) lo = mid + 1; else hi = mid; }
    return lo < s.n && s.v[lo].p == p ? s.v[lo].refs : 0;
}

// ---- Release of each object class: the moment an object dies it leaves the registry -----------------
struct Cls { void** vtable; RefFn release; };
Cls           g_cls[32];
volatile LONG g_cls_count = 0;   // entries are published before their vtable is patched

ULONG __stdcall H_Release(void* obj) {
    void** vt = *(void***)obj;
    RefFn orig = nullptr;
    for (LONG i = 0, n = g_cls_count; i < n && !orig; i++) if (g_cls[i].vtable == vt) orig = g_cls[i].release;
    if (!orig) return 1;   // cannot happen: only registered vtables are patched
    ULONG r = orig(obj);
    if (r == 0) {
        EnterCriticalSection(&g_cs);
        erase(obj);
        LeaveCriticalSection(&g_cs);
    }
    return r;
}

// Under g_cs. Hooks the Release of the object's class the first time an object of that class is seen.
// False if it cannot: such an object must not be tracked (its death would go unnoticed).
bool hook_class(void* obj) {
    void** vt = *(void***)obj;
    LONG n = g_cls_count;
    for (LONG i = 0; i < n; i++) if (g_cls[i].vtable == vt) return true;
    if (n == 32 || vt[SLOT_RELEASE] == (void*)H_Release) return false;
    g_cls[n].vtable = vt;
    g_cls[n].release = (RefFn)vt[SLOT_RELEASE];
    MemoryBarrier();
    g_cls_count = n + 1;
    return util::WritePointer(&vt[SLOT_RELEASE], (void*)H_Release);
}

inline bool engine_call(void* dev, void* ret) {
    return dev == g_device && (uintptr_t)ret - g_api_lo < g_api_hi - g_api_lo;
}

void track(void* obj, int kind) {
    if (!obj) return;
    EnterCriticalSection(&g_cs);
    if (hook_class(obj)) insert(obj, kind);
    else g_overflow = true;   // an untracked object: the snapshot can no longer be guaranteed
    LeaveCriticalSection(&g_cs);
}

// Releases the pins of the snapshot in `slot`: objects nothing else holds are destroyed.
void drop_slot(int slot) {
    SlotMask bit = SlotBit(slot);
    if (!(g_pinned_slots & bit)) return;
    size_t n = snapshot_entries(bit, bit);
    for (size_t i = 0; i < n; i++) release(g_work[i].p);
    g_pinned_slots &= ~bit;
    g_slot[slot].n = 0;
}

// Before a device reset or a new device: every pin goes, and those snapshots can no longer be restored.
void drop_all_pins(const char* why) {
    if (!g_pinned_slots) return;
    util::Log("d3dguard: %s while snapshots pin objects: pins released, those snapshots can no longer be restored", why);
    for (int s = 0; s < MAX_SLOTS; s++)
        if (g_pinned_slots & SlotBit(s)) { drop_slot(s); g_slot[s].intact = false; }
}

// ---- device hooks ------------------------------------------------------------------------------------
typedef HRESULT (__stdcall* ResetFn)(void*, void*);
typedef HRESULT (__stdcall* TexFn)(void*, UINT, UINT, UINT, DWORD, DWORD, DWORD, void**, HANDLE*);
typedef HRESULT (__stdcall* VolFn)(void*, UINT, UINT, UINT, UINT, DWORD, DWORD, DWORD, void**, HANDLE*);
typedef HRESULT (__stdcall* CubeFn)(void*, UINT, UINT, DWORD, DWORD, DWORD, void**, HANDLE*);
typedef HRESULT (__stdcall* BufFn)(void*, UINT, DWORD, DWORD, DWORD, void**, HANDLE*);
typedef HRESULT (__stdcall* RtFn)(void*, UINT, UINT, DWORD, DWORD, DWORD, BOOL, void**, HANDLE*);
typedef HRESULT (__stdcall* PlainFn)(void*, UINT, UINT, DWORD, DWORD, void**, HANDLE*);
typedef HRESULT (__stdcall* TypeFn)(void*, DWORD, void**);
typedef HRESULT (__stdcall* PtrFn)(void*, const void*, void**);
typedef HRESULT (__stdcall* OutFn)(void*, void**);

ResetFn o_Reset;
TexFn   o_CreateTexture;
VolFn   o_CreateVolumeTexture;
CubeFn  o_CreateCubeTexture;
BufFn   o_CreateVertexBuffer, o_CreateIndexBuffer;
RtFn    o_CreateRenderTarget, o_CreateDepthStencilSurface;
PlainFn o_CreateOffscreenPlainSurface;
TypeFn  o_CreateStateBlock, o_CreateQuery;
OutFn   o_EndStateBlock;
PtrFn   o_CreateVertexDeclaration, o_CreateVertexShader, o_CreatePixelShader;

HRESULT __stdcall H_Reset(void* dev, void* params) {
    if (dev == g_device) drop_all_pins("device reset");   // a reset needs every default-pool resource released
    return o_Reset(dev, params);
}

#define TRACK_IF_OK(kind) \
    if (hr >= 0 && out && engine_call(dev, _ReturnAddress())) track(*out, kind); \
    return hr

HRESULT __stdcall H_CreateTexture(void* dev, UINT w, UINT h, UINT levels, DWORD usage, DWORD fmt, DWORD pool, void** out, HANDLE* sh) {
    HRESULT hr = o_CreateTexture(dev, w, h, levels, usage, fmt, pool, out, sh);
    TRACK_IF_OK(K_TEXTURE);
}
HRESULT __stdcall H_CreateVolumeTexture(void* dev, UINT w, UINT h, UINT d, UINT levels, DWORD usage, DWORD fmt, DWORD pool, void** out, HANDLE* sh) {
    HRESULT hr = o_CreateVolumeTexture(dev, w, h, d, levels, usage, fmt, pool, out, sh);
    TRACK_IF_OK(K_VOLUME);
}
HRESULT __stdcall H_CreateCubeTexture(void* dev, UINT edge, UINT levels, DWORD usage, DWORD fmt, DWORD pool, void** out, HANDLE* sh) {
    HRESULT hr = o_CreateCubeTexture(dev, edge, levels, usage, fmt, pool, out, sh);
    TRACK_IF_OK(K_CUBE);
}
HRESULT __stdcall H_CreateVertexBuffer(void* dev, UINT len, DWORD usage, DWORD fvf, DWORD pool, void** out, HANDLE* sh) {
    HRESULT hr = o_CreateVertexBuffer(dev, len, usage, fvf, pool, out, sh);
    TRACK_IF_OK(K_VB);
}
HRESULT __stdcall H_CreateIndexBuffer(void* dev, UINT len, DWORD usage, DWORD fmt, DWORD pool, void** out, HANDLE* sh) {
    HRESULT hr = o_CreateIndexBuffer(dev, len, usage, fmt, pool, out, sh);
    TRACK_IF_OK(K_IB);
}
HRESULT __stdcall H_CreateRenderTarget(void* dev, UINT w, UINT h, DWORD fmt, DWORD ms, DWORD msq, BOOL lockable, void** out, HANDLE* sh) {
    HRESULT hr = o_CreateRenderTarget(dev, w, h, fmt, ms, msq, lockable, out, sh);
    TRACK_IF_OK(K_SURFACE);
}
HRESULT __stdcall H_CreateDepthStencilSurface(void* dev, UINT w, UINT h, DWORD fmt, DWORD ms, DWORD msq, BOOL discard, void** out, HANDLE* sh) {
    HRESULT hr = o_CreateDepthStencilSurface(dev, w, h, fmt, ms, msq, discard, out, sh);
    TRACK_IF_OK(K_SURFACE);
}
HRESULT __stdcall H_CreateOffscreenPlainSurface(void* dev, UINT w, UINT h, DWORD fmt, DWORD pool, void** out, HANDLE* sh) {
    HRESULT hr = o_CreateOffscreenPlainSurface(dev, w, h, fmt, pool, out, sh);
    TRACK_IF_OK(K_SURFACE);
}
HRESULT __stdcall H_CreateStateBlock(void* dev, DWORD type, void** out) {
    HRESULT hr = o_CreateStateBlock(dev, type, out);
    TRACK_IF_OK(K_STATEBLOCK);
}
HRESULT __stdcall H_EndStateBlock(void* dev, void** out) {
    HRESULT hr = o_EndStateBlock(dev, out);
    TRACK_IF_OK(K_STATEBLOCK);
}
HRESULT __stdcall H_CreateVertexDeclaration(void* dev, const void* elements, void** out) {
    HRESULT hr = o_CreateVertexDeclaration(dev, elements, out);
    TRACK_IF_OK(K_DECL);
}
HRESULT __stdcall H_CreateVertexShader(void* dev, const void* code, void** out) {
    HRESULT hr = o_CreateVertexShader(dev, code, out);
    TRACK_IF_OK(K_VS);
}
HRESULT __stdcall H_CreatePixelShader(void* dev, const void* code, void** out) {
    HRESULT hr = o_CreatePixelShader(dev, code, out);
    TRACK_IF_OK(K_PS);
}
HRESULT __stdcall H_CreateQuery(void* dev, DWORD type, void** out) {   // out == null: support check only
    HRESULT hr = o_CreateQuery(dev, type, out);
    TRACK_IF_OK(K_QUERY);
}
#undef TRACK_IF_OK

struct DevHook { int slot; void* fn; void** orig; };
const DevHook kDevHooks[] = {
    { DEV_Reset,                       (void*)H_Reset,                       (void**)&o_Reset },
    { DEV_CreateTexture,               (void*)H_CreateTexture,               (void**)&o_CreateTexture },
    { DEV_CreateVolumeTexture,         (void*)H_CreateVolumeTexture,         (void**)&o_CreateVolumeTexture },
    { DEV_CreateCubeTexture,           (void*)H_CreateCubeTexture,           (void**)&o_CreateCubeTexture },
    { DEV_CreateVertexBuffer,          (void*)H_CreateVertexBuffer,          (void**)&o_CreateVertexBuffer },
    { DEV_CreateIndexBuffer,           (void*)H_CreateIndexBuffer,           (void**)&o_CreateIndexBuffer },
    { DEV_CreateRenderTarget,          (void*)H_CreateRenderTarget,          (void**)&o_CreateRenderTarget },
    { DEV_CreateDepthStencilSurface,   (void*)H_CreateDepthStencilSurface,   (void**)&o_CreateDepthStencilSurface },
    { DEV_CreateOffscreenPlainSurface, (void*)H_CreateOffscreenPlainSurface, (void**)&o_CreateOffscreenPlainSurface },
    { DEV_CreateStateBlock,            (void*)H_CreateStateBlock,            (void**)&o_CreateStateBlock },
    { DEV_EndStateBlock,               (void*)H_EndStateBlock,               (void**)&o_EndStateBlock },
    { DEV_CreateVertexDeclaration,     (void*)H_CreateVertexDeclaration,     (void**)&o_CreateVertexDeclaration },
    { DEV_CreateVertexShader,          (void*)H_CreateVertexShader,          (void**)&o_CreateVertexShader },
    { DEV_CreatePixelShader,           (void*)H_CreatePixelShader,           (void**)&o_CreatePixelShader },
    { DEV_CreateQuery,                 (void*)H_CreateQuery,                 (void**)&o_CreateQuery },
};

void** g_dev_vtable = nullptr;
bool   g_dev_patch_failed = false;

// Under g_cs.
void on_engine_device(void* dev) {
    void** vt = *(void***)dev;
    if (g_dev_vtable && g_dev_vtable != vt) {
        util::Log("d3dguard: new device class (vtable %p, was %p): D3D objects are no longer tracked", vt, g_dev_vtable);
        g_active = false;
        g_device = nullptr;
        return;
    }
    g_device = dev;
    if (!g_dev_vtable) {
        // Patched once, even partly: patching again would take our hooks for the originals.
        g_dev_vtable = vt;
        for (const DevHook& h : kDevHooks) {
            *h.orig = vt[h.slot];
            if (!util::WritePointer(&vt[h.slot], h.fn)) {
                util::Log("d3dguard: could not patch device slot %d: D3D objects are NOT tracked", h.slot);
                g_dev_patch_failed = true;   // the slots patched so far only track; nothing gets pinned
                break;
            }
        }
    }
    if (g_dev_patch_failed) return;
    g_active = true;
    util::Log("d3dguard: engine device %p hooked (vtable %p)", dev, vt);
}

// ---- IDirect3D9::CreateDevice and Direct3DCreate9 -----------------------------------------------------
typedef HRESULT (__stdcall* CreateDeviceFn)(void*, UINT, DWORD, HWND, DWORD, void*, void**);
CreateDeviceFn o_CreateDevice = nullptr;
void** g_d3d_vtable = nullptr;

// True if the engine's shader API (shaderapidx9.dll, or shaderapivk.dll with -vulkan) is among the
// callers. Not just the direct caller: the Steam overlay wraps Direct3DCreate9 and CreateDevice, so
// these calls reach us from GameOverlayRenderer.dll on behalf of the shader API.
bool shader_api_on_stack() {
    if (!g_api_lo) {
        const char* name = "shaderapidx9.dll";
        HMODULE m = GetModuleHandleA(name);
        if (!m) m = GetModuleHandleA(name = "shaderapivk.dll");
        if (!m) return false;
        IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((uintptr_t)m + ((IMAGE_DOS_HEADER*)m)->e_lfanew);
        g_api_hi = (uintptr_t)m + nt->OptionalHeader.SizeOfImage;
        g_api_lo = (uintptr_t)m;
        util::Log("d3dguard: shader API is %s", name);
    }
    void* frames[12];
    USHORT n = RtlCaptureStackBackTrace(0, 12, frames, nullptr);
    for (USHORT i = 0; i < n; i++) if ((uintptr_t)frames[i] - g_api_lo < g_api_hi - g_api_lo) return true;
    return false;
}

HRESULT __stdcall H_CreateDevice(void* d3d, UINT adapter, DWORD type, HWND focus, DWORD flags, void* params, void** out) {
    HRESULT hr = o_CreateDevice(d3d, adapter, type, focus, flags, params, out);
    if (hr >= 0 && out && *out && shader_api_on_stack()) {
        if (g_device) drop_all_pins("new device");   // the snapshots' objects belong to the old device
        EnterCriticalSection(&g_cs);
        on_engine_device(*out);
        LeaveCriticalSection(&g_cs);
    }
    return hr;
}

void on_d3d(void* d3d) {
    if (!d3d || !shader_api_on_stack()) return;
    void** vt = *(void***)d3d;
    if (g_d3d_vtable) {
        if (g_d3d_vtable != vt) util::Log("d3dguard: the shader API created an IDirect3D9 of another runtime: ignored");
        return;
    }
    void* orig = vt[D3D_CREATEDEVICE];
    o_CreateDevice = (CreateDeviceFn)orig;
    if (util::WritePointer(&vt[D3D_CREATEDEVICE], (void*)H_CreateDevice)) g_d3d_vtable = vt;
    else util::Log("d3dguard: could not patch IDirect3D9::CreateDevice");
}

// One hook per runtime that may get loaded (d3d9.dll and dxvk_d3d9.dll can both be present).
typedef void* (__stdcall* Create9Fn)(UINT);
Create9Fn g_create9[2] = {};
int       g_runtimes = 0;

template <int N> void* __stdcall H_Direct3DCreate9(UINT sdk) {
    void* d3d = g_create9[N](sdk);
    on_d3d(d3d);
    return d3d;
}
void* const kCreate9Hooks[2] = { (void*)H_Direct3DCreate9<0>, (void*)H_Direct3DCreate9<1> };

} // namespace

bool HookRuntime(uintptr_t base, const char* name) {
    static bool init = false;
    if (!init) {
        InitializeCriticalSection(&g_cs);
        g_tab = (Obj*)VirtualAlloc(nullptr, CAP * sizeof(Obj), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        g_work = (Obj*)VirtualAlloc(nullptr, CAP * sizeof(Obj), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        init = true;
    }
    if (!g_tab || !g_work) { util::Log("d3dguard: out of memory"); return false; }
    if (g_runtimes == 2) return false;
    void* fn = util::FindExport(base, "Direct3DCreate9");
    if (!fn || !util::InlineHookAt(fn, kCreate9Hooks[g_runtimes], (void**)&g_create9[g_runtimes])) {
        util::Log("d3dguard: could not hook %s!Direct3DCreate9: D3D objects will NOT be tracked", name);
        return false;
    }
    g_runtimes++;
    util::Log("d3dguard: hooked %s!Direct3DCreate9", name);
    return true;
}

bool Active() { return g_active; }
bool Healthy() { return g_active && !g_overflow; }
bool Intact(int slot) { return g_active && !g_overflow && slot >= 0 && slot < MAX_SLOTS && g_slot[slot].intact; }

void OnSave(int slot) {
    if (!g_active || slot < 0 || slot >= MAX_SLOTS) return;
    drop_slot(slot);   // a save replaces whatever the slot held
    SlotObjs& so = g_slot[slot];
    size_t n = snapshot_entries(0, 0);
    if (so.cap < n) {
        if (so.v) VirtualFree(so.v, 0, MEM_RELEASE);
        so.cap = n + n / 2 + 256;
        so.v = (Saved*)VirtualAlloc(nullptr, so.cap * sizeof(Saved), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!so.v) { so.cap = 0; g_overflow = true; return; }
    }
    so.n = 0;
    size_t per[K_COUNT] = {};
    const SlotMask bit = SlotBit(slot);
    for (size_t i = 0; i < n; i++) {
        const Obj& e = g_work[i];
        LONG engine = (LONG)add_ref(e.p) - 1 - SlotCount(e.mask);   // our new reference is excluded
        if (engine <= 0) { release(e.p); continue; }               // only other snapshots hold it
        so.v[so.n++] = Saved{ e.p, engine };                       // the new reference is this slot's pin
        per[e.kind]++;
        EnterCriticalSection(&g_cs);
        if (Obj* o = find(e.p)) o->mask |= bit;
        LeaveCriticalSection(&g_cs);
    }
    // Sorted by address for saved_refs().
    struct Cmp { static int f(const void* x, const void* y) { void* a = ((const Saved*)x)->p; void* b = ((const Saved*)y)->p; return a < b ? -1 : a > b; } };
    qsort(so.v, so.n, sizeof(Saved), Cmp::f);
    so.intact = true;
    g_pinned_slots |= bit;
    util::Log("d3dguard: pinned %u objects: %u textures, %u cube, %u volume, %u surfaces, %u VB, %u IB, %u VS, %u PS, "
              "%u decls, %u queries, %u state blocks%s",
              (unsigned)so.n, (unsigned)per[K_TEXTURE], (unsigned)per[K_CUBE], (unsigned)per[K_VOLUME],
              (unsigned)per[K_SURFACE], (unsigned)per[K_VB], (unsigned)per[K_IB], (unsigned)per[K_VS], (unsigned)per[K_PS],
              (unsigned)per[K_DECL], (unsigned)per[K_QUERY], (unsigned)per[K_STATEBLOCK],
              g_overflow ? " - REGISTRY OVERFLOWED" : "");
}

void AfterRestore(int slot) {
    if (!g_active || slot < 0 || slot >= MAX_SLOTS) return;
    size_t n = snapshot_entries(0, 0);
    size_t adjusted = 0, freed = 0;
    size_t freed_per[K_COUNT] = {};
    for (size_t i = 0; i < n; i++) {
        const Obj& e = g_work[i];
        // The restored engine holds exactly the references it held at save time (none for an object the
        // restored timeline does not have); every snapshot pinning the object holds one more.
        LONG want = saved_refs(slot, e.p) + SlotCount(e.mask);
        LONG cur = (LONG)add_ref(e.p) - 1;   // without this temporary reference
        if (cur != want) adjusted++;
        for (; cur < want; cur++) add_ref(e.p);
        for (; cur > want; cur--) release(e.p);
        release(e.p);                        // the temporary reference: an object nothing holds dies here
        if (!want) { freed++; freed_per[e.kind]++; }
    }
    if (adjusted || freed)
        util::Log("d3dguard: restore: %u reference counts set back, %u objects the restored timeline does not have released "
                  "(%u textures, %u VB, %u IB, %u shaders)", (unsigned)adjusted, (unsigned)freed,
                  (unsigned)(freed_per[K_TEXTURE] + freed_per[K_CUBE] + freed_per[K_VOLUME]), (unsigned)freed_per[K_VB],
                  (unsigned)freed_per[K_IB], (unsigned)(freed_per[K_VS] + freed_per[K_PS]));
}

void OnDrop(int slot) {
    if (!g_active || slot < 0 || slot >= MAX_SLOTS) return;
    SlotObjs& so = g_slot[slot];
    unsigned n = (unsigned)so.n;
    drop_slot(slot);
    if (so.v) VirtualFree(so.v, 0, MEM_RELEASE);
    so = SlotObjs{};   // not intact
    if (n) util::Log("d3dguard: %u pins released (%u objects still alive)", n, (unsigned)g_live);
}

} // namespace d3dguard
