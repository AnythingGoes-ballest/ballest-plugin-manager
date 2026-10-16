// Offline test of the model text (models.hpp): material statements ("material <name> game", "set", "animate"), and a
// character's rig (moves, keys, lean, drops).
// No game: only models::Parse runs. Built and run by tools/tests/run.sh.
#include <cmath>
#include <cstdio>
#include <string>

#include "../../src/host/models.hpp"

namespace {
int failures = 0;

void Check(bool ok, const std::string& what) {
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

bool Near(float a, float b) { return std::fabs(a - b) < 1e-3f; }

// Parses `text`, expecting success or failure; returns the model.
models::Model Parsed(const std::string& text, bool expectOk, const std::string& what) {
    models::Model m;
    std::string error;
    const bool ok = models::Parse(text, &m, &error);
    Check(ok == expectOk, what + (ok ? " (parsed)" : " (" + error + ")"));
    return m;
}
}  // namespace

int main() {
    const std::string sphere = "sphere core r=40\n";
    {
        const models::Model m = Parsed(
            "material core game /Game/Art/Some/M_Thing\n"
            "set core \"Stars Color 01\" #ff0000*4\n"
            "set core \"Roughness\" 0.25\n"
            "set core \"Tint\" 0.5,0.25,1\n"
            "set core \"Panorama\" /Game/Textures/T_Cube.T_Cube\n"
            "animate core \"EmissiveColor\" #000000 #ffffff*2 period=2 wave=saw phase=90 run=0.5\n"
            "animate core \"Glow\" 1 3\n" + sphere,
            true, "game material with settings and animations");
        Check(m.materials.size() == 1 && m.materials[0].finish == models::Finish::Game, "game finish");
        if (!m.materials.empty()) {
            const models::Material& c = m.materials[0];
            Check(c.asset == L"/Game/Art/Some/M_Thing.M_Thing", "asset path gets its object name");
            Check(c.settings.size() == 4, "four settings");
            if (c.settings.size() == 4) {
                Check(c.settings[0].parameter == "Stars Color 01" && c.settings[0].kind == models::Setting::Kind::Colour &&
                          Near(c.settings[0].value[0], 4) && Near(c.settings[0].value[1], 0),
                      "quoted name, #rrggbb*k colour");
                Check(c.settings[1].kind == models::Setting::Kind::Scalar && Near(c.settings[1].value[0], 0.25f), "number");
                Check(c.settings[2].kind == models::Setting::Kind::Colour && Near(c.settings[2].value[1], 0.25f) &&
                          Near(c.settings[2].value[3], 1),
                      "linear r,g,b colour");
                Check(c.settings[3].kind == models::Setting::Kind::Texture && c.settings[3].asset == L"/Game/Textures/T_Cube.T_Cube",
                      "texture asset");
            }
            Check(c.motions.size() == 2, "two animations");
            if (c.motions.size() == 2) {
                const models::Motion& a = c.motions[0];
                Check(a.colour && a.wave == models::Motion::Wave::Saw && Near(static_cast<float>(a.period), 2) &&
                          Near(static_cast<float>(a.phase), 90) && Near(static_cast<float>(a.run), 0.5f) && Near(a.to[0], 2),
                      "colour animation options");
                const models::Motion& b = c.motions[1];
                Check(!b.colour && b.wave == models::Motion::Wave::Sine && Near(static_cast<float>(b.period), 4) && Near(b.to[0], 3),
                      "number animation defaults");
            }
        }
    }
    {
        // "set" works on the plain kinds too
        const models::Model m = Parsed("material red plastic #ff0000\nset red \"Roughness\" 0.1\nsphere red r=10\n", true, "set on plastic");
        Check(m.materials.size() == 1 && m.materials[0].settings.size() == 1, "plastic keeps its setting");
    }
    {
        const models::Model m = Parsed("material core game /Game/M\nrace core \"EmissiveColor\" 1.6\n" + sphere, true, "race factor");
        Check(!m.materials.empty() && Near(m.materials[0].RaceFactor("EmissiveColor"), 1.6f) && Near(m.materials[0].RaceFactor("X"), 1),
              "race factor kept per parameter");
    }
    Parsed("material core game /Game/M\nrace core \"X\" -1\n" + sphere, false, "race factor from 0");
    {   // "asset": a material from the plugin's own cooked content, only under /Game/Mods/
        const models::Model m = Parsed("material core asset /Game/Mods/my_mod/MI_Shell.MI_Shell\nset core \"Tint\" #00ffff\n" + sphere,
                                       true, "asset material");
        Check(m.materials.size() == 1 && m.materials[0].finish == models::Finish::Game &&
                  m.materials[0].asset == L"/Game/Mods/my_mod/MI_Shell.MI_Shell" && m.materials[0].settings.size() == 1,
              "asset material is a game-finish material on its /Game/Mods path");
    }
    Parsed("material core asset /Game/Art/Materials/M_Thing\n" + sphere, false, "asset outside /Game/Mods refused");
    Parsed("material core asset\n" + sphere, false, "asset without a path refused");
    {
        const models::Model m = Parsed("material core game /Game/M\ngroup halo facing\n" + sphere, true, "facing group");
        Check(!m.groups.empty() && m.groups.back().facing, "facing kept");
    }
    Parsed("material core game /Game/M\ngroup halo facing spin=z speed=10\n" + sphere, false, "facing can't spin");
    Parsed("material core game NotAPath\n" + sphere, false, "game material needs an asset path");
    Parsed("material core game /Game/M\nset nothing \"X\" 1\n" + sphere, false, "set needs a defined material");
    Parsed("material core game /Game/M\nset core X\n" + sphere, false, "set needs a value");
    Parsed("material core game /Game/M\nset core \"X\" banana\n" + sphere, false, "set rejects a value of no kind");
    Parsed("material core game /Game/M\nset core \"X\" missing.png\n" + sphere, false, "set needs the picture to exist");
    Parsed("material core game /Game/M\nset core \"X\" #12345\n" + sphere, false, "set rejects a short colour");
    Parsed("material core game /Game/M\nanimate core \"X\" 1 #ffffff\n" + sphere, false, "animate needs matching kinds");
    Parsed("material core game /Game/M\nanimate core \"X\" 1 2 period=0\n" + sphere, false, "animate needs a period above 0");
    Parsed("material core game /Game/M\nanimate core \"X\" 1 2 wave=bounce\n" + sphere, false, "animate rejects an unknown wave");
    Parsed("material core game /Game/M\nanimate core \"X\" 1 2 speed=3\n" + sphere, false, "animate rejects an unknown option");
    Parsed("material core game /Game/M\nset core \"X 1\n" + sphere, false, "set rejects an unclosed quote");
    // Moves, lean and drops (a character's rig): keys name groups anywhere in the text, sorted by group then time.
    {
        const std::string rig =
            "material skin plastic #ffcc99\n"
            "tempo rate=0.6 run=0.25 max=3 calm=0.2 full=8\n"
            "group hero travel\n"
            "group lean on=hero pivot=0,0,-50 lean=10 bob=3\n"
            "sphere skin r=50\n"
            "group arm on=lean pivot=0,30,0 swing=y angle=40\n"
            "sphere skin r=8 at=0,58,-10\n"
            "group egg on=lean pivot=-58,0,-30 drop=land push=-160,0,380 life=4 max=5 carry=0.85\n"
            "sphere skin r=11 at=-58,0,-30\n"
            "moves gap=0.4,1.6\n"
            "move wave when=run weight=2 blend=0.2\n"
            "key 1.3 arm\n"
            "key 0 arm swing=0\n"
            "key 0.5 arm rot=10,0,-120 at=0,0,6 swing=0\n"
            "key 0.4 lean at=0,0,12\n"
            "move spin when=idle blend=0 length=2\n"
            "key 0 lean\n"
            "key 1 lean rot=0,360,0\n";
        const models::Model m = Parsed(rig, true, "a rig with moves");
        Check(m.moves.size() == 2 && Near(static_cast<float>(m.gap[0]), 0.4f) && Near(static_cast<float>(m.gap[1]), 1.6f), "two moves, gap");
        if (m.moves.size() == 2) {
            const models::Move& w = m.moves[0];
            Check(w.when == models::When::Run && Near(static_cast<float>(w.weight), 2) && Near(static_cast<float>(w.length), 1.3f) &&
                      Near(static_cast<float>(w.blend), 0.2f), "wave: when, weight, length from the last key, blend");
            Check(w.keys.size() == 4 && w.keys[0].group == 1 && w.keys[1].group == 2 && w.keys[1].time == 0 && w.keys[3].time > 1,
                  "keys sorted by group (lean before arm), then time");
            if (w.keys.size() == 4) Check(Near(static_cast<float>(w.keys[2].rot[2]), -120) && w.keys[2].swing == 0, "key rot and swing");
            Check(Near(static_cast<float>(m.moves[1].length), 2), "length= past the last key");
        }
        Check(m.groups.size() >= 4 && m.groups[1].lean == 10 && m.groups[1].keyed && m.groups[2].keyed, "lean, keyed groups");
        if (m.groups.size() >= 4) {
            const models::Group& e = m.groups[3];
            Check(e.drop == static_cast<int>(models::When::Land) && e.max == 5 && Near(static_cast<float>(e.carry), 0.85f) &&
                      Near(static_cast<float>(e.push[2]), 380) && e.Moves(), "drop group");
        }
        const std::string skin = "material skin plastic #ffffff\n";
        Parsed(skin + "group a pivot=1,0,0\nsphere skin r=1\nmove m\nkey 0 nobody\n", false, "key: unknown group");
        Parsed(skin + "group a travel\nsphere skin r=1\nmove m\nkey 1 a rot=0,10,0\n", false, "key on a travelling group");
        Parsed(skin + "group a travel lean=5\nsphere skin r=1\n", false, "lean on a travelling group");
        Parsed(skin + "group a pivot=1,0,0 drop=land max=9\nsphere skin r=1\n", false, "drop: max above 8");
        Parsed(skin + "group a pivot=1,0,0\nsphere skin r=1\nkey 0 a\n", false, "key before any move");
        Parsed(skin + "group a pivot=1,0,0\nsphere skin r=1\nmove m when=fly\nkey 1 a\n", false, "move: bad when");
    }
    std::printf("models_test: %s (%d failure(s))\n", failures ? "FAILED" : "ok", failures);
    return failures ? 1 : 0;
}
