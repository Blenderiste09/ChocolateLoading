// RAM snapshot orchestration. A snapshot is:
//   * the arena (the engine heap),
//   * the writable sections (.data/.bss) of the domain modules,
//   * the committed pages of the engine's direct VirtualAlloc reservations tracked by vatrack (hunk),
// all copied into one pagefile-backed section per snapshot. Save()/Restore() quiesce the engine's worker
// pool and async filesystem, suspend every other thread, copy the memory, and resume. Both must run on
// the main thread at a frame boundary (FRAME_START) with the engine's threading cvars forced off (see
// control.cpp).
//
// Snapshots have names (up to MAX_SNAPSHOTS at once, "quick" by default) and each one sits in a slot
// whose number the guards use as a bit. Restoring the snapshot that was saved or restored last only
// copies back the pages written since (the arena's write watch); restoring another one copies its whole
// image back.
//
// What the game state references outside that memory is kept consistent by the guards: MP3 decoders
// (soundguard), Direct3D objects (d3dguard), open files (fileguard), steam_api callback registrations
// (steamguard) and Bink videos (videoguard). Each one pins at save time what a snapshot refers to,
// undoes what the discarded timeline created at restore time, and lets go when no snapshot needs it any
// more. That is also what lets a snapshot outlive a level change: restoring it from another instance of
// the same map (after a death or a load) brings the game back to that instance, and with
// Options::cross_map, restoring it from another map brings the game back to the snapshot's map. Without
// cross_map (the default) the snapshots of a map are dropped when another map is loaded, which releases
// the old map's memory, GPU objects and files at once.
#pragma once
#include "sdk.h"
#include "slotmask.h"
#include <stddef.h>

namespace snapshot {

const int MAX_SNAPSHOTS = MAX_SNAPSHOT_SLOTS;   // 64: one bit each in the guards' masks
const int MAX_NAME = 32;                 // characters, terminator included
extern const char* const DEFAULT_NAME;   // "quick"

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
    bool cross_map       = false;         // keep the snapshots when another map is loaded (else only reloads of the same map)
};

// Resolves the engine interfaces used for quiescing. `engine_factory` may be null (early loader).
bool Init(CreateInterfaceFn engine_factory, char* err, size_t errlen);
void SetDomain(const char* module_list);    // modules whose writable sections are captured
Options& Opts();

// Names: letters, digits, '_', '-' and '.', up to MAX_NAME - 1 characters; case-insensitive.
bool ValidName(const char* name);
bool Save(const char* name, char* err, size_t errlen);      // replaces a snapshot of the same name
bool Restore(const char* name, char* err, size_t errlen);
bool Drop(const char* name);                // false if there is no such snapshot
void DropAll();
bool Exists(const char* name);
int  Count();
// The names of the existing snapshots, oldest first (pointers valid until the next save/drop).
int  Names(const char** out, int max);
// The snapshot saved or restored last ("" when it was dropped).
const char* Current();

void OnLevelInit(const char* map);
void OnLevelShutdown();                     // keeps the snapshots if the guards can, else drops them
// Without Options::cross_map: drops the snapshots taken on another map than the current one. Returns how
// many were dropped. OnLevelInit calls it, and so should a change of the option.
int  DropOtherMaps();
bool LevelEligible();                       // the current map was loaded after the allocator hook
void ForceEligible(bool v);                 // early loader: the hook was active from process start
double LastRestoreMs();                     // duration of the last successful restore

void PrintInfo();                           // arena statistics, then PrintList()
void PrintList();                           // one line per snapshot

} // namespace snapshot
