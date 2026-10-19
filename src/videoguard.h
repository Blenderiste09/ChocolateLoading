// Keeps Bink videos (elevator screens, vgui_movie_display panels) snapshot-safe.
//
// valve_avi.dll plays .bik files through binkw32.dll. The HBINK of each video lives in the arena
// (valve_avi routes Bink's memory to g_pMemAlloc), but Bink's I/O thread, its file handle and its own
// globals are outside the snapshot. A video open at save time and closed later - a level change closes
// all of the old map's videos - would come back with a closed file and no I/O thread; a video opened
// after the save would stay open in binkw32 while the restore frees its HBINK.
//
// So, as soundguard does for MP3 decoders: while a snapshot exists, BinkClose of a video that was open
// at save time is deferred (the restored game owns it again); the videos opened after the save are
// closed for real before a restore; dropping the snapshot closes the deferred ones.
#pragma once
#include <stdint.h>

namespace videoguard {

// Hooks BinkOpen/BinkClose of binkw32.dll mapped at `binkw32_base` (early loader: when it is mapped).
bool Hook(uintptr_t binkw32_base);
bool Hooked();

void OnSave();
void BeforeRestore();   // close the videos opened since the save (call with the other threads running)
void AfterRestore();
void OnDrop();

} // namespace videoguard
