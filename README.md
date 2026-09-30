# RuntimeUtils for Alien: Isolation

A fork of [RyanJGray's DevTools](https://github.com/RyanJGray/AlienIsolation.DevTools) to provide runtime utility in OpenCAGE to improve workflows for modders working in Alien: Isolation.

## Features

- **Hot reload** - press a key (INSERT by default) to restart the current level.
- **Debug text** - makes the `DebugText` and `DebugTextStacking` script entities work. Retail builds of the game disable these entities and strip the code that draws them, so the ASI re-enables them in memory and draws their text itself: `text` followed by any linked or non-default inputs in brackets, positioned by `alignment`, styled by `size` and `colour`, hidden after `duration` seconds (or kept until `stop` when the duration is -1, live-updating its inputs). Stacking text is a block on the middle left, newest at the bottom, five entries deep. Everything clears on level change. `DebugPositionMarker` draws XYZ axes at its `world_pos` while started, and `DebugEnvironmentMarker` draws its `text` there in its `colour` and `size`.

- **Zone loading** - the game only streams in the zones around the player and whatever the camera's view ray lands on, so a free camera (e.g. Cinematic Tools) can fly into areas that never load. With `LoadAllZones` on, every zone of the level is registered as viewed each frame and streams in. The ASI also exports `OpenCAGE_SetForceZoneLoading(bool)` so other injected tools can switch this on while their camera is active; Cinematic Tools does so. The live link's camera sync switches it on too while OpenCAGE drives the camera. Each of these only turns its own request on and off, and every zone streams in while any of them wants it (so Cinematic Tools turning its camera off leaves `LoadAllZones` on).

- **Live link** - OpenCAGE edits the scripting of the level the game is running, without a reload: entities added, removed, re-parameterised or moved in the editor change in the running game, and the entity inspector can call an entity's methods (`start`, `trigger`, ...) in it. The game camera can also follow OpenCAGE's 3D viewport camera, or the viewport the game camera. It serves a WebSocket on `127.0.0.1:8765` (local connections only) that OpenCAGE connects to (launch the game from OpenCAGE with Enable Live Link ticked; OpenCAGE connects by itself, and its Live Link button next to Launch Game turns it off and on). See [Live link](#live-link) below.

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
LiveLink=1                  ; 1/0 - let OpenCAGE edit the running level's scripting and call entity methods
LiveLinkPort=8765           ; the local port OpenCAGE connects to
LiveLinkCamera=1            ; 1/0 - let OpenCAGE's viewport camera drive the game camera when it asks (camera sync)
```

`OpenCAGE_Utils.log` is written next to `AI.exe` each run: what the live link did, and every `DebugText` / `DebugTextStacking` that started, with its text.

## Live link

The game keeps a level's `COMMANDS.PAK` in memory exactly as it is on disk (the entity manager reads the whole file into one block), and a composite template *is* the file's composite record, its arrays being offsets from the start of that block. So OpenCAGE sends a composite as a one-composite PAK image (written by CathodeLib), with the positions of the offset words in it; the ASI copies it into memory it never frees, rebases those offsets onto the block (32-bit wrap-around reaches anywhere), turns the parameters into the game's variable objects as the game does when it loads the file, and points the template at the new arrays. It then brings every running instance of the composite in line with the change, working out the difference against what the game is running:

- entities (and proxies) that went are reverted and shut down, taken out of their zones and removed from the instance;
- new ones are constructed as entities or proxies by the entity manager (which builds a whole instance for a composite), added, initialised and validated, and put in the instance's zones - as the game does when it creates and initialises an instance's entities;
- entities whose parameters changed - directly, or through an alias (an override of something inside a nested instance) - have their parameter cache flushed and are live edited (put back in their initial state, repositioned, and left to react to the edit themselves), which is what CA's own live link did in the 2014 development build;
- every entity's cached parameters and links are flushed, so link changes are picked up.

Method calls are queued as triggers the way the game's own code queues them, so they run on the next frame like a link firing. Everything that touches entities runs on the entity thread, in a hook on the entity manager's per-frame processing. A composite that is not in the running level (a new one), and new models or other resources, need the level saved and loaded again.

Because a live edit puts an entity back in its initial state before the entity reacts to the edit, something that was started (a marker, a `Thinker`, a looping effect) stops when it is edited, unless it starts on reset: start it again.

Edits and method calls are held while the level's scripts are not running - the entity manager's script transport reads paused (1) from a level's load through its intro and opening cutscene (the game queues its own triggers then) and pause menu (2) while the game is paused, rather than running (5) - and carried out once they have run again for 5 s (a level start can let them run for a moment between its pauses); after 20 s of waiting they are turned away ("still starting" / "paused") and not carried out. OpenCAGE sends its own edits again by itself, and a method call from OpenCAGE waits for the edits made before it; anything else has to be sent again once `STATUS` says `playing=1`. Edits and calls for a level the game is not running are turned away at once. Each change of the transport is written to the log. Edits and calls pushed while a level was starting were found to be able to leave the game on a black loading screen for good. `STATUS` reports `playing` (edits are taken now), `loading`, and the game camera's position and facing.

**Camera sync.** With LiveLink Camera (above OpenCAGE's 3D viewport) on "Sync viewport camera to game", OpenCAGE sends its viewport camera (`CAMERA`: position, forward, up and vertical field of view, in the game's world space) and the game renders from it. The ASI hooks the game's hand-over of the camera it picked to the renderer and the sound listener: it writes OpenCAGE's pose into that camera's state, lets the hand-over run, and puts the camera's own state back straight after - so the rendered frame (and what is heard) moves while the camera behaviours keep the game's camera. What the hand-over passes on stays OpenCAGE's until the next frame, though, so game logic that reads the rendered view follows the viewport too: camera viewcone triggers, the current camera FOV script value, the alien's checks for being in the player's view and camera-direction AI conditions. It works in gameplay, cutscenes, the pause menu and while a level starts: `CAMERA` is stored and answered at once, never held like edits. The pose applies while the OpenCAGE connection that sent it lasts and the level it was sent for is running - disconnecting gives the game its camera back; while no level or another level is running it waits, and applies again when its level is back - and a new pose is turned away for another level, when no level is running, or with `LiveLinkCamera=0` (OpenCAGE sends its latest pose again every 2 s while the sync is on, so the camera picks up once the level is there). Every zone of the level streams in while it applies (see Zone loading), as the game would otherwise only stream the zones around the player. The overlay shows "Camera: following OpenCAGE", the log each change, and `STATUS` `camera_sync=1`. Cinematic Tools hooks the same hand-over (after this ASI does): the two chain, and while OpenCAGE drives the camera its pose wins (Cinematic Tools' camera shows again once OpenCAGE lets go).

On "Sync game camera to viewport" it goes the other way: OpenCAGE asks for the game's camera (`CAMERA_GET`) and puts its viewport camera there - position, facing and vertical field of view - taking no camera control of its own meanwhile. The same hook keeps the state of the camera it hands on every frame, before any pose of OpenCAGE's is written over it, so the answer is always the game's own camera as of its last frame (Cinematic Tools' while its camera is on): position, unit forward and up, field of view and a frame counter. It is answered at once, and turned away when no level or another level is running, with `LiveLinkCamera=0`, and while no frame of the running level has been drawn from a camera in the last second (a level loading).

Messages are binary, little-endian: `u32 'OCLL'`, `u16` version (1), `u16` command, `u32` request id, then the command's data (see `LIVE_LINK_SERVER.h`). Each is answered with the same header (command | 0x8000), `u8` success, a `u32`-length message, and any data. Commands: `STATUS`, `CALL_METHOD`, `APPLY_COMPOSITE`, `LOAD_LEVEL`, `SCREENSHOT` (the back buffer to a .bmp), `DESCRIBE` (a composite's running instances), `CAMERA` (a pose for the game camera to render from, or to let go of it) and `CAMERA_GET` (the game camera's own pose, as of its last frame). The older JSON text message `{"version":1,"load_level":"..."}` still loads a level.

## Building

Build `OpenCAGE_Utils.vcxproj` as Release|Win32. When building from the command line pass the project directory as the solution directory, e.g. `msbuild OpenCAGE_Utils.vcxproj /p:Configuration=Release /p:Platform=Win32 /p:SolutionDir=<path to this folder>\`. The output is `build/OpenCAGE_Utils.asi`, loaded by the bundled `winmm.dll` ASI loader (which OpenCAGE copies into the game folder as `d3d11.dll`).

The game offsets used by the hooks are for the Steam retail build of `AI.exe`.
