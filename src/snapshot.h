// RAM snapshot orchestration. A snapshot is:
//   * the arena (the engine heap), copied into a pagefile-backed section,
//   * the writable sections (.data/.bss) of the domain modules,
//   * the committed pages of the engine's direct VirtualAlloc reservations tracked by vatrack (hunk).
// Save()/Restore() quiesce the engine's worker pool and async filesystem, suspend every other thread,
// copy the memory, and resume. Both must run on the main thread at a frame boundary (FRAME_START) with
// the engine's threading cvars forced off (see control.cpp).
//
// What the game state references outside that memory is kept consistent by the guards: MP3 decoders
// (soundguard), Direct3D objects (d3dguard), open files (fileguard), steam_api callback registrations
// (steamguard) and Bink videos (videoguard). Each one pins at save time what the snapshot refers to,
// undoes what the discarded timeline created at restore time, and lets go when the snapshot is dropped.
// That is also what lets a snapshot outlive a level change: restoring it from another map (or another
// instance of the same map, after a death or a load) brings the game back to the snapshot's map.
#pragma once
#include "sdk.h"
#include <stddef.h>

namespace snapshot {

// What to do with blocks allocated after the snapshot that are still alive at restore time.
enum ZombiePolicy {
    ZOMBIES_FREE = 0,   // free them (correct when everything is captured: early loader)
    ZOMBIES_KEEP = 1,   // keep them allocated (plugin_load mode: uncaptured code may reference them)
};

struct Options {
    bool suspend_threads = true;          // suspend every other thread during the copy
    int  zombie_policy   = ZOMBIES_KEEP;
    bool clear_input     = true;          // release held +buttons after a restore
    bool verify_heap     = false;         // diagnostic: heap check + arena checksum around each op (slow)
    bool full_revert     = false;         // diagnostic: copy the whole arena back, not just dirty pages
    bool capture_va      = true;          // capture the engine's direct VirtualAlloc regions (the hunk)
    int  copy_threads    = -1;            // extra threads copying the arena image at save time (-1: auto)
};

// Resolves the engine interfaces used for quiescing. `engine_factory` may be null (early loader).
bool Init(CreateInterfaceFn engine_factory, char* err, size_t errlen);
void SetDomain(const char* module_list);    // modules whose writable sections are captured
Options& Opts();

bool Save(char* err, size_t errlen);       // a failed save leaves no snapshot
bool Restore(char* err, size_t errlen);
void Drop();                               // forget the snapshot and release its buffers
bool Valid();

void OnLevelInit(const char* map);
void OnLevelShutdown();                     // keeps the snapshot if the guards can, else drops it
bool LevelEligible();                       // the current map was loaded after the allocator hook
void ForceEligible(bool v);                 // early loader: the hook was active from process start
double LastRestoreMs();                     // duration of the last successful restore

void PrintInfo();

} // namespace snapshot
