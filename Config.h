#pragma once

/*
	Runtime settings, read once from OpenCAGE_Utils.ini in the game folder (next to AI.exe). Every key is
	optional; missing keys keep their defaults. Example:

	  [RuntimeUtils]
	  HotReload=1          ; 1/0 - reload the current level on a key press
	  HotReloadKey=INSERT  ; a key name (INSERT, DELETE, HOME, END, PAGEUP, PAGEDOWN, F1..F12, A..Z, 0..9)
	                       ; or a Windows virtual-key code, decimal or hex (e.g. 0x2D)
	  DebugText=1          ; 1/0 - draw DebugText / DebugTextStacking script entities on screen
*/
namespace Config
{
	struct Settings
	{
		bool hotReload = true;
		int hotReloadKey = 0x2D; // VK_INSERT
		bool debugText = true;
	};

	// Loads the settings on first use.
	const Settings& Get();

	// Human-readable name of a virtual-key code, for logging.
	const char* KeyName(int virtualKey);
}
