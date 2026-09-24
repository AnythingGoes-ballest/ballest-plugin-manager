// Keyboard and mouse state, polled once per frame and only while the game window has focus, so keys pressed in
// other programs never reach plugins. Codes are Windows virtual keys.
#pragma once

namespace input {

void Frame();
bool Down(int vk);
bool Pressed(int vk);           // went down this frame
void Simulate(int vk);          // test hook: reported as pressed on the next frame

}  // namespace input
