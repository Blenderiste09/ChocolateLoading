# Portal 2 reverse-engineering notes

All addresses below are **analysis addresses** from the Ghidra projects in `re/ghidra_projects`
(Windows PE image base `0x10000000`; the Mac/Linux dylibs/so are based at `0`). They are
observations from decompilation, not original source. The Mac/Linux binaries carry **symbols**;
the Windows binaries do not (only a PDB *reference*, not the PDB itself), so names on Windows come
from RTTI, string xrefs and matching against the symbolised Mac build.

## Binaries

| Project (re/ghidra_projects) | Binary | Has symbols |
|---|---|---|
| `win_engine` / `win_tier0` / `win_vstdlib` / `win_matsys` / `win_fs` | current Windows DLLs (copied to `re/bin/win`) | no |
| `mac_engine` / `mac_tier0` / `mac_vstdlib` / `mac_matsys` / `mac_fs` / `mac_server` / `mac_client` | `E:\P2\MAC\...` dylibs | **yes** |

Build identity: `portal2/steam.inf` → `PatchVersion=2.0.0.1`, `appID=620`. Windows DLLs are 32-bit
PE, VS2010 linker (10.00), **static CRT** (`/MT`). `portal2.exe` is LARGEADDRESSAWARE; the DLLs are
not marked LAA themselves but run in the LAA host.

## tier0 memory allocator (`CStdMemAlloc`, the snapshot target)

`tier0.dll` exports `g_pMemAlloc` (an `IMemAlloc*`). Every engine/game allocation funnels through
its vtable. `IMemAlloc` vtable (MSVC overload order, verified on Windows `tier0.dll` by the
`ret imm` of each thunk, and against the Mac `CStdMemAlloc::vtable` symbols):

| slot | method | `ret` |
|---|---|---|
| 0 | `Alloc(size, file, line)` (dbg) | `ret 0xC` |
| 1 | `Alloc(size)` | `ret 4` |
| 2 | `Realloc(p, size, file, line)` | `ret 0x10` |
| 3 | `Realloc(p, size)` | `ret 8` |
| 4 | `Free(p, file, line)` | `ret 0xC` |
| 5 | `Free(p)` | `ret 4` |
| 6 | `Expand(...)` dbg (returns 0, "no longer supported") | |
| 7 | `Expand(p, size)` | |
| 8 | `GetSize(p)` | `ret 4` |
| 37 | `RegionAlloc(region, size, file, line)` | `ret 0x10` |
| 38 | `RegionAlloc(region, size)` | `ret 8` |
| 47 | vtable length | |

`memhook.cpp` verifies these `ret` immediates at install time (`check_slots`) and refuses to hook if
tier0's layout ever changes.

The real allocator is a small-block heap + fallback (Windows `tier0.dll`):
- `FUN_1000a270` / `FUN_1000a5a0` = `Alloc`/`AllocDbg`: blocks `<= 0x800` come from a small-block
  heap keyed by `(size-1 & ~3)`; above `0x40000` and on SBH exhaustion it falls back to
  `FUN_1001a020` (dlmalloc-like). The "DRASTIC MEMORY OVERFLOW: Fell out of small block heap!"
  string is in this path.
- `FUN_1000a3e0` / `FUN_1000a710` = `Free`/`FreeDbg`: pointer-range test decides SBH vs fallback.
- `FUN_1000a4d0` / `FUN_1000a800` = `GetSize`.

We do **not** reuse tier0's heap. Instead `memhook` replaces the vtable thunks and routes
allocations whose **return address** is inside a "domain" module into our own `arena`
(`src/arena.cpp`). Non-domain allocations and frees of non-arena pointers are forwarded to the
original tier0 functions. KeyValues memory (`vstdlib!KeyValuesSystem()` vtable slots 1/2
`AllocKeyValuesMemory`/`FreeKeyValuesMemory`) is routed the same way, because KeyValues uses its own
pool, not `g_pMemAlloc`.

## vstdlib objects

- `g_pThreadPool` (exported `IThreadPool*`). vtable (from Mac `CThreadPool::vtable`):
  7 `NumThreads`, 8 `NumIdleThreads`, 9 `SuspendExecution`, 10 `ResumeExecution`,
  11/12 `YieldWait`. We suspend/resume the pool around a snapshot copy.
- `KeyValuesSystem()` exported fn → `IKeyValuesSystem*`. vtable: 1 `AllocKeyValuesMemory`,
  2 `FreeKeyValuesMemory`.
- `VEngineCvar007` = `CCvar` (`ICvar`). vtable: 9 `RegisterConCommand`, 10 `UnregisterConCommand`,
  13/14 `FindCommandBase`, 15/16 `FindVar`. (Mac `CCvar::vtable` @ `0x25238`.)

`ConCommand`/`ConVar` memory layout is in `src/sdk.h`; verified against the Mac symbols and the
tier1 headers. We never author a `ConCommand` vtable: we copy the vtable pointer of an existing
engine command (`echo`) and set `m_fnCommandCallback` + the "new callback" bit, so Dispatch is the
engine's own code.

## Server plugin interface

`engine.dll` `CPlugin::Load` (Windows `FUN_10136f50`, Mac `CPlugin::Load` @ `0x30d4f0`) tries
`ISERVERPLUGINCALLBACKS` **003 → 002 → 001** via `Sys_GetFactory`. The engine's `CServerPlugin`
dispatches to **fixed vtable slots** independent of the version a plugin answered to; those slots
are the **003** layout (verified from the vtable-call offsets in each `CServerPlugin::*` method on
Mac):

| slot | method | engine call offset |
|---|---|---|
| 5 | LevelInit | (server dll `0xc8` path) |
| 6 | ServerActivate | |
| 7 | GameFrame | `+0x1c` |
| 8 | LevelShutdown | |
| 9 | ClientActive | `+0x24` |
| 10 | **ClientFullyConnect** | `+0x28` |
| 11 | ClientDisconnect | `+0x2c` |
| 12 | ClientPutInServer | `+0x30` |
| 13 | SetCommandClient | `+0x34` |
| 14 | ClientSettingsChanged | `+0x38` |
| 15 | ClientConnect | `+0x3c` |
| 16 | ClientCommand | `+0x40` |
| 17 | NetworkIDValidated | `+0x44` |
| 18 | OnQueryCvarValueFinished | `+0x48` |
| 19 | OnEdictAllocated | `+0x4c` |
| 20 | OnEdictFreed | `+0x50` |

Our plugin exports `CreateInterface` and returns the object for **`ISERVERPLUGINCALLBACKS003`**.
`plugin_load <name>` (engine `plugin_load` cmd, Mac @ `0x30eb80`) loads `<name>.dll` from the GAME
search path (the Portal 2 root), calls `CreateInterface`, then `Load(appSystemFactory, gameFactory)`.

## Frame loop / snapshot timing

- `CEngine::Frame` (Mac `0x3215f0`, Windows `FUN_101f9e30`, found via the `CEngine` RTTI vtable at
  `0x10394d54` → its `Frame` slot). Calls `HostState_Frame` → `_Host_RunFrame`.
- `_Host_RunFrame` (Mac `0x219b00`): per-tick it calls `_Host_RunFrame_Input`,
  `_Host_RunFrame_Server`, `_Host_RunFrame_Client`, then `_Host_RunFrame_Render` +
  `_Host_RunFrame_Sound`. When `g_bThreadedEngine` is set it offloads the server tick to the thread
  pool as `_Host_RunFrame_Server_Async` (a `CFunctorJob`) and `WaitForAsyncServer` later. So the
  server can run on a worker thread → we must quiesce the pool before touching memory.
- Threaded sound: `_Host_RunFrame_Sound` posts `g_pSoundJob` when `snd_mix_async`/`host_thread_mode`;
  `Host_EndThreadedSound` joins it. Material queue: `CMaterialSystem::EndFrame` (Mac `0x175b0`)
  spins up its own `MatQueue` thread pool when `mat_queue_mode != 0` and defers rendering via
  `CMatQueuedRenderContext`. We force `mat_queue_mode 0` for 1-2 frames before a snapshot op and
  restore it after.
- The client's per-frame hook point we use is `IBaseClientDLL::FrameStageNotify`
  (`VClient016`, slot 35; `ClientDLL_FrameStageNotify` Mac `0x11bf60` → `_g_ClientDLL + 0x8c`).
  We run save/restore at `FRAME_START` (stage 0), outside any queued rendering.

## Input / net after a restore

- `IBaseClientDLL::IN_ClearStates` = `VClient016` slot 18 (`g_ClientDLL + 0x48`; engine
  `ClearIOStates` Mac `0x31d450`, Windows `FUN_101f6d90`). Called after a restore to drop held
  buttons.
- Net timeouts are time-based (`CNetChan::IsTimedOut`/`IsTimingOut` compare against
  `net_time`). For a local listen-server game this is not a problem across a restore because we
  preserve `CEngine`'s timing fields and the loopback channel has no real timeout. For a remote
  server it would disconnect — snapshots are intended for single-player / listen server.

## Arena design (`src/arena.{h,cpp}`)

- One big `VirtualAlloc(MEM_RESERVE | MEM_WRITE_WATCH)` region; committed on demand. All allocator
  metadata lives inside the region, so snapshotting = copying `[Base, top)` + the writable sections
  of the domain modules.
- TLSF free lists (segregated by size class) for O(1) alloc/free with boundary-tag coalescing.
- Snapshot: freeze every live block. After the snapshot, frozen blocks are never reused (frees are
  deferred, reallocs never grow in place), so their addresses stay stable. New allocations come only
  from space that was free at snapshot time or from heap growth.
- Restore: blocks allocated after the snapshot that are still live ("zombies") can optionally be
  kept (something outside the domain may reference them); everything else is reverted by copying the
  image back (skipping kept zombies), then the free lists are rebuilt around the kept zombies.
- `MEM_WRITE_WATCH` + `GetWriteWatch` give the set of dirty pages so a restore copies back only what
  changed (falls back to a full copy on overflow).

`tests/arena_test.cpp` stress-tests all of this (random alloc/free/realloc, 6×5 save/restore cycles
with surviving zombies, full heap consistency checks). ~32 ns per alloc+free.

## 2026-10-18 (evening) — findings that made restores work

- **engine!0x10124d60 = CShadowMgr::AddShadowsOnSurfaceToRenderList(ShadowDecalHandle_t)** (__thiscall,
  this = CShadowMgr). Walks `m_ShadowDecals` (CUtlLinkedList<ShadowDecal_t, unsigned short>, memory at
  this+0x30, 0x14-byte elements: +0x00 surface, +0x08 ushort shadow index, +0x0e m_NextRender,
  +0x12 next; 0xffff = invalid), Shadow_t records (0x150 bytes) at this+8 (+0x78 flags, +0x7a sort
  order, +0x130 frame stamp), render queue heads (ushort array) at this+0x84, counters at this+0x1418c /
  0x141d0. The start handle comes from the BSP surface data in the **hunk**. Global CShadowMgr object at
  engine+0x63d180 in the RE build. ConVar checks via DAT_1063cc74/DAT_1063cccc (+0x30 = m_nValue).
- **The hunk** is a tier1 CMemoryStack statically linked into engine.dll: VirtualAlloc(MEM_RESERVE,
  PAGE_NOACCESS) once (48 MB in sp maps), then MEM_COMMIT PAGE_READWRITE as it grows.
- **vstdlib+0x32780** (RE build: 0x10032780) is a CTSQueue (the thread pool job queue). Pop loop =
  vstdlib!0x10009870: m_Head {pNode,+0 seq,+4}, m_Tail {+8,+0xc}, End() == this, 64-bit CAS via
  tier0!ThreadInterlockedAssignIf64; spins while head.pNode->pNext == NULL (= a freed node).
- **IVEngineClient "VEngineClient015"**: CEngineClient object at engine+0x432c1c (RE build), vtable at
  engine+0x3553f4. Slots (same on Mac and Windows): 7 ClientCmd, 104 ExecuteClientCmd,
  108 ClientCmd_Unrestricted (cdll_engine_int.cpp:0x330 split-screen guard + Cbuf_AddText ×2).
- tier0 RTTI: `.?AVCStdMemAlloc@@` (the g_pMemAlloc in use by default), `.?AVCHeapMemAlloc@@`, `.?AVIMemAlloc@@`.
- portal2.exe imports only USER32 and KERNEL32; launcher.dll (→ tier0, vstdlib) is LoadLibrary'd by main().

## 2026-10-19 - sound (vaudio_miles / Miles) and engine sound interface

- The engine mixes audio itself and outputs through DirectSound (engine strings "DirectSound
  initialized"). Miles (mss32.dll + bin/*.asi, *.flt) is only used as a codec through vaudio_miles.dll.
- **vaudio_miles.dll** (Ghidra project `win_vaudio`): `CreateInterface("VAudio002")` returns a NEW
  `CVAudio` (4 bytes, vtable vaudio!0x1000c374, RTTI `.?AVCVAudio@@`). Slots: 0 deleting dtor,
  1 `CreateMP3StreamDecoder(IAudioStreamEvent*)` (new CMilesMP3, 0x40 bytes, via g_pMemAlloc -> arena;
  init FUN_100012b0 opens the "ASI stream" through RIB_request_interface and stores ASI_stream_open /
  close / process / property pointers inside the object at +0x20..+0x30), 2 `DestroyMP3StreamDecoder
  (IAudioStream*)` (calls the decoder's deleting dtor -> ASI_stream_close), 3/4 open/close a Miles
  digital driver. Create/Destroy are __thiscall, `ret 4`. vaudio_miles does not install Miles memory
  callbacks: Miles stream state lives in Miles' own heap (not in the arena).
- Crash signature when a restored channel uses a closed stream: access violation in
  `mssmp3.asi+0xaa89` (write to 0) called from `vaudio_miles.dll+0x1184` <- engine mixing code.
- **IEngineSound** "IEngineSoundClient003" (engine, object at engine+0x430038 in the RE build, vtable
  engine!0x1034611c): slot 9 = `StopAllSounds(bool)` -> engine!0x10017880 = S_StopAllSounds (takes the
  sound mutex, logs "Stopping All Sounds..." / "Stopping: Channel:%2d %s" with developer 1). Same slot on
  Mac (CEngineSoundClient vtable).
- `cl_soundscape_flush` (client) exists but is FCVAR_CHEAT (flags 0x10004008).

## 2026-10-19 - what a level change releases outside the snapshot (snapshots that outlive a map)

A restore after a level change used to hang right away: `failed to lock vertex buffer in
CMeshDX8::LockVertexBuffer` then an error box (engine+0x1f5711 in a MessageBox loop). Everything below is
state the engine references from the arena but that lives elsewhere; each has a guard in `src/`.

- **Direct3D 9 objects** (d3dguard). `shaderapidx9.dll` imports only `d3d9!Direct3DCreate9` (no
  `Direct3DCreate9Ex`: plain D3D9). With `-vulkan` the engine loads `shaderapivk.dll`, which imports
  `dxvk_d3d9.dll!Direct3DCreate9` (both DLLs ship in `bin\`). Prologues: system d3d9 `8B FF 55 8B EC`
  (hotpatch), DXVK `55 89 E5 56 53` (MinGW). The Steam overlay wraps Direct3DCreate9 and CreateDevice: the
  calls reach the hook from `GameOverlayRenderer.dll+0x7c5bb`, so "called by the shader API" must be
  checked on the whole stack for those two; the device `Create*` methods are called directly by
  shaderapidx9 (return address test). COM slots used (d3d9.h order): IDirect3D9 16 CreateDevice;
  IDirect3DDevice9 16 Reset, 23 CreateTexture, 24 CreateVolumeTexture, 25 CreateCubeTexture,
  26 CreateVertexBuffer, 27 CreateIndexBuffer, 28 CreateRenderTarget, 29 CreateDepthStencilSurface,
  36 CreateOffscreenPlainSurface, 59 CreateStateBlock, 61 EndStateBlock, 86 CreateVertexDeclaration,
  91 CreateVertexShader, 106 CreatePixelShader, 118 CreateQuery. sp_a2_laser_intro at save time:
  ~5670 live objects (889 textures, 647 VB, 603 IB, 955 VS, 2460 PS, 70 queries, 15 surfaces); a change
  to sp_a2_laser_stairs and back: 416 reference counts to set back, ~605 objects of the new map to release.
- **Map files** (fileguard). The map's BSP (pak lump) and the VPK chunks are opened by filesystem_stdio
  with `CreateFileA` (+ `ReadFile`/`GetOverlappedResult`); a level change closes the old BSP and the new
  one usually gets the same handle value. ~52 game files are open in a test chamber.
- **steam_api registrations** (steamguard). steam_api.dll (bin\, base 0x3b400000 in Ghidra) keeps a
  `std::multimap<int id, CCallbackBase*>` (singleton returned by FUN_3b406d50, +0) and a
  `multimap<SteamAPICall_t, CCallbackBase*>` for call results (+0x40). CCallbackBase: +0 vtable,
  +4 flags (bit0 registered, bit1 game server), +8 m_iCallback. `SteamAPI_RegisterCallback(obj, id)`
  (rva 0x98c0, `55 8B EC 83 3D <abs> 00`) sets the flag and inserts (no early-out);
  `SteamAPI_UnregisterCallback(obj)` (rva 0xa190) is a `jmp` thunk to FUN_3b406bc0 (`55 8B EC 83 EC 0C`),
  which needs flags&1 and erases (obj->m_iCallback, obj); `SteamAPI_RegisterCallResult(obj, hCall)`
  (rva 0x9880) / `SteamAPI_UnregisterCallResult(obj, hCall)` (rva 0xa170, `55 8B EC FF 75 10`). A call
  result is dispatched (and erased) through the internal callback 703 (SteamAPICallCompleted_t). The
  game imports them from engine, client, server and matchmaking. Crash signature when a call result
  issued after the save completes after a restore: `exception 0xc0000005 at 00000000: execute at
  00000000` under `steam_api+0x6b35 <- SteamGameServer_RunCallbacks <- engine+0x196c36` (the restore set
  the CCallResult's member-function pointer back to null). 38 callbacks are registered in a test chamber;
  ~4 call results are in flight a few seconds after a save.
- **Bink videos** (videoguard). `valve_avi.dll` (interfaces VAvi001 / VBik001, singleton in its .data,
  hence added to the domain) opens .bik files with `_BinkOpen@8` (flags 0x4400 or 0x6400, | 0x4000000
  when the filesystem gives a direct path) and routes Bink's memory to g_pMemAlloc:
  `BinkSetMemory(0x10003c40, 0x10003c60)` = thunks to IMemAlloc slots 1 (Alloc) / 5 (Free). binkw32.dll
  itself has a private heap, threads (CreateThread) and opens the file with CreateFileA. Prologues:
  `_BinkOpen@8` `55 8B EC 83 E4 F8`, `_BinkClose@4` `56 8B 74 24 08 57`. The arrival elevator of a test
  chamber has one video open (`@arrival_sign_*` vgui_movie_display panels).
- **Single player exit**: test chambers have `trigger_once 'transition_trigger'`, logic_relay
  `@transition_from_map`, point_changelevel `@changelevel`, and point_teleport `@exit_teleport` (into
  the exit elevator). `ent_fire @exit_teleport Teleport` makes the player take the exit as in normal
  play (one LevelShutdown, then LevelInit of the next map ~1 s later, ~6.6 s in total). The vscripts are
  shipped encrypted (.nuc). `ent_find` does not exist in Portal 2; `script_execute <file>` runs a .nut
  from portal2\scripts\vscripts (Entities.Next() to enumerate).
- The vatrack regions keep their base across a level change (the 11 regions of a test chamber are
  reserved once), but their committed size can shrink: going from sp_a3_jump_intro to sp_a3_bomb_flings,
  the engine.dll reservation of 3840 KB (128 KB committed at save) was partly decommitted, the old
  va_revert skipped the whole segment and the first frame after the restore read an uncommitted page
  (`exception 0xc0000005 at engine.dll+0x1eabd8`, called from client rendering). va_revert now commits
  such pages again before copying them back, and reports the segments whose region was released.

## 2026-10-19 - speed of a save / restore, and SAR

- Measured phases before the rework (sp_a2_laser_intro, 85 threads, Ryzen 7 5700X): save = suspend
  63 ms + arena copy 72 ms (175 MB) + modules 4 ms + VA 9-18 ms; restore = suspend 57 ms + everything
  else ~7 ms. The suspension cost was **toolhelp**: `Thread32First/Next` walk every thread of the system
  (thousands with Steam/browsers running) and remap the snapshot section at each step (~50 ms); the
  OpenThread/SuspendThread calls themselves took 0.2 ms in total. `NtGetNextThread` (ntdll, Vista+)
  walks only our threads: suspend 1.5 ms. The arena copy cost is the page faults of the store section's
  fresh views (2.4 GB/s single-threaded); 8 MB chunks with their own view over 3 / 7 workers: 29 / 25 ms.
  Restore copies are bandwidth bound (pages already resident): parallel copying does not help there.
- SAR 1.15.5 (`D:\OpenCode\sourceautorecord`, owner-approved reading for compatibility only) hooks engine
  functions with MinHook / its own Hook class (ProcessTick, Host_AccumulateTime, Cmd_ExecuteCommand,
  _Host_RunFrame_Render, BinkNextFrame/ShouldSkip/Wait - not BinkOpen/BinkClose) and swaps the vtable
  pointer of interface objects for copies (CHLClient included, so FrameStageNotify chains through our slot
  patch, which is in place earlier). srconfigs (`autoexecgood.cfg`) loads it with `plugin_load sar`, runs
  `sar_update pre exit`, `sar_fast_load_preset full` and demo autorecord (`sar_autorecord 1`).
- Demo files: CDemoFile writes through the filesystem's CStdioFile (CRT FILE, buffer in the arena, 4 KB
  blocks). At stop it seeks to 0 and rewrites the 0x430-byte header. A restore that reverted the recorder
  but not the kernel file pointer made the next flush land after the header (first message unreadable).
  Header layout used by the checker (scratch tool): magic, demo/net protocol, server/client/map/game dir
  (260 bytes each), playback time float @0x420, ticks @0x424, frames @0x428, signon length @0x42c; then
  messages: cmd byte, tick int32, slot byte; signon/packet carry 2 x 76-byte CmdInfo + 2 sequence ints +
  size-prefixed data; dem_stop = 7.

## 2026-10-19 - demo custom data, SAR records and HUD (named snapshots, v0.4.0)

- **IVEngineClient slot 139 = RecordDemoCustomData(callback, data, len)**: Windows engine!0x1007c080
  (entry at vtable+0x22c: CEngineClient vtable engine+0x3553f4, entry 0x10355620); Mac
  `CEngineClient::RecordDemoCustomData` @0x0011ac60, entry 0x00579154 in `__ZTV13CEngineClient`
  (0x00578f20 + 8 + 139*4). Both begin `push ebp; mov ebp,esp; mov ecx,[demorecorder]` (Windows global
  0x1050031c), call `IsRecording` at +0x10 (slot 4), binary-search the callback in
  `g_RegisteredDemoCustomDataCallbacks` (sorted by function pointer, 8-byte entries), then call
  `RecordCustomData(index, data, len)` at +0x38 (slot 14). sarlink reads the global from those bytes.
- The client registers one callback: `RadialMenuMouseCallback` (Mac client @0x005ef930): x/y ints from
  the first 8 bytes into the CRadialMenu HUD element (+0x254/+0x258). SAR therefore puts 8 bytes of
  cursor position (its last recorded one, zero until the co-op radial menu records one) before its own
  records, all sent as callback 0.
- **SAR records** (first byte after the 8): 0x01 timescale cheat, 0x02 initial cvar (name\0value\0),
  0x03/0x04 entity input, 0x05 portal placement, 0x06 CM flags, 0x07 crouch fly, 0x08 pause, 0x09 wait
  (tick + command), 0x0A / 0x12 speedrun time, 0x0B timestamp, 0x0C file checksum, 0x0D hwait, 0x0E
  entity slot/serial, 0x0F frametime, **0x10 queued command** (the text, for commands run before the demo
  started: Cmd_ExecuteCommand detour, FCVAR_DONTRECORD excluded), 0x11 vpk checksum, 0x13 speedrun id,
  0x14 vscript checksum; 0xFE signed checksum, appended after dem_stop (tick -1) by AddDemoChecksum when
  a segment ends (SetSignonState NEW, StopRecording; not at `quit`). mdp prints unknown types as
  "Unhandled message type" and goes on; SAR's demo playback acts on 0x05/0x06/0x07 only. Our snap_*
  commands are FCVAR_DONTRECORD (a dem_consolecmd would be executed at playback).
- SAR keeps its demo bookkeeping in its own memory (`currentDemo`, `lastDemoNumber`,
  `isRecordingDemo`), outside the snapshot, and names the file it signs from it. The engine's recorder
  (m_bRecording +0x646, m_nDemoNumber +0x648, start tick +0x53c) is reverted with engine.dll .data.
  After a restore from another map they disagree: the restored segment is truncated back to the save
  (its signature from the level change is cut), the next map's segments stay without dem_stop (their
  handles are closed by fileguard), and SAR later signs the segment its own counter names. Header tick
  counts are `net_time / interval_per_tick - start tick` at stop (CDemoRecorder::GetRecordingTick, Mac
  @0x00125340); negative values were seen in a session with restores (open question).
- **HUD**: `sar_hud_set_text <id> <text>` (FCVAR_DONTRECORD), text after the id taken as one quoted
  argument, `#RRGGBB` starts a colour and `#r` resets it; the lines are drawn by id order. srconfigs uses
  ids 1-4 (aliases.cfg) and 100 (attempt counter); ours is 50.
