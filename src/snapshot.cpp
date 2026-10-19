#define _CRT_SECURE_NO_WARNINGS
#include "snapshot.h"
#include "arena.h"
#include "copypool.h"
#include "d3dguard.h"
#include "fileguard.h"
#include "memhook.h"
#include "soundguard.h"
#include "steamguard.h"
#include "tlsguard.h"
#include "vatrack.h"
#include "videoguard.h"
#include "util.h"
#include <windows.h>
#include <string.h>
#include <stdio.h>

namespace snapshot {

namespace {

Options g_opts;

// ---- engine interfaces used to quiesce worker threads --------------------------------------------
void*    g_pool = nullptr;      // IThreadPool*  (vstdlib g_pThreadPool)
void*    g_fs = nullptr;        // IFileSystem*  (VFileSystem017)
void*    g_client = nullptr;    // IBaseClientDLL* (VClient016), for IN_ClearStates
void*    g_sound = nullptr;     // IEngineSound* (IEngineSoundClient003)
uint8_t* g_cengine = nullptr;   // the CEngine instance (its frame timing must survive a restore)

typedef int  (__thiscall* PoolIntFn)(void*);
typedef void (__thiscall* FsFinishAllFn)(void*, int);
typedef bool (__thiscall* FsBoolFn)(void*);
typedef void (__thiscall* VoidFn)(void*);
typedef void (__thiscall* StopAllSoundsFn)(void*, bool);

// Before a restore, the sounds of the timeline being discarded are stopped through the engine, so that
// every channel, mixer and MP3 decoder they own is released properly (soundguard keeps the decoders the
// snapshot still needs alive); the restore then brings back the sounds that played at save time.
void stop_all_sounds() {
    if (g_sound) vfunc<StopAllSoundsFn>(g_sound, soundslot::StopAllSounds)(g_sound, true);
}

bool g_pool_suspended = false, g_fs_suspended = false;

// Drain and park the job pool and the async filesystem so no job runs while memory is copied.
void quiesce_begin() {
    if (g_fs) {
        vfunc<FsFinishAllFn>(g_fs, fsslot::AsyncFinishAll)(g_fs, 0);
        g_fs_suspended = vfunc<FsBoolFn>(g_fs, fsslot::AsyncSuspend)(g_fs);
    }
    if (g_pool) {
        vfunc<PoolIntFn>(g_pool, poolslot::SuspendExecution)(g_pool);
        g_pool_suspended = true;
    }
}

void quiesce_end() {
    if (g_pool && g_pool_suspended) vfunc<PoolIntFn>(g_pool, poolslot::ResumeExecution)(g_pool);
    if (g_fs && g_fs_suspended) vfunc<FsBoolFn>(g_fs, fsslot::AsyncResume)(g_fs);
    g_pool_suspended = g_fs_suspended = false;
}

// ---- arena image store: a pagefile-backed section, read through one sliding view (restore) and -----
// ---- written in parallel 8 MB chunks with a view each (save) ---------------------------------------
const size_t WINDOW = 32u << 20;   // multiple of the 64 KB allocation granularity
HANDLE   g_section = nullptr;
size_t   g_section_size = 0;
uint8_t* g_view = nullptr;
size_t   g_view_index = (size_t)-1;

void store_unmap() {
    if (g_view) UnmapViewOfFile(g_view);
    g_view = nullptr;
    g_view_index = (size_t)-1;
}

void store_free() {
    store_unmap();
    if (g_section) CloseHandle(g_section);
    g_section = nullptr;
    g_section_size = 0;
}

bool store_ensure(size_t size) {
    size = (size + WINDOW - 1) & ~(WINDOW - 1);
    if (g_section && g_section_size >= size) return true;
    store_free();
    g_section = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, (DWORD)size, nullptr);
    g_section_size = g_section ? size : 0;
    return g_section != nullptr;
}

uint8_t* store_window(size_t index) {
    if (g_view_index == index) return g_view;
    store_unmap();
    size_t off = index * WINDOW;
    size_t len = g_section_size - off < WINDOW ? g_section_size - off : WINDOW;
    g_view = (uint8_t*)MapViewOfFile(g_section, FILE_MAP_ALL_ACCESS, 0, (DWORD)off, len);
    if (g_view) g_view_index = index;
    return g_view;
}

bool store_read(size_t off, uint8_t* dst, size_t len) {
    while (len) {
        size_t in = off % WINDOW, n = WINDOW - in;
        if (n > len) n = len;
        uint8_t* w = store_window(off / WINDOW);
        if (!w) return false;
        memcpy(dst, w + in, n);
        off += n; dst += n; len -= n;
    }
    return true;
}

// Writes the arena image with every copy worker: each 8 MB chunk is copied through its own view of the
// section. Most of the time goes into the page faults of those fresh views, which the workers take in
// parallel. Runs while the other threads are suspended (the workers are not).
const size_t CHUNK = 8u << 20;   // multiple of the 64 KB allocation granularity
struct StoreJob { const uint8_t* src; size_t len; volatile LONG failed; };

void write_chunk(void* ctx, int part) {
    StoreJob* j = (StoreJob*)ctx;
    size_t off = (size_t)part * CHUNK, n = j->len - off < CHUNK ? j->len - off : CHUNK;
    uint8_t* v = (uint8_t*)MapViewOfFile(g_section, FILE_MAP_WRITE, 0, (DWORD)off, n);
    if (!v) { InterlockedExchange(&j->failed, 1); return; }
    memcpy(v, j->src + off, n);
    UnmapViewOfFile(v);
}

bool store_write(const uint8_t* src, size_t len) {
    store_unmap();
    StoreJob j = { src, len, 0 };
    copypool::Run(write_chunk, &j, (int)((len + CHUNK - 1) / CHUNK));
    return !j.failed;
}

// Copies [a, b) of the arena back from the image, skipping the kept zombie ranges (sorted).
bool restore_range(uintptr_t a, uintptr_t b, const arena::Range* z, size_t nz) {
    uintptr_t base = arena::Base();
    size_t lo = 0, hi = nz;
    while (lo < hi) { size_t mid = (lo + hi) / 2; if (z[mid].end <= a) lo = mid + 1; else hi = mid; }
    uintptr_t cur = a;
    for (size_t i = lo; i < nz && z[i].start < b && cur < b; i++) {
        if (z[i].start > cur && !store_read(cur - base, (uint8_t*)cur, z[i].start - cur)) return false;
        if (z[i].end > cur) cur = z[i].end;
    }
    if (cur < b && !store_read(cur - base, (uint8_t*)cur, b - cur)) return false;
    return true;
}

// ---- dirty page list (filled by GetWriteWatch on restore) -----------------------------------------
uintptr_t* g_dirty = nullptr;
size_t     g_dirty_cap = 0;

bool dirty_ensure(size_t pages) {
    if (g_dirty_cap >= pages) return true;
    if (g_dirty) VirtualFree(g_dirty, 0, MEM_RELEASE);
    g_dirty = (uintptr_t*)VirtualAlloc(nullptr, pages * sizeof(uintptr_t), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_dirty_cap = g_dirty ? pages : 0;
    return g_dirty != nullptr;
}

void dirty_free() {
    if (g_dirty) VirtualFree(g_dirty, 0, MEM_RELEASE);
    g_dirty = nullptr;
    g_dirty_cap = 0;
}

// ---- module images: copies of the writable sections of the domain modules -------------------------
const int MAX_MODS = 96;
struct ModImage {
    char      module[64];
    char      section[9];
    uintptr_t start, end;
    uint8_t*  data;          // copy of [start, end), allocated by Save, released by Drop
};
ModImage g_mods[MAX_MODS];
int      g_mod_count = 0;
bool     g_mods_truncated = false;
char     g_domain[1024] = "";

// Rebuilds the section list in domain order. Buffers of sections that did not move are kept; those of
// sections that disappeared are released.
void refresh_modules() {
    ModImage old[MAX_MODS];
    int old_count = g_mod_count;
    memcpy(old, g_mods, sizeof(old));
    g_mod_count = 0;
    g_mods_truncated = false;
    char buf[1024];
    strncpy(buf, g_domain, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    char* ctx = nullptr;
    for (char* tok = strtok_s(buf, " ,;", &ctx); tok; tok = strtok_s(nullptr, " ,;", &ctx)) {
        util::Module m;
        if (!util::GetModule(tok, &m)) continue;
        for (int s = 0; s < m.rw_count; s++) {
            if (g_mod_count == MAX_MODS) { g_mods_truncated = true; break; }
            ModImage& im = g_mods[g_mod_count++];
            memset(&im, 0, sizeof(im));
            strncpy(im.module, m.name, sizeof(im.module) - 1);
            memcpy(im.section, m.rw[s].name, sizeof(im.section));
            im.start = m.rw[s].start;
            im.end = m.rw[s].end;
            for (int k = 0; k < old_count; k++)
                if (old[k].data && old[k].start == im.start && old[k].end == im.end) {
                    im.data = old[k].data;
                    old[k].data = nullptr;
                    break;
                }
        }
    }
    for (int k = 0; k < old_count; k++) if (old[k].data) VirtualFree(old[k].data, 0, MEM_RELEASE);
}

bool modules_alloc() {
    for (int i = 0; i < g_mod_count; i++)
        if (!g_mods[i].data) {
            g_mods[i].data = (uint8_t*)VirtualAlloc(nullptr, g_mods[i].end - g_mods[i].start, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!g_mods[i].data) return false;
        }
    return true;
}

void modules_free() {
    for (int i = 0; i < g_mod_count; i++) {
        if (g_mods[i].data) VirtualFree(g_mods[i].data, 0, MEM_RELEASE);
        g_mods[i].data = nullptr;
    }
}

size_t modules_bytes() {
    size_t n = 0;
    for (int i = 0; i < g_mod_count; i++) n += g_mods[i].end - g_mods[i].start;
    return n;
}

// ---- engine direct-VirtualAlloc regions (the hunk): committed writable pages, copied as segments --
const size_t MAX_VA_SEGS = 8192;
struct VaSeg { uintptr_t addr, alloc_base; size_t off, len; };
VaSeg    g_va_segs[MAX_VA_SEGS];
size_t   g_va_seg_count = 0;
size_t   g_va_bytes = 0;
uint8_t* g_va_shadow = nullptr;   // all captured bytes, back to back
size_t   g_va_shadow_cap = 0;

inline bool va_writable(DWORD protect) {
    DWORD p = protect & 0xFF;   // ignore the PAGE_GUARD/NOCACHE modifier bits
    return !(protect & PAGE_GUARD) &&
           (p == PAGE_READWRITE || p == PAGE_EXECUTE_READWRITE || p == PAGE_WRITECOPY || p == PAGE_EXECUTE_WRITECOPY);
}

bool va_reserve(size_t need) {
    if (need <= g_va_shadow_cap) return true;
    size_t ncap = g_va_shadow_cap ? g_va_shadow_cap : (16u << 20);
    while (ncap < need) ncap *= 2;
    uint8_t* n = (uint8_t*)VirtualAlloc(nullptr, ncap, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!n) return false;
    if (g_va_shadow) {
        memcpy(n, g_va_shadow, g_va_bytes);
        VirtualFree(g_va_shadow, 0, MEM_RELEASE);
    }
    g_va_shadow = n;
    g_va_shadow_cap = ncap;
    return true;
}

void va_free() {
    if (g_va_shadow) VirtualFree(g_va_shadow, 0, MEM_RELEASE);
    g_va_shadow = nullptr;
    g_va_shadow_cap = 0;
    g_va_seg_count = 0;
    g_va_bytes = 0;
}

// Calls fn(start, end, alloc_base) for every committed writable span of the tracked regions.
template <class F> bool va_for_each_span(F fn) {
    vatrack::Region regs[512];
    size_t nr = vatrack::GetRegions(regs, 512);
    for (size_t i = 0; i < nr; i++) {
        uintptr_t a = regs[i].base, end = regs[i].base + regs[i].size;
        MEMORY_BASIC_INFORMATION mbi;
        while (a < end && VirtualQuery((void*)a, &mbi, sizeof(mbi))) {
            uintptr_t rend = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
            if (rend > end) rend = end;
            if (mbi.State == MEM_COMMIT && va_writable(mbi.Protect) && !fn(a, rend, (uintptr_t)mbi.AllocationBase))
                return false;
            a = rend;
            if (mbi.RegionSize == 0) break;
        }
    }
    return true;
}

// Bytes va_capture() will need; used to size the buffer before the other threads are suspended.
size_t va_capture_size() {
    size_t total = 0;
    va_for_each_span([&](uintptr_t a, uintptr_t b, uintptr_t) { total += b - a; return true; });
    return total;
}

// Copies every committed writable page of the tracked regions. Fails (rather than producing a partial
// image) when a span cannot be stored.
// Lays out every committed writable span of the tracked regions in the shadow buffer (g_va_segs); the
// bytes themselves are copied by the caller, in parallel. Fails (rather than producing a partial image)
// when a span cannot be stored.
bool va_layout() {
    g_va_seg_count = 0;
    g_va_bytes = 0;
    return va_for_each_span([](uintptr_t a, uintptr_t b, uintptr_t alloc_base) {
        size_t len = b - a;
        if (g_va_seg_count == MAX_VA_SEGS || !va_reserve(g_va_bytes + len)) return false;
        g_va_segs[g_va_seg_count++] = VaSeg{ a, alloc_base, g_va_bytes, len };
        g_va_bytes += len;
        return true;
    });
}

// Save: copies of the module sections and VirtualAlloc segments, handed to copypool::CopyAll.
copypool::Copy g_copies[MAX_MODS + MAX_VA_SEGS];

// Writes the captured pages back wherever the same reservation still exists. Pages its owner decommitted
// since the save are committed again first: the restored owner believes they are committed. A segment
// whose region was released (or made read-only) since the save is left alone and counted in *skipped.
// No logging here: the other threads are suspended. (Sequential: these pages are resident, the copy is
// bound by memory bandwidth and the copy workers measurably gain nothing here.)
void va_revert(size_t* skipped, size_t* recommitted) {
    *skipped = *recommitted = 0;
    for (size_t i = 0; i < g_va_seg_count; i++) {
        const VaSeg& s = g_va_segs[i];
        uintptr_t a = s.addr, end = s.addr + s.len;
        bool ok = true;
        while (a < end && ok) {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery((void*)a, &mbi, sizeof(mbi)) || (uintptr_t)mbi.AllocationBase != s.alloc_base || mbi.State == MEM_FREE) {
                ok = false;
                break;
            }
            uintptr_t rend = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
            if (rend > end) rend = end;
            if (mbi.State == MEM_RESERVE) {
                ok = VirtualAlloc((void*)a, rend - a, MEM_COMMIT, PAGE_READWRITE) != nullptr;
                if (ok) (*recommitted)++;
            } else if (!va_writable(mbi.Protect)) {
                ok = false;
            }
            a = rend;
        }
        if (ok) memcpy((void*)s.addr, g_va_shadow + s.off, s.len);
        else (*skipped)++;
    }
}

// ---- blocks allocated since the save that threads still reference (see tlsguard.h) -----------------
const size_t MAX_ROOTS = 65536;
const size_t MAX_SCAN = 8u << 20;   // bytes of kept blocks scanned for further references
uintptr_t g_roots[MAX_ROOTS];
uint8_t*  g_marks = nullptr;        // per zombie: kept
uint32_t* g_work = nullptr;         // zombies kept but not scanned yet
size_t    g_marks_cap = 0;

// Index of the zombie containing address v, or -1 (z is sorted by address).
ptrdiff_t find_zombie(const arena::Range* z, size_t nz, uintptr_t v) {
    size_t lo = 0, hi = nz;
    while (lo < hi) { size_t mid = (lo + hi) / 2; if (z[mid].end <= v) lo = mid + 1; else hi = mid; }
    return lo < nz && z[lo].start <= v ? (ptrdiff_t)lo : -1;
}

// While the other threads are suspended: moves to the front of z (keeping the address order) the
// zombies that a live thread references from its TLS / FLS slots, and the zombies those reference in
// turn; returns how many. They are kept by the restore instead of being freed. Uses VirtualAlloc only
// (no heap: a suspended thread may own its lock).
size_t keep_thread_referenced(arena::Range* z, size_t nz, size_t* bytes) {
    *bytes = 0;
    if (!nz || !tlsguard::Installed()) return 0;
    if (g_marks_cap < nz) {
        if (g_marks) VirtualFree(g_marks, 0, MEM_RELEASE);
        size_t cap = nz + nz / 2;
        g_marks = (uint8_t*)VirtualAlloc(nullptr, cap * (1 + sizeof(uint32_t)), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        g_marks_cap = g_marks ? cap : 0;
        if (!g_marks) return 0;
    }
    g_work = (uint32_t*)(g_marks + g_marks_cap);
    memset(g_marks, 0, nz);
    int nthreads = 0;
    const void* const* threads = util::SuspendedThreads(&nthreads);
    size_t nroots = tlsguard::CollectRoots(threads, nthreads, g_roots, MAX_ROOTS);
    size_t nwork = 0, kept = 0, scanned = 0;
    auto mark = [&](uintptr_t v) {
        ptrdiff_t i = find_zombie(z, nz, v);
        if (i >= 0 && !g_marks[i]) { g_marks[i] = 1; g_work[nwork++] = (uint32_t)i; kept++; }
    };
    for (size_t r = 0; r < nroots; r++) mark(g_roots[r]);
    while (nwork && scanned < MAX_SCAN) {
        const arena::Range& b = z[g_work[--nwork]];
        for (uintptr_t p = (b.start + 3) & ~(uintptr_t)3; p + 4 <= b.end; p += 4) mark(*(const uintptr_t*)p);
        scanned += b.end - b.start;
    }
    size_t j = 0;
    for (size_t i = 0; i < nz; i++)
        if (g_marks[i]) { *bytes += z[i].end - z[i].start; z[j++] = z[i]; }
    return j;
}

// ---- ranges that keep their CURRENT value across a restore ----------------------------------------
struct Preserve { uintptr_t addr; size_t len; uint8_t saved[64]; const char* what; };
Preserve g_preserve[16];
int      g_preserve_count = 0;

void add_preserve(void* addr, size_t len, const char* what) {
    if (!addr || len > sizeof(g_preserve[0].saved) || g_preserve_count >= 16) return;
    g_preserve[g_preserve_count].addr = (uintptr_t)addr;
    g_preserve[g_preserve_count].len = len;
    g_preserve[g_preserve_count].what = what;
    g_preserve_count++;
}

// ---- timing of the phases of a save / restore (written to the log afterwards) -----------------------
struct Phases {
    const char* name[12];
    double      at[13];
    int         n = 0;
    Phases() { at[0] = util::Now(); }
    void mark(const char* what) {   // no logging here: the other threads may be suspended
        if (n < 12) { name[n] = what; at[++n] = util::Now(); }
    }
    void log(const char* op) const {
        char buf[512];
        int len = _snprintf(buf, sizeof(buf), "[snap] %s phases (ms):", op);
        for (int i = 0; i < n && len > 0 && len < (int)sizeof(buf); i++)
            len += _snprintf(buf + len, sizeof(buf) - len, " %s %.1f", name[i], (at[i + 1] - at[i]) * 1000.0);
        buf[sizeof(buf) - 1] = 0;
        util::Log("%s", buf);
    }
};

// ---- snapshot state ------------------------------------------------------------------------------
bool      g_valid = false;
uintptr_t g_image_end = 0;          // the arena image covers [arena::Base(), g_image_end)
int       g_level_gen = 0, g_snap_level_gen = -1;
bool      g_level_eligible = false;
char      g_map[128] = "", g_snap_map[128] = "";
double    g_snap_time = 0;
size_t    g_last_zombies = 0, g_last_pages = 0, g_total_zombie_bytes = 0;
int       g_restore_count = 0;
double    g_last_restore_ms = 0;
uint64_t  g_save_checksum = 0;

// Forgets the snapshot but keeps the buffers for the next save.
void invalidate() {
    if (!g_valid && !arena::SnapshotActive()) return;
    arena::Lock();
    arena::DropSnapshot();
    arena::Unlock();
    arena::FlushPendingOld();
    soundguard::OnDrop();
    d3dguard::OnDrop();
    fileguard::OnDrop();
    steamguard::OnDrop();
    videoguard::OnDrop();
    g_valid = false;
}

// Why the snapshot cannot outlive the current map instance (nullptr if it can). After a level change
// the engine has released the old map's GPU objects, files and MP3 decoders: only the guards keep the
// ones the snapshot references alive.
const char* level_change_blocker() {
    if (!memhook::RouteAll()) return "the plugin was not started by chocoload.exe";
    if (g_opts.zombie_policy != ZOMBIES_FREE) return "-snap_zombies_keep would keep the whole new map allocated";
    if (!d3dguard::Intact()) return d3dguard::Active() ? "its Direct3D objects could not all be kept (device reset?)" : "Direct3D objects are not tracked";
    if (!fileguard::Intact()) return "the game's file handles are not tracked";
    // The audio, Steam and video guards only matter when their module is in the process.
    if (GetModuleHandleA("vaudio_miles.dll") && !soundguard::Hooked()) return "MP3 decoders are not guarded";
    if (GetModuleHandleA("steam_api.dll") && !steamguard::Hooked()) return "Steam callbacks are not guarded";
    if (GetModuleHandleA("binkw32.dll") && !videoguard::Hooked()) return "Bink videos are not guarded";
    return nullptr;
}

} // namespace

Options& Opts() { return g_opts; }

bool Init(CreateInterfaceFn engine_factory, char* err, size_t errlen) {
    HMODULE vs = GetModuleHandleA("vstdlib.dll");
    void** pp = vs ? (void**)GetProcAddress(vs, "g_pThreadPool") : nullptr;
    g_pool = pp ? *pp : nullptr;
    if (g_pool) {
        int n = vfunc<PoolIntFn>(g_pool, poolslot::NumThreads)(g_pool);
        int idle = vfunc<PoolIntFn>(g_pool, poolslot::NumIdleThreads)(g_pool);
        util::Log("thread pool: %d threads (%d idle)", n, idle);
        if (n < 0 || n > 128 || idle < 0 || idle > n) {   // vtable layout sanity check
            util::Log("thread pool sanity check failed, not using it");
            g_pool = nullptr;
        }
    }
    g_fs = engine_factory ? engine_factory("VFileSystem017", nullptr) : nullptr;
    if (!g_fs) {   // early loader: no engine factory, ask filesystem_stdio directly
        HMODULE fsm = GetModuleHandleA("filesystem_stdio.dll");
        CreateInterfaceFn ff = fsm ? (CreateInterfaceFn)GetProcAddress(fsm, "CreateInterface") : nullptr;
        g_fs = ff ? ff("VFileSystem017", nullptr) : nullptr;
    }
    HMODULE cl = GetModuleHandleA("client.dll");
    CreateInterfaceFn cf = cl ? (CreateInterfaceFn)GetProcAddress(cl, "CreateInterface") : nullptr;
    g_client = cf ? cf("VClient016", nullptr) : nullptr;
    HMODULE en = GetModuleHandleA("engine.dll");
    CreateInterfaceFn ef = en ? (CreateInterfaceFn)GetProcAddress(en, "CreateInterface") : nullptr;
    g_sound = ef ? ef("IEngineSoundClient003", nullptr) : nullptr;

    // CEngine keeps real-time frame accumulators; reverting them would make the next frame see a huge
    // (or negative) frame time, so they keep their current value across a restore.
    util::Module eng;
    if (util::GetModule("engine.dll", &eng)) {
        void* vt = util::FindVTableByRTTI(eng, ".?AVCEngine@@");
        g_cengine = vt ? (uint8_t*)util::FindObjectWithVTable(eng, vt) : nullptr;
        util::Log("CEngine vtable %p object %p", vt, g_cengine);
    }
    g_preserve_count = 0;
    if (g_cengine) add_preserve(g_cengine + 0x10, 0x1C, "CEngine frame timing");

    util::Log("snapshot init: pool=%p fs=%p client=%p sound=%p", g_pool, g_fs, g_client, g_sound);
    if (!g_fs) { _snprintf(err, errlen, "VFileSystem017 not found"); return false; }
    return true;
}

void SetDomain(const char* list) {
    strncpy(g_domain, list, sizeof(g_domain) - 1);
    g_domain[sizeof(g_domain) - 1] = 0;
    refresh_modules();
}

bool Valid() { return g_valid; }
double LastRestoreMs() { return g_last_restore_ms; }
bool LevelEligible() { return g_level_eligible; }
void ForceEligible(bool v) { g_level_eligible = v; }

void OnLevelInit(const char* map) {
    g_level_gen++;
    strncpy(g_map, map ? map : "", sizeof(g_map) - 1);
    // The level's objects are in the arena only if the hook was active before the map loaded.
    g_level_eligible = memhook::Installed();
    refresh_modules();
}

void OnLevelShutdown() {
    g_level_eligible = false;
    if (!g_valid) return;
    if (const char* why = level_change_blocker()) {
        util::Con("[snap] level change: the snapshot of '%s' is dropped (%s)", g_snap_map, why);
        Drop();
    } else {
        util::Log("[snap] level change: the snapshot of '%s' is kept", g_snap_map);
    }
}

void Drop() {
    invalidate();
    store_free();
    dirty_free();
    modules_free();
    va_free();
}

bool Save(char* err, size_t errlen) {
    if (!memhook::Installed()) { _snprintf(err, errlen, "allocator hook not installed"); return false; }
    if (!g_level_eligible) { _snprintf(err, errlen, "this map was loaded before the plugin: reload the map first"); return false; }
    if (memhook::Overflowed()) util::ConWarn("[snap] warning: the arena overflowed earlier, the snapshot may be incomplete");
    invalidate();   // a save replaces the previous snapshot; if it fails there is no snapshot at all
    double t0 = util::Now();

    if (!soundguard::Hooked()) util::ConWarn("[snap] warning: MP3 decoders are not guarded, a restore may crash if a voice line or music plays now");
    quiesce_begin();
    refresh_modules();
    for (int i = 0; i < g_mod_count; i++)
        util::Log("[snap]   module %-24s %-8s %08x-%08x %6u KB", g_mods[i].module, g_mods[i].section,
                  (unsigned)g_mods[i].start, (unsigned)g_mods[i].end, (unsigned)((g_mods[i].end - g_mods[i].start) >> 10));
    if (g_mods_truncated) util::ConWarn("[snap] warning: more than %d writable sections, some are NOT captured", MAX_MODS);
    bool capture_va = g_opts.capture_va && vatrack::Installed();
    if (g_opts.capture_va && !capture_va) util::ConWarn("[snap] warning: VirtualAlloc tracking unavailable, the engine hunk is NOT captured");
    if (capture_va) {
        vatrack::LogRegions();
        if (vatrack::Overflowed()) util::ConWarn("[snap] warning: too many VirtualAlloc regions, some are NOT captured");
    }

    // The copy workers are created by the first save (the option is parsed after Init).
    static bool pool_started = false;
    if (!pool_started) {
        pool_started = true;
        int n = g_opts.copy_threads;
        if (n < 0) {
            SYSTEM_INFO si;
            GetSystemInfo(&si);
            n = (int)si.dwNumberOfProcessors - 1;
            if (n > 3) n = 3;
        }
        copypool::Start(n);
    }

    // Allocate everything up front: once the other threads are suspended we only copy.
    uintptr_t lo, hi;
    arena::HeapSpan(&lo, &hi);
    size_t estimate = (hi - arena::Base()) + (64u << 20);   // headroom for growth until the freeze
    const char* fail = nullptr;
    if (!store_ensure(estimate)) fail = "could not create the snapshot store";
    else if (!dirty_ensure(estimate / 4096 + 16)) fail = "out of memory (dirty page list)";
    else if (!modules_alloc()) fail = "out of memory (module images)";
    else if (capture_va && !va_reserve(va_capture_size() + (8u << 20))) fail = "out of memory (VirtualAlloc regions)";
    if (fail) { quiesce_end(); _snprintf(err, errlen, "%s", fail); return false; }

    double t1 = util::Now();
    Phases ph;
    util::MuteConsole(true);
    int suspended = g_opts.suspend_threads ? util::SuspendOtherThreads(copypool::ThreadIds(), copypool::Workers()) : 0;
    ph.mark("suspend");
    arena::Lock();
    uintptr_t end = arena::BeginSnapshot();
    ph.mark("freeze");
    if (!end || end - arena::Base() > g_section_size) fail = "arena image does not fit the store";
    else if (!store_write((const uint8_t*)arena::Base(), end - arena::Base())) fail = "could not write the arena image";
    ph.mark("arena");
    if (!fail) {
        int n = 0;
        for (int i = 0; i < g_mod_count; i++)
            g_copies[n++] = copypool::Copy{ g_mods[i].data, (void*)g_mods[i].start, g_mods[i].end - g_mods[i].start };
        g_va_seg_count = 0;
        g_va_bytes = 0;
        if (capture_va && !va_layout()) fail = "could not capture the VirtualAlloc regions";
        for (size_t i = 0; !fail && i < g_va_seg_count; i++)
            g_copies[n++] = copypool::Copy{ g_va_shadow + g_va_segs[i].off, (void*)g_va_segs[i].addr, g_va_segs[i].len };
        if (!fail) copypool::CopyAll(g_copies, n);
        ph.mark("modules+VA");
    }
    if (!fail && g_opts.verify_heap) g_save_checksum = arena::ChecksumAllocated();
    arena::EndSnapshot();
    if (fail) arena::DropSnapshot();
    arena::Unlock();
    store_unmap();
    ph.mark("unmap");
    if (suspended) util::ResumeOtherThreads();
    ph.mark("resume");
    util::MuteConsole(false);
    quiesce_end();
    arena::FlushPendingOld();
    double t2 = util::Now();
    if (fail) { _snprintf(err, errlen, "%s", fail); return false; }
    ph.log("save");

    // Everything outside the memory image that the snapshot references is pinned now: MP3 decoders,
    // Direct3D objects, open files, Steam callback registrations, Bink videos.
    soundguard::OnSave();
    d3dguard::OnSave();
    fileguard::OnSave();
    steamguard::OnSave();
    videoguard::OnSave();
    g_valid = true;
    g_image_end = end;
    g_snap_level_gen = g_level_gen;
    strcpy(g_snap_map, g_map);
    g_snap_time = util::Now();
    g_restore_count = 0;
    util::Con("[snap] saved: arena %.1f MB + modules %.1f MB + VA %.1f MB (%u segs) in %.1f ms (prepare %.1f ms, %d threads)",
              (end - arena::Base()) / 1048576.0, modules_bytes() / 1048576.0, g_va_bytes / 1048576.0,
              (unsigned)g_va_seg_count, (t2 - t1) * 1000.0, (t1 - t0) * 1000.0, suspended);
    if (g_opts.verify_heap) {
        char e[256];
        if (!arena::Check(e, sizeof(e))) util::ConWarn("[snap] heap check FAILED after save: %s", e);
    }
    return true;
}

bool Restore(char* err, size_t errlen) {
    if (!g_valid) { _snprintf(err, errlen, "no snapshot"); return false; }
    bool other_level = g_snap_level_gen != g_level_gen;   // the snapshot outlived a level change
    if (d3dguard::Active() && !d3dguard::Intact()) {
        // A device reset released the GPU objects the snapshot refers to (even in the same map).
        _snprintf(err, errlen, "the graphics device was reset since the snapshot was taken (alt-tab in fullscreen, "
                  "video mode change): take a new snapshot");
        Drop();
        return false;
    }
    if (other_level) util::Log("[snap] restoring the snapshot of '%s' from '%s'", g_snap_map, g_map);
    double t0 = util::Now();

    stop_all_sounds();
    soundguard::BeforeRestore();
    steamguard::BeforeRestore();
    videoguard::BeforeRestore();
    quiesce_begin();
    for (int i = 0; i < g_preserve_count; i++) memcpy(g_preserve[i].saved, (void*)g_preserve[i].addr, g_preserve[i].len);

    double t1 = util::Now();
    Phases ph;
    util::MuteConsole(true);
    int suspended = g_opts.suspend_threads ? util::SuspendOtherThreads() : 0;
    ph.mark("suspend");
    arena::Lock();
    char fail[256] = "";
    size_t nz = 0, kept = 0, zbytes = 0, pages = 0, va_skipped = 0, va_recommitted = 0;
    bool checksum_ok = true;
    arena::Range* z = arena::CollectZombies(&nz);
    ph.mark("zombies");
    if (nz == (size_t)-1) {
        strcpy(fail, "heap walk failed (corruption or out of memory) - nothing restored");
    } else {
        if (g_opts.zombie_policy == ZOMBIES_KEEP) {
            kept = nz;
            for (size_t i = 0; i < kept; i++) zbytes += z[i].end - z[i].start;
        } else {
            kept = g_opts.keep_thread_refs ? keep_thread_referenced(z, nz, &zbytes) : 0;
        }
        ph.mark("thread refs");
        // 1) arena: the pages written since the snapshot (or everything on overflow / full_revert)
        bool full = g_opts.full_revert;
        size_t n = full ? 0 : arena::DirtyPages(arena::Base(), g_image_end, g_dirty, g_dirty_cap, &full);
        ph.mark("dirty list");
        bool ok = true;
        if (full) {
            ok = restore_range(arena::Base(), g_image_end, z, kept);
            pages = (g_image_end - arena::Base()) / 4096;
        } else {
            for (size_t i = 0; i < n && ok; i++) {
                uintptr_t a = g_dirty[i], b = a + 4096;
                if (a < arena::Base()) a = arena::Base();
                if (b > g_image_end) b = g_image_end;
                if (a < b) ok = restore_range(a, b, z, kept);
            }
            pages = n;
        }
        if (!ok) {
            strcpy(fail, "could not read the arena image - the game state is now inconsistent");
        } else {
            // 2) module sections and VirtualAlloc regions, 3) rebuild the arena free lists
            ph.mark("arena");
            for (int i = 0; i < g_mod_count; i++) memcpy((void*)g_mods[i].start, g_mods[i].data, g_mods[i].end - g_mods[i].start);
            va_revert(&va_skipped, &va_recommitted);
            ph.mark("modules+VA");
            bool rebuilt = arena::FinishRestore(kept, fail, sizeof(fail));
            ph.mark("rebuild");
            if (!rebuilt) {
                char why[200];
                strncpy(why, fail, sizeof(why) - 1);
                why[sizeof(why) - 1] = 0;
                _snprintf(fail, sizeof(fail), "arena rebuild failed: %s", why);
            } else if (g_opts.verify_heap && g_opts.zombie_policy == ZOMBIES_FREE && !kept) {
                checksum_ok = arena::ChecksumAllocated() == g_save_checksum;
            }
        }
    }
    arena::Unlock();
    store_unmap();
    for (int i = 0; i < g_preserve_count; i++) memcpy((void*)g_preserve[i].addr, g_preserve[i].saved, g_preserve[i].len);
    ph.mark("unmap");
    if (suspended) util::ResumeOtherThreads();
    ph.mark("resume");
    util::MuteConsole(false);
    quiesce_end();
    double t2 = util::Now();
    if (fail[0]) { _snprintf(err, errlen, "%s", fail); return false; }
    ph.log("restore");
    if (va_recommitted) util::Log("[snap] %u VirtualAlloc ranges decommitted since the save were committed again", (unsigned)va_recommitted);
    if (va_skipped)
        util::ConWarn("[snap] warning: %u VirtualAlloc segments could not be reverted (their region was released since the save)",
                      (unsigned)va_skipped);

    soundguard::AfterRestore();
    d3dguard::AfterRestore();
    fileguard::AfterRestore();
    steamguard::AfterRestore();
    videoguard::AfterRestore();
    if (other_level) {   // we are back in the snapshot's map instance
        g_level_gen = g_snap_level_gen;
        strcpy(g_map, g_snap_map);
        g_level_eligible = true;
    }
    if (g_opts.clear_input && g_client) vfunc<VoidFn>(g_client, clientslot::IN_ClearStates)(g_client);
    g_restore_count++;
    g_last_zombies = kept;
    g_total_zombie_bytes += zbytes;
    g_last_pages = pages;
    g_last_restore_ms = (t2 - t0) * 1000.0;
    util::Con("[snap] restored%s in %.1f ms (prepare %.1f ms): %u pages, %u zombies kept (%.1f KB), %d threads",
              other_level ? " (back to the snapshot's map)" : "", (t2 - t1) * 1000.0, (t1 - t0) * 1000.0, (unsigned)pages,
              (unsigned)kept, zbytes / 1024.0, suspended);
    if (g_opts.verify_heap) {
        char e[256];
        if (!arena::Check(e, sizeof(e))) util::ConWarn("[snap] heap check FAILED after restore: %s", e);
        if (g_opts.zombie_policy == ZOMBIES_FREE && !kept) {   // kept blocks are extra allocated blocks
            if (checksum_ok) util::Con("[snap] allocated-blocks checksum MATCHES save (arena fully reverted)");
            else util::ConWarn("[snap] allocated-blocks checksum MISMATCH (arena NOT fully reverted!)");
        }
    }
    return true;
}

void PrintInfo() {
    arena::Stats st = arena::GetStats();
    util::Con("[snap] domain: %s", memhook::DomainDescription());
    util::Con("[snap] arena: base %p, heap %.1f MB, committed %.1f MB, used %.1f MB in %u blocks (%u frozen), "
              "%u free blocks, %u deferred frees, %u deferred old frees",
              (void*)arena::Base(), st.heap_bytes / 1048576.0, st.committed / 1048576.0, st.used_bytes / 1048576.0,
              (unsigned)st.used_blocks, (unsigned)st.frozen_blocks, (unsigned)st.free_blocks, (unsigned)st.deferred,
              (unsigned)arena::DeferredOldCount());
    util::Con("[snap] module sections: %d (%.1f MB), VirtualAlloc regions: %s", g_mod_count, modules_bytes() / 1048576.0,
              vatrack::Installed() ? "tracked" : "NOT tracked");
    util::Con("[snap] map '%s' eligible=%d overflow=%d", g_map, (int)g_level_eligible, (int)memhook::Overflowed());
    if (g_valid)
        util::Con("[snap] snapshot of '%s'%s (%.0f s ago): image %.1f MB + VA %.1f MB, restored %d times, "
                  "last restore %u pages / %u zombies kept, zombies total %.1f MB",
                  g_snap_map, g_snap_level_gen != g_level_gen ? " (another map: snap_load goes back there)" : "",
                  util::Now() - g_snap_time, (g_image_end - arena::Base()) / 1048576.0, g_va_bytes / 1048576.0,
                  g_restore_count, (unsigned)g_last_pages, (unsigned)g_last_zombies, g_total_zombie_bytes / 1048576.0);
    else
        util::Con("[snap] no snapshot");
}

} // namespace snapshot
