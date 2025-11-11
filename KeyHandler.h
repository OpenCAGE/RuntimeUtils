#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <d3d11.h>
#include <string>

class KeyHandler
{
public:
    static LRESULT CALLBACK WndProcHandler(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    static void InitKeyHandler(IDXGISwapChain* pSwapChain);
    static bool IsInitialised();
};

