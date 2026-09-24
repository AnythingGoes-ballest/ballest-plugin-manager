// Script API bindings. Each namespace below is one area of the host; the C++ side of every function is a thin
// adapter onto the module that owns the behaviour (ui, input, replay, game, plugins). Handle types are
// registered as asOBJ_NOCOUNT: the host owns them for the plugin's lifetime and scripts cannot delete them.
#include "api.hpp"

#include <angelscript.h>
#include <scriptarray/scriptarray.h>
#include <scriptstdstring/scriptstdstring.h>

#include <string>
#include <utility>

#include "game.hpp"
#include "input.hpp"
#include "log.hpp"
#include "plugins.hpp"
#include "engine.hpp"
#include "race.hpp"
#include "registry.hpp"
#include "replay.hpp"
#include "storage.hpp"
#include "testchannel.hpp"
#include "ui.hpp"

namespace api {
namespace {

asIScriptEngine* e = nullptr;

void Check(int result, const char* what) {
    if (result < 0) hostlog::Error(std::string("API registration failed: ") + what + " (" + std::to_string(result) + ")");
}
void Global(const char* decl, const asSFuncPtr& fn) { Check(e->RegisterGlobalFunction(decl, fn, asCALL_CDECL), decl); }
void Method(const char* type, const char* decl, const asSFuncPtr& fn) {
    Check(e->RegisterObjectMethod(type, decl, fn, asCALL_CDECL_OBJFIRST), decl);
}

// --- Log, Host -------------------------------------------------------------------------------------------------
void LogInfo(const std::string& m) { hostlog::Write("info", plugins::CurrentId(), m); }
void LogWarn(const std::string& m) { hostlog::Write("warn", plugins::CurrentId(), m); }
void LogError(const std::string& m) { hostlog::Write("error", plugins::CurrentId(), m); }
unsigned LogLineCount() { return static_cast<unsigned>(hostlog::LineCount()); }
std::string LogLine(unsigned i) { return hostlog::Line(i); }
std::string HostVersion() { return plugins::kHostVersion; }
double HostTime() { return game::Seconds(); }
void ConsoleRun(const std::string& command) { testchannel::Enqueue(command); }
std::string StorageGet(const std::string& key, const std::string& fallback) { return storage::Get(plugins::CurrentId(), key, fallback); }
void StorageSet(const std::string& key, const std::string& value) { storage::Set(plugins::CurrentId(), key, value); }

// --- Plugins -----------------------------------------------------------------------------------------------------
plugins::Info PluginAt(unsigned i) {
    const auto list = plugins::List();
    return i < list.size() ? list[i] : plugins::Info{};
}
unsigned PluginCount() { return static_cast<unsigned>(plugins::List().size()); }
std::string PluginId(unsigned i) { return PluginAt(i).id; }
std::string PluginName(unsigned i) { return PluginAt(i).name; }
std::string PluginVersion(unsigned i) { return PluginAt(i).version; }
std::string PluginStatus(unsigned i) { return PluginAt(i).status; }
std::string PluginAuthor(unsigned i) { return PluginAt(i).author; }
std::string PluginDescription(unsigned i) { return PluginAt(i).description; }
std::string PluginIcon(unsigned i) { return PluginAt(i).icon; }
bool PluginEssential(unsigned i) { return PluginAt(i).essential; }
bool PluginInstalled(const std::string& id) { return plugins::Find(id, nullptr); }
std::string PluginInstalledVersion(const std::string& id) {
    plugins::Info info;
    return plugins::Find(id, &info) ? info.version : "";
}
// Installing and removing change what runs in the game, so only an essential plugin (the plugin manager) may.
bool MayManage() {
    if (plugins::CurrentIsEssential()) return true;
    hostlog::Write("warn", plugins::CurrentId(), "only the plugin manager can install or remove plugins");
    return false;
}
void PluginInstall(const std::string& id) {
    if (MayManage()) registry::Install(id);
}
void PluginRemove(const std::string& id) {
    if (MayManage()) registry::Remove(id);
}

// --- Registry ------------------------------------------------------------------------------------------------------
registry::Entry EntryAt(unsigned i) {
    const auto& list = registry::Entries();
    return i < list.size() ? list[i] : registry::Entry{};
}
unsigned RegistryCount() { return static_cast<unsigned>(registry::Entries().size()); }
std::string RegistryId(unsigned i) { return EntryAt(i).id; }
std::string RegistryName(unsigned i) { return EntryAt(i).name; }
std::string RegistryDescription(unsigned i) { return EntryAt(i).description; }
std::string RegistryAuthor(unsigned i) { return EntryAt(i).author; }
std::string RegistryVersion(unsigned i) { return EntryAt(i).version; }
std::string RegistryPage(unsigned i) { return i < registry::Entries().size() ? EntryAt(i).Page() : ""; }
std::string RegistryIcon(unsigned i) { return EntryAt(i).icon; }

// Opens a page in the player's browser through the game (KismetSystemLibrary.LaunchURL). Only GitHub pages.
void OpenUrl(const std::string& url) {
    if (url.rfind("https://github.com/", 0) != 0 || url.find_first_of(" \"<>") != std::string::npos) {
        hostlog::Write("warn", plugins::CurrentId(), "not opening " + url + " (only https://github.com/ pages)");
        return;
    }
    const std::wstring w = eng::Widen(url);
    eng::Call(eng::FindCdo("KismetSystemLibrary"), "LaunchURL", eng::FString{w.c_str(), static_cast<int32_t>(w.size() + 1), static_cast<int32_t>(w.size() + 1)});
    hostlog::Write("info", plugins::CurrentId(), "opened " + url);
}

// --- UI: footer ----------------------------------------------------------------------------------------------------
template <class T>
bool TakeFlag(T* holder, bool T::*flag) {
    const bool was = holder->*flag;
    holder->*flag = false;
    return was;
}
ui::FooterButton* AddFooterButton(const std::string& label) { return ui::AddFooterButton(plugins::Current(), label); }
bool FooterClicked(ui::FooterButton* b) { return TakeFlag(b, &ui::FooterButton::clickPending); }
bool FooterHovered(ui::FooterButton* b) { return b->hovered; }
void FooterLabel(ui::FooterButton* b, const std::string& s) { b->label = s; }

ui::Panel* CreatePanel() { return ui::CreatePanel(plugins::Current()); }
void PanelClear(ui::Panel* p) { p->lines.clear(); }
void PanelAddLine(ui::Panel* p, const std::string& s) { p->lines.push_back(s); }
void PanelTitle(ui::Panel* p, const std::string& s) { p->title = s; }
bool PanelGetVisible(ui::Panel* p) { return p->visible; }
void PanelSetVisible(ui::Panel* p, bool v) { p->visible = v; }
ui::FooterButton* PanelAddButton(ui::Panel* p, const std::string& s) { return ui::AddPanelButton(p, s); }
void SetCursorVisible(bool v) { game::RequestCursor(plugins::Current(), v); }

// --- UI: windows ---------------------------------------------------------------------------------------------------
ui::Window* NewWindow() { return ui::MakeWindow(plugins::Current()); }
void WinAnchor(ui::Window* w, float x, float y) {
    w->anchorX = x;
    w->anchorY = y;
    w->layoutDirty = true;
}
void WinPivot(ui::Window* w, float x, float y) {
    w->pivotX = x;
    w->pivotY = y;
    w->layoutDirty = true;
}
void WinOffset(ui::Window* w, float x, float y) {
    w->offsetX = x;
    w->offsetY = y;
    w->layoutDirty = true;
}
void WinBackground(ui::Window* w, float r, float g, float b, float a) {
    w->background = {r, g, b, a};
    w->layoutDirty = true;
}
bool WinGetVisible(ui::Window* w) { return w->visible; }
void WinSetVisible(ui::Window* w, bool v) { w->visible = v; }
ui::Widget* WinText(ui::Window* w, const std::string& s, float size) { return ui::AddWidget(w, ui::Kind::Text, s, size); }
ui::Widget* WinButton(ui::Window* w, const std::string& s) { return ui::AddWidget(w, ui::Kind::Button, s, 0); }
ui::Widget* WinIconButton(ui::Window* w, const std::string& icon) { return ui::AddWidget(w, ui::Kind::IconButton, icon, 0); }
ui::Widget* WinSlider(ui::Window* w, float width) { return ui::AddWidget(w, ui::Kind::Slider, "", width); }
ui::Widget* WinDropdown(ui::Window* w, float width) { return ui::AddWidget(w, ui::Kind::Dropdown, "", width); }
void WinSpace(ui::Window* w, float width) { ui::AddWidget(w, ui::Kind::Space, "", width); }
void WinNewRow(ui::Window* w) { ui::NewRow(w); }
void WinStartSidebar(ui::Window* w, float width) { ui::StartSidebar(w, width); }
void WinStartMain(ui::Window* w) { ui::StartMain(w); }
void WinScreenSize(ui::Window* w, float width, float height) {
    w->screenWidth = width;
    w->screenHeight = height;
    w->layoutDirty = true;
}
void ButtonBackground(ui::Widget* w, float r, float g, float b, float a) {
    w->background = {r, g, b, a};
    w->backgroundDirty = true;
}
ui::Widget* WinTextArea(ui::Window* w, float width, float height, float size) {
    ui::Widget* area = ui::AddWidget(w, ui::Kind::TextArea, "", width);
    area->height = height;
    area->size = size;
    return area;
}
int WinStartView(ui::Window* w) { return ui::StartView(w); }
void WinShowView(ui::Window* w, int view) { ui::ShowView(w, view); }
void WinClearView(ui::Window* w, int view) { ui::ClearView(w, view); }
ui::Widget* WinImage(ui::Window* w, const std::string& path, float width, float height) {
    ui::Widget* image = ui::AddWidget(w, ui::Kind::Image, path, width);
    image->height = height;
    return image;
}
ui::Widget* WinTextInput(ui::Window* w, float width, const std::string& hint, float size) {
    ui::Widget* input = ui::AddWidget(w, ui::Kind::TextInput, hint, width);
    input->size = size;
    return input;
}

void SetWidgetText(ui::Widget* w, const std::string& s) { w->text = s; }
std::string GetWidgetText(ui::Widget* w) { return w->text; }
void TextColor(ui::Widget* w, float r, float g, float b, float a) {
    w->color = {r, g, b, a};
    w->colorDirty = true;
}
bool Clicked(ui::Widget* w) { return TakeFlag(w, &ui::Widget::clickPending); }
bool Hovered(ui::Widget* w) { return w->hovered; }
float SliderGet(ui::Widget* w) { return w->value; }
void SliderSet(ui::Widget* w, float v) {
    if (!w->dragging) w->value = v < 0 ? 0 : (v > 1 ? 1 : v);        // the user's drag wins
}
bool SliderDragging(ui::Widget* w) { return w->dragging; }
void DropdownAdd(ui::Widget* w, const std::string& s) { ui::AddOption(w, s); }
int DropdownGet(ui::Widget* w) { return w->selected; }
void DropdownSet(ui::Widget* w, int i) {
    if (i >= 0 && i < static_cast<int>(w->options.size())) w->selected = i;
}
bool DropdownChanged(ui::Widget* w) { return TakeFlag(w, &ui::Widget::changedPending); }
bool InputSubmitted(ui::Widget* w) { return TakeFlag(w, &ui::Widget::submitPending); }
std::string InputText(ui::Widget* w) { return w->submitted; }
bool InputFocused(ui::Widget* w) { return w->focused; }
void InputFocus(ui::Widget* w) {
    w->focusRequested = true;
    w->focusAttempts = 0;
}
void InputSubmit(ui::Widget* w) { w->submitRequested = true; }

// --- Input, Replay -------------------------------------------------------------------------------------------------
// While a text input has keyboard focus the keys are being typed there, so plugins see none of them.
bool KeyPressed(int key) { return !ui::Typing() && input::Pressed(key); }
bool KeyDown(int key) { return !ui::Typing() && input::Down(key); }

void RegisterCore() {
    RegisterScriptArray(e, true);
    RegisterStdString(e);
    RegisterStdStringUtils(e);          // string.split, join (needs the array type)
    e->SetDefaultNamespace("Log");
    Global("void Info(const string &in)", asFUNCTION(LogInfo));
    Global("void Warn(const string &in)", asFUNCTION(LogWarn));
    Global("void Error(const string &in)", asFUNCTION(LogError));
    Global("uint LineCount()", asFUNCTION(LogLineCount));
    Global("string Line(uint)", asFUNCTION(LogLine));

    e->SetDefaultNamespace("Host");
    Global("string Version()", asFUNCTION(HostVersion));
    Global("double Time()", asFUNCTION(HostTime));
    Global("void OpenUrl(const string &in)", asFUNCTION(OpenUrl));

    e->SetDefaultNamespace("Plugins");
    Global("uint Count()", asFUNCTION(PluginCount));
    Global("string Id(uint)", asFUNCTION(PluginId));
    Global("string Name(uint)", asFUNCTION(PluginName));
    Global("string Version(uint)", asFUNCTION(PluginVersion));
    Global("string Status(uint)", asFUNCTION(PluginStatus));
    Global("string Author(uint)", asFUNCTION(PluginAuthor));
    Global("string Description(uint)", asFUNCTION(PluginDescription));
    Global("string Icon(uint)", asFUNCTION(PluginIcon));
    Global("bool Essential(uint)", asFUNCTION(PluginEssential));
    Global("bool IsInstalled(const string &in id)", asFUNCTION(PluginInstalled));
    Global("string InstalledVersion(const string &in id)", asFUNCTION(PluginInstalledVersion));
    Global("void Install(const string &in id)", asFUNCTION(PluginInstall));
    Global("void Remove(const string &in id)", asFUNCTION(PluginRemove));
    Global("string Pending(const string &in id)", asFUNCTION(registry::Pending));
    Global("string DefaultIcon()", asFUNCTION(registry::DefaultIcon));

    e->SetDefaultNamespace("Registry");
    Global("void Refresh()", asFUNCTION(registry::Refresh));
    Global("string State()", asFUNCTION(registry::State));
    Global("uint Count()", asFUNCTION(RegistryCount));
    Global("string Id(uint)", asFUNCTION(RegistryId));
    Global("string Name(uint)", asFUNCTION(RegistryName));
    Global("string Description(uint)", asFUNCTION(RegistryDescription));
    Global("string Author(uint)", asFUNCTION(RegistryAuthor));
    Global("string Version(uint)", asFUNCTION(RegistryVersion));
    Global("string Page(uint)", asFUNCTION(RegistryPage));
    Global("string Icon(uint)", asFUNCTION(RegistryIcon));

    e->SetDefaultNamespace("Console");
    Global("void Run(const string &in)", asFUNCTION(ConsoleRun));

    e->SetDefaultNamespace("Storage");
    Global("string Get(const string &in key, const string &in fallback = \"\")", asFUNCTION(StorageGet));
    Global("void Set(const string &in key, const string &in value)", asFUNCTION(StorageSet));
}

void RegisterUi() {
    e->SetDefaultNamespace("UI");
    for (const char* type : {"FooterButton", "Panel", "Window", "Text", "Button", "Slider", "Dropdown", "TextArea", "TextInput", "Image"})
        Check(e->RegisterObjectType(type, 0, asOBJ_REF | asOBJ_NOCOUNT), type);
    Global("void SetCursorVisible(bool)", asFUNCTION(SetCursorVisible));
    Global("bool CursorShown()", asFUNCTION(game::CursorShown));

    Global("FooterButton@ AddFooterButton(const string &in)", asFUNCTION(AddFooterButton));
    Method("FooterButton", "bool Clicked()", asFUNCTION(FooterClicked));
    Method("FooterButton", "bool get_hovered() property", asFUNCTION(FooterHovered));
    Method("FooterButton", "void set_label(const string &in) property", asFUNCTION(FooterLabel));

    Global("Panel@ CreatePanel()", asFUNCTION(CreatePanel));
    Method("Panel", "void Clear()", asFUNCTION(PanelClear));
    Method("Panel", "void AddLine(const string &in)", asFUNCTION(PanelAddLine));
    Method("Panel", "void set_title(const string &in) property", asFUNCTION(PanelTitle));
    Method("Panel", "bool get_visible() property", asFUNCTION(PanelGetVisible));
    Method("Panel", "void set_visible(bool) property", asFUNCTION(PanelSetVisible));
    Method("Panel", "FooterButton@ AddButton(const string &in)", asFUNCTION(PanelAddButton));

    Global("Window@ CreateWindow()", asFUNCTION(NewWindow));
    Method("Window", "void SetAnchor(float, float)", asFUNCTION(WinAnchor));
    Method("Window", "void SetPivot(float, float)", asFUNCTION(WinPivot));
    Method("Window", "void SetOffset(float, float)", asFUNCTION(WinOffset));
    Method("Window", "void SetBackground(float, float, float, float)", asFUNCTION(WinBackground));
    Method("Window", "bool get_visible() property", asFUNCTION(WinGetVisible));
    Method("Window", "void set_visible(bool) property", asFUNCTION(WinSetVisible));
    Method("Window", "Text@ AddText(const string &in, float size = 16)", asFUNCTION(WinText));
    Method("Window", "Button@ AddButton(const string &in)", asFUNCTION(WinButton));
    Method("Window", "Button@ AddIconButton(const string &in)", asFUNCTION(WinIconButton));
    Method("Window", "Slider@ AddSlider(float)", asFUNCTION(WinSlider));
    Method("Window", "Dropdown@ AddDropdown(float)", asFUNCTION(WinDropdown));
    Method("Window", "void AddSpace(float)", asFUNCTION(WinSpace));
    Method("Window", "void NewRow()", asFUNCTION(WinNewRow));
    Method("Window", "void StartSidebar(float width)", asFUNCTION(WinStartSidebar));
    Method("Window", "void StartMain()", asFUNCTION(WinStartMain));
    Method("Window", "void SetScreenSize(float width, float height)", asFUNCTION(WinScreenSize));
    Method("Window", "int StartView()", asFUNCTION(WinStartView));
    Method("Window", "void ShowView(int)", asFUNCTION(WinShowView));
    Method("Window", "void ClearView(int)", asFUNCTION(WinClearView));
    Method("Window", "Image@ AddImage(const string &in path, float width, float height)", asFUNCTION(WinImage));
    Method("Window", "TextArea@ AddTextArea(float width, float height, float size = 14)", asFUNCTION(WinTextArea));
    Method("Window", "TextInput@ AddTextInput(float width, const string &in hint = \"\", float size = 18)", asFUNCTION(WinTextInput));

    Method("Text", "void set_text(const string &in) property", asFUNCTION(SetWidgetText));
    Method("Text", "string get_text() property", asFUNCTION(GetWidgetText));
    Method("Text", "void SetColor(float, float, float, float)", asFUNCTION(TextColor));
    Method("Button", "bool Clicked()", asFUNCTION(Clicked));
    Method("Button", "bool get_hovered() property", asFUNCTION(Hovered));
    Method("Button", "void SetBackground(float, float, float, float)", asFUNCTION(ButtonBackground));
    Method("Button", "void set_label(const string &in) property", asFUNCTION(SetWidgetText));
    Method("Button", "void set_icon(const string &in) property", asFUNCTION(SetWidgetText));
    Method("Slider", "float get_value() property", asFUNCTION(SliderGet));
    Method("Slider", "void set_value(float) property", asFUNCTION(SliderSet));
    Method("Slider", "bool get_dragging() property", asFUNCTION(SliderDragging));
    Method("Dropdown", "void AddOption(const string &in)", asFUNCTION(DropdownAdd));
    Method("Dropdown", "int get_selected() property", asFUNCTION(DropdownGet));
    Method("Dropdown", "void set_selected(int) property", asFUNCTION(DropdownSet));
    Method("Dropdown", "bool Changed()", asFUNCTION(DropdownChanged));
    Method("Image", "void set_path(const string &in) property", asFUNCTION(SetWidgetText));
    Method("Image", "string get_path() property", asFUNCTION(GetWidgetText));
    Method("TextArea", "void set_text(const string &in) property", asFUNCTION(SetWidgetText));
    Method("TextArea", "string get_text() property", asFUNCTION(GetWidgetText));
    Method("TextInput", "bool Submitted()", asFUNCTION(InputSubmitted));
    Method("TextInput", "string get_text() property", asFUNCTION(InputText));
    Method("TextInput", "bool get_focused() property", asFUNCTION(InputFocused));
    Method("TextInput", "void Focus()", asFUNCTION(InputFocus));
    Method("TextInput", "void Submit()", asFUNCTION(InputSubmit));
}

// Keys are Windows virtual-key codes; the common ones are named, plus A-Z and N0-N9.
void RegisterInput() {
    e->SetDefaultNamespace("Input");
    Check(e->RegisterEnum("Key"), "Input::Key");
    const std::pair<const char*, int> keys[] = {
        {"Space", 0x20}, {"Enter", 0x0D}, {"Escape", 0x1B}, {"Tab", 0x09}, {"Shift", 0x10}, {"Ctrl", 0x11}, {"Alt", 0x12},
        {"Left", 0x25}, {"Up", 0x26}, {"Right", 0x27}, {"Down", 0x28}, {"MouseLeft", 0x01}, {"MouseRight", 0x02},
        {"MouseMiddle", 0x04}, {"F1", 0x70}, {"F2", 0x71}, {"F3", 0x72}, {"F4", 0x73}, {"F5", 0x74}, {"F6", 0x75},
        {"F7", 0x76}, {"F8", 0x77}, {"F9", 0x78}, {"F10", 0x79}, {"F11", 0x7A}, {"F12", 0x7B}};
    for (const auto& [name, vk] : keys) Check(e->RegisterEnumValue("Key", name, vk), name);
    for (char c = 'A'; c <= 'Z'; ++c) Check(e->RegisterEnumValue("Key", std::string(1, c).c_str(), c), "letter");
    for (char c = '0'; c <= '9'; ++c) Check(e->RegisterEnumValue("Key", (std::string("N") + c).c_str(), c), "digit");
    Global("bool Pressed(Key)", asFUNCTION(KeyPressed));
    Global("bool Down(Key)", asFUNCTION(KeyDown));
}

void RegisterRace() {
    e->SetDefaultNamespace("Race");
    Global("bool OnTrack()", asFUNCTION(race::OnTrack));
    Global("bool IsActive()", asFUNCTION(race::Active));
    Global("int Restarts()", asFUNCTION(race::Restarts));
}

void RegisterReplay() {
    e->SetDefaultNamespace("Replay");
    Check(e->RegisterEnum("Camera"), "Replay::Camera");
    Check(e->RegisterEnumValue("Camera", "Default", replay::CameraDefault), "Default");
    Check(e->RegisterEnumValue("Camera", "Follow3D", replay::CameraFollow3D), "Follow3D");
    Check(e->RegisterEnumValue("Camera", "Free", replay::CameraFree), "Free");
    Global("bool IsActive()", asFUNCTION(replay::Active));
    Global("double Time()", asFUNCTION(replay::Time));
    Global("double Length()", asFUNCTION(replay::Length));
    Global("void Seek(double)", asFUNCTION(replay::Seek));
    Global("void Restart()", asFUNCTION(replay::Restart));
    Global("int CameraMode()", asFUNCTION(replay::CameraMode));
    Global("void SetCameraMode(int)", asFUNCTION(replay::SetCameraMode));
}

}  // namespace

void Register(asIScriptEngine* engine) {
    e = engine;
    RegisterCore();
    RegisterUi();
    RegisterInput();
    RegisterRace();
    RegisterReplay();
    e->SetDefaultNamespace("");
}

}  // namespace api
