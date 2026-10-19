// Keeps steam_api's callback registrations consistent with the snapshot.
//
// The game receives Steam notifications through CCallback / CCallResult objects (in the arena or in a
// module's .data) that it registers with steam_api.dll: SteamAPI_RegisterCallback(obj, id) for a
// callback type, SteamAPI_RegisterCallResult(obj, hAPICall) for the result of one asynchronous call.
// steam_api keeps those pointers in its own heap, outside the snapshot. A restore therefore leaves it
// with registrations the restored game knows nothing about: an object registered after the save is
// freed (or reverted) by the restore, and when its notification arrives steam_api calls into garbage -
// e.g. a call result issued after the save whose member-function pointer the restore set back to null
// ("execute at 00000000" under SteamGameServer_RunCallbacks after a level change).
//
// So the registrations made by the game itself (callsite.h) are tracked, and around a restore:
//  * before the memory copy, while those objects are still intact, the callbacks registered since the
//    save and the call results issued since the save are unregistered;
//  * after it, the callbacks that were registered at save time but unregistered since are registered
//    again (the restored objects are those registered ones).
#pragma once
#include <stdint.h>

namespace steamguard {

// Hooks the registration functions of steam_api.dll mapped at `steam_api_base`. Call it as soon as the
// DLL is mapped (early loader), before the game registers its first callback.
bool Hook(uintptr_t steam_api_base);
bool Hooked();          // hooked and the registry never overflowed

void OnSave();
void BeforeRestore();   // call before the memory is restored, with the other threads running
void AfterRestore();
void OnDrop();

} // namespace steamguard
