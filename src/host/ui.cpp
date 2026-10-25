// The UI frame and test hooks, delegating to the footer (footer.cpp) and windows (windows.cpp).
#include "ui.hpp"

#include "campaigns.hpp"

namespace ui {

void Frame() {
    footer::Frame();
    menu::Frame();
    campaigns::Frame();
    windows::Frame();
}

// "window:<label>" only looks at window buttons, for labels the footer also uses.
bool SimulateClick(const std::string& label) {
    if (label.rfind("window:", 0) == 0) return windows::SimulateClick(label.substr(7));
    return footer::SimulateClick(label) || menu::SimulateClick(label) || windows::SimulateClick(label);
}
bool SimulateSelect(const std::string& firstOption, int index) { return windows::SimulateSelect(firstOption, index); }
bool SimulateClickAt(double x, double y, std::string* what) {
    return footer::SimulateClickAt(x, y, what) || windows::SimulateClickAt(x, y, what);
}
void SimulateSlider(float value) { windows::SimulateSlider(value); }
bool SimulateSubmit(const std::string& text) { return windows::SimulateSubmit(text); }
bool SimulateType(const std::string& text) { return windows::SimulateType(text); }
bool Typing() { return windows::Typing(); }

void RemoveOwner(int owner) {
    footer::RemoveOwner(owner);
    menu::RemoveOwner(owner);
    campaigns::RemoveOwner(owner);
    windows::RemoveOwner(owner);
}

void HideOwner(int owner) {
    footer::HideOwner(owner);
    windows::HideOwner(owner);
}

std::string Status() {
    const std::string w = windows::Status();
    return footer::Status() + menu::Status() + (w.empty() ? "" : " " + w);
}

}  // namespace ui
