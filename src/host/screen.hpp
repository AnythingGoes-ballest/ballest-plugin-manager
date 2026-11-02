// Screen:: for plugins: a picture of the game's view without any UI (the game's own `shot`), cut to a thumbnail
// (the middle of the view at 16:10, 480 x 300) and saved as a PNG in the plugin's cache folder,
// PluginManager\cache\<plugin id>\<name>.png, for a window's image. One capture at a time; it takes a frame or two
// for the game to write its screenshot, so the plugin polls State(). Game thread only (the API side).
#pragma once
#include <string>

namespace screen {

// Starts a capture into <name>.png (a plain name: letters, digits, -_.); false if refused (`why` says why).
bool Capture(int owner, const std::string& pluginId, const std::string& name, std::string* why);
// "idle", "pending", "done: <full path>" or "error: <why>" for this plugin's last capture.
std::string State(int owner);
void Frame();                       // each frame: finds the game's screenshot and makes the thumbnail
void RemoveOwner(int owner);

}  // namespace screen
