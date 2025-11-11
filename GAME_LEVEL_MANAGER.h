#pragma once

#include "DevTools.h"

namespace GAME_LEVEL_MANAGER
{
	inline void* m_instance = nullptr;

	int __fastcall h_get_level_from_name(void* _this, void* _EDX, char* level_name);
	typedef int(__thiscall* t_get_level_from_name)(void*, char*);
	inline auto get_level_from_name = reinterpret_cast<t_get_level_from_name>(DEVTOOLS_RELATIVE_ADDRESS(0x0077b340 - 0x00400000));

	int __fastcall h_get_current_level(void* _this, void* _EDX);
	typedef int(__thiscall* t_get_current_level)(void*);
	inline auto get_current_level = reinterpret_cast<t_get_current_level>(DEVTOOLS_RELATIVE_ADDRESS(0x00fc2ab0 - 0x00400000));

	void __fastcall h_queue_level(void* _this, void* _EDX, int level);
	typedef void(__thiscall* t_queue_level)(void*, int);
	inline auto queue_level = reinterpret_cast<t_queue_level>(DEVTOOLS_RELATIVE_ADDRESS(0x0077b320 - 0x00400000));

	void __fastcall h_request_next_level(void* _this, void* _EDX, bool is_part_of_playlist);
	typedef void(__thiscall* t_request_next_level)(void*, bool);
	inline auto request_next_level = reinterpret_cast<t_request_next_level>(DEVTOOLS_RELATIVE_ADDRESS(0x0079a650 - 0x00400000));

	int __fastcall h_get_level_or_make_new(void* _this, void* _EDX, const char* level_name);
	typedef int(__thiscall* t_get_level_or_make_new)(void*, const char*);
	inline auto get_level_or_make_new = reinterpret_cast<t_get_level_or_make_new>(DEVTOOLS_RELATIVE_ADDRESS(0x007a86c0 - 0x00400000));
}
