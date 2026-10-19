#define _CRT_SECURE_NO_WARNINGS
#include "autotest.h"
#include "arena.h"
#include "control.h"
#include "gameapi.h"
#include "snapshot.h"
#include "util.h"
#include <windows.h>
#include <psapi.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace autotest {

namespace {

enum State { DISABLED, SETTLING, SAVING, PROBING, CYCLING, BENCH_SAVE, BENCH_LOAD, BENCH_WAIT, DONE };

State    g_state = DISABLED;
int      g_cycles = 20;
int      g_settle = 600;          // FRAME_STARTs to wait once the map is eligible
bool     g_probe = false;
bool     g_bench = false;
char     g_level_spec[64] = "";   // -snap_autotest_mapchange <spec>: restore from another map instance
char     g_level_cmd[160] = "";   // the console command that changes the level (from the spec)
int      g_rounds = 1;            // -snap_autotest_rounds: level change + restore, that many times
int      g_round = 0;
int      g_hops = 1;              // -snap_autotest_hops: level changes in a row before each restore
int      g_hop = 0;
uint32_t g_next = 0;              // frame at which the current state may proceed
bool     g_settle_started = false;

// ---- probe script: each step runs `wait` frames after the previous one finished --------------------
enum StepOp { S_CMD, S_SAVE, S_RESTORE, S_CHANGELEVEL, S_MEMINFO };
struct Step { int wait; StepOp op; const char* cmd; };
const Step kProbe[] = {
    { 0,   S_CMD,     "echo [snap] probe: P0 (before save); getpos" },
    { 10,  S_SAVE,    nullptr },
    { 5,   S_CMD,     "noclip" },
    { 2,   S_CMD,     "+forward" },
    { 60,  S_CMD,     "-forward" },
    { 10,  S_CMD,     "echo [snap] probe: P1 (moved after save); getpos" },
    { 2,   S_CMD,     "jpeg" },
    { 20,  S_RESTORE, nullptr },
    { 30,  S_CMD,     "echo [snap] probe: P2 (after restore, must equal P0); getpos" },
    { 2,   S_CMD,     "jpeg" },
    // A busier session: wander around firing both portals for ~5 s, then restore again.
    { 10,  S_CMD,     "noclip; +left; +forward" },
    { 60,  S_CMD,     "+attack" },
    { 10,  S_CMD,     "-attack" },
    { 60,  S_CMD,     "+attack2" },
    { 10,  S_CMD,     "-attack2; -left; +right; +back" },
    { 120, S_CMD,     "+attack" },
    { 10,  S_CMD,     "-attack; -right; -back; -forward" },
    { 10,  S_CMD,     "echo [snap] probe: P3 (after a busy session); getpos" },
    { 2,   S_CMD,     "jpeg" },
    { 20,  S_RESTORE, nullptr },
    { 30,  S_CMD,     "echo [snap] probe: P4 (after the second restore, must equal P0); getpos" },
    { 2,   S_CMD,     "jpeg" },
};
const int kProbeSteps = (int)(sizeof(kProbe) / sizeof(kProbe[0]));
// -snap_autotest_mapchange: go to another map with the snapshot alive, then restore from there.
const Step kMapChange[] = {
    { 10,  S_CHANGELEVEL, nullptr },
    { 300, S_CMD,     "echo [snap] probe: P5 (in the other map, before restoring); getpos" },
    { 2,   S_CMD,     "jpeg" },
    { 10,  S_RESTORE, nullptr },
    { 30,  S_CMD,     "echo [snap] probe: P6 (restored from the other map, must equal P0); getpos" },
    { 2,   S_CMD,     "jpeg" },
    { 2,   S_MEMINFO, nullptr },
};
const int kMapChangeSteps = (int)(sizeof(kMapChange) / sizeof(kMapChange[0]));
const Step* g_script = nullptr;
int  g_script_len = 0;
int  g_step = 0;
bool g_step_busy = false;         // the current step's save/restore/level change has not completed yet
int  g_step_levelinits = 0;
double g_step_t0 = 0;

// ---- benchmark ----------------------------------------------------------------------------------
double g_bench_t0 = 0;
int    g_bench_levelinits = 0;

void finish(const char* verdict) {
    util::Con("[snap] AUTOTEST %s", verdict);
    g_state = DONE;
}

void start_cycles() {
    control::StartTestCycles(g_cycles, 30);
    g_state = CYCLING;
}

void run_script(const Step* script, int len) {
    g_script = script;
    g_script_len = len;
    g_step = 0;
    g_step_busy = false;
    g_next = control::Frame() + (len ? script[0].wait : 0);
    g_state = PROBING;
}

// The probe is done; the map-change script (if requested) runs next, then the restore cycles.
void script_done() {
    if (g_level_spec[0] && (g_script == kProbe || (g_script == kMapChange && ++g_round < g_rounds))) {
        run_script(kMapChange, kMapChangeSteps);
        return;
    }
    start_cycles();
}

void tick_probe() {
    if (g_step_busy) {
        const Step& s = g_script[g_step];
        if (s.op == S_CHANGELEVEL) {
            if (util::Now() - g_step_t0 > 180.0) { finish("FAILED: the level change timed out"); return; }
            if (control::LevelInits() == g_step_levelinits || !gameapi::InGame()) return;
            util::Con("[snap] probe: level changed (%s) in %.0f ms, snapshot %s", g_level_spec, (util::Now() - g_step_t0) * 1000.0,
                      snapshot::Valid() ? "still valid" : "GONE");
            if (snapshot::Valid() && ++g_hop < g_hops) {   // leave this map too, after it settled
                g_step_busy = false;
                g_next = control::Frame() + 300;
                return;
            }
            g_hop = 0;
        } else if (control::Busy()) {
            return;
        }
        g_step_busy = false;
        if (!snapshot::Valid()) { finish("FAILED: the snapshot did not survive a probe step"); return; }
        g_step++;
        if (g_step < g_script_len) g_next = control::Frame() + g_script[g_step].wait;
        return;
    }
    if (g_step >= g_script_len) { script_done(); return; }
    if (control::Frame() < g_next) return;
    const Step& s = g_script[g_step];
    switch (s.op) {
    case S_CMD:     gameapi::RunCommand(s.cmd); break;
    case S_SAVE:    util::Con("[snap] probe: saving"); control::Request(control::OP_SAVE); g_step_busy = true; return;
    case S_RESTORE: util::Con("[snap] probe: restoring"); control::Request(control::OP_LOAD); g_step_busy = true; return;
    case S_CHANGELEVEL:
        util::Con("[snap] probe: round %d/%d, level change %d/%d: %s (with the snapshot alive)", g_round + 1, g_rounds,
                  g_hop + 1, g_hops, g_level_cmd);
        g_step_levelinits = control::LevelInits();
        g_step_t0 = util::Now();
        gameapi::RunCommand(g_level_cmd);
        g_step_busy = true;
        return;
    case S_MEMINFO: {
        MEMORYSTATUSEX ms;
        ms.dwLength = sizeof(ms);
        PROCESS_MEMORY_COUNTERS_EX pm;
        pm.cb = sizeof(pm);
        if (GlobalMemoryStatusEx(&ms) && K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof(pm)))
            util::Con("[snap] probe: memory after round %d: address space used %.0f MB, private %.0f MB",
                      g_round + 1, (ms.ullTotalVirtual - ms.ullAvailVirtual) / 1048576.0, pm.PrivateUsage / 1048576.0);
        break;
    }
    }
    g_step++;
    if (g_step < g_script_len) g_next = control::Frame() + g_script[g_step].wait;
}

} // namespace

void ParseCommandLine() {
    if (!util::CommandLineHas("-snap_autotest")) return;
    g_cycles = util::CommandLineInt("-snap_autotest_cycles", g_cycles);
    g_settle = util::CommandLineInt("-snap_autotest_settle", g_settle);
    if (g_cycles < 1) g_cycles = 1;
    if (g_settle < 30) g_settle = 30;
    g_probe = util::CommandLineHas("-snap_autotest_probe");
    g_bench = util::CommandLineHas("-snap_autotest_bench");
    // <map>: changelevel <map>; map:<map>: map <map> (full server restart); reload: save a game now and
    // load it back (a new instance of the same map, as after a death); transition: teleport the player
    // into the exit elevator of a single player test chamber, whose own trigger then leaves the map;
    // exec:<cfg>: any console script.
    if (util::CommandLineStr("-snap_autotest_mapchange", g_level_spec, sizeof(g_level_spec))) {
        if (!strncmp(g_level_spec, "map:", 4)) _snprintf(g_level_cmd, sizeof(g_level_cmd), "map %s", g_level_spec + 4);
        else if (!strncmp(g_level_spec, "exec:", 5)) _snprintf(g_level_cmd, sizeof(g_level_cmd), "exec %s", g_level_spec + 5);
        else if (!strcmp(g_level_spec, "reload")) _snprintf(g_level_cmd, sizeof(g_level_cmd), "save chocoreload; load chocoreload");
        else if (!strcmp(g_level_spec, "transition")) _snprintf(g_level_cmd, sizeof(g_level_cmd), "ent_fire @exit_teleport Teleport");
        else _snprintf(g_level_cmd, sizeof(g_level_cmd), "changelevel %s", g_level_spec);
        g_level_cmd[sizeof(g_level_cmd) - 1] = 0;
    }
    g_rounds = util::CommandLineInt("-snap_autotest_rounds", 1);
    if (g_rounds < 1) g_rounds = 1;
    g_hops = util::CommandLineInt("-snap_autotest_hops", 1);
    if (g_hops < 1) g_hops = 1;
    g_state = SETTLING;
    const snapshot::Options& o = snapshot::Opts();
    util::Con("[snap] autotest: %d cycles, settle %d, probe %d, mapchange '%s' x%d, bench %d, verify %d, full_revert %d, zombies %s, va %d",
              g_cycles, g_settle, (int)g_probe, g_level_spec, g_rounds, (int)g_bench, (int)o.verify_heap, (int)o.full_revert,
              o.zombie_policy == snapshot::ZOMBIES_FREE ? "free" : "keep", (int)o.capture_va);
}

void Tick() {
    uint32_t frame = control::Frame();
    switch (g_state) {
    case SETTLING:
        if (!snapshot::LevelEligible() || !gameapi::InGame()) { g_settle_started = false; return; }
        if (!g_settle_started) {
            g_settle_started = true;
            g_next = frame + g_settle;
            util::Con("[snap] autotest: map ready, settling %d frames", g_settle);
            return;
        }
        if (frame < g_next) return;
        if (g_probe) {
            run_script(kProbe, kProbeSteps);
        } else {
            util::Con("[snap] autotest: saving snapshot");
            control::Request(control::OP_SAVE);
            g_state = SAVING;
        }
        break;
    case SAVING:
        if (control::Busy()) return;
        if (!snapshot::Valid()) { finish("FAILED: the save did not produce a snapshot"); return; }
        if (g_level_spec[0]) run_script(kMapChange, kMapChangeSteps);
        else start_cycles();
        break;
    case PROBING:
        tick_probe();
        break;
    case CYCLING: {
        if (control::TestCyclesRunning() || control::Busy()) return;
        snapshot::PrintInfo();
        char err[256];
        bool ok = arena::Check(err, sizeof(err));
        util::Con("[snap] autotest heap check: %s", ok ? "OK" : err);
        if (!ok) { finish("FAILED: heap check"); return; }
        if (!g_bench) { finish("COMPLETE"); return; }
        g_next = frame + 30;
        g_state = BENCH_SAVE;
        break;
    }
    case BENCH_SAVE:
        if (frame < g_next) return;
        util::Con("[snap] bench: writing a regular save game");
        gameapi::RunCommand("save chocobench");
        g_next = frame + 120;
        g_state = BENCH_LOAD;
        break;
    case BENCH_LOAD:
        if (frame < g_next) return;
        util::Con("[snap] bench: loading it back the normal way");
        g_bench_levelinits = control::LevelInits();
        g_bench_t0 = util::Now();
        gameapi::RunCommand("load chocobench");
        g_state = BENCH_WAIT;
        break;
    case BENCH_WAIT: {
        double dt = util::Now() - g_bench_t0;
        if (dt > 180.0) { util::ConWarn("[snap] bench: timed out"); finish("COMPLETE"); return; }
        if (control::LevelInits() == g_bench_levelinits || !gameapi::InGame()) return;
        util::Con("[snap] bench: normal 'load' took %.0f ms (command -> in game); snap_load restore took %.0f ms",
                  dt * 1000.0, snapshot::LastRestoreMs());
        finish("COMPLETE");
        break;
    }
    default:
        break;
    }
}

} // namespace autotest
