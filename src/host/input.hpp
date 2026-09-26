// Keyboard, mouse and controller state, polled once per frame and only while the game window has focus, so keys
// pressed in other programs never reach plugins. Keys 1..255 are Windows virtual keys; controller buttons (read with
// XInput, pad 1 to 4 together) follow from kPadA.
#pragma once

namespace input {

constexpr int kPadA = 0x100, kPadB = 0x101, kPadX = 0x102, kPadY = 0x103, kPadLB = 0x104, kPadRB = 0x105, kPadLT = 0x106,
              kPadRT = 0x107, kPadL3 = 0x108, kPadR3 = 0x109, kPadView = 0x10A, kPadMenu = 0x10B, kPadUp = 0x10C,
              kPadDown = 0x10D, kPadLeft = 0x10E, kPadRight = 0x10F, kPadStickUp = 0x110, kPadStickDown = 0x111,
              kPadStickLeft = 0x112, kPadStickRight = 0x113;
constexpr int kKeyCount = 0x114;

void Frame();
bool Down(int key);
bool Pressed(int key);          // went down this frame
int AnyPressed();               // a key that went down this frame, or 0
void Simulate(int key);         // test hook: reported as pressed on the next frame

}  // namespace input
