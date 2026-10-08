#pragma once

// Window: the OS window and its renderer, frame pacing, and the core's own interface: tab rows, grips, the focus border, dragging, menus and the list of prefix keys. The only module that calls Clay.

#include "interface/Layout.h"

#pragma region Declarations

/// @brief Declares the window's settings, then opens the main OS window and its renderer with the core's font.
/// @param title Title of the OS window.
/// @return SHUResult_Ok, SHUResult_ErrBadData if the core's settings file gives a setting no valid value, SHUResult_ErrFile if the font cannot be loaded, SHUResult_ErrInternal if SDL fails, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSIWindow_Initialize(const char *title);

/// @brief Closes the OS window and frees what the window holds. Also releases what a failed ECSIWindow_Initialize made.
void ECSIWindow_Terminate(void);

/// @brief Gets the main OS window, for dialogs that belong to it.
/// @return The window.
SDL_Window *ECSIWindow_GetMain(void);

/// @brief Gets how long the main loop may wait before the window is drawn again.
/// @return -1 if no frame is needed or the window cannot be seen, 0 if a frame is due, or the milliseconds until the frame limit allows the next frame.
i32 ECSIWindow_GetFrameWait(void);

/// @brief Draws the current workspace: panels first, then the core's own interface.
/// @param nowTicks Current time in nanoseconds.
void ECSIWindow_Render(u64 nowTicks);

/// @brief Draws a frame and saves it as a PNG file, for tests.
/// @param path Path of the file.
/// @return SHUResult_Ok, or SHUResult_ErrFile if the picture cannot be read or saved.
SHUWUR SHUResult ECSIWindow_Screenshot(const char *path);

/// @brief Handles a press of the main pointer button on the core's interface: dividers, tabs, close buttons, tab rows and grips. A press on a tab or grip can start dragging its panel, and a press on a tab row's empty part its whole group.
/// @param x Horizontal position in layout units.
/// @param y Vertical position in layout units.
/// @return true if the core's interface used the press.
bool ECSIWindow_PointerDown(f32 x, f32 y);

/// @brief Handles pointer movement: dragging a divider, a panel or a group, showing grips, and the pointer's shape over dividers.
/// @param x Horizontal position in layout units.
/// @param y Vertical position in layout units.
/// @return true if a divider or a panel is being dragged.
bool ECSIWindow_PointerMove(f32 x, f32 y);

/// @brief Handles a release of the main pointer button: a dragged panel or group lands where the pointer is.
/// @return The panel whose grip was clicked without dragging, for its menu, or NULL.
ECSPanel ECSIWindow_PointerUp(void);

/// @brief Finds the panel whose tab or grip is at a point, as drawn in the last frame.
/// @param x Horizontal position in layout units.
/// @param y Vertical position in layout units.
/// @return The panel, or NULL.
ECSPanel ECSIWindow_TabAt(f32 x, f32 y);

/// @brief Finds the group whose tab row is at a point, on a tab or not.
/// @param x Horizontal position in layout units.
/// @param y Vertical position in layout units.
/// @return The group's shown panel, or NULL.
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
/// @param type The data's type, or NULL to hide the marks. It must stay valid while it is shown.
/// @param x Horizontal position of the pointer in layout units.
/// @param y Vertical position of the pointer in layout units.
void ECSIWindow_ShowDataDrag(const char *type, f32 x, f32 y);

/// @brief Shows or hides the list of keys that follow the core prefix.
/// @param lines Each key's text and description, one after the other; a NULL key text makes the description a heading. They must stay valid while they are shown.
/// @param count Number of lines; 0 hides the list.
void ECSIWindow_ShowPrefixKeys(const char *const *lines, usz count);

/// @brief Shows, changes or hides a menu: the panel menu at level 0, and its submenus at the next levels. The menus at deeper levels close. It is kept inside the OS window.
/// @param level The menu's level, below 4.
/// @param anchor In layout units: the point the panel menu opens at, or the entry a submenu opens beside.
/// @param lines Each entry's key text and label, one after the other. They must stay valid while they are shown.
/// @param count Number of entries; 0 hides the menu. The first entry is highlighted.
void ECSIWindow_ShowMenu(usz level, SDL_FRect anchor, const char *const *lines, usz count);

/// @brief Highlights an entry of a shown menu.
void ECSIWindow_SelectMenuItem(usz level, usz index);

/// @brief Gets where a shown menu's entry is, to open a submenu beside it.
/// @return The entry's rectangle, in layout units.
SDL_FRect ECSIWindow_MenuItemRect(usz level, usz index);

/// @brief Finds the menu entry at a point, in the deepest menu that holds it.
/// @return true if there is an entry at the point.
bool ECSIWindow_MenuItemAt(f32 x, f32 y, usz *retLevel, usz *retIndex);

/// @brief Checks whether a point is on any shown menu.
bool ECSIWindow_MenuContains(f32 x, f32 y);

#pragma endregion Declarations
