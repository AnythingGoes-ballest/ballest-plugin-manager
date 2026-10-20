#include "input.hpp"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <initializer_list>
#include <string>

namespace input {
namespace {

bool gDown[kKeyCount] = {}, gWasDown[kKeyCount] = {};
int gSimulated = 0;
bool gHeld[kKeyCount] = {};             // test hook: keys reported as held whatever the keyboard says

bool GameHasFocus() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

bool Valid(int key) { return key > 0 && key < kKeyCount; }

// --- the mouse wheel: from the game window's own messages, so it is seen whatever the game's input mode (the plugin
// manager window makes input UI-only, where the player controller no longer sees the wheel) ------------------------
HWND gHooked = nullptr;
WNDPROC gOriginalProc = nullptr;
std::atomic<int> gWheelDelta{0};
double gWheel = 0;

LRESULT CALLBACK WheelProc(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_MOUSEWHEEL) gWheelDelta += GET_WHEEL_DELTA_WPARAM(w);
    return CallWindowProcW(gOriginalProc, window, message, w, l);
}

// The game's window (the foreground one, while it has focus): its window procedure wrapped once, again if it is made anew.
void HookWheel() {
    if (gHooked && IsWindow(gHooked)) return;
    HWND window = GetForegroundWindow();
    wchar_t cls[64] = {};
    if (!window || !GetClassNameW(window, cls, 64) || std::wstring(cls) != L"UnrealWindow") return;
    const LONG_PTR old = SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WheelProc));
    if (!old) return;
    gOriginalProc = reinterpret_cast<WNDPROC>(old);
    gHooked = window;
}

// --- controllers: XInput (the Windows API for Xbox-style pads, which also covers most others through Steam Input) ---
struct PadState {                       // XINPUT_STATE
    DWORD packet;
    WORD buttons;
    BYTE leftTrigger, rightTrigger;
    SHORT thumbLX, thumbLY, thumbRX, thumbRY;
};
using GetStateFn = DWORD(WINAPI*)(DWORD, PadState*);
GetStateFn gGetState = nullptr;
bool gTriedLoad = false;
ULONGLONG gNextPadScan = 0;             // XInputGetState on an empty slot is slow; empty slots are retried every 2 s
bool gPadPresent[4] = {};

GetStateFn Loader() {
    if (!gTriedLoad) {
        gTriedLoad = true;
        for (const wchar_t* dll : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"})
            if (HMODULE m = LoadLibraryW(dll)) {
                gGetState = reinterpret_cast<GetStateFn>(GetProcAddress(m, "XInputGetState"));
                if (gGetState) break;
            }
    }
    return gGetState;
}

// XINPUT_GAMEPAD_* button bits, in the order of the Pad* keys from kPadA.
constexpr WORD kPadBits[] = {0x1000, 0x2000, 0x4000, 0x8000, 0x0100, 0x0200, 0, 0, 0x0040, 0x0080, 0x0020, 0x0010,
                             0x0001, 0x0002, 0x0004, 0x0008};
constexpr int kTriggerThreshold = 30;   // XINPUT_GAMEPAD_TRIGGER_THRESHOLD
constexpr int kStickThreshold = 16384;  // half way: a deliberate push, as a key press

void ReadPads(bool* down) {
    GetStateFn getState = Loader();
    if (!getState) return;
    const ULONGLONG now = GetTickCount64();
    const bool rescan = now >= gNextPadScan;
    if (rescan) gNextPadScan = now + 2000;
    for (DWORD pad = 0; pad < 4; ++pad) {
        if (!gPadPresent[pad] && !rescan) continue;
        PadState s{};
        gPadPresent[pad] = getState(pad, &s) == ERROR_SUCCESS;
        if (!gPadPresent[pad]) continue;
        for (int i = 0; i < 16; ++i)
            if (kPadBits[i] && (s.buttons & kPadBits[i])) down[kPadA + i] = true;
        if (s.leftTrigger > kTriggerThreshold) down[kPadLT] = true;
        if (s.rightTrigger > kTriggerThreshold) down[kPadRT] = true;
        if (s.thumbLY > kStickThreshold) down[kPadStickUp] = true;
        if (s.thumbLY < -kStickThreshold) down[kPadStickDown] = true;
        if (s.thumbLX < -kStickThreshold) down[kPadStickLeft] = true;
        if (s.thumbLX > kStickThreshold) down[kPadStickRight] = true;
    }
}

}  // namespace

void Frame() {
    const bool focus = GameHasFocus();
    bool now[kKeyCount] = {};
    if (focus) {
        for (int vk = 1; vk < 256; ++vk) now[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
        ReadPads(now);
        HookWheel();
    }
    gWheel = gWheelDelta.exchange(0) / static_cast<double>(WHEEL_DELTA);
    if (!focus) gWheel = 0;
    for (int key = 1; key < kKeyCount; ++key) {
        gWasDown[key] = gDown[key];
        gDown[key] = now[key] || gHeld[key];
    }
    if (gSimulated) {
        gWasDown[gSimulated] = false;
        gDown[gSimulated] = true;
        gSimulated = 0;
    }
}

bool Down(int key) { return Valid(key) && gDown[key]; }
double Wheel() { return gWheel; }
int PrimaryButton() { return GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON; }
int SecondaryButton() { return GetSystemMetrics(SM_SWAPBUTTON) ? VK_LBUTTON : VK_RBUTTON; }
bool Pressed(int key) { return Valid(key) && gDown[key] && !gWasDown[key]; }
void Simulate(int key) { gSimulated = Valid(key) ? key : 0; }
void SimulateHeld(int key, bool held) {
    if (Valid(key)) gHeld[key] = held;
}

int AnyPressed() {
    // Shift, Ctrl and Alt are reported as their left/right keys too (0xA0..0xA5); only the generic ones count here.
    for (int key = 1; key < kKeyCount; ++key)
        if ((key < 0xA0 || key > 0xA5) && Pressed(key)) return key;
    return 0;
}

}  // namespace input
