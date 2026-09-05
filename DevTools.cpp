#include "DevTools.h"
#include <cstdint>
#include <stdio.h>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

using namespace DevTools;

uintptr_t m_game_imageBaseAddress = NULL;

uintptr_t GameProcess::GetBaseAddress()
{
	if (m_game_imageBaseAddress == NULL)
	{
		m_game_imageBaseAddress = reinterpret_cast<uintptr_t>(GetModuleHandle(NULL));
	}

	return m_game_imageBaseAddress;
}

bool DevTools::EnableEntity(uintptr_t slotOffset)
{
	const uintptr_t stub = DEVTOOLS_RELATIVE_ADDRESS(0x00599180);
	const uintptr_t real = DEVTOOLS_RELATIVE_ADDRESS(0x004c3010);
	uintptr_t* slot = reinterpret_cast<uintptr_t*>(DEVTOOLS_RELATIVE_ADDRESS(slotOffset));

	if (*slot == real)
		return true;
	if (*slot != stub)
		return false;

	DWORD oldProtect;
	VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &oldProtect);
	*slot = real;
	VirtualProtect(slot, sizeof(*slot), oldProtect, &oldProtect);
	return true;
}
