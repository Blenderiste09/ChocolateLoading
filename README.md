# ChocolateLoading - RAM snapshots for Portal 2

Saves the complete game state in memory (`snap_save`) and restores it almost instantly (`snap_load`):
about **10 ms**, versus roughly 0.9-1.2 s to load the same spot from a regular save game that is
already cached (and much more from a cold disk).

## Install

Requires Visual Studio (C++ x86 tools).

```powershell
cd D:\OpenCode\ChocolateLoading
.\build.ps1 -Deploy      # copies chocolateloading.dll and chocoload.exe into the Portal 2 folder
```

## Launch

Steam must be running. Start the game with **`chocoload.exe`** (in the Portal 2 folder) instead of
Steam: it starts `portal2.exe` and loads the plugin before the engine, which is required for the whole
game memory to be captured. Its arguments are passed to the game, for example:

```
chocoload.exe -novid -condebug
```

(a desktop shortcut to `chocoload.exe` with those arguments works well). On the first frame the console
shows `ChocolateLoading 0.4.0 ready (early loader)` followed by `Loaded plugin "chocolateloading"`.

## Use

In a map (not in the main menu, not during a loading screen):

| Command               | Effect                                                                   |
|-----------------------|--------------------------------------------------------------------------|
| `snap_save [name]`    | snapshot the current state (~40 ms), replacing a snapshot of that name   |
| `snap_load [name]`    | go back to a snapshot, as many times as you like, from any map           |
| `snap_drop [name\|*]` | discard a snapshot (`*`: all of them) and release its memory             |
| `snap_list`           | the snapshots, their map and age                                         |
| `snap_info`           | statistics (sizes, number of restores, ...)                              |

Without a name, `snap_save`, `snap_load` and `snap_drop` use the snapshot `quick`. Names are letters,
digits, `_`, `-` and `.` (up to 31, case-insensitive), e.g. `snap_save intro4_segmented`; up to 16
snapshots can exist at once. Restoring the snapshot you saved or restored last takes ~8 ms; switching to
another one copies its whole image back, ~50 ms.

Binding keys is the most convenient way to play with it:

```
bind F6 snap_save
bind F7 snap_load
bind F8 "snap_load intro4_segmented"
```

The snapshots survive level changes: after taking the elevator to the next map, loading a save game or
reloading after a death, `snap_load` takes you back to the snapshot, in its map (~20 ms, more than
twenty times faster than a regular load). A snapshot stays until you replace it (`snap_save` with its
name) or drop it (`snap_drop`).

## With SAR

- SAR's HUD shows a `Snap:` line with the snapshots that exist (the one saved or restored last in
  green), or `Snap: none`. `-snap_hud_id N` on chocoload's command line picks another HUD text id than
  50 (`0`: no line).
- Every `snap_save` and `snap_load` is written into the demo being recorded, the way SAR records
  commands: SAR's tools and mdp show `snap_save quick` / `snap_load quick` as commands at their tick.
  `snap_load` takes the demo back to the save with the rest of the game, so the demo shows the save
  followed by the load at the same tick; what was played between them is not in it.

## Limitations

- While a snapshot exists, the graphics, sound, video and file resources of its map are kept alive even
  after you leave that map: expect a few hundred MB of extra memory until it is replaced or dropped.
- A graphics device reset (alt-tab in exclusive fullscreen, changing the video mode) releases resources
  the snapshots need: `snap_load` then refuses and asks for a new snapshot. Windowed and borderless modes
  are not affected.
- Loaded with `plugin_load` instead of `chocoload.exe`, the plugin comes too late: allocations made
  before it are not captured and restores are not reliable in that mode.
- Works with SAR loaded (tested with SAR 1.15.5 and the srconfigs presets: fast loads, demo
  autorecord). A demo being recorded is put back with the game on `snap_load`, so it stays readable.
  Coming back from another map is not clean for demos yet: the segments recorded in the maps you came
  back from stay on disk unfinished, and SAR's own bookkeeping (segment numbers, signature) is not put
  back, so the restored segment may end up without SAR's signature.
- Tested in single player only (not in co-op). Do not use it on VAC-secured servers: it injects code
  into the game process.
- Sound: a restore stops the sounds started after the snapshot and brings back the ones that were
  playing when it was taken; an MP3 voice line or music track may glitch briefly at that moment.
- Each snapshot costs memory while it exists: about the size of the engine heap (170-240 MB, in a
  pagefile-backed section) plus ~60 MB for the module data and the engine hunk.

## For developers

See `AGENTS.md` (architecture, status, the automated in-game tests `-snap_autotest*`) and
`re/notes.md` (reverse-engineering notes: addresses, vtable slots and the evidence for each).
