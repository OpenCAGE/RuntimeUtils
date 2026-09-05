#include "ZONE_LOADER.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <atomic>

namespace
{
	std::atomic<bool> g_forced = false;
}

void ZONE_LOADER::SetForced(bool forced)
{
	g_forced = forced;
}

bool ZONE_LOADER::IsForced()
{
	return g_forced;
}

__declspec(noinline)
void __cdecl ZONE_LOADER::h_match_current_zones_to_povs()
{
	match_current_zones_to_povs();

	if (!g_forced)
		return;

	char* zoneManager = static_cast<char*>(*zone_manager_instance);
	if (!zoneManager)
		return;
	char* zones = *reinterpret_cast<char**>(zoneManager + kZoneArrayOffset);
	if (!zones)
		return;

	const int count = *reinterpret_cast<int*>(zones + kArrayCountOffset);
	char** allocations = *reinterpret_cast<char***>(zones + kArrayDataOffset);
	if (!allocations)
		return;

	for (int i = 0; i < count; i++)
	{
		char* allocation = allocations[i];
		if (!allocation)
			continue;
		char* zone = *reinterpret_cast<char**>(allocation + kAllocationObjectOffset);
		if (!zone)
			continue;
		add_viewpoint(zoneManager, kPlayerViewer, *reinterpret_cast<unsigned int*>(zone + kZoneIdOffset));
	}
}

// For other tools injected into the game (Cinematic Tools toggles this with its free camera).
extern "C" __declspec(dllexport) void OpenCAGE_SetForceZoneLoading(bool forced)
{
	ZONE_LOADER::SetForced(forced);
}
