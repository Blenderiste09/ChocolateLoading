#define _CRT_SECURE_NO_WARNINGS
#include "fileguard.h"
#include "callsite.h"
#include "util.h"
#include <windows.h>

namespace fileguard {

namespace {

typedef LONG (NTAPI* NtCreateFileFn)(PHANDLE, ACCESS_MASK, void*, void*, PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
typedef LONG (NTAPI* NtOpenFileFn)(PHANDLE, ACCESS_MASK, void*, void*, ULONG, ULONG);
typedef LONG (NTAPI* NtCloseFn)(HANDLE);
NtCreateFileFn o_NtCreateFile = nullptr;
NtOpenFileFn   o_NtOpenFile = nullptr;
NtCloseFn      o_NtClose = nullptr;
bool           g_installed = false;
bool           g_overflow = false;
CRITICAL_SECTION g_cs;           // serialises the writers

// ---- registry of the game's open files, keyed by handle value ---------------------------------------
// NtClose runs for every handle of the process, also while the other threads are suspended, so its
// fast path reads the table without the lock. Entries therefore never move: a removed entry becomes a
// tombstone, and when tombstones pile up the live entries are copied to a fresh table that replaces the
// old one (the old one is kept, a reader may still be scanning it).
const uintptr_t EMPTY = 0, TOMB = 1;      // handle values are multiples of 4
struct Ent {
    volatile uintptr_t h;
    uint32_t mask;       // snapshots (one bit per slot) taken while the file was open
    uint8_t  alive;      // the current timeline has it open (else its close is deferred)
    uint8_t  writable;   // opened with write access
};
const size_t CAP = 8192;                  // power of two
const size_t MAX_LIVE = CAP / 4;
const size_t MAX_USED = CAP / 2;          // live entries + tombstones
Ent* volatile g_tab = nullptr;
size_t g_live = 0, g_used = 0;

inline size_t home(uintptr_t h) { return (size_t)(((h >> 2) * 2654435761u) & (CAP - 1)); }

bool maybe_tracked(uintptr_t h) {         // lock-free
    Ent* t = g_tab;
    if (!t) return false;
    for (size_t i = home(h), n = 0; n < CAP; i = (i + 1) & (CAP - 1), n++) {
        uintptr_t v = t[i].h;
        if (v == h) return true;
        if (v == EMPTY) return false;
    }
    return false;
}

Ent* find(uintptr_t h) {                  // under g_cs
    Ent* t = g_tab;
    for (size_t i = home(h), n = 0; n < CAP; i = (i + 1) & (CAP - 1), n++) {
        if (t[i].h == h) return &t[i];
        if (t[i].h == EMPTY) return nullptr;
    }
    return nullptr;
}

void place(Ent* t, uintptr_t h, const Ent& fields) {
    size_t i = home(h);
    while (t[i].h != EMPTY && t[i].h != TOMB) i = (i + 1) & (CAP - 1);
    t[i].mask = fields.mask;
    t[i].alive = fields.alive;
    t[i].writable = fields.writable;
    MemoryBarrier();
    t[i].h = h;
}

void rebuild() {                          // under g_cs
    Ent* n = (Ent*)VirtualAlloc(nullptr, CAP * sizeof(Ent), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!n) return;
    Ent* t = g_tab;
    for (size_t i = 0; i < CAP; i++)
        if (t[i].h != EMPTY && t[i].h != TOMB) place(n, t[i].h, t[i]);
    MemoryBarrier();
    g_tab = n;
    g_used = g_live;
}

void add(uintptr_t h, ACCESS_MASK access) {
    Ent fields = {};
    fields.alive = 1;
    fields.writable = (access & (FILE_WRITE_DATA | FILE_APPEND_DATA | GENERIC_WRITE | GENERIC_ALL)) != 0;
    EnterCriticalSection(&g_cs);
    Ent* e = find(h);
    if (e) {   // a stale entry (closed behind our back): this is a new file
        e->mask = 0;
        e->alive = 1;
        e->writable = fields.writable;
    } else if (g_live >= MAX_LIVE) {
        g_overflow = true;
    } else {
        if (g_used + 1 > MAX_USED) rebuild();
        place(g_tab, h, fields);
        g_live++;
        g_used++;
    }
    LeaveCriticalSection(&g_cs);
}

void remove(Ent* e) {                     // under g_cs
    e->h = TOMB;
    g_live--;
}

template <class Fn> void for_each_live(Fn fn) {   // under g_cs
    Ent* t = g_tab;
    for (size_t i = 0; i < CAP; i++) if (t[i].h != EMPTY && t[i].h != TOMB) fn(t[i]);
}

// ---- per snapshot: where each file was (kernel file pointer and size at save time) --------------------
const int MAX_SLOTS = 32;
struct FilePos { uintptr_t h; int64_t pos, size; };   // size < 0: not a regular file
struct SlotFiles { FilePos* v; size_t n, cap; };
SlotFiles g_slot[MAX_SLOTS];

const FilePos* slot_find(int slot, uintptr_t h) {
    for (size_t i = 0; i < g_slot[slot].n; i++) if (g_slot[slot].v[i].h == h) return &g_slot[slot].v[i];
    return nullptr;
}

void slot_add(int slot, const FilePos& p) {
    SlotFiles& s = g_slot[slot];
    if (s.n == s.cap) {
        size_t cap = s.cap ? s.cap * 2 : 256;
        FilePos* v = (FilePos*)VirtualAlloc(nullptr, cap * sizeof(FilePos), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!v) { g_overflow = true; return; }
        if (s.v) { memcpy(v, s.v, s.n * sizeof(FilePos)); VirtualFree(s.v, 0, MEM_RELEASE); }
        s.v = v;
        s.cap = cap;
    }
    s.v[s.n++] = p;
}

void log_file(const char* what, uintptr_t h, int64_t a, int64_t b) {
    wchar_t path[MAX_PATH] = L"";
    GetFinalPathNameByHandleW((HANDLE)h, path, MAX_PATH, 0);
    util::Log("fileguard: %s %ls (%lld -> %lld)", what, path[0] ? path : L"(unnamed)", a, b);
}

FilePos capture_position(uintptr_t h) {
    FilePos p = { h, 0, -1 };
    LARGE_INTEGER zero = {}, pos, size;
    if (GetFileType((HANDLE)h) == FILE_TYPE_DISK && GetFileSizeEx((HANDLE)h, &size) &&
        SetFilePointerEx((HANDLE)h, zero, &pos, FILE_CURRENT)) {
        p.pos = pos.QuadPart;
        p.size = size.QuadPart;
    }
    return p;
}

// Puts a file back where the snapshot left it. The game's view of the file (CRT buffer, demo recorder,
// file system objects) was restored with the memory; the kernel's file pointer, and the bytes a file
// opened for writing gained since the save (e.g. the demo being recorded), are reverted here. Returns
// true if the file had grown and was cut back.
bool revert_position(const FilePos& p, bool writable) {
    HANDLE h = (HANDLE)p.h;
    LARGE_INTEGER size, to;
    bool cut = false;
    if (p.size < 0) return false;
    if (writable && GetFileSizeEx(h, &size) && size.QuadPart > p.size) {
        to.QuadPart = p.size;
        cut = SetFilePointerEx(h, to, nullptr, FILE_BEGIN) && SetEndOfFile(h);
        if (cut) log_file("cut back to its size at save time", p.h, size.QuadPart, p.size);
    }
    to.QuadPart = p.pos;
    SetFilePointerEx(h, to, nullptr, FILE_BEGIN);
    return cut;
}

// Removes the entries matching `pred` (which may update them) and closes their handles outside the lock.
template <class Pred> size_t close_where(Pred pred) {
    static uintptr_t doomed[CAP];
    size_t n = 0;
    EnterCriticalSection(&g_cs);
    Ent* t = g_tab;
    for (size_t i = 0; i < CAP; i++)
        if (t[i].h != EMPTY && t[i].h != TOMB && pred(t[i])) { doomed[n++] = t[i].h; remove(&t[i]); }
    LeaveCriticalSection(&g_cs);
    for (size_t i = 0; i < n; i++) {
        wchar_t path[MAX_PATH] = L"";
        GetFinalPathNameByHandleW((HANDLE)doomed[i], path, MAX_PATH, 0);
        util::Log("fileguard: closing %ls", path[0] ? path : L"(unnamed)");
        o_NtClose((HANDLE)doomed[i]);
    }
    return n;
}

// ---- ntdll hooks ---------------------------------------------------------------------------------
LONG NTAPI H_NtCreateFile(PHANDLE fh, ACCESS_MASK access, void* oa, void* iosb, PLARGE_INTEGER alloc, ULONG attrs,
                          ULONG share, ULONG disposition, ULONG options, PVOID ea, ULONG ea_len) {
    LONG st = o_NtCreateFile(fh, access, oa, iosb, alloc, attrs, share, disposition, options, ea, ea_len);
    if (st >= 0 && fh && *fh && callsite::DirectGameCaller()) add((uintptr_t)*fh, access);
    return st;
}

LONG NTAPI H_NtOpenFile(PHANDLE fh, ACCESS_MASK access, void* oa, void* iosb, ULONG share, ULONG options) {
    LONG st = o_NtOpenFile(fh, access, oa, iosb, share, options);
    if (st >= 0 && fh && *fh && callsite::DirectGameCaller()) add((uintptr_t)*fh, access);
    return st;
}

LONG NTAPI H_NtClose(HANDLE handle) {
    uintptr_t h = (uintptr_t)handle;
    if (!maybe_tracked(h)) return o_NtClose(handle);
    EnterCriticalSection(&g_cs);
    Ent* e = find(h);
    if (e && e->mask && (!e->alive || callsite::DirectGameCaller())) {
        e->alive = 0;   // a snapshot still refers to it: keep it open, report success
        LeaveCriticalSection(&g_cs);
        return 0;
    }
    if (e) remove(e);
    LeaveCriticalSection(&g_cs);
    return o_NtClose(handle);
}

} // namespace

void Install() {
    if (g_installed) return;
    InitializeCriticalSection(&g_cs);
    g_tab = (Ent*)VirtualAlloc(nullptr, CAP * sizeof(Ent), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_tab) { util::Log("fileguard: out of memory"); return; }
    // NtClose first: until it is hooked nothing is tracked, so no close can be missed.
    if (util::InlineHook("ntdll.dll", "NtClose", (void*)H_NtClose, (void**)&o_NtClose) &&
        util::InlineHook("ntdll.dll", "NtCreateFile", (void*)H_NtCreateFile, (void**)&o_NtCreateFile) &&
        util::InlineHook("ntdll.dll", "NtOpenFile", (void*)H_NtOpenFile, (void**)&o_NtOpenFile)) {
        g_installed = true;
        util::Log("fileguard: hooked ntdll!NtCreateFile/NtOpenFile/NtClose (game file handles)");
    } else {
        util::Log("fileguard: could not hook ntdll (close %s, create %s, open %s) - snapshots will not survive a level change",
                  o_NtClose ? "ok" : "failed", o_NtCreateFile ? "ok" : "failed", o_NtOpenFile ? "ok" : "failed");
    }
}

bool Installed() { return g_installed; }
bool Intact() { return g_installed && !g_overflow; }

void OnSave(int slot) {
    if (!g_installed || slot < 0 || slot >= MAX_SLOTS) return;
    size_t n = 0, writable = 0;
    g_slot[slot].n = 0;
    EnterCriticalSection(&g_cs);
    for_each_live([&](Ent& e) {
        if (!e.alive) return;
        e.mask |= 1u << slot;
        FilePos p = capture_position(e.h);
        slot_add(slot, p);
        n++;
        if (e.writable && p.size >= 0) {
            writable++;
            log_file("open for writing", e.h, p.pos, p.size);
        }
    });
    LeaveCriticalSection(&g_cs);
    util::Log("fileguard: pinned %u open game files (%u open for writing)%s", (unsigned)n, (unsigned)writable,
              g_overflow ? " - REGISTRY OVERFLOWED" : "");
}

void AfterRestore(int slot) {
    if (!g_installed || slot < 0 || slot >= MAX_SLOTS) return;
    const uint32_t bit = 1u << slot;
    size_t revived = 0, cut = 0;
    EnterCriticalSection(&g_cs);
    for_each_live([&](Ent& e) {
        if (!(e.mask & bit)) { e.alive = 0; return; }   // not open in the restored timeline
        if (!e.alive) { e.alive = 1; revived++; }
        if (const FilePos* p = slot_find(slot, e.h)) cut += revert_position(*p, e.writable != 0);
    });
    LeaveCriticalSection(&g_cs);
    size_t closed = close_where([](const Ent& e) { return !e.alive && !e.mask; });
    if (revived || closed || cut)
        util::Log("fileguard: restore: %u deferred closes undone, %u files cut back, %u files closed",
                  (unsigned)revived, (unsigned)cut, (unsigned)closed);
}

void OnDrop(int slot) {
    if (!g_installed || slot < 0 || slot >= MAX_SLOTS) return;
    const uint32_t bit = 1u << slot;
    size_t closed = close_where([bit](Ent& e) {
        e.mask &= ~bit;
        return !e.alive && !e.mask;
    });
    g_slot[slot].n = 0;
    if (closed) util::Log("fileguard: %u deferred file closes done", (unsigned)closed);
}

} // namespace fileguard
