#include "hoverfx.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>

#include "widgets.hpp"

namespace w = ui::widgets;

namespace hoverfx {
namespace {

const ui::Color kLime = ux::Hex("#BEFF00"), kWhite = ux::Hex("#FFFFFF");
const ui::Color kPale = ux::Hex("#C4FF1C");        // the sparks: a strong lime (drawn faint, it loses its green)
constexpr int kSparks = 13, kSmokeFrames = 32;
constexpr double kSmokeLoop = 7.3;
std::mt19937 gRandom{20261007};

void Build(Fx& fx, Obj tree, Obj canvas, Obj canvasOver, int zUnder, int zOver) {
    fx.sparks.clear();
    fx.puffs.clear();
    for (ux::Node* n : {&fx.glow, &fx.smokeA, &fx.smokeB, &fx.thread, &fx.wash}) *n = ux::MakePicture(tree, canvas, 0, zUnder);
    fx.glow.Picture(ux::Picture("fx-glow.png"));
    fx.thread.Picture(ux::Picture("fx-thread.png"));
    fx.wash.Picture(ux::Picture("fx-wash.png"));
    for (int i = 0; i < kSparks; ++i) {
        fx.puffs.push_back(ux::MakePicture(tree, canvasOver, 0, zOver));
        fx.puffs.back().Picture(ux::Picture("fx-puff.png"));
        fx.sparks.push_back(ux::MakePicture(tree, canvasOver, 0, zOver + 1));
        fx.sparks.back().Picture(ux::Picture("fx-spark.png"));
    }
    fx.canvas = eng::MakeWeak(canvas);
    fx.canvasOver = eng::MakeWeak(canvasOver);
    fx.flipped = false;
}

void HideOne(Fx& fx) {
    for (ux::Node* n : {&fx.glow, &fx.smokeA, &fx.smokeB, &fx.thread, &fx.wash}) n->Opacity(0);
    for (auto& n : fx.sparks) n.Opacity(0);
    for (auto& n : fx.puffs) n.Opacity(0);
}

void Start(Fx& fx, const std::string& key, double now) {
    fx.key = key;
    fx.at = now;
    fx.endAt = 1e9;
    fx.plan.clear();
    auto u = [](double a, double b) { return std::uniform_real_distribution<double>(a, b)(gRandom); };
    for (int i = 0; i < kSparks; ++i) {
        Spark s{};
        s.edge = u(0, 1) < 0.6;
        s.t0 = i * 0.045 + u(0, 0.03);
        s.life = u(0.9, 1.25);
        s.startIn = u(0, 18);
        s.side = u(0, 1) < 0.5 ? -1 : 1;
        s.jitter = s.edge ? u(-2, 2) : u(-6, 6);
        s.along = u(30, 95);
        s.dist = u(55, 100);
        s.amp = u(3, 7) * (u(0, 1) < 0.5 ? -1 : 1);
        s.f = u(0.9, 1.6);
        s.ph = u(0, 6.28);
        s.drift = u(0.1, 0.4);
        s.r = u(0.8, 1.25);
        s.shown = 0.3 + 0.7 * std::pow(u(0, 1), 4);
        s.whiteFor = u(14, 34);
        fx.plan.push_back(s);
    }
}

// As customize.cpp's UpdateFx, with the direction free: dir -1 runs left from the anchor (the pictures as drawn), +1
// right (they are drawn mirrored).
void Update(Fx& fx, double anchor, double end, double cy, double top, double height, double hover, double now) {
    const double dir = end < anchor ? -1 : 1;
    const bool flip = dir > 0;
    if (flip != fx.flipped) {
        for (ux::Node* n : {&fx.glow, &fx.smokeA, &fx.smokeB, &fx.thread, &fx.wash})
            if (Obj o = n->Get()) eng::Call(o, "SetRenderScale", w::Vec2{flip ? -1.0 : 1.0, 1.0});
        fx.flipped = flip;
    }
    const double since = now - fx.at;
    const double fade = 1 - ux::Smooth((now - fx.endAt) / 0.18);
    const double shimmer = since > 0 ? fade * ux::Smooth(since / 0.35) * std::exp(-std::max(0.0, since - 0.35) / 0.9) : 0;
    const double snipe = since > 0 ? fade * ux::Smooth(since / 0.04) * std::exp(-std::max(0.0, since - 0.04) / 0.14) : 0;
    const double len = std::fabs(end - anchor), lo = std::min(anchor, end), hi = std::max(anchor, end);
    const double ga = fx.glowSet ? fx.glowFrom : anchor;          // the glow's and smoke's own start
    const double rx = std::fabs(end - ga) + 76, ry = 0.9 * height * (1 + 0.06 * std::sin(now * 2.1));
    fx.glow.Box(dir < 0 ? ga - rx : ga, cy - ry, rx, 2 * ry);
    fx.glow.Tint(kLime);
    fx.glow.Opacity(static_cast<float>(0.22 * shimmer));
    const double p = std::fmod(now / kSmokeLoop, 1.0) * kSmokeFrames;
    const int f0 = static_cast<int>(p) % kSmokeFrames;
    const double frac = p - std::floor(p);
    char name[32];
    std::snprintf(name, sizeof name, "fx-smoke%02d.png", f0);
    fx.smokeA.Picture(ux::Picture(name));
    std::snprintf(name, sizeof name, "fx-smoke%02d.png", (f0 + 1) % kSmokeFrames);
    fx.smokeB.Picture(ux::Picture(name));
    for (ux::Node* n : {&fx.smokeA, &fx.smokeB}) {
        n->Box(dir < 0 ? ga - rx * 1.19 : ga, cy - ry * 1.48, rx * 1.19, ry * 2.96);
        n->Tint(kLime);
    }
    fx.smokeA.Opacity(static_cast<float>(0.34 * hover * fade * (1 - frac)));
    fx.smokeB.Opacity(static_cast<float>(0.34 * hover * fade * frac));
    fx.thread.Box(dir < 0 ? end - 113 : anchor, cy - 12, len + 113, 24);
    fx.thread.Tint(kWhite);
    fx.thread.Opacity(static_cast<float>(0.12 * snipe));
    const double run = since / 0.22;
    const double washX = anchor + dir * run * (len + 113) * 1.15;
    fx.wash.Box(washX - 75, cy - 20, 150, 40);
    fx.wash.Tint(kWhite);
    fx.wash.Opacity(run > 0 && run < 1 ? static_cast<float>(0.15 * fade * std::sin(3.14159265 * run)) : 0.0f);
    for (size_t i = 0; i < fx.plan.size() && i < fx.sparks.size(); ++i) {
        const Spark& s = fx.plan[i];
        const double moved = (since - s.t0) / s.life, age = moved / s.shown;
        if (age <= 0 || age >= 1) {
            fx.sparks[i].Opacity(0);
            fx.puffs[i].Opacity(0);
            continue;
        }
        const double sx = s.edge ? anchor + dir * (2 + s.startIn) : end - dir * 4;
        const double sy = s.edge ? (s.side < 0 ? top - 2 : top + height + 2) + s.jitter : cy + s.jitter;
        const double dist = s.edge ? len + s.along : s.dist;
        const double go = (1 - std::exp(-3.2 * moved)) / (1 - std::exp(-3.2));
        const double px = sx + dir * dist * go;
        const double py = sy + s.amp * std::min(1.0, go * 4) * std::sin(s.ph + go * 6.28318 * s.f) + (cy - sy) * s.drift * go;
        const double shape = age < 0.28 ? ux::Smooth(age / 0.15)
                             : age < 0.75 ? 1 - 0.88 * ux::Smooth((age - 0.28) / 0.47)
                                          : 0.12 * (1 - ux::Smooth((age - 0.75) / 0.25));
        const double size = 32 * s.r * (1 - 0.5 * age);
        fx.sparks[i].Box(px + fx.overDx - size / 2, py + fx.overDy - size / 2, size, size);
        const double dx = std::max({lo - px, 0.0, px - hi}), dy = std::max({top - py, 0.0, py - (top + height)});
        fx.sparks[i].Tint(ux::Mix(kWhite, kPale, ux::Smooth(0.65 * std::hypot(dx, dy) / s.whiteFor + 0.35 * age)));
        fx.sparks[i].Opacity(static_cast<float>(fade * shape * 0.9));
        const double puff = fade * ux::Smooth((age - 0.25) / 0.45) * std::pow(1 - age, 1.1) * 0.6;
        const double puffSize = 2 * ((1.5 + 5.5 * age) * 1.6 + 4);
        fx.puffs[i].Box(px + fx.overDx - puffSize / 2, py + fx.overDy - puffSize / 2, puffSize, puffSize);
        fx.puffs[i].Tint(kLime);
        fx.puffs[i].Opacity(static_cast<float>(puff * 0.45));
    }
}

}  // namespace

void Pair::Begin() {
    for (Fx& f : fx) f.drawn = false;
}

void Pair::Item(Obj tree, Obj canvas, int zUnder, int zOver, const std::string& key, bool hovered, double anchor, double end, double cy, double top,
                double height, double hover, double now) {
    Item(tree, canvas, canvas, 0, 0, zUnder, zOver, key, hovered, anchor, end, cy, top, height, hover, now);
}

void Pair::Item(Obj tree, Obj canvas, Obj canvasOver, double dx, double dy, int zUnder, int zOver, const std::string& key, bool hovered,
                double anchor, double end, double cy, double top, double height, double hover, double now) {
    if (hovered && current != key) {
        Fx& f = fx[next];
        next = 1 - next;
        fx[next].endAt = std::min(fx[next].endAt, now);         // the last item's goes, quickly
        if (eng::Get(f.canvas) != canvas || eng::Get(f.canvasOver) != canvasOver || !f.glow.Get()) Build(f, tree, canvas, canvasOver, zUnder, zOver);
        Start(f, key, now);
        current = key;
    } else if (!hovered && current == key) {
        current.clear();
    }
    for (Fx& f : fx) {
        if (f.key != key || !f.glow.Get() || eng::Get(f.canvas) != canvas) continue;
        f.overDx = dx;
        f.overDy = dy;
        f.glowFrom = glowFrom;
        f.glowSet = glowSet;
        Update(f, anchor, end, cy, top, height, hover, now);
        f.drawn = true;
    }
}

void Pair::Finish() {
    glowSet = false;
    for (Fx& f : fx)
        if (!f.drawn && f.glow.Get()) HideOne(f);
}

void Pair::Hide() {
    for (Fx& f : fx)
        if (f.glow.Get()) HideOne(f);
    current.clear();
}

}  // namespace hoverfx
