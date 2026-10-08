#pragma once

// Layout: the OS window, workspaces, layout trees, and the core's own interface (tab rows, grips, focus border).

#include "Panels.h"

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

/// @brief Checks whether the window needs a new frame.
/// @return true if the layout changed or a visible panel needs drawing.
bool ECSI_LayoutWantsFrame(void);

/// @brief Draws the current workspace: panels first, then the core's own interface.
/// @param nowTicks Current time in nanoseconds.
void ECSI_LayoutRender(u64 nowTicks);

/// @brief Finds the visible panel under a point.
/// @param x Horizontal position in layout units.
/// @param y Vertical position in layout units.
/// @return The panel, or NULL.
ECSPanel ECSI_LayoutPanelAt(f32 x, f32 y);

/// @brief Handles a pointer press on the core's interface: dividers, tabs and grips. A press on a tab or grip can start dragging its panel.
/// @param x Horizontal position in layout units.
/// @param y Vertical position in layout units.
/// @return true if the core's interface used the press.
bool ECSI_LayoutPointerDown(f32 x, f32 y);

/// @brief Handles pointer movement: dragging a divider or a panel, and showing grips.
/// @param x Horizontal position in layout units.
/// @param y Vertical position in layout units.
/// @return true if a divider or a panel is being dragged.
bool ECSI_LayoutPointerMove(f32 x, f32 y);

/// @brief Handles a pointer release: a dragged panel lands where the pointer is.
void ECSI_LayoutPointerUp(void);

/// @brief Stops dragging a panel without moving it.
/// @return true if a panel was being dragged.
bool ECSI_LayoutCancelDrag(void);

/// @brief Gets the focused panel of the current workspace.
/// @return The panel, or NULL if the workspace has no panels.
ECSPanel ECSI_LayoutGetFocus(void);

/// @brief Sets the focused panel of the current workspace.
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

/// @brief Locks the focused panel's group, or unlocks it if it is locked.
void ECSI_LayoutToggleLock(void);

/// @brief Maximizes the focused panel's group, or restores it if it is maximized.
void ECSI_LayoutToggleMaximize(void);

/// @brief Closes a panel and tidies the layout tree.
/// @param panel Panel to close.
void ECSI_LayoutClosePanel(ECSPanel panel);

/// @brief Gets every panel of every workspace.
/// @return stb_ds array of the panels. Free it with arrfree.
ECSPanel *ECSI_LayoutGetPanels(void);

/// @brief Shows or hides the list of keys that follow the core prefix.
/// @param lines Each key's text and description, one after the other. They must stay valid while they are shown.
/// @param count Number of keys; 0 hides the list.
void ECSI_LayoutShowPrefixKeys(const char *const *lines, usz count);

#pragma endregion Declarations
