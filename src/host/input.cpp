#include "input.hpp"

#include <windows.h>

namespace input {
namespace {

bool gDown[256] = {}, gWasDown[256] = {};
int gSimulated = 0;

bool GameHasFocus() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

bool Valid(int vk) { return vk > 0 && vk < 256; }

}  // namespace

void Frame() {
    const bool focus = GameHasFocus();
    for (int vk = 1; vk < 256; ++vk) {
        gWasDown[vk] = gDown[vk];
        gDown[vk] = focus && (GetAsyncKeyState(vk) & 0x8000) != 0;
    }
    if (gSimulated) {
        gWasDown[gSimulated] = false;
        gDown[gSimulated] = true;
        gSimulated = 0;
    }
}

bool Down(int vk) { return Valid(vk) && gDown[vk]; }
bool Pressed(int vk) { return Valid(vk) && gDown[vk] && !gWasDown[vk]; }
void Simulate(int vk) { gSimulated = Valid(vk) ? vk : 0; }

}  // namespace input
