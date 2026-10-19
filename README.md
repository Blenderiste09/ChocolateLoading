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
shows `ChocolateLoading 0.3.0 ready (early loader)` followed by `Loaded plugin "chocolateloading"`.

## Use

In a map (not in the main menu, not during a loading screen):

| Command     | Effect                                                                     |
|-------------|----------------------------------------------------------------------------|
| `snap_save` | snapshot the current state (~40 ms)                                        |
| `snap_load` | go back to the snapshot (~10 ms), as many times as you like, from any map  |
| `snap_drop` | discard the snapshot and release its memory                                |
| `snap_info` | statistics (sizes, number of restores, ...)                                |

Binding keys is the most convenient way to play with it:

```
bind F6 snap_save
bind F7 snap_load
```

The snapshot survives level changes: after taking the elevator to the next map, loading a save game or
reloading after a death, `snap_load` takes you back to the snapshot, in its map (a few tens of ms
more than within the map, still more than ten times faster than a regular load). It stays until you take another one
(`snap_save`) or drop it (`snap_drop`).

## Limitations

- While a snapshot exists, the graphics, sound, video and file resources of its map are kept alive even
  after you leave that map: expect a few hundred MB of extra memory until the next `snap_save` or
  `snap_drop`.
- A graphics device reset (alt-tab in exclusive fullscreen, changing the video mode) releases resources
  the snapshot needs: `snap_load` then refuses and asks for a new snapshot. Windowed and borderless modes
  are not affected.
- Loaded with `plugin_load` instead of `chocoload.exe`, the plugin comes too late: allocations made
  before it are not captured and restores are not reliable in that mode.
- Works with SAR loaded (tested with SAR 1.15.5 and the srconfigs presets: fast loads, demo
  autorecord). A demo being recorded is put back with the game on `snap_load`, so it stays valid; the
  demo segments started in a map you came back from stay on disk.
- Tested in single player only (not in co-op). Do not use it on VAC-secured servers: it injects code
  into the game process.
- Sound: a restore stops the sounds started after the snapshot and brings back the ones that were
  playing when it was taken; an MP3 voice line or music track may glitch briefly at that moment.
- The snapshot costs memory while it exists: about the size of the engine heap (170-240 MB, in a
  pagefile-backed section) plus ~60 MB for the module data and the engine hunk.

## For developers

See `AGENTS.md` (architecture, status, the automated in-game tests `-snap_autotest*`) and
`re/notes.md` (reverse-engineering notes: addresses, vtable slots and the evidence for each).
