#pragma once

// Layout: the OS window, workspaces, layout trees, and the core's own interface (tab rows, grips, focus border).

#include "interface/Panels.h"

#pragma region Declarations

/// @brief A node of a layout tree: a split or a group.
typedef struct ECSI_Node ECSI_Node;

/// @brief Opens the main OS window, loads the font of the core's interface, and prepares the layout.
/// @param title Title of the OS window.
/// @param fontPath Path of the TrueType font of the core's interface.
/// @return SHUResult_Ok, SHUResult_ErrFile if the font cannot be loaded, SHUResult_ErrInternal if SDL fails, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSI_LayoutInitialize(const char *title, const char *fontPath);

/// @brief Destroys every workspace, panel and node, then closes the OS window. Also releases what a failed ECSI_LayoutInitialize made.
void ECSI_LayoutTerminate(void);

/// @brief Creates a split node.
/// @param retNode The new split.
/// @param vertical true if its children are stacked from top to bottom.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSI_LayoutSplitCreate(ECSI_Node **retNode, bool vertical);

/// @brief Creates an empty group node.
/// @param retNode The new group.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSI_LayoutGroupCreate(ECSI_Node **retNode);

/// @brief Adds a child to a split.
/// @param split The split.
/// @param child Child to add. The split owns it from now on.
/// @param fixedSize Size in layout units, or 0 to use a share of the remaining space.
/// @param share Share of the remaining space, used when fixedSize is 0.
void ECSI_LayoutSplitAdd(ECSI_Node *split, ECSI_Node *child, f32 fixedSize, f32 share);

/// @brief Adds a panel to a group.
/// @param group The group.
/// @param panel Panel to add. The group owns it from now on.
void ECSI_LayoutGroupAdd(ECSI_Node *group, ECSPanel panel);

/// @brief Chooses the panel a group shows.
/// @param group The group.
/// @param index Position of the panel, starting at 0. Ignored if the group has no such panel.
void ECSI_LayoutGroupShow(ECSI_Node *group, usz index);

/// @brief Locks or unlocks a group: the user cannot move or close the panels of a locked group, and it accepts no dropped panels.
/// @param group The group.
/// @param locked true to lock it.
void ECSI_LayoutGroupSetLocked(ECSI_Node *group, bool locked);

/// @brief Destroys a node, its children and their panels, and sets the handle to NULL.
/// @param node Node to destroy. Must not be part of a workspace.
void ECSI_LayoutNodeDestroy(ECSI_Node **node);

/// @brief Adds a workspace.
/// @param name Name of the workspace.
/// @param tree Layout tree of the main OS window, or NULL. The workspace owns it from now on.
/// @param focus Panel of the tree to focus, or NULL for the first panel.
/// @param maximized Group of the tree that fills the window, or NULL.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSI_LayoutWorkspaceAdd(const char *name, ECSI_Node *tree, ECSPanel focus, ECSI_Node *maximized);

/// @brief Switches to a workspace.
/// @param index Position of the workspace, starting at 0. Ignored if there is no such workspace.
void ECSI_LayoutWorkspaceSwitch(usz index);

/// @brief Describes every workspace for a session: its name, its layout tree with each panel's saved state, the focused panel and the maximized group.
/// @param retWorkspaces The value to set to a list of workspaces.
/// @param retCurrent Position of the current workspace, starting at 0.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSI_LayoutSave(ECSValue *retWorkspaces, usz *retCurrent);

/// @brief Gets the current workspace.
/// @return Its position, starting at 0.
usz ECSI_LayoutGetCurrentWorkspace(void);

/// @brief Asks for the window to be drawn again.
void ECSI_LayoutRequestFrame(void);

/// @brief Gets how long the main loop may wait before the window is drawn again.
/// @return -1 if no frame is needed or the window cannot be seen, 0 if a frame is due, or the milliseconds until the frame limit allows the next frame.
i32 ECSI_LayoutGetFrameWait(void);

/// @brief Draws the current workspace: panels first, then the core's own interface.
/// @param nowTicks Current time in nanoseconds.
void ECSI_LayoutRender(u64 nowTicks);

/// @brief Finds the visible panel under a point.
/// @param x Horizontal position in layout units.
/// @param y Vertical position in layout units.
/// @return The panel, or NULL.
ECSPanel ECSI_LayoutPanelAt(f32 x, f32 y);

/// @brief Handles a press of the main pointer button on the core's interface: dividers, tabs, close buttons, tab rows and grips. A press on a tab or grip can start dragging its panel, and a press on a tab row's empty part its whole group.
/// @param x Horizontal position in layout units.
/// @param y Vertical position in layout units.
/// @return true if the core's interface used the press.
bool ECSI_LayoutPointerDown(f32 x, f32 y);

/// @brief Handles pointer movement: dragging a divider, a panel or a group, showing grips, and the pointer's shape over dividers.
/// @param x Horizontal position in layout units.
/// @param y Vertical position in layout units.
/// @return true if a divider or a panel is being dragged.
bool ECSI_LayoutPointerMove(f32 x, f32 y);

/// @brief Handles a release of the main pointer button: a dragged panel or group lands where the pointer is.
/// @return The panel whose grip was clicked without dragging, for its menu, or NULL.
ECSPanel ECSI_LayoutPointerUp(void);

/// @brief Finds the panel whose tab or grip is at a point, as drawn in the last frame.
/// @param x Horizontal position in layout units.
/// @param y Vertical position in layout units.
/// @return The panel, or NULL.
ECSPanel ECSI_LayoutTabAt(f32 x, f32 y);

/// @brief Finds the group whose tab row is at a point, on a tab or not.
/// @param x Horizontal position in layout units.
/// @param y Vertical position in layout units.
/// @return The group's shown panel, or NULL.
ECSPanel ECSI_LayoutTabRowAt(f32 x, f32 y);

/// @brief Scrolls the tab row at a point, if there is one.
/// @param x Horizontal position in layout units.
/// @param y Vertical position in layout units.
/// @param steps Wheel steps; positive scrolls toward the last tab.
/// @return true if a tab row was scrolled.
bool ECSI_LayoutScrollTabs(f32 x, f32 y, f32 steps);

/// @brief Stops dragging a panel without moving it.
/// @return true if a panel was being dragged.
bool ECSI_LayoutCancelDrag(void);

/// @brief Gets the focused panel of the current workspace.
/// @return The panel, or NULL if the workspace has no panels.
ECSPanel ECSI_LayoutGetFocus(void);

/// @brief Sets the focused panel of the current workspace. The panel that loses focus and the one that gets it are told.
/// @param panel Panel to focus.
void ECSI_LayoutSetFocus(ECSPanel panel);

/// @brief Finds the nearest panel in a direction from the focused panel.
/// @param dx -1 for left, 1 for right, 0 otherwise.
/// @param dy -1 for up, 1 for down, 0 otherwise.
/// @return The panel, or NULL if there is none in that direction.
ECSPanel ECSI_LayoutFindNeighbour(i32 dx, i32 dy);

/// @brief Moves the focused panel into the group of the nearest panel in a direction, or docks it along that edge of the OS window if there is none.
/// @param dx -1 for left, 1 for right, 0 otherwise.
/// @param dy -1 for up, 1 for down, 0 otherwise.
void ECSI_LayoutMoveFocus(i32 dx, i32 dy);

/// @brief Shows the next tab of the focused panel's group.
/// @return The panel shown now, or NULL.
ECSPanel ECSI_LayoutNextTab(void);

/// @brief Checks whether a panel's group is locked.
/// @param panel The panel.
/// @return true if the user cannot move or close the panel.
bool ECSI_LayoutIsLocked(ECSPanel panel);

/// @brief Checks whether a panel's group is maximized.
/// @param panel The panel.
/// @return true if the group fills its workspace.
bool ECSI_LayoutIsMaximized(ECSPanel panel);

/// @brief Gets the panels of a panel's group in the current workspace.
/// @param panel The panel.
/// @param retCount Gets the number of panels; 0 if the panel is not in the current workspace.
/// @param retShown Gets the position of the shown panel.
/// @return The panels, valid until the layout changes, or NULL.
const ECSPanel *ECSI_LayoutGetGroup(ECSPanel panel, usz *retCount, usz *retShown);

/// @brief Shows a panel's tab in its group of the current workspace, and focuses the panel.
/// @param panel The panel.
void ECSI_LayoutShowTab(ECSPanel panel);

/// @brief Checks whether there is a closed panel that ECSI_LayoutReopen can open again.
bool ECSI_LayoutCanReopen(void);

/// @brief Locks the focused panel's group, or unlocks it if it is locked.
void ECSI_LayoutToggleLock(void);

/// @brief Maximizes the focused panel's group, or restores it if it is maximized.
void ECSI_LayoutToggleMaximize(void);

/// @brief Checks whether a panel is in the layout of any workspace. It compares pointers only, so the panel may already be destroyed.
/// @param panel The panel.
/// @return true if a workspace holds the panel.
bool ECSI_LayoutHasPanel(ECSPanel panel);

/// @brief Closes a panel of any workspace and tidies its tree. If it had focus, the focus moves to a panel near it.
/// @param panel Panel to close.
void ECSI_LayoutClosePanel(ECSPanel panel);

/// @brief Gets the main OS window, for dialogs that belong to it.
/// @return The window.
SDL_Window *ECSI_LayoutGetWindow(void);

/// @brief Gets every panel of every workspace.
/// @return stb_ds array of the panels. Free it with arrfree.
ECSPanel *ECSI_LayoutGetPanels(void);

/// @brief Opens the last closed panel again, with the state it had: next to a panel that stayed in its group, or else in the focused group. Its workspace is shown.
void ECSI_LayoutReopen(void);

/// @brief Moves the focused panel into another workspace's focused group. The current workspace stays shown. Locked panels stay.
/// @param index Position of the other workspace, starting at 0.
void ECSI_LayoutMoveToWorkspace(usz index);

/// @brief Shows or hides the list of keys that follow the core prefix.
/// @param lines Each key's text and description, one after the other; a NULL key text makes the description a heading. They must stay valid while they are shown.
/// @param count Number of lines; 0 hides the list.
void ECSI_LayoutShowPrefixKeys(const char *const *lines, usz count);

/// @brief Shows, changes or hides a menu: the panel menu at level 0, and its submenus at the next levels. The menus at deeper levels close. It is kept inside the OS window.
/// @param level The menu's level, below 4.
/// @param anchor In layout units: the point the panel menu opens at, or the entry a submenu opens beside.
/// @param lines Each entry's key text and label, one after the other. They must stay valid while they are shown.
/// @param count Number of entries; 0 hides the menu. The first entry is highlighted.
void ECSI_LayoutShowMenu(usz level, SDL_FRect anchor, const char *const *lines, usz count);

/// @brief Highlights an entry of a shown menu.
void ECSI_LayoutSelectMenuItem(usz level, usz index);

/// @brief Gets where a shown menu's entry is, to open a submenu beside it.
/// @return The entry's rectangle, in layout units.
SDL_FRect ECSI_LayoutMenuItemRect(usz level, usz index);

/// @brief Finds the menu entry at a point, in the deepest menu that holds it.
/// @return true if there is an entry at the point.
bool ECSI_LayoutMenuItemAt(f32 x, f32 y, usz *retLevel, usz *retIndex);

/// @brief Checks whether a point is on any shown menu.
bool ECSI_LayoutMenuContains(f32 x, f32 y);

#pragma endregion Declarations
