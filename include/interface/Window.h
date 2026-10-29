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

/// @brief Finds the popup under a point of the event window: the popup whose SDL popup window it is, or the newest popup drawn inside it that holds the point.
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

/// @brief Scrolls the tab row at a point, if there is one.
/// @param x Horizontal position in layout units.
/// @param y Vertical position in layout units.
/// @param steps Wheel steps; positive scrolls toward the last tab.
/// @return true if a tab row was scrolled.
bool ECSIWindow_ScrollTabs(f32 x, f32 y, f32 steps);

/// @brief Stops dragging a panel without moving it.
/// @return true if a panel was being dragged.
bool ECSIWindow_CancelDrag(void);

/// @brief Shows or hides the marks of a data drag: every visible panel that accepts the data's type is marked, and the one under the pointer is filled.
/// @param type The data's type, or nullptr to hide the marks. It must stay valid while it is shown.
/// @param x Horizontal position of the pointer in the event window's layout units.
/// @param y Vertical position of the pointer in layout units.
void ECSIWindow_ShowDataDrag(const char *type, f32 x, f32 y);

/// @brief Shows or hides the list of keys that follow the core prefix, in the focused panel's OS window.
/// @param lines Each key's text and description, one after the other; a nullptr key text makes the description a heading. They must stay valid while they are shown.
/// @param count Number of lines; 0 hides the list.
void ECSIWindow_ShowPrefixKeys(const char *const *lines, usz count);

/// @brief Shows, changes or hides a menu: the panel menu at level 0, in the event window, and its submenus at the next levels. The menus at deeper levels close. It is kept inside the OS window.
/// @param level The menu's level, below 4.
/// @param anchor In layout units: the point the panel menu opens at, or the entry a submenu opens beside.
/// @param lines Each entry's key text and label, one after the other. They must stay valid while they are shown.
/// @param count Number of entries; 0 hides the menu. The first entry is highlighted.
void ECSIWindow_ShowMenu(usz level, SDL_FRect anchor, const char *const *lines, usz count);

/// @brief Highlights an entry of a shown menu.
void ECSIWindow_SelectMenuItem(usz level, usz index);

/// @brief Gets the highlighted entry of a shown menu.
usz ECSIWindow_GetSelectedMenuItem(usz level);

/// @brief Gets where a shown menu's entry is, to open a submenu beside it.
/// @return The entry's rectangle, in layout units.
SDL_FRect ECSIWindow_MenuItemRect(usz level, usz index);

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
