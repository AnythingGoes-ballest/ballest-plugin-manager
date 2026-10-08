#include "runs.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <sstream>
#include <vector>

#include "engine.hpp"
#include "game.hpp"
#include "ghostdata.hpp"
#include "ghosts.hpp"
#include "json.hpp"
#include "log.hpp"
#include "race.hpp"

namespace runs {
namespace {

using eng::Obj;

struct Vec3 {
    double x, y, z;
};
struct Rot {
    double pitch, yaw, roll;
};
constexpr uint8_t kNoBone[8] = {};
constexpr double kSampleEvery = 0.1;    // the game's own replays: about ten samples a second
constexpr double kShortest = 1.0;       // shorter runs (a restart straight after the start) aren't kept
constexpr double kLeastDistance = 300;   // cm: a run whose ball never got this far from where it started isn't kept

struct Saved {
    std::string file, key, name;
    double time = 0;
    bool finished = false;
    long long endedAt = 0;              // unix seconds
    bool pinned = false;                // kept whatever comes after, and not counted in Keep
};
std::vector<Saved> gRuns;               // newest first
bool gLoaded = false;
int gKeep = 10;

struct Sample {
    double t;
    Vec3 at, velocity;
    Rot ball, control;
};
bool gRecording = false;
int gRunId = -1, gEndedRunId = -1;
double gNoPawnSince = -1;                // game seconds since the run lost its pawn (a finish on its way), or -1
double gLastSample = -1, gNow = 0;
std::string gKey, gName;
std::vector<Sample> gSamples;

std::map<std::string, ghostdata::Replay> gReplays;     // file -> read once

std::wstring Dir() { return hostlog::DataDir() + L"\\runs"; }
std::wstring IndexFile() { return Dir() + L"\\index.txt"; }

std::string ReadFile(const std::wstring& path) {
    std::string data;
    if (FILE* f = _wfopen(path.c_str(), L"rb")) {
        char buf[16384];
        for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) data.append(buf, n);
        std::fclose(f);
    }
    return data;
}

bool WriteFile(const std::wstring& path, const std::string& data) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
    return true;
}

// index.txt: one run a line, newest first: file, track key, track name, time, finished, ended (unix), pinned,
// tab-separated.
void Load() {
    if (gLoaded) return;
    gLoaded = true;
    std::stringstream in(ReadFile(IndexFile()));
    for (std::string line; std::getline(in, line);) {
        std::vector<std::string> f;
        std::stringstream cells(line);
        for (std::string c; std::getline(cells, c, '\t');) f.push_back(c);
        if (f.size() < 6) continue;
        Saved s{f[0], f[1], f[2], std::atof(f[3].c_str()), f[4] == "1", std::atoll(f[5].c_str()), f.size() > 6 && f[6] == "1"};
        if (GetFileAttributesW((Dir() + L"\\" + eng::Widen(s.file)).c_str()) != INVALID_FILE_ATTRIBUTES) gRuns.push_back(s);
    }
}

std::string Clean(const std::string& s) {           // no tabs or line breaks in the index
    std::string out = s;
    for (char& c : out)
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    return out;
}

void SaveIndex() {
    std::string text;
    char b[64];
    for (const auto& s : gRuns) {
        std::snprintf(b, sizeof b, "\t%.3f\t%d\t%lld\t%d\n", s.time, s.finished ? 1 : 0, s.endedAt, s.pinned ? 1 : 0);
        text += Clean(s.file) + "\t" + Clean(s.key) + "\t" + Clean(s.name) + b;
    }
    WriteFile(IndexFile(), text);
}

// The oldest runs go once more than Keep aren't pinned; pinned ones stay.
void Prune() {
    int unpinned = 0;
    for (const auto& s : gRuns) unpinned += s.pinned ? 0 : 1;
    for (size_t i = gRuns.size(); i-- > 0 && unpinned > gKeep;) {
        if (gRuns[i].pinned) continue;
        DeleteFileW((Dir() + L"\\" + eng::Widen(gRuns[i].file)).c_str());
        gReplays.erase(gRuns[i].file);
        gRuns.erase(gRuns.begin() + static_cast<std::ptrdiff_t>(i));
        --unpinned;
    }
}

// --- reading the ball ------------------------------------------------------------------------------------------------
Obj Ball() {
    Obj controller = game::PlayerController();
    Obj pawn = controller ? eng::Call(controller, "K2_GetPawn").ReturnObj() : nullptr;
    return pawn && eng::FindProp(eng::ClassOf(pawn), "RaceId") ? pawn : nullptr;
}

// The game's own race clock (BP_MyPlayerController_C.ActualRaceTime, the timer on screen): runs are recorded on it,
// so a run's ghost lines up with the race timer and with replays (measured: the world clock ran ~2 s apart, the
// race timer starting before the race turns active).
double RaceClock(Obj controller) {
    double t = -1;
    if (!controller || !eng::ReadBytes(controller, "ActualRaceTime", &t, sizeof t)) return -1;
    return t;
}

void TakeSample(double t) {
    Obj ball = Ball();
    Obj controller = game::PlayerController();
    if (!ball || !controller) return;
    Obj root = eng::Call(ball, "K2_GetRootComponent").ReturnObj();
    Sample s{};
    s.t = t;
    s.at = eng::Call(ball, "K2_GetActorLocation").ReturnAs<Vec3>();
    s.ball = eng::Call(ball, "K2_GetActorRotation").ReturnAs<Rot>();
    s.velocity = root ? eng::Call(root, "GetPhysicsLinearVelocity", kNoBone).ReturnAs<Vec3>() : Vec3{};
    s.control = eng::Call(controller, "GetControlRotation").ReturnAs<Rot>();
    gSamples.push_back(s);
}

// --- writing a run in the game's replay format ---------------------------------------------------------------------
std::string Quote(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        if (static_cast<unsigned char>(c) < 0x20) continue;
        out += c;
    }
    return out + "\"";
}

std::string Number(double v) {
    char b[40];
    std::snprintf(b, sizeof b, "%.6f", v);
    return b;
}

std::string Xyz(const Vec3& v) { return "{\"x\":" + Number(v.x) + ",\"y\":" + Number(v.y) + ",\"z\":" + Number(v.z) + "}"; }
std::string Pyr(const Rot& r) { return "{\"pitch\":" + Number(r.pitch) + ",\"yaw\":" + Number(r.yaw) + ",\"roll\":" + Number(r.roll) + "}"; }

// The player's look and name, from their newest ghost the game saved (Saved\Ghosts\<level>_<name>.json).
struct Look {
    std::string username = "you", steamId = "0", skin = "None", ghostSkin = "None", accessory = "None", accessoryGhost = "None",
                special = "None", prefs = "{\"bBasicBallTexture\":true,\"bBasicBallGloss\":true,\"basicBallTextureSliderValue\":0}";
};

Look NewestLook() {
    Look look;
    std::wstring dir = hostlog::DataDir();
    dir = dir.substr(0, dir.find_last_of(L'\\')) + L"\\Ghosts";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.json").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return look;
    std::wstring newest;
    FILETIME best{};
    do {
        if (CompareFileTime(&fd.ftLastWriteTime, &best) > 0) {
            best = fd.ftLastWriteTime;
            newest = fd.cFileName;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    json::Value v;
    std::string error;
    if (newest.empty() || !json::Parse(ReadFile(dir + L"\\" + newest), v, error)) return look;
    look.username = v.Str("username", look.username);
    if (const json::Value* id = v.Get("steamId"))
        if (const json::Value* r = id->Get("result"); r && r->type == json::Value::Number) {
            char b[32];
            std::snprintf(b, sizeof b, "%.0f", r->number);
            look.steamId = b;
        }
    look.skin = v.Str("skinMaterial", look.skin);
    look.ghostSkin = v.Str("ghostSkinMaterial", look.ghostSkin);
    look.accessory = v.Str("accessory", look.accessory);
    look.accessoryGhost = v.Str("accessoryGhostMaterial", look.accessoryGhost);
    look.special = v.Str("?SpecialSkinClass", look.special);
    if (const json::Value* p = v.Get("ballerSkinPrefs")) {
        auto flag = [&](const char* k) { const json::Value* x = p->Get(k); return x && x->type == json::Value::Bool && x->boolean; };
        const json::Value* slider = p->Get("basicBallTextureSliderValue");
        look.prefs = std::string("{\"bBasicBallTexture\":") + (flag("bBasicBallTexture") ? "true" : "false") + ",\"bBasicBallGloss\":" +
                     (flag("bBasicBallGloss") ? "true" : "false") + ",\"basicBallTextureSliderValue\":" +
                     Number(slider && slider->type == json::Value::Number ? slider->number : 0) + "}";
    }
    return look;
}

std::string RunJson(double time) {
    const Look look = NewestLook();
    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%Y.%m.%d-%H.%M.%S", &local);
    std::string locations, times, rotations, velocities, controls;
    for (size_t i = 0; i < gSamples.size(); ++i) {
        const auto& s = gSamples[i];
        const char* sep = i ? "," : "";
        locations += sep + Xyz(s.at);
        times += sep + Number(s.t);
        rotations += sep + Pyr(s.ball);
        velocities += sep + Xyz(s.velocity);
        controls += sep + Pyr(s.control);
    }
    return "{\"levelName\":" + Quote(gName) + ",\"bestTime\":" + Number(time) + ",\"skinMaterial\":" + Quote(look.skin) +
           ",\"accessory\":" + Quote(look.accessory) + ",\"username\":" + Quote(look.username) + ",\"locations\":[" + locations +
           "],\"elapsedTime\":[" + times + "],\"rotation\":[" + rotations + "],\"accessoryGhostMaterial\":" + Quote(look.accessoryGhost) +
           ",\"ghostSkinMaterial\":" + Quote(look.ghostSkin) + ",\"timestamp\":" + Quote(stamp) + ",\"velocities\":[" + velocities +
           "],\"controlRotations\":[" + controls + "],\"?CheckpointSplits\":[],\"steamId\":{\"result\":" + look.steamId +
           "},\"?SpecialSkinClass\":" + Quote(look.special) + ",\"ballerSkinPrefs\":" + look.prefs + "}";
}

void Begin(int runId) {
    gRecording = true;
    gRunId = runId;
    gLastSample = -1;
    gSamples.clear();
    gKey = race::CurrentTrack().key;
    gName = race::CurrentTrack().name;
}

void End(bool finished) {
    gRecording = false;
    gNoPawnSince = -1;
    gEndedRunId = gRunId;
    const double time = gNow;                // the race clock: a finished run's own time
    if (gSamples.size() < 5 || time < kShortest || gKey.empty()) return;
    // A run the ball never got going in (sitting at the start, or in the menu with the race clock running: measured,
    // runs of 45 s and 509 s that never moved) isn't worth a place among the kept ones.
    double furthest = 0;
    for (const Sample& s : gSamples) {
        const double dx = s.at.x - gSamples.front().at.x, dy = s.at.y - gSamples.front().at.y, dz = s.at.z - gSamples.front().at.z;
        furthest = std::max(furthest, std::sqrt(dx * dx + dy * dy + dz * dz));
    }
    if (!finished && furthest < kLeastDistance) {
        hostlog::Info("runs: not keeping a run of " + gName + " that never left the start");
        return;
    }
    if (finished && gSamples.back().t < time - 0.01) TakeSample(time);    // the finish too (after a restart the ball is back at the start)
    Load();
    CreateDirectoryW(Dir().c_str(), nullptr);
    const long long ended = static_cast<long long>(std::time(nullptr));
    char name[64];
    std::snprintf(name, sizeof name, "run_%lld_%d.json", ended, static_cast<int>(GetTickCount() % 100000));
    if (!WriteFile(Dir() + L"\\" + eng::Widen(name), RunJson(time))) {
        hostlog::Warn("runs: couldn't save a run");
        return;
    }
    gRuns.insert(gRuns.begin(), Saved{name, gKey, gName, time, finished, ended});
    Prune();
    SaveIndex();
    char b[96];
    std::snprintf(b, sizeof b, "%.3f s, %s", time, finished ? "finished" : "not finished");
    hostlog::Info("runs: kept a run of " + gName + " (" + b + ")");
}

const ghostdata::Replay* ReplayOf(int i) {
    Load();
    if (i < 0 || i >= static_cast<int>(gRuns.size())) return nullptr;
    const std::string& file = gRuns[static_cast<size_t>(i)].file;
    auto it = gReplays.find(file);
    if (it == gReplays.end()) {
        ghostdata::Replay r;
        std::string error;
        if (!ghostdata::Parse(ReadFile(Dir() + L"\\" + eng::Widen(file)), &r, &error)) {
            hostlog::Warn("runs: couldn't read " + file + ": " + error);
            return nullptr;
        }
        it = gReplays.emplace(file, std::move(r)).first;
    }
    return &it->second;
}

}  // namespace

void Frame() {
    Obj controller = game::PlayerController();
    const bool on = race::OnTrack();
    const int id = race::RunId();
    if (gRecording && race::Practice()) {
        // A practice run isn't one of the player's runs, and its clock is stopped (measured: kept as 999.999 s when
        // the track was left): it's dropped, and this run id isn't recorded again.
        gRecording = false;
        gNoPawnSince = -1;
        gEndedRunId = gRunId;
        hostlog::Info("runs: a practice run isn't kept");
        return;
    }
    if (gRecording) {
        // Leaving the track, a new run, or the race clock going back (measured: on a restart the clock is back at 0
        // about a second before the run's id changes): the run ends at the furthest time it reached.
        const double clock = controller ? RaceClock(controller) : gNow;
        if (on && race::Complete()) {
            // The finish comes before the run's id check: crossing the line takes the ball's pawn away (measured: the
            // id reads -1 with the race complete, so a finished run was being kept as unfinished).
            gNow = std::max(gNow, clock);
            End(true);
            return;
        }
        if (on && id < 0 && clock >= gNow - 0.5) {
            // No pawn for a moment, the clock still going: the finish may land a frame or two later.
            if (gNoPawnSince < 0) gNoPawnSince = game::Seconds();
            if (game::Seconds() - gNoPawnSince < 1.5) return;
        }
        if (!on || id != gRunId || clock < gNow - 0.5) {
            End(false);
            return;
        }
        gNoPawnSince = -1;
        gNow = std::max(gNow, clock);
        if (gNow >= gLastSample + kSampleEvery - 1e-6 || gLastSample < 0) {
            gLastSample = gNow;
            TakeSample(gLastSample);
        }
    }
    if (!gRecording && on && controller && race::Active() && !race::Complete() && !race::Practice() && id >= 0 &&
        id != gEndedRunId) {
        gNow = std::max(0.0, RaceClock(controller));
        Begin(id);
    }
}

void SetKeep(int count) {
    Load();
    gKeep = std::clamp(count, 1, 500);
    const size_t before = gRuns.size();
    Prune();
    if (gRuns.size() != before) SaveIndex();
}

int Count() {
    Load();
    return static_cast<int>(gRuns.size());
}

namespace {
const Saved* At(int i) {
    Load();
    return i >= 0 && i < static_cast<int>(gRuns.size()) ? &gRuns[static_cast<size_t>(i)] : nullptr;
}
}  // namespace

std::string Id(int i) { return At(i) ? At(i)->file : ""; }
std::string TrackKey(int i) { return At(i) ? At(i)->key : ""; }
std::string TrackName(int i) { return At(i) ? At(i)->name : ""; }
double Time(int i) { return At(i) ? At(i)->time : 0; }
bool Finished(int i) { return At(i) && At(i)->finished; }
double Age(int i) { return At(i) ? static_cast<double>(std::time(nullptr) - At(i)->endedAt) : 0; }
bool Pinned(int i) { return At(i) && At(i)->pinned; }

void SetPinned(int i, bool pinned) {
    Load();
    if (i < 0 || i >= static_cast<int>(gRuns.size()) || gRuns[static_cast<size_t>(i)].pinned == pinned) return;
    gRuns[static_cast<size_t>(i)].pinned = pinned;
    Prune();                            // unpinning one can take the list past Keep
    SaveIndex();
}

double Elapsed() { return gRecording ? gNow : -1; }

int Ball(int owner, int i) {
    const ghostdata::Replay* r = ReplayOf(i);
    return r ? ghosts::PlayerBallFor(owner, *r) : 0;
}

bool Place(int owner, int id, int i, double t) {
    const ghostdata::Replay* r = ReplayOf(i);
    return r && ghosts::PlaceBallFor(owner, id, *r, t);
}

}  // namespace runs
