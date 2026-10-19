// Keeps MP3 sounds snapshot-safe without cutting the audio.
//
// The engine mixes audio itself (DirectSound) but decodes MP3 data (voice lines, music) through
// vaudio_miles.dll: every decoder is a CMilesMP3 object, allocated in the arena (so part of a snapshot),
// that wraps a Miles "ASI stream" whose state lives in Miles' own heap (NOT part of a snapshot). When a
// decoder that existed at save time is destroyed later, Miles frees its stream; a restore then brings
// back the sound channel and its CMilesMP3, which now points to freed Miles memory (crash in mssmp3.asi).
//
// So while a snapshot exists, the decoders it references ("pinned") are not really destroyed: we hook
// CVAudio::DestroyMP3StreamDecoder and defer the destruction until the snapshot is dropped or replaced.
// A restore brings those decoders back to life (their owners are restored); decoders created after the
// snapshot are destroyed for real before a restore, since their owners are being discarded.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace soundguard {

// Patches CVAudio's CreateMP3StreamDecoder / DestroyMP3StreamDecoder (vaudio_miles.dll, "VAudio002").
// Call it as soon as vaudio_miles.dll is mapped (early loader), before any decoder exists.
bool Hook(uintptr_t vaudio_base, size_t vaudio_size);
bool Hooked();

void OnSave();          // the snapshot just taken references every live decoder: pin them
void BeforeRestore();   // destroy the decoders created after the snapshot (call with threads running)
void AfterRestore();    // the pinned decoders are alive again
void OnDrop();          // the snapshot is gone: really destroy the pinned decoders that were destroyed since

} // namespace soundguard
