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
#include "editor.hpp"
#include "engine.hpp"
#include "race.hpp"
#include "registry.hpp"
#include "replay.hpp"
#include "settings.hpp"
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
void UpdateHost() {
    if (MayManage()) registry::UpdateHost();
}

// --- Settings ------------------------------------------------------------------------------------------------------
settings::Setting SettingAt(unsigned i) {
    const auto& list = settings::List();
    return i < list.size() ? list[i] : settings::Setting{};
}
unsigned SettingCount() { return static_cast<unsigned>(settings::List().size()); }
std::string SettingPlugin(unsigned i) { return SettingAt(i).pluginId; }
std::string SettingName(unsigned i) { return SettingAt(i).name; }
std::string SettingDescription(unsigned i) { return SettingAt(i).description; }
bool SettingHidden(unsigned i) { return SettingAt(i).hidden; }
bool SettingHasRange(unsigned i) { return SettingAt(i).hasRange; }
double SettingMin(unsigned i) { return SettingAt(i).min; }
double SettingMax(unsigned i) { return SettingAt(i).max; }
std::string SettingKind(unsigned i) {
    switch (SettingAt(i).kind) {
        case settings::Kind::Bool: return "bool";
        case settings::Kind::Int: return "int";
        case settings::Kind::UInt: return "uint";
        case settings::Kind::Float: return "float";
        case settings::Kind::Double: return "double";
        case settings::Kind::String: return "string";
    }
    return "";
}
std::string SettingGet(unsigned i) { return settings::Get(i); }
bool SettingIsDefault(unsigned i) { return settings::IsDefault(i); }
// Changing another plugin's settings is for the plugin manager; a plugin changes its own by assigning the variable.
bool SettingSet(unsigned i, const std::string& value) { return MayManage() && settings::Set(i, value); }
void SettingReset(unsigned i) {
    if (MayManage()) settings::Reset(i);
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
void WinSetZOrder(ui::Window* w, int z) {
    w->zOrder = z;
    w->layoutDirty = true;
}
int WinGetZOrder(ui::Window* w) { return w->zOrder; }
void WinBlocksClicks(ui::Window* w, bool block) {
    w->blocksClicks = block;
    w->layoutDirty = true;
}
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
void SetTextSize(ui::Widget* w, float size) {
    if (size == w->size) return;
    w->size = size;
    w->window->layoutDirty = true;          // a font is only set while building (it holds a shared pointer)
}
float GetTextSize(ui::Widget* w) { return w->size; }
void SetWidgetVisible(ui::Widget* w, bool visible) { w->visible = visible; }
bool GetWidgetVisible(ui::Widget* w) { return w->visible; }
void WinSetMovable(ui::Window* w, bool movable) { ui::SetMovable(w, movable, plugins::CurrentId()); }
bool WinGetMovable(ui::Window* w) { return w->movable; }
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
void InputSetValue(ui::Widget* w, const std::string& s) {
    w->pendingValue = s;
    w->valuePending = true;
}
void InputClearOnSubmit(ui::Widget* w, bool on) { w->clearOnSubmit = on; }
bool CheckGet(ui::Widget* w) { return w->checked; }
void CheckSet(ui::Widget* w, bool on) { w->checked = on; }
bool CheckChanged(ui::Widget* w) { return TakeFlag(w, &ui::Widget::changedPending); }
void CheckColor(ui::Widget* w, float r, float g, float b, float a) {
    w->color = {r, g, b, a};
    w->colorSet = w->colorDirty = true;
}
ui::Widget* WinCheckBox(ui::Window* w, const std::string& label, float size) {
    ui::Widget* box = ui::AddWidget(w, ui::Kind::CheckBox, label, 0);
    box->size = size;
    return box;
}
void WinStartHeader(ui::Window* w) { ui::StartHeader(w); }
void WinStartCard(ui::Window* w) { ui::StartCard(w); }
void WinEndCard(ui::Window* w) { ui::EndCard(w); }
void WinCardBackground(ui::Window* w, float r, float g, float b, float a) {
    w->cardBackground = {r, g, b, a};
    w->layoutDirty = true;
}
void WinDockInEditorDetails(ui::Window* w) {
    w->dock = ui::Dock::EditorDetails;
    w->layoutDirty = true;
}

// --- Editor ----------------------------------------------------------------------------------------------------------
CScriptArray* IdArray(const std::vector<int>& ids) {
    CScriptArray* array = CScriptArray::Create(e->GetTypeInfoByDecl("array<int>"), static_cast<asUINT>(ids.size()));
    for (size_t i = 0; i < ids.size(); ++i) *static_cast<int*>(array->At(static_cast<asUINT>(i))) = ids[i];
    return array;
}
std::vector<int> IdVector(const CScriptArray* array) {
    std::vector<int> ids;
    for (asUINT i = 0; array && i < array->GetSize(); ++i) ids.push_back(*static_cast<const int*>(array->At(i)));
    return ids;
}
CScriptArray* EditorSelection() { return IdArray(editor::Selection()); }
CScriptArray* EditorPlaced() { return IdArray(editor::Placed()); }
CScriptArray* EditorDuplicate() {
    plugins::GameWork work;
    return IdArray(editor::DuplicateSelection());
}
bool EditorLocation(int id, double& x, double& y, double& z) {
    editor::Vec3 v;
    const bool ok = editor::Location(id, &v);
    x = v.x, y = v.y, z = v.z;
    return ok;
}
bool EditorRotation(int id, double& pitch, double& yaw, double& roll) {
    editor::Rot r;
    const bool ok = editor::Rotation(id, &r);
    pitch = r.pitch, yaw = r.yaw, roll = r.roll;
    return ok;
}
bool EditorSetLocation(int id, double x, double y, double z) { return editor::SetLocation(id, {x, y, z}); }
bool EditorSetRotation(int id, double pitch, double yaw, double roll) { return editor::SetRotation(id, {pitch, yaw, roll}); }
void EditorViewForward(double& x, double& y, double& z) {
    const editor::Vec3 f = editor::ViewForward();
    x = f.x, y = f.y, z = f.z;
}
void EditorSelect(const CScriptArray* ids) {
    plugins::GameWork work;
    editor::Select(IdVector(ids));
}
void EditorRotatePieces(const CScriptArray* ids, double cx, double cy, double cz, double dx, double dy, double dz) {
    plugins::GameWork work;
    editor::RotatePieces(IdVector(ids), {cx, cy, cz}, dx, dy, dz);
}

// --- Input, Replay -------------------------------------------------------------------------------------------------
// While a text input has keyboard focus the keys are being typed there, so plugins see none of them, except Escape,
// which types nothing and is how a player leaves a menu.
constexpr int kEscape = 0x1B;
bool KeyPressed(int key) { return (!ui::Typing() || key == kEscape) && input::Pressed(key); }
bool KeyDown(int key) { return (!ui::Typing() || key == kEscape) && input::Down(key); }

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
    Global("void OpenFolder()", asFUNCTION(plugins::OpenFolder));
    Global("void UpdateHost()", asFUNCTION(UpdateHost));
    Global("string HostUpdateState()", asFUNCTION(registry::HostUpdateState));

    e->SetDefaultNamespace("Settings");
    Global("uint Count()", asFUNCTION(SettingCount));
    Global("string Plugin(uint)", asFUNCTION(SettingPlugin));
    Global("string Name(uint)", asFUNCTION(SettingName));
    Global("string Description(uint)", asFUNCTION(SettingDescription));
    Global("string Kind(uint)", asFUNCTION(SettingKind));
    Global("bool Hidden(uint)", asFUNCTION(SettingHidden));
    Global("bool HasRange(uint)", asFUNCTION(SettingHasRange));
    Global("double Min(uint)", asFUNCTION(SettingMin));
    Global("double Max(uint)", asFUNCTION(SettingMax));
    Global("string Get(uint)", asFUNCTION(SettingGet));
    Global("bool IsDefault(uint)", asFUNCTION(SettingIsDefault));
    Global("bool Set(uint, const string &in)", asFUNCTION(SettingSet));
    Global("void Reset(uint)", asFUNCTION(SettingReset));

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
    Global("string HostVersion()", asFUNCTION(registry::HostVersion));

    e->SetDefaultNamespace("Console");
    Global("void Run(const string &in)", asFUNCTION(ConsoleRun));

    e->SetDefaultNamespace("Storage");
    Global("string Get(const string &in key, const string &in fallback = \"\")", asFUNCTION(StorageGet));
    Global("void Set(const string &in key, const string &in value)", asFUNCTION(StorageSet));
}

void RegisterUi() {
    e->SetDefaultNamespace("UI");
    for (const char* type : {"FooterButton", "Panel", "Window", "Text", "Button", "Slider", "Dropdown", "TextArea", "TextInput", "Image", "CheckBox"})
        Check(e->RegisterObjectType(type, 0, asOBJ_REF | asOBJ_NOCOUNT), type);
    Global("void SetCursorVisible(bool)", asFUNCTION(SetCursorVisible));
    Global("bool CursorShown()", asFUNCTION(game::CursorShown));
    Global("void ResetPositions(const string &in pluginId)", asFUNCTION(ui::ResetPositions));
    Global("bool HasMovable(const string &in pluginId)", asFUNCTION(ui::HasMovable));

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
    Method("Window", "void SetBlocksClicks(bool)", asFUNCTION(WinBlocksClicks));
    Method("Window", "CheckBox@ AddCheckBox(const string &in label, float size = 16)", asFUNCTION(WinCheckBox));
    Method("Window", "void DockInEditorDetails()", asFUNCTION(WinDockInEditorDetails));
    Method("Window", "void StartHeader()", asFUNCTION(WinStartHeader));
    Method("Window", "void StartCard()", asFUNCTION(WinStartCard));
    Method("Window", "void EndCard()", asFUNCTION(WinEndCard));
    Method("Window", "void SetCardBackground(float, float, float, float)", asFUNCTION(WinCardBackground));
    Method("Window", "void set_zOrder(int) property", asFUNCTION(WinSetZOrder));
    Method("Window", "int get_zOrder() property", asFUNCTION(WinGetZOrder));
    Method("Window", "void set_movable(bool) property", asFUNCTION(WinSetMovable));
    Method("Window", "bool get_movable() property", asFUNCTION(WinGetMovable));
    Method("Window", "void ShowView(int)", asFUNCTION(WinShowView));
    Method("Window", "void ClearView(int)", asFUNCTION(WinClearView));
    Method("Window", "Image@ AddImage(const string &in path, float width, float height)", asFUNCTION(WinImage));
    Method("Window", "TextArea@ AddTextArea(float width, float height, float size = 14)", asFUNCTION(WinTextArea));
    Method("Window", "TextInput@ AddTextInput(float width, const string &in hint = \"\", float size = 18)", asFUNCTION(WinTextInput));

    Method("Text", "void set_text(const string &in) property", asFUNCTION(SetWidgetText));
    Method("Text", "string get_text() property", asFUNCTION(GetWidgetText));
    Method("Text", "void SetColor(float, float, float, float)", asFUNCTION(TextColor));
    for (const char* type : {"Text", "Button", "Slider", "Dropdown", "TextArea", "TextInput", "Image"}) {
        Method(type, "void set_visible(bool) property", asFUNCTION(SetWidgetVisible));
        Method(type, "bool get_visible() property", asFUNCTION(GetWidgetVisible));
    }
    Method("Text", "void set_size(float) property", asFUNCTION(SetTextSize));
    Method("Text", "float get_size() property", asFUNCTION(GetTextSize));
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
    Method("TextInput", "void set_value(const string &in) property", asFUNCTION(InputSetValue));
    Method("TextInput", "void set_clearOnSubmit(bool) property", asFUNCTION(InputClearOnSubmit));
    Method("CheckBox", "bool get_checked() property", asFUNCTION(CheckGet));
    Method("CheckBox", "void set_checked(bool) property", asFUNCTION(CheckSet));
    Method("CheckBox", "bool Changed()", asFUNCTION(CheckChanged));
    Method("CheckBox", "void SetColor(float, float, float, float)", asFUNCTION(CheckColor));
    Method("CheckBox", "void set_visible(bool) property", asFUNCTION(SetWidgetVisible));
    Method("CheckBox", "bool get_visible() property", asFUNCTION(GetWidgetVisible));
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

void RegisterEditor() {
    e->SetDefaultNamespace("Editor");
    Global("bool IsOpen()", asFUNCTION(editor::Open));
    Global("array<int>@ Selection()", asFUNCTION(EditorSelection));
    Global("array<int>@ Placed()", asFUNCTION(EditorPlaced));
    Global("bool GetLocation(int, double &out, double &out, double &out)", asFUNCTION(EditorLocation));
    Global("bool GetRotation(int, double &out, double &out, double &out)", asFUNCTION(EditorRotation));
    Global("bool SetLocation(int, double, double, double)", asFUNCTION(EditorSetLocation));
    Global("bool SetRotation(int, double, double, double)", asFUNCTION(EditorSetRotation));
    Global("void ViewForward(double &out, double &out, double &out)", asFUNCTION(EditorViewForward));
    Global("void Select(const array<int>@)", asFUNCTION(EditorSelect));
    Global("array<int>@ DuplicateSelection()", asFUNCTION(EditorDuplicate));
    Global("void RotatePieces(const array<int>@, double, double, double, double, double, double)", asFUNCTION(EditorRotatePieces));
    Global("void SetTabCycling(bool)", asFUNCTION(editor::SetTabCycling));
    Global("void SetRotateAroundCenter(bool)", asFUNCTION(editor::SetRotateAroundCenter));
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
    Global("double CameraDistance()", asFUNCTION(replay::CameraDistance));
    Global("void SetCameraDistance(double)", asFUNCTION(replay::SetCameraDistance));
    Global("bool SeeThrough()", asFUNCTION(replay::SeeThrough));
    Global("void SetSeeThrough(bool)", asFUNCTION(replay::SetSeeThrough));
}

}  // namespace

void Register(asIScriptEngine* engine) {
    e = engine;
    RegisterCore();
    RegisterUi();
    RegisterInput();
    RegisterRace();
    RegisterEditor();
    RegisterReplay();
    e->SetDefaultNamespace("");
}

}  // namespace api
