// Keeps MP3 sounds snapshot-safe without cutting the audio.
//
// The engine mixes audio itself (DirectSound) but decodes MP3 data (voice lines, music) through
// vaudio_miles.dll: every decoder is a CMilesMP3 object, allocated in the arena (so part of a snapshot),
// that wraps a Miles "ASI stream" whose state lives in Miles' own heap (NOT part of a snapshot). When a
// decoder that existed at save time is destroyed later, Miles frees its stream; a restore then brings
// back the sound channel and its CMilesMP3, which now points to freed Miles memory (crash in mssmp3.asi).
//
// So while a snapshot refers to a decoder, its destruction is deferred (pinset.h): we hook
// CVAudio::DestroyMP3StreamDecoder and really destroy a decoder only once no snapshot refers to it and
// the current timeline no longer has it. `slot` is the snapshot's slot (snapshot.cpp).
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace soundguard {

// Patches CVAudio's CreateMP3StreamDecoder / DestroyMP3StreamDecoder (vaudio_miles.dll, "VAudio002").
// Call it as soon as vaudio_miles.dll is mapped (early loader), before any decoder exists.
bool Hook(uintptr_t vaudio_base, size_t vaudio_size);
bool Hooked();

void OnSave(int slot);          // the snapshot just taken refers to every live decoder
void BeforeRestore(int slot);   // destroy the decoders the restored timeline does not have (threads running)
void AfterRestore(int slot);    // the slot's decoders are alive again
void OnDrop(int slot);          // destroy the decoders nothing refers to any more

} // namespace soundguard
