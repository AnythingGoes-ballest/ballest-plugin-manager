// The plugin registry: registry.json lists the plugins that can be installed, each pinned to a commit of its GitHub
// repo with a SHA-256 for every file. Downloads run on a worker thread; installs and removals are applied on the
// game thread in Frame, between plugin callbacks.
//
//   { "raw_base": "https://raw.githubusercontent.com/",          (optional; file:///... for a local test mirror)
//     "plugins": [ { "id": "replay-manager", "name": "...", "description": "...", "author": "...",
//                    "repo": "owner/name", "version": "0.1.0", "commit": "<sha>", "min_host": "0.3.0",
//                    "icon": "icon.png", "files": { "info.toml": "<sha256>", "main.as": "<sha256>", ... } } ] }
//
// Where it is read from: %LOCALAPPDATA%\Ballest\Saved\PluginManager\registry_url.txt if that file exists (a URL,
// https:// or file:///), otherwise kDefaultUrl.
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace registry {

constexpr const char* kDefaultUrl = "https://raw.githubusercontent.com/AnythingGoes-ballest/ballest-plugin-manager/main/registry.json";

struct Entry {
    std::string id, name, description, author, repo, version, commit, minHost;
    std::string iconFile;                                   // one of `files`, or ""
    std::vector<std::pair<std::string, std::string>> files; // name, sha256
    std::string icon;                                       // local copy of the icon once downloaded, or ""
    std::string Page() const { return "https://github.com/" + repo; }
};

void Frame();                       // game thread, before plugins run
void Refresh();                     // fetch registry.json again
std::string State();                // "", "loading", "ready", or "error: ..."
const std::vector<Entry>& Entries();

// Game thread. Both are applied on the next frame; Pending tells how it is going.
void Install(const std::string& id);        // install or update to the registry's version
void Remove(const std::string& id);         // unload and delete (not essential plugins)
std::string Pending(const std::string& id); // "", "installing", "removing", or "error: ..."

std::string DefaultIcon();          // the plugin manager's default-icon.png, or ""

}  // namespace registry
