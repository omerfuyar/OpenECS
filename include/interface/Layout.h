#pragma once

// Layout: workspaces and their layout trees: operations, tidying, sizes, focus, closed panels and saving.

#include "interface/Panels.h"

#pragma region Declarations

/// @brief Height of a tab row.
#define OPENECS_TAB_ROW_HEIGHT 26.0f

/// @brief A node of a layout tree: a split or a group.
typedef struct ECSINode ECSINode;

/// @brief Type of a layout node.
typedef enum ECSINodeType
{
    ECSINodeType_Split = 0,
    ECSINodeType_Group,
} ECSINodeType;

struct ECSINode
{
    ECSINodeType type;
    ECSINode *parent;
    f32 fixedSize; // size along the parent split, in layout units; 0 to use the share
    f32 share;     // share of the parent split's remaining space
    f32 x;         // rectangle in layout units, computed
    f32 y;
    f32 width;
    f32 height;

    // split
    bool vertical;        // children are stacked from top to bottom
    ECSINode **children; // stb_ds array

    // group; its tab row's fields belong to the window
    ECSPanel *panels;    // stb_ds array
    usz shown;           // index of the panel shown
    bool locked;         // its panels cannot be moved or closed, and it accepts no dropped panels
    f32 tabScroll;       // how far the tab row is scrolled, in layout units
    f32 tabsWidth;       // width of all its tabs together, measured in the last frame
    ECSPanel scrolledTo; // the shown panel whose tab was last scrolled into view
};

/// @brief Where a moved panel lands.
typedef enum ECSIZone
{
    ECSIZone_None = 0,
    ECSIZone_Center, // group with the target
    ECSIZone_Tabs,   // insert between the target's tabs
    ECSIZone_Left,   // split the target toward a side
    ECSIZone_Right,
    ECSIZone_Top,
    ECSIZone_Bottom,
    ECSIZone_WindowLeft, // dock along a whole edge of the OS window
    ECSIZone_WindowRight,
    ECSIZone_WindowTop,
    ECSIZone_WindowBottom,
} ECSIZone;

/// @brief A place to drop a panel.
typedef struct ECSIDrop
{
    ECSIZone zone;
    ECSINode *group; // the target group; NULL for the window's edges
    usz index;        // tabs: the gap between tabs, starting at 0
} ECSIDrop;

/// @brief Function called for each visible group.
typedef void (*ECSILayoutGroupFunction)(ECSINode *group, void *userData);

/// @brief Function that forgets the nodes the window points to, because nodes may have been freed or hidden.
typedef void (*ECSILayoutForgetFunction)(void);

/// @brief Sets the function that runs after the trees change, so the window forgets the nodes it points to.
/// @param function The function, or NULL.
void ECSILayout_SetForget(ECSILayoutForgetFunction function);

/// @brief Destroys every workspace, panel and node.
void ECSILayout_Terminate(void);

/// @brief Creates a split node.
/// @param retNode The new split.
/// @param vertical true if its children are stacked from top to bottom.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSILayout_SplitCreate(ECSINode **retNode, bool vertical);

/// @brief Creates an empty group node.
/// @param retNode The new group.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSILayout_GroupCreate(ECSINode **retNode);

/// @brief Adds a child to a split.
/// @param split The split.
/// @param child Child to add. The split owns it from now on.
/// @param fixedSize Size in layout units, or 0 to use a share of the remaining space.
/// @param share Share of the remaining space, used when fixedSize is 0.
void ECSILayout_SplitAdd(ECSINode *split, ECSINode *child, f32 fixedSize, f32 share);

/// @brief Adds a panel to a group.
/// @param group The group.
/// @param panel Panel to add. The group owns it from now on.
void ECSILayout_GroupAdd(ECSINode *group, ECSPanel panel);

/// @brief Chooses the panel a group shows.
/// @param group The group.
/// @param index Position of the panel, starting at 0. Ignored if the group has no such panel.
void ECSILayout_GroupShow(ECSINode *group, usz index);

/// @brief Locks or unlocks a group: the user cannot move or close the panels of a locked group, and it accepts no dropped panels.
/// @param group The group.
/// @param locked true to lock it.
void ECSILayout_GroupSetLocked(ECSINode *group, bool locked);

/// @brief Destroys a node, its children and their panels, and sets the handle to NULL.
/// @param node Node to destroy. Must not be part of a workspace.
void ECSILayout_NodeDestroy(ECSINode **node);

/// @brief Adds a workspace.
/// @param name Name of the workspace.
/// @param tree Layout tree of the main OS window, or NULL. The workspace owns it from now on.
/// @param focus Panel of the tree to focus, or NULL for the first panel.
/// @param maximized Group of the tree that fills the window, or NULL.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSILayout_WorkspaceAdd(const char *name, ECSINode *tree, ECSPanel focus, ECSINode *maximized);

/// @brief Switches to a workspace.
/// @param index Position of the workspace, starting at 0. Ignored if there is no such workspace.
void ECSILayout_WorkspaceSwitch(usz index);

/// @brief Describes every workspace for a session: its name, its layout tree with each panel's saved state, the focused panel and the maximized group.
/// @param retWorkspaces The value to set to a list of workspaces.
/// @param retCurrent Position of the current workspace, starting at 0.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSILayout_Save(ECSValue *retWorkspaces, usz *retCurrent);

/// @brief Gets the current workspace.
/// @return Its position, starting at 0.
usz ECSILayout_GetCurrentWorkspace(void);

/// @brief Asks for the window to be drawn again.
void ECSILayout_RequestFrame(void);

/// @brief Checks whether the window needs a frame: one was asked for, or a visible panel draws continuously.
bool ECSILayout_WantsFrame(void);

/// @brief Computes every rectangle of the current workspace for the OS window's size, and tells the panels that became visible or hidden, or changed size. A frame starts with it, so it clears the request for one.
/// @param width Width of the OS window in layout units.
/// @param height Height of the OS window in layout units.
void ECSILayout_Update(f32 width, f32 height);

/// @brief Calls a function for every visible group of the current workspace.
/// @param function The function.
/// @param userData Passed to the function.
void ECSILayout_ForEachGroup(ECSILayoutGroupFunction function, void *userData);

/// @brief Finds the group that holds a panel in the current workspace.
/// @param panel The panel, or NULL.
/// @return The group, or NULL.
ECSINode *ECSILayout_GroupOf(ECSPanel panel);

/// @brief Gets the current workspace's tree.
/// @return The tree, or NULL if the workspace has no panels.
ECSINode *ECSILayout_GetTree(void);

/// @brief Finds the visible group at a point.
/// @param x Horizontal position in layout units.
/// @param y Vertical position in layout units.
/// @return The group, or NULL.
ECSINode *ECSILayout_GroupAt(f32 x, f32 y);

/// @brief Finds the divider at a point, between two children of a split. A maximized group hides the dividers.
/// @param x Horizontal position in layout units.
/// @param y Vertical position in layout units.
/// @param retSplit Gets the split.
/// @param retDivider Gets the position of the child before the divider.
/// @return true if there is a divider at the point.
bool ECSILayout_DividerAt(f32 x, f32 y, ECSINode **retSplit, usz *retDivider);

/// @brief Moves a divider to a point. Fixed children keep fixed sizes; shared children turn their sizes into shares.
/// @param split The split.
/// @param divider Position of the child before the divider.
/// @param x Horizontal position in layout units.
/// @param y Vertical position in layout units.
void ECSILayout_MoveDivider(ECSINode *split, usz divider, f32 x, f32 y);

/// @brief Moves a panel, or its whole group, to a drop place in the current workspace, as the user does. Locks stop it. The panel, or the group's shown panel, gets the focus.
/// @param panel The panel.
/// @param group true to move the panel's whole group.
/// @param drop Where it lands.
void ECSILayout_Drop(ECSPanel panel, bool group, const ECSIDrop *drop);

/// @brief Finds the visible panel under a point.
/// @param x Horizontal position in layout units.
/// @param y Vertical position in layout units.
/// @return The panel, or NULL.
ECSPanel ECSILayout_PanelAt(f32 x, f32 y);

/// @brief Gets the focused panel of the current workspace.
/// @return The panel, or NULL if the workspace has no panels.
ECSPanel ECSILayout_GetFocus(void);

/// @brief Sets the focused panel of the current workspace. The panel that loses focus and the one that gets it are told.
/// @param panel Panel to focus.
void ECSILayout_SetFocus(ECSPanel panel);

/// @brief Finds the nearest panel in a direction from the focused panel.
/// @param dx -1 for left, 1 for right, 0 otherwise.
/// @param dy -1 for up, 1 for down, 0 otherwise.
/// @return The panel, or NULL if there is none in that direction.
ECSPanel ECSILayout_FindNeighbour(i32 dx, i32 dy);

/// @brief Moves the focused panel into the group of the nearest panel in a direction, or docks it along that edge of the OS window if there is none.
/// @param dx -1 for left, 1 for right, 0 otherwise.
/// @param dy -1 for up, 1 for down, 0 otherwise.
void ECSILayout_MoveFocus(i32 dx, i32 dy);

/// @brief Shows the next tab of the focused panel's group.
/// @return The panel shown now, or NULL.
ECSPanel ECSILayout_NextTab(void);

/// @brief Checks whether a panel's group is locked.
/// @param panel The panel.
/// @return true if the user cannot move or close the panel.
bool ECSILayout_IsLocked(ECSPanel panel);

/// @brief Checks whether a panel's group is maximized.
/// @param panel The panel.
/// @return true if the group fills its workspace.
bool ECSILayout_IsMaximized(ECSPanel panel);

/// @brief Gets the panels of a panel's group in the current workspace.
/// @param panel The panel.
/// @param retCount Gets the number of panels; 0 if the panel is not in the current workspace.
/// @param retShown Gets the position of the shown panel.
/// @return The panels, valid until the layout changes, or NULL.
const ECSPanel *ECSILayout_GetGroup(ECSPanel panel, usz *retCount, usz *retShown);

/// @brief Shows a panel's tab in its group of the current workspace, and focuses the panel.
/// @param panel The panel.
void ECSILayout_ShowTab(ECSPanel panel);

/// @brief Checks whether there is a closed panel that ECSILayout_Reopen can open again.
bool ECSILayout_CanReopen(void);

/// @brief Locks the focused panel's group, or unlocks it if it is locked.
void ECSILayout_ToggleLock(void);

/// @brief Maximizes the focused panel's group, or restores it if it is maximized.
void ECSILayout_ToggleMaximize(void);

/// @brief Checks whether a panel is in the layout of any workspace. It compares pointers only, so the panel may already be destroyed.
/// @param panel The panel.
/// @return true if a workspace holds the panel.
bool ECSILayout_HasPanel(ECSPanel panel);

/// @brief Closes a panel of any workspace and tidies its tree. If it had focus, the focus moves to a panel near it.
/// @param panel Panel to close.
void ECSILayout_ClosePanel(ECSPanel panel);

/// @brief Gets every panel of every workspace.
/// @return stb_ds array of the panels. Free it with arrfree.
ECSPanel *ECSILayout_GetPanels(void);

/// @brief Opens the last closed panel again, with the state it had: next to a panel that stayed in its group, or else in the focused group. Its workspace is shown.
void ECSILayout_Reopen(void);

/// @brief Moves the focused panel into another workspace's focused group. The current workspace stays shown. Locked panels stay.
/// @param index Position of the other workspace, starting at 0.
void ECSILayout_MoveToWorkspace(usz index);

#pragma endregion Declarations
