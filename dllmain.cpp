// dllmain.cpp - CS2 Internal Cheat Entry Point
#include "pch.h"

// Forward declarations
namespace Hooks {
    void Initialize();
    void Shutdown();
}

// Global module handle
HMODULE g_hModule = nullptr;

// Main cheat thread
DWORD WINAPI MainThread(LPVOID lpParam) {
    // Allocate console for debugging
    AllocConsole();
    FILE* fDummy;
    freopen_s(&fDummy, "CONOUT$", "w", stdout);
    freopen_s(&fDummy, "CONOUT$", "w", stderr);
    freopen_s(&fDummy, "CONIN$", "r", stdin);
    
    printf("[+] CS2 Cheat Loaded!\n");
    printf("[+] Waiting for game modules...\n");
    
    // Wait for game to fully load
    while (!GetModuleHandleA("client.dll") || !GetModuleHandleA("engine2.dll")) {
        Sleep(100);
    }
    printf("[+] client.dll: 0x%p\n", GetModuleHandleA("client.dll"));
    printf("[+] engine2.dll: 0x%p\n", GetModuleHandleA("engine2.dll"));
    
    Sleep(1000); // Extra delay for stability
    
    printf("[+] Initializing hooks...\n");
    
    // Initialize hooks
    Hooks::Initialize();
    
    printf("[+] Hooks initialized!\n");
    printf("[+] Press INSERT to toggle menu\n");
    printf("[+] Press END to unload\n");
    
    // Main loop - wait for unload key
    while (true) {
        if (GetAsyncKeyState(VK_END) & 1) {
            break;
        }
        Sleep(100);
    }
    
    printf("[+] Unloading...\n");
    
    // Cleanup
    Hooks::Shutdown();
    
    // Close console
    FreeConsole();
    
    // Free library and exit thread
    FreeLibraryAndExitThread(g_hModule, 0);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        g_hModule = hModule;
        
        HANDLE hThread = CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
        if (hThread) {
            CloseHandle(hThread);
        }
    }
    return TRUE;
}
