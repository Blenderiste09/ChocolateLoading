#define _CRT_SECURE_NO_WARNINGS
#include "control.h"
#include "arena.h"
#include "gameapi.h"
#include "memhook.h"
#include "sarlink.h"
#include "snapshot.h"
#include "util.h"
#include <string.h>
#include <stdlib.h>

namespace control {

namespace {

// ---- operation queue --------------------------------------------------------------------------
Op       g_pending = OP_NONE;
char     g_pending_name[snapshot::MAX_NAME] = "";
uint32_t g_frame = 0;
uint32_t g_ready_frame = 0;        // frame at which the pending operation may run
double   g_request_time = 0;
int      g_levelinits = 0;
char     g_domain[1024] = "";

// ---- stress test (snap_test / autotest) -------------------------------------------------------
int      g_test_cycles = 0;
int      g_test_interval = 0;
uint32_t g_test_next = 0;
char     g_test_name[snapshot::MAX_NAME] = "";

// ---- engine threading ---------------------------------------------------------------------------
// A snapshot must be taken/restored with the game effectively single-threaded: a render-queue,
// threaded-host, async-mixer or queued-packet thread caught mid-update to one of its lock-free
// structures would make the image inconsistent. These cvars are forced to 0 when an operation is
// requested (the engine notices on the next frames and stops queueing work to those threads) and
// restored to the user's values once the operation, or the stress test, is over. The value fields are
// written directly: no change callback runs, the engine reads them every frame.
const char* const kThreadCvars[] = { "mat_queue_mode", "host_thread_mode", "snd_mix_async", "net_queued_packet_thread" };
const int   kThreadCvarCount = 4;
bool  g_forced = false;
int   g_saved_int[kThreadCvarCount];
float g_saved_float[kThreadCvarCount];

void force_threading_off(bool on) {
    if (on == g_forced) return;
    for (int i = 0; i < kThreadCvarCount; i++) {
        ConVar* v = gameapi::FindVar(kThreadCvars[i]);
        if (!v) continue;
        if (on) {
            g_saved_int[i] = v->m_nValue;
            g_saved_float[i] = v->m_fValue;
            v->m_nValue = 0;
            v->m_fValue = 0.0f;
        } else {
            v->m_nValue = g_saved_int[i];
            v->m_fValue = g_saved_float[i];
        }
    }
    g_forced = on;
}

void set_name(char* dst, const char* name) {
    strncpy(dst, name, snapshot::MAX_NAME - 1);
    dst[snapshot::MAX_NAME - 1] = 0;
}

void run_pending() {
    char err[256] = "";
    Op op = g_pending;
    g_pending = OP_NONE;
    const char* name = g_pending_name;
    bool ok;
    if (op == OP_SAVE) {
        sarlink::RecordDemoCommand("snap_save", name);   // before the capture: the snapshot's demo has it
        ok = snapshot::Save(name, err, sizeof(err));
    } else {
        ok = snapshot::Restore(name, err, sizeof(err));
        if (ok) sarlink::RecordDemoCommand("snap_load", name);   // the demo is back at the save now
    }
    sarlink::UpdateHud();
    if (!ok) util::ConWarn("[snap] %s of '%s' failed: %s", op == OP_SAVE ? "save" : "restore", name, err);
    else if (op == OP_LOAD) util::Con("[snap] snap_load: %.0f ms from the command to the restored state", (util::Now() - g_request_time) * 1000.0);
}

// ---- console commands ---------------------------------------------------------------------------
const char* name_arg(const CCommand& a) { return a.ArgC() >= 2 ? a.Arg(1) : snapshot::DEFAULT_NAME; }

void cmd_save(const CCommand& a) { Request(OP_SAVE, name_arg(a)); }
void cmd_load(const CCommand& a) { Request(OP_LOAD, name_arg(a)); }
void cmd_list(const CCommand&) { snapshot::PrintList(); }
void cmd_info(const CCommand&) { snapshot::PrintInfo(); }

void cmd_drop(const CCommand& a) {
    const char* name = name_arg(a);
    if (!strcmp(name, "*")) {
        int n = snapshot::Count();
        snapshot::DropAll();
        util::Con("[snap] %d snapshot(s) dropped", n);
    } else if (snapshot::Drop(name)) {
        util::Con("[snap] snapshot '%s' dropped", name);
    } else {
        util::ConWarn("[snap] no snapshot named '%s'", name);
    }
    sarlink::UpdateHud();
}

void cmd_check(const CCommand&) {
    char err[256];
    double t = util::Now();
    bool ok = arena::Check(err, sizeof(err));
    util::Con("[snap] heap check: %s (%.1f ms)", ok ? "OK" : err, (util::Now() - t) * 1000.0);
}

void cmd_opt(const CCommand& a) {
    snapshot::Options& o = snapshot::Opts();
    if (a.ArgC() >= 3) {
        bool v = atoi(a.Arg(2)) != 0;
        const char* n = a.Arg(1);
        if (!_stricmp(n, "suspend_threads")) o.suspend_threads = v;
        else if (!_stricmp(n, "zombies")) o.zombie_policy = v ? snapshot::ZOMBIES_KEEP : snapshot::ZOMBIES_FREE;
        else if (!_stricmp(n, "clear_input")) o.clear_input = v;
        else if (!_stricmp(n, "verify")) o.verify_heap = v;
        else util::ConWarn("[snap] unknown option '%s'", n);
    }
    util::Con("[snap] options: suspend_threads %d | zombies %d (0 free, 1 keep) | clear_input %d | verify %d",
              (int)o.suspend_threads, o.zombie_policy, (int)o.clear_input, (int)o.verify_heap);
}

void cmd_domain(const CCommand& a) {
    if (a.ArgC() >= 2) {
        SetDomain(a.ArgS());
        snapshot::DropAll();
        sarlink::UpdateHud();
        util::Con("[snap] domain changed (takes full effect after the next map load)");
    }
    util::Con("[snap] domain: %s", memhook::DomainDescription());
}

void cmd_test(const CCommand& a) {
    if (!snapshot::Current()[0]) { util::ConWarn("[snap] test: save or restore a snapshot first"); return; }
    StartTestCycles(a.ArgC() >= 2 ? atoi(a.Arg(1)) : 10, a.ArgC() >= 3 ? atoi(a.Arg(2)) : 60);
}

} // namespace

void SetDomain(const char* list) {
    strncpy(g_domain, list, sizeof(g_domain) - 1);
    g_domain[sizeof(g_domain) - 1] = 0;
    memhook::SetDomain(g_domain);
    snapshot::SetDomain(g_domain);
}

void RegisterCommands() {
    gameapi::AddCommand("snap_save", "snap_save [name]: save a RAM snapshot of the game state (default name: quick)", cmd_save);
    gameapi::AddCommand("snap_load", "snap_load [name]: restore a RAM snapshot (default name: quick)", cmd_load);
    gameapi::AddCommand("snap_drop", "snap_drop [name|*]: discard a snapshot (* = all) and release its memory", cmd_drop);
    gameapi::AddCommand("snap_list", "List the RAM snapshots", cmd_list);
    gameapi::AddCommand("snap_info", "Snapshot / arena statistics", cmd_info);
    gameapi::AddCommand("snap_check", "Check the arena heap consistency", cmd_check);
    gameapi::AddCommand("snap_opt", "snap_opt <suspend_threads|zombies|clear_input|verify> <0|1>", cmd_opt);
    gameapi::AddCommand("snap_domain", "Show or set the list of modules whose memory is snapshotted", cmd_domain);
    gameapi::AddCommand("snap_test", "snap_test [cycles] [frames]: restore the current snapshot repeatedly", cmd_test);
}

bool Request(Op op, const char* name) {
    if (!gameapi::InGame()) { util::ConWarn("[snap] not in a map (menu or loading screen): nothing done"); return false; }
    if (!snapshot::ValidName(name)) {
        util::ConWarn("[snap] invalid snapshot name '%s': up to %d letters, digits, '_', '-' or '.'", name, snapshot::MAX_NAME - 1);
        return false;
    }
    if (op == OP_LOAD && !snapshot::Exists(name)) { util::ConWarn("[snap] no snapshot named '%s' (snap_list shows them)", name); return false; }
    if (op == OP_SAVE && !snapshot::LevelEligible()) {
        util::ConWarn("[snap] this map was loaded before the plugin: load the map again first");
        return false;
    }
    if (op == OP_SAVE && !snapshot::Exists(name) && snapshot::Count() >= snapshot::MAX_SNAPSHOTS) {
        util::ConWarn("[snap] %d snapshots already exist: drop one first (snap_drop <name>)", snapshot::MAX_SNAPSHOTS);
        return false;
    }
    bool was_forced = g_forced;
    force_threading_off(true);
    g_pending = op;
    set_name(g_pending_name, name);
    g_request_time = util::Now();
    g_ready_frame = g_frame + (was_forced ? 1 : 4);   // let the worker threads drain first
    return true;
}

bool Busy() { return g_pending != OP_NONE; }
uint32_t Frame() { return g_frame; }

void OnFrameStart() {
    g_frame++;
    if (g_test_cycles > 0 && !snapshot::Exists(g_test_name)) {
        util::ConWarn("[snap] test: snapshot '%s' dropped, test stopped", g_test_name);
        g_test_cycles = 0;
    }
    if (g_test_cycles > 0 && g_pending == OP_NONE && g_frame >= g_test_next) {
        g_test_cycles--;
        util::Con("[snap] test: restore (%d left)", g_test_cycles);
        g_pending = OP_LOAD;   // the cvars are already forced off for the whole test
        set_name(g_pending_name, g_test_name);
        g_ready_frame = g_frame;
        g_request_time = util::Now();
        g_test_next = g_frame + g_test_interval;
    }
    if (g_pending != OP_NONE && g_frame >= g_ready_frame) run_pending();
    if (g_test_cycles == 0 && g_pending == OP_NONE && g_forced) force_threading_off(false);
    sarlink::OnFrame();
}

void StartTestCycles(int cycles, int interval_frames) {
    g_test_cycles = cycles < 1 ? 1 : cycles;
    g_test_interval = interval_frames < 2 ? 2 : interval_frames;
    set_name(g_test_name, snapshot::Current());
    force_threading_off(true);
    g_test_next = g_frame + 2;
    util::Con("[snap] test: %d restores of '%s', one every %d frames", g_test_cycles, g_test_name, g_test_interval);
}

bool TestCyclesRunning() { return g_test_cycles > 0; }

void OnLevelInit(const char* map) {
    g_levelinits++;
    memhook::SetDomain(g_domain);   // plugin_load mode: modules such as server.dll load with the map
    snapshot::OnLevelInit(map);
}

void OnLevelShutdown() {
    g_pending = OP_NONE;
    g_test_cycles = 0;
    snapshot::OnLevelShutdown();
    force_threading_off(false);
    sarlink::UpdateHud();
}

int LevelInits() { return g_levelinits; }

} // namespace control
