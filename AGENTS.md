# ChocolateLoading - developer handbook

Goal: make Portal 2 loading as fast as possible. The approach is a DLL that keeps the whole game state
in its own heap and can **snapshot RAM and restore it**, so going back to a point of a map is a memory
copy (~10 ms) instead of a level load (~1 s warm, more cold). User-facing instructions are in
`README.md`.

This file is the entry point for every new contributor and every AI session. Read it fully first.

## Hard rules (from the project owner)

- **Do not look at, read, or copy from the other plugins in the game folder: `SAR` (`sar.dll`) and
  `FastLoads` (`FastLoads.dll`, `P2Plugin.dll`, `portal2_hello.dll`), nor their logs/configs.**
  This project is independent. (They are unrelated test artifacts that happen to sit in the folder.)
  Exception granted by the owner on 2026-10-19: to check that snapshots work with SAR loaded, the
  srconfigs autoexec (`portal2\cfg\autoexecgood.cfg`, renamed to `autoexec.cfg` for the test, then back)
  may be used and SAR's source in `D:\OpenCode\sourceautorecord` read to understand interactions.
  Never copy its code.
- Game install: `E:\Program Files (x86)\Steam\steamapps\common\Portal 2`.
- Symbolised reference builds (use for RE, 99% same code): Mac `E:\P2\MAC`, Linux `E:\P2\Linux`.
- Ghidra: `D:\ghidra\ghidra_12.1.4_PUBLIC`. The REA skill + its MCP tools are available for decomp.

## Layout

```
src/                  the plugin (build\chocolateloading.dll, x86)
  plugin.cpp          entry points: DllMain + early-loader bootstrap, IServerPluginCallbacks, frame hook
  control.{h,cpp}     runs snap_save/snap_load at a safe point (FRAME_START), threading cvars, snap_* commands
  autotest.{h,cpp}    unattended in-game self-test (-snap_autotest*): probe script, stress cycles, benchmark
  snapshot.{h,cpp}    what a snapshot is: capture and restore of arena + module .data + VirtualAlloc regions;
                      up to 16 named snapshots, each in a slot whose number the guards use as a bit
  sarlink.{h,cpp}     what SAR users see: snap_save/snap_load records in the demo, a line of SAR's HUD
  arena.{h,cpp}       snapshot-aware TLSF heap that holds the whole engine heap (the core algorithm)
  copypool.{h,cpp}    worker threads that share the big copies of a save with the main thread
  memhook.{h,cpp}     redirects tier0's allocator (g_pMemAlloc) and KeyValues into the arena
  vatrack.{h,cpp}     NtAllocateVirtualMemory hook: registry of the engine's direct reservations (the hunk)
  callsite.{h,cpp}    "was this hooked call made directly by game code?" (stack walk shared by the NT hooks)
  soundguard.{h,cpp}  keeps MP3 decoders (vaudio_miles / Miles) alive while a snapshot references them
  d3dguard.{h,cpp}    same for the Direct3D 9 objects of the shader API (textures, buffers, shaders...)
  fileguard.{h,cpp}   same for the game's open files (map BSP, VPK chunks): NtCreateFile/NtOpenFile/NtClose
  steamguard.{h,cpp}  keeps steam_api's callback / call-result registrations consistent across a restore
  videoguard.{h,cpp}  keeps Bink videos (binkw32, elevator screens) alive while a snapshot references them
  pinset.h            the registry soundguard / videoguard share: objects pinned by a mask of snapshots
  gameapi.{h,cpp}     engine interfaces: CreateInterface lookup, ICvar commands/cvars, IVEngineClient
  sdk.h               Portal 2 vtable slots / struct layouts (all RE-verified, see re/notes.md)
  util.{h,cpp}        logging, module sections, vtable/inline hooking, RTTI scan, thread suspension
tests/arena_test.cpp  standalone arena stress test (no game needed)
tools/                chocoload.cpp (the launcher users run) + diagnostics for a running game:
                      stackdump (threads' EIP + call stack), peek (dwords), ptrinfo (where an address
                      lives, follows pointer fields), whichmod (modules), memmap (large private regions)
re/                   reverse-engineering workspace
  notes.md            ALL the RE findings (addresses, vtable slots, evidence) - read this
  ghidra.ps1 / import.sh / query.sh   drive headless Ghidra (persistent projects)
  ghidra_scripts/P2Query.java         batch query script (decomp/disasm/xrefs/vtable/...)
  ghidra_projects/<name>/             analyzed projects (win_* and mac_* per binary)
  bin/win/            copies of the Windows DLLs (Ghidra rejects paths with spaces or `$`)
  out/                query outputs and import logs
build.ps1             build + optional -Test / -Deploy
```

## Build & deploy

Visual Studio 18 (Community) is installed; no cmake/ninja. Node 24 is available, Python is not.

```powershell
cd D:\OpenCode\ChocolateLoading
.\build.ps1            # build\chocolateloading.dll (+ .pdb) and build\tools\*.exe, x86, static CRT
.\build.ps1 -Test     # also build and run tests\arena_test.exe (must print "ALL OK")
.\build.ps1 -Deploy   # also copy the DLL (+ .pdb) and chocoload.exe into the Portal 2 folder
```

The build must stay warning-free at /W3. The DLL is 32-bit, `/MT`, built with /GS, ASLR and DEP, and
imports only KERNEL32: tier0/vstdlib/engine/client are resolved at runtime.

## Using it in-game

1. Steam must be running. Launch `<Portal 2>\chocoload.exe [portal2 args]` (e.g. `-novid -condebug`).
   `-condebug` mirrors the console to `portal2\console.log`, the easiest way to read output from
   outside the game. On the first frame the plugin registers its commands and runs
   `plugin_load chocolateloading` itself (for the level callbacks).
2. In a map: `snap_save [name]`, `snap_load [name]` (default name `quick`; letters, digits, `_ - .`, up
   to 31 characters, case-insensitive), `snap_drop [name|*]`, `snap_list`, `snap_info`, `snap_check`
   (arena consistency), `snap_test [cycles] [frames]` (restore the current snapshot repeatedly),
   `snap_opt <suspend_threads|zombies|clear_input|verify> <0|1>`, `snap_domain [modules]` (show/set the
   captured modules).
- Log file: `<Portal 2>\chocolateloading.log` (always written, even without `-condebug`).

Launch options:

| Option | Effect |
|--------|--------|
| `-snap_arena_mb <256..2048>` | arena reservation (default 1024 MB) |
| `-snap_zombies_keep` / `-snap_zombies_free` | keep or free post-snapshot blocks on restore (default: free with chocoload, keep with plugin_load) |
| `-snap_no_va` | diagnostic: do not capture the engine's VirtualAlloc regions (restores then hang) |
| `-snap_crt_in_arena` | diagnostic: put the CRTs' per-thread data in the arena (reproduces the freeze on quit; the probe crashes) |
| `-snap_full_revert` | diagnostic: copy the whole arena back instead of the dirty pages |
| `-snap_copy_threads N` | copy workers used by a save (default: logical CPUs - 1, at most 3; 0 = main thread only) |
| `-snap_hud_id N` | SAR HUD text id of the snapshot line (default 50; 0 = no line) |
| `-snap_autotest` | unattended self-test once a map is loaded (see `src/autotest.h`) |
| `-snap_autotest_probe` | functional check: getpos, save, noclip/move/fire portals, restore, getpos, jpeg |
| `-snap_autotest_bench` | afterwards time a regular `save` + `load` of the same spot |
| `-snap_autotest_mapchange <spec>` | then leave the map with the snapshot alive and restore from there: `<map>` (changelevel), `map:<map>`, `reload` (save + load), `transition` (exit elevator), `exec:<cfg>` |
| `-snap_autotest_hops N` | leave N maps in a row before restoring (A -> B -> C, then back to A) |
| `-snap_autotest_rounds N` | repeat that level change + restore N times, logging the process memory |
| `-snap_autotest_quit` | finally `quit` the game (the exit path, with the snapshot still alive) |
| `-snap_autotest_cycles N` / `-snap_autotest_settle N` | number of restores (20) / frames to wait first (600) |
| `-snap_autotest_verify` | arena heap check + checksum around every op (restores then take ~440 ms) |

**Regression test after any change** (run it on several maps):

```
chocoload.exe -novid -sw -w 1280 -h 720 -condebug -snap_autotest -snap_autotest_probe -snap_autotest_bench
              -snap_autotest_cycles 100 -snap_autotest_settle 3000 +sv_cheats 1 +map sp_a2_laser_intro
```

Expect `AUTOTEST COMPLETE`, the game still responding afterwards, and in console.log the `setpos` line
printed after each `probe: restoring` identical to the first one (P2 == P4 == P0). The probe then saves
a second snapshot `probe2` elsewhere and switches between the two (whole-image restores): N1 == N3 ==
P0 and N2 == N0. It also writes `jpeg` screenshots to `portal2\screenshots` (with SAR, the HUD shows the
`Snap:` line).

**And across a level change** (the snapshot must survive it): add `-snap_autotest_mapchange transition
-snap_autotest_rounds 2` on a test chamber (the player takes the exit elevator, as in normal play), or
`-snap_autotest_mapchange <next map>` on maps without one; `reload` and `map:<map>` cover the other ways
of leaving a map instance. Expect `level changed ... 2 snapshot(s) kept` on the console, P6 == P7 == P0
and N4 == N0 after every round (odd rounds restore `quick` first: its dirty pages from the other map;
even rounds `probe2` first: a whole image from the other map) and a stable `memory after round N` line.

## How it works

**Loading early.** `chocoload.exe` creates portal2.exe suspended and patches its EXE entry point with
`jmp $`. When the main thread spins there, process initialisation is done but `main()` has not yet loaded
launcher.dll (portal2.exe only imports USER32 and KERNEL32). chocoload then injects the DLL, restores the
entry bytes and resumes. `DllMain` (env `CHOCOLOAD_EARLY=1`) hooks ntdll!NtAllocateVirtualMemory
(`vatrack`) and registers a DLL-load notification: the moment tier0.dll is mapped, before its
initialisation, `memhook::InstallEarly` patches the `CStdMemAlloc` vtable (found by RTTI) and routes
**every** allocation to the arena. A worker thread completes the hook (KeyValues) and hooks the client's
`FrameStageNotify`; the first frame finishes the setup on the main thread.

**What a snapshot contains** (`snapshot.cpp`):
1. the arena = the whole engine heap (~170-240 MB), copied to a pagefile-backed section;
2. the writable sections (.data/.bss) of the `DEFAULT_DOMAIN` modules (22 sections, 15.3 MB; tier0 is
   excluded) - the log lists each `[snap]   module` line at every save;
3. the committed pages of the engine's direct VirtualAlloc reservations tracked by `vatrack`, above all
   the hunk (a tier1 CMemoryStack in engine.dll: 48 MB reserved PAGE_NOACCESS, ~40 MB committed) - the
   log lists them as `vatrack:   <owner> <base> reserved/committed`.

**Save / restore.** Requested from the console, they run at the next safe `FRAME_START` (`control.cpp`):
the threading cvars (`mat_queue_mode`, `host_thread_mode`, `snd_mix_async`, `net_queued_packet_thread`)
are forced to 0 a few frames before, the job pool and async filesystem are suspended, every other thread
is suspended, then memory is copied. The arena is reserved with MEM_WRITE_WATCH, so a restore copies back
only the pages written since the snapshot; blocks allocated after the snapshot ("zombies") are freed,
the free lists are rebuilt, `CEngine`'s real-time frame accumulators keep their current value, and held
input is released.

**Several snapshots.** Up to 16 exist at once, by name, each with its own section (~250-300 MB of
pagefile-backed memory, nothing mapped but the current one's module/VA part). The arena's write watch and
frozen blocks follow one of them, the current one (saved or restored last): restoring it copies back the
dirty pages; restoring another one copies its whole image back, and `arena::BeginFullRestore` makes the
free ranges and top recorded at its save the arena's bookkeeping. Each guard keeps, per object, a mask
of the snapshots that pin it plus whether the current timeline has it (`pinset.h` for the MP3 decoders
and videos; d3dguard records each snapshot's reference counts, fileguard each snapshot's file positions
and sizes, steamguard each snapshot's registrations); dropping a snapshot clears its bit.

**SAR** is always loaded by the players (srconfigs autoexec). `sarlink` writes each `snap_save` (just
before the capture, so the snapshot's own copy of the demo has it) and `snap_load` (just after the
restore) into the demo being recorded, the way SAR records the commands run before a demo starts: a
custom data message (callback 0, 8 bytes of co-op radial menu cursor, zero) holding SAR's queued-command
record (0x10 + the command text). SAR, mdp and the other parsers list it as a command at its tick; SAR's
playback does not run it. The restore takes the demo back to the save (fileguard), so the demo reads
`snap_save X` then `snap_load X` at the same tick, and what was played in between is not in it. With SAR
loaded, `sar_hud_set_text 50` shows `Snap: <names>` (the current one in green) or `Snap: none`.

**Speed.** Each save / restore logs its phases (`[snap] save phases (ms): suspend .. freeze .. arena ..
modules+VA .. unmap .. resume`). Two things matter: thread suspension walks only our process with
`NtGetNextThread` and queues every suspension before waiting for any (toolhelp's `Thread32Next` walks
every thread of the system and cost ~50-60 ms per operation), and the copies are shared with the
`copypool` workers, which `SuspendOtherThreads` leaves running: the arena image is written in 8 MB
chunks, each through its own view of the store section (the page faults of those fresh views are the
real cost), and the module sections and VirtualAlloc segments in 1 MB pieces: save 92-123 ms with the
main thread alone, 40-41 ms with 3 workers (Ryzen 7 5700X). A restore of the current snapshot copies
its dirty arena pages on the main thread (resident pages, bound by memory bandwidth), then the module
sections and VirtualAlloc segments (~56 MB) with the workers, from a view of that snapshot's section that
stays mapped: through fresh views the page faults came back at every restore (modules+VA 7.8 ms instead
of 3.7). Total 7.4-8.3 ms. Restoring another snapshot copies its whole image (~230 MB, views per 4 MB
piece, with the workers): 43-50 ms.

**What lives outside the snapshot** is kept consistent by the guards, all built the same way: at save
time they pin what the snapshot references, at restore time they undo what the discarded timeline
created, and they let go when the snapshot is dropped. `soundguard` (MP3 decoders, see Sound below),
`d3dguard` (every D3D9 object the shader API creates gets an extra reference at save; a restore sets the
reference counts back to their save-time values and releases the objects created since), `fileguard`
(closing a game file that was open at save is deferred; files opened since are closed at restore; a
restore also puts back each file's kernel file pointer, and cuts a file opened for writing back to its
size at save time - the demo being recorded, `workshop_log.txt`),
`steamguard` (callbacks registered and call results issued since the save are withdrawn before the
memory copy, callbacks unregistered since are registered again after it) and `videoguard` (BinkClose of
a video open at save is deferred, videos opened since are closed). Within one map they rarely matter;
across a level change they are what makes a restore possible: the old map's textures, buffers, BSP
handle, elevator video and pending Steam calls are all released or replaced by the new map.

**Level changes.** On LevelShutdown the snapshots are kept when every guard is active (early loader,
zombies freed, D3D / files / Steam / sound / video hooked: `level_change_blocker()` in snapshot.cpp),
otherwise they are dropped with the reason on the console. Restoring one from another map instance (next
map, `load`, death reload, `map`) brings the game back to the snapshot's map, and snapshot.cpp adopts the
snapshot's level generation and map name again. The cost is memory: the old map's arena blocks stay
frozen until the next `snap_save` (or until the current snapshot is dropped), and its GPU objects, files
and videos stay pinned while a snapshot of that map exists. A D3D device reset (exclusive fullscreen
alt-tab, video mode change) must release the default-pool resources, so d3dguard drops its pins before
it and the snapshots taken before it can no longer be restored.

**Sound.** The engine mixes audio itself (DirectSound) and its channels/mixers live in the arena, so they
are part of the snapshot; a save does not touch the sound. MP3 data (voice lines, music) is decoded
through vaudio_miles: each decoder (`CMilesMP3`, in the arena) wraps a Miles stream whose state lives in
Miles' own heap, outside the snapshot. `soundguard` hooks `CVAudio::Create/DestroyMP3StreamDecoder` (at
vaudio_miles map time) and defers the destruction of decoders the snapshot references until the snapshot
is dropped. A restore first stops the sounds of the discarded timeline (`IEngineSound::StopAllSounds`),
which destroys the decoders created after the snapshot, then brings back the channels that played at
save time; they keep playing (possibly with a short glitch for an MP3 stream).

See `src/arena.h` for the snapshot model of the heap and `re/notes.md` for every address and slot.

## Status (2026-10-19, v0.4.0)

**Working.** 100 consecutive restores at ~8 ms each (save ~40-45 ms) since the suspension / copypool
work (before: 58 ms and 145-170 ms), checksum-identical arena after every restore, no leak, game
responsive afterwards; the probe gives a bit-identical player position. Benchmark (regular `save` +
`load` of the same spot, warm cache): sp_a2_laser_intro 914 ms, sp_a4_finale4 1150 ms. **With SAR**
(1.15.5, srconfigs presets: fast loads, demo autorecord; always loaded since v0.4.0) everything passes
too; a regular load is then ~550 ms. Since v0.3.0 the snapshots survive level changes: a restore from
the next map takes ~20 ms, and repeated round trips do not grow the process memory. Since v0.4.0 there
are up to 16 named snapshots: switching to another one (whole image) takes 43-50 ms, also from another
map; `snap_save` / `snap_load` are recorded in SAR demos and shown on SAR's HUD.

The bugs that made restores hang or crash, and that must not come back:
1. **The hunk was not captured.** CMemoryStack reserves PAGE_NOACCESS and commits later: vatrack must
   accept any protection, and only DIRECT game-code reservations (never ntdll heap growth, which would
   revert the process heap). Symptom: endless loop in CShadowMgr::AddShadowsOnSurfaceToRenderList or
   "CUtlLinkedList overflow" in console.log.
2. **strtok is not reentrant**: the VirtualAlloc hook runs inside any loop that allocates, so a strtok
   in both silently truncated the captured module list. Always use strtok_s.
3. **Allocations made before the hook** landed in tier0's own heap, are never reverted, and corrupt
   structures that mix them with arena blocks (vstdlib's lock-free job queue). The hook must be in place
   before tier0's initialisation.
4. **MP3 decoders**: a voice line playing at save time and ending before the restore left the restored
   channel with a freed Miles stream (access violation in mssmp3.asi, the game closes silently). Fixed by
   soundguard (see How it works). Reproduced reliably on sp_a2_turret_intro with the probe.
5. **Steam call results**: an asynchronous Steam call issued after the save (stats, game server logon)
   completed after the restore, and steam_api called a CCallResult whose member-function pointer the
   restore had reset to null (`execute at 00000000` under SteamGameServer_RunCallbacks, typically during
   the next map load). Fixed by steamguard; latent before v0.3.0 even without a level change.
6. **D3D objects released by a level change** (`failed to lock vertex buffer in
   CMeshDX8::LockVertexBuffer`, then an error box): the reason snapshots used to be dropped on level
   shutdown. Fixed by d3dguard, with fileguard / videoguard for the old BSP handle and elevator videos.
7. **Decommitted VirtualAlloc pages**: a level change can decommit part of a tracked reservation (an
   engine.dll region on sp_a3_jump_intro -> sp_a3_bomb_flings); the restore skipped the segment and the
   next frame read an uncommitted page in engine.dll. va_revert now commits the pages again and copies
   them back (log: `VirtualAlloc ranges decommitted since the save were committed again`).
8. **Demos recorded across a restore were corrupt** (not a crash; found with SAR's demo autorecord): the
   demo recorder's memory went back to the save but the file kept its size and kernel file pointer; after
   a level change (the discarded timeline stopped the demo, rewriting its header at offset 0) the next
   writes landed right after the header. fileguard now records each pinned file's pointer and size at
   save time and puts them back.
9. **The CRTs' per-thread data in the arena** (the freeze on `quit` after a long session): the `_tiddata`
   block (0x214 bytes) a CRT allocates for a thread born after a save was freed by the restore, and the
   thread's exit (`__freefls`) worked on reused memory. memhook routes those blocks (recognised by the
   `push 214h; push 1; call` before the return address) to tier0's own heap. The probe checks it
   (`S_TLS_*` steps); `-snap_crt_in_arena` reproduces the bug.
10. **A module's .data is several regions** (its copy-on-write pages become read-write once written):
   checking the first region only skipped 13 sections at every restore, and the engine then freed again
   blocks the restore had freed (access violation in `arena::release`). The check walks every region.

Validated on 9 maps (sp_a1_intro3, sp_a2_laser_intro, sp_a2_turret_intro, sp_a2_triple_laser,
sp_a2_bts3, sp_a3_01, sp_a3_jump_intro, sp_a4_tb_trust_drop, sp_a4_finale4): probe + 50 restores +
benchmark each. v0.4.0 with SAR on sp_a2_laser_intro (transition + quit, reload), sp_a2_turret_intro,
sp_a3_jump_intro, sp_a1_intro3 (transition) and sp_a2_bts3 (changelevel): probe with two snapshots, 2
map-change rounds, 50 restores, benchmark. Not verified: co-op.

Next ideas:
- Shorter snap_load latency: a request waits 4 frames for the workers to drain unless the threading
  cvars are already forced - now longer than the restore itself (~9 ms). Keeping them forced while a
  snapshot exists would make it 1 frame, but costs frame rate (no queued rendering).
- Faster restore: the hunk and the module .data are copied in full (~56 MB, ~4 ms); MEM_WRITE_WATCH on
  the tracked reservations would allow dirty-page copies like the arena.
- Demos across a restore from another map (re/notes.md, "demo custom data"): SAR's segment bookkeeping is
  outside the snapshot, the next map's segments are left without dem_stop and the restored segment loses
  SAR's signature. One way: a restore ends the current segment and starts the next one, like a `load`.
- Arenas larger than 1 GB.

## RE workflow

Projects are already imported and analyzed. To query one (no re-analysis):

```bash
cd re && printf 'decomp CEngine::Frame\nvtable 0x25238 44\nxrefs 0x10394d54\n' > out/q.txt && ./query.sh mac_engine out/q.txt
```

`query.sh <proj> <cmdfile>` runs `ghidra_scripts/P2Query.java`; commands: `decomp`, `disasm [n]`,
`syms <re>`, `funcs <re>`, `vtable <addr> <n>`, `xrefs <addr>`, `strings <re>`, `bytes <addr> <n>`,
`funcat`, `callers`, `callees`, `findbytes <hex??>`. Projects: `win_engine win_tier0 win_vstdlib
win_matsys win_fs mac_engine mac_tier0 mac_vstdlib mac_matsys mac_fs mac_server mac_client`.
Prefer the **Mac** projects for names, then confirm addresses on the **Windows** project (that's the
binary we actually hook). To import another binary: `./import.sh <proj> <win-path-without-$> [cpus]
[maxmem]` then add `ghidra_projects/<proj>.progname` (the file name). The REA MCP tools (`rea__*`,
Ghidra provider) also work directly on the Windows DLLs and are good for one-off decompiles.

Diagnosing a crash: with the early loader the log records fatal exceptions (`exception 0xc0000005 at
module+offset: write to ...` followed by the stack, even when the game's crash handler closes it
silently) and, for a normal exit, who called ExitProcess (`process exit requested`).
Diagnosing a hang in a running game: `build\tools\stackdump <pid> all` shows each thread's EIP and
call stack as module+rva (Ghidra address = 0x10000000 + rva for the game DLLs); `ptrinfo <pid> <addr>
[offsets]` tells whether a structure lives in the arena (allocation base = the `arena reserved` address
of the same run's log), a module, or another region.
