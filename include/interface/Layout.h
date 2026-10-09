#pragma once

// Layout: workspaces and their layout trees: operations, tidying, sizes, focus, closed panels and saving.

#include "interface/Panels.h"

#pragma region Declarations

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

/// @brief The layout of one OS window: the main window, or a pop-out window (DESIGN 6.1).
typedef struct ECSIRoot
{
    u32 id;              // unique while the program runs; the window finds its root by it
    ECSINode *tree;      // NULL when the root has no panels; only a workspace's main root is ever empty
    ECSINode *maximized; // group that fills the OS window, or NULL
    f32 width;           // size of the OS window, in layout units
    f32 height;
} ECSIRoot;

/// @brief Describes a root for ECSILayout_WorkspaceAdd.
typedef struct ECSIRootDesc
{
    ECSINode *tree;      // the layout tree, or NULL
    ECSINode *maximized; // group of the tree that fills the OS window, or NULL
    f32 width;           // a pop-out window's size, in layout units
    f32 height;
} ECSIRootDesc;

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
    ECSIZone_PopOut, // a new pop-out window
} ECSIZone;

/// @brief A place to drop a panel.
typedef struct ECSIDrop
{
    ECSIZone zone;
    ECSIRoot *root;  // the root it lands in; NULL for a pop-out
    ECSINode *group; // the target group; NULL for the window's edges and a pop-out
    usz index;       // tabs: the gap between tabs, starting at 0
    f32 width;       // pop-out: the new window's size in layout units; 0 for the panel's size
    f32 height;
} ECSIDrop;

/// @brief Function called for each visible group.
typedef void (*ECSILayoutGroupFunction)(ECSINode *group, void *userData);

/// @brief Function that forgets the nodes the window points to, because nodes may have been freed or hidden.
typedef void (*ECSILayoutForgetFunction)(void);

/// @brief Sets the function that runs after the trees change, so the window forgets the nodes it points to.
/// @param function The function, or NULL.
void ECSILayout_SetForget(ECSILayoutForgetFunction function);

/// @brief Declares the layout's settings: the tab row height, the divider width and how many closed panels can be reopened.
/// @return SHUResult_Ok, SHUResult_ErrBadData if the core's settings file gives a setting no value, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSILayout_Initialize(void);

/// @brief Gets the height of a tab row, from ecs.tabRowHeight.
/// @return The height in layout units.
f32 ECSILayout_GetTabRowHeight(void);

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

/// @brief Adds a workspace. Its trees are tidied, and a pop-out root without panels is left out.
/// @param name Name of the workspace.
/// @param roots The main window's root, then one for each pop-out window. The workspace owns their trees from now on, also when it fails.
/// @param count Number of roots, at least 1.
/// @param focus Panel of the trees to focus, or NULL for the first panel.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSILayout_WorkspaceAdd(const char *name, const ECSIRootDesc *roots, usz count, ECSPanel focus);

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

/// @brief Gets the roots of the current workspace: the main window's, then the pop-out windows', in the order they were made.
/// @param retCount Gets the number of roots; 0 if there is no workspace.
/// @return The roots, valid until the layout changes.
ECSIRoot *const *ECSILayout_GetRoots(usz *retCount);

/// @brief Finds a root of any workspace by its id.
/// @param id The root's id.
/// @param retShown Gets whether the root belongs to the current workspace, or NULL.
/// @return The root, or NULL if it is gone.
ECSIRoot *ECSILayout_FindRoot(u32 id, bool *retShown);

/// @brief Finds the root that holds a panel, in any workspace.
/// @param panel The panel, or NULL.
/// @return The root, or NULL.
ECSIRoot *ECSILayout_RootOf(ECSPanel panel);

/// @brief Sets the size of a root's OS window, which ECSILayout_Update lays the root out in.
/// @param root The root.
/// @param width Width in layout units.
/// @param height Height in layout units.
void ECSILayout_SetRootSize(ECSIRoot *root, f32 width, f32 height);

/// @brief Gets every panel of a root, for closing them.
/// @param root The root.
/// @return stb_ds array of the panels. Free it with arrfree.
ECSPanel *ECSILayout_GetRootPanels(const ECSIRoot *root);

/// @brief Gives the focus to the panel of a root that had it last, when the root's OS window gets the system's focus. Nothing changes if the focused panel is in the root.
/// @param root The root.
void ECSILayout_FocusRoot(ECSIRoot *root);

/// @brief Checks whether the user can pop a panel out: its group is not locked, and it is not alone in its OS window.
/// @param panel The panel.
bool ECSILayout_CanPopOut(ECSPanel panel);

/// @brief Pops the focused panel out into a new OS window, as the user does. Locks stop it, and a panel alone in its OS window stays.
void ECSILayout_PopOut(void);

/// @brief Asks for the window to be drawn again.
void ECSILayout_RequestFrame(void);

/// @brief Checks whether the window needs a frame: one was asked for, or a visible panel draws continuously.
bool ECSILayout_WantsFrame(void);

/// @brief Computes every rectangle of the current workspace's roots for their sizes, and tells the panels that became visible or hidden, or changed size. A frame starts with it, so it clears the request for one.
void ECSILayout_Update(void);

/// @brief Calls a function for every visible group of a root.
/// @param root The root, or NULL for none.
/// @param function The function.
/// @param userData Passed to the function.
void ECSILayout_ForEachGroup(const ECSIRoot *root, ECSILayoutGroupFunction function, void *userData);

/// @brief Finds the group that holds a panel in the current workspace.
/// @param panel The panel, or NULL.
/// @return The group, or NULL.
ECSINode *ECSILayout_GroupOf(ECSPanel panel);

/// @brief Finds the visible group of a root at a point.
/// @param root The root, or NULL for none.
/// @param x Horizontal position in layout units.
/// @param y Vertical position in layout units.
/// @return The group, or NULL.
ECSINode *ECSILayout_GroupAt(const ECSIRoot *root, f32 x, f32 y);

/// @brief Finds the divider of a root at a point, between two children of a split. A maximized group hides the dividers.
/// @param root The root, or NULL for none.
/// @param x Horizontal position in layout units.
/// @param y Vertical position in layout units.
/// @param retSplit Gets the split.
/// @param retDivider Gets the position of the child before the divider.
/// @return true if there is a divider at the point.
bool ECSILayout_DividerAt(const ECSIRoot *root, f32 x, f32 y, ECSINode **retSplit, usz *retDivider);

/// @brief Moves a divider to a point. Fixed children keep fixed sizes; shared children turn their sizes into shares.
/// @param split The split.
/// @param divider Position of the child before the divider.
/// @param x Horizontal position in layout units.
/// @param y Vertical position in layout units.
void ECSILayout_MoveDivider(ECSINode *split, usz divider, f32 x, f32 y);

/// @brief Moves a panel, or its whole group, to a drop place in the current workspace, as the user does: within its OS window, into another one, or out into a new one. Locks stop it. The panel, or the group's shown panel, gets the focus.
/// @param panel The panel.
/// @param group true to move the panel's whole group.
/// @param drop Where it lands.
void ECSILayout_Drop(ECSPanel panel, bool group, const ECSIDrop *drop);

/// @brief Finds the visible panel of a root under a point.
/// @param root The root, or NULL for none.
/// @param x Horizontal position in layout units.
/// @param y Vertical position in layout units.
/// @return The panel, or NULL.
ECSPanel ECSILayout_PanelAt(const ECSIRoot *root, f32 x, f32 y);

/// @brief Gets the focused panel of the current workspace.
/// @return The panel, or NULL if the workspace has no panels.
ECSPanel ECSILayout_GetFocus(void);

/// @brief Sets the focused panel of the current workspace. The panel that loses focus and the one that gets it are told.
/// @param panel Panel to focus.
void ECSILayout_SetFocus(ECSPanel panel);

/// @brief Finds the nearest panel in a direction from the focused panel, in its OS window.
/// @param dx -1 for left, 1 for right, 0 otherwise.
/// @param dy -1 for up, 1 for down, 0 otherwise.
/// @return The panel, or NULL if there is none in that direction.
ECSPanel ECSILayout_FindNeighbour(i32 dx, i32 dy);

/// @brief Moves the focused panel into the group of the nearest panel in a direction, or docks it along that edge of its OS window if there is none.
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
