#include "leaderboard.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

#include "engine.hpp"
#include "game.hpp"
#include "log.hpp"
#include "widgets.hpp"

namespace leaderboard {
namespace {

using eng::Obj;
namespace w = ui::widgets;

std::string gNote;
int gNoteOwner = -1;

struct Titled {
    eng::Weak title;                            // the TXT_HEader text block
    std::string original, shown;
};
std::vector<Titled> gTitles;

// The in-map leaderboard: a WBP_Leaderboard_C inside the race UI (menus have others, for the track picker).
Obj InMapBoard() {
    static double lastLook = -100;
    static eng::Weak cached;
    if (Obj board = eng::Get(cached)) return board;
    if (game::Seconds() - lastLook < 1) return nullptr;
    lastLook = game::Seconds();
    Obj cls = eng::FindClass("WBP_Leaderboard_C"), race = eng::FindClass("WBP_RaceUIManager_C");
    if (!cls || !race) return nullptr;
    Obj found = nullptr;
    eng::ForEachObject([&](Obj o) {
        if (eng::ClassOf(o) != cls || eng::IsDefaultObject(o)) return true;
        for (Obj outer = eng::OuterOf(o); outer; outer = eng::OuterOf(outer))
            if (eng::ClassOf(outer) == race && !eng::IsDefaultObject(outer)) {
                found = o;
                return false;
            }
        return true;
    });
    cached = eng::MakeWeak(found);
    return found;
}

}  // namespace

int Players() {
    Obj board = InMapBoard();
    if (!board || !eng::Call(board, "IsVisible").ReturnBool()) return -1;
    eng::Prop handleProp;
    const int offset = eng::NestedOffset(eng::ClassOf(board), {"NativeActiveLeaderboardRecord", "LeaderboardHandle"}, &handleProp);
    if (offset < 0 || handleProp.size != 8) return -1;
    uint64_t handle = 0;
    std::memcpy(&handle, board + offset, sizeof handle);
    if (!handle) return -1;
    Obj steam = eng::FindCdo("SIK_UserStatsLibrary");
    if (!steam) return -1;
    // Measured: the function takes the handle as a 4-byte LeaderboardID (the game's handles fit: 0x013BAB71).
    eng::Params p(eng::FunctionOn(steam, "GetLeaderboardEntryCount"));
    const int32_t idSize = p.SizeOf("LeaderboardID");
    if ((idSize != 4 && idSize != 8) || (idSize == 4 && handle > 0xFFFFFFFFull) || !p.SetArg(0, &handle, static_cast<size_t>(idSize))) return -1;
    eng::Invoke(steam, p);
    size_t size = 0;
    const uint8_t* r = p.Return(&size);
    if (!r || size < 4) return -1;
    int32_t count = 0;
    std::memcpy(&count, r, 4);
    return count > 0 ? count : -1;
}

void SetTitleNote(int owner, const std::string& note) {
    gNote = note;
    gNoteOwner = note.empty() ? -1 : owner;
}

void RemoveOwner(int owner) {
    if (owner == gNoteOwner) SetTitleNote(owner, "");
}

void Frame() {
    static double lastLook = -100;
    if (game::Seconds() - lastLook < 0.25) return;
    lastLook = game::Seconds();
    Obj board = gNote.empty() && gTitles.empty() ? nullptr : InMapBoard();
    Obj title = board ? eng::ReadObj(board, "TXT_HEader") : nullptr;
    // A title seen for the first time is remembered as the game wrote it, to put the note after (and back without).
    if (title && std::none_of(gTitles.begin(), gTitles.end(), [&](const Titled& t) { return eng::Get(t.title) == title; }))
        gTitles.push_back({eng::MakeWeak(title), w::ReadText(title), ""});
    for (auto it = gTitles.begin(); it != gTitles.end();) {
        Obj text = eng::Get(it->title);
        if (!text) {
            it = gTitles.erase(it);
            continue;
        }
        const std::string wanted = gNote.empty() ? it->original : it->original + "  " + gNote;
        if (wanted != it->shown) {
            w::SetText(text, wanted);
            it->shown = wanted;
        }
        ++it;
    }
}

}  // namespace leaderboard
