// Win32/DX11 lifecycle adapted from Dear ImGui v1.91.9b.
// Copyright (c) 2014-2025 Omar Cornut. MIT license in third_party/imgui/LICENSE.txt.

// Learn about Dear ImGui:
// - FAQ                  https://dearimgui.com/faq
// - Getting Started      https://dearimgui.com/getting-started
// - Documentation        https://dearimgui.com/docs (same as your local docs/ folder).
// - Introduction, links and more at the top of imgui.cpp

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "ui.hpp"
#include "desktop.hpp"
#include <algorithm>
#include <d3d11.h>
#include <filesystem>
#include <fstream>
#include <tchar.h>
#include <vector>

// Data
static ID3D11Device *g_pd3dDevice = nullptr;
static ID3D11DeviceContext *g_pd3dDeviceContext = nullptr;
static IDXGISwapChain *g_pSwapChain = nullptr;
static bool g_SwapChainOccluded = false;
static float g_fontDpi = 1.0f, g_styleDpi = 1.0f;
static UINT g_ResizeWidth = 0, g_ResizeHeight = 0;
static ID3D11RenderTargetView *g_mainRenderTargetView = nullptr;

// Forward declarations of helper functions
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
void save_rendered_frame(IDXGISwapChain *, ID3D11Device *, ID3D11DeviceContext *, const wchar_t *);

// Main code
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR command_line, int) {
    bool capture_run = command_line && wcsstr(command_line, L"--capture");
    LARGE_INTEGER profile_frequency{};
    QueryPerformanceFrequency(&profile_frequency);
    // Create application window
    ImGui_ImplWin32_EnableDpiAwareness();
    WNDCLASSEXW wc = {
        sizeof(wc), CS_CLASSDC,         WndProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr,
        nullptr,    L"HeapglassWindow", nullptr};
    ::RegisterClassExW(&wc);
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"Heapglass", WS_OVERLAPPEDWINDOW, 40, 40, 1600, 1000,
                                nullptr, nullptr, wc.hInstance, nullptr);

    // Initialize Direct3D
    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    // Show the window
    ::ShowWindow(hwnd, capture_run ? SW_HIDE : SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard; // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;     // Enable Docking
    // Setup Dear ImGui style
    ImGui::StyleColorsDark();
    wchar_t local_path[32768]{};
    GetEnvironmentVariableW(L"LOCALAPPDATA", local_path, 32768);
    std::filesystem::path settings = std::filesystem::path(local_path) / L"Heapglass";
    std::filesystem::create_directories(settings);
    static std::string ini = hg::narrow((settings / L"layout.ini").wstring());
    io.IniFilename = ini.c_str();
    float dpi = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);
    g_fontDpi = dpi;
    g_styleDpi = dpi;
    wchar_t windows_directory[MAX_PATH]{};
    GetWindowsDirectoryW(windows_directory, MAX_PATH);
    auto font_directory = std::filesystem::path(windows_directory) / L"Fonts";
    auto body_font = hg::narrow((font_directory / L"segoeui.ttf").wstring());
    auto hex_font = hg::narrow((font_directory / L"consola.ttf").wstring());
    static const ImWchar body_ranges[] = {0x20, 0x024f, 0x0370, 0x06ff, 0x2000, 0x206f, 0x4e00, 0x9fff, 0};
    io.Fonts->AddFontFromFileTTF(body_font.c_str(), 15.0f * dpi, nullptr, body_ranges);
    static const ImWchar cjk_ranges[] = {0x2e80, 0x9fff, 0};
    for (auto name : {L"msyh.ttc", L"YuGothR.ttc", L"malgun.ttf", L"simsun.ttc"}) {
        auto path = font_directory / name;
        if (std::filesystem::is_regular_file(path)) {
            ImFontConfig config;
            config.MergeMode = true;
            auto utf8 = hg::narrow(path.wstring());
            io.Fonts->AddFontFromFileTTF(utf8.c_str(), 15.0f * dpi, &config, cjk_ranges);
            break;
        }
    }
    auto emoji = font_directory / L"seguiemj.ttf";
    if (std::filesystem::is_regular_file(emoji)) {
        static const ImWchar ranges[] = {0x1f600, 0x1f6c5, 0};
        ImFontConfig config;
        config.MergeMode = true;
        auto utf8 = hg::narrow(emoji.wstring());
        io.Fonts->AddFontFromFileTTF(utf8.c_str(), 15.0f * dpi, &config, ranges);
    }
    io.Fonts->AddFontFromFileTTF(hex_font.c_str(), 14.0f * dpi);
    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowRounding = 0;
    style.FrameRounding = 3;
    style.GrabRounding = 3;
    style.WindowPadding = ImVec2(12, 10);
    style.ItemSpacing = ImVec2(8, 6);
    style.Colors[ImGuiCol_WindowBg] = ImVec4(.055f, .070f, .085f, 1);
    style.Colors[ImGuiCol_ChildBg] = ImVec4(.055f, .070f, .085f, 1);
    style.Colors[ImGuiCol_Header] = ImVec4(.12f, .26f, .29f, 1);
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(.17f, .34f, .37f, 1);
    style.Colors[ImGuiCol_HeaderActive] = ImVec4(.18f, .40f, .43f, 1);
    style.Colors[ImGuiCol_Button] = ImVec4(.13f, .26f, .29f, 1);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(.17f, .36f, .39f, 1);
    style.Colors[ImGuiCol_CheckMark] = ImVec4(.38f, .83f, .78f, 1);
    style.Colors[ImGuiCol_Text] = ImVec4(.88f, .92f, .94f, 1);
    style.Colors[ImGuiCol_TextDisabled] = ImVec4(.58f, .65f, .70f, 1);
    style.Colors[ImGuiCol_TabSelected] = ImVec4(.13f, .28f, .31f, 1);
    style.Colors[ImGuiCol_TabSelectedOverline] = ImVec4(.38f, .83f, .78f, 1);
    style.ScaleAllSizes(dpi);

    // When viewports are enabled we tweak WindowRounding/WindowBg so platform windows can look identical to
    // regular ones. Style configured above.
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        style.WindowRounding = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }

    // Setup Platform/Renderer backends
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    hg::UI app;
    unsigned capture_stage = 0;
    auto run_start = GetTickCount64();
    if (capture_run) {
        hg::Limits limits;
        limits.stack_frames = 8;
        app.controller.configure(limits);
        app.controller.launch(hg::executable_directory() + L"\\HeapglassTarget.exe", L"--duration 20");
    } else if (command_line && wcsstr(command_line, L"--test-target"))
        app.auto_target();
    ImVec4 clear_color = ImVec4(.04f, .05f, .065f, 1);

    std::vector<double> draw_times;
    double frame_seconds = 0;
    uint64_t frame_count = 0;
    // Main loop
    bool done = false;
    while (!done) {
        // Poll and handle messages (inputs, window resize, etc.)
        // See the WndProc() function below for our to dispatch events to the Win32 backend.
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done)
            break;

        // Handle window being minimized or screen locked
        if (!capture_run && g_SwapChainOccluded &&
            g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            ::Sleep(10);
            continue;
        }
        g_SwapChainOccluded = false;

        // Handle window resize (we don't resize directly in the WM_SIZE handler)
        if (g_ResizeWidth != 0 && g_ResizeHeight != 0) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            CreateRenderTarget();
        }

        // Start the Dear ImGui frame
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        auto draw_begin = hg::ticks();
        app.draw();
        if (frame_count > 120 && draw_times.size() < 36000) {
            draw_times.push_back(double(hg::ticks() - draw_begin) * 1000. /
                                 double(profile_frequency.QuadPart));
            frame_seconds += io.DeltaTime;
        }
        ++frame_count;

        // Rendering
        ImGui::Render();
        const float clear_color_with_alpha[4] = {clear_color.x * clear_color.w, clear_color.y * clear_color.w,
                                                 clear_color.z * clear_color.w, clear_color.w};
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        // Update and Render additional Platform Windows
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
        }

        if (capture_run && ((capture_stage == 0 && GetTickCount64() - run_start > 8000) ||
                            (capture_stage == 1 && GetTickCount64() - run_start > 12000) ||
                            (capture_stage == 2 && GetTickCount64() - run_start > 16000))) {
            try {
                auto path = hg::executable_directory() + (capture_stage == 0 ? L"\\Heapglass.allocations.png"
                                                          : capture_stage == 1 ? L"\\Heapglass.memory-map.png"
                                                                               : L"\\Heapglass.stack.png");
                save_rendered_frame(g_pSwapChain, g_pd3dDevice, g_pd3dDeviceContext, path.c_str());
            } catch (const std::exception &e) {
                std::ofstream error{std::filesystem::path(hg::executable_directory()) / L"capture-error.txt"};
                error << e.what();
            }
            if (capture_stage == 0)
                app.show_memory_map();
            else if (capture_stage == 1)
                app.show_allocation_stack();
            else
                done = true;
            ++capture_stage;
        }
        // Present
        HRESULT hr = g_pSwapChain->Present(1, 0); // Present with vsync
        // HRESULT hr = g_pSwapChain->Present(0, 0); // Present without vsync
        g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
    }

    if (command_line && wcsstr(command_line, L"--profile") && !draw_times.empty()) {
        std::sort(draw_times.begin(), draw_times.end());
        double sum = 0;
        for (double n : draw_times)
            sum += n;
        std::ofstream profile{std::filesystem::path(hg::executable_directory()) /
                              L"Heapglass.ui-profile.json"};
        profile << "{\"frames\":" << draw_times.size() << ",\"mean_draw_ms\":" << sum / draw_times.size()
                << ",\"p95_draw_ms\":" << draw_times[draw_times.size() * 95 / 100]
                << ",\"max_draw_ms\":" << draw_times.back()
                << ",\"mean_frame_ms_including_present\":" << frame_seconds * 1000. / draw_times.size()
                << "}\n";
    }
    // Cleanup
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);

    return 0;
}

// Helper functions
bool CreateDeviceD3D(HWND hWnd) {
    // Setup swap chain
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    // createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_0,
    };
    HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags,
                                                featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain,
                                                &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res ==
        DXGI_ERROR_UNSUPPORTED) // Try high-performance WARP software driver if hardware is not available.
        res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags,
                                            featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain,
                                            &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res != S_OK)
        return false;

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) {
        g_pSwapChain->Release();
        g_pSwapChain = nullptr;
    }
    if (g_pd3dDeviceContext) {
        g_pd3dDeviceContext->Release();
        g_pd3dDeviceContext = nullptr;
    }
    if (g_pd3dDevice) {
        g_pd3dDevice->Release();
        g_pd3dDevice = nullptr;
    }
}

void CreateRenderTarget() {
    ID3D11Texture2D *pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

void CleanupRenderTarget() {
    if (g_mainRenderTargetView) {
        g_mainRenderTargetView->Release();
        g_mainRenderTargetView = nullptr;
    }
}

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0 // From Windows SDK 8.1+ headers
#endif

// Forward declare message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam,
                                                             LPARAM lParam);

// Win32 message handler
// You can read the io.WantCaptureMouse, io.WantCaptureKeyboard flags to tell if dear imgui wants to use your
// inputs.
// - When io.WantCaptureMouse is true, do not dispatch mouse input data to your main application, or
// clear/overwrite your copy of the mouse data.
// - When io.WantCaptureKeyboard is true, do not dispatch keyboard input data to your main application, or
// clear/overwrite your copy of the keyboard data. Generally you may always pass all inputs to dear imgui, and
// hide them from your application based on those two flags.
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED)
            return 0;
        g_ResizeWidth = (UINT)LOWORD(lParam); // Queue resize
        g_ResizeHeight = (UINT)HIWORD(lParam);
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) // Disable ALT application menu
            return 0;
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    case WM_DPICHANGED:
        if (ImGui::GetCurrentContext()) {
            hg::apply_dpi(hWnd, HIWORD(wParam), g_fontDpi, g_styleDpi,
                          reinterpret_cast<const RECT *>(lParam));
        }
        break;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}
