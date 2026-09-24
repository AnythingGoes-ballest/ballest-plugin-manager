#include "testchannel.hpp"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "engine.hpp"
#include "game.hpp"
#include "input.hpp"
#include "log.hpp"
#include "editor.hpp"
#include "plugins.hpp"
#include "registry.hpp"
#include "replay.hpp"
#include "settings.hpp"
#include "ui.hpp"

namespace testchannel {
namespace {

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

void ViewTarget() {
    eng::Obj controller = game::PlayerController();
    Report("controller " + eng::PathOf(controller) + " view target " + eng::PathOf(eng::Call(controller, "GetViewTarget").ReturnObj()));
}

using Args = std::vector<std::string>;

std::string Arg(const Args& a, size_t i) { return i < a.size() ? a[i] : std::string(); }

void Run(const std::string& cmd) {
    static const std::map<std::string, void (*)(const Args&, const std::string&)> commands = {
        {"state", [](const Args&, const std::string&) { Report("state " + ui::Status() + " | plugins: " + plugins::Summary()); }},
        {"click", [](const Args&, const std::string& c) { Report(c + (ui::SimulateClick(c.substr(6)) ? " -> ok" : " -> no such button")); }},
        {"select", [](const Args& a, const std::string& c) {
             Report(c + (ui::SimulateSelect(Arg(a, 1), std::atoi(Arg(a, 2).c_str())) ? " -> ok" : " -> no such dropdown"));
         }},
        {"slider", [](const Args& a, const std::string& c) {
             ui::SimulateSlider(static_cast<float>(std::atof(Arg(a, 1).c_str())));
             Report(c);
         }},
        {"submit", [](const Args&, const std::string& c) { Report(c + (ui::SimulateSubmit(c.substr(7)) ? " -> ok" : " -> no text input")); }},
        {"press", [](const Args& a, const std::string& c) {
             input::Simulate(std::atoi(Arg(a, 1).c_str()));
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
                 if (list[i].pluginId == Arg(a, 1) && list[i].variable == Arg(a, 2))
                     return Report(c + (settings::Set(i, Arg(a, 3)) ? " -> " + settings::Get(i) : " -> not a value"));
             Report(c + " -> no such setting");
         }},
        {"editor", [](const Args& a, const std::string&) {
             if (Arg(a, 1) == "rotatecontext") editor::ForceRotateContext(Arg(a, 2) == "on");
             Report(editor::Status());
         }},
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
    };
    const Args words = Words(cmd);
    const auto it = words.empty() ? commands.end() : commands.find(words[0]);
    if (it == commands.end()) hostlog::Warn("test: unknown command '" + cmd + "'");
    else it->second(words, cmd);
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
    std::vector<std::string> queued;
    queued.swap(gQueue);            // a command that queues another runs it next frame
    for (const auto& cmd : queued) Run(cmd);
    if (++gFrames % 30 == 0) PollFile();
}

void Enqueue(const std::string& command) { gQueue.push_back(command); }

}  // namespace testchannel
