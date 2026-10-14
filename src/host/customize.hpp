// The Customize page, with
// Cosmetic Kit on, the host draws over the game's page, on a screen of its own:
//   * the top strip above the cosmetics panel: "local" and "public", and the round cosmetic browser button;
//   * the loadout tabs (local mode): one per loadout and a "+";
//   * the slot tabs in place of the game's balls/hats/bfx tabs (which are hidden): an item slot showing what is worn,
//     with its name on a small tab attached to its left, growing dock-style with the pointer's distance (two label
//     styles, a Cosmetic Kit setting);
//   * the game's back button moved to sit 16 units under the last slot tab (local mode; public mode leaves it be).
// The custom section's "cosmetic browser" button is acted on here too.
// Positions are in 1920x1080 game pixels (= widget units), measured from the game's own balls tab.
// Game thread only.
#pragma once

namespace customize {

void Frame();                   // every frame, after cosmetics::Frame

}  // namespace customize
