// Keeps the file handles a snapshot references open, so that it survives a level change.
//
// The engine's file objects live in the arena (filesystem_stdio: pack files, the map's BSP pak lump, VPK
// chunks...), but the Win32 handles they hold are kernel state. A level change closes the old map's BSP
// and opens the new one, which usually gets the SAME handle value: after a restore the old map's pack
// file would read the new map's BSP through a handle that now names another file.
//
// So every file the game opens itself (callsite.h: NtCreateFile/NtOpenFile called by a game module) is
// tracked, and while a snapshot exists:
//  * closing a handle that was open at save time is deferred: the game sees a successful close, the
//    handle stays open (and its value is not reused) because the snapshot still refers to it;
//  * a restore makes those handles the restored game's again, and closes the game files opened after
//    the save (nothing in the restored state knows them);
//  * dropping the snapshot really closes the handles whose close was deferred.
#pragma once

namespace fileguard {

void Install();         // hooks ntdll!NtCreateFile/NtOpenFile/NtClose; early loader, before the engine starts
bool Installed();

void OnSave();          // pin the game's open files
void AfterRestore();    // un-defer the pinned handles, close the files opened since the save
void OnDrop();          // close the handles whose close was deferred
bool Intact();          // installed and every game file handle could be tracked

} // namespace fileguard
