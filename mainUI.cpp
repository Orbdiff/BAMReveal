#ifndef IM_PI
#define IM_PI 3.14159265358979323846f
#endif

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"
#include "bam/BAM.h"
#include "bam/BAMSysInspector.h"
#include "bam/BAMEntries.h"
#include "privilege/SeDebugPrivilege.h"

#include <d3d11.h>
#include <tchar.h>
#include <windows.h>
#include <string>
#include <vector>
#include <codecvt>
#include <locale>
#include <thread>
#include <algorithm>
#include <shlwapi.h>

#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

std::vector<BAMReader::Entry> bamEntries;
std::thread bamThread;
static std::string inspectionText;
static ImVec2 window_pos = ImVec2(0, 0);
static ImVec2 window_size;
bool bamLoaded = false;
bool bamThreadStarted = false;
static bool showModal = false;
static bool showModalDeleted = false;
static bool is_maximized = true;
static bool window_open = true;

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

ID3D11Device* g_pd3dDevice = nullptr;
ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
IDXGISwapChain* g_pSwapChain = nullptr;
ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;
ImFont* g_Font = nullptr;

// + OR - YEP!
inline ImVec2 operator+(const ImVec2& lhs, const ImVec2& rhs) {
    return ImVec2(lhs.x + rhs.x, lhs.y + rhs.y);
}

inline ImVec2 operator-(const ImVec2& lhs, const ImVec2& rhs) {
    return ImVec2(lhs.x - rhs.x, lhs.y - rhs.y);
}

std::string WStringToString(const std::wstring& wstr) {
    std::wstring_convert<std::codecvt_utf8<wchar_t>> conv;
    return conv.to_bytes(wstr);
}

void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

void CleanupRenderTarget() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[1] = { D3D_FEATURE_LEVEL_11_0 };
    if (D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, featureLevelArray, 1,
        D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext) != S_OK)
        return false;

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    
    // SeDebugPrivilege
    EnableDebugPrivilege();

    WNDCLASSEX wc = { sizeof(WNDCLASSEX), CS_CLASSDC, WndProc, 0L, 0L,
                      GetModuleHandle(NULL), NULL, NULL, NULL, NULL,
                      _T("ImGuiWindowClass"), NULL };
    RegisterClassEx(&wc);

    HWND hwnd = CreateWindow(
        wc.lpszClassName, _T("BAMReveal"),
        WS_POPUP, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720,
        NULL, NULL, wc.hInstance, NULL);

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        UnregisterClass(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    RECT workArea;
    SystemParametersInfo(SPI_GETWORKAREA, 0, &workArea, 0);

    SetWindowPos(hwnd, HWND_TOP,
        workArea.left, workArea.top,
        workArea.right - workArea.left,
        workArea.bottom - workArea.top,
        SWP_NOZORDER | SWP_FRAMECHANGED);

    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;

    // Stlye
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;
    
    colors[ImGuiCol_Text] = ImVec4(0.95f, 0.96f, 0.98f, 1.00f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.55f, 0.60f, 1.00f);
    colors[ImGuiCol_WindowBg] = ImVec4(0.11f, 0.12f, 0.14f, 1.00f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.10f, 0.10f, 0.11f, 1.00f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.08f, 0.08f, 0.10f, 0.94f);
    colors[ImGuiCol_Border] = ImVec4(0.29f, 0.29f, 0.30f, 0.60f);
    colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.10f);

    colors[ImGuiCol_FrameBg] = ImVec4(0.20f, 0.21f, 0.23f, 1.00f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.30f, 0.31f, 0.33f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.25f, 0.26f, 0.28f, 1.00f);

    colors[ImGuiCol_TitleBg] = ImVec4(0.14f, 0.14f, 0.18f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.18f, 0.19f, 0.23f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.09f, 0.09f, 0.09f, 0.50f);

    colors[ImGuiCol_MenuBarBg] = ImVec4(0.13f, 0.14f, 0.15f, 1.00f);
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.02f, 0.02f, 0.02f, 0.39f);
    colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.20f, 0.25f, 0.30f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.25f, 0.30f, 0.35f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.30f, 0.35f, 0.40f, 1.00f);

    colors[ImGuiCol_CheckMark] = ImVec4(0.40f, 0.70f, 0.90f, 1.00f);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.30f, 0.60f, 0.80f, 1.00f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.40f, 0.70f, 0.90f, 1.00f);

    colors[ImGuiCol_Button] = ImVec4(0.20f, 0.30f, 0.45f, 0.70f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.30f, 0.45f, 0.60f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.25f, 0.40f, 0.55f, 1.00f);

    colors[ImGuiCol_Header] = ImVec4(0.20f, 0.25f, 0.35f, 0.80f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.25f, 0.30f, 0.40f, 0.80f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.30f, 0.35f, 0.45f, 1.00f);

    colors[ImGuiCol_Separator] = ImVec4(0.40f, 0.40f, 0.45f, 0.50f);
    colors[ImGuiCol_SeparatorHovered] = ImVec4(0.60f, 0.60f, 0.70f, 1.00f);
    colors[ImGuiCol_SeparatorActive] = ImVec4(0.70f, 0.70f, 0.90f, 1.00f);

    colors[ImGuiCol_ResizeGrip] = ImVec4(0.20f, 0.25f, 0.30f, 0.70f);
    colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.30f, 0.35f, 0.40f, 0.80f);
    colors[ImGuiCol_ResizeGripActive] = ImVec4(0.40f, 0.45f, 0.50f, 1.00f);

    colors[ImGuiCol_Tab] = ImVec4(0.15f, 0.18f, 0.23f, 1.00f);
    colors[ImGuiCol_TabHovered] = ImVec4(0.28f, 0.30f, 0.37f, 1.00f);
    colors[ImGuiCol_TabActive] = ImVec4(0.20f, 0.22f, 0.27f, 1.00f);
    colors[ImGuiCol_TabUnfocused] = ImVec4(0.15f, 0.16f, 0.20f, 1.00f);
    colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.17f, 0.18f, 0.22f, 1.00f);

    colors[ImGuiCol_TableHeaderBg] = ImVec4(0.15f, 0.16f, 0.20f, 1.00f);
    colors[ImGuiCol_TableBorderStrong] = ImVec4(0.35f, 0.35f, 0.40f, 1.00f);
    colors[ImGuiCol_TableBorderLight] = ImVec4(0.25f, 0.25f, 0.30f, 1.00f);
    colors[ImGuiCol_TableRowBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_TableRowBgAlt] = ImVec4(0.10f, 0.10f, 0.13f, 0.25f);

	// General style settings
    style.FrameRounding = 6.0f;
    style.GrabRounding = 4.0f;
    style.WindowRounding = 6.0f;
    style.ScrollbarRounding = 6.0f;
    style.TabRounding = 4.0f;

    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.ScrollbarSize = 14.0f;

    style.ItemSpacing = ImVec2(10, 8);
    style.ItemInnerSpacing = ImVec2(8, 6);
    style.CellPadding = ImVec2(6, 4);
    style.WindowPadding = ImVec2(12, 12);
    style.FramePadding = ImVec2(10, 6);

    // Font
    ImFontConfig fontConfig;
    fontConfig.OversampleH = 3;
    fontConfig.OversampleV = 3;
    fontConfig.PixelSnapH = true;
    g_Font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\arial.ttf", 17.5f, &fontConfig);

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    MSG msg;
    ZeroMemory(&msg, sizeof(msg));
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, NULL, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            continue;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        ImGui::PushFont(g_Font);

        if (!bamThreadStarted) {
            bamThreadStarted = true;
            bamThread = std::thread([&]() {
                BAMReader reader;
                auto results = reader.GetBAMValues();

                for (auto& entry : results) {
                    if (!entry.path.empty() && GetFileAttributesW(entry.path.c_str()) != INVALID_FILE_ATTRIBUTES) {
                        std::wstring widePath = entry.path;
                        entry.isSigned = VerifyFileSignature(widePath.c_str());
                    }
                    else {
                        entry.isSigned = false;
                    }
                }

                bamEntries = std::move(results);
                bamLoaded = true;
                });
            bamThread.detach();
        }

        ImGuiIO& io = ImGui::GetIO();

        if (is_maximized) {
            window_pos = ImVec2(0, 0);
            window_size = io.DisplaySize;
        }

        if (!is_maximized) {
            window_size = ImVec2(800, 600);
            window_pos = ImVec2((io.DisplaySize.x - window_size.x) * 0.5f,
                (io.DisplaySize.y - window_size.y) * 0.5f);

            SetWindowPos(hwnd, NULL,
                static_cast<int>(window_pos.x),
                static_cast<int>(window_pos.y),
                static_cast<int>(window_size.x),
                static_cast<int>(window_size.y),
                SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
        else {
            RECT workArea;
            SystemParametersInfo(SPI_GETWORKAREA, 0, &workArea, 0);

            int width = workArea.right - workArea.left;
            int height = workArea.bottom - workArea.top;

            window_pos = ImVec2((float)workArea.left, (float)workArea.top);
            window_size = ImVec2((float)width, (float)height);

            SetWindowPos(hwnd, NULL,
                workArea.left,
                workArea.top,
                width,
                height,
                SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }

        ImGui::SetNextWindowPos(window_pos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(window_size, ImGuiCond_Always);
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;

        ImGui::Begin("BAMReveal", &window_open, flags);
        {
            float title_bar_height = 30.0f;

            ImGui::BeginChild("TitleBar", ImVec2(ImGui::GetWindowWidth(), title_bar_height), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            {
                ImGui::SetCursorPosX(10);
                ImGui::SetCursorPosY((title_bar_height - ImGui::GetTextLineHeight()) * 0.5f);
                ImGui::Text("BAMReveal");

                float button_width = 35.0f;
                float buttons_total_width = button_width * 2;
                float right_padding = 50.0f;

                ImGui::SameLine(ImGui::GetWindowWidth() - buttons_total_width - right_padding);
                if (ImGui::Button(is_maximized ? "[-]" : "[+]", ImVec2(button_width, title_bar_height))) {
                    is_maximized = !is_maximized;
                    if (!is_maximized) {
                        window_size = ImVec2(800, 600);
                        window_pos = ImVec2((io.DisplaySize.x - window_size.x) * 0.5f,
                            (io.DisplaySize.y - window_size.y) * 0.5f);
                    }
                }

                ImGui::SameLine();
                if (ImGui::Button("X", ImVec2(button_width, title_bar_height))) {
                    exit(0);
                }
            }
            ImGui::EndChild();

            ImVec2 child_size = ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y - title_bar_height);
            ImGui::BeginChild("Loading", child_size, true, ImGuiWindowFlags_HorizontalScrollbar);

            if (!bamLoaded) {
                ImVec2 pos = ImGui::GetWindowPos();
                ImVec2 size = ImGui::GetWindowSize();
                ImVec2 center = ImVec2(pos.x + size.x * 0.5f, pos.y + size.y * 0.5f);

                float radius = 16.0f;
                float thickness = 3.0f;
                float speed = 4.0f;
                float angle = ImGui::GetTime() * speed;

                ImDrawList* draw_list = ImGui::GetWindowDrawList();
                draw_list->PathClear();
                for (int i = 0; i < 12; ++i) {
                    float a = angle + i * IM_PI * 2.0f / 12.0f;
                    ImVec2 p = ImVec2(center.x + cosf(a) * radius, center.y + sinf(a) * radius);
                    draw_list->PathLineTo(p);
                }
                draw_list->PathStroke(ImColor(0, 120, 255, 180), false, thickness);

                static const char* messages[] = {
                    "Parse BAM Entries...",
                    "Parse digital signatures...",
                    "Checking bam.sys thread...",
                    "Parse Deleted BAM Entries...",
                    "Finalizing the parse..."
                };
                const int numMessages = sizeof(messages) / sizeof(messages[0]);

                int index = static_cast<int>(ImGui::GetTime() / 9.0f);
                if (index >= numMessages) index = numMessages - 1;

                ImVec2 textSize = ImGui::CalcTextSize(messages[index]);
                ImVec2 textPos = ImVec2(center.x - textSize.x * 0.5f, center.y + radius + 10.0f);
                draw_list->AddText(textPos, IM_COL32(160, 160, 160, 255), messages[index]);
            } else {

                static bool showModal = false;

                ImGui::BeginGroup();

				// Search bar
                static char searchBuffer[128] = "";
                ImGui::InputTextWithHint("##Search", "Search...", searchBuffer, IM_ARRAYSIZE(searchBuffer));

                float spacing = ImGui::GetStyle().ItemSpacing.x;
                float padding = ImGui::GetStyle().FramePadding.x * 2;

                float button1Width = ImGui::CalcTextSize("BAMEntries Deleted").x + padding;
                float button2Width = ImGui::CalcTextSize("CreationDate BAM").x + padding;

                float totalWidth = button1Width + spacing + button2Width;
                float availableWidth = ImGui::GetContentRegionAvail().x;

                ImGui::SameLine(availableWidth - totalWidth);

                // Button1: "BAMEntries Delete"
                if (ImGui::Button("BAMEntries Deleted", ImVec2(button1Width, 0))) {
                    inspectionText.clear();

                    std::wstring result = SearchInFile();
                    inspectionText = WStringToString(result);

                    showModalDeleted = true;
                }

                ImGui::SameLine();

                // Button 2: "CreationDate BAM"
                if (ImGui::Button("CreationDate BAM", ImVec2(button2Width, 0))) {
                    inspectionText.clear();

                    std::wstring result = FindBamSysThreadText();
                    if (!result.empty()) {
                        result += L"\n" + GetSystemBootTimeText();
                    }
                    inspectionText = WStringToString(result);

                    showModal = true;
                }

                ImGui::EndGroup();

                if (ImGui::BeginTable("BAMTable", 3, ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Reorderable)) {

                    ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_None);
                    ImGui::TableSetupColumn("Execution Time", ImGuiTableColumnFlags_DefaultSort, ImGuiTableColumnFlags_None);
                    ImGui::TableSetupColumn("Signature Status", ImGuiTableColumnFlags_None);
                    ImGui::TableHeadersRow();

                    ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs();
                    if (sortSpecs && sortSpecs->SpecsDirty) {
                        const ImGuiTableColumnSortSpecs& spec = sortSpecs->Specs[0];

                        std::sort(bamEntries.begin(), bamEntries.end(),
                            [&](const auto& a, const auto& b) {
                                switch (spec.ColumnIndex) {
                                case 1: // Timestamp
                                    return (spec.SortDirection == ImGuiSortDirection_Ascending) ?
                                        (a.timestamp < b.timestamp) : (a.timestamp > b.timestamp);

                                case 2: // Signature status
                                    return (spec.SortDirection == ImGuiSortDirection_Ascending) ?
                                        (a.isSigned < b.isSigned) : (a.isSigned > b.isSigned);

                                default:
                                    return false;
                                }
                            });

                        sortSpecs->SpecsDirty = false;
                    }

                    std::string search = searchBuffer;
                    std::transform(search.begin(), search.end(), search.begin(), ::tolower);

                    for (const auto& entry : bamEntries) {
                        std::string pathStrs = WStringToString(entry.path);
                        std::string sigStr = entry.isSigned ? "signed" : "unsigned";

                        std::string pathLower = pathStrs;
                        std::transform(pathLower.begin(), pathLower.end(), pathLower.begin(), ::tolower);
                        std::transform(sigStr.begin(), sigStr.end(), sigStr.begin(), ::tolower);

                        if (!search.empty() &&
                            pathLower.find(search) == std::string::npos &&
                            sigStr.find(search) == std::string::npos)
                            continue;

                        ImGui::TableNextRow();

                        ImGui::TableSetColumnIndex(0);

                        std::string pathStr = WStringToString(entry.path);
                        bool isDevicePath = (pathStr.rfind("\\Device\\", 0) == 0);

                        if (isDevicePath) {
                            ImGui::TextColored(ImVec4(0.7f, 0.2f, 0.9f, 1.0f), "%s", pathStr.c_str());
                        }
                        else {
                            ImGui::Text("%s", pathStr.c_str());
                        }

                        ImGui::PushID(&entry);

                        if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
                            ImGui::OpenPopup("popup_path_options");
                        }

                        if (ImGui::BeginPopup("popup_path_options")) {

                            if (ImGui::Selectable("Copy Path")) {
                                std::wstring wpath = entry.path;
                                wchar_t folderPath[MAX_PATH];
                                wcscpy_s(folderPath, wpath.c_str());
                                PathRemoveFileSpecW(folderPath);

                                if (OpenClipboard(NULL)) {
                                    EmptyClipboard();
                                    size_t sizeInBytes = (wcslen(folderPath) + 1) * sizeof(wchar_t);
                                    HGLOBAL hGlob = GlobalAlloc(GMEM_MOVEABLE, sizeInBytes);
                                    if (hGlob) {
                                        void* pGlob = GlobalLock(hGlob);
                                        memcpy(pGlob, folderPath, sizeInBytes);
                                        GlobalUnlock(hGlob);
                                        SetClipboardData(CF_UNICODETEXT, hGlob);
                                    }
                                    CloseClipboard();
                                }
                            }
                            if (ImGui::Selectable("Open Path")) {
                                std::wstring wpath = entry.path;
                                wchar_t folderPath[MAX_PATH];
                                wcscpy_s(folderPath, wpath.c_str());
                                PathRemoveFileSpecW(folderPath);
                                ShellExecuteW(NULL, L"explore", folderPath, NULL, NULL, SW_SHOWNORMAL);
                            }
                            ImGui::EndPopup();
                        }

                        ImGui::PopID();

                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text("%s", WStringToString(entry.timestamp).c_str());

                        ImGui::TableSetColumnIndex(2);
                        if (GetFileAttributesW(entry.path.c_str()) == INVALID_FILE_ATTRIBUTES) {
                            ImGui::TextColored(ImVec4(0.8f, 0.5f, 0.1f, 1.0f), "Not Found");
                        }
                        else {
                            ImGui::TextColored(
                                entry.isSigned ? ImVec4(0.2f, 0.8f, 0.2f, 1.0f) : ImVec4(0.9f, 0.2f, 0.2f, 1.0f),
                                entry.isSigned ? "Signed" : "Unsigned"
                            );
                        }
                    }
                    ImGui::EndTable();
                }

                if (showModal) {
                    ImGui::OpenPopup("CreationDate BAM");
                    ImGui::SetNextWindowSize(ImVec2(400, 200), ImGuiCond_Appearing);

                    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
                    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
                    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 6.0f);

                    if (ImGui::BeginPopupModal("CreationDate BAM", NULL, ImGuiWindowFlags_NoResize)) {
                        float windowWidth = ImGui::GetWindowWidth();
                        float buttonSize = ImGui::GetFrameHeight();
                        float spacing = ImGui::GetStyle().ItemSpacing.x;

                        ImGui::TextUnformatted(inspectionText.c_str());
                        ImGui::SameLine();
                        ImGui::SetCursorPosX(windowWidth - buttonSize - spacing);

                        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
                        if (ImGui::Button("X", ImVec2(buttonSize, buttonSize))) {
                            ImGui::CloseCurrentPopup();
                            showModal = false;
                        }
                        ImGui::PopStyleVar();
                        ImGui::EndPopup();
                    }

                    ImGui::PopStyleVar(4);
                }

                if (showModalDeleted) {
                    ImGui::OpenPopup("BAMEntries Deleted");
                    ImGui::SetNextWindowSize(ImVec2(800, 500), ImGuiCond_Appearing);

                    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
                    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
                    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 6.0f);

                    if (ImGui::BeginPopupModal("BAMEntries Deleted", NULL, ImGuiWindowFlags_NoResize)) {
                        float windowWidth = ImGui::GetWindowWidth();
                        float buttonSize = ImGui::GetFrameHeight();
                        float spacing = ImGui::GetStyle().ItemSpacing.x;

                        ImGui::TextUnformatted(inspectionText.c_str());
                        ImGui::SameLine();
                        ImGui::SetCursorPosX(windowWidth - buttonSize - spacing);

                        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
                        if (ImGui::Button("X", ImVec2(buttonSize, buttonSize))) {
                            ImGui::CloseCurrentPopup();
                            showModalDeleted = false;
                        }
                        ImGui::PopStyleVar();
                        ImGui::EndPopup();
                    }

                    ImGui::PopStyleVar(4);
                }

            }
        }

        ImGui::EndChild();
        ImGui::End();
        ImGui::PopFont();
        ImGui::Render();

        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        const float clear_color[4] = { 0.87f, 0.87f, 0.87f, 1.00f };
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0);
    }

    // Cleanup
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    UnregisterClass(wc.lpszClassName, wc.hInstance);
    return 0;
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_SIZE:
        if (g_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;

    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU)
            return 0;
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}