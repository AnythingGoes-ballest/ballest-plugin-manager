// Plugin runtime: discovers plugins/<id>/info.toml, compiles each plugin's AngelScript into its own module, and
// calls its callbacks on the game thread with a time budget. A plugin that throws or overruns is stopped; the
// game and the other plugins carry on. Plugins can also be loaded and unloaded while the game runs (installs and
// removals from the plugin browser). The script API itself is in api.cpp.
#pragma once
#include <string>
#include <vector>

namespace plugins {

constexpr const char* kHostVersion = "0.5.0";

void LoadAll(const std::wstring& pluginsDir);
void Frame(float dt);
std::wstring Dir();                         // the plugins folder
void OpenFolder();                          // shows the plugins folder in File Explorer

// Outside plugin callbacks only (the registry calls these from its own frame step).
bool Load(const std::string& id);           // a plugin folder that appeared (installed); false if it cannot load
void Unload(const std::string& id);         // stops it, frees its script and takes its UI off screen

struct Info {
    std::string id, name, version, author, description, status;
    std::string icon;                       // an image file, or "" for none
    bool essential = false;                 // part of the host's own setup: cannot be removed
};
std::vector<Info> List();                   // loaded plugins, in load order
bool Find(const std::string& id, Info* out);
int Current();                              // index of the plugin whose code is running now, or -1
std::string CurrentId();
bool CurrentIsEssential();
std::string Summary();                      // "id=status; ..." for logs and tests

int CompareVersions(const std::string& a, const std::string& b);    // "1.2.0" vs "1.10": -1, 0, 1

}  // namespace plugins
