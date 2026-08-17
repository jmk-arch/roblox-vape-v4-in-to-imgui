//
//
//
//
//
//
//
//
//

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"
#include "vape_gui.h"
#include "vape_assets.h"

#include <d3d11.h>
#include <shellapi.h>
#include <tchar.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

//
void SetupVapeGui(vape::VapeGui& gui);

// ---------------------------------------------------------------------------
//
// ---------------------------------------------------------------------------
static ID3D11Device*           g_pd3dDevice = nullptr;
static ID3D11DeviceContext*    g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*         g_pSwapChain = nullptr;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;
static UINT                    g_ResizeWidth = 0, g_ResizeHeight = 0;

static void SetProjectWorkingDirectory() {
    wchar_t exePath[32768] = {};
    DWORD len = GetModuleFileNameW(nullptr, exePath, (DWORD)std::size(exePath));
    if (len == 0 || len >= std::size(exePath)) return;

    std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
    std::filesystem::path projectDir = exeDir;
    std::error_code ec;
    if (!std::filesystem::exists(projectDir / L"assets", ec))
        projectDir = exeDir.parent_path();
    if (std::filesystem::exists(projectDir / L"assets", ec))
        SetCurrentDirectoryW(projectDir.c_str());
}

static void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    if (pBackBuffer) {
        g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
        pBackBuffer->Release();
    }
}

static void CleanupRenderTarget() {
    if (g_mainRenderTargetView) {
        g_mainRenderTargetView->Release();
        g_mainRenderTargetView = nullptr;
    }
}

static bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd = {};
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
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = {
        D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0,
    };
    HRESULT res = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags,
        featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain,
        &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED)
        res = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags,
            featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain,
            &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res != S_OK) return false;

    CreateRenderTarget();
    return true;
}

static void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg,
                                                             WPARAM wParam, LPARAM lParam);

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;

    switch (msg) {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) return 0;
        g_ResizeWidth = (UINT)LOWORD(lParam);
        g_ResizeHeight = (UINT)HIWORD(lParam);
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
//
// ---------------------------------------------------------------------------
static bool SaveBackbufferBMP(const char* path) {
    ID3D11Texture2D* back = nullptr;
    if (FAILED(g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;

    D3D11_TEXTURE2D_DESC desc = {};
    back->GetDesc(&desc);

    D3D11_TEXTURE2D_DESC sd = desc;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;

    ID3D11Texture2D* staging = nullptr;
    if (FAILED(g_pd3dDevice->CreateTexture2D(&sd, nullptr, &staging))) {
        back->Release();
        return false;
    }
    g_pd3dDeviceContext->CopyResource(staging, back);

    D3D11_MAPPED_SUBRESOURCE map = {};
    if (FAILED(g_pd3dDeviceContext->Map(staging, 0, D3D11_MAP_READ, 0, &map))) {
        staging->Release(); back->Release();
        return false;
    }

    const int W = (int)desc.Width, H = (int)desc.Height;
    const int rowBytes = W * 3;
    const int pad = (4 - (rowBytes % 4)) % 4;
    const int imgSize = (rowBytes + pad) * H;

    unsigned char fileHdr[14] = { 'B','M' };
    unsigned int fileSize = 14 + 40 + imgSize;
    memcpy(&fileHdr[2], &fileSize, 4);
    unsigned int offset = 54;
    memcpy(&fileHdr[10], &offset, 4);

    unsigned char infoHdr[40] = {};
    unsigned int hdrSize = 40; memcpy(&infoHdr[0], &hdrSize, 4);
    int w = W, h = H;           memcpy(&infoHdr[4], &w, 4); memcpy(&infoHdr[8], &h, 4);
    unsigned short planes = 1, bpp = 24;
    memcpy(&infoHdr[12], &planes, 2); memcpy(&infoHdr[14], &bpp, 2);
    memcpy(&infoHdr[20], &imgSize, 4);

    FILE* f = nullptr;
    fopen_s(&f, path, "wb");
    if (!f) {
        g_pd3dDeviceContext->Unmap(staging, 0);
        staging->Release(); back->Release();
        return false;
    }
    fwrite(fileHdr, 1, 14, f);
    fwrite(infoHdr, 1, 40, f);

    std::vector<unsigned char> row(rowBytes + pad, 0);
    const unsigned char* src = (const unsigned char*)map.pData;
    for (int yy = H - 1; yy >= 0; yy--) {
        const unsigned char* p = src + (size_t)yy * map.RowPitch;
        for (int xx = 0; xx < W; xx++) {
            //
            row[xx * 3 + 0] = p[xx * 4 + 2];
            row[xx * 3 + 1] = p[xx * 4 + 1];
            row[xx * 3 + 2] = p[xx * 4 + 0];
        }
        fwrite(row.data(), 1, rowBytes + pad, f);
    }
    fclose(f);

    g_pd3dDeviceContext->Unmap(staging, 0);
    staging->Release();
    back->Release();
    return true;
}

// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    SetProjectWorkingDirectory();

    std::string shotPath;
    std::string openCategory;
    std::string configPath;
    bool openSettings = false;
    int shotFrames = 30;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) shotPath = argv[++i];
        else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) shotFrames = atoi(argv[++i]);
        else if (strcmp(argv[i], "--settings") == 0) openSettings = true;
        else if (strcmp(argv[i], "--category") == 0 && i + 1 < argc) openCategory = argv[++i];
        else if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) configPath = argv[++i];
    }

    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L,
                       GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr,
                       L"VapeV4ImGui", nullptr };
    ::RegisterClassExW(&wc);
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"Vape V4 - ImGui",
                                WS_OVERLAPPEDWINDOW, 100, 100, 1000, 720,
                                nullptr, nullptr, wc.hInstance, nullptr);

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        std::printf("D3D11 초기화 실패\n");
        return 1;
    }

    //
    ::ShowWindow(hwnd, shotPath.empty() ? SW_SHOWDEFAULT : SW_HIDE);
    ::UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    //
    //
    //
    {
        ImFontConfig cfg;
        cfg.OversampleH = 2;
        cfg.OversampleV = 2;
        cfg.PixelSnapH = true;
        const char* arial = "C:\\Windows\\Fonts\\arial.ttf";
        if (GetFileAttributesA(arial) != INVALID_FILE_ATTRIBUTES) {
            //
            //
            static ImWchar ranges[] = { 0x0020, 0x00FF, 0x200A, 0x200A, 0 };
            io.Fonts->AddFontFromFileTTF(arial, 14.0f, &cfg, ranges);
            vape::g_fontSmall = io.Fonts->AddFontFromFileTTF(arial, 11.0f, &cfg, ranges);
            vape::g_fontBox   = io.Fonts->AddFontFromFileTTF(arial, 12.0f, &cfg, ranges);
            vape::g_fontTitle = io.Fonts->AddFontFromFileTTF(arial, 13.0f, &cfg, ranges);
            vape::g_fontItem  = io.Fonts->AddFontFromFileTTF(arial, 15.0f, &cfg, ranges);
            const char* arialBold = "C:\\Windows\\Fonts\\arialbd.ttf";
            if (GetFileAttributesA(arialBold) != INVALID_FILE_ATTRIBUTES)
                vape::g_fontSemiBold = io.Fonts->AddFontFromFileTTF(arialBold, 14.0f, &cfg, ranges);
        } else {
            std::printf("[경고] Arial 을 찾을 수 없어 기본 폰트를 씁니다.\n");
        }
    }

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    //
    {
        int n = vape::LoadAssets(g_pd3dDevice, "assets");
        std::printf(n > 0 ? "에셋 %d 개 로드됨\n" : "에셋 로드 실패 - 벡터 아이콘 사용\n", n);
    }

    vape::VapeGui gui;
    SetupVapeGui(gui);
    if (!configPath.empty()) gui.ConfigPath = configPath;

    //
    if (gui.Load())
        std::printf("설정을 불러왔습니다: %s\n", gui.ConfigPath.c_str());

    if (openSettings) gui.OpenSettings();
    if (!openCategory.empty()) gui.OpenCategory(openCategory, true);

    //
    const float clear[4] = { 0.12f, 0.12f, 0.13f, 1.00f };

    bool done = false;
    int frame = 0;
    while (!done) {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        if (g_ResizeWidth != 0 && g_ResizeHeight != 0) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight,
                                        DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            CreateRenderTarget();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        //
        //
        if (ImGui::IsKeyPressed(ImGuiKey_Escape) &&
            !ImGui::GetIO().WantTextInput && !gui.IsBinding() &&
            std::find(gui.Keybind.begin(), gui.Keybind.end(), "Escape") == gui.Keybind.end())
            done = true;

        gui.Render();
        if (gui.ExitRequested) done = true;

        ImGui::Render();
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        //
        if (!shotPath.empty() && frame == shotFrames) {
            bool ok = SaveBackbufferBMP(shotPath.c_str());
            std::printf(ok ? "스크린샷 저장: %s\n" : "스크린샷 실패: %s\n", shotPath.c_str());
            done = true;
        }

        g_pSwapChain->Present(shotPath.empty() ? 1 : 0, 0);
        frame++;
    }

    //
    if (shotPath.empty() && !gui.SkipSaveOnExit && gui.Save())
        std::printf("설정을 저장했습니다: %s\n", gui.ConfigPath.c_str());

    const bool restartRequested = gui.RestartRequested;

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);

    if (restartRequested) {
        wchar_t exePath[32768] = {};
        const DWORD len = GetModuleFileNameW(nullptr, exePath, (DWORD)std::size(exePath));
        if (len > 0 && len < std::size(exePath)) {
            std::wstring restartArgs;
            if (!configPath.empty()) {
                restartArgs = L"--config \"";
                restartArgs += std::filesystem::path(configPath).wstring();
                restartArgs += L"\"";
            }
            ShellExecuteW(nullptr, L"open", exePath,
                          restartArgs.empty() ? nullptr : restartArgs.c_str(),
                          nullptr, SW_SHOWNORMAL);
        }
    }
    return 0;
}

