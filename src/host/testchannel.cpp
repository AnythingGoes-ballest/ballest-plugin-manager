#include "testchannel.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "cosmetics.hpp"
#include "engine.hpp"
#include "draw.hpp"
#include "postprocess.hpp"
#include "game.hpp"
#include "hud.hpp"
#include "race.hpp"
#include "input.hpp"
#include "leaderboard.hpp"
#include "log.hpp"
#include "editor.hpp"
#include "models.hpp"
#include "plugins.hpp"
#include "registry.hpp"
#include "replay.hpp"
#include "settings.hpp"
#include "tracks.hpp"
#include "ui.hpp"
#if __has_include("sandbox.hpp")
#include "sandbox.hpp"
#define HOST_HAS_SANDBOX 1
#endif

namespace testchannel {

bool MutationsAllowed() {
#ifdef HOST_HAS_SANDBOX
    return sandbox::Complete();
#else
    return false;
#endif
}

namespace {

// The commands that only read the game or the host, allowed in any game. Every other command changes something (a
// test channel command once teleported a ball into the goal and the time reached the real leaderboard), so it runs
// only when MutationsAllowed().
bool ReadOnlyCommand(const std::string& name) {
    static const std::set<std::string> kReadOnly = {
        "state", "fps", "soundstate", "checkpoints", "openstate", "functions", "instances", "props", "find", "struct",
        "viewtarget", "materials", "watch", "children", "fnbytes", "matparams", "race", "ballstate", "ballsave", "hud",
        "widgetpath", "strprop", "objprop", "listprop", "dumptypes", "objbytes", "membytes", "objarray", "cosmetics",
        "skinmats", "sandbox", "sandboxtest", "fade"};
    return kReadOnly.count(name) > 0;
}

long long gHostFrames = 0;              // host frames so far, and where the last "fps" started counting
long long gFpsFrames = 0;
double gFpsSince = -1;
std::vector<std::pair<int, ULONGLONG>> gPostedUps;     // keys posted down, and when to let them go

std::vector<std::string> Words(const std::string& s) {
    std::vector<std::string> out;
    for (size_t i = 0; i < s.size();) {
        size_t j = s.find(' ', i);
        if (j == std::string::npos) j = s.size();
        if (j > i) out.push_back(s.substr(i, j - i));
        i = j + 1;
    }
    return out;
}

// Live, non-default instances of a class, filtered by path fragments joined with '+' (all must appear).
std::vector<eng::Obj> Instances(const std::string& className, const std::string& filter) {
    std::vector<std::string> parts;
    for (size_t start = 0; start <= filter.size();) {
        size_t end = filter.find('+', start);
        if (end == std::string::npos) end = filter.size();
        if (end > start) parts.push_back(filter.substr(start, end - start));
        start = end + 1;
    }
    std::vector<eng::Obj> out;
    eng::Obj cls = eng::FindClass(className);
    eng::ForEachObject([&](eng::Obj o) {
        if (!eng::IsA(o, cls) || eng::IsDefaultObject(o)) return true;
        const std::string path = eng::PathOf(o);
        for (const auto& part : parts)
            if (path.find(part) == std::string::npos) return true;
        out.push_back(o);
        return true;
    });
    return out;
}

std::string Hex(const uint8_t* p, size_t n) {
    std::string s;
    char b[4];
    for (size_t i = 0; i < n && i < 64; ++i) {
        std::snprintf(b, sizeof b, "%02x", p[i]);
        s += b;
    }
    return s;
}

void Report(const std::string& line) { hostlog::Info("test: " + line); }

// --- measurement commands ------------------------------------------------------------------------------------------

void Functions(const std::string& className) {
    eng::Obj cls = eng::FindClass(className);
    for (const auto& name : eng::FunctionNames(cls)) Report(className + "." + eng::Describe(eng::FindFunction(cls, name)));
}

void Props(const std::string& className, const std::string& filter) {
    const auto list = Instances(className, filter);
    if (list.empty()) return Report("no instance of " + className);
    eng::Obj o = list.front();
    Report("props of " + eng::PathOf(o));
    for (const auto& name : eng::PropertyNames(eng::ClassOf(o))) {
        const eng::Prop p = eng::FindProp(eng::ClassOf(o), name);
        Report("  " + name + " (" + eng::KindOf(p) + ") @" + hostlog::Hex(p.offset) + " size " + std::to_string(p.size) + " = " +
               Hex(o + p.offset, p.size));
    }
}

void Find(const std::string& fragment) {
    int shown = 0;
    eng::ForEachObject([&](eng::Obj o) {
        if (eng::ObjName(o).find(fragment) == std::string::npos) return true;
        Report("found " + eng::ObjName(eng::ClassOf(o)) + " " + eng::PathOf(o));
        return ++shown < 60;
    });
}

void Struct(const std::string& className, const std::string& function) {
    eng::Obj fn = eng::FindFunction(eng::FindClass(className), function);
    if (!fn) return Report("no function " + className + "." + function);
    for (const auto& param : eng::ParamsOf(fn)) {
        eng::Obj st = eng::StructOf(eng::FindProp(fn, param.name));
        if (!st) continue;
        Report(param.name + " is " + eng::ObjName(st));
        for (const auto& field : eng::PropertyNames(st)) {
            const eng::Prop fp = eng::FindProp(st, field);
            Report("  " + field + " @" + hostlog::Hex(fp.offset) + " size " + std::to_string(fp.size) + " " + eng::KindOf(fp) +
                   (eng::StructOf(fp) ? " " + eng::ObjName(eng::StructOf(fp)) : ""));
        }
    }
}

// "pov <filter>": watches a leaderboard entry's replay the way the game does, by handing the entry's own GhostKey
// (read, not built) to BP_MyPlayerController.PlayerWantsToPOVGhost. Calling the entry's Blueprint helpers directly
// hung the game once and crashed it once, so nothing on the entry is called.
void Pov(const std::string& filter) {
    const auto list = Instances("WBP_LeaderboardEntry_C", filter);
    if (list.empty()) return Report("no leaderboard entry matching " + filter);
    eng::Obj entry = list.front();
    bool hasKey = false;
    uint8_t key[96];
    if (!eng::ReadBool(entry, "bHasGhostKey", &hasKey) || !hasKey || !eng::ReadBytes(entry, "GhostKey", key, sizeof key))
        return Report("pov: " + eng::PathOf(entry) + " has no ghost key");
    eng::Obj controller = game::PlayerController();
    eng::Params p(eng::FunctionOn(controller, "PlayerWantsToPOVGhost"));
    const bool ok = p.Set("GhostKey", key, sizeof key) && eng::Invoke(controller, p);
    Report("pov " + eng::PathOf(entry) + (ok ? " -> ok" : " -> FAILED"));
}

void CallNoArgs(const std::string& className, const std::string& function, const std::string& filter) {
    const auto list = Instances(className, filter);
    if (list.empty()) return Report("no instance of " + className);
    const eng::Params p = eng::Call(list.front(), function.c_str());
    size_t size = 0;
    const uint8_t* ret = p.Return(&size);
    std::string result = ret ? " return " + Hex(ret, size) : "";
    if (ret && size == sizeof(eng::Obj))
        if (eng::Obj o = p.ReturnObj(); eng::IsLive(o)) result += " (" + eng::PathOf(o) + ")";
    Report("call " + eng::Describe(p.Fn()) + " on " + eng::PathOf(list.front()) + (p.Invoked() ? " ok" : " FAILED") + result);
}

// "callx <Class> <Function> [filter] | arg | arg ...": a call with arguments, for measuring the game.
//   i:5  f:1.5  d:1.5  u8:3  b:1  s:text (FString)  t:text (FText)  n:name (FName)  o:Class[,filter] (an object)
//   v:x,y,z (FVector)
// Every output parameter and the return value are reported (hex, and the object's path for pointers).
void CallWithArgs(const std::string& cmd) {
    std::vector<std::string> parts;
    for (size_t start = 0;;) {
        const size_t bar = cmd.find('|', start);
        std::string part = cmd.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
        while (!part.empty() && part.front() == ' ') part.erase(0, 1);
        while (!part.empty() && part.back() == ' ') part.pop_back();
        parts.push_back(part);
        if (bar == std::string::npos) break;
        start = bar + 1;
    }
    const std::vector<std::string> head = Words(parts[0]);
    auto word = [&](size_t i) { return i < head.size() ? head[i] : std::string(); };
    const auto list = Instances(word(1), word(3));
    if (list.empty()) return Report("no instance of " + word(1));
    eng::Obj target = list.front();
    eng::Params p(eng::FunctionOn(target, word(2).c_str()));
    std::vector<eng::Params> keepAlive;              // text and names built for arguments
    std::vector<std::wstring> strings;
    strings.reserve(parts.size());
    for (size_t i = 1; i < parts.size(); ++i) {
        const std::string& a = parts[i];
        const size_t colon = a.find(':');
        const std::string type = a.substr(0, colon), value = colon == std::string::npos ? "" : a.substr(colon + 1);
        const int index = static_cast<int>(i - 1);
        // A string, text, name or object written into a parameter of another kind is the same size but not the same
        // thing (a string written into an out FText hung the game once), so those kinds are checked.
        const auto params = eng::ParamsOf(p.Fn());
        std::vector<eng::ParamInfo> inputs;
        for (const auto& param : params)
            if (!param.isReturn) inputs.push_back(param);
        if (index >= static_cast<int>(inputs.size())) return Report("callx: more arguments than parameters");
        const std::string kind = eng::KindOf(eng::FindProp(p.Fn(), inputs[index].name));
        const std::string wanted = type == "s" ? "StrProperty" : type == "t" ? "TextProperty" : type == "n" ? "NameProperty" : "";
        if ((!wanted.empty() && kind != wanted) || (type == "o" && kind.find("Object") == std::string::npos && kind.find("Class") == std::string::npos))
            return Report("callx: argument " + std::to_string(i) + " (" + a + ") does not fit " + inputs[index].name + ", a " + kind);
        if (type == "i") p.SetArg(index, static_cast<int32_t>(std::atoi(value.c_str())));
        else if (type == "f") p.SetArg(index, static_cast<float>(std::atof(value.c_str())));
        else if (type == "d") p.SetArg(index, std::atof(value.c_str()));
        else if (type == "u8") p.SetArg(index, static_cast<uint8_t>(std::atoi(value.c_str())));
        else if (type == "b") p.SetArg(index, static_cast<uint8_t>(value == "1" || value == "true"));
        else if (type == "s" || type == "n") {
            strings.push_back(eng::Widen(value));
            const std::wstring& w = strings.back();
            const eng::FString fs{w.c_str(), static_cast<int32_t>(w.size() + 1), static_cast<int32_t>(w.size() + 1)};
            if (type == "s") {
                p.SetArg(index, fs);
            } else {
                keepAlive.push_back(eng::Call(eng::FindCdo("KismetStringLibrary"), "Conv_StringToName", fs));
                size_t size = 0;
                const uint8_t* name = keepAlive.back().Return(&size);
                if (name) p.SetArg(index, name, size);
            }
        } else if (type == "t") {
            keepAlive.push_back(eng::MakeText(value));
            size_t size = 0;
            const uint8_t* text = keepAlive.back().Return(&size);
            if (text) p.SetArg(index, text, size);
        } else if (type == "o") {
            const size_t comma = value.find(',');
            const auto objects = Instances(value.substr(0, comma), comma == std::string::npos ? "" : value.substr(comma + 1));
            p.SetArg(index, objects.empty() ? nullptr : objects.front());
        } else if (type == "v") {
            double v[3] = {0, 0, 0};
            std::sscanf(value.c_str(), "%lf,%lf,%lf", &v[0], &v[1], &v[2]);
            p.SetArg(index, v, sizeof v);
        } else {
            return Report("callx: unknown argument type '" + type + "'");
        }
    }
    const bool ok = eng::Invoke(target, p);
    std::string result;
    for (const auto& param : eng::ParamsOf(p.Fn())) {
        if (!param.isOut && !param.isReturn) continue;
        size_t size = 0;
        const uint8_t* bytes = p.Get(param.name.c_str(), &size);
        if (!bytes) continue;
        result += " " + param.name + "=" + Hex(bytes, size);
        eng::Obj o = nullptr;
        if (size == sizeof o) {
            std::memcpy(&o, bytes, sizeof o);
            if (eng::IsLive(o)) result += " (" + eng::PathOf(o) + ")";
        }
    }
    Report("callx " + eng::Describe(p.Fn()) + " on " + eng::PathOf(target) + (ok ? " ok" : " FAILED") + result);
}

// "materials [fragment]": loaded materials (UMaterial) whose blend mode is not opaque, with the mode, for finding one
// to reuse. EBlendMode: 0 opaque, 1 masked, 2 translucent, 3 additive, 4 modulate.
// "watch <leaderboard entry filter>": the game's replay camera follows that entry's ghost, which keeps playing in a
// race after "pov": BP_MyPlayerController.Start_FreeCam, then its AC_GhostFollowCam is given the entry's GhostKey
// (SetNativeGhostSource) and follows it. A long-running replay view for testing replay features.
void Watch(const std::string& filter) {
    const auto list = Instances("WBP_LeaderboardEntry_C", filter);
    if (list.empty()) return Report("no leaderboard entry matching " + filter);
    uint8_t key[96];
    bool hasKey = false;
    if (!eng::ReadBool(list.front(), "bHasGhostKey", &hasKey) || !hasKey || !eng::ReadBytes(list.front(), "GhostKey", key, sizeof key))
        return Report("watch: no ghost key");
    eng::Obj controller = game::PlayerController();
    eng::Call(controller, "Start_FreeCam");
    eng::Obj freeCam = eng::Call(controller, "GetViewTarget").ReturnObj();
    eng::Obj followCam = eng::ReadObj(freeCam, "AC_GhostFollowCam");
    eng::Obj ghosts = eng::Call(eng::FindCdo("SubsystemBlueprintLibrary"), "GetWorldSubsystem", controller,
                                eng::FindClass("BallestGhostWorldSubsystem")).ReturnObj();
    if (!followCam || !ghosts) return Report("watch: no follow camera or ghost subsystem");
    eng::Params source(eng::FunctionOn(followCam, "SetNativeGhostSource"));
    const bool ok = source.Set("Subsystem", ghosts) && source.Set("Key", key, sizeof key) && eng::Invoke(followCam, source);
    eng::Call(followCam, "SetGhostFollowEnabled", uint8_t{1});
    Report(std::string("watch -> ") + (ok && source.ReturnBool() ? "following" : "not following"));
}

// "objitem <Class> [filter]": the raw bytes of that object's entry in the global object array (FUObjectItem), for
// measuring the flag bits (root set and the like).
void ObjItem(const std::string& cls, const std::string& filter) {
    const auto list = Instances(cls, filter);
    if (list.empty()) return Report("no instance of " + cls);
    for (size_t n = 0; n < list.size() && n < 3; ++n) {
        eng::Obj o = list[n];
        int32_t index = -1;
        std::memcpy(&index, o + 0xC, 4);
        const uint8_t* item = eng::ItemOf(index);
        Report("objitem " + eng::PathOf(o) + " index " + std::to_string(index) + ": " + (item ? Hex(item, 24) : std::string("?")));
    }
}

void Materials(const std::string& fragment) {
    eng::Obj cls = eng::FindClass("Material");
    int shown = 0;
    eng::ForEachObject([&](eng::Obj o) {
        if (!eng::IsA(o, cls) || eng::IsDefaultObject(o)) return true;
        uint8_t mode = 0;
        if (!eng::ReadBytes(o, "BlendMode", &mode, 1) || mode == 0) return true;
        const std::string path = eng::PathOf(o);
        if (!fragment.empty() && path.find(fragment) == std::string::npos) return true;
        Report("material " + std::to_string(mode) + " " + path);
        return ++shown < 200;
    });
    Report("materials: " + std::to_string(shown) + " shown");
}

// The camera fade on each ball (the game's BallestCameraFadeComponent, from 2026-10-06): its settings and every mesh it
// has given fade materials, with the materials before and after.
void Fade(const std::string& filter) {
    auto names = [](const uint8_t* arr) {
        struct { eng::Obj* data; int32_t num, max; } h;
        std::memcpy(&h, arr, sizeof h);
        std::string out;
        for (int i = 0; i < h.num && i < 16; ++i) out += (i ? ", " : "") + (h.data[i] ? eng::PathOf(h.data[i]) : std::string("null"));
        return "[" + out + "]";
    };
    for (eng::Obj ball : Instances("BP_RollingBall_C", filter)) {
        eng::Obj fade = eng::ReadObj(ball, "CameraProximityFade");
        if (!fade) { Report("fade: " + eng::PathOf(ball) + " has none"); continue; }
        bool on = false;
        float start = 0, full = 0, pad = 0;
        eng::ReadBool(fade, "bFadeEnabled", &on);
        eng::ReadBytes(fade, "StartClearance", &start, 4);
        eng::ReadBytes(fade, "FullFadeClearance", &full, 4);
        eng::ReadBytes(fade, "VisualBoundsPadding", &pad, 4);
        struct { uint8_t* data; int32_t num, max; } snaps{};
        eng::ReadBytes(fade, "MaterialSnapshots", &snaps, sizeof snaps);
        Report("fade: " + eng::PathOf(ball) + " enabled " + std::to_string(on) + " start " + std::to_string(start) +
               " full " + std::to_string(full) + " padding " + std::to_string(pad) + " body " +
               eng::PathOf(eng::ReadObj(fade, "RadialBody")) + " snapshots " + std::to_string(snaps.num));
        for (int i = 0; i < snaps.num && i < 32; ++i) {
            const uint8_t* e = snaps.data + i * 0x48;
            eng::Obj mesh;
            std::memcpy(&mesh, e, sizeof mesh);
            std::string data;                   // the mesh's custom primitive data (floats)
            struct { float* data; int32_t num, max; } cpd{};
            if (mesh && eng::ReadBytes(mesh, "CustomPrimitiveData", &cpd, sizeof cpd))
                for (int k = 0; k < cpd.num && k < 8; ++k) data += " " + std::to_string(cpd.data[k]);
            Report("fade:   " + (mesh ? eng::PathOf(mesh) : std::string("null")) + " data" + data + " overrides " + names(e + 0x08) +
                   " originals " + names(e + 0x18) + " applied " + names(e + 0x28));
        }
    }
}

void ViewTarget() {
    eng::Obj controller = game::PlayerController();
    Report("controller " + eng::PathOf(controller) + " view target " + eng::PathOf(eng::Call(controller, "GetViewTarget").ReturnObj()));
}

std::string gSavedBall;                 // ballsave / ballload

using Args = std::vector<std::string>;

std::string Arg(const Args& a, size_t i) { return i < a.size() ? a[i] : std::string(); }

void Run(const std::string& cmd) {
    static const std::map<std::string, void (*)(const Args&, const std::string&)> commands = {
        {"state", [](const Args&, const std::string&) { Report("state " + ui::Status() + " | plugins: " + plugins::Summary()); }},
        {"click", [](const Args&, const std::string& c) { Report(c + (ui::SimulateClick(c.substr(6)) ? " -> ok" : " -> no such button")); }},
        {"select", [](const Args&, const std::string& c) {          // select <first option> <index> (the option may have spaces)
             const std::string rest = c.size() > 7 ? c.substr(7) : "";
             const size_t space = rest.find_last_of(' ');
             const std::string first = space == std::string::npos ? rest : rest.substr(0, space);
             const int index = space == std::string::npos ? 0 : std::atoi(rest.c_str() + space + 1);
             Report(c + (ui::SimulateSelect(first, index) ? " -> ok" : " -> no such dropdown"));
         }},
        {"slider", [](const Args& a, const std::string& c) {
             ui::SimulateSlider(static_cast<float>(std::atof(Arg(a, 1).c_str())));
             Report(c);
         }},
        {"submit", [](const Args&, const std::string& c) { Report(c + (ui::SimulateSubmit(c.substr(7)) ? " -> ok" : " -> no text input")); }},
        {"type", [](const Args&, const std::string& c) { Report(c + (ui::SimulateType(c.substr(5)) ? " -> ok" : " -> no text input")); }},
        {"press", [](const Args& a, const std::string& c) {
             input::Simulate(std::atoi(Arg(a, 1).c_str()));
             Report(c);
         }},
        {"post", [](const Args& a, const std::string& c) {       // post <vk> [ms]: a key pressed in the game's own window
             const int vk = std::atoi(Arg(a, 1).c_str());
             HWND w = static_cast<HWND>(game::WindowHandle());
             if (!w || vk <= 0 || vk > 255) return Report(c + " -> no game window or key");
             const LPARAM scan = static_cast<LPARAM>(MapVirtualKeyW(static_cast<UINT>(vk), 0)) << 16;
             PostMessageW(w, WM_KEYDOWN, static_cast<WPARAM>(vk), 1 | scan);
             gPostedUps.push_back({vk, GetTickCount64() + static_cast<ULONGLONG>(std::max(30, std::atoi(Arg(a, 2).c_str())))});
             Report(c + " -> ok");
         }},
        {"rowclick", [](const Args& a, const std::string& c) {    // rowclick <i>: a click on the leaderboard's extra row i
             leaderboard::TestClickRow(std::atoi(Arg(a, 1).c_str()));
             Report(c + " -> ok");
         }},
        {"rowsscroll", [](const Args& a, const std::string& c) {  // rowsscroll <offset>: the rows' list scrolled there
             leaderboard::TestScroll(static_cast<float>(std::atof(Arg(a, 1).c_str())));
             Report(c + " -> ok");
         }},
        {"rowstab", [](const Args&, const std::string& c) {      // rowstab: as if the leaderboard's rows button were clicked
             leaderboard::TestToggleTab();
             Report(c + " -> ok");
         }},
        {"rowpin", [](const Args& a, const std::string& c) {      // rowpin <i>: a click on the extra row i's rank (pin)
             leaderboard::TestPinRow(std::atoi(Arg(a, 1).c_str()));
             Report(c + " -> ok");
         }},
        {"clickat", [](const Args& a, const std::string& c) {    // clickat <x> <y>: a left click posted to the game's window
             HWND w = static_cast<HWND>(game::WindowHandle());       // (client pixels); the real mouse isn't moved
             if (!w) return Report(c + " -> no game window");
             const LPARAM at = MAKELPARAM(std::atoi(Arg(a, 1).c_str()), std::atoi(Arg(a, 2).c_str()));
             PostMessageW(w, WM_MOUSEMOVE, 0, at);
             PostMessageW(w, WM_LBUTTONDOWN, MK_LBUTTON, at);
             PostMessageW(w, WM_LBUTTONUP, 0, at);
             Report(c + " -> ok");
         }},
        {"hold", [](const Args& a, const std::string& c) {       // hold <vk> 1|0: a key or button held, for posted clicks
             input::SimulateHeld(std::atoi(Arg(a, 1).c_str()), Arg(a, 2) != "0");
             Report(c);
         }},
        {"fakereplay", [](const Args& a, const std::string&) { replay::Simulate(Arg(a, 1) == "on", a.size() > 2 ? std::atof(Arg(a, 2).c_str()) : 30); }},
        {"replaytime", [](const Args&, const std::string&) {
             Report("replay time " + std::to_string(replay::Time()) + " of " + std::to_string(replay::Length()));
         }},
        {"install", [](const Args& a, const std::string& c) {
             registry::Install(Arg(a, 1));
             Report(c + " -> " + (registry::Pending(Arg(a, 1)).empty() ? "nothing to do" : registry::Pending(Arg(a, 1))));
         }},
        {"remove", [](const Args& a, const std::string& c) {
             registry::Remove(Arg(a, 1));
             Report(c + " -> " + (registry::Pending(Arg(a, 1)).empty() ? "nothing to do" : registry::Pending(Arg(a, 1))));
         }},
        {"setting", [](const Args& a, const std::string& c) {
             const auto& list = settings::List();
             for (size_t i = 0; i < list.size(); ++i)
                 if (list[i].pluginId == Arg(a, 1) && list[i].variable == Arg(a, 2)) {
                     // the value is the rest of the line (it may have spaces)
                     const size_t at = c.find(Arg(a, 2)) + Arg(a, 2).size() + 1;
                     const std::string value = at < c.size() ? c.substr(at) : "";
                     return Report(c + (settings::Set(i, value) ? " -> " + settings::Get(i) : " -> not a value"));
                 }
             Report(c + " -> no such setting");
         }},
        {"editor", [](const Args& a, const std::string&) {
             if (Arg(a, 1) == "rotatecontext") editor::ForceRotateContext(Arg(a, 2) == "on");
             if (Arg(a, 1) == "pieces") return Report(editor::PiecesStatus());
             if (Arg(a, 1) == "budget") {                   // editor budget [limit]: used of limit, and set the limit
                 if (a.size() > 2) editor::SetBudgetLimit(-2, std::atoi(Arg(a, 2).c_str()));
                 return Report("editor budget " + std::to_string(editor::BudgetUsed()) + " of " + std::to_string(editor::BudgetLimit()));
             }
             if (Arg(a, 1) == "screen") {                   // editor screen <id>: where the piece's middle is, in window pixels
                 double x = 0, y = 0;
                 const bool on = editor::ScreenPosition(std::atoi(Arg(a, 2).c_str()), &x, &y);
                 const float scale = eng::Call(eng::FindCdo("WidgetLayoutLibrary"), "GetViewportScale", game::PlayerController()).ReturnAs<float>(0.0f);
                 char buf[128];
                 std::snprintf(buf, sizeof buf, "editor screen %s: %s %.0f,%.0f", Arg(a, 2).c_str(), on ? "on screen" : "off screen", x * scale, y * scale);
                 return Report(buf);
             }
             if (Arg(a, 1) == "call") Report(Arg(a, 2) + (editor::CallHandler(Arg(a, 2)) ? " -> ok" : " -> failed"));
             if (Arg(a, 1) == "clicks") {
                 editor::SetWatchingClicks(Arg(a, 2) != "off");
                 Report(std::string("editor clicks ") + (Arg(a, 2) != "off" ? "watched" : "left to the game"));
             }
             if (Arg(a, 1) == "select") {                   // editor select <id>,<id>,...
                 std::vector<int> ids;
                 const std::string list = Arg(a, 2);
                 for (size_t i = 0; i < list.size(); i = list.find(',', i) == std::string::npos ? list.size() : list.find(',', i) + 1)
                     ids.push_back(std::atoi(list.c_str() + i));
                 editor::Select(ids);
             }
             Report(editor::Status());
         }},
        {"openworkshop", [](const Args& a, const std::string& c) {  // openworkshop <published file id>: as the host's Tracks API
             Report(c + (tracks::OpenWorkshop(std::strtoull(Arg(a, 1).c_str(), nullptr, 10)) ? " -> started" : " -> failed: " + tracks::OpenState()));
         }},
        {"teleport", [](const Args& a, const std::string& c) {       // teleport <x> <y> <z>: the ball being played
             Report(c + (race::MoveBall(std::atof(Arg(a, 1).c_str()), std::atof(Arg(a, 2).c_str()), std::atof(Arg(a, 3).c_str()))
                             ? " -> ok" : " -> no ball"));
         }},
        {"fps", [](const Args&, const std::string& c) {            // fps: the average frame rate since the last "fps"
             const double now = game::Seconds();
             if (gFpsSince >= 0 && now > gFpsSince)
                 Report(c + " -> " + std::to_string(static_cast<double>(gHostFrames - gFpsFrames) / (now - gFpsSince)) + " frames a second over " +
                        std::to_string(now - gFpsSince) + " s");
             else
                 Report(c + " -> counting from now");
             gFpsSince = now;
             gFpsFrames = gHostFrames;
         }},
        {"pp", [](const Args& a, const std::string& c) {           // pp <setting> <x> [y z w]: a post-process setting (test filter)
             std::string error;
             const bool ok = postprocess::Set(-2, Arg(a, 1), std::atof(Arg(a, 2).c_str()), std::atof(Arg(a, 3).c_str()),
                                              std::atof(Arg(a, 4).c_str()), a.size() > 5 ? std::atof(Arg(a, 5).c_str()) : 1.0, &error);
             Report(c + (ok ? " -> ok" : " -> " + error));
         }},
        {"ppweight", [](const Args& a, const std::string& c) {     // ppweight <0..1>: the test filter's strength; ppclear: none
             Report(c + (postprocess::Weight(-2, std::atof(Arg(a, 1).c_str())) ? " -> ok" : " -> failed"));
         }},
        {"ppclear", [](const Args&, const std::string& c) {
             postprocess::Clear(-2);
             Report(c + " -> ok");
         }},
        {"console", [](const Args&, const std::string& c) {        // console <command>: an engine console command (t.MaxFPS 30)
             const std::wstring command = eng::Widen(c.size() > 8 ? c.substr(8) : "");
             eng::Obj lib = eng::FindCdo("KismetSystemLibrary");
             eng::Params p(eng::FunctionOn(lib, "ExecuteConsoleCommand"));
             p.Set("WorldContextObject", game::PlayerController());
             p.Set("Command", eng::FString{command.c_str(), static_cast<int32_t>(command.size() + 1), static_cast<int32_t>(command.size() + 1)});
             Report(c + (eng::Invoke(lib, p) ? " -> ok" : " -> failed"));
         }},
        {"spin", [](const Args& a, const std::string& c) {         // spin <x> <y> <z>: the ball's spin (degrees a second)
             eng::Obj pawn = eng::Call(game::PlayerController(), "K2_GetPawn").ReturnObj();
             eng::Obj sphere = pawn ? eng::ReadObj(pawn, "Sphere") : nullptr;
             if (!sphere) return Report(c + " -> no ball");
             struct V { double x, y, z; } v{std::atof(Arg(a, 1).c_str()), std::atof(Arg(a, 2).c_str()), std::atof(Arg(a, 3).c_str())};
             eng::Params p(eng::FunctionOn(sphere, "SetPhysicsAngularVelocityInDegrees"));
             p.Set("NewAngVel", v);
             p.Set("bAddToCurrent", uint8_t{0});
             Report(c + (eng::Invoke(sphere, p) ? " -> ok" : " -> failed"));
         }},
        {"playsound", [](const Args&, const std::string& c) {      // playsound <file.wav or game sound path>: Draw::Sound
             Report(c + (draw::Sound(c.size() > 10 ? c.substr(10) : "", 1, 1) ? " -> ok" : " -> failed"));
         }},
        {"soundstate", [](const Args&, const std::string& c) {     // soundstate: whether the last custom sound is playing
             Report(c + " -> " + draw::LastSoundState());
         }},
        {"fling", [](const Args& a, const std::string& c) {        // fling <vx> <vy> <vz>: the ball's velocity (cm/s), as a hit gives it
             eng::Obj pawn = eng::Call(game::PlayerController(), "K2_GetPawn").ReturnObj();
             eng::Obj sphere = pawn ? eng::ReadObj(pawn, "Sphere") : nullptr;
             if (!sphere) return Report(c + " -> no ball");
             struct V { double x, y, z; } v{std::atof(Arg(a, 1).c_str()), std::atof(Arg(a, 2).c_str()), std::atof(Arg(a, 3).c_str())};
             eng::Params p(eng::FunctionOn(sphere, "SetPhysicsLinearVelocity"));
             p.Set("NewVel", v);
             p.Set("bAddToCurrent", uint8_t{0});
             Report(c + (eng::Invoke(sphere, p) ? " -> ok" : " -> failed"));
         }},
        {"checkpoints", [](const Args&, const std::string&) {      // the map's checkpoints, the current one, the counters
             Report("checkpoints " + std::to_string(race::CheckpointCount()) + ", current " + std::to_string(race::CurrentCheckpoint()) +
                    " | restarts " + std::to_string(race::Restarts()) + " respawns " + std::to_string(race::Respawns()) +
                    " falls " + std::to_string(race::Falls()));
             for (int i = 0; i < race::CheckpointCount(); ++i) {
                 double x = 0, y = 0, z = 0;
                 double tx = 0, ty = 0, tz = 0;
                 race::CheckpointPosition(i, &x, &y, &z);
                 const bool trigger = race::CheckpointTrigger(i, &tx, &ty, &tz);
                 char line[160];
                 std::snprintf(line, sizeof line, "  %d at %.0f %.0f %.0f, trigger %s%.0f %.0f %.0f", i, x, y, z, trigger ? "" : "(none) ", tx, ty, tz);
                 Report(line);
             }
         }},
        {"openstate", [](const Args&, const std::string&) { Report("openstate " + tracks::OpenState()); }},
        {"open", [](const Args& a, const std::string& c) { Report(c + (game::OpenLevel(Arg(a, 1)) ? " -> ok" : " -> failed")); }},
        {"functions", [](const Args& a, const std::string&) { Functions(Arg(a, 1)); }},
        {"instances", [](const Args& a, const std::string&) {
             for (eng::Obj o : Instances(Arg(a, 1), Arg(a, 2))) Report("instance " + eng::PathOf(o));
         }},
        {"props", [](const Args& a, const std::string&) { Props(Arg(a, 1), Arg(a, 2)); }},
        {"find", [](const Args& a, const std::string&) { Find(Arg(a, 1)); }},
        {"struct", [](const Args& a, const std::string&) { Struct(Arg(a, 1), Arg(a, 2)); }},
        {"call", [](const Args& a, const std::string&) { CallNoArgs(Arg(a, 1), Arg(a, 2), Arg(a, 3)); }},
        {"callx", [](const Args&, const std::string& c) { CallWithArgs(c); }},
        {"viewtarget", [](const Args&, const std::string&) { ViewTarget(); }},
        {"pov", [](const Args& a, const std::string&) { Pov(Arg(a, 1)); }},
        {"fade", [](const Args& a, const std::string&) { Fade(Arg(a, 1)); }},
        {"setfloat", [](const Args& a, const std::string& c) {    // setfloat <Class> <filter> <property> <value>: on each
             const float v = std::strtof(Arg(a, 4).c_str(), nullptr);
             int n = 0;
             for (eng::Obj o : Instances(Arg(a, 1), Arg(a, 2))) n += eng::WriteBytes(o, Arg(a, 3), &v, sizeof v) ? 1 : 0;
             Report(c + " -> " + std::to_string(n) + " set");
         }},
        {"materials", [](const Args& a, const std::string&) { Materials(Arg(a, 1)); }},
        {"watch", [](const Args& a, const std::string&) { Watch(Arg(a, 1)); }},
        {"objitem", [](const Args& a, const std::string&) { ObjItem(Arg(a, 1), Arg(a, 2)); }},
        {"loadglass", [](const Args&, const std::string&) { Report(replay::TestLoadGlass()); }},
        {"crash", [](const Args& a, const std::string&) {
             if (a.size() > 1) return plugins::CrashNext(Arg(a, 1));     // "crash <plugin id>": in that plugin's next call
             Report("crash: faulting in host code now");
             *static_cast<volatile int*>(nullptr) = 1;
         }},
        {"cosmetic", [](const Args& a, const std::string&) {
             const std::string kind = Arg(a, 1), id = Arg(a, 2), what = Arg(a, 3);
             const bool ok = kind == "ball"  ? cosmetics::AddBall(id, id, eng::Widen(what), L"")
                             : kind == "hat" ? cosmetics::AddHat(id, id, what, a.size() > 4 ? std::atof(Arg(a, 4).c_str()) : 1, L"")
                                             : cosmetics::AddBfx(id, id, what, std::atof(Arg(a, 4).c_str()), L"");
             Report("cosmetic " + kind + " " + id + (ok ? " -> ok" : " -> failed"));
         }},
        {"cosmetics", [](const Args&, const std::string&) { Report(cosmetics::Status()); }},
        {"wear", [](const Args& a, const std::string& c) {        // wear ball|hat|bfx <custom id> ("" for the game's own)
             const cosmetics::Kind kind = Arg(a, 1) == "hat" ? cosmetics::Kind::Hat : Arg(a, 1) == "bfx" ? cosmetics::Kind::Bfx : cosmetics::Kind::Ball;
             Report(c + (cosmetics::Equip(kind, Arg(a, 2)) ? " -> ok" : " -> failed"));
         }},
        {"cosmodel", [](const Args& a, const std::string& c) {    // cosmodel ball|hat <id> <model file> [ball image]
             std::string text;
             if (FILE* f = _wfopen(eng::Widen(Arg(a, 3)).c_str(), L"rb")) {
                 char buf[4096];
                 for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
                 fclose(f);
             }
             const bool ok = Arg(a, 1) == "ball" ? cosmetics::AddBall(Arg(a, 2), Arg(a, 2), eng::Widen(Arg(a, 4)), L"", text)
                                                 : cosmetics::AddHat(Arg(a, 2), Arg(a, 2), "", 1, L"", text);
             Report(c + (ok ? " -> ok" : " -> failed"));
         }},
        {"cosmetictile", [](const Args& a, const std::string& c) {
             Report(c + (cosmetics::ClickTile(std::atoi(Arg(a, 1).c_str())) ? " -> ok" : " -> failed"));
         }},
        {"cosmode", [](const Args& a, const std::string& c) {      // cosmode public|local: the Customize page's mode
             Report(c + (cosmetics::SetLocalMode(Arg(a, 1) == "local") ? " -> ok" : " -> no page"));
         }},
        {"extratab", [](const Args& a, const std::string& c) {     // extratab <slot> | extratab -: an extra's tab, or the page's own
             Report(c + (cosmetics::ShowExtraTab(Arg(a, 1) == "-" ? "" : Arg(a, 1)) ? " -> ok" : " -> failed"));
         }},
        {"wearextra", [](const Args& a, const std::string& c) {    // wearextra <slot> <id> ("-" for none)
             Report(c + (cosmetics::EquipExtra(Arg(a, 1), Arg(a, 2) == "-" ? "" : Arg(a, 2)) ? " -> ok" : " -> failed"));
         }},
        {"extratile", [](const Args& a, const std::string& c) {    // extratile <n>: presses a tile on the shown extra tab
             Report(c + (cosmetics::ClickExtraTile(std::atoi(Arg(a, 1).c_str())) ? " -> ok" : " -> failed"));
         }},
        {"children", [](const Args& a, const std::string&) {       // children <owner class> <property>: a panel's children
             const auto owners = Instances(Arg(a, 1), "Transient");
             if (owners.empty()) return Report("children: no " + Arg(a, 1));
             eng::Obj panel = eng::ReadObj(owners.front(), Arg(a, 2));
             const int32_t n = eng::Call(panel, "GetChildrenCount").ReturnAs<int32_t>(-1);
             Report("children of " + eng::PathOf(panel) + ": " + std::to_string(n));
             for (int32_t i = 0; i < n && i < 40; ++i) {
                 eng::Obj c = eng::Call(panel, "GetChildAt", i).ReturnObj();
                 Report("  " + std::to_string(i) + " " + eng::ObjName(eng::ClassOf(c)) + " " + eng::PathOf(c));
             }
         }},
        {"loadasset", [](const Args& a, const std::string&) {
             eng::Obj o = cosmetics::LoadAsset(eng::Widen(Arg(a, 1)));
             Report("loadasset " + Arg(a, 1) + " -> " + (o ? eng::PathOf(o) : std::string("null")));
         }},
        {"fnbytes", [](const Args& a, const std::string&) {       // a UFunction's words; exe pointers marked
             eng::Obj fn = eng::FindFunction(eng::FindClass(Arg(a, 1)), Arg(a, 2));
             if (!fn) return Report("fnbytes: no function");
             std::string line;
             for (int off = 0; off < 0x100; off += 8) {
                 uint64_t v = 0;
                 std::memcpy(&v, fn + off, 8);
                 line += hostlog::Hex(off) + "=" + hostlog::Hex(v) + (eng::InImage(reinterpret_cast<void*>(v)) ? "* " : " ");
             }
             Report("fnbytes " + Arg(a, 2) + " base " + hostlog::Hex(eng::Base()) + ": " + line);
         }},
        {"glasslook", [](const Args& a, const std::string& c) {     // glasslook <lean 0|1> <lead 0|1>
             models::TuneGlassProbeLook(Arg(a, 1) != "0", Arg(a, 2) != "0");
             Report(c + " -> ok");
         }},
        {"glassrate", [](const Args& a, const std::string& c) {     // glassrate <seconds between captures> [face pixels]
             models::TuneGlassProbe(std::atof(Arg(a, 1).c_str()), std::atoi(Arg(a, 2).c_str()));
             Report(c + " -> ok");
         }},
        {"glassprobe", [](const Args& a, const std::string& c) {    // glassprobe on|off: allow live reflections (default on)
             models::SetGlassProbe(Arg(a, 1) == "on");
             Report(c + " -> ok");
         }},
        {"glassparent", [](const Args& a, const std::string& c) {   // glassparent <material path>|default: then re-wear the ball
             models::SetRefractingGlassParent(Arg(a, 1) == "default" ? "" : Arg(a, 1));
             Report(c + " -> ok");
         }},
        {"glasstex", [](const Args& a, const std::string& c) {    // glasstex <asset path> <parameter...>: live glass texture
             std::string name;
             for (size_t i = 2; i < a.size(); ++i) name += (name.empty() ? "" : " ") + a[i];
             Report(c + " -> " + std::to_string(models::TuneRefractingGlassTexture(name, Arg(a, 1))) + " material(s)");
         }},
        {"glassset", [](const Args& a, const std::string& c) {    // glassset <parameter> <value> [g b a]: live refracting glass
             // The parameter name can hold spaces ("[Glass] Refraction"): everything before the first number.
             std::string name;
             std::vector<float> values;
             for (size_t i = 1; i < a.size(); ++i) {
                 char* end = nullptr;
                 const float v = std::strtof(a[i].c_str(), &end);
                 if (end && *end == 0 && !a[i].empty()) values.push_back(v);
                 else name += (name.empty() ? "" : " ") + a[i];
             }
             Report(c + " -> " + std::to_string(models::TuneRefractingGlass(name, values)) + " material(s)");
         }},
        {"matparams", [](const Args& a, const std::string&) {    // matparams <material>: its parameter names and values
             eng::Obj m = cosmetics::LoadAsset(eng::Widen(Arg(a, 1)));
             if (!m) m = eng::FindObjectByName(Arg(a, 1));          // a material made at runtime, by its name
             if (!m) return Report("matparams: not loaded");
             // Entry sizes, measured: FScalarParameterValue 36, FVectorParameterValue 48, FTextureParameterValue 40.
             for (const auto& [list, entrySize] : {std::pair{"ScalarParameterValues", 36}, std::pair{"VectorParameterValues", 48},
                                                   std::pair{"TextureParameterValues", 40}}) {
                 struct { uint8_t* data; int32_t num, max; } arr{};
                 const eng::Prop prop = eng::FindProp(eng::ClassOf(m), list);
                 if (!prop || !eng::ReadBytes(m, list, &arr, sizeof arr)) continue;
                 const int stride = entrySize;
                 std::string line = std::string(list) + " (" + std::to_string(arr.num) + "):";
                 for (int i = 0; i < arr.num && stride; ++i) {
                     uint32_t ci = 0; int32_t num = 0;
                     std::memcpy(&ci, arr.data + i * stride, 4); std::memcpy(&num, arr.data + i * stride + 4, 4);
                     line += " " + eng::Name(ci, num) + "=" + Hex(arr.data + i * stride + 16, 16);
                 }
                 Report(line);
             }
         }},
        {"spawnfx", [](const Args& a, const std::string& c) {     // spawnfx <niagara system path> <height above the ball> [scale]
             eng::Obj pawn = eng::Call(game::PlayerController(), "K2_GetPawn").ReturnObj();
             eng::Obj system = cosmetics::LoadAsset(eng::Widen(Arg(a, 1)));
             if (!pawn || !system) return Report(c + " -> no pawn or system");
             struct V { double x, y, z; };
             V at = eng::Call(pawn, "K2_GetActorLocation").ReturnAs<V>();
             at.z += std::atof(Arg(a, 2).c_str());
             eng::Obj lib = eng::FindCdo("NiagaraFunctionLibrary");
             eng::Params p(eng::FunctionOn(lib, "SpawnSystemAtLocation"));
             const double k = a.size() > 3 ? std::atof(Arg(a, 3).c_str()) : 1;
             const V zero{0, 0, 0}, one{k, k, k};
             p.Set("WorldContextObject", pawn);
             p.Set("SystemTemplate", system);
             p.Set("Location", at);
             p.Set("Rotation", zero);
             p.Set("Scale", one);
             p.Set("bAutoDestroy", uint8_t{1});
             p.Set("bAutoActivate", uint8_t{1});
             eng::Invoke(lib, p);
             Report(c + (p.ReturnObj() ? " -> spawned" : " -> failed"));
         }},
        {"openmap", [](const Args& a, const std::string& c) {     // openmap <file name fragment>: Create page entry -> editor
             for (eng::Obj o : Instances("WBP_TrackLibraryEntry_C", "Transient")) {
                 const eng::Prop p = eng::FindProp(eng::ClassOf(o), "AssociatedPath");
                 if (!p || eng::ReadFString(o + p.offset).find(Arg(a, 1)) == std::string::npos) continue;
                 return Report(c + (eng::Call(o, "LoadMapForEdit").Invoked() ? " -> ok" : " -> failed"));
             }
             Report(c + " -> no such map on the Create page");
         }},
        {"sandbox", [](const Args&, const std::string&) { Report("sandbox: " + sandbox::Status()); }},
        {"sandboxtest", [](const Args&, const std::string&) { Report("sandbox test: " + sandbox::SelfTest()); }},
        {"hubopen", [](const Args&, const std::string& c) {        // the play page, then its hub tab (WBP_0_Play tab 2)
             for (eng::Obj manager : Instances("WBP_MainMenu_UIManager_C", "Transient")) {
                 eng::Call(manager, "DoPlay");
                 eng::Obj play = eng::ReadObj(manager, "WBP_Play_1");
                 const int32_t hubTab = 2;          // SwitchToPageMatchingTabIndex: 2 shows Body_WS 1 (the hub)
                 if (!play || !eng::WriteBytes(play, "ActiveTabIndex", &hubTab, sizeof hubTab)) return Report(c + " -> no play page");
                 return Report(c + (eng::Call(play, "SwitchToPageMatchingTabIndex").Invoked() ? " -> ok" : " -> failed"));
             }
             Report(c + " -> no main menu");
         }},
        {"hubtag", [](const Args& a, const std::string& c) {      // hubtag <i>: picks the hub home's i-th filter tag, as
             // its button does (WBP_HubHomeFilterGroup: UIManager.AddOrRemoveTagFilter(false, false, tag))
             const auto groups = Instances("WBP_HubHomeFilterGroup_C", "Transient");
             const auto managers = Instances("WBP_MainMenu_UIManager_C", "Transient");
             if (groups.empty() || managers.empty()) return Report(c + " -> no hub filter group or menu");
             uint8_t array[16] = {};
             if (!eng::ReadBytes(groups.front(), "Tags", array, sizeof array)) return Report(c + " -> no tags");
             const uint8_t* data = nullptr;
             int32_t num = 0;
             std::memcpy(&data, array, sizeof data);
             std::memcpy(&num, array + 8, sizeof num);
             const int i = std::atoi(Arg(a, 1).c_str());
             if (!data || i < 0 || i >= num) return Report(c + " -> only " + std::to_string(num) + " tags");
             uint32_t name[2] = {};
             std::memcpy(name, data + i * 8, sizeof name);
             eng::Params p(eng::FunctionOn(managers.front(), "AddOrRemoveTagFilter"));
             p.SetArg(0, uint8_t{0});
             p.SetArg(1, uint8_t{0});
             p.SetArg(2, data + i * 8, 8);
             const bool ok = eng::Invoke(managers.front(), p);
             Report(c + " -> " + eng::Name(name[0], static_cast<int32_t>(name[1])) + (ok ? " picked" : " failed"));
         }},
        {"race", [](const Args&, const std::string&) {             // the race: run, track, practice, input
             const auto& t = race::CurrentTrack();
             double x = 0, y = 0;
             bool jump = false;
             race::Input(&x, &y, &jump);
             char buf[512];
             std::snprintf(buf, sizeof buf, "race: on track %d active %d complete %d run %d restarts %d practice %d paused %d | key %s | name %s | author %s | author time %.3f custom %d | input %.2f,%.2f jump %d",
                           race::OnTrack(), race::Active(), race::Complete(), race::RunId(), race::Restarts(), race::Practice(), race::Paused(),
                           t.key.c_str(), t.name.c_str(), t.author.c_str(), t.authorTime, t.custom, x, y, jump);
             Report(buf);
         }},
        {"ballsave", [](const Args&, const std::string&) {
             gSavedBall = race::SaveBall();
             std::string flat = gSavedBall;
             for (char& c : flat)
                 if (c == '\n') c = '|';
             Report("ballsave: " + flat);
         }},
        {"ballload", [](const Args& a, const std::string& c) { Report(c + (race::LoadBall(gSavedBall, Arg(a, 1) != "0") ? " -> ok" : " -> failed")); }},
        {"practice", [](const Args&, const std::string&) { race::StartPractice(); Report(std::string("practice ") + (race::Practice() ? "on" : "off")); }},
        {"pause", [](const Args& a, const std::string& c) { Report(c + (race::SetPaused(Arg(a, 1) == "1") ? " -> ok" : " -> failed")); }},
        {"hud", [](const Args&, const std::string&) {              // the HUD's elements
             for (const auto& el : hud::Elements())
                 Report("hud: " + el.key + " (" + el.className + ")" + (el.shown ? " shown" : el.parentShown ? " hidden for now" : " hidden") +
                        (el.label.empty() ? "" : " \"" + el.label + "\"") + " opacity " + std::to_string(el.opacity).substr(0, 4));
         }},
        {"widgetpath", [](const Args& a, const std::string&) {    // widgetpath <Class> <filter>: each instance's parents up its tree
             for (eng::Obj o : Instances(Arg(a, 1), Arg(a, 2))) {
                 std::string line = eng::ObjName(o);
                 eng::Obj x = o;
                 for (int i = 0; i < 30 && x; ++i) {
                     eng::Obj parent = eng::Call(x, "GetParent").ReturnObj();
                     if (!parent) {
                         parent = eng::OuterOf(eng::OuterOf(x));
                         line += " <tree of> ";
                     } else {
                         line += " < ";
                     }
                     x = parent;
                     if (x) line += eng::ObjName(x) + "(" + eng::ObjName(eng::ClassOf(x)) + ")";
                     if (x && !eng::FindFunction(eng::ClassOf(x), "GetParent")) break;
                 }
                 Report("widgetpath " + line);
             }
         }},
        {"hudset", [](const Args& a, const std::string& c) {       // hudset <key> <x> <y> <scale> <mode 0/1/2> | hudset <key> clear
             if (Arg(a, 2) == "clear") hud::ClearLayout(Arg(a, 1));
             else hud::SetLayout(Arg(a, 1), std::atof(Arg(a, 2).c_str()), std::atof(Arg(a, 3).c_str()), std::atof(Arg(a, 4).c_str()), std::atoi(Arg(a, 5).c_str()));
             Report(c + " -> ok");
         }},
        {"hudtint", [](const Args& a, const std::string& c) {      // hudtint <key> <part> <r> <g> <b> <a> | hudtint <key> <part> reset
             if (Arg(a, 3) == "reset") { hud::ResetPartColor(Arg(a, 1), Arg(a, 2)); return Report(c + " -> reset"); }
             Report(c + (hud::SetPartColor(Arg(a, 1), Arg(a, 2), static_cast<float>(std::atof(Arg(a, 3).c_str())), static_cast<float>(std::atof(Arg(a, 4).c_str())),
                                           static_cast<float>(std::atof(Arg(a, 5).c_str())), static_cast<float>(std::atof(Arg(a, 6).c_str()))) ? " -> ok" : " -> no such part"));
         }},
        {"strprop", [](const Args& a, const std::string&) {       // strprop <Class> <filter> <property>: an FString of each
             for (eng::Obj o : Instances(Arg(a, 1), Arg(a, 2))) {
                 const eng::Prop p = eng::FindProp(eng::ClassOf(o), Arg(a, 3));
                 Report(eng::PathOf(o) + " " + Arg(a, 3) + " = " + (p ? eng::ReadFString(o + p.offset) : std::string("(no property)")));
             }
         }},
        {"objprop", [](const Args& a, const std::string&) {       // objprop <Class> <filter> <property>: an object of each
             std::string prop = Arg(a, 3);                           // the rest of the line: a name may have spaces
             for (size_t i = 4; i < a.size(); ++i) prop += " " + Arg(a, i);
             for (eng::Obj o : Instances(Arg(a, 1), Arg(a, 2))) {
                 eng::Obj v = eng::ReadObj(o, prop);
                 Report(eng::ObjName(o) + " " + prop + " = " + (v ? eng::ObjName(eng::ClassOf(v)) + " " + eng::PathOf(v) : std::string("null")));
             }
         }},
        {"listprop", [](const Args& a, const std::string&) {      // listprop <Class> <filter> <property>: strings or floats
             for (eng::Obj o : Instances(Arg(a, 1), Arg(a, 2))) {
                 struct { uint8_t* data; int32_t num, max; } arr{};
                 if (!eng::ReadBytes(o, Arg(a, 3), &arr, sizeof arr) || !arr.data || arr.num <= 0 || arr.num > 64) {
                     Report(eng::ObjName(o) + " " + Arg(a, 3) + ": empty");
                     continue;
                 }
                 // An FString array starts with a character pointer (8-aligned, not tiny); anything else is read as floats,
                 // which never reads past the elements (4 bytes each at least).
                 uint64_t first = 0;
                 std::memcpy(&first, arr.data, 4);
                 std::string line = eng::ObjName(o) + " " + Arg(a, 3) + " (" + std::to_string(arr.num) + "):";
                 float f[64];
                 std::memcpy(f, arr.data, static_cast<size_t>(arr.num) * 4);
                 for (int i = 0; i < arr.num; ++i) line += " " + std::to_string(f[i]);
                 Report(line + " (as floats)");
                 if (arr.max >= arr.num && (reinterpret_cast<uintptr_t>(arr.data) % 8) == 0) {
                     uint64_t p0 = 0;
                     std::memcpy(&p0, arr.data, 8);
                     if (p0 > 0x100000 && p0 % 2 == 0 && p0 < 0x7fffffffffffull) {
                         std::string strings;
                         for (int i = 0; i < arr.num; ++i) strings += " [" + eng::ReadFString(arr.data + i * 16) + "]";
                         Report(eng::ObjName(o) + " as strings:" + strings);
                     }
                 }
             }
         }},
        {"dumptypes", [](const Args&, const std::string&) {      // every type: types\\Ballest.usmap + types.txt
             Report(eng::DumpTypes(hostlog::DataDir() + L"\\types"));
         }},
        {"objbytes", [](const Args& a, const std::string&) {       // objbytes <object name> <bytes>: raw memory, 8 per group
             eng::Obj o = eng::FindObjectByName(Arg(a, 1));
             if (!o) return Report("objbytes: none named " + Arg(a, 1));
             const int bytes = std::min(std::max(std::atoi(Arg(a, 2).c_str()), 8), 0x100);
             std::string line = eng::ObjName(eng::ClassOf(o)) + " " + eng::PathOf(o) + ":";
             for (int off = 0; off < bytes; off += 8) {
                 uint64_t v = 0;
                 std::memcpy(&v, o + off, 8);
                 line += " +" + hostlog::Hex(off) + "=" + hostlog::Hex(v);
             }
             Report(line);
         }},
        {"membytes", [](const Args& a, const std::string&) {       // membytes <hex address> <bytes>: raw memory if readable
             const uintptr_t address = std::strtoull(Arg(a, 1).c_str(), nullptr, 16);
             const int bytes = std::min(std::max(std::atoi(Arg(a, 2).c_str()), 8), 0x200);
             uint8_t buf[0x200];
             if (!eng::ReadMemory(address, buf, static_cast<size_t>(bytes))) return Report("membytes: not readable");
             std::string line = "membytes " + Arg(a, 1) + ":";
             for (int off = 0; off < bytes; off += 8) {
                 uint64_t v = 0;
                 std::memcpy(&v, buf + off, 8);
                 line += " +" + hostlog::Hex(off) + "=" + hostlog::Hex(v);
             }
             Report(line);
         }},
        {"objarray", [](const Args& a, const std::string&) {     // objarray <Class> <filter> <property>: a TArray<UObject*>
             const auto list = Instances(Arg(a, 1), Arg(a, 2));
             if (list.empty()) return Report("objarray: no " + Arg(a, 1));
             int i = 0;
             for (eng::Obj o : eng::ReadObjArray(list.front(), Arg(a, 3)))
                 Report("  [" + std::to_string(i++) + "] " + (eng::IsLive(o) ? eng::PathOf(o) : std::string("?")));
         }},
        {"enable", [](const Args& a, const std::string& c) {       // enable <plugin id> 0|1: turn a plugin off or on
             Report(c + (plugins::SetEnabled(Arg(a, 1), Arg(a, 2) == "1") ? " -> ok" : " -> nothing to do"));
         }},
        {"ballstate", [](const Args&, const std::string&) {        // the racing balls: skin actor, sphere, mesh, material
             eng::Obj cls = eng::FindClass("BP_RollingBall_C");
             eng::ForEachObject([&](eng::Obj o) {
                 if (eng::ClassOf(o) != cls || eng::IsDefaultObject(o)) return true;
                 eng::Obj skin = eng::ReadObj(o, "CustomSkinChild"), sphere = eng::ReadObj(o, "Sphere");
                 bool hidden = false;
                 if (skin && eng::IsLive(skin)) eng::ReadBool(skin, "bHidden", &hidden);
                 struct V { double x, y, z; };
                 const V v = eng::Call(o, "GetVelocity").ReturnAs<V>();
                 bool actorHidden = false;
                 eng::ReadBool(o, "bHidden", &actorHidden);
                 Report("ball " + eng::ObjName(o) + (actorHidden ? " actor hidden" : " actor shown") + " speed " +
                        std::to_string(static_cast<int>(std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z))) +
                        " skin " + (skin && eng::IsLive(skin) ? eng::ObjName(skin) + (hidden ? " hidden" : " SHOWN") : std::string("none")) +
                        " sphere " + (eng::Call(sphere, "IsVisible").ReturnBool() ? "visible" : "hidden") + " mesh " +
                        eng::ObjName(eng::ReadObj(sphere, "StaticMesh")) + " material " +
                        eng::ObjName(eng::Call(sphere, "GetMaterial", int32_t{0}).ReturnObj()));
                 return true;
             });
             eng::Obj menu = eng::FindClass("BP_MenuBall_C");          // and the menu balls' hats: mesh and size
             eng::ForEachObject([&](eng::Obj o) {
                 if (eng::ClassOf(o) != menu || eng::IsDefaultObject(o)) return true;
                 eng::Obj slot = eng::ReadObj(o, "AccessorySlot");
                 double scale[3] = {};
                 if (slot) eng::ReadBytes(slot, "RelativeScale3D", scale, sizeof scale);
                 Report("menu ball " + eng::ObjName(o) + " hat " + eng::ObjName(slot ? eng::ReadObj(slot, "StaticMesh") : nullptr) +
                        " scale " + std::to_string(scale[0]) + "," + std::to_string(scale[1]) + "," + std::to_string(scale[2]));
                 return true;
             });
         }},
        {"slomo", [](const Args& a, const std::string& c) {        // slomo <dilation>: game time runs this fast (1 = normal)
             const bool ok = eng::Call(eng::FindCdo("GameplayStatics"), "SetGlobalTimeDilation", game::PlayerController(),
                                       static_cast<float>(std::atof(Arg(a, 1).c_str()))).Invoked();
             Report(c + (ok ? " -> ok" : " -> failed"));
         }},
        {"skinmats", [](const Args&, const std::string&) {        // every ball skin: material, parent, texture params
             eng::Obj cls = eng::FindClass("PDA_BallSkin_C"), inst = eng::FindClass("MaterialInstance");
             eng::ForEachObject([&](eng::Obj o) {
                 if (eng::ClassOf(o) != cls || eng::IsDefaultObject(o)) return true;
                 eng::Obj m = eng::ReadObj(o, "SkinMaterial");
                 std::string line = eng::ObjName(o) + ": " + (m ? eng::PathOf(m) : "none");
                 if (m && eng::IsA(m, inst)) {
                     line += " parent " + eng::ObjName(eng::ReadObj(m, "Parent")) + " textures";
                     struct { uint8_t* data; int32_t num, max; } arr{};
                     if (eng::ReadBytes(m, "TextureParameterValues", &arr, sizeof arr))
                         for (int i = 0; i < arr.num && i < 12; ++i) {          // FTextureParameterValue: 40 bytes (measured)
                             uint32_t ci = 0; int32_t num = 0;
                             std::memcpy(&ci, arr.data + i * 40, 4); std::memcpy(&num, arr.data + i * 40 + 4, 4);
                             line += " [" + eng::Name(ci, num) + "]";
                         }
                 }
                 Report(line);
                 return true;
             });
         }},
        {"gc", [](const Args&, const std::string&) {
             eng::Call(eng::FindCdo("KismetSystemLibrary"), "CollectGarbage");
             Report("gc requested");
         }},
        {"replaycam", [](const Args& a, const std::string&) {
             replay::SetCameraDistance(std::atof(Arg(a, 1).c_str()));
             replay::SetSeeThrough(Arg(a, 2) == "1");
         }},
    };
    const Args words = Words(cmd);
    const auto it = words.empty() ? commands.end() : commands.find(words[0]);
    if (it == commands.end()) {
        hostlog::Warn("test: unknown command '" + cmd + "'");
    } else if (!ReadOnlyCommand(words[0]) && !MutationsAllowed()) {
        hostlog::Warn("test: refused '" + words[0] + "': it changes the game, which only a completely sandboxed test copy may do");
    } else {
        if (!ReadOnlyCommand(words[0])) race::TaintRun("test command " + words[0]);
        it->second(words, cmd);
    }
}

std::vector<std::string> gQueue;
unsigned gFrames = 0;

void PollFile() {
    const std::wstring path = hostlog::DataDir() + L"\\test_command.txt";
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    char buf[512] = {};
    DWORD read = 0;
    ReadFile(f, buf, sizeof buf - 1, &read, nullptr);
    CloseHandle(f);
    DeleteFileW(path.c_str());
    std::string cmd(buf, read);
    while (!cmd.empty() && (cmd.back() == '\n' || cmd.back() == '\r' || cmd.back() == ' ')) cmd.pop_back();
    Run(cmd);
}

}  // namespace

void Frame() {
    ++gHostFrames;
    // Keys posted with "post": let go when their time is up (the game reads a press and a release, not a state).
    for (auto it = gPostedUps.begin(); it != gPostedUps.end();) {
        if (GetTickCount64() < it->second) {
            ++it;
            continue;
        }
        if (HWND w = static_cast<HWND>(game::WindowHandle())) {
            const LPARAM scan = static_cast<LPARAM>(MapVirtualKeyW(static_cast<UINT>(it->first), 0)) << 16;
            PostMessageW(w, WM_KEYUP, static_cast<WPARAM>(it->first), 1 | scan | (1LL << 30) | (1LL << 31));
        }
        it = gPostedUps.erase(it);
    }
    std::vector<std::string> queued;
    queued.swap(gQueue);            // a command that queues another runs it next frame
    for (const auto& cmd : queued) Run(cmd);
    if (++gFrames % 30 == 0) PollFile();
}

void Enqueue(const std::string& command) { gQueue.push_back(command); }

}  // namespace testchannel
