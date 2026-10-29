#include <windows.h>
#include <tlhelp32.h>
#include <dwmapi.h>
#include <vector>
#include <string>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <chrono>
#include <cstdint>

#include <d3d9.h>

#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "d3d9.lib")

// ==================== Структуры и конфигурация ====================
struct ESPConfig {
    bool  enableESP    = true;
    float color[3]     = { 0.0f, 0.9f, 1.0f }; 
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
    int   activationKey= 'X'; 
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
static int                g_language = 0; 
static float              g_menuColor[3] = { 0.0f, 0.9f, 1.0f }; 

static LPDIRECT3D9        g_pD3D          = nullptr;
static LPDIRECT3DDEVICE9  g_pDevice       = nullptr;
static D3DPRESENT_PARAMETERS g_d3dpp      = {};
static HWND               g_hwndOverlay   = nullptr;
static HWND               g_hwndGame      = nullptr;
static HANDLE             g_hProcess      = nullptr;
static bool               g_showMenu      = true;   

static ImVec2             g_menuPos       = ImVec2(0, 0);
static ImVec2             g_menuSize      = ImVec2(0, 0);

// Переменные для отладки (вкладка TEST)
static uintptr_t g_debug_entityList = 0;
static uintptr_t g_debug_localPlayer = 0;
static int       g_debug_myHealth = -1;
static bool      g_debug_viewMatrixValid = false;

// Список найденных оффсетов здоровья (для Bruteforce)
static std::vector<uintptr_t> g_healthOffsets;

// ==================== НОВЫЕ ОФФСЕТЫ ====================
const uintptr_t OFFSET_ENTITY_LIST = 0x10313C;   // Твой EnemyList
const uintptr_t OFFSET_LOCAL_PLAYER = 0x100DE4;  // Твой LocalPlayer
const uintptr_t OFFSET_MY_HP       = 0x100774;   // Твой PlayerHP (статический)
const uintptr_t OFFSET_ENEMY_HEALTH = 0x1BC;     // HP врага (относительно его структуры)
const uintptr_t OFFSET_VIEW_MATRIX = 0x00DF5D04; // Матрица камеры (нужно проверить)

// ==================== Функция перевода ====================
const char* T(const char* ru, const char* en) {
    return (g_language == 0) ? ru : en;
}

// ==================== Стиль ====================
void ApplyCustomStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding     = ImVec2(10, 10);
    style.FramePadding      = ImVec2(6, 4);
    style.ItemSpacing       = ImVec2(8, 8);
    style.WindowRounding    = 4.0f;
    style.FrameRounding     = 2.0f;
    style.GrabRounding      = 2.0f;

    ImVec4* colors = style.Colors;
    ImVec4 mainColor = ImVec4(g_menuColor[0], g_menuColor[1], g_menuColor[2], 1.0f);
    
    colors[ImGuiCol_WindowBg]         = ImVec4(0.08f, 0.08f, 0.08f, 0.95f); 
    colors[ImGuiCol_ChildBg]          = ImVec4(0.05f, 0.05f, 0.05f, 0.50f);
    colors[ImGuiCol_Border]           = ImVec4(mainColor.x, mainColor.y, mainColor.z, 0.30f);
    colors[ImGuiCol_FrameBg]          = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]   = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
    colors[ImGuiCol_FrameBgActive]    = ImVec4(0.40f, 0.40f, 0.40f, 1.00f);
    colors[ImGuiCol_CheckMark]        = ImVec4(1.0f, 1.0f, 1.0f, 1.00f); 
    colors[ImGuiCol_SliderGrab]       = mainColor;
    colors[ImGuiCol_SliderGrabActive] = mainColor;
    colors[ImGuiCol_Tab]              = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
    colors[ImGuiCol_TabHovered]       = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    colors[ImGuiCol_TabActive]        = mainColor; 
    colors[ImGuiCol_Text]             = ImVec4(0.90f, 0.90f, 0.90f, 1.00f);
}

// ==================== Хелперы процесса ====================
static DWORD GetProcessIdByName(const wchar_t* name) {
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe{ sizeof(pe) };
        if (Process32FirstW(snap, &pe)) {
            do {
                if (_wcsicmp(pe.szExeFile, name) == 0) { pid = pe.th32ProcessID; break; }
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
                if (_wcsicmp(me.szModule, moduleName) == 0) { baseAddress = (uintptr_t)me.modBaseAddr; break; }
            } while (Module32NextW(snap, &me));
        }
        CloseHandle(snap);
    }
    return baseAddress;
}

static bool CreateD3D9Device() {
    g_pD3D = Direct3DCreate9(D3D_SDK_VERSION);
    if (!g_pD3D) return false;
    ZeroMemory(&g_d3dpp, sizeof(g_d3dpp));
    g_d3dpp.Windowed = TRUE;
    g_d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    g_d3dpp.BackBufferFormat = D3DFMT_A8R8G8B8;   
    g_d3dpp.BackBufferWidth = 1; g_d3dpp.BackBufferHeight = 1;
    g_d3dpp.EnableAutoDepthStencil = TRUE;
    g_d3dpp.AutoDepthStencilFormat = D3DFMT_D16;
    g_d3dpp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

    HRESULT hr = g_pD3D->CreateDevice(
        D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_hwndOverlay,
        D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED,
        &g_d3dpp, &g_pDevice
    );
    if (FAILED(hr)) { g_pD3D->Release(); g_pD3D = nullptr; return false; }
    return true;
}

// ==================== Функция Bruteforce ====================
void ScanForHealthOffsets(HANDLE hProcess, uintptr_t localPlayer) {
    g_healthOffsets.clear();
    if (!hProcess || !localPlayer) return;

    int healthVal = 0;
    for (uintptr_t offset = 0; offset < 0x1000; offset += 4) {
        if (ReadProcessMemory(hProcess, (LPCVOID)(localPlayer + offset), &healthVal, sizeof(healthVal), nullptr)) {
            if (healthVal >= 0 && healthVal <= 100) {
                g_healthOffsets.push_back(offset);
            }
        }
    }
}

// ==================== Меню ImGui ====================
void RenderImGuiMenu() {
    ImGui::SetNextWindowSize(ImVec2(520, 400), ImGuiCond_FirstUseEver);
    ImGui::Begin("##Menu", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);

    g_menuPos = ImGui::GetWindowPos();
    g_menuSize = ImGui::GetWindowSize();

    ImGui::TextColored(ImVec4(g_menuColor[0], g_menuColor[1], g_menuColor[2], 1.0f), "FLACKSCheat V1 [ CS 1.6 ]");
    ImGui::SameLine(ImGui::GetWindowWidth() - 120);
    ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "FPS: %.0f", ImGui::GetIO().Framerate);
    ImGui::SameLine(ImGui::GetWindowWidth() - 30);
    if (ImGui::Button("X", ImVec2(20, 20))) g_showMenu = false; 
    
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::BeginTabBar("CheatTabs", ImGuiTabBarFlags_FittingPolicyScroll)) {
        
        // ================= ESP =================
        if (ImGui::BeginTabItem(T("  ESP  ", "  ESP  "))) {
            ImGui::Spacing();
            ImGui::Columns(3, "esp_cols", false);
            ImGui::Checkbox(T("Боксы", "Box"), &g_esp.showBox);
            ImGui::Checkbox(T("Линии", "Line"), &g_esp.showName); 
            ImGui::NextColumn();
            ImGui::Checkbox(T("Никнейм", "Name"), &g_esp.showName);
            ImGui::Checkbox(T("Здоровье", "Health"), &g_esp.showHealth);
            ImGui::NextColumn();
            ImGui::Checkbox(T("Включить ESP", "Enable ESP"), &g_esp.enableESP);
            ImGui::Spacing();
            ImGui::TextDisabled(T("Дистанция", "Distance"));
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderFloat("##dist", &g_esp.distance, 50.0f, 2000.0f, "%.0f m");
            ImGui::Columns(1); 
            ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
            ImGui::Text(T("Настройки визуала:", "Visual Settings:"));
            ImGui::ColorEdit3(T("Цвет ESP", "ESP Color"), g_esp.color, ImGuiColorEditFlags_NoInputs);
            ImGui::SliderFloat(T("Толщина", "Thickness"), &g_esp.thickness, 0.5f, 5.0f, "%.1f px");
            ImGui::EndTabItem();
        }

        // ================= AIM =================
        if (ImGui::BeginTabItem(T("  AIM  ", "  AIM  "))) {
            ImGui::Spacing();
            ImGui::Columns(2, "aim_cols", false);
            ImGui::Checkbox(T("Включить Aimbot", "Enable Aimbot"), &g_aim.enableAim);
            ImGui::Spacing();
            ImGui::TextDisabled(T("Плавность (Smooth)", "Smooth"));
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderFloat("##smooth", &g_aim.smooth, 1.0f, 20.0f, "%.1f");
            ImGui::NextColumn();
            ImGui::TextDisabled(T("Угол обзора (FOV)", "FOV"));
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderFloat("##fov", &g_aim.fov, 1.0f, 30.0f, "%.1f°");
            ImGui::Spacing();
            ImGui::TextDisabled(T("Клавиша: X", "Key: X"));
            ImGui::Columns(1);
            ImGui::EndTabItem();
        }

        // ================= TEST =================
        if (ImGui::BeginTabItem(T("  TEST  ", "  TEST  "))) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), T("Отладка оффсетов памяти:", "Memory Offset Debug:"));
            ImGui::Separator();
            ImGui::Spacing();

            if (g_debug_entityList > 0) ImGui::TextColored(ImVec4(0, 1, 0, 1), "[OK] EnemyList Base: 0x%X", g_debug_entityList);
            else ImGui::TextColored(ImVec4(1, 0, 0, 1), "[FAIL] EnemyList Base: 0x%X", g_debug_entityList);

            if (g_debug_localPlayer > 0) ImGui::TextColored(ImVec4(0, 1, 0, 1), "[OK] LocalPlayer Ptr: 0x%X", g_debug_localPlayer);
            else ImGui::TextColored(ImVec4(1, 0, 0, 1), "[FAIL] LocalPlayer Ptr: 0x%X", g_debug_localPlayer);

            if (g_debug_myHealth >= 0) ImGui::TextColored(ImVec4(0, 1, 0, 1), "[OK] My HP (Static): %d", g_debug_myHealth);
            else ImGui::TextColored(ImVec4(1, 0, 0, 1), "[FAIL] My HP (Static): %d", g_debug_myHealth);

            if (g_debug_viewMatrixValid) ImGui::TextColored(ImVec4(0, 1, 0, 1), "[OK] ViewMatrix");
            else ImGui::TextColored(ImVec4(1, 0, 0, 1), "[FAIL] ViewMatrix (Zeros)");

            ImGui::EndTabItem();
        }

        // ================= SETTING =================
        if (ImGui::BeginTabItem(T("  SETTING  ", "  SETTING  "))) {
            ImGui::Spacing();
            ImGui::Columns(2, "set_cols", false);
            ImGui::Text(T("Язык / Language", "Language"));
            const char* langs[] = { "Русский", "English" };
            ImGui::SetNextItemWidth(-1);
            ImGui::Combo("##lang", &g_language, langs, IM_ARRAYSIZE(langs));
            ImGui::NextColumn();
            ImGui::Text(T("Цвет меню", "Menu Color"));
            if (ImGui::ColorEdit3("##menucol", g_menuColor, ImGuiColorEditFlags_NoInputs)) ApplyCustomStyle(); 
            ImGui::Columns(1);
            ImGui::Spacing(); ImGui::Spacing();
            if (ImGui::Button(T("Сбросить настройки", "Reset Settings"), ImVec2(-1, 25))) {
                g_language = 0; g_menuColor[0] = 0.0f; g_menuColor[1] = 0.9f; g_menuColor[2] = 1.0f;
                ApplyCustomStyle();
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

// ==================== Математика ====================
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

// ==================== Чтение памяти и Aimbot ====================
std::vector<PlayerData> GetPlayersAndProcessAim(HANDLE hProcess, uintptr_t hwBase, int sw, int sh) {
    std::vector<PlayerData> players;
    if (!hProcess || !hwBase) return players;

    float viewMatrix[4][4];
    ReadProcessMemory(hProcess, (LPCVOID)(hwBase + OFFSET_VIEW_MATRIX), &viewMatrix, sizeof(viewMatrix), nullptr);
    g_debug_viewMatrixValid = (viewMatrix[0][0] != 0.0f);

    // Читаем статический HP
    ReadProcessMemory(hProcess, (LPCVOID)(hwBase + OFFSET_MY_HP), &g_debug_myHealth, sizeof(g_debug_myHealth), nullptr);

    uintptr_t entityListBase = 0;
    ReadProcessMemory(hProcess, (LPCVOID)(hwBase + OFFSET_ENTITY_LIST), &entityListBase, sizeof(entityListBase), nullptr);
    g_debug_entityList = entityListBase;
    if (!entityListBase) return players;

    uintptr_t localPlayerPtr = 0;
    ReadProcessMemory(hProcess, (LPCVOID)(hwBase + OFFSET_LOCAL_PLAYER), &localPlayerPtr, sizeof(localPlayerPtr), nullptr);
    g_debug_localPlayer = localPlayerPtr;

    float centerX = sw / 2.0f;
    float centerY = sh / 2.0f;
    float closestDist = 9999.0f;
    float targetX = 0, targetY = 0;
    bool hasTarget = false;

    for (int i = 1; i <= 32; i++) {
        uintptr_t entityPtr = 0;
        ReadProcessMemory(hProcess, (LPCVOID)(entityListBase + (i * 4)), &entityPtr, sizeof(entityPtr), nullptr);
        
        if (!entityPtr) continue;
        if (entityPtr == localPlayerPtr) continue;

        PlayerData p;
        ReadProcessMemory(hProcess, (LPCVOID)(entityPtr + OFFSET_ENEMY_HEALTH), &p.health, sizeof(p.health), nullptr);
        if (p.health <= 0 || p.health > 100) continue;

        ReadProcessMemory(hProcess, (LPCVOID)(entityPtr + 0x304), &p.worldPos, sizeof(p.worldPos), nullptr);
        ReadProcessMemory(hProcess, (LPCVOID)(entityPtr + 0x10), &p.name, 31, nullptr);
        p.name[31] = '\0';

        if (WorldToScreen(p.worldPos, p.screenPos, viewMatrix, sw, sh)) {
            p.isValid = true;
            players.push_back(p);

            if (g_aim.enableAim) {
                float dist = std::hypot(p.screenPos[0] - centerX, p.screenPos[1] - centerY);
                float maxFovPixels = (sw / 90.0f) * g_aim.fov; 
                if (dist < maxFovPixels && dist < closestDist) {
                    closestDist = dist;
                    targetX = p.screenPos[0];
                    targetY = p.screenPos[1];
                    hasTarget = true;
                }
            }
        }
    }

    if (g_aim.enableAim && hasTarget && (GetAsyncKeyState(g_aim.activationKey) & 0x8000)) {
        float x = targetX - centerX;
        float y = targetY - centerY;
        float smooth = (g_aim.smooth < 1.0f) ? 1.0f : g_aim.smooth;
        int moveX = (int)(x / smooth);
        int moveY = (int)(y / smooth);
        mouse_event(MOUSEEVENTF_MOVE, moveX, moveY, 0, 0);
    }

    return players;
}

// ==================== Отрисовка ESP ====================
void DrawESPOverlay(ImDrawList* drawList, const std::vector<PlayerData>& players) {
    if (!g_esp.enableESP || !drawList) return;
    ImU32 col = IM_COL32((int)(g_esp.color[0]*255), (int)(g_esp.color[1]*255), (int)(g_esp.color[2]*255), 255);

    for (const auto& p : players) {
        if (!p.isValid || p.health <= 0) continue;
        float x = p.screenPos[0]; float y = p.screenPos[1];
        const float h = 60.0f; const float w = 30.0f;

        if (g_esp.showBox) drawList->AddRect(ImVec2(x - w/2, y - h), ImVec2(x + w/2, y), col, 0.0f, 0, g_esp.thickness);
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

    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ==================== Точка входа ====================
int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    g_hwndGame = FindWindowW(L"Valve001", nullptr);
    if (!g_hwndGame) {
        MessageBoxW(nullptr, L"CS 1.6 is not running.", L"Error", MB_ICONERROR);
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
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED, 
        wc.lpszClassName, L"Overlay",
        WS_POPUP, rect.left, rect.top, width, height,
        nullptr, nullptr, hInstance, nullptr
    );
    if (!g_hwndOverlay) return 1;

    SetLayeredWindowAttributes(g_hwndOverlay, 0, 255, LWA_ALPHA);
    {
        MARGINS margins = { -1, -1, -1, -1 };
        DwmExtendFrameIntoClientArea(g_hwndOverlay, &margins);
    }

    ShowWindow(g_hwndOverlay, SW_SHOW);
    UpdateWindow(g_hwndOverlay);

    if (!CreateD3D9Device()) return 1;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    
    ApplyCustomStyle();
    io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\tahoma.ttf", 16.0f, nullptr, io.Fonts->GetGlyphRangesCyrillic());

    ImGui_ImplWin32_Init(g_hwndOverlay);
    ImGui_ImplDX9_Init(g_pDevice);

    DWORD pid = GetProcessIdByName(L"hl.exe");
    uintptr_t hwBase = 0;
    if (pid) {
        g_hProcess = OpenProcess(PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION, FALSE, pid);
        if (g_hProcess) hwBase = GetModuleBaseAddress(pid, L"hw.dll");
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
            if (!insertPressed) { g_showMenu = !g_showMenu; insertPressed = true; }
        } else { insertPressed = false; }

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

        ImGui::GetIO().MouseDrawCursor = g_showMenu;
        if (g_showMenu) RenderImGuiMenu();

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

        g_pDevice->Present(nullptr, nullptr, nullptr, nullptr);
        
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(g_hwndOverlay, &pt);

        bool overMenu = false;
        if (g_showMenu) {
            if (pt.x >= g_menuPos.x && pt.x <= g_menuPos.x + g_menuSize.x &&
                pt.y >= g_menuPos.y && pt.y <= g_menuPos.y + g_menuSize.y) overMenu = true;
        }

        LONG_PTR exStyle = GetWindowLongPtrW(g_hwndOverlay, GWL_EXSTYLE);
        if (overMenu) {
            if (exStyle & WS_EX_TRANSPARENT) {
                SetWindowLongPtrW(g_hwndOverlay, GWL_EXSTYLE, exStyle & ~WS_EX_TRANSPARENT);
                SetWindowPos(g_hwndOverlay, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
            }
        } else {
            if (!(exStyle & WS_EX_TRANSPARENT)) {
                SetWindowLongPtrW(g_hwndOverlay, GWL_EXSTYLE, exStyle | WS_EX_TRANSPARENT);
                SetWindowPos(g_hwndOverlay, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
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