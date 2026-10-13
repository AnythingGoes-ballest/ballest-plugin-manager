// The Customize page's slot hover effect (customize.cpp: a lime glow, smoke, a quick white thread with a wash along it,
// sparks white turning lime ending in puffs), for other screens' side buttons: the plugin manager's settings nav and the
// cosmetic browser's nav. It comes out of an item's `anchor` edge and runs past its far `end`; either way round (the
// Customize page's goes left from its slots, these go right). The same pictures (fx-*.png) and timings; the Customize
// page keeps its own copy (its look is locked). Game thread only.
#pragma once
#include <string>
#include <vector>

#include "ux.hpp"

namespace hoverfx {

using eng::Obj;

struct Spark {
    double t0, life, along, startIn, side, jitter, dist, amp, f, ph, drift, r, shown, whiteFor;
    bool edge;
};

struct Fx {
    std::string key;                        // the item it plays for, or ""
    eng::Weak canvas, canvasOver;          // the glow, smoke, thread and wash; the sparks and puffs (may be another)
    double overDx = 0, overDy = 0;          // where the sparks' canvas has the first one's origin
    double glowFrom = 0;                    // where the glow and smoke start (the item's far edge), when set
    bool glowSet = false;
    ux::Node glow, smokeA, smokeB, thread, wash;
    std::vector<ux::Node> sparks, puffs;
    std::vector<Spark> plan;
    double at = -100, endAt = 1e9;
    bool drawn = false, flipped = false;
};

// Two effects, so the last item's can finish as the next one starts.
struct Pair {
    Fx fx[2];
    int next = 0;
    std::string current;
    // Each frame: Begin, then Item for every item (its effect drawn if it has one), then Finish (hides the rest).
    void Begin();
    // An item this frame: `hovered` starts its effect the frame it is first hovered. The item's box: anchor and end are
    // its two ends along x (the effect leaves from anchor past end), cy, top and height across.
    void Item(Obj tree, Obj canvas, int zUnder, int zOver, const std::string& key, bool hovered, double anchor, double end, double cy, double top,
              double height, double hover, double now);
    // The same, the sparks and puffs on a canvas of their own (unclipped, over everything: so they can fly out of a
    // window), whose origin is (dx, dy) from the first's.
    void Item(Obj tree, Obj canvas, Obj canvasOver, double dx, double dy, int zUnder, int zOver, const std::string& key, bool hovered, double anchor,
              double end, double cy, double top, double height, double hover, double now);
    void Finish();
    // The glow and smoke from another edge than the sparks (for the next Item call: the item's right end, say, while its
    // sparks start further in).
    double glowFrom = 0;
    bool glowSet = false;
    void GlowFrom(double x) {
        glowFrom = x;
        glowSet = true;
    }
    void Hide();
};

// How long an effect plays, about (its sparks and shimmer): for a hover's white to fade back over the same time.
constexpr double kDuration = 0.9;

}  // namespace hoverfx
