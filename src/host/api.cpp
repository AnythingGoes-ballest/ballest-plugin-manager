// Script API bindings. Each namespace below is one area of the host; the C++ side of every function is a thin
// adapter onto the module that owns the behaviour (ui, input, replay, game, plugins). Handle types are
// registered as asOBJ_NOCOUNT: the host owns them for the plugin's lifetime and scripts cannot delete them.
#include "api.hpp"
#include "widgets.hpp"

#include <cmath>
#include <cstdlib>
#include <windows.h>

#include <angelscript.h>
#include <scriptarray/scriptarray.h>
#include <scriptstdstring/scriptstdstring.h>

#include <sstream>
#include <cstring>
#include <cctype>
#include <map>
#include <string>
#include <utility>

#include "cosmetics.hpp"
#include "draw.hpp"
#include "postprocess.hpp"
#include "ghosts.hpp"
#include "tracks.hpp"
#include "workshop.hpp"
#include "hub.hpp"
#include "game.hpp"
#include "hud.hpp"
#include "input.hpp"
#include "leaderboard.hpp"
#include "log.hpp"
#include "plugins.hpp"
#include "editor.hpp"
#include "engine.hpp"
#include "race.hpp"
#include "registry.hpp"
#include "replay.hpp"
#include "runs.hpp"
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
void ConsoleRun(const std::string& command) {
    // Not for plugins in general (review rule, now enforced here): the bundled plugin manager's console box, and the
    // API tests in a completely sandboxed test copy.
    if (!plugins::CurrentIsEssential() && !(plugins::CurrentId() == "api-tests" && testchannel::MutationsAllowed())) {
        hostlog::Write("warn", plugins::CurrentId(), "Console::Run is only for the plugin manager");
        return;
    }
    testchannel::Enqueue(command);
}
std::string StorageGet(const std::string& key, const std::string& fallback) { return storage::Get(plugins::CurrentId(), key, fallback); }
void StorageSet(const std::string& key, const std::string& value) { storage::Set(plugins::CurrentId(), key, value); }

// --- Plugins -----------------------------------------------------------------------------------------------------
std::string PluginFolder() {
    const std::wstring dir = plugins::CurrentDir();
    return dir.empty() ? "" : eng::Narrow(dir.c_str(), static_cast<int>(dir.size())) + "\\";
}
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
bool PluginEnabled(unsigned i) { return !plugins::IsOff(PluginAt(i).id); }
void PluginSetEnabled(const std::string& id, bool on) {
    if (MayManage()) plugins::SetEnabled(id, on);
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
CScriptArray* SettingChoices(unsigned i) {
    const auto& choices = SettingAt(i).choices;
    CScriptArray* array = CScriptArray::Create(e->GetTypeInfoByDecl("array<string>"), static_cast<asUINT>(choices.size()));
    for (asUINT k = 0; k < choices.size(); ++k) *static_cast<std::string*>(array->At(k)) = choices[k];
    return array;
}
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
// The plugin manager sets anyone's; a plugin its own (a settings panel of its own, as the ghost viewer's).
bool MaySet(unsigned i) { return MayManage() || (i < settings::List().size() && settings::List()[i].plugin == plugins::Current()); }
bool SettingSet(unsigned i, const std::string& value) { return MaySet(i) && settings::Set(i, value); }
void SettingReset(unsigned i) {
    if (MaySet(i)) settings::Reset(i);
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
float UiFooterHeight() { return static_cast<float>(ui::footer::Height()); }
bool UiScreenSize(float& width, float& height) {
    double w = 0, h = 0;
    if (!game::ScreenSize(&w, &h)) return false;
    width = static_cast<float>(w);
    height = static_cast<float>(h);
    return true;
}

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
void DropdownStyle(ui::Widget* w, float br, float bg, float bb, float ba, float hr, float hg, float hb, float tr, float tg, float tb,
                   float sr, float sg, float sb) {
    w->styled = true;
    w->background = {br, bg, bb, ba};
    w->hoverBackground = {hr, hg, hb, ba};
    w->color = {tr, tg, tb, 1};
    w->colorSet = true;
    w->selectedColor = {sr, sg, sb, 1};
    if (w->window) w->window->layoutDirty = true;          // (a dropdown's styles are read when it is built)
}
void ButtonSize(ui::Widget* w, float width, float height) {
    w->iconW = width;
    w->iconH = height;
    if (w->window) w->window->layoutDirty = true;
}
void ButtonCornerRadius(ui::Widget* w, float radius) {
    w->buttonRadius = radius < 0 ? 0 : radius;
    if (w->window) w->window->layoutDirty = true;          // (its style brushes are set when it is built)
}
void ButtonTextColor(ui::Widget* w, float r, float g, float b, float a) {
    w->color = {r, g, b, a};
    w->colorSet = true;
    w->colorDirty = true;
}
void WinFont(ui::Window* w, const std::string& font, const std::string& typeface) {
    w->font = font;
    w->typeface = typeface;
    w->layoutDirty = true;                                  // (fonts are given when the widgets are built)
}
// Text/Button.SetFont: applied in place; "" (back to the window's or the default font) rebuilds the window. A path
// that isn't a font of the game's (/Game/..., a UFont) is ignored with one warning (widgets::GameFont).
void TextFont(ui::Widget* w, const std::string& font, const std::string& typeface) {
    w->font = font;
    w->typeface = typeface;
    if (font.empty()) {
        if (w->window) w->window->layoutDirty = true;
        return;
    }
    plugins::GameWork work;
    if (eng::Obj text = eng::Get(w->kind == ui::Kind::Button ? w->label : w->main)) ui::widgets::SetFontFace(text, font, typeface);
}
void WinCornerRadius(ui::Window* w, float radius) {
    w->cornerRadius = radius < 0 ? 0 : radius;
    w->layoutDirty = true;
}
// SetPadding, SetRowGap and SetGapBefore: any negative value puts the host's default back.
void WinPadding(ui::Window* w, float x, float y) {
    w->paddingX = x < 0 || y < 0 ? -1 : x;
    w->paddingY = x < 0 || y < 0 ? -1 : y;
    w->layoutDirty = true;
}
void WinRowGap(ui::Window* w, float gap) {
    w->rowGap = gap < 0 ? -1 : gap;
    w->layoutDirty = true;
}
void WinBackground(ui::Window* w, float r, float g, float b, float a) {
    w->background = {r, g, b, a};
    w->layoutDirty = true;
}
bool WinGetVisible(ui::Window* w) { return w->visible; }
void WinSetVisible(ui::Window* w, bool v) { w->visible = v; }
ui::Widget* WinText(ui::Window* w, const std::string& s, float size) { return ui::AddWidget(w, ui::Kind::Text, s, size); }
ui::Widget* WinTextAt(ui::Window* w, const std::string& s, float size, float x, float y) {
    return ui::AddPlaced(w, ui::Kind::Text, s, size, x, y, 0, 0);
}
ui::Widget* WinRectAt(ui::Window* w, float x, float y, float width, float height) {
    return ui::AddPlaced(w, ui::Kind::Rect, "", 0, x, y, width, height);
}
void PlacedMove(ui::Widget* item, float x, float y) { ui::Place(item, x, y, item->pw, item->ph); }
void RectPlace(ui::Widget* item, float x, float y, float width, float height) { ui::Place(item, x, y, width, height); }
void RectColor(ui::Widget* item, float r, float g, float b, float a) {
    if (item->color.r == r && item->color.g == g && item->color.b == b && item->color.a == a) return;
    item->color = {r, g, b, a};
    item->colorDirty = true;
}
ui::Widget* WinButton(ui::Window* w, const std::string& s) { return ui::AddWidget(w, ui::Kind::Button, s, 0); }
ui::Widget* WinIconButton(ui::Window* w, const std::string& icon) { return ui::AddWidget(w, ui::Kind::IconButton, icon, 0); }
ui::Widget* WinSlider(ui::Window* w, float width) { return ui::AddWidget(w, ui::Kind::Slider, "", width); }
ui::Widget* WinDropdown(ui::Window* w, float width) { return ui::AddWidget(w, ui::Kind::Dropdown, "", width); }
void WinSpace(ui::Window* w, float width) { ui::AddWidget(w, ui::Kind::Space, "", width); }
void WinNewRow(ui::Window* w) { ui::NewRow(w); }
void WinStartSidebar(ui::Window* w, float width) { ui::StartSidebar(w, width); }
void WinStartMain(ui::Window* w) { ui::StartMain(w); }
// A place and size in pixels from the top left. On a window already on screen it is applied in place (no rebuild),
// so it can follow the mouse every frame.
void WinRect(ui::Window* w, float x, float y, float width, float height) {
    const bool anchored = w->anchorX == 0 && w->anchorY == 0 && w->pivotX == 0 && w->pivotY == 0 && w->rectWidth > 0;
    w->anchorX = w->anchorY = w->pivotX = w->pivotY = 0;
    w->offsetX = x;
    w->offsetY = y;
    w->rectWidth = std::max(1.0f, width);
    w->rectHeight = std::max(1.0f, height);
    if (anchored) w->rectPending = true;
    else w->layoutDirty = true;
}
void WinScreenSize(ui::Window* w, float width, float height) {
    w->screenWidth = width;
    w->screenHeight = height;
    w->layoutDirty = true;
}
void ButtonBackground(ui::Widget* w, float r, float g, float b, float a) {
    w->backgroundSet = true;
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
void WinSetScrolling(ui::Window* w, int view, bool on) { ui::SetScrolling(w, view, on); }
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
void SetTextWidth(ui::Widget* w, float width) {
    w->textWidth = width;
    w->window->layoutDirty = true;
}
void SetTextFill(ui::Widget* w, bool fill) {
    w->fill = fill;
    w->window->layoutDirty = true;
}
void SetWidgetGapBefore(ui::Widget* w, float gap) {
    w->gapBefore = gap < 0 ? -1 : gap;
    w->window->layoutDirty = true;
}
void SetTextAlign(ui::Widget* w, int align) {
    w->justify = static_cast<uint8_t>(align < 0 ? 0 : align > 2 ? 2 : align);
    w->window->layoutDirty = true;
}
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
void DropdownClear(ui::Widget* w) { ui::ClearOptions(w); }
int DropdownGet(ui::Widget* w) { return w->selected; }
void DropdownSet(ui::Widget* w, int i) {
    if (i >= 0 && i < static_cast<int>(w->options.size())) w->selected = i;
}
bool DropdownChanged(ui::Widget* w) { return TakeFlag(w, &ui::Widget::changedPending); }
bool InputSubmitted(ui::Widget* w) { return TakeFlag(w, &ui::Widget::submitPending); }
std::string InputText(ui::Widget* w) { return w->submitted; }
std::string InputTyped(ui::Widget* w) { return w->typed; }
void HostMaximizeAtStart(int mode) { game::SetMaximizeAtStart(plugins::CurrentId(), mode); }
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
void InputClearButton(ui::Widget* w, bool on) { w->clearButton = on; }
bool InputCleared(ui::Widget* w) {
    const bool c = w->clearedPending;
    w->clearedPending = false;
    return c;
}
void InputReadOnly(ui::Widget* w, bool on) {
    w->readOnly = on;
    w->readOnlyPending = true;
}
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
void WinDockInHub(ui::Window* w) {
    w->dock = ui::Dock::Hub;
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
bool EditorSetOutline(int id, bool on) { return editor::SetOutline(id, on); }
bool EditorScreenPosition(int id, float& x, float& y) {
    double sx = 0, sy = 0;
    const bool ok = editor::ScreenPosition(id, &sx, &sy);
    x = static_cast<float>(sx);
    y = static_cast<float>(sy);
    return ok;
}
float InputWheel() { return static_cast<float>(game::MouseWheel()); }
bool InputMousePosition(float& x, float& y) {
    double mx = 0, my = 0;
    const bool ok = game::MousePosition(&mx, &my);
    x = static_cast<float>(mx);
    y = static_cast<float>(my);
    return ok;
}
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
bool EditorNextClick(int& piece, int& modifiers, bool& wasSelected) {
    editor::Click c;
    const bool got = editor::NextClick(&c);
    piece = c.piece, modifiers = c.modifiers, wasSelected = c.wasSelected;
    return got;
}
CScriptArray* EditorPieces() { return IdArray(editor::Pieces()); }
bool EditorTyping() {
    plugins::GameWork work;
    return ui::Typing() || editor::Typing();
}
std::wstring PluginFile(const std::string& path, bool* ok);
std::string PluginReadFile(const std::string& file);
CScriptArray* Base64Ints(const std::string& b64, int bytes);
// A game texture ("/Game/...") as it is, or a PNG inside the plugins folder; "" if it is neither.
std::string IconPath(const std::string& icon) {
    if (icon.rfind("/Game/", 0) == 0) return icon;
    bool ok = false;
    const std::wstring file = PluginFile(icon, &ok);
    return ok ? eng::Narrow(file.c_str(), static_cast<int>(file.size())) : "";
}
int EditorAddToolbarChoice(const std::string& icon, const CScriptArray* options, int selected) {
    std::vector<std::string> list;
    for (asUINT i = 0; options && i < options->GetSize(); ++i) list.push_back(*static_cast<const std::string*>(options->At(i)));
    return list.empty() ? -1 : editor::AddToolbarChoice(plugins::Current(), IconPath(icon), list, selected);
}
bool EditorSetBudgetLimit(int limit) { return editor::SetBudgetLimit(plugins::Current(), limit); }
void EditorAddHotkey(const std::string& icon, const std::string& label, const std::string& second) {
    editor::AddHotkey(plugins::Current(), IconPath(icon), label, second.empty() ? "" : IconPath(second));
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
    // Math, with the names of AngelScript's own math add-on (in doubles).
    e->SetDefaultNamespace("Math");
    Global("double sin(double)", asFUNCTIONPR(std::sin, (double), double));
    Global("double cos(double)", asFUNCTIONPR(std::cos, (double), double));
    Global("double tan(double)", asFUNCTIONPR(std::tan, (double), double));
    Global("double asin(double)", asFUNCTIONPR(std::asin, (double), double));
    Global("double acos(double)", asFUNCTIONPR(std::acos, (double), double));
    Global("double atan(double)", asFUNCTIONPR(std::atan, (double), double));
    Global("double atan2(double, double)", asFUNCTIONPR(std::atan2, (double, double), double));
    Global("double sqrt(double)", asFUNCTIONPR(std::sqrt, (double), double));
    Global("double pow(double, double)", asFUNCTIONPR(std::pow, (double, double), double));
    Global("double abs(double)", asFUNCTIONPR(std::fabs, (double), double));
    Global("double floor(double)", asFUNCTIONPR(std::floor, (double), double));
    Global("double ceil(double)", asFUNCTIONPR(std::ceil, (double), double));
    e->SetDefaultNamespace("");
    e->SetDefaultNamespace("Log");
    Global("void Info(const string &in)", asFUNCTION(LogInfo));
    Global("void Warn(const string &in)", asFUNCTION(LogWarn));
    Global("void Error(const string &in)", asFUNCTION(LogError));
    Global("uint LineCount()", asFUNCTION(LogLineCount));
    Global("string Line(uint)", asFUNCTION(LogLine));

    e->SetDefaultNamespace("Host");
    Global("string Version()", asFUNCTION(HostVersion));
    Global("double Time()", asFUNCTION(HostTime));
    Global("int MapNumber()", asFUNCTION(game::Generation));
    Global("bool WindowMaximized()", asFUNCTION(game::WindowMaximized));
    Global("bool WindowFitsScreen()", asFUNCTION(game::WindowFitsScreen));
    Global("bool MaximizeWindow()", asFUNCTION(game::MaximizeWindow));
    Global("void MaximizeAtStart(int mode)", asFUNCTION(HostMaximizeAtStart));
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
    Global("bool Enabled(uint)", asFUNCTION(PluginEnabled));
    Global("void SetEnabled(const string &in id, bool)", asFUNCTION(PluginSetEnabled));
    Global("bool IsInstalled(const string &in id)", asFUNCTION(PluginInstalled));
    Global("string InstalledVersion(const string &in id)", asFUNCTION(PluginInstalledVersion));
    Global("void Install(const string &in id)", asFUNCTION(PluginInstall));
    Global("void Remove(const string &in id)", asFUNCTION(PluginRemove));
    Global("string Pending(const string &in id)", asFUNCTION(registry::Pending));
    Global("string DefaultIcon()", asFUNCTION(registry::DefaultIcon));
    Global("void OpenFolder()", asFUNCTION(plugins::OpenFolder));
    Global("string Folder()", asFUNCTION(PluginFolder));
    Global("string ReadFile(const string &in file)", asFUNCTION(PluginReadFile));
    Global("array<int>@ Base64Ints(const string &in b64, int bytes)", asFUNCTION(Base64Ints));
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
    Global("array<string>@ Choices(uint)", asFUNCTION(SettingChoices));
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
    for (const char* type : {"FooterButton", "Panel", "Window", "Text", "Button", "Slider", "Dropdown", "TextArea", "TextInput", "Image", "CheckBox", "Rect"})
        Check(e->RegisterObjectType(type, 0, asOBJ_REF | asOBJ_NOCOUNT), type);
    Global("void SetCursorVisible(bool)", asFUNCTION(SetCursorVisible));
    Global("bool ScreenSize(float &out, float &out)", asFUNCTION(UiScreenSize));
    Global("float FooterHeight()", asFUNCTION(UiFooterHeight));
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
    Method("Window", "void SetCornerRadius(float)", asFUNCTION(WinCornerRadius));
    Method("Window", "void SetPadding(float x, float y)", asFUNCTION(WinPadding));
    Method("Window", "void SetRowGap(float)", asFUNCTION(WinRowGap));
    Method("Window", "void SetFont(const string &in font, const string &in typeface = \"\")", asFUNCTION(WinFont));
    Method("Text", "void SetFont(const string &in font, const string &in typeface = \"\")", asFUNCTION(TextFont));
    Method("Button", "void SetTextColor(float, float, float, float)", asFUNCTION(ButtonTextColor));
    Method("Button", "void SetCornerRadius(float)", asFUNCTION(ButtonCornerRadius));
    Method("Button", "void SetSize(float width, float height)", asFUNCTION(ButtonSize));
    Method("Dropdown", "void SetStyle(float r, float g, float b, float a, float hoverR, float hoverG, float hoverB, float textR, float textG, float textB, float selectedR, float selectedG, float selectedB)", asFUNCTION(DropdownStyle));
    Method("Dropdown", "bool get_hovered() property", asFUNCTION(Hovered));
    Method("Button", "void SetFont(const string &in font, const string &in typeface = \"\")", asFUNCTION(TextFont));
    Method("Window", "bool get_visible() property", asFUNCTION(WinGetVisible));
    Method("Window", "void set_visible(bool) property", asFUNCTION(WinSetVisible));
    Method("Window", "Text@ AddText(const string &in, float size = 16)", asFUNCTION(WinText));
    Method("Window", "Text@ AddTextAt(const string &in, float size, float x, float y)", asFUNCTION(WinTextAt));
    Method("Window", "Rect@ AddRect(float x, float y, float width, float height)", asFUNCTION(WinRectAt));
    Method("Window", "Button@ AddButton(const string &in)", asFUNCTION(WinButton));
    Method("Window", "Button@ AddIconButton(const string &in)", asFUNCTION(WinIconButton));
    Method("Window", "Slider@ AddSlider(float)", asFUNCTION(WinSlider));
    Method("Window", "Dropdown@ AddDropdown(float)", asFUNCTION(WinDropdown));
    Method("Window", "void AddSpace(float)", asFUNCTION(WinSpace));
    Method("Window", "void NewRow()", asFUNCTION(WinNewRow));
    Method("Window", "void StartSidebar(float width)", asFUNCTION(WinStartSidebar));
    Method("Window", "void StartMain()", asFUNCTION(WinStartMain));
    Method("Window", "void SetScreenSize(float width, float height)", asFUNCTION(WinScreenSize));
    Method("Window", "void SetRect(float x, float y, float width, float height)", asFUNCTION(WinRect));
    Method("Window", "int StartView()", asFUNCTION(WinStartView));
    Method("Window", "void SetBlocksClicks(bool)", asFUNCTION(WinBlocksClicks));
    Method("Window", "CheckBox@ AddCheckBox(const string &in label, float size = 16)", asFUNCTION(WinCheckBox));
    Method("Window", "void DockInEditorDetails()", asFUNCTION(WinDockInEditorDetails));
    Method("Window", "void DockInHub()", asFUNCTION(WinDockInHub));
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
    Method("Window", "void SetScrolling(int, bool)", asFUNCTION(WinSetScrolling));
    Method("Window", "Image@ AddImage(const string &in path, float width, float height)", asFUNCTION(WinImage));
    Method("Window", "TextArea@ AddTextArea(float width, float height, float size = 14)", asFUNCTION(WinTextArea));
    Method("Window", "TextInput@ AddTextInput(float width, const string &in hint = \"\", float size = 18)", asFUNCTION(WinTextInput));

    Method("Text", "void set_text(const string &in) property", asFUNCTION(SetWidgetText));
    Method("Text", "string get_text() property", asFUNCTION(GetWidgetText));
    Method("Text", "void SetColor(float, float, float, float)", asFUNCTION(TextColor));
    Method("Text", "void SetPosition(float, float)", asFUNCTION(PlacedMove));
    Method("Rect", "void SetRect(float x, float y, float width, float height)", asFUNCTION(RectPlace));
    Method("Rect", "void SetColor(float, float, float, float)", asFUNCTION(RectColor));
    for (const char* type : {"Text", "Button", "Slider", "Dropdown", "TextArea", "TextInput", "Image", "Rect"}) {
        Method(type, "void set_visible(bool) property", asFUNCTION(SetWidgetVisible));
        Method(type, "bool get_visible() property", asFUNCTION(GetWidgetVisible));
    }
    Method("Text", "void set_size(float) property", asFUNCTION(SetTextSize));
    Method("Text", "float get_size() property", asFUNCTION(GetTextSize));
    Method("Text", "void SetWidth(float)", asFUNCTION(SetTextWidth));
    Method("Text", "void SetAlign(int)", asFUNCTION(SetTextAlign));
    Method("Text", "void SetFill(bool)", asFUNCTION(SetTextFill));
    Method("Button", "bool Clicked()", asFUNCTION(Clicked));
    Method("Button", "bool get_hovered() property", asFUNCTION(Hovered));
    Method("Button", "void SetBackground(float, float, float, float)", asFUNCTION(ButtonBackground));
    Method("Button", "void set_label(const string &in) property", asFUNCTION(SetWidgetText));
    Method("Button", "void set_icon(const string &in) property", asFUNCTION(SetWidgetText));
    Method("Slider", "float get_value() property", asFUNCTION(SliderGet));
    Method("Slider", "void set_value(float) property", asFUNCTION(SliderSet));
    Method("Slider", "bool get_dragging() property", asFUNCTION(SliderDragging));
    Method("Dropdown", "void AddOption(const string &in)", asFUNCTION(DropdownAdd));
    Method("Dropdown", "void ClearOptions()", asFUNCTION(DropdownClear));
    Method("Dropdown", "int get_selected() property", asFUNCTION(DropdownGet));
    Method("Dropdown", "void set_selected(int) property", asFUNCTION(DropdownSet));
    Method("Dropdown", "bool Changed()", asFUNCTION(DropdownChanged));
    Method("Image", "void set_path(const string &in) property", asFUNCTION(SetWidgetText));
    Method("Image", "string get_path() property", asFUNCTION(GetWidgetText));
    Method("TextArea", "void set_text(const string &in) property", asFUNCTION(SetWidgetText));
    Method("TextArea", "string get_text() property", asFUNCTION(GetWidgetText));
    Method("TextInput", "bool Submitted()", asFUNCTION(InputSubmitted));
    Method("TextInput", "string get_text() property", asFUNCTION(InputText));
    Method("TextInput", "string get_typed() property", asFUNCTION(InputTyped));
    Method("TextInput", "bool get_focused() property", asFUNCTION(InputFocused));
    Method("TextInput", "void Focus()", asFUNCTION(InputFocus));
    Method("TextInput", "void Submit()", asFUNCTION(InputSubmit));
    Method("TextInput", "void set_value(const string &in) property", asFUNCTION(InputSetValue));
    Method("TextInput", "void set_clearOnSubmit(bool) property", asFUNCTION(InputClearOnSubmit));
    // an x at the box's right end that empties it (set it straight after AddTextInput, before the window is shown)
    Method("TextInput", "void set_clearButton(bool) property", asFUNCTION(InputClearButton));
    Method("TextInput", "bool Cleared()", asFUNCTION(InputCleared));
    Method("TextInput", "void set_readOnly(bool) property", asFUNCTION(InputReadOnly));
    Method("CheckBox", "bool get_checked() property", asFUNCTION(CheckGet));
    Method("CheckBox", "void set_checked(bool) property", asFUNCTION(CheckSet));
    Method("CheckBox", "bool Changed()", asFUNCTION(CheckChanged));
    Method("CheckBox", "void SetColor(float, float, float, float)", asFUNCTION(CheckColor));
    Method("CheckBox", "void set_visible(bool) property", asFUNCTION(SetWidgetVisible));
    Method("CheckBox", "bool get_visible() property", asFUNCTION(GetWidgetVisible));
    for (const char* type : {"Text", "Button", "Slider", "Dropdown", "TextArea", "TextInput", "Image", "CheckBox"}) {
        Method(type, "void SetGapBefore(float)", asFUNCTION(SetWidgetGapBefore));
    }
}

// Keys are Windows virtual-key codes, and controller buttons after them (input.hpp); the common ones are named,
// plus A-Z and N0-N9.
const std::vector<std::pair<std::string, int>>& KeyNames() {
    static std::vector<std::pair<std::string, int>> names;
    if (!names.empty()) return names;
    names = {{"Space", 0x20}, {"Enter", 0x0D}, {"Escape", 0x1B}, {"Tab", 0x09}, {"Shift", 0x10}, {"Ctrl", 0x11}, {"Alt", 0x12}, {"Win", 0x5B}, {"RightWin", 0x5C},
             {"Left", 0x25}, {"Up", 0x26}, {"Right", 0x27}, {"Down", 0x28}, {"MouseLeft", 0x01}, {"MouseRight", 0x02},
             {"MouseMiddle", 0x04}, {"MouseBack", 0x05}, {"MouseForward", 0x06}, {"Backspace", 0x08}, {"PageUp", 0x21},
             {"PageDown", 0x22}, {"End", 0x23}, {"Home", 0x24}, {"Insert", 0x2D}, {"Delete", 0x2E}, {"Minus", 0xBD},
             {"Equals", 0xBB}, {"LeftBracket", 0xDB}, {"RightBracket", 0xDD}, {"Semicolon", 0xBA}, {"Quote", 0xDE},
             {"Comma", 0xBC}, {"Period", 0xBE}, {"Slash", 0xBF}, {"Backslash", 0xDC}, {"Tilde", 0xC0},
             {"NumpadPlus", 0x6B}, {"NumpadMinus", 0x6D}, {"NumpadMultiply", 0x6A}, {"NumpadDivide", 0x6F},
             {"F1", 0x70}, {"F2", 0x71}, {"F3", 0x72}, {"F4", 0x73}, {"F5", 0x74}, {"F6", 0x75},
             {"F7", 0x76}, {"F8", 0x77}, {"F9", 0x78}, {"F10", 0x79}, {"F11", 0x7A}, {"F12", 0x7B},
             {"PadA", input::kPadA}, {"PadB", input::kPadB}, {"PadX", input::kPadX}, {"PadY", input::kPadY},
             {"PadLB", input::kPadLB}, {"PadRB", input::kPadRB}, {"PadLT", input::kPadLT}, {"PadRT", input::kPadRT},
             {"PadL3", input::kPadL3}, {"PadR3", input::kPadR3}, {"PadView", input::kPadView}, {"PadMenu", input::kPadMenu},
             {"PadUp", input::kPadUp}, {"PadDown", input::kPadDown}, {"PadLeft", input::kPadLeft}, {"PadRight", input::kPadRight},
             {"PadStickUp", input::kPadStickUp}, {"PadStickDown", input::kPadStickDown}, {"PadStickLeft", input::kPadStickLeft},
             {"PadStickRight", input::kPadStickRight}};
    for (char c = 'A'; c <= 'Z'; ++c) names.push_back({std::string(1, c), c});
    for (char c = '0'; c <= '9'; ++c) names.push_back({std::string("N") + c, c});
    for (int i = 0; i < 10; ++i) names.push_back({"Numpad" + std::to_string(i), 0x60 + i});
    return names;
}
std::string KeyName(int key) {
    for (const auto& [name, code] : KeyNames())
        if (code == key) return name;
    return key ? "Key" + std::to_string(key) : "";
}
int AnyKeyPressed() { return ui::Typing() ? 0 : input::AnyPressed(); }

void RegisterInput() {
    e->SetDefaultNamespace("Input");
    Check(e->RegisterEnum("Key"), "Input::Key");
    Check(e->RegisterEnumValue("Key", "None", 0), "None");
    for (const auto& [name, code] : KeyNames()) Check(e->RegisterEnumValue("Key", name.c_str(), code), name.c_str());
    Global("bool Pressed(Key)", asFUNCTION(KeyPressed));
    Global("bool Down(Key)", asFUNCTION(KeyDown));
    Global("bool MousePosition(float &out, float &out)", asFUNCTION(InputMousePosition));
    Global("float Wheel()", asFUNCTION(InputWheel));
    Global("Key AnyPressed()", asFUNCTION(AnyKeyPressed));
    Global("string Name(Key)", asFUNCTION(KeyName));
}

std::string RaceTrackKey() { return race::CurrentTrack().key; }
bool RaceBallPosition(double& x, double& y, double& z) { return race::BallPosition(&x, &y, &z); }
bool RaceCheckpointPosition(int index, double& x, double& y, double& z) { return race::CheckpointPosition(index, &x, &y, &z); }
std::string RaceTrackName() { return race::CurrentTrack().name; }
std::string RaceTrackAuthor() { return race::CurrentTrack().author; }
std::string RaceTrackImage() {
    const race::Track& t = race::CurrentTrack();
    return t.custom ? t.image : t.key.rfind("map:", 0) == 0 ? tracks::Image(t.key.substr(4)) : "";
}
double RaceAuthorTime() { return race::CurrentTrack().authorTime; }
bool RaceCustomTrack() { return race::CurrentTrack().custom; }
bool RaceInput(double& x, double& y, bool& jump) { return race::Input(&x, &y, &jump); }
bool RaceSetPaused(bool paused) {
    plugins::GameWork work;
    return race::SetPaused(paused);
}
bool RacePaused() { return race::Paused(); }
void RaceHideBall(bool hidden) { hud::HideBall(plugins::Current(), hidden); }
void RaceFreezeBall(bool frozen) { hud::FreezeBall(plugins::Current(), frozen); }
std::string RaceSaveBall() {
    plugins::GameWork work;
    return race::SaveBall();
}
bool RaceLoadBall(const std::string& state, bool momentum) {
    plugins::GameWork work;
    return race::LoadBall(state, momentum);
}
void RaceStartPractice() {
    plugins::GameWork work;
    race::StartPractice();
}

// Hud: Elements() takes a snapshot the other getters answer from, so a plugin walking the list sees one HUD.
std::vector<hud::Element> gHudSnapshot;
const hud::Element* HudFind(const std::string& key) {
    for (const auto& el : gHudSnapshot)
        if (el.key == key) return &el;
    return nullptr;
}
CScriptArray* HudElements() {
    plugins::GameWork work;
    gHudSnapshot = hud::Elements();
    CScriptArray* array = CScriptArray::Create(e->GetTypeInfoByDecl("array<string>"), static_cast<asUINT>(gHudSnapshot.size()));
    for (size_t i = 0; i < gHudSnapshot.size(); ++i) *static_cast<std::string*>(array->At(static_cast<asUINT>(i))) = gHudSnapshot[i].key;
    return array;
}
std::string HudName(const std::string& key) { const auto* el = HudFind(key); return el ? el->name : ""; }
std::string HudLabel(const std::string& key) { const auto* el = HudFind(key); return el ? el->label : ""; }
bool HudShown(const std::string& key) { const auto* el = HudFind(key); return el && el->shown; }
bool HudParentShown(const std::string& key) { const auto* el = HudFind(key); return el && el->parentShown; }
void HudSetLayout(const std::string& key, double x, double y, double scale, int mode) { hud::SetLayout(key, x, y, scale, mode); }
void HudClearLayout(const std::string& key) { hud::ClearLayout(key); }
void HudSetEditing(bool on) { hud::SetEditing(on); }
void HudSetBlink(const std::string& key) { hud::SetBlink(key); }
bool HudSetPartColor(const std::string& key, const std::string& part, float r, float g, float b, float a) {
    return hud::SetPartColor(key, part, r, g, b, a);
}
void HudResetPartColor(const std::string& key, const std::string& part) { hud::ResetPartColor(key, part); }

bool RaceNextBounce(double& strength, double& x, double& y, double& z, double& nx, double& ny, double& nz, bool& ground);

void RegisterRace() {
    e->SetDefaultNamespace("Race");
    Global("bool OnTrack()", asFUNCTION(race::OnTrack));
    Global("bool IsActive()", asFUNCTION(race::Active));
    Global("int Restarts()", asFUNCTION(race::Restarts));
    Global("int Respawns()", asFUNCTION(race::Respawns));
    Global("int Falls()", asFUNCTION(race::Falls));
    Global("int CheckpointCount()", asFUNCTION(race::CheckpointCount));
    Global("bool CheckpointPosition(int, double &out, double &out, double &out)", asFUNCTION(RaceCheckpointPosition));
    Global("int CurrentCheckpoint()", asFUNCTION(race::CurrentCheckpoint));
    Global("int RunId()", asFUNCTION(race::RunId));
    Global("bool IsComplete()", asFUNCTION(race::Complete));
    Global("string TrackKey()", asFUNCTION(RaceTrackKey));
    Global("string TrackName()", asFUNCTION(RaceTrackName));
    Global("string TrackAuthor()", asFUNCTION(RaceTrackAuthor));
    Global("string TrackImage()", asFUNCTION(RaceTrackImage));
    Global("double AuthorTime()", asFUNCTION(RaceAuthorTime));
    Global("bool IsCustomTrack()", asFUNCTION(RaceCustomTrack));
    Global("bool GetInput(double &out, double &out, bool &out)", asFUNCTION(RaceInput));
    Global("bool SetPaused(bool)", asFUNCTION(RaceSetPaused));
    Global("bool IsPaused()", asFUNCTION(RacePaused));
    Global("string SaveBall()", asFUNCTION(RaceSaveBall));
    Global("bool LoadBall(const string &in, bool momentum = true)", asFUNCTION(RaceLoadBall));
    Global("void StartPractice()", asFUNCTION(RaceStartPractice));
    Global("bool IsPractice()", asFUNCTION(race::Practice));
    Global("void HideBall(bool)", asFUNCTION(RaceHideBall));
    Global("void FreezeBall(bool)", asFUNCTION(RaceFreezeBall));
    Global("bool BallPosition(double &out, double &out, double &out)", asFUNCTION(RaceBallPosition));
    Global("bool NextBounce(double &out strength, double &out x, double &out y, double &out z, double &out nx, double &out ny, "
           "double &out nz, bool &out ground)", asFUNCTION(RaceNextBounce));
}

void HudHideGame(bool hidden) { hud::HideGame(plugins::Current(), hidden); }
void HudHideEditor(bool hidden) { hud::HideEditor(plugins::Current(), hidden); }

void RegisterHud() {
    e->SetDefaultNamespace("Hud");
    Check(e->RegisterEnum("Mode"), "Hud::Mode");
    Check(e->RegisterEnumValue("Mode", "Normal", hud::kNormal), "Normal");
    Check(e->RegisterEnumValue("Mode", "Off", hud::kOff), "Off");
    Check(e->RegisterEnumValue("Mode", "On", hud::kOn), "On");
    Global("array<string>@ Elements()", asFUNCTION(HudElements));
    Global("string Name(const string &in)", asFUNCTION(HudName));
    Global("string Label(const string &in)", asFUNCTION(HudLabel));
    Global("bool Shown(const string &in)", asFUNCTION(HudShown));
    Global("bool ParentShown(const string &in)", asFUNCTION(HudParentShown));
    Global("void SetLayout(const string &in, double, double, double, Mode = Normal)", asFUNCTION(HudSetLayout));
    Global("void ClearLayout(const string &in)", asFUNCTION(HudClearLayout));
    Global("void SetEditing(bool)", asFUNCTION(HudSetEditing));
    Global("void SetBlink(const string &in)", asFUNCTION(HudSetBlink));
    Global("bool SetPartColor(const string &in, const string &in, float, float, float, float)", asFUNCTION(HudSetPartColor));
    Global("void ResetPartColor(const string &in, const string &in)", asFUNCTION(HudResetPartColor));
    Global("void HideGame(bool)", asFUNCTION(HudHideGame));
    Global("void HideEditor(bool)", asFUNCTION(HudHideEditor));
}

void RegisterEditor() {
    e->SetDefaultNamespace("Editor");
    Global("bool IsOpen()", asFUNCTION(editor::Open));
    Global("bool IsTesting()", asFUNCTION(race::EditorTesting));
    Global("array<int>@ Selection()", asFUNCTION(EditorSelection));
    Global("array<int>@ Placed()", asFUNCTION(EditorPlaced));
    Global("bool GetLocation(int, double &out, double &out, double &out)", asFUNCTION(EditorLocation));
    Global("bool GetRotation(int, double &out, double &out, double &out)", asFUNCTION(EditorRotation));
    Global("bool SetLocation(int, double, double, double)", asFUNCTION(EditorSetLocation));
    Global("bool SetRotation(int, double, double, double)", asFUNCTION(EditorSetRotation));
    Global("void ViewForward(double &out, double &out, double &out)", asFUNCTION(EditorViewForward));
    Global("bool ScreenPosition(int, float &out, float &out)", asFUNCTION(EditorScreenPosition));
    Global("bool SetOutline(int, bool)", asFUNCTION(EditorSetOutline));
    Global("void Select(const array<int>@)", asFUNCTION(EditorSelect));
    Global("array<int>@ DuplicateSelection()", asFUNCTION(EditorDuplicate));
    Global("void RotatePieces(const array<int>@, double, double, double, double, double, double)", asFUNCTION(EditorRotatePieces));
    Global("void SetTabCycling(bool)", asFUNCTION(editor::SetTabCycling));
    Global("void SetRotateAroundCenter(bool)", asFUNCTION(editor::SetRotateAroundCenter));
    Check(e->RegisterEnum("RotateMode"), "Editor::RotateMode");
    Check(e->RegisterEnumValue("RotateMode", "RotateDefault", editor::kRotateDefault), "RotateDefault");
    Check(e->RegisterEnumValue("RotateMode", "RotateAroundCenter", editor::kRotateAroundCenter), "RotateAroundCenter");
    Check(e->RegisterEnumValue("RotateMode", "RotateMirrored", editor::kRotateMirrored), "RotateMirrored");
    Global("void SetRotateMode(RotateMode)", asFUNCTION(editor::SetRotateMode));
    Global("RotateMode GetRotateMode()", asFUNCTION(editor::RotateMode));
    Check(e->RegisterEnum("ClickFlag"), "Editor::ClickFlag");
    Check(e->RegisterEnumValue("ClickFlag", "ClickShift", editor::kClickShift), "ClickShift");
    Check(e->RegisterEnumValue("ClickFlag", "ClickCtrl", editor::kClickCtrl), "ClickCtrl");
    Check(e->RegisterEnumValue("ClickFlag", "ClickAlt", editor::kClickAlt), "ClickAlt");
    Check(e->RegisterEnumValue("ClickFlag", "ClickOnGizmo", editor::kClickOnGizmo), "ClickOnGizmo");
    Global("bool NextClick(int &out, int &out, bool &out)", asFUNCTION(EditorNextClick));
    Global("array<int>@ Pieces()", asFUNCTION(EditorPieces));
    Global("string PieceClass(int)", asFUNCTION(editor::PieceClass));
    Global("string MapName()", asFUNCTION(editor::MapName));
    Global("bool Typing()", asFUNCTION(EditorTyping));
    Global("int AddToolbarChoice(const string &in, const array<string>@, int = 0)", asFUNCTION(EditorAddToolbarChoice));
    Global("int ToolbarChoice(int)", asFUNCTION(editor::ToolbarChoiceSelected));
    Global("void SetToolbarChoice(int, int)", asFUNCTION(editor::SetToolbarChoiceSelected));
    Global("void AddHotkey(const string &in, const string &in, const string &in = \"\")", asFUNCTION(EditorAddHotkey));
    Global("int BudgetLimit()", asFUNCTION(editor::BudgetLimit));
    Global("int BudgetUsed()", asFUNCTION(editor::BudgetUsed));
    Global("bool SetBudgetLimit(int)", asFUNCTION(EditorSetBudgetLimit));
}

// --- Cosmetics -----------------------------------------------------------------------------------------------------
// Image files must be inside the plugins folder: a plugin gives its own with Plugins::Folder().
std::wstring PluginFile(const std::string& path, bool* ok) {
    *ok = true;
    if (path.empty()) return L"";
    wchar_t full[MAX_PATH];
    const std::wstring wide = eng::Widen(path);
    const DWORD n = GetFullPathNameW(wide.c_str(), MAX_PATH, full, nullptr);
    const std::wstring root = plugins::Dir() + L"\\";
    const std::wstring resolved(full, n > 0 && n < MAX_PATH ? n : 0);
    if (resolved.empty() || _wcsnicmp(resolved.c_str(), root.c_str(), root.size()) != 0 ||
        GetFileAttributesW(resolved.c_str()) == INVALID_FILE_ATTRIBUTES) {
        hostlog::Write("warn", plugins::CurrentId(), "cosmetics: " + path + " is not a file in the plugins folder");
        *ok = false;
    }
    return resolved;
}
// A model file (models.hpp's format) in the plugins folder, read whole; "" for none. A 3D model file (.glb, .gltf,
// .obj) is a model of that mesh alone. The files a model's "mesh" lines name are found next to it, must be in the
// plugins folder too, and are passed on by their full path.
bool IsMeshFile(const std::string& path) {
    std::string lower = path;
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const char* ext : {".glb", ".gltf", ".obj"})
        if (lower.size() > std::strlen(ext) && lower.compare(lower.size() - std::strlen(ext), std::string::npos, ext) == 0) return true;
    return false;
}
bool ModelText(const std::string& path, std::string* text, bool hat = false) {
    text->clear();
    if (path.empty()) return true;
    bool ok = false;
    const std::wstring file = PluginFile(path, &ok);
    if (!ok) return false;
    if (IsMeshFile(path)) {
        // On a ball: standing on its bottom, 80 cm across; on a hat: on the hat slot, 40 cm across.
        *text = "mesh \"" + eng::Narrow(file.c_str(), static_cast<int>(file.size())) + (hat ? "\" size=40\n" : "\" size=80 at=0,0,-44\n");
        return true;
    }
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::string raw;
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(h, buf, sizeof buf, &n, nullptr) && n > 0 && raw.size() < 256 * 1024) raw.append(buf, n);
    CloseHandle(h);
    const std::string folder = eng::Narrow(file.c_str(), static_cast<int>(file.find_last_of(L"\\/") + 1));
    std::stringstream lines(raw);
    std::string line;
    while (std::getline(lines, line)) {
        const size_t first = line.find_first_not_of(" \t");
        if (first != std::string::npos && line.compare(first, 5, "mesh ") == 0) {
            size_t at = line.find_first_not_of(" \t", first + 4), end;
            std::string name;
            if (at != std::string::npos && line[at] == '"') {
                end = line.find('"', at + 1);
                if (end == std::string::npos) end = line.size() - 1;
                name = line.substr(at + 1, end - at - 1);
                ++end;
            } else if (at != std::string::npos) {
                end = line.find_first_of(" \t\r", at);
                if (end == std::string::npos) end = line.size();
                name = line.substr(at, end - at);
            } else {
                end = line.size();
            }
            const bool absolute = name.size() > 1 && name[1] == ':';
            bool inside = false;
            const std::wstring resolved = PluginFile(absolute ? name : folder + name, &inside);
            if (!inside) return false;
            line = line.substr(0, first) + "mesh \"" + eng::Narrow(resolved.c_str(), static_cast<int>(resolved.size())) + "\"" + line.substr(end);
        }
        *text += line + "\n";
    }
    return true;
}
bool CosmeticsAddBall(const std::string& id, const std::string& name, const std::string& image, const std::string& preview,
                      const std::string& model) {
    bool imageOk = false, previewOk = false;
    std::string text;
    const std::wstring file = PluginFile(image, &imageOk), picture = PluginFile(preview, &previewOk);
    return imageOk && previewOk && ModelText(model, &text) && cosmetics::AddBall(id, name, file, picture, text);   // image "": clear
}
bool CosmeticsAddHat(const std::string& id, const std::string& name, const std::string& mesh, double scale,
                     const std::string& preview, const std::string& model) {
    bool ok = false;
    std::string text;
    const std::wstring picture = PluginFile(preview, &ok);
    return ok && scale > 0 && ModelText(model, &text, true) && cosmetics::AddHat(id, name, mesh, scale, picture, text);
}
bool CosmeticsAddBfx(const std::string& id, const std::string& name, const std::string& base, double scale,
                     const std::string& preview, const std::string& system, const std::string& sound) {
    bool ok = false;
    const std::wstring picture = PluginFile(preview, &ok);
    return ok && scale > 0 && cosmetics::AddBfx(id, name, base, scale, picture, system, sound);
}
bool CosmeticsAddExtra(const std::string& slot, const std::string& id, const std::string& name, const std::string& preview,
                       const std::string& model) {
    bool ok = false;
    std::string text;
    const std::wstring picture = PluginFile(preview, &ok);
    return ok && ModelText(model, &text) && cosmetics::AddExtra(slot, id, name, picture, text);
}
bool CosmeticsEquipExtra(const std::string& slot, const std::string& id) { return cosmetics::EquipExtra(slot, id); }
std::string CosmeticsEquippedExtra(const std::string& slot) { return cosmetics::EquippedExtra(slot); }
bool CosmeticsPreviewBall(double& x, double& y, double& z, double& radius, double& facing) {
    return cosmetics::PreviewBall(&x, &y, &z, &radius, &facing);
}
cosmetics::Kind KindOf(int kind) { return static_cast<cosmetics::Kind>(kind < 0 || kind > 2 ? 0 : kind); }
int CosmeticsCount(int kind) { return cosmetics::Count(KindOf(kind)); }
bool CosmeticsEquip(int kind, const std::string& id) { return kind >= 0 && kind <= 2 && cosmetics::Equip(KindOf(kind), id); }
std::string CosmeticsEquipped(int kind) { return kind >= 0 && kind <= 2 ? cosmetics::Equipped(KindOf(kind)) : ""; }

void LeaderboardNote(const std::string& note) { leaderboard::SetTitleNote(plugins::Current(), note); }
void LeaderboardOverallNote(const std::string& note) { leaderboard::SetOverallNote(plugins::Current(), note); }

void RegisterLeaderboard() {
    e->SetDefaultNamespace("Leaderboard");
    Global("int Players()", asFUNCTION(leaderboard::Players));
    Global("void SetTitleNote(const string &in)", asFUNCTION(LeaderboardNote));
    Global("int OverallPlayers()", asFUNCTION(leaderboard::OverallPlayers));
    Global("void SetOverallNote(const string &in)", asFUNCTION(LeaderboardOverallNote));
}

void RegisterCosmetics() {
    e->SetDefaultNamespace("Cosmetics");
    Check(e->RegisterEnum("Kind"), "Cosmetics::Kind");
    Check(e->RegisterEnumValue("Kind", "Ball", 0), "Ball");
    Check(e->RegisterEnumValue("Kind", "Hat", 1), "Hat");
    Check(e->RegisterEnumValue("Kind", "Bfx", 2), "Bfx");
    Global("bool AddBall(const string &in, const string &in, const string &in, const string &in = \"\", const string &in = \"\")",
           asFUNCTION(CosmeticsAddBall));
    Global("bool AddHat(const string &in, const string &in, const string &in, double, const string &in = \"\", const string &in = \"\")",
           asFUNCTION(CosmeticsAddHat));
    Global("bool AddBfx(const string &in, const string &in, const string &in, double, const string &in = \"\", const string &in = \"\", "
           "const string &in = \"\")",
           asFUNCTION(CosmeticsAddBfx));
    Global("int Count(Kind)", asFUNCTION(CosmeticsCount));
    Global("bool Equip(Kind, const string &in)", asFUNCTION(CosmeticsEquip));
    Global("string Equipped(Kind)", asFUNCTION(CosmeticsEquipped));
    Global("bool AddExtra(const string &in, const string &in, const string &in, const string &in, const string &in)",
           asFUNCTION(CosmeticsAddExtra));
    Global("bool EquipExtra(const string &in, const string &in)", asFUNCTION(CosmeticsEquipExtra));
    Global("string EquippedExtra(const string &in)", asFUNCTION(CosmeticsEquippedExtra));
    Global("bool PreviewBall(double &out x, double &out y, double &out z, double &out radius, double &out facing)",
           asFUNCTION(CosmeticsPreviewBall));
}

// --- ghosts, drawing and the camera -------------------------------------------------------------------------------
const ghosts::Ghost* GhostAt(int i) {
    const auto& all = ghosts::All();
    return i >= 0 && static_cast<size_t>(i) < all.size() ? &all[static_cast<size_t>(i)] : nullptr;
}
bool GhostsLoad(const std::string& leaderboard, int count) { return ghosts::Load(leaderboard, count); }
// A file of the calling plugin's own folder ("" if the name leaves it or the file can't be read).
// Refused: "..", a drive or stream (":"), a leading slash, a NUL, and Windows device names (CON, NUL, COM1...) in any part.
bool DeviceName(std::string part) {
    part = part.substr(0, part.find('.'));
    while (!part.empty() && (part.back() == ' ')) part.pop_back();
    for (char& ch : part) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    if (part == "CON" || part == "PRN" || part == "AUX" || part == "NUL" || part == "CONIN$" || part == "CONOUT$") return true;
    return part.size() == 4 && (part.rfind("COM", 0) == 0 || part.rfind("LPT", 0) == 0) && part[3] >= '0' && part[3] <= '9';
}
std::wstring OwnFile(const std::string& file) {
    if (file.empty() || file.find("..") != std::string::npos || file.find(':') != std::string::npos || file[0] == 92 || file[0] == 47 ||
        file.find('\0') != std::string::npos)
        return L"";
    for (size_t a = 0; a <= file.size();) {
        const size_t b = file.find_first_of("/\\", a);
        if (DeviceName(file.substr(a, b == std::string::npos ? std::string::npos : b - a))) return L"";
        if (b == std::string::npos) break;
        a = b + 1;
    }
    const std::wstring dir = plugins::CurrentDir();
    return dir.empty() ? L"" : dir + L"\\" + eng::Widen(file);
}
bool GhostsCrowdTrailStyle(int id, bool glow, float brightness, double fadeSeconds, float fadeMin) {
    return ghosts::CrowdTrailStyle(plugins::Current(), id, glow, brightness, fadeSeconds, fadeMin);
}
bool GhostsCrowdGroupGlass(int id, int group, float opacity) { return ghosts::CrowdGroupGlass(plugins::Current(), id, group, opacity); }
bool GhostsCrowdTrailsPending() { return ghosts::CrowdTrailsPending(plugins::Current()); }
bool GhostsCrowdBallScale(int id, double scale) { return ghosts::CrowdBallScale(plugins::Current(), id, scale); }
void GhostsPrefetchFile(const std::string& file) {
    const std::wstring path = OwnFile(file);
    if (!path.empty()) ghosts::PrefetchLocal(path);
}
bool GhostsPrefetchReady(const std::string& file) {
    const std::wstring path = OwnFile(file);
    return !path.empty() && ghosts::PrefetchReady(path);
}
int GhostsLoadFile(const std::string& file) {
    const std::wstring path = OwnFile(file);
    if (path.empty()) return -1;
    plugins::GameWork work;
    return ghosts::LoadLocal(path);
}
std::string PluginReadFile(const std::string& file) {
    const std::wstring path = OwnFile(file);
    if (path.empty()) return "";
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return "";
    std::string text;
    char buf[65536];
    for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
    fclose(f);
    std::erase(text, '\r');                     // (Windows line ends: scripts split on "\n" only)
    return text;
}
int GhostsPlayerBall(int i, bool realSkin) {
    plugins::GameWork work;
    return i < 0 ? 0 : ghosts::PlayerBall(plugins::Current(), static_cast<size_t>(i), realSkin);
}
bool GhostsBallName(int id, bool shown) { return ghosts::ShowPlayerName(plugins::Current(), id, shown); }
template <typename T>
std::vector<T> VectorOf(const CScriptArray* a) {
    std::vector<T> out;
    if (a)
        for (asUINT i = 0; i < a->GetSize(); ++i) out.push_back(*static_cast<const T*>(a->At(i)));
    return out;
}
int GhostsCrowdCreate(double radius, const CScriptArray* palette) {
    plugins::GameWork work;
    return ghosts::CrowdCreate(plugins::Current(), radius, VectorOf<float>(palette));
}
bool GhostsCrowdMembers(int id, const CScriptArray* ghostsIn, const CScriptArray* groups) {
    plugins::GameWork work;
    return ghosts::CrowdMembers(plugins::Current(), id, VectorOf<int>(ghostsIn), VectorOf<int>(groups));
}
bool GhostsCrowdSkins(int id) { return ghosts::CrowdSkins(plugins::Current(), id); }
bool GhostsCrowdTrails(int id, double radius, float opacity, double chunk) { return ghosts::CrowdTrails(plugins::Current(), id, radius, opacity, chunk); }
bool GhostsCrowdTrailsUpTo(int id, double t) { return ghosts::CrowdTrailsUpTo(plugins::Current(), id, t); }
bool GhostsCrowdShowTrails(int id, bool shown) { return ghosts::CrowdShowTrails(plugins::Current(), id, shown); }
bool GhostsCrowdTimes(int id, const CScriptArray* offsets, const CScriptArray* shown) {
    return ghosts::CrowdTimes(plugins::Current(), id, VectorOf<double>(offsets), VectorOf<bool>(shown));
}
bool GhostsCrowdPlace(int id, double t) {
    plugins::GameWork work;
    return ghosts::CrowdPlace(plugins::Current(), id, t);
}
bool GhostsPlaceBall(int id, int i, double t) { return i >= 0 && ghosts::PlacePlayerBall(plugins::Current(), id, static_cast<size_t>(i), t); }
bool DrawGlow(int id, float r, float g, float b, float bright) { return draw::Glow(plugins::Current(), id, r, g, b, bright); }
bool DrawFade(int id, float opacity) { return draw::Fade(plugins::Current(), id, opacity); }
bool GhostsView(int i, double t, double& x, double& y, double& z, double& pitch, double& yaw, double& fov) {
    plugins::GameWork work;
    double out[6];
    if (i < 0 || !ghosts::View(static_cast<size_t>(i), t, out)) return false;
    x = out[0];
    y = out[1];
    z = out[2];
    pitch = out[3];
    yaw = out[4];
    fov = out[5];
    return true;
}
std::string GhostsState() { return ghosts::State(); }
std::string GhostsLeaderboard() { return ghosts::Leaderboard(); }
int GhostsEntries() { return ghosts::Entries(); }
int GhostsWithoutReplay() { return ghosts::WithoutReplay(); }
int GhostsCount() { return static_cast<int>(ghosts::All().size()); }
std::string GhostName(int i) { const auto* g = GhostAt(i); return g ? g->replay.name : ""; }
int GhostRank(int i) { const auto* g = GhostAt(i); return g ? g->rank : 0; }
double GhostTime(int i) { const auto* g = GhostAt(i); return g ? g->replay.time : 0; }
bool GhostIsOwn(int i) { const auto* g = GhostAt(i); return g && g->own; }
int GhostSampleCount(int i) { const auto* g = GhostAt(i); return g ? static_cast<int>(g->replay.samples.size()) : 0; }
bool GhostSample(int i, int j, double& t, double& x, double& y, double& z) {
    const auto* g = GhostAt(i);
    if (!g || j < 0 || static_cast<size_t>(j) >= g->replay.samples.size()) return false;
    const auto& s = g->replay.samples[static_cast<size_t>(j)];
    t = s.t;
    x = s.at.x;
    y = s.at.y;
    z = s.at.z;
    return true;
}
bool GhostPosition(int i, double t, double& x, double& y, double& z) {
    const auto* g = GhostAt(i);
    if (!g) return false;
    const auto p = ghostdata::At(g->replay, t);
    x = p.x;
    y = p.y;
    z = p.z;
    return true;
}
CScriptArray* DoubleArray(const std::vector<double>& values) {
    CScriptArray* array = CScriptArray::Create(e->GetTypeInfoByDecl("array<double>"), static_cast<asUINT>(values.size()));
    for (asUINT k = 0; k < values.size(); ++k) *static_cast<double*>(array->At(k)) = values[k];
    return array;
}
CScriptArray* GhostSplits(int i) { const auto* g = GhostAt(i); return DoubleArray(g ? g->replay.splits : std::vector<double>{}); }
CScriptArray* GhostOrder(int i) { return IdArray(i >= 0 ? ghosts::Order(static_cast<size_t>(i)) : std::vector<int>{}); }
int GhostsCheckpointCount() { return static_cast<int>(ghosts::Checkpoints().size()); }
bool GhostsCheckpoint(int k, int& number, double& x, double& y, double& z) {
    const auto all = ghosts::Checkpoints();
    if (k < 0 || static_cast<size_t>(k) >= all.size()) return false;
    const auto& c = all[static_cast<size_t>(k)];
    number = c.number;
    x = c.at.x;
    y = c.at.y;
    z = c.at.z;
    return true;
}

std::vector<std::array<double, 3>> PathOf(const CScriptArray* xyz) {
    std::vector<std::array<double, 3>> path;
    if (!xyz) return path;
    for (asUINT k = 0; k + 2 < xyz->GetSize(); k += 3)
        path.push_back({*static_cast<const double*>(xyz->At(k)), *static_cast<const double*>(xyz->At(k + 1)), *static_cast<const double*>(xyz->At(k + 2))});
    return path;
}
int DrawTube(const CScriptArray* xyz, double radius, float r, float g, float b, bool glow, float opacity) {
    plugins::GameWork work;
    return draw::Tube(plugins::Current(), PathOf(xyz), radius, r, g, b, glow, opacity < 0 ? 0 : opacity);
}
int DrawSegments(const CScriptArray* xyz, double radius, float r, float g, float b, bool glow, float opacity) {
    plugins::GameWork work;
    return draw::Segments(plugins::Current(), PathOf(xyz), radius, r, g, b, glow, opacity);
}
bool DrawRetube(int id, const CScriptArray* xyz, double radius) {
    plugins::GameWork work;
    return draw::Retube(plugins::Current(), id, PathOf(xyz), radius);
}
// base64 of little-endian signed integers (bytes 1, 2 or 4 each; 1 = unsigned bytes) as numbers: compact data files
CScriptArray* Base64Ints(const std::string& b64, int bytes) {
    static const std::string kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<uint8_t> raw;
    raw.reserve(b64.size() * 3 / 4);
    uint32_t acc = 0;
    int bits = 0;
    for (char ch : b64) {
        const size_t v = kAlphabet.find(ch);
        if (v == std::string::npos) continue;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            raw.push_back(static_cast<uint8_t>((acc >> bits) & 0xff));
        }
    }
    if (bytes != 1 && bytes != 2 && bytes != 4) bytes = 1;
    const size_t n = raw.size() / static_cast<size_t>(bytes);
    asITypeInfo* type = asGetActiveContext()->GetEngine()->GetTypeInfoByDecl("array<int>");
    CScriptArray* out = CScriptArray::Create(type, static_cast<asUINT>(n));
    for (size_t k = 0; k < n; ++k) {
        int v = 0;
        if (bytes == 1) v = raw[k];
        else if (bytes == 2) { int16_t x; std::memcpy(&x, raw.data() + k * 2, 2); v = x; }
        else { int32_t x; std::memcpy(&x, raw.data() + k * 4, 4); v = x; }
        *static_cast<int*>(out->At(static_cast<asUINT>(k))) = v;
    }
    return out;
}
int DrawBall(double radius, float r, float g, float b, bool glow) {
    plugins::GameWork work;
    return draw::Ball(plugins::Current(), radius, r, g, b, glow);
}
bool DrawMove(int id, double x, double y, double z) { return draw::Move(plugins::Current(), id, x, y, z); }
int DrawModel(const std::string& file) {
    std::string text, error;
    if (!ModelText(file, &text)) return 0;
    const int id = draw::Model(plugins::Current(), text, &error);
    if (!id) hostlog::Write("warn", plugins::CurrentId(), "draw: " + file + ": the model is not valid (" + error + ")");
    return id;
}
bool DrawTurn(int id, double pitch, double yaw, double roll) { return draw::Turn(plugins::Current(), id, pitch, yaw, roll); }
bool DrawScale(int id, double scale) { return draw::Scale(plugins::Current(), id, scale); }
bool DrawEffect(const std::string& system, double x, double y, double z, double scale, double nx, double ny, double nz) {
    return draw::Effect(system, x, y, z, scale, nx, ny, nz);
}
bool DrawSound(const std::string& sound, double volume, double pitch) { return draw::Sound(sound, volume, pitch); }
bool PostProcessSet(const std::string& name, double x, double y, double z, double w) {
    std::string error;
    if (postprocess::Set(plugins::Current(), name, x, y, z, w, &error)) return true;
    hostlog::Write("warn", plugins::CurrentId(), "post-process: " + error);
    return false;
}
bool PostProcessWeight(double weight) { return postprocess::Weight(plugins::Current(), weight); }
void PostProcessClear() { postprocess::Clear(plugins::Current()); }
bool CameraShake(double scale) { return draw::Shake(scale); }
// Bounces: each plugin reads from its own place in the list (race.hpp), starting from the first call.
std::map<int, int> gBounceRead;
bool RaceNextBounce(double& strength, double& x, double& y, double& z, double& nx, double& ny, double& nz, bool& ground) {
    const int plugin = plugins::Current();
    auto it = gBounceRead.find(plugin);
    if (it == gBounceRead.end()) it = gBounceRead.emplace(plugin, race::LatestBounce()).first;
    race::Bounce b;
    if (!race::BounceAfter(it->second, &b)) return false;
    it->second = b.serial;
    strength = b.strength, x = b.x, y = b.y, z = b.z, nx = b.nx, ny = b.ny, nz = b.nz, ground = b.ground;
    return true;
}
bool DrawShow(int id, bool shown) { return draw::Show(plugins::Current(), id, shown); }
void DrawRemove(int id) { draw::Remove(plugins::Current(), id); }
void DrawClear() { draw::Clear(plugins::Current()); }
bool CameraProject(double x, double y, double z, float& sx, float& sy) {
    double a = 0, b = 0;
    const bool ok = draw::Project(x, y, z, &a, &b);
    sx = static_cast<float>(a);
    sy = static_cast<float>(b);
    return ok;
}
bool CameraTake() { return draw::TakeCamera(plugins::Current()); }
bool CameraSet(double x, double y, double z, double pitch, double yaw, double fov) {
    return draw::SetCamera(plugins::Current(), x, y, z, pitch, yaw, fov);
}
void CameraRelease() { draw::ReleaseCamera(plugins::Current()); }
bool CameraHas() { return draw::HasCamera(plugins::Current()); }
// Camera::Sweep: a sphere swept from (sx,sy,sz) to (ex,ey,ez) on the Camera trace channel, the player's ball ignored.
// fraction = how far along it got before the first blocking hit (1 = clear). For plugin cameras: pull in when blocked.
bool CameraSweep(double sx, double sy, double sz, double ex, double ey, double ez, double radius, double& fraction) {
    fraction = 1;
    plugins::GameWork work;
    eng::Obj controller = game::PlayerController();
    eng::Obj library = eng::FindCdo("KismetSystemLibrary");
    if (!controller || !library) return false;
    eng::Obj pawn = eng::Call(controller, "K2_GetPawn").ReturnObj();
    // every ball is ignored (the player's, other balls on the track and their skin actors): a plugin camera
    // follows balls, so a ball between it and its target must not pull the shot in; the list is refreshed twice a second
    static std::vector<eng::Weak> balls;
    static double ballsAt = -1;
    const double now = game::Seconds();
    if (now - ballsAt > 0.5 || now < ballsAt) {
        ballsAt = now;
        balls.clear();
        static eng::Obj actorCls = nullptr;
        if (!actorCls) actorCls = eng::FindClass("Actor");
        eng::ForEachObject([&](eng::Obj o) {
            if (!eng::IsA(o, actorCls) || eng::IsDefaultObject(o)) return true;
            const std::string n = eng::ObjName(o);
            if (n.find("RollingBall") != std::string::npos || n.find("BP_LBall") != std::string::npos) balls.push_back(eng::MakeWeak(o));
            return true;
        });
    }
    std::vector<eng::Obj> ignore;
    if (pawn) ignore.push_back(pawn);
    for (auto& w : balls)
        if (eng::Obj o = eng::Get(w)) ignore.push_back(o);
    struct {
        eng::Obj* data;
        int32_t num, max;
    } actors{ignore.data(), static_cast<int32_t>(ignore.size()), static_cast<int32_t>(ignore.size())};
    eng::Params trace(eng::FunctionOn(library, "SphereTraceSingle"));
    const editor::Vec3 a{sx, sy, sz}, b{ex, ey, ez};
    trace.Set("WorldContextObject", controller);
    trace.Set("Start", a);
    trace.Set("End", b);
    trace.Set("Radius", static_cast<float>(radius));
    trace.Set("TraceChannel", uint8_t{1});              // TraceTypeQuery2 = ECC_Camera
    trace.Set("ActorsToIgnore", actors);
    trace.Set("bIgnoreSelf", uint8_t{1});
    if (!eng::Invoke(library, trace)) return false;
    if (!trace.ReturnBool()) return true;
    static eng::Prop timeProp;
    if (!timeProp) timeProp = eng::FindProp(eng::StructOf(eng::FindProp(trace.Fn(), "OutHit")), "Time");
    const uint8_t* hit = trace.Get("OutHit");
    if (!hit || !timeProp) return true;
    float t = 1;
    std::memcpy(&t, hit + timeProp.offset, sizeof t);
    fraction = t < 0 ? 0 : t > 1 ? 1 : t;
    return true;
}

CScriptArray* StringArrayOf(const std::vector<std::string>& values) {
    CScriptArray* array = CScriptArray::Create(e->GetTypeInfoByDecl("array<string>"), static_cast<asUINT>(values.size()));
    for (asUINT k = 0; k < values.size(); ++k) *static_cast<std::string*>(array->At(k)) = values[k];
    return array;
}
CScriptArray* TracksOfficial() {
    plugins::GameWork work;
    std::vector<std::string> levels;
    for (const auto& t : tracks::OfficialTracks()) levels.push_back(t.level);
    return StringArrayOf(levels);
}
std::string TracksGroup(const std::string& level) {
    plugins::GameWork work;
    for (const auto& t : tracks::OfficialTracks())
        if (t.level == level) return t.group;
    return "";
}
std::string TracksImage(const std::string& level) {
    plugins::GameWork work;
    return tracks::Image(level);
}
bool TracksOpen(const std::string& level) {
    plugins::GameWork work;
    return tracks::Open(level);
}
std::string TracksResultImage(int i) { return i < 0 ? "" : tracks::ResultImage(static_cast<size_t>(i)); }
std::string TracksTitle(const std::string& level) {
    plugins::GameWork work;
    return tracks::Title(level);
}
void TracksSearch(const std::string& text) { tracks::Search(text); }
std::string TracksSearchState() { return tracks::SearchState(); }
int TracksResultCount() { return static_cast<int>(tracks::Results().size()); }
int TracksTotal() { return tracks::Total(); }
std::string TracksResultTitle(int i) {
    const auto& r = tracks::Results();
    return i >= 0 && static_cast<size_t>(i) < r.size() ? r[static_cast<size_t>(i)].title : "";
}
std::string TracksResultId(int i) {
    const auto& r = tracks::Results();
    return i >= 0 && static_cast<size_t>(i) < r.size() ? std::to_string(r[static_cast<size_t>(i)].id) : "";
}
bool TracksOpenWorkshop(const std::string& id) {
    plugins::GameWork work;
    return tracks::OpenWorkshop(std::strtoull(id.c_str(), nullptr, 10));
}
std::string TracksOpenState() { return tracks::OpenState(); }

// --- Workshop ------------------------------------------------------------------------------------------------------
uint64_t IdOf(const std::string& s) { return std::strtoull(s.c_str(), nullptr, 10); }
std::string IdText(uint64_t id) { return id ? std::to_string(id) : ""; }
int Reported(int query, const std::string& error) {
    if (query < 0) hostlog::Write("warn", plugins::CurrentId(), "Workshop: " + error);
    return query;
}
int WorkshopFind(const std::string& text, const std::string& sort, int page, int days, const CScriptArray* with,
                 const CScriptArray* without, bool anyTag) {
    std::string error;
    return Reported(workshop::Find(text, sort, page, days, VectorOf<std::string>(with), VectorOf<std::string>(without), anyTag, &error), error);
}
int WorkshopFindList(const std::string& list, const std::string& user, const std::string& sort, int page) {
    std::string error;
    return Reported(workshop::FindList(list, IdOf(user), sort, page, &error), error);
}
int WorkshopFindIds(const CScriptArray* ids) {
    std::vector<uint64_t> numbers;
    for (const auto& id : VectorOf<std::string>(ids))
        if (uint64_t n = IdOf(id)) numbers.push_back(n);
    std::string error;
    return Reported(workshop::FindIds(numbers, &error), error);
}
std::string WorkshopState(int q) { return workshop::State(q); }
int WorkshopCount(int q) { return workshop::Count(q); }
int WorkshopTotal(int q) { return workshop::Total(q); }
std::string WorkshopId(int q, int i) { return IdText(workshop::IdAt(q, i)); }
void WorkshopForget(int q) { workshop::Forget(q); }
const steam::Details& ItemOf(const std::string& id) {
    static const steam::Details none;
    const steam::Details* item = workshop::Item(IdOf(id));
    return item ? *item : none;
}
std::string WorkshopTitle(const std::string& id) { return ItemOf(id).title; }
std::string WorkshopAuthor(const std::string& id) { return IdText(ItemOf(id).owner); }
std::string WorkshopDescription(const std::string& id) { return ItemOf(id).description; }
std::string WorkshopTags(const std::string& id) { return ItemOf(id).tags; }
int64_t WorkshopCreated(const std::string& id) { return ItemOf(id).created; }
int64_t WorkshopUpdated(const std::string& id) { return ItemOf(id).updated; }
int WorkshopVotesUp(const std::string& id) { return static_cast<int>(ItemOf(id).votesUp); }
int WorkshopVotesDown(const std::string& id) { return static_cast<int>(ItemOf(id).votesDown); }
float WorkshopScore(const std::string& id) { return ItemOf(id).score; }
int64_t WorkshopPlays(const std::string& id) { return static_cast<int64_t>(ItemOf(id).plays); }
int64_t WorkshopSubscribers(const std::string& id) { return static_cast<int64_t>(ItemOf(id).subscribers); }
int64_t WorkshopFavorites(const std::string& id) { return static_cast<int64_t>(ItemOf(id).favorites); }
int64_t WorkshopSize(const std::string& id) { return ItemOf(id).size; }
std::string WorkshopImage(const std::string& id) { return workshop::Image(IdOf(id)); }
std::string WorkshopName(const std::string& steamId) { return workshop::Name(IdOf(steamId)); }
std::string WorkshopMe() { return IdText(workshop::Me()); }
double WorkshopMyBest(const std::string& id) {
    plugins::GameWork work;
    return workshop::MyBest(IdOf(id));
}
int WorkshopMyMedal(const std::string& id) {
    plugins::GameWork work;
    return workshop::MyMedal(IdOf(id));
}
CScriptArray* WorkshopFinished() {
    plugins::GameWork work;
    std::vector<std::string> ids;
    for (uint64_t id : workshop::Finished()) ids.push_back(std::to_string(id));
    return StringArrayOf(ids);
}
int WorkshopMyRank(const std::string& id) { return workshop::MyRank(IdOf(id)); }
int WorkshopPlayers(const std::string& id) { return workshop::Players(IdOf(id)); }

bool HubShown() {
    plugins::GameWork work;
    return hub::Shown();
}
bool HubSearch(const std::string& text, const std::string& sort, int days, const std::string& author, const CScriptArray* withTags,
               bool anyTag, bool gameTags, bool show) {
    plugins::GameWork work;
    std::string error;
    if (hub::Search(text, sort, days, IdOf(author), VectorOf<std::string>(withTags), show, anyTag, gameTags, &error)) return true;
    hostlog::Write("warn", plugins::CurrentId(), "Hub::Search: " + error);
    return false;
}
std::string HubView() {
    plugins::GameWork work;
    return hub::View();
}
bool HubListShown() {
    plugins::GameWork work;
    return hub::ListShown();
}
CScriptArray* HubEntries() {
    plugins::GameWork work;
    std::vector<std::string> ids;
    for (uint64_t id : hub::Entries()) ids.push_back(std::to_string(id));
    return StringArrayOf(ids);
}
bool HubHideEntry(const std::string& id, bool hidden) {
    plugins::GameWork work;
    return hub::HideEntry(IdOf(id), hidden);
}
void HubSetAuthorButton(const std::string& label) {
    plugins::GameWork work;
    hub::SetAuthorButton(label);
}
bool HubAuthorButtonClicked() { return hub::AuthorButtonClicked(); }
CScriptArray* HubThumbnails() {
    plugins::GameWork work;
    std::vector<std::string> ids;
    for (uint64_t id : hub::Thumbnails()) ids.push_back(std::to_string(id));
    return StringArrayOf(ids);
}
bool HubSetEntryBadge(const std::string& id, const std::string& text) {
    plugins::GameWork work;
    return hub::SetEntryBadge(IdOf(id), text);
}
std::string HubFocused() {
    plugins::GameWork work;
    return IdText(hub::Focused());
}
std::string HubFocusedAuthor() {
    plugins::GameWork work;
    return IdText(hub::FocusedAuthor());
}

void RegisterWorkshop() {
    e->SetDefaultNamespace("Workshop");
    Global("int Find(const string &in text = \"\", const string &in sort = \"top\", int page = 1, int days = 7, "
           "const array<string>@ withTags = null, const array<string>@ withoutTags = null, bool anyTag = false)",
           asFUNCTION(WorkshopFind));
    Global("int FindList(const string &in list, const string &in user = \"\", const string &in sort = \"new\", int page = 1)",
           asFUNCTION(WorkshopFindList));
    Global("int FindIds(const array<string>@ ids)", asFUNCTION(WorkshopFindIds));
    Global("string State(int)", asFUNCTION(WorkshopState));
    Global("int Count(int)", asFUNCTION(WorkshopCount));
    Global("int Total(int)", asFUNCTION(WorkshopTotal));
    Global("string Id(int, int)", asFUNCTION(WorkshopId));
    Global("void Forget(int)", asFUNCTION(WorkshopForget));
    Global("string Title(const string &in)", asFUNCTION(WorkshopTitle));
    Global("string Author(const string &in)", asFUNCTION(WorkshopAuthor));
    Global("string Description(const string &in)", asFUNCTION(WorkshopDescription));
    Global("string Tags(const string &in)", asFUNCTION(WorkshopTags));
    Global("int64 Created(const string &in)", asFUNCTION(WorkshopCreated));
    Global("int64 Updated(const string &in)", asFUNCTION(WorkshopUpdated));
    Global("int VotesUp(const string &in)", asFUNCTION(WorkshopVotesUp));
    Global("int VotesDown(const string &in)", asFUNCTION(WorkshopVotesDown));
    Global("float Score(const string &in)", asFUNCTION(WorkshopScore));
    Global("int64 Plays(const string &in)", asFUNCTION(WorkshopPlays));
    Global("int64 Subscribers(const string &in)", asFUNCTION(WorkshopSubscribers));
    Global("int64 Favorites(const string &in)", asFUNCTION(WorkshopFavorites));
    Global("int64 Size(const string &in)", asFUNCTION(WorkshopSize));
    Global("string Image(const string &in)", asFUNCTION(WorkshopImage));
    Global("string Name(const string &in)", asFUNCTION(WorkshopName));
    Global("string Me()", asFUNCTION(WorkshopMe));
    Global("double MyBest(const string &in)", asFUNCTION(WorkshopMyBest));
    Global("int MyMedal(const string &in)", asFUNCTION(WorkshopMyMedal));
    Global("array<string>@ Finished()", asFUNCTION(WorkshopFinished));
    Global("int MyRank(const string &in)", asFUNCTION(WorkshopMyRank));
    Global("int Players(const string &in)", asFUNCTION(WorkshopPlayers));
    e->SetDefaultNamespace("Hub");
    Global("bool Shown()", asFUNCTION(HubShown));
    Global("bool Search(const string &in text = \"\", const string &in sort = \"top\", int days = 7, const string &in author = \"\", "
           "const array<string>@ withTags = null, bool anyTag = false, bool gameTags = true, bool show = true)",
           asFUNCTION(HubSearch));
    Global("bool ListShown()", asFUNCTION(HubListShown));
    Global("string View()", asFUNCTION(HubView));
    Global("array<string>@ Entries()", asFUNCTION(HubEntries));
    Global("bool HideEntry(const string &in, bool)", asFUNCTION(HubHideEntry));
    Global("array<string>@ Thumbnails()", asFUNCTION(HubThumbnails));
    Global("void SetAuthorButton(const string &in label)", asFUNCTION(HubSetAuthorButton));
    Global("bool AuthorButtonClicked()", asFUNCTION(HubAuthorButtonClicked));
    Global("bool SetEntryBadge(const string &in id, const string &in text)", asFUNCTION(HubSetEntryBadge));
    Global("string Focused()", asFUNCTION(HubFocused));
    Global("string FocusedAuthor()", asFUNCTION(HubFocusedAuthor));
}

void RegisterGhosts() {
    e->SetDefaultNamespace("Ghosts");
    Global("bool Load(const string &in leaderboard = \"\", int count = 25)", asFUNCTION(GhostsLoad));
    Global("int LoadFile(const string &in file)", asFUNCTION(GhostsLoadFile));
    Global("void PrefetchFile(const string &in file)", asFUNCTION(GhostsPrefetchFile));
    Global("bool PrefetchReady(const string &in file)", asFUNCTION(GhostsPrefetchReady));
    Global("void CrowdTrailDetail(int points, double budgetMs)", asFUNCTION(ghosts::CrowdTrailDetail));
    Global("bool CrowdTrailsPending()", asFUNCTION(GhostsCrowdTrailsPending));
    Global("bool CrowdTrailStyle(int id, bool glow, float brightness, double fadeSeconds = 0, float fadeMin = 0)", asFUNCTION(GhostsCrowdTrailStyle));
    Global("bool CrowdBallScale(int id, double scale)", asFUNCTION(GhostsCrowdBallScale));
    Global("bool CrowdGroupGlass(int id, int group, float opacity)", asFUNCTION(GhostsCrowdGroupGlass));
    Global("string State()", asFUNCTION(GhostsState));
    Global("string Leaderboard()", asFUNCTION(GhostsLeaderboard));
    Global("int Entries()", asFUNCTION(GhostsEntries));
    Global("int WithoutReplay()", asFUNCTION(GhostsWithoutReplay));
    Global("int Count()", asFUNCTION(GhostsCount));
    Global("string Name(int)", asFUNCTION(GhostName));
    Global("int Rank(int)", asFUNCTION(GhostRank));
    Global("double Time(int)", asFUNCTION(GhostTime));
    Global("bool IsOwn(int)", asFUNCTION(GhostIsOwn));
    Global("int SampleCount(int)", asFUNCTION(GhostSampleCount));
    Global("bool Sample(int, int, double &out, double &out, double &out, double &out)", asFUNCTION(GhostSample));
    Global("bool Position(int, double, double &out, double &out, double &out)", asFUNCTION(GhostPosition));
    Global("array<double>@ Splits(int)", asFUNCTION(GhostSplits));
    Global("array<int>@ CheckpointOrder(int)", asFUNCTION(GhostOrder));
    Global("int CheckpointCount()", asFUNCTION(GhostsCheckpointCount));
    Global("int PlayerBall(int ghost, bool realSkin = false)", asFUNCTION(GhostsPlayerBall));
    Global("bool PlaceBall(int id, int ghost, double time)", asFUNCTION(GhostsPlaceBall));
    Global("bool ShowBallName(int id, bool shown)", asFUNCTION(GhostsBallName));
    Global("int CrowdCreate(double radius, const array<float>@ palette)", asFUNCTION(GhostsCrowdCreate));
    Global("bool CrowdMembers(int id, const array<int>@ ghosts, const array<int>@ groups)", asFUNCTION(GhostsCrowdMembers));
    Global("bool CrowdSkins(int id)", asFUNCTION(GhostsCrowdSkins));
    Global("bool CrowdTrails(int id, double radius, float opacity, double chunkSeconds = 0)", asFUNCTION(GhostsCrowdTrails));
    Global("bool CrowdTrailsUpTo(int id, double time)", asFUNCTION(GhostsCrowdTrailsUpTo));
    Global("bool CrowdShowTrails(int id, bool shown)", asFUNCTION(GhostsCrowdShowTrails));
    Global("bool CrowdTimes(int id, const array<double>@ offsets, const array<bool>@ shown)", asFUNCTION(GhostsCrowdTimes));
    Global("bool CrowdPlace(int id, double time)", asFUNCTION(GhostsCrowdPlace));
    Global("bool View(int, double, double &out, double &out, double &out, double &out, double &out, double &out)", asFUNCTION(GhostsView));
    Global("bool Checkpoint(int, int &out, double &out, double &out, double &out)", asFUNCTION(GhostsCheckpoint));
    e->SetDefaultNamespace("Tracks");
    Global("array<string>@ Official()", asFUNCTION(TracksOfficial));
    Global("string Title(const string &in)", asFUNCTION(TracksTitle));
    Global("string Group(const string &in)", asFUNCTION(TracksGroup));
    Global("string Image(const string &in)", asFUNCTION(TracksImage));
    Global("bool Open(const string &in)", asFUNCTION(TracksOpen));
    Global("void Search(const string &in)", asFUNCTION(TracksSearch));
    Global("string SearchState()", asFUNCTION(TracksSearchState));
    Global("int ResultCount()", asFUNCTION(TracksResultCount));
    Global("int Total()", asFUNCTION(TracksTotal));
    Global("string ResultTitle(int)", asFUNCTION(TracksResultTitle));
    Global("string ResultId(int)", asFUNCTION(TracksResultId));
    Global("string ResultImage(int)", asFUNCTION(TracksResultImage));
    Global("bool OpenWorkshop(const string &in)", asFUNCTION(TracksOpenWorkshop));
    Global("string OpenState()", asFUNCTION(TracksOpenState));
    e->SetDefaultNamespace("Draw");
    Global("int Tube(const array<double>@ path, double radius, float r, float g, float b, bool glow = false, float opacity = 1)", asFUNCTION(DrawTube));
    Global("bool Glow(int id, float r, float g, float b, float brightness)", asFUNCTION(DrawGlow));
    Global("bool Fade(int id, float opacity)", asFUNCTION(DrawFade));
    Global("int Ball(double radius, float r, float g, float b, bool glow = false)", asFUNCTION(DrawBall));
    Global("int Segments(const array<double>@ pairs, double radius, float r, float g, float b, bool glow = false, float opacity = 1)", asFUNCTION(DrawSegments));
    Global("bool Retube(int id, const array<double>@ path, double radius)", asFUNCTION(DrawRetube));
    Global("bool Move(int, double, double, double)", asFUNCTION(DrawMove));
    Global("bool Show(int, bool)", asFUNCTION(DrawShow));
    Global("void Remove(int)", asFUNCTION(DrawRemove));
    Global("void Clear()", asFUNCTION(DrawClear));
    Global("int Model(const string &in)", asFUNCTION(DrawModel));
    Global("bool Turn(int, double pitch, double yaw, double roll)", asFUNCTION(DrawTurn));
    Global("bool Scale(int, double)", asFUNCTION(DrawScale));
    Global("bool Effect(const string &in, double x, double y, double z, double scale = 1, double nx = 0, double ny = 0, double nz = 1)",
           asFUNCTION(DrawEffect));
    Global("bool Sound(const string &in, double volume = 1, double pitch = 1)", asFUNCTION(DrawSound));
    e->SetDefaultNamespace("PostProcess");
    Global("bool Set(const string &in, double x, double y = 0, double z = 0, double w = 1)", asFUNCTION(PostProcessSet));
    Global("bool SetWeight(double)", asFUNCTION(PostProcessWeight));
    Global("void Clear()", asFUNCTION(PostProcessClear));
    e->SetDefaultNamespace("Camera");
    Global("bool Shake(double scale = 1)", asFUNCTION(CameraShake));
    Global("bool Project(double, double, double, float &out, float &out)", asFUNCTION(CameraProject));
    Global("bool Take()", asFUNCTION(CameraTake));
    Global("bool Set(double x, double y, double z, double pitch, double yaw, double fov = 90)", asFUNCTION(CameraSet));
    Global("void Release()", asFUNCTION(CameraRelease));
    Global("bool IsTaken()", asFUNCTION(CameraHas));
    Global("bool Sweep(double sx, double sy, double sz, double ex, double ey, double ez, double radius, double &out fraction)",
           asFUNCTION(CameraSweep));
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

// --- Runs: the player's own recent runs -------------------------------------------------------------------------------
void RunsSetKeep(int count) { runs::SetKeep(count); }
int RunsCount() { return runs::Count(); }
std::string RunsId(int i) { return runs::Id(i); }
std::string RunsTrackKey(int i) { return runs::TrackKey(i); }
std::string RunsTrackName(int i) { return runs::TrackName(i); }
double RunsTime(int i) { return runs::Time(i); }
bool RunsFinished(int i) { return runs::Finished(i); }
double RunsAge(int i) { return runs::Age(i); }
bool RunsPinned(int i) { return runs::Pinned(i); }
void RunsSetPinned(int i, bool pinned) { runs::SetPinned(i, pinned); }
double RunsElapsed() { return runs::Elapsed(); }
int RunsBall(int i) {
    plugins::GameWork work;
    return runs::Ball(plugins::Current(), i);
}
bool RunsPlace(int id, int i, double t) {
    plugins::GameWork work;
    return runs::Place(plugins::Current(), id, i, t);
}

void LeaderboardExtraRows(const std::string& title, const CScriptArray* names, const CScriptArray* times, const CScriptArray* ghostsOn,
                          const CScriptArray* pinned) {
    plugins::GameWork work;
    leaderboard::SetExtraRows(plugins::Current(), title, VectorOf<std::string>(names), VectorOf<double>(times), VectorOf<bool>(ghostsOn),
                              VectorOf<bool>(pinned));
}
void LeaderboardExtraRowsIcon(const std::string& file) {
    plugins::GameWork work;
    // a file of the plugin's own folder (or a full path)
    std::wstring path = eng::Widen(file);
    if (!file.empty() && file.find(':') == std::string::npos) path = plugins::CurrentDir() + L"\\" + path;
    leaderboard::SetExtraRowsIcon(plugins::Current(), path);
}
bool LeaderboardExtraRowsShown() { return leaderboard::ExtraRowsShown(); }
int LeaderboardExtraRowClicked() { return leaderboard::ExtraRowClicked(); }
int LeaderboardExtraRowPinClicked() { return leaderboard::ExtraRowPinClicked(); }

void RegisterRuns() {
    e->SetDefaultNamespace("Leaderboard");
    Global("void SetExtraRows(const string &in title, const array<string>@ names, const array<double>@ times, const array<bool>@ ghosts, "
           "const array<bool>@ pinned = null)",
           asFUNCTION(LeaderboardExtraRows));
    Global("void SetExtraRowsIcon(const string &in file)", asFUNCTION(LeaderboardExtraRowsIcon));
    Global("bool ExtraRowsShown()", asFUNCTION(LeaderboardExtraRowsShown));
    Global("int ExtraRowClicked()", asFUNCTION(LeaderboardExtraRowClicked));
    Global("int ExtraRowPinClicked()", asFUNCTION(LeaderboardExtraRowPinClicked));
    e->SetDefaultNamespace("Runs");
    Global("void SetKeep(int)", asFUNCTION(RunsSetKeep));
    Global("int Count()", asFUNCTION(RunsCount));
    Global("string Id(int)", asFUNCTION(RunsId));
    Global("string TrackKey(int)", asFUNCTION(RunsTrackKey));
    Global("string TrackName(int)", asFUNCTION(RunsTrackName));
    Global("double Time(int)", asFUNCTION(RunsTime));
    Global("bool IsFinished(int)", asFUNCTION(RunsFinished));
    Global("double Age(int)", asFUNCTION(RunsAge));
    Global("bool IsPinned(int)", asFUNCTION(RunsPinned));
    Global("void SetPinned(int, bool)", asFUNCTION(RunsSetPinned));
    Global("double Elapsed()", asFUNCTION(RunsElapsed));
    Global("int Ball(int)", asFUNCTION(RunsBall));
    Global("bool PlaceBall(int id, int run, double time)", asFUNCTION(RunsPlace));
}

void Register(asIScriptEngine* engine) {
    e = engine;
    RegisterCore();
    RegisterUi();
    RegisterInput();
    RegisterGhosts();
    RegisterWorkshop();
    RegisterRuns();
    RegisterRace();
    RegisterHud();
    RegisterEditor();
    RegisterReplay();
    RegisterCosmetics();
    RegisterLeaderboard();
    e->SetDefaultNamespace("");
}

}  // namespace api
