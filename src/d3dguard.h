// Keeps alive the Direct3D 9 objects a snapshot references, so that it survives a level change.
//
// Every texture, vertex/index buffer, surface, shader, vertex declaration and query the engine uses is a
// COM object of the D3D9 runtime (d3d9.dll, or dxvk_d3d9.dll with -vulkan), which lives outside the
// arena. The engine's tables of those objects (shaderapidx9's textures, meshes, shaders...) are in the
// arena, so a restore brings back pointers to the objects that existed at save time. Within one map they
// are still alive; after a level change the engine has released most of them (the old map's textures,
// lightmaps, world and model buffers) and the restored pointers would dangle: "failed to lock vertex
// buffer in CMeshDX8::LockVertexBuffer", then an error box.
//
// So, as soundguard does for MP3 decoders:
//  * every object created by the engine's shader API is tracked: Direct3DCreate9 -> CreateDevice ->
//    the device's Create* methods, and the Release of each object class to see the objects die;
//  * a save "pins" every live object: one extra reference per snapshot, and the engine's own count is
//    recorded for that snapshot;
//  * a restore sets each object's reference count to what the restored engine holds (its count at save
//    time, or none for an object the restored timeline does not have) plus one per snapshot pinning it;
//    an object nothing holds any more is released for good;
//  * dropping a snapshot releases its pins.
// A device reset (fullscreen alt-tab, video mode change) needs every default-pool resource released:
// all pins are dropped before it and those snapshots can no longer be restored (Intact() turns false).
#pragma once
#include <stdint.h>

namespace d3dguard {

// Hooks Direct3DCreate9 of a D3D9 runtime (d3d9.dll or dxvk_d3d9.dll) mapped at `runtime_base`. Call it
// as soon as the runtime is mapped (early loader), before the shader API creates its device.
bool HookRuntime(uintptr_t runtime_base, const char* name);
bool Active();          // the engine's device was created through the hooks: its objects are tracked
bool Healthy();         // active and the registry never overflowed

void OnSave(int slot);          // pin every live object for the snapshot in `slot` (other threads running)
void AfterRestore(int slot);    // set the reference counts back for the restored snapshot
void OnDrop(int slot);          // the snapshot is gone: release its pins
// True when every object the snapshot in `slot` refers to is guaranteed to be alive: the guard is
// healthy and no device reset dropped its pins.
bool Intact(int slot);

} // namespace d3dguard
