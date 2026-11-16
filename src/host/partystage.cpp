#include "partystage.hpp"
#include <windows.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <map>
#include <random>
#include <sstream>
#include "editor.hpp"
#include "draw.hpp"
#include "cosmetics.hpp"
#include "engine.hpp"
#include "files.hpp"
#include "game.hpp"
#include "ghostdata.hpp"
#include "hud.hpp"
#include "json.hpp"
#include "layout.hpp"
#include "lobby.hpp"
#include "log.hpp"
#include "nativeghost.hpp"
#include "net.hpp"
#include "plugins.hpp"
#include "race.hpp"
#include "replay.hpp"
#include "runs.hpp"
#include "sandbox.hpp"
#include "together.hpp"
#include "tracks.hpp"
#include "widgets.hpp"

namespace partystage {
namespace {
using eng::Obj;
using J = json::Value;
constexpr size_t kLimit = 2 * 1024 * 1024;
using PaletteMap = std::map<std::string,std::pair<std::string,std::string>>;
const PaletteMap& Palette();
bool AllowedPalettePath(const std::string& path);
J Text(const std::string& s) { J v; v.type = J::String; v.string = s; return v; }
std::string Write(const J& v) {
    switch (v.type) {
    case J::Null: return "null";
    case J::Bool: return v.boolean ? "true" : "false";
    case J::Number: { std::ostringstream s; s << std::setprecision(17) << v.number; return s.str(); }
    case J::String: {
        std::string s = "\"";
        const char* hex = "0123456789abcdef";
        for (unsigned char c : v.string) {
            if (c == '"' || c == '\\') { s += '\\'; s += static_cast<char>(c); }
            else if (c < 32) { s += "\\u00"; s += hex[c >> 4]; s += hex[c & 15]; }
            else s += static_cast<char>(c);
        }
        return s + '"';
    }
    case J::Array: { std::string s = "["; for (const auto& x : v.items) { if (s.size() > 1) s += ','; s += Write(x); } return s + ']'; }
    case J::Object: { std::string s = "{"; for (const auto& [key, x] : v.members) { if (s.size() > 1) s += ','; s += Write(Text(key)) + ':' + Write(x); } return s + '}'; }
    }
    return "null";
}
J* At(J& v, const std::string& key) { for (auto& p : v.members) if (p.first == key) return &p.second; return nullptr; }
const J* At(const J* v, const std::string& key) { return v ? v->Get(key) : nullptr; }
double Num(const J* v, const std::string& key, double fallback = 0) {
    const J* n = At(v, key); return n && n->type == J::Number ? n->number : fallback;
}
bool Parse(const std::string& text, J& v) { std::string why; return !text.empty() && text.size() <= kLimit && json::Parse(text, v, why); }
std::string Class(const J& item) { return ghostdata::ObjectPath(item.Str("actorToSpawn")); }
bool Ordinary(const J& item,const editor::Vec3& finish) {
    if (!AllowedPalettePath(Class(item))) return false;
    const J* t = item.Get("itemTransform"), *s = At(t, "scale3D"), *p = At(t, "translation"), *q = At(t, "rotation");
    if (!s || !p || !q || item.type != J::Object) return false;
    for (const char* axis : {"x", "y", "z"})
        if (std::abs(Num(s, axis, -9) - 1) > 1e-6 || !std::isfinite(Num(p, axis)) || std::abs(Num(p, axis)) > 1000000) return false;
    const double dx = Num(p,"x")-finish.x, dy = Num(p,"y")-finish.y, dz = Num(p,"z")-finish.z;
    if (dx*dx + dy*dy + dz*dz < 500*500) return false;
    double length = 0;
    for (const char* axis : {"x", "y", "z", "w"}) { const double a = Num(q, axis, 99); if (!std::isfinite(a)) return false; length += a*a; }
    return std::abs(length - 1) < 0.01;
}
struct Stage {
    J base, map;
    editor::Vec3 finish{},placement{},start{};
    std::string token, file, error, oldRun, finished, captured, watchId;
    std::vector<std::string> offers;
    int generation = -1, openingFrom = -1, choice = -1, run = -1;
    bool building = false, committed = false, started = false, replayPrepared = false, replayHeld = false, replayView = false;
    int replayGeneration = -1;
    double replayPeak = 0;
    std::vector<ghostdata::Replay> paths;
    std::vector<std::string> replayIds, replayNames;
    eng::Weak chosen;
    struct Fixed { eng::Weak actor; editor::Vec3 at, scale; editor::Rot rot; };
    std::vector<Fixed> fixed;
    std::vector<double> times;
    eng::Weak policySubsystem;
    bool policySaved=false;
    std::array<uint8_t,4> originalPolicy{};
    eng::Weak drivenBall, retiredBall, retiredRoot;
    int drivenGeneration=-1;
    bool retiredCollision=false, retiredTick=false, retiredPhysics=false, retiredHidden=false;
    bool replayCameraPlaced=false;
    int replayPauseFlags=-1;
    double replayLastFrame=-1, replayGapLogged=-1;
    struct MenuButton { eng::Weak widget, hitbox; bool enabled, hitEnabled, active; };
    std::vector<MenuButton> blockedImprove;
    std::vector<eng::Weak> checkedButtons;
    int raceLimit=60, pendingRespawn=0;
    double lastRespawn=-100;
    std::array<uint8_t,96> startTransform{};
    bool haveStart=false, physicalStartSaved=false;
    std::vector<bool> completed;
};
std::map<int, Stage> stages;
Stage& S() { return stages[plugins::Current()]; }
size_t ItemLimit(const Stage& s) {const auto* items=s.base.Get("items");return items?items->items.size()+512:0;}
bool TemporaryName(const std::string& name) {
    const std::string prefix="Map_LevelEditorMain&Party_",suffix=".balledit";
    if (name.size()<=prefix.size()+suffix.size()+2 || name.compare(0,prefix.size(),prefix)!=0 ||
        name.compare(name.size()-suffix.size(),suffix.size(),suffix)!=0) return false;
    const auto middle=name.substr(prefix.size(),name.size()-prefix.size()-suffix.size());
    const size_t split=middle.find('_');
    if (split<1 || split>40 || split==std::string::npos) return false;
    const auto token=middle.substr(0,split),number=middle.substr(split+1);
    if (token.find_first_not_of("0123456789abcdef")!=std::string::npos || number.empty() || number.size()>5 || number.find_first_not_of("0123456789")!=std::string::npos) return false;
    const int round=std::atoi(number.c_str());return round>=1 && round<=10256;
}
std::wstring TemporaryPath(const std::string& name) {
    if (!TemporaryName(name) || tracks::LocalMapsFolder().empty()) return L"";
    std::wstring path=eng::Widen(tracks::LocalMapsFolder())+L"\\"+eng::Widen(name);
    wchar_t full[32768];const DWORD n=GetFullPathNameW(path.c_str(),32768,full,nullptr);
    return n && n<32768?std::wstring(full,n):L"";
}
std::map<std::string,std::string> temporaryMaps;
bool temporaryLoaded=false;
double nextTemporaryCleanup=0;
std::wstring TemporaryLedger() {return hostlog::DataDir()+L"\\party-temporary-maps.tsv";}
void LoadTemporaryLedger() {
    if (temporaryLoaded || tracks::LocalMapsFolder().empty() || hostlog::DataDir().empty()) return;
    temporaryLoaded=true;
    std::ifstream file(TemporaryLedger().c_str(),std::ios::binary);
    for (std::string line;std::getline(file,line);) {
        if (!line.empty() && line.back()=='\r') line.pop_back();
        const auto at=line.find('\t');if (at==std::string::npos) continue;
        const auto name=line.substr(0,at),hash=line.substr(at+1);
        if (TemporaryName(name) && hash.size()==64 && hash.find_first_not_of("0123456789abcdef")==std::string::npos) temporaryMaps[name]=hash;
    }
}
bool SaveTemporaryLedger() {
    std::string text;for (const auto& [name,hash]:temporaryMaps) text+=name+'\t'+hash+'\n';
    return files::WriteWhole(TemporaryLedger(),text);
}
bool RememberTemporary(const std::string& name,const std::string& text) {
    LoadTemporaryLedger();if (!temporaryLoaded || !TemporaryName(name)) return false;
    temporaryMaps[name]=net::Sha256(text);
    if (SaveTemporaryLedger()) return true;
    temporaryMaps.erase(name);return false;
}
void CleanupTemporaryMaps() {
    LoadTemporaryLedger();if (!temporaryLoaded || temporaryMaps.empty()) return;
    bool changed=false;int removed=0;
    for (auto it=temporaryMaps.begin();it!=temporaryMaps.end();) {
        const auto& [name,hash]=*it;
        const bool active=std::any_of(stages.begin(),stages.end(),[&](const auto& entry){return !entry.second.token.empty() && entry.second.file==name;});
        if (active) {++it;continue;}
        const auto path=TemporaryPath(name);
        const DWORD attributes=path.empty()?INVALID_FILE_ATTRIBUTES:GetFileAttributesW(path.c_str());
        bool forget=false;
        if (attributes==INVALID_FILE_ATTRIBUTES) forget=true;
        else if (attributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)) {
            hostlog::Warn("party stage: retained non-regular temporary map: "+name);forget=true;
        } else {
            std::ifstream file(path.c_str(),std::ios::binary|std::ios::ate);
            const auto length=file.tellg();
            std::string text;
            if (file && length>=0 && static_cast<uint64_t>(length)<=kLimit) {
                text.resize(static_cast<size_t>(length));file.seekg(0);file.read(text.data(),static_cast<std::streamsize>(text.size()));
            }
            const bool readable=static_cast<bool>(file);
            file.close(); // Windows cannot delete the file while this read handle is open.
            if (!readable || text.empty() || net::Sha256(text)!=hash) {
                // A player may explicitly edit/save one of these tracks. Stop
                // owning it rather than deleting an authored change.
                hostlog::Info("party stage: retained edited temporary map: "+name);forget=true;
            } else if (DeleteFileW(path.c_str())) {forget=true;++removed;}
        }
        if (forget) {it=temporaryMaps.erase(it);changed=true;}else ++it;
    }
    if (changed && !SaveTemporaryLedger()) hostlog::Warn("party stage: could not update temporary-map ownership ledger");
    if (removed) hostlog::Info("party stage: removed temporary maps: "+std::to_string(removed));
}
using NativeFunction=void (*)(Obj,uint8_t*,void*);
struct RetryHook { const char* cls; const char* name; int kind; Obj fn=nullptr; NativeFunction original=nullptr; int otherAt=-1; };
RetryHook retryHooks[]={
    {"BP_MyPlayerController_C","InpActEvt_IA_RestartLevel_K2Node_EnhancedInputActionEvent_9",2},
    {"BP_MyPlayerController_C","InpActEvt_IA_RestartCheckpoint_K2Node_EnhancedInputActionEvent_14",1},
    {"BP_MyPlayerController_C","InpActEvt_IA_RestartCheckpoint_K2Node_EnhancedInputActionEvent_15",1},
    {"BP_MyPlayerController_C","ManuallyRestartBall",2},
    {"BP_KillZone_C","ReceiveActorBeginOverlap",3}
};
NativeFunction retryInterpreter=nullptr;
void PartyRetryHook(Obj context,uint8_t* frame,void* result) {
    Obj node=nullptr,self=nullptr; uint8_t* params=nullptr;
    if (frame) {
        std::memcpy(&node,frame+layout::kFFrameFunctionOffset,8);
        std::memcpy(&self,frame+layout::kFFrameObjectOffset,8);
        std::memcpy(&params,frame+layout::kFFrameParametersOffset,8);
    }
    for (auto& h:retryHooks) if (h.fn==node) {
        for (auto& [owner,s]:stages) {
            if (!s.started || s.building || !s.finished.empty() || s.drivenGeneration!=game::Generation() || !s.haveStart || !race::Active() || race::Complete() || self!=context) continue;
            if (h.kind==3) {
                Obj other=nullptr;
                if (!params || h.otherAt<0) continue;
                std::memcpy(&other,params+h.otherAt,8);
                if (!other || other!=eng::Get(s.drivenBall)) continue;
            } else if (context!=game::PlayerController()) continue;
            s.pendingRespawn=h.kind;
            return; // This event belongs to this managed attempt only.
        }
        if (h.original) h.original(context,frame,result);
        return;
    }
    if (retryInterpreter) retryInterpreter(context,frame,result);
}
void InstallRetryHooks() {
    for (auto& h:retryHooks) {
        Obj fn=eng::FindFunction(eng::FindClass(h.cls),h.name);
        if (!fn) continue;
        NativeFunction entry=nullptr; std::memcpy(&entry,fn+layout::kUFunctionNativeFunctionOffset,8);
        if (entry==PartyRetryHook) continue;
        // Only reflected Blueprint events; never patch the game's code/binaries.
        Obj reference=eng::FindFunction(eng::FindClass(h.cls),h.kind==3?"PlayWaterDeathSplash":"ToggleMouseCursor");
        NativeFunction interpreter=nullptr;
        if (reference) std::memcpy(&interpreter,reference+layout::kUFunctionNativeFunctionOffset,8);
        if (!entry || entry!=interpreter || !eng::InImage(reinterpret_cast<void*>(entry))) continue;
        if (h.kind==3) for (const auto& p:eng::ParamsOf(fn)) if (p.name=="OtherActor" && p.size==8) h.otherAt=p.offset;
        h.fn=fn;h.original=entry;
        retryInterpreter=entry;
        const NativeFunction hook=PartyRetryHook;std::memcpy(fn+layout::kUFunctionNativeFunctionOffset,&hook,8);
        hostlog::Info(std::string("party stage: scoped retry event: ")+h.name);
    }
}
void ContinuousRespawn(Stage& s) {
    const int kind=s.pendingRespawn;s.pendingRespawn=0;
    if (!kind || game::Seconds()-s.lastRespawn<0.2 || !race::Active() || race::Complete()) return;
    Obj ball=eng::Get(s.drivenBall),pc=game::PlayerController();
    if (!ball || !s.haveStart) return;
    auto transform=s.startTransform;
    if (kind!=2) {
        auto get=eng::Call(eng::FindCdo("BallestCourseModeLibrary"),"GetCourseRespawnTransform",pc);
        size_t n=0;const auto* t=get.Get("OutTransform",&n);
        if (get.ReturnBool() && t && n==transform.size()) std::memcpy(transform.data(),t,n);
    }
    runs::CaptureRespawnSample(false);
    struct MovementInterface {Obj object;void* implementation;} movement{};
    if (eng::ReadBytes(ball,"BallestMovementInterface",&movement,sizeof movement) && movement.object &&
        eng::FindFunction(eng::ClassOf(movement.object),"ResetBallMovementState"))
        eng::Call(movement.object,"ResetBallMovementState",uint8_t{1});
    eng::Params move(eng::FunctionOn(ball,"K2_SetActorTransform"));
    move.Set("NewTransform",transform.data(),transform.size());move.Set("bSweep",uint8_t{0});move.Set("bTeleport",uint8_t{1});
    if (!eng::Invoke(ball,move) || !move.ReturnBool()) { s.error="Party respawn transform was refused";return; }
    struct V3 {double x,y,z;}; const uint64_t noBone=0;
    if (Obj root=eng::Call(ball,"K2_GetRootComponent").ReturnObj()) {
        eng::Call(root,"SetPhysicsLinearVelocity",V3{},uint8_t{0},noBone);
        eng::Call(root,"SetPhysicsAngularVelocityInRadians",V3{},uint8_t{0},noBone);
    }
    runs::CaptureRespawnSample(true);
    race::TaintRun("Party continuous retry on own unpublished map");
    s.lastRespawn=game::Seconds();
    hostlog::Info("party stage: respawn kind="+std::to_string(kind)+" elapsed="+std::to_string(race::RaceElapsed())+"; same attempt, no countdown");
}
void CancelPostRaceMenu() {
    Obj ui=race::RaceUi();
    uint64_t handle=0;
    if (!ui || !eng::ReadBytes(ui,"WaitForPostScreen",&handle,sizeof handle) || !handle) return;
    Obj lib=eng::FindCdo("KismetSystemLibrary");
    if (eng::Call(lib,"K2_IsTimerActiveHandle",ui,handle).ReturnBool()) {
        eng::Call(lib,"K2_ClearAndInvalidateTimerHandle",ui,handle);
        const uint64_t cleared=0;
        eng::WriteBytes(ui,"WaitForPostScreen",&cleared,sizeof cleared);
        hostlog::Info("party stage: cancelled delayed native post-race menu");
    }
}
void BlockImprove(Stage& s) {
    Obj ui=race::RaceUi();
    if (!ui || s.token.empty() || s.building) return;
    for (const char* page:{"WBP_GameplayPause_2","WBP_PostTrackv2_5"}) {
        Obj group=eng::ReadObj(eng::ReadObj(ui,page),"WBP_MenuTextGroup");
        Obj box=eng::ReadObj(group,"Vbox");
        const int n=box ? eng::Call(box,"GetChildrenCount").ReturnAs<int>() : 0;
        for (int i=0;i<n && i<30;++i) {
            Obj button=eng::Call(box,"GetChildAt",i).ReturnObj();
            if (!button || std::any_of(s.checkedButtons.begin(),s.checkedButtons.end(),[&](const auto& w){return eng::Get(w)==button;})) continue;
            const auto text=ui::widgets::ReadText(eng::ReadObj(button,"ButtonText"));
            if (text.empty()) continue; // widget may still be constructing
            s.checkedButtons.push_back(eng::MakeWeak(button));
            if (text!="improve" && text!="Improve") continue;
            Obj hit=eng::ReadObj(button,"HitBox"); bool active=false;
            eng::ReadBool(button,"bActive",&active);
            s.blockedImprove.push_back({eng::MakeWeak(button),eng::MakeWeak(hit),eng::Call(button,"GetIsEnabled").ReturnBool(),hit && eng::Call(hit,"GetIsEnabled").ReturnBool(),active});
            hostlog::Info("party stage: disabled native Improve: "+eng::PathOf(button));
        }
    }
    for (const auto& b:s.blockedImprove) if (Obj button=eng::Get(b.widget)) {
        eng::Call(button,"SetIsActive",uint8_t{0});
        eng::Call(button,"SetIsEnabled",uint8_t{0});
        if (Obj hit=eng::Get(b.hitbox)) eng::Call(hit,"SetIsEnabled",uint8_t{0});
    }
}
void RestoreImprove(Stage& s) {
    for (const auto& b:s.blockedImprove) if (Obj button=eng::Get(b.widget)) {
        eng::Call(button,"SetIsActive",static_cast<uint8_t>(b.active));
        eng::Call(button,"SetIsEnabled",static_cast<uint8_t>(b.enabled));
        if (Obj hit=eng::Get(b.hitbox)) eng::Call(hit,"SetIsEnabled",static_cast<uint8_t>(b.hitEnabled));
    }
    s.blockedImprove.clear(); s.checkedButtons.clear();
}
void RestoreFinishedBall(Stage& s) {
    if (s.drivenGeneration==game::Generation()) if (Obj ball=eng::Get(s.retiredBall)) {
        eng::Call(ball,"SetActorEnableCollision",static_cast<uint8_t>(s.retiredCollision));
        eng::Call(ball,"SetActorTickEnabled",static_cast<uint8_t>(s.retiredTick));
        eng::Call(ball,"SetActorHiddenInGame",static_cast<uint8_t>(s.retiredHidden));
        if (Obj root=eng::Get(s.retiredRoot)) eng::Call(root,"SetSimulatePhysics",static_cast<uint8_t>(s.retiredPhysics));
    }
    s.retiredBall={}; s.retiredRoot={};
}
void RetireFinishedBall(Stage& s,bool timedOut=false) {
    if (s.drivenGeneration!=game::Generation() || eng::Get(s.retiredBall) || (!timedOut && !race::Complete())) return;
    Obj ball=eng::Get(s.drivenBall);
    if (!ball) { hostlog::Info("party stage: finished pawn already removed"); return; }
    s.retiredBall=s.drivenBall;
    s.retiredCollision=eng::Call(ball,"GetActorEnableCollision").ReturnBool();
    s.retiredTick=eng::Call(ball,"IsActorTickEnabled").ReturnBool();
    eng::ReadBool(ball,"bHidden",&s.retiredHidden);
    if (Obj root=eng::Call(ball,"K2_GetRootComponent").ReturnObj()) {
        const uint8_t noBone[8]={};
        s.retiredRoot=eng::MakeWeak(root);
        s.retiredPhysics=eng::Call(root,"IsSimulatingPhysics",noBone).ReturnBool();
        eng::Call(root,"SetSimulatePhysics",uint8_t{0});
    }
    eng::Call(ball,"SetActorEnableCollision",uint8_t{0});
    eng::Call(ball,"SetActorTickEnabled",uint8_t{0});
    eng::Call(ball,"SetActorHiddenInGame",uint8_t{1});
    hostlog::Info("party stage: finished pawn physics/collision/tick disabled after recording");
}
Obj GhostSubsystem() {
    return eng::Call(eng::FindCdo("SubsystemBlueprintLibrary"),"GetWorldSubsystem",game::PlayerController(),eng::FindClass("BallestGhostWorldSubsystem")).ReturnObj();
}
void HidePersonalBest(Stage& s) {
    Obj sub=GhostSubsystem();
    if (!sub) return;
    auto p=eng::Call(sub,"GetPlaybackPolicy"); size_t count=0; const auto* data=p.Return(&count);
    if (!data || count!=4) return;
    if (eng::Get(s.policySubsystem)!=sub) {
        std::memcpy(s.originalPolicy.data(),data,4); s.policySubsystem=eng::MakeWeak(sub); s.policySaved=true;
    }
    if (data[2]) {
        eng::Call(sub,"SetPersonalBestGhostsVisible",uint8_t{0});
        hostlog::Info("party stage: personal-best ghost hidden for this world");
    }
}
void ReleaseView(Stage& s,int owner) {
    draw::ReleaseCamera(owner);
    s.replayView=false;
}
void FrameView(Stage& s,int owner) {
    if (!s.replayView || s.replayGeneration!=game::Generation() || s.replayCameraPlaced) return;
    // Frame the authored course rather than the complete recordings. A DNF can
    // fall far below the arena and used to pull the fixed camera far away.
    // This view remains stable while every ghost plays and while results hold.
    const double x=(s.start.x+s.finish.x)*0.5;
    const double y=(s.start.y+s.finish.y)*0.5;
    const double z=(s.start.z+s.finish.z)*0.5+180;
    const double course=std::hypot(s.finish.x-s.start.x,s.finish.y-s.start.y);
    const double distance=std::clamp(course*0.72+650.0,3000.0,5600.0);
    const double cx=x-distance*0.22, cy=y-distance*0.62, cz=z+distance*0.42;
    constexpr double degrees=57.29577951308232;
    s.replayCameraPlaced=draw::SetCamera(owner,cx,cy,cz,std::atan2(z-cz,std::hypot(x-cx,y-cy))*degrees,std::atan2(y-cy,x-cx)*degrees,68);
}
const PaletteMap& Palette() {
    static PaletteMap result;
    if (!result.empty()) return result;
    const std::string base="/Game/LevelEditor/PackageAssets/MultiplayerLevelEditor/UI/Data/ContentBrowser/";
    for (const char* category:{"DA_BallestTracks","DA_BallestRamps","DA_BallestBowls","DA_BallestPrimitives","DA_Props","DA_Special","DA_Hazards","DA_BallestMisc"}) {
        Obj asset=cosmetics::LoadAsset(eng::Widen(base+category+"."+category));
        const auto prop=asset ? eng::FindProp(eng::ClassOf(asset),"Items") : eng::Prop{};
        const auto inner=eng::InnerOf(prop);
        struct Array { uint8_t* data; int32_t size,capacity; } items{};
        if (!prop || inner.size!=24 || !eng::ReadBytes(asset,"Items",&items,sizeof items) || !items.data || items.size<0 || items.size>1024) continue;
        for(int i=0;i<items.size;i++) {
            const auto* row=items.data+i*inner.size;
            uint32_t name[2]{}; Obj texture=nullptr,cls=nullptr;
            std::memcpy(name,row,8); std::memcpy(&texture,row+8,8); std::memcpy(&cls,row+16,8);
            if (cls && texture) result[eng::PathOf(cls)]={eng::Name(name[0],static_cast<int32_t>(name[1])),eng::PathOf(texture)};
        }
    }
    hostlog::Info("party stage: native palette metadata: "+std::to_string(result.size())+" items");
    return result;
}
std::string Lower(std::string value) {
    std::transform(value.begin(),value.end(),value.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
    return value;
}
bool AllowedPalettePath(const std::string& path) {
    const auto found=Palette().find(path);
    if (found==Palette().end()) return false;
    const std::string value=Lower(path+" "+found->second.first);
    // These items change race logic or are editor-only visuals rather than
    // Party Mode obstacles. Starts/finishes are protected along with checkpoints.
    for (const char* blocked:{"checkpoint","pointlight","point_light","point light","decal","start","finish","goal"})
        if (value.find(blocked)!=std::string::npos) return false;
    return true;
}
std::vector<std::string> AllowedPalette() {
    std::vector<std::string> result;
    for (const auto& entry:Palette()) if (AllowedPalettePath(entry.first)) result.push_back(entry.first);
    return result;
}
bool Fail(Stage& s, const std::string& why) { s.error = why; return false; }
bool Valid(Stage& s, const J& map) {
    const J* a = map.Get("items"), *b = s.base.Get("items");
    if (!a || !b || a->type != J::Array || b->type != J::Array || a->items.size() < b->items.size() || a->items.size() > ItemLimit(s) ||
        !map.Get("worldSettings") || Write(*map.Get("worldSettings")) != Write(*s.base.Get("worldSettings"))) return false;
    for (size_t i = 0; i < a->items.size(); ++i) {
        if (i < b->items.size() ? Write(a->items[i]) != Write(b->items[i]) : !Ordinary(a->items[i],s.finish)) return false;
    }
    return true;
}
std::wstring FullFile(Stage& s) {
    std::wstring file = eng::Widen(tracks::LocalMapsFolder()) + L"\\" + eng::Widen(s.file);
    std::replace(file.begin(),file.end(),L'/',L'\\');
    wchar_t full[32768]; const DWORD n = GetFullPathNameW(file.c_str(),32768,full,nullptr);
    return n>0 && n<32768 ? std::wstring(full,n) : file;
}
bool Save(Stage& s, int round) {
    if (s.token.empty() || round < 1 || round > 10256 || !Valid(s,s.map) || tracks::LocalMapsFolder().empty()) return Fail(s,"Map/session is not ready");
    s.file = "Map_LevelEditorMain&Party_" + s.token + "_" + std::to_string(round) + ".balledit";
    const std::string text = Write(s.map);
    HANDLE f = CreateFileW(FullFile(s).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        if (GetLastError() != ERROR_FILE_EXISTS) return Fail(s,"Cannot create the temporary round map");
        std::ifstream old(FullFile(s).c_str(),std::ios::binary); std::ostringstream contents; contents << old.rdbuf();
        return contents.str() == text || Fail(s,"A different map already occupies this session filename");
    }
    DWORD written = 0;
    const bool ok = WriteFile(f,text.data(),static_cast<DWORD>(text.size()),&written,nullptr) && written == text.size();
    CloseHandle(f);
    if (!ok) return Fail(s,"Cannot write the temporary round map");
    if (!RememberTemporary(s.file,text)) {
        DeleteFileW(FullFile(s).c_str());
        return Fail(s,"Cannot track temporary round map ownership");
    }
    return true;
}
template<class T> bool Field(const uint8_t* data, Obj structure, size_t size, const char* name, T& out) {
    const auto p = eng::FindProp(structure,name);
    if (!data || !p || p.size != sizeof(T) || p.offset < 0 || static_cast<size_t>(p.offset) + sizeof(T) > size) return false;
    std::memcpy(&out,data + p.offset,sizeof(T)); return true;
}
std::string SerializeWorld() {
    Obj lib = eng::FindCdo("SKGMLEStatics");
    eng::Params call(lib ? eng::FunctionOn(lib,"SerializeLevel") : nullptr);
    Obj pc = game::PlayerController();
    if (!pc || !call.Set("WorldContextObject",pc) || !call.Set("bSaveAsBase",uint8_t{0}) || !eng::Invoke(lib,call)) return "";
    const auto* ok = call.Get("Success");
    return ok && *ok && call.Return() ? eng::ReadFString(call.Return()) : "";
}
editor::Vec3 Vector(const J* object) { return {Num(object,"x"),Num(object,"y"),Num(object,"z")}; }
std::vector<int> SpawnItem(const J& item) {
    const J* transform = item.Get("itemTransform"), *quat = At(transform,"rotation");
    struct Quat { double x,y,z,w; } q{Num(quat,"x"),Num(quat,"y"),Num(quat,"z"),Num(quat,"w",1)};
    const auto rot = eng::Call(eng::FindCdo("KismetMathLibrary"),"Quat_Rotator",q).ReturnAs<editor::Rot>();
    return editor::SpawnPieces({{Class(item),Vector(At(transform,"translation")),rot,{1,1,1}}},false);
}
// A bounded content codec, no transport: Windows XPRESS_HUFF + base64, over existing lobby chat.
using CreateCodec = BOOL(WINAPI*)(DWORD,void*,void**);
using CloseCodec = BOOL(WINAPI*)(void*);
using Code = BOOL(WINAPI*)(void*,void*,SIZE_T,void*,SIZE_T,SIZE_T*);
struct Codec {
    CreateCodec create = nullptr; CloseCodec close = nullptr; Code code = nullptr;
    explicit Codec(bool compress) {
        static HMODULE module = LoadLibraryW(L"cabinet.dll");
        if (!module) return;
        create = reinterpret_cast<CreateCodec>(GetProcAddress(module,compress ? "CreateCompressor" : "CreateDecompressor"));
        close = reinterpret_cast<CloseCodec>(GetProcAddress(module,compress ? "CloseCompressor" : "CloseDecompressor"));
        code = reinterpret_cast<Code>(GetProcAddress(module,compress ? "Compress" : "Decompress"));
    }
};
const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
std::string Encode(const std::string& bytes) {
    std::string out; unsigned buffer = 0; int bits = 0;
    for (unsigned char c : bytes) { buffer = (buffer << 8) | c; bits += 8; while (bits >= 6) { bits -= 6; out += alphabet[(buffer >> bits) & 63]; } }
    if (bits) out += alphabet[(buffer << (6-bits)) & 63];
    return out;
}
std::string Decode(const std::string& text) {
    if (text.size() > kLimit*2 || text.size()%4 == 1) return "";
    std::string out; unsigned buffer = 0; int bits = 0;
    for (char c : text) { const size_t v = alphabet.find(c); if (v == std::string::npos) return ""; buffer = (buffer << 6) | static_cast<unsigned>(v); bits += 6; if (bits >= 8) { bits -= 8; out += static_cast<char>((buffer >> bits)&255); } }
    return out;
}
} // namespace

std::string Digest(const std::string& text) { return text.size() <= kLimit ? net::Sha256(text) : ""; }
std::string Pack(const std::string& text) {
    if (text.empty() || text.size() > kLimit) return "";
    Codec c(true); void* handle = nullptr; SIZE_T count = 0;
    if (!c.create || !c.close || !c.code || !c.create(4,nullptr,&handle)) return "";
    c.code(handle,const_cast<char*>(text.data()),text.size(),nullptr,0,&count);
    if (!count || count > kLimit*2) { c.close(handle); return ""; }
    std::string bytes(count + 4,'\0'); const uint32_t length = static_cast<uint32_t>(text.size()); std::memcpy(bytes.data(),&length,4);
    const bool ok = c.code(handle,const_cast<char*>(text.data()),text.size(),bytes.data()+4,count,&count) != 0;
    c.close(handle); if (!ok) return ""; bytes.resize(count+4); return Encode(bytes);
}
std::string Unpack(const std::string& text) {
    std::string bytes = Decode(text); uint32_t length = 0;
    if (bytes.size() < 5) return ""; std::memcpy(&length,bytes.data(),4);
    if (!length || length > kLimit) return "";
    Codec c(false); void* handle = nullptr; SIZE_T written = 0;
    if (!c.create || !c.close || !c.code || !c.create(4,nullptr,&handle)) return "";
    std::string out(length,'\0'); const bool ok = c.code(handle,bytes.data()+4,bytes.size()-4,out.data(),length,&written) != 0;
    c.close(handle); return ok && written == length ? out : "";
}
bool Reset(const std::string& token) {
    Stage& s = S();
    if (plugins::Current() < 0 || token.empty() || token.size() > 40 || token.find_first_not_of("0123456789abcdef") != std::string::npos) return Fail(s,"Invalid session token");
    Stage fresh;
    std::ifstream file((plugins::CurrentDir()+L"\\maps\\party_base.balledit").c_str(),std::ios::binary); std::ostringstream text; text << file.rdbuf();
    if (!file || !Parse(text.str(),fresh.base) || !fresh.base.Get("items") || fresh.base.Get("items")->type!=J::Array || fresh.base.Get("items")->items.size()<2 || fresh.base.Get("items")->items.size()>512 || !fresh.base.Get("worldSettings")) return Fail(s,"The bundled base map is missing or invalid");
    const J* data = At(fresh.base.Get("worldSettings"),"levelData");
    if (Num(data,"levelPublishedFileId",999) != -1) return Fail(s,"The bundled map is not unpublished");
    int starts=0,finishes=0;
    for (const auto& item:fresh.base.Get("items")->items) {
        const auto cls=Class(item);const auto pos=Vector(At(item.Get("itemTransform"),"translation"));
        if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z) || std::max({std::abs(pos.x),std::abs(pos.y),std::abs(pos.z)})>1000000) return Fail(s,"The bundled track has an invalid object position");
        if (cls=="/Game/MapItems/PlayerStart/MP_PlayerStart.MP_PlayerStart_C") {fresh.start=pos;++starts;}
        if (cls=="/Game/Core/Gameplay/MapTriggers/BP_Checkpoint.BP_Checkpoint_C") {fresh.finish=pos;++finishes;}
    }
    if (starts!=1 || finishes!=1) return Fail(s,"The bundled track must have one start and one finish");
    fresh.placement={fresh.start.x+455,fresh.start.y-715,fresh.start.z+32};
    hostlog::Info("party stage: base items="+std::to_string(fresh.base.Get("items")->items.size())+" finish="+std::to_string(fresh.finish.x)+","+std::to_string(fresh.finish.y)+","+std::to_string(fresh.finish.z));
    fresh.map = fresh.base; fresh.token = token; s = std::move(fresh); return true;
}
std::string Snapshot() { Stage& s = S(); return s.token.empty() ? "" : Write(s.map); }
bool SetSnapshot(const std::string& text) {
    Stage& s = S(); J map;
    if (!Parse(text,map) || !Valid(s,map)) return Fail(s,"Map snapshot changed protected base data or contains an invalid item");
    s.map = std::move(map); return true;
}
bool OpenBuild(int round) {
    Stage& s = S();
    if (!Save(s,round)) return false;
    ReleaseView(s,plugins::Current());
    nativeghost::CloseSession(); hud::HideGame(plugins::Current(),true,false); hud::HideEditor(plugins::Current(),true);
    s.replayPrepared=false;
    s.building = true; s.generation = -1; s.chosen = {}; s.fixed.clear(); s.offers.clear(); s.choice = -1; s.committed = false; s.started = false; s.finished.clear();
    s.openingFrom = game::Generation();
    return together::OpenMap(eng::Narrow(FullFile(s).c_str(),static_cast<int>(FullFile(s).size()))) || Fail(s,"The native editor could not open the round map");
}
bool BuildReady() {
    Stage& s = S();
    if (!s.building || game::Generation() == s.openingFrom || !editor::Open() || !editor::OwnHandler() || together::IsServer() || together::IsClient()) return false;
    if (s.generation == game::Generation()) return true;
    const auto pieces = editor::Pieces();
    if (pieces.size() != s.map.Get("items")->items.size()) return false;
    s.fixed.clear();
    for (int id : pieces) {
        Stage::Fixed fixed; Obj actor = eng::ObjectAt(id);
        if (!actor || !editor::Location(id,&fixed.at) || !editor::Rotation(id,&fixed.rot) || !editor::Scale(id,&fixed.scale)) return false;
        fixed.actor = eng::MakeWeak(actor); s.fixed.push_back(fixed);
    }
    s.generation = game::Generation();
    CleanupTemporaryMaps();
    hud::HideGame(plugins::Current(),false);
    s.offers = AllowedPalette();
    if (s.offers.size()<3) return Fail(s,"The native editor palette has fewer than three safe objects");
    std::mt19937 random(static_cast<uint32_t>(GetTickCount64()) ^ static_cast<uint32_t>(std::strtoull(lobby::OwnId().c_str(),nullptr,10)));
    std::shuffle(s.offers.begin(),s.offers.end(),random); s.offers.resize(3);
    return true;
}
std::vector<std::string> Offers() {
    std::vector<std::string> result;
    if (BuildReady()) for (const auto& path : S().offers) {
        auto found=Palette().find(path);
        result.push_back(found==Palette().end() ? path : found->second.first);
    }
    return result;
}
std::vector<std::string> OfferIcons() {
    std::vector<std::string> result;
    if (BuildReady()) for (const auto& path:S().offers) {
        auto found=Palette().find(path);
        result.push_back(found==Palette().end() ? "" : found->second.second);
        if(found!=Palette().end()) hostlog::Info("party stage: native offer "+found->second.first+" icon="+found->second.second);
    }
    return result;
}
int Choose(int index) {
    Stage& s = S(); if (!BuildReady() || s.choice >= 0 || index < 0 || index >= 3) return 0;
    // In front of the original start, off the driving line until the player places it.
    auto ids = editor::SpawnPieces({{s.offers[static_cast<size_t>(index)],s.placement,{0,0,0},{1,1,1}}},false);
    if (ids.size() != 1) { Fail(s,"The chosen native piece could not be spawned"); return 0; }
    s.choice = index; s.chosen = eng::MakeWeak(eng::ObjectAt(ids[0])); editor::Select(ids); return ids[0];
}
std::string Capture() {
    Stage& s = S(); Obj actor = eng::Get(s.chosen);
    if (!BuildReady() || !actor || s.committed) { Fail(s,"Choose and place one item first"); return ""; }
    if (editor::Pieces().size()!=s.map.Get("items")->items.size()+1) { Fail(s,"Only the one offered object may be added in this round"); return ""; }
    const auto bounds = eng::Call(actor,"GetActorBounds",uint8_t{1},editor::Vec3{},editor::Vec3{},uint8_t{0});
    const auto* origin = bounds.Get("Origin"); const auto* extent = bounds.Get("BoxExtent");
    if (origin && extent) {
        editor::Vec3 o{}, e{}; std::memcpy(&o,origin,sizeof o); std::memcpy(&e,extent,sizeof e);
        const double x = std::max(0.0,std::abs(o.x-s.finish.x)-e.x), y = std::max(0.0,std::abs(o.y-s.finish.y)-e.y), z = std::max(0.0,std::abs(o.z-s.finish.z)-e.z);
        if (x*x+y*y+z*z < 500*500) { Fail(s,"Move the entire object away from the protected finish area"); return ""; }
    } else { Fail(s,"Cannot verify the object's bounds"); return ""; }
    J exported; const std::string raw = SerializeWorld();
    if (!Parse(raw,exported) || !exported.Get("items")) { Fail(s,"Native map serialization failed"); return ""; }
    editor::Vec3 pos; if (!editor::Location(s.chosen.index,&pos)) return "";
    for (const J& item : exported.Get("items")->items) {
        if (Class(item) != s.offers[static_cast<size_t>(s.choice)]) continue;
        const auto at = Vector(At(item.Get("itemTransform"),"translation"));
        if (std::abs(at.x-pos.x)+std::abs(at.y-pos.y)+std::abs(at.z-pos.z) > 0.01) continue;
        if (!Ordinary(item,s.finish)) { Fail(s,"The object must have unit scale and stay outside the finish area"); return ""; }
        s.committed = true;
        Stage::Fixed fixed; fixed.actor = s.chosen;
        editor::Location(s.chosen.index,&fixed.at); editor::Rotation(s.chosen.index,&fixed.rot); editor::Scale(s.chosen.index,&fixed.scale);
        s.fixed.push_back(fixed); editor::Select({}); s.captured=Write(item); return s.captured;
    }
    Fail(s,"The native export did not contain the placed object"); return "";
}
bool Append(const std::string& text, bool local) {
    Stage& s = S(); J item;
    if (!Parse(text,item) || !Ordinary(item,s.finish) || !s.map.Get("items") || s.map.Get("items")->items.size() >= ItemLimit(s)) return Fail(s,"Invalid shared native item");
    if (local && (!eng::Get(s.chosen) || !s.committed || text!=s.captured)) return Fail(s,"The local item is not the committed native object");
    if (editor::Open() && !local) {
        const auto selection=editor::Selection();
        const auto made=SpawnItem(item);
        if (made.size()!=1) return Fail(s,"The shared native object could not be shown");
        Stage::Fixed fixed; fixed.actor=eng::MakeWeak(eng::ObjectAt(made[0]));
        editor::Location(made[0],&fixed.at); editor::Rotation(made[0],&fixed.rot); editor::Scale(made[0],&fixed.scale);
        s.fixed.push_back(fixed); editor::Select(selection);
    }
    At(s.map,"items")->items.push_back(item);
    return true;
}
bool OpenRace(int round) {
    Stage& s = S();
    // Build used this filename already; racing needs a fresh filename for the updated snapshot.
    if (!Save(s,round+10000)) return false;
    s.building = false; s.started = false; s.haveStart=false; s.physicalStartSaved=false; s.pendingRespawn=0; s.finished.clear(); s.oldRun = runs::Count() ? runs::Id(0) : "";
    hud::HideGame(plugins::Current(),false);
    return tracks::OpenLocal(s.file) || Fail(s,"The native local race could not open");
}
bool RaceReady() {
    Stage& s = S(); const auto& track = race::CurrentTrack();
    const bool ready=!s.building && !s.file.empty() && track.file.find(s.file) != std::string::npos && race::OnTrack() && !editor::Open() && race::RacingBall() && !race::Active() && !s.started;
    if (ready && !s.haveStart) {
        auto pose=eng::Call(race::RacingBall(),"GetTransform");size_t n=0;const auto* t=pose.Return(&n);
        if (t && n==s.startTransform.size()) {std::memcpy(s.startTransform.data(),t,n);s.haveStart=true;}
        InstallRetryHooks();
    }
    if (ready && s.haveStart) CleanupTemporaryMaps();
    return ready && s.haveStart;
}
bool SetRaceLimit(int seconds) {
    Stage& s=S();if (s.token.empty() || seconds<1 || seconds>600 || s.started) return Fail(s,"Party race limit must be 1 to 600 seconds before a race starts");
    s.raceLimit=seconds;return true;
}
double RaceRemaining() {
    Stage& s=S();if (!s.started || s.drivenGeneration!=game::Generation()) return s.raceLimit;
    return std::max(0.0,s.raceLimit-std::max(0.0,race::RaceElapsed()));
}
bool ReplayFinished(int player) {return player>=0 && static_cast<size_t>(player)<S().completed.size() && S().completed[static_cast<size_t>(player)];}
bool StartRace() {
    Stage& s = S();
    if (s.building || s.started || s.file.empty() || race::CurrentTrack().file.find(s.file)==std::string::npos || !race::OnTrack() || editor::Open()) return Fail(s,"The native race is not ready");
    Obj pc = game::PlayerController(), ps = eng::ReadObj(pc,"PlayerState");
    const auto prop = ps ? eng::FindProp(eng::ClassOf(ps),"RaceRunState") : eng::Prop{};
    Obj structure = prop ? eng::StructOf(prop) : nullptr;
    int64_t attempt = 0, revision = 0; uint8_t phase=0;
    if (!structure || !Field(ps+prop.offset,structure,static_cast<size_t>(prop.size),"AttemptSerial",attempt) || !Field(ps+prop.offset,structure,static_cast<size_t>(prop.size),"CourseRevision",revision) || !Field(ps+prop.offset,structure,static_cast<size_t>(prop.size),"Phase",phase)) return Fail(s,"Native race identity is unavailable");
    Obj requests = eng::Call(ps,"GetRaceRequests").ReturnObj();
    s.drivenBall=eng::MakeWeak(race::PlayedBallActor()); s.drivenGeneration=game::Generation();
    s.retiredBall={}; s.retiredRoot={};
    // The native Play button may have already started this same local attempt.
    // Adopt it, including its countdown, instead of issuing a second restart.
    if (phase==1 || phase==2) {
        s.started=true; s.run=static_cast<int>(attempt);
        hostlog::Info("party stage: adopted native Play; attempt="+std::to_string(attempt));
        return true;
    }
    if (!RaceReady()) return Fail(s,"The native race is not ready");
    const auto start = eng::Call(requests,"RequestStart",attempt,revision);
    if (!start.ReturnBool()) return Fail(s,"The native race rejected RequestStart");
    s.started = true; s.run = static_cast<int>(attempt+1); s.oldRun = runs::Count() ? runs::Id(0) : "";
    hostlog::Write("info",plugins::CurrentId(),"party stage: native start accepted; map="+s.file+" previousAttempt="+std::to_string(attempt));
    return true;
}
std::string FinishedReplay() {
    Stage& s = S(); if (!s.finished.empty()) return s.finished;
    if (!s.started || s.drivenGeneration!=game::Generation()) return "";
    const bool expired=race::RaceElapsed()>=s.raceLimit;
    if (!expired && (!race::Complete() || runs::Count()<1 || runs::Id(0)==s.oldRun || !runs::Finished(0) || runs::TrackKey(0)!=race::CurrentTrack().key)) return "";
    const auto raw=expired && !race::Complete() ? runs::CurrentReplayText() : runs::ReplayText(0);
    ghostdata::Replay replay; std::string why;
    if (!ghostdata::Parse(raw,&replay,&why) || replay.samples.size()<2 || replay.time<=0 || replay.time>3600) { Fail(s,"The just-finished run has no valid replay"); return ""; }
    J value;if (!Parse(raw,value)) return "";
    const bool completed=race::Complete() && replay.time<=s.raceLimit;
    J flag;flag.type=J::Bool;flag.boolean=completed;value.members.emplace_back("?PartyFinished",flag);
    if (!completed) {
        if (auto* t=At(value,"bestTime")) t->number=s.raceLimit;
        const auto* stamps=value.Get("elapsedTime");size_t keep=0;
        if (stamps) while (keep<stamps->items.size() && stamps->items[keep].number<=s.raceLimit) ++keep;
        for (const char* key:{"elapsedTime","locations","rotation","velocities","controlRotations"}) if (auto* a=At(value,key)) a->items.resize(std::min(keep,a->items.size()));
        // Hold the final observed pose until the deadline so the native replay
        // transport has the same duration as the DNF outcome.
        if (auto* t=At(value,"elapsedTime"); t && !t->items.empty() && t->items.back().number<s.raceLimit) {
            J deadline;deadline.type=J::Number;deadline.number=s.raceLimit;t->items.push_back(deadline);
            for (const char* key:{"locations","rotation","velocities","controlRotations"})
                if (auto* a=At(value,key); a && !a->items.empty()) { const J last=a->items.back();a->items.push_back(last); }
        }
        hostlog::Info("party stage: race time expired; DNF at "+std::to_string(s.raceLimit)+" s");
    }
    s.finished = Write(value);
    CancelPostRaceMenu();
    hud::HideGame(plugins::Current(),true,false);
    RetireFinishedBall(s,!completed);
    return s.finished;
}
bool PrepareReplayGroup(const std::vector<std::string>& ids,const std::vector<std::string>& recordings) {
    Stage& s=S();std::string why;
    if (ids.size()<2 || ids.size()>4 || ids.size()!=recordings.size()) return Fail(s,"A replay needs 2 to 4 participants");
    std::vector<ghostdata::Replay> paths(ids.size());std::vector<bool> completed;std::vector<double> times;
    for (size_t i=0;i<ids.size();++i) {
        if (ids[i].empty() || std::find(ids.begin(),ids.begin()+static_cast<ptrdiff_t>(i),ids[i])!=ids.begin()+static_cast<ptrdiff_t>(i) || !ghostdata::Parse(recordings[i],&paths[i],&why) || paths[i].samples.size()<2 || paths[i].time<=0 || paths[i].time>3600) return Fail(s,"Invalid or duplicate participant recording");
        J metadata;if (!Parse(recordings[i],metadata)) return Fail(s,"Invalid Party replay metadata");
        const auto* finished=metadata.Get("?PartyFinished");completed.push_back(!finished || (finished->type==J::Bool && finished->boolean));times.push_back(paths[i].time);
    }
    auto presentationId = [](const std::string& id) {
        // Test lobbies use t<pid>, not Steam accounts. Only in the completely
        // offline sandbox, map those test keys to distinct valid presentation
        // keys. Never manufacture identities in a player's connected game.
        if (sandbox::On() && id.size()>1 && id[0]=='t' && id.find_first_not_of("0123456789",1)==std::string::npos)
            return std::to_string(76561197960265728ull + std::strtoull(id.c_str()+1,nullptr,10));
        return id;
    };
    auto named = [](const std::string& id,const std::string& raw) {
        J value; if (!Parse(raw,value)) return raw;
        for (const auto& member : lobby::Members()) if (member.id==id) {
            if (J* name=At(value,"username")) *name=Text(member.name);
        }
        return Write(value);
    };

    s.paths=std::move(paths);s.completed=std::move(completed);s.times=std::move(times);
    s.replayIds.clear();s.replayNames.clear();
    for (const auto& id:ids) {
        s.replayIds.push_back(presentationId(id));std::string name="Player";
        for(const auto& member:lobby::Members()) if(member.id==id) name=member.name;
        s.replayNames.push_back(name);
    }
    s.replayHeld=false;s.replayPeak=0;s.replayGeneration=game::Generation();
    s.watchId=s.replayIds[0];nativeghost::SetRealSkins(true);
    if (!nativeghost::OpenAsLocal(s.watchId,&why)) return Fail(s,"Native replay preparation: "+why);
    for(size_t i=0;i<ids.size();++i) {
        if (!nativeghost::Load(s.replayIds[i],named(ids[i],recordings[i]),&why) || !nativeghost::Select(s.replayIds[i],1,&why)) return Fail(s,"Native replay participant: "+why);
    }
    s.replayPrepared=true;return true;
}
bool ReplayReady() { Stage& s = S(); return s.replayPrepared && !s.replayIds.empty() && std::all_of(s.replayIds.begin(),s.replayIds.end(),[](const auto& id){return nativeghost::Ready(id);}); }
bool StartReplay() {
    Stage& s = S(); std::string why;
    if (!ReplayReady()) return Fail(s,"The native replay records are not ready");
    Obj pc=game::PlayerController();
    Obj sub=eng::Call(eng::FindCdo("SubsystemBlueprintLibrary"),"GetWorldSubsystem",pc,eng::FindClass("BallestGhostWorldSubsystem")).ReturnObj();
    hud::HideGame(plugins::Current(),true,false);
    eng::Call(sub,"SetSpectateRate",1.0);
    eng::Call(sub,"SetSpectatePaused",uint8_t{0});
    const auto playback=eng::Call(sub,"StartPlayback",0.0);
    const uint8_t* accepted=playback.Return();
    if (!accepted || !accepted[0]) return Fail(s,"Native ghost playback could not begin");
    // Generic native ghost playback plus our own fixed camera. Native POV has
    // a separate follow/pause/menu lifecycle and must not own this presentation.
    CancelPostRaceMenu();
    if (!draw::TakeCamera(plugins::Current())) return Fail(s,"The replay camera is in use by another plugin");
    s.replayView=true; s.replayCameraPlaced=false; s.replayPauseFlags=-1; s.replayLastFrame=-1; FrameView(s,plugins::Current());
    hostlog::Info("native ghost: prepared replay camera framing the course");
    return true;
}
bool HoldReplay() {
    Stage& s=S();
    if (!s.replayPrepared || !s.replayView || s.replayGeneration!=game::Generation()) return Fail(s,"No replay is active");
    if (s.replayHeld) return true;
    const double now=eng::Call(GhostSubsystem(),"GetPlaybackTime").ReturnAs<double>();
    if (std::isfinite(now) && now>=0) s.replayPeak=std::max(s.replayPeak,now);
    s.replayHeld=true;
    if (Obj sub=GhostSubsystem()) eng::Call(sub,"StopPlayback");
    draw::TakeCamera(plugins::Current());
    hostlog::Info("party stage: replay completed; native playback stopped without a pause request");
    return true;
}
double ReplayClock() {
    Stage& s=S();
    if (s.replayHeld) return s.replayPeak;
    if (!s.replayPrepared || !s.replayView || s.replayGeneration!=game::Generation()) return -1;
    const double now=eng::Call(GhostSubsystem(),"GetPlaybackTime").ReturnAs<double>();
    if (std::isfinite(now) && now>=0) s.replayPeak=std::max(s.replayPeak,now);
    if (s.replayPeak>=*std::max_element(s.times.begin(),s.times.end())) {
        // Retain the final result clock, but never put the game into its pause
        // presentation. The script advances immediately or shows its own end card.
        HoldReplay();
    }
    return s.replayPeak;
}
double FinishTime(int player) { return player>=0 && static_cast<size_t>(player)<S().times.size() ? S().times[static_cast<size_t>(player)] : -1; }
std::string Error() { return S().error; }
void Frame() {
    Obj menuController=game::PlayerController();
    if (menuController && eng::ObjName(eng::ClassOf(menuController))=="BP_MainMenuController_C" && game::Seconds()>=nextTemporaryCleanup &&
        std::none_of(stages.begin(),stages.end(),[](const auto& entry){return !entry.second.token.empty();})) {
        nextTemporaryCleanup=game::Seconds()+1;CleanupTemporaryMaps();
    }
    for (auto& [owner,s] : stages) {
        if (s.started && !s.physicalStartSaved && s.drivenGeneration==game::Generation() && race::Active() && !race::Complete()) {
            // Pretrack presentation may park the pawn away from its race spawn.
            // Capture the course's physical start once the initial run is live.
            auto pose=eng::Call(eng::FindCdo("BallestCourseModeLibrary"),"GetCourseRespawnTransform",game::PlayerController());
            size_t n=0;const auto* t=pose.Get("OutTransform",&n);
            if (pose.ReturnBool() && t && n==s.startTransform.size()) {
                std::memcpy(s.startTransform.data(),t,n);s.haveStart=true;s.physicalStartSaved=true;
            }
        }
        if (!s.token.empty() && s.started && !s.building && s.finished.empty() && s.drivenGeneration==game::Generation()) ContinuousRespawn(s);
        BlockImprove(s);
        if (s.replayView && s.replayGeneration==game::Generation()) {
            CancelPostRaceMenu();
            Obj pc=game::PlayerController(),sub=GhostSubsystem();
            const int flags=(race::Paused()?1:0) | (eng::Call(sub,"IsSpectatePaused").ReturnBool()?2:0) |
                (eng::Call(pc,"IsNativeLocalPauseRequested").ReturnBool()?4:0) | (eng::Call(pc,"GetNativePOVOpenedPauseMenu").ReturnBool()?8:0);
            if (flags!=s.replayPauseFlags) {
                s.replayPauseFlags=flags;
                hostlog::Info("party stage: replay pause flags="+std::to_string(flags)+" complete="+std::to_string(s.replayHeld));
            }
            const double now=game::Seconds();
            if (s.replayLastFrame>=0 && now-s.replayLastFrame>0.1 && now-s.replayGapLogged>1) {
                s.replayGapLogged=now;
                hostlog::Warn("party stage: replay frame gap ms="+std::to_string((now-s.replayLastFrame)*1000));
            }
            s.replayLastFrame=now;
        }
        if (!s.token.empty() && race::OnTrack() && !editor::Open()) HidePersonalBest(s);
        FrameView(s,owner);
        if (s.replayPrepared && s.replayView && s.replayGeneration==game::Generation()) {
            for(size_t i=0;i<s.replayIds.size();++i) nativeghost::NameWatchedTag(s.replayIds[i],s.replayNames[i]);
        }
        if (!s.building || s.generation != game::Generation() || !editor::Open()) continue;
        // Preserve every earlier object; only the current offered object may move/rotate.
        for (const auto& f : s.fixed) if (Obj actor=eng::Get(f.actor)) {
            editor::Vec3 at,scale; editor::Rot rot;
            if (editor::Location(f.actor.index,&at) && std::memcmp(&at,&f.at,sizeof at)!=0) editor::SetLocation(f.actor.index,f.at);
            if (editor::Rotation(f.actor.index,&rot) && std::memcmp(&rot,&f.rot,sizeof rot)!=0) editor::SetRotation(f.actor.index,f.rot);
            if (editor::Scale(f.actor.index,&scale) && std::memcmp(&scale,&f.scale,sizeof scale)!=0) eng::Call(actor,"SetActorScale3D",f.scale);
        }
        if (Obj actor=eng::Get(s.chosen)) {
            editor::Vec3 scale; if (editor::Scale(s.chosen.index,&scale) && (scale.x!=1 || scale.y!=1 || scale.z!=1)) eng::Call(actor,"SetActorScale3D",editor::Vec3{1,1,1});
        }
    }
}
void End() { Stopped(plugins::Current()); }
void Stopped(int plugin) {
    auto it=stages.find(plugin);
    if (it==stages.end()) return;
    RestoreImprove(it->second);
    RestoreFinishedBall(it->second);
    ReleaseView(it->second,plugin);
    if (it->second.replayPrepared) {
        if (it->second.replayGeneration==game::Generation()) nativeghost::CloseSession();
        nativeghost::SetRealSkins(false);
    }
    hud::HideGame(plugin,false);
    hud::HideEditor(plugin,false);
    if (it->second.policySaved) if (Obj sub=eng::Get(it->second.policySubsystem)) {
        eng::Params restore(eng::FunctionOn(sub,"SetPlaybackPolicy"));
        restore.Set("InPolicy",it->second.originalPolicy.data(),it->second.originalPolicy.size()); eng::Invoke(sub,restore);
        hostlog::Info("party stage: personal-best playback policy restored");
    }
    stages.erase(it);
}
} // namespace partystage
