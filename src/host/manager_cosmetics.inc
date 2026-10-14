// --- the cosmetics page ----------------------------------------------------------------------------------------------------
// Included into manager.cpp's anonymous namespace (after the plugins page): it draws with the window's own pieces.
//
// For finding cosmetics, not wearing them (that's the Customize page). Shared cosmetics are plugins: each its own repo,
// listed in the registry (registry::IsCosmetic) and installed like any plugin, into plugins\cosmetics\<id>. **A plugin
// is a pack**: getting or removing is the whole plugin, and what is in it is known once it runs (the cosmetics it adds
// through the Cosmetics API, noted by packages.cpp). The page is the plugins page's twin (CosmeticsPage, below): its
// rows are the cosmetic plugins, an open card lists the pack's cosmetics (PackItems).

// A cosmetic an installed pack added (packages::Items), as the page shows it.
struct Cos {
    std::string key, id, name, slot, pack;
    long day = 0;                           // the day its pack joined the registry (days since 1970), for newest
    std::wstring preview;                   // its picture ("" for the game's picture of it, else its slot's)
    Color tint{1, 1, 1, 1};
};
std::vector<Cos> gCos;
double gCosAt = -100;

// A pack: a cosmetic plugin, from the registry and (once installed) what it added; its cosmetics newest first.
struct PackEntry {
    std::string name;                       // its plugin's id
    std::string title;
    std::vector<const Cos*> items;
    bool installed = false;
};
std::vector<PackEntry> gPacks;

std::string SlotPicture(const std::string& slot) {
    const bool known = slot == "ball" || slot == "hat" || slot == "bfx" || slot == "arms" || slot == "bounce";
    return known ? "slot-" + slot + ".png" : "slot-extra.png";
}

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

// The packs: every cosmetic plugin the registry lists, and any other installed plugin that added cosmetics; each with
// its cosmetics.
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
            if (p.installed) p.title = info.name;
            if (const registry::Entry* e = EntryOf(id); e && !e->name.empty()) p.title = e->name;
            gPacks.push_back(std::move(p));
        }
        return gPacks[at[id]];
    };
    for (const registry::Entry& e : registry::Entries())
        if (registry::IsCosmetic(e.id, e.dependencies)) add(e.id);
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
        c.id = it.id, c.name = it.name.empty() ? it.id : it.name, c.slot = it.category;
        c.pack = it.pack.empty() ? it.plugin : it.pack;
        c.preview = it.preview;
        if (const registry::Entry* e = EntryOf(c.pack)) c.day = e->added.empty() ? 0 : DayOf(e->added);
        gCos.push_back(std::move(c));
    }
    BuildPacks();
}

// A cosmetic's picture on its tile (the plugins' tile: dark, a brighter ring).
void CosPicture(Layer& l, const std::string& key, int z, const Rect& r, const Cos& c, double radius, double ring, double hover) {
    pmui::Box(l, key + ".b", z, r, kTile, radius).Outline(static_cast<float>(ring), Mix(ux::Hex("#383B41"), kWhite, 0.85 * hover));
    Obj texture = c.preview.empty() ? nullptr : ux::LoadPicture(c.preview);
    if (!texture) texture = cosmetics::PreviewOf(c.slot, c.id);
    const Rect in = r.Grow(-ring);
    if (texture) pmui::Texture(l, key + ".i", z + 1, in, texture, c.tint, std::max(0.0, radius - ring));
    else pmui::Pic(l, key + ".s", z + 1, in.Grow(-in.w * 0.14), SlotPicture(c.slot).c_str(), A(Mix(kWhite, c.tint, 0.5), 0.7));
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

// A settings nav tab's hover (Lvcid): the plugin rows' lit edge, all lime, on the tab's left: its trim lit from the
// middle out, sparks along the edge, and round the tab's pill (never over it) a glow, smoke and particles drifting left,
// out of it. Timed as Accent. `radius`: the tab's corners (a pill's: half its height); `fx`: a layer over everything
// (gFx) where the glow, smoke and particles go, unclipped, hanging out past the tab and the window.
void SideAccent(Layer& sb, const std::string& k, const Rect& r, bool over, double radius, Layer& fx) {
    Anim& lamp = gAnim[k + ".acc.on"];
    const double before = lamp.value;
    const double on = EaseUpDown(k + ".acc.on", over, 0.09, 0.2), lit = Smooth(on);
    const double flare = on > before ? std::sin(3.14159265 * on) : 0;
    // (as Accent's: revealed with the pages' curve and speed; unhovered, it fades out where it is, not shrinking back)
    const double grow = HoverCurve(k + ".acc.len", over || lit > 0.01, kHoverIn, kHoverOut);
    if (lit < 0.01) return;
    const double now = pmui::Now(), breathe = 0.5 + 0.5 * std::sin(now * 1.7);
    Layer& in = pmui::Group(sb, k + ".acc", 4, r, true);
    const double cy = r.h / 2, len = std::max(4.0, r.h * grow);
    constexpr int kBands = 8;
    for (int i = 0; i < kBands; ++i) {
        const double half = len / 2 * (i + 1) / kBands;
        Layer& band = pmui::Group(in, "b" + std::to_string(i), 0, {0, cy - half, radius + 2, 2 * half}, true);
        const double oy = cy - half;
        // (the trim alone, an outline: nothing drawn over what's inside the tab)
        pmui::Box(band, "lime", 0, {0, -oy, r.w, r.h}, A(kWhite, 0), radius).Outline(2, A(Mix(kLime, AccentPale(), 0.5 * flare), 0.24 * lit));
    }
    // the loose effects, on fx round the tab's pill: the smoke and the particles drifting left, out of it (Lvcid)
    const double frames = std::fmod(now / 7.3, 1.0) * 32;
    const int f0 = static_cast<int>(frames) % 32;
    const double frac = frames - std::floor(frames);
    auto loose = [&](Layer& g, double dx, double dy) {
        pmui::Pic(g, "glow", 1, {dx - 80, dy + cy - r.h * 0.9, 160, r.h * 1.8}, "fx-glow.png", A(kLime, (0.3 + 0.15 * breathe) * lit + 0.25 * flare));
        char name[32];
        for (int n = 0; n < 2; ++n) {
            std::snprintf(name, sizeof name, "fx-smoke%02d.png", (f0 + n) % 32);
            pmui::Pic(g, "smoke" + std::to_string(n), 1, {dx - 150, dy + r.h * 0.06, 150, r.h * 0.88}, name,
                      A(kLime, 0.32 * lit * (n == 0 ? 1 - frac : frac)));
        }
        for (int i = 0; i < 5; ++i) {
            const double period = 1.4 + 0.11 * ((i * 37) % 7);
            const double t = now / period + i * 0.2, local = t - std::floor(t);
            const unsigned seed = static_cast<unsigned>(std::floor(t)) * 2654435761u + static_cast<unsigned>(i) * 40503u;
            const double r1 = (seed % 1000) / 1000.0, r2 = ((seed / 1000) % 1000) / 1000.0;
            const double drift = (30 + 60 * r2) * local;
            const double px = -3 - drift, py = cy - len / 2 + 5 + (len - 10) * r1 - 6 * local;
            const double size = 8 * (1 - 0.3 * local) * (0.8 + 0.4 * r2);
            pmui::Pic(g, "p" + std::to_string(i), 2, {dx + px - size / 2, dy + py - size / 2, size, size}, "fx-puff.png",
                      A(Mix(kLime, kWhite, 0.35), std::sin(3.14159265 * local) * 0.5 * lit));
        }
    };
    const Rect s = pmui::OnScreen(sb, r);
    const Rect hole{s.x - fx.ox, s.y - fx.oy, s.w, s.h};
    AroundPill(fx, k + ".acc.fx", 0, {hole.x - 100, hole.y - 0.45 * hole.h, hole.w + 100, 1.9 * hole.h}, hole, radius, loose);
    for (int i = 0; i < 3; ++i) {
        const double period = 0.6 + 0.15 * i, t = now / period + i * 0.37, local01 = t - std::floor(t);
        const unsigned seed = static_cast<unsigned>(std::floor(t)) * 2246822519u + static_cast<unsigned>(i) * 3266489917u;
        const double sy = cy - len / 2 + 6 + (len - 12) * ((seed % 1000) / 1000.0), size = 8;
        const double tw = std::sin(3.14159265 * local01);
        const double ex = CornerInset(radius, std::min(sy, r.h - sy));      // (on the pill's curve)
        pmui::Pic(in, "spark" + std::to_string(i), 2, {ex + 1.5 - size / 2, sy - size / 2, size, size}, "fx-spark.png", A(Mix(kWhite, kLime, 0.3), 0.85 * tw * tw * lit));
    }
}

// --- the page ------------------------------------------------------------------------------------------------------------
// The plugins page's own rows for the cosmetic packs (they are plugins): owned / explore for installed / browse, its
// views (compact and big), its search, its open card (the description and facts, then the pack's cosmetics as little
// cards). Where a plugin's cog would be, the pack's symbol (they have no settings). Getting is the whole plugin, always.
// Its own tab, searches, open row, slide and view: swapped in round the plugins page's while it draws.

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

// (the tab, installed / browse = owned / explore, is shared by both pages (Lvcid): picking explore on the plugins page
// and going to cosmetics shows explore, not one click more; so is the strip's sliding box with it)
struct RowsState {
    std::string installedSearch, expanded, scrollTo, view;     // (view "": read from the prefs)
    double installedReadAt = -100;
    bool installedFocused = false;
    FieldRef installedField;
};
RowsState gCosRowsState;

void SwapRows(RowsState& o) {
    std::swap(gS.installedSearch, o.installedSearch);
    std::swap(gS.expanded, o.expanded);
    std::swap(gS.scrollTo, o.scrollTo);
    std::swap(gS.installedReadAt, o.installedReadAt);
    std::swap(gS.installedFocused, o.installedFocused);
    std::swap(gInstalledField, o.installedField);
    std::swap(gPrefs.view, o.view);
}

void CosmeticsPage(Layer& wl, const Rect& area) {
    pmui::Level(kPageLevel);
    if (!packages::KitOn()) {
        KitMissing(wl, area);
        return;
    }
    if (pmui::Now() - gCosAt > 0.25) BuildCos();
    // its view: compact (the small list, by default: Lvcid) or big, kept as the cosmetics page's own (prefs "cosview")
    RowsState& st = gCosRowsState;
    if (st.view.empty()) st.view = gPrefs.cosView;
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
