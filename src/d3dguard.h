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
//  * a save "pins" every live object: one extra reference, and the engine's own count is recorded;
//  * a restore sets each pinned object's reference count back to its value at save time (the restored
//    engine holds exactly those references) and fully releases the objects created after the save
//    (nothing in the restored state knows them);
//  * dropping the snapshot releases the pins: objects the engine no longer uses are destroyed then.
// A device reset (fullscreen alt-tab, video mode change) needs every default-pool resource released:
// the pins are dropped before it and the snapshot can no longer be restored (Intact() turns false).
#pragma once
#include <stdint.h>

namespace d3dguard {

// Hooks Direct3DCreate9 of a D3D9 runtime (d3d9.dll or dxvk_d3d9.dll) mapped at `runtime_base`. Call it
// as soon as the runtime is mapped (early loader), before the shader API creates its device.
bool HookRuntime(uintptr_t runtime_base, const char* name);
bool Active();          // the engine's device was created through the hooks: its objects are tracked

void OnSave();          // pin every tracked object (call with the other threads running)
void AfterRestore();    // restore the pinned counts, release the objects created after the save
void OnDrop();          // the snapshot is gone: release the pins
// True when every object the current snapshot references is guaranteed to be alive: the guard is
// active, no device reset dropped the pins and the registry never overflowed.
bool Intact();

} // namespace d3dguard
