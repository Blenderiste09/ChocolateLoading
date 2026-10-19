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

void Request(Op op);          // refused (with a console message) outside a map or during loading
bool Busy();                  // an operation is queued
void OnFrameStart();          // drive the queue; call once per FRAME_START
uint32_t Frame();             // FRAME_STARTs seen so far

// Stress test: restore the snapshot `cycles` times, one restore every `interval_frames` frames.
void StartTestCycles(int cycles, int interval_frames);
bool TestCyclesRunning();

// Level callbacks (from the server plugin interface).
void OnLevelInit(const char* map);
void OnLevelShutdown();
int  LevelInits();            // number of LevelInit callbacks seen

} // namespace control
