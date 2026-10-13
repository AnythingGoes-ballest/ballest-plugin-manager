// --- the cosmetics page ----------------------------------------------------------------------------------------------------
// Included into manager.cpp's anonymous namespace (after the plugins page): it draws with the window's own pieces.
//
// The cosmetic browser, moved in (Lvcid, 2026-10-07): it's for finding cosmetics, not wearing them. Shared cosmetics are
// plugins (AnythingGoes, 2026-10-08): each its own repo, listed in the registry (registry::IsCosmetic) and installed like
// any plugin, into plugins\cosmetics\<id>. **A plugin is a pack**: getting or removing is the whole plugin, and what is
// in it is known once it runs (the cosmetics it adds through the Cosmetics API, noted by packages.cpp).
//   sidebar   EXPLORE: all (every pack as its card, newest first: AnythingGoes wants packs got whole, never one
//             cosmetic; Lvcid, 2026-10-10), packs (a dropdown: each pack, opening its page), each
//             slot shown; LIBRARY: downloaded, published (the player's own shared ones: none can be yet, so its empty
//             state).
//             Hovered: the plugin rows' lit edge, inside the tab's left (lime, smoke, sparks).
//   sub bar   the sidebar's fold button, a search, the sort, a "customize" key back to the Customize page (when it's
//             behind the window), the views (big grid, small grid by default, list).
//   tiles     the picture on its tile, name, by; NEW (its plugin joined the registry lately); a quiet check (it's yours).
//             A pack not got yet is a card of its own: its plugin's icon, name, by, and get (lime).
//   details   a drawer on the right: the picture, name, by, version, its pack's description, what it is, the facts,
//             the rest of its pack, remove (its pack).
//   popups    get and remove, of a whole pack, with its pictures and "show the pack".
// A pack's page: back, its pictures, name, by, and get the pack (installing its plugin, and Cosmetic Kit first when it
// needs it) or remove; a pack not got yet shows its description until its cosmetics are there.

void ClosePopup();
void OpenPack(const std::string& name);

struct Cos {
    std::string key, id, name, slot, author, version, pack, description, credits;
    bool published = false;                 // the player's own, shared (to come: none can be yet)
    bool fresh = false;
    long day = 0;                           // the day its pack joined the registry (days since 1970), for newest
    std::wstring preview;                   // its picture ("" for the game's picture of it, else its slot's)
    Color tint{1, 1, 1, 1};
};
std::vector<Cos> gCos;
double gCosAt = -100;

// A pack: a cosmetic plugin, from the registry and (once installed) what it added; its cosmetics newest first.
struct PackEntry {
    std::string name;                       // its plugin's id
    std::string title, author, version, description, icon, pending;     // pending: registry::Pending (installing, ...)
    std::vector<const Cos*> items;
    bool fresh = false, installed = false, essential = false;
    long day = 0;
};
std::vector<PackEntry> gPacks;

struct Glide {
    std::string from;                       // the category left, while the list comes in again
    double at = -100;
    Rect start, now;
};
struct CosState {
    std::string slot = "all", open, pack, search;
    std::string expanded;                   // featured / owned: the pack row open (its details shown in place)
    int sort = 0;
    bool searchFocused = false;
    bool side = true;                       // the categories' sidebar out (its button folds it away)
    bool packsOpen = false;                 // the sidebar's packs dropdown (closed at first; kept for the session)
    double searchReadAt = -100;
    Glide glide;
} gC;
Obj gCosField = nullptr;
const char* const kCosSorts[] = {"newest", "name", "author", "pack"};
constexpr int kCosSortCount = 4;
constexpr double kCosDrawer = 480;
constexpr double kCosSide = 270;            // the categories' sidebar (the page moves right for it)

std::string SlotPicture(const std::string& slot) {
    const bool known = slot == "ball" || slot == "hat" || slot == "bfx" || slot == "arms" || slot == "bounce";
    return known ? "slot-" + slot + ".png" : "slot-extra.png";
}

std::string SlotLabel(const std::string& slot) { return packages::LabelOf(slot); }

const PackEntry* FindPack(const std::string& name) {
    for (const PackEntry& p : gPacks)
        if (p.name == name) return &p;
    return nullptr;
}

// A pack's name as shown: its plugin's name, else its id's dashes and underscores as spaces.
std::string PackName(const std::string& pack) {
    if (const PackEntry* p = FindPack(pack); p && !p->title.empty()) return p->title;
    std::string out = pack;
    for (char& ch : out)
        if (ch == '-' || ch == '_') ch = ' ';
    return out;
}

const registry::Entry* EntryOf(const std::string& id) {
    for (const registry::Entry& e : registry::Entries())
        if (e.id == id) return &e;
    return nullptr;
}

// The packs: every cosmetic plugin the registry lists (Cosmetic Kit Plus aside: it's only a dependency), and any other
// installed plugin that added cosmetics; each with its cosmetics.
void BuildPacks() {
    gPacks.clear();
    std::map<std::string, size_t> at;
    auto add = [&](const std::string& id) -> PackEntry& {
        if (!at.count(id)) {
            at[id] = gPacks.size();
            PackEntry p;
            p.name = id;
            plugins::Info info;
            p.installed = plugins::Find(id, &info);
            if (p.installed) {
                p.title = info.name, p.author = info.author, p.version = info.version, p.description = info.description;
                p.icon = info.icon, p.essential = info.essential;
            }
            if (const registry::Entry* e = EntryOf(id)) {
                if (!e->name.empty()) p.title = e->name;
                if (!e->author.empty()) p.author = e->author;
                if (!p.installed) p.version = e->version;
                if (!e->description.empty()) p.description = e->description;
                if (p.icon.empty()) p.icon = e->icon;
                p.day = e->added.empty() ? 0 : DayOf(e->added);
                p.fresh = Fresh(*e) || gTestNew;
            }
            p.pending = registry::Pending(id);
            gPacks.push_back(std::move(p));
        }
        return gPacks[at[id]];
    };
    for (const registry::Entry& e : registry::Entries())
        if (registry::IsCosmetic(e.id, e.dependencies) && !registry::DependencyOnly(e.id)) add(e.id);
    for (const Cos& c : gCos)
        if (!c.pack.empty()) add(c.pack).items.push_back(&c);
    for (PackEntry& p : gPacks)
        std::stable_sort(p.items.begin(), p.items.end(), [](const Cos* a, const Cos* b) { return a->day > b->day; });
    std::stable_sort(gPacks.begin(), gPacks.end(), [](const PackEntry& a, const PackEntry& b) { return Lower(PackName(a.name)) < Lower(PackName(b.name)); });
}

void BuildCos() {
    gCosAt = pmui::Now();
    gCos.clear();
    const std::vector<std::string> shown = packages::ShownSlots(true);
    auto slotShown = [&](const std::string& s) { return std::find(shown.begin(), shown.end(), s) != shown.end(); };
    for (const packages::Item& it : packages::Items()) {
        if (!slotShown(it.category)) continue;
        if (!plugins::Find(it.plugin, nullptr)) continue;       // its plugin was removed (its cosmetics go at the next start)
        Cos c;
        c.key = it.category + "/" + it.id;
        c.id = it.id, c.name = it.name.empty() ? it.id : it.name, c.slot = it.category, c.author = it.author;
        c.version = it.version, c.pack = it.pack.empty() ? it.plugin : it.pack, c.credits = it.credits;
        c.preview = it.preview;
        if (const registry::Entry* e = EntryOf(c.pack)) {
            c.day = e->added.empty() ? 0 : DayOf(e->added);
            c.fresh = Fresh(*e);
            if (c.author.empty()) c.author = e->author;
            c.description = e->description;
        }
        c.fresh = c.fresh || gTestNew;
        gCos.push_back(std::move(c));
    }
    BuildPacks();
}

const Cos* FindCos(const std::string& key) {
    for (const Cos& c : gCos)
        if (c.key == key) return &c;
    return nullptr;
}

std::vector<const Cos*> PackOf(const Cos& c) {
    if (const PackEntry* p = FindPack(c.pack)) return p->items;
    return {&c};
}

std::string ByLine(const Cos& c) {
    return c.author.empty() ? "from " + PackName(c.pack) : "by " + c.author;
}

bool Busy(const PackEntry& p) { return p.pending == "installing" || p.pending == "removing"; }
bool RemovablePack(const PackEntry& p) { return p.installed && !p.essential && !Busy(p); }
bool Removable(const Cos& c) {
    const PackEntry* p = FindPack(c.pack);
    return p && RemovablePack(*p);
}

// --- doing ---------------------------------------------------------------------------------------------------------------
// Getting a pack installs its plugin (registry::Install: its dependencies first, Cosmetic Kit among them).
void StartGet(const PackEntry& p) {
    if (p.installed || Busy(p)) return;
    registry::Install(p.name);
    hostlog::Info("manager: getting the " + p.name + " pack");
    gCosAt = -100;
    gModelAt = -100;
}

void AskGet(const PackEntry& p) {
    if (p.installed || Busy(p)) return;
    gS.popup = "cos.get";
    gS.popupId = p.name;
}

void AskRemove(const std::string& pack) {
    const PackEntry* p = FindPack(pack);
    if (!p || !RemovablePack(*p)) return;
    gS.popup = "cos.remove";
    gS.popupId = pack;
}

// --- pieces --------------------------------------------------------------------------------------------------------------
// A cosmetic's picture on its tile (the plugins' tile: dark, a brighter ring).
void CosPicture(Layer& l, const std::string& key, int z, const Rect& r, const Cos& c, double radius, double ring, double hover) {
    pmui::Box(l, key + ".b", z, r, kTile, radius).Outline(static_cast<float>(ring), Mix(ux::Hex("#383B41"), kWhite, 0.85 * hover));
    Obj texture = c.preview.empty() ? nullptr : ux::LoadPicture(c.preview);
    if (!texture) texture = cosmetics::PreviewOf(c.slot, c.id);
    const Rect in = r.Grow(-ring);
    if (texture) pmui::Texture(l, key + ".i", z + 1, in, texture, c.tint, std::max(0.0, radius - ring));
    else pmui::Pic(l, key + ".s", z + 1, in.Grow(-in.w * 0.14), SlotPicture(c.slot).c_str(), A(Mix(kWhite, c.tint, 0.5), 0.7));
}

// Get (lime: grows, turns white and shines when hovered), getting (turning arrows), got (a quiet lime check).
bool CosGetButton(Layer& l, const std::string& u, int z, const Rect& b, bool got, bool getting) {
    const double s = b.w;
    if (got || getting) {
        pmui::Box(l, u + "get.b", z, b, kButtonFill, s * 0.28).Outline(2, kAbEdge);
        pmui::Pic(l, u + "get.i", z + 1, b.Grow(-s * 0.26), getting ? "pm-refresh.png" : "check.png", getting ? kSub : A(kLime, 0.75))
            .Transform(1, getting ? std::fmod(pmui::Now() * 300, 360) : 0);
        return false;
    }
    const double h = EaseUpDown(u + "get", pmui::Over(l, b), 0.1, 0.18);
    const double k = 1 + 0.09 * Smooth(h), gs = s * k;
    const Rect g{b.Cx() - gs / 2, b.Cy() - gs / 2, gs, gs};
    Shine(l, u + "get.shine", z, g, "slot", Smooth((h - 0.65) / 0.35));
    Glow(l, u + "get.glow", z, g, kLime, 0.25 * (1 - h));
    pmui::Bubble(l, u + "get.bub", z + 1, g, s * 0.28 * k, 1);
    pmui::Box(l, u + "get.w", z + 2, g, A(kWhite, h), s * 0.28 * k);
    pmui::Pic(l, u + "get.i", z + 3, g.Grow(-gs * 0.28), "pm-down.png", kBlack);
    return pmui::Click(l, b, u + "get");
}

// NEW: just the letters, ballest's pink, a subtle pink glow round them, breathing slowly (Lvcid: no pill), its middle
// at (cx, cy); in the space the pill took.
void NewTag(Layer& l, const std::string& u, int z, double cx, double cy, double px) { NewLetters(l, u + "new", z, cx, cy, px); }

// The same, its top left corner at (x, y).
void NewTagAt(Layer& l, const std::string& u, int z, double x, double y, double px) {
    const double tw = Measured(l, u + "new", "NEW", px);
    NewTag(l, u, z, x + (tw + 14) / 2, y + px * 0.75, px);
}

void Clicked(const Cos& c) {
    gC.open = gC.open == c.key ? "" : c.key;
}

// A tile of the big grid: the picture, name, by, its chips, get.
void CosTile(Layer& list, const Cos& c, const Rect& R) {
    const std::string u = "ct." + c.key + ".";
    const bool over = pmui::Over(list, R);
    const double h = Smooth(Ease(u + "h", over || gC.open == c.key, kRowEase));
    const Rect C = R.Moved(0, -4 * h);
    Glow(list, u + "glow", 1, C, kWhite, 0.035 * h);
    Layer& card = pmui::Group(list, u + "card", 2, C, true);
    const bool picked = gC.open == c.key;
    pmui::Box(card, "bg", 0, {0, 0, C.w, C.h}, kCard, 20).Outline(2, picked ? Mix(kTrim, kLime, 0.7) : Mix(kTrim, kTrimHover, h));
    pmui::Pic(card, "sheen", 1, {0, 0, C.w * 0.6, C.h}, "pm-fade-left.png", A(kWhite, 0.06 * h));
    const double pw = C.w - 24;
    CosPicture(card, "pic", 2, {12, 12, pw, pw}, c, 16, 3, h);
    if (c.fresh) NewTagAt(card, u, 5, 12 + 10, 12 + 10, 12.5);
    const double ny = pw + 12 + 24;
    const double nw = Measured(card, "name", c.name, 19);
    pmui::Text(card, "name", 4, pmui::Fit(c.name, nw, C.w - 32), 19, "Regular", Lift(ux::Hex("#F2F3F5"), h), 16, ny);
    const std::string by = ByLine(c);
    pmui::Text(card, "by", 4, pmui::Fit(by, Measured(card, "by", by, 14), C.w - 32), 14, "Semilight", Lift(kVer, h), 16, ny + 25);
    // the bottom line: its pack; the check on the right (it's yours)
    const double by2 = C.h - 18 - 20;
    if (!c.pack.empty()) Chip(card, "chip", 4, 14, by2, pmui::Fit(PackName(c.pack), Measured(card, "chip.m", PackName(c.pack), 13), C.w - 100), ChipKind::Plain, h);
    if (pmui::Click(card, {0, 0, C.w, C.h}, u + "open")) Clicked(c);
    CosGetButton(card, u, 6, {C.w - 14 - 40, C.h - 14 - 40, 40, 40}, true, false);
}

// A tile of the small grid: the picture, its name under it; get on the picture's corner.
void CosSmall(Layer& list, const Cos& c, const Rect& R) {
    const std::string u = "cs." + c.key + ".";
    const bool over = pmui::Over(list, R);
    const double h = Smooth(Ease(u + "h", over || gC.open == c.key, kRowEase));
    const Rect C = R.Moved(0, -3 * h);
    Layer& card = pmui::Group(list, u + "card", 2, C, true);
    const bool picked = gC.open == c.key;
    pmui::Box(card, "bg", 0, {0, 0, C.w, C.h}, kCard, 16).Outline(2, picked ? Mix(kTrim, kLime, 0.7) : Mix(kTrim, kTrimHover, h));
    const double pw = C.w - 16;
    CosPicture(card, "pic", 2, {8, 8, pw, pw}, c, 12, 2, h);
    if (c.fresh) NewTagAt(card, u, 5, 8 + 8, 8 + 8, 11);
    const double nw = Measured(card, "name", c.name, 15);
    pmui::Text(card, "name", 4, pmui::Fit(c.name, nw, C.w - 20), 15, "Regular", Lift(ux::Hex("#F2F3F5"), h), 10, pw + 8 + 18);
    if (pmui::Click(card, {0, 0, C.w, C.h}, u + "open")) Clicked(c);
    CosGetButton(card, u, 6, {C.w - 8 - 32 - 4, pw + 8 - 32 - 4, 32, 32}, true, false);
}

// A row of the list: the plugins page's row (left tab with get, the card, the button panel).
void CosRow(Layer& list, const Cos& c, const Rect& whole) {
    const std::string u = "cr." + c.key + ".";
    const bool over = pmui::Over(list, whole);
    const double h = Smooth(Ease(u + "h", over || gC.open == c.key, kRowEase));
    constexpr double radius = 30, panelW = 46 * 2 + 11 + 30 + 30;
    const double cy = whole.h / 2;
    const Rect tab{whole.x, whole.y, kGetTab, whole.h};
    TabLines(list, u, tab, radius, h);
    CosGetButton(list, u, 5, {tab.x + tab.w / 2 - 23, whole.y + cy - 23, 46, 46}, true, false);
    const Rect R{whole.x + kGetTab, whole.y, whole.w - kGetTab - panelW + kPanelRound, whole.h};
    Accent(list, u, R, radius, over);
    Layer& row = pmui::Group(list, u + "row", 3, R, true);
    const bool picked = gC.open == c.key;
    pmui::BoxCorners(row, "bg", 0, {0, 0, R.w + 40, R.h}, kCard, radius, 0, 0, radius).Outline(2, picked ? Mix(kTrim, kLime, 0.7) : Mix(kTrim, kTrimHover, 0.6 * h));
    pmui::Pic(row, "sheen", 1, {0, 0, std::min(220.0, R.w * 0.3), R.h}, "pm-fade-left.png", A(kWhite, 0.07 * h));
    CosPicture(row, "pic", 3, {18, cy - 32, 64, 64}, c, 15, 3, 0);
    double x = 18 + 64 + 18;
    const double nx = x;
    x += pmui::Text(row, "name", 4, c.name, 25, "Regular", Lift(ux::Hex("#F2F3F5"), h), x, cy - 17) + 12;
    if (!c.version.empty()) x += pmui::Text(row, "ver", 4, c.version, 16, "Semilight", Lift(kVerMuted, h), x, cy - 15) + 12;
    const std::string by = ByLine(c);
    if (!by.empty()) x += pmui::Text(row, "by", 4, by, 16, "Semilight", Lift(kVer, h), x, cy - 15) + 12;
    if (!c.pack.empty()) x += Chip(row, "chip", 4, x, cy - 16, PackName(c.pack), ChipKind::Plain, h) + 10;
    if (c.fresh) NewTag(row, u, 5, x + 24, cy - 16, 12);
    const std::string line = SlotLabel(c.slot) + (c.credits.empty() ? "" : "  \xC2\xB7  " + c.credits);
    pmui::Text(row, "desc", 4, line, 16.5, "Semilight", Lift(kDescLine, h), nx, cy + 19);
    // the panel: the pack, remove
    const double px = R.R() - kPanelRound;
    const Color edge = Mix(kTrim, kTrimHover, 0.6 * h);
    pmui::BoxCorners(list, u + "panel", 4, {px, whole.y, whole.R() - px, whole.h}, kLinkPanel, kPanelRound, 0, 0, kPanelRound).Outline(2, edge);
    double bx = px + 24;
    const double by2 = whole.y + cy - 23;
    if (c.pack.empty()) OffButton(list, u + "pack", 6, {bx, by2, 46, 46}, "pm-pack.png");
    else if (IconButton(list, u + "pack", 6, {bx, by2, 46, 46}, "pm-pack.png", 0, 13, nullptr, 0, &kButtonFill)) OpenPack(c.pack);
    bx += 57;
    if (Removable(c)) {
        if (IconButton(list, u + "remove", 6, {bx, by2, 46, 46}, "pm-trash.png", 0, 13, &kRemoveRed, 0.08, &kButtonFill)) AskRemove(c.pack);
    } else {
        OffButton(list, u + "remove", 6, {bx, by2, 46, 46}, "pm-trash.png");
    }
    if (pmui::Click(row, {0, 0, R.w - kPanelRound, R.h}, u + "open")) Clicked(c);
}

// The details drawer, on the right of the page.
void CosDetails(Layer& wl, const Rect& area) {
    const Cos* cp = FindCos(gC.open);
    if (!cp) gC.open.clear();
    const double t = Smooth(Ease("cos.drawer", cp != nullptr, 0.05));
    if (t < 0.01 || !cp) return;
    const Cos& c = *cp;
    const Rect d{area.R() - kCosDrawer * t, area.y, kCosDrawer, area.h};
    Glow(wl, "cos.d.shadow", 40, d, kBlack, 0.6 * t);
    Layer& dl = pmui::Group(wl, "cos.drawer", 41, d, true, t);
    pmui::BoxCorners(dl, "bg", 0, {0, 0, d.w + 30, d.h}, ux::Hex("#0B0C0E"), 22, 0, 0, 0).Outline(2, kSideEdge);
    const std::string u = "cd.";
    if (RoundButton(dl, u + "close", 4, {d.w - 22 - 40, 18, 40, 40}, "pm-x.png", 16)) gC.open.clear();
    CosPicture(dl, "pic", 2, {28, 22, 180, 180}, c, 20, 3, 0);
    if (c.fresh) NewTag(dl, u, 5, 28 + 28, 22 + 24, 13);
    double x = 228;
    pmui::Text(dl, "name", 3, pmui::Fit(c.name, Measured(dl, "name", c.name, 28), d.w - x - 70), 28, "Regular", kWhite, x, 50);
    const std::string by = ByLine(c) + (c.version.empty() ? "" : "  \xC2\xB7  " + c.version);
    pmui::Text(dl, "by", 3, by, 16, "Semilight", kSub, x, 84);
    double chipX = x;
    chipX += Chip(dl, "slot", 3, chipX, 114, SlotLabel(c.slot), ChipKind::Plain, 0) + 8;
    if (!c.pack.empty()) Chip(dl, "packc", 3, chipX, 114, PackName(c.pack), ChipKind::Plain, 0);
    // yours (what's listed is installed)
    const double ay = 148;
    pmui::Box(dl, u + "got", 3, {x, ay, 120, 44}, kBlack, 13).Outline(2, kLime);
    pmui::Text(dl, u + "got.t", 4, "yours", 17, "Bold", kLime, x + 60, ay + 22, 0.5);
    double y = 222;                             // under the picture (22 + 180) and 20 more
    if (!c.description.empty()) {
        double ph = pmui::Para(dl, "desc", 3, c.description, 16.5, "Semilight", kDescOpen, {28, y, d.w - 56, 80});
        y += std::max(24.0, ph) + 18;
    }
    // what it's made of, in plain words (Lvcid: the parameter categories, friendly; still being designed)
    pmui::Box(dl, "f.bar", 3, {28, y + 3, 3, 14}, kLime, 1.5);
    pmui::Text(dl, "f.t", 3, "WHAT IT'S MADE OF", 14, "Bold", kHead, 40, y + 10);
    y += 30;
    std::vector<std::pair<std::string, std::string>> features = {{"kind", SlotLabel(c.slot)}};
    if (!c.credits.empty()) features.push_back({"credits", c.credits});
    for (size_t i = 0; i < features.size(); ++i) {
        const std::string k = "f" + std::to_string(i);
        pmui::Box(dl, k + ".b", 3, {28, y, d.w - 56, 34}, kSetRow, 10);
        pmui::Text(dl, k + ".k", 4, features[i].first, 14, "Semilight", kVer, 42, y + 17);
        pmui::Text(dl, k + ".v", 4, features[i].second, 15.5, "Regular", kText, 150, y + 17);
        y += 38;
    }
    y += 12;
    // the facts
    std::vector<std::pair<std::string, std::string>> facts;
    facts.push_back({"id", c.id});
    if (!c.version.empty()) facts.push_back({"version", c.version});
    double fx = 28;
    for (size_t i = 0; i < facts.size(); ++i) {
        const std::string k = "fact" + std::to_string(i);
        const double a = pmui::Text(dl, k + ".k", 3, facts[i].first, 14, "Semilight", kVer, fx, y + 8);
        const double b = pmui::Text(dl, k + ".v", 3, facts[i].second, 16, "Regular", kText, fx, y + 30);
        fx += std::max({a, b, 40.0}) + 28;
    }
    y += 58;
    // the rest of its pack
    const std::vector<const Cos*> pack = PackOf(c);
    if (pack.size() > 1 && y + 110 < d.h - 80) {
        pmui::Text(dl, "p.t", 3, "MORE IN " + PackName(c.pack), 14, "Bold", kHead, 28, y + 10);
        double mx = 28;
        for (const Cos* o : pack) {
            if (o == &c) continue;
            if (mx + 64 > d.w - 28) break;
            const Rect r{mx, y + 26, 64, 64};
            const std::string k = "p." + o->key;
            const double oh = Ease(k, pmui::Over(dl, r));
            CosPicture(dl, k, 3, r, *o, 12, 2, oh);
            if (pmui::Click(dl, r, k)) gC.open = o->key;
            mx += 72;
        }
    }
    // the bottom: the whole pack, remove
    const double bb = d.h - 22 - 44;
    if (!c.pack.empty()) {
        const double pw = PillWidth(dl, u + "gopack", "show the pack", 16, "pm-pack.png", 18);
        if (Pill(dl, u + "gopack", 3, {28, bb, pw, 44}, "show the pack", 16, "pm-pack.png", PillKind::Ghost, false, 13, &kTrimHover)) OpenPack(c.pack);
    }
    if (Removable(c)) {
        if (IconButton(dl, u + "remove", 3, {d.w - 28 - 44, bb, 44, 44}, "pm-trash.png", 0, 13, &kRemoveRed, 0.08, &kButtonFill)) AskRemove(c.pack);
    } else {
        OffButton(dl, u + "remove", 3, {d.w - 28 - 44, bb, 44, 44}, "pm-trash.png");
    }
}

// What the list shows, in its order.
std::vector<const Cos*> CosShown() {
    std::vector<const Cos*> out;
    for (const Cos& c : gCos) {
        if (!gC.pack.empty()) {
            if (c.pack != gC.pack) continue;
        } else if (gC.slot == "downloaded") {
            // (every cosmetic listed is installed: all of them)
        } else if (gC.slot == "published") {
            if (!c.published) continue;
        } else if (gC.slot == "packs") {
            if (c.pack.empty()) continue;
        } else if (gC.slot != "all" && c.slot != gC.slot) {
            continue;
        }
        if (!Bare(gC.search).empty() && SearchScore(c.name + " " + c.author + " " + c.pack + " " + c.id + " " + c.description, gC.search) < 0) continue;
        out.push_back(&c);
    }
    auto byName = [](const Cos* a, const Cos* b) { return Lower(a->name) < Lower(b->name); };
    switch (gC.sort) {
        case 1: std::stable_sort(out.begin(), out.end(), byName); break;
        case 2: std::stable_sort(out.begin(), out.end(), [](const Cos* a, const Cos* b) { return Lower(a->author) < Lower(b->author); }); break;
        case 3: std::stable_sort(out.begin(), out.end(), [](const Cos* a, const Cos* b) { return Lower(PackName(a->pack)) < Lower(PackName(b->pack)); }); break;
        default: std::stable_sort(out.begin(), out.end(), [](const Cos* a, const Cos* b) { return a->day > b->day; }); break;
    }
    return out;
}

// --- packs -------------------------------------------------------------------------------------------------------------
// What the page lists: cosmetics, and packs as one card each (all: a pack of two or more where its first shown one
// would be, and every pack with nothing to list yet (not got, or nothing it adds can be picked), Lvcid; packs: every
// pack; a slot's tab or a pack's page: the cosmetics themselves). In the sort's order.
struct Listed {
    const Cos* c = nullptr;
    const PackEntry* pack = nullptr;
};
bool PackMatches(const PackEntry& p) {
    if (Bare(gC.search).empty() || SearchScore(PackName(p.name) + " " + p.author + " " + p.name + " " + p.description, gC.search) >= 0) return true;
    for (const Cos* c : p.items)
        if (SearchScore(c->name + " " + c->id, gC.search) >= 0) return true;
    return false;
}
std::vector<Listed> Arrange(const std::vector<const Cos*>& shown) {
    std::vector<Listed> out;
    const bool all = gC.pack.empty() && (gC.slot == "all" || gC.slot == "downloaded"), packs = gC.pack.empty() && gC.slot == "packs";
    const bool owned = gC.pack.empty() && gC.slot == "downloaded";
    if (packs || all) {
        for (const PackEntry& p : gPacks)
            if (PackMatches(p) && (!owned || p.installed)) out.push_back({nullptr, &p});
    } else {
        for (const Cos* c : shown) out.push_back({c, nullptr});
    }
    if (!all && !packs) return out;         // (the cosmetics are in the sort's order already)
    auto day = [](const Listed& l) { return l.pack ? l.pack->day : l.c->day; };
    auto name = [](const Listed& l) { return Lower(l.pack ? PackName(l.pack->name) : l.c->name); };
    auto author = [](const Listed& l) { return Lower(l.pack ? l.pack->author : l.c->author); };
    auto pack = [](const Listed& l) { return Lower(PackName(l.pack ? l.pack->name : l.c->pack)); };
    switch (gC.sort) {
        case 1: std::stable_sort(out.begin(), out.end(), [&](const Listed& a, const Listed& b) { return name(a) < name(b); }); break;
        case 2: std::stable_sort(out.begin(), out.end(), [&](const Listed& a, const Listed& b) { return author(a) < author(b); }); break;
        case 3: std::stable_sort(out.begin(), out.end(), [&](const Listed& a, const Listed& b) { return pack(a) < pack(b); }); break;
        default: std::stable_sort(out.begin(), out.end(), [&](const Listed& a, const Listed& b) { return day(a) > day(b); }); break;
    }
    return out;
}

void OpenPack(const std::string& name) {
    gC.glide.from = gC.slot + "/" + gC.pack;
    gC.glide.at = pmui::Now();
    gC.pack = name;
    gC.open.clear();
}

// A pack's plugin, installed, with an update waiting (its update mark and its page's update), or null.
const Plug* PackUpdate(const PackEntry& p) {
    for (const Plug& q : gInstalled)
        if (q.id == p.name && q.update) return &q;
    return nullptr;
}

// A get that needs a newer plugin manager (Lvcid): a lock, why on hover; nothing to press.
void LockedGet(Layer& l, const std::string& u, int z, const Rect& b, const std::string& need) {
    LockedCircle(l, u + "lockget", z, b, LockReason(need));       // (as the plugins page's: Lvcid)
}

// A pack's pictures in r: one, or up to four in a square of four.
void PackPictures(Layer& l, const std::string& key, int z, const Rect& r, const PackEntry& p, double radius, double ring, double hover) {
    if (p.items.empty()) {
        pmui::Box(l, key + ".b", z, r, kTile, radius).Outline(static_cast<float>(ring), Mix(ux::Hex("#383B41"), kWhite, 0.85 * hover));
        Obj texture = p.icon.empty() ? nullptr : ux::LoadPicture(eng::Widen(p.icon));
        if (!texture) texture = ux::LoadPicture(eng::Widen(registry::DefaultIcon()));
        const Rect in = r.Grow(-ring);
        if (texture) pmui::Texture(l, key + ".i", z + 1, in, texture, kWhite, std::max(0.0, radius - ring));
        return;
    }
    if (p.items.size() == 1) return CosPicture(l, key + ".0", z, r, *p.items[0], radius, ring, hover);
    const double g = std::max(4.0, r.w * 0.04), q = (r.w - g) / 2;
    for (size_t i = 0; i < 4; ++i) {
        const Rect cell{r.x + (i % 2) * (q + g), r.y + (i / 2) * (q + g), q, q};
        if (i < p.items.size()) CosPicture(l, key + "." + std::to_string(i), z, cell, *p.items[i], radius * 0.7, ring, hover);
        else pmui::Box(l, key + ".e" + std::to_string(i), z, cell, kTile, radius * 0.7).Outline(static_cast<float>(ring), ux::Hex("#26282D"));
    }
}

std::string PackCount(const PackEntry& p) {
    if (!p.installed) return Busy(p) ? "getting..." : "not got yet";
    if (const Plug* q = FindPlug(p.name); q && !q->enabled && p.items.empty()) return "turned off";
    return std::to_string(p.items.size()) + (p.items.size() == 1 ? " item" : " items");     // (Lvcid's word)
}

// A pack's card, in each view's shape: kind 0 the big grid's tile, 1 the small grid's, 2 the list's row. A click opens it.
void PackCard(Layer& list, const PackEntry& p, const Rect& R, int kind) {
    const std::string u = "pk." + p.name + "." + std::to_string(kind) + ".";
    const bool over = pmui::Over(list, R);
    const double h = Smooth(Ease(u + "h", over, kRowEase));
    const std::string by = p.author.empty() ? "by several" : "by " + p.author;
    const Color name = Lift(ux::Hex("#F2F3F5"), h);
    if (kind == 2) {
        constexpr double radius = 30;
        const double cy = R.h / 2;
        TabLines(list, u, {R.x, R.y, kGetTab, R.h}, radius, h);
        if (p.installed) pmui::Pic(list, u + "tab.i", 5, {R.x + kGetTab / 2 - 14, R.y + cy - 14, 28, 28}, "pm-pack.png", Mix(kSegText, kLime, 0.6 + 0.4 * h));
        const Rect C{R.x + kGetTab, R.y, R.w - kGetTab, R.h};
        Accent(list, u, C, radius, over);
        Layer& row = pmui::Group(list, u + "row", 3, C, true);
        pmui::Box(row, "bg", 0, {0, 0, C.w, C.h}, kCard, radius).Outline(2, Mix(kTrim, kTrimHover, 0.6 * h));
        pmui::Pic(row, "sheen", 1, {0, 0, std::min(220.0, C.w * 0.3), C.h}, "pm-fade-left.png", A(kWhite, 0.07 * h));
        PackPictures(row, "pic", 3, {18, cy - 32, 64, 64}, p, 15, 3, 0);
        double x = 18 + 64 + 18;
        x += pmui::Text(row, "name", 4, PackName(p.name), 25, "Regular", name, x, cy - 17) + 12;
        x += pmui::Text(row, "by", 4, by, 16, "Semilight", Lift(kVer, h), x, cy - 15) + 12;
        if (p.fresh) NewTag(row, u, 5, x + 24, cy - 16, 12);
        const std::string line = PackCount(p) + (p.installed || p.description.empty() ? "" : "  \xC2\xB7  " + p.description);
        pmui::Text(row, "count", 4, pmui::Fit(line, Measured(row, "count.m", line, 16.5), C.w - 18 - 64 - 18 - 70), 16.5, "Semilight", Lift(kDescLine, h), 18 + 64 + 18, cy + 19);
        const double ch = Ease(u + "chev", over);
        pmui::Pic(row, "chev", 4, {C.w - 18 - 30 - 6 * ch, cy - 15, 30, 30}, "pm-chev.png", Mix(Lift(kSegText, h), kWhite, ch)).Transform(1, -90);
        if (pmui::Click(row, {0, 0, C.w, C.h}, u + "open")) OpenPack(p.name);
        const Rect gb{R.x + kGetTab / 2 - 23, R.y + cy - 23, 46, 46};
        if (const std::string need = p.installed ? "" : NeedsHost(p.name); !need.empty()) LockedGet(list, u, 5, gb, need);
        else if (!p.installed && CosGetButton(list, u, 5, gb, false, Busy(p))) AskGet(p);
        if (const Plug* up = PackUpdate(p); up && DotShown("upd." + up->id + "@" + up->latest, over))
            pmui::Box(list, u + "upd.dot", 7, {C.x + 10, R.y + 6, 12, 12}, kRed, 6).Outline(2, kCard);
        return;
    }
    const bool small = kind == 1;
    const Rect C = R.Moved(0, -(small ? 3 : 4) * h);
    if (!small) Glow(list, u + "glow", 1, C, kWhite, 0.035 * h);
    Layer& card = pmui::Group(list, u + "card", 2, C, true);
    const double radius = small ? 16 : 20, pad = small ? 8 : 12, pw = C.w - 2 * pad;
    pmui::Box(card, "bg", 0, {0, 0, C.w, C.h}, kCard, radius).Outline(2, Mix(kTrim, kTrimHover, h));
    if (!small) pmui::Pic(card, "sheen", 1, {0, 0, C.w * 0.6, C.h}, "pm-fade-left.png", A(kWhite, 0.06 * h));
    PackPictures(card, "pic", 2, {pad, pad, pw, pw}, p, small ? 12 : 16, small ? 2 : 3, h);
    if (p.fresh) NewTagAt(card, u, 5, pad + (small ? 8 : 10), pad + (small ? 8 : 10), small ? 11 : 12.5);
    // a pack's mark on its picture's top right corner
    const double ms = small ? 24 : 30;
    const Rect mark{pad + pw - ms - (small ? 6 : 8), pad + (small ? 6 : 8), ms, ms};
    pmui::Box(card, "mark", 5, mark, A(kBlack, 0.72), ms * 0.3).Outline(1, A(kLime, 0.35));
    pmui::Pic(card, "mark.i", 6, mark.Grow(-ms * 0.2), "pm-pack.png", kLime);
    if (small) {
        pmui::Text(card, "name", 4, pmui::Fit(PackName(p.name), Measured(card, "name", PackName(p.name), 15), C.w - 20), 15, "Regular", name, 10, pw + pad + 18);
    } else {
        const double ny = pw + pad + 24;
        pmui::Text(card, "name", 4, pmui::Fit(PackName(p.name), Measured(card, "name", PackName(p.name), 19), C.w - 32), 19, "Regular", name, 16, ny);
        pmui::Text(card, "by", 4, pmui::Fit(by, Measured(card, "by", by, 14), C.w - 32), 14, "Semilight", Lift(kVer, h), 16, ny + 25);
        const double by2 = C.h - 18 - 20;
        Chip(card, "chip", 4, 14, by2, PackCount(p), ChipKind::Plain, h);
    }
    if (pmui::Click(card, {0, 0, C.w, C.h}, u + "open")) OpenPack(p.name);
    // get (lime) while it's not got, else a quiet check (before: the last drawn wins the click)
    const double gs = small ? 32 : 40;
    const Rect gb = small ? Rect{C.w - 8 - gs - 4, pw + pad - gs - 4, gs, gs} : Rect{C.w - 14 - gs, C.h - 14 - gs, gs, gs};
    if (const std::string need = p.installed ? "" : NeedsHost(p.name); !need.empty()) LockedGet(list, u, 6, gb.Moved(C.x, C.y), need);
    else if (CosGetButton(card, u, 6, gb, p.installed, Busy(p))) AskGet(p);
    // an update waiting: a red dot on its top right corner (seen once, as the rows' update dots)
    if (const Plug* up = PackUpdate(p); up && DotShown("upd." + up->id + "@" + up->latest, over))
        pmui::Box(card, "upd.dot", 7, {C.w - 20, 8, 12, 12}, kRed, 6).Outline(2, kCard);
}

// --- featured and owned: a pack as a row (Lvcid, 2026-10-10) ----------------------------------------------------------
// As simple as the plugins page: each pack a row; a click opens it in place (its description, the facts, what's in it,
// a little smaller and in a little), no page of its own. Getting is the whole pack, always (AnythingGoes).
constexpr double kPackRowH = 92;
std::map<std::string, double> gPackExtra;   // an open pack row's details' height, from the frame before

// An owned pack's key in its row's left tab: the pack icon (several cosmetics) or a check (just one), on the dark
// button; hovered, a nulime ring round it and a soft nulime glow (the logo's hover, as a rounded square). A click opens
// the row.
bool PackKey(Layer& l, const std::string& u, int z, const Rect& b, bool single) {
    const double h = EaseUpDown(u + "key", pmui::Over(l, b), 0.06, 0.14);
    if (h > 0.01) Glow(l, u + "key.glow", z, b, kNuLime, 0.35 * h);
    pmui::Box(l, u + "key.b", z + 1, b, kButtonFill, 13).Outline(2, kAbEdge);
    if (h > 0.01) pmui::Box(l, u + "key.ring", z + 2, b, A(kTile, 0), 13).Outline(3, A(kNuLime, 0.85 * h));
    const Color rest = single ? A(kLime, 0.75) : kSub;
    pmui::Pic(l, u + "key.i", z + 3, b.Grow(-b.w * 0.26), single ? "check.png" : "pm-pack.png", Mix(rest, kNuLime, h));
    return pmui::Click(l, b, u + "key");
}

void TogglePackRow(const std::string& name) { gC.expanded = gC.expanded == name ? "" : name; }

// A pack's row at R (its height: closed `base`, more while open).
void PackRow(Layer& list, const PackEntry& p, const Rect& R, double base) {
    const std::string u = "pr." + p.name + ".";
    const bool over = pmui::Over(list, R);
    const double h = Smooth(Ease(u + "h", over, kRowEase));
    const double open = Smooth(gAnim["pr." + p.name].open);
    constexpr double radius = 26;
    const double cy = base / 2;
    TabLines(list, u, {R.x, R.y, kGetTab, R.h}, radius, h);
    const Rect C{R.x + kGetTab, R.y, R.w - kGetTab, R.h};
    const bool clicked = pmui::Click(list, C, u + "open");       // (before its own buttons, which win)
    Accent(list, u, C, radius, over);
    Layer& row = pmui::Group(list, u + "row", 3, C, true);
    pmui::Box(row, "bg", 0, {0, 0, C.w, C.h}, kCard, radius).Outline(2, Mix(gC.expanded == p.name ? kOpenEdge : kTrim, kTrimHover, 0.6 * h));
    pmui::Pic(row, "sheen", 1, {0, 0, std::min(220.0, C.w * 0.3), C.h}, "pm-fade-left.png", A(kWhite, 0.07 * h));
    PackPictures(row, "pic", 3, {18, cy - 30, 60, 60}, p, 14, 3, 0);
    const double nx = 18 + 60 + 18;
    // the right end: its update (as the plugins page's update all covers it), then remove
    double right = C.w - 18;
    if (p.installed && RemovablePack(p)) {
        if (IconButton(row, "rm", 5, {right - 40, cy - 20, 40, 40}, "pm-trash.png", 0, 13, &kRemoveRed, 0.08, &kButtonFill)) AskRemove(p.name);
        right -= 40 + 12;
    }
    if (const Plug* up = p.installed ? PackUpdate(p) : nullptr) {
        if (const std::string need = NeedsHost(p.name); !need.empty()) {
            right -= LockedPill(row, "upd", right, cy, "update " + up->latest, 1.3, true, true, false, LockReason(need)) + 12;
        } else {
            const bool busy = up->pending == "installing";
            const std::string label = busy ? "updating..." : "update " + up->latest;
            const double uw = PillWidth(row, "upd", label, 16, "pm-down.png", 18);
            if (Pill(row, "upd", 6, {right - uw, cy - 20, uw, 40}, label, 16, "pm-down.png", PillKind::Lime) && !busy) {
                InstallUpdate(*up);
                gCosAt = -100;
                gModelAt = -100;
            }
            right -= uw + 12;
        }
    }
    // name, by, NEW; under it how many and (closed, not got) what it is
    const Color ink = Lift(ux::Hex("#F2F3F5"), h);
    double x = nx;
    const double nameMax = std::max(80.0, right - nx - 200);
    x += pmui::Text(row, "name", 4, pmui::Fit(PackName(p.name), Measured(row, "name.m", PackName(p.name), 23), nameMax), 23, "Regular",
                    Mix(ink, kLime, open), x, cy - 14) + 12;
    const std::string by = p.author.empty() ? "by several" : "by " + p.author;
    x += pmui::Text(row, "by", 4, by, 15, "Semilight", Lift(kVer, h), x, cy - 12) + 12;
    if (p.fresh || gTestNew) NewTag(row, u, 5, x + 20, cy - 13, 12);
    const std::string line = PackCount(p) + (p.installed || p.description.empty() ? "" : "  \xC2\xB7  " + p.description);
    pmui::Text(row, "count", 4, pmui::Fit(line, pmui::Width(line, 15.5, "Semilight"), right - nx - 14), 15.5, "Semilight",
               A(Lift(kDescLine, h), 1 - open), nx, cy + 16);
    // open: the description, the facts, and what's in it
    if (open > 0.01 || gC.expanded == p.name) {
        static std::map<std::string, double> descH;
        double y = base - 4;
        const double w = std::min(900.0, C.w - nx - 40);
        if (!p.description.empty()) {
            const double ph = pmui::Para(row, "desc", 4, p.description, 16.5, "Semilight", kDescOpen, {nx, y, w, 300});
            if (ph > 0) descH[p.name] = ph;
            y += (descH[p.name] > 0 ? descH[p.name] : 24) + 12;
        }
        std::string facts = by + "  \xC2\xB7  " + PackCount(p);
        if (!p.version.empty()) facts += "  \xC2\xB7  version " + p.version;
        pmui::Text(row, "facts", 4, facts, 14, "Semilight", kVer, nx, y + 8);
        y += 30;
        if (p.items.empty()) {
            pmui::Text(row, "none", 4, p.installed ? "It adds nothing that can be picked on the Customize page." : "Its cosmetics show here once it's installed.", 15,
                       "Semilight", kSub, nx + 16, y + 10);
            y += 32;
        } else {
            // its cosmetics, each a little card (in a little from the text): a click shows its details
            constexpr double kItem = 68, kItemGap = 14, kNameH = 30;
            const double x0 = nx + 16;
            double ix = x0;
            for (const Cos* c : p.items) {
                if (ix + kItem > C.w - 24 && ix > x0) {
                    ix = x0;
                    y += kItem + kNameH;
                }
                const Rect tr{ix, y, kItem, kItem};
                const std::string k = "it." + c->key;
                const double th = Ease(u + k, pmui::Over(row, tr) || gC.open == c->key);
                const Rect tm = tr.Moved(0, -3 * th);
                CosPicture(row, k, 5, tm, *c, 14, 2, th);
                pmui::Text(row, k + ".n", 5, pmui::Fit(c->name, Measured(row, k + ".n", c->name, 12.5), kItem + kItemGap - 4), 12.5, "Regular",
                           Mix(kSub, kWhite, th), tr.Cx(), tr.B() + 13, 0.5);
                if (pmui::Click(row, tr, u + k)) Clicked(*c);
                ix += kItem + kItemGap;
            }
            y += kItem + kNameH;
        }
        gPackExtra[p.name] = std::max(48.0, y - base + 12);
    }
    // the left tab: owned, its key (opens it); not got, get (the whole pack); one needing a newer plugin manager, locked
    const Rect kb{R.x + kGetTab / 2 - 23, R.y + cy - 23, 46, 46};
    bool toggle = clicked;
    if (p.installed) toggle = PackKey(list, u, 5, kb, p.items.size() == 1) || toggle;
    else if (const std::string need = NeedsHost(p.name); !need.empty()) LockedGet(list, u, 5, kb, need);
    else if (CosGetButton(list, u, 5, kb, false, Busy(p))) AskGet(p);
    if (const Plug* up = p.installed ? PackUpdate(p) : nullptr; up && DotShown("upd." + up->id + "@" + up->latest, over))
        pmui::Box(list, u + "upd.dot", 7, {C.x + 10, R.y + 6, 12, 12}, kRed, 6).Outline(2, kCard);
    if (toggle) TogglePackRow(p.name);
}

// Cosmetic Kit not installed, or off: what the page needs.
void KitMissing(Layer& wl, const Rect& area) {
    const double cx = area.w / 2, cy = area.y + area.h / 2 - 60;
    const char* const slots[] = {"ball", "hat", "bfx"};
    for (int i = 0; i < 3; ++i) {
        const Rect r{cx - 50 + (i - 1) * 112, cy - 170 + (i == 1 ? 0 : 24), 100, 100};
        pmui::Box(wl, "kit.s" + std::to_string(i), 10, r, kTile, 22).Outline(3, ux::Hex("#383B41"));
        pmui::Pic(wl, "kit.si" + std::to_string(i), 11, r.Grow(-16), SlotPicture(slots[i]).c_str(), A(kWhite, i == 1 ? 0.75 : 0.4));
    }
    pmui::Text(wl, "kit.t", 10, "Cosmetics need Cosmetic Kit", 30, "Regular", kWhite, cx, cy - 20, 0.5);
    pmui::Text(wl, "kit.s", 10, "It adds the custom section and loadouts to the Customize page, and this page (Cosmetic Kit Plus adds more slots).", 17, "Semilight", kSub, cx, cy + 18, 0.5);
    const Plug* kit = FindPlug("cosmetic-kit");
    if (kit && kit->installed) {
        pmui::Text(wl, "kit.off", 10, "Cosmetic Kit is off", 18, "Regular", kRowLabel, cx - 40, cy + 74, 1);
        if (Toggle(wl, "kit.tog", 10, cx - 20, cy + 59, false, false)) {
            plugins::SetEnabled("cosmetic-kit", true);
            gModelAt = -100;
        }
    } else {
        const double pw = PillWidth(wl, "kit.get", "find Cosmetic Kit", 18, "pm-down.png", 24);
        if (Pill(wl, "kit.get", 10, {cx - pw / 2, cy + 52, pw, 46}, "find Cosmetic Kit", 18, "pm-down.png", PillKind::Lime)) {
            SetPage("plugins");
            gS.tab = "browse";
            gS.installedSearch = "cosmetic kit";     // (the page's one search: its results show browse's too)
            if (gInstalledField) w::SetText(gInstalledField, gS.installedSearch);
        }
    }
}

// A sidebar tab's hover (Lvcid): the plugin rows' lit edge, all lime, on the tab's left inside it (there's no room
// outside: the window's edge): its trim lit from the middle out, a glow and smoke drifting right, sparks along the
// edge, particles drifting off it. Timed as Accent. `radius`: the tab's corners (a pill's: half its height); `fx`: a
// layer over everything (gFx) where the glow, smoke and particles go, unclipped, hanging out past the tab and the window.
void SideAccent(Layer& sb, const std::string& k, const Rect& r, bool over, const Color& /*back*/, double radius = 12, Layer* fx = nullptr) {
    Anim& lamp = gAnim[k + ".acc.on"];
    const double before = lamp.value;
    const double on = EaseUpDown(k + ".acc.on", over, 0.09, 0.2), lit = Smooth(on);
    const double flare = on > before ? std::sin(3.14159265 * on) : 0;
    // (as Accent's: revealed with the pages' curve and speed; unhovered, it fades out where it is, not shrinking back)
    const double grow = HoverCurve(k + ".acc.len", over || lit > 0.01, kHoverIn, kHoverOut);
    if (lit < 0.01) return;
    const double now = pmui::Now(), breathe = 0.5 + 0.5 * std::sin(now * 1.7);
    Layer& in = pmui::Group(sb, k + ".acc", fx ? 4 : 2, r, true);
    const double cy = r.h / 2, len = std::max(4.0, r.h * grow);
    constexpr int kBands = 8;
    for (int i = 0; i < kBands; ++i) {
        const double half = len / 2 * (i + 1) / kBands;
        Layer& band = pmui::Group(in, "b" + std::to_string(i), 0, {0, cy - half, radius + 2, 2 * half}, true);
        const double oy = cy - half;
        // (the trim alone, an outline: nothing drawn over what's inside the tab)
        pmui::Box(band, "lime", 0, {0, -oy, r.w, r.h}, A(kWhite, 0), radius).Outline(2, A(Mix(kLime, AccentPale(), 0.5 * flare), 0.24 * lit));
    }
    // The loose effects. With fx (the settings nav): on it, round the tab's pill and never over it, the smoke and the
    // particles drifting left, out of it (Lvcid); else inside the tab, drifting right into it.
    const double frames = std::fmod(now / 7.3, 1.0) * 32;
    const int f0 = static_cast<int>(frames) % 32;
    const double frac = frames - std::floor(frames);
    const bool out = fx != nullptr;
    auto loose = [&](Layer& g, double dx, double dy) {
        pmui::Pic(g, "glow", 1, {dx - 80, dy + cy - r.h * 0.9, 160, r.h * 1.8}, "fx-glow.png", A(kLime, (0.3 + 0.15 * breathe) * lit + 0.25 * flare));
        char name[32];
        for (int n = 0; n < 2; ++n) {
            std::snprintf(name, sizeof name, "fx-smoke%02d.png", (f0 + n) % 32);
            pmui::Pic(g, "smoke" + std::to_string(n), 1, {dx + (out ? -150 : 0), dy + r.h * 0.06, 150, r.h * 0.88}, name,
                      A(kLime, 0.32 * lit * (n == 0 ? 1 - frac : frac)))
                .Transform(1, out ? 0 : 180);             // (turned: it drifts right, into the tab)
        }
        for (int i = 0; i < 5; ++i) {
            const double period = 1.4 + 0.11 * ((i * 37) % 7);
            const double t = now / period + i * 0.2, local = t - std::floor(t);
            const unsigned seed = static_cast<unsigned>(std::floor(t)) * 2654435761u + static_cast<unsigned>(i) * 40503u;
            const double r1 = (seed % 1000) / 1000.0, r2 = ((seed / 1000) % 1000) / 1000.0;
            const double drift = (30 + 60 * r2) * local;
            const double px = out ? -3 - drift : 3 + drift, py = cy - len / 2 + 5 + (len - 10) * r1 - 6 * local;
            const double size = 8 * (1 - 0.3 * local) * (0.8 + 0.4 * r2);
            pmui::Pic(g, "p" + std::to_string(i), 2, {dx + px - size / 2, dy + py - size / 2, size, size}, "fx-puff.png",
                      A(Mix(kLime, kWhite, 0.35), std::sin(3.14159265 * local) * 0.5 * lit));
        }
    };
    if (fx) {
        const Rect s = pmui::OnScreen(sb, r);
        const Rect hole{s.x - fx->ox, s.y - fx->oy, s.w, s.h};
        AroundPill(*fx, k + ".acc.fx", 0, {hole.x - 100, hole.y - 0.45 * hole.h, hole.w + 100, 1.9 * hole.h}, hole, radius, loose);
    } else {
        loose(in, 0, 0);
    }
    for (int i = 0; i < 3; ++i) {
        const double period = 0.6 + 0.15 * i, t = now / period + i * 0.37, local01 = t - std::floor(t);
        const unsigned seed = static_cast<unsigned>(std::floor(t)) * 2246822519u + static_cast<unsigned>(i) * 3266489917u;
        const double sy = cy - len / 2 + 6 + (len - 12) * ((seed % 1000) / 1000.0), size = 8;
        const double tw = std::sin(3.14159265 * local01);
        const double ex = CornerInset(radius, std::min(sy, r.h - sy));      // (on the pill's curve)
        pmui::Pic(in, "spark" + std::to_string(i), 2, {ex + 1.5 - size / 2, sy - size / 2, size, size}, "fx-spark.png", A(Mix(kWhite, kLime, 0.3), 0.85 * tw * tw * lit));
    }
}

// The sidebar (Lvcid): EXPLORE (all, packs with its dropdown of each pack, each slot), then LIBRARY (downloaded,
// published). The open one is the lime bubble; hovered, the lit edge (SideAccent) and its words lighten. It scrolls
// when it's taller than the page. `side`: how far out it is.
struct SideTab {
    std::string tab, label;
    int count = 0;
    const char* icon = nullptr;
};
void CosSidebar(Layer& wl, const Rect& area, double side, const std::vector<SideTab>& explore, const std::vector<SideTab>& library) {
    if (side < 1) return;
    Layer& sb = pmui::Group(wl, "cos.side", 4, {side - kCosSide, area.y, kCosSide, area.h}, true, std::min(1.0, side / kCosSide * 1.5));
    static const Color kSideBack = ux::Hex("#0B0C0E");
    pmui::BoxCorners(sb, "bg", 0, {0, 0, kCosSide, area.h}, kSideBack, 0, 0, 0, 24);
    pmui::Box(sb, "edge", 1, {kCosSide - 2, 0, 2, area.h}, kSideEdge);
    // how tall it is (the packs' dropdown counted as far as it's open), so it can scroll
    const double drop = Smooth(Ease("side.packs.drop", gC.packsOpen, 0.07));
    const double nPacks = static_cast<double>(gPacks.size());
    const double content = 22 + 26 + static_cast<double>(explore.size()) * 50 + drop * nPacks * 40 + 12 + 22 + 26 + static_cast<double>(library.size()) * 50 + 16;
    const double scroll = Scrolled("cos.side", sb, {0, 0, kCosSide, area.h}, content);
    Layer& col = pmui::Group(sb, "col", 1, {0, -scroll, kCosSide, std::max(area.h, content)}, false);
    double y = 22;
    auto heading = [&](const std::string& key, const std::string& text) {
        pmui::Text(col, key, 1, text, 13, "Bold", kHead, 30, y + 8);
        y += 26;
    };
    // one tab: its icon, words and count; `on`: open; `indent`: a pack under packs
    auto item = [&](const std::string& k, const Rect& r, bool on, const char* icon, const Cos* picture, const std::string& label, int count,
                    double px, double alpha) -> bool {
        const bool over = pmui::Over(col, r) && alpha > 0.5;
        const double h = EaseUpDown(k, over && !on, kEase * 0.6, 0.1);
        if (on) Glow(col, k + ".glow", 1, r, kLime, 0.45 * alpha);
        pmui::Bubble(col, k + ".bub", 2, r, 12, on ? alpha : 0.0);
        if (!on) SideAccent(col, k, r, over, kSideBack);
        const Color ink = A(on ? kBlack : Mix(kSub, kWhite, h), alpha);
        const double is = r.h * 0.52;
        const Rect ir{r.x + 12, r.Cy() - is / 2, is, is};
        if (picture) CosPicture(col, k + ".pic", 4, ir.Grow(2), *picture, 6, 1.5, 0);
        else if (icon) pmui::Pic(col, k + ".i", 4, ir, icon, ink);
        else pmui::Pic(col, k + ".i", 4, ir.Grow(2), SlotPicture(k.substr(5)).c_str(), ink);
        const double tx = r.x + 12 + is + 12;
        const std::string n = std::to_string(count);
        const double cw = Measured(col, k + ".n", n, 13) + 14;
        pmui::Text(col, k + ".t", 4, pmui::Fit(label, Measured(col, k + ".t", label, px), r.R() - 12 - cw - 10 - tx), px, "Regular", ink, tx, r.Cy());
        const Rect chip{r.R() - 12 - cw, r.Cy() - 11, cw, 22};
        pmui::Box(col, k + ".c", 4, chip, A(on ? A(kBlack, 0.85) : Mix(kChip, ux::Hex("#2A2C31"), h), alpha), 7);
        pmui::Text(col, k + ".n", 5, n, 13, "Bold", A(on ? kLime : Mix(kSub, kWhite, h), alpha), chip.Cx(), r.Cy(), 0.5);
        return alpha > 0.5 && pmui::Click(col, r, k);
    };
    auto pick = [&](const std::string& tab) {
        if (gC.slot == tab && gC.pack.empty()) return;
        gC.glide.from = gC.slot + "/" + gC.pack;           // (the list comes in again)
        gC.glide.at = pmui::Now();
        gC.slot = tab;
        gC.pack.clear();
        gC.open.clear();
    };
    heading("h1", "EXPLORE");
    for (const SideTab& t : explore) {
        const std::string k = "side." + t.tab;
        const Rect r{14, y, kCosSide - 30, 46};
        const bool on = gC.slot == t.tab && (gC.pack.empty() || t.tab != "packs");
        const bool clicked = item(k, r, on && gC.pack.empty(), t.icon, nullptr, t.label, t.count, 19, 1);
        if (t.tab == "packs") {
            // its dropdown's arrow (over the tab, before its count): a click on it only opens or closes the list
            const Rect ar{r.R() - 12 - 40 - 34, r.Cy() - 14, 28, 28};
            const double ah = Ease(k + ".arrow", pmui::Over(col, ar));
            const bool lime = on && gC.pack.empty();
            pmui::Pic(col, k + ".arrow", 6, ar.Grow(-4), "pm-chev.png", lime ? kBlack : Mix(kSegText, kWhite, ah)).Transform(1, 180 * drop);
            if (pmui::Click(col, ar, k + ".arrow")) gC.packsOpen = !gC.packsOpen;
            else if (clicked) {
                if (!gC.packsOpen) gC.packsOpen = true;
                pick("packs");
            }
            y += 50;
            // each pack, under it, sliding open
            if (drop > 0.01) {
                Layer& dl = pmui::Group(col, "side.packs.list", 3, {0, y, kCosSide, nPacks * 40 * drop}, true);
                double py = 0;
                for (const PackEntry& p : gPacks) {
                    const std::string pk = "side.pk." + p.name;
                    const Rect pr{30, py, kCosSide - 46, 36};
                    // (in the dropdown's group: drawn with the column's item, moved into it)
                    const bool pon = !gC.pack.empty() && gC.pack == p.name;
                    const bool over = pmui::Over(dl, pr) && drop > 0.9;
                    const double h = EaseUpDown(pk, over && !pon, kEase * 0.6, 0.1);
                    if (pon) Glow(dl, pk + ".glow", 1, pr, kLime, 0.4);
                    pmui::Bubble(dl, pk + ".bub", 2, pr, 10, pon ? 1.0 : 0.0);
                    if (!pon) SideAccent(dl, pk, pr, over, kSideBack);
                    const Color ink = pon ? kBlack : Mix(kSub, kWhite, h);
                    // (its first cosmetic's picture; a pack not got has none listed yet: its plugin's icon, no count)
                    const Rect pic{pr.x + 10, pr.Cy() - 11, 22, 22};
                    if (!p.items.empty()) CosPicture(dl, pk + ".pic", 4, pic, *p.items[0], 6, 1.5, 0);
                    else PackPictures(dl, pk + ".icon", 4, pic, p, 6, 1.5, 0);
                    const std::string n = p.items.empty() ? "" : std::to_string(p.items.size());
                    const double cw = n.empty() ? 0 : Measured(dl, pk + ".n", n, 12) + 12;
                    const std::string label = PackName(p.name);
                    pmui::Text(dl, pk + ".t", 4, pmui::Fit(label, Measured(dl, pk + ".t", label, 16), pr.R() - 10 - cw - 8 - (pr.x + 42)), 16, "Regular", ink, pr.x + 42, pr.Cy());
                    if (!n.empty()) {
                        const Rect chip{pr.R() - 10 - cw, pr.Cy() - 10, cw, 20};
                        pmui::Box(dl, pk + ".c", 4, chip, pon ? A(kBlack, 0.85) : Mix(kChip, ux::Hex("#2A2C31"), h), 6);
                        pmui::Text(dl, pk + ".n", 5, n, 12, "Bold", pon ? kLime : Mix(kSub, kWhite, h), chip.Cx(), pr.Cy(), 0.5);
                    }
                    if (drop > 0.9 && pmui::Click(dl, pr, pk)) {
                        OpenPack(p.name);           // (first: the list glides in from where it was)
                        gC.slot = "packs";
                    }
                    py += 40;
                }
                y += nPacks * 40 * drop;
            }
            continue;
        }
        if (clicked) pick(t.tab);
        y += 50;
    }
    y += 12;
    pmui::Box(col, "sep", 1, {28, y, kCosSide - 58, 2}, kPopupRing, 1);
    y += 22;
    heading("h2", "LIBRARY");
    for (const SideTab& t : library) {
        const std::string k = "side." + t.tab;
        const Rect r{14, y, kCosSide - 30, 46};
        if (item(k, r, gC.slot == t.tab && gC.pack.empty(), t.icon, nullptr, t.label, t.count, 19, 1)) pick(t.tab);
        y += 50;
    }
}

void CosmeticsPageOld(Layer& wl, const Rect& area) {
    pmui::Level(kPageLevel);
    if (!packages::KitOn()) {
        KitMissing(wl, area);
        return;
    }
    if (pmui::Now() - gCosAt > 0.25) BuildCos();
    const double cy = area.y + kSubH / 2;
    // the tabs (Lvcid, 2026-10-10): explore (featured: every pack, newest first; each slot shown), the library (owned:
    // the packs got; published)
    std::vector<SideTab> explore = {{"all", "featured", static_cast<int>(gPacks.size()), "pm-star.png"}};
    for (const std::string& sl : packages::ShownSlots(true)) {
        int n = 0;
        for (const Cos& c : gCos) n += c.slot == sl;
        explore.push_back({sl, SlotLabel(sl), n, nullptr});
    }
    int owned = 0, published = 0;
    for (const PackEntry& p : gPacks) owned += p.installed;
    for (const Cos& c : gCos) published += c.published;
    const std::vector<SideTab> library = {{"downloaded", "owned", owned, "pm-pack.png"}, {"published", "published", published, "pm-star.png"}};
    bool known = false;
    for (const SideTab& t : explore) known = known || t.tab == gC.slot;
    for (const SideTab& t : library) known = known || t.tab == gC.slot;
    if (!known) gC.slot = "all";
    if (!gC.pack.empty() && !FindPack(gC.pack)) gC.pack.clear();
    if (!gC.glide.from.empty() && pmui::Now() - gC.glide.at > 0.3) gC.glide.from.clear();
    const double side = Smooth(Ease("cos.side", gC.side, 0.06)) * kCosSide;
    CosSidebar(wl, area, side, explore, library);
    Layer& page = pmui::Group(wl, "cos.page", 5, {side, 0, area.w - side, area.y + area.h}, false);
    const Rect pa{0, area.y, area.w - side, area.h};
    // its button: folds the sidebar away and back (the page moves with it)
    {
        const Rect r{22, cy - 21, 42, 42};
        const double h = Ease("cos.side.b", pmui::Over(page, r));
        Glow(page, "cos.side.b.glow", 10, r, kWhite, 0.35 * h);
        pmui::Box(page, "cos.side.b", 11, r, Mix(kField, kWhite, h), 12).Outline(2, Mix(kAbEdge, kWhite, h));
        pmui::Pic(page, "cos.side.b.i", 12, {r.Cx() - 10, r.Cy() - 10, 20, 20}, "pm-chev.png", Mix(kSub, kBlack, h)).Transform(1, gC.side ? 90 : -90);
        if (pmui::Click(page, r, "cos.side.b")) gC.side = !gC.side;
    }
    const double stripR = 22 + 42;
    // the views, on the right: big grid, small grid, list
    const std::vector<std::pair<std::string, const char*>> views = {{"big", "pm-grid.png"}, {"small", "pm-gridsmall.png"}, {"list", "pm-thick.png"}};
    // (featured and owned are rows, as the plugins page: no views there)
    const bool packRows = gC.pack.empty() && (gC.slot == "all" || gC.slot == "downloaded");
    const double vw = packRows ? 0 : 4 + 3 * 40 + 2 * 4 + 4;
    const Rect vbox{pa.w - 22 - vw, cy - 23, vw, 46};
    if (!packRows) pmui::Box(page, "cos.views", 10, vbox, kStrip, 12).Outline(2, kBlack);
    double vx = vbox.x + 4;
    for (const auto& [view, icon] : views) {
        if (packRows) break;
        const Rect r{vx, vbox.y + 6, 40, 34};
        const bool on = gPrefs.cosView == view;
        const double h = Ease("cos.view." + view, pmui::Over(page, r));
        if (on) Glow(page, "cos.view." + view + ".glow", 11, r, kLime, 0.35);
        pmui::Box(page, "cos.view." + view + ".b", 12, r, on ? kLime : kBlack, 8);
        pmui::Pic(page, "cos.view." + view + ".i", 13, {r.Cx() - 10, r.Cy() - 10, 20, 20}, icon, on ? kBlack : Mix(kSegText, kWhite, h));
        if (pmui::Click(page, r, "cos.view." + view) && !on) {
            gPrefs.cosView = view;
            PrefsChanged();
        }
        vx += 44;
    }
    double right = vbox.x - 12;
    // back to the Customize page, when it's behind the window (the settings back button's look, pointing on)
    if (cosmetics::PageOnScreen()) {
        const double tw = Measured(page, "cos.cust.t", "customize", 18);
        const Rect r{right - (tw + 36), cy - 22, tw + 36, 44};
        const double h = Ease("cos.cust", pmui::Over(page, r));
        const Color ink = Mix(kLime, kWhite, h);
        Glow(page, "cos.cust.glow", 10, {r.x, cy - 12, tw + 4, 24}, kLime, 0.2 * h);
        pmui::Text(page, "cos.cust.t", 11, "customize", 18, "Bold", ink, r.x, cy);
        pmui::Pic(page, "cos.cust.i", 11, {r.x + tw + 10 + 4 * Smooth(h), cy - 18, 18, 36}, "pm-back.png", ink).Transform(1, 180);
        if (pmui::Click(page, r, "cos.cust")) {
            Close();
            return;
        }
        right = r.x - 16;
    }
    // the sort, its list under it
    const std::string label = std::string("sort: ") + kCosSorts[gC.sort];
    const double bw = Measured(page, "cos.sort.t", label, 16) + 52;
    const Rect br{right - bw, cy - 20, bw, 40};
    {
        if (gS.sortOpen) pmui::Level(kDropLevel);
        const double h = EaseTo("cos.sort", pmui::Over(page, br) ? 1.0 : gS.sortOpen ? kOpenLit : 0.0);
        pmui::Box(page, "cos.sort.b", 14, br, kField, 12).Outline(2, Mix(kFieldEdge, kAbEdge, h));
        pmui::Text(page, "cos.sort.t", 15, label, 16, "Regular", Mix(kSub, kWhite, h), br.x + 16, cy);
        pmui::Pic(page, "cos.sort.i", 15, {br.R() - 30, cy - 9, 18, 18}, "pm-chev.png", Mix(kSub, kWhite, h)).Transform(1, gS.sortOpen ? 180 : 0);
        if (pmui::Click(page, br, "cos.sort")) gS.sortOpen = !gS.sortOpen;
        pmui::Level(kPageLevel);
    }
    // the search, between the strip and the sort
    const Rect sr{stripR + 14, cy - 20, std::max(140.0, br.x - 14 - stripR - 14), 40};
    gCosField = pmui::Field(page, "cos.search.field", 16, {sr.x + 46, sr.y + 4, sr.w - 56, sr.h - 8}, "", 16, kWhite);
    const bool focused = Focused(gCosField);
    SearchLook(page, "cos.search", 13, sr, "search cosmetics", 16, Ease("cos.search", focused || !gC.search.empty()), gC.search.empty() && !focused);
    if (gCosField && ((focused && pmui::Now() - gC.searchReadAt > 0.12) || (gC.searchFocused && !focused))) {
        gC.searchReadAt = pmui::Now();
        gC.search = w::ReadText(gCosField);
    }
    if (gCosField && !focused && !gC.searchFocused && pmui::Now() - gC.searchReadAt > 0.5) {
        gC.searchReadAt = pmui::Now();
        if (w::ReadText(gCosField) != gC.search) w::SetText(gCosField, gC.search);
    }
    gC.searchFocused = focused;
    // the list (narrower while the details drawer is out)
    const double drawer = Smooth(Ease("cos.drawer.room", FindCos(gC.open) != nullptr, 0.05)) * kCosDrawer;
    const Rect view{0, pa.y + kSubH, pa.w - drawer, pa.h - kSubH};
    Layer& clip = pmui::Group(page, "cos.list", 20, view, true);
    const double in = gC.glide.from.empty() ? 1.0 : EaseOut((pmui::Now() - gC.glide.at) / 0.2, 3);
    Layer& list = pmui::Group(clip, "cos.list.in", 0, {0, (1 - in) * 10, view.w, view.h}, false, 0.2 + 0.8 * in);
    const std::vector<const Cos*> shown = CosShown();
    const std::string key = "cos." + gC.slot + "." + gC.pack + "." + gPrefs.cosView;
    double top = 6;
    const PackEntry* openPack = gC.pack.empty() ? nullptr : FindPack(gC.pack);
    if (openPack) {
        // a pack's page: back, its pictures, its name, by, how many; get the rest of it (when some can be got)
        const PackEntry& p = *openPack;
        top -= gS.scrolls[key].pos;         // (the header scrolls with the items under it)
        const Rect back{22, top + 12, 40, 40};
        const double bh = Ease("pack.back", pmui::Over(list, back));
        pmui::Box(list, "pack.back.b", 1, back, Mix(kField, kWhite, bh), 12).Outline(2, Mix(kAbEdge, kWhite, bh));
        pmui::Pic(list, "pack.back.i", 2, {back.Cx() - 9, back.Cy() - 9, 18, 18}, "pm-chev.png", Mix(kSub, kBlack, bh)).Transform(1, 90);
        if (pmui::Click(list, back, "pack.back")) {
            gC.glide.from = gC.slot + "/" + gC.pack;
            gC.glide.at = pmui::Now();
            gC.pack.clear();
            gC.open.clear();
        }
        PackPictures(list, "pack.pic", 1, {78, top + 6, 52, 52}, p, 12, 2, 0);
        const double textW = std::max(120.0, view.w - 146 - 34 - 330);   // (left of the pills: get, or update and remove)
        pmui::Text(list, "pack.t", 1, pmui::Fit(PackName(p.name), Measured(list, "pack.t", PackName(p.name), 28), textW), 28, "Regular", kWhite, 146, top + 22);
        const std::string packLine = (p.author.empty() ? std::string("by several") : "by " + p.author) + "  \xC2\xB7  " + PackCount(p) +
                                     (p.installed && !p.version.empty() ? "  \xC2\xB7  " + p.version : "");
        pmui::Text(list, "pack.s", 1, pmui::Fit(packLine, pmui::Width(packLine, 15, "Semilight"), textW), 15, "Semilight", kVer, 146, top + 46);
        // get the pack (its plugin), or remove it
        // (one that needs a newer plugin manager: locked, why on hover)
        const std::string need = NeedsHost(p.name);
        if (!p.installed && !need.empty()) {
            LockedPill(list, "pack.get", view.w - 34, top + 33, "get the pack", 1.45, true, true, false, LockReason(need));
        } else if (!p.installed) {
            const bool busy = Busy(p);
            const std::string getLabel = busy ? "getting..." : "get the pack";
            const double pw = PillWidth(list, "pack.get", getLabel, 17, "pm-down.png", 20);
            if (Pill(list, "pack.get", 2, {view.w - 34 - pw, top + 12, pw, 42}, getLabel, 17, "pm-down.png", PillKind::Lime) && !busy) StartGet(p);
        } else {
            double edge = view.w - 34;
            if (RemovablePack(p)) {
                const double rw = PillWidth(list, "pack.remove", "remove", 16, "pm-trash.png", 18);
                if (Pill(list, "pack.remove", 2, {edge - rw, top + 13, rw, 40}, "remove", 16, "pm-trash.png", PillKind::Ghost, false, 13, &kTrimHover))
                    AskRemove(p.name);
                edge -= rw + 10;
            }
            // its update (the plugins page's update all covers it too)
            if (const Plug* up = PackUpdate(p)) {
                if (!need.empty()) {
                    LockedPill(list, "pack.upd", edge, top + 33, "update " + up->latest, 1.45, true, true, false, LockReason(need));
                } else {
                    const bool busy = up->pending == "installing";
                    const std::string upLabel = busy ? "updating..." : "update " + up->latest;
                    const double uw = PillWidth(list, "pack.upd", upLabel, 17, "pm-down.png", 20);
                    if (Pill(list, "pack.upd", 2, {edge - uw, top + 12, uw, 42}, upLabel, 17, "pm-down.png", PillKind::Lime) && !busy) {
                        InstallUpdate(*up);
                        gCosAt = -100;
                        gModelAt = -100;
                    }
                }
            }
        }
        if (p.pending.rfind("error", 0) == 0)
            pmui::Text(list, "pack.err", 2, p.pending, 14, "Semilight", kRemoveRed, view.w - 34, top + 66, 1);
        top += 72 + gS.scrolls[key].pos;    // (the items' own scroll moves them from here)
    }
    const std::vector<Listed> listed = Arrange(shown);
    if (listed.empty() && openPack && Bare(gC.search).empty()) {
        // a pack with nothing to list: not got yet (what's in it shows once it's installed), or nothing it adds is picked
        double y = top + 20;
        if (!openPack->description.empty()) y += pmui::Para(list, "pack.desc", 1, openPack->description, 18, "Semilight", kDescOpen, {34, y, std::min(900.0, view.w - 68), 120}) + 22;
        const std::string msg = !openPack->installed ? "Its cosmetics show here, and on the Customize page, once it's installed."
                                                     : "Nothing in this pack can be picked on the Customize page.";
        pmui::Text(list, "empty", 1, msg, 16, "Semilight", kVer, 34, y + 10);
    } else if (listed.empty()) {
        const std::string msg = !Bare(gC.search).empty() ? "Nothing matches \"" + gC.search + "\"."
                              : gC.slot == "downloaded" ? "No packs yet: get some from featured."
                              : gC.slot == "published" ? "Nothing published yet."
                              : registry::State().rfind("error", 0) == 0 ? "The list of cosmetics couldn't be loaded (the console says why)."
                              : registry::State() != "ready" ? "The list of cosmetics hasn't loaded yet."
                              : gC.slot == "packs" || gC.slot == "all" ? "No packs yet."
                                                                       : "Nothing here yet.";
        pmui::Text(list, "empty", 1, msg, 18, "Semilight", kSub, view.w / 2, top + 80, 0.5);
        if (gC.slot == "published" && Bare(gC.search).empty())
            pmui::Text(list, "empty.s", 1, "Your own cosmetics, once they're shared as a plugin in the registry, show here.", 16, "Semilight", kVer, view.w / 2, top + 112, 0.5);
    } else if (packRows) {
        // featured / owned: each pack a row; a click opens it in place
        const double rowW = view.w - 22 - 34 + 8;
        std::vector<double> hs;
        double content = top;
        for (const Listed& l : listed) {
            Anim& a = gAnim["pr." + l.pack->name];
            a.open = ux::Approach(a.open, gC.expanded == l.pack->name ? 1.0 : 0.0, pmui::Dt(), 0.07);
            const auto it = gPackExtra.find(l.pack->name);
            hs.push_back(kPackRowH + (it != gPackExtra.end() ? it->second : 160) * Smooth(a.open));
            content += hs.back() + kGap;
        }
        content += 22;
        const double scroll = Scrolled(key, list, {0, 0, view.w, view.h}, content);
        double y = top - scroll;
        for (size_t i = 0; i < listed.size(); ++i) {
            const Rect r{14, y, rowW, hs[i]};
            if (r.B() > -40 && r.y < view.h + 40 && listed[i].pack) PackRow(list, *listed[i].pack, r, kPackRowH);
            y += hs[i] + kGap;
        }
        ScrollBar(list, key, 40, {view.w - 20, 8, 8, view.h - 30}, view.h);      // (8 wide: near the game's own, Lvcid)
    } else if (gPrefs.cosView == "list") {
        const double rowW = view.w - 22 - 34 + 8, rh = kThickH;
        const double content = top + static_cast<double>(listed.size()) * (rh + kGap) + 22;
        const double scroll = Scrolled(key, list, {0, 0, view.w, view.h}, content);
        for (size_t i = 0; i < listed.size(); ++i) {
            const Rect r{14, top + i * (rh + kGap) - scroll, rowW, rh};
            if (r.B() < -40 || r.y > view.h + 40) continue;
            if (listed[i].pack) PackCard(list, *listed[i].pack, r, 2);
            else CosRow(list, *listed[i].c, r);
        }
        ScrollBar(list, key, 40, {view.w - 20, 8, 8, view.h - 30}, view.h);      // (8 wide: near the game's own, Lvcid)
    } else {
        const bool small = gPrefs.cosView == "small";
        const double gridW = view.w - 22 - 34;
        const double target = small ? 168 : 250;
        const int columns = std::max(2, static_cast<int>((gridW + kCardGap) / (target + kCardGap)));
        const double gap = small ? 12 : kCardGap;
        const double cw = (gridW - (columns - 1) * gap) / columns;
        const double ch = small ? cw - 16 + 8 + 34 : cw - 24 + 12 + 108;
        const double content = top + std::ceil(static_cast<double>(listed.size()) / columns) * (ch + gap) + 22;
        const double scroll = Scrolled(key, list, {0, 0, view.w, view.h}, content);
        for (size_t i = 0; i < listed.size(); ++i) {
            const Rect r{22 + (i % columns) * (cw + gap), top + 4 + (i / columns) * (ch + gap) - scroll, cw, ch};
            if (r.B() < -40 || r.y > view.h + 40) continue;
            if (listed[i].pack) PackCard(list, *listed[i].pack, r, small ? 1 : 0);
            else if (small) CosSmall(list, *listed[i].c, r);
            else CosTile(list, *listed[i].c, r);
        }
        ScrollBar(list, key, 40, {view.w - 20, 8, 8, view.h - 30}, view.h);      // (8 wide: near the game's own, Lvcid)
    }
    CosDetails(page, {0, pa.y + kSubH, pa.w, pa.h - kSubH});
    // the sort's list, over the page
    if (gS.sortOpen) {
        pmui::Level(kDropLevel);
        const Rect lr{br.x, br.B() + 6, std::max(br.w, 200.0), 12 + 40.0 * kCosSortCount};
        Glow(page, "cos.sort.shadow", 700, lr, kBlack, 0.8);
        pmui::Box(page, "cos.sort.list", 701, lr, kPopup, 12).Outline(2, kPopupRing);
        for (int i = 0; i < kCosSortCount; ++i) {
            const Rect r{lr.x + 6, lr.y + 6 + 40.0 * i, lr.w - 12, 40};
            const std::string k = "cos.sort." + std::to_string(i);
            const double oh = Ease(k, pmui::Over(page, r));
            pmui::Box(page, k + ".b", 702, r, A(ux::Hex("#24262B"), oh), 9);
            pmui::Text(page, k + ".t", 703, kCosSorts[i], 16, "Regular", gC.sort == i ? kLime : Mix(kRowLabel, kWhite, oh), r.x + 12, r.Cy());
            if (pmui::Click(page, r, k)) {
                gC.sort = i;
                gS.sortOpen = false;
            }
        }
        if (pmui::PressedOutside(page, lr) && pmui::PressedOutside(page, br)) gS.sortOpen = false;
        pmui::Level(kPageLevel);
    }
}

// The get / remove popup, of a whole pack (its plugin): its pictures, "show the pack".
void CosPopup(Layer& wl, double W, double H) {
    const PackEntry* pp = FindPack(gS.popupId);
    const bool remove = gS.popup == "cos.remove";
    if (!pp || (remove ? !RemovablePack(*pp) : pp->installed)) {
        ClosePopup();
        return;
    }
    const PackEntry& p = *pp;
    pmui::Level(kPopupLevel);
    const double t = PopupIn();
    pmui::PreferFocus(remove ? "no" : "yes");     // (a removal starts on cancel)
    const std::string name = PackName(p.name);
    const std::string text = remove ? "Its plugin is removed, and its cosmetics with it (gone from the Customize page after the next start)."
                                    : "Its plugin is installed (in the plugins folder's cosmetics folder), and its cosmetics show on the Customize page.";
    const bool pictures = !p.items.empty();
    const double bodyH = 26 + 52 + 16 + 26 + 18 + 24 + (pictures ? 28 + 64 + 18 : 0) + 18 + 48 + 24;     // (24: its words' second line)
    const Rect b{std::floor(W / 2 - 320), std::floor(H / 2 - bodyH / 2), 640, bodyH};
    {
        ux::RawColors raw;                      // (a dark veil in every theme)
        pmui::Box(wl, "pop.dim", 900, {0, 0, W, H}, A(kBlack, kFocusDim * t), 24);
    }
    Glow(wl, "pop.shadow", 901, b, kBlack, 0.8 * t);
    Layer& pop = pmui::Group(wl, "pop", 902, b, false, t);
    pmui::Box(pop, "ring", 0, {-2, -2, b.w + 4, b.h + 4}, kPopupRing, 24);
    pmui::Box(pop, "bg", 1, {0, 0, b.w, b.h}, kPopup, 22).Outline(2, kBlack);
    PackPictures(pop, "pic", 2, {28, 26, 52, 52}, p, 13, 2, 0);
    const std::string title = (remove ? "remove " : "get ") + name + (remove ? "?" : "");
    pmui::Text(pop, "title", 2, pmui::Fit(title, Measured(pop, "title.m", title, 28), b.w - 96 - 28), 28, "Regular", kWhite, 96, 52);
    pmui::Para(pop, "text", 2, text, 16.5, "Semilight", kPopupText, {28, 26 + 52 + 16, b.w - 56, 60});
    double y = 26 + 52 + 16 + 26 + 18 + 24;
    if (pictures) {
        pmui::Text(pop, "pk.t", 2, "THE " + name + " PACK  \xC2\xB7  " + PackCount(p), 13, "Bold", kHead, 28, y + 8);
        y += 28;
        double px = 28;
        for (const Cos* o : p.items) {
            if (px + 56 > b.w - 28) break;
            CosPicture(pop, "pk." + o->key, 2, {px, y, 56, 56}, *o, 11, 2, 0);
            px += 64;
        }
        y += 64 + 18;
    }
    y += 18;
    const std::string yes = remove ? "remove" : "get";
    const double yw = PillWidth(pop, "yes", yes, 18, remove ? nullptr : "pm-down.png", 24);
    const double nw = PillWidth(pop, "no", "cancel", 18, nullptr, 24);
    const bool confirm = Pill(pop, "yes", 3, {b.w - 28 - yw, y, yw, 48}, yes, 18, remove ? nullptr : "pm-down.png", PillKind::Lime, false, 13);
    const bool cancel = Pill(pop, "no", 3, {b.w - 28 - yw - 12 - nw, y, nw, 48}, "cancel", 18, nullptr, PillKind::Ghost, false, 13);
    bool goPack = false;
    if (gC.pack != p.name) {
        const double gw = PillWidth(pop, "gopack", "show the pack", 16, "pm-pack.png", 18);
        goPack = Pill(pop, "gopack", 3, {28, y + 2, gw, 44}, "show the pack", 16, "pm-pack.png", PillKind::Ghost, false, 13, &kTrimHover);
    }
    if (confirm) {
        if (remove) {
            registry::Remove(p.name);
            hostlog::Info("manager: removing the " + p.name + " pack");
            if (const Cos* open = FindCos(gC.open); open && open->pack == p.name) gC.open.clear();
            gModelAt = -100;
        } else {
            StartGet(p);
        }
        gCosAt = -100;
        ClosePopup();
    } else if (goPack) {
        OpenPack(p.name);
        ClosePopup();
    } else if (cancel || pmui::PressedOutside(pop, {0, 0, b.w, b.h})) {
        ClosePopup();
    }
}

// --- the cosmetics page, as the plugins page (Lvcid, 2026-10-10) ------------------------------------------------------
// The plugins page's own rows for the cosmetic packs (they are plugins): owned / explore for installed / browse, its
// views (compact and big), its search, its open card (the description and facts, then the pack's cosmetics as little
// cards). Where a plugin's cog would be, the pack's key, flush with the card's left edge (they have no settings).
// Getting is the whole plugin, always (AnythingGoes). Its own tab, searches, open row, slide and view: swapped in round
// the plugins page's while it draws. DEBUG can show the earlier page (sidebar, grids) instead.
bool gTestOldCos = false;

std::string PackSymbol(const std::string& id) {
    const PackEntry* p = FindPack(id);
    if (!p || p->items.empty()) return "pm-pack.png";
    const std::string slot = p->items[0]->slot;
    for (const Cos* c : p->items)
        if (c->slot != slot) return "pm-pack.png";
    const bool known = slot == "ball" || slot == "hat" || slot == "bfx" || slot == "arms" || slot == "bounce";
    return "pm-slot-" + (known ? slot : std::string("extra")) + ".png";
}

// What's in a pack, under its open card's facts: each cosmetic a little card (its picture, its name under it), from x0;
// returns where they end.
double PackItems(Layer& row, const std::string& u, const std::string& id, double x0, double y, double right) {
    const PackEntry* p = FindPack(id);
    if (!p) return y;
    if (p->items.empty()) {
        plugins::Info info;
        const bool waiting = p->installed && plugins::Find(id, &info) && info.status.rfind("needs ", 0) == 0;
        std::string why = "It adds nothing that can be picked on the Customize page.";
        if (waiting) {
            // (it can't run without another plugin: Example Arms without Cosmetic Kit Plus, say)
            const std::string need = info.status.substr(6);
            const Plug* q = FindPlug(need);
            why = "It needs " + (q ? q->name : need) + " to run; its cosmetics show here then.";
        }
        pmui::Text(row, u + "items.none", 4, !p->installed ? "Its cosmetics show here once it's installed." : why,
                   15, "Semilight", kSub, x0, y + 12);
        return y + 30;
    }
    constexpr double kItem = 68, kItemGap = 14, kNameH = 30;
    double x = x0;
    for (const Cos* c : p->items) {
        if (x + kItem > right && x > x0) {
            x = x0;
            y += kItem + kNameH;
        }
        const Rect tr{x, y, kItem, kItem};
        const std::string k = u + "it." + c->key;
        const double th = Ease(k, pmui::Over(row, tr));
        CosPicture(row, k, 5, tr.Moved(0, -3 * th), *c, 14, 2, th);
        pmui::Text(row, k + ".n", 5, pmui::Fit(c->name, Measured(row, k + ".n", c->name, 12.5), kItem + kItemGap - 4), 12.5, "Regular",
                   Mix(kSub, kWhite, th), tr.Cx(), tr.B() + 13, 0.5);
        x += kItem + kItemGap;
    }
    return y + kItem + kNameH;
}

struct RowsState {
    std::string tab = "installed", installedSearch, browseSearch, expanded, scrollTo, swapFrom, view;     // ("": read from the prefs)
    double installedReadAt = -100, browseReadAt = -100, swapAt = -100;
    bool installedFocused = false, browseFocused = false;
    Rect slideFrom, slideNow;
    Obj installedField = nullptr, browseField = nullptr;
};
RowsState gCosRowsState;

void SwapRows(RowsState& o) {
    std::swap(gS.tab, o.tab);
    std::swap(gS.installedSearch, o.installedSearch);
    std::swap(gS.browseSearch, o.browseSearch);
    std::swap(gS.expanded, o.expanded);
    std::swap(gS.scrollTo, o.scrollTo);
    std::swap(gS.swapFrom, o.swapFrom);
    std::swap(gS.installedReadAt, o.installedReadAt);
    std::swap(gS.browseReadAt, o.browseReadAt);
    std::swap(gS.swapAt, o.swapAt);
    std::swap(gS.installedFocused, o.installedFocused);
    std::swap(gS.browseFocused, o.browseFocused);
    std::swap(gSlideFrom, o.slideFrom);
    std::swap(gSlideNow, o.slideNow);
    std::swap(gInstalledField, o.installedField);
    std::swap(gBrowseField, o.browseField);
    std::swap(gPrefs.view, o.view);
}

void CosmeticsPage(Layer& wl, const Rect& area) {
    if (gTestOldCos) return CosmeticsPageOld(wl, area);
    pmui::Level(kPageLevel);
    if (!packages::KitOn()) {
        KitMissing(wl, area);
        return;
    }
    if (pmui::Now() - gCosAt > 0.25) BuildCos();
    // its view: compact (the small list, by default: Lvcid) or big, kept as the cosmetics page's own (prefs "cosview")
    RowsState& st = gCosRowsState;
    if (st.view.empty()) st.view = gPrefs.cosView == "thick" ? "thick" : "compact";
    SwapRows(st);
    gCosRows = true;
    Layer& cl = pmui::Group(wl, "cosrows", 5, {0, 0, area.w, area.y + area.h}, false);
    PluginsPage(cl, area);
    gCosRows = false;
    SwapRows(st);
    if (st.view != gPrefs.cosView) {
        gPrefs.cosView = st.view;
        PrefsChanged();
    }
}

