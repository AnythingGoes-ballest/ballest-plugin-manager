#include "registry.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "engine.hpp"
#include "json.hpp"
#include "log.hpp"
#include "net.hpp"
#include "plugins.hpp"
#include "sandbox.hpp"

namespace registry {
namespace {

// --- the worker thread: jobs run there, their results come back to the game thread ----------------------------------

std::mutex gLock;
std::condition_variable gWake;
std::deque<std::function<void()>> gJobs;             // run on the worker
std::vector<std::function<void()>> gResults;         // run on the game thread, in Frame
bool gWorkerStarted = false;

void Post(std::function<void()> job) {
    std::lock_guard<std::mutex> g(gLock);
    if (!gWorkerStarted) {
        gWorkerStarted = true;
        std::thread([] {
            for (;;) {
                std::function<void()> next;
                {
                    std::unique_lock<std::mutex> g(gLock);
                    gWake.wait(g, [] { return !gJobs.empty(); });
                    next = std::move(gJobs.front());
                    gJobs.pop_front();
                }
                next();
            }
        }).detach();
    }
    gJobs.push_back(std::move(job));
    gWake.notify_one();
}

void Deliver(std::function<void()> result) {
    std::lock_guard<std::mutex> g(gLock);
    gResults.push_back(std::move(result));
}

// --- state (game thread) ---------------------------------------------------------------------------------------------

std::vector<Entry> gEntries;
std::string gState;
std::string gRawBase = "https://raw.githubusercontent.com/";
std::map<std::string, std::string> gPending;
bool gLocalRegistry = false;                         // read from file:/// (a test copy)

struct HostRelease {
    std::string version, repo, commit, dll, dllSha;
    std::vector<std::pair<std::string, std::string>> files;     // path under the game folder, sha256
};
HostRelease gHost;
std::string gHostState;
const std::set<std::string> kDependencyOnly = {};     // (Cosmetic Kit Plus is a plugin of its own again: offered as one)

// The host's own placing (Lvcid, 2026-10-07): registry.json's "category" and "priority" override it.
struct Place {
    const char *id, *category;
    int priority;
};
const Place kPlaces[] = {
    {"plugin-manager", "core", 100},
    {"hub-plus", "enhancements", 50},       {"create-extensions", "enhancements", 40}, {"cosmetic-kit", "enhancements", 30}, {"cosmetic-kit-plus", "enhancements", 25},
    {"player-count", "enhancements", 10},
    {"replay-manager", "utilities", 50},    {"practice-checkpoints", "utilities", 40}, {"grind-stats", "utilities", 30},
    {"grind-timer", "utilities", 20},       {"fit-window", "utilities", 10},
    {"screen-filters", "fun", 10},
    // the cosmetic plugins (the cosmetics page lists them; they're kept in plugins\cosmetics)
    {"fun-balls", "cosmetics", 0},          {"example-bounce", "cosmetics", 0},        {"example-arms", "cosmetics", 0},
    {"chicken-cosmetic", "cosmetics", 0},   {"clear-tec-balls", "cosmetics", 0},       {"dominus-ball", "cosmetics", 0},
    {"example-cosmetics", "cosmetics", 0},  {"fish-bowl", "cosmetics", 0},             {"glass-balls", "cosmetics", 0},
    {"halloween-pack", "cosmetics", 0},     {"monkey-balls", "cosmetics", 0},          {"pac-man", "cosmetics", 0},
    {"rainbow-ball", "cosmetics", 0},       {"two-tone-balls", "cosmetics", 0},
};
bool NeedsKit(const std::vector<std::string>& dependencies) {
    return std::any_of(dependencies.begin(), dependencies.end(), [](const std::string& d) { return d == "cosmetic-kit" || d == "cosmetic-kit-plus"; });
}
// Installed for a fresh install, on (the enhancements).
const char* const kDefaults[] = {"hub-plus", "create-extensions", "cosmetic-kit", "player-count"};
std::vector<std::string> gRemovals;                  // applied at the start of the next Frame

// --- validation: registry values end up in paths and URLs, so only plain names are accepted -------------------------

bool Plain(const std::string& s, const char* extra, size_t maxLength) {
    if (s.empty() || s.size() > maxLength || s[0] == '.') return false;
    for (char c : s)
        if (!std::isalnum(static_cast<unsigned char>(c)) && !std::strchr(extra, c)) return false;
    return true;
}
bool ValidId(const std::string& s) { return Plain(s, "-_", 64); }
bool ValidFile(const std::string& s) { return Plain(s, "-_.", 96) && s.find("..") == std::string::npos; }
// A plugin's file: a name, or a path of up to three names separated by "/" (a plugin's assets in subfolders).
bool ValidPluginFile(const std::string& s) {
    size_t start = 0;
    for (int part = 0; part < 3; ++part) {
        const size_t slash = s.find('/', start);
        if (!ValidFile(s.substr(start, slash == std::string::npos ? std::string::npos : slash - start))) return false;
        if (slash == std::string::npos) return true;
        start = slash + 1;
    }
    return false;
}
bool ValidRepo(const std::string& s) {
    const size_t slash = s.find('/');
    return slash != std::string::npos && Plain(s.substr(0, slash), "-_.", 64) && Plain(s.substr(slash + 1), "-_.", 100);
}
bool Hex(const std::string& s, size_t min, size_t max) {
    if (s.size() < min || s.size() > max) return false;
    for (char c : s)
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

// Where a plugin is installed: plugins\<id>, or plugins\cosmetics\<id> for a cosmetic one.
std::wstring InstallPath(const Entry& e) {
    return (IsCosmetic(e.id, e.dependencies) ? plugins::CosmeticsDir() : plugins::Dir()) + L"\\" + eng::Widen(e.id);
}
std::wstring StagingPath(const std::string& id) { return plugins::Dir() + L"\\." + eng::Widen(id) + L".download"; }
std::wstring IconCache() { return hostlog::DataDir() + L"\\cache"; }

bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

// Deletes a folder and everything in it. Only ever called on a plugin or staging folder built from a valid id.
void DeleteTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            const std::wstring path = dir + L"\\" + name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) DeleteTree(path);
            else {
                SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(path.c_str());
            }
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    RemoveDirectoryW(dir.c_str());
}

// Written whole or not at all: to a ".part" file, then moved over the real one (a crash mid-write left a cut-off file
// under the right name, e.g. an icon never fetched again).
bool WriteAll(const std::wstring& path, const std::string& data) {
    const std::wstring part = path + L".part";
    {
        std::ofstream f(part.c_str(), std::ios::binary | std::ios::trunc);
        f.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!f) {
            f.close();
            DeleteFileW(part.c_str());
            return false;
        }
    }
    if (MoveFileExW(part.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
    DeleteFileW(part.c_str());
    return false;
}

// A SHA-256 as net::Sha256 writes it (lower case): the registry's own may be upper case.
std::string LowerHex(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'F') c = static_cast<char>(c + 32);
    return s;
}

std::string FileUrl(const Entry& e, const std::string& file) {
    return gRawBase + e.repo + "/" + e.commit + "/" + (e.path.empty() ? "" : e.path + "/") + file;
}

std::string Sha(const Entry& e, const std::string& file) {
    for (const auto& [name, sha] : e.files)
        if (name == file) return sha;
    return "";
}

// --- reading registry.json --------------------------------------------------------------------------------------------

std::string RegistryUrl() {
    std::ifstream f((hostlog::DataDir() + L"\\registry_url.txt").c_str());
    std::string url;
    if (f && std::getline(f, url)) {
        while (!url.empty() && (url.back() == '\r' || url.back() == ' ')) url.pop_back();
        if (!url.empty()) return url;
    }
    return kDefaultUrl;
}

bool ReadEntry(const json::Value& v, Entry& e, std::string& why) {
    e.id = v.Str("id");
    e.name = v.Str("name", e.id);
    e.description = v.Str("description");
    e.author = v.Str("author");
    e.repo = v.Str("repo");
    e.path = v.Str("path");
    e.version = v.Str("version", "0.0.0");
    e.commit = v.Str("commit");
    e.minHost = v.Str("min_host");
    e.iconFile = v.Str("icon");
    if (const json::Value* deps = v.Get("dependencies"); deps && deps->type == json::Value::Array)
        for (const auto& d : deps->items)
            if (d.type == json::Value::String && ValidId(d.string)) e.dependencies.push_back(d.string);
    if (!ValidId(e.id)) return why = "bad id '" + e.id + "'", false;
    if (!ValidRepo(e.repo)) return why = e.id + ": bad repo", false;
    if (!e.path.empty() && !ValidPluginFile(e.path)) return why = e.id + ": bad path", false;
    if (!Hex(e.commit, 40, 40)) return why = e.id + ": commit must be a full SHA", false;
    const json::Value* files = v.Get("files");
    if (!files || files->type != json::Value::Object || files->members.empty()) return why = e.id + ": no files", false;
    for (const auto& [name, sha] : files->members) {
        if (!ValidPluginFile(name) || sha.type != json::Value::String || !Hex(sha.string, 64, 64))
            return why = e.id + ": bad file entry '" + name + "'", false;
        e.files.emplace_back(name, LowerHex(sha.string));
    }
    if (!e.iconFile.empty() && Sha(e, e.iconFile).empty()) e.iconFile.clear();       // must be one of the files
    return true;
}

void FetchIcon(const Entry& e) {
    const std::string sha = Sha(e, e.iconFile);
    const std::wstring path = IconCache() + L"\\" + eng::Widen(e.id + "-" + sha.substr(0, 16)) + L".png";
    const std::string narrow = eng::Narrow(path.c_str(), static_cast<int>(path.size()));
    const std::string id = e.id, url = FileUrl(e, e.iconFile);
    if (Exists(path)) {
        for (auto& entry : gEntries)
            if (entry.id == id) entry.icon = narrow;
        return;
    }
    Post([=] {
        std::string body, error;
        const bool ok = net::Fetch(url, body, error) && net::Sha256(body) == sha;
        if (ok) {
            CreateDirectoryW(IconCache().c_str(), nullptr);
            WriteAll(path, body);
        }
        Deliver([=] {
            if (!ok) return hostlog::Warn("registry: icon for " + id + " not downloaded" + (error.empty() ? " (hash mismatch)" : ": " + error));
            for (auto& entry : gEntries)
                if (entry.id == id) entry.icon = narrow;
        });
    });
}

// "plugins/<id>/<file>": the only paths a host release may write besides version.dll.
bool ValidBundledPath(const std::string& path) {
    const size_t a = path.find('/'), b = a == std::string::npos ? a : path.find('/', a + 1);
    return a != std::string::npos && b != std::string::npos && path.substr(0, a) == "plugins" &&
           ValidId(path.substr(a + 1, b - a - 1)) && ValidFile(path.substr(b + 1));
}

void ReadHost(const json::Value* v) {
    gHost = HostRelease{};
    if (!v || v->type != json::Value::Object) return;
    HostRelease h;
    h.version = v->Str("version");
    h.repo = v->Str("repo");
    h.commit = v->Str("commit");
    h.dll = v->Str("dll");
    h.dllSha = LowerHex(v->Str("dll_sha256"));
    const bool fromRelease = h.dll.rfind("https://github.com/" + h.repo + "/releases/download/", 0) == 0;
    const bool fromTestCopy = gLocalRegistry && h.dll.rfind("file:///", 0) == 0;
    std::string why;
    if (h.version.empty() || !ValidRepo(h.repo) || !Hex(h.commit, 40, 40) || !Hex(h.dllSha, 64, 64)) why = "incomplete";
    else if (!fromRelease && !fromTestCopy) why = "the DLL must come from the repo's GitHub releases";
    const json::Value* files = v->Get("files");
    if (why.empty() && files && files->type == json::Value::Object)
        for (const auto& [path, sha] : files->members) {
            if (!ValidBundledPath(path) || sha.type != json::Value::String || !Hex(sha.string, 64, 64)) {
                why = "bad file '" + path + "'";
                break;
            }
            h.files.emplace_back(path, LowerHex(sha.string));
        }
    if (!why.empty()) return hostlog::Warn("registry: host entry ignored: " + why);
    gHost = h;
}

std::wstring GameDir() {
    const std::wstring dir = plugins::Dir();
    return dir.substr(0, dir.find_last_of(L'\\'));
}

void Loaded(const std::string& body, const std::string& fetchError, const std::string& url) {
    json::Value root;
    std::string error = fetchError;
    if (error.empty() && !json::Parse(body, root, error)) error = "registry.json: " + error;
    const json::Value* list = root.Get("plugins");
    if (error.empty() && (!list || list->type != json::Value::Array)) error = "registry.json has no plugins list";
    if (!error.empty()) {
        gState = "error: " + error;
        hostlog::Warn("registry: " + error + " (" + url + ")");
        return;
    }
    gRawBase = root.Str("raw_base", "https://raw.githubusercontent.com/");
    if (gRawBase.empty()) gRawBase = "https://raw.githubusercontent.com/";     // ("raw_base": "" in the registry)
    if (gRawBase.back() != '/') gRawBase += '/';
    gLocalRegistry = url.rfind("file:///", 0) == 0;
    ReadHost(root.Get("host"));
    gEntries.clear();
    for (const auto& item : list->items) {
        Entry e;
        std::string why;
        if (item.type == json::Value::Object && ReadEntry(item, e, why)) {
            const json::Value* listed = item.Get("listed");
            e.listed = !DependencyOnly(e.id) && !(listed && listed->type == json::Value::Bool && !listed->boolean);
            Placing(e.id, &e.category, &e.priority);
            e.category = item.Str("category", e.category);
            e.added = item.Str("added");
            if (const json::Value* priority = item.Get("priority"); priority && priority->type == json::Value::Number)
                e.priority = static_cast<int>(priority->number);
            if (e.category.empty() && NeedsKit(e.dependencies)) e.category = "cosmetics";
            if (e.category == "cosmetics") e.listed = false;      // the plugin manager's cosmetics page lists these
            gEntries.push_back(e);
        } else {
            hostlog::Warn("registry: skipped an entry: " + why);
        }
    }
    gState = "ready";
    hostlog::Info("registry: " + std::to_string(gEntries.size()) + " plugin(s) from " + url +
                  (gHost.version.empty() ? "" : "; host " + gHost.version + " (this is " + plugins::kHostVersion + ")"));
    for (const auto& e : gEntries)
        if (!e.iconFile.empty()) FetchIcon(e);
}

// --- installing and removing -------------------------------------------------------------------------------------------

void InstallFailed(const Entry& e, const std::string& error) {
    DeleteTree(StagingPath(e.id));
    gPending[e.id] = "error: " + error;
    hostlog::Warn("registry: install of " + e.id + " failed: " + error);
}

void Apply(const Entry& e) {
    // The download is complete and verified: swap it in and start it. Plugins that depend on it are stopped with it
    // and started again after it (one that arrived first, while it was missing, starts now).
    // (a cosmetic plugin goes into plugins\cosmetics: wherever it was before is cleared)
    const std::vector<std::string> dependents = plugins::Dependents(e.id);
    const std::wstring before = plugins::FolderOf(e.id), target = InstallPath(e);
    plugins::Unload(e.id);
    if (!before.empty() && before != target) DeleteTree(before);
    DeleteTree(target);
    if (target.compare(0, plugins::CosmeticsDir().size(), plugins::CosmeticsDir()) == 0) CreateDirectoryW(plugins::CosmeticsDir().c_str(), nullptr);
    if (!MoveFileW(StagingPath(e.id).c_str(), target.c_str())) {
        gPending[e.id] = "error: could not move the plugin into place (" + std::to_string(GetLastError()) + ")";
        DeleteTree(StagingPath(e.id));
        return;
    }
    gPending.erase(e.id);
    const bool ok = plugins::Load(e.id);
    hostlog::Info("registry: installed " + e.id + " " + e.version + (ok ? "" : " (it did not start; see the log)"));
    for (const auto& id : dependents)
        if (plugins::Restart(id)) hostlog::Info("registry: started " + id + " again with " + e.id);
}

}  // namespace

std::string Pending(const std::string& id);

bool DependencyOnly(const std::string& id) { return kDependencyOnly.count(id) > 0; }

bool IsCosmetic(const std::string& id, const std::vector<std::string>& dependencies) {
    std::string category;
    int priority = 0;
    Placing(id, &category, &priority);
    return category == "cosmetics" || (category.empty() && NeedsKit(dependencies));
}

void Frame() {
    for (const auto& id : gRemovals) {
        const std::wstring dir = plugins::FolderOf(id);
        plugins::Unload(id);
        if (!dir.empty()) DeleteTree(dir);
        gPending.erase(id);
        hostlog::Info("registry: removed " + id);
    }
    gRemovals.clear();
    std::vector<std::function<void()>> results;
    {
        std::lock_guard<std::mutex> g(gLock);
        results.swap(gResults);
    }
    for (auto& result : results) result();
    InstallDefaults();
}

void Refresh() {
    if (gState == "loading") return;
    gState = "loading";
    const std::string url = RegistryUrl();
    Post([url] {
        std::string body, error;
        net::Fetch(url, body, error);
        Deliver([=] { Loaded(body, error, url); });
    });
}

void Placing(const std::string& id, std::string* category, int* priority) {
    *category = "";
    *priority = 0;
    for (const Place& p : kPlaces)
        if (id == p.id) {
            *category = p.category;
            *priority = p.priority;
        }
    for (const Entry& e : gEntries)                 // the registry's own, when it has them
        if (e.id == id && !e.category.empty()) {
            *category = e.category;
            *priority = e.priority;
        }
}

int CategoryRank(const std::string& category) {
    if (category == "core") return 0;
    if (category == "enhancements") return 1;
    if (category == "utilities") return 2;
    if (category == "fun") return 3;
    if (category == "cosmetics") return 4;
    return 5;
}

std::string CategoryTitle(const std::string& category) {
    if (category == "core") return "PLUGIN MANAGER";
    if (category == "enhancements") return "ENHANCEMENTS";
    if (category == "utilities") return "UTILITIES";
    if (category == "fun") return "FUN";
    if (category == "cosmetics") return "COSMETICS";
    return category.empty() ? "" : "OTHER";
}

void InstallDefaults() {
    static bool done = false;
    if (done || gState != "ready") return;
    done = true;
    const std::wstring marker = hostlog::DataDir() + L"\\defaults.txt";
    if (GetFileAttributesW(marker.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    // fresh: nothing installed but what ships with the host
    bool fresh = true;
    for (const plugins::Info& info : plugins::List())
        if (info.id != "plugin-manager" && info.id != "hello-world") fresh = false;
    std::string installed;
    if (fresh)
        for (const char* id : kDefaults)
            for (const Entry& e : gEntries)
                if (e.id == id) {
                    Install(id);
                    installed += std::string(installed.empty() ? "" : ", ") + id;
                }
    HANDLE h = CreateFileW(marker.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        const std::string text = fresh ? "fresh install: " + installed + "\n" : "not a fresh install: nothing added\n";
        DWORD written = 0;
        WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
        CloseHandle(h);
    }
    hostlog::Info(fresh ? "registry: a fresh install: getting " + (installed.empty() ? std::string("nothing (none listed)") : installed)
                        : "registry: not a fresh install, no default plugins added");
}

std::string State() { return gState; }
const std::vector<Entry>& Entries() { return gEntries; }

void Install(const std::string& id) {
    const Entry* found = nullptr;
    for (const auto& e : gEntries)
        if (e.id == id) found = &e;
    const std::string busy = Pending(id);
    if (!found || busy == "installing" || busy == "removing") return;
    if (!found->minHost.empty() && plugins::CompareVersions(found->minHost, plugins::kHostVersion) > 0) {
        gPending[id] = "error: needs host " + found->minHost;
        hostlog::Error("registry: " + id + " needs plugin manager " + found->minHost + " (this is " + plugins::kHostVersion +
                       "): not installed");
        return;
    }
    plugins::Info info;
    if (plugins::Find(id, &info) && info.essential) return;
    // Its dependencies first, when they are not installed: they arrive before it, so it starts with them loaded. (Not
    // deeper than 8: two entries needing each other recursed until the game crashed.)
    static int depth = 0;
    for (const auto& dependency : found->dependencies)
        if (!plugins::Find(dependency, nullptr)) {
            if (depth >= 8) {
                hostlog::Error("registry: " + id + " needs " + dependency + ": dependencies too deep (a loop?), not followed");
                continue;
            }
            hostlog::Info("registry: " + id + " needs " + dependency + "; installing that first");
            ++depth;
            Install(dependency);
            --depth;
        }
    gPending[id] = "installing";
    const Entry e = *found;
    std::vector<std::pair<std::string, std::string>> urls;
    for (const auto& [file, sha] : e.files) urls.emplace_back(file, FileUrl(e, file));
    Post([e, urls] {
        // Everything is downloaded and checked before anything is written into the plugins folder.
        std::vector<std::pair<std::string, std::string>> bodies;
        std::string error;
        for (const auto& [file, url] : urls) {
            std::string body;
            if (!net::Fetch(url, body, error)) {
                error = file + ": " + error;
                break;
            }
            if (net::Sha256(body) != Sha(e, file)) {
                error = file + " does not match the registry's SHA-256";
                break;
            }
            bodies.emplace_back(file, std::move(body));
        }
        const std::wstring staging = StagingPath(e.id);
        if (error.empty()) {
            DeleteTree(staging);
            if (!CreateDirectoryW(staging.c_str(), nullptr)) error = "could not create the download folder";
            for (const auto& [file, body] : bodies) {
                std::wstring path = staging + L"\\" + eng::Widen(file);
                for (size_t i = staging.size() + 1; i < path.size(); ++i)       // its subfolders first
                    if (path[i] == L'/') {
                        path[i] = L'\\';
                        CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
                    }
                if (error.empty() && !WriteAll(path, body)) error = "could not write " + file;
            }
        }
        Deliver([e, error] {
            if (!error.empty()) return InstallFailed(e, error);
            Apply(e);
        });
    });
}

void Remove(const std::string& id) {
    plugins::Info info;
    if (!ValidId(id) || !plugins::Find(id, &info) || Pending(id) == "installing") return;
    if (info.essential) {
        gPending[id] = "error: cannot remove an essential plugin";
        return;
    }
    gPending[id] = "removing";
    gRemovals.push_back(id);
}

std::string Pending(const std::string& id) {
    const auto it = gPending.find(id);
    return it == gPending.end() ? "" : it->second;
}

std::string HostVersion() { return gHost.version; }
std::string HostUpdateState() { return gHostState; }

void UpdateHost() {
    if (gHost.version.empty() || gHostState == "downloading" || gHostState == "restart") return;
    if (plugins::CompareVersions(gHost.version, plugins::kHostVersion) <= 0) return;
    if (sandbox::On()) return hostlog::Info("registry: a sandboxed test copy doesn't update the host other copies share");
    gHostState = "downloading";
    const HostRelease h = gHost;
    const std::string rawBase = gRawBase;
    const std::wstring game = GameDir();
    Post([h, rawBase, game] {
        // Everything is downloaded and checked first; the new DLL waits beside the old one as version.dll.new.
        std::string dll, error;
        std::vector<std::pair<std::string, std::string>> bodies;
        if (!net::Fetch(h.dll, dll, error)) error = "version.dll: " + error;
        else if (net::Sha256(dll) != h.dllSha) error = "version.dll does not match the registry's SHA-256";
        for (const auto& [path, sha] : h.files) {
            if (!error.empty()) break;
            std::string body;
            if (!net::Fetch(rawBase + h.repo + "/" + h.commit + "/" + path, body, error)) error = path + ": " + error;
            else if (net::Sha256(body) != sha) error = path + " does not match the registry's SHA-256";
            else bodies.emplace_back(path, std::move(body));
        }
        const std::wstring fresh = game + L"\\version.dll.new";
        if (error.empty() && !WriteAll(fresh, dll)) error = "could not write version.dll.new";
        Deliver([h, bodies, error, game, fresh] {
            if (!error.empty()) {
                DeleteFileW(fresh.c_str());
                gHostState = "error: " + error;
                return hostlog::Warn("registry: host update failed: " + error);
            }
            // The running DLL can be renamed but not replaced: move it aside, then the new one into its place.
            const std::wstring current = game + L"\\version.dll";
            const std::wstring old = game + L"\\version.dll.old-" + std::to_wstring(GetTickCount64());
            if (!MoveFileExW(current.c_str(), old.c_str(), 0)) {
                DeleteFileW(fresh.c_str());
                gHostState = "error: could not move the running version.dll aside (" + std::to_string(GetLastError()) + ")";
                return hostlog::Warn("registry: " + gHostState);
            }
            if (!MoveFileExW(fresh.c_str(), current.c_str(), 0)) {
                const DWORD code = GetLastError();
                MoveFileExW(old.c_str(), current.c_str(), 0);
                gHostState = "error: could not put the new version.dll in place (" + std::to_string(code) + ")";
                return hostlog::Warn("registry: " + gHostState);
            }
            // The bundled plugins' files; the running scripts are not reloaded, so both change at the restart.
            for (const auto& [path, body] : bodies) {
                std::wstring target = game + L"\\" + eng::Widen(path);
                for (auto& c : target)
                    if (c == L'/') c = L'\\';
                CreateDirectoryW(target.substr(0, target.find_last_of(L'\\')).c_str(), nullptr);
                if (!WriteAll(target, body)) hostlog::Warn("registry: could not write " + path);
            }
            gHostState = "restart";
            hostlog::Info("registry: host " + h.version + " installed; it runs from the next start of the game");
        });
    });
}

void CleanUpOldHost(const std::wstring& gameDir) {
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((gameDir + L"\\version.dll.old-*").c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do DeleteFileW((gameDir + L"\\" + fd.cFileName).c_str());
        while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    DeleteFileW((gameDir + L"\\version.dll.new").c_str());       // an update that never finished
}

std::string DefaultIcon() {
    const std::wstring path = plugins::Dir() + L"\\plugin-manager\\default-icon.png";
    return Exists(path) ? eng::Narrow(path.c_str(), static_cast<int>(path.size())) : "";
}

}  // namespace registry
