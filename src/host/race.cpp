#include "race.hpp"

#include "engine.hpp"
#include "game.hpp"
#include "log.hpp"

using eng::Obj;

namespace race {
namespace {

bool gOnTrack = false, gActive = false;
int gRestarts = 0;
eng::Weak gBall;                // the ball whose counter was read last
int32_t gLastCounter = 0;

// The ball's restart counter, or false if this pawn is not a ball (a replay camera, the menu ball, ...).
bool ReadCounter(Obj pawn, int32_t* counter) {
    return pawn && eng::FindProp(eng::ClassOf(pawn), "RestartCounterThisSession") &&
           eng::ReadBytes(pawn, "RestartCounterThisSession", counter, sizeof *counter);
}

}  // namespace

void Frame() {
    Obj controller = game::PlayerController();
    gOnTrack = controller && eng::FindProp(eng::ClassOf(controller), "bRaceActive");
    gActive = false;
    if (!gOnTrack) return;
    eng::ReadBool(controller, "bRaceActive", &gActive);

    Obj pawn = eng::Call(controller, "K2_GetPawn").ReturnObj();
    int32_t counter = 0;
    if (!ReadCounter(pawn, &counter)) return;
    if (eng::Get(gBall) == pawn && counter > gLastCounter) {
        gRestarts += counter - gLastCounter;
        hostlog::Info("race restarted from the beginning (" + std::to_string(gRestarts) + " this session)");
    }
    gBall = eng::MakeWeak(pawn);
    gLastCounter = counter;
}

bool OnTrack() { return gOnTrack; }
bool Active() { return gActive; }
int Restarts() { return gRestarts; }

}  // namespace race
