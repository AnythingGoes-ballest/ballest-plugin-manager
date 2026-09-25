#include "plugins.hpp"

#include <windows.h>

#include <angelscript.h>
#include <scriptbuilder/scriptbuilder.h>

#include <algorithm>
#include <set>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <thread>

#include "api.hpp"
#include "engine.hpp"
#include "game.hpp"
#include "editor.hpp"
#include "leaderboard.hpp"
#include "log.hpp"
#include "settings.hpp"
#include "ui.hpp"

namespace plugins {
namespace {

// A plugin keeps its slot (its index) for the whole session, even once removed: the index names its script module
// and owns its UI, and a reinstall gets a new slot.
struct Plugin {
    std::string id, name, version, author, description, minHost, icon;
    std::wstring dir;
    std::vector<std::string> files;
    std::vector<std::string> dependencies;      // ids of plugins that must be running first ([meta] dependencies)
    int timeoutMs = 50;
    bool essential = false;
    std::string status = "not loaded";
    bool running = false, removed = false;
    asIScriptModule* module = nullptr;
    asIScriptContext* ctx = nullptr;
    asIScriptFunction* update = nullptr;
    asIScriptFunction* onSettingsChanged = nullptr;
};

asIScriptEngine* gEngine = nullptr;
std::vector<Plugin> gPlugins;
std::wstring gDir;
ULONGLONG gDeadline = 0;
ULONGLONG gGameWorkLeft = 0;            // how much more game work this callback may have off its budget
bool gInFrame = false;
int gRunning = -1;                      // the plugin whose script is running, or -1
std::string gCrashNext;                 // test hook: the plugin whose next callback faults on purpose
std::set<std::string> gOff;             // plugins the player turned off: listed, not started (saved in off.txt)
std::vector<std::pair<std::string, bool>> gToggles;     // asked for during plugin code, applied after it

std::wstring OffFile() { return hostlog::DataDir() + L"\\off.txt"; }

void ReadOff() {
    std::ifstream f(OffFile().c_str());
    for (std::string line; std::getline(f, line);) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) gOff.insert(line);
    }
}

void WriteOff() {
    std::ofstream f(OffFile().c_str(), std::ios::trunc);
    for (const auto& id : gOff) f << id << "\n";
}
constexpr ULONGLONG kMaxGameWorkMs = 5000;

// --- manifest: the subset of TOML info.toml uses ([section], key = "string" | number | ["a", "b"]) ------------------

std::string Trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

std::string Unquote(const std::string& v) {
    return v.size() >= 2 && v.front() == '"' && v.back() == '"' ? v.substr(1, v.size() - 2) : v;
}

std::map<std::string, std::string> ParseToml(const std::string& text) {
    std::map<std::string, std::string> out;
    std::string section, line;
    std::istringstream in(text);
    while (std::getline(in, line)) {
        // A '#' starts a comment unless it is inside a string.
        bool quoted = false;
        for (size_t i = 0; i < line.size(); ++i) {
            if (line[i] == '"') quoted = !quoted;
            if (line[i] == '#' && !quoted) {
                line.resize(i);
                break;
            }
        }
        line = Trim(line);
        if (line.empty()) continue;
        if (line.front() == '[' && line.back() == ']') section = Trim(line.substr(1, line.size() - 2));
        else if (const size_t eq = line.find('='); eq != std::string::npos)
            out[section + "." + Trim(line.substr(0, eq))] = Trim(line.substr(eq + 1));
    }
    return out;
}

std::vector<std::string> StringList(const std::string& v) {
    std::vector<std::string> out;
    std::istringstream in(v.size() >= 2 && v.front() == '[' ? v.substr(1, v.size() - 2) : v);
    for (std::string item; std::getline(in, item, ',');)
        if (!Trim(item).empty()) out.push_back(Unquote(Trim(item)));
    return out;
}

bool ReadFile(const std::wstring& path, std::string& out) {
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

bool ReadManifest(const std::wstring& dir, const std::string& id, Plugin& p) {
    std::string text;
    if (!ReadFile(dir + L"\\info.toml", text)) return false;
    auto kv = ParseToml(text);
    auto value = [&](const char* key, const std::string& fallback) { return kv.count(key) ? Unquote(kv[key]) : fallback; };
    p.dir = dir;
    p.id = id;
    p.name = value("meta.name", id);
    p.version = value("meta.version", "0.0.0");
    p.author = value("meta.author", "");
    p.description = value("meta.description", "");
    p.minHost = value("meta.min_host", "");
    p.essential = value("meta.essential", "false") == "true";
    p.files = kv.count("script.files") ? StringList(kv["script.files"]) : std::vector<std::string>{"main.as"};
    if (kv.count("meta.dependencies")) p.dependencies = StringList(kv["meta.dependencies"]);
    if (kv.count("script.timeout")) p.timeoutMs = std::max(1, std::atoi(kv["script.timeout"].c_str()));
    // The plugin's own icon, else the default one the plugin manager ships.
    const std::string icon = value("meta.icon", "icon.png");
    if (icon.find_first_of("/\\") == std::string::npos && Exists(dir + L"\\" + eng::Widen(icon)))
        p.icon = eng::Narrow(dir.c_str(), static_cast<int>(dir.size())) + "\\" + icon;
    return true;
}

// --- running scripts -------------------------------------------------------------------------------------------------

void MessageCallback(const asSMessageInfo* msg, void*) {
    const char* level = msg->type == asMSGTYPE_ERROR ? "error" : msg->type == asMSGTYPE_WARNING ? "warn" : "info";
    hostlog::Write(level, "compiler", std::string(msg->section) + " (" + std::to_string(msg->row) + ", " +
                                          std::to_string(msg->col) + "): " + msg->message);
}

void LineCallback(asIScriptContext* ctx, void*) {
    if (GetTickCount64() > gDeadline) ctx->Abort();
}

// A stopped plugin's script never runs again this session, so nothing it put on screen would answer any more:
// its windows and panels are hidden and its cursor request dropped, so nothing dead is left blocking clicks.
void Stop(Plugin& p, const std::string& why) {
    p.running = false;
    p.status = why;
    hostlog::Write("error", p.id, why);
    const int index = static_cast<int>(&p - gPlugins.data());
    ui::HideOwner(index);
    game::RequestCursor(index, false);
}

// Runs one callback within the plugin's time budget; an exception or overrun stops the plugin.
void Run(Plugin& p, asIScriptFunction* fn, const float* dt) {
    const int index = static_cast<int>(&p - gPlugins.data());
    struct Running {                    // which plugin is running, for Current() and for a fault's blame
        int previous;
        explicit Running(int i) : previous(gRunning) { gRunning = i; }
        ~Running() { gRunning = previous; }
    } running(index);
    if (!gCrashNext.empty() && gCrashNext == p.id) {
        gCrashNext.clear();
        *static_cast<volatile int*>(nullptr) = 1;       // test: a fault inside host code while this plugin runs
    }
    if (!fn || !p.running) return;
    p.ctx->Prepare(fn);
    if (dt) p.ctx->SetArgFloat(0, *dt);
    gDeadline = GetTickCount64() + p.timeoutMs;
    gGameWorkLeft = kMaxGameWorkMs;
    switch (const int r = p.ctx->Execute()) {
        case asEXECUTION_FINISHED:
            return;
        case asEXECUTION_EXCEPTION:
            return Stop(p, std::string("exception: ") + p.ctx->GetExceptionString() + " in " +
                               p.ctx->GetExceptionFunction()->GetDeclaration() + " line " +
                               std::to_string(p.ctx->GetExceptionLineNumber()));
        case asEXECUTION_ABORTED:
            return Stop(p, "stopped: exceeded its " + std::to_string(p.timeoutMs) + " ms budget");
        default:
            return Stop(p, "stopped: execution result " + std::to_string(r));
    }
}

// A plugin's files are the ones its manifest lists; #include is not followed.
int RefuseInclude(const char* include, const char* from, CScriptBuilder*, void*) {
    hostlog::Write("error", "compiler", std::string(from) + ": #include \"" + include + "\" is not supported; list the file in info.toml");
    return -1;
}

Plugin* Loaded(const std::string& id);

// Each plugin compiles into its own module, named by its id: another plugin that depends on it imports its functions
// by that name (import void AddBall(...) from "cosmetic-kit";), and API calls tell which plugin made them from the module
// of the function running. The script builder keeps each global's metadata, which is where [Setting] tags come from.
void Start(size_t index) {
    Plugin& p = gPlugins[index];
    if (gOff.count(p.id) && !p.essential) {
        p.status = "off";
        hostlog::Write("info", p.id, "off (turned off in the plugin manager)");
        return;
    }
    if (!p.minHost.empty() && CompareVersions(p.minHost, kHostVersion) > 0) {
        p.status = "needs host " + p.minHost;
        hostlog::Write("error", p.id, p.status + " (this is " + kHostVersion + ")");
        return;
    }
    for (const auto& dependency : p.dependencies) {
        const Plugin* d = Loaded(dependency);
        if (!d || !d->running) {
            p.status = "needs " + dependency;
            hostlog::Write("error", p.id, p.status + (gOff.count(dependency) ? " (it is turned off)" : " (install it, or it stopped)"));
            return;
        }
    }
    CScriptBuilder builder;
    builder.SetIncludeCallback(RefuseInclude, nullptr);
    if (builder.StartNewModule(gEngine, p.id.c_str()) < 0) return;
    asIScriptModule* m = builder.GetModule();
    p.module = m;
    for (const auto& file : p.files) {
        std::string code;
        if (!ReadFile(p.dir + L"\\" + eng::Widen(file), code)) {
            p.status = "error: missing " + file;
            hostlog::Write("error", p.id, p.status);
            return;
        }
        builder.AddSectionFromMemory((p.id + "/" + file).c_str(), code.data(), static_cast<unsigned>(code.size()));
    }
    if (builder.BuildModule() < 0) {
        p.status = "error: does not compile (see log)";
        hostlog::Write("error", p.id, p.status);
        return;
    }
    // Imported functions (from its dependencies' modules) are bound now that those modules exist.
    if (m->GetImportedFunctionCount() > 0 && m->BindAllImportedFunctions() < 0) {
        p.status = "error: an imported function was not found in its dependencies (see log)";
        for (asUINT i = 0; i < m->GetImportedFunctionCount(); ++i)
            hostlog::Write("error", p.id, std::string("import ") + m->GetImportedFunctionDeclaration(i) + " from \"" +
                                              m->GetImportedFunctionSourceModule(i) + "\"");
        hostlog::Write("error", p.id, p.status);
        return;
    }
    settings::Collect(static_cast<int>(index), p.id, m, builder);      // saved values are in place before Main
    p.ctx = gEngine->CreateContext();
    p.ctx->SetLineCallback(asFUNCTION(LineCallback), nullptr, asCALL_CDECL);
    p.update = m->GetFunctionByDecl("void Update(float)");
    p.onSettingsChanged = m->GetFunctionByDecl("void OnSettingsChanged()");
    p.running = true;
    p.status = "running";
    hostlog::Write("info", p.id, "loaded " + p.name + " " + p.version);
    Run(p, m->GetFunctionByDecl("void Main()"), nullptr);
}

Plugin* Loaded(const std::string& id) {
    for (auto& p : gPlugins)
        if (p.id == id && !p.removed) return &p;
    return nullptr;
}

Info InfoOf(const Plugin& p) { return {p.id, p.name, p.version, p.author, p.description, p.status, p.icon, p.essential}; }

}  // namespace

void LoadAll(const std::wstring& pluginsDir) {
    gDir = pluginsDir;
    gEngine = asCreateScriptEngine();
    if (!gEngine) {
        hostlog::Error("AngelScript engine could not be created");
        return;
    }
    gEngine->SetMessageCallback(asFUNCTION(MessageCallback), nullptr, asCALL_CDECL);
    api::Register(gEngine);
    ReadOff();

    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((pluginsDir + L"\\*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) {
        hostlog::Warn("no plugins folder at " + eng::Narrow(pluginsDir.c_str(), static_cast<int>(pluginsDir.size())));
        return;
    }
    do {
        // Folders starting with '.' are the registry's work in progress (downloads not yet verified).
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        Plugin p;
        const std::wstring name = fd.cFileName;
        if (ReadManifest(pluginsDir + L"\\" + name, eng::Narrow(name.c_str(), static_cast<int>(name.size())), p)) gPlugins.push_back(p);
    } while (FindNextFileW(find, &fd));
    FindClose(find);

    // The plugin manager first, so its UI exists before the others report in; the rest by id, each after the plugins it
    // depends on (a dependency that is missing, or a cycle, leaves the plugin in place: Start reports it).
    std::sort(gPlugins.begin(), gPlugins.end(), [](const Plugin& a, const Plugin& b) {
        const bool am = a.id == "plugin-manager", bm = b.id == "plugin-manager";
        return am != bm ? am : a.id < b.id;
    });
    std::vector<Plugin> ordered;
    std::set<std::string> placed;
    for (size_t pass = 0; pass <= gPlugins.size() && ordered.size() < gPlugins.size(); ++pass)
        for (const auto& p : gPlugins) {
            if (placed.count(p.id)) continue;
            bool ready = true;
            for (const auto& d : p.dependencies) {
                const bool exists = std::any_of(gPlugins.begin(), gPlugins.end(), [&](const Plugin& q) { return q.id == d; });
                ready = ready && (!exists || placed.count(d) || pass == gPlugins.size());
            }
            if (ready) {
                ordered.push_back(p);
                placed.insert(p.id);
            }
        }
    gPlugins = ordered;
    hostlog::Info("found " + std::to_string(gPlugins.size()) + " plugin(s)");
    for (size_t i = 0; i < gPlugins.size(); ++i) Start(i);
}

void Frame(float dt) {
    gInFrame = true;
    for (size_t i = 0; i < gPlugins.size(); ++i) {
        Plugin& p = gPlugins[i];
        if (settings::TakeChanged(static_cast<int>(i)) && p.running) Run(p, p.onSettingsChanged, nullptr);
        if (p.running && p.update) Run(p, p.update, &dt);
    }
    gInFrame = false;
    const auto toggles = std::move(gToggles);
    gToggles.clear();
    for (const auto& [id, on] : toggles) SetEnabled(id, on);
    settings::Frame();
}

std::wstring Dir() { return gDir; }

void OpenFolder() {
    // Starting Explorer can take longer than a plugin's whole time budget (measured: the plugin manager was stopped
    // for it), so it is started from a thread of its own and the caller returns at once.
    if (gDir.empty()) return;
    std::thread([dir = gDir] {
        wchar_t windows[MAX_PATH];
        const UINT n = GetWindowsDirectoryW(windows, MAX_PATH);
        std::wstring command = L"\"" + std::wstring(windows, n) + L"\\explorer.exe\" \"" + dir + L"\"";
        STARTUPINFOW si{};
        si.cb = sizeof si;
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            hostlog::Info("opened the plugins folder");
        } else {
            hostlog::Warn("could not open the plugins folder (" + std::to_string(GetLastError()) + ")");
        }
    }).detach();
}

bool Load(const std::string& id) {
    if (gInFrame || !gEngine || Loaded(id)) return false;
    Plugin p;
    if (!ReadManifest(gDir + L"\\" + eng::Widen(id), id, p)) return false;
    gPlugins.push_back(p);
    Start(gPlugins.size() - 1);
    return gPlugins.back().running;
}

std::vector<std::string> Dependents(const std::string& id) {
    std::vector<std::string> out;
    for (const auto& p : gPlugins)
        if (!p.removed && std::find(p.dependencies.begin(), p.dependencies.end(), id) != p.dependencies.end())
            out.push_back(p.id);
    return out;
}

namespace {

// Frees a plugin's script, and with it its settings, UI and cursor request.
void Release(size_t i) {
    Plugin& p = gPlugins[i];
    p.running = false;
    p.update = p.onSettingsChanged = nullptr;
    settings::Forget(static_cast<int>(i));             // before the module (the variables) goes
    if (p.ctx) p.ctx->Release();
    p.ctx = nullptr;
    if (p.module) p.module->Discard();
    p.module = nullptr;
    // Its script is gone, so nothing can use its UI handles any more.
    ui::RemoveOwner(static_cast<int>(i));
    game::RequestCursor(static_cast<int>(i), false);
    leaderboard::RemoveOwner(static_cast<int>(i));
    editor::RemoveOwner(static_cast<int>(i));
}

// Plugins that depend on `id` lose their scripts before it does (their imported functions point into its module),
// but stay listed, as needing it, and start again when it is back (Restart).
void SuspendDependents(const std::string& id) {
    for (size_t i = 0; i < gPlugins.size(); ++i) {
        Plugin& p = gPlugins[i];
        if (p.removed || !p.module || std::find(p.dependencies.begin(), p.dependencies.end(), id) == p.dependencies.end())
            continue;
        SuspendDependents(p.id);
        hostlog::Write("info", p.id, "stopping: it depends on " + id + ", which is being unloaded");
        Release(i);
        gPlugins[i].status = "needs " + id;
    }
}

}  // namespace

void Unload(const std::string& id) {
    if (gInFrame) return;
    SuspendDependents(id);
    for (size_t i = 0; i < gPlugins.size(); ++i) {
        if (gPlugins[i].id != id || gPlugins[i].removed) continue;
        Release(i);
        gPlugins[i].removed = true;
        gPlugins[i].status = "removed";
        hostlog::Write("info", gPlugins[i].id, "unloaded");
    }
}

bool Restart(const std::string& id);

bool SetEnabled(const std::string& id, bool on) {
    if (gInFrame) {                     // from a plugin (the plugin manager's button): after this frame's plugin code
        gToggles.emplace_back(id, on);
        return true;
    }
    Plugin* p = Loaded(id);
    if (!p || p->essential || on == !gOff.count(id)) return false;
    if (on) {
        gOff.erase(id);
        WriteOff();
        hostlog::Write("info", id, "turned on");
        Restart(id);
        for (const auto& dependent : Dependents(id)) Restart(dependent);     // the ones waiting for it
    } else {
        gOff.insert(id);
        WriteOff();
        SuspendDependents(id);
        const size_t i = static_cast<size_t>(p - gPlugins.data());
        Release(i);
        gPlugins[i].status = "off";
        hostlog::Write("info", id, "turned off");
    }
    return true;
}

bool IsOff(const std::string& id) { return gOff.count(id) > 0; }

bool Restart(const std::string& id) {
    if (gInFrame) return false;
    for (size_t i = 0; i < gPlugins.size(); ++i) {
        Plugin& p = gPlugins[i];
        if (p.id != id || p.removed || p.running || gOff.count(id)) continue;
        Release(i);
        Start(i);
        return gPlugins[i].running;
    }
    return false;
}

std::vector<Info> List() {
    std::vector<Info> out;
    for (const auto& p : gPlugins)
        if (!p.removed) out.push_back(InfoOf(p));
    return out;
}

bool Find(const std::string& id, Info* out) {
    const Plugin* p = Loaded(id);
    if (p && out) *out = InfoOf(*p);
    return p != nullptr;
}

// The plugin whose script function is running: the module of the innermost function of the running context, so a
// function a plugin imported from its dependency counts as the dependency's (its log lines, storage and UI are its
// own). Only asked while Run has a plugin running: after a fault the abandoned context could still be reported as
// active.
int Current() {
    if (gRunning < 0) return -1;
    asIScriptContext* ctx = asGetActiveContext();
    asIScriptFunction* fn = ctx ? ctx->GetFunction(0) : nullptr;
    const char* module = fn ? fn->GetModuleName() : nullptr;
    if (module)
        for (size_t i = 0; i < gPlugins.size(); ++i)
            if (!gPlugins[i].removed && gPlugins[i].id == module) return static_cast<int>(i);
    return gRunning;
}

bool RecoverFromFault(const std::string& where) {
    gInFrame = false;
    if (gRunning < 0 || gRunning >= static_cast<int>(gPlugins.size())) return false;
    Plugin& p = gPlugins[static_cast<size_t>(gRunning)];
    gRunning = -1;
    p.ctx = nullptr;                    // abandoned mid-call: never touched again (a small leak, until restart)
    Stop(p, "stopped: crashed in host code (" + where + ")");
    return true;
}

void CrashNext(const std::string& id) { gCrashNext = id; }

std::string CurrentId() {
    const int i = Current();
    return i >= 0 && i < static_cast<int>(gPlugins.size()) ? gPlugins[static_cast<size_t>(i)].id : "?";
}

std::wstring CurrentDir() {
    const int i = Current();
    return i >= 0 && i < static_cast<int>(gPlugins.size()) ? gPlugins[static_cast<size_t>(i)].dir : std::wstring();
}

bool CurrentIsEssential() {
    const int i = Current();
    return i >= 0 && i < static_cast<int>(gPlugins.size()) && gPlugins[static_cast<size_t>(i)].essential;
}

std::string Summary() {
    std::string s;
    for (const auto& p : gPlugins)
        if (!p.removed) s += (s.empty() ? "" : "; ") + p.id + "=" + p.status;
    return s;
}

int CompareVersions(const std::string& a, const std::string& b) {
    std::istringstream x(a), y(b);
    for (std::string pa, pb;;) {
        const bool ha = static_cast<bool>(std::getline(x, pa, '.')), hb = static_cast<bool>(std::getline(y, pb, '.'));
        if (!ha && !hb) return 0;
        const long na = ha ? std::atol(pa.c_str()) : 0, nb = hb ? std::atol(pb.c_str()) : 0;
        if (na != nb) return na < nb ? -1 : 1;
    }
}

GameWork::GameWork() : start_(GetTickCount64()) {}

GameWork::~GameWork() {
    const ULONGLONG spent = std::min<ULONGLONG>(GetTickCount64() - start_, gGameWorkLeft);
    gDeadline += spent;
    gGameWorkLeft -= spent;
}

}  // namespace plugins
