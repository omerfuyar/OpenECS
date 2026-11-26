#pragma once

// Window: the OS windows and their renderers, frame pacing, and the core's own interface: tab rows, grips, the focus border, dragging, menus and the list of prefix keys. The only module that calls Clay.

#include "interface/Popups.h"

#pragma region Declarations

/// @brief Declares the window's settings, then opens the main OS window and its renderer with the core's font.
/// @param title Title of the OS window.
/// @return SHUResult_Ok, SHUResult_ErrBadData if the core's settings file gives a setting no valid value, SHUResult_ErrFile if the font cannot be loaded, SHUResult_ErrInternal if SDL fails, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSIWindow_Initialize(const char *title);

/// @brief Sets what the preset tells a new user, which the title bar that OpenECS draws shows after the title; nullptr or "" shows none.
void ECSIWindow_SetHint(const char *hint);

/// @brief Closes the OS windows and frees what the window holds. Also releases what a failed ECSIWindow_Initialize made.
void ECSIWindow_Terminate(void);

/// @brief Destroys the GPU device, after the plugins that may hold its objects. ECSIWindow_Terminate leaves it, so it outlives the OS windows.
void ECSIWindow_DestroyDevice(void);

/// @brief Gets the main OS window, for dialogs that belong to it.
/// @return The window.
SDL_Window *ECSIWindow_GetMain(void);

/// @brief Gets the OS window of the current workspace with a number, for tests. A root made since the last frame gets its window first.
/// @param number 1 for the main window, then the pop-out windows in the order of ECSILayout_GetRoots.
/// @return The window, or nullptr if there is none with that number.
SDL_Window *ECSIWindow_Get(usz number);

/// @brief Gets the number of the OS window that shows a panel, as ECSIWindow_Get counts them.
/// @return The number, or 0 if the panel is not in the current workspace.
usz ECSIWindow_NumberOf(ECSPanel panel);

/// @brief Gets the OS window that shows a panel, for its text input.
/// @return The window, or the main window if the panel is not shown.
SDL_Window *ECSIWindow_GetPanelWindow(ECSPanel panel);

/// @brief Gets the root that an OS window shows.
/// @param id SDL's id of the window.
/// @param retMain Gets whether it is the main window, or nullptr.
/// @return The root, or nullptr if the window shows none now.
ECSIRoot *ECSIWindow_GetRoot(SDL_WindowID id, bool *retMain);

/// @brief Notes that an OS window got the focus, so it counts as in front of the others when a dragged panel is over windows that overlap.
/// @param id The OS window's id; a window that is not one of the core's OS windows is ignored.
void ECSIWindow_Focused(SDL_WindowID id);

/// @brief Chooses the OS window of the pointer event being handled. The pointer functions below and ECSIWindow_PanelAt take positions in its layout units.
/// @param id SDL's id of the window; an unknown id chooses the main window.
void ECSIWindow_SetEventWindow(SDL_WindowID id);

/// @brief Finds the popup under a point of the event window: the popup whose SDL popup window it is, or the newest popup drawn inside it that holds the
/// point, other than a tooltip.
/// @param x Horizontal position in layout units.
/// @param y Vertical position in layout units.
/// @param retX Gets the point's position in the popup's surface.
/// @param retY Gets the point's position in the popup's surface.
/// @return The popup, or nullptr.
ECSPopup ECSIWindow_PopupAt(f32 x, f32 y, f32 *retX, f32 *retY);

/// @brief Converts a point of the event window to a popup's surface pixels, for the popup that got a press, also outside it.
/// @param popup The popup.
/// @param x Horizontal position in layout units.
/// @param y Vertical position in layout units.
/// @param retX Gets the position in the popup's surface.
/// @param retY Gets the position in the popup's surface.
void ECSIWindow_ToPopup(ECSPopup popup, f32 x, f32 y, f32 *retX, f32 *retY);

/// @brief Gets where a popup shows, for tests.
/// @param popup The popup.
/// @param retRect Gets its rectangle in the layout units of its panel's OS window.
/// @param retWindow Gets the number of that OS window, as ECSIWindow_Get counts them.
/// @return false if it has not been shown yet.
bool ECSIWindow_PopupRect(ECSPopup popup, SDL_FRect *retRect, usz *retWindow);

/// @brief Finds the SDL popup window that shows a point of an OS window, for tests, as the system gives it the pointer.
/// @param window The OS window.
/// @param x Horizontal position in its layout units.
/// @param y Vertical position in its layout units.
/// @param retRect Gets where the popup window shows, in the layout units of the OS window.
/// @return The newest such popup window, or nullptr.
SDL_Window *ECSIWindow_PopupWindowAt(SDL_Window *window, f32 x, f32 y, SDL_FRect *retRect);

/// @brief Finds the visible panel under a point of the event window, or of another OS window that the point falls in on the screen.
/// @param x Horizontal position in layout units.
/// @param y Vertical position in layout units.
/// @param retX Gets the point's position in the OS window of the panel.
/// @param retY Gets the point's position in the OS window of the panel.
/// @return The panel, or nullptr.
ECSPanel ECSIWindow_PanelAt(f32 x, f32 y, f32 *retX, f32 *retY);

/// @brief Gets how long the main loop may wait before the window is drawn again.
/// @return -1 if no frame is needed or the window cannot be seen, 0 if a frame is due, or the milliseconds until the frame limit allows the next frame.
i32 ECSIWindow_GetFrameWait(void);

/// @brief Draws the current workspace in its OS windows: panels first, then the core's own interface. Pop-out windows open, close, show and hide to match the workspace's roots.
/// @param nowTicks Current time in nanoseconds.
void ECSIWindow_Render(u64 nowTicks);

/// @brief Draws a frame and saves an OS window's picture as a PNG file, for tests.
/// @param path Path of the file.
/// @param number The OS window, as ECSIWindow_Get counts them.
/// @return SHUResult_Ok, or SHUResult_ErrFile if there is no such window, or the picture cannot be read or saved.
SHUWUR SHUResult ECSIWindow_Screenshot(const char *path, usz number);

/// @brief Draws a frame and reads one pixel of what an OS window shows, for tests.
/// @param number The OS window, as ECSIWindow_Get counts them.
/// @param x Position in the window's layout units.
/// @param y Position in the window's layout units.
/// @param retColor Gets the pixel, 0xAARRGGBB.
/// @return SHUResult_Ok, or SHUResult_ErrBadData if there is no such window or the position is outside it.
SHUWUR SHUResult ECSIWindow_ReadPixel(usz number, f32 x, f32 y, ECSColor *retColor);

/// @brief Handles a press of the main pointer button on the core's interface: dividers, tabs, close buttons, tab rows and grips. A press on a tab or grip can start dragging its panel, and a press on a tab row's empty part its whole group.
/// @param x Horizontal position in layout units.
/// @param y Vertical position in layout units.
/// @return true if the core's interface used the press.
bool ECSIWindow_PointerDown(f32 x, f32 y);

/// @brief Handles pointer movement: dragging a divider, a panel or a group, also into other OS windows or out of them, showing grips, and the pointer's shape over dividers.
/// @param x Horizontal position in layout units.
/// @param y Vertical position in layout units.
/// @return true if a divider or a panel is being dragged.
bool ECSIWindow_PointerMove(f32 x, f32 y);

/// @brief Handles a release of the main pointer button: a dragged panel or group lands where the pointer is.
/// @return The panel whose grip was clicked without dragging, for its menu, or nullptr.
ECSPanel ECSIWindow_PointerUp(void);

/// @brief Finds the panel whose tab or grip is at a point, as drawn in the last frame.
/// @param x Horizontal position in layout units.
/// @param y Vertical position in layout units.
/// @return The panel, or nullptr.
ECSPanel ECSIWindow_TabAt(f32 x, f32 y);

/// @brief Finds the group whose tab row is at a point, on a tab or not.
/// @param x Horizontal position in layout units.
/// @param y Vertical position in layout units.
/// @return The group's shown panel, or nullptr.
ECSPanel ECSIWindow_TabRowAt(f32 x, f32 y);

/// @brief Shows the next or the previous tab of the tab row at a point of the event window, for a turn of the wheel; the focus goes with the shown panel when the group has it.
/// @param x Horizontal position in layout units.
/// @param y Vertical position in layout units.
/// @param steps Wheel steps; positive shows the next tab. Parts of a step add up.
/// @return true if the point is on a tab row.
bool ECSIWindow_WheelTabs(f32 x, f32 y, f32 steps);

/// @brief Stops dragging a panel without moving it.
/// @return true if a panel was being dragged.
bool ECSIWindow_CancelDrag(void);

/// @brief Shows or hides the marks of a data drag: every visible panel that accepts the data's type is marked, and the one under the pointer is filled.
/// @param type The data's type, or nullptr to hide the marks. It must stay valid while it is shown.
/// @param x Horizontal position of the pointer in the event window's layout units.
/// @param y Vertical position of the pointer in layout units.
void ECSIWindow_ShowDataDrag(const char *type, f32 x, f32 y);

/// @brief Keeps the preset's hint at its start, even when it does not fit, as a test needs; it does not slide from now on.
void ECSIWindow_HoldHint(void);

/// @brief Shows, changes or hides a menu: the panel menu at level 0, and its submenus at the next levels. The menus at deeper levels close. It is kept inside the OS window.
/// @param level The menu's level, below 4.
/// @param panel At level 0: the panel whose OS window shows the menu, for a menu that a function opens, or nullptr for the OS window of the event that opens it.
/// @param anchor In layout units: the point the panel menu opens at, or the entry a submenu opens beside.
/// @param lines Each entry's key text and label, one after the other; a nullptr key text makes the label a heading, with a line above it. They must stay valid while they are shown.
/// @param count Number of entries; 0 hides the menu. The first entry is highlighted.
void ECSIWindow_ShowMenu(usz level, ECSPanel panel, SDL_FRect anchor, const char *const *lines, usz count);

/// @brief Highlights an entry of a shown menu.
void ECSIWindow_SelectMenuItem(usz level, usz index);

/// @brief Gets the highlighted entry of a shown menu.
usz ECSIWindow_GetSelectedMenuItem(usz level);

/// @brief Gets where a shown menu's entry is, to open a submenu beside it.
/// @return The entry's rectangle, in layout units.
SDL_FRect ECSIWindow_MenuItemRect(usz level, usz index);

/// @brief Gets where a shown menu is, for tests.
/// @param level The menu's level, below 4.
/// @param retRect Gets its rectangle, in the layout units of its OS window.
/// @param retWindow Gets the number of that OS window, as ECSIWindow_Get counts them.
void ECSIWindow_MenuRect(usz level, SDL_FRect *retRect, usz *retWindow);

/// @brief Finds the menu entry at a point of the event window, in the deepest menu that holds it.
/// @return true if there is an entry at the point.
bool ECSIWindow_MenuItemAt(f32 x, f32 y, usz *retLevel, usz *retIndex);

/// @brief Tells whether a point of the OS window that has the pointer is on the title bar that OpenECS draws, apart from its buttons.
/// @param x Horizontal position in layout units.
/// @param y Vertical position in layout units.
/// @return true on the title bar; false elsewhere, or when the system draws the title bar.
bool ECSIWindow_TitleBarAt(f32 x, f32 y);

/// @brief Checks whether a point of the event window is on any shown menu.
bool ECSIWindow_MenuContains(f32 x, f32 y);

/// @brief What a part of the title bar that OpenECS draws is (DESIGN 6.11).
typedef enum ECSITitleKind
{
    ECSITitleKind_Name,      // the tool's name, a label
    ECSITitleKind_Menu,      // a menu of ecs.titleMenus
    ECSITitleKind_Button,    // a button of ecs.titleButtons
    ECSITitleKind_Workspace, // the workspaces' menu, at the right
    ECSITitleKind_Window,    // a window button at the right end: minimize, maximize or restore, and close
} ECSITitleKind;

/// @brief A menu or a button of the title bar, as Menus gives it.
typedef struct ECSITitleItem
{
    const char *label;
    ECSITitleKind kind; // ECSITitleKind_Menu, ECSITitleKind_Button or ECSITitleKind_Workspace
    bool on;            // a button whose setting is true, or the menu that is open
    bool enabled;       // false for a button whose function cannot be used now
} ECSITitleItem;

/// @brief A part of the title bar as an OS window drew it in its last frame.
typedef struct ECSIDrawnTitleItem
{
    ECSITitleKind kind;
    usz index;         // for a menu or a button, its place in the items of ECSIWindow_SetTitleItems; for a window button, 1 minimize, 2 maximize or restore, 3 close; 0 for the others
    const char *label; // valid until the items change or the workspace is renamed
    bool on;
    bool enabled;
    SDL_FRect rect; // in layout units
} ECSIDrawnTitleItem;

/// @brief Sets the menus and buttons that the title bar shows after the tool's name. The texts are copied; the window draws again only when they changed.
/// @param items The menus, then the buttons.
void ECSIWindow_SetTitleItems(const ECSITitleItem *items, usz count);

/// @brief Finds the part of the title bar at a point of the event window that Menus handles: a menu, a button or the workspaces' menu.
/// @param retItem Gets the part, as its OS window drew it.
/// @return true if a part is at the point.
bool ECSIWindow_TitleItemAt(f32 x, f32 y, ECSIDrawnTitleItem *retItem);

/// @brief Gets the part of the title bar whose tooltip is due: the pointer has been on it for half a second.
/// @param retItem Gets the part.
/// @return true if a tooltip is due.
bool ECSIWindow_GetDueTooltip(ECSIDrawnTitleItem *retItem);

/// @brief Sets the text of the due tooltip, which shows below its part of the title bar. The texts are copied.
/// @param text What the part does, or nullptr for no tooltip.
/// @param keys The keys that run it, or nullptr.
void ECSIWindow_SetTooltip(const char *text, const char *keys);

/// @brief Draws a frame and gets the parts of an OS window's title bar, as it drew them, for tests.
/// @param number The window's number, as ECSIWindow_Get counts them.
/// @param retCount Gets how many there are.
/// @return The parts, valid until the next frame, or nullptr if there is no such window or it shows no title bar.
const ECSIDrawnTitleItem *ECSIWindow_GetTitleBar(usz number, usz *retCount);

/// @brief Gets the parts of the title bar of a panel's OS window as its last frame drew them.
/// @param panel The panel, or nullptr for the main window.
/// @param retCount Gets how many there are.
/// @return The parts, valid until the next frame, or nullptr when the window shows no title bar.
const ECSIDrawnTitleItem *ECSIWindow_GetDrawnTitle(ECSPanel panel, usz *retCount);

/// @brief Notes that the pointer left the event window, so nothing in it stays under the pointer.
void ECSIWindow_PointerLeave(void);

/// @brief A message dialog that the core draws over an OS window while Input waits for its answer (DESIGN 4.5, 7.7).
typedef struct ECSIDialogView
{
    const char *title;
    const char *message;
    const char *const *buttons;
    usz count;
    usz selected; // the button that Return presses
    i32 hover;    // the button under the pointer, or -1
} ECSIDialogView;

/// @brief Shows a message dialog over the OS window of the event being handled, or over the main window; nullptr hides it. The view lives until it is hidden.
/// @return The SDL id of the OS window that shows it, or 0 when there is none.
SDL_WindowID ECSIWindow_ShowDialog(const ECSIDialogView *dialog);

/// @brief Draws and presents the OS window that shows the dialog, from the panels' last pixels: no panel draws while a dialog waits.
void ECSIWindow_RenderDialog(void);

/// @brief Finds the dialog's button at a point of its OS window, in the last frame.
/// @return The button's index, or -1 if no button is there.
i32 ECSIWindow_DialogButtonAt(f32 x, f32 y);

/// @brief Gives the rectangle of a button of the shown dialog in the last frame, in its OS window's layout units.
/// @return false if no dialog is shown or the button is not laid out.
bool ECSIWindow_DialogButtonRect(usz index, SDL_FRect *retRect);

#pragma endregion Declarations
