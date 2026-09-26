// Windows: rows of widgets anywhere on screen, in any map. Each is a plain UserWidget added to the viewport
// (the viewport keeps it alive) holding CanvasPanel > Border > [sidebar] + VerticalBox of views, each view a
// VerticalBox of rows (HorizontalBoxes). Built when the window is visible and a player controller exists, rebuilt
// after a map change or a layout change. Switching views only changes visibility.
#include <windows.h>

#include <cctype>
#include <cstdlib>
#include <map>

#include "editor.hpp"
#include "game.hpp"
#include "input.hpp"
#include "log.hpp"
#include "plugins.hpp"
#include "storage.hpp"
#include "ui.hpp"
#include "widgets.hpp"

using eng::Obj;
namespace w = ui::widgets;

namespace ui {
namespace {

std::vector<std::unique_ptr<Window>> gWindows;
float gSimulatedSlider = -1;
bool gTyping = false;
constexpr int kEnterKey = 0x0D;             // VK_RETURN
constexpr int kFocusAttempts = 60;          // frames to keep asking for keyboard focus
constexpr Color kButtonColor{0.15f, 0.15f, 0.15f, 1};
constexpr Color kWhite{1, 1, 1, 1};
constexpr Color kInputBackground{0.08f, 0.08f, 0.1f, 1};

// A PNG or JPG file as a texture. Loaded once per file while the texture lives (textures nothing shows any more are
// collected by the engine and loaded again when needed).
Obj LoadTexture(const std::string& path) {
    static std::map<std::string, eng::Weak> textures;
    if (Obj cached = eng::Get(textures[path])) return cached;
    const std::wstring wide = eng::Widen(path);
    if (path.empty() || GetFileAttributesW(wide.c_str()) == INVALID_FILE_ATTRIBUTES) return nullptr;
    const eng::FString file{wide.c_str(), static_cast<int32_t>(wide.size() + 1), static_cast<int32_t>(wide.size() + 1)};
    Obj texture = eng::Call(eng::FindCdo("KismetRenderingLibrary"), "ImportFileAsTexture2D", game::PlayerController(), file).ReturnObj();
    if (!texture) hostlog::Warn("image could not be loaded: " + path);
    textures[path] = eng::MakeWeak(texture);
    return texture;
}

void ShowImage(Obj image, const std::string& path) {
    Obj texture = LoadTexture(path);
    if (image && texture) eng::Call(image, "SetBrushFromTexture", texture, uint8_t{0});
}

// Play is a right-pointing triangle built from stacked bars; pause is two bars. Drawn from rectangles rather
// than glyphs the game's font may not have. Icons never take hits, so the button under them gets the click.
Obj BuildIconButton(Obj tree, Widget& item) {
    Obj button = w::Spawn("Button", tree), frame = w::Spawn("SizeBox", tree), overlay = w::Spawn("Overlay", tree);
    Obj pause = w::Spawn("HorizontalBox", tree), play = w::Spawn("VerticalBox", tree);
    Obj down = w::Spawn("VerticalBox", tree), up = w::Spawn("VerticalBox", tree);
    if (!button || !frame || !overlay || !pause || !play || !down || !up) return nullptr;
    w::Unfocusable(button);
    eng::Call(button, "SetBackgroundColor", kButtonColor);
    const bool arrow = item.text == "down" || item.text == "up";
    eng::Call(frame, "SetWidthOverride", arrow ? 30.0f : 56.0f);
    eng::Call(frame, "SetHeightOverride", 22.0f);
    for (int i = 0; i < 2; ++i) w::AddToRow(pause, w::Block(tree, 5, 18, kWhite), i == 0 ? 0.0f : 5.0f);
    constexpr int kRows = 9;
    for (int r = 0; r < kRows; ++r) {
        const float width = static_cast<float>(std::min(r, kRows - 1 - r) + 1) * 3.5f;
        if (Obj slot = eng::Call(play, "AddChildToVerticalBox", w::Block(tree, width, 2, kWhite)).ReturnObj())
            eng::Call(slot, "SetHorizontalAlignment", w::kAlignLeft);
    }
    // Arrows: rows of shrinking (down) or growing (up) width, centred.
    constexpr int kArrowRows = 6;
    for (int r = 0; r < kArrowRows; ++r)
        for (Obj box : {down, up}) {
            const int step = box == down ? kArrowRows - 1 - r : r;
            if (Obj slot = eng::Call(box, "AddChildToVerticalBox", w::Block(tree, 2.0f + 2.5f * static_cast<float>(step), 2, kWhite)).ReturnObj())
                eng::Call(slot, "SetHorizontalAlignment", w::kAlignCenter);
        }
    for (Obj icon : {pause, play, down, up}) w::AddToOverlay(overlay, icon, w::kAlignCenter, w::kAlignCenter, {0, 0, 0, 0});
    w::AddChild(frame, overlay);
    w::AddChild(button, frame);
    item.main = eng::MakeWeak(button);
    item.iconA = eng::MakeWeak(play);
    item.iconB = eng::MakeWeak(pause);
    item.iconC = eng::MakeWeak(down);
    item.iconD = eng::MakeWeak(up);
    item.shownText.clear();                 // the right icon is shown on the first sync
    return button;
}

Obj BuildWidget(Obj tree, Widget& item) {
    switch (item.kind) {
        case Kind::Text: {
            Obj text = w::Spawn("TextBlock", tree);
            w::SetVisibility(text, w::kHitTestInvisible);     // clicks go to what is under it (a drag surface)
            w::SetFontSize(text, item.size);
            w::SetText(text, item.text);
            w::SetTextColor(text, item.color);
            if (item.justify) eng::Call(text, "SetJustification", item.justify);
            item.main = eng::MakeWeak(text);
            item.shownText = item.text;
            item.colorDirty = false;
            if (item.textWidth <= 0) return text;
            Obj box = w::Spawn("SizeBox", tree);                // a column: the same width whatever the text
            if (!box) return text;
            eng::Call(box, "SetWidthOverride", item.textWidth);
            w::AddChild(box, text);
            return box;
        }
        case Kind::Button: {
            Obj button = w::Spawn("Button", tree), text = w::Spawn("TextBlock", tree);
            if (!button || !text) return nullptr;
            w::Unfocusable(button);
            eng::Call(button, "SetBackgroundColor", item.background);
            item.backgroundDirty = false;
            w::SetFontSize(text, 15);
            w::SetText(text, item.text);
            w::SetTextColor(text, kWhite);
            w::AddChild(button, text);
            item.main = eng::MakeWeak(button);
            item.label = eng::MakeWeak(text);
            item.shownText = item.text;
            return button;
        }
        case Kind::IconButton:
            return BuildIconButton(tree, item);
        case Kind::Slider: {
            Obj box = w::Spawn("SizeBox", tree), slider = w::Spawn("Slider", tree);
            if (!box || !slider) return nullptr;
            w::Unfocusable(slider);
            eng::Call(box, "SetWidthOverride", item.width);
            eng::Call(slider, "SetMinValue", 0.0f);
            eng::Call(slider, "SetMaxValue", 1.0f);
            eng::Call(slider, "SetSliderBarColor", Color{0.4f, 0.4f, 0.4f, 1});
            eng::Call(slider, "SetSliderHandleColor", Color{0.55f, 0.85f, 0.0f, 1});    // the game's lime green
            w::AddChild(box, slider);
            item.main = eng::MakeWeak(slider);
            item.shownValue = -1;
            return box;
        }
        case Kind::Dropdown: {
            Obj box = w::Spawn("SizeBox", tree), combo = w::Spawn("ComboBoxString", tree);
            if (!box || !combo) return nullptr;
            w::Unfocusable(combo);
            // Left alone, the open list's text is black on the dark panel. The styles are read when the dropdown's
            // Slate widget is built, so they are set before it is on screen.
            w::WriteSlateColor(combo, {"ForegroundColor"}, kWhite);
            w::WriteSlateColor(combo, {"ItemStyle", "TextColor"}, kWhite);
            w::WriteSlateColor(combo, {"ItemStyle", "SelectedTextColor"}, kWhite);
            eng::Call(box, "SetWidthOverride", item.width);
            w::AddChild(box, combo);
            for (const auto& option : item.options) {
                const std::wstring ws = eng::Widen(option);
                eng::Call(combo, "AddOption", eng::FString{ws.c_str(), static_cast<int32_t>(ws.size() + 1), static_cast<int32_t>(ws.size() + 1)});
            }
            item.main = eng::MakeWeak(combo);
            item.shownSelected = -2;        // the selection is applied once it is on screen
            return box;
        }
        case Kind::Space: {
            Obj box = w::Spawn("SizeBox", tree);
            if (item.width > 0) eng::Call(box, "SetWidthOverride", item.width);
            item.main = eng::MakeWeak(box);
            return box;
        }
        case Kind::TextArea: {
            Obj box = w::Spawn("SizeBox", tree), scroll = w::Spawn("ScrollBox", tree), text = w::Spawn("TextBlock", tree);
            if (!box || !scroll || !text) return nullptr;
            if (item.width > 0) eng::Call(box, "SetWidthOverride", item.width);
            if (item.height > 0) eng::Call(box, "SetHeightOverride", item.height);
            w::SetFontSize(text, item.size);
            eng::Call(text, "SetAutoWrapText", uint8_t{1});
            w::SetTextColor(text, item.color);
            w::SetText(text, item.text);
            w::AddChild(scroll, text);
            w::AddChild(box, scroll);
            item.main = eng::MakeWeak(scroll);
            item.label = eng::MakeWeak(text);
            item.shownText = item.text;
            item.scrollToEnd = true;
            return box;
        }
        case Kind::TextInput: {
            Obj box = w::Spawn("SizeBox", tree), input = w::Spawn("EditableTextBox", tree);
            if (!box || !input) return nullptr;
            if (item.width > 0) eng::Call(box, "SetWidthOverride", item.width);
            eng::WriteBool(input, "ClearKeyboardFocusOnCommit", false);    // keep typing after Enter
            // Left alone the typed text is light grey on a light box. Styles are read when the Slate widget is
            // built, so they are set before it is on screen.
            for (const char* member : {"ForegroundColor", "FocusedForegroundColor"}) w::WriteSlateColor(input, {"WidgetStyle", member}, kWhite);
            w::WriteSlateColor(input, {"WidgetStyle", "TextStyle", "ColorAndOpacity"}, kWhite);
            w::WriteSlateColor(input, {"WidgetStyle", "BackgroundColor"}, kInputBackground);
            w::SetFontSize(input, item.size, {"WidgetStyle", "TextStyle", "Font"});
            if (!item.text.empty()) w::SetHintText(input, item.text);
            w::AddChild(box, input);
            item.main = eng::MakeWeak(input);
            if (!item.pendingValue.empty()) item.valuePending = true;       // shown again after a rebuild
            return box;
        }
        case Kind::CheckBox: {
            Obj row = w::Spawn("HorizontalBox", tree), box = w::Spawn("CheckBox", tree), text = w::Spawn("TextBlock", tree);
            if (!row || !box || !text) return nullptr;
            w::Unfocusable(box);
            eng::Call(box, "SetIsChecked", static_cast<uint8_t>(item.checked));
            w::SetFontSize(text, item.size);
            w::SetText(text, item.text);
            w::SetTextColor(text, item.colorSet ? item.color : kWhite);
            item.colorDirty = false;
            w::SetVisibility(text, w::kHitTestInvisible);
            w::AddToRow(row, box, 0);
            w::AddToRow(row, text, 4);
            item.main = eng::MakeWeak(box);
            item.label = eng::MakeWeak(text);
            item.shownChecked = item.checked;
            return row;
        }
        case Kind::Image: {
            Obj box = w::Spawn("SizeBox", tree), image = w::Spawn("Image", tree);
            if (!box || !image) return nullptr;
            if (item.width > 0) eng::Call(box, "SetWidthOverride", item.width);
            if (item.height > 0) eng::Call(box, "SetHeightOverride", item.height);
            ShowImage(image, item.text);
            w::SetVisibility(image, w::kHitTestInvisible);
            w::AddChild(box, image);
            item.main = eng::MakeWeak(image);
            item.shownText = item.text;
            return box;
        }
    }
    return nullptr;
}

void Forget(Window& win) {
    win.host = win.border = win.slot = win.dragSurface = {};
    win.hudApplied = false;
    win.dragging = false;
    win.viewBoxes.clear();
    win.shownVisible = false;
    win.appliedView = -1;
    for (auto& item : win.items) {
        item->main = item->label = item->iconA = item->iconB = item->iconC = item->iconD = item->outer = {};
        item->wasPressed = item->dragging = item->focused = false;
    }
}

void Build(Window& win) {
    Obj host = nullptr, tree = nullptr, canvas = nullptr, dock = nullptr;
    if (win.dock == Dock::EditorDetails) {
        // A section of the editor's details panel: built into that panel's own widget tree.
        dock = editor::DetailsContainer();
        if (!dock) return;
        tree = eng::OuterOf(dock);
    } else if (!w::NewScreen(game::PlayerController(), &host, &tree, &canvas)) {
        return;
    }
    Obj border = w::Spawn("Border", tree), column = w::Spawn("VerticalBox", tree);
    if (!border || !column) return;
    const bool sized = !dock && win.screenWidth > 0 && win.screenHeight > 0;
    if (dock) {
        Obj slot = eng::Call(dock, "AddChildToVerticalBox", border).ReturnObj();
        if (!slot) return;
        eng::Call(slot, "SetPadding", w::Margin{0, 10, 0, 0});
        host = border;                          // what is removed when the section is rebuilt
    } else if (sized) {
        const double marginX = (1.0 - win.screenWidth) / 2, marginY = (1.0 - win.screenHeight) / 2;
        w::StretchOnCanvas(canvas, border, marginX, marginY, 1.0 - marginX, 1.0 - marginY);
    } else {
        win.slot = eng::MakeWeak(w::AddToCanvas(canvas, border, win.anchorX, win.anchorY, {win.pivotX, win.pivotY}, {win.offsetX, win.offsetY}));
    }
    eng::Call(border, "SetBrushColor", win.background);
    const w::Margin padding = sized ? w::Margin{16, 16, 16, 16} : w::Margin{12, 8, 12, 8};

    // With a sidebar: HorizontalBox > [SizeBox > sidebar VerticalBox, rows VerticalBox (fills)].
    Obj content = column, sidebar = nullptr;
    if (win.sidebarWidth > 0) {
        Obj body = w::Spawn("HorizontalBox", tree), sideBox = w::Spawn("SizeBox", tree);
        sidebar = w::Spawn("VerticalBox", tree);
        if (!body || !sideBox || !sidebar) return;
        eng::Call(sideBox, "SetWidthOverride", win.sidebarWidth);
        w::AddChild(sideBox, sidebar);
        if (Obj slot = eng::Call(body, "AddChildToHorizontalBox", sideBox).ReturnObj())
            eng::Call(slot, "SetPadding", w::Margin{0, 0, 16, 0});
        w::FillSlot(eng::Call(body, "AddChildToHorizontalBox", column).ReturnObj());
        content = body;
    }
    const bool movable = win.movable && !sized;
    if (win.blocksClicks || movable) {
        // Border > Overlay > [a button that draws nothing and fills the window, the content]. The button takes
        // every click that lands on the window between widgets, so nothing underneath (the game's menus, or the
        // game itself) receives it. The padding goes on the content so the button reaches the window's edges.
        Obj overlay = w::Spawn("Overlay", tree), blocker = w::Spawn("Button", tree);
        if (!overlay || !blocker) return;
        w::Unfocusable(blocker);
        w::Transparent(blocker);
        w::AddChild(border, overlay);
        w::AddToOverlay(overlay, blocker, w::kAlignFill, w::kAlignFill, {0, 0, 0, 0});
        if (movable) win.dragSurface = eng::MakeWeak(blocker);
        w::AddToOverlay(overlay, content, w::kAlignFill, w::kAlignFill, padding);
    } else {
        eng::Call(border, "SetPadding", padding);
        w::AddChild(border, content);
    }

    // The header (rows of view -1, shown with every view) above one VerticalBox per view, each taking all the
    // height; only the shown view is visible.
    Obj headerBox = w::Spawn("VerticalBox", tree);
    if (!headerBox) return;
    if (Obj slot = eng::Call(column, "AddChildToVerticalBox", headerBox).ReturnObj()) eng::Call(slot, "SetPadding", w::Margin{0, 0, 0, 12});
    std::vector<Obj> viewBoxes;
    for (int v = 0; v < win.views; ++v) {
        Obj box = w::Spawn("VerticalBox", tree);
        // A scrolling view is its rows in a ScrollBox that takes the height; it is what is shown and hidden.
        const bool scrolls = std::find(win.scrollingViews.begin(), win.scrollingViews.end(), v) != win.scrollingViews.end();
        Obj outer = scrolls ? w::Spawn("ScrollBox", tree) : box;
        if (scrolls && outer) w::AddChild(outer, box);
        w::FillSlot(eng::Call(column, "AddChildToVerticalBox", outer).ReturnObj());
        viewBoxes.push_back(box);
        win.viewBoxes.push_back(eng::MakeWeak(outer));
    }

    // A row that holds something with a fill height (a text area of height 0) takes the leftover height. The rows
    // of a card go into that card's column, inside a rounded box; the card sits in the view like one row.
    std::vector<Obj> rows(win.rowView.size(), nullptr);
    std::vector<int> rowsInView(static_cast<size_t>(win.views) + 1, 0);    // [0] is the header, [v + 1] view v
    std::vector<Obj> cardColumns(static_cast<size_t>(win.cards), nullptr), cardSlots(static_cast<size_t>(win.cards), nullptr);
    std::vector<int> rowsInCard(static_cast<size_t>(win.cards), 0);
    for (size_t r = 0; r < rows.size(); ++r) {
        if (win.rowRetired[r]) continue;
        Obj row = w::Spawn("HorizontalBox", tree);
        const size_t viewSlot = static_cast<size_t>(win.rowView[r] + 1);
        Obj parent = win.rowView[r] < 0 ? headerBox : viewBoxes[viewSlot - 1];
        const int card = win.rowCard[r];
        bool firstInParent = rowsInView[viewSlot] == 0;
        if (card >= 0) {
            Obj& cardColumn = cardColumns[static_cast<size_t>(card)];
            if (!cardColumn) {
                Obj box = w::Spawn("Border", tree);
                cardColumn = w::Spawn("VerticalBox", tree);
                if (!box || !cardColumn) return;
                w::RoundCorners(box, 10);
                eng::Call(box, "SetBrushColor", win.cardBackground);
                eng::Call(box, "SetPadding", w::Margin{14, 10, 14, 10});
                w::AddChild(box, cardColumn);
                Obj cardSlot = eng::Call(parent, "AddChildToVerticalBox", box).ReturnObj();
                if (!cardSlot) return;
                cardSlots[static_cast<size_t>(card)] = cardSlot;
                if (rowsInView[viewSlot]++ > 0) eng::Call(cardSlot, "SetPadding", w::Margin{0, 8, 0, 0});
            }
            parent = cardColumn;
            firstInParent = rowsInCard[static_cast<size_t>(card)]++ == 0;
        } else {
            rowsInView[viewSlot]++;
        }
        Obj slot = eng::Call(parent, "AddChildToVerticalBox", row).ReturnObj();
        if (!slot) return;
        if (!firstInParent) eng::Call(slot, "SetPadding", w::Margin{0, card >= 0 ? 4.0f : 8.0f, 0, 0});
        for (const auto& item : win.items)
            if (!item->retired && !item->inSidebar && item->row == static_cast<int>(r) && item->kind == Kind::TextArea && item->height <= 0) {
                w::FillSlot(slot);
                if (card >= 0) w::FillSlot(cardSlots[static_cast<size_t>(card)]);     // the card takes the height too
            }
        rows[r] = row;
    }
    std::vector<int> placedInRow(rows.size(), 0);
    int placedInSidebar = 0;
    for (auto& itemPtr : win.items) {
        Widget& item = *itemPtr;
        if (item.retired) continue;
        Obj widget = BuildWidget(tree, item);
        item.outer = eng::MakeWeak(widget);
        item.normalVisibility = widget ? eng::Call(widget, "GetVisibility").ReturnAs<uint8_t>(0) : 0;
        item.shownVisible = true;
        if (!item.visible) {
            w::SetVisibility(widget, w::kCollapsed);
            item.shownVisible = false;
        }
        if (item.inSidebar) {
            // One per line, as wide as the sidebar.
            Obj slot = sidebar && widget ? eng::Call(sidebar, "AddChildToVerticalBox", widget).ReturnObj() : nullptr;
            if (slot) {
                eng::Call(slot, "SetHorizontalAlignment", w::kAlignFill);
                if (placedInSidebar++ > 0) eng::Call(slot, "SetPadding", w::Margin{0, 6, 0, 0});
            }
            continue;
        }
        int& placed = placedInRow[static_cast<size_t>(item.row)];
        Obj slot = w::AddToRow(rows[static_cast<size_t>(item.row)], widget, placed++ == 0 ? 0.0f : (item.kind == Kind::Text ? 14.0f : 8.0f));
        const bool fillsWidth =
            (item.kind == Kind::Space || item.kind == Kind::TextArea || item.kind == Kind::TextInput || item.kind == Kind::Image) && item.width <= 0;
        if (fillsWidth) w::FillSlot(slot);
        if (item.kind == Kind::TextArea && item.height <= 0) eng::Call(slot, "SetVerticalAlignment", w::kAlignFill);
    }
    w::SetVisibility(border, w::kSelfHitTestInvisible);
    if (!dock) eng::Call(host, "AddToViewport", static_cast<int32_t>(win.zOrder));
    win.dockedIn = eng::MakeWeak(dock);
    win.host = eng::MakeWeak(host);
    win.border = eng::MakeWeak(border);
    win.generation = game::Generation();
    win.shownVisible = true;
    win.layoutDirty = false;
    int live = 0;
    for (const auto& item : win.items) live += item->retired ? 0 : 1;
    hostlog::Info("window built with " + std::to_string(live) + " widgets");
}

std::string PositionKey(const Window& win, const char* axis) { return "window." + std::to_string(win.ordinal) + "." + axis; }

// While the drag surface is held (it can only be pressed while the cursor is on screen), the window follows the
// mouse; the position is saved when it is let go.
void Drag(Window& win) {
    Obj surface = eng::Get(win.dragSurface), slot = eng::Get(win.slot);
    if (!win.movable || !surface || !slot) return;
    const bool pressed = eng::Call(surface, "IsPressed").ReturnBool();
    if (!pressed) {
        if (win.dragging) {
            win.dragging = false;
            hostlog::Info("window moved to " + std::to_string(static_cast<int>(win.offsetX)) + ", " + std::to_string(static_cast<int>(win.offsetY)));
            storage::Set(win.storageOwner, PositionKey(win, "x"), std::to_string(static_cast<int>(win.offsetX)));
            storage::Set(win.storageOwner, PositionKey(win, "y"), std::to_string(static_cast<int>(win.offsetY)));
        }
        return;
    }
    struct Vec2d {
        double x, y;
    };
    const Vec2d mouse = eng::Call(eng::FindCdo("WidgetLayoutLibrary"), "GetMousePositionOnViewport", game::PlayerController())
                            .ReturnAs<Vec2d>(Vec2d{-1, -1});
    if (mouse.x < 0) return;
    if (!win.dragging) {
        win.dragging = true;
        hostlog::Info("window drag started at mouse " + std::to_string(static_cast<int>(mouse.x)) + ", " + std::to_string(static_cast<int>(mouse.y)));
        win.dragMouseX = mouse.x;
        win.dragMouseY = mouse.y;
        win.dragOffsetX = win.offsetX;
        win.dragOffsetY = win.offsetY;
    }
    win.offsetX = static_cast<float>(win.dragOffsetX + mouse.x - win.dragMouseX);
    win.offsetY = static_cast<float>(win.dragOffsetY + mouse.y - win.dragMouseY);
    eng::Call(slot, "SetPosition", w::Vec2{win.offsetX + win.hudX, win.offsetY + win.hudY});
}

// A HUD layout's offset, size and opacity, applied when they change or the window was rebuilt.
void ApplyHud(Window& win) {
    Obj slot = eng::Get(win.slot), border = eng::Get(win.border);
    if (!slot || !border) return;
    if (win.hudApplied && win.hudX == win.shownHudX && win.hudY == win.shownHudY && win.hudScale == win.shownHudScale &&
        win.hudOpacity == win.shownHudOpacity)
        return;
    if (!win.hudApplied && win.hudX == 0 && win.hudY == 0 && win.hudScale == 1 && win.hudOpacity == 1) return;
    eng::Call(slot, "SetPosition", w::Vec2{win.offsetX + win.hudX, win.offsetY + win.hudY});
    eng::Call(border, "SetRenderScale", w::Vec2{win.hudScale, win.hudScale});
    eng::Call(border, "SetRenderOpacity", win.hudOpacity);
    win.shownHudX = win.hudX;
    win.shownHudY = win.hudY;
    win.shownHudScale = win.hudScale;
    win.shownHudOpacity = win.hudOpacity;
    win.hudApplied = true;
}

void Sync(Widget& item) {
    Obj main = eng::Get(item.main);
    if (!main) return;
    if (item.visible != item.shownVisible) {
        w::SetVisibility(eng::Get(item.outer), item.visible ? item.normalVisibility : w::kCollapsed);
        item.shownVisible = item.visible;
    }
    switch (item.kind) {
        case Kind::Text:
            if (item.text != item.shownText) {
                w::SetText(main, item.text);
                item.shownText = item.text;
            }
            if (item.colorDirty) {
                w::SetTextColor(main, item.color);
                item.colorDirty = false;
            }
            break;
        case Kind::Button:
        case Kind::IconButton: {
            if (item.backgroundDirty) {
                eng::Call(main, "SetBackgroundColor", item.background);
                item.backgroundDirty = false;
            }
            if (item.text != item.shownText) {
                if (item.kind == Kind::Button) {
                    w::SetText(eng::Get(item.label), item.text);
                } else {
                    const std::string names[4] = {"play", "pause", "down", "up"};
                    const eng::Weak* icons[4] = {&item.iconA, &item.iconB, &item.iconC, &item.iconD};
                    for (int n = 0; n < 4; ++n)
                        w::SetVisibility(eng::Get(*icons[n]), item.text == names[n] ? w::kHitTestInvisible : w::kCollapsed);
                }
                item.shownText = item.text;
            }
            // A click is a press that ends while the pointer is still over the button.
            const bool pressed = eng::Call(main, "IsPressed").ReturnBool();
            item.hovered = eng::Call(main, "IsHovered").ReturnBool();
            if (item.wasPressed && !pressed && item.hovered) item.clickPending = true;
            item.wasPressed = pressed;
            break;
        }
        case Kind::Slider:
            item.dragging = eng::Call(main, "HasMouseCapture").ReturnBool();
            if (gSimulatedSlider >= 0) {
                item.dragging = true;
                item.value = gSimulatedSlider;
            } else if (item.dragging) {
                item.value = eng::Call(main, "GetValue").ReturnAs<float>(item.value);
            } else if (item.value != item.shownValue) {
                eng::Call(main, "SetValue", item.value);
            }
            item.shownValue = item.value;
            break;
        case Kind::Dropdown:
            if (item.selected != item.shownSelected) {
                eng::Call(main, "SetSelectedIndex", static_cast<int32_t>(item.selected));
                item.shownSelected = item.selected;
            } else {
                const int32_t index = eng::Call(main, "GetSelectedIndex").ReturnAs<int32_t>(item.selected);
                if (index >= 0 && index != item.selected) {
                    item.selected = item.shownSelected = index;
                    item.changedPending = true;
                }
            }
            break;
        case Kind::Space:
            break;
        case Kind::CheckBox: {
            if (item.colorDirty) {
                w::SetTextColor(eng::Get(item.label), item.color);
                item.colorDirty = false;
            }
            const bool now = eng::Call(main, "IsChecked").ReturnBool();
            if (now != item.shownChecked) {             // the player clicked it
                item.checked = item.shownChecked = now;
                item.changedPending = true;
            } else if (item.checked != item.shownChecked) {
                eng::Call(main, "SetIsChecked", static_cast<uint8_t>(item.checked));
                item.shownChecked = item.checked;
            }
            break;
        }
        case Kind::Image:
            if (item.text != item.shownText) {
                ShowImage(main, item.text);
                item.shownText = item.text;
            }
            break;
        case Kind::TextArea: {
            // Follow new text only when already scrolled to the end, so reading further up is not interrupted.
            const float offset = eng::Call(main, "GetScrollOffset").ReturnAs<float>(0);
            const float end = eng::Call(main, "GetScrollOffsetOfEnd").ReturnAs<float>(0);
            if (item.text != item.shownText) {
                w::SetText(eng::Get(item.label), item.text);
                item.shownText = item.text;
                if (offset >= end - 2) item.scrollToEnd = true;
            }
            if (item.scrollToEnd) {
                eng::Call(main, "ScrollToEnd");
                item.scrollToEnd = false;
            }
            break;
        }
        case Kind::TextInput: {
            if (item.valuePending) {
                w::SetText(main, item.pendingValue);
                item.valuePending = false;
            }
            item.focused = eng::Call(main, "HasKeyboardFocus").ReturnBool();
            // Focus can only be taken once the box is on screen, so it is asked for until it sticks.
            if (item.focusRequested) {
                if (item.focused || ++item.focusAttempts > kFocusAttempts) item.focusRequested = false;
                else eng::Call(main, "SetKeyboardFocus");
            }
            if ((item.focused && input::Pressed(kEnterKey)) || item.submitRequested) {
                item.submitRequested = false;
                const std::string typed = w::ReadText(main);
                if (!typed.empty()) {
                    item.submitted = typed;
                    item.submitPending = true;
                    if (item.clearOnSubmit) w::SetText(main, "");
                    else item.pendingValue = typed;             // kept, and shown again after a rebuild
                }
            }
            break;
        }
    }
}

}  // namespace

namespace {
void AddRow(Window* win, int view) {
    win->rowView.push_back(view);
    win->rowRetired.push_back(false);
    win->rowCard.push_back(win->openCard);
    win->addRow = static_cast<int>(win->rowView.size()) - 1;
    win->layoutDirty = true;
}
}  // namespace

void NewRow(Window* win) { AddRow(win, win->rowView[static_cast<size_t>(win->addRow)]); }

int StartView(Window* win) {
    win->addingToSidebar = false;
    win->openCard = -1;
    AddRow(win, win->views++);
    return win->views - 1;
}

void SetScrolling(Window* win, int view, bool on) {
    auto& list = win->scrollingViews;
    const auto it = std::find(list.begin(), list.end(), view);
    if (on == (it != list.end())) return;
    if (on) list.push_back(view);
    else list.erase(it);
    win->layoutDirty = true;
}

void ShowView(Window* win, int view) {
    if (view >= 0 && view < win->views) win->shownView = view;
}

void ClearView(Window* win, int view) {
    if (view < 0 || view >= win->views) return;
    for (size_t r = 0; r < win->rowView.size(); ++r)
        if (win->rowView[r] == view) win->rowRetired[r] = true;
    for (auto& item : win->items)
        if (!item->inSidebar && win->rowView[static_cast<size_t>(item->row)] == view) item->retired = true;
    win->addingToSidebar = false;
    win->openCard = -1;
    AddRow(win, view);
}

bool RowEmpty(const Window* win, int row) {
    for (const auto& item : win->items)
        if (!item->retired && !item->inSidebar && item->row == row) return false;
    return true;
}

void StartHeader(Window* win) {
    win->addingToSidebar = false;
    win->openCard = -1;
    AddRow(win, -1);
}

void StartCard(Window* win) {
    win->addingToSidebar = false;
    win->openCard = win->cards++;
    const int view = win->rowView[static_cast<size_t>(win->addRow)];
    if (RowEmpty(win, win->addRow) && win->rowCard[static_cast<size_t>(win->addRow)] < 0)
        win->rowCard[static_cast<size_t>(win->addRow)] = win->openCard;    // the empty row just started becomes the card's
    else
        AddRow(win, view);
    win->layoutDirty = true;
}

void EndCard(Window* win) {
    if (win->openCard < 0) return;
    win->openCard = -1;
    AddRow(win, win->rowView[static_cast<size_t>(win->addRow)]);
}

void StartSidebar(Window* win, float width) {
    win->sidebarWidth = width;
    win->addingToSidebar = true;
    win->layoutDirty = true;
}

void StartMain(Window* win) { win->addingToSidebar = false; }

void SetMovable(Window* win, bool movable, const std::string& pluginId) {
    if (movable && !win->movable) {
        win->storageOwner = pluginId;
        win->ordinal = 0;
        for (auto& other : gWindows) {
            if (other.get() == win) break;
            if (other->owner == win->owner) ++win->ordinal;
        }
        win->defaultOffsetX = win->offsetX;
        win->defaultOffsetY = win->offsetY;
        if (storage::Has(pluginId, PositionKey(*win, "x")) && storage::Has(pluginId, PositionKey(*win, "y"))) {
            win->offsetX = static_cast<float>(std::atof(storage::Get(pluginId, PositionKey(*win, "x"), "0").c_str()));
            win->offsetY = static_cast<float>(std::atof(storage::Get(pluginId, PositionKey(*win, "y"), "0").c_str()));
        }
    }
    win->movable = movable;
    win->layoutDirty = true;
}

void ResetPositions(const std::string& pluginId) {
    for (auto& win : gWindows) {
        if (!win->movable || win->storageOwner != pluginId) continue;
        win->offsetX = win->defaultOffsetX;
        win->offsetY = win->defaultOffsetY;
        storage::Erase(pluginId, PositionKey(*win, "x"));
        storage::Erase(pluginId, PositionKey(*win, "y"));
        win->layoutDirty = true;
    }
}

bool HasMovable(const std::string& pluginId) {
    for (auto& win : gWindows)
        if (win->movable && win->storageOwner == pluginId) return true;
    return false;
}

std::vector<HudWindow> HudWindows() {
    std::vector<HudWindow> out;
    std::map<int, int> counts;
    for (auto& win : gWindows) {
        const int n = counts[win->owner]++;
        if (win->dock != Dock::Screen || !win->visible || !eng::Get(win->border)) continue;
        out.push_back({"Window/" + plugins::IdAt(win->owner) + "/" + std::to_string(n), plugins::NameAt(win->owner), win.get()});
    }
    return out;
}

Window* MakeWindow(int owner) {
    gWindows.push_back(std::make_unique<Window>());
    gWindows.back()->owner = owner;
    return gWindows.back().get();
}

Widget* AddWidget(Window* win, Kind kind, const std::string& text, float sizeOrWidth) {
    auto item = std::make_unique<Widget>();
    item->window = win;
    item->kind = kind;
    item->row = win->addRow;
    item->inSidebar = win->addingToSidebar;
    item->text = text;
    (kind == Kind::Text ? item->size : item->width) = sizeOrWidth;
    win->items.push_back(std::move(item));
    win->layoutDirty = true;
    return win->items.back().get();
}

void AddOption(Widget* dropdown, const std::string& option) {
    dropdown->options.push_back(option);
    dropdown->window->layoutDirty = true;
}

namespace windows {

void Frame() {
    gTyping = false;
    Obj typingWidget = nullptr;
    for (auto& winPtr : gWindows) {
        Window& win = *winPtr;
        // Built in an earlier map: those widgets went with it. Forgotten without being touched.
        if (win.host.o && win.generation != game::Generation()) Forget(win);
        // Docked into a panel that has been replaced (the editor was reopened): build it again in the new one.
        if (win.dock == Dock::EditorDetails && eng::Get(win.host) && eng::Get(win.dockedIn) != editor::DetailsContainer()) {
            eng::Call(eng::Get(win.host), "RemoveFromParent");
            Forget(win);
        }
        const bool built = eng::Get(win.host) != nullptr;
        if (win.visible && (!built || win.layoutDirty)) {
            if (built) {
                eng::Call(eng::Get(win.host), "RemoveFromParent");
                Forget(win);
            }
            if (game::PlayerController()) Build(win);
        }
        Obj border = eng::Get(win.border);
        if (!border) continue;
        if (win.visible != win.shownVisible) {
            w::SetVisibility(border, win.visible ? w::kSelfHitTestInvisible : w::kCollapsed);
            win.shownVisible = win.visible;
        }
        Drag(win);
        ApplyHud(win);
        if (win.shownView != win.appliedView) {
            for (size_t v = 0; v < win.viewBoxes.size(); ++v)
                w::SetVisibility(eng::Get(win.viewBoxes[v]), static_cast<int>(v) == win.shownView ? w::kSelfHitTestInvisible : w::kCollapsed);
            win.appliedView = win.shownView;
        }
        if (win.visible)
            for (auto& item : win.items) {
                if (item->retired) continue;
                // Widgets of hidden views are not synced; their text inputs cannot have focus.
                const int view = win.rowView[static_cast<size_t>(item->row)];
                if (!item->inSidebar && view >= 0 && view != win.shownView) {
                    item->focused = false;
                    continue;
                }
                Sync(*item);
                gTyping = gTyping || item->focused;
                // A requested focus is given through the input mode too, which also keeps keys from the game.
                if (item->kind == Kind::TextInput && (item->focused || item->focusRequested) && !typingWidget)
                    typingWidget = eng::Get(item->main);
            }
    }
    game::SetTypingWidget(typingWidget);
    gSimulatedSlider = -1;
}

bool Typing() { return gTyping; }

void HideOwner(int owner) {
    for (auto& win : gWindows)
        if (win->owner == owner) win->visible = false;
}

void RemoveOwner(int owner) {
    for (auto it = gWindows.begin(); it != gWindows.end();) {
        if ((*it)->owner != owner) {
            ++it;
            continue;
        }
        if (Obj host = eng::Get((*it)->host)) eng::Call(host, "RemoveFromParent");
        it = gWindows.erase(it);
    }
}

// "label" is the first button with that label; "label#n" the nth (from 1), for cards that repeat a label.
bool SimulateClick(const std::string& label) {
    std::string wanted = label;
    int nth = 1;
    const size_t hash = label.rfind('#');
    if (hash != std::string::npos && hash + 1 < label.size() && std::isdigit(static_cast<unsigned char>(label[hash + 1]))) {
        wanted = label.substr(0, hash);
        nth = std::atoi(label.c_str() + hash + 1);
    }
    for (auto& win : gWindows)
        for (auto& item : win->items)
            if (!item->retired && (item->kind == Kind::Button || item->kind == Kind::IconButton) && (item->text == wanted || wanted == "icon") &&
                --nth == 0)
                return item->clickPending = true;
    return false;
}

bool SimulateSelect(const std::string& firstOption, int index) {
    for (auto& win : gWindows)
        for (auto& item : win->items)
            if (!item->retired && item->kind == Kind::Dropdown && !item->options.empty() && item->options[0] == firstOption && index >= 0 &&
                index < static_cast<int>(item->options.size())) {
                item->selected = index;
                item->changedPending = true;
                return true;
            }
    return false;
}

void SimulateSlider(float value) { gSimulatedSlider = value; }

bool SimulateSubmit(const std::string& command) {
    // "@<hint fragment>|<text>" picks the first text input whose hint contains the fragment; otherwise the first.
    std::string hint, text = command;
    if (command.rfind("@", 0) == 0 && command.find('|') != std::string::npos) {
        hint = command.substr(1, command.find('|') - 1);
        text = command.substr(command.find('|') + 1);
    }
    for (auto& win : gWindows)
        for (auto& item : win->items)
            if (!item->retired && item->kind == Kind::TextInput && item->text.find(hint) != std::string::npos) {
                item->submitted = text;
                return item->submitPending = true;
            }
    return false;
}

std::string Status() {
    std::string s;
    for (auto& win : gWindows) {
        s += std::string(s.empty() ? "" : " ") + "window=" + (eng::Get(win->host) ? (win->shownVisible ? "shown" : "hidden") : "absent");
        for (auto& item : win->items)
            if (!item->retired && item->kind == Kind::Text) {
                std::string text = item->text;
                for (char& c : text)
                    if (c == '\n') c = '/';        // one log line per status
                s += " text[" + text + "]";
            }
    }
    return s;
}

}  // namespace windows
}  // namespace ui
