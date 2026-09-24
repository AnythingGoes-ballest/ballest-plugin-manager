// Custom cosmetics: balls, hats and goal explosions (the Customize page's "bfx") made at runtime, shown in a "custom"
// section of the game's own Customize page, equipped through the game's own buttons. Measured on the game:
//   * the page (WBP_1_CustomizeColor_C) has three tabs (balls, hats, bfx) whose sections are pairs in a ScrollBox: a
//     header HorizontalBox, then a Border around a UniformGridPanel. BuildCategory(CategoryData, ButtonsInCategory
//     out, bRequireUnlockToShow) makes the tile buttons for one cosmetic list.
//   * cosmetics are data assets: PDA_BallSkin_C (SkinMaterial, SkinGhostVariant, BallType), PDA_Accessory_C
//     (AccessoryMesh, AccessoryGhostMaterial), PDA_GoalExplo_C (NiagaraSystem, SFX, RelativeScale), all with
//     CosmeticName and PreviewTexture; lists are PDA_SkinList_C (ListSkins), PDA_AccessoryList_C (AccessoryList)
//     and PDA_GoalExploList_C (GoalExplosions), each at +0x60.
//   * an image on the ball: a dynamic copy of the game's LBall material (MI_LBall05) with the image in its "Base
//     Color" texture parameter, on Unreal's plain sphere mesh (the ball mesh's panelled UVs repeat the image).
//   * objects the host makes are kept from garbage collection by the game instance's ReferencedObjects array; the
//     object array's root-set flag alone did not keep them (measured: collected by the next garbage collection).
//   * the game saves the page's choice to the profile, which is also what multiplayer is told: a custom asset there
//     is saved as a path no later session can load (measured: the ball came back with no material). So the save keeps
//     the game's own choice, and the host puts the custom one on the player's own balls and checkpoints.
// Game thread only.
#pragma once
#include <string>
#include <vector>

#include "engine.hpp"

namespace cosmetics {

enum class Kind { Ball = 0, Hat = 1, Bfx = 2 };     // the Customize page's tab order

// Each returns false (and logs why) if the cosmetic could not be made. An id already added (the plugin was reloaded)
// is kept as it is and true is returned. One asked for
// before the game is ready (no player yet) is made as soon as it is, and true is returned.
// `model`: text in the format of models.hpp, "" for none. On a ball it is built around the ball (its centre, radius 50);
// on a hat it is built on the hat slot (the top of the ball), and the hat's mesh may then be "".
bool AddBall(const std::string& id, const std::string& name, const std::wstring& image, const std::wstring& preview,
             const std::string& model = "");
bool AddHat(const std::string& id, const std::string& name, const std::string& meshPath, double scale,
            const std::wstring& preview, const std::string& model = "");     // scale: of the hat where the game puts hats
// `system`, `sound`: another Niagara system and sound than the base explosion's (asset paths), "" for the base's.
bool AddBfx(const std::string& id, const std::string& name, const std::string& baseExplosion, double scale,
            const std::wstring& preview, const std::string& system = "", const std::string& sound = "");
int Count(Kind kind);
// The custom cosmetic the player wears, per kind ("" for the game's own). Chosen on the Customize page, or set here
// (a plugin restoring the player's choice); false if there is no such custom cosmetic.
bool Equip(Kind kind, const std::string& id);
std::string Equipped(Kind kind);

void Frame();                   // the custom section on the Customize page, and the plain sphere for image balls
std::string Status();           // for the test channel
bool ClickTile(int index);      // test: presses a custom tile of the section on screen (-n: the game's n-th), as a click does

// Engine helpers the cosmetics need, usable elsewhere.
eng::Obj LoadAsset(const std::wstring& path);      // an asset by object path, loaded if it is not in memory
void KeepAlive(eng::Obj o);                        // never garbage collected (referenced by the game instance)

}  // namespace cosmetics
