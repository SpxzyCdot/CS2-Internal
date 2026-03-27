// hooks/present.cpp - IDXGISwapChain::Present Hook
#include "pch.h"
#include <vector>
#include <algorithm>
#include <cstring>
#include <map>
#include <mmsystem.h>
#include <wincodec.h>
#include <shlwapi.h>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "shlwapi.lib")

#include "imgui_edited.hpp"
#include "imgui_settings.h"
#include "font.h"
#include "texture.h"

extern HMODULE g_hModule;

// #include "imgui_edited.cpp" // Removed to prevent duplicate symbols

// Definitions for extern declarations in imgui_settings.h
namespace font
{
	ImFont* icomoon = nullptr;
	ImFont* lexend_bold = nullptr;
	ImFont* lexend_regular = nullptr;
	ImFont* lexend_general_bold = nullptr;

	ImFont* icomoon_widget = nullptr;
	ImFont* icomoon_widget2 = nullptr;
}

namespace c
{
	ImVec4 accent = ImColor(112, 110, 215);

	namespace background
	{
		ImVec4 filling = ImColor(12, 12, 12);
		ImVec4 stroke = ImColor(24, 26, 36);
		ImVec2 size = ImVec2(900, 515);

		float rounding = 6;
	}

	namespace elements
	{
		ImVec4 mark = ImColor(255, 255, 255);

		ImVec4 stroke = ImColor(28, 26, 37);
		ImVec4 background = ImColor(15, 15, 17);
		ImVec4 background_widget = ImColor(21, 23, 26);

		ImVec4 text_active = ImColor(255, 255, 255);
		ImVec4 text_hov = ImColor(81, 92, 109);
		ImVec4 text = ImColor(43, 51, 63);

		float rounding = 4;
	}

	namespace tab
	{
		ImVec4 tab_active = ImColor(22, 22, 30);
		ImVec4 border = ImColor(14, 14, 15);
	}
}

// Global textures (embedded + cs2_figure.png for ESP preview window)
ID3D11ShaderResourceView* g_pEspPreviewTexture = nullptr;
ID3D11ShaderResourceView* g_pEspPreviewFigureTexture = nullptr;
int g_espFigureTexW = 0, g_espFigureTexH = 0; // aspect ratio for preview

// Helper to load texture from memory using WIC
bool LoadTextureFromMemory(ID3D11Device* pDevice, const void* data, size_t size, ID3D11ShaderResourceView** out_srv, int* out_width, int* out_height)
{
    if (!pDevice || !data || size == 0) return false;

    // Create WIC factory
    IWICImagingFactory* pFactory = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFactory));
    if (FAILED(hr)) return false;

    // Create stream from memory
    IStream* pStream = SHCreateMemStream((const BYTE*)data, (UINT)size);
    if (!pStream) { pFactory->Release(); return false; }

    // Create decoder
    IWICBitmapDecoder* pDecoder = nullptr;
    hr = pFactory->CreateDecoderFromStream(pStream, nullptr, WICDecodeMetadataCacheOnDemand, &pDecoder);
    if (FAILED(hr)) { pStream->Release(); pFactory->Release(); return false; }

    // Get frame
    IWICBitmapFrameDecode* pFrame = nullptr;
    hr = pDecoder->GetFrame(0, &pFrame);
    if (FAILED(hr)) { pDecoder->Release(); pStream->Release(); pFactory->Release(); return false; }

    // Convert format
    IWICFormatConverter* pConverter = nullptr;
    hr = pFactory->CreateFormatConverter(&pConverter);
    if (FAILED(hr)) { pFrame->Release(); pDecoder->Release(); pStream->Release(); pFactory->Release(); return false; }

    hr = pConverter->Initialize(pFrame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) { pConverter->Release(); pFrame->Release(); pDecoder->Release(); pStream->Release(); pFactory->Release(); return false; }

    // Get size and copy pixels
    UINT width, height;
    pConverter->GetSize(&width, &height);
    *out_width = (int)width;
    *out_height = (int)height;

    std::vector<unsigned char> image_data(width * height * 4);
    pConverter->CopyPixels(nullptr, width * 4, width * height * 4, image_data.data());

    // Cleanup WIC objects
    pConverter->Release();
    pFrame->Release();
    pDecoder->Release();
    pStream->Release();
    pFactory->Release();

    // Create D3D11 texture
    D3D11_TEXTURE2D_DESC desc;
    ZeroMemory(&desc, sizeof(desc));
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0;

    ID3D11Texture2D* pTexture = nullptr;
    D3D11_SUBRESOURCE_DATA subResource;
    subResource.pSysMem = image_data.data();
    subResource.SysMemPitch = width * 4;
    subResource.SysMemSlicePitch = 0;
    
    pDevice->CreateTexture2D(&desc, &subResource, &pTexture);

    if (!pTexture) return false;

    // Create SRV
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
    ZeroMemory(&srvDesc, sizeof(srvDesc));
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = desc.MipLevels;
    srvDesc.Texture2D.MostDetailedMip = 0;
    
    pDevice->CreateShaderResourceView(pTexture, &srvDesc, out_srv);
        
    pTexture->Release();

    return true;
}

// Load texture from file (e.g. cs2_figure.png next to DLL)
bool LoadTextureFromFile(ID3D11Device* pDevice, const wchar_t* path, ID3D11ShaderResourceView** out_srv, int* out_width, int* out_height)
{
    if (!pDevice || !path || !out_srv) return false;
    IWICImagingFactory* pFactory = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFactory));
    if (FAILED(hr)) return false;
    IWICBitmapDecoder* pDecoder = nullptr;
    hr = pFactory->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &pDecoder);
    if (FAILED(hr)) { pFactory->Release(); return false; }
    IWICBitmapFrameDecode* pFrame = nullptr;
    hr = pDecoder->GetFrame(0, &pFrame);
    if (FAILED(hr)) { pDecoder->Release(); pFactory->Release(); return false; }
    IWICFormatConverter* pConverter = nullptr;
    hr = pFactory->CreateFormatConverter(&pConverter);
    if (FAILED(hr)) { pFrame->Release(); pDecoder->Release(); pFactory->Release(); return false; }
    hr = pConverter->Initialize(pFrame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) { pConverter->Release(); pFrame->Release(); pDecoder->Release(); pFactory->Release(); return false; }
    UINT width, height;
    pConverter->GetSize(&width, &height);
    if (out_width) *out_width = (int)width;
    if (out_height) *out_height = (int)height;
    std::vector<unsigned char> image_data(width * height * 4);
    pConverter->CopyPixels(nullptr, width * 4, width * height * 4, image_data.data());
    pConverter->Release();
    pFrame->Release();
    pDecoder->Release();
    pFactory->Release();
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D* pTexture = nullptr;
    D3D11_SUBRESOURCE_DATA subResource = {};
    subResource.pSysMem = image_data.data();
    subResource.SysMemPitch = width * 4;
    hr = pDevice->CreateTexture2D(&desc, &subResource, &pTexture);
    if (FAILED(hr) || !pTexture) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.MostDetailedMip = 0;
    hr = pDevice->CreateShaderResourceView(pTexture, &srvDesc, out_srv);
    pTexture->Release();
    return SUCCEEDED(hr);
}


// ImGui Win32 handler
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace Hooks {
    // Typedefs
    typedef HRESULT(__stdcall* Present_t)(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);
    typedef HRESULT(__stdcall* ResizeBuffers_t)(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags);
    typedef LRESULT(__stdcall* WndProc_t)(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
    typedef bool(__fastcall* CreateMove_t)(void*, int, float, bool);

    // Original functions
    Present_t oPresent = nullptr;
    ResizeBuffers_t oResizeBuffers = nullptr;
    WndProc_t oWndProc = nullptr;
    CreateMove_t oCreateMove = nullptr;
    
    // D3D11 objects
    ID3D11Device* g_pDevice = nullptr;
    ID3D11DeviceContext* g_pContext = nullptr;
    ID3D11RenderTargetView* g_pRenderTargetView = nullptr;

    // Window
    HWND g_hWnd = nullptr;

    // Threads
    HANDLE g_hAimbotThread = nullptr;
    HANDLE g_hTriggerbotThread = nullptr;
    bool g_bRunning = false;

    // State
    bool g_bInitialized = false;
    bool g_bMenuOpen = true;

    // Feature toggles
    bool g_bEspEnabled = true;
    bool g_bEspBoxes = true;
    bool g_bEspHealth = true;
    bool g_bEspNames = true;
    bool g_bEspSkeleton = true;
    bool g_bEspTracers = true;
    bool g_bAimbotEnabled = false;
    bool g_bTriggerbotEnabled = false;
    bool g_bTeamCheck = true;
    
    // Watermark settings
    bool g_bShowWatermark = true;
    bool g_bShowFpsPing = true;
    
    // Triggerbot settings
    int g_nTriggerbotDelay = 40;

    // GUI State
    int g_nCurrentTab = 0;
    int g_nPreviousTab = 0;
    float g_fTabAnim = 1.0f;
    float g_fSidebarHover = 0.0f;
    float g_fHeaderPulse = 0.0f;
    bool g_bShowEspPreview = true;

    // ESP Colors
    ImVec4 g_colorEspBox = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    ImVec4 g_colorEspSkeleton = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    ImVec4 g_colorEspName = ImVec4(1.0f, 1.0f, 0.0f, 1.0f);
    ImVec4 g_colorEspTracer = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);

    // Aimbot settings
    float g_fAimbotFov = 5.0f;
    float g_fAimbotSmooth = 5.0f;
    bool g_bAimbotFovCircle = true;
    bool g_bAimbotRcs = true;
    bool g_bAimbotVisibleOnly = true;
    int g_nAimbotBone = 6;
    float g_fRecoilControl = 100.0f; // Restored for backward compatibility
    int g_nAimbotKey = VK_RBUTTON;
    bool g_bWaitingForKey = false;
    std::string g_szCurrentKeyName = "Right Mouse";

    // Static Forward Declarations for Missing Symbols

    bool g_bFovChangerEnabled = false;
    int g_nFovValue = 90;
    bool g_bC4EspEnabled = true;
    bool g_bBombTimerEnabled = true;
    bool g_bBulletTracersEnabled = false;
    ImVec4 g_colorBulletTracer = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    float g_fBulletTracerDuration = 1.0f;
    bool g_bDamageIndicatorEnabled = false;
    bool g_bKeybindOverlayIngame = true;
    
    
    // RCS (Recoil Control System) tracking
    struct RecoilControlSystem {
        bool enabled = true;
        float strength = 1.0f; // 0.0 = off, 1.0 = full control
        bool onlyVertical = false; // Only compensate vertical recoil
        
        // State
        Vector3 oldPunch = {0, 0, 0};
        Vector3 originalViewAngles = {0, 0, 0};
        bool wasAttacking = false;
    } g_RCS;



    int g_lastShotsFired = 0;

    // Missing Globals
    uintptr_t g_lockedPawn = 0;
    QAngle g_targetAngle;
    bool g_bAimbotSnaplines = false;
    bool g_bAimbotActive = false;
    ImVec4 g_colorAimbotSnaplines = ImVec4(1.0f, 0.0f, 1.0f, 1.0f);

    struct BulletTracer { Vector3 start, end; float time; };
    std::vector<BulletTracer> g_bulletTracers;
    float g_fLastDamageDealt = 0.0f;
    float g_fDamageIndicatorTime = 0.0f;
    float g_fDamageIndicatorValue = 0.0f;

    // Layout constants - moved up for forward usage
    const float TAB_LEFT_PANEL_WIDTH = 320.0f;
    const float ANIM_SPEED = 8.0f;
    const ImVec4 ACCENT_PRIMARY{0.00f, 0.92f, 1.00f, 1.00f};   // Bright cyan
    const ImVec4 ACCENT_GLOW{0.00f, 0.85f, 1.00f, 0.35f};
    const ImVec4 ACCENT_SECONDARY{1.00f, 0.50f, 0.00f, 1.00f}; // Vibrant orange
    const ImVec4 SECTION_HEADER{0.55f, 0.90f, 1.00f, 1.00f};
    const ImVec4 TEXT_MUTED{0.58f, 0.68f, 0.82f, 1.00f};
    const ImVec4 BG_CARD{0.09f, 0.10f, 0.14f, 1.00f};
    const ImVec4 BG_CARD_HOVER{0.12f, 0.16f, 0.24f, 1.00f};

    // Animation states
    struct AnimState { float value = 0.0f; bool active = false; };
    std::map<std::string, AnimState> g_CheckboxAnims;

    // Draw a section header (same style for all tabs) with subtle accent line
    void TabSectionHeader(const char* title) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, SECTION_HEADER);
        ImGui::SetWindowFontScale(0.92f);
        ImGui::Text("%s", title);
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();
        ImVec2 rMin = ImGui::GetItemRectMin();
        ImVec2 rMax = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(rMin.x, rMax.y + 4), ImVec2(rMax.x + 80, rMax.y + 4), ImGui::ColorConvertFloat4ToU32(ImVec4(ACCENT_PRIMARY.x, ACCENT_PRIMARY.y, ACCENT_PRIMARY.z, 0.5f)), 2.0f);
        ImGui::Spacing();
        ImGui::Spacing();
    }
    
    // Custom animated checkbox
    bool AnimatedCheckbox(const char* label, bool* v) {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (window->SkipItems) return false;

        ImGuiContext& g = *GImGui;
        const ImGuiStyle& style = g.Style;
        const ImGuiID id = window->GetID(label);
        const ImVec2 label_size = ImGui::CalcTextSize(label, NULL, true);

        const float square_sz = ImGui::GetFrameHeight();
        const ImVec2 pos = window->DC.CursorPos;
        const ImRect total_bb(pos.x, pos.y, pos.x + square_sz + (label_size.x > 0.0f ? style.ItemInnerSpacing.x + label_size.x : 0.0f), pos.y + label_size.y + style.FramePadding.y * 2.0f);
        ImGui::ItemSize(total_bb, style.FramePadding.y);
        if (!ImGui::ItemAdd(total_bb, id)) return false;

        bool hovered, held;
        bool pressed = ImGui::ButtonBehavior(total_bb, id, &hovered, &held);
        if (pressed) {
            *v = !(*v);
            ImGui::MarkItemEdited(id);
        }

        auto& state = g_CheckboxAnims[label];
        float target = *v ? 1.0f : 0.0f;
        float speed = 8.0f * g.IO.DeltaTime;
        if (state.value < target) state.value = std::min(state.value + speed, target);
        else if (state.value > target) state.value = std::max(state.value - speed, target);

        const ImRect check_bb(pos.x, pos.y, pos.x + square_sz, pos.y + square_sz);
        ImGui::RenderNavHighlight(total_bb, id);
        ImGui::RenderFrame(check_bb.Min, check_bb.Max, ImGui::GetColorU32((held && hovered) ? ImGuiCol_FrameBgActive : hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), true, style.FrameRounding);

        ImU32 check_col = ImGui::GetColorU32(ACCENT_PRIMARY);
        if (state.value > 0.01f) {
            float pad = std::max(1.0f, (float)(int)(square_sz / 6.0f));
            float check_mark_speed = state.value;
            
            // Draw animated checkmark
            ImVec2 p1 = ImVec2(check_bb.Min.x + pad, check_bb.Min.y + square_sz * 0.5f);
            ImVec2 p2 = ImVec2(check_bb.Min.x + square_sz * 0.4f, check_bb.Min.y + square_sz - pad);
            ImVec2 p3 = ImVec2(check_bb.Max.x - pad, check_bb.Min.y + pad);

            // Calculate current path based on animation progress
            if (check_mark_speed < 0.5f) {
                float t = check_mark_speed * 2.0f;
                window->DrawList->AddLine(p1, ImVec2(p1.x + (p2.x - p1.x) * t, p1.y + (p2.y - p1.y) * t), check_col, 2.0f);
            } else {
                float t = (check_mark_speed - 0.5f) * 2.0f;
                window->DrawList->AddLine(p1, p2, check_col, 2.0f);
                window->DrawList->AddLine(p2, ImVec2(p2.x + (p3.x - p2.x) * t, p2.y + (p3.y - p2.y) * t), check_col, 2.0f);
            }
        }

        if (label_size.x > 0.0f)
            ImGui::RenderText(ImVec2(check_bb.Max.x + style.ItemInnerSpacing.x, check_bb.Min.y + style.FramePadding.y), label);

        return pressed;
    }
    
    // Helper function to get key name
    std::string GetKeyName(int vkCode) {
        switch (vkCode) {
            case VK_LBUTTON: return "Left Mouse";
            case VK_RBUTTON: return "Right Mouse";
            case VK_MBUTTON: return "Middle Mouse";
            case VK_XBUTTON1: return "Mouse 4";
            case VK_XBUTTON2: return "Mouse 5";
            case VK_SHIFT: return "Shift";
            case VK_CONTROL: return "Ctrl";
            case VK_MENU: return "Alt";
            case VK_SPACE: return "Space";
            default:
                if (vkCode >= 0x41 && vkCode <= 0x5A) { // A-Z
                    return std::string(1, (char)vkCode);
                }
                return "Key " + std::to_string(vkCode);
        }
    }

    // Helper for FindPattern
    static BYTE getByte(const char* pat) {
        return (BYTE)strtol(pat, nullptr, 16);
    }

    // Pattern scanning helper
    uintptr_t FindPattern(const char* moduleName, const char* pattern) {
        uintptr_t moduleBase = (uintptr_t)GetModuleHandleA(moduleName);
        if (!moduleBase) return 0;

        MODULEINFO moduleInfo;
        GetModuleInformation(GetCurrentProcess(), (HMODULE)moduleBase, &moduleInfo, sizeof(MODULEINFO));
        
        uintptr_t sz = moduleInfo.SizeOfImage;
        const char* pat = pattern;
        uintptr_t firstMatch = 0;
        
        for (uintptr_t pCur = moduleBase; pCur < moduleBase + sz; pCur++) {
            if (!*pat) return firstMatch;
            if (*(PBYTE)pat == '\?' || *(BYTE*)pCur == getByte(pat)) {
                if (!firstMatch) firstMatch = pCur;
                if (!pat[2]) return firstMatch;
                if (*(PWORD)pat == '\?\?' || *(PBYTE)pat != '\?') pat += 3;
                else pat += 2;
            } else {
                pat = pattern;
                firstMatch = 0;
            }
        }
        return 0;
    }

    // Forward declarations
    void RenderMenu();
    void RenderESP();
    void RenderKeybindTab();
    void RenderKeybindOverview(float widthLeft = 0);
    void RenderKeybindOverlayIngame();
    void RenderConfigTab();
    void RenderConfigPreview();
    void RunAimbot();
    void RunTriggerbot();
    void RunNoRecoil();
    void RunFovChanger();
    void RunBulletTracers();
    void RenderBulletTracers();
    void RenderWatermark();
    void RenderC4ESP();
    void UpdateDamageIndicator();
    void RenderDamageIndicator();
    Vector3 GetBonePosition(uintptr_t pawn, int boneId);
    DWORD WINAPI AimbotThread(LPVOID lpParam);
    DWORD WINAPI TriggerbotThread(LPVOID lpParam);

    // WndProc hook
    LRESULT CALLBACK hkWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (msg == WM_KEYDOWN && wParam == VK_INSERT) {
            g_bMenuOpen = !g_bMenuOpen;
            return 0;
        }

        if (g_bMenuOpen) {
            if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) {
                return 0;
            }
        }

        return CallWindowProcA(oWndProc, hWnd, msg, wParam, lParam);
    }



    // Present hook
    HRESULT __stdcall hkPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
        if (!g_bInitialized) {
            // Get device and context
            if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D11Device), (void**)&g_pDevice))) {
                g_pDevice->GetImmediateContext(&g_pContext);

                // Get window handle
                DXGI_SWAP_CHAIN_DESC desc;
                pSwapChain->GetDesc(&desc);
                g_hWnd = desc.OutputWindow;

                // Create render target view
                ID3D11Texture2D* pBackBuffer = nullptr;
                pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&pBackBuffer);
                if (pBackBuffer) {
                    g_pDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_pRenderTargetView);
                    pBackBuffer->Release();
                }

                // Hook WndProc
                oWndProc = (WndProc_t)SetWindowLongPtrA(g_hWnd, GWLP_WNDPROC, (LONG_PTR)hkWndProc);

                // Initialize ImGui
                ImGui::CreateContext();
                ImGuiIO& io = ImGui::GetIO();
                io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
                
                // Load Fonts - Aternos Style
                ImFontConfig font_config;
                font_config.PixelSnapH = false;
                font_config.OversampleH = 5;
                font_config.OversampleV = 5;
                font_config.RasterizerMultiply = 1.2f;

                static const ImWchar ranges[] = {
                    0x0020, 0x00FF, // Basic Latin + Latin Supplement
                    0x0400, 0x052F, // Cyrillic + Cyrillic Supplement
                    0x2DE0, 0x2DFF, // Cyrillic Extended-A
                    0xA640, 0xA69F, // Cyrillic Extended-B
                    0xE000, 0xE226, // icons
                    0,
                };
                
                font::lexend_general_bold = io.Fonts->AddFontFromMemoryTTF(lexend_bold, sizeof(lexend_bold), 18.f, &font_config, ranges);
                font::lexend_bold = io.Fonts->AddFontFromMemoryTTF(lexend_regular, sizeof(lexend_regular), 17.f, &font_config, ranges);
                font::lexend_regular = io.Fonts->AddFontFromMemoryTTF(lexend_regular, sizeof(lexend_regular), 14.f, &font_config, ranges);
                font::icomoon = io.Fonts->AddFontFromMemoryTTF(icomoon, sizeof(icomoon), 20.f, &font_config, ranges);
                font::icomoon_widget = io.Fonts->AddFontFromMemoryTTF(icomoon_widget, sizeof(icomoon_widget), 15.f, &font_config, ranges);
                font::icomoon_widget2 = io.Fonts->AddFontFromMemoryTTF(icomoon, sizeof(icomoon), 16.f, &font_config, ranges);
                
                // Load Texture
                int w = 0, h = 0;
                LoadTextureFromMemory(g_pDevice, esp_preview1, sizeof(esp_preview1), &g_pEspPreviewTexture, &w, &h);
                // ESP Preview window: load cs2_figure.png from DLL directory
                if (g_hModule) {
                    wchar_t pathBuf[MAX_PATH];
                    if (GetModuleFileNameW(g_hModule, pathBuf, MAX_PATH)) {
                        PathRemoveFileSpecW(pathBuf);
                        wcscat_s(pathBuf, L"\\cs2_figure.png");
                        LoadTextureFromFile(g_pDevice, pathBuf, &g_pEspPreviewFigureTexture, &g_espFigureTexW, &g_espFigureTexH);
                    }
                }

                ImGui::StyleColorsDark();
                ImGui_ImplWin32_Init(g_hWnd);
                ImGui_ImplDX11_Init(g_pDevice, g_pContext);

                g_bInitialized = true;
            }
        }

        if (g_bInitialized) {
            // Start ImGui frame
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            // Render menu
            if (g_bMenuOpen) {
                RenderMenu();
            }

            // Game-dependent code: wrap in SEH so we don't crash when joining / loading
            __try {
                if (g_bEspEnabled) RenderESP();
                if (g_bC4EspEnabled || g_bBombTimerEnabled) RenderC4ESP();
                UpdateDamageIndicator();
                if (g_bDamageIndicatorEnabled) RenderDamageIndicator();
                if (g_bKeybindOverlayIngame) RenderKeybindOverlayIngame();
                if (g_bShowWatermark || g_bShowFpsPing) RenderWatermark();
                RunNoRecoil();
                RunFovChanger();
                RunBulletTracers();
                RenderBulletTracers();

            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                // Skip this frame on access violation (e.g. when joining game / loading map)
            }

            // NOTE: Aimbot and Triggerbot run in separate threads now!

            // Render ImGui
            ImGui::Render();
            g_pContext->OMSetRenderTargets(1, &g_pRenderTargetView, nullptr);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        }

        return oPresent(pSwapChain, SyncInterval, Flags);
    }

    // ResizeBuffers hook (handle window resize)
    HRESULT __stdcall hkResizeBuffers(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags) {
        if (g_pRenderTargetView) {
            g_pRenderTargetView->Release();
            g_pRenderTargetView = nullptr;
        }

        HRESULT hr = oResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);

        // Recreate render target view
        ID3D11Texture2D* pBackBuffer = nullptr;
        pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&pBackBuffer);
        if (pBackBuffer) {
            g_pDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_pRenderTargetView);
            pBackBuffer->Release();
        }

        return hr;
    }

    // Get swapchain via dummy device
    IDXGISwapChain* GetSwapChain() {
        // Create dummy window
        WNDCLASSEXA wc = { sizeof(WNDCLASSEXA), CS_CLASSDC, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr, "DummyClass", nullptr };
        RegisterClassExA(&wc);
        HWND hWnd = CreateWindowA("DummyClass", "", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);

        DXGI_SWAP_CHAIN_DESC sd = {};
        sd.BufferCount = 1;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = hWnd;
        sd.SampleDesc.Count = 1;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        ID3D11Device* pDevice = nullptr;
        ID3D11DeviceContext* pContext = nullptr;
        IDXGISwapChain* pSwapChain = nullptr;

        D3D_FEATURE_LEVEL featureLevel;
        D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &pSwapChain, &pDevice, &featureLevel, &pContext);

        // Get vtable
        void** pVTable = *(void***)pSwapChain;

        // Store pointers for hooking
        static void* presentAddr = pVTable[8];
        static void* resizeBuffersAddr = pVTable[13];

        // Cleanup dummy objects
        pContext->Release();
        pDevice->Release();
        pSwapChain->Release();
        DestroyWindow(hWnd);
        UnregisterClassA("DummyClass", wc.hInstance);

        // Return vtable addresses via static
        return (IDXGISwapChain*)presentAddr; // Abuse return to pass present address
    }
    // CreateMove Hook for Silent Aim and RCS
    bool __fastcall hkCreateMove(void* input, int slot, float frametime, bool relative) {
        bool result = oCreateMove(input, slot, frametime, relative);
        
        uintptr_t clientBase = (uintptr_t)GetModuleHandleA("client.dll");
        uintptr_t localPawn = *(uintptr_t*)(clientBase + cs2_dumper::offsets::client_dll::dwLocalPlayerPawn);
        uintptr_t cmd_ptr = *(uintptr_t*)((uintptr_t)input + 0x250);
        
        if (cmd_ptr && localPawn) {
            QAngle* pAngles = (QAngle*)(cmd_ptr + 0x54); // cmd->m_pViewAngles
            if (pAngles) {
                // Sync command angles to aimbot target so first shot hits (no "aim over target" delay)
                if (g_lockedPawn && g_bAimbotActive) {
                    *pAngles = g_targetAngle;
                }
                pAngles->Clamp();
            }
        }
        
        return result;
    }

    void Initialize() {
        // ... previous initialization code ...
        // Get Present address
        WNDCLASSEXA wc = { sizeof(WNDCLASSEXA), CS_CLASSDC, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr, "DX11Hook", nullptr };
        RegisterClassExA(&wc);
        HWND hWnd = CreateWindowA("DX11Hook", "", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);

        DXGI_SWAP_CHAIN_DESC sd = {};
        sd.BufferCount = 1;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = hWnd;
        sd.SampleDesc.Count = 1;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        ID3D11Device* pDevice = nullptr;
        ID3D11DeviceContext* pContext = nullptr;
        IDXGISwapChain* pSwapChain = nullptr;
        D3D_FEATURE_LEVEL featureLevel;

        if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &pSwapChain, &pDevice, &featureLevel, &pContext))) {
            DestroyWindow(hWnd);
            UnregisterClassA("DX11Hook", wc.hInstance);
            return;
        }

        // Get vtable
        void** pVTable = *(void***)pSwapChain;
        void* pPresent = pVTable[8];
        void* pResizeBuffers = pVTable[13];

        // Cleanup
        pContext->Release();
        pDevice->Release();
        pSwapChain->Release();
        DestroyWindow(hWnd);
        UnregisterClassA("DX11Hook", wc.hInstance);

        // Initialize MinHook
        MH_Initialize();

        // Hook Present
        MH_CreateHook(pPresent, &hkPresent, (void**)&oPresent);
        MH_EnableHook(pPresent);

        // Hook ResizeBuffers
        MH_CreateHook(pResizeBuffers, &hkResizeBuffers, (void**)&oResizeBuffers);
        MH_EnableHook(pResizeBuffers);

        // CreateMove hook
        uintptr_t createMoveAddr = FindPattern("client.dll", "48 8B C4 48 89 58 18 48 89 70 20 48 89 48 08 55 41 54 41 55 41 56 41 57");
        if (createMoveAddr) {
            MH_CreateHook((void*)createMoveAddr, &hkCreateMove, (void**)&oCreateMove);
            MH_EnableHook((void*)createMoveAddr);
            printf("[+] CreateMove hooked at 0x%p\n", (void*)createMoveAddr);
        }

        // DrawObject / Material System removed

        // Start feature threads
        g_bRunning = true;
        g_hAimbotThread = CreateThread(nullptr, 0, AimbotThread, nullptr, 0, nullptr);
        g_hTriggerbotThread = CreateThread(nullptr, 0, TriggerbotThread, nullptr, 0, nullptr);
        
        printf("[+] Feature threads started\n");
    }

    void Shutdown() {
        // Stop feature threads
        g_bRunning = false;
        
        if (g_hAimbotThread) {
            WaitForSingleObject(g_hAimbotThread, 1000);
            CloseHandle(g_hAimbotThread);
            g_hAimbotThread = nullptr;
        }
        if (g_hTriggerbotThread) {
            WaitForSingleObject(g_hTriggerbotThread, 1000);
            CloseHandle(g_hTriggerbotThread);
            g_hTriggerbotThread = nullptr;
        }
        
        printf("[+] Feature threads stopped\n");

        // Disable hooks
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();

        // Restore WndProc
        if (oWndProc) {
            SetWindowLongPtrA(g_hWnd, GWLP_WNDPROC, (LONG_PTR)oWndProc);
        }

        // Shutdown ImGui
        if (g_bInitialized) {
            ImGui_ImplDX11_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
        }

        // Release D3D objects
        if (g_pRenderTargetView) g_pRenderTargetView->Release();
        if (g_pContext) g_pContext->Release();
        if (g_pDevice) g_pDevice->Release();
    }



    // Apply custom ImGui styling - Aternos Theme uses its own colors from c:: namespace
    void ApplyCustomStyle() {
        ImGui::StyleColorsDark();
        // Additional global style tweaks if needed, but mostly handled by widgets
    }

    void DrawSkeletonPreviewRect(ImDrawList* draw, ImVec2 center, float width, float height, ImVec4 color);
    // Draw skeleton preview - uses width & height so it fits the figure/box rect
    void DrawSkeletonPreview(ImDrawList* draw, ImVec2 center, float height, ImVec4 color) {
        float width = height * 0.38f; // default for legacy callers
        DrawSkeletonPreviewRect(draw, center, width, height, color);
    }
    void DrawSkeletonPreviewRect(ImDrawList* draw, ImVec2 center, float width, float height, ImVec4 color) {
        ImU32 col = ImGui::ColorConvertFloat4ToU32(color);
        float headSize = height * 0.12f;
        float torsoHeight = height * 0.35f;
        float legHeight = height * 0.45f;
        float shoulderWidth = width * 0.48f;
        float armOut = width * 0.18f;
        float hipWidth = width * 0.22f;
        float legOut = width * 0.2f;

        ImVec2 head(0, -height/2 + headSize/2);
        ImVec2 neck(0, -height/2 + headSize);
        ImVec2 chest(0, neck.y + torsoHeight * 0.3f);
        ImVec2 pelvis(0, neck.y + torsoHeight);

        ImVec2 lShoulder(-shoulderWidth/2, neck.y);
        ImVec2 rShoulder(shoulderWidth/2, neck.y);
        // Straight arms: elbow and hand collinear with shoulder (no curve)
        ImVec2 lElbow(-shoulderWidth/2 - armOut, chest.y);
        ImVec2 rElbow(shoulderWidth/2 + armOut, chest.y);
        ImVec2 lHand(-shoulderWidth/2 - armOut, pelvis.y);
        ImVec2 rHand(shoulderWidth/2 + armOut, pelvis.y);

        ImVec2 lHip(-hipWidth, pelvis.y);
        ImVec2 rHip(hipWidth, pelvis.y);
        ImVec2 lKnee(-legOut, pelvis.y + legHeight * 0.5f);
        ImVec2 rKnee(legOut, pelvis.y + legHeight * 0.5f);
        ImVec2 lFoot(-legOut, height/2 - height*0.02f);
        ImVec2 rFoot(legOut, height/2 - height*0.02f);

        auto drawBone = [&](ImVec2 a, ImVec2 b) {
            draw->AddLine(ImVec2(center.x + a.x, center.y + a.y),
                         ImVec2(center.x + b.x, center.y + b.y), col, 2.0f);
        };
        draw->AddCircle(ImVec2(center.x + head.x, center.y + head.y), headSize/2, col, 12, 2.0f);
        drawBone(neck, chest);
        drawBone(chest, pelvis);
        drawBone(neck, lShoulder);
        drawBone(neck, rShoulder);
        drawBone(lShoulder, lElbow);
        drawBone(rShoulder, rElbow);
        drawBone(lElbow, lHand);
        drawBone(rElbow, rHand);
        drawBone(pelvis, lHip);
        drawBone(pelvis, rHip);
        drawBone(lHip, lKnee);
        drawBone(rHip, rKnee);
        drawBone(lKnee, lFoot);
        drawBone(rKnee, rFoot);
    }

    // CS2-style tactical silhouette: helmet, vest, rifle, stance (for preview only)
    void DrawCS2StyleSilhouette(ImDrawList* draw, ImVec2 center, float height, ImVec4 color) {
        ImU32 col = ImGui::ColorConvertFloat4ToU32(color);
        ImU32 colDark = ImGui::ColorConvertFloat4ToU32(ImVec4(color.x * 0.6f, color.y * 0.6f, color.z * 0.6f, color.w));
        
        float h = height;
        float w = height * 0.32f;
        
        // Ground line (floor)
        float groundY = center.y + h * 0.48f;
        draw->AddLine(ImVec2(center.x - w * 1.8f, groundY), ImVec2(center.x + w * 1.8f, groundY), colDark, 1.5f);
        
        // Helmet (rounded top - like CS2 CT/T head)
        float headR = h * 0.11f;
        ImVec2 headC(center.x, center.y - h * 0.42f);
        draw->AddCircle(headC, headR, col, 16, 2.0f);
        draw->AddCircle(headC, headR - 1.0f, colDark, 16, 1.0f);
        
        // Vest / torso (trapezoid body)
        ImVec2 vestTL(center.x - w * 0.55f, center.y - h * 0.28f);
        ImVec2 vestTR(center.x + w * 0.55f, center.y - h * 0.28f);
        ImVec2 vestBL(center.x - w * 0.45f, center.y + h * 0.12f);
        ImVec2 vestBR(center.x + w * 0.45f, center.y + h * 0.12f);
        draw->AddQuad(vestTL, vestTR, vestBR, vestBL, col, 2.0f);
        
        // Rifle (horizontal line from chest forward - AK/M4 style)
        float rifleLen = h * 0.38f;
        ImVec2 rifleStart(center.x + w * 0.35f, center.y - h * 0.08f);
        ImVec2 rifleEnd(center.x + w * 0.35f + rifleLen, center.y - h * 0.02f);
        draw->AddLine(rifleStart, rifleEnd, col, 2.5f);
        draw->AddLine(ImVec2(rifleEnd.x, rifleEnd.y - 2), ImVec2(rifleEnd.x + 8, rifleEnd.y), colDark, 1.5f); // muzzle
        // Stock
        draw->AddLine(ImVec2(rifleStart.x - 5, rifleStart.y), ImVec2(rifleStart.x, rifleStart.y + 4), colDark, 1.5f);
        
        // Arms (holding rifle)
        draw->AddLine(ImVec2(center.x - w * 0.35f, center.y - h * 0.22f), rifleStart, col, 1.8f);
        draw->AddLine(ImVec2(center.x + w * 0.25f, center.y - h * 0.18f), rifleEnd, col, 1.8f);
        
        // Legs (tactical stance - slightly spread)
        float legTop = center.y + h * 0.14f;
        draw->AddLine(ImVec2(center.x - w * 0.2f, legTop), ImVec2(center.x - w * 0.35f, groundY - 2), col, 2.0f);
        draw->AddLine(ImVec2(center.x + w * 0.2f, legTop), ImVec2(center.x + w * 0.35f, groundY - 2), col, 2.0f);
    }

    // Render ESP preview window - same structure as other tab previews
    void RenderESPPreview() {
        ImGui::BeginChild("ESPPreview", ImVec2(0, 0), true, ImGuiWindowFlags_NoScrollbar);
        
        ImGui::PushStyleColor(ImGuiCol_Text, ACCENT_PRIMARY);
        ImGui::SetWindowFontScale(1.05f);
        ImGui::Text("ESP PREVIEW");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(ACCENT_PRIMARY.x, ACCENT_PRIMARY.y, ACCENT_PRIMARY.z, 0.5f));
        ImGui::Separator();
        ImGui::PopStyleColor();
        ImGui::Spacing();
        
        ImDrawList* draw = ImGui::GetWindowDrawList();
        ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
        ImVec2 canvas_size = ImGui::GetContentRegionAvail();
        
        // Background - simple dark color
        draw->AddRectFilled(canvas_pos, ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y), IM_COL32(10, 14, 22, 255));
        
        // Subtle grid with cyan tint
        for (int i = 1; i < 8; i++) {
            float y = canvas_pos.y + (canvas_size.y / 8) * i;
            draw->AddLine(ImVec2(canvas_pos.x, y), ImVec2(canvas_pos.x + canvas_size.x, y), IM_COL32(0, 60, 90, 22), 1.0f);
        }
        
        float playerHeight = 220.0f;
        float playerWidth = 65.0f;
        ImVec2 center(canvas_pos.x + canvas_size.x / 2, canvas_pos.y + canvas_size.y / 2 + 15);
        ImVec2 topLeft(center.x - playerWidth / 2, center.y - playerHeight / 2);
        ImVec2 bottomRight(center.x + playerWidth / 2, center.y + playerHeight / 2);
        
        // NEW: Draw Player Image (SAS Model)
        if (g_pEspPreviewTexture) {
            // Adjust image bounds slightly to fit the box nicely (texture usually has whitespace/padding)
            // Or just draw it exactly in the box area.
            // Assuming the image provided is a cutout, we can draw it with preserving aspect if needed, but fitting the box is simplest.
            draw->AddImage((ImTextureID)g_pEspPreviewTexture, topLeft, bottomRight);
        }

        // ESP box
        if (g_bEspBoxes) {
            ImU32 boxColor = ImGui::ColorConvertFloat4ToU32(g_colorEspBox);
            draw->AddRect(
                ImVec2(topLeft.x - 1, topLeft.y - 1),
                ImVec2(bottomRight.x + 1, bottomRight.y + 1),
                IM_COL32(0, 180, 255, 60), 0.0f, 0, 3.0f
            );
            draw->AddRect(topLeft, bottomRight, boxColor, 0.0f, 0, 2.5f);
        }
        
        if (g_bEspHealth) {
            float healthPercent = 0.75f;
            ImVec2 healthBarStart(topLeft.x - 8, topLeft.y);
            ImVec2 healthBarEnd(topLeft.x - 3, bottomRight.y);
            draw->AddRectFilled(healthBarStart, healthBarEnd, IM_COL32(25, 28, 35, 220));
            ImVec2 healthStart(topLeft.x - 7, bottomRight.y - 1);
            ImVec2 healthEnd(topLeft.x - 4, bottomRight.y - playerHeight * healthPercent);
            draw->AddRectFilledMultiColor(healthStart, healthEnd, IM_COL32(0, 200, 100, 255), IM_COL32(0, 200, 100, 255), IM_COL32(80, 255, 150, 255), IM_COL32(80, 255, 150, 255));
            draw->AddRect(healthBarStart, healthBarEnd, IM_COL32(0, 0, 0, 255), 0.0f, 0, 1.5f);
        }
        
        if (g_bEspNames) {
            ImU32 nameColor = ImGui::ColorConvertFloat4ToU32(g_colorEspName);
            const char* name = "Enemy Player";
            ImVec2 textSize = ImGui::CalcTextSize(name);
            ImVec2 namePos(center.x - textSize.x / 2, topLeft.y - 22);
            draw->AddText(ImVec2(namePos.x + 1, namePos.y + 1), IM_COL32(0, 0, 0, 200), name);
            draw->AddText(namePos, nameColor, name);
        }
        
        if (g_bEspSkeleton) {
            DrawSkeletonPreview(draw, center, playerHeight, g_colorEspSkeleton);
        }

        // Crosshair in center (CS2 style) with subtle pulse
        float pulse = 0.85f + 0.15f * (float)sin(ImGui::GetTime() * 2.5);
        float chSize = 6.0f;
        ImU32 chColor = IM_COL32(255, (int)(200 * pulse), (int)(60 * pulse), 220);
        draw->AddLine(ImVec2(center.x - chSize - 2, center.y), ImVec2(center.x - 2, center.y), chColor, 1.8f);
        draw->AddLine(ImVec2(center.x + 2, center.y), ImVec2(center.x + chSize + 2, center.y), chColor, 1.8f);
        draw->AddLine(ImVec2(center.x, center.y - chSize - 2), ImVec2(center.x, center.y - 2), chColor, 1.8f);
        draw->AddLine(ImVec2(center.x, center.y + 2), ImVec2(center.x, center.y + chSize + 2), chColor, 1.8f);
        
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + canvas_size.y - ImGui::GetTextLineHeight() - 12);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.62f, 0.72f, 1.0f));
        ImGui::SetWindowFontScale(0.85f);
        ImGui::TextWrapped("Toggle options to see real-time ESP preview");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();
        
        ImGui::EndChild();
    }


    // Combat tab preview: FOV circle + recoil bar (same structure as ESP preview)
    void RenderCombatPreview() {
        ImGui::BeginChild("CombatPreview", ImVec2(0, 0), true, ImGuiWindowFlags_NoScrollbar);
        
        ImGui::PushStyleColor(ImGuiCol_Text, ACCENT_PRIMARY);
        ImGui::SetWindowFontScale(1.05f);
        ImGui::Text("COMBAT PREVIEW");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(ACCENT_PRIMARY.x, ACCENT_PRIMARY.y, ACCENT_PRIMARY.z, 0.5f));
        ImGui::Separator();
        ImGui::PopStyleColor();
        ImGui::Spacing();
        
        ImDrawList* draw = ImGui::GetWindowDrawList();
        ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
        ImVec2 canvas_size = ImGui::GetContentRegionAvail();
        
        // Multi-color background removed for cleaner look
        draw->AddRectFilled(canvas_pos, ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y), IM_COL32(10, 14, 22, 255));
        
        for (int i = 1; i < 6; i++) {
            float x = canvas_pos.x + (canvas_size.x / 6) * i;
            draw->AddLine(ImVec2(x, canvas_pos.y), ImVec2(x, canvas_pos.y + canvas_size.y), IM_COL32(0, 60, 90, 18), 1.0f);
        }
        
        ImVec2 center(canvas_pos.x + canvas_size.x / 2, canvas_pos.y + canvas_size.y / 2);
        
        // FOV circle (scaled to fit preview) with glow
        float radius = (g_fAimbotFov * 10.0f) * 0.35f;
        if (radius > 2.0f) {
            draw->AddCircle(center, radius + 2, IM_COL32(0, 180, 255, 35), 48, 2.0f);
            draw->AddCircle(center, radius, IM_COL32(0, 220, 255, 140), 48, 2.0f);
            draw->AddCircle(center, radius - 0.5f, IM_COL32(0, 255, 255, 80), 48, 1.0f);
        }
        
        // Crosshair with pulse
        float pulse = 0.88f + 0.12f * (float)sin(ImGui::GetTime() * 2.2);
        float ch = 8.0f;
        ImU32 chCol = IM_COL32(255, (int)(200 * pulse), (int)(80 * pulse), 230);
        draw->AddLine(ImVec2(center.x - ch, center.y), ImVec2(center.x + ch, center.y), chCol, 1.8f);
        draw->AddLine(ImVec2(center.x, center.y - ch), ImVec2(center.x, center.y + ch), chCol, 1.8f);
        
        // Recoil bar (bottom of preview)
        float barW = canvas_size.x * 0.6f;
        float barH = 14.0f;
        float barX = center.x - barW / 2;
        float barY = canvas_pos.y + canvas_size.y - 50;
        
        draw->AddRectFilled(ImVec2(barX, barY), ImVec2(barX + barW, barY + barH), IM_COL32(80, 40, 40, 200));
        float fillW = barW * (g_fRecoilControl / 100.0f);
        draw->AddRectFilledMultiColor(
            ImVec2(barX, barY), ImVec2(barX + fillW, barY + barH),
            IM_COL32(0, 200, 120, 255), IM_COL32(0, 200, 120, 255),
            IM_COL32(0, 255, 160, 255), IM_COL32(0, 255, 160, 255)
        );
        draw->AddRect(ImVec2(barX, barY), ImVec2(barX + barW, barY + barH), IM_COL32(0, 180, 255, 150), 0.0f, 0, 1.5f);
        
        ImGui::SetCursorPosY(barY + barH - canvas_pos.y + 8);
        ImGui::PushStyleColor(ImGuiCol_Text, SECTION_HEADER);
        ImGui::SetWindowFontScale(0.9f);
        ImGui::Text("FOV: %.1f  |  RCS: %.0f%%", g_fAimbotFov, g_fRecoilControl);
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();
        
        ImGui::EndChild();
    }

    // Aimbot Tab
    void RenderAimbotTab() {
        // Use edited::BeginChild if available or standard ImGui::BeginChild but styled
        // implementation of edited::BeginChild seems to wrap ImGui::BeginChild
        edited::BeginChild("AimbotSettings", ImVec2(TAB_LEFT_PANEL_WIDTH, 0));
        
        ImGui::SetCursorPos(ImVec2(16, 12));
        ImGui::BeginGroup();
        
        ImGui::PushStyleColor(ImGuiCol_Text, ACCENT_PRIMARY);
        ImGui::SetWindowFontScale(0.95f);
        ImGui::Text("COMBAT - AIMBOT");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();
        ImGui::Spacing();
        
        edited::Checkbox("Enable Aimbot", "Master switch for aimbot", &g_bAimbotEnabled);
        edited::SliderFloat("FOV", "Field of view", &g_fAimbotFov, 1.0f, 50.0f, "%.1f");
        edited::SliderFloat("Smoothing", "Slower aiming", &g_fAimbotSmooth, 1.0f, 100.0f, "%.1f");
        edited::Checkbox("Visible Only", "Aim only at visible enemies", &g_bAimbotVisibleOnly);
        edited::Checkbox("Draw FOV Circle", "Visualise FOV", &g_bAimbotFovCircle);
        
        if (edited::Checkbox("RCS (No Recoil)", "Recoil Control System", &g_RCS.enabled)) {
            // Optional: reset state on toggle?
        }
        if (g_RCS.enabled) {
            edited::SliderFloat("RCS Strength", "Amount of recoil control", &g_RCS.strength, 0.0f, 2.0f, "%.2f");
            edited::Checkbox("Vertical Only", "Only control pitch", &g_RCS.onlyVertical);
        }
        
        edited::Checkbox("Snaplines", "Draw lines to targets", &g_bAimbotSnaplines);
        if (g_bAimbotSnaplines) {
             edited::ColorEdit4("Snap Color", "Color of snaplines", (float*)&g_colorAimbotSnaplines);
        }
        
        const char* bones[] = { "Head", "Neck", "Chest" };
        static int boneIdx = 0;
        // edited::Combo might differ. Checking header:
        // bool Combo(const char* label, const char* description, int* current_item, const char* const items[], int items_count, int popup_max_height_in_items = -1);
        if (edited::Combo("Target Bone", "Bone to aim at", &boneIdx, bones, IM_ARRAYSIZE(bones))) {
            if (boneIdx == 0) g_nAimbotBone = 6;
            else if (boneIdx == 1) g_nAimbotBone = 5;
            else if (boneIdx == 2) g_nAimbotBone = 4;
        }

        ImGui::Spacing();
        ImGui::Separator();
        TabSectionHeader("TRIGGERBOT");
        edited::Checkbox("Enable Triggerbot", "Auto-fire when aiming at enemy", &g_bTriggerbotEnabled);
        edited::SliderInt("Trigger delay", "Delay before firing (ms)", &g_nTriggerbotDelay, 0, 500, "%d");

        ImGui::Separator();
        TabSectionHeader("KEYBIND");
        
        // edited::Keybind(const char* label, const char* description, int* key);
        edited::Keybind("Aimbot Key", "Key to activate aimbot", &g_nAimbotKey);
        
        ImGui::EndGroup();
        // edited::EndChild(); // implementation of EndChild just calls ImGui::EndChild
        edited::EndChild();

        ImGui::SameLine();
        RenderCombatPreview();
    }

    // ESP Tab
    void RenderESPTab() {
        edited::BeginChild("VisualSettings", ImVec2(TAB_LEFT_PANEL_WIDTH, 0));
        
        ImGui::SetCursorPos(ImVec2(16, 12));
        ImGui::BeginGroup();
        
        ImGui::PushStyleColor(ImGuiCol_Text, ACCENT_PRIMARY);
        ImGui::SetWindowFontScale(0.95f);
        ImGui::Text("VISUALS");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();
        ImGui::Spacing();
        
        edited::Checkbox("Enable ESP", "Master switch", &g_bEspEnabled);
        edited::Checkbox("Box", "Draw 2D Box", &g_bEspBoxes);
        if (g_bEspBoxes) {
             edited::ColorEdit4("Box Color", "Color of boxes", (float*)&g_colorEspBox);
        }
        edited::Checkbox("Skeleton", "Draw bones", &g_bEspSkeleton);
        if (g_bEspSkeleton) {
             edited::ColorEdit4("Skeleton Color", "Color of skeleton", (float*)&g_colorEspSkeleton);
        }
        edited::Checkbox("Name", "Draw player name", &g_bEspNames);
        if (g_bEspNames) {
             edited::ColorEdit4("Name Color", "Color of names", (float*)&g_colorEspName);
        }
        edited::Checkbox("Health Bar", "Draw health", &g_bEspHealth);
        edited::Checkbox("Tracers", "Lines to players", &g_bEspTracers);
        if (g_bEspTracers) {
             edited::ColorEdit4("Tracer Color", "Color of tracers", (float*)&g_colorEspTracer);
        }
        ImGui::EndGroup();
        // edited::EndChild();
        edited::EndChild(); // Using namespace edited

        ImGui::SameLine();
        
        // Visual Preview
        edited::BeginChild("VisualPreview", ImVec2(0, 0));
        
        static bool dummy = true;
        static float dummyCol[4] = {1,1,1,1};
        static int dummyHp = 100;
        
        if (g_pEspPreviewTexture) {
             edited::esp_preview(
                 (ImTextureID)g_pEspPreviewTexture, 
                 &g_bEspNames, (float*)&g_colorEspName,
                 &dummy, dummyCol, // weapon
                 &dummyHp, dummyCol, // hp text?
                 &dummy, dummyCol, // zoom
                 &g_bBombTimerEnabled, dummyCol, // bomb
                 &g_bC4EspEnabled, dummyCol, // c4
                 &dummy, dummyCol, // money
                 &dummy, dummyCol, // hit
                 &g_bEspBoxes, (float*)&g_colorEspBox,
                 &g_bEspHealth, dummyCol // hp line
             );
        } else {
             ImGui::Text("Texture not loaded");
        }
        
        edited::EndChild();
    }

    // Misc tab preview panel - shortcuts and info (same structure as other tabs)
    void RenderMiscPreview() {
        ImGui::BeginChild("MiscPreview", ImVec2(0, 0), true, ImGuiWindowFlags_NoScrollbar);
        
        ImDrawList* draw = ImGui::GetWindowDrawList();
        ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
        ImVec2 canvas_size = ImGui::GetContentRegionAvail();
        // Multi-color background removed for cleaner look
        draw->AddRectFilled(canvas_pos, ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y), IM_COL32(10, 14, 22, 255));
        
        ImGui::SetCursorPos(ImVec2(16, 12));
        ImGui::BeginGroup();
        
        ImGui::PushStyleColor(ImGuiCol_Text, ACCENT_PRIMARY);
        ImGui::SetWindowFontScale(1.05f);
        ImGui::Text("INFO");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(ACCENT_PRIMARY.x, ACCENT_PRIMARY.y, ACCENT_PRIMARY.z, 0.5f));
        ImGui::Separator();
        ImGui::PopStyleColor();
        ImGui::Spacing();
        
        ImGui::PushStyleColor(ImGuiCol_Text, SECTION_HEADER);
        ImGui::Text("Keyboard Shortcuts");
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::Bullet();
        ImGui::Text("INSERT  -  Toggle menu");
        ImGui::Bullet();
        ImGui::Text("END  -  Unload");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        
        ImGui::PushStyleColor(ImGuiCol_Text, SECTION_HEADER);
        ImGui::Text("Team Check");
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, TEXT_MUTED);
        ImGui::TextWrapped("When enabled, ESP, Aimbot and Triggerbot ignore teammates. Disable only for testing.");
        ImGui::PopStyleColor();
        
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, TEXT_MUTED);
        ImGui::SetWindowFontScale(0.88f);
        ImGui::TextWrapped("Triggerbot fires when crosshair is on an enemy. Use a small delay (e.g. 50-100 ms) for a more natural feel.");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();
        
        ImGui::EndGroup();
        ImGui::EndChild();
    }

    // Misc Tab
    void RenderMiscTab() {
        edited::BeginChild("MiscSettings", ImVec2(TAB_LEFT_PANEL_WIDTH, 0));
        
        ImGui::SetCursorPos(ImVec2(16, 12));
        ImGui::BeginGroup();
        
        ImGui::PushStyleColor(ImGuiCol_Text, ACCENT_PRIMARY);
        ImGui::SetWindowFontScale(0.95f);
        ImGui::Text("MISC");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::Spacing();

        edited::Checkbox("Damage Indicator", "Show damage dealt", &g_bDamageIndicatorEnabled);
        edited::Checkbox("Bomb Timer", "Show C4 timer", &g_bBombTimerEnabled);
        edited::Checkbox("Team Check", "Ignore teammates", &g_bTeamCheck);
        ImGui::Spacing();
        
        edited::Checkbox("FOV Changer", "Change view FOV", &g_bFovChangerEnabled);
        if (g_bFovChangerEnabled) {
            edited::SliderInt("FOV Value", "Target FOV", &g_nFovValue, 60, 140);
        }
        ImGui::Separator();
        TabSectionHeader("WATERMARK / STATUS");
        edited::Checkbox("Show Watermark", "Display cheat name", &g_bShowWatermark);
        edited::Checkbox("Show FPS & Ping", "Display performance info", &g_bShowFpsPing);

        edited::Checkbox("Bullet Tracers", "Draw lines for bullets", &g_bBulletTracersEnabled);
        if (g_bBulletTracersEnabled) {
             edited::ColorEdit4("Tracer Color", "Color of tracers", (float*)&g_colorBulletTracer);
             edited::SliderFloat("Tracer duration", "Duration in seconds", &g_fBulletTracerDuration, 0.5f, 5.0f, "%.1f");
        }
        
        ImGui::EndGroup();
        edited::EndChild();

        ImGui::SameLine();
        RenderMiscPreview();
    }

    // Keybind Manager Tab
    void RenderKeybindTab() {
        edited::BeginChild("KeybindSettings", ImVec2(TAB_LEFT_PANEL_WIDTH, 0));
        
        ImGui::SetCursorPos(ImVec2(16, 12));
        ImGui::BeginGroup();
        
        ImGui::PushStyleColor(ImGuiCol_Text, ACCENT_PRIMARY);
        ImGui::SetWindowFontScale(0.95f);
        ImGui::Text("KEYBIND MANAGER");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();
        ImGui::Spacing();
        
        edited::Checkbox("Show keybinds", "Overlay in-game", &g_bKeybindOverlayIngame);
        
        edited::Keybind("Aimbot Key", "Key to activate aimbot", &g_nAimbotKey);
        
        ImGui::EndGroup();
        edited::EndChild();
        ImGui::SameLine();
        RenderKeybindOverview(0);
    }
    
    void RenderKeybindOverview(float widthLeft) {
        ImVec2 size = (widthLeft > 0) ? ImVec2(widthLeft, 0) : ImVec2(0, 0);
        ImGui::BeginChild("KeybindOverview", size, true, ImGuiWindowFlags_NoScrollbar);
        
        ImDrawList* draw = ImGui::GetWindowDrawList();
        ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
        ImVec2 canvas_size = ImGui::GetContentRegionAvail();
        
        // Multi-color background removed for cleaner look
        draw->AddRectFilled(canvas_pos, ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y), IM_COL32(10, 14, 22, 255));
        
        ImGui::SetCursorPos(ImVec2(16, 12));
        ImGui::BeginGroup();
        
        ImGui::PushStyleColor(ImGuiCol_Text, ACCENT_PRIMARY);
        ImGui::SetWindowFontScale(1.05f);
        ImGui::Text("ALL KEYBINDS");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(ACCENT_PRIMARY.x, ACCENT_PRIMARY.y, ACCENT_PRIMARY.z, 0.5f));
        ImGui::Separator();
        ImGui::PopStyleColor();
        ImGui::Spacing();
        
        struct KeybindEntry { const char* function; const char* key; bool highlight; };
        KeybindEntry entries[] = {
            { "Menu Toggle", "INSERT", false },
            { "Unload", "END", false },
            { "Aimbot", g_szCurrentKeyName.c_str(), true },
        };
        
        ImGui::PushStyleColor(ImGuiCol_Text, SECTION_HEADER);
        ImGui::Text("%-20s  %s", "Function", "Key");
        ImGui::PopStyleColor();
        ImGui::Separator();
        ImGui::Spacing();
        
        for (const auto& e : entries) {
            if (e.highlight) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.9f, 1.0f, 1.0f));
            ImGui::Text("%-20s  %s", e.function, e.key);
            if (e.highlight) ImGui::PopStyleColor();
        }
        
        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, TEXT_MUTED);
        ImGui::SetWindowFontScale(0.88f);
        ImGui::TextWrapped("Highlighted keybinds can be changed in the left panel.");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();
        
        ImGui::EndGroup();
        ImGui::EndChild();
    }
    
    void RenderKeybindOverlayIngame() {
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        ImGuiIO& io = ImGui::GetIO();
        float sw = io.DisplaySize.x;
        float sh = io.DisplaySize.y;
        
        struct KeybindRow { const char* func; std::string key; };
        std::vector<KeybindRow> activeBinds;
        
        activeBinds.push_back({ "Menu", "INSERT" });
        activeBinds.push_back({ "Unload", "END" });
        
        if (g_bAimbotEnabled) {
            activeBinds.push_back({ "Aimbot", g_szCurrentKeyName });
        }
        
        // Bunnyhop was removed earlier, but the overlay still had it hardcoded.
        // We only show what's actually relevant now.
        
        if (activeBinds.empty()) return;

        float maxFuncW = 0, maxKeyW = 0;
        for (const auto& bind : activeBinds) {
            ImVec2 f = ImGui::CalcTextSize(bind.func);
            ImVec2 k = ImGui::CalcTextSize(bind.key.c_str());
            if (f.x > maxFuncW) maxFuncW = f.x;
            if (k.x > maxKeyW) maxKeyW = k.x;
        }
        
        float pad = 12.0f;
        float boxW = pad + maxFuncW + 24.0f + maxKeyW + pad;
        float boxH = pad + 20.0f + (activeBinds.size() * 18.0f) + pad;
        float x = 20.0f;
        float y = (sh - boxH) * 0.5f;
        
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + boxW, y + boxH), IM_COL32(12, 14, 18, 235));
        dl->AddRect(ImVec2(x, y), ImVec2(x + boxW, y + boxH), IM_COL32(0, 180, 255, 180));
        dl->AddText(ImVec2(x + pad, y + 4), IM_COL32(0, 200, 255, 255), "KEYBINDS");
        y += 24.0f;
        
        for (const auto& bind : activeBinds) {
            dl->AddText(ImVec2(x + pad, y), IM_COL32(200, 205, 220, 255), bind.func);
            dl->AddText(ImVec2(x + boxW - pad - maxKeyW, y), IM_COL32(0, 220, 255, 255), bind.key.c_str());
            y += 18.0f;
        }
    }

    void RenderWatermark() {
        ImGuiIO& io = ImGui::GetIO();
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        
        char buf[128];
        std::string text = "";
        
        if (g_bShowWatermark) {
            text += "PXL | CS2 Internal";
        }
        
        if (g_bShowFpsPing) {
            if (!text.empty()) text += " | ";
            
            float fps = io.Framerate;
            int ping = 0;
            
            // Try to get ping from local controller
            uintptr_t clientBase = (uintptr_t)GetModuleHandleA("client.dll");
            if (clientBase) {
                uintptr_t localController = *(uintptr_t*)(clientBase + cs2_dumper::offsets::client_dll::dwLocalPlayerController);
                if (localController) {
                    ping = *(int*)(localController + 0x720); // m_iPing offset
                }
            }
            
            snprintf(buf, sizeof(buf), "FPS: %.0f | Ping: %d ms", fps, ping);
            text += buf;
        }
        
        if (text.empty()) return;
        
        ImVec2 ts = ImGui::CalcTextSize(text.c_str());
        float padding = 10.0f;
        float boxW = ts.x + padding * 2.0f;
        float boxH = ts.y + padding;
        
        float x = io.DisplaySize.x - boxW - 20.0f;
        float y = 20.0f;
        
        // Premium box design: transparent dark bg with gradient border
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + boxW, y + boxH), IM_COL32(10, 12, 16, 200), 4.0f);
        
        // Animated gradient border
        float time = (float)ImGui::GetTime();
        ImVec4 colA = ImVec4(0.0f, 0.7f, 1.0f, 0.8f); // Cyan
        ImVec4 colB = ImVec4(0.5f, 0.0f, 1.0f, 0.8f); // Purple
        
        float t = (sinf(time * 2.0f) + 1.0f) * 0.5f;
        ImU32 borderCol = ImGui::GetColorU32(ImVec4(
            colA.x + (colB.x - colA.x) * t,
            colA.y + (colB.y - colA.y) * t,
            colA.z + (colB.z - colA.z) * t,
            colA.w
        ));
        
        dl->AddRect(ImVec2(x, y), ImVec2(x + boxW, y + boxH), borderCol, 4.0f, 0, 1.5f);
        
        // Text with subtle glow
        dl->AddText(ImVec2(x + padding + 1, y + padding / 2 + 1), IM_COL32(0, 0, 0, 150), text.c_str());
        dl->AddText(ImVec2(x + padding, y + padding / 2), IM_COL32(230, 240, 255, 255), text.c_str());
    }

    // Config tab state
    static std::vector<std::string> g_configNames;
    static int g_nSelectedConfig = -1;
    static char g_szNewConfigName[128] = "New Config";

    void RenderConfigTab() {
        edited::BeginChild("ConfigSettings", ImVec2(TAB_LEFT_PANEL_WIDTH, 0));
        
        ImGui::SetCursorPos(ImVec2(16, 12));
        ImGui::BeginGroup();
        
        ImGui::PushStyleColor(ImGuiCol_Text, ACCENT_PRIMARY);
        ImGui::SetWindowFontScale(0.95f);
        ImGui::Text("CONFIG MANAGER");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        
        float totalWidth = ImGui::GetContentRegionAvail().x;
        
        TabSectionHeader("Config Name");
        ImGui::SetNextItemWidth(totalWidth - 32);
        ImGui::InputText("##ConfigName", g_szNewConfigName, sizeof(g_szNewConfigName));
        
        ImGui::Spacing();
        TabSectionHeader("Actions");
        if (ImGui::Button("Save Config", ImVec2(totalWidth - 32, 36))) {
            if (g_szNewConfigName[0]) {
                std::string name(g_szNewConfigName);
                if (std::find(g_configNames.begin(), g_configNames.end(), name) == g_configNames.end())
                    g_configNames.push_back(name);
                g_nSelectedConfig = (int)(g_configNames.size() - 1);
            }
        }
        ImGui::Spacing();
        if (ImGui::Button("Load Config", ImVec2(totalWidth - 32, 36))) {
            if (g_nSelectedConfig >= 0 && g_nSelectedConfig < (int)g_configNames.size())
                snprintf(g_szNewConfigName, sizeof(g_szNewConfigName), "%s", g_configNames[g_nSelectedConfig].c_str());
        }
        ImGui::Spacing();
        if (ImGui::Button("Delete Config", ImVec2(totalWidth - 32, 36))) {
            if (g_nSelectedConfig >= 0 && g_nSelectedConfig < (int)g_configNames.size()) {
                g_configNames.erase(g_configNames.begin() + g_nSelectedConfig);
                g_nSelectedConfig = -1;
            }
        }
        ImGui::EndGroup();
        edited::EndChild();

        ImGui::SameLine();
        
        // Right Column: Config List
        edited::BeginChild("ConfigList", ImVec2(0, 0));
        TabSectionHeader("Saved Configs");
        
        for (size_t i = 0; i < g_configNames.size(); i++) {
            bool sel = (g_nSelectedConfig == (int)i);
            // edited::Selectable(const char* label, bool selected = false, ImGuiSelectableFlags flags = 0, const ImVec2& size = ImVec2(0, 0));
            ImGui::PushID((int)i);
            if (edited::Selectable(g_configNames[i].c_str(), sel, 0, ImVec2(0, 32))) 
                g_nSelectedConfig = (int)i;
            ImGui::PopID();
        }
        edited::EndChild();
    }

    // Active tab state
    static int active_tab = 0;

    void RenderMenu() {
        // Ensure fonts are set up for imgui_edited
        if (!font::lexend_bold && ImGui::GetIO().Fonts->Fonts.Size > 0) {
            // Fallback: use first loaded font for all if not set
            font::lexend_bold = ImGui::GetIO().Fonts->Fonts[0];
            font::lexend_regular = font::lexend_bold;
            font::icomoon = font::lexend_bold;
            font::icomoon_widget = font::lexend_bold;
        }

        static bool styleApplied = false;
        if (!styleApplied) {
            ImGui::StyleColorsDark();
            styleApplied = true;
        }
        
        ImGui::SetNextWindowSize(c::background::size);
        ImGui::Begin("Menu", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar);
        {
            const ImVec2& pos = ImGui::GetWindowPos();
            const ImVec2& region = ImGui::GetContentRegionMax();

            // Background
            ImGui::GetBackgroundDrawList()->AddRectFilled(pos, pos + region, ImGui::GetColorU32(c::background::filling), c::background::rounding);
            ImGui::GetBackgroundDrawList()->AddRect(pos, pos + region, ImGui::GetColorU32(c::background::stroke), c::background::rounding);

            // Decoration lines
            ImGui::GetBackgroundDrawList()->AddRectFilledMultiColor(pos + ImVec2(0, region.y / 2), pos + ImVec2(2, region.y), ImGui::GetColorU32(c::accent), ImGui::GetColorU32(c::accent), ImGui::GetColorU32(c::background::filling), ImGui::GetColorU32(c::background::filling));
            ImGui::GetBackgroundDrawList()->AddRectFilledMultiColor(pos + ImVec2(0, 0), pos + ImVec2(2, region.y / 2), ImGui::GetColorU32(c::background::filling), ImGui::GetColorU32(c::background::filling), ImGui::GetColorU32(c::accent), ImGui::GetColorU32(c::accent));

            ImGui::GetBackgroundDrawList()->AddRectFilledMultiColor(pos + ImVec2(region.x - 2, region.y / 2), pos + ImVec2(region.x, region.y), ImGui::GetColorU32(c::accent), ImGui::GetColorU32(c::accent), ImGui::GetColorU32(c::background::filling), ImGui::GetColorU32(c::background::filling));
            ImGui::GetBackgroundDrawList()->AddRectFilledMultiColor(pos + ImVec2(region.x - 2, 0), pos + ImVec2(region.x, region.y / 2), ImGui::GetColorU32(c::background::filling), ImGui::GetColorU32(c::background::filling), ImGui::GetColorU32(c::accent), ImGui::GetColorU32(c::accent));

            // Logo / Title
            if(font::lexend_bold) ImGui::PushFont(font::lexend_bold);
            ImGui::GetWindowDrawList()->AddText(font::lexend_bold, 20.f, pos + ImVec2(20, 15), ImGui::GetColorU32(c::elements::text_active), "CS2 INTERNAL");
            if(font::lexend_bold) ImGui::PopFont();

            // Tabs
            ImGui::SetCursorPos(ImVec2(20, 55));
            ImGui::BeginGroup();
            {
                ImGui::PushID(0);
                if (edited::Tab(active_tab == 0, "b", "Combat", "Aim assistance", ImVec2(150, 40))) active_tab = 0;
                ImGui::PopID();
                ImGui::Spacing();
                
                ImGui::PushID(1);
                if (edited::Tab(active_tab == 1, "c", "Visuals", "ESP, C4, Tracers", ImVec2(150, 40))) active_tab = 1;
                ImGui::PopID();
                ImGui::Spacing();
                
                ImGui::PushID(2);
                if (edited::Tab(active_tab == 2, "g", "Misc", "Other features", ImVec2(150, 40))) active_tab = 2;
                ImGui::PopID();
                ImGui::Spacing();
                
                ImGui::PushID(3);
                if (edited::Tab(active_tab == 3, "e", "Settings", "Menu config", ImVec2(150, 40))) active_tab = 3;
                ImGui::PopID();
            }
            ImGui::EndGroup();

            // Content Area
            ImGui::SetCursorPos(ImVec2(190, 55));
            
            if (active_tab == 0) // Legit -> Combat
            {
                edited::BeginChild("##Legit1", ImVec2(c::background::size.x - 230, c::background::size.y - 80), 0, ImGuiWindowFlags_AlwaysVerticalScrollbar);
                {
                    ImGui::TextColored(ImColor(ImGui::GetColorU32(c::elements::text)), "Aimbot");
                    
                    edited::Checkbox("Enable Aimbot", "Master switch", &g_bAimbotEnabled);
                    // Silent Aim Removed
                    
                    if (g_bAimbotEnabled) {
                        edited::SliderFloat("FOV", "Field of view", &g_fAimbotFov, 0.1f, 180.0f, "%.1f");
                        edited::SliderFloat("Smooth", "Aim smoothing", &g_fAimbotSmooth, 1.0f, 20.0f, "%.1f");
                        
                        const char* bones[] = { "Head", "Neck", "Chest", "Stomach", "Pelvis" };
                        static int bone_idx = 0;
                        if (g_nAimbotBone == 6) bone_idx = 0;
                        else if (g_nAimbotBone == 5) bone_idx = 1;
                        else if (g_nAimbotBone == 4) bone_idx = 2;
                        else if (g_nAimbotBone == 2) bone_idx = 3;
                        else bone_idx = 4;
                        
                        if (edited::Combo("Bone", "Target hitbox", &bone_idx, bones, 5)) {
                            switch(bone_idx) {
                                case 0: g_nAimbotBone = 6; break;
                                case 1: g_nAimbotBone = 5; break;
                                case 2: g_nAimbotBone = 4; break;
                                case 3: g_nAimbotBone = 2; break;
                                case 4: g_nAimbotBone = 0; break;
                            }
                        }
                        
                        edited::Checkbox("Visible Only", "Only aim at visible", &g_bAimbotVisibleOnly);
                        edited::Checkbox("Draw FOV", "Draw FOV circle", &g_bAimbotFovCircle);
                        edited::Checkbox("RCS", "Recoil Control System", &g_bAimbotRcs);
                    }

                    ImGui::Spacing();
                    ImGui::TextColored(ImColor(ImGui::GetColorU32(c::elements::text)), "Triggerbot");
                    
                    edited::Checkbox("Enable Triggerbot", "Auto fire when on target", &g_bTriggerbotEnabled);
                    if (g_bTriggerbotEnabled) {
                        edited::SliderInt("Delay", "Shot delay (ms)", &g_nTriggerbotDelay, 0, 500, "%d ms");
                    }
                    
                    ImGui::Spacing();
                    ImGui::TextColored(ImColor(ImGui::GetColorU32(c::elements::text)), "Recoil");
                    
                    static bool rcs_standalone = true; 
                    if(edited::Checkbox("Standalone RCS", "Recoil control without aimbot", &g_RCS.enabled)) {
                       g_bAimbotRcs = g_RCS.enabled;
                    }
                    if (g_RCS.enabled) {
                        edited::SliderFloat("RCS Strength", "Amount of control", &g_RCS.strength, 0.0f, 1.0f, "%.2f");
                    }
                }
                edited::EndChild();
            }
            else if (active_tab == 1) // Visuals
            {
                edited::BeginChild("##Visuals1", ImVec2((c::background::size.x - 230) / 2, c::background::size.y - 80), 0, ImGuiWindowFlags_AlwaysVerticalScrollbar);
                {
                    ImGui::TextColored(ImColor(ImGui::GetColorU32(c::elements::text)), "ESP");
                    edited::Checkbox("Enabled", "Master switch", &g_bEspEnabled);
                    edited::Checkbox("Boxes", "Draw 2D boxes", &g_bEspBoxes);
                    edited::Checkbox("Skeleton", "Draw bone skeleton", &g_bEspSkeleton);
                    edited::Checkbox("Names", "Draw player names", &g_bEspNames);
                    edited::Checkbox("Health", "Draw health bar", &g_bEspHealth);
                    ImGui::Spacing();
                    ImGui::TextColored(ImColor(ImGui::GetColorU32(c::elements::text)), "Colors");
                    ImGui::ColorEdit4("Box", (float*)&g_colorEspBox, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
                    ImGui::ColorEdit4("Skeleton", (float*)&g_colorEspSkeleton, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
                    ImGui::ColorEdit4("Name", (float*)&g_colorEspName, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
                    ImGui::Spacing();
                    ImGui::TextColored(ImColor(ImGui::GetColorU32(c::elements::text)), "Other");
                    edited::Checkbox("C4 ESP", "Bomb timer/loc", &g_bC4EspEnabled);
                    edited::Checkbox("Tracers", "Bullet tracers", &g_bBulletTracersEnabled);
                }
                edited::EndChild();
            }
            else if (active_tab == 2) // Misc
            {
                edited::BeginChild("##Misc1", ImVec2((c::background::size.x - 230) / 2, c::background::size.y - 80), 0, ImGuiWindowFlags_AlwaysVerticalScrollbar);
                {
                    ImGui::TextColored(ImColor(ImGui::GetColorU32(c::elements::text)), "General");
                    
                    edited::Checkbox("FOV Changer", "Custom view FOV", &g_bFovChangerEnabled);
                    if (g_bFovChangerEnabled) {
                        edited::SliderInt("View FOV", "Camera Field of View", &g_nFovValue, 60, 150, "%d");
                    }
                    ImGui::Spacing();
                    edited::Checkbox("Team Check", "Ignore teammates", &g_bTeamCheck);
                    edited::Checkbox("Watermark", "Show watermark", &g_bShowWatermark);
                    edited::Checkbox("FPS & Ping", "Show stats", &g_bShowFpsPing);
                    edited::Checkbox("Keybinds List", "Show active keybinds", &g_bKeybindOverlayIngame);

                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    static int key = g_nAimbotKey;
                    if (edited::Keybind("Aimbot Key", "Key to hold for aimbot", &key)) {
                        g_nAimbotKey = key;
                    }
                }
                edited::EndChild();
            }
            else if (active_tab == 3) // Settings
            {
                edited::BeginChild("##Settings1", ImVec2((c::background::size.x - 230) / 2, c::background::size.y - 80), 0, ImGuiWindowFlags_AlwaysVerticalScrollbar);
                {
                    ImGui::TextColored(ImColor(ImGui::GetColorU32(c::elements::text)), "Menu Config");
                    
                    
                    if (ImGui::Button("Unload Cheat", ImVec2(150, 30))) {
                        // Safe Unload sequence
                        g_bRunning = false; 
                        // Wait for threads to finish their current iteration
                        Sleep(100);
                        // Shutdown MinHook and restore WNDPROC
                        Shutdown();
                        // Free library handled in Shutdown? No. Let's create a thread to free library.
                        CreateThread(nullptr, 0, [](LPVOID) -> DWORD { 
                            Sleep(100); 
                            FreeLibraryAndExitThread(g_hModule, 0); 
                            return 0; 
                        }, nullptr, 0, nullptr);
                    }
                    
                    ImGui::TextDisabled("Aternos GUI ported to CS2 Internal");
                }
                edited::EndChild();
            }
            
        }
        ImVec2 menuPos = ImGui::GetWindowPos();
        ImVec2 menuSize = ImGui::GetWindowSize();
        ImGui::End();

        // ESP Preview: fixed size, no resize/move/scroll – box with padding, figure slightly smaller
        const float PREVIEW_W = 440.f, PREVIEW_H = 560.f;
        ImGui::SetNextWindowSize(ImVec2(PREVIEW_W, PREVIEW_H), ImGuiCond_Always);
        ImGui::SetNextWindowPos(ImVec2(menuPos.x + menuSize.x + 12, menuPos.y), ImGuiCond_Always);
        if (ImGui::Begin("ESP Preview", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
            ID3D11ShaderResourceView* previewTex = g_pEspPreviewFigureTexture ? g_pEspPreviewFigureTexture : g_pEspPreviewTexture;
            if (previewTex) {
                ImDrawList* draw = ImGui::GetWindowDrawList();
                ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
                ImVec2 canvas_size = ImVec2(PREVIEW_W - 20, PREVIEW_H - 40);
                ImGui::Dummy(canvas_size);
                float aspect = (g_espFigureTexW > 0 && g_espFigureTexH > 0)
                    ? (float)g_espFigureTexW / (float)g_espFigureTexH
                    : 0.46f;
                // Figur etwas kleiner (~74% der Höhe)
                float playerHeight = canvas_size.y * 0.74f;
                float playerWidth = playerHeight * aspect;
                if (playerWidth > canvas_size.x * 0.85f) {
                    playerWidth = canvas_size.x * 0.85f;
                    playerHeight = playerWidth / aspect;
                }
                ImVec2 center(canvas_pos.x + canvas_size.x * 0.5f, canvas_pos.y + canvas_size.y * 0.5f);
                ImVec2 topLeft(center.x - playerWidth * 0.5f, center.y - playerHeight * 0.5f);
                ImVec2 bottomRight(center.x + playerWidth * 0.5f, center.y + playerHeight * 0.5f);
                draw->AddImage((ImTextureID)previewTex, topLeft, bottomRight);
                // Box: oben näher am Kopf, unten bisschen runter; Name + Health hängen an boxMin/boxMax
                const float boxPadTop = 16.f, boxPadBottom = 12.f, boxPadSide = 14.f;
                ImVec2 boxMin(topLeft.x - boxPadSide, topLeft.y - boxPadTop);
                ImVec2 boxMax(bottomRight.x + boxPadSide, bottomRight.y + boxPadBottom);
                if (g_bEspBoxes) {
                    ImU32 boxColor = ImGui::ColorConvertFloat4ToU32(g_colorEspBox);
                    draw->AddRect(ImVec2(boxMin.x - 1, boxMin.y - 1), ImVec2(boxMax.x + 1, boxMax.y + 1), IM_COL32(0, 180, 255, 60), 0.0f, 0, 3.0f);
                    draw->AddRect(boxMin, boxMax, boxColor, 0.0f, 0, 2.5f);
                }
                if (g_bEspHealth) {
                    float healthPercent = 0.75f;
                    ImVec2 healthBarStart(boxMin.x - 8, boxMin.y);
                    ImVec2 healthBarEnd(boxMin.x - 3, boxMax.y);
                    draw->AddRectFilled(healthBarStart, healthBarEnd, IM_COL32(25, 28, 35, 220));
                    ImVec2 healthStart(boxMin.x - 7, boxMax.y - 1);
                    ImVec2 healthEnd(boxMin.x - 4, boxMax.y - (boxMax.y - boxMin.y) * healthPercent);
                    draw->AddRectFilledMultiColor(healthStart, healthEnd, IM_COL32(0, 200, 100, 255), IM_COL32(0, 200, 100, 255), IM_COL32(80, 255, 150, 255), IM_COL32(80, 255, 150, 255));
                    draw->AddRect(healthBarStart, healthBarEnd, IM_COL32(0, 0, 0, 255), 0.0f, 0, 1.5f);
                }
                if (g_bEspNames) {
                    ImU32 nameColor = ImGui::ColorConvertFloat4ToU32(g_colorEspName);
                    const char* name = "Enemy Player";
                    ImVec2 textSize = ImGui::CalcTextSize(name);
                    ImVec2 namePos(center.x - textSize.x * 0.5f, boxMin.y - 20);
                    draw->AddText(ImVec2(namePos.x + 1, namePos.y + 1), IM_COL32(0, 0, 0, 200), name);
                    draw->AddText(namePos, nameColor, name);
                }
                if (g_bEspSkeleton) {
                    float skelW = playerWidth * 0.88f, skelH = playerHeight * 0.88f;
                    DrawSkeletonPreviewRect(draw, center, skelW, skelH, g_colorEspSkeleton);
                }
            } else {
                ImGui::Text("Place cs2_figure.png next to the DLL for preview.");
            }
        }
        ImGui::End();
    }

    void RenderESP() {
        // Get game modules and offsets
        uintptr_t clientBase = (uintptr_t)GetModuleHandleA("client.dll");
        if (!clientBase) return;

        // Get entity list
        uintptr_t entityList = *(uintptr_t*)(clientBase + cs2_dumper::offsets::client_dll::dwEntityList);
        if (!entityList) return;

        // Get local player pawn
        uintptr_t localPawn = *(uintptr_t*)(clientBase + cs2_dumper::offsets::client_dll::dwLocalPlayerPawn);
        if (!localPawn) return;

        // Get local team
        int localTeam = *(uint8_t*)(localPawn + 0x3F3); // m_iTeamNum

        // Get view matrix
        view_matrix_t viewMatrix;
        memcpy(&viewMatrix, (void*)(clientBase + cs2_dumper::offsets::client_dll::dwViewMatrix), sizeof(view_matrix_t));

        // Get screen size
        ImGuiIO& io = ImGui::GetIO();
        int screenWidth = (int)io.DisplaySize.x;
        int screenHeight = (int)io.DisplaySize.y;

        // Get background draw list
        ImDrawList* drawList = ImGui::GetBackgroundDrawList();

        // Iterate entities
        for (int i = 1; i <= 64; i++) {
            // Get list entry
            uintptr_t listEntry = *(uintptr_t*)(entityList + 0x10 + 8 * (i >> 9));
            if (!listEntry) continue;

            // Get controller (STRIDE IS 0x70!)
            uintptr_t controller = *(uintptr_t*)(listEntry + 0x70 * (i & 0x1FF));
            if (!controller) continue;

            // Get pawn handle
            uint32_t pawnHandle = *(uint32_t*)(controller + 0x6C4); // m_hPawn
            if (!pawnHandle) continue;

            // Get pawn entry
            uintptr_t pawnEntry = *(uintptr_t*)(entityList + 0x10 + 8 * ((pawnHandle & 0x7FFF) >> 9));
            if (!pawnEntry) continue;

            // Get pawn
            uintptr_t pawn = *(uintptr_t*)(pawnEntry + 0x70 * (pawnHandle & 0x1FF));
            if (!pawn || pawn == localPawn) continue;

            // Read pawn data
            int health = *(int*)(pawn + 0x354); // m_iHealth
            int team = *(uint8_t*)(pawn + 0x3F3); // m_iTeamNum
            uint8_t lifeState = *(uint8_t*)(pawn + 0x35C); // m_lifeState

            // Skip dead players
            if (health <= 0 || lifeState != 0) continue;
            
            // Team check: skip teammates if enabled
            if (g_bTeamCheck && team == localTeam) continue;

            // Get game scene node for position
            uintptr_t gameSceneNode = *(uintptr_t*)(pawn + 0x338); // m_pGameSceneNode
            if (!gameSceneNode) continue;

            // Get origin (vec origin is at offset 0xD0 in CGameSceneNode)
            Vector3 origin = *(Vector3*)(gameSceneNode + 0xD0);

            // World to screen
            Vector2 screenPos;
            Vector2 headScreenPos;
            Vector3 headPos = GetBonePosition(pawn, 6); // Use actual head bone position
            Vector3 headPosTop = headPos;
            headPosTop.z += 8.0f; // Add a small offset to the top of the box

            if (!WorldToScreen(origin, screenPos, viewMatrix, screenWidth, screenHeight)) continue;
            if (!WorldToScreen(headPosTop, headScreenPos, viewMatrix, screenWidth, screenHeight)) continue;

            // Calculate box dimensions
            float boxHeight = screenPos.y - headScreenPos.y;
            float boxWidth = boxHeight * 0.5f;

            // Team color
            ImU32 color = (team == 2) ? IM_COL32(255, 100, 100, 255) : IM_COL32(100, 100, 255, 255); // T=red, CT=blue

            // Draw box
            if (g_bEspBoxes) {
                drawList->AddRect(
                    ImVec2(headScreenPos.x - boxWidth / 2, headScreenPos.y),
                    ImVec2(headScreenPos.x + boxWidth / 2, screenPos.y),
                    color, 0.0f, 0, 2.0f
                );
            }

            // Draw Tracer ESP
            if (g_bEspTracers) {
                ImU32 tracerColor = ImGui::ColorConvertFloat4ToU32(g_colorEspTracer);
                drawList->AddLine(
                    ImVec2((float)screenWidth * 0.5f, (float)screenHeight),
                    ImVec2(screenPos.x, screenPos.y),
                    tracerColor, 1.5f
                );
            }

            // Draw health bar
            if (g_bEspHealth) {
                float healthPercent = health / 100.0f;
                ImU32 healthColor = IM_COL32(255 * (1 - healthPercent), 255 * healthPercent, 0, 255);
                
                drawList->AddRectFilled(
                    ImVec2(headScreenPos.x - boxWidth / 2 - 6, screenPos.y),
                    ImVec2(headScreenPos.x - boxWidth / 2 - 2, screenPos.y - boxHeight * healthPercent),
                    healthColor
                );
                drawList->AddRect(
                    ImVec2(headScreenPos.x - boxWidth / 2 - 6, headScreenPos.y),
                    ImVec2(headScreenPos.x - boxWidth / 2 - 2, screenPos.y),
                    IM_COL32(0, 0, 0, 255)
                );
            }

            // Draw name
            if (g_bEspNames) {
                // Get player name from controller
                const char* name = (const char*)(controller + 0x6F8); // m_iszPlayerName
                if (name && name[0]) {
                    ImVec2 textSize = ImGui::CalcTextSize(name);
                    ImU32 nameColor = ImGui::ColorConvertFloat4ToU32(g_colorEspName);
                    drawList->AddText(
                        ImVec2(headScreenPos.x - textSize.x / 2, headScreenPos.y - 15),
                        nameColor,
                        name
                    );
                }
            }
            
            // Draw skeleton
            if (g_bEspSkeleton) {
                // Bone connections for CS2
                const int bones[][2] = {
                    {6, 5},   // Head to neck
                    {5, 4},   // Neck to upper spine
                    {4, 2},   // Upper spine to pelvis
                    {5, 8},   // Neck to left shoulder
                    {5, 13},  // Neck to right shoulder
                    {8, 9},   // Left shoulder to elbow
                    {13, 14}, // Right shoulder to elbow
                    {9, 10},  // Left elbow to hand
                    {14, 15}, // Right elbow to hand
                    {2, 22},  // Pelvis to left hip
                    {2, 25},  // Pelvis to right hip
                    {22, 23}, // Left hip to knee
                    {25, 26}, // Right hip to knee
                    {23, 24}, // Left knee to foot
                    {26, 27}  // Right knee to foot
                };
                
                ImU32 skeletonColor = ImGui::ColorConvertFloat4ToU32(g_colorEspSkeleton);
                
                for (const auto& bone : bones) {
                    Vector3 pos1 = GetBonePosition(pawn, bone[0]);
                    Vector3 pos2 = GetBonePosition(pawn, bone[1]);
                    
                    if (pos1.x == 0 && pos1.y == 0) continue;
                    if (pos2.x == 0 && pos2.y == 0) continue;
                    
                    Vector2 screen1, screen2;
                    if (WorldToScreen(pos1, screen1, viewMatrix, screenWidth, screenHeight) &&
                        WorldToScreen(pos2, screen2, viewMatrix, screenWidth, screenHeight)) {
                        drawList->AddLine(ImVec2(screen1.x, screen1.y), ImVec2(screen2.x, screen2.y), skeletonColor, 2.0f);
                    }
                }
            }
        } // End of for loop

        // Draw Aimbot FOV circle
        if (g_bAimbotEnabled && g_bAimbotFovCircle) {
            drawList->AddCircle(
                ImVec2(screenWidth / 2.0f, screenHeight / 2.0f),
                g_fAimbotFov * 10.0f,
                IM_COL32(255, 255, 255, 100),
                64, 1.5f
            );
        }

        // Silent Aim FOV Removed

        // Draw Aimbot Snaplines
        if (g_bAimbotSnaplines && g_lockedPawn) {
            Vector3 targetPos = GetBonePosition(g_lockedPawn, g_nAimbotBone);
            Vector2 screenPos;
            if (WorldToScreen(targetPos, screenPos, viewMatrix, screenWidth, screenHeight)) {
                drawList->AddLine(
                    ImVec2(screenWidth / 2.0f, screenHeight / 2.0f),
                    ImVec2(screenPos.x, screenPos.y),
                    ImGui::ColorConvertFloat4ToU32(g_colorAimbotSnaplines),
                    1.5f
                );
            }
        }
    } // End of RenderESP function


    Vector3 GetBonePosition(uintptr_t pawn, int boneId) {
        uintptr_t gameSceneNode = *(uintptr_t*)(pawn + 0x338); // m_pGameSceneNode
        if (!gameSceneNode) return { 0, 0, 0 };
        
        uintptr_t boneArray = *(uintptr_t*)(gameSceneNode + 0x1E0); // dwBoneMatrix
        if (!boneArray) boneArray = *(uintptr_t*)(gameSceneNode + 0x1F0); // Try fallback
        if (!boneArray) return { 0, 0, 0 };
        
        return *(Vector3*)(boneArray + boneId * 32);
    }



    void RunAimbot() {
        g_bAimbotActive = (GetAsyncKeyState(g_nAimbotKey) & 0x8000);

        uintptr_t clientBase = (uintptr_t)GetModuleHandleA("client.dll");
        if (!clientBase) return;

        uintptr_t entityList = *(uintptr_t*)(clientBase + cs2_dumper::offsets::client_dll::dwEntityList);
        if (!entityList) return;

        uintptr_t localPawn = *(uintptr_t*)(clientBase + cs2_dumper::offsets::client_dll::dwLocalPlayerPawn);
        if (!localPawn) return;

        int localTeam = *(uint8_t*)(localPawn + 0x3F3);

        uintptr_t localSceneNode = *(uintptr_t*)(localPawn + 0x338);
        if (!localSceneNode) return;
        
        Vector3 localOrigin = *(Vector3*)(localSceneNode + 0xD0);
        Vector3 viewOffset = *(Vector3*)(localPawn + 0xD58);
        if (viewOffset.z < 10.0f) viewOffset.z = 64.0f;
        
        Vector3 eyePos = localOrigin + viewOffset;
        float viewMatrix[4][4];
        memcpy(viewMatrix, (void*)(clientBase + cs2_dumper::offsets::client_dll::dwViewMatrix), sizeof(viewMatrix));

        int screenWidth = (int)ImGui::GetIO().DisplaySize.x;
        int screenHeight = (int)ImGui::GetIO().DisplaySize.y;

        Vector2 screenCenter(screenWidth / 2.0f, screenHeight / 2.0f);
        QAngle currentAngles = *(QAngle*)(clientBase + cs2_dumper::offsets::client_dll::dwViewAngles);
        Vector3 aimPunch = *(Vector3*)(localPawn + 0x16CC); // m_aimPunchAngle

        float bestAimDist = g_fAimbotFov * 10.0f;
        QAngle bestAimAngle;
        uintptr_t bestAimPawn = 0;
        bool foundAimTarget = false;
        
        int targetBone = g_nAimbotBone;

        // Sticky Target check REMOVED to satisfy "always closest" request.
        // We now search for the best target every single frame.
        
        // Entity loop for new targets
        // if (!foundAimTarget) { // Removed condition
        for (int i = 0; i < 64; i++) {
            uintptr_t listEntry = *(uintptr_t*)(entityList + 0x10 + 8 * (i >> 9));
            if (!listEntry) continue;

            uintptr_t controller = *(uintptr_t*)(listEntry + 0x70 * (i & 0x1FF));
            if (!controller) continue;

            uint32_t pawnHandle = *(uint32_t*)(controller + 0x6C4);
            if (!pawnHandle) continue;

            uintptr_t pawnEntry = *(uintptr_t*)(entityList + 0x10 + 8 * ((pawnHandle & 0x7FFF) >> 9));
            if (!pawnEntry) continue;

            uintptr_t pawn = *(uintptr_t*)(pawnEntry + 0x70 * (pawnHandle & 0x1FF));
            if (!pawn || pawn == localPawn) continue;

            int health = *(int*)(pawn + 0x354);
            uint8_t lifeState = *(uint8_t*)(pawn + 0x35C);
            if (health <= 0 || lifeState != 0) continue;
            
            if (g_bTeamCheck) {
                int enemyTeamNum = *(uint8_t*)(pawn + 0x3F3);
                if (localTeam == enemyTeamNum) continue;
            }

            Vector3 targetPos = GetBonePosition(pawn, targetBone);
            if (targetPos.x == 0.0f && targetPos.y == 0.0f) continue;

            Vector2 targetScreen;
            bool onScreen = WorldToScreen(targetPos, targetScreen, viewMatrix, screenWidth, screenHeight);
            float dist = sqrtf(powf(targetScreen.x - screenCenter.x, 2) + powf(targetScreen.y - screenCenter.y, 2));

            if (dist < bestAimDist) {
                if (!g_bAimbotVisibleOnly || (*(uint32_t*)(pawn + 0x26E0 + 0xC))) {
                    bestAimDist = dist;
                    bestAimAngle = Vector3::CalculateAngle(eyePos, targetPos);
                    bestAimPawn = pawn;
                    foundAimTarget = true;
                }
            }
        }
        // }

        // Update global target for snaplines/silent aim
        if (foundAimTarget) {
            g_lockedPawn = bestAimPawn;
            // g_targetAngle = bestAimAngle; // REMOVED
            
            // Apply actual aiming ONLY if key is pressed
            if (g_bAimbotActive) {
                if (g_bAimbotRcs) {
                    bestAimAngle.x -= aimPunch.x * 2.0f;
                    bestAimAngle.y -= aimPunch.y * 2.0f;
                    // g_targetAngle = bestAimAngle; // REMOVED
                }

                bestAimAngle.Clamp();

                if (g_fAimbotSmooth <= 1.0f) {
                    currentAngles = bestAimAngle;
                } else {
                    QAngle delta = bestAimAngle - currentAngles;
                    delta.Clamp();
                    currentAngles.x += delta.x / g_fAimbotSmooth;
                    currentAngles.y += delta.y / g_fAimbotSmooth;
                }

                currentAngles.Clamp();
                g_targetAngle = currentAngles;
                *(QAngle*)(clientBase + cs2_dumper::offsets::client_dll::dwViewAngles) = currentAngles;
            }
        } else {
            g_lockedPawn = 0;
        }
    }


    // Static variables for triggerbot state and debug timing
    static DWORD lastTriggerbotLog = 0;
    static int lastCrosshairId = -1;

    void RunTriggerbot() {
        uintptr_t clientBase = (uintptr_t)GetModuleHandleA("client.dll");
        if (!clientBase) return;

        uintptr_t localPawn = *(uintptr_t*)(clientBase + cs2_dumper::offsets::client_dll::dwLocalPlayerPawn);
        if (!localPawn) return;

        // Get crosshair entity ID (m_iIDEntIndex in C_CSPlayerPawnBase)
        // offset 0x3EAC provided by user
        int crosshairId = *(int*)(localPawn + 0x3EAC);
        
        // Debug log every 500ms to avoid spam
        DWORD now = GetTickCount();
        if (now - lastTriggerbotLog > 500 || crosshairId != lastCrosshairId) {
            if (crosshairId > 0) {
                printf("[Triggerbot] CrosshairID: %d\n", crosshairId);
            }
            lastTriggerbotLog = now;
            lastCrosshairId = crosshairId;
        }

        // Entity IDs can be much higher than 64 in CS2!
        // Only filter out invalid (0 or negative)
        if (crosshairId <= 0) return;

        // Get entity at crosshair using the full entity handle
        uintptr_t entityList = *(uintptr_t*)(clientBase + cs2_dumper::offsets::client_dll::dwEntityList);
        if (!entityList) {
            printf("[Triggerbot] Entity list is null\n");
            return;
        }

        // Use correct entity lookup with crosshairId (mask with 0x7FFF for safety)
        int entityIndex = crosshairId & 0x7FFF;
        uintptr_t listEntry = *(uintptr_t*)(entityList + 0x10 + 8 * (entityIndex >> 9));
        if (!listEntry) {
            printf("[Triggerbot] List entry is null for ID %d (index %d)\n", crosshairId, entityIndex);
            return;
        }

        uintptr_t entity = *(uintptr_t*)(listEntry + 0x70 * (entityIndex & 0x1FF));
        if (!entity) {
            printf("[Triggerbot] Entity is null for ID %d (index %d)\n", crosshairId, entityIndex);
            return;
        }

        // Check entity health
        int entityHealth = *(int*)(entity + 0x354);
        if (entityHealth <= 0) {
            return; // Dead entity
        }

        // Check if enemy
        int localTeam = *(uint8_t*)(localPawn + 0x3F3);
        int entityTeam = *(uint8_t*)(entity + 0x3F3);

        printf("[Triggerbot] Entity HP: %d, LocalTeam: %d, EntityTeam: %d\n", entityHealth, localTeam, entityTeam);

        // Team check: only shoot if team check is disabled OR entity is on different team
        if (g_bTeamCheck && entityTeam == localTeam) {
            return; // Don't shoot teammates
        }
        
        if (entityTeam != 0) {
            printf("[Triggerbot] SHOOTING at target!\n");
            
            // Use SendInput for reliable input simulation
            INPUT inputs[2] = {};
            
            // Mouse down
            inputs[0].type = INPUT_MOUSE;
            inputs[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
            
            // Mouse up
            inputs[1].type = INPUT_MOUSE;
            inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
            
            // Send mouse down
            SendInput(1, &inputs[0], sizeof(INPUT));
            Sleep(30);
            // Send mouse up
            SendInput(1, &inputs[1], sizeof(INPUT));
        }
    }

    // Aimbot thread - runs independently from render loop
    DWORD WINAPI AimbotThread(LPVOID lpParam) {
        printf("[+] Aimbot thread started\n");
        
        while (g_bRunning) {
            if (g_bAimbotEnabled) {
                RunAimbot();
            }
            Sleep(1); // ~1000Hz polling rate
        }
        
        printf("[+] Aimbot thread exiting\n");
        return 0;
    }

    // Triggerbot thread - runs independently from render loop
    DWORD WINAPI TriggerbotThread(LPVOID lpParam) {
        printf("[+] Triggerbot thread started\n");
        
        while (g_bRunning) {
            if (g_bTriggerbotEnabled) {
                RunTriggerbot();
            }
            Sleep(5); // ~200Hz polling rate (enough for triggerbot)
        }
        
        printf("[+] Triggerbot thread exiting\n");
        return 0;
    }

    // No Recoil - Total stabilization: eliminates drift and shake
    void RunNoRecoil() {
        if (!g_RCS.enabled) {
            g_RCS.oldPunch = Vector3(0, 0, 0);
            return;
        }

        uintptr_t clientBase = (uintptr_t)GetModuleHandleA("client.dll");
        if (!clientBase) return;

        uintptr_t localPawn = *(uintptr_t*)(clientBase + cs2_dumper::offsets::client_dll::dwLocalPlayerPawn);
        if (!localPawn) {
            g_RCS.oldPunch = Vector3(0, 0, 0);
            return;
        }

        // 1) Remove camera shake (view punch)
        // Offset 0x1410 from C_BasePlayerPawn::m_pCameraServices
        uintptr_t pCameraServices = *(uintptr_t*)(localPawn + 0x1410); 
        if (pCameraServices) {
            *(Vector3*)(pCameraServices + 0x48) = Vector3(0, 0, 0); // m_vecCsViewPunchAngle
        }

        // 2) Advanced RCS with View Restoration
        int shotsFired = *(int*)(localPawn + 0x270C); // m_iShotsFired
        
        // Get view angles
        QAngle* pViewAngles = (QAngle*)(clientBase + cs2_dumper::offsets::client_dll::dwViewAngles);
        
        if (shotsFired > 0) {
            // Store original angles at start of spray (Shot 1)
            if (shotsFired == 1) {
                if (!g_RCS.wasAttacking) {
                    g_RCS.originalViewAngles = *pViewAngles;
                    g_RCS.wasAttacking = true;
                }
            } else {
                g_RCS.wasAttacking = true; // Ensure state is set if we join mid-spray
            }

            Vector3 aimPunch = *(Vector3*)(localPawn + 0x16CC); // m_aimPunchAngle

            if (shotsFired > 1) {
                // Calculate punch delta (old - current)
                Vector3 delta;
                delta.x = (g_RCS.oldPunch.x - aimPunch.x) * 2.0f;
                delta.y = (g_RCS.oldPunch.y - aimPunch.y) * 2.0f;
                delta.z = 0.0f;

                // Apply strength
                delta.x *= g_RCS.strength;
                delta.y *= g_RCS.strength;

                // Only vertical option
                if (g_RCS.onlyVertical) {
                    delta.y = 0.0f;
                }

                // Apply RCS
                pViewAngles->x += delta.x;
                pViewAngles->y += delta.y;
                
                pViewAngles->Clamp();
            }
            
            // Update old punch EVERY frame while shooting
            g_RCS.oldPunch = aimPunch;

        } else {
            // Restore original view angles when spray ends
            if (g_RCS.wasAttacking) {
                *pViewAngles = g_RCS.originalViewAngles; 
                g_RCS.wasAttacking = false;
            }
            g_RCS.oldPunch = Vector3(0, 0, 0);
        }
    }
    
    // FOV Changer - modify desired FOV on local controller AND camera services to prevent resets
    void RunFovChanger() {
        if (!g_bFovChangerEnabled) return;
        uintptr_t clientBase = (uintptr_t)GetModuleHandleA("client.dll");
        if (!clientBase) return;
        
        uintptr_t localPawn = *(uintptr_t*)(clientBase + cs2_dumper::offsets::client_dll::dwLocalPlayerPawn);
        if (!localPawn) return;

        // Force disable FOV changer if we are scoped to prevent flickering
        bool isScoped = *(bool*)(localPawn + cs2_dumper::schemas::client_dll::C_CSPlayerPawn::m_bIsScoped);
        if (isScoped) return;
        
        // Controller-side FOV
        uintptr_t localController = *(uintptr_t*)(clientBase + cs2_dumper::offsets::client_dll::dwLocalPlayerController);
        if (localController) {
            *(uint32_t*)(localController + 0x78C) = (uint32_t)g_nFovValue; // m_iDesiredFOV
        }
        
        // Pawn-side FOV - force camera services to prevent reset during reload/animation
        // CCSPlayer_CameraServices offset is 0x1410 in C_BasePlayerPawn
        uintptr_t cameraServices = *(uintptr_t*)(localPawn + 0x1410); 
        if (cameraServices) {
            // Set both FOV and FOVStart to prevent jitter/reset during reload/animation
            *(uint32_t*)(cameraServices + 0x290) = (uint32_t)g_nFovValue; // m_iFOV
            *(uint32_t*)(cameraServices + 0x294) = (uint32_t)g_nFovValue; // m_iFOVStart
        }
    }

    // C4 ESP + Bomb Timer - draw planted bomb with precise timer and defuse indicator
    void RenderC4ESP() {
        uintptr_t clientBase = (uintptr_t)GetModuleHandleA("client.dll");
        if (!clientBase) return;
        uintptr_t plantedC4 = *(uintptr_t*)(clientBase + cs2_dumper::offsets::client_dll::dwPlantedC4);
        if (!plantedC4) return;
        
        bool defused = *(bool*)(plantedC4 + 0x11C4);
        if (defused) return;
        
        uintptr_t globalVars = *(uintptr_t*)(clientBase + cs2_dumper::offsets::client_dll::dwGlobalVars);
        float curTime = globalVars ? *(float*)(globalVars + 0x10) : 0.0f;
        float blowTime = *(float*)(plantedC4 + 0x11A0);
        bool beingDefused = *(bool*)(plantedC4 + 0x11AC);
        float defuseCountDown = *(float*)(plantedC4 + 0x11C0);
        float defuseLength = *(float*)(plantedC4 + 0x11BC);
        float timeLeft = (curTime > 0 && blowTime > curTime) ? (blowTime - curTime) : 45.0f;
        
        ImGuiIO& io = ImGui::GetIO();
        int sw = (int)io.DisplaySize.x, sh = (int)io.DisplaySize.y;
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        
        if (g_bBombTimerEnabled) {
            char buf[128];
            if (beingDefused) {
                float defuseTimeLeft = defuseCountDown - curTime;
                snprintf(buf, sizeof(buf), "DEFUSING %.1fs | BOMB %.1fs", defuseTimeLeft > 0 ? defuseTimeLeft : 0, timeLeft);
            } else {
                snprintf(buf, sizeof(buf), "BOMB %.1fs", timeLeft > 0 ? timeLeft : 0);
            }
            ImVec2 ts = ImGui::CalcTextSize(buf);
            float x = sw - ts.x - 24;
            float y = 24.0f;
            dl->AddRectFilled(ImVec2(x - 8, y - 4), ImVec2((float)(sw - 12), y + ts.y + 8), IM_COL32(20, 20, 20, 220));
            dl->AddRect(ImVec2(x - 8, y - 4), ImVec2((float)(sw - 12), y + ts.y + 8), beingDefused ? IM_COL32(80, 255, 80, 255) : IM_COL32(255, 80, 80, 255));
            dl->AddText(ImVec2(x, y + 2), IM_COL32(255, 255, 255, 255), buf);
        }
        
        if (!g_bC4EspEnabled && !g_bBombTimerEnabled) return;
        if (!g_bC4EspEnabled) return;
        
        uintptr_t gameSceneNode = *(uintptr_t*)(plantedC4 + 0x338);
        if (!gameSceneNode) return;
        Vector3 bombPos = *(Vector3*)(gameSceneNode + 0xD0);
        
        view_matrix_t viewMatrix;
        memcpy(&viewMatrix, (void*)(clientBase + cs2_dumper::offsets::client_dll::dwViewMatrix), sizeof(view_matrix_t));
        
        Vector2 screenPos;
        if (!WorldToScreen(bombPos, screenPos, viewMatrix, sw, sh)) return;
        
        char buf[64];
        snprintf(buf, sizeof(buf), "BOMB %.1fs", timeLeft > 0 ? timeLeft : 0);
        ImVec2 ts = ImGui::CalcTextSize(buf);
        dl->AddRectFilled(ImVec2(screenPos.x - ts.x/2 - 4, screenPos.y - 20), ImVec2(screenPos.x + ts.x/2 + 4, screenPos.y + 4), IM_COL32(40, 40, 40, 200));
        dl->AddRect(ImVec2(screenPos.x - ts.x/2 - 4, screenPos.y - 20), ImVec2(screenPos.x + ts.x/2 + 4, screenPos.y + 4), IM_COL32(255, 80, 80, 255));
        dl->AddText(ImVec2(screenPos.x - ts.x/2, screenPos.y - 18), IM_COL32(255, 255, 255, 255), buf);
    }

    // Bullet Tracers - detect shots and draw trails
    void RunBulletTracers() {
        if (!g_bBulletTracersEnabled) return;
        uintptr_t clientBase = (uintptr_t)GetModuleHandleA("client.dll");
        if (!clientBase) return;
        uintptr_t localPawn = *(uintptr_t*)(clientBase + cs2_dumper::offsets::client_dll::dwLocalPlayerPawn);
        if (!localPawn) return;
        
        static int s_lastShotsFired = 0;
        int shotsFired = *(int*)(localPawn + 0x270C); // m_iShotsFired (UPDATED)
        
        // Reset if match changed or desync occurred
        if (shotsFired < s_lastShotsFired) s_lastShotsFired = 0;

        if (shotsFired > s_lastShotsFired && shotsFired > 0) {
            uintptr_t gameSceneNode = *(uintptr_t*)(localPawn + 0x338);
            if (gameSceneNode) {
                Vector3 origin = *(Vector3*)(gameSceneNode + 0xD0);
                Vector3 viewOffset = *(Vector3*)(localPawn + 0xD58);
                if (viewOffset.z < 10.0f) viewOffset.z = 64.0f;
                Vector3 eyePos = origin + viewOffset;
                
                QAngle ang = *(QAngle*)(clientBase + cs2_dumper::offsets::client_dll::dwViewAngles);
                
                // Add punch to tracer direction so it matches where bullets actually go
                Vector3 punch = *(Vector3*)(localPawn + 0x16CC);
                ang.x += punch.x * 2.0f;
                ang.y += punch.y * 2.0f;

                float radP = ang.x * (3.14159265f / 180.0f);
                float radY = ang.y * (3.14159265f / 180.0f);
                float cosP = cosf(radP), sinP = sinf(radP);
                float cosY = cosf(radY), sinY = sinf(radY);
                Vector3 fwd(cosP * cosY, cosP * sinY, -sinP);
                
                Vector3 endPos = eyePos + fwd * 10000.0f;
                g_bulletTracers.push_back({ eyePos, endPos, (float)ImGui::GetTime() });
            }
        }
        s_lastShotsFired = shotsFired;
        
        float now = (float)ImGui::GetTime();
        float duration = (g_fBulletTracerDuration > 0.1f) ? g_fBulletTracerDuration : 2.0f;
        g_bulletTracers.erase(std::remove_if(g_bulletTracers.begin(), g_bulletTracers.end(),
            [now, duration](const BulletTracer& t) { return (now - t.time) > duration; }), g_bulletTracers.end());
    }
    
    void RenderBulletTracers() {
        if (g_bulletTracers.empty()) return;
        uintptr_t clientBase = (uintptr_t)GetModuleHandleA("client.dll");
        if (!clientBase) return;
        view_matrix_t vm;
        memcpy(&vm, (void*)(clientBase + cs2_dumper::offsets::client_dll::dwViewMatrix), sizeof(vm));
        ImGuiIO& io = ImGui::GetIO();
        int sw = (int)io.DisplaySize.x, sh = (int)io.DisplaySize.y;
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        float now = (float)ImGui::GetTime();
        
        float duration = (g_fBulletTracerDuration > 0.1f) ? g_fBulletTracerDuration : 2.0f;
        for (const auto& t : g_bulletTracers) {
            float age = now - t.time;
            float alpha = 1.0f - (age / duration);
            if (alpha <= 0) continue;
            
            Vector2 s1, s2;
            if (WorldToScreen(t.start, s1, vm, sw, sh) && WorldToScreen(t.end, s2, vm, sw, sh)) {
                // Visual optimization: vibrant double-trace for glow effect
                float innerThickness = 1.3f * alpha;
                float outerThickness = 3.5f * alpha;
                
                ImU32 innerCol = ImGui::ColorConvertFloat4ToU32(ImVec4(
                    g_colorBulletTracer.x, g_colorBulletTracer.y, g_colorBulletTracer.z, 
                    g_colorBulletTracer.w * alpha
                ));
                
                ImU32 glowCol = ImGui::ColorConvertFloat4ToU32(ImVec4(
                    g_colorBulletTracer.x, g_colorBulletTracer.y, g_colorBulletTracer.z, 
                    g_colorBulletTracer.w * 0.35f * alpha
                ));
                
                // Draw outer glow first
                dl->AddLine(ImVec2(s1.x, s1.y), ImVec2(s2.x, s2.y), glowCol, outerThickness);
                // Draw sharp inner line
                dl->AddLine(ImVec2(s1.x, s1.y), ImVec2(s2.x, s2.y), innerCol, innerThickness);
            }
        }
    }
    
    void UpdateDamageIndicator() {
        if (!g_bDamageIndicatorEnabled) return;
        uintptr_t clientBase = (uintptr_t)GetModuleHandleA("client.dll");
        if (!clientBase) return;
        uintptr_t localController = *(uintptr_t*)(clientBase + cs2_dumper::offsets::client_dll::dwLocalPlayerController);
        if (!localController) return;
        uintptr_t actionTracking = *(uintptr_t*)(localController + 0x818);
        if (!actionTracking) return;
        float totalDamage = *(float*)(actionTracking + 0x130);
        if (totalDamage < g_fLastDamageDealt) g_fLastDamageDealt = totalDamage;
        if (totalDamage > g_fLastDamageDealt) {
            g_fDamageIndicatorValue = totalDamage - g_fLastDamageDealt;
            g_fDamageIndicatorTime = (float)ImGui::GetTime();
        }
        g_fLastDamageDealt = totalDamage;
    }
    
    void RenderDamageIndicator() {
        float age = (float)ImGui::GetTime() - g_fDamageIndicatorTime;
        if (age > 2.0f || g_fDamageIndicatorValue <= 0) return;
        float alpha = 1.0f - (age / 2.0f);
        if (alpha <= 0) return;
        char buf[32];
        snprintf(buf, sizeof(buf), "-%.0f", g_fDamageIndicatorValue);
        ImVec2 ts = ImGui::CalcTextSize(buf);
        ImGuiIO& io = ImGui::GetIO();
        float x = io.DisplaySize.x * 0.5f - ts.x * 0.5f;
        float y = io.DisplaySize.y * 0.35f;
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        dl->AddText(ImVec2(x + 1, y + 1), IM_COL32(0, 0, 0, (int)(alpha * 200)), buf);
        dl->AddText(ImVec2(x, y), IM_COL32(255, 80, 80, (int)(alpha * 255)), buf);
    }
}