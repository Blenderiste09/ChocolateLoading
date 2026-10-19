// Runs snapshot operations at a safe point and owns the snap_* console commands.
//
// snap_save / snap_load never act immediately: Request() forces the engine's threading cvars off, the
// worker threads get a few frames to drain, then the operation runs on the main thread at FRAME_START
// (OnFrameStart, called from the client's FrameStageNotify hook).
#pragma once
#include <stdint.h>

namespace control {

enum Op { OP_NONE, OP_SAVE, OP_LOAD };

// Modules whose memory is routed (plugin_load mode) and captured; also changed by snap_domain.
void SetDomain(const char* module_list);
void RegisterCommands();

// Queues a save or restore of the snapshot `name`. Refused (false, with a console message) outside a
// map, during loading, for an invalid name, a restore of a snapshot that does not exist, or a new
// snapshot when there are already snapshot::MAX_SNAPSHOTS.
bool Request(Op op, const char* name);
bool Busy();                  // an operation is queued
void OnFrameStart();          // drive the queue; call once per FRAME_START
uint32_t Frame();             // FRAME_STARTs seen so far

// Stress test: restore the current snapshot (the one saved or restored last) `cycles` times, one
// restore every `interval_frames` frames.
void StartTestCycles(int cycles, int interval_frames);
bool TestCyclesRunning();

// Level callbacks (from the server plugin interface).
void OnLevelInit(const char* map);
void OnLevelShutdown();
int  LevelInits();            // number of LevelInit callbacks seen

} // namespace control
