#include "nativeghost.hpp"

#include <algorithm>
#include <cmath>
#include <array>
#include <cstring>
#include <map>
#include <vector>

#include "engine.hpp"
#include "game.hpp"
#include "layout.hpp"
#include "log.hpp"
#include "steam.hpp"
#include "widgets.hpp"

namespace nativeghost {
int ApplyRealSkins();                         // (below)
namespace {

using eng::Obj;
using eng::Params;

constexpr size_t kIdentitySize = 80, kKeySize = 96, kTicketSize = 120, kRecordSize = 0x100;   // a record is under 0x100 (its size is read)

struct Session {
    std::array<uint8_t, 16> id{};
    std::array<uint8_t, kIdentitySize> identity{};      // its FStrings are the engine's (kept alive: never freed here)
    std::map<std::string, std::array<uint8_t, kKeySize>> keys;
    bool valid = false;
    bool byGame = false;                                // opened by the game's own EnsureNativeGhostSession
};
Session gSession;
// Real skins: the replay's ghost materials replaced by its real ones (skinMaterial for ghostSkinMaterial; the
// accessory's ghost material left out, so the accessory mesh keeps its own), so the game draws the ghost in the
// player's actual, opaque look. Applied to every replay handed over here when SetRealSkins is on.
bool gRealSkins = false;
std::string JsonStringValue(const std::string& text, const std::string& key, size_t* start, size_t* end) {
    const size_t k = text.find("\"" + key + "\"");
    if (k == std::string::npos) return "";
    const size_t colon = text.find(':', k);
    const size_t open = colon == std::string::npos ? std::string::npos : text.find('"', colon);
    if (open == std::string::npos || open > colon + 4) return "";
    size_t close = open + 1;
    while (close < text.size() && text[close] != '"') close += text[close] == '\\' ? 2 : 1;
    if (close >= text.size()) return "";
    *start = open;
    *end = close + 1;
    return text.substr(open + 1, close - open - 1);
}
std::string RealSkin(const std::string& json) {
    // measured: the game's ghost renderer looks the replay's (skin, ghost skin, special class) tuple up in its native
    // catalog; a changed tuple isn't there and the ghost gets the generic white body. So the replay stays as it is
    // and the real look comes from the session's appearance instead (ApplyRealSkins).
    if (true) return json;
    std::string text = json;
    size_t a = 0, b = 0;
    const std::string skin = JsonStringValue(text, "skinMaterial", &a, &b);
    if (skin.empty() || skin == "None") return json;
    a = b = 0;
    JsonStringValue(text, "ghostSkinMaterial", &a, &b);
    if (b > a) text.replace(a, b - a, "\"" + skin + "\"");
    a = b = 0;
    JsonStringValue(text, "accessoryGhostMaterial", &a, &b);
    if (b > a) text.replace(a, b - a, "\"None\"");
    return text;
}


Obj Subsystem() {
    Obj controller = game::PlayerController();
    Obj cls = eng::FindClass("BallestGhostWorldSubsystem");
    if (!controller || !cls) return nullptr;
    return eng::Call(eng::FindCdo("SubsystemBlueprintLibrary"), "GetWorldSubsystem", controller, cls).ReturnObj();
}

// BallestGhostResult: bSuccess @0, Error @1, Message (FString) @8.
bool ResultOk(const Params& p, std::string* error) {
    size_t size = 0;
    const uint8_t* r = p.Return(&size);
    if (!r || size < 24) {
        if (error) *error = "no result";
        return false;
    }
    if (r[0]) return true;
    if (error) *error = "error " + std::to_string(r[1]) + ": " + eng::ReadFString(r + 8);
    return false;
}

// The open session (its identity read once per session id), or false.
bool CurrentSession(Obj sub) {
    if (!sub || !eng::Call(sub, "HasOpenGhostSession").ReturnBool()) return false;
    std::array<uint8_t, 16> id{};
    Params idCall = eng::Call(sub, "GetGhostSessionId");
    size_t size = 0;
    const uint8_t* r = idCall.Return(&size);
    if (!r || size != 16) return false;
    std::memcpy(id.data(), r, 16);
    if (gSession.valid && id == gSession.id) return true;
    Params identity = eng::Call(sub, "GetGhostSessionIdentity");
    r = identity.Return(&size);
    if (!r || size != kIdentitySize) return false;
    gSession = Session{};
    gSession.id = id;
    std::memcpy(gSession.identity.data(), r, kIdentitySize);
    std::memset(const_cast<uint8_t*>(r), 0, kIdentitySize);        // ours now (never freed: the engine's strings stay valid)
    gSession.valid = true;
    return true;
}

const uint8_t* KeyOf(Obj sub, const std::string& steamId, std::string* error) {
    auto it = gSession.keys.find(steamId);
    if (it != gSession.keys.end()) return it->second.data();
    Obj lib = eng::FindCdo("BallestGhostInteropLibrary");      // MakeGhostKey's library (types dump)
    Obj fn = lib ? eng::FunctionOn(lib, "MakeGhostKey") : nullptr;
    if (!fn) {
        if (error) *error = "MakeGhostKey not found";
        return nullptr;
    }
    (void)sub;
    const std::wstring wide = eng::Widen(steamId);
    Params p(fn);
    p.Set("Identity", gSession.identity.data(), kIdentitySize);
    p.Set("SteamID", eng::FString{wide.c_str(), static_cast<int32_t>(wide.size() + 1), static_cast<int32_t>(wide.size() + 1)});
    if (!eng::Invoke(lib, p) || !ResultOk(p, error)) return nullptr;
    size_t size = 0;
    const uint8_t* key = p.Get("KeyOut", &size);
    if (!key || size != kKeySize) {
        if (error) *error = "no key";
        return nullptr;
    }
    std::array<uint8_t, kKeySize> kept{};
    std::memcpy(kept.data(), key, kKeySize);
    std::memset(const_cast<uint8_t*>(key), 0, kKeySize);           // the engine's strings, kept with the key
    return gSession.keys.emplace(steamId, kept).first->second.data();
}

bool Record(Obj sub, const uint8_t* key, std::array<uint8_t, kRecordSize>* out) {
    Params p(eng::FunctionOn(sub, "GetGhostRecord"));
    p.Set("Key", key, kKeySize);
    if (!eng::Invoke(sub, p) || !p.ReturnBool()) return false;
    size_t size = 0;
    const uint8_t* state = p.Get("StateOut", &size);
    if (!state || size > kRecordSize) return false;
    out->fill(0);
    std::memcpy(out->data(), state, size);
    return true;
}

Obj GameInstance() {
    Obj controller = game::PlayerController();
    return controller ? eng::Call(eng::FindCdo("GameplayStatics"), "GetGameInstance", controller).ReturnObj() : nullptr;
}

std::string IdentityText(const uint8_t* id) {
    long long pfid = 0;
    std::memcpy(&pfid, id + 0x20, 8);
    return "level '" + eng::ReadFString(id) + "' record '" + eng::ReadFString(id + 0x10) + "' board '" + eng::ReadFString(id + 0x28) +
           "' pfid " + std::to_string(pfid) + " ugc " + std::to_string(id[0x38]);
}

}  // namespace

namespace {
eng::FString F(const std::wstring& w) { return eng::FString{w.c_str(), static_cast<int32_t>(w.size() + 1), static_cast<int32_t>(w.size() + 1)}; }

// Tries to open a session with one identity (the bytes of a BallestLeaderboardIdentity); `why` gets the game's answer.
bool TryOpen(Obj sub, Obj debug, const uint8_t* identity, const std::wstring& steamId, const std::string& label, std::string* why) {
    Params valid(eng::FunctionOn(debug, "IsLeaderboardIdentityValid"));
    valid.Set("Identity", identity, kIdentitySize);
    eng::Invoke(debug, valid);
    Params open(eng::FunctionOn(sub, "OpenGhostSession"));
    open.Set("Identity", identity, kIdentitySize);
    open.Set("LocalSteamId", F(steamId));
    std::string error;
    const bool ok = eng::Invoke(sub, open) && ResultOk(open, &error);
    hostlog::Info("native ghost: session with " + label + " (" + IdentityText(identity) + ", valid " + (valid.ReturnBool() ? "1" : "0") +
                  "): " + (ok ? std::string("open") : error));
    if (!ok) *why += label + ": " + error + "; ";
    return ok;
}
}  // namespace

bool OpenSession(const std::string& boardName, std::string* error) {
    Obj sub = Subsystem();
    Obj gi = GameInstance();
    Obj debug = eng::FindCdo("BallestLeaderboardDebugLibrary");
    if (!sub || !gi || !debug) {
        if (error) *error = "no subsystem, game instance or identity library";
        return false;
    }
    if (CurrentSession(sub)) return true;
    const eng::Prop level = eng::FindProp(eng::ClassOf(gi), "ActiveLevel");
    const std::string levelName = level ? eng::ReadFString(gi + level.offset) : "";
    if (levelName.empty()) {
        if (error) *error = "no active level";
        return false;
    }
    const uint64_t own = steam::OwnSteamId();
    const std::wstring steamId = eng::Widen(own ? std::to_string(own) : std::string());
    const std::wstring wl = eng::Widen(levelName), wb = eng::Widen(boardName.empty() ? levelName : boardName);
    std::string why;
    bool ok = false;
    // 1. the game's own identity for this track (game instance CurrentLeaderboardIdentity; for a local map, measured:
    //    level and record the map's name, board "ballest_v0_-1_Climb_<name>", file id 0, UGC)
    const eng::Prop current = eng::FindProp(eng::ClassOf(gi), "CurrentLeaderboardIdentity");
    if (current && current.size == kIdentitySize && !eng::ReadFString(gi + current.offset).empty())
        ok = TryOpen(sub, debug, gi + current.offset, steamId, "the game's identity", &why);
    // 2. a core (non-UGC) identity for the level, as the game makes for its own tracks, opened by the game's own
    //    session set-up (game instance EnsureNativeGhostSession(identity): OpenGhostSession, the saved personal best,
    //    its rival-replay request handler bound to the subsystem, the playback policy from the player's settings, the
    //    label size and the local opacity reference), else directly
    Params core(eng::FunctionOn(debug, "MakeCoreLeaderboardIdentity"));
    if (!ok) {
        core.Set("LevelName", F(wl));
        core.Set("LeaderboardName", F(wb));
        size_t size = 0;
        const uint8_t* id = eng::Invoke(debug, core) ? core.Return(&size) : nullptr;
        if (id && size == kIdentitySize) {
            Params ensure(eng::FunctionOn(gi, "EnsureNativeGhostSession"));
            bool gameOk = false;
            if (ensure.Fn()) {
                ensure.Set("GameplayIdentity", id, kIdentitySize);
                eng::Invoke(gi, ensure);
                size_t n = 0;
                const uint8_t* success = ensure.Get("Success", &n);
                gameOk = success && success[0] && CurrentSession(sub);
            }
            hostlog::Info(std::string("native ghost: the game's own session set-up for ") + IdentityText(id) + ": " + (gameOk ? "open" : "failed"));
            ok = gameOk || TryOpen(sub, debug, id, steamId, "a core identity", &why);
            gSession.byGame = gameOk;
        }
    }
    if (!ok) {
        if (error) *error = why;
        return false;
    }
    // as the game's own session set-up does (EnsureNativeGhostSession): ghosts on and shown
    size_t size = 0;
    Params policy = eng::Call(sub, "GetPlaybackPolicy");
    const uint8_t* got = policy.Return(&size);
    if (got && size >= 4) {
        uint8_t want[8] = {};
        std::memcpy(want, got, size < 8 ? size : 8);
        want[0] = 1;
        want[1] = 1;
        Params set(eng::FunctionOn(sub, "SetPlaybackPolicy"));
        set.Set("InPolicy", want, size < 8 ? size : 8);
        eng::Invoke(sub, set);
    }
    ApplyRealSkins();
    return CurrentSession(sub);
}

bool HasSession() { return CurrentSession(Subsystem()); }

std::string SessionDescription() {
    Obj sub = Subsystem();
    if (!sub) return "no ghost subsystem";
    if (!CurrentSession(sub)) {
        std::string s = "no ghost session";
        if (Obj gi = GameInstance()) {
            const eng::Prop level = eng::FindProp(eng::ClassOf(gi), "ActiveLevel");
            const eng::Prop pfid = eng::FindProp(eng::ClassOf(gi), "CurrentLevelPubFileID");
            const eng::Prop current = eng::FindProp(eng::ClassOf(gi), "CurrentLeaderboardIdentity");
            long long id = 0;
            if (pfid) std::memcpy(&id, gi + pfid.offset, 8);
            s += " | active level '" + (level ? eng::ReadFString(gi + level.offset) : std::string("?")) + "' pfid " + std::to_string(id) +
                 " | current identity " + (current ? IdentityText(gi + current.offset) : std::string("?"));
        }
        return s;
    }
    const uint8_t* id = gSession.identity.data();
    long long pfid = 0;
    std::memcpy(&pfid, id + 0x20, 8);
    std::string s = "session: level '" + eng::ReadFString(id) + "' record '" + eng::ReadFString(id + 0x10) + "' board '" +
                    eng::ReadFString(id + 0x28) + "' pfid " + std::to_string(pfid) + " ugc " + std::to_string(id[0x38]);
    {
        const double t = eng::Call(sub, "GetPlaybackTime").ReturnAs<double>(-1);
        Params counters = eng::Call(sub, "GetPlaybackCounters");
        size_t n = 0;
        const uint8_t* c = counters.Return(&n);
        int32_t v[12] = {};
        if (c && n >= 48) std::memcpy(v, c, 48);
        s += " | playback " + std::to_string(t) + " s, registered " + std::to_string(v[0]) + " active " + std::to_string(v[1]) +
             " visible " + std::to_string(v[3]) + " rendered " + std::to_string(v[6]) + " labels " + std::to_string(v[11]);
    }
    Params records = eng::Call(sub, "GetGhostRecords");
    size_t size = 0;
    const uint8_t* r = records.Return(&size);
    if (r && size == 16) {
        const uint8_t* data = nullptr;
        int32_t num = 0;
        std::memcpy(&data, r, 8);
        std::memcpy(&num, r + 8, 4);
        s += " | " + std::to_string(num) + " records:";
        const size_t stride = static_cast<size_t>(Params(eng::FunctionOn(sub, "GetGhostRecord")).SizeOf("StateOut"));
        s += " (" + std::to_string(stride) + " bytes each)";
        for (int i = 0; i < num && i < 20 && data && stride > 0xbc; ++i) {
            const uint8_t* rec = data + static_cast<size_t>(i) * stride;
            double best = 0;
            std::memcpy(&best, rec + 0xa8, 8);
            s += " [" + eng::ReadFString(rec + 0x50) + " '" + eng::ReadFString(rec + 0x98) + "' load " + std::to_string(rec[0x7e]) +
                 " exists " + std::to_string(rec[0x90]) + " replay " + std::to_string(rec[0x91]) + " visible " + std::to_string(rec[0x93]) +
                 " opts " + std::to_string(rec[0x78]) + std::to_string(rec[0x79]) + std::to_string(rec[0x7a]) + std::to_string(rec[0x7b]) + std::to_string(rec[0x7c]) + " override " + std::to_string(rec[0x7d]) + " best " + std::to_string(best) + " duration " + std::to_string(*reinterpret_cast<const double*>(rec + 0xb0)) + " samples " + std::to_string(*reinterpret_cast<const int32_t*>(rec + 0xb8)) + " present " + std::to_string(rec[0x92]) + " error '" + eng::ReadFString(rec + 0x80) + "']";
        }
    }
    return s;
}

bool Load(const std::string& steamId, const std::string& replayJson, std::string* error) {
    if (steamId.empty() || steamId.size() > 20 || steamId.find_first_not_of("0123456789") != std::string::npos) {
        if (error) *error = "not a Steam id";
        return false;
    }
    Obj sub = Subsystem();
    if (!CurrentSession(sub)) {
        if (error) *error = "no ghost session";
        return false;
    }
    const uint8_t* key = KeyOf(sub, steamId, error);
    if (!key) return false;
    Params begin(eng::FunctionOn(sub, "BeginReplayRequest"));
    begin.Set("Key", key, kKeySize);
    if (!eng::Invoke(sub, begin) || !ResultOk(begin, error)) return false;
    size_t size = 0;
    const uint8_t* ticket = begin.Get("TicketOut", &size);
    if (!ticket || size != kTicketSize) {
        if (error) *error = "no ticket";
        return false;
    }
    // The replay is that player's: its steamId.result set to the id (the game refuses 0, "invalid unsigned decimal
    // Steam ID", measured with a host-recorded run that had none)
    std::string text = replayJson;
    const size_t at = text.find("\"steamId\"");
    const size_t open = at == std::string::npos ? std::string::npos : text.find('{', at);
    const size_t close = open == std::string::npos ? std::string::npos : text.find('}', open);
    if (close != std::string::npos && close - at < 80) text.replace(open, close - open + 1, "{\"result\":" + steamId + "}");
    const std::wstring wide = eng::Widen(RealSkin(text));
    Params accept(eng::FunctionOn(sub, "AcceptReplayJson"));
    accept.Set("Ticket", ticket, kTicketSize);
    accept.Set("RawJson", eng::FString{wide.c_str(), static_cast<int32_t>(wide.size() + 1), static_cast<int32_t>(wide.size() + 1)});
    if (!eng::Invoke(sub, accept) || !ResultOk(accept, error)) return false;
    ApplyRealSkins();                           // (appearances the catalog added since)
    hostlog::Info("native ghost: replay of " + steamId + " handed to the game (" + std::to_string(replayJson.size()) + " bytes)");
    return true;
}

bool HasReplay(const std::string& steamId) {
    Obj sub = Subsystem();
    if (!CurrentSession(sub)) return false;
    const uint8_t* key = KeyOf(sub, steamId, nullptr);
    std::array<uint8_t, kRecordSize> rec{};
    return key && Record(sub, key, &rec) && rec[0x91];
}

bool Ready(const std::string& steamId) {
    Obj sub = Subsystem();
    if (!CurrentSession(sub)) return false;
    const uint8_t* key = KeyOf(sub, steamId, nullptr);
    std::array<uint8_t, kRecordSize> rec{};
    return key && Record(sub, key, &rec) && rec[0x91] && rec[0x7e] == 2;
}

bool Select(const std::string& steamId, int visibility, std::string* error) {
    Obj sub = Subsystem();
    if (!CurrentSession(sub)) {
        if (error) *error = "no ghost session";
        return false;
    }
    const uint8_t* key = KeyOf(sub, steamId, error);
    if (!key) return false;
    Params p(eng::FunctionOn(sub, "SetGhostSelection"));
    p.Set("Key", key, kKeySize);
    p.Set("Override", static_cast<uint8_t>(visibility < 0 || visibility > 2 ? 0 : visibility));
    if (!eng::Invoke(sub, p) || !ResultOk(p, error)) return false;
    hostlog::Info("native ghost: " + steamId + " selection " + std::to_string(visibility));
    return true;
}

bool Visible(const std::string& steamId) {
    Obj sub = Subsystem();
    if (!CurrentSession(sub)) return false;
    const uint8_t* key = KeyOf(sub, steamId, nullptr);
    std::array<uint8_t, kRecordSize> rec{};
    return key && Record(sub, key, &rec) && rec[0x93];
}

bool Watch(const std::string& steamId, std::string* error) {
    Obj sub = Subsystem();
    Obj controller = game::PlayerController();
    if (!controller || !CurrentSession(sub)) {
        if (error) *error = "no ghost session";
        return false;
    }
    const uint8_t* key = KeyOf(sub, steamId, error);
    if (!key) return false;
    Params p(eng::FunctionOn(controller, "PlayerWantsToPOVGhost"));
    p.Set("GhostKey", key, kKeySize);
    if (!eng::Invoke(controller, p)) {
        if (error) *error = "PlayerWantsToPOVGhost failed";
        return false;
    }
    if (!Watching()) {
        // measured 2026-10-12: for a replay handed over here the game's follow camera refuses the ghost
        // (AC_GhostFollowCam.SetNativeGhostSource false; record ready, visible, 466 samples, playback running), so
        // PlayerWantsToPOVGhost stops before replay mode (it does select the ghost)
        if (error) *error = "the game's replay camera refused the ghost (SetNativeGhostSource false)";
        return false;
    }
    hostlog::Info("native ghost: replay mode for " + steamId);
    return true;
}

bool StopWatching() {
    Obj controller = game::PlayerController();
    if (!controller || !Watching()) return false;
    eng::Call(controller, "PlayerWantsToStopPOV");
    return true;
}

bool Watching() {
    Obj controller = game::PlayerController();
    bool on = false;
    return controller && eng::ReadBool(controller, "bPOVSpectating", &on) && on;
}

}  // namespace nativeghost

namespace nativeghost {
std::string DebugWatch(const std::string& steamId) {
    Obj sub = Subsystem();
    Obj controller = game::PlayerController();
    if (!controller || !CurrentSession(sub)) return "no session";
    const uint8_t* key = KeyOf(sub, steamId, nullptr);
    if (!key) return "no key";
    Params get(eng::FunctionOn(controller, "GetGhostFollowCam"));
    if (!get.Fn()) return "no GetGhostFollowCam";
    eng::Invoke(controller, get);
    Obj cam = get.GetObj("GhostFollowCam");
    size_t n = 0;
    const uint8_t* ok = get.Get("Success", &n);
    std::string s = "follow cam " + (cam ? eng::PathOf(cam) : std::string("none")) + " success " + std::to_string(ok ? ok[0] : 9);
    if (!cam) return s;
    eng::Call(cam, "ClearGhostFollow", uint8_t{0});
    {
        const double t = eng::Call(sub, "GetPlaybackTime").ReturnAs<double>(0);
        Params start = eng::Call(sub, "StartPlayback", t);
        size_t rn = 0;
        const uint8_t* r = start.Return(&rn);
        s += " | StartPlayback(" + std::to_string(t) + ") " + (r && r[0] ? "ok" : "failed: " + (r ? eng::ReadFString(r + 8) : std::string("?")));
        Params vis(eng::FunctionOn(sub, "IsGhostPresentationVisible"));
        vis.Set("Key", key, kKeySize);
        eng::Invoke(sub, vis);
        s += " presentation visible " + std::string(vis.ReturnBool() ? "1" : "0");
    }
    Params set(eng::FunctionOn(cam, "SetNativeGhostSource"));
    set.Set("Subsystem", sub);
    set.Set("Key", key, kKeySize);
    eng::Invoke(cam, set);
    s += " | SetNativeGhostSource " + std::string(set.ReturnBool() ? "true" : "false");
    eng::Call(controller, "Start_FreeCam");
    Obj view = eng::Call(controller, "GetViewTarget").ReturnObj();
    Obj cam2 = view ? eng::ReadObj(view, "AC_GhostFollowCam") : nullptr;
    s += " | after Start_FreeCam: view " + (view ? eng::ObjName(view) : std::string("none")) + (cam2 == cam ? " (same cam)" : " (other cam)");
    if (cam2) {
        eng::Call(cam2, "SetGhostFollowEnabled", uint8_t{1});
        Params set2(eng::FunctionOn(cam2, "SetNativeGhostSource"));
        set2.Set("Subsystem", sub);
        set2.Set("Key", key, kKeySize);
        eng::Invoke(cam2, set2);
        s += " SetNativeGhostSource " + std::string(set2.ReturnBool() ? "true" : "false");
        cam = cam2;
    }
    s += " | HasNativeGhostSource " + std::string(eng::Call(cam, "HasNativeGhostSource").ReturnBool() ? "1" : "0");
    s += " HasGhostSamples " + std::string(eng::Call(cam, "HasGhostSamples").ReturnBool() ? "1" : "0");
    return s;
}
}  // namespace nativeghost

// --- replays as the game's rivals ---------------------------------------------------------------------------------
// The game's rival ghosts (read from BP_BallgameGameInstance): RequestNativeRivalRoster -> BeginRivalRosterRequest
// (a roster ticket) -> the roster downloaded -> AcceptRivalRoster(ticket, sources: key + Steam UGC handle each); the
// subsystem then asks for each replay through its OnRivalReplayReadRequested delegate, which the game instance's
// OnNativeRivalReplayReadRequested_Handler(Request: key, UGC handle, delivery ticket) answers by downloading the UGC
// file and calling CompleteRivalReplayRead(ticket, raw JSON). Here the roster is the replays handed over, each with a
// made-up UGC handle, and that handler is wrapped: a request for one of them is answered with its JSON at once
// (the handler itself isn't run for it; any other request goes on to the game's handler as before).
namespace nativeghost {
namespace {
using NativeFunction = void (*)(Obj context, uint8_t* frame, void* result);
NativeFunction gRivalHandlerOriginal = nullptr;
Obj gRivalHandler = nullptr;
std::map<std::string, std::string> gRivalJson;          // Steam id -> replay JSON, for the requests to come
std::vector<std::string> gRoster;                       // the roster's players, in order
int gAnswered = 0;

std::string WithSteamId(const std::string& json, const std::string& steamId) {
    std::string text = json;
    const size_t at = text.find("\"steamId\"");
    const size_t open = at == std::string::npos ? std::string::npos : text.find('{', at);
    const size_t close = open == std::string::npos ? std::string::npos : text.find('}', open);
    if (close != std::string::npos && close - at < 80) text.replace(open, close - open + 1, "{\"result\":" + steamId + "}");
    return text;
}

void HookedRivalRequest(Obj context, uint8_t* frame, void* result) {
    Obj node = nullptr;
    uint8_t* params = nullptr;
    if (frame) {
        std::memcpy(&node, frame + layout::kFFrameFunctionOffset, sizeof node);
        std::memcpy(&params, frame + layout::kFFrameParametersOffset, sizeof params);
    }
    if (node == gRivalHandler && params) {
        // Request: Key (96: identity 80 + SteamID FString @0x50), UGCHandle @0x60, Ticket (120) @0x68
        const std::string steamId = eng::ReadFString(params + 0x50);
        auto it = gRivalJson.find(steamId);
        if (it != gRivalJson.end()) {
            Obj sub = Subsystem();
            const std::wstring wide = eng::Widen(RealSkin(WithSteamId(it->second, steamId)));
            Params done(eng::FunctionOn(sub, "CompleteRivalReplayRead"));
            done.Set("Ticket", params + 0x68, kTicketSize);
            done.Set("RawJson", eng::FString{wide.c_str(), static_cast<int32_t>(wide.size() + 1), static_cast<int32_t>(wide.size() + 1)});
            std::string error;
            const bool ok = sub && eng::Invoke(sub, done) && ResultOk(done, &error);
            ++gAnswered;
            hostlog::Info("native ghost: the game asked for rival " + steamId + "'s replay; answered " + (ok ? "ok" : "refused: " + error));
            return;
        }
    }
    gRivalHandlerOriginal(context, frame, result);
}

bool WatchRivalRequests() {
    if (gRivalHandler) return true;
    Obj gi = GameInstance();
    Obj fn = gi ? eng::FindFunction(eng::ClassOf(gi), "OnNativeRivalReplayReadRequested_Handler") : nullptr;
    if (!fn) return false;
    NativeFunction entry = nullptr;
    std::memcpy(&entry, fn + layout::kUFunctionNativeFunctionOffset, sizeof entry);
    if (!entry || !eng::InImage(reinterpret_cast<void*>(entry))) {
        hostlog::Warn("native ghost: the rival replay handler is not as measured");
        return false;
    }
    gRivalHandler = fn;
    gRivalHandlerOriginal = entry;
    NativeFunction hooked = &HookedRivalRequest;
    std::memcpy(fn + layout::kUFunctionNativeFunctionOffset, &hooked, sizeof hooked);
    hostlog::Info("native ghost: answering the game's rival replay requests for handed-over replays");
    return true;
}
}  // namespace

bool LoadAsRival(const std::string& steamId, const std::string& replayJson, std::string* error) {
    if (steamId.empty() || steamId.size() > 20 || steamId.find_first_not_of("0123456789") != std::string::npos) {
        if (error) *error = "not a Steam id";
        return false;
    }
    Obj sub = Subsystem();
    if (!CurrentSession(sub)) {
        if (error) *error = "no ghost session";
        return false;
    }
    if (!gSession.byGame) {
        if (error) *error = "the session wasn't opened by the game (its rival handler isn't bound)";
        return false;
    }
    if (!WatchRivalRequests()) {
        if (error) *error = "the game's rival replay handler wasn't found";
        return false;
    }
    static std::array<uint8_t, 16> rosterSession{};
    if (rosterSession != gSession.id) {         // a new session (a new map): a roster of its own
        rosterSession = gSession.id;
        gRoster.clear();
        gRivalJson.clear();
    }
    gRivalJson[steamId] = replayJson;
    if (std::find(gRoster.begin(), gRoster.end(), steamId) == gRoster.end()) gRoster.push_back(steamId);
    // a roster of every replay handed over this session (a roster replaces the last one)
    std::vector<uint8_t> sources;
    for (size_t i = 0; i < gRoster.size(); ++i) {
        const uint8_t* key = KeyOf(sub, gRoster[i], error);
        if (!key) return false;
        uint8_t source[104] = {};
        std::memcpy(source, key, kKeySize);
        const int64_t handle = 7000000 + static_cast<int64_t>(i);      // made up: never downloaded (the request is answered here)
        std::memcpy(source + 96, &handle, 8);
        sources.insert(sources.end(), source, source + sizeof source);
    }
    Params beginCall(eng::FunctionOn(sub, "BeginRivalRosterRequest"));
    if (!eng::Invoke(sub, beginCall) || !ResultOk(beginCall, error)) return false;
    size_t n = 0;
    const uint8_t* ticket = beginCall.Get("TicketOut", &n);
    if (!ticket || n != 24) {
        if (error) *error = "no roster ticket";
        return false;
    }
    struct Array {
        uint8_t* data;
        int32_t num, max;
    } list{sources.data(), static_cast<int32_t>(gRoster.size()), static_cast<int32_t>(gRoster.size())};
    Params accept(eng::FunctionOn(sub, "AcceptRivalRoster"));
    accept.Set("Ticket", ticket, 24);
    accept.Set("Sources", list);
    const int before = gAnswered;
    if (!eng::Invoke(sub, accept) || !ResultOk(accept, error)) return false;
    ApplyRealSkins();
    hostlog::Info("native ghost: roster of " + std::to_string(gRoster.size()) + " rival(s) accepted (" + std::to_string(gAnswered - before) +
                  " replay request(s) answered so far)");
    return true;
}

}  // namespace nativeghost

namespace nativeghost {
// The game's replay camera follows only the session's own ghost (measured 2026-10-12: AC_GhostFollowCam
// SetNativeGhostSource is true for the record whose Steam id is the session's local player, flagged own-or-personal-
// best, whether it came from the save or from AcceptReplayJson, and false for every other record: rivals from a
// roster, one-off replays, game-written ghost files alike). So for replay mode the session is opened again with the
// watched player as its local player (core identity of the level).
void CloseSession() {
    if (Obj sub = Subsystem()) eng::Call(sub, "CloseGhostSession");
    gSession = Session{};
}

bool OpenAsLocal(const std::string& localId, std::string* error) {
    Obj sub = Subsystem();
    Obj gi = GameInstance();
    Obj debug = eng::FindCdo("BallestLeaderboardDebugLibrary");
    if (!sub || !gi || !debug) {
        if (error) *error = "no subsystem";
        return false;
    }
    CloseSession();
    const eng::Prop level = eng::FindProp(eng::ClassOf(gi), "ActiveLevel");
    const std::wstring wl = eng::Widen(level ? eng::ReadFString(gi + level.offset) : "");
    Params core(eng::FunctionOn(debug, "MakeCoreLeaderboardIdentity"));
    core.Set("LevelName", F(wl));
    core.Set("LeaderboardName", F(wl));
    size_t size = 0;
    const uint8_t* id = eng::Invoke(debug, core) ? core.Return(&size) : nullptr;
    if (!id || size != kIdentitySize) {
        if (error) *error = "no identity";
        return false;
    }
    std::string why;
    if (!TryOpen(sub, debug, id, eng::Widen(localId), "local player " + localId, &why)) {
        if (error) *error = why;
        return false;
    }
    // shown, as the game's own set-up turns ghosts on
    Params policy = eng::Call(sub, "GetPlaybackPolicy");
    const uint8_t* got = policy.Return(&size);
    if (got && size >= 4) {
        uint8_t want[8] = {};
        std::memcpy(want, got, size < 8 ? size : 8);
        want[0] = want[1] = 1;
        Params set(eng::FunctionOn(sub, "SetPlaybackPolicy"));
        set.Set("InPolicy", want, size < 8 ? size : 8);
        eng::Invoke(sub, set);
    }
    ApplyRealSkins();
    return CurrentSession(sub);
}

std::string DebugReopenAs(const std::string& localId) {
    std::string error;
    return OpenAsLocal(localId, &error) ? "reopened as " + localId : error;
}
}  // namespace nativeghost

namespace nativeghost {
bool LoadOwnBest() {
    Obj gi = GameInstance();
    if (!gi || !CurrentSession(Subsystem())) return false;
    // the game's own: GetCorrectHighscoreFromSave -> BeginReplayRequest(key of the player's own Steam id) ->
    // AcceptSavedHighscore (BP_BallgameGameInstance LoadSavedPersonalBestIntoNativeGhostSession)
    eng::Call(gi, "LoadSavedPersonalBestIntoNativeGhostSession");
    return true;
}

std::string RenameTags(const std::string& from, const std::string& to) {
    Obj cls = eng::FindClass("BallestGhostTagWidget");
    int n = 0, renamed = 0;
    const std::wstring w = eng::Widen(to);
    eng::ForEachObject([&](Obj o) {
        if (!cls || !eng::IsA(o, cls) || eng::IsDefaultObject(o)) return true;
        ++n;
        Obj text = eng::ReadObj(o, "NameText");
        if (text && ui::widgets::ReadText(text) == from) {
            eng::Call(o, "SetGhostName", eng::FString{w.c_str(), static_cast<int32_t>(w.size() + 1), static_cast<int32_t>(w.size() + 1)});
            ++renamed;
        }
        return true;
    });
    return std::to_string(renamed) + " of " + std::to_string(n) + " tags renamed";
}

std::string NameTags() {
    std::string s;
    Obj cls = eng::FindClass("BallestGhostTagWidget");
    if (!cls) return "no tag class";
    int n = 0;
    eng::ForEachObject([&](Obj o) {
        if (!eng::IsA(o, cls) || eng::IsDefaultObject(o)) return true;
        Obj text = eng::ReadObj(o, "NameText");
        const bool shown = eng::Call(o, "IsVisible").ReturnBool();
        s += " [" + (text ? ui::widgets::ReadText(text) : std::string("?")) + (shown ? " shown" : " hidden") + "]";
        ++n;
        return true;
    });
    return std::to_string(n) + " name tags:" + s;
}
}  // namespace nativeghost

namespace nativeghost {
// The watched ghost's name tag in replay mode. Its record is the session's own (the replay camera's rule), so the
// game names it after the local player (measured: record Username "AnythingGoes" for a replay whose username was
// another's; the label canvas sets that name on its BallestGhostTagWidget again every few frames, measured: a
// renamed tag was back within seconds). So every frame the tag showing the record's name gets the replay's name
// again (if two tags show it, the player's own best is shown too: the one nearest the screen's middle, where the
// replay camera keeps the watched ghost).
void NameWatchedTag(const std::string& steamId, const std::string& name) {
    Obj sub = Subsystem();
    if (!CurrentSession(sub) || name.empty()) return;
    const uint8_t* key = KeyOf(sub, steamId, nullptr);
    std::array<uint8_t, kRecordSize> rec{};
    if (!key || !Record(sub, key, &rec)) return;
    const std::string recordName = eng::ReadFString(rec.data() + 0x98);
    if (recordName.empty() || recordName == name) return;
    Obj best = nullptr;
    double bestDistance = 1e30;
    // the label canvas's own pool of tags (BallestGhostWorldSubsystem.LabelCanvas -> WidgetTagPool), not a scan
    Obj canvasWidget = eng::ReadObj(sub, "LabelCanvas");
    const std::vector<Obj> pool = canvasWidget ? eng::ReadObjArray(canvasWidget, "WidgetTagPool") : std::vector<Obj>{};
    for (Obj o : pool) {
        Obj text = o ? eng::ReadObj(o, "NameText") : nullptr;
        if (!text || ui::widgets::ReadText(text) != recordName) continue;
        double d = 0;
        if (Obj slot = eng::ReadObj(o, "Slot")) {
            struct V2 { double x, y; };
            const V2 at = eng::Call(slot, "GetPosition").ReturnAs<V2>();
            Obj canvas = eng::Call(o, "GetParent").ReturnObj();
            const V2 size = canvas ? eng::Call(canvas, "GetDesiredSize").ReturnAs<V2>() : V2{0, 0};
            d = std::abs(at.x - (size.x > 1 ? size.x / 2 : 0));
        }
        if (d < bestDistance) {
            bestDistance = d;
            best = o;
        }
    }
    if (!best) return;
    const std::wstring w = eng::Widen(name);
    eng::Call(best, "SetGhostName", eng::FString{w.c_str(), static_cast<int32_t>(w.size() + 1), static_cast<int32_t>(w.size() + 1)});
}
}  // namespace nativeghost

namespace nativeghost {
bool OwnBestWanted(const std::string& steamId) {
    Obj sub = Subsystem();
    if (!CurrentSession(sub)) return false;
    const uint8_t* key = KeyOf(sub, steamId, nullptr);
    std::array<uint8_t, kRecordSize> rec{};
    if (!key || !Record(sub, key, &rec)) return false;
    if (rec[0x7d] == 1) return true;            // shown by a toggle
    if (rec[0x7d] == 2) return false;
    Params policy = eng::Call(sub, "GetPlaybackPolicy");
    size_t n = 0;
    const uint8_t* p = policy.Return(&n);
    return p && n >= 4 && p[0] && p[1] && p[2];  // enabled, visible, own-or-personal-best shown (the player's setting)
}
}  // namespace nativeghost

namespace nativeghost {
int Override(const std::string& steamId) {
    Obj sub = Subsystem();
    if (!CurrentSession(sub)) return 0;
    const uint8_t* key = KeyOf(sub, steamId, nullptr);
    std::array<uint8_t, kRecordSize> rec{};
    return key && Record(sub, key, &rec) ? rec[0x7d] : 0;
}
}  // namespace nativeghost

namespace nativeghost {
void SetRealSkins(bool on) { gRealSkins = on; }
}  // namespace nativeghost

namespace nativeghost {
namespace {
// BallestGhostWorldSubsystem.ActiveAppearance (BallestGhostAppearanceConfig @0x8e0): DefaultLayers (TArray of
// BallestGhostRenderLayerDefinition, 0xa0 each: MaterialSource @0x10), SpecialAppearances (TArray of
// BallestGhostSpecialAppearanceDefinition, 0xc0 each: Layers TArray @0x28), MinOpacity @0xf0, MaxOpacity @0xf8.
constexpr size_t kAppearance = 0x8e0, kLayerSize = 0xa0, kSpecialSize = 0xc0;
struct Arr {
    uint8_t* data;
    int32_t num, max;
};
std::string LayerText(const uint8_t* layer) {
    Obj mesh = nullptr;
    std::memcpy(&mesh, layer + 8, 8);
    float opacity = 0;
    std::memcpy(&opacity, layer + 0x94, 4);
    Arr mats{};
    std::memcpy(&mats, layer + 0x18, sizeof mats);
    return "{mesh src " + std::to_string(layer[0]) + " " + (mesh ? eng::ObjName(mesh) : std::string("-")) + ", mat src " +
           std::to_string(layer[0x10]) + " (" + std::to_string(mats.num) + " mats), opacity x" + std::to_string(opacity) + "}";
}
}  // namespace

std::string Appearance() {
    Obj sub = Subsystem();
    if (!sub) return "no subsystem";
    const uint8_t* a = sub + kAppearance;
    Arr layers{}, specials{};
    std::memcpy(&layers, a, sizeof layers);
    std::memcpy(&specials, a + 0x10, sizeof specials);
    float minO = 0, maxO = 0;
    std::memcpy(&minO, a + 0xf0, 4);
    std::memcpy(&maxO, a + 0xf8, 4);
    std::string s = "opacity " + std::to_string(minO) + ".." + std::to_string(maxO) + " | default layers:";
    for (int i = 0; i < layers.num && i < 8 && layers.data; ++i) s += " " + LayerText(layers.data + i * kLayerSize);
    s += " | " + std::to_string(specials.num) + " special appearances";
    for (int i = 0; i < specials.num && i < 3 && specials.data; ++i) {
        const uint8_t* sp = specials.data + i * kSpecialSize;
        Arr sl{};
        std::memcpy(&sl, sp + 0x28, sizeof sl);
        s += " [" + std::to_string(sl.num) + " layers:";
        for (int j = 0; j < sl.num && j < 4 && sl.data; ++j) s += " " + LayerText(sl.data + j * kLayerSize);
        s += "]";
    }
    return s;
}

// The session's ghosts in the players' real looks: every layer that draws the replay's ghost skin variant
// (ReplayGhostSkinMaterial) draws its real skin (ReplaySkinMaterial) instead, at full opacity. The subsystem is the
// race world's own (a new one with each map), so this stays on the track it was made on.
int ApplyRealSkins() {
    Obj sub = Subsystem();
    if (!sub || !gRealSkins) return 0;
    uint8_t* a = sub + kAppearance;
    int changed = 0;
    auto layersOf = [&](uint8_t* arr) {
        Arr layers{};
        std::memcpy(&layers, arr, sizeof layers);
        for (int i = 0; i < layers.num && i < 64 && layers.data; ++i) {
            uint8_t* layer = layers.data + i * kLayerSize;
            if (layer[0x10] == 2) {             // ReplayGhostSkinMaterial -> ReplaySkinMaterial
                layer[0x10] = 1;
                ++changed;
            }
        }
    };
    layersOf(a);
    Arr specials{};
    std::memcpy(&specials, a + 0x10, sizeof specials);
    for (int i = 0; i < specials.num && i < 256 && specials.data; ++i) layersOf(specials.data + i * kSpecialSize + 0x28);
    const float one = 1.0f;
    std::memcpy(a + 0xf0, &one, 4);             // MinOpacity
    std::memcpy(a + 0xf8, &one, 4);             // MaxOpacity
    static int logged = -1;
    if (changed && changed != logged) hostlog::Info("native ghost: real looks for this track's ghosts (" + std::to_string(changed) + " layer(s) on the real skin, opaque)");
    if (changed) logged = changed;
    return changed;
}
}  // namespace nativeghost
