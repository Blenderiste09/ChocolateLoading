// Keeps the file handles the snapshots refer to open, so that they survive a level change.
//
// The engine's file objects live in the arena (filesystem_stdio: pack files, the map's BSP pak lump, VPK
// chunks...), but the Win32 handles they hold are kernel state. A level change closes the old map's BSP
// and opens the new one, which usually gets the SAME handle value: after a restore the old map's pack
// file would read the new map's BSP through a handle that now names another file.
//
// So every file the game opens itself (callsite.h: NtCreateFile/NtOpenFile called by a game module) is
// tracked with the set of snapshots that refer to it (taken while it was open):
//  * the game closing a file a snapshot refers to is deferred: the game sees a successful close, the
//    handle stays open (and its value is not reused);
//  * a restore makes the restored snapshot's files the game's again - with the kernel file pointer and,
//    for a file opened for writing, the size they had at save time (the demo being recorded) - and closes
//    the other files the game had open, unless another snapshot still refers to them;
//  * a file is really closed once no snapshot refers to it and the current timeline no longer has it.
#pragma once

namespace fileguard {

void Install();         // hooks ntdll!NtCreateFile/NtOpenFile/NtClose; early loader, before the engine starts

void OnSave(int slot);          // the snapshot in `slot` refers to the game's open files
void AfterRestore(int slot);
void OnDrop(int slot);
bool Intact();          // installed and every game file handle could be tracked

} // namespace fileguard
