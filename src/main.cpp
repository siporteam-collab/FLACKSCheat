#include <windows.h>
#include <tlhelp32.h>
#include <dwmapi.h>
#include <vector>
#include <string>
#include <cmath>
#include <cstdio>
#include <thread>
#include <chrono>

#include <d3d9.h>

#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "d3d9.lib")

// ==================== Конфигурация и структуры ====================
struct ESPConfig {
    bool  enableESP    = true;
    float color[3]     = { 0.0f, 0.9f, 1.0f }; // Неоновый голубой по умолчанию для ESP
    float distance     = 500.0f;
    bool  showBox      = true;
    bool  showName     = true;
    bool  showHealth   = true;
    float thickness    = 1.5f;
};

struct AimConfig {
    bool  enableAim    = false;
    float fov          = 5.0f;
    float smooth       = 5.0f;
    int   targetBone   = 8;
    int   activationKey= VK_ALT;
};

struct PlayerData {
    char  name[32]      = {};
    int   health        = 0;
    float worldPos[3]   = {};
    float screenPos[2]  = {};
    bool  isValid       = false;
};

// ==================== Глобальные переменные ====================
static ESPConfig          g_esp;
static AimConfig          g_aim;
static LPDIRECT3D9        g_pD3D          = nullptr;
static LPDIRECT3DDEVICE9  g_pDevice       = nullptr;
static D3DPRESENT_PARAMETERS g_d3dpp      = {};
static HWND               g_hwndOverlay   = nullptr;
static HWND               g_hwndGame      = nullptr;
static HANDLE             g_hProcess      = nullptr;
static bool               g_showMenu      = true;   

// ==================== Оффсеты CS 1.6 (hw.dll) ====================
const uintptr_t OFFSET_VIEW_MATRIX = 0x00DF5D04; 
const uintptr_t OFFSET_ENTITY_LIST = 0x00C7438C; 

// ==================== Неоновый стиль ImGui (Cyberpunk Theme) ====================
void ApplyCustomStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding    = 10.0f;
    style.ChildRounding     = 6.0f;
    style.FrameRounding     = 6.0f;
    style.GrabRounding      = 6.0f;
    style.PopupRounding     = 6.0f;
    style.ScrollbarRounding = 6.0f;
    style.WindowPadding     = ImVec2(14, 14);
    style.FramePadding      = ImVec2(8, 6);
    style.ItemSpacing       = ImVec2(10, 10);
    style.WindowBorderSize  = 1.0f;
    style.FrameBorderSize   = 1.0f;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg]         = ImVec4(0.06f, 0.06f, 0.09f, 0.94f); // Глубокий темный фон
    colors[ImGuiCol_ChildBg]          = ImVec4(0.09f, 0.09f, 0.13f, 0.60f);
    colors[ImGuiCol_PopupBg]          = ImVec4(0.08f, 0.08f, 0.12f, 0.96f);
    colors[ImGuiCol_Border]           = ImVec4(0.00f, 0.80f, 1.00f, 0.45f); // Неоновая обводка
    colors[ImGuiCol_BorderShadow]     = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_FrameBg]          = ImVec4(0.11f, 0.11f, 0.16f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]   = ImVec4(0.16f, 0.16f, 0.24f, 1.00f);
    colors[ImGuiCol_FrameBgActive]    = ImVec4(0.20f, 0.20f, 0.32f, 1.00f);
    colors[ImGuiCol_TitleBg]          = ImVec4(0.08f, 0.08f, 0.12f, 1.00f);
    colors[ImGuiCol_TitleBgActive]    = ImVec4(0.10f, 0.10f, 0.16f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.06f, 0.06f, 0.09f, 0.75f);
    colors[ImGuiCol_CheckMark]        = ImVec4(0.00f, 0.90f, 1.00f, 1.00f); // Неоновый голубой чекбокс
    colors[ImGuiCol_SliderGrab]       = ImVec4(0.00f, 0.80f, 1.00f, 1.00f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.40f, 0.95f, 1.00f, 1.00f);
    colors[ImGuiCol_Button]           = ImVec4(0.12f, 0.12f, 0.18f, 1.00f);
    colors[ImGuiCol_ButtonHovered]    = ImVec4(0.00f, 0.60f, 0.80f, 0.85f);
    colors[ImGuiCol_ButtonActive]     = ImVec4(0.00f, 0.85f, 1.00f, 1.00f);
    colors[ImGuiCol_Header]           = ImVec4(0.12f, 0.12f, 0.18f, 1.00f);
    colors[ImGuiCol_HeaderHovered]    = ImVec4(0.00f, 0.60f, 0.80f, 0.60f);
    colors[ImGuiCol_HeaderActive]     = ImVec4(0.00f, 0.80f, 1.00f, 0.80f);
    colors[ImGuiCol_Tab]              = ImVec4(0.08f, 0.08f, 0.12f, 1.00f);
    colors[ImGuiCol_TabHovered]       = ImVec4(0.00f, 0.60f, 0.80f, 0.80f);
    colors[ImGuiCol_TabActive]        = ImVec4(0.00f, 0.70f, 0.90f, 1.00f);
    colors[ImGuiCol_Text]             = ImVec4(0.90f, 0.90f, 0.95f, 1.00f);
    colors[ImGuiCol_TextDisabled]     = ImVec4(0.40f, 0.40f, 0.50f, 1.00f);
}

// ==================== Хелперы процесса и модулей ====================
static DWORD GetProcessIdByName(const wchar_t* name) {
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe{ sizeof(pe) };
        if (Process32FirstW(snap, &pe)) {
            do {
                if (_wcsicmp(pe.szExeFile, name) == 0) {
                    pid = pe.th32ProcessID;
                    break;
                }
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }
    return pid;
}

static uintptr_t GetModuleBaseAddress(DWORD pid, const wchar_t* moduleName) {
    uintptr_t baseAddress = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W me{ sizeof(me) };
        if (Module32FirstW(snap, &me)) {
            do {
                if (_wcsicmp(me.szModule, moduleName) == 0) {
                    baseAddress = (uintptr_t)me.modBaseAddr;
                    break;
                }
            } while (Module32NextW(snap, &me));
        }
        CloseHandle(snap);
    }
    return baseAddress;
}

static void SetOverlayClickThrough(bool clickThrough) {
    LONG_PTR exStyle = GetWindowLongPtrW(g_hwndOverlay, GWL_EXSTYLE);
    if (clickThrough)
        exStyle |= WS_EX_TRANSPARENT;   
    else
        exStyle &= ~WS_EX_TRANSPARENT;  

    SetWindowLongPtrW(g_hwndOverlay, GWLP_WNDPROC, (LONG_PTR)GetWindowLongPtrW(g_hwndOverlay, GWLP_WNDPROC)); 
    SetWindowLongPtrW(g_hwndOverlay, GWL_EXSTYLE, exStyle);
    SetWindowPos(g_hwndOverlay, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

static bool CreateD3D9Device() {
    g_pD3D = Direct3DCreate9(D3D_SDK_VERSION);
    if (!g_pD3D) return false;

    ZeroMemory(&g_d3dpp, sizeof(g_d3dpp));
    g_d3dpp.Windowed               = TRUE;
    g_d3dpp.SwapEffect             = D3DSWAPEFFECT_DISCARD;
    g_d3dpp.BackBufferFormat       = D3DFMT_A8R8G8B8;   
    g_d3dpp.BackBufferWidth        = 1;
    g_d3dpp.BackBufferHeight       = 1;
    g_d3dpp.EnableAutoDepthStencil = TRUE;
    g_d3dpp.AutoDepthStencilFormat = D3DFMT_D16;
    g_d3dpp.PresentationInterval   = D3DPRESENT_INTERVAL_IMMEDIATE;

    HRESULT hr = g_pD3D->CreateDevice(
        D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_hwndOverlay,
        D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED,
        &g_d3dpp, &g_pDevice
    );

    if (FAILED(hr)) {
        g_pD3D->Release();
        g_pD3D = nullptr;
        return false;
    }
    return true;
}

// ==================== Неоновое меню ImGui ====================
void RenderImGuiMenu() {
    ImGui::SetNextWindowSize(ImVec2(420, 340), ImGuiCond_FirstUseEver);
    ImGui::Begin("NEON EXTERNAL - CS 1.6", nullptr, ImGuiWindowFlags_NoCollapse);

    ImGui::TextColored(ImVec4(0.0f, 0.9f, 1.0f, 1.0f), "[ INSERT ] — Скрыть / Показать меню");
    ImGui::Spacing();

    if (ImGui::BeginTabBar("CheatTabs")) {
        if (ImGui::BeginTabItem("  ESP  ")) {
            ImGui::Spacing();
            ImGui::Checkbox("Включить ESP", &g_esp.enableESP);
            ImGui::Separator();
            ImGui::Checkbox("Отображать боксы", &g_esp.showBox);
            ImGui::Checkbox("Отображать никнеймы", &g_esp.showName);
            ImGui::Checkbox("Отображать здоровье", &g_esp.showHealth);
            
            ImGui::Spacing();
            ImGui::ColorEdit3("Цвет ESP", g_esp.color, ImGuiColorEditFlags_NoInputs);
            ImGui::SliderFloat("Дистанция", &g_esp.distance, 50.0f, 2000.0f, "%.0f м");
            ImGui::SliderFloat("Толщина линий", &g_esp.thickness, 0.5f, 5.0f, "%.1f px");
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("  AIMBOT  ")) {
            ImGui::Spacing();
            ImGui::Checkbox("Включить Aimbot", &g_aim.enableAim);
            ImGui::Separator();
            
            ImGui::SliderFloat("FOV (Угол обзора)", &g_aim.fov, 1.0f, 30.0f, "%.1f°");
            ImGui::SliderFloat("Smooth (Плавность)", &g_aim.smooth, 1.0f, 20.0f, "%.1f");
            
            const char* bones[] = { "Голова", "Тело" };
            int currentBone = (g_aim.targetBone == 8) ? 0 : 1;
            if (ImGui::Combo("Кость цели", &currentBone, bones, IM_ARRAYSIZE(bones))) {
                g_aim.targetBone = (currentBone == 0) ? 8 : 3;
            }

            ImGui::Spacing();
            ImGui::TextDisabled("Удержание клавиши: [ ALT ]");
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
}

// ==================== Математика WorldToScreen ====================
bool WorldToScreen(float* world, float* screen, float matrix[4][4], int sw, int sh) {
    float w = matrix[3][0]*world[0] + matrix[3][1]*world[1] + matrix[3][2]*world[2] + matrix[3][3];
    if (w < 0.01f) return false;

    float invW = 1.0f / w;
    float x = (matrix[0][0]*world[0] + matrix[0][1]*world[1] + matrix[0][2]*world[2] + matrix[0][3]) * invW;
    float y = (matrix[1][0]*world[0] + matrix[1][1]*world[1] + matrix[1][2]*world[2] + matrix[1][3]) * invW;

    screen[0] = (sw * 0.5f) + (x * (sw * 0.5f));
    screen[1] = (sh * 0.5f) - (y * (sh * 0.5f));
    return true;
}

// ==================== Чтение реальной памяти игры (hw.dll) ====================
std::vector<PlayerData> GetPlayersAndProcessAim(HANDLE hProcess, uintptr_t hwBase, int sw, int sh) {
    std::vector<PlayerData> players;
    if (!hProcess || !hwBase) return players;

    float viewMatrix[4][4];
    ReadProcessMemory(hProcess, (LPCVOID)(hwBase + OFFSET_VIEW_MATRIX), &viewMatrix, sizeof(viewMatrix), nullptr);

    uintptr_t entityListBase = 0;
    ReadProcessMemory(hProcess, (LPCVOID)(hwBase + OFFSET_ENTITY_LIST), &entityListBase, sizeof(entityListBase), nullptr);
    if (!entityListBase) return players;

    for (int i = 1; i <= 32; i++) {
        uintptr_t entityPtr = 0;
        ReadProcessMemory(hProcess, (LPCVOID)(entityListBase + (i * 4)), &entityPtr, sizeof(entityPtr), nullptr);
        
        if (!entityPtr) continue;

        PlayerData p;
        ReadProcessMemory(hProcess, (LPCVOID)(entityPtr + 0x94), &p.health, sizeof(p.health), nullptr);
        if (p.health <= 0 || p.health > 100) continue;

        ReadProcessMemory(hProcess, (LPCVOID)(entityPtr + 0x304), &p.worldPos, sizeof(p.worldPos), nullptr);
        ReadProcessMemory(hProcess, (LPCVOID)(entityPtr + 0x10), &p.name, 31, nullptr);
        p.name[31] = '\0';

        if (WorldToScreen(p.worldPos, p.screenPos, viewMatrix, sw, sh)) {
            p.isValid = true;
            players.push_back(p);
        }
    }

    return players;
}

// ==================== Отрисовка ESP ====================
void DrawESPOverlay(ImDrawList* drawList, const std::vector<PlayerData>& players) {
    if (!g_esp.enableESP || !drawList) return;

    ImU32 col = IM_COL32(
        (int)(g_esp.color[0] * 255.0f),
        (int)(g_esp.color[1] * 255.0f),
        (int)(g_esp.color[2] * 255.0f),
        255
    );

    for (const auto& p : players) {
        if (!p.isValid || p.health <= 0) continue;

        float x = p.screenPos[0];
        float y = p.screenPos[1];
        const float h = 60.0f;
        const float w = 30.0f;

        if (g_esp.showBox)
            drawList->AddRect(ImVec2(x - w/2, y - h), ImVec2(x + w/2, y), col, 0.0f, 0, g_esp.thickness);

        if (g_esp.showName) {
            ImVec2 ts = ImGui::CalcTextSize(p.name);
            drawList->AddText(ImVec2(x - ts.x/2, y - h - 16), col, p.name);
        }

        if (g_esp.showHealth) {
            int hp = (p.health > 100) ? 100 : (p.health < 0 ? 0 : p.health);
            float hpHeight = h * ((float)hp / 100.0f);
            drawList->AddRectFilled(ImVec2(x - w/2 - 6, y - h), ImVec2(x - w/2 - 3, y), IM_COL32(40, 40, 50, 220));
            drawList->AddRectFilled(ImVec2(x - w/2 - 6, y - hpHeight), ImVec2(x - w/2 - 3, y), IM_COL32(0, 255, 180, 255));
        }
    }
}

// ==================== WndProc ====================
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (g_showMenu && ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ==================== Точка входа wWinMain ====================
int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    g_hwndGame = FindWindowW(L"Valve001", nullptr);
    if (!g_hwndGame) {
        MessageBoxW(nullptr, L"CS 1.6 не запущена.\nЗапусти игру и попробуй снова.", L"External Cheat", MB_ICONERROR);
        return 1;
    }

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"CS16OverlayClass";
    RegisterClassExW(&wc);

    RECT rect = {};
    GetWindowRect(g_hwndGame, &rect);
    int width  = rect.right  - rect.left;
    int height = rect.bottom - rect.top;

    g_hwndOverlay = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW,
        wc.lpszClassName, L"Overlay",
        WS_POPUP, rect.left, rect.top, width, height,
        nullptr, nullptr, hInstance, nullptr
    );
    if (!g_hwndOverlay) return 1;

    {
        MARGINS margins = { -1, -1, -1, -1 };
        DwmExtendFrameIntoClientArea(g_hwndOverlay, &margins);
    }

    ShowWindow(g_hwndOverlay, SW_SHOW);
    UpdateWindow(g_hwndOverlay);

    SetOverlayClickThrough(!g_showMenu);

    if (!CreateD3D9Device()) {
        MessageBoxW(nullptr, L"Не удалось создать D3D9 устройство.", L"Error", MB_ICONERROR);
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    
    ApplyCustomStyle();

    ImGui_ImplWin32_Init(g_hwndOverlay);
    ImGui_ImplDX9_Init(g_pDevice);

    DWORD pid = GetProcessIdByName(L"hl.exe");
    uintptr_t hwBase = 0;
    if (pid) {
        g_hProcess = OpenProcess(
            PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION,
            FALSE, pid
        );
        if (g_hProcess) {
            hwBase = GetModuleBaseAddress(pid, L"hw.dll");
        }
    }

    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        static bool insertPressed = false;
        if (GetAsyncKeyState(VK_INSERT) & 0x8000) {
            if (!insertPressed) {
                g_showMenu = !g_showMenu;
                SetOverlayClickThrough(!g_showMenu);
                if (!g_showMenu && g_hwndGame) {
                    SetForegroundWindow(g_hwndGame);
                }
                insertPressed = true;
            }
        } else {
            insertPressed = false;
        }

        if (g_hwndGame && !IsWindow(g_hwndGame)) break;

        if (g_hwndGame) {
            static RECT lastRect = {};
            RECT cur = {};
            GetWindowRect(g_hwndGame, &cur);
            if (memcmp(&cur, &lastRect, sizeof(RECT)) != 0) {
                width  = cur.right  - cur.left;
                height = cur.bottom - cur.top;

                SetWindowPos(g_hwndOverlay, HWND_TOPMOST, cur.left, cur.top, width, height, SWP_NOACTIVATE | SWP_NOREDRAW);

                if (g_d3dpp.BackBufferWidth != (UINT)width || g_d3dpp.BackBufferHeight != (UINT)height) {
                    g_d3dpp.BackBufferWidth  = width;
                    g_d3dpp.BackBufferHeight = height;

                    ImGui_ImplDX9_InvalidateDeviceObjects();
                    g_pDevice->Reset(&g_d3dpp);
                    ImGui_ImplDX9_CreateDeviceObjects();
                }
                lastRect = cur;
            }
        }

        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        if (g_showMenu) {
            RenderImGuiMenu();
        }

        auto players = GetPlayersAndProcessAim(g_hProcess, hwBase, width, height);
        DrawESPOverlay(ImGui::GetBackgroundDrawList(), players);

        ImGui::EndFrame();
        ImGui::Render();

        g_pDevice->SetRenderState(D3DRS_ZENABLE, FALSE);
        g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        g_pDevice->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);

        g_pDevice->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0);

        if (SUCCEEDED(g_pDevice->BeginScene())) {
            ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
            g_pDevice->EndScene();
        }

        HRESULT hr = g_pDevice->Present(nullptr, nullptr, nullptr, nullptr);
        if (hr == D3DERR_DEVICELOST) {
            if (g_pDevice->TestCooperativeLevel() == D3DERR_DEVICENOTRESET) {
                ImGui_ImplDX9_InvalidateDeviceObjects();
                g_pDevice->Reset(&g_d3dpp);
                ImGui_ImplDX9_CreateDeviceObjects();
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (g_hProcess) CloseHandle(g_hProcess);

    ImGui_ImplDX9_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    if (g_pDevice) g_pDevice->Release();
    if (g_pD3D)    g_pD3D->Release();

    DestroyWindow(g_hwndOverlay);
    UnregisterClassW(wc.lpszClassName, hInstance);

    return 0;
}