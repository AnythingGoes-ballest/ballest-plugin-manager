#include "game.hpp"

#include <windows.h>

#include <set>

#include "log.hpp"

using eng::Obj;

namespace game {
namespace {

eng::Weak gViewport, gController;
int gGeneration = 0;
std::set<int> gCursorOwners;                // plugins currently asking for the cursor
bool gCursorTaken = false, gCursorWasShown = false, gInputModeChanged = false;
eng::Weak gTypingWidget;
bool gTypingMode = false;

void UpdateCursor(Obj controller) {
    Obj widgets = eng::FindCdo("WidgetBlueprintLibrary");
    if (!widgets) return;
    if (!gCursorOwners.empty()) {
        bool shown = false;
        eng::ReadBool(controller, "bShowMouseCursor", &shown);
        if (!gCursorTaken) {
            gCursorTaken = true;
            gCursorWasShown = shown;
        }
        if (!shown) {
            eng::WriteBool(controller, "bShowMouseCursor", true);
            // Only the controller is passed; the other parameters stay zero: no focus widget, no mouse lock,
            // cursor kept while a button is held.
            eng::Call(widgets, "SetInputMode_GameAndUIEx", controller);
            gInputModeChanged = true;
        }
    } else if (gCursorTaken) {
        gCursorTaken = false;
        eng::WriteBool(controller, "bShowMouseCursor", gCursorWasShown);
        // Only undo an input mode the host set: the main menu, where the cursor is already shown, keeps its own.
        if (gInputModeChanged) eng::Call(widgets, "SetInputMode_GameOnly", controller);
        gInputModeChanged = false;
    }
}

void UpdateTyping(Obj controller) {
    Obj widgets = eng::FindCdo("WidgetBlueprintLibrary");
    Obj typing = eng::Get(gTypingWidget);
    if (!widgets || (typing != nullptr) == gTypingMode) return;
    if (typing) {
        // No mouse lock, input not flushed.
        eng::Call(widgets, "SetInputMode_UIOnlyEx", controller, typing, uint8_t{0}, uint8_t{0});
        hostlog::Info("typing: input is UI-only");
    } else {
        eng::Call(widgets, "SetInputMode_GameAndUIEx", controller);
        hostlog::Info("typing ended: input is game and UI");
    }
    gTypingMode = typing != nullptr;
}

}  // namespace

void SetViewportClient(Obj client) { gViewport = eng::MakeWeak(client); }

Obj PlayerController() { return eng::Get(gController); }
int Generation() { return gGeneration; }

double Seconds() {
    static LARGE_INTEGER frequency{}, start{};
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!frequency.QuadPart) {
        QueryPerformanceFrequency(&frequency);
        start = now;
    }
    return static_cast<double>(now.QuadPart - start.QuadPart) / static_cast<double>(frequency.QuadPart);
}

void Frame() {
    // The controller is looked up every frame; a different one means a different map.
    Obj viewport = eng::Get(gViewport);
    Obj controller =
        viewport ? eng::Call(eng::FindCdo("GameplayStatics"), "GetPlayerController", viewport, int32_t{0}).ReturnObj() : nullptr;
    if (controller != gController.o) {
        ++gGeneration;
        gCursorTaken = gInputModeChanged = gTypingMode = false;     // the new controller starts with the game's own state
    }
    gController = eng::MakeWeak(controller);
    if (controller) {
        UpdateCursor(controller);
        UpdateTyping(controller);
    }
}

bool CursorShown() {
    bool shown = false;
    if (Obj controller = PlayerController()) eng::ReadBool(controller, "bShowMouseCursor", &shown);
    return shown;
}

void SetTypingWidget(Obj textInput) { gTypingWidget = eng::MakeWeak(textInput); }

void RequestCursor(int owner, bool visible) {
    if (visible) gCursorOwners.insert(owner);
    else gCursorOwners.erase(owner);
}

bool OpenLevel(const std::string& map) {
    Obj strings = eng::FindCdo("KismetStringLibrary"), statics = eng::FindCdo("GameplayStatics");
    Obj controller = PlayerController();
    if (!strings || !statics || !controller) return false;
    const std::wstring w = eng::Widen(map);
    const eng::FString fs{w.c_str(), static_cast<int32_t>(w.size() + 1), static_cast<int32_t>(w.size() + 1)};
    const eng::Params name = eng::Call(strings, "Conv_StringToName", fs);
    size_t size = 0;
    const uint8_t* fname = name.Return(&size);
    if (!name.Invoked() || !fname) return false;
    eng::Params open(eng::FunctionOn(statics, "OpenLevel"));
    open.SetArg(0, controller);
    open.SetArg(1, fname, size);
    open.SetArg(2, uint8_t{1});     // bAbsolute; Options stays an empty string
    hostlog::Info("opening map " + map);
    return eng::Invoke(statics, open);
}

}  // namespace game
