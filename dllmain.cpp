#include "DevTools.h"

#include "KeyHandler.h"
#include "WebSocketHandler.h"

#include "GAME_LEVEL_MANAGER.h"

#include <detours.h>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

typedef HRESULT(WINAPI* tD3D11CreateDeviceAndSwapChain)(
    void* pAdapter,
    D3D_DRIVER_TYPE      DriverType,
    HMODULE              Software,
    UINT                 Flags,
    const void* pFeatureLevels,
    UINT                 FeatureLevels,
    UINT                 SDKVersion,
    const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
    IDXGISwapChain** ppSwapChain,
    ID3D11Device** ppDevice,
    void* pFeatureLevel,
    ID3D11DeviceContext** ppImmediateContext
);

tD3D11CreateDeviceAndSwapChain d3d11CreateDeviceAndSwapChain = nullptr;

typedef HRESULT(WINAPI* tD3D11Present)(IDXGISwapChain* swapChain, UINT SyncInterval, UINT Flags);

tD3D11Present d3d11Present = nullptr;

void hookFunctionCall(int offset, void* replacementFunction)
{
	const SIZE_T patchSize = 5;
	DWORD oldProtect;
	char* patchLocation = reinterpret_cast<char*>(offset);

	VirtualProtect(patchLocation, patchSize, PAGE_EXECUTE_READWRITE, &oldProtect);

    uint8_t instruction[patchSize] = {0xE8, 0x0, 0x0, 0x0, 0x0};
	const uint32_t relativeAddress = reinterpret_cast<uint32_t>(replacementFunction) - (reinterpret_cast<uint32_t>(patchLocation) + sizeof(instruction));

	memcpy_s(instruction + 1, patchSize - 1, &relativeAddress, patchSize - 1);
	memcpy_s(patchLocation, patchSize, instruction, sizeof(instruction));

	VirtualProtect(patchLocation, patchSize, oldProtect, &oldProtect);
}

HRESULT WINAPI hD3D11Present(
    IDXGISwapChain* swapChain,
    UINT        SyncInterval,
    UINT        Flags
) {
    return d3d11Present(swapChain, SyncInterval, Flags);
}

HRESULT WINAPI hD3D11CreateDeviceAndSwapChain(
    void* pAdapter,
    D3D_DRIVER_TYPE      DriverType,
    HMODULE              Software,
    UINT                 Flags,
    const void* pFeatureLevels,
    UINT                 FeatureLevels,
    UINT                 SDKVersion,
    const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
    IDXGISwapChain** ppSwapChain,
    ID3D11Device** ppDevice,
    void* pFeatureLevel,
    ID3D11DeviceContext** ppImmediateContext
) {
    HRESULT res = d3d11CreateDeviceAndSwapChain(
        pAdapter,
        DriverType,
        Software,
        Flags,
        pFeatureLevels,
        FeatureLevels,
        SDKVersion,
        pSwapChainDesc,
        ppSwapChain,
        ppDevice,
        pFeatureLevel,
        ppImmediateContext
    );
	
    if (!KeyHandler::IsInitialised())
    {
        KeyHandler::InitKeyHandler(*ppSwapChain);

        if (*ppSwapChain)
        {
            DetourTransactionBegin();
            DetourUpdateThread(GetCurrentThread());

            void** pVMTPresent = *reinterpret_cast<void***>(*ppSwapChain);
            d3d11Present = static_cast<tD3D11Present>(pVMTPresent[8]);

            DEVTOOLS_DETOURS_ATTACH(d3d11Present, hD3D11Present);

            const auto result = DetourTransactionCommit();
		}
    }
    return res;
}

BOOL APIENTRY DllMain(HMODULE /*hModule*/, DWORD  ul_reason_for_call, LPVOID /*lpReserved*/)
{
    if (DetourIsHelperProcess())
    {
        return TRUE;
    }

    if (ul_reason_for_call == DLL_PROCESS_ATTACH)
    {
        DetourRestoreAfterWith();
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());

        const HMODULE hModule = GetModuleHandle(L"d3d11");
    	
        if (hModule)
        {
            d3d11CreateDeviceAndSwapChain = reinterpret_cast<tD3D11CreateDeviceAndSwapChain>(GetProcAddress(hModule, "D3D11CreateDeviceAndSwapChain"));
        	
            if (d3d11CreateDeviceAndSwapChain)
            {
                DEVTOOLS_DETOURS_ATTACH(d3d11CreateDeviceAndSwapChain, hD3D11CreateDeviceAndSwapChain);
            }
            else
            {
                MessageBox(NULL, L"Fatal Error - GetProcAddress(\"D3D11CreateDeviceAndSwapChain\") failed!", L"AlienIsolation.DevTools", MB_ICONERROR);
            }
        }
        else
        {
            MessageBox(NULL, L"Fatal Error - GetModuleHandle(\"d3d11\") failed: MODULE_NOT_FOUND!", L"AlienIsolation.DevTools", MB_ICONERROR);
        }

        DEVTOOLS_DETOURS_ATTACH(GAME_LEVEL_MANAGER::get_level_from_name, GAME_LEVEL_MANAGER::h_get_level_from_name);
        DEVTOOLS_DETOURS_ATTACH(GAME_LEVEL_MANAGER::queue_level, GAME_LEVEL_MANAGER::h_queue_level);
        DEVTOOLS_DETOURS_ATTACH(GAME_LEVEL_MANAGER::request_next_level, GAME_LEVEL_MANAGER::h_request_next_level);
        DEVTOOLS_DETOURS_ATTACH(GAME_LEVEL_MANAGER::get_level_or_make_new, GAME_LEVEL_MANAGER::h_get_level_or_make_new);

        const long result = DetourTransactionCommit();
        if (result != NO_ERROR)
        {
            switch (result)
            {
                case ERROR_INVALID_BLOCK:
                    MessageBox(NULL, L"Fatal Error - The function referenced is too small to be detoured", L"AlienIsolation.DevTools", MB_ICONERROR);
                    break;
                case ERROR_INVALID_HANDLE:
                    MessageBox(NULL, L"Fatal Error - The ppPointer parameter is null or points to a null pointer", L"AlienIsolation.DevTools", MB_ICONERROR);
                    break;
                case ERROR_INVALID_OPERATION:
                    MessageBox(NULL, L"Fatal Error - No pending transaction exists", L"AlienIsolation.DevTools", MB_ICONERROR);
                    break;
                case ERROR_NOT_ENOUGH_MEMORY:
                    MessageBox(NULL, L"Fatal Error - Not enough memory exists to complete the operation", L"AlienIsolation.DevTools", MB_ICONERROR);
                    break;
                case ERROR_INVALID_PARAMETER:
                    MessageBox(NULL, L"Fatal Error - An invalid parameter has been passed", L"AlienIsolation.DevTools", MB_ICONERROR);
                    break;
                default:
                    MessageBox(NULL, L"Fatal Error - Unknown Detours error", L"AlienIsolation.DevTools", MB_ICONERROR);
                    break;
            }
        }

        WebSocketHandler::Initialize(8765);
    }
    else if (ul_reason_for_call == DLL_PROCESS_DETACH)
    {
        WebSocketHandler::Shutdown();

        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());

        DEVTOOLS_DETOURS_DETACH(d3d11CreateDeviceAndSwapChain, hD3D11CreateDeviceAndSwapChain);
        DEVTOOLS_DETOURS_DETACH(d3d11Present, hD3D11Present);

        DEVTOOLS_DETOURS_DETACH(GAME_LEVEL_MANAGER::get_level_from_name, GAME_LEVEL_MANAGER::h_get_level_from_name);
        DEVTOOLS_DETOURS_DETACH(GAME_LEVEL_MANAGER::queue_level, GAME_LEVEL_MANAGER::h_queue_level);
        DEVTOOLS_DETOURS_DETACH(GAME_LEVEL_MANAGER::request_next_level, GAME_LEVEL_MANAGER::h_request_next_level);
        DEVTOOLS_DETOURS_DETACH(GAME_LEVEL_MANAGER::get_level_or_make_new, GAME_LEVEL_MANAGER::h_get_level_or_make_new);
        
        DetourTransactionCommit();
    }
	
    return TRUE;
}
