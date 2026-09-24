// Building blocks for making and styling the game's own UMG widgets through reflection. Every function takes
// objects obtained this frame and does nothing when they are null.
#pragma once
#include <string>
#include <vector>

#include "engine.hpp"
#include "ui.hpp"

namespace ui::widgets {

using eng::Obj;

struct Margin {
    float left, top, right, bottom;
};
struct Vec2 {
    double x, y;
};

// ESlateVisibility
constexpr uint8_t kCollapsed = 1, kHitTestInvisible = 3, kSelfHitTestInvisible = 4;
// EHorizontalAlignment / EVerticalAlignment
constexpr uint8_t kAlignFill = 0, kAlignLeft = 1, kAlignCenter = 2, kAlignEnd = 3;

// A new widget of a UMG class ("TextBlock", "Button", ...), owned by `outer` (a WidgetTree or UserWidget).
// It must be added to a panel in the same frame, or the garbage collector removes it within seconds.
Obj Spawn(const char* className, Obj outer);

Obj AddChild(Obj panel, Obj child);                                 // generic PanelWidget.AddChild; returns the slot
Obj AddToRow(Obj row, Obj child, float leftPadding);               // HorizontalBox, vertically centred
Obj AddToOverlay(Obj overlay, Obj child, uint8_t alignX, uint8_t alignY, Margin padding);
Obj AddToCanvas(Obj canvas, Obj child, double anchorX, double anchorY, Vec2 pivot, Vec2 position);
// Stretched between two anchor points (fractions of the canvas), whatever the content's size.
Obj StretchOnCanvas(Obj canvas, Obj child, double minX, double minY, double maxX, double maxY);
// A HorizontalBoxSlot or VerticalBoxSlot takes the space left over by its siblings.
void FillSlot(Obj slot);

void SetVisibility(Obj widget, uint8_t visibility);
void SetText(Obj textWidget, const std::string& s);                 // TextBlock or EditableTextBox
void SetHintText(Obj editableTextBox, const std::string& s);
// The text a TextBlock or EditableTextBox shows. Converting it leaves one small engine string unfreed (the host
// has no engine allocator to free it with), so this is for occasional reads such as a submitted command.
std::string ReadText(Obj textWidget);
void SetTextColor(Obj textBlock, Color c);
Color TextColor(Obj textBlock);
// The Size of an FSlateFontInfo member; by default a TextBlock's Font.
void SetFontSize(Obj widget, float size, std::vector<const char*> fontPath = {"Font"});
void CopyFont(Obj from, Obj to, float sizeScale = 1.0f);            // font and colour of another text block
void WriteSlateColor(Obj widget, std::vector<const char*> path, Color c);   // an FSlateColor member
void Unfocusable(Obj widget);           // so Space never presses the last clicked control
void Transparent(Obj button);           // a Button that draws nothing but its content

Obj Block(Obj outer, float width, float height, Color c);          // a solid rectangle
Obj FindFirst(Obj widget, Obj cls);     // the first widget of a class under `widget`, searching user widgets

}  // namespace ui::widgets
