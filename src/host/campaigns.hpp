// Custom campaigns: plugin campaigns inside the game's own track selection (the play page, WBP_0_Play).
//
// When at least one plugin has registered a campaign, the play page's sidebar gets a "custom campaigns" tab after the
// game's own (circuit, trials, hub, collabs): a clone of the game's tab widget (WBP_SubPageLabel_C) in its tab bar
// (PLAYLISTTABS), so it hovers, highlights and sounds like them. The tab shows a page in the play page's own body
// switcher (Body_WS): the registered campaigns as cards. Choosing one shows that campaign's screen in the same place
// (its title as the page header, its courses as cards with the plugin's text, e.g. a best time); choosing a course
// tells the plugin, which opens it. The game's back button and Escape step from a campaign's screen back to the list;
// on the list they do what they always do (back to the main menu). Measured and explained in campaigns.cpp.
#pragma once
#include <string>

namespace campaigns {

// A campaign of the plugin `owner`. Images: a PNG/JPG file or a game texture ("/Game/..."), "" for none. Returns its
// id (> 0).
int Register(int owner, const std::string& title, const std::string& image, const std::string& description);
bool Remove(int owner, int id);
// A course on the campaign's screen, in order; returns its index, or -1 for an unknown campaign.
int AddCourse(int owner, int id, const std::string& title, const std::string& image);
bool SetCourseInfo(int owner, int id, int course, const std::string& info);   // the course card's right-hand text
bool SetCourseDetail(int owner, int id, int course, const std::string& detail);   // a line under its title
// The campaign's colours: "card=#C8102E hover=#FFC20E text=#FFFFFF ..." (keys: kThemeKeys in campaigns.cpp, docs in
// api-examples.txt). False, with the reason, for an unknown key or a value that isn't #RRGGBB / #RRGGBBAA.
bool SetTheme(int owner, int id, const std::string& theme, std::string* error);
bool SetNote(int owner, int id, const std::string& note);                      // a line under the campaign's back bar
bool TakeOpened(int owner, int id);         // the player opened the campaign's screen (once per opening)
int TakePicked(int owner, int id);          // the course the player chose (once), or -1
bool Show(int owner, int id);               // open the play page at this campaign's screen (from the main menu)
bool Shown(int id);                         // this campaign's screen is on screen
bool ListShown();                           // the custom campaigns list is on screen

void Frame();                               // game thread, every frame
void RemoveOwner(int owner);                // a plugin unloaded: its campaigns go
std::string Command(const std::string& args);   // the test command "campaigns ..." (see campaigns.cpp)

}  // namespace campaigns
