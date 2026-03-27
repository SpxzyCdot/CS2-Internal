// pch.h: Precompiled header for CS2 Internal Cheat
#ifndef PCH_H
#define PCH_H

// Windows
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <psapi.h>

// DirectX 11
#include <d3d11.h>
#include <dxgi.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

// MinHook (compiled from source)
#include "minhook-master/include/MinHook.h"

// ImGui
#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui-master/imgui.h"
#include "imgui-master/imgui_internal.h"
#include "imgui-master/backends/imgui_impl_dx11.h"
#include "imgui-master/backends/imgui_impl_win32.h"

// CS2 Dumps
#include "offsets.hpp"
#include "client_dll.hpp"
#include "buttons.hpp"

// STL
#include <cstdint>
#include <string>
#include <cmath>

// SDK
#include "sdk/structs.h"

#endif //PCH_H
