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
    uint8_t pinned;     // open at save time: the snapshot refers to it
    uint8_t deferred;   // closed by the game since the save, close deferred
    uint8_t writable;   // opened with write access
    int64_t pos, size;  // file pointer and size at save time (size < 0: not a regular file)
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
    t[i].pinned = fields.pinned;
    t[i].deferred = fields.deferred;
    t[i].writable = fields.writable;
    t[i].pos = fields.pos;
    t[i].size = fields.size;
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
    fields.writable = (access & (FILE_WRITE_DATA | FILE_APPEND_DATA | GENERIC_WRITE | GENERIC_ALL)) != 0;
    fields.size = -1;
    EnterCriticalSection(&g_cs);
    Ent* e = find(h);
    if (e) {   // a stale entry (closed behind our back): this is a new file
        e->pinned = 0;
        e->deferred = 0;
        e->writable = fields.writable;
        e->size = -1;
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
    if (e && e->pinned && (e->deferred || callsite::DirectGameCaller())) {
        e->deferred = 1;   // the snapshot still refers to it: keep it open, report success
        LeaveCriticalSection(&g_cs);
        return 0;
    }
    if (e) remove(e);
    LeaveCriticalSection(&g_cs);
    return o_NtClose(handle);
}

// Removes the entries matching `pred` and closes their handles (outside the lock).
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

template <class Fn> void for_each_live(Fn fn) {   // under g_cs
    Ent* t = g_tab;
    for (size_t i = 0; i < CAP; i++) if (t[i].h != EMPTY && t[i].h != TOMB) fn(t[i]);
}

void log_file(const char* what, uintptr_t h, int64_t a, int64_t b) {
    wchar_t path[MAX_PATH] = L"";
    GetFinalPathNameByHandleW((HANDLE)h, path, MAX_PATH, 0);
    util::Log("fileguard: %s %ls (%lld -> %lld)", what, path[0] ? path : L"(unnamed)", a, b);
}

// Records the file pointer and size of a regular file (size -1 for anything else).
void capture_position(Ent& e) {
    HANDLE h = (HANDLE)e.h;
    LARGE_INTEGER zero = {}, pos, size;
    e.size = -1;
    if (GetFileType(h) == FILE_TYPE_DISK && GetFileSizeEx(h, &size) && SetFilePointerEx(h, zero, &pos, FILE_CURRENT)) {
        e.pos = pos.QuadPart;
        e.size = size.QuadPart;
    }
}

// Puts a pinned file back where the snapshot left it. The game's view of the file (CRT buffer, demo
// recorder, file system objects) was restored with the memory; the kernel's file pointer, and the bytes
// a file opened for writing gained since the save (e.g. the demo being recorded), are reverted here.
// Returns true if the file had grown and was cut back.
bool revert_position(const Ent& e) {
    HANDLE h = (HANDLE)e.h;
    LARGE_INTEGER size, to;
    bool cut = false;
    if (e.size < 0) return false;
    if (e.writable && GetFileSizeEx(h, &size) && size.QuadPart > e.size) {
        to.QuadPart = e.size;
        cut = SetFilePointerEx(h, to, nullptr, FILE_BEGIN) && SetEndOfFile(h);
        if (cut) log_file("cut back to its size at save time", e.h, size.QuadPart, e.size);
    }
    to.QuadPart = e.pos;
    SetFilePointerEx(h, to, nullptr, FILE_BEGIN);
    return cut;
}

} // namespace

void Install() {
    if (g_installed) return;
    InitializeCriticalSection(&g_cs);
    g_tab = (Ent*)VirtualAlloc(nullptr, CAP * sizeof(Ent), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_tab) { util::Log("fileguard: out of memory"); return; }
    // NtClose last: until it is hooked nothing is tracked, so no close can be missed.
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

void OnSave() {
    if (!g_installed) return;
    size_t writable = 0;
    EnterCriticalSection(&g_cs);
    for_each_live([&](Ent& e) {
        e.pinned = 1;
        e.deferred = 0;
        capture_position(e);
        if (e.writable && e.size >= 0) {
            writable++;
            log_file("open for writing", e.h, e.pos, e.size);
        }
    });
    size_t n = g_live;
    LeaveCriticalSection(&g_cs);
    util::Log("fileguard: pinned %u open game files (%u open for writing)%s", (unsigned)n, (unsigned)writable,
              g_overflow ? " - REGISTRY OVERFLOWED" : "");
}

void AfterRestore() {
    if (!g_installed) return;
    size_t revived = 0, cut = 0;
    EnterCriticalSection(&g_cs);
    for_each_live([&](Ent& e) {
        if (!e.pinned) return;
        if (e.deferred) { e.deferred = 0; revived++; }
        if (revert_position(e)) cut++;
    });
    LeaveCriticalSection(&g_cs);
    size_t closed = close_where([](const Ent& e) { return !e.pinned; });
    if (revived || closed || cut)
        util::Log("fileguard: restore: %u deferred closes undone, %u files cut back, %u files opened since the save closed",
                  (unsigned)revived, (unsigned)cut, (unsigned)closed);
}

void OnDrop() {
    if (!g_installed) return;
    size_t closed = close_where([](const Ent& e) { return e.pinned && e.deferred; });
    EnterCriticalSection(&g_cs);
    for_each_live([](Ent& e) { e.pinned = 0; });
    LeaveCriticalSection(&g_cs);
    if (closed) util::Log("fileguard: %u deferred file closes done", (unsigned)closed);
}

} // namespace fileguard
