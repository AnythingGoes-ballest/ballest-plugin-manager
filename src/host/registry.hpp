// The plugin registry: registry.json lists the plugins that can be installed, each pinned to a commit of its GitHub
// repo with a SHA-256 for every file. Downloads run on a worker thread; installs and removals are applied on the
// game thread in Frame, between plugin callbacks.
//
//   { "raw_base": "https://raw.githubusercontent.com/",          (optional; file:///... for a local test mirror)
//     "plugins": [ { "id": "replay-manager", "name": "...", "description": "...", "author": "...",
//                    "repo": "owner/name", "version": "0.1.0", "commit": "<sha>", "min_host": "0.3.0",
//                    "icon": "icon.png", "files": { "info.toml": "<sha256>", "main.as": "<sha256>", ... } } ] }
//
// "path" (optional) is the plugin's folder inside its repo, for a repo that holds several plugins:
// "path": "plugins/grind-stats". Files are downloaded from <repo>/<commit>/<path>/<file>; `files` stays relative to
// the plugin's folder, and is installed the same way either way.
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
    std::string id, name, description, author, repo, path, version, commit, minHost;
    std::vector<std::string> dependencies;                  // ids installed before it
    std::string iconFile;                                   // one of `files`, or ""
    std::vector<std::pair<std::string, std::string>> files; // name, sha256
    // Maintainer-built cooked content attached to the plugin's own GitHub release (never in its repo):
    // "assets/<name>.utoc|.ucas|.pak" -> {url under github.com/<repo>/releases/download/, sha256}.
    struct ReleaseFile { std::string name, url, sha; };
    std::vector<ReleaseFile> releaseFiles;
    std::string icon;                                       // local copy of the icon once downloaded, or ""
    bool listed = true;                                     // offered in the plugin browser (false: installed only as a dependency)
    std::string category;                                   // where it's listed (see Placing)
    std::string added;                                      // "YYYY-MM-DD" it joined the registry, or "" (its NEW tag)
    int priority = 0;                                       // higher first within its category
    std::string Page() const {
        return "https://github.com/" + repo + (path.empty() ? "" : "/tree/" + commit + "/" + path);
    }
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

// Where a plugin is listed in the plugin manager, in this order: "core" (the plugin manager), "enhancements",
// "utilities", "fun", "cosmetics", then everything without one. "cosmetics": not in the plugin browser (the plugin
// manager's cosmetics page lists them). registry.json's "category" and "priority" (higher first) say so for each
// plugin (one the registry doesn't list, or without them: none); only the plugin manager itself is placed by the host
// (core, first).
void Placing(const std::string& id, std::string* category, int* priority);
int CategoryRank(const std::string& category);         // 0 core .. 3 fun, 4 cosmetics, 5 anything else


// A cosmetic plugin: placed in "cosmetics", or placed nowhere and needing Cosmetic Kit (or Plus). Listed on the cosmetics
// page instead of the plugin browser, and kept in plugins\cosmetics\<id> (plugins::CosmeticsDir).
bool IsCosmetic(const std::string& id, const std::vector<std::string>& dependencies);

// The host itself. registry.json's "host" entry names the newest release:
//   "host": { "version": "0.4.2", "repo": "owner/name", "commit": "<the tag's full SHA>",
//             "dll": "https://github.com/<repo>/releases/download/v0.4.2/version.dll", "dll_sha256": "<sha256>",
//             "files": { "plugins/plugin-manager/main.as": "<sha256>", ... } }       (the bundled plugins)
// The running version.dll cannot be overwritten, but Windows lets it be renamed: an update renames it to
// version.dll.old-<n>, puts the new one in its place, replaces the bundled plugin files, and asks for a restart.
// The old copies are deleted at the next start. The DLL is only taken from the repo's GitHub releases (or a
// file:/// URL when the registry itself is a local test copy), and only with the registry's SHA-256.
std::string HostVersion();          // the newest host in the registry, or ""
void UpdateHost();                  // plugin manager only (checked in the API)
std::string HostUpdateState();      // "", "downloading", "restart", or "error: ..."
void CleanUpOldHost(const std::wstring& gameDir);  // at startup: deletes what an update left behind

}  // namespace registry
