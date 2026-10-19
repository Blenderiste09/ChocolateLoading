#define _CRT_SECURE_NO_WARNINGS
#include "memhook.h"
#include "arena.h"
#include "sdk.h"
#include "util.h"
#include <windows.h>
#include <intrin.h>
#include <string.h>
#include <stdio.h>

#pragma intrinsic(_ReturnAddress, _AddressOfReturnAddress)

namespace memhook {

namespace {

typedef void*  (__thiscall* AllocFn)(void*, size_t);
typedef void*  (__thiscall* AllocDbgFn)(void*, size_t, const char*, int);
typedef void*  (__thiscall* ReallocFn)(void*, void*, size_t);
typedef void*  (__thiscall* ReallocDbgFn)(void*, void*, size_t, const char*, int);
typedef void   (__thiscall* FreeFn)(void*, void*);
typedef void   (__thiscall* FreeDbgFn)(void*, void*, const char*, int);
typedef void*  (__thiscall* ExpandFn)(void*, void*, size_t);
typedef void*  (__thiscall* ExpandDbgFn)(void*, void*, size_t, const char*, int);
typedef size_t (__thiscall* GetSizeFn)(void*, void*);
typedef void*  (__thiscall* RegionAllocFn)(void*, int, size_t);
typedef void*  (__thiscall* RegionAllocDbgFn)(void*, int, size_t, const char*, int);
typedef void*  (__thiscall* KVAllocFn)(void*, int);
typedef void   (__thiscall* KVFreeFn)(void*, void*);

AllocFn          o_Alloc;
AllocDbgFn       o_AllocDbg;
ReallocFn        o_Realloc;
ReallocDbgFn     o_ReallocDbg;
FreeFn           o_Free;
FreeDbgFn        o_FreeDbg;
ExpandFn         o_Expand;
ExpandDbgFn      o_ExpandDbg;
GetSizeFn        o_GetSize;
RegionAllocFn    o_RegionAlloc;
RegionAllocDbgFn o_RegionAllocDbg;
KVAllocFn        o_KVAlloc;
KVFreeFn         o_KVFree;

void*    g_memalloc = nullptr;      // tier0's CStdMemAlloc object (learnt from the first hooked call)
bool     g_installed = false;
void**   g_vt_hooked = nullptr;     // the IMemAlloc vtable we patched
bool     g_arena_ok = false;
volatile LONG g_overflow = 0;
uint8_t  g_route[65536];            // domain mode: 1 = 64 KB granule inside a domain module
char     g_domain_desc[512];
// When set, EVERY g_pMemAlloc/KeyValues allocation goes to the arena regardless of the caller.
// Everything using tier0's allocator is Source engine memory (Steam, the driver and the OS use their
// own heaps), so with the early loader this captures the whole engine heap from process start.
volatile bool g_route_all = false;

inline bool route_in(const void* ra) { return g_route_all || g_route[(uintptr_t)ra >> 16] != 0; }

// ---- the CRTs' per-thread data stays out of the arena --------------------------------------------------
// Each game DLL links its own static CRT, whose per-thread data (_tiddata: errno, strtok state, locale
// references...) is allocated through tier0 like everything else, but is referenced only from the
// thread's FLS slot - per-thread state outside the snapshot. In the arena, a restore would revert it or,
// when the thread got it after the save, free it: the CRT then read garbage in __freefls when the thread
// exited or when the DLL was unloaded at "quit" (an access violation under the loader lock, which froze
// the game in its crash handlers). It is not game state, so it goes to tier0's own heap.
//
// The three allocations of _tiddata in a VS2010 CRT (__getptd_noexit, _beginthread, _beginthreadex) are
// "push 0x214; push 1; call _calloc_crt": one of the return addresses on the stack is right after such a
// call.
const size_t TIDDATA_SIZE = 0x214;
bool g_crt_in_arena = false;

bool readable_code(uintptr_t a) {
    MEMORY_BASIC_INFORMATION mbi;
    return VirtualQuery((void*)a, &mbi, sizeof(mbi)) && mbi.Type == MEM_IMAGE && mbi.State == MEM_COMMIT &&
           (mbi.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
}

__declspec(noinline) bool crt_thread_data(size_t size) {
    if (size != TIDDATA_SIZE || g_crt_in_arena) return false;
    const uintptr_t* sp = (const uintptr_t*)_AddressOfReturnAddress();
    for (int i = 0; i < 48; i++) {
        uintptr_t r = sp[i];
        if (r < 0x10000 || !readable_code(r - 12) || !readable_code(r - 1)) continue;
        const uint8_t* c = (const uint8_t*)r;
        if (c[-5] == 0xE8 && c[-7] == 0x6A && c[-6] == 0x01 && c[-12] == 0x68 && *(const uint32_t*)(c - 11) == TIDDATA_SIZE)
            return true;
    }
    return false;
}
inline void overflow() { InterlockedExchange(&g_overflow, 1); }

void old_free(void* p) { o_Free(g_memalloc, p); }

// -- tier0 IMemAlloc hooks (thiscall emulated with fastcall: ecx = this, edx = unused) -------------
void* alloc_routed(const void* ra, size_t size) {
    if (route_in(ra) && !crt_thread_data(size)) {
        void* p = arena::Alloc(size);
        if (p) return p;
        overflow();   // arena exhausted: fall back to tier0's heap (the snapshot becomes incomplete)
    }
    return nullptr;
}

void free_routed(const void* ra, void* p, bool dbg, const char* f, int l) {
    if (!p) return;
    if (arena::Contains(p)) { arena::Free(p); return; }
    // A tier0-heap block freed by domain code while a snapshot exists: the snapshot may still need it
    // (a restore brings back the state that referenced it), so the free is parked until the snapshot
    // is dropped or replaced.
    if (arena::SnapshotActive() && route_in(ra)) { arena::DeferOldFree(p); return; }
    if (dbg) o_FreeDbg(g_memalloc, p, f, l); else o_Free(g_memalloc, p);
}

void* realloc_routed(const void* ra, void* p, size_t size, bool dbg, const char* f, int l) {
    if (!p) {
        void* q = alloc_routed(ra, size);
        if (q) return q;
        return dbg ? o_AllocDbg(g_memalloc, size, f, l) : o_Alloc(g_memalloc, size);
    }
    if (arena::Contains(p)) {
        if (size == 0) { arena::Free(p); return nullptr; }
        void* q = arena::Realloc(p, size);
        if (q) return q;
        overflow();
        q = o_Alloc(g_memalloc, size);
        if (!q) return nullptr;
        size_t old = arena::UsableSize(p);
        memcpy(q, p, old < size ? old : size);
        arena::Free(p);
        return q;
    }
    if (route_in(ra) && size) {
        void* q = arena::Alloc(size);
        if (q) {
            size_t old = o_GetSize(g_memalloc, p);
            memcpy(q, p, old < size ? old : size);
            free_routed(ra, p, false, nullptr, 0);
            return q;
        }
        overflow();
    }
    return dbg ? o_ReallocDbg(g_memalloc, p, size, f, l) : o_Realloc(g_memalloc, p, size);
}

// With the early install the vtable is patched before tier0 has even constructed its allocator, so
// the object pointer is learnt from the first call (always the same singleton).
inline void note_self(void* self) { if (!g_memalloc) g_memalloc = self; }

void* __fastcall H_Alloc(void* self, void*, size_t size) {
    note_self(self);
    void* p = alloc_routed(_ReturnAddress(), size);
    return p ? p : o_Alloc(self, size);
}
void* __fastcall H_AllocDbg(void* self, void*, size_t size, const char* f, int l) {
    note_self(self);
    void* p = alloc_routed(_ReturnAddress(), size);
    return p ? p : o_AllocDbg(self, size, f, l);
}
void* __fastcall H_RegionAlloc(void* self, void*, int region, size_t size) {
    note_self(self);
    void* p = alloc_routed(_ReturnAddress(), size);
    return p ? p : o_RegionAlloc(self, region, size);
}
void* __fastcall H_RegionAllocDbg(void* self, void*, int region, size_t size, const char* f, int l) {
    note_self(self);
    void* p = alloc_routed(_ReturnAddress(), size);
    return p ? p : o_RegionAllocDbg(self, region, size, f, l);
}
void* __fastcall H_Realloc(void* self, void*, void* p, size_t size) {
    note_self(self);
    return realloc_routed(_ReturnAddress(), p, size, false, nullptr, 0);
}
void* __fastcall H_ReallocDbg(void* self, void*, void* p, size_t size, const char* f, int l) {
    note_self(self);
    return realloc_routed(_ReturnAddress(), p, size, true, f, l);
}
void __fastcall H_Free(void* self, void*, void* p) { note_self(self); free_routed(_ReturnAddress(), p, false, nullptr, 0); }
void __fastcall H_FreeDbg(void* self, void*, void* p, const char* f, int l) { note_self(self); free_routed(_ReturnAddress(), p, true, f, l); }
void* __fastcall H_Expand(void* self, void*, void* p, size_t size) {
    if (arena::Contains(p)) return nullptr;
    return o_Expand(self, p, size);
}
void* __fastcall H_ExpandDbg(void* self, void*, void* p, size_t size, const char* f, int l) {
    if (arena::Contains(p)) return nullptr;
    return o_ExpandDbg(self, p, size, f, l);
}
size_t __fastcall H_GetSize(void* self, void*, void* p) {
    if (p && arena::Contains(p)) return arena::UsableSize(p);
    return o_GetSize(self, p);
}

// -- KeyValuesSystem hooks ---------------------------------------------------------------------
void* __fastcall H_KVAlloc(void* self, void*, int size) {
    if (route_in(_ReturnAddress())) {
        void* p = arena::Alloc((size_t)size);
        if (p) return p;
        overflow();
    }
    return o_KVAlloc(self, size);
}
void __fastcall H_KVFree(void* self, void*, void* p) {
    if (p && arena::Contains(p)) { arena::Free(p); return; }
    o_KVFree(self, p);
}

// The Dbg/non-Dbg overloads differ by their number of stack arguments, which their "ret imm16" gives:
// checking it catches a changed vtable layout before we patch the wrong slots.
bool check_slots(void** vt, char* err, size_t errlen) {
    struct { int slot; int imm; } exp[] = {
        { memslot::AllocDbg, 0x0C }, { memslot::Alloc, 0x04 },
        { memslot::ReallocDbg, 0x10 }, { memslot::Realloc, 0x08 },
        { memslot::FreeDbg, 0x0C }, { memslot::Free, 0x04 },
        { memslot::GetSize, 0x04 },
        { memslot::RegionAllocDbg, 0x10 }, { memslot::RegionAlloc, 0x08 },
    };
    for (auto& e : exp) {
        int r = util::FirstRetImm(vt[e.slot]);
        if (r != e.imm) {
            _snprintf(err, errlen, "IMemAlloc slot %d: ret %d, expected %d (tier0 layout changed?)", e.slot, r, e.imm);
            return false;
        }
    }
    return true;
}

bool ensure_arena(size_t reserve, char* err, size_t errlen) {
    if (g_arena_ok) return true;
    // Reserve the arena: try the requested size, then smaller ones.
    size_t sz = reserve;
    while (sz >= (256u << 20) && !arena::Init(sz)) sz -= (128u << 20);
    if (sz < (256u << 20)) { _snprintf(err, errlen, "could not reserve the arena"); return false; }
    arena::SetOldFreeFn(old_free);
    util::Log("arena reserved: %u MB at %p", (unsigned)(sz >> 20), (void*)arena::Base());
    g_arena_ok = true;
    return true;
}

bool patch_memalloc(void** vt, char* err, size_t errlen) {
    if (!check_slots(vt, err, errlen)) return false;
    o_Alloc          = (AllocFn)vt[memslot::Alloc];
    o_AllocDbg       = (AllocDbgFn)vt[memslot::AllocDbg];
    o_Realloc        = (ReallocFn)vt[memslot::Realloc];
    o_ReallocDbg     = (ReallocDbgFn)vt[memslot::ReallocDbg];
    o_Free           = (FreeFn)vt[memslot::Free];
    o_FreeDbg        = (FreeDbgFn)vt[memslot::FreeDbg];
    o_Expand         = (ExpandFn)vt[memslot::Expand];
    o_ExpandDbg      = (ExpandDbgFn)vt[memslot::ExpandDbg];
    o_GetSize        = (GetSizeFn)vt[memslot::GetSize];
    o_RegionAlloc    = (RegionAllocFn)vt[memslot::RegionAlloc];
    o_RegionAllocDbg = (RegionAllocDbgFn)vt[memslot::RegionAllocDbg];
    // Order matters: the functions that accept arena pointers are hooked before the ones that can
    // start returning them.
    bool ok = true;
    ok &= util::WritePointer(&vt[memslot::GetSize], (void*)H_GetSize);
    ok &= util::WritePointer(&vt[memslot::Free], (void*)H_Free);
    ok &= util::WritePointer(&vt[memslot::FreeDbg], (void*)H_FreeDbg);
    ok &= util::WritePointer(&vt[memslot::Realloc], (void*)H_Realloc);
    ok &= util::WritePointer(&vt[memslot::ReallocDbg], (void*)H_ReallocDbg);
    ok &= util::WritePointer(&vt[memslot::Expand], (void*)H_Expand);
    ok &= util::WritePointer(&vt[memslot::ExpandDbg], (void*)H_ExpandDbg);
    ok &= util::WritePointer(&vt[memslot::Alloc], (void*)H_Alloc);
    ok &= util::WritePointer(&vt[memslot::AllocDbg], (void*)H_AllocDbg);
    ok &= util::WritePointer(&vt[memslot::RegionAlloc], (void*)H_RegionAlloc);
    ok &= util::WritePointer(&vt[memslot::RegionAllocDbg], (void*)H_RegionAllocDbg);
    if (!ok) { _snprintf(err, errlen, "IMemAlloc vtable patch failed"); return false; }
    g_vt_hooked = vt;
    return true;
}

} // namespace

bool InstallEarly(uintptr_t tier0_base, size_t tier0_size, size_t reserve, char* err, size_t errlen) {
    if (g_vt_hooked) return true;
    util::Module m;
    memset(&m, 0, sizeof(m));
    m.base = tier0_base;
    m.end = tier0_base + tier0_size;
    void** vt = (void**)util::FindVTableByRTTI(m, ".?AVCStdMemAlloc@@");
    if (!vt) { _snprintf(err, errlen, "CStdMemAlloc vtable not found in tier0"); return false; }
    if (!ensure_arena(reserve, err, errlen)) return false;
    g_route_all = true;   // before the patch: the very first hooked call already goes to the arena
    if (!patch_memalloc(vt, err, errlen)) return false;
    return true;
}

bool Install(size_t reserve, char* err, size_t errlen) {
    if (g_installed) return true;
    HMODULE t0 = GetModuleHandleA("tier0.dll");
    HMODULE vs = GetModuleHandleA("vstdlib.dll");
    if (!t0 || !vs) { _snprintf(err, errlen, "tier0/vstdlib not loaded"); return false; }
    void** pp = (void**)GetProcAddress(t0, "g_pMemAlloc");
    if (!pp || !*pp) { _snprintf(err, errlen, "g_pMemAlloc not found"); return false; }
    g_memalloc = *pp;
    void** vt = *(void***)g_memalloc;
    if (vt != g_vt_hooked) {
        if (g_vt_hooked) util::Log("memhook: WARNING g_pMemAlloc uses a different vtable (%p) than the early hook (%p)", vt, g_vt_hooked);
        if (!ensure_arena(reserve, err, errlen)) return false;
        if (!patch_memalloc(vt, err, errlen)) return false;
    }

    // KeyValuesSystem() returns a static object of vstdlib: until vstdlib's initialisation has run its
    // vtable pointer is not set yet, so check that the vtable and both slots point into vstdlib.
    typedef void* (*KVSysFn)();
    KVSysFn kvsys = (KVSysFn)GetProcAddress(vs, "KeyValuesSystem");
    void* kv = kvsys ? kvsys() : nullptr;
    util::Module vsm;
    if (!kv || !util::GetModule("vstdlib.dll", &vsm)) { _snprintf(err, errlen, "KeyValuesSystem not found"); return false; }
    void** kvt = *(void***)kv;
    auto in_vstdlib = [&](const void* p) { return (uintptr_t)p >= vsm.base && (uintptr_t)p < vsm.end; };
    if (!in_vstdlib(kvt) || !in_vstdlib(kvt[kvslot::AllocKeyValuesMemory]) || !in_vstdlib(kvt[kvslot::FreeKeyValuesMemory])) {
        _snprintf(err, errlen, "KeyValuesSystem not initialised yet");
        return false;
    }
    o_KVAlloc = (KVAllocFn)kvt[kvslot::AllocKeyValuesMemory];
    o_KVFree  = (KVFreeFn)kvt[kvslot::FreeKeyValuesMemory];
    bool ok = true;
    ok &= util::WritePointer(&kvt[kvslot::FreeKeyValuesMemory], (void*)H_KVFree);
    ok &= util::WritePointer(&kvt[kvslot::AllocKeyValuesMemory], (void*)H_KVAlloc);
    if (!ok) { _snprintf(err, errlen, "KeyValues vtable patch failed"); return false; }
    g_installed = true;
    return true;
}

void SetCrtThreadDataInArena(bool on) { g_crt_in_arena = on; }

bool Installed() { return g_installed; }
void SetRouteAll(bool on) { g_route_all = on; }
bool RouteAll() { return g_route_all; }
bool Overflowed() { return g_overflow != 0; }

void SetDomain(const char* list) {
    static uint8_t table[65536];
    memset(table, 0, sizeof(table));
    g_domain_desc[0] = 0;
    char buf[1024];
    strncpy(buf, list, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    char* tctx = nullptr;
    for (char* tok = strtok_s(buf, " ,;", &tctx); tok; tok = strtok_s(nullptr, " ,;", &tctx)) {
        util::Module m;
        if (!util::GetModule(tok, &m)) continue;
        for (uintptr_t a = m.base; a < m.end; a += 0x10000) table[a >> 16] = 1;
        if (strlen(g_domain_desc) + strlen(m.name) + 2 < sizeof(g_domain_desc)) {
            if (g_domain_desc[0]) strcat(g_domain_desc, " ");
            strcat(g_domain_desc, m.name);
        }
    }
    // Publish: byte-wise copy is fine, each entry is read independently.
    memcpy(g_route, table, sizeof(g_route));
}

const char* DomainDescription() { return g_domain_desc; }

} // namespace memhook
