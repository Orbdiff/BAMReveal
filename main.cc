#include <Windows.h>
#include <d3d11.h>
#include <thread>
#include <atomic>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <unordered_map>
#include <set>
#include <wrl/client.h>
#include <shlobj.h>
#include <shellapi.h>
#include <algorithm>
#include <wrl/client.h>

#include "wil/resource.h"

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include "privilege/_privilege.hpp"
#include "bam/bam.h"
#include "bam/bam_sys.h"
#include "bam/registry_bam.h"
#include "bam/deleted_values.hh"
#include "ui/bam_ui.h"
#include "ui/_font.h"
#include "ui/_time_utils.h"

static ID3D11Device* g_Device = nullptr;
static ID3D11DeviceContext* g_Context = nullptr;
static IDXGISwapChain* g_SwapChain = nullptr;
static ID3D11RenderTargetView* g_RTV = nullptr;
using Microsoft::WRL::ComPtr;

std::atomic<bool> g_Loading = false;
std::vector<BAMEntryUI> g_BamUI;
static BamThreadInfo g_cachedBamInfo{};

struct IconDataDX11
{
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> TextureView;
    int Width = 0;
    int Height = 0;
    bool IsLoaded = false;
};

std::mutex g_iconMutex;
std::condition_variable g_iconCv;
std::queue<std::wstring> g_iconQueue;
std::unordered_map<std::wstring, IconDataDX11> g_iconsCache;
std::set<std::wstring> g_pendingIcons;
bool g_iconThreadExit = false;

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    if (msg == WM_DESTROY)
    {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

void CreateRenderTarget()
{
    ComPtr<ID3D11Texture2D> backBuffer;
    HRESULT hr = g_SwapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr) || !backBuffer)
    {
        MessageBoxA(nullptr, "Failed to get swap chain back buffer.", "Error", MB_OK | MB_ICONERROR);
        return;
    }

    hr = g_Device->CreateRenderTargetView(backBuffer.Get(), nullptr, &g_RTV);
    if (FAILED(hr))
    {
        MessageBoxA(nullptr, "Failed to create render target view.", "Error", MB_OK | MB_ICONERROR);
    }
}

void InitD3D(HWND hwnd)
{
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        &sd,
        &g_SwapChain,
        &g_Device,
        nullptr,
        &g_Context
    );

    CreateRenderTarget();
}

void LoadBAMAsync()
{
    g_Loading = true;
    auto raw = ReadBAM();
    g_BamUI = ConvertToUI(raw);
    g_Loading = false;
}

bool LoadFileIconDX11(ID3D11Device* device, const std::wstring& filePath, IconDataDX11& outIcon)
{
    if (!device || filePath.empty())
        return false;

    SHFILEINFO shfi{};
    if (!SHGetFileInfoW(filePath.c_str(), FILE_ATTRIBUTE_NORMAL, &shfi, sizeof(shfi),
        SHGFI_ICON | SHGFI_SMALLICON) || !shfi.hIcon)
        return false;

    wil::unique_hicon hIcon{ shfi.hIcon };
    ICONINFO iconInfo{};
    if (!GetIconInfo(hIcon.get(), &iconInfo))
        return false;

    wil::unique_hbitmap hbmColor{ iconInfo.hbmColor };
    BITMAP bm{};
    if (!GetObject(hbmColor.get(), sizeof(BITMAP), &bm))
        return false;

    const int width = bm.bmWidth;
    const int height = bm.bmHeight;

    wil::unique_hdc hdc{ CreateCompatibleDC(nullptr) };
    if (!hdc)
        return false;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    std::vector<BYTE> pixels(width * height * 4);
    if (!GetDIBits(hdc.get(), hbmColor.get(), 0, height, pixels.data(), &bmi, DIB_RGB_COLORS))
        return false;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData{};
    initData.pSysMem = pixels.data();
    initData.SysMemPitch = width * 4;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device->CreateTexture2D(&desc, &initData, &texture)))
        return false;

    if (FAILED(device->CreateShaderResourceView(texture.Get(), nullptr, &outIcon.TextureView)))
        return false;

    outIcon.Width = width;
    outIcon.Height = height;
    outIcon.IsLoaded = true;
    return true;
}

void IconWorkerThread(ID3D11Device* device)
{
    while (true)
    {
        std::wstring path;
        {
            std::unique_lock<std::mutex> lock(g_iconMutex);
            g_iconCv.wait(lock, [] { return !g_iconQueue.empty() || g_iconThreadExit; });
            if (g_iconThreadExit && g_iconQueue.empty()) break;
            path = g_iconQueue.front();
            g_iconQueue.pop();
        }

        IconDataDX11 icon;
        LoadFileIconDX11(device, path, icon);

        {
            std::lock_guard<std::mutex> lock(g_iconMutex);
            g_iconsCache[path] = std::move(icon);
            g_pendingIcons.erase(path);
        }
    }
}

void EnsureIconLoadedAsync(ID3D11Device* device, const std::wstring& path)
{
    if (path.empty() || !device) return;

    std::lock_guard<std::mutex> lock(g_iconMutex);
    if (g_iconsCache.contains(path) || g_pendingIcons.contains(path))
        return;

    g_pendingIcons.insert(path);
    g_iconQueue.push(path);
    g_iconCv.notify_one();

    static bool threadStarted = false;
    if (!threadStarted)
    {
        threadStarted = true;
        std::thread(IconWorkerThread, device).detach();
    }
}

IconDataDX11* GetOrQueueIcon(ID3D11Device* device, const std::wstring& path)
{
    if (path.empty() || !device)
        return nullptr;

    {
        std::lock_guard<std::mutex> lock(g_iconMutex);
        auto it = g_iconsCache.find(path);
        if (it != g_iconsCache.end())
            return &it->second;
    }

    EnsureIconLoadedAsync(device, path);
    return nullptr;
}

std::wstring StringToWString(const std::string& str)
{
    if (str.empty()) return L"";
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), nullptr, 0);
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), &wstrTo[0], size_needed);
    return wstrTo;
}

std::string ws2s(const std::wstring& wstr)
{
    if (wstr.empty()) return {};

    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), nullptr, 0, nullptr, nullptr);
    std::string result(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), result.data(), size_needed, nullptr, nullptr);
    return result;
}

int WINAPI WinMain
(
    _In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPSTR     lpCmdLine,
    _In_ int       nShowCmd
)
{
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"BAMReveal";

    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowW(
        wc.lpszClassName,
        L"BAMReveal made by Diff",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        1280, 720,
        nullptr,
        nullptr,
        wc.hInstance,
        nullptr
    );

    ShowWindow(hwnd, SW_MAXIMIZE);
    UpdateWindow(hwnd);

    InitD3D(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    (void)io;

    ImFontConfig CustomFont;
    CustomFont.FontDataOwnedByAtlas = false;
    ImFont* font = io.Fonts->AddFontFromMemoryTTF((void*)Custom, static_cast<int>(Custom_len), 17.0f, &CustomFont);
    io.FontDefault = font;

    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    colors[ImGuiCol_Text] = ImVec4(0.90f, 0.90f, 0.90f, 1.00f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.55f, 0.55f, 1.00f);
    colors[ImGuiCol_WindowBg] = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.12f, 0.12f, 0.12f, 1.00f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.12f, 0.12f, 0.12f, 1.00f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.18f, 0.20f, 0.24f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.22f, 0.25f, 0.30f, 1.00f);
    colors[ImGuiCol_Border] = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
    colors[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_TitleBg] = ImVec4(0.13f, 0.13f, 0.13f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.18f, 0.18f, 0.18f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.10f, 0.10f, 0.10f, 0.90f);
    colors[ImGuiCol_MenuBarBg] = ImVec4(0.12f, 0.12f, 0.12f, 1.00f);
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.08f, 0.08f, 0.08f, 1.00f);
    colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.35f, 0.35f, 0.35f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.30f, 0.55f, 0.85f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.35f, 0.60f, 0.95f, 1.00f);
    colors[ImGuiCol_Button] = ImVec4(0.18f, 0.18f, 0.18f, 1.00f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.25f, 0.45f, 0.75f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.30f, 0.55f, 0.85f, 1.00f);
    colors[ImGuiCol_CheckMark] = ImVec4(0.35f, 0.60f, 0.95f, 1.00f);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.25f, 0.45f, 0.75f, 1.00f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.30f, 0.55f, 0.85f, 1.00f);
    colors[ImGuiCol_Header] = ImVec4(0.25f, 0.25f, 0.25f, 0.80f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.25f, 0.45f, 0.75f, 0.90f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.30f, 0.55f, 0.85f, 0.90f);
    colors[ImGuiCol_Tab] = ImVec4(0.12f, 0.12f, 0.12f, 1.00f);
    colors[ImGuiCol_TabHovered] = ImVec4(0.25f, 0.45f, 0.75f, 1.00f);
    colors[ImGuiCol_TabActive] = ImVec4(0.30f, 0.55f, 0.85f, 1.00f);
    colors[ImGuiCol_TabUnfocused] = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
    colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.20f, 0.30f, 0.45f, 1.00f);
    colors[ImGuiCol_Separator] = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
    colors[ImGuiCol_SeparatorHovered] = ImVec4(0.25f, 0.45f, 0.75f, 1.00f);
    colors[ImGuiCol_SeparatorActive] = ImVec4(0.30f, 0.55f, 0.85f, 1.00f);
    colors[ImGuiCol_ResizeGrip] = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
    colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.25f, 0.45f, 0.75f, 1.00f);
    colors[ImGuiCol_ResizeGripActive] = ImVec4(0.30f, 0.55f, 0.85f, 1.00f);
    colors[ImGuiCol_TableHeaderBg] = ImVec4(0.16f, 0.16f, 0.16f, 1.00f);
    colors[ImGuiCol_TableBorderStrong] = ImVec4(0.28f, 0.28f, 0.28f, 1.00f);
    colors[ImGuiCol_TableBorderLight] = ImVec4(0.22f, 0.22f, 0.22f, 1.00f);
    colors[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_TableRowBgAlt] = ImVec4(0.13f, 0.13f, 0.13f, 1.00f);

    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.ScrollbarRounding = 6.0f;
    style.TabRounding = 5.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.ScrollbarSize = 14.0f;
    style.ItemSpacing = ImVec2(8, 6);
    style.ItemInnerSpacing = ImVec2(6, 4);
    style.CellPadding = ImVec2(6, 4);
    style.WindowPadding = ImVec2(14, 14);
    style.FramePadding = ImVec2(8, 5);

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_Device, g_Context);

    if (!EnableDebugPrivilege()) {
        MessageBoxA(nullptr, "Failed to enable SeDebugPrivilege. Please run BAMReveal with perms Administrator.", "Warning", MB_OK);
    }

    std::thread(LoadBAMAsync).detach();

    static char g_searchBuffer[256] = {};
    static bool g_afterLogonOnly = false;
    static bool g_showUnsignedCheat = false;
    static time_t g_logonTime = static_cast<time_t>(GetCurrentUserLogonTime());

    static float fadeAlpha = 1.0f;
    static bool lastAfterLogon = g_afterLogonOnly;
    static bool lastShowUnsigned = g_showUnsignedCheat;
    static std::string lastSearch;

    static int selectedRow = -1;

    static float loadingStartTime = 0.0f;
    static bool showLoadingAnimation = true;
    static float fadeOutAlpha = 1.0f;
    const float minLoadingDuration = 3.0f;
    const float fadeOutSpeed = 2.0f;

    MSG msg{};
    while (msg.message != WM_QUIT)
    {
        if (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            continue;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        ImGuiWindowFlags windowFlags =ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar;

        RECT rect;
        GetClientRect(hwnd, &rect);
        ImGui::SetNextWindowSize(ImVec2(float(rect.right - rect.left), float(rect.bottom - rect.top)), ImGuiCond_Always);
        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::Begin("BAM Reveal", nullptr, windowFlags);

        if (g_Loading || showLoadingAnimation)
        {
            static double loadingStartTime = 0.0;
            static float fadeOutAlpha = 1.0f;
            const float minLoadingDuration = 1.0f;
            const float fadeOutSpeed = 2.0f;

            if (loadingStartTime == 0.0)
                loadingStartTime = ImGui::GetTime();

            ImVec2 pos = ImGui::GetWindowPos();
            ImVec2 size = ImGui::GetWindowSize();
            ImVec2 center = ImVec2(pos.x + size.x * 0.5f, pos.y + size.y * 0.5f - 20.0f);

            ImDrawList* draw_list = ImGui::GetWindowDrawList();
            double t = ImGui::GetTime();

            const int particleCount = 100;
            for (int i = 0; i < particleCount; ++i)
            {
                float seed = i * 17.23f;
                float ft = static_cast<float>(t);
                float x = pos.x + fmodf(seed * 41.0f + ft * 25.0f, size.x);
                float y = pos.y + fmodf(seed * 59.0f + ft * 18.0f, size.y);
                float alpha = 60.0f + 60.0f * sinf(ft + seed);
                float radius = 1.0f + 1.0f * sinf(ft + seed * 0.5f);
                draw_list->AddCircleFilled(ImVec2(x, y), radius, IM_COL32(255, 220, 120, (int)alpha));
            }

            float baseRadius = 32.0f;
            float pulse = 0.9f + 0.1f * sinf(static_cast<float>(t) * 2.5f);
            float radius = baseRadius * pulse;

            ImU32 ringColors[3] = { IM_COL32(255,200,100,220), IM_COL32(255,160,50,200), IM_COL32(220,120,30,180) };
            for (int i = 0; i < 3; ++i)
            {
                float angle = static_cast<float>(t * 2.2 + i * 1.4);
                draw_list->PathArcTo(center, radius - i * 6.0f, angle, angle + 1.6f, 48);
                draw_list->PathStroke(ringColors[i], false, 3.5f);
            }

            const char* loadingText = "Parsing BAM...";
            ImVec2 textSize = ImGui::CalcTextSize(loadingText);
            ImVec2 textPos = ImVec2(center.x - textSize.x * 0.5f, center.y + baseRadius + 16.0f);

            float textPulse = 0.85f + 0.15f * sinf(static_cast<float>(t) * 2.0f);
            int alpha = static_cast<int>(textPulse * 255.0f * fadeOutAlpha);
            if (alpha < 100) alpha = 100;

            draw_list->AddText(textPos, IM_COL32(255, 80, 80, alpha), loadingText);

            if (!g_Loading && (ImGui::GetTime() - loadingStartTime) >= minLoadingDuration)
            {
                fadeOutAlpha -= io.DeltaTime * fadeOutSpeed;
                if (fadeOutAlpha <= 0.0f)
                {
                    fadeOutAlpha = 0.0f;
                    showLoadingAnimation = false;
                    loadingStartTime = 0.0;
                }
            }
        } else {
            ImGui::PushItemWidth(300);
            ImGui::InputTextWithHint("##SearchBAM", "Search...", g_searchBuffer, IM_ARRAYSIZE(g_searchBuffer));
            ImGui::PopItemWidth();

            ImGui::SameLine(0, 10);
            ImGui::Checkbox("Show in Instance", &g_afterLogonOnly);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Show all paths after logon-time");
            ImGui::SameLine(0, 10);
            ImGui::Checkbox("Show Only Unsigned/Cheat", &g_showUnsignedCheat);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Show paths without a signature, cheat signature and yara rules");

            float buttonWidth = 160.0f;
            float avail = ImGui::GetContentRegionAvail().x;

            static bool showDeletedBamPopup = false;
            static bool isReadingDeletedBam = false;
            static DeletedBAMEntriesResult deletedBamPopupResultLocal{};
            static std::mutex deletedBamMutex;
            static float fadeAlphaDeletedBam = 0.0f;

            ImGui::SameLine(avail - buttonWidth * 3 - 20);
            if (ImGui::Button("Deleted BAM", ImVec2(buttonWidth, 0)))
            {
                showDeletedBamPopup = true;
                isReadingDeletedBam = true;
                fadeAlphaDeletedBam = 0.0f;

                std::thread([] {
                    DeletedBAMEntriesResult result = FindDeletedBAMEntriesInSystemHive();

                    {
                        std::lock_guard<std::mutex> lock(deletedBamMutex);
                        deletedBamPopupResultLocal = std::move(result);
                        isReadingDeletedBam = false;
                    }
                    }).detach();

                ImGui::OpenPopup("SYSTEM Hive##DeletedBamModal");
            }

            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Show deleted BAM Entries");
            }

            static bool showDeniedPopup = false;
            static bool isReadingDenied = false;
            static std::vector<DeniedRegistryEntry> deniedPopupResult;
            static char g_searchDeniedBuffer[128] = "";
            static float fadeAlphaDenied = 0.0f;
            static std::string lastSearchDenied;

            ImGui::SameLine(avail - buttonWidth * 2 - 10);
            if (ImGui::Button("Registry BAM", ImVec2(buttonWidth, 0)))
            {
                showDeniedPopup = true;
                isReadingDenied = true;
                fadeAlphaDenied = 0.0f;

                std::thread([] {
                    auto entries = GetDeniedBAMEntries();
                    deniedPopupResult = std::move(entries);
                    isReadingDenied = false;
                    }).detach();

                ImGui::OpenPopup("Denied BAMRegistry Information##DeniedBamModal");
            }

            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Show keys in registry denied");
            }

            static bool showBamPopup = false;
            static bool isReadingBam = false;
            static BamThreadInfo bamPopupResult{};
            static float fadeAlphaBam = 0.0f;

            ImGui::SameLine(avail - buttonWidth);
            if (ImGui::Button("BAM Info", ImVec2(buttonWidth, 0)))
            {
                showBamPopup = true;
                isReadingBam = true;
                fadeAlphaBam = 0.0f;

                std::thread([] {
                    BamThreadInfo* info = new BamThreadInfo(GetBamThreadInfo());
                    ImGui::GetIO().UserData = info;
                    }).detach();

                ImGui::OpenPopup("BAM System Information##BamInfoModal");
            }

            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("BAM Creation Time");
            }

            std::string currentSearch(g_searchBuffer);
            std::transform(currentSearch.begin(), currentSearch.end(), currentSearch.begin(), ::tolower);

            bool filtersChanged =
                (lastAfterLogon != g_afterLogonOnly) ||
                (lastShowUnsigned != g_showUnsignedCheat) ||
                (lastSearch != currentSearch);

            if (filtersChanged)
            {
                fadeAlpha = 0.0f;
                lastAfterLogon = g_afterLogonOnly;
                lastShowUnsigned = g_showUnsignedCheat;
                lastSearch = currentSearch;
            }

            fadeAlpha += io.DeltaTime * 2.0f;
            if (fadeAlpha > 1.0f) fadeAlpha = 1.0f;

            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, fadeAlpha);

            std::vector<BAMEntryUI> filteredBam;
            for (const auto& e : g_BamUI)
            {
                if (g_afterLogonOnly && e.execTime < g_logonTime)
                    continue;

                if (g_showUnsignedCheat &&
                    !(e.signature == BamSignature::Unsigned ||
                        e.signature == BamSignature::Cheat))
                    continue;

                if (!currentSearch.empty())
                {
                    std::string pathLower = e.path;
                    std::transform(pathLower.begin(), pathLower.end(), pathLower.begin(), ::tolower);

                    std::string timeLower = e.time;
                    std::transform(timeLower.begin(), timeLower.end(), timeLower.begin(), ::tolower);

                    std::string sigLower;
                    switch (e.signature)
                    {
                    case BamSignature::Signed:   sigLower = "signed"; break;
                    case BamSignature::Unsigned: sigLower = "unsigned"; break;
                    case BamSignature::Cheat:    sigLower = "cheat"; break;
                    default:                     sigLower = "not found"; break;
                    }

                    if (pathLower.find(currentSearch) == std::string::npos &&
                        timeLower.find(currentSearch) == std::string::npos &&
                        sigLower.find(currentSearch) == std::string::npos)
                        continue;
                }

                filteredBam.push_back(e);
            }

            fadeAlphaBam += io.DeltaTime * 2.0f;
            if (fadeAlphaBam > 1.0f) fadeAlphaBam = 1.0f;

            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, fadeAlphaBam);

            if (ImGui::BeginPopupModal(
                "BAM System Information##BamInfoModal",
                &showBamPopup,
                ImGuiWindowFlags_NoCollapse))
            {
                ImGui::SetWindowSize(ImVec2(620, 320), ImGuiCond_Once);

                if (isReadingBam)
                {
                    auto ptr = static_cast<BamThreadInfo*>(ImGui::GetIO().UserData);
                    if (ptr)
                    {
                        bamPopupResult = *ptr;
                        delete ptr;
                        ImGui::GetIO().UserData = nullptr;
                        isReadingBam = false;
                    }
                }

                ImVec2 winSize = ImGui::GetWindowSize();

                if (isReadingBam)
                {
                    ImGui::Dummy(ImVec2(0, winSize.y * 0.35f));
                    ImGui::SetCursorPosX(
                        (winSize.x - ImGui::CalcTextSize("Reading System threads...").x) * 0.5f
                    );
                    ImGui::TextColored(
                        ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                        "Reading System threads..."
                    );
                }
                else if (!bamPopupResult.valid)
                {
                    ImGui::Dummy(ImVec2(0, winSize.y * 0.35f));
                    ImGui::SetCursorPosX(
                        (winSize.x - ImGui::CalcTextSize("No BAM thread found.").x) * 0.5f
                    );
                    ImGui::TextColored(
                        ImVec4(0.8f, 0.4f, 0.4f, 1.0f),
                        "No BAM thread found."
                    );
                }
                else
                {
                    ImGui::Text("Boot Time:");
                    ImGui::BulletText("%04d/%02d/%02d %02d:%02d:%02d",
                        bamPopupResult.logonTime.wYear,
                        bamPopupResult.logonTime.wMonth,
                        bamPopupResult.logonTime.wDay,
                        bamPopupResult.logonTime.wHour,
                        bamPopupResult.logonTime.wMinute,
                        bamPopupResult.logonTime.wSecond);

                    ImGui::Spacing();

                    ImGui::Text("BAM Thread Creation:");
                    ImGui::BulletText("%04d/%02d/%02d %02d:%02d:%02d",
                        bamPopupResult.creationTime.wYear,
                        bamPopupResult.creationTime.wMonth,
                        bamPopupResult.creationTime.wDay,
                        bamPopupResult.creationTime.wHour,
                        bamPopupResult.creationTime.wMinute,
                        bamPopupResult.creationTime.wSecond);

                    ImGui::Spacing();

                    ImGui::Text("Time After Boot:");
                    ImGui::BulletText("%llu seconds",
                        bamPopupResult.timeAfterBootSeconds);

                    ImGui::Spacing();

                    if (bamPopupResult.bamRestarted)
                        ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "[!] BAM has been restarted");
                    else
                        ImGui::TextColored(ImVec4(0.3f, 1, 0.3f, 1), "[+] BAM started normally");
                }

                ImGui::EndPopup();
            }

            ImGui::PopStyleVar();

            fadeAlphaDeletedBam += io.DeltaTime * 2.0f;
            if (fadeAlphaDeletedBam > 1.0f) fadeAlphaDeletedBam = 1.0f;
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, fadeAlphaDeletedBam);

            if (ImGui::BeginPopupModal(
                "SYSTEM Hive##DeletedBamModal",
                &showDeletedBamPopup,
                ImGuiWindowFlags_NoCollapse))
            {
                ImGui::SetWindowSize(ImVec2(800, 320), ImGuiCond_Once);

                std::vector<std::wstring> deletedPathsCopy;
                {
                    std::lock_guard<std::mutex> lock(deletedBamMutex);
                    deletedPathsCopy = deletedBamPopupResultLocal.deletedPaths;
                }

                if (isReadingDeletedBam)
                {
                    ImGui::Dummy(ImVec2(0, ImGui::GetWindowSize().y * 0.35f));
                    ImGui::SetCursorPosX(
                        (ImGui::GetWindowSize().x - ImGui::CalcTextSize("Reading SYSTEM hive...").x) * 0.5f
                    );
                    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Reading SYSTEM hive...");
                }
                else if (deletedPathsCopy.empty())
                {
                    ImGui::Dummy(ImVec2(0, ImGui::GetWindowSize().y * 0.35f));
                    ImGui::SetCursorPosX(
                        (ImGui::GetWindowSize().x - ImGui::CalcTextSize("No deleted BAM entries found.").x) * 0.5f
                    );
                    ImGui::TextColored(ImVec4(0.8f, 0.4f, 0.4f, 1.0f), "No deleted BAM entries found.");
                }
                else
                {
                    if (ImGui::BeginTable("DeletedBAMTable", 1, ImGuiTableFlags_Resizable | ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
                    {
                        ImGui::TableSetupColumn("Deleted BAM Paths", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableHeadersRow();

                        for (const auto& path : deletedPathsCopy)
                        {
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            ImGui::TextUnformatted(ws2s(path).c_str());
                        }
                        ImGui::EndTable();
                    }
                }

                ImGui::EndPopup();
            }
            ImGui::PopStyleVar();

            fadeAlphaDenied += io.DeltaTime * 2.0f;
            if (fadeAlphaDenied > 1.0f) fadeAlphaDenied = 1.0f;

            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, fadeAlphaDenied);

            std::string currentSearchDeniedStr(g_searchDeniedBuffer);
            std::transform(currentSearchDeniedStr.begin(), currentSearchDeniedStr.end(), currentSearchDeniedStr.begin(), ::tolower);

            if (ImGui::BeginPopupModal(
                "Denied BAMRegistry Information##DeniedBamModal",
                &showDeniedPopup,
                ImGuiWindowFlags_NoCollapse))
            {
                ImGui::SetWindowSize(ImVec2(1200, 320), ImGuiCond_Once);

                if (isReadingDenied)
                {
                    ImGui::Dummy(ImVec2(0, ImGui::GetWindowSize().y * 0.35f));
                    ImGui::SetCursorPosX(
                        (ImGui::GetWindowSize().x - ImGui::CalcTextSize("Reading Registry keys...").x) * 0.5f
                    );
                    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Reading Registry keys...");
                }
                else if (deniedPopupResult.empty())
                {
                    ImGui::Dummy(ImVec2(0, ImGui::GetWindowSize().y * 0.35f));
                    ImGui::SetCursorPosX(
                        (ImGui::GetWindowSize().x - ImGui::CalcTextSize("No denied keys found.").x) * 0.5f
                    );
                    ImGui::TextColored(ImVec4(0.8f, 0.4f, 0.4f, 1.0f), "No denied keys found.");
                }
                else
                {
                    std::vector<DeniedRegistryEntry> filteredDenied;
                    for (const auto& e : deniedPopupResult)
                    {
                        if (!currentSearchDeniedStr.empty())
                        {
                            std::string keyLower = ws2s(e.keyPath);
                            std::transform(keyLower.begin(), keyLower.end(), keyLower.begin(), ::tolower);

                            std::string permLower = ws2s(e.deniedPermission);
                            std::transform(permLower.begin(), permLower.end(), permLower.begin(), ::tolower);

                            if (keyLower.find(currentSearchDeniedStr) == std::string::npos &&
                                permLower.find(currentSearchDeniedStr) == std::string::npos)
                                continue;
                        }

                        filteredDenied.push_back(e);
                    }

                    if (ImGui::BeginTable("DeniedTable", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
                    {
                        ImGui::TableSetupColumn("Registry Key", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("Denied Permission", ImGuiTableColumnFlags_WidthFixed, 140.0f);
                        ImGui::TableHeadersRow();

                        for (const auto& e : filteredDenied)
                        {
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            ImGui::TextUnformatted(ws2s(e.keyPath).c_str());
                            ImGui::TableNextColumn();
                            ImGui::TextUnformatted(ws2s(e.deniedPermission).c_str());
                        }
                        ImGui::EndTable();
                    }
                }
                ImGui::EndPopup();
            }

            ImGui::PopStyleVar();

            if (ImGui::BeginTable("BAMTable", 3,
                ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY))
            {
                float col0Width = 0.0f;
                float col2Width = 0.0f;

                for (const auto& e : filteredBam)
                {
                    col0Width = std::max(col0Width, ImGui::CalcTextSize(e.time.c_str()).x);

                    const char* sigText = nullptr;
                    switch (e.signature)
                    {
                    case BamSignature::Signed: sigText = "Signed"; break;
                    case BamSignature::Unsigned: sigText = "Unsigned"; break;
                    case BamSignature::Cheat: sigText = "Cheat"; break;
                    default: sigText = "Not Found"; break;
                    }
                    col2Width = std::max(col2Width, ImGui::CalcTextSize(sigText).x);
                }

                col0Width += 16.0f;
                col2Width += 16.0f;

                ImGui::TableSetupColumn("Executed Time", ImGuiTableColumnFlags_WidthFixed, col0Width);
                ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Signature", ImGuiTableColumnFlags_WidthFixed, col2Width);
                ImGui::TableHeadersRow();

                if (filteredBam.empty())
                {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted("");
                    ImGui::TableSetColumnIndex(1);
                    const char* msg = "No BAM entries found.";
                    float cellWidth = ImGui::GetContentRegionAvail().x;
                    float textWidth = ImGui::CalcTextSize(msg).x;
                    float textPosX = (cellWidth - textWidth) * 0.5f;
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + textPosX);
                    ImGui::TextColored(ImVec4(0.8f, 0.4f, 0.4f, 1.0f), "%s", msg);
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextUnformatted("");
                } else {
                        for (size_t i = 0; i < filteredBam.size(); i++)
                        {
                            const auto& e = filteredBam[i];
                            ImGui::TableNextRow();

                            ImGui::PushID(static_cast<int>(i));

                            ImGui::TableSetColumnIndex(0);
                            ImGui::TextUnformatted(e.time.c_str());

                            ImGui::TableSetColumnIndex(1);
                            ImGui::BeginGroup();

                            std::wstring wname = StringToWString(e.path);
                            IconDataDX11* iconPtr = GetOrQueueIcon(g_Device, wname);
                            if (iconPtr && iconPtr->IsLoaded)
                            {
                                ImGui::Image(iconPtr->TextureView.Get(), ImVec2(16, 16));
                                ImGui::SameLine(0, 5);
                            }

                            ImVec4 pathColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
                            if (e.signature == BamSignature::Cheat)
                                pathColor = ImVec4(1.0f, 0.55f, 0.0f, 1.0f);

                            ImGui::PushStyleColor(ImGuiCol_Text, pathColor);
                            ImGuiSelectableFlags selectableFlags = ImGuiSelectableFlags_SpanAllColumns;
                            if (ImGui::Selectable(e.path.c_str(), selectedRow == (int)i, selectableFlags))
                                selectedRow = (int)i;
                            ImGui::PopStyleColor();

                            std::string popupId = "RowPopup_" + std::to_string(i);
                            if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
                                ImGui::OpenPopup(popupId.c_str());

                            if (ImGui::BeginPopup(popupId.c_str()))
                            {
                                if (ImGui::MenuItem("Open Path"))
                                {
                                    std::wstring folderPath(e.path.begin(), e.path.end());
                                    size_t pos = folderPath.find_last_of(L"\\/");
                                    if (pos != std::wstring::npos)
                                        folderPath = folderPath.substr(0, pos);
                                    if (!folderPath.empty())
                                        ShellExecuteW(NULL, L"explore", folderPath.c_str(), NULL, NULL, SW_SHOWNORMAL);
                                }

                                if (ImGui::MenuItem("Copy Path"))
                                    ImGui::SetClipboardText(e.path.c_str());

                                ImGui::EndPopup();
                            }

                            ImGui::EndGroup();

                            ImGui::TableSetColumnIndex(2);
                            ImVec4 sigColor;
                            const char* sigText = nullptr;
                            switch (e.signature)
                            {
                            case BamSignature::Signed:
                                sigText = "Signed";
                                sigColor = ImVec4(0.0f, 1.0f, 0.0f, 1.0f);
                                break;
                            case BamSignature::Unsigned:
                                sigText = "Unsigned";
                                sigColor = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
                                break;
                            case BamSignature::Cheat:
                                sigText = "Cheat";
                                sigColor = ImVec4(1.0f, 0.55f, 0.0f, 1.0f);
                                break;
                            default:
                                sigText = "Not Found";
                                sigColor = ImVec4(1.0f, 1.0f, 0.0f, 1.0f);
                                break;
                            }
                            ImGui::TextColored(sigColor, "%s", sigText);

                            ImGui::PopID();
                        }
                    }

                    ImGui::EndTable();
                }
                ImGui::PopStyleVar();
            }

        ImGui::End();
        ImGui::Render();
        g_Context->OMSetRenderTargets(1, &g_RTV, nullptr);
        const float clear_color[4] = { 0.1f, 0.1f, 0.1f, 1.0f };
        g_Context->ClearRenderTargetView(g_RTV, clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        g_SwapChain->Present(1, 0);
    }

    g_iconThreadExit = true;
    g_iconCv.notify_all();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    if (g_RTV) g_RTV->Release();
    if (g_SwapChain) g_SwapChain->Release();
    if (g_Context) g_Context->Release();
    if (g_Device) g_Device->Release();

    return 0;
}