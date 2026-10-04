#include "game.hpp"
#include "sandbox.hpp"

#include <cstring>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <thread>

#include <windows.h>

#include <set>

#include "log.hpp"
#include "race.hpp"

using eng::Obj;

namespace game {
namespace {

eng::Weak gViewport, gController;
int gGeneration = 0;
std::set<int> gCursorOwners;                // plugins currently asking for the cursor
bool gCursorTaken = false, gCursorWasShown = false, gInputModeChanged = false;
eng::Weak gTypingWidget;
bool gTypingMode = false;

void UpdateCursor(Obj controller) {
    Obj widgets = eng::FindCdo("WidgetBlueprintLibrary");
    if (!widgets) return;
    if (!gCursorOwners.empty()) {
        bool shown = false;
        eng::ReadBool(controller, "bShowMouseCursor", &shown);
        if (!gCursorTaken) {
            gCursorTaken = true;
            gCursorWasShown = shown;
        }
        if (!shown) {
            eng::WriteBool(controller, "bShowMouseCursor", true);
            // Only the controller is passed; the other parameters stay zero: no focus widget, no mouse lock,
            // cursor kept while a button is held.
            eng::Call(widgets, "SetInputMode_GameAndUIEx", controller);
            gInputModeChanged = true;
        }
    } else if (gCursorTaken) {
        gCursorTaken = false;
        eng::WriteBool(controller, "bShowMouseCursor", gCursorWasShown);
        // Only undo an input mode the host set: the main menu, where the cursor is already shown, keeps its own.
        if (gInputModeChanged) eng::Call(widgets, "SetInputMode_GameOnly", controller);
        gInputModeChanged = false;
    }
}

void UpdateTyping(Obj controller) {
    Obj widgets = eng::FindCdo("WidgetBlueprintLibrary");
    // Only on a track, where the game's keys (R, Backspace...) would act on what's typed. The menus route keys to
    // the focused box themselves, and switching their input mode there made the first click after typing go
    // nowhere (the user's report: leaving the hub's search took two clicks).
    Obj typing = race::OnTrack() ? eng::Get(gTypingWidget) : nullptr;
    if (!widgets || (typing != nullptr) == gTypingMode) return;
    if (typing) {
        // No mouse lock, input not flushed.
        eng::Call(widgets, "SetInputMode_UIOnlyEx", controller, typing, uint8_t{0}, uint8_t{0});
        hostlog::Info("typing: input is UI-only");
    } else {
        eng::Call(widgets, "SetInputMode_GameAndUIEx", controller);
        hostlog::Info("typing ended: input is game and UI");
    }
    gTypingMode = typing != nullptr;
}

}  // namespace

void SetViewportClient(Obj client) { gViewport = eng::MakeWeak(client); }

Obj PlayerController() { return eng::Get(gController); }
int Generation() { return gGeneration; }

double Seconds() {
    static LARGE_INTEGER frequency{}, start{};
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!frequency.QuadPart) {
        QueryPerformanceFrequency(&frequency);
        start = now;
    }
    return static_cast<double>(now.QuadPart - start.QuadPart) / static_cast<double>(frequency.QuadPart);
}

void Frame() {
    // The controller is looked up every frame; a different one means a different map.
    Obj viewport = eng::Get(gViewport);
    Obj controller =
        viewport ? eng::Call(eng::FindCdo("GameplayStatics"), "GetPlayerController", viewport, int32_t{0}).ReturnObj() : nullptr;
    if (controller != gController.o) {
        ++gGeneration;
        gCursorTaken = gInputModeChanged = gTypingMode = false;     // the new controller starts with the game's own state
    }
    gController = eng::MakeWeak(controller);
    if (controller) {
        UpdateCursor(controller);
        UpdateTyping(controller);
    }
}

bool CursorShown() {
    bool shown = false;
    if (Obj controller = PlayerController()) eng::ReadBool(controller, "bShowMouseCursor", &shown);
    return shown;
}

void SetTypingWidget(Obj textInput) { gTypingWidget = eng::MakeWeak(textInput); }

void RequestCursor(int owner, bool visible) {
    if (visible) gCursorOwners.insert(owner);
    else gCursorOwners.erase(owner);
}

bool OpenLevel(const std::string& map) {
    Obj strings = eng::FindCdo("KismetStringLibrary"), statics = eng::FindCdo("GameplayStatics");
    Obj controller = PlayerController();
    if (!strings || !statics || !controller) return false;
    const std::wstring w = eng::Widen(map);
    const eng::FString fs{w.c_str(), static_cast<int32_t>(w.size() + 1), static_cast<int32_t>(w.size() + 1)};
    const eng::Params name = eng::Call(strings, "Conv_StringToName", fs);
    size_t size = 0;
    const uint8_t* fname = name.Return(&size);
    if (!name.Invoked() || !fname) return false;
    eng::Params open(eng::FunctionOn(statics, "OpenLevel"));
    open.SetArg(0, controller);
    open.SetArg(1, fname, size);
    open.SetArg(2, uint8_t{1});     // bAbsolute; Options stays an empty string
    hostlog::Info("opening map " + map);
    return eng::Invoke(statics, open);
}

bool MousePosition(double* x, double* y) {
    Obj controller = PlayerController();
    if (!controller) return false;
    struct Vec2d {
        double x, y;
    };
    const Vec2d mouse = eng::Call(eng::FindCdo("WidgetLayoutLibrary"), "GetMousePositionOnViewport", controller).ReturnAs<Vec2d>(Vec2d{-1, -1});
    if (mouse.x < 0 || mouse.y < 0) return false;
    *x = mouse.x;
    *y = mouse.y;
    return true;
}

namespace {
HWND GameWindow() {
    struct Search {
        DWORD pid;
        HWND found;
    } search{GetCurrentProcessId(), nullptr};
    EnumWindows(
        [](HWND w, LPARAM p) -> BOOL {
            auto* s = reinterpret_cast<Search*>(p);
            DWORD pid = 0;
            GetWindowThreadProcessId(w, &pid);
            char cls[64] = {};
            GetClassNameA(w, cls, sizeof cls);
            if (pid != s->pid || !IsWindowVisible(w) || std::strcmp(cls, "UnrealWindow") != 0) return TRUE;
            s->found = w;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&search));
    return search.found;
}
}  // namespace

bool WindowMaximized() {
    HWND w = GameWindow();
    return w && IsZoomed(w);
}

bool WindowFitsScreen() {
    HWND w = GameWindow();
    if (!w) return true;
    if ((GetWindowLongW(w, GWL_STYLE) & WS_CAPTION) != WS_CAPTION) return true;     // fullscreen or borderless
    RECT r;
    MONITORINFO m{};
    m.cbSize = sizeof m;
    if (!GetWindowRect(w, &r) || !GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST), &m)) return true;
    // The window's rectangle includes Windows 10/11's invisible resize borders (about 7 px each side and below), which
    // may hang past the work area without anything showing: a few pixels are allowed.
    constexpr LONG kSlack = 12;
    return r.left >= m.rcWork.left - kSlack && r.right <= m.rcWork.right + kSlack && r.top >= m.rcWork.top &&
           r.bottom <= m.rcWork.bottom + kSlack;
}

bool MaximizeWindow() {
    if (sandbox::On()) return false;            // a test copy's window stays as it was started, behind the player's
    HWND w = GameWindow();
    if (!w || (GetWindowLongW(w, GWL_STYLE) & WS_CAPTION) != WS_CAPTION) return false;
    ShowWindow(w, SW_MAXIMIZE);
    return IsZoomed(w) != 0;
}

namespace {
std::wstring AtStartFile() { return hostlog::DataDir() + L"\\window_at_start.txt"; }

std::vector<std::pair<std::string, int>> ReadAtStart() {
    std::vector<std::pair<std::string, int>> entries;
    FILE* f = _wfopen(AtStartFile().c_str(), L"r");
    if (!f) return entries;
    char id[256];
    int mode = 0;
    while (fscanf(f, "%255s %d", id, &mode) == 2) entries.push_back({id, mode});
    fclose(f);
    return entries;
}

bool FitsWorkArea(HWND w) {
    RECT r;
    MONITORINFO m{};
    m.cbSize = sizeof m;
    if (!GetWindowRect(w, &r) || !GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST), &m)) return true;
    constexpr LONG kSlack = 12;         // the invisible resize borders (see WindowFitsScreen)
    return r.left >= m.rcWork.left - kSlack && r.right <= m.rcWork.right + kSlack && r.top >= m.rcWork.top &&
           r.bottom <= m.rcWork.bottom + kSlack;
}
}  // namespace

void SetMaximizeAtStart(const std::string& pluginId, int mode) {
    auto entries = ReadAtStart();
    entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const auto& e) { return e.first == pluginId; }), entries.end());
    if (mode > 0) entries.push_back({pluginId, mode});
    FILE* f = _wfopen(AtStartFile().c_str(), L"w");
    if (!f) return;
    for (const auto& [id, m] : entries) fprintf(f, "%s %d\n", id.c_str(), m);
    fclose(f);
}

// Runs on a thread of its own from the host's start, while the game is still loading: the window appears seconds
// before any plugin can run (measured: Fit Window's own check maximized it only once the engine was up).
void EarlyWindowFit(const std::wstring& pluginsDir) {
    if (sandbox::On()) return;
    std::set<std::string> off;
    if (FILE* f = _wfopen((hostlog::DataDir() + L"\\off.txt").c_str(), L"r")) {
        char id[256];
        while (fscanf(f, "%255s", id) == 1) off.insert(id);
        fclose(f);
    }
    int mode = 0;
    std::string by;
    for (const auto& [id, m] : ReadAtStart()) {
        const std::wstring folder = pluginsDir + L"\\" + std::wstring(id.begin(), id.end());
        if (off.count(id) || GetFileAttributesW(folder.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        if (m > mode) {
            mode = m;
            by = id;
        }
    }
    if (mode == 0) return;
    std::thread([mode, by] {
        const ULONGLONG start = GetTickCount64();
        int maximized = 0;
        // The game can resize its window again while it loads: watched for the first 30 s, maximized each time it
        // stops fitting (at most three times, so a player un-maximizing it on purpose is left alone).
        while (GetTickCount64() - start < 30000 && maximized < 3) {
            Sleep(25);
            HWND w = GameWindow();
            if (!w || IsZoomed(w) || (GetWindowLongW(w, GWL_STYLE) & WS_CAPTION) != WS_CAPTION) continue;
            if (mode == 1 && FitsWorkArea(w)) continue;
            ShowWindowAsync(w, SW_MAXIMIZE);
            ++maximized;
            hostlog::Info("window maximized at start for " + by + ", " + std::to_string(GetTickCount64() - start) + " ms in");
            Sleep(500);
        }
    }).detach();
}

void* WindowHandle() { return GameWindow(); }

bool ScreenSize(double* width, double* height) {
    Obj controller = PlayerController();
    if (!controller) return false;
    struct Vec2d {
        double x, y;
    };
    Obj layout = eng::FindCdo("WidgetLayoutLibrary");
    const Vec2d size = eng::Call(layout, "GetViewportSize", controller).ReturnAs<Vec2d>(Vec2d{0, 0});
    const float scale = eng::Call(layout, "GetViewportScale", controller).ReturnAs<float>(0.0f);
    if (size.x <= 0 || size.y <= 0 || scale <= 0) return false;
    *width = size.x / scale;
    *height = size.y / scale;
    return true;
}

double MouseWheel() {
    Obj controller = PlayerController();
    Obj fn = controller ? eng::FindFunction(eng::ClassOf(controller), "GetInputAnalogKeyState") : nullptr;
    if (!fn) return 0;
    // FKey: its KeyName (an FName) first, the rest (the key's details, looked up by name) left empty.
    const std::wstring key = L"MouseWheelAxis";
    const eng::Params name = eng::Call(eng::FindCdo("KismetStringLibrary"), "Conv_StringToName",
                                       eng::FString{key.c_str(), static_cast<int32_t>(key.size() + 1), static_cast<int32_t>(key.size() + 1)});
    const uint8_t* fname = name.Return();
    eng::Params p(fn);
    const int32_t size = p.SizeOf("Key");
    if (!fname || size < 8) return 0;
    std::vector<uint8_t> bytes(static_cast<size_t>(size), 0);
    std::memcpy(bytes.data(), fname, 8);
    p.Set("Key", bytes.data(), bytes.size());
    if (!eng::Invoke(controller, p)) return 0;
    return p.ReturnAs<float>(0.0f);
}

}  // namespace game
