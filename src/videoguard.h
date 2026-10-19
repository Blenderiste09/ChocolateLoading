// Keeps Bink videos (elevator screens, vgui_movie_display panels) snapshot-safe.
//
// valve_avi.dll plays .bik files through binkw32.dll. The HBINK of each video lives in the arena
// (valve_avi routes Bink's memory to g_pMemAlloc), but Bink's I/O thread, its file handle and its own
// globals are outside the snapshot. A video open at save time and closed later - a level change closes
// all of the old map's videos - would come back with a closed file and no I/O thread; a video opened
// after the save would stay open in binkw32 while the restore frees its HBINK.
//
// So, as soundguard does for MP3 decoders (pinset.h): BinkClose of a video a snapshot refers to is
// deferred; the videos the restored timeline does not have are closed for real before a restore; a video
// is closed once no snapshot refers to it and the current timeline no longer has it.
#pragma once
#include <stdint.h>

namespace videoguard {

// Hooks BinkOpen/BinkClose of binkw32.dll mapped at `binkw32_base` (early loader: when it is mapped).
bool Hook(uintptr_t binkw32_base);
bool Hooked();

void OnSave(int slot);
void BeforeRestore(int slot);   // close the videos the restored timeline does not have (threads running)
void AfterRestore(int slot);
void OnDrop(int slot);

} // namespace videoguard
