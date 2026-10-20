// The Classic theme's cosmetic browser: a window over the Customize page, opened from the strip's globe button or the
// custom section's "cosmetic browser" while the plugin manager has its Classic theme (the other themes open its
// cosmetics page instead). Left: search, then All, Newest, one entry per slot (and, until its own popup is designed,
// Cosmetic Kit's settings). Right: the chosen view's cosmetics as cards (picture, name, author, worn or not); a click
// wears one. It lists what is installed (cosmetic plugins' cosmetics); getting more is the classic menu's browse.
// Game thread only.
#pragma once

namespace browser {

void Open();
void Close();
bool IsOpen();
void Frame();                   // every frame, after customize::Frame

}  // namespace browser
