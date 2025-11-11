#include "KeyHandler.h"
#include "GAME_LEVEL_MANAGER.h"

#include <d3d11.h>

static IDXGISwapChain* g_swapChain = nullptr;
static HWND g_hWindow = nullptr;
static WNDPROC g_originalWndProcHandler = nullptr;
static bool g_keyHandlerInitialised = false;

LRESULT CALLBACK KeyHandler::WndProcHandler(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) 
{
    if (uMsg == WM_KEYUP) 
    {
        if (wParam == VK_INSERT) 
        {
            int level = GAME_LEVEL_MANAGER::get_current_level(GAME_LEVEL_MANAGER::m_instance);
            GAME_LEVEL_MANAGER::queue_level(GAME_LEVEL_MANAGER::m_instance, level);
            GAME_LEVEL_MANAGER::request_next_level(GAME_LEVEL_MANAGER::m_instance, true);
        }
    }

    return CallWindowProc(g_originalWndProcHandler, hWnd, uMsg, wParam, lParam);
}

void KeyHandler::InitKeyHandler(IDXGISwapChain* pSwapChain)
{
    if (g_keyHandlerInitialised) 
    {
        return;
    }

    g_swapChain = pSwapChain;

    DXGI_SWAP_CHAIN_DESC dxgiSwapChainDesc;
    if (FAILED(g_swapChain->GetDesc(&dxgiSwapChainDesc)))
    {
        return;
    }

    g_hWindow = dxgiSwapChainDesc.OutputWindow;
    g_originalWndProcHandler = reinterpret_cast<WNDPROC>(SetWindowLongPtr(g_hWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WndProcHandler)));

    g_keyHandlerInitialised = true;
}

bool KeyHandler::IsInitialised()
{
    return g_keyHandlerInitialised && g_swapChain != nullptr;
}
