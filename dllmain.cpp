#include "DevTools.h"

#include "Config.h"
#include "Menu.h"

// Game-specific code classes.
#include "GAME_LEVEL_MANAGER.h"
#include "GameFlow.h"
#include "DEBUG_TEXT.h"
#include "DEBUG_MARKER.h"
#include "ZONE_LOADER.h"

// External includes.
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

// Menu - D3D11 CreateDeviceAndSwapChain hook.
tD3D11CreateDeviceAndSwapChain d3d11CreateDeviceAndSwapChain = nullptr;

typedef HRESULT(WINAPI* tD3D11Present)(
    IDXGISwapChain* swapChain,
    UINT            SyncInterval,
    UINT            Flags
);

// Menu - D3D11 Present hook.
tD3D11Present d3d11Present = nullptr;

// This will work for now, but I need to write a replacement that will restore the original call bytes, we just overwrite them.
void hookFunctionCall(int offset, void* replacementFunction)
{
	const SIZE_T patchSize = 5;
	DWORD oldProtect;
	char* patchLocation = reinterpret_cast<char*>(offset);

	// Change the memory page protection.
	VirtualProtect(patchLocation, patchSize, PAGE_EXECUTE_READWRITE, &oldProtect);
	//to fill out the last 4 bytes of instruction, we need the offset between
	//the payload function and the instruction immediately AFTER the call instruction

	//32 bit relative call opcode is E8, takes 1 32 bit operand for call offset
    uint8_t instruction[patchSize] = {0xE8, 0x0, 0x0, 0x0, 0x0};
	const uint32_t relativeAddress = reinterpret_cast<uint32_t>(replacementFunction) - (reinterpret_cast<uint32_t>(patchLocation) + sizeof(instruction));

	// Copy the remaining bytes of the instruction (after the opcode).
	memcpy_s(instruction + 1, patchSize - 1, &relativeAddress, patchSize - 1);

	// Install the hook.
	memcpy_s(patchLocation, patchSize, instruction, sizeof(instruction));

    // Restore original memory page protections.
	VirtualProtect(patchLocation, patchSize, oldProtect, &oldProtect);
}

HRESULT WINAPI hD3D11Present(
    IDXGISwapChain* swapChain,
    UINT        SyncInterval,
    UINT        Flags
) {
    Menu::DrawMenu();

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

    // If the Menu class hasn't already been initialised, initialise it now.
    if (!Menu::IsInitialised())
    {
        Menu::InitMenu(*ppSwapChain);

        if (*ppSwapChain)
        {
            DetourTransactionBegin();
            DetourUpdateThread(GetCurrentThread());

            void** pVMTPresent = *reinterpret_cast<void***>(*ppSwapChain);

        	// Store reference to the original D3D11Present function from the SwapChain VTable.
            d3d11Present = static_cast<tD3D11Present>(pVMTPresent[8]);

            DEVTOOLS_DETOURS_ATTACH(d3d11Present, hD3D11Present);

            const auto result = DetourTransactionCommit();
		}
    }

    return res;
}

// Attaches or detaches every hook, according to which features are enabled in the config.
// Detours rejects detaching something that was never attached, so both directions must agree.
static void AttachHooks(bool attach)
{
    const Config::Settings& config = Config::Get();

    auto hook = [attach](auto& original, auto detoured)
    {
        if (attach) DEVTOOLS_DETOURS_ATTACH(original, detoured);
        else DEVTOOLS_DETOURS_DETACH(original, detoured);
    };

    // Rendering hooks: needed for the hot reload key (window procedure) and the debug text overlay.
    if ((config.hotReload || Config::AnyDebug()) && d3d11CreateDeviceAndSwapChain)
    {
        hook(d3d11CreateDeviceAndSwapChain, hD3D11CreateDeviceAndSwapChain);
        if (!attach && d3d11Present)
            hook(d3d11Present, hD3D11Present);
    }

    // GAME_LEVEL_MANAGER hooks: capture the level manager so the hot reload key can restart the current level.
    if (config.hotReload)
    {
        hook(GAME_LEVEL_MANAGER::get_level_from_name, GAME_LEVEL_MANAGER::h_get_level_from_name);
        hook(GAME_LEVEL_MANAGER::queue_level, GAME_LEVEL_MANAGER::h_queue_level);
        hook(GAME_LEVEL_MANAGER::request_next_level, GAME_LEVEL_MANAGER::h_request_next_level);
    }

    // GameFlow hooks.
    hook(GameFlow::start_gameplay, GameFlow::h_start_gameplay);

    // Zone streaming: always hooked, so Cinematic Tools can switch forced loading on and off while running.
    hook(ZONE_LOADER::match_current_zones_to_povs, ZONE_LOADER::h_match_current_zones_to_povs);

    // DEBUG_TEXT / DEBUG_TEXT_STACKING hooks.
    if (Config::AnyDebug())
        hook(DEBUG_TEXT::level_close, DEBUG_TEXT::h_level_close);

    if (config.debugText)
    {
        hook(DEBUG_TEXT::destructor, DEBUG_TEXT::h_destructor);
        hook(DEBUG_TEXT::on_start, DEBUG_TEXT::h_on_start);
        hook(DEBUG_TEXT::on_update, DEBUG_TEXT::h_on_update);
        hook(DEBUG_TEXT::on_clear_of_alignment, DEBUG_TEXT::h_on_clear_of_alignment);
        hook(DEBUG_TEXT::on_stop, DEBUG_TEXT::h_on_stop);
        hook(DEBUG_TEXT::on_clear_all, DEBUG_TEXT::h_on_clear_all);
    }

    if (config.debugTextStacking)
    {
        hook(DEBUG_TEXT::stacking_destructor, DEBUG_TEXT::h_stacking_destructor);
        hook(DEBUG_TEXT::stacking_on_start, DEBUG_TEXT::h_stacking_on_start);
        hook(DEBUG_TEXT::stacking_on_clear_all, DEBUG_TEXT::h_stacking_on_clear_all);
        hook(DEBUG_TEXT::stacking_on_clear_last, DEBUG_TEXT::h_stacking_on_clear_last);
    }

    if (config.debugEnvironmentMarker)
        hook(DEBUG_MARKER::environment_on_update, DEBUG_MARKER::h_environment_on_update);

    if (config.debugPositionMarker)
        hook(DEBUG_MARKER::position_on_update, DEBUG_MARKER::h_position_on_update);
}

BOOL APIENTRY DllMain( HMODULE /*hModule*/,
                       DWORD  ul_reason_for_call,
                       LPVOID /*lpReserved*/
                     )
{
    if (DetourIsHelperProcess())
    {
        return TRUE;
    }

    if (ul_reason_for_call == DLL_PROCESS_ATTACH)
    {
        const Config::Settings& config = Config::Get();
        ZONE_LOADER::SetForced(config.loadAllZones);

        // Re-enable the DebugText / DebugTextStacking script entities, which retail builds disable.
        if (Config::AnyDebug())
        {
            if (!DEBUG_TEXT::EnableEntities(config.debugText, config.debugTextStacking) || !DEBUG_MARKER::EnableEntities(config.debugEnvironmentMarker))
            {
                MessageBox(NULL, L"Warning - could not enable the DebugText entities: unexpected AI.exe build?", L"AlienIsolation.DevTools", MB_ICONWARNING);
            }
        }

        DetourRestoreAfterWith();
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());

    	// Menu hooks / initialisation code, adapted from Alias Isolation.
        if (config.hotReload || Config::AnyDebug())
        {
            const HMODULE hModule = GetModuleHandle(L"d3d11");

            if (hModule)
            {
                d3d11CreateDeviceAndSwapChain = reinterpret_cast<tD3D11CreateDeviceAndSwapChain>(GetProcAddress(hModule, "D3D11CreateDeviceAndSwapChain"));

                if (!d3d11CreateDeviceAndSwapChain)
                {
                    MessageBox(NULL, L"Fatal Error - GetProcAddress(\"D3D11CreateDeviceAndSwapChain\") failed!", L"AlienIsolation.DevTools", MB_ICONERROR);
                }
            }
            else
            {
                MessageBox(NULL, L"Fatal Error - GetModuleHandle(\"d3d11\") failed: MODULE_NOT_FOUND!", L"AlienIsolation.DevTools", MB_ICONERROR);
            }
        }

        AttachHooks(true);

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
    }
    else if (ul_reason_for_call == DLL_PROCESS_DETACH)
    {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());

        AttachHooks(false);

        DetourTransactionCommit();
    }

    return TRUE;
}
