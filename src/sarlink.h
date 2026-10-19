// What a SAR user sees of the snapshots: records in the demo being recorded, and a line of SAR's HUD.
//
// Demos. SAR writes its own data into demos (for run verification) as custom data messages: id 0, the
// one callback the game registers (the co-op radial menu, whose 8 bytes of cursor position come first),
// then a SAR record whose first byte is its type. The commands run before a demo starts are written
// that way, as "queued command" records (type 0x10, the command text). Each snap_save and snap_load is
// recorded the same way, so SAR, mdp and the other demo tools list it as a command at its tick (SAR's
// demo playback does not execute those records). A save is recorded just before the memory is captured,
// so the snapshot's own copy of the demo has it; a load right after the restore, which took the demo
// back to the save (fileguard): it follows the save record, at the save's tick.
//
// HUD. When SAR is loaded, one of its HUD text lines (sar_hud_set_text) lists the snapshots that exist,
// the one saved or restored last in green, or says there is none.
#pragma once

namespace sarlink {

// Records "<cmd> <name>" in the demo being recorded, if any. Main thread, in a map.
void RecordDemoCommand(const char* cmd, const char* name);

void SetHudId(int id);   // SAR HUD text id of the line (0: no HUD line)
void UpdateHud();        // show the current snapshots now if the line changed
void OnFrame();          // periodic check: SAR may be loaded (or reloaded) at any time

} // namespace sarlink
