#include "splitref.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "cosmetics.hpp"
#include "engine.hpp"
#include "game.hpp"
#include "log.hpp"
#include "race.hpp"
#include "runs.hpp"
#include "widgets.hpp"

namespace splitref {
namespace {

using eng::Obj;
namespace w = ui::widgets;

struct DoubleArray {                    // TArray<double> as UE lays it out
    double* data;
    int32_t num, max;
};

int gOwner = -1;
std::string gKey;
std::vector<double> gSplits;
double gTotal = 0;

eng::Weak gWidget;                      // the race UI's WBP_Split
std::vector<double> gOriginal;          // what the game had put there, to give back on Clear
eng::Weak gOriginalOf;
bool gOverridden = false;
std::string gLastLog;
double gFinishedAt = -1;                // when the finish on the reference's track was seen (game seconds)
std::string gFinishedRun;

bool Applies() { return gOwner >= 0 && !gKey.empty() && race::OnTrack() && race::CurrentTrack().key == gKey; }

Obj SplitWidget() {
    static double lastLook = -100;
    if (Obj o = eng::Get(gWidget)) return o;
    if (game::Seconds() - lastLook < 1) return nullptr;
    lastLook = game::Seconds();
    Obj cls = eng::FindClass("WBP_Split_C");
    if (!cls) return nullptr;
    Obj found = nullptr;
    eng::ForEachObject([&](Obj o) {
        if (eng::ClassOf(o) == cls && !eng::IsDefaultObject(o) && eng::PathOf(o).rfind("/Engine/Transient", 0) == 0) found = o;
        return found == nullptr;
    });
    gWidget = eng::MakeWeak(found);
    return found;
}

DoubleArray* PBSplits(Obj widget) {
    const eng::Prop p = eng::FindProp(eng::ClassOf(widget), "PBSplits");
    if (!p || p.size != sizeof(DoubleArray)) return nullptr;
    return reinterpret_cast<DoubleArray*>(widget + p.offset);
}

std::vector<double> Read(const DoubleArray* a) {
    std::vector<double> v;
    if (a && a->data && a->num > 0 && a->num <= a->max && a->num < 10000) v.assign(a->data, a->data + a->num);
    return v;
}

// A TArray<double> the engine allocated, holding `values`: BFL_MathHelperLibrary.OrderFloatArray(ascending, in, out)
// copies its input into its output array (sorted; splits are increasing already, so the same order).
bool EngineArray(Obj widget, const std::vector<double>& values, DoubleArray* out) {
    Obj lib = eng::FindCdo("BFL_MathHelperLibrary_C");
    Obj fn = lib ? eng::FunctionOn(lib, "OrderFloatArray") : nullptr;
    if (!fn) return false;
    eng::Params p(fn);
    DoubleArray in{const_cast<double*>(values.data()), static_cast<int32_t>(values.size()), static_cast<int32_t>(values.size())};
    p.Set("bDescending", uint8_t{0});
    p.Set("ArrayIn", in);
    p.Set("__WorldContext", widget);
    if (!eng::Invoke(lib, p)) return false;
    size_t size = 0;
    const uint8_t* got = p.Get("ArrayOut", &size);
    if (!got || size != sizeof(DoubleArray)) return false;
    std::memcpy(out, got, sizeof *out);
    // the params buffer is ours: the input still points at `values` (never freed by the engine), the output now
    // belongs to whoever takes it
    std::memset(const_cast<uint8_t*>(got), 0, sizeof(DoubleArray));
    return out->num == static_cast<int32_t>(values.size()) && (values.empty() || out->data);
}

// Puts `values` into the widget's PBSplits: in place when it has room, else in an array the engine allocated (the old
// buffer, the engine's own, is left as it is: a few bytes, once per map at most).
bool Write(Obj widget, DoubleArray* a, const std::vector<double>& values) {
    const int32_t n = static_cast<int32_t>(values.size());
    if (n <= a->max && (n == 0 || a->data)) {
        if (n > 0) std::memcpy(a->data, values.data(), sizeof(double) * values.size());
        a->num = n;
        return true;
    }
    DoubleArray fresh{};
    if (!EngineArray(widget, values, &fresh)) return false;
    *a = fresh;
    return true;
}

std::string Join(const std::vector<double>& v) {
    std::string s;
    char b[32];
    for (size_t i = 0; i < v.size(); ++i) {
        std::snprintf(b, sizeof b, "%s%.3f", i ? "," : "", v[i]);
        s += b;
    }
    return s;
}

void GiveBack() {
    if (!gOverridden) return;
    gOverridden = false;
    Obj widget = eng::Get(gWidget);
    if (!widget || widget != eng::Get(gOriginalOf)) return;         // a new map's widget has the game's own anyway
    if (DoubleArray* a = PBSplits(widget)) Write(widget, a, gOriginal);
    hostlog::Info("splits: the game's own reference is back");
}

// The finish: its delta against the reference's total (or none), over what the game shows for 4 seconds.
void Finish(Obj widget) {
    if (!race::Complete()) return;
    if (runs::Count() <= 0 || !runs::Finished(0) || runs::TrackKey(0) != gKey || runs::Age(0) > 5) return;
    const std::string run = runs::Id(0);
    if (run != gFinishedRun) {
        gFinishedRun = run;
        gFinishedAt = game::Seconds();
    }
    if (game::Seconds() - gFinishedAt > 4) return;
    if (gTotal <= 0) {
        w::SetVisibility(widget, 2);                // Hidden, as the widget hides itself
        return;
    }
    const double delta = runs::Time(0) - gTotal;
    Obj value = eng::ReadObj(widget, "SplitValue"), text = eng::ReadObj(widget, "SplitDelta"), bg = eng::ReadObj(widget, "SplitBG");
    if (!value || !text) return;
    const std::string sign = delta >= 0 ? "+" : "-";
    char shown[32];
    const double a = std::fabs(delta);
    const int ms = static_cast<int>(std::lround(a * 1000));
    std::snprintf(shown, sizeof shown, "%02d:%02d.%03d", ms / 60000, (ms / 1000) % 60, ms % 1000);
    if (w::ReadText(value) == sign && w::ReadText(text) == shown) return;
    w::SetText(value, sign);
    w::SetText(text, shown);
    if (bg)
        if (Obj material = cosmetics::LoadAsset(delta >= 0 ? L"/Game/UI/UIMaterialLab/Widgets/ComponentMaterials/MI_UI_RedSplit.MI_UI_RedSplit"
                                                           : L"/Game/UI/UIMaterialLab/Widgets/ComponentMaterials/MI_UI_GreenSplit.MI_UI_GreenSplit"))
            eng::Call(bg, "SetBrushFromMaterial", material);
    hostlog::Info("splits: finish shown against the reference total: " + sign + shown);
}

}  // namespace

void Frame() {
    if (!Applies()) {
        gFinishedRun.clear();
        return;
    }
    Obj widget = SplitWidget();
    DoubleArray* a = widget ? PBSplits(widget) : nullptr;
    if (!a) return;
    if (widget != eng::Get(gOriginalOf)) {          // a new widget (a new map): what the game put there, kept
        gOriginalOf = eng::MakeWeak(widget);
        gOriginal = Read(a);
    }
    const std::vector<double> have = Read(a);
    if (have != gSplits) {
        if (Write(widget, a, gSplits)) {
            gOverridden = true;
            const std::string line = "splits: reference for " + gKey + " put in (" + std::to_string(gSplits.size()) + " splits" +
                                     (gSplits.empty() ? ", no deltas" : ": " + Join(gSplits)) + "; the game had " +
                                     std::to_string(have.size()) + ")";
            if (line != gLastLog) hostlog::Info(line);
            gLastLog = line;
        }
    }
    Finish(widget);
}

void Set(int owner, const std::string& trackKey, const std::vector<double>& splits, double total) {
    gOwner = owner;
    gKey = trackKey;
    gSplits.clear();
    for (double s : splits)
        if (std::isfinite(s) && s > 0 && s < 1e6) gSplits.push_back(s);
    gTotal = std::isfinite(total) && total > 0 ? total : 0;
}

void Clear(int owner) {
    if (owner != gOwner) return;
    GiveBack();
    gOwner = -1;
    gKey.clear();
    gSplits.clear();
    gTotal = 0;
}

std::string Status() {
    Obj widget = eng::Get(gWidget);
    DoubleArray* a = widget ? PBSplits(widget) : nullptr;
    return std::string("splits: applies ") + (Applies() ? "1" : "0") + " key '" + gKey + "' reference [" + Join(gSplits) +
           "] total " + std::to_string(gTotal) + " | widget PBSplits [" + (a ? Join(Read(a)) : std::string("no widget")) + "]" +
           (widget ? " shows '" + w::ReadText(eng::ReadObj(widget, "SplitValue")) + w::ReadText(eng::ReadObj(widget, "SplitDelta")) +
                         "' visibility " + std::to_string(eng::Call(widget, "GetVisibility").ReturnAs<uint8_t>(9))
                   : std::string());
}

}  // namespace splitref
