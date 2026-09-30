#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct IDXGISwapChain;

/*
	The live link connection: a WebSocket server on 127.0.0.1 (port 8765 by default) that OpenCAGE connects to.

	Binary messages carry requests, little-endian:
	  u32 magic 'OCLL', u16 version (1), u16 command, u32 request id, then the command's payload.
	Every request is answered with the same header (command | 0x8000), then u8 ok, u32 length + UTF-8 message, then any
	reply payload. Text messages are the older JSON packets ({"version":1,"load_level":"..."}), which get no reply.
	root is the root composite of the level OpenCAGE has open: a request for another level is refused on the entity
	thread. Connections that carry an Origin header (web pages) are refused, and one client is served at a time (another
	is turned away at once).

	Requests on entities are carried out on the entity thread (from the hook on the entity manager's per-frame
	processing), screenshots and level loads on the render thread (the hook where D3D11 shows each finished frame), so the
	socket thread never touches the game. CAMERA and CAMERA_GET are the exceptions: the socket thread only checks which
	level is running (plain reads of the entity manager) and stores the pose, or reads the camera hook's last snapshot of
	the game camera, answering at once - they are never queued behind edits held while a level starts - and the camera
	hook applies the pose (see LIVE_CAMERA.cpp).
*/
namespace LIVE_LINK_SERVER
{
	enum Command : uint16_t
	{
		STATUS = 1,          // -> message "running=0|1\nroot=..\nroot_name=..\ntemplates=..\nstate=..\nplaying=0|1\nloading=0|1[\nloading_reason=..]\ncamera=x,y,z\ncamera_forward=..\ncamera_up=..\ncamera_sync=0|1"
		                     //    (CALL_METHOD and APPLY_COMPOSITE are held while loading=1 - scripts paused, or running again for under 5 s:
		                     //    level starting, pause menu - and turned away after 20 s; camera_sync=1 while the game renders from CAMERA's pose)
		CALL_METHOD = 2,     // u32 root (0: any), u32 composite, u32 entity, u32 method, u32 path count, u32 path[count] (instance entity ids from the root)
		APPLY_COMPOSITE = 3, // u32 root (0: any), u32 composite, u32 image size, image, u32 relocation count, u32 relocations[count]
		LOAD_LEVEL = 4,      // u32 length, level name (e.g. "PRODUCTION\\BSP_TORRENS" or "BSP_TORRENS")
		SCREENSHOT = 5,      // u32 length, path of a .bmp to write (nothing else is written)
		DESCRIBE = 6,        // u32 composite -> message listing its running instances
		CAMERA = 7,          // u32 root (0: any), u8 active (1: render from this pose, 0: give the game its camera back), then only if active:
		                     //    f32 position[3], f32 forward[3], f32 up[3], f32 fov (vertical degrees; <= 0 keeps the game's own) - game world
		                     //    space (Y up, left-handed). -> "Camera following OpenCAGE" / "Camera released". The pose applies while this
		                     //    connection lasts and the level stays the one it was sent for; zones are streamed in while it does.
		CAMERA_GET = 8,      // u32 root (0: any) -> message "position=x,y,z\nforward=x,y,z\nup=x,y,z\nfov=deg\nframe=N": the game's own camera
		                     //    as of its last frame (never CAMERA's pose), in game world space - forward and up unit, fov vertical
		                     //    degrees, 4 decimals whatever the locale; frame is bumped by every frame the hook keeps. Refused with
		                     //    no level running, another level running, LiveLinkCamera=0, or no frame of the running level drawn from a
		                     //    camera in the last second ("The game has not drawn a frame from its camera yet": a level loading).
	};

	void Start(uint16_t port);
	void Stop();

	// Entity thread: carries out the queued entity requests.
	void ProcessEntityRequests();

	// Render thread, after the overlay has drawn: carries out queued screenshots and level loads.
	void ProcessRenderRequests(IDXGISwapChain* swapChain);

	// Whether OpenCAGE is connected, and a line about the last thing done (if it was recent), for the overlay.
	bool Connected();
	std::string LastActivity(unsigned int withinMs);

	// Which connection is current: bumped when OpenCAGE connects and when it goes, so something a request left behind
	// (the camera pose) can tell its connection has ended.
	uint32_t CurrentConnection();

	// Sets the overlay's activity line (for things done outside the server, e.g. the camera hook).
	void ShowActivity(const std::string& text);
}
