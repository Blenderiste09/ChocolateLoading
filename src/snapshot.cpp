#define _CRT_SECURE_NO_WARNINGS
#include "snapshot.h"
#include "arena.h"
#include "copypool.h"
#include "d3dguard.h"
#include "fileguard.h"
#include "memhook.h"
#include "soundguard.h"
#include "steamguard.h"
#include "vatrack.h"
#include "videoguard.h"
#include "util.h"
#include <windows.h>
#include <string.h>
#include <stdio.h>

namespace snapshot {

const char* const DEFAULT_NAME = "quick";

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
// snapshots still need alive); the restore then brings back the sounds that played at save time.
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

// ---- module sections: the writable sections of the domain modules ---------------------------------
const int MAX_MODS = 96;
struct ModSection { char module[64]; char section[9]; uintptr_t start, end; };
ModSection g_mods[MAX_MODS];
int        g_mod_count = 0;
bool       g_mods_truncated = false;
char       g_domain[1024] = "";

void refresh_modules() {
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
            ModSection& ms = g_mods[g_mod_count++];
            memset(&ms, 0, sizeof(ms));
            strncpy(ms.module, m.name, sizeof(ms.module) - 1);
            memcpy(ms.section, m.rw[s].name, sizeof(ms.section));
            ms.start = m.rw[s].start;
            ms.end = m.rw[s].end;
        }
    }
}

size_t modules_bytes() {
    size_t n = 0;
    for (int i = 0; i < g_mod_count; i++) n += g_mods[i].end - g_mods[i].start;
    return n;
}

// ---- engine direct-VirtualAlloc regions (the hunk): committed writable spans -----------------------
const size_t MAX_VA_SEGS = 8192;

inline bool va_writable(DWORD protect) {
    DWORD p = protect & 0xFF;   // ignore the PAGE_GUARD/NOCACHE modifier bits
    return !(protect & PAGE_GUARD) &&
           (p == PAGE_READWRITE || p == PAGE_EXECUTE_READWRITE || p == PAGE_WRITECOPY || p == PAGE_EXECUTE_WRITECOPY);
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

size_t va_capture_size() {
    size_t total = 0;
    va_for_each_span([&](uintptr_t a, uintptr_t b, uintptr_t) { total += b - a; return true; });
    return total;
}

// ---- snapshots ------------------------------------------------------------------------------------
// One pagefile-backed section per snapshot: the arena image [0, image_end - Base), then the module
// sections and the VirtualAlloc segments back to back (Seg::off). The arena is cut into chunks and
// segments into pieces that are each copied through their own view, in parallel (copypool), since most
// of the cost is the page faults of fresh views.
struct Seg { uintptr_t addr, alloc_base; size_t off, len; };   // alloc_base 0: a module section
struct Snap {
    bool      used;
    char      name[MAX_NAME];
    HANDLE    section;
    size_t    section_size;
    uintptr_t image_end;                 // the arena image covers [arena::Base(), image_end)
    Seg*      segs;                      // module sections, then VirtualAlloc segments
    size_t    nsegs, nmods;
    size_t    mod_bytes, va_bytes;
    arena::Range* free_s;                // the arena's free ranges at save time (for a full restore)
    size_t    nfree, free_cap;
    uintptr_t top_s;
    int       level_gen;
    char      map[128];
    double    time;
    int       restores;
    uint64_t  checksum;
    uint64_t  order;                     // save order, for Names()
};
Snap     g_snaps[MAX_SNAPSHOTS];
int      g_synced = -1;      // the snapshot the arena's write watch and frozen blocks follow (last saved/restored)
uint64_t g_save_counter = 0;

int find_snap(const char* name) {
    for (int i = 0; i < MAX_SNAPSHOTS; i++) if (g_snaps[i].used && !_stricmp(g_snaps[i].name, name)) return i;
    return -1;
}

void section_free(Snap& s) {
    if (s.section) CloseHandle(s.section);
    s.section = nullptr;
    s.section_size = 0;
}

bool section_ensure(Snap& s, size_t size) {
    size = (size + 0xFFFF) & ~(size_t)0xFFFF;
    if (s.section && s.section_size >= size) return true;
    section_free(s);
    s.section = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, (DWORD)size, nullptr);
    s.section_size = s.section ? size : 0;
    return s.section != nullptr;
}

// ---- parallel copies between memory and a section (each piece through its own view) ---------------
const size_t PIECE = 4u << 20;
const int    MAX_PIECES = 16384;
struct Piece { uint8_t* mem; size_t off, len; };
Piece         g_pieces[MAX_PIECES];
int           g_npieces = 0;
HANDLE        g_job_section = nullptr;
bool          g_job_write = false;
const uint8_t* g_job_view = nullptr;   // reads: a view already mapped over the section from g_job_view_off
size_t        g_job_view_off = 0;
volatile LONG g_job_failed = 0;

bool pieces_add(void* mem, size_t off, size_t len) {
    for (size_t done = 0; done < len; done += PIECE) {
        if (g_npieces == MAX_PIECES) return false;
        size_t n = len - done < PIECE ? len - done : PIECE;
        g_pieces[g_npieces++] = Piece{ (uint8_t*)mem + done, off + done, n };
    }
    return true;
}

void piece_copy(void*, int i) {
    const Piece& p = g_pieces[i];
    if (g_job_view) { memcpy(p.mem, g_job_view + (p.off - g_job_view_off), p.len); return; }
    size_t base = p.off & ~(size_t)0xFFFF, delta = p.off - base;   // views start on the allocation granularity
    uint8_t* v = (uint8_t*)MapViewOfFile(g_job_section, g_job_write ? FILE_MAP_WRITE : FILE_MAP_READ, 0, (DWORD)base, delta + p.len);
    if (!v) { InterlockedExchange(&g_job_failed, 1); return; }
    if (g_job_write) memcpy(v + delta, p.mem, p.len);
    else memcpy(p.mem, v + delta, p.len);
    UnmapViewOfFile(v);
}

// Copies the listed pieces to (write) or from the section, each piece through its own view, or from
// `view` (mapped at section offset `view_off`) when given.
bool pieces_run(HANDLE section, bool write, const uint8_t* view = nullptr, size_t view_off = 0) {
    g_job_section = section;
    g_job_write = write;
    g_job_view = view;
    g_job_view_off = view_off;
    g_job_failed = 0;
    copypool::Run(piece_copy, nullptr, g_npieces);
    g_npieces = 0;
    return !g_job_failed;
}

// ---- sequential reads through one sliding view (dirty pages of the arena) ----------------------------
const size_t WINDOW = 32u << 20;   // multiple of the 64 KB allocation granularity
HANDLE   g_view_section = nullptr;
uint8_t* g_view = nullptr;
size_t   g_view_index = (size_t)-1;

void view_unmap() {
    if (g_view) UnmapViewOfFile(g_view);
    g_view = nullptr;
    g_view_index = (size_t)-1;
}

bool section_read(const Snap& s, size_t off, uint8_t* dst, size_t len) {
    while (len) {
        size_t in = off % WINDOW, n = WINDOW - in, index = off / WINDOW;
        if (n > len) n = len;
        if (g_view_section != s.section || g_view_index != index) {
            view_unmap();
            size_t at = index * WINDOW, vlen = s.section_size - at < WINDOW ? s.section_size - at : WINDOW;
            g_view = (uint8_t*)MapViewOfFile(s.section, FILE_MAP_READ, 0, (DWORD)at, vlen);
            if (!g_view) return false;
            g_view_section = s.section;
            g_view_index = index;
        }
        memcpy(dst, g_view + in, n);
        off += n; dst += n; len -= n;
    }
    return true;
}

// Copies [a, b) of the arena back from the image, skipping the kept zombie ranges (sorted).
bool restore_range(const Snap& s, uintptr_t a, uintptr_t b, const arena::Range* z, size_t nz) {
    uintptr_t base = arena::Base();
    size_t lo = 0, hi = nz;
    while (lo < hi) { size_t mid = (lo + hi) / 2; if (z[mid].end <= a) lo = mid + 1; else hi = mid; }
    uintptr_t cur = a;
    for (size_t i = lo; i < nz && z[i].start < b && cur < b; i++) {
        if (z[i].start > cur && !section_read(s, cur - base, (uint8_t*)cur, z[i].start - cur)) return false;
        if (z[i].end > cur) cur = z[i].end;
    }
    if (cur < b && !section_read(s, cur - base, (uint8_t*)cur, b - cur)) return false;
    return true;
}

// ---- the module / VirtualAlloc copies of the current snapshot, kept mapped -------------------------
// Restoring the current snapshot copies them back whole (~56 MB). Through fresh views that would cost
// the page faults of mapping them again at every restore, so one view over them stays mapped.
uint8_t* g_seg_view = nullptr;
int      g_seg_view_slot = -1;
size_t   g_seg_view_off = 0;

void seg_view_unmap() {
    if (g_seg_view) UnmapViewOfFile(g_seg_view);
    g_seg_view = nullptr;
    g_seg_view_slot = -1;
}

void seg_view_map(int slot) {
    if (g_seg_view_slot == slot) return;
    seg_view_unmap();
    const Snap& s = g_snaps[slot];
    if (!s.nsegs) return;
    size_t off = s.segs[0].off & ~(size_t)0xFFFF, end = s.segs[s.nsegs - 1].off + s.segs[s.nsegs - 1].len;
    g_seg_view = (uint8_t*)MapViewOfFile(s.section, FILE_MAP_READ, 0, (DWORD)off, end - off);
    if (!g_seg_view) return;   // restores then map a view per piece
    g_seg_view_slot = slot;
    g_seg_view_off = off;
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

// ---- segments: layout at save time, checks at restore time -------------------------------------------
bool segs_ensure(Snap& s) {
    if (s.segs) return true;
    s.segs = (Seg*)VirtualAlloc(nullptr, (MAX_MODS + MAX_VA_SEGS) * sizeof(Seg), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    return s.segs != nullptr;
}

// Lays out the module sections and the VirtualAlloc segments after the arena image (offset `off`).
// Runs while the other threads are suspended. Fails rather than producing a partial image.
bool segs_layout(Snap& s, size_t off, bool capture_va) {
    s.nsegs = 0;
    s.mod_bytes = s.va_bytes = 0;
    for (int i = 0; i < g_mod_count; i++) {
        size_t len = g_mods[i].end - g_mods[i].start;
        s.segs[s.nsegs++] = Seg{ g_mods[i].start, 0, off, len };
        off += len;
        s.mod_bytes += len;
    }
    s.nmods = s.nsegs;
    if (!capture_va) return off <= s.section_size;
    return va_for_each_span([&](uintptr_t a, uintptr_t b, uintptr_t alloc_base) {
        if (s.nsegs == MAX_MODS + MAX_VA_SEGS || off + (b - a) > s.section_size) return false;
        s.segs[s.nsegs++] = Seg{ a, alloc_base, off, b - a };
        off += b - a;
        s.va_bytes += b - a;
        return true;
    });
}

// Whether a captured VirtualAlloc segment can be written back: the same reservation must still exist.
// Pages its owner decommitted since the save are committed again first (the restored owner believes
// they are committed). False if the region was released (or made read-only) since the save. No logging
// here: the other threads are suspended.
bool va_writable_again(const Seg& g, size_t* recommitted) {
    uintptr_t a = g.addr, end = g.addr + g.len;
    while (a < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void*)a, &mbi, sizeof(mbi)) || (uintptr_t)mbi.AllocationBase != g.alloc_base || mbi.State == MEM_FREE)
            return false;
        uintptr_t rend = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (rend > end) rend = end;
        if (mbi.State == MEM_RESERVE) {
            if (!VirtualAlloc((void*)a, rend - a, MEM_COMMIT, PAGE_READWRITE)) return false;
            (*recommitted)++;
        } else if (!va_writable(mbi.Protect)) {
            return false;
        }
        a = rend;
    }
    return true;
}

// A module section is usually several regions: its copy-on-write pages become read-write once written.
bool module_writable(const Seg& g) {
    for (uintptr_t a = g.addr, end = g.addr + g.len; a < end;) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void*)a, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || mbi.Type != MEM_IMAGE || !va_writable(mbi.Protect))
            return false;
        a = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
    return true;
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

// ---- level state -----------------------------------------------------------------------------------
int    g_level_gen = 0;
bool   g_level_eligible = false;
char   g_map[128] = "";
double g_last_restore_ms = 0;

// Releases what the snapshot in `slot` holds: the guards' pins, the arena's bookkeeping if it followed
// that snapshot, and (unless `keep_buffers`) its section.
void release_slot(int slot, bool keep_buffers) {
    Snap& s = g_snaps[slot];
    if (!s.used) return;
    if (g_seg_view_slot == slot) seg_view_unmap();
    if (g_synced == slot) {
        arena::Lock();
        arena::DropSnapshot();
        arena::Unlock();
        arena::FlushPendingOld();
        g_synced = -1;
    }
    soundguard::OnDrop(slot);
    d3dguard::OnDrop(slot);
    fileguard::OnDrop(slot);
    steamguard::OnDrop(slot);
    videoguard::OnDrop(slot);
    s.used = false;
    if (!keep_buffers) section_free(s);
}

// Why the snapshots cannot outlive the current map instance (nullptr if they can). After a level change
// the engine has released the old map's GPU objects, files and MP3 decoders: only the guards keep the
// ones the snapshots reference alive.
const char* level_change_blocker() {
    if (!memhook::RouteAll()) return "the plugin was not started by chocoload.exe";
    if (g_opts.zombie_policy != ZOMBIES_FREE) return "-snap_zombies_keep would keep the whole new map allocated";
    if (!d3dguard::Healthy()) return d3dguard::Active() ? "Direct3D objects could not all be tracked" : "Direct3D objects are not tracked";
    if (!fileguard::Intact()) return "the game's file handles are not tracked";
    // The audio, Steam and video guards only matter when their module is in the process.
    if (GetModuleHandleA("vaudio_miles.dll") && !soundguard::Hooked()) return "MP3 decoders are not guarded";
    if (GetModuleHandleA("steam_api.dll") && !steamguard::Hooked()) return "Steam callbacks are not guarded";
    if (GetModuleHandleA("binkw32.dll") && !videoguard::Hooked()) return "Bink videos are not guarded";
    return nullptr;
}

void start_copy_workers() {
    static bool started = false;   // created by the first save (the option is parsed after Init)
    if (started) return;
    started = true;
    int n = g_opts.copy_threads;
    if (n < 0) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        n = (int)si.dwNumberOfProcessors - 1;
        if (n > 3) n = 3;
    }
    copypool::Start(n);
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

bool ValidName(const char* name) {
    size_t n = name ? strlen(name) : 0;
    if (!n || n >= MAX_NAME) return false;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        if (!ok) return false;
    }
    return true;
}

bool Exists(const char* name) { return find_snap(name) >= 0; }

int Count() {
    int n = 0;
    for (int i = 0; i < MAX_SNAPSHOTS; i++) n += g_snaps[i].used;
    return n;
}

int Names(const char** out, int max) {
    int idx[MAX_SNAPSHOTS], n = 0;
    for (int i = 0; i < MAX_SNAPSHOTS; i++) if (g_snaps[i].used) idx[n++] = i;
    for (int i = 1; i < n; i++)   // oldest first
        for (int j = i; j > 0 && g_snaps[idx[j]].order < g_snaps[idx[j - 1]].order; j--) { int t = idx[j]; idx[j] = idx[j - 1]; idx[j - 1] = t; }
    for (int i = 0; i < n && i < max; i++) out[i] = g_snaps[idx[i]].name;
    return n < max ? n : max;
}

const char* Current() { return g_synced >= 0 ? g_snaps[g_synced].name : ""; }
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
    int n = Count();
    if (!n) return;
    if (const char* why = level_change_blocker()) {
        util::Con("[snap] level change: %d snapshot(s) dropped (%s)", n, why);
        DropAll();
    } else {
        util::Log("[snap] level change: %d snapshot(s) kept", n);
    }
}

bool Drop(const char* name) {
    int slot = find_snap(name);
    if (slot < 0) return false;
    release_slot(slot, false);
    return true;
}

void DropAll() {
    for (int i = 0; i < MAX_SNAPSHOTS; i++) release_slot(i, false);
}

bool Save(const char* name, char* err, size_t errlen) {
    if (!ValidName(name)) { _snprintf(err, errlen, "invalid name '%s' (letters, digits, _ - . only)", name); return false; }
    if (!memhook::Installed()) { _snprintf(err, errlen, "allocator hook not installed"); return false; }
    if (!g_level_eligible) { _snprintf(err, errlen, "this map was loaded before the plugin: reload the map first"); return false; }
    int slot = find_snap(name);
    if (slot < 0)
        for (int i = 0; i < MAX_SNAPSHOTS && slot < 0; i++) if (!g_snaps[i].used) slot = i;
    if (slot < 0) { _snprintf(err, errlen, "%d snapshots already exist: drop one first (snap_drop <name>)", MAX_SNAPSHOTS); return false; }
    if (memhook::Overflowed()) util::ConWarn("[snap] warning: the arena overflowed earlier, the snapshot may be incomplete");
    // A save replaces the snapshot of that name; if it fails there is no snapshot of that name at all.
    Snap& s = g_snaps[slot];
    release_slot(slot, true);
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
    start_copy_workers();

    // Allocate everything up front: once the other threads are suspended we only copy.
    uintptr_t lo, hi;
    arena::HeapSpan(&lo, &hi);
    size_t arena_room = (((hi - arena::Base()) + (64u << 20)) + 0xFFFF) & ~(size_t)0xFFFF;   // + growth until the freeze
    size_t total = arena_room + modules_bytes() + (capture_va ? va_capture_size() + (8u << 20) : 0);
    const char* fail = nullptr;
    if (!section_ensure(s, total)) fail = "could not create the snapshot store";
    else if (!segs_ensure(s)) fail = "out of memory (segment list)";
    else if (!dirty_ensure(arena_room / 4096 + 16)) fail = "out of memory (dirty page list)";
    if (fail) { quiesce_end(); _snprintf(err, errlen, "%s", fail); return false; }

    double t1 = util::Now();
    Phases ph;
    util::MuteConsole(true);
    int suspended = g_opts.suspend_threads ? util::SuspendOtherThreads(copypool::ThreadIds(), copypool::Workers()) : 0;
    ph.mark("suspend");
    arena::Lock();
    uintptr_t end = arena::BeginSnapshot();
    ph.mark("freeze");
    if (!end || end - arena::Base() > arena_room) fail = "arena image does not fit the store";
    else if (!segs_layout(s, arena_room, capture_va)) fail = "could not lay out the module and VirtualAlloc copies";
    if (!fail) {
        // The arena's free ranges at this snapshot, for a full restore later.
        size_t nfree;
        uintptr_t top;
        const arena::Range* fr = arena::SnapshotFreeRanges(&nfree, &top);
        if (s.free_cap < nfree) {
            if (s.free_s) VirtualFree(s.free_s, 0, MEM_RELEASE);
            s.free_cap = nfree + nfree / 2 + 1024;
            s.free_s = (arena::Range*)VirtualAlloc(nullptr, s.free_cap * sizeof(arena::Range), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!s.free_s) s.free_cap = 0;
        }
        if (!s.free_s && nfree) fail = "out of memory (free ranges)";
        else {
            memcpy(s.free_s, fr, nfree * sizeof(arena::Range));
            s.nfree = nfree;
            s.top_s = top;
        }
    }
    if (!fail) {
        bool listed = pieces_add((void*)arena::Base(), 0, end - arena::Base());
        for (size_t i = 0; listed && i < s.nsegs; i++) listed = pieces_add((void*)s.segs[i].addr, s.segs[i].off, s.segs[i].len);
        if (!pieces_run(s.section, true) || !listed) fail = "could not write the snapshot";
        ph.mark("copy");
    }
    if (!fail && g_opts.verify_heap) s.checksum = arena::ChecksumAllocated();
    arena::EndSnapshot();
    if (fail) arena::DropSnapshot();
    arena::Unlock();
    ph.mark("unlock");
    if (suspended) util::ResumeOtherThreads();
    ph.mark("resume");
    util::MuteConsole(false);
    quiesce_end();
    arena::FlushPendingOld();
    double t2 = util::Now();
    if (fail) {
        g_synced = -1;
        section_free(s);
        _snprintf(err, errlen, "%s", fail);
        return false;
    }
    ph.log("save");

    // Everything outside the memory image that the snapshot references is pinned now: MP3 decoders,
    // Direct3D objects, open files, Steam callback registrations, Bink videos.
    soundguard::OnSave(slot);
    d3dguard::OnSave(slot);
    fileguard::OnSave(slot);
    steamguard::OnSave(slot);
    videoguard::OnSave(slot);
    s.used = true;
    strncpy(s.name, name, MAX_NAME - 1);
    s.name[MAX_NAME - 1] = 0;
    s.image_end = end;
    s.level_gen = g_level_gen;
    strcpy(s.map, g_map);
    s.time = util::Now();
    s.restores = 0;
    s.order = ++g_save_counter;
    g_synced = slot;
    seg_view_map(slot);
    util::Con("[snap] saved '%s': arena %.1f MB + modules %.1f MB + VA %.1f MB in %.1f ms (prepare %.1f ms, %d threads)",
              s.name, (end - arena::Base()) / 1048576.0, s.mod_bytes / 1048576.0, s.va_bytes / 1048576.0,
              (t2 - t1) * 1000.0, (t1 - t0) * 1000.0, suspended);
    if (g_opts.verify_heap) {
        char e[256];
        if (!arena::Check(e, sizeof(e))) util::ConWarn("[snap] heap check FAILED after save: %s", e);
    }
    return true;
}

bool Restore(const char* name, char* err, size_t errlen) {
    int slot = find_snap(name);
    if (slot < 0) { _snprintf(err, errlen, "no snapshot named '%s'", name); return false; }
    Snap& s = g_snaps[slot];
    if (d3dguard::Active() && !d3dguard::Intact(slot)) {
        // A device reset released the GPU objects the snapshot refers to (even in the same map).
        _snprintf(err, errlen, "the graphics device was reset since '%s' was taken (alt-tab in fullscreen, "
                  "video mode change): take it again", s.name);
        release_slot(slot, false);
        return false;
    }
    // The snapshot saved or restored last only needs the pages written since; any other one is copied
    // back whole (the arena's write watch and frozen blocks follow a single snapshot).
    bool full = slot != g_synced;
    if (slot != g_synced && g_opts.zombie_policy == ZOMBIES_KEEP) {
        _snprintf(err, errlen, "with -snap_zombies_keep only the last snapshot saved or restored can be restored");
        return false;
    }
    bool other_level = s.level_gen != g_level_gen;   // the snapshot outlived a level change
    if (other_level) util::Log("[snap] restoring '%s' (map '%s') from '%s'", s.name, s.map, g_map);
    double t0 = util::Now();
    if (!dirty_ensure((s.image_end - arena::Base()) / 4096 + 16)) { _snprintf(err, errlen, "out of memory (dirty page list)"); return false; }

    stop_all_sounds();
    soundguard::BeforeRestore(slot);
    steamguard::BeforeRestore(slot);
    videoguard::BeforeRestore(slot);
    quiesce_begin();
    for (int i = 0; i < g_preserve_count; i++) memcpy(g_preserve[i].saved, (void*)g_preserve[i].addr, g_preserve[i].len);

    double t1 = util::Now();
    Phases ph;
    util::MuteConsole(true);
    int suspended = g_opts.suspend_threads ? util::SuspendOtherThreads(copypool::ThreadIds(), copypool::Workers()) : 0;
    ph.mark("suspend");
    arena::Lock();
    char fail[256] = "";
    size_t kept = 0, zbytes = 0, pages = 0, va_skipped = 0, va_recommitted = 0, mods_skipped = 0;
    bool checksum_ok = true, ok = true;
    const arena::Range* z = nullptr;
    if (!full) {
        // 1) arena: the pages written since the snapshot, skipping the zombies kept (KEEP policy)
        size_t nz = 0;
        z = arena::CollectZombies(&nz);
        ph.mark("zombies");
        if (nz == (size_t)-1) {
            strcpy(fail, "heap walk failed (corruption or out of memory) - nothing restored");
            ok = false;
        } else {
            kept = g_opts.zombie_policy == ZOMBIES_KEEP ? nz : 0;
            for (size_t i = 0; i < kept; i++) zbytes += z[i].end - z[i].start;
            bool overflow = false;
            size_t n = arena::DirtyPages(arena::Base(), s.image_end, g_dirty, g_dirty_cap, &overflow);
            ph.mark("dirty list");
            if (overflow || g_opts.full_revert) {
                ok = restore_range(s, arena::Base(), s.image_end, z, kept);
                pages = (s.image_end - arena::Base()) / 4096;
            } else {
                for (size_t i = 0; i < n && ok; i++) {
                    uintptr_t a = g_dirty[i], b = a + 4096;
                    if (a < arena::Base()) a = arena::Base();
                    if (b > s.image_end) b = s.image_end;
                    if (a < b) ok = restore_range(s, a, b, z, kept);
                }
                pages = n;
            }
            if (!ok) strcpy(fail, "could not read the arena image - the game state is now inconsistent");
            ph.mark("arena");
        }
    } else {
        // 1) arena: the whole image, in parallel; its free ranges become the arena's bookkeeping
        if (!arena::BeginFullRestore(s.free_s, s.nfree, s.top_s)) {
            strcpy(fail, "out of memory (arena bookkeeping) - nothing restored");
            ok = false;
        } else {
            ok = pieces_add((void*)arena::Base(), 0, s.image_end - arena::Base());
            ok = pieces_run(s.section, false) && ok;
            pages = (s.image_end - arena::Base()) / 4096;
            if (!ok) strcpy(fail, "could not read the arena image - the game state is now inconsistent");
        }
        ph.mark("arena (full)");
    }
    if (ok) {
        // 2) module sections and VirtualAlloc segments, 3) rebuild the arena free lists
        bool listed = true;
        for (size_t i = 0; i < s.nsegs; i++) {
            const Seg& g = s.segs[i];
            bool writable = i < s.nmods ? module_writable(g) : va_writable_again(g, &va_recommitted);
            if (writable) listed = pieces_add((void*)g.addr, g.off, g.len) && listed;
            else if (i < s.nmods) mods_skipped++;
            else va_skipped++;
        }
        bool mapped = g_seg_view_slot == slot;
        if (!pieces_run(s.section, false, mapped ? g_seg_view : nullptr, g_seg_view_off) || !listed)
            strcpy(fail, "could not read the module / VirtualAlloc copies - the game state is now inconsistent");
        ph.mark("modules+VA");
        if (!fail[0] && !arena::FinishRestore(kept, fail, sizeof(fail))) {
            char why[200];
            strncpy(why, fail, sizeof(why) - 1);
            why[sizeof(why) - 1] = 0;
            _snprintf(fail, sizeof(fail), "arena rebuild failed: %s", why);
        } else if (!fail[0] && g_opts.verify_heap && g_opts.zombie_policy == ZOMBIES_FREE) {
            checksum_ok = arena::ChecksumAllocated() == s.checksum;
        }
        ph.mark("rebuild");
    }
    arena::Unlock();
    view_unmap();
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
    if (va_skipped || mods_skipped)
        util::ConWarn("[snap] warning: %u VirtualAlloc segments and %u module sections could not be reverted (released since the save)",
                      (unsigned)va_skipped, (unsigned)mods_skipped);

    soundguard::AfterRestore(slot);
    d3dguard::AfterRestore(slot);
    fileguard::AfterRestore(slot);
    steamguard::AfterRestore(slot);
    videoguard::AfterRestore(slot);
    if (other_level) {   // we are back in the snapshot's map instance
        g_level_gen = s.level_gen;
        strcpy(g_map, s.map);
        g_level_eligible = true;
    }
    g_synced = slot;
    seg_view_map(slot);
    if (g_opts.clear_input && g_client) vfunc<VoidFn>(g_client, clientslot::IN_ClearStates)(g_client);
    s.restores++;
    g_last_restore_ms = (t2 - t0) * 1000.0;
    util::Con("[snap] restored '%s'%s in %.1f ms (prepare %.1f ms): %u pages%s, %u zombies kept (%.1f KB), %d threads",
              s.name, other_level ? " (back to its map)" : "", (t2 - t1) * 1000.0, (t1 - t0) * 1000.0, (unsigned)pages,
              full ? " (whole image)" : "", (unsigned)kept, zbytes / 1024.0, suspended);
    if (g_opts.verify_heap) {
        char e[256];
        if (!arena::Check(e, sizeof(e))) util::ConWarn("[snap] heap check FAILED after restore: %s", e);
        if (g_opts.zombie_policy == ZOMBIES_FREE) {
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
    PrintList();
}

void PrintList() {
    const char* names[MAX_SNAPSHOTS];
    int n = Names(names, MAX_SNAPSHOTS);
    size_t total = 0;
    for (int k = 0; k < n; k++) {
        const Snap& s = g_snaps[find_snap(names[k])];
        total += s.section_size;
        util::Con("[snap] '%s'%s: map '%s'%s, %.0f s ago, image %.1f MB + modules %.1f MB + VA %.1f MB, restored %d times",
                  s.name, &s - g_snaps == g_synced ? " (current)" : "", s.map, s.level_gen != g_level_gen ? " (another map)" : "",
                  util::Now() - s.time, (s.image_end - arena::Base()) / 1048576.0, s.mod_bytes / 1048576.0,
                  s.va_bytes / 1048576.0, s.restores);
    }
    if (n) util::Con("[snap] %d snapshot(s), %.0f MB of pagefile-backed memory", n, total / 1048576.0);
    else util::Con("[snap] no snapshot");
}

} // namespace snapshot
