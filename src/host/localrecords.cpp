#include "localrecords.hpp"

#include <cstring>
#include <set>
#include <vector>

#include "engine.hpp"
#include "game.hpp"
#include "layout.hpp"
#include "log.hpp"
#include "race.hpp"
#include "tracks.hpp"

namespace localrecords {
namespace {

using eng::Obj;
using eng::Params;
using NativeFunction = void (*)(Obj context, uint8_t* frame, void* result);

constexpr size_t kHighScoreSize = 312;          // SLevelHighScore (types dump)
constexpr size_t kHighScoreOffset = 0x20;       // SBallerProfile.HighScoreToSave
constexpr size_t kSkinPrefsOffset = 0x168;      // SBallerProfile.BallerSkinPrefs (16)

std::set<std::string> gSuppressed;
NativeFunction gCaptureOriginal = nullptr;
Obj gCaptureFunction = nullptr;

Obj GameInstance() {
    Obj controller = game::PlayerController();
    return controller ? eng::Call(eng::FindCdo("GameplayStatics"), "GetGameInstance", controller).ReturnObj() : nullptr;
}

bool OwnUnpublished(const std::string& levelName, std::string* error) {
    const long long id = levelName.empty() ? -2 : tracks::EditorMapPublishedId(levelName);
    if (id == -1 || id == 0) return true;
    if (error) *error = id > 0 ? "'" + levelName + "' has been published" : "'" + levelName + "' is not a saved map of the player's own";
    return false;
}

// Runs inside the game's own CaptureFinishPostingSnapshot (BallestScorePostingSubsystem, native: the controller takes
// it at the finish, and the profile save applies a finish's record only if CanApplyFinishPersonalBest(snapshot) or no
// snapshot was captured). On a suppressed level the snapshot comes back marked practice-only, so the game's own
// rules keep the run out of its records (measured: a hook on the Blueprint AsyncSaveProfileChanges is never run for
// the game's own Blueprint-to-Blueprint calls; a native function's entry is).
void HookedCapture(Obj context, uint8_t* frame, void* result) {
    gCaptureOriginal(context, frame, result);
    const race::Track& track = race::CurrentTrack();
    std::string why;
    // the name, and the map raced now is the player's own never-published one (not another track of the same name)
    if (result && race::OnTrack() && gSuppressed.count(track.name) && race::OwnUnpublishedMapHere(&why)) {
        static_cast<uint8_t*>(result)[8] = 1;        // BallestFinishPostingSnapshot.bPracticeOnly
        hostlog::Info("local records: the finish on '" + track.name + "' kept out of the game's records (suppressed)");
    }
}

bool WatchSaves(std::string* error) {
    if (gCaptureFunction) return true;
    Obj fn = eng::FindFunction(eng::FindClass("BallestScorePostingSubsystem"), "CaptureFinishPostingSnapshot");
    if (!fn) {
        if (error) *error = "the game's CaptureFinishPostingSnapshot wasn't found";
        return false;
    }
    NativeFunction entry = nullptr;
    std::memcpy(&entry, fn + layout::kUFunctionNativeFunctionOffset, sizeof entry);
    if (!entry || !eng::InImage(reinterpret_cast<void*>(entry))) {
        if (error) *error = "CaptureFinishPostingSnapshot is not as measured";
        return false;
    }
    gCaptureFunction = fn;
    gCaptureOriginal = entry;
    NativeFunction hooked = &HookedCapture;
    std::memcpy(fn + layout::kUFunctionNativeFunctionOffset, &hooked, sizeof hooked);
    hostlog::Info("local records: watching the game's finish snapshots");
    return true;
}

Obj Profile() {
    Obj gi = GameInstance();
    if (!gi) return nullptr;
    Params p(eng::FunctionOn(gi, "GetCleanBallerProfile"));
    if (!p.Fn() || !eng::Invoke(gi, p)) return nullptr;
    return p.GetObj("CleanProfile");
}


std::set<std::string> gPendingClears;           // asked for while the map wasn't raced: cleared when it is

// The level is the map raced (or test-played) now, and that map is the player's own never-published one: a record
// filed under the name is then that map's (the game keys records by display name only).
bool RacedNow(const std::string& levelName) {
    std::string why;
    return race::OnTrack() && race::CurrentTrack().name == levelName && race::OwnUnpublishedMapHere(&why);
}

bool ClearNow(const std::string& levelName, std::string* error) {
    Obj gi = GameInstance();
    Obj profile = Profile();
    if (!gi || !profile) {
        if (error) *error = "no game instance or profile";
        return false;
    }
    double best = 0;
    int splits = 0, samples = 0;
    if (!Read(levelName, &best, &splits, &samples)) return true;             // no record: nothing to clear (none made)
    if (best <= 0 && splits == 0 && samples == 0) return true;               // already empty
    Params save(eng::FunctionOn(gi, "AsyncSaveProfileChanges"));
    const int32_t size = save.SizeOf("SaveParams");
    if (!save.Fn() || size < static_cast<int32_t>(kSkinPrefsOffset + 16)) {
        if (error) *error = "AsyncSaveProfileChanges is not as measured";
        return false;
    }
    std::vector<uint8_t> params(static_cast<size_t>(size), 0);
    // the record: the level's name and nothing else (no time, medal, splits or replay)
    const std::wstring wide = eng::Widen(levelName);
    const eng::FString name{wide.c_str(), static_cast<int32_t>(wide.size() + 1), static_cast<int32_t>(wide.size() + 1)};
    std::memcpy(params.data() + kHighScoreOffset, &name, sizeof name);
    // every other field as the game skips it: no name, no objects, the welcome flag off, the profile's own skin
    // preferences (the queue copies them when they differ)
    uint8_t prefs[16] = {};
    if (!eng::ReadBytes(profile, "BallerSkinPreferences", prefs, sizeof prefs)) {
        if (error) *error = "the profile's skin preferences can't be read";
        return false;
    }
    std::memcpy(params.data() + kSkinPrefsOffset, prefs, sizeof prefs);
    save.Set("SaveParams", params.data(), params.size());
    save.Set("bSyncronous", uint8_t{0});
    const bool ok = eng::Invoke(gi, save);
    if (!ok) {
        if (error) *error = "the game's save request failed";
        return false;
    }
    hostlog::Info("local records: the game's record for '" + levelName + "' replaced by an empty one (saved by the game)");
    return true;
}

}  // namespace

bool Clear(const std::string& levelName, std::string* error) {
    if (!OwnUnpublished(levelName, error)) return false;
    if (RacedNow(levelName)) return ClearNow(levelName, error);
    if (gPendingClears.insert(levelName).second)
        hostlog::Info("local records: the game's record for '" + levelName + "' is cleared when that map is raced");
    return true;
}

void Frame() {
    if (gPendingClears.empty() || !race::OnTrack()) return;
    const std::string name = race::CurrentTrack().name;
    if (!gPendingClears.count(name) || !RacedNow(name)) return;
    gPendingClears.erase(name);
    std::string error;
    if (!ClearNow(name, &error)) hostlog::Warn("local records: clearing '" + name + "' failed: " + error);
}

bool Suppress(const std::string& levelName, std::string* error) {
    if (!OwnUnpublished(levelName, error) || !WatchSaves(error)) return false;
    if (gSuppressed.insert(levelName).second) hostlog::Info("local records: the game saves no record for '" + levelName + "' this session");
    return true;
}

void Unsuppress(const std::string& levelName) { gSuppressed.erase(levelName); }

bool Read(const std::string& levelName, double* best, int* splits, int* samples) {
    *best = 0;
    *splits = *samples = 0;
    Obj profile = Profile();
    const eng::Prop map = profile ? eng::FindProp(eng::ClassOf(profile), "HighScores") : eng::Prop{};
    if (!map) return false;
    // TMap<FString, SLevelHighScore> as UE lays it out: a sparse array of set elements (the pair, then the hash's
    // next id and index), its allocation bits in a TBitArray (4 inline words, else a heap pointer)
    const uint8_t* m = profile + map.offset;
    const uint8_t* data = nullptr;
    int32_t num = 0, maxBits = 0;
    std::memcpy(&data, m, 8);
    std::memcpy(&num, m + 8, 4);
    const uint8_t* bits = m + 0x10;
    std::memcpy(&maxBits, m + 0x10 + 16 + 8 + 4, 4);
    if (maxBits > 128) std::memcpy(&bits, m + 0x10 + 16, 8);
    constexpr size_t kStride = 16 + kHighScoreSize + 8;
    for (int32_t i = 0; data && bits && i < num && i < 100000; ++i) {
        if (!(bits[i / 8] & (1 << (i % 8)))) continue;
        const uint8_t* element = data + static_cast<size_t>(i) * kStride;
        if (eng::ReadFString(element) != levelName) continue;
        const uint8_t* record = element + 16;
        std::memcpy(best, record + 0x10, 8);                    // BestTime
        int32_t n = 0;
        std::memcpy(&n, record + 0x108 + 8, 4);                 // ?CheckpointSplits (Array<Double>) Num
        *splits = n;
        std::memcpy(&n, record + 0xa0 + 8, 4);                  // Locations Num
        *samples = n;
        return true;
    }
    return false;
}

int Medal(const std::string& levelName) {
    Obj profile = Profile();
    const eng::Prop map = profile ? eng::FindProp(eng::ClassOf(profile), "LevelNameMedalMap") : eng::Prop{};
    if (!map) return -1;
    const uint8_t* m = profile + map.offset;
    const uint8_t* data = nullptr;
    int32_t num = 0, maxBits = 0;
    std::memcpy(&data, m, 8);
    std::memcpy(&num, m + 8, 4);
    const uint8_t* bits = m + 0x10;
    std::memcpy(&maxBits, m + 0x10 + 16 + 8 + 4, 4);
    if (maxBits > 128) std::memcpy(&bits, m + 0x10 + 16, 8);
    constexpr size_t kStride = 24 + 8;          // the pair (FString, enum byte; 8-aligned), then the hash's id and index
    for (int32_t i = 0; data && bits && i < num && i < 100000; ++i) {
        if (!(bits[i / 8] & (1 << (i % 8)))) continue;
        const uint8_t* element = data + static_cast<size_t>(i) * kStride;
        if (eng::ReadFString(element) == levelName) return element[16];
    }
    return -1;
}

}  // namespace localrecords
