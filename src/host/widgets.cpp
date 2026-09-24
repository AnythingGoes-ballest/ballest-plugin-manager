#include "widgets.hpp"

#include <vector>

namespace ui::widgets {

Obj Spawn(const char* className, Obj outer) {
    Obj cls = eng::FindClass(className);
    if (!cls || !outer) return nullptr;
    return eng::Call(eng::FindCdo("GameplayStatics"), "SpawnObject", cls, outer).ReturnObj();
}

Obj AddChild(Obj panel, Obj child) { return child ? eng::Call(panel, "AddChild", child).ReturnObj() : nullptr; }

Obj AddToRow(Obj row, Obj child, float leftPadding) {
    Obj slot = child ? eng::Call(row, "AddChildToHorizontalBox", child).ReturnObj() : nullptr;
    if (slot) {
        eng::Call(slot, "SetPadding", Margin{leftPadding, 0, 0, 0});
        eng::Call(slot, "SetVerticalAlignment", kAlignCenter);
    }
    return slot;
}

Obj AddToOverlay(Obj overlay, Obj child, uint8_t alignX, uint8_t alignY, Margin padding) {
    Obj slot = child ? eng::Call(overlay, "AddChildToOverlay", child).ReturnObj() : nullptr;
    if (slot) {
        eng::Call(slot, "SetHorizontalAlignment", alignX);
        eng::Call(slot, "SetVerticalAlignment", alignY);
        eng::Call(slot, "SetPadding", padding);
    }
    return slot;
}

Obj AddToCanvas(Obj canvas, Obj child, double anchorX, double anchorY, Vec2 pivot, Vec2 position) {
    Obj slot = child ? eng::Call(canvas, "AddChildToCanvas", child).ReturnObj() : nullptr;
    if (slot) {
        const double anchors[4] = {anchorX, anchorY, anchorX, anchorY};     // FAnchors: minimum, maximum
        eng::Params a(eng::FunctionOn(slot, "SetAnchors"));
        a.SetArg(0, anchors, sizeof anchors);
        eng::Invoke(slot, a);
        eng::Call(slot, "SetAlignment", pivot);
        eng::Call(slot, "SetAutoSize", uint8_t{1});
        eng::Call(slot, "SetPosition", position);
    }
    return slot;
}

Obj StretchOnCanvas(Obj canvas, Obj child, double minX, double minY, double maxX, double maxY) {
    Obj slot = child ? eng::Call(canvas, "AddChildToCanvas", child).ReturnObj() : nullptr;
    if (slot) {
        const double anchors[4] = {minX, minY, maxX, maxY};             // FAnchors: minimum, maximum
        eng::Params a(eng::FunctionOn(slot, "SetAnchors"));
        a.SetArg(0, anchors, sizeof anchors);
        eng::Invoke(slot, a);
        eng::Call(slot, "SetAutoSize", uint8_t{0});
        eng::Call(slot, "SetOffsets", Margin{0, 0, 0, 0});           // with stretched anchors: the edge insets
    }
    return slot;
}

void FillSlot(Obj slot) {
    // FSlateChildSize, measured: { float Value @0x0, uint8 SizeRule @0x4 }; ESlateSizeRule::Fill = 1.
    struct ChildSize {
        float value;
        uint8_t rule, pad[3];
    };
    if (slot) eng::Call(slot, "SetSize", ChildSize{1.0f, 1, {0, 0, 0}});
}

void SetVisibility(Obj widget, uint8_t visibility) {
    if (widget) eng::Call(widget, "SetVisibility", visibility);
}

namespace {
// Calls a setter taking one FText (SetText, SetHintText, ...).
void CallWithText(Obj widget, const char* setter, const std::string& s) {
    if (!widget) return;
    const eng::Params text = eng::MakeText(s);
    size_t size = 0;
    const uint8_t* ftext = text.Return(&size);
    if (!text.Invoked() || !ftext) return;
    eng::Params set(eng::FunctionOn(widget, setter));
    set.SetArg(0, ftext, size);
    if (eng::Invoke(widget, set)) eng::ReleaseText(ftext);
}
}  // namespace

void SetText(Obj textWidget, const std::string& s) { CallWithText(textWidget, "SetText", s); }
void SetHintText(Obj editableTextBox, const std::string& s) { CallWithText(editableTextBox, "SetHintText", s); }

std::string ReadText(Obj textWidget) {
    if (!textWidget) return "";
    const eng::Params got = eng::Call(textWidget, "GetText");
    size_t size = 0;
    const uint8_t* ftext = got.Return(&size);
    if (!got.Invoked() || !ftext) return "";
    Obj library = eng::FindCdo("KismetTextLibrary");
    eng::Params convert(eng::FunctionOn(library, "Conv_TextToString"));
    convert.SetArg(0, ftext, size);
    const uint8_t* fstring = eng::Invoke(library, convert) ? convert.Return() : nullptr;
    std::string s = fstring ? eng::ReadFString(fstring) : "";
    eng::ReleaseText(ftext);        // GetText handed the host a reference of its own
    return s;
}

void SetTextColor(Obj textBlock, Color c) {
    Obj textClass = eng::FindClass("TextBlock");
    const int base = eng::NestedOffset(textClass, {"ColorAndOpacity"});
    const int color = eng::NestedOffset(textClass, {"ColorAndOpacity", "SpecifiedColor"});
    const int rule = eng::NestedOffset(textClass, {"ColorAndOpacity", "ColorUseRule"});
    eng::Params p(eng::FunctionOn(textBlock, "SetColorAndOpacity"));
    const int size = p.SizeOf("InColorAndOpacity");
    if (size <= 0 || base < 0 || color < 0 || rule < 0) return;
    std::vector<uint8_t> slateColor(size, 0);
    std::memcpy(slateColor.data() + (color - base), &c, sizeof c);
    slateColor[rule - base] = 0;                    // ESlateColorStylingMode::UseColor_Specified
    p.SetArg(0, slateColor.data(), slateColor.size());
    eng::Invoke(textBlock, p);
}

Color TextColor(Obj textBlock) {
    Color c{1, 1, 1, 1};
    const int off = eng::NestedOffset(eng::FindClass("TextBlock"), {"ColorAndOpacity", "SpecifiedColor"});
    if (off >= 0 && textBlock) std::memcpy(&c, textBlock + off, sizeof c);
    return c;
}

void SetFontSize(Obj widget, float size, std::vector<const char*> fontPath) {
    fontPath.push_back("Size");
    eng::Prop last;
    const int off = widget ? eng::NestedOffset(eng::ClassOf(widget), fontPath, &last) : -1;
    if (off >= 0 && last.size == sizeof size) std::memcpy(widget + off, &size, sizeof size);
}

// Plain members only (object pointers, names, numbers): nothing reference-counted is copied.
void CopyFont(Obj from, Obj to, float sizeScale) {
    if (!from || !to) return;
    Obj cls = eng::ClassOf(to);
    const std::vector<std::vector<const char*>> members = {{"Font", "FontObject"}, {"Font", "TypefaceFontName"}, {"Font", "Size"},
                                                           {"ColorAndOpacity", "SpecifiedColor"}, {"ColorAndOpacity", "ColorUseRule"}};
    for (const auto& path : members) {
        eng::Prop last;
        const int off = eng::NestedOffset(cls, path, &last);
        if (off >= 0) std::memcpy(to + off, from + off, last.size);
    }
    if (sizeScale != 1.0f) {
        eng::Prop last;
        const int off = eng::NestedOffset(cls, {"Font", "Size"}, &last);
        float size = 0;
        if (off >= 0 && last.size == sizeof size) {
            std::memcpy(&size, to + off, sizeof size);
            SetFontSize(to, size * sizeScale);
        }
    }
}

void WriteSlateColor(Obj widget, std::vector<const char*> path, Color c) {
    if (!widget) return;
    path.push_back("SpecifiedColor");
    const int color = eng::NestedOffset(eng::ClassOf(widget), path);
    path.back() = "ColorUseRule";
    const int rule = eng::NestedOffset(eng::ClassOf(widget), path);
    if (color < 0 || rule < 0) return;
    std::memcpy(widget + color, &c, sizeof c);
    widget[rule] = 0;                               // ESlateColorStylingMode::UseColor_Specified
}

void Unfocusable(Obj widget) {
    for (const char* name : {"IsFocusable", "bIsFocusable"})
        if (widget && eng::FindProp(eng::ClassOf(widget), name)) eng::WriteBool(widget, name, false);
}

void Transparent(Obj button) {
    if (!button) return;
    for (const char* state : {"Normal", "Hovered", "Pressed", "Disabled"}) {
        const int off = eng::NestedOffset(eng::ClassOf(button), {"WidgetStyle", state, "DrawAs"});
        if (off >= 0) button[off] = 0;      // ESlateBrushDrawType::NoDrawType
    }
}

Obj Block(Obj outer, float width, float height, Color c) {
    Obj box = Spawn("SizeBox", outer), fill = Spawn("Border", outer);
    if (!box || !fill) return nullptr;
    eng::Call(box, "SetWidthOverride", width);
    eng::Call(box, "SetHeightOverride", height);
    eng::Call(fill, "SetBrushColor", c);
    AddChild(box, fill);
    return box;
}

Obj FindFirst(Obj widget, Obj cls) {
    std::vector<std::pair<Obj, int>> pending{{widget, 0}};
    while (!pending.empty()) {
        auto [w, depth] = pending.back();
        pending.pop_back();
        if (!w || depth > 12) continue;
        if (eng::IsA(w, cls)) return w;
        // Pushed in reverse so the search runs in order: a user widget's own tree first, then panel slots.
        Obj widgetClass = eng::ClassOf(w);
        if (eng::FindProp(widgetClass, "Slots")) {
            const auto slots = eng::ReadObjArray(w, "Slots");
            for (auto it = slots.rbegin(); it != slots.rend(); ++it) pending.push_back({eng::ReadObj(*it, "Content"), depth + 1});
        }
        if (eng::FindProp(widgetClass, "WidgetTree"))
            pending.push_back({eng::ReadObj(eng::ReadObj(w, "WidgetTree"), "RootWidget"), depth + 1});
    }
    return nullptr;
}

}  // namespace ui::widgets
