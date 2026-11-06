// Plugin tabs on the main menu's Create page (WBP_4_Create_C, measured 2026-11-05 from its blueprint and in a slot):
//   * the left bar is a StackBox, PLAYLISTTABS, of WBP_SubPageLabel_C entries ("files", "published"); a label is a
//     WBP_ButtonBase_C (Button_33, SetIsActive, bActive) whose InactiveButtonClicked the page binds to switch pages;
//   * the content is VerticalBox_0 (under LevelSelectOverlay): the "+new" bar and SizeBox_1, holding
//     CommonActivatableWidgetSwitcher_1 with the files and published lists.
// A plugin tab is a label of the same kind after the game's, and a page (a VerticalBox) added to VerticalBox_0. Its
// label shows its page in place of the game's content and marks the game's labels inactive; clicking one of those
// (which the game then marks active) gives the content back. Built again whenever the page is made again.
// Game thread only.
#pragma once
#include <string>

#include "engine.hpp"

namespace createpage {

void Frame();
int AddTab(int owner, const std::string& label);
eng::Obj TabPage(int tab);              // the page a docked window builds into; null while the Create page isn't made
bool TabOpen(int tab);                  // its page is showing (on screen in the Create page)
void OpenTab(int tab);
void RemoveOwner(int owner);
bool TabTapped(eng::Obj userWidget);    // test: a tap on a plugin's label
std::string Status();

}  // namespace createpage
