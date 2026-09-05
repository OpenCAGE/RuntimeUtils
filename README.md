# RuntimeUtils for Alien: Isolation

A fork of [RyanJGray's DevTools](https://github.com/RyanJGray/AlienIsolation.DevTools) to provide runtime utility in OpenCAGE to improve workflows for modders working in Alien: Isolation.

## Features

- **Hot reload** - press a key (INSERT by default) to restart the current level.
- **Debug text** - makes the `DebugText` and `DebugTextStacking` script entities work. Retail builds of the game disable these entities and strip the code that draws them, so the ASI re-enables them in memory and draws their text itself: `text` followed by any linked or non-default inputs in brackets, positioned by `alignment`, styled by `size` and `colour`, hidden after `duration` seconds (or kept until `stop` when the duration is -1, live-updating its inputs). Stacking text is a block on the middle left, newest at the bottom, five entries deep. Everything clears on level change. `DebugPositionMarker` draws XYZ axes at its `world_pos` while started, and `DebugEnvironmentMarker` draws its `text` there in its `colour` and `size`.

- **Zone loading** - the game only streams in the zones around the player and whatever the camera's view ray lands on, so a free camera (e.g. Cinematic Tools) can fly into areas that never load. With `LoadAllZones` on, every zone of the level is registered as viewed each frame and streams in. The ASI also exports `OpenCAGE_SetForceZoneLoading(bool)` so other injected tools can switch this on while their camera is active; Cinematic Tools does so.

## Configuration

Settings are read from `OpenCAGE_Utils.ini` next to `AI.exe`. The file is optional; every key falls back to its default.

```ini
[RuntimeUtils]
HotReload=1          ; 1/0 - reload the current level on a key press
HotReloadKey=INSERT  ; a key name (INSERT, DELETE, HOME, END, PAGEUP, PAGEDOWN, F1..F12, A..Z, 0..9)
                     ; or a Windows virtual-key code, decimal or hex (e.g. 0x2D)
DebugText=1                 ; 1/0 - DebugText entities draw their text on screen
DebugTextStacking=1         ; 1/0 - DebugTextStacking entities draw their text on screen
DebugEnvironmentMarker=1    ; 1/0 - DebugEnvironmentMarker entities draw their text at a world position
DebugPositionMarker=1       ; 1/0 - DebugPositionMarker entities draw axes at a world position
LoadAllZones=0              ; 1/0 - stream every zone of the level in, not only those around the player
```

## Building

Build `OpenCAGE_Utils.vcxproj` as Release|Win32. When building from the command line pass the project directory as the solution directory, e.g. `msbuild OpenCAGE_Utils.vcxproj /p:Configuration=Release /p:Platform=Win32 /p:SolutionDir=<path to this folder>\`. The output is `build/OpenCAGE_Utils.asi`, loaded by the bundled `winmm.dll` ASI loader (which OpenCAGE copies into the game folder as `d3d11.dll`).

The game offsets used by the hooks are for the Steam retail build of `AI.exe`.
