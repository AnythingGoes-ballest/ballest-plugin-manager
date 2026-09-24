# Windows and widgets

Plugins show things with three kinds of UI, all built from the game's own widgets:

| | Made with | Good for |
|---|---|---|
| **Footer button** | `UI::AddFooterButton("label")` | An entry point in every menu: the bar at the bottom of the main menu, the in-race menus and the track editor. |
| **Panel** | `UI::CreatePanel()` | A few lines and buttons that open above the footer. |
| **Window** | `UI::CreateWindow()` | Anything else: overlays, menus, controls. |

Build everything once in `Main()` and keep the handles in global variables. In `Update()`, read what the player did
and change what's shown.

## Placing a window

A window is placed by three things:

- **Anchor**: a point on the screen, as fractions. `(0, 0)` is the top left, `(1, 1)` the bottom right.
- **Pivot**: the point of the window that sits on the anchor, also as fractions.
- **Offset**: pixels to move it from there.

```cpp
// Top right corner, 40 pixels in from each edge
window.SetAnchor(1, 0);
window.SetPivot(1, 0);
window.SetOffset(-40, 40);
```

```cpp
// Centred on screen
window.SetAnchor(0.5f, 0.5f);
window.SetPivot(0.5f, 0.5f);
```

A new window sits centred at the bottom of the screen, 40 pixels up.

## Rows

Widgets go left to right in rows. `NewRow()` starts the next row. `AddSpace(0)` takes up the leftover room, pushing
what comes after it to the right:

```cpp
window.AddText("Show timer", 18);
window.AddSpace(0);
UI::Button@ toggle = window.AddButton("on");
window.NewRow();
window.AddText("restarts 12", 18);
```

A hidden widget (`widget.visible = false`) takes no space, so its row closes up.

## Reading what the player did

Buttons, dropdowns, text boxes and tick boxes report events with functions that return `true` **once**: reading
them clears them. Check each one in a single place, every frame:

```cpp
void Update(float dt)
{
    if (resetButton.Clicked())      // a click
        seconds = 0;
    if (speedBox.Changed())         // the player picked an option
        speed = speedBox.selected;
    if (nameInput.Submitted())      // Enter in a text box
        name = nameInput.text;
    if (showBox.Changed())          // ticked or unticked
        timerWindow.visible = showBox.checked;
}
```

Setting a widget's value from your code (`speedBox.selected = 2`, `showBox.checked = true`) doesn't count as a
change.

## The mouse cursor

Clicks only reach your widgets while the cursor is on screen. The game shows it in its menus, including the pause
menu. For a window meant to be clicked during a race, ask for the cursor while it's open:

```cpp
window.visible = open;
UI::SetCursorVisible(open);
```

`UI::CursorShown()` tells you whether the cursor is on screen for any reason. It's handy for showing buttons only
when they can be clicked.

## Movable windows

Set `movable` after placing the window, and the player can drag it whenever the cursor is on screen. Where they
leave it is saved, and **reset position** on the plugin's settings page puts it back where you placed it.

```cpp
window.SetOffset(40, 40);
window.movable = true;
```

## Menus: a header, views and cards

For something bigger, like the plugin manager's own menu, a window can have a **header** (rows shown above every
view, for tabs), several **views** (only one shows at a time) and **cards** (rows grouped in a rounded box):

```cpp
window.SetScreenSize(0.6f, 0.7f);           // 60% by 70% of the screen
window.SetBlocksClicks(true);               // clicks never reach the game underneath
window.zOrder = 500;                        // in front of other windows (default 100)
window.SetCardBackground(0.012f, 0.014f, 0.019f, 1);

window.StartHeader();                       // the tabs, shown with every view
UI::Button@ statsTab = window.AddButton("stats");
UI::Button@ optionsTab = window.AddButton("options");

int statsView = window.StartView();         // the header ends here
window.StartCard();
window.AddText("Best time", 19);
window.AddSpace(0);
window.AddText("0:42.17", 19);
window.EndCard();

int optionsView = window.StartView();
window.StartCard();
window.AddText("options go here", 19);
window.EndCard();
window.ShowView(statsView);

// in Update:
if (optionsTab.Clicked())
    window.ShowView(optionsView);
```

`ClearView(n)` empties a view so you can fill it again, for example when a list changes. A window can also have a
sidebar (`StartSidebar(width)`, then `StartMain()`) for navigation down the left instead of tabs.

## Colours

Colours are red, green, blue and opacity from 0 to 1, in **linear** values: the game brightens them on screen, so a
linear `0.05` shows as roughly `0.25`. To match a colour picked on screen (0 to 1 per channel, `c`), use
`c / 12.92` for `c` up to `0.04`, else `((c + 0.055) / 1.055) ^ 2.4`. A near-black window background is around
`0.005`.

## Text boxes and typing

While the player types in a text box, the keys go to the box. The game doesn't get them, and `Input::Pressed`
reports nothing. So a hotkey in your plugin never fires by accident while someone types.

## A section in the track editor

`DockInEditorDetails()` turns a window into a section of the track editor's details panel, shown under the game's
transform and paint sections while pieces are selected:

```cpp
@section = UI::CreateWindow();
section.DockInEditorDetails();
section.SetBackground(0, 0, 0, 0);          // blend in with the panel
section.AddText("my tools", 18);
```

See [Create Extensions](https://github.com/AnythingGoes-ballest/ballest-create-extensions) for a full example.

Every window and widget function is in the [UI reference](../reference/api/ui.md).
