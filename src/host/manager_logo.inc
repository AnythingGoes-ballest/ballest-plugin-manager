// The plugin manager logo, animated, and the welcome (part of manager.cpp, inside its namespace).
//
// The animation (Lvcid, 2026-10-08; reference: Handoff/sources/pm/logo/logo_intro.py and its video
// Handoff/images/logo/logo_animated.mp4) is drawn from the logo's layers (pm-logo-bowl, -glow, -greys, -dot, pm-glint,
// pm-twinkle: logo_layers.py) and moved with the same curves (logo_curves.inc): the lime light turning on, the glint
// once round the ring (lime in the glow, a sheen at the bottom, dim up the left), the twinkle where it ends, and the
// bowl turning its light round to the top left, settling. In the game: the corner's replay while an update banner
// shows (the light already on), and the welcome's: that replay with the light turning on as it starts, big in the
// screen's middle, then flying to its corner. (Intro, the whole animation from unlit, isn't used now: Lvcid.)

#include "logo_curves.inc"

namespace logo {

constexpr double kPi = 3.14159265358979;
constexpr double kGlowAt = -46 * kPi / 180;               // the glow's middle on the ring (the lime peak)
constexpr double kReplayTwinkle = kLogoTwinkleAt - kLogoGlintAt;     // (in the replay: from the glint's start)
constexpr double kReplayLen = kReplayTwinkle + kLogoTwinkleLen + 0.1;

double Clamp01(double v) { return std::clamp(v, 0.0, 1.0); }

// A table of logo_curves.inc at u (0..1 of its own time).
double Curve(const double* table, double u) {
    const double at = Clamp01(u) * (kLogoSteps - 1);
    const int i = std::min(static_cast<int>(at), kLogoSteps - 2);
    return table[i] + (table[i + 1] - table[i]) * (at - i);
}

// A colour as the animation names it (sRGB, 0..255) as a tint.
Color Srgb(double r, double g, double b, double a = 1) {
    auto lin = [](double c) { return static_cast<float>(std::pow(std::clamp(c, 0.0, 255.0) / 255.0, 2.2)); };
    return {lin(r), lin(g), lin(b), static_cast<float>(Clamp01(a))};
}

struct Look {
    double lit = 1, glow = 1;              // the lime square's light (a little over 1 as it arrives), its glow
    double turn = 0;                       // the bowl, turned from where it rests (degrees, clockwise)
    double glint = -1, twinkle = -1;       // their own time 0..1 (-1: not now)
    double refresh = 0, spin = 0;          // the refresh button (hovered): the lime square as a refresh symbol, turning
    double flash = 0;                      // its symbol white (clicked: white, fading back to lime)
};

// The whole animation's look `t` seconds from the light's beat (the video's time; before it, unlit).
[[maybe_unused]] Look Intro(double t) {
    Look k;
    // the light: its own lime, brighter in light only, a quick rise easing out, a small lift as it arrives
    const double u = Clamp01((t - kLogoLightAt) / kLogoLightLen);
    const double on = u < 1 ? 1 - std::pow(2.0, -9 * u) : 1.0;
    k.lit = on + 0.10 * std::pow(std::sin(kPi * Clamp01((u - 0.12) / 0.6)), 2);
    const double g = Clamp01((t - kLogoLightAt - 0.07) / kLogoLightLen);     // its glow, just behind it
    k.glow = 1 - std::pow(1 - g, 3);
    k.turn = -kLogoTurnSweep * (1 - Curve(kLogoTurn, (t - kLogoTurnAt) / kLogoTurnLen));
    if (const double gu = (t - kLogoGlintAt) / kLogoGlintLen; gu > 0 && gu < 1) k.glint = gu;
    if (const double p = (t - kLogoTwinkleAt) / kLogoTwinkleLen; p > 0 && p < 1) k.twinkle = p;
    return k;
}

// The corner's replay `t` seconds in (an update banner showing): the glint and the twinkle, the bowl swaying a little
// and back; the light stays on.
Look Replay(double t) {
    Look k;
    if (const double gu = t / kLogoGlintLen; gu > 0 && gu < 1) k.glint = gu;
    if (const double p = (t - kReplayTwinkle) / kLogoTwinkleLen; p > 0 && p < 1) k.twinkle = p;
    const double s = Clamp01(t / kReplayLen);
    k.turn = 22 * std::sin(kPi * s * s * (3 - 2 * s));
    return k;
}

// The corner's own quiet animation (Lvcid, 2026-10-11): just the twinkle (no glint, no sway), every 9 s while no update
// is offered (Frame).
constexpr double kNudgeLen = kLogoTwinkleLen + 0.1;
Look Nudge(double t) {
    Look k;
    if (const double p = t / kLogoTwinkleLen; p > 0 && p < 1) k.twinkle = p;
    return k;
}

// The welcome's (Lvcid): the corner's replay, the lime light turning on as it starts (before then, unlit).
Look Welcome(double t) {
    Look k = Replay(t);
    const double u = Clamp01(t / kLogoLightLen);
    const double on = u < 1 ? 1 - std::pow(2.0, -9 * u) : 1.0;
    k.lit = on + 0.10 * std::pow(std::sin(kPi * Clamp01((u - 0.12) / 0.6)), 2);
    const double g = Clamp01((t - 0.07) / kLogoLightLen);       // its glow, just behind it
    k.glow = 1 - std::pow(1 - g, 3);
    return k;
}

// The glint's own colours along its path, peak to peak (Lvcid): a cool sheen from north, a sheen of the ring's own tone
// at the bottom (77 deg), dim up the left (211..267), never gone; the glow's lime over it (GlowLime).
struct Key {
    double deg, r, g, b, amount;
};
constexpr Key kKeys[] = {{-90, 168, 176, 190, 0.45}, {77, 78, 80, 96, 0.75}, {211, 60, 62, 78, 0.75}, {267, 60, 62, 78, 0.75}};
void KeyAt(double deg, double rgb[3], double* amount) {
    const Key* a = &kKeys[0];
    const Key* b = &kKeys[0];
    if (deg >= kKeys[std::size(kKeys) - 1].deg) a = b = &kKeys[std::size(kKeys) - 1];
    else
        for (size_t i = 0; i + 1 < std::size(kKeys); ++i)
            if (deg >= kKeys[i].deg && deg <= kKeys[i + 1].deg) a = &kKeys[i], b = &kKeys[i + 1];
    const double t = b->deg > a->deg ? Clamp01((deg - a->deg) / (b->deg - a->deg)) : 0;
    rgb[0] = a->r + (b->r - a->r) * t;
    rgb[1] = a->g + (b->g - a->g) * t;
    rgb[2] = a->b + (b->b - a->b) * t;
    *amount = a->amount + (b->amount - a->amount) * t;
}

// How much the glow lights the glint at `a` (radians, either pass): strongest at its middle, carried on a little after.
double GlowLime(double a) {
    const double d = std::remainder(a - kGlowAt, 2 * kPi);
    const double width = (d < 0 ? 30 : 46) * kPi / 180;
    return std::exp(-(d / width) * (d / width));
}

// Its thickness by place (Lvcid): the ring's width at the lime peak, thinnest at the bottom, thin through the dark,
// thicker again only from the dark's edge into the lime.
double Thickness(double deg) {
    constexpr double kThin = 0.30;
    if (deg < 90) return 1 - (1 - kThin) * Smooth(Clamp01((deg + 46) / 136));
    if (deg < 250) return kThin;
    return kThin + (1 - kThin) * Smooth(Clamp01((deg - 250) / 64));
}

// The glint at its time u (0..1), in the logo r: a chain of soft dots along the ring's inner edge, each in the colour of
// its own place, a crisp head and a trail that lengthens a little with speed and tapers.
void Glint(Layer& l, const std::string& key, int z, const Rect& r, double u, double glowNow, float op) {
    const double D = r.w, ringW = 3 * D / 86, outer = D / 2;
    const double head = Curve(kLogoGlintHead, u), speed = Curve(kLogoGlintSpeed, u);
    const double fadeLen = kLogoGlintFade / kLogoGlintLen;
    const double fadeOut = 1 - Smooth(Clamp01((u - (1 - fadeLen)) / fadeLen));      // onto the lime peak: the twinkle's
    const double tail = 0.16 + 0.42 * speed;
    constexpr int kDots = 80;
    const double ahead = 0.12, step = (ahead + 2.4 * tail) / kDots;
    for (int j = 0; j <= kDots; ++j) {
        const double d = ahead - j * step, along = head + d;
        const double mask = d > 0 ? std::exp(-(d / 0.045) * (d / 0.045)) : std::exp(-std::pow(-d / tail, 1.5));
        const double fade = Smooth(Clamp01((along - kLogoGlintStart) / (kGlowAt - kLogoGlintStart))) *
                            (along <= kLogoGlintEnd + 0.02 ? 1.0 : 0.0) * fadeOut;
        if (fade * mask < 0.004) continue;
        const double deg = along * 180 / kPi;
        const double hw = std::max(0.0015 * D, ringW / 2 * Thickness(deg) * (0.45 + 0.55 * mask));
        const double rc = outer - ringW + hw;               // riding the ring's inner edge
        double rgb[3], amount;
        KeyAt(deg, rgb, &amount);
        const double w = GlowLime(along) * glowNow;          // lit by the glow as it passes
        const double lime[3] = {176, 250, 30};
        for (int c = 0; c < 3; ++c) rgb[c] += (lime[c] - rgb[c]) * w;
        amount += (1 - amount) * w;
        const double a = std::min(0.999, fade * mask * amount);
        // the dots overlap: each takes its share, so together they make `a` as one stroke would
        const double overlap = std::max(1.0, 2 * hw / std::max(1e-6, rc * step));
        const double each = 1 - std::pow(1 - a, 1 / overlap);
        const double size = 2 * hw / 0.64;                    // (the picture: solid to 0.64 of its radius)
        const double cx = r.Cx() + rc * std::cos(along), cy = r.Cy() + rc * std::sin(along);
        pmui::Pic(l, key + ".g" + std::to_string(j), z, {cx - size / 2, cy - size / 2, size, size}, "pm-glint.png",
                  Srgb(rgb[0], rgb[1], rgb[2], each))
            .Opacity(op);
    }
}

// The twinkle at its time p (0..1), on the ring at the lime peak: from nothing to nothing, growing and shrinking,
// turning; lime, quickly white, back to lime; in sharply, out smoothly.
void Twinkle(Layer& l, const std::string& key, int z, const Rect& r, double p, float op) {
    const double D = r.w, ringR = D / 2 - 1.5 * D / 86;
    const double cx = r.Cx() + ringR * std::cos(kGlowAt), cy = r.Cy() + ringR * std::sin(kGlowAt);
    const double size = std::pow(std::sin(kPi * std::pow(p, 0.75)), 1.2);
    const double opacity = Smooth(Clamp01(p / 0.1)) * (1 - Smooth(Clamp01((p - 0.3) / 0.7)));
    const double white = std::exp(-((p - 0.32) / 0.16) * ((p - 0.32) / 0.16));
    const double side = 0.72 * D * size;
    if (side < 0.5) return;
    ux::Node& n = pmui::Pic(l, key + ".tw", z, {cx - side / 2, cy - side / 2, side, side}, "pm-twinkle.png",
                            Srgb(176 + 79 * white, 250 + 5 * white, 30 + 220 * white, opacity));
    n.Transform(1, 10 + 50 * (1 - (1 - p) * (1 - p)));
    n.Opacity(op);
}

// The logo in r (square; 86 across at its usual size) as `k` shows it.
void Draw(Layer& l, const std::string& key, int z, const Rect& r, const Look& k, double opacity) {
    const float op = static_cast<float>(opacity);
    ux::Node& bowl = pmui::Pic(l, key + ".bowl", z, r, r.w > 130 ? "pm-logo-bowl.png" : "pm-logo-bowl-s.png", kWhite);
    bowl.Transform(1, k.turn);                               // (its light swings round with it)
    bowl.Opacity(op);
    const double rf = Smooth(Clamp01(k.refresh));
    if (k.glow * (1 - rf) > 0.002) pmui::Pic(l, key + ".glow", z + 1, r, "pm-logo-glow.png", A(kWhite, k.glow * (1 - rf))).Opacity(op);
    const double m = 36 * r.w / 86;
    const Rect mark{r.Cx() - m / 2, r.Cy() - m / 2, m, m};
    pmui::Pic(l, key + ".greys", z + 2, mark, "pm-logo-greys.png", kWhite).Opacity(static_cast<float>(opacity * (1 - 0.75 * rf)));
    // the lime square in linear light: unlit, its lime at 12% (an LED that's off); lit, full (the lift a touch over)
    const double level = 0.12 + 0.88 * k.lit;
    const Color c = Srgb(150, 226, 0);
    const Color lime{std::min(1.0f, c.r * static_cast<float>(level)), std::min(1.0f, c.g * static_cast<float>(level)),
                     std::min(1.0f, c.b * static_cast<float>(level)), 1};
    pmui::Pic(l, key + ".lime", z + 2, {mark.x + m * 576 / 1024, mark.y, m * 448 / 1024, m * 448 / 1024}, "pm-logo-dot.png", lime)
        .Opacity(static_cast<float>(opacity * (1 - rf)));
    if (rf > 0.002) {
        // the refresh symbol: from the lime square to the middle, growing; its glow hugs its shape (as the ring glow
        // does the ring), turning with it (Lvcid)
        const double lx = mark.x + m * 800 / 1024, ly = mark.y + m * 224 / 1024;
        const double cx = lx + (r.Cx() - lx) * rf, cy = ly + (r.Cy() - ly) * rf, s = m * (0.44 + 0.56 * rf);
        ux::Node& halo = pmui::Pic(l, key + ".rglow", z + 3, {cx - s, cy - s, 2 * s, 2 * s}, "pm-refresh-glow.png", A(Mix(Srgb(176, 250, 30), kWhite, k.flash), 0.7 * rf));
        halo.Transform(1, k.spin);
        halo.Opacity(op);
        ux::Node& sym = pmui::Pic(l, key + ".r", z + 4, {cx - s / 2, cy - s / 2, s, s}, "pm-refresh.png", A(Mix(kLucidNuLime, kWhite, k.flash), rf));      // (the symbol nulime, #96E200 as the logo's square, any theme; its glow the lime: Lvcid)
        sym.Transform(1, k.spin);
        sym.Opacity(op);
    }
    if (k.glint >= 0) Glint(l, key, z + 3, r, k.glint, k.glow, op);
    if (k.twinkle >= 0) Twinkle(l, key, z + 4, r, k.twinkle, op);
}

}  // namespace logo

// --- the welcome ------------------------------------------------------------------------------------------------------
// The first open since an install or update (Lvcid): the screen dims, the logo fades in big in the screen's middle (no
// window at all, not even its edge) and plays its animation; after its twinkle it's gone in a sharp fade, and the window
// comes in, sharply too, on the splash (the corner's logo with it): what's new in this version, "read more" (its release
// page), "take the tour" (to come) and "continue". The intro plays through (it's short: Lvcid): no key, click or
// closing stops it. On the splash, Esc or B is "continue" (to the window); A, Start, Enter or Space press the picked
// button (continue unless moved); a click outside the window closes it. "what's new" (the sub bar) shows it again.
enum class Welcome { None, Intro, Splash };
Welcome gWelcome = Welcome::None;
double gWelcomeAt = -100;                   // the intro's start (it fades in, then the animation's own time runs)
double gSplashAt = -100;                    // the splash's start (its pieces come in one by one)
bool gSplashPending = false;
int gSplashPick = 2;                        // picked by keys or a pad: 0 read more, 2 continue (1, the tour, to come)
bool gSplashKeys = false;                   // a key or pad moved or pressed the pick (its ring shows only then)
// The big logo (Lvcid): a moment of only the dim, then a quick fade in, unlit; as it reaches full opacity its lime
// light starts turning on and the animation (logo::Welcome: the corner's replay) begins.
constexpr double kIntroHold = 0.25;         // nothing but the dim
constexpr double kIntroFade = 0.12;         // the big logo fading in (sharp: eased out)
constexpr double kIntroLead = kIntroHold + kIntroFade;
constexpr double kBigLogo = 260;            // across, in the screen's middle (the corner's is 86)
// After its twinkle (Lvcid): the big logo goes in a sharp fade, a beat, then the window and the splash come in sharply.
// Sharp fades in (kSharpIn) are the window's way for anything coming in: the splash, popups.
constexpr double kOutAt = logo::kReplayTwinkle + kLogoTwinkleLen, kOutLen = 0.1;
constexpr double kSplashAt = kOutAt + kOutLen + 0.05;
constexpr double kSharpIn = 0.12;
constexpr double kWelcomeLen = kSplashAt;                                      // the logo's part, all of it
// The corner's replay (logo::Replay) while an update banner shows (otherwise logo::Nudge, the twinkle alone, every 9 s): every 10 s (the first 2 s after it shows); when the banner goes
// (later, update now) the one playing finishes and no more start; closing the window cuts it off where it is.
double gReplayAt = -100, gReplayNext = -1;

// The animation's own time in the intro (before 0: still fading in).
double IntroTime() { return pmui::Now() - gWelcomeAt - kIntroLead; }
double IntroOpacity() {
    const double t = IntroTime();
    if (t >= kOutAt) return 1 - logo::Clamp01((t - kOutAt) / kOutLen);       // (going: sharp, straight down)
    const double u = logo::Clamp01((pmui::Now() - gWelcomeAt - kIntroHold) / kIntroFade);
    return 1 - (1 - u) * (1 - u);
}
bool Animating() { return gWelcomeAt > -50 && IntroTime() < kWelcomeLen; }
// (started from Open, before this frame's drawing: the game's clock, as pmui::Now() is only brought up to date while
// the window is drawn, so on the session's first open it is still 0 and the intro would look long over)
void StartWelcome() {
    gWelcome = Welcome::Intro;
    gWelcomeAt = game::Seconds();
    gSplashPick = 2;
    gSplashKeys = false;
}
void ShowSplash() {
    gWelcome = Welcome::Splash;
    gSplashAt = game::Seconds();
    gSplashPick = 2;
    gSplashKeys = false;
}
void SkipIntro() {
    if (gWelcome != Welcome::Intro && !(gWelcome == Welcome::Splash && Animating())) return;
    hostlog::Info("manager: welcome skipped to the splash");
    if (gWelcome == Welcome::Intro) ShowSplash();
    gWelcomeAt = pmui::Now() - kIntroLead - kWelcomeLen;     // the logo in its corner, still
}
void EndWelcome() {
    gWelcome = Welcome::None;
    hostlog::Info("manager: welcome closed");
}

// This version's highlights (whats_new.inc).
std::vector<std::string> Highlights() {
    static const std::string all =
#include "whats_new.inc"
        ;
    std::vector<std::string> out;
    const std::string head = std::string("[") + plugins::kHostVersion + "]";
    bool in = false;
    size_t at = 0;
    while (at <= all.size()) {
        size_t end = all.find('\n', at);
        if (end == std::string::npos) end = all.size();
        std::string line = all.substr(at, end - at);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty() && line[0] == '[') in = line == head;
        else if (in && !line.empty()) out.push_back(line);
        at = end + 1;
    }
    return out;
}

bool SkipKey() {
    return input::Pressed(VK_ESCAPE) || input::Pressed(VK_SPACE) || input::Pressed(VK_RETURN) || input::Pressed(input::kPadA) ||
           input::Pressed(input::kPadB) || input::Pressed(input::kPadMenu);
}

// The splash's keys: Esc or B continue; left and right (or Tab, a pad's d-pad or stick) move the pick; A, Start, Enter
// or Space press it.
void SplashKeys() {
    if (Animating() && SkipKey()) return SkipIntro();         // (still flying in: a key settles it first)
    if (input::Pressed(VK_ESCAPE) || input::Pressed(input::kPadB)) return EndWelcome();
    if (input::Pressed(VK_LEFT) || input::Pressed(VK_RIGHT) || input::Pressed(VK_TAB) || input::Pressed(input::kPadLeft) ||
        input::Pressed(input::kPadRight) || input::Pressed(input::kPadStickLeft) || input::Pressed(input::kPadStickRight)) {
        gSplashPick = gSplashPick == 0 ? 2 : 0;               // (the tour is to come: skipped)
        gSplashKeys = true;
    }
    if (input::Pressed(VK_RETURN) || input::Pressed(VK_SPACE) || input::Pressed(input::kPadA) || input::Pressed(input::kPadMenu)) {
        gSplashKeys = true;
        if (gSplashPick == 0) OpenUrl(ReleasePage(plugins::kHostVersion));
        else EndWelcome();
    }
}

// The splash, over the whole window (its pages aren't drawn meanwhile): what's new, in pieces coming in one by one.
void Splash(Layer& wl, double W, double H) {
    pmui::Level(kPopupLevel);
    const double since = pmui::Now() - gSplashAt;
    auto in = [&](int) { return EaseOut(logo::Clamp01(since / kSharpIn), 2); };     // (sharp, all at once: Lvcid)
    const double colW = std::min(760.0, W - 160), x = std::floor((W - colW) / 2);
    double y = std::max(96.0, std::floor(H * 0.14));
    int piece = 0;
    double t = in(piece++);
    {
        // (Ballest Light: a stronger shadow under the small lime words, to read on the light panel: Lvcid)
        std::optional<pmui::TextShadowScope> shade;
        if (BallestLight()) shade.emplace(Color{0, 0, 0, static_cast<float>(0.85 * t)});
        pmui::Text(wl, "wn.kicker", 10, "what's new", 17, "Bold", A(kLime, t), x, y + (1 - t) * 10);
    }
    y += 42;
    t = in(piece++);
    pmui::Text(wl, "wn.title", 10, std::string("Plugin Manager ") + plugins::kHostVersion, 38, "Bold", A(kText, t), x, y + (1 - t) * 10);
    y += 58;
    const double bottom = H - 96;                             // (the buttons' row)
    Layer& list = pmui::Group(wl, "wn.list", 10, {x - 4, y, colW + 8, std::max(0.0, bottom - 36 - y)}, true);
    std::vector<std::string> lines = Highlights();
    if (lines.empty()) lines.push_back("This version's changes are on its release page: read more.");
    static std::map<std::string, double> heights;            // (a paragraph's height comes two frames late: kept)
    double ly = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        t = in(piece++);
        const std::string key = "wn.l" + std::to_string(i);
        const double oy = ly + (1 - t) * 10;
        pmui::Box(list, key + ".dot", 0, {4, oy + 10, 8, 8}, A(kLime, t), 4);
        const double ph = pmui::Para(list, key, 0, lines[i], 19, "Regular", A(kDescOpen, t), {28, oy, colW - 24, 200});
        if (ph > 0) heights[key] = ph;
        ly += (heights.count(key) ? heights[key] : 28) + 18;
    }
    // the buttons: read more (left), take the tour (to come: blacked out) and continue (right)
    t = in(piece++);
    Layer& row = pmui::Group(wl, "wn.row", 11, {x - 8, bottom - 8 + (1 - t) * 10, colW + 16, 64}, false, t);
    const double by = 8;
    const double cw = std::max(200.0, PillWidth(row, "wn.go", "continue", 20, nullptr, 30));
    const Rect go{8 + colW - cw, by, cw, 48};
    const double rw = PillWidth(row, "wn.more", "read more", 18, nullptr, 24);
    const Rect more{8, by, rw, 48};
    const double tw = Measured(row, "wn.tour.t", "take the tour", 18) + 48;
    const Rect tour{go.x - 14 - tw, by, tw, 48};
    if (BallestLight()) {
        // (Ballest Light: its buttons on soft shadows, lifting them off the light panel: Lvcid)
        ux::RawColors raw;
        for (const auto& [k, b] : {std::pair<const char*, Rect>{"wn.more.sh", more}, {"wn.tour.sh", tour}, {"wn.go.sh", go}})
            pmui::Sliced(row, k, 0, b.Moved(0, 3).Grow(kGlowRoom), "pm-glow.png", kGlowMargin, kGlowUnits, A(kBlack, 0.28));
    } else if (CurrentTheme() == 0) {
        // (Ballest Night: a soft white glow round each, as the cards' and what's new's: Lvcid)
        for (const auto& [k, b] : {std::pair<const char*, Rect>{"wn.more.gl", more}, {"wn.tour.gl", tour}, {"wn.go.gl", go}})
            pmui::Sliced(row, k, 0, b.Grow(kGlowRoom), "pm-glow.png", kGlowMargin, kGlowUnits, A(kWhite, std::string(k) == "wn.tour.gl" ? 0.12 : 0.4));      // (0.16 hid behind the buttons: only the glow's far edge shows past them)
    }
    if (Pill(row, "wn.more", 1, more, "read more", 18, nullptr, PillKind::Ghost, false, 14)) OpenUrl(ReleasePage(plugins::kHostVersion));
    pmui::Box(row, "wn.tour", 1, tour, kOffFill, 14).Outline(2, A(kAbEdge, 0.5));
    pmui::Text(row, "wn.tour.t", 2, "take the tour", 18, "Regular", A(kSub, 0.3), tour.Cx(), tour.Cy(), 0.5);
    if (Pill(row, "wn.go", 1, go, "continue", 20, nullptr, PillKind::Lime, false, 14)) EndWelcome();
    // the pick, once keys or a pad are used: a lime ring round it
    if (gSplashKeys) {
        const Rect p = (gSplashPick == 0 ? more : go).Grow(5);
        pmui::Box(row, "wn.pick", 0, p, A(kLime, 0), 19).Outline(2, kLime);
    }
}
