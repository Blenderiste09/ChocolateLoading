// Unattended in-game self-test, enabled with the -snap_autotest launch option.
//
// Once a map is loaded and has run for -snap_autotest_settle frames:
//   * default: snap_save, then -snap_autotest_cycles restores (one every 30 frames);
//   * -snap_autotest_probe: a scripted functional check first (getpos, save, move with noclip and fire
//     portals, restore, getpos + jpeg screenshots) so that console.log shows whether the player is back
//     exactly where it was saved; it also starts a thread that enters launcher.dll's CRT right after the
//     save and checks that the restore kept that thread's CRT data (see tlsguard.h);
//   * -snap_autotest_mapchange <spec>: then leave the map with the snapshot alive, settle, restore and
//     check the position again (P6 must equal P0). <spec> is a map name (changelevel), map:<name> (the
//     map command: full server restart), reload (save + load: a new instance of the same map, as after
//     a death), transition (the player is teleported into the exit elevator, whose trigger leaves the
//     map as in normal play) or exec:<cfg> (any console script); -snap_autotest_hops N leaves N maps in
//     a row before restoring; -snap_autotest_rounds N repeats it all N times and logs the process memory
//     after each round;
//   * -snap_autotest_bench: afterwards, a regular "save" + "load" of the same spot, timed until the
//     player is back in game, for comparison with snap_load;
//   * -snap_autotest_quit: finally "quit", to exercise the game's exit path with the snapshot alive.
// Progress and the final "AUTOTEST COMPLETE" / "AUTOTEST FAILED" lines go to the console and the log.
#pragma once

namespace autotest {

void ParseCommandLine();   // enables the test when -snap_autotest is present
void Tick();               // call once per FRAME_START, after control::OnFrameStart()

} // namespace autotest
