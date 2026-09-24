// The world the host runs in: the viewport client (the world context for engine calls), this frame's player
// controller, map changes, real time, and the mouse cursor. Game thread only.
#pragma once
#include <string>

#include "engine.hpp"

namespace game {

void SetViewportClient(eng::Obj client);
void Frame();                               // start of every host frame, before anything else

eng::Obj PlayerController();                // this frame's controller, or null
// Changes on every map load (the player controller is replaced). Anything built in an earlier map is gone and
// must be forgotten, never touched.
int Generation();
double Seconds();                           // real time since the host started

// While any plugin requests it, the cursor stays visible with game-and-UI input (the game hides it again on its
// own, e.g. when a replay starts). When the last request is released, what the game had is restored.
void RequestCursor(int owner, bool visible);
bool CursorShown();                         // the mouse cursor is on screen (menus, pause, a plugin asked)

// The text input being typed in, or null. While there is one, input is UI-only with it focused: the game's
// viewport ignores keys, so the player controller (whose "any key" event moves menu focus) and the pawn never
// see what is typed. Afterwards input returns to game-and-UI, as while the cursor is requested.
void SetTypingWidget(eng::Obj textInput);

// Opens a map by name the way GameplayStatics.OpenLevel does (test tooling; skips the menu's level setup, so
// medal times and the leaderboard stay unloaded in that map).
bool OpenLevel(const std::string& map);

}  // namespace game
