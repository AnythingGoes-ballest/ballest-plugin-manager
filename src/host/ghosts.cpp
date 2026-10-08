#include "ghosts.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_set>

#include "engine.hpp"
#include "game.hpp"
#include "log.hpp"
#include "cosmetics.hpp"
#include "draw.hpp"
#include "models.hpp"
#include "steam.hpp"

namespace ghosts {
namespace {

using eng::Obj;

// Replays downloading at once. Measured, 1000 replays each on fresh tracks, failed ones asked for again: 4 at once
// 7.4/s; 8 10.6/s; 12 8.1/s; 16 10.8 and 8.5/s; 24 9.9/s; none failed. Steam serves about 10 a second however many
// are asked for, so 8 (more only loads Steam).
constexpr int kParallelDownloads = 8;
constexpr int kDownloadTries = 3;       // a replay that fails to download is asked for again, up to this many times
constexpr int kPage = 100;              // entries asked of Steam at a time
constexpr int kParallelPages = 4;       // pages of entries asked for at once

int gSerial = 0;                        // bumped by every Load: late results of an older load are dropped
int gGeneration = -1;
int gLoadedGeneration = -1;             // the map the ghosts here belong to
std::string gState = "idle", gLeaderboard;
int gEntries = 0, gWaiting = 0;
std::unordered_set<uint64_t> gNoReplay;    // entries asked for that have no replay file (nothing to download)
uint64_t gHandle = 0;
std::vector<Ghost> gGhosts;
struct Pending {
    steam::Entry entry;
    bool own = false;
    int tries = 0;
};
std::deque<Pending> gQueue;
int gDownloading = 0;
std::unordered_set<uint64_t> gListed;   // steam ids queued, downloading, parsing or loaded
int gNextPage = 1, gLastRank = 0, gPagesOut = 0;
bool gPagesDone = false;

// Replays are parsed on worker threads (a replay is ~80 KB of JSON): the downloads hand them over, and Frame takes
// the parsed ghosts back on the game thread.
struct Job {
    int serial;
    Pending pending;
    std::string bytes;
    std::vector<ghostdata::Point> checkpoints;     // where the track's checkpoints are, and their numbers
    std::vector<int> numbers;
};
struct Parsed {
    int serial;
    Pending pending;
    Ghost ghost;
    bool ok;
    std::string error;
};
std::mutex gJobsLock;
std::condition_variable gJobsReady;
std::deque<Job> gJobs;
std::vector<Parsed> gParsed;            // guarded by gJobsLock
int gParsing = 0;                       // handed to the workers and not yet taken back (game thread)
bool gWorkersStarted = false;

void Worker() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(gJobsLock);
            gJobsReady.wait(lock, [] { return !gJobs.empty(); });
            job = std::move(gJobs.front());
            gJobs.pop_front();
        }
        Parsed out{job.serial, job.pending, {}, false, {}};
        out.ok = ghostdata::Parse(job.bytes, &out.ghost.replay, &out.error);
        // Its checkpoint order here, off the game thread: worked out on the game thread for a thousand runs at once
        // it held up a frame (measured with the rest of what the viewer does then: 83 ms).
        if (out.ok && !job.checkpoints.empty()) {
            for (int i : ghostdata::CheckpointOrder(out.ghost.replay, job.checkpoints))
                out.ghost.order.push_back(i < 0 ? 0 : job.numbers[static_cast<size_t>(i)]);
            out.ghost.orderOf = static_cast<int>(job.checkpoints.size());
        }
        std::lock_guard<std::mutex> lock(gJobsLock);
        gParsed.push_back(std::move(out));
    }
}

void Parse(int serial, const Pending& pending, std::string bytes) {
    if (!gWorkersStarted) {
        gWorkersStarted = true;
        const unsigned cores = std::thread::hardware_concurrency();
        const unsigned workers = cores > 2 ? std::min(cores - 1, 4u) : 1u;
        for (unsigned i = 0; i < workers; ++i) std::thread(Worker).detach();
    }
    ++gParsing;
    Job job{serial, pending, std::move(bytes), {}, {}};
    for (const auto& c : Checkpoints()) {
        job.checkpoints.push_back(c.at);
        job.numbers.push_back(c.number);
    }
    {
        std::lock_guard<std::mutex> lock(gJobsLock);
        gJobs.push_back(std::move(job));
    }
    gJobsReady.notify_one();
}
std::vector<Checkpoint> gCheckpoints;
int gCheckpointsGeneration = -1;
double gCheckpointsScanned = -100;

std::string Narrow(const eng::FString& s) {
    if (!s.data || s.num <= 1) return "";
    return eng::Narrow(s.data, s.num - 1);
}

// The game's leaderboard for the track on screen: its Steam handle and name.
bool ActiveLeaderboard(uint64_t* handle, std::string* name) {
    Obj controller = game::PlayerController();
    Obj instance = controller ? eng::Call(eng::FindCdo("GameplayStatics"), "GetGameInstance", controller).ReturnObj() : nullptr;
    Obj fn = instance ? eng::FindFunction(eng::ClassOf(instance), "GetActiveLevelLeaderboardRecord") : nullptr;
    if (!fn) return false;
    eng::Params p(fn);
    p.Set("bDeferAndRefresh", uint8_t{0});
    if (!eng::Invoke(instance, p)) return false;
    size_t size = 0;
    const uint8_t* record = p.Get("NativeRecordOut", &size);
    if (!record || size < 0x50) return false;
    std::memcpy(handle, record + 0x0, sizeof *handle);             // LeaderboardHandle
    eng::FString resolved{};
    std::memcpy(&resolved, record + 0x40, sizeof resolved);          // ResolvedSteamLeaderboardName
    *name = Narrow(resolved);
    return *handle != 0 || !name->empty();
}

void ShowProgress() {
    gState = "loading replays " + std::to_string(gGhosts.size()) + "/" +
             std::to_string(gGhosts.size() + gParsing + gDownloading + gQueue.size());
}

void Finish() {
    if (gDownloading == 0 && gQueue.empty() && gWaiting == 0 && gParsing == 0 && gState.rfind("loading", 0) == 0) {
        gState = "ready";
        hostlog::Info("ghosts: " + std::to_string(gGhosts.size()) + " replay(s) of " + gLeaderboard + " ready");
    }
}

void Pump(int serial) {
    while (serial == gSerial && gDownloading < kParallelDownloads && !gQueue.empty()) {
        const Pending next = gQueue.front();
        gQueue.pop_front();
        ++gDownloading;
        steam::DownloadFile(next.entry.file, [serial, next](bool ok, std::string bytes) {
            if (serial != gSerial) return;
            --gDownloading;
            if (ok) {
                Parse(serial, next, std::move(bytes));
            } else if (next.tries + 1 < kDownloadTries) {
                Pending again = next;
                ++again.tries;
                gQueue.push_back(again);        // at the back: Steam gets a moment before it's asked again
            } else {
                hostlog::Warn("ghosts: rank " + std::to_string(next.entry.rank) + "'s replay didn't download (" +
                              std::to_string(kDownloadTries) + " tries)");
            }
            ShowProgress();
            Pump(serial);
            Finish();
        });
    }
}

void Queue(int serial, const std::vector<steam::Entry>& entries, bool own) {
    for (const auto& e : entries) {
        if (!e.file) {
            gNoReplay.insert(e.steamId);
            continue;
        }
        if (gListed.insert(e.steamId).second) {
            gQueue.push_back({e, own});
        } else if (own) {
            for (auto& p : gQueue)
                if (p.entry.steamId == e.steamId) p.own = true;
            for (auto& g : gGhosts)
                if (g.steamId == e.steamId) g.own = true;
        }
    }
    Pump(serial);
}

// Pages of kPage ranks, kParallelPages asked for at once; each one answered asks for the next until the last rank
// wanted (or a page comes back empty: the end of the leaderboard).
void AskPages(int serial) {
    while (serial == gSerial && !gPagesDone && gPagesOut < kParallelPages && gNextPage <= gLastRank) {
        const int first = gNextPage, last = std::min(first + kPage - 1, gLastRank);
        gNextPage = last + 1;
        ++gPagesOut;
        steam::DownloadEntries(gHandle, first, last, [serial, first](bool ok, std::vector<steam::Entry> entries) {
            if (serial != gSerial) return;
            --gPagesOut;
            if (!ok && first == 1) {
                gPagesDone = true;
                gState = "error: the leaderboard's entries didn't download";
            }
            gEntries = std::max({gEntries, entries.empty() ? 0 : entries.back().rank, steam::EntryCount(gHandle)});
            if (gState == "loading entries") gState = "loading replays";
            Queue(serial, entries, false);
            if (!ok || entries.empty()) gPagesDone = true;
            AskPages(serial);
            if (gPagesOut == 0 && (gPagesDone || gNextPage > gLastRank) && gWaiting > 0) --gWaiting;
            Finish();
        });
    }
}

void Entries(int serial, uint64_t handle, int count) {
    gHandle = handle;
    gState = "loading entries";
    gWaiting = 2;                       // the pages, and the player's own entry
    const int total = steam::EntryCount(handle);
    gLastRank = count > 0 ? count : total > 0 ? total : 1 << 30;
    gNextPage = 1;
    gPagesOut = 0;
    gPagesDone = false;
    AskPages(serial);
    steam::DownloadOwnEntry(handle, [serial](bool ok, std::vector<steam::Entry> entries) {
        if (serial != gSerial) return;
        --gWaiting;
        if (ok) Queue(serial, entries, true);
        Finish();
    });
}

// The workers' parsed ghosts, taken in on the game thread.
void TakeParsed() {
    std::vector<Parsed> parsed;
    {
        std::lock_guard<std::mutex> lock(gJobsLock);
        if (gParsed.empty()) return;
        parsed.swap(gParsed);
    }
    bool added = false;
    for (auto& p : parsed) {
        --gParsing;
        if (p.serial != gSerial) continue;
        if (!p.ok) {
            hostlog::Warn("ghosts: rank " + std::to_string(p.pending.entry.rank) + "'s replay: " + p.error);
            continue;
        }
        p.ghost.rank = p.pending.entry.rank;
        p.ghost.own = p.pending.own;
        p.ghost.steamId = p.pending.entry.steamId;
        gGhosts.push_back(std::move(p.ghost));
        added = true;
    }
    if (gParsing < 0) gParsing = 0;
    if (added) std::sort(gGhosts.begin(), gGhosts.end(), [](const Ghost& a, const Ghost& b) { return a.rank < b.rank; });
    if (gState.rfind("loading", 0) == 0) ShowProgress();
    Finish();
}

}  // namespace

void ForgetCrowds();
void BuildCrowdTrails();

// A new map: everything loaded for the last one goes (its replays can be tens of megabytes), and what is still
// downloading for it is dropped when it arrives.
void Frame() {
    TakeParsed();
    BuildCrowdTrails();
    if (game::Generation() == gGeneration) return;
    gGeneration = game::Generation();
    if (gGhosts.empty() && gQueue.empty() && gState == "idle") return;
    ++gSerial;
    std::vector<Ghost>().swap(gGhosts);
    std::deque<Pending>().swap(gQueue);
    std::vector<Checkpoint>().swap(gCheckpoints);
    gListed.clear();
    gNoReplay.clear();
    gPagesDone = true;
    gDownloading = gWaiting = 0;
    gEntries = 0;
    gHandle = 0;
    gLeaderboard.clear();
    gLoadedGeneration = -1;
    gState = "idle";
    ForgetCrowds();                     // their actors went with the map
    hostlog::Info("ghosts: left the track; its replays are unloaded");
}

bool Load(const std::string& leaderboard, int count) {
    if (!steam::Available()) {
        gState = "error: Steam isn't available";
        return false;
    }
    const int serial = ++gSerial;
    gQueue.clear();
    gDownloading = gWaiting = 0;
    count = std::max(count, 0);
    uint64_t handle = 0;
    std::string name = leaderboard;
    if (name.empty() && !ActiveLeaderboard(&handle, &name)) {
        gState = "error: no track, or the game has no leaderboard for it yet";
        return false;
    }
    gListed.clear();
    gNoReplay.clear();
    if (name == gLeaderboard && gLoadedGeneration == game::Generation()) {
        // The same leaderboard: keep what is here and still wanted.
        if (count > 0)
            gGhosts.erase(std::remove_if(gGhosts.begin(), gGhosts.end(), [count](const Ghost& g) { return !g.own && g.rank > count; }), gGhosts.end());
        for (const auto& g : gGhosts) gListed.insert(g.steamId);
    } else {
        gGhosts.clear();
        gEntries = 0;
    }
    gLoadedGeneration = game::Generation();
    gLeaderboard = name;
    hostlog::Info("ghosts: loading " + (count > 0 ? "the top " + std::to_string(count) : std::string("every entry")) + " of " +
                  (name.empty() ? "the track's leaderboard" : name));
    if (handle) {
        Entries(serial, handle, count);
        return true;
    }
    gState = "loading: finding the leaderboard";
    steam::FindLeaderboard(name, [serial, count](uint64_t found) {
        if (serial != gSerial) return;
        if (!found) {
            gState = "error: no leaderboard called " + gLeaderboard;
            return;
        }
        Entries(serial, found, count);
    });
    return true;
}


namespace {
int Base64Value(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
std::vector<uint8_t> Base64(const std::string& s) {
    std::vector<uint8_t> out;
    out.reserve(s.size() * 3 / 4);
    uint32_t acc = 0;
    int bits = 0;
    for (unsigned char c : s) {
        const int v = Base64Value(c);
        if (v < 0) continue;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((acc >> bits) & 0xff));
        }
    }
    return out;
}
std::vector<std::string> Fields(const std::string& line) {
    std::vector<std::string> f;
    size_t a = 0;
    for (;;) {
        const size_t b = line.find('\t', a);
        f.push_back(line.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) break;
        a = b + 1;
    }
    return f;
}
}  // namespace

bool ParseLocal(const std::wstring& path, std::vector<Ghost>* out) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    std::string text;
    char buf[65536];
    for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
    fclose(f);
    size_t at = 0;
    auto nextLine = [&](std::string* line) {
        if (at >= text.size()) return false;
        size_t e = text.find('\n', at);
        if (e == std::string::npos) e = text.size();
        *line = text.substr(at, e - at);
        if (!line->empty() && line->back() == '\r') line->pop_back();
        at = e + 1;
        return true;
    };
    std::string line;
    if (!nextLine(&line)) return false;
    const auto head = Fields(line);
    // RUN1; AIV1 is an older name of the same format
    if (head.size() < 5 || (head[0] != "RUN1" && head[0] != "AIV1")) return false;
    const double ox = std::atof(head[1].c_str()), oy = std::atof(head[2].c_str()), oz = std::atof(head[3].c_str());
    const double unit = std::atof(head[4].c_str());
    std::vector<Ghost> loaded;
    while (nextLine(&line)) {
        const auto c = Fields(line);
        if (c.size() < 6) continue;
        Ghost g;
        auto& r = g.replay;
        r.name = c[0];
        r.time = std::atof(c[1].c_str());
        if (c[2] != "-") r.look.skinMaterial = c[2];
        const double t0 = std::atof(c[3].c_str()), dt = std::atof(c[4].c_str());
        const std::vector<uint8_t> raw = Base64(c[5]);
        const size_t n = raw.size() / 6;
        r.samples.resize(n);
        for (size_t k = 0; k < n; ++k) {
            int16_t q[3];
            std::memcpy(q, raw.data() + k * 6, sizeof q);
            auto& s = r.samples[k];
            s.t = t0 + static_cast<double>(k) * dt;
            s.at = {ox + q[0] * unit, oy + q[1] * unit, oz + q[2] * unit};
        }
        for (size_t k = 0; k < n; ++k) {
            const auto& a = r.samples[k > 0 ? k - 1 : 0].at;
            const auto& b = r.samples[k + 1 < n ? k + 1 : n - 1].at;
            const double span = dt * static_cast<double>((k + 1 < n ? k + 1 : n - 1) - (k > 0 ? k - 1 : 0));
            auto& s = r.samples[k];
            if (span > 0) s.velocity = {(b.x - a.x) / span, (b.y - a.y) / span, (b.z - a.z) / span};
            if (std::abs(s.velocity.x) + std::abs(s.velocity.y) > 1) s.yaw = std::atan2(s.velocity.y, s.velocity.x) * 57.29577951308232;
            else if (k > 0) s.yaw = r.samples[k - 1].yaw;
            s.pitch = -15;
        }
        r.hasView = true;
        g.rank = static_cast<int>(loaded.size()) + 1;
        loaded.push_back(std::move(g));
    }
    *out = std::move(loaded);
    return true;
}

// PrefetchLocal: the file read and parsed on a worker thread (a plugin stepping through files loads the next while one
// shows); LoadLocal takes it from here when it is ready, else parses on the game thread as before.
std::mutex gPrefetchLock;
std::map<std::wstring, std::shared_ptr<std::vector<Ghost>>> gPrefetched;      // null while being parsed
void PrefetchLocal(const std::wstring& path) {
    {
        std::lock_guard<std::mutex> lock(gPrefetchLock);
        if (gPrefetched.count(path)) return;
        if (gPrefetched.size() > 6) gPrefetched.clear();
        gPrefetched[path] = nullptr;
    }
    std::thread([path] {
        auto v = std::make_shared<std::vector<Ghost>>();
        if (!ParseLocal(path, v.get())) v->clear();
        std::lock_guard<std::mutex> lock(gPrefetchLock);
        gPrefetched[path] = v;
    }).detach();
}
bool PrefetchReady(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(gPrefetchLock);
    auto it = gPrefetched.find(path);
    return it != gPrefetched.end() && it->second != nullptr;
}

int LoadLocal(const std::wstring& path) {
    std::vector<Ghost> loaded;
    bool have = false;
    {
        std::lock_guard<std::mutex> lock(gPrefetchLock);
        auto it = gPrefetched.find(path);
        if (it != gPrefetched.end() && it->second) {
            loaded = std::move(*it->second);
            gPrefetched.erase(it);
            have = !loaded.empty();
        }
    }
    if (!have && !ParseLocal(path, &loaded)) return -1;
    ++gSerial;                          // anything still downloading is dropped
    gQueue.clear();
    gDownloading = gWaiting = 0;
    gGhosts.swap(loaded);
    gEntries = static_cast<int>(gGhosts.size());
    gLoadedGeneration = game::Generation();
    gGeneration = game::Generation();
    gLeaderboard = "local";
    gState = "ready";
    return static_cast<int>(gGhosts.size());
}

std::string State() { return gState; }
std::string Leaderboard() { return gLeaderboard; }
int Entries() { return gEntries; }
int WithoutReplay() { return static_cast<int>(gNoReplay.size()); }
const std::vector<Ghost>& All() { return gGhosts; }

std::vector<Checkpoint> Checkpoints() {
    // Found once a map: a scan goes through every object. A map with none (the tower trials) is scanned again at
    // most every 5 s, in case its checkpoints appear later; without that, each caller rescanned (measured: a plugin
    // asking for 25 runs' checkpoint orders on The Tower ran over its time budget).
    if (gCheckpointsGeneration == game::Generation() && (!gCheckpoints.empty() || game::Seconds() - gCheckpointsScanned < 5))
        return gCheckpoints;
    gCheckpointsScanned = game::Seconds();
    gCheckpoints.clear();
    gCheckpointsGeneration = game::Generation();
    // What records a split: the goals (BP_Checkpoint_C) on most tracks. The Tower CPs has one goal and 14
    // MP_ActualCheckpoint_Strip_C (a BP_ActualCheckpointBase_C), matching its runs' 14 splits (read from the map);
    // on tracks with goals, each goal has a strip next to it (Leth Trial: 9 and 9), so the goals are used there.
    Obj cls = eng::FindClass("BP_Checkpoint_C"), stripClass = eng::FindClass("BP_ActualCheckpointBase_C");
    std::vector<Obj> found, strips;
    eng::ForEachObject([&](Obj o) {
        if (eng::IsDefaultObject(o) || !eng::IsLive(o)) return true;
        if (cls && eng::ClassOf(o) == cls) found.push_back(o);
        else if (stripClass && eng::IsA(o, stripClass)) strips.push_back(o);
        return true;
    });
    struct Vec3 {
        double x, y, z;
    };
    if (found.size() < 2 && !strips.empty()) {
        // Numbered from the bottom up: the tower is climbed.
        for (Obj o : strips) {
            const Vec3 at = eng::Call(o, "K2_GetActorLocation").ReturnAs<Vec3>();
            gCheckpoints.push_back({0, {at.x, at.y, at.z}});
        }
        std::sort(gCheckpoints.begin(), gCheckpoints.end(), [](const Checkpoint& a, const Checkpoint& b) { return a.at.z < b.at.z; });
        for (size_t i = 0; i < gCheckpoints.size(); ++i) gCheckpoints[i].number = static_cast<int>(i) + 1;
        return gCheckpoints;
    }
    // Placed in a level rather than the editor (the game's own tracks, e.g. Map_LethTrial_01), checkpoints keep the
    // default GoalNumber 0 (read from the map: none of its 9 BP_Checkpoint_C set it). Then they are numbered 1..n in
    // the order of their names (BP_Checkpoint_C_0, _1, ...), so each still has its own number and colour.
    std::vector<std::pair<int, Obj>> named;
    for (Obj o : found) {
        const std::string name = eng::ObjName(o);
        const size_t cut = name.find_last_of('_');
        named.push_back({cut == std::string::npos ? 0 : std::atoi(name.c_str() + cut + 1), o});
    }
    std::sort(named.begin(), named.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [suffix, o] : named) {
        int32_t number = 0;
        eng::ReadBytes(o, "GoalNumber", &number, sizeof number);
        const Vec3 at = eng::Call(o, "K2_GetActorLocation").ReturnAs<Vec3>();
        gCheckpoints.push_back({number, {at.x, at.y, at.z}});
    }
    std::vector<int> numbers;
    for (const auto& c : gCheckpoints) numbers.push_back(c.number);
    std::sort(numbers.begin(), numbers.end());
    const bool distinct = std::adjacent_find(numbers.begin(), numbers.end()) == numbers.end() && (numbers.empty() || numbers[0] >= 1);
    if (!distinct)
        for (size_t i = 0; i < gCheckpoints.size(); ++i) gCheckpoints[i].number = static_cast<int>(i) + 1;
    std::sort(gCheckpoints.begin(), gCheckpoints.end(), [](const Checkpoint& a, const Checkpoint& b) { return a.number < b.number; });
    return gCheckpoints;
}

namespace {
struct Rig {
    bool ok = false;
    double arm = 0, fov = 90;
    ghostdata::Point origin, target, socket;    // origin: the arm's start, from the tracked ball
};
Rig gRig;
int gRigGeneration = -1;

// The ball's camera rig, read once a map.
const Rig& ReadRig() {
    if (gRigGeneration == game::Generation() && gRig.ok) return gRig;
    gRigGeneration = game::Generation();
    gRig = Rig{};
    Obj cls = eng::FindClass("BallAsyncCameraTargetComponent");
    Obj found = nullptr;
    if (cls)
        eng::ForEachObject([&](Obj o) {
            if (eng::ClassOf(o) == cls && !eng::IsDefaultObject(o) && eng::IsLive(o)) found = o;
            return found == nullptr;
        });
    Obj arm = found ? eng::ReadObj(found, "SpringArmComponent") : nullptr;
    Obj camera = found ? eng::ReadObj(found, "CameraComponent") : nullptr;
    Obj tracked = found ? eng::ReadObj(found, "TrackedComponent") : nullptr;
    if (!arm) {
        // Before the race the component's references aren't filled in (measured: SpringArmComponent empty on the
        // pre-race screen) and the ball isn't the controller's pawn. The player's ball (BP_RollingBall_C) holds its
        // own SpringArm, Camera and Sphere (read from the type dump).
        Obj ballClass = eng::FindClass("BP_RollingBall_C");
        Obj ball = nullptr;
        if (ballClass)
            eng::ForEachObject([&](Obj o) {
                if (eng::ClassOf(o) == ballClass && !eng::IsDefaultObject(o) && eng::IsLive(o)) ball = o;
                return ball == nullptr;
            });
        if (ball) {
            arm = eng::ReadObj(ball, "SpringArm");
            camera = eng::ReadObj(ball, "Camera");
            tracked = eng::ReadObj(ball, "Sphere");
        }
    }
    if (!arm) {
        hostlog::Warn(std::string("ghosts: no camera rig for pov: ") + (found ? "BallAsyncCameraTargetComponent has no spring arm" : "no BallAsyncCameraTargetComponent"));
        return gRig;
    }
    struct Vec3 {
        double x, y, z;
    };
    float length = 0, fov = 0;
    Vec3 socket{}, target{};
    eng::ReadBytes(arm, "TargetArmLength", &length, sizeof length);
    eng::ReadBytes(arm, "SocketOffset", &socket, sizeof socket);
    eng::ReadBytes(arm, "TargetOffset", &target, sizeof target);
    if (camera) eng::ReadBytes(camera, "FieldOfView", &fov, sizeof fov);
    const Vec3 armAt = eng::Call(arm, "K2_GetComponentLocation").ReturnAs<Vec3>();
    const Vec3 ballAt = tracked ? eng::Call(tracked, "K2_GetComponentLocation").ReturnAs<Vec3>() : armAt;
    gRig.ok = true;
    gRig.arm = length;
    gRig.fov = fov > 0 ? fov : 90;
    gRig.origin = {armAt.x - ballAt.x, armAt.y - ballAt.y, armAt.z - ballAt.z};
    gRig.target = {target.x, target.y, target.z};
    gRig.socket = {socket.x, socket.y, socket.z};
    hostlog::Info("ghosts: camera rig: arm " + std::to_string(length) + ", fov " + std::to_string(gRig.fov) + ", socket z " +
                  std::to_string(socket.z) + ", target z " + std::to_string(target.z));
    return gRig;
}
}  // namespace

bool View(size_t ghost, double t, double out[6]) {
    if (ghost >= gGhosts.size()) return false;
    const auto& replay = gGhosts[ghost].replay;
    double pitch = 0, yaw = 0;
    if (!ghostdata::ViewAt(replay, t, &pitch, &yaw)) return false;
    const Rig& rig = ReadRig();
    if (!rig.ok) return false;
    // USpringArmComponent: the arm starts at its origin plus TargetOffset (world space), points back along the
    // rotation for TargetArmLength, then SocketOffset (in the rotation's space) moves its end.
    const double p = pitch * 3.14159265358979 / 180, y = yaw * 3.14159265358979 / 180;
    const double fx = std::cos(p) * std::cos(y), fy = std::cos(p) * std::sin(y), fz = std::sin(p);
    const double rx = -std::sin(y), ry = std::cos(y);
    const double ux = -std::sin(p) * std::cos(y), uy = -std::sin(p) * std::sin(y), uz = std::cos(p);
    const ghostdata::Point ball = ghostdata::At(replay, t);
    const double sx = rig.socket.x - rig.arm, sy = rig.socket.y, sz = rig.socket.z;
    out[0] = ball.x + rig.origin.x + rig.target.x + fx * sx + rx * sy + ux * sz;
    out[1] = ball.y + rig.origin.y + rig.target.y + fy * sx + ry * sy + uy * sz;
    out[2] = ball.z + rig.origin.z + rig.target.z + fz * sx + uz * sz;
    out[3] = pitch;
    out[4] = yaw;
    out[5] = rig.fov;
    return true;
}

namespace {
Obj LoadPath(const std::string& written) {
    const std::string path = ghostdata::ObjectPath(written);
    return path.empty() ? nullptr : cosmetics::LoadAsset(eng::Widen(path));
}
}  // namespace

int PlayerBall(int owner, size_t ghost, bool realSkin) {
    if (ghost >= gGhosts.size()) return 0;
    return PlayerBallFor(owner, gGhosts[ghost].replay, realSkin);
}

int PlayerBallFor(int owner, const ghostdata::Replay& replay, bool realSkin) {
    Obj controller = game::PlayerController();
    Obj cls = cosmetics::LoadAsset(L"/Game/SocketIO/BP_NonPlayerRollingBall.BP_NonPlayerRollingBall_C");
    if (!controller || !cls) {
        hostlog::Warn("ghosts: the game's player ball (BP_NonPlayerRollingBall_C) isn't available");
        return 0;
    }
    struct Transform {
        uint8_t bytes[96];
    } t{};
    struct Vec3 {
        double x, y, z;
    };
    const Vec3 zero{0, 0, 0}, one{1, 1, 1};
    const eng::Params made = eng::Call(eng::FindCdo("KismetMathLibrary"), "MakeTransform", zero, zero, one);
    if (const uint8_t* r = made.Return()) std::memcpy(t.bytes, r, sizeof t.bytes);
    Obj statics = eng::FindCdo("GameplayStatics");
    eng::Params begin(eng::FunctionOn(statics, "BeginDeferredActorSpawnFromClass"));
    begin.Set("WorldContextObject", controller);
    begin.Set("ActorClass", cls);
    begin.Set("SpawnTransform", t);
    begin.Set("CollisionHandlingOverride", uint8_t{1});
    begin.Set("TransformScaleMethod", uint8_t{1});
    eng::Invoke(statics, begin);
    Obj ball = begin.ReturnObj();
    if (!ball) return 0;
    eng::Params finish(eng::FunctionOn(statics, "FinishSpawningActor"));
    finish.Set("Actor", ball);
    finish.Set("SpawnTransform", t);
    finish.Set("TransformScaleMethod", uint8_t{1});
    eng::Invoke(statics, finish);
    // Its collision would push the player's ball about: none.
    eng::Call(ball, "SetActorEnableCollision", uint8_t{0});
    const auto& look = replay.look;
    Obj skin = realSkin ? LoadPath(look.skinMaterial) : LoadPath(look.ghostSkinMaterial);
    if (!skin) skin = realSkin ? LoadPath(look.ghostSkinMaterial) : LoadPath(look.skinMaterial);
    if (realSkin) {
        // BP_NonPlayerRollingBall's tick sets GhostOpacity = clamp(distance to your ball / MaxOpacityDistance, MinOpacity,
        // MaxOpacity) on the skin, the accessory and a special skin's child (read from its blueprint): pinned at 1
        const double full = 1.0;
        eng::WriteBytes(ball, "MinOpacity", &full, sizeof full);
        eng::WriteBytes(ball, "MaxOpacity", &full, sizeof full);
    }
    eng::Params dress(eng::FunctionOn(ball, "CreateNonPlayerRollingBall"));
    const std::wstring name = eng::Widen(replay.name);
    dress.Set("PlayerName", eng::FString{name.c_str(), static_cast<int32_t>(name.size() + 1), static_cast<int32_t>(name.size() + 1)});
    dress.Set("Skin Material", skin);
    dress.Set("AccessoryMesh", LoadPath(look.accessory));
    dress.Set("?AccessoryMaterial", realSkin ? Obj{} : LoadPath(look.accessoryGhostMaterial));   // (none: the mesh's own)
    dress.Set("?SpecialSkinClass", LoadPath(look.specialSkinClass));
    dress.Set("bPersonalBest", uint8_t{0});
    uint8_t prefs[16] = {};                     // SBallerSkinPreferences: texture @0, gloss @1, slider (double) @8
    prefs[0] = look.basicTexture;
    prefs[1] = look.basicGloss;
    std::memcpy(prefs + 8, &look.textureSlider, sizeof look.textureSlider);
    dress.Set("BasicBallPrefs", prefs, sizeof prefs);
    eng::Invoke(ball, dress);
    if (realSkin)
        hostlog::Info("ghosts: " + replay.name + "'s ball in its real skin " + (skin ? ghostdata::ObjectPath(look.skinMaterial) : std::string("(none loaded)")) +
                      (look.specialSkinClass.empty() || look.specialSkinClass == "None" ? "" : ", special " + ghostdata::ObjectPath(look.specialSkinClass)) +
                      (ghostdata::ObjectPath(look.accessory).empty() ? "" : ", accessory " + ghostdata::ObjectPath(look.accessory)));
    return draw::Adopt(owner, ball);
}

bool PlacePlayerBall(int owner, int id, size_t ghost, double t) {
    if (ghost >= gGhosts.size()) return false;
    return PlaceBallFor(owner, id, gGhosts[ghost].replay, t);
}

bool PlaceBallFor(int owner, int id, const ghostdata::Replay& replay, double t) {
    Obj ball = draw::ActorOf(owner, id);
    if (!ball) return false;
    const ghostdata::Point at = ghostdata::At(replay, t);
    const ghostdata::Sample* s = ghostdata::SampleAt(replay, t);
    struct Vec3 {
        double x, y, z;
    };
    struct Rot {
        double pitch, yaw, roll;
    };
    eng::Params move(eng::FunctionOn(ball, "K2_SetActorLocation"));
    move.Set("NewLocation", Vec3{at.x, at.y, at.z});
    move.Set("bSweep", uint8_t{0});
    move.Set("bTeleport", uint8_t{1});
    eng::Invoke(ball, move);
    if (!s) return true;
    eng::Params target(eng::FunctionOn(ball, "UpdateTargetTransform"));
    target.Set("TargetLocation", Vec3{at.x, at.y, at.z});
    target.Set("TargetVelocity", Vec3{s->velocity.x, s->velocity.y, s->velocity.z});
    target.Set("TargetBallAxisRotation", Rot{s->ballPitch, s->ballYaw, s->ballRoll});
    target.Set("TargetControlRotation", Rot{s->pitch, s->yaw, 0});
    return eng::Invoke(ball, target);
}

bool ShowPlayerName(int owner, int id, bool shown) {
    Obj ball = draw::ActorOf(owner, id);
    Obj label = ball ? eng::ReadObj(ball, "PlayerLabel") : nullptr;
    Obj widget = label ? eng::Call(label, "GetUserWidgetObject").ReturnObj() : nullptr;
    return widget && eng::Call(widget, "SetRenderOpacity", shown ? 1.0f : 0.0f).Invoked();
}

// --- crowds ---
namespace {
struct CrowdGroup {
    eng::Weak component;                // an InstancedStaticMeshComponent
    std::vector<int> ghosts;            // its members, in instance order
    std::vector<uint8_t> transforms;    // their FTransforms, kTransformSize bytes each
    bool skinned = false;               // the game's ball in one skin, full size and rolling as the replay did
    std::string skin;                   // a skinned group's material path
    bool keepColour = false;            // CrowdGroupGlass: never moved to a skin group
};
// Skinned groups past this many would each be another draw: the rest keep their colour.
constexpr size_t kMaxSkinGroups = 400;
struct Crowd {
    int owner = -1;
    double scale = 1;
    std::vector<CrowdGroup> groups;
    std::vector<double> offsets;
    std::vector<bool> shown;
    std::vector<float> palette;         // r, g, b per colour group
    std::vector<int> colourOf;          // per ghost, its colour group (-1: not a member)
    // Trails (CrowdTrails): members still to draw, in rank order, and per colour the mesh being filled and how many
    // trails it has.
    std::deque<int> trailQueue;
    double trailRadius = 0;
    float trailOpacity = 1;
    // With trailChunk (seconds), each run's trail is cut into pieces by its own clock, and the pieces of a chunk share
    // meshes of their own, shown once playback is past the chunk's end (CrowdTrailsUpTo): the trails so far.
    double trailChunk = 0, trailsUpTo = 1e18;
    struct TrailMesh {
        int id = 0;
        int chunk = -1;                 // -1: whole trails
        int pieces = 0;
        bool visible = true;
        int fadeLevel = -1;             // the opacity step last given (CrowdTrailStyle fading)
    };
    std::vector<TrailMesh> trailMeshes;
    std::map<std::pair<int, int>, size_t> trailOpen;    // (colour, chunk): the mesh being filled, in trailMeshes
    bool trailsShown = true;
    // CrowdTrailStyle: glowing trails (brightness), and chunk pieces fading with age to fadeMin over fadeSeconds
    bool trailGlow = false;
    float trailBright = 5;
    double fadeSeconds = 0;
    float fadeMin = 0;
    double skinScale = 1;               // CrowdBallScale: skinned balls' size (1 = the game's ball)
};
// Trails per mesh: each mesh is one object to draw, and a tube added rebuilds only its own mesh. In chunks, the pieces
// are short, so a mesh takes more of them.
constexpr int kTrailsPerMesh = 5, kPiecesPerChunkMesh = 50;
// A crowd trail's points at most (evenly through the run), and the time spent adding trails a frame.
size_t kTrailPoints = 160;
double kTrailBudgetMs = 3;
constexpr double kTrailSpacing = 25;   // cm
std::map<int, Crowd> gCrowds;
// FTransform as this build lays it out, measured once from KismetMathLibrary.MakeTransform: its size and where
// the translation and scale doubles are (the rotation, identity, is taken from the measured transform itself).
size_t kTransformSize = 0;
int gTranslationAt = -1, gScaleAt = -1;
std::vector<uint8_t> gIdentity;

bool MeasureTransform() {
    if (kTransformSize) return true;
    struct Vec3 {
        double x, y, z;
    };
    const Vec3 at{1111.5, 2222.5, 3333.5}, rotation{0, 0, 0}, scale{4.25, 5.25, 6.25};
    const eng::Params made = eng::Call(eng::FindCdo("KismetMathLibrary"), "MakeTransform", at, rotation, scale);
    size_t size = 0;
    const uint8_t* bytes = made.Return(&size);
    if (!bytes || size < 80) return false;
    for (size_t o = 0; o + 24 <= size; o += 8) {
        double v[3];
        std::memcpy(v, bytes + o, sizeof v);
        if (v[0] == at.x && v[1] == at.y && v[2] == at.z) gTranslationAt = static_cast<int>(o);
        if (v[0] == scale.x && v[1] == scale.y && v[2] == scale.z) gScaleAt = static_cast<int>(o);
    }
    if (gTranslationAt < 0 || gScaleAt < 0) return false;
    kTransformSize = size;
    gIdentity.assign(bytes, bytes + size);

    hostlog::Info("ghosts: FTransform is " + std::to_string(size) + " bytes, translation at " + std::to_string(gTranslationAt) +
                  ", scale at " + std::to_string(gScaleAt));
    return true;
}

// A TArray view of our own buffer, for passing to a function that only reads it.
struct ArrayView {
    const void* data;
    int32_t num, max;
};

Obj Component(const CrowdGroup& g) { return eng::Get(g.component); }

void Place(Crowd& c, CrowdGroup& g, double t) {
    const size_t n = g.ghosts.size();
    if (!n) return;
    g.transforms.resize(n * kTransformSize);
    for (size_t k = 0; k < n; ++k) {
        uint8_t* at = g.transforms.data() + k * kTransformSize;
        std::memcpy(at, gIdentity.data(), kTransformSize);
        const int ghost = g.ghosts[k];
        const bool on = ghost >= 0 && static_cast<size_t>(ghost) < gGhosts.size() && static_cast<size_t>(ghost) < c.shown.size() && c.shown[ghost];
        double where[3] = {0, 0, 0}, size[3] = {0, 0, 0};
        if (on) {
            const double offset = static_cast<size_t>(ghost) < c.offsets.size() ? c.offsets[ghost] : 0;
            const auto& replay = gGhosts[static_cast<size_t>(ghost)].replay;
            const ghostdata::Point p = ghostdata::At(replay, t + offset);
            where[0] = p.x;
            where[1] = p.y;
            where[2] = p.z;
            // A skinned ball is the game's ball size (the sphere is 100 cm across, like it). It doesn't roll: a crowd ball
            // turned to the replay's rotation drew as an egg, even paused (measured, the balls round without it), and
            // not from the rotation written (the engine's own MakeTransform drew the same), motion blur (off: the same),
            // the teleport or render-state flags, or the mesh (SM_PlayerBall and the sphere alike).
            size[0] = size[1] = size[2] = g.skinned ? c.skinScale : c.scale;
        }
        std::memcpy(at + gTranslationAt, where, sizeof where);
        std::memcpy(at + gScaleAt, size, sizeof size);
    }
    Obj component = Component(g);
    if (!component) return;
    eng::Params p(eng::FunctionOn(component, "BatchUpdateInstancesTransforms"));
    const ArrayView view{g.transforms.data(), static_cast<int32_t>(n), static_cast<int32_t>(n)};
    p.Set("StartInstanceIndex", int32_t{0});
    p.Set("NewInstancesTransforms", &view, sizeof view);
    p.Set("bWorldSpace", uint8_t{1});
    p.Set("bMarkRenderStateDirty", uint8_t{1});
    p.Set("bTeleport", uint8_t{1});
    eng::Invoke(component, p);
}
// Another instanced mesh on the crowd's actor, in this mesh and material.
Obj AddGroupComponent(Obj actor, Obj mesh, Obj material) {
    eng::Params add(eng::FunctionOn(actor, "AddComponentByClass"));
    add.Set("Class", eng::FindClass("InstancedStaticMeshComponent"));
    add.Set("bManualAttachment", uint8_t{0});
    add.Set("RelativeTransform", gIdentity.data(), kTransformSize);
    add.Set("bDeferredFinish", uint8_t{0});
    eng::Invoke(actor, add);
    Obj component = add.ReturnObj();
    if (!component) return nullptr;
    eng::Call(component, "SetStaticMesh", mesh);
    eng::Call(component, "SetCollisionEnabled", uint8_t{0});
    eng::Call(component, "SetCastShadow", uint8_t{0});
    if (material) eng::Call(component, "SetMaterial", int32_t{0}, material);
    return component;
}

// A group's instances made again for its members, all hidden (size 0) until placed.
void Refill(CrowdGroup& g) {
    Obj component = Component(g);
    if (!component) return;
    eng::Call(component, "ClearInstances");
    const size_t n = g.ghosts.size();
    if (!n) return;
    std::vector<uint8_t> hidden(n * kTransformSize);
    const double zero[3] = {0, 0, 0};
    for (size_t k = 0; k < n; ++k) {
        std::memcpy(hidden.data() + k * kTransformSize, gIdentity.data(), kTransformSize);
        std::memcpy(hidden.data() + k * kTransformSize + gScaleAt, zero, sizeof zero);
    }
    eng::Params add(eng::FunctionOn(component, "AddInstances"));
    const ArrayView view{hidden.data(), static_cast<int32_t>(n), static_cast<int32_t>(n)};
    add.Set("InstanceTransforms", &view, sizeof view);
    add.Set("bShouldReturnIndices", uint8_t{0});
    add.Set("bWorldSpace", uint8_t{1});
    add.Set("bUpdateNavigation", uint8_t{0});
    eng::Invoke(component, add);
}
}  // namespace

void ForgetCrowds() { gCrowds.clear(); }

void CrowdTrailDetail(int points, double budgetMs) {
    kTrailPoints = static_cast<size_t>(std::clamp(points, 8, 2000));
    kTrailBudgetMs = std::clamp(budgetMs, 0.5, 40.0);
}
bool CrowdTrailStyle(int owner, int id, bool glow, float brightness, double fadeSeconds, float fadeMin) {
    auto it = gCrowds.find(id);
    if (it == gCrowds.end() || it->second.owner != owner) return false;
    it->second.trailGlow = glow;
    it->second.trailBright = brightness;
    it->second.fadeSeconds = fadeSeconds;
    it->second.fadeMin = fadeMin;
    for (auto& m : it->second.trailMeshes) m.fadeLevel = -1;
    return true;
}
bool CrowdGroupGlass(int owner, int id, int group, float opacity) {
    auto it = gCrowds.find(id);
    if (it == gCrowds.end() || it->second.owner != owner || group < 0) return false;
    Crowd& c = it->second;
    if (static_cast<size_t>(group) >= c.groups.size() || static_cast<size_t>(group) * 3 + 2 >= c.palette.size()) return false;
    CrowdGroup& g = c.groups[static_cast<size_t>(group)];
    Obj component = Component(g);
    if (!component) return false;
    const size_t k = static_cast<size_t>(group) * 3;
    Obj material = opacity < 1 ? models::NewGlassMaterial(c.palette[k], c.palette[k + 1], c.palette[k + 2], opacity)
                               : models::NewGlowMaterial(c.palette[k], c.palette[k + 1], c.palette[k + 2], 8);
    if (material) eng::Call(component, "SetMaterial", int32_t{0}, material);
    if (opacity < 1) eng::Call(component, "SetTranslucentSortPriority", int32_t{10});
    g.keepColour = opacity < 1;
    return material != nullptr;
}
bool CrowdBallScale(int owner, int id, double scale) {
    auto it = gCrowds.find(id);
    if (it == gCrowds.end() || it->second.owner != owner) return false;
    it->second.skinScale = scale;
    return true;
}
bool CrowdTrailsPending(int owner) {
    for (const auto& [id, c] : gCrowds)       // (a crowd removed with Draw::Remove builds nothing more)
        if (c.owner == owner && !c.trailQueue.empty() && draw::ActorOf(c.owner, id)) return true;
    return false;
}

int CrowdCreate(int owner, double radius, const std::vector<float>& palette) {
    Obj controller = game::PlayerController();
    Obj mesh = cosmetics::LoadAsset(L"/Engine/BasicShapes/Sphere.Sphere");
    Obj actorClass = eng::FindClass("Actor"), ismClass = eng::FindClass("InstancedStaticMeshComponent");
    if (!controller || !mesh || !actorClass || !ismClass || !MeasureTransform()) {
        hostlog::Warn("ghosts: a crowd can't be made here");
        return 0;
    }
    Obj statics = eng::FindCdo("GameplayStatics");
    eng::Params begin(eng::FunctionOn(statics, "BeginDeferredActorSpawnFromClass"));
    begin.Set("WorldContextObject", controller);
    begin.Set("ActorClass", actorClass);
    begin.Set("SpawnTransform", gIdentity.data(), kTransformSize);
    begin.Set("CollisionHandlingOverride", uint8_t{1});
    begin.Set("TransformScaleMethod", uint8_t{1});
    eng::Invoke(statics, begin);
    Obj actor = begin.ReturnObj();
    if (!actor) return 0;
    eng::Params finish(eng::FunctionOn(statics, "FinishSpawningActor"));
    finish.Set("Actor", actor);
    finish.Set("SpawnTransform", gIdentity.data(), kTransformSize);
    finish.Set("TransformScaleMethod", uint8_t{1});
    eng::Invoke(statics, finish);
    Crowd c;
    c.owner = owner;
    c.scale = radius * 2 / 100;         // the engine's sphere is 100 cm across
    c.palette = palette;
    for (size_t k = 0; k + 2 < palette.size(); k += 3) {
        Obj component = AddGroupComponent(actor, mesh, models::NewGlowMaterial(palette[k], palette[k + 1], palette[k + 2], 8));
        if (!component) continue;
        CrowdGroup g;
        g.component = eng::MakeWeak(component);
        c.groups.push_back(std::move(g));
    }
    const int id = draw::Adopt(owner, actor);
    if (id) gCrowds[id] = std::move(c);
    return id;
}

bool CrowdMembers(int owner, int id, const std::vector<int>& ghosts, const std::vector<int>& groups) {
    auto it = gCrowds.find(id);
    if (it == gCrowds.end() || it->second.owner != owner || !draw::ActorOf(owner, id)) return false;
    Crowd& c = it->second;
    for (auto& g : c.groups) g.ghosts.clear();
    c.colourOf.assign(gGhosts.size(), -1);
    for (size_t k = 0; k < ghosts.size() && k < groups.size(); ++k)
        if (ghosts[k] >= 0 && static_cast<size_t>(ghosts[k]) < c.colourOf.size()) c.colourOf[static_cast<size_t>(ghosts[k])] = groups[k];
    for (size_t k = 0; k < ghosts.size() && k < groups.size(); ++k)
        if (groups[k] >= 0 && static_cast<size_t>(groups[k]) < c.groups.size()) c.groups[static_cast<size_t>(groups[k])].ghosts.push_back(ghosts[k]);
    for (auto& g : c.groups) Refill(g);
    return true;
}

bool CrowdSkins(int owner, int id) {
    auto it = gCrowds.find(id);
    Obj actor = draw::ActorOf(owner, id);
    if (it == gCrowds.end() || it->second.owner != owner || !actor) return false;
    Crowd& c = it->second;
    // The engine's sphere (960 triangles, 100 cm across like the game's ball): SM_PlayerBall is 196,608 triangles at LOD 0
    // (999 runs drew at 40 fps; on the sphere 82, measured) and its LOD 1 is flat (reported: the balls looked 2D).
    Obj mesh = cosmetics::LoadAsset(L"/Engine/BasicShapes/Sphere.Sphere");
    if (!mesh) {
        hostlog::Warn("ghosts: the crowd can't wear skins here");
        return false;
    }
    const auto started = std::chrono::steady_clock::now();
    // Each member to the group of its skin (the ghost version, as the player balls wear it), made the first time the
    // skin is met; a skin that doesn't load, or one past kMaxSkinGroups, keeps the member in its colour.
    std::map<std::string, size_t> byPath;
    constexpr size_t kNone = static_cast<size_t>(-1);
    size_t coloured = c.groups.size();
    // Skinned groups made by an earlier call are used again (members change with CrowdMembers; their components stay).
    for (size_t gi = 0; gi < c.groups.size(); ++gi)
        if (c.groups[gi].skinned) {
            byPath.emplace(c.groups[gi].skin, gi);
            coloured = std::min(coloured, gi);
        }
    int moved = 0;
    for (size_t gi = 0; gi < coloured; ++gi) {
        if (c.groups[gi].skinned || c.groups[gi].keepColour) continue;
        std::vector<int> keep;
        const std::vector<int> members = c.groups[gi].ghosts;
        for (int ghost : members) {
            size_t to = kNone;
            if (ghost >= 0 && static_cast<size_t>(ghost) < gGhosts.size()) {
                const auto& look = gGhosts[static_cast<size_t>(ghost)].replay.look;
                std::string path = ghostdata::ObjectPath(look.ghostSkinMaterial);
                if (path.empty()) path = ghostdata::ObjectPath(look.skinMaterial);
                if (!path.empty()) {
                    auto found = byPath.find(path);
                    if (found == byPath.end()) {
                        size_t made = kNone;
                        Obj material = c.groups.size() < coloured + kMaxSkinGroups ? cosmetics::LoadAsset(eng::Widen(path)) : nullptr;
                        if (Obj component = material ? AddGroupComponent(actor, mesh, material) : nullptr) {
                            CrowdGroup g;
                            g.component = eng::MakeWeak(component);
                            g.skinned = true;
                            g.skin = path;
                            c.groups.push_back(std::move(g));
                            made = c.groups.size() - 1;
                        }
                        found = byPath.emplace(path, made).first;
                    }
                    to = found->second;
                }
            }
            if (to == kNone) {
                keep.push_back(ghost);
            } else {
                c.groups[to].ghosts.push_back(ghost);
                ++moved;
            }
        }
        c.groups[gi].ghosts = std::move(keep);
    }
    for (auto& g : c.groups) Refill(g);
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    hostlog::Info("ghosts: " + std::to_string(moved) + " crowd balls in " + std::to_string(c.groups.size() - coloured) + " skins (" +
                  std::to_string(took) + " ms)");
    return true;
}

namespace {
bool TrailMeshWanted(const Crowd& c, const Crowd::TrailMesh& m) {
    return c.trailsShown && (m.chunk < 0 || (m.chunk + 1) * c.trailChunk <= c.trailsUpTo);
}

void ShowTrailMeshes(Crowd& c) {
    for (auto& m : c.trailMeshes) {
        const bool want = TrailMeshWanted(c, m);
        if (want == m.visible) continue;
        draw::Show(c.owner, m.id, want);
        m.visible = want;
    }
}
}  // namespace

bool CrowdTrails(int owner, int id, double radius, float opacity, double chunkSeconds) {
    auto it = gCrowds.find(id);
    if (it == gCrowds.end() || it->second.owner != owner) return false;
    Crowd& c = it->second;
    // Built again from nothing: the trails already made go, and only the members shown (CrowdTimes) get one, so
    // showing fewer runs takes their trails away too (a mesh holds several, so they can't be hidden one by one).
    for (const auto& m : c.trailMeshes) draw::Remove(owner, m.id);
    c.trailMeshes.clear();
    c.trailOpen.clear();
    c.trailQueue.clear();
    for (size_t ghost = 0; ghost < c.colourOf.size(); ++ghost)
        if (c.colourOf[ghost] >= 0 && (ghost >= c.shown.size() || c.shown[ghost])) c.trailQueue.push_back(static_cast<int>(ghost));
    c.trailRadius = radius;
    c.trailOpacity = opacity;
    c.trailChunk = chunkSeconds > 0 ? chunkSeconds : 0;
    return true;
}

bool CrowdTrailsUpTo(int owner, int id, double t) {
    auto it = gCrowds.find(id);
    if (it == gCrowds.end() || it->second.owner != owner) return false;
    Crowd& c = it->second;
    c.trailsUpTo = t;
    ShowTrailMeshes(c);
    if (c.fadeSeconds > 0 && c.trailChunk > 0 && c.trailOpacity < 1) {
        constexpr int kLevels = 12;
        for (auto& m : c.trailMeshes) {
            if (!m.visible || m.chunk < 0) continue;
            const double age = t - (m.chunk + 1) * c.trailChunk;
            const double f = std::clamp(1 - age / c.fadeSeconds, 0.0, 1.0);
            const int level = static_cast<int>(std::lround(f * kLevels));
            if (level == m.fadeLevel) continue;
            m.fadeLevel = level;
            const float op = c.fadeMin + (c.trailOpacity - c.fadeMin) * static_cast<float>(level) / kLevels;
            draw::Fade(owner, m.id, op);
        }
    }
    return true;
}

// Some of each crowd's queued trails, within kTrailBudgetMs.
void BuildCrowdTrails() {
    const auto started = std::chrono::steady_clock::now();
    auto spent = [&] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count(); };
    for (auto& [id, c] : gCrowds) {
        if (c.trailQueue.empty()) continue;
        if (!draw::ActorOf(c.owner, id)) {      // removed (Draw::Remove): nothing left to build for
            c.trailQueue.clear();
            continue;
        }
        while (!c.trailQueue.empty() && spent() < kTrailBudgetMs) {
            const int ghost = c.trailQueue.front();
            c.trailQueue.pop_front();
            if (ghost < 0 || static_cast<size_t>(ghost) >= gGhosts.size() || static_cast<size_t>(ghost) >= c.colourOf.size()) continue;
            const int colour = c.colourOf[static_cast<size_t>(ghost)];
            if (colour < 0 || static_cast<size_t>(colour) * 3 + 2 >= c.palette.size()) continue;
            const auto& samples = gGhosts[static_cast<size_t>(ghost)].replay.samples;
            const size_t n = samples.size();
            if (n < 2) continue;
            // Thinned to kTrailPoints, and no two points closer than kTrailSpacing: a ball sitting nearly still (at the
            // start and finish) jitters by a centimetre or two, and a tube swept through those points threw spikes
            // across the whole track (measured, with 1 cm apart allowed).
            const size_t stride = (n + kTrailPoints - 1) / kTrailPoints;
            std::vector<std::array<double, 3>> path;
            std::vector<double> times;
            for (size_t k = 0; k < n; k += stride) {
                const auto& sample = samples[k + stride >= n ? n - 1 : k];
                const auto& p = sample.at;
                if (!path.empty()) {
                    const auto& q = path.back();
                    const double dx = p.x - q[0], dy = p.y - q[1], dz = p.z - q[2];
                    if (dx * dx + dy * dy + dz * dz < kTrailSpacing * kTrailSpacing) continue;
                }
                path.push_back({p.x, p.y, p.z});
                times.push_back(sample.t);
            }
            if (path.size() < 2) continue;
            // Whole, or in pieces by chunk of the run's clock; each piece starts where the one before ended.
            std::vector<std::pair<int, std::vector<std::array<double, 3>>>> pieces;
            if (c.trailChunk <= 0) {
                pieces.push_back({-1, path});
            } else {
                for (size_t k = 0; k < path.size(); ++k) {
                    const int chunk = static_cast<int>(times[k] / c.trailChunk);
                    if (pieces.empty() || pieces.back().first != chunk) {
                        if (!pieces.empty()) pieces.back().second.push_back(path[k]);
                        pieces.push_back({chunk, {}});
                        if (k > 0) pieces.back().second.push_back(path[k - 1]);
                    }
                    pieces.back().second.push_back(path[k]);
                }
            }
            const int colourIndex = colour;
            const size_t ci = static_cast<size_t>(colour);
            for (auto& [chunk, piece] : pieces) {
                if (piece.size() < 2) continue;
                const auto key = std::make_pair(colourIndex, chunk);
                auto open = c.trailOpen.find(key);
                Obj mesh = nullptr;
                const int limit = chunk < 0 ? kTrailsPerMesh : kPiecesPerChunkMesh;
                if (open != c.trailOpen.end() && c.trailMeshes[open->second].pieces < limit)
                    mesh = draw::ActorOf(c.owner, c.trailMeshes[open->second].id);
                if (!mesh) {
                    models::Colour look;
                    look.r = c.palette[ci * 3];
                    look.g = c.palette[ci * 3 + 1];
                    look.b = c.palette[ci * 3 + 2];
                    look.opacity = c.trailOpacity;
                    look.glow = c.trailGlow && c.trailOpacity >= 1;
                    look.bright = c.trailBright;
                    mesh = models::SpawnMesh(look);
                    if (!mesh) continue;
                    Crowd::TrailMesh m;
                    m.id = draw::Adopt(c.owner, mesh);
                    m.chunk = chunk;
                    c.trailMeshes.push_back(m);
                    c.trailOpen[key] = c.trailMeshes.size() - 1;
                    open = c.trailOpen.find(key);
                    if (!TrailMeshWanted(c, c.trailMeshes.back())) {
                        draw::Show(c.owner, m.id, false);
                        c.trailMeshes.back().visible = false;
                    }
                }
                models::AppendTube(mesh, piece, c.trailRadius, 4);
                ++c.trailMeshes[open->second].pieces;
            }
        }
        if (c.trailQueue.empty()) hostlog::Info("ghosts: the crowd's trails are drawn");
        break;                          // one crowd a frame
    }
}

bool CrowdShowTrails(int owner, int id, bool shown) {
    auto it = gCrowds.find(id);
    if (it == gCrowds.end() || it->second.owner != owner) return false;
    it->second.trailsShown = shown;
    ShowTrailMeshes(it->second);
    return true;
}

bool CrowdTimes(int owner, int id, const std::vector<double>& offsets, const std::vector<bool>& shown) {
    auto it = gCrowds.find(id);
    if (it == gCrowds.end() || it->second.owner != owner) return false;
    it->second.offsets = offsets;
    it->second.shown = shown;
    return true;
}

bool CrowdPlace(int owner, int id, double t) {
    auto it = gCrowds.find(id);
    if (it == gCrowds.end() || it->second.owner != owner) return false;
    if (!draw::ActorOf(owner, id)) {
        gCrowds.erase(it);              // removed with Draw::Remove / Clear, or went with the map
        return false;
    }
    for (auto& g : it->second.groups) Place(it->second, g, t);
    return true;
}

std::vector<int> Order(size_t ghost) {
    if (ghost >= gGhosts.size()) return {};
    Ghost& g = gGhosts[ghost];
    const auto checkpoints = Checkpoints();
    if (g.orderOf != static_cast<int>(checkpoints.size())) {     // not worked out yet, or the checkpoints changed since
        std::vector<ghostdata::Point> points;
        for (const auto& c : checkpoints) points.push_back(c.at);
        g.order.clear();
        for (int i : ghostdata::CheckpointOrder(g.replay, points)) g.order.push_back(i < 0 ? 0 : checkpoints[static_cast<size_t>(i)].number);
        g.orderOf = static_cast<int>(checkpoints.size());
    }
    return g.order;
}

}  // namespace ghosts
