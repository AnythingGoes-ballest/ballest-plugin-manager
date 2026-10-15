// Offline test of the model text's material statements (models.hpp): "material <name> game", "set" and "animate".
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
    std::printf("models_test: %s (%d failure(s))\n", failures ? "FAILED" : "ok", failures);
    return failures ? 1 : 0;
}
