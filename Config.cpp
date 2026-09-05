#include "Config.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>

namespace
{
	struct NamedKey { const char* name; int virtualKey; };
	const NamedKey kNamedKeys[] = {
		{ "INSERT", VK_INSERT }, { "DELETE", VK_DELETE }, { "HOME", VK_HOME }, { "END", VK_END },
		{ "PAGEUP", VK_PRIOR }, { "PAGEDOWN", VK_NEXT }, { "PAUSE", VK_PAUSE }, { "SCROLLLOCK", VK_SCROLL },
		{ "BACKSPACE", VK_BACK }, { "TAB", VK_TAB }, { "SPACE", VK_SPACE },
		{ "F1", VK_F1 }, { "F2", VK_F2 }, { "F3", VK_F3 }, { "F4", VK_F4 }, { "F5", VK_F5 }, { "F6", VK_F6 },
		{ "F7", VK_F7 }, { "F8", VK_F8 }, { "F9", VK_F9 }, { "F10", VK_F10 }, { "F11", VK_F11 }, { "F12", VK_F12 },
		{ "NUMPAD0", VK_NUMPAD0 }, { "NUMPAD1", VK_NUMPAD1 }, { "NUMPAD2", VK_NUMPAD2 }, { "NUMPAD3", VK_NUMPAD3 },
		{ "NUMPAD4", VK_NUMPAD4 }, { "NUMPAD5", VK_NUMPAD5 }, { "NUMPAD6", VK_NUMPAD6 }, { "NUMPAD7", VK_NUMPAD7 },
		{ "NUMPAD8", VK_NUMPAD8 }, { "NUMPAD9", VK_NUMPAD9 },
	};

	// Accepts a key name from the table, a single letter/digit, or a virtual-key code (decimal or 0x-prefixed hex).
	int ParseKey(const std::string& text, int fallback)
	{
		std::string value;
		for (char c : text)
			if (!isspace(static_cast<unsigned char>(c)))
				value += static_cast<char>(toupper(static_cast<unsigned char>(c)));
		if (value.empty())
			return fallback;

		for (const NamedKey& key : kNamedKeys)
			if (value == key.name)
				return key.virtualKey;

		if (value.size() == 1 && ((value[0] >= 'A' && value[0] <= 'Z') || (value[0] >= '0' && value[0] <= '9')))
			return value[0]; // Virtual-key codes for letters and digits match their ASCII values.

		char* end = nullptr;
		const long code = strtol(value.c_str(), &end, 0);
		if (end && *end == '\0' && code > 0 && code < 256)
			return static_cast<int>(code);

		return fallback;
	}

	std::string IniPath()
	{
		// Next to the game executable, which is also where the launcher drops the ASI.
		char exePath[MAX_PATH] = {};
		GetModuleFileNameA(nullptr, exePath, MAX_PATH);
		std::string path = exePath;
		const size_t slash = path.find_last_of("\\/");
		path = (slash == std::string::npos) ? "" : path.substr(0, slash + 1);
		return path + "OpenCAGE_Utils.ini";
	}

	Config::Settings Load()
	{
		Config::Settings settings;
		const std::string ini = IniPath();
		const char* section = "RuntimeUtils";

		settings.hotReload = GetPrivateProfileIntA(section, "HotReload", settings.hotReload ? 1 : 0, ini.c_str()) != 0;
		settings.debugText = GetPrivateProfileIntA(section, "DebugText", settings.debugText ? 1 : 0, ini.c_str()) != 0;

		char key[64] = {};
		GetPrivateProfileStringA(section, "HotReloadKey", "", key, sizeof(key), ini.c_str());
		settings.hotReloadKey = ParseKey(key, settings.hotReloadKey);

		return settings;
	}
}

const Config::Settings& Config::Get()
{
	static const Settings settings = Load();
	return settings;
}

const char* Config::KeyName(int virtualKey)
{
	for (const NamedKey& key : kNamedKeys)
		if (key.virtualKey == virtualKey)
			return key.name;

	static char buffer[8];
	if ((virtualKey >= 'A' && virtualKey <= 'Z') || (virtualKey >= '0' && virtualKey <= '9'))
	{
		buffer[0] = static_cast<char>(virtualKey);
		buffer[1] = '\0';
	}
	else
	{
		snprintf(buffer, sizeof(buffer), "0x%02X", virtualKey);
	}
	return buffer;
}
