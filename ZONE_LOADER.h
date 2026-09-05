#pragma once

#include "DevTools.h"
#include <cstdint>

namespace ZONE_LOADER
{
	inline void** zone_manager_instance = reinterpret_cast<void**>(DEVTOOLS_RELATIVE_ADDRESS(0x0134f860));

	typedef void(__thiscall* t_add_viewpoint)(void*, unsigned int, unsigned int);
	inline auto add_viewpoint = reinterpret_cast<t_add_viewpoint>(DEVTOOLS_RELATIVE_ADDRESS(0x005456c0));

	typedef void(__cdecl* t_match_current_zones_to_povs)();
	inline auto match_current_zones_to_povs = reinterpret_cast<t_match_current_zones_to_povs>(DEVTOOLS_RELATIVE_ADDRESS(0x0034c870));
	void __cdecl h_match_current_zones_to_povs();

	constexpr uintptr_t kZoneArrayOffset = 0x18;  
	constexpr uintptr_t kArrayCountOffset = 0x8;
	constexpr uintptr_t kArrayDataOffset = 0xc;
	constexpr uintptr_t kAllocationObjectOffset = 0xc; 
	constexpr uintptr_t kZoneIdOffset = 0xc; 
	constexpr unsigned int kPlayerViewer = 0;

	void SetForced(bool forced);
	bool IsForced();
}
