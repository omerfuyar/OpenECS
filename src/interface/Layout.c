#include "interface/Layout.h"

#include "runtime/Events.h"
#include "runtime/Plugins.h"
#include "runtime/Settings.h"

#include "SDL3/SDL.h"
#include "stb/stbSDL3.h"

#pragma region Source Only

/// @brief Smallest size a divider drag leaves to a child.
#define OPENECS_MIN_CHILD_SIZE 32.0f

/// @brief Smallest size of a new pop-out window, in layout units.
#define OPENECS_MIN_POP_OUT_SIZE 200.0f

/// @brief A named arrangement of panels.
typedef struct ECSIWorkspace
{
    char *name;
    ECSIRoot **roots; // stb_ds array: the main window's root, which is always there, then the pop-out windows'
    ECSPanel focus;
} ECSIWorkspace;

/// @brief A closed panel that ecs.layout.reopen can open again.
typedef struct ECSIClosedPanel
{
    ECSValue *saved; // its type, state and stateVersion, as ECSIPanel_Save writes them
    u32 neighbour;   // id of a panel that stayed in its group, or 0
} ECSIClosedPanel;

static struct
{
    ECSIWorkspace *workspaces; // stb_ds array
    usz current;
    ECSIClosedPanel *closed; // stb_ds array of the panels ecs.layout.reopen can open again, the last closed last
    bool frameNeeded;
    bool emptied; // the user closed the last panel of every workspace
    u32 lastRootID;
    ECSILayoutForgetFunction Forget; // the window's, or nullptr

    // from the settings
    f32 tabRowHeight;
    f32 dividerSize; // gap between the children of a split; dragging it resizes them
    i64 reopenLimit; // how many closed panels ecs.layout.reopen remembers
} LAYOUT = {0};

static ECSIWorkspace *ECSILayout_GetCurrent(void)
{
    return arrlenu(LAYOUT.workspaces) == 0 ? nullptr : &LAYOUT.workspaces[LAYOUT.current];
}

/// @brief Computes the rectangles of a node and its children, and places the shown panel of each group.
static void ECSILayout_Place(ECSINode *node, f32 x, f32 y, f32 width, f32 height)
{
    node->x = x;
    node->y = y;
    node->width = width;
    node->height = height;

    if (node->type == ECSINodeType_Group)
    {
        f32 top = arrlenu(node->panels) >= 2 ? LAYOUT.tabRowHeight : 0.0f;

        if (arrlenu(node->panels) > 0)
        {
            ECSIPanel_SetRect(node->panels[node->shown], x, y + top, width, SDL_max(0.0f, height - top));
        }

        return;
    }

    f32 total = node->vertical ? height : width;
    f32 available = total - LAYOUT.dividerSize * (f32)(arrlenu(node->children) - 1);
    f32 fixedSum = 0.0f;
    f32 shareSum = 0.0f;

    for (usz i = 0; i < arrlenu(node->children); i++)
    {
        if (node->children[i]->fixedSize > 0.0f)
        {
            fixedSum += node->children[i]->fixedSize;
        }
        else
        {
            shareSum += node->children[i]->share;
        }
    }

    f32 remaining = SDL_max(0.0f, available - fixedSum);
    f32 start = node->vertical ? y : x;
    f32 end = start + total;
    f32 position = start;

    for (usz i = 0; i < arrlenu(node->children); i++)
    {
        ECSINode *child = node->children[i];
        f32 size = child->fixedSize > 0.0f ? child->fixedSize : (shareSum > 0.0f ? remaining * child->share / shareSum : 0.0f);

        // the last child takes what is left, so rounding leaves no gap
        size = i + 1 == arrlenu(node->children) ? end - position : SDL_min(size, end - position);
        size = SDL_max(0.0f, size);

        if (node->vertical)
        {
            ECSILayout_Place(child, x, position, width, size);
        }
        else
        {
            ECSILayout_Place(child, position, y, size, height);
        }

        position += size + LAYOUT.dividerSize;
    }
}

/// @brief Calls a function for every group of a tree.
static void ECSILayout_ForEachGroupInTree(ECSINode *node, ECSILayoutGroupFunction function, void *userData)
{
    if (node == nullptr)
    {
        return;
    }

    if (node->type == ECSINodeType_Group)
    {
        function(node, userData);
        return;
    }

    for (usz i = 0; i < arrlenu(node->children); i++)
    {
        ECSILayout_ForEachGroupInTree(node->children[i], function, userData);
    }
}

/// @brief Which groups of a root show their panels, for ECSILayout_TellVisible.
typedef struct ECSIVisibleGroups
{
    bool current;   // the root belongs to the current workspace
    ECSINode *only; // its maximized group, which hides the others; nullptr when none is maximized
} ECSIVisibleGroups;

/// @brief Tells each panel of a group whether it is visible: the shown panel of a group of the current workspace that no maximized group hides. userData is an ECSIVisibleGroups.
static void ECSILayout_TellVisible(ECSINode *group, void *userData)
{
    const ECSIVisibleGroups *visibleGroups = userData;
    bool groupVisible = visibleGroups->current && (visibleGroups->only == nullptr || visibleGroups->only == group);

    for (usz i = 0; i < arrlenu(group->panels); i++)
    {
        ECSIPanel_SetVisible(group->panels[i], groupVisible && i == group->shown);
    }
}

/// @brief Finds the group that holds a panel.
static ECSINode *ECSILayout_FindGroupInTree(ECSINode *node, ECSPanel panel)
{
    if (node == nullptr)
    {
        return nullptr;
    }

    if (node->type == ECSINodeType_Group)
    {
        for (usz i = 0; i < arrlenu(node->panels); i++)
        {
            if (node->panels[i] == panel)
            {
                return node;
            }
        }

        return nullptr;
    }

    for (usz i = 0; i < arrlenu(node->children); i++)
    {
        ECSINode *group = ECSILayout_FindGroupInTree(node->children[i], panel);

        if (group != nullptr)
        {
            return group;
        }
    }

    return nullptr;
}

/// @brief Finds the group that holds a panel in a workspace, and its root.
/// @param retRoot Gets the root, or nullptr.
static ECSINode *ECSILayout_FindGroupInWorkspace(const ECSIWorkspace *workspace, ECSPanel panel, ECSIRoot **retRoot)
{
    for (usz i = 0; workspace != nullptr && panel != nullptr && i < arrlenu(workspace->roots); i++)
    {
        ECSINode *group = ECSILayout_FindGroupInTree(workspace->roots[i]->tree, panel);

        if (group != nullptr)
        {
            if (retRoot != nullptr)
            {
                *retRoot = workspace->roots[i];
            }

            return group;
        }
    }

    return nullptr;
}

/// @brief Finds the workspace, root and group that hold a panel, in any workspace.
/// @param retWorkspace Gets the workspace, or nullptr.
/// @param retRoot Gets the root, or nullptr.
/// @param retGroup Gets the group, or nullptr.
/// @return false if the panel is not in the layout.
static bool ECSILayout_Locate(ECSPanel panel, ECSIWorkspace **retWorkspace, ECSIRoot **retRoot, ECSINode **retGroup)
{
    for (usz i = 0; panel != nullptr && i < arrlenu(LAYOUT.workspaces); i++)
    {
        ECSINode *group = ECSILayout_FindGroupInWorkspace(&LAYOUT.workspaces[i], panel, retRoot);

        if (group != nullptr)
        {
            if (retWorkspace != nullptr)
            {
                *retWorkspace = &LAYOUT.workspaces[i];
            }

            if (retGroup != nullptr)
            {
                *retGroup = group;
            }

            return true;
        }
    }

    return false;
}

/// @brief Emits ecs.focusChanged with the panel that has the focus now, or none.
static void ECSILayout_EmitFocus(ECSPanel panel)
{
    ECSValue *value = nullptr;
    ECSValue *field = nullptr;

    if (ECSValue_Create(&value) == SHUResult_Ok)
    {
        ECSValue_SetTable(value);

        // with no focused panel, the table stays empty
        if (panel != nullptr && ECSValue_SetTableField(value, "panel", &field) == SHUResult_Ok)
        {
            ECSValue_SetInteger(field, ECSPanel_GetID(panel));
        }

        ECSIEvents_EmitCore("ecs.focusChanged", value);
    }

    ECSValue_Destroy(&value);
}

/// @brief Tells the panel that loses the focus and the one that gets it, either of which may be nullptr, and emits ecs.focusChanged.
static void ECSILayout_TellFocus(ECSPanel old, ECSPanel focus)
{
    ECSPanelEvent event = {.type = ECSPanelEventType_Unfocused};

    if (old != nullptr)
    {
        ECSIPanel_PostEvent(old, &event);
    }

    if (focus != nullptr)
    {
        event.type = ECSPanelEventType_Focused;
        ECSIPanel_PostEvent(focus, &event);
    }

    ECSILayout_EmitFocus(focus);
}

/// @brief Changes a workspace's focus. In the current workspace, the panel that loses focus and the one that gets it are told.
static void ECSILayout_ChangeFocus(ECSIWorkspace *workspace, ECSPanel panel)
{
    ECSPanel old = workspace->focus;

    if (old == panel)
    {
        return;
    }

    workspace->focus = panel;
    LAYOUT.frameNeeded = true;

    if (panel != nullptr)
    {
        panel->focusTicks = SDL_GetTicksNS();
    }

    if (workspace == ECSILayout_GetCurrent())
    {
        ECSILayout_TellFocus(old, panel);
    }
}

/// @brief Finds the first panel of a tree.
static ECSPanel ECSILayout_FirstPanel(ECSINode *node)
{
    if (node == nullptr)
    {
        return nullptr;
    }

    if (node->type == ECSINodeType_Group)
    {
        return arrlenu(node->panels) > 0 ? node->panels[node->shown] : nullptr;
    }

    for (usz i = 0; i < arrlenu(node->children); i++)
    {
        ECSPanel panel = ECSILayout_FirstPanel(node->children[i]);

        if (panel != nullptr)
        {
            return panel;
        }
    }

    return nullptr;
}

/// @brief Finds the first panel of a workspace: in the main window, else in the first pop-out window.
static ECSPanel ECSILayout_FirstPanelOf(const ECSIWorkspace *workspace)
{
    for (usz i = 0; i < arrlenu(workspace->roots); i++)
    {
        ECSPanel panel = ECSILayout_FirstPanel(workspace->roots[i]->tree);

        if (panel != nullptr)
        {
            return panel;
        }
    }

    return nullptr;
}

/// @brief Makes an empty root with a new id.
static ECSIRoot *ECSILayout_NewRoot(void)
{
    ECSIRoot *root = SDL_calloc(1, sizeof(ECSIRoot));

    if (root != nullptr)
    {
        root->id = ++LAYOUT.lastRootID;
    }

    return root;
}

/// @brief Destroys a root, its tree and its panels.
static void ECSILayout_FreeRoot(ECSIRoot *root)
{
    if (root->tree != nullptr)
    {
        ECSILayout_NodeDestroy(&root->tree);
    }

    SDL_free(root);
}

/// @brief Frees one node, without its children or panels.
static void ECSILayout_FreeNode(ECSINode *node)
{
    arrfree(node->children);
    arrfree(node->panels);
    SDL_free(node);
}

/// @brief Closes the panels of a tree and frees its nodes; the panels are destroyed after the current pass of the main loop.
static void ECSILayout_CloseTree(ECSINode *node)
{
    for (usz i = 0; i < arrlenu(node->children); i++)
    {
        ECSILayout_CloseTree(node->children[i]);
    }

    for (usz i = 0; i < arrlenu(node->panels); i++)
    {
        ECSIPanel_Close(&node->panels[i]);
    }

    ECSILayout_FreeNode(node);
}

/// @brief Destroys the panels of a tree at once and frees its nodes.
static void ECSILayout_DestroyTree(ECSINode *node)
{
    ECSILayout_NodeDestroy(&node);
}

/// @brief Function that closes or destroys the panels of a tree, and frees its nodes.
typedef void (*ECSILayoutTreeFunction)(ECSINode *tree);

/// @brief Frees every workspace, and the closed panels that ecs.layout.reopen remembers.
static void ECSILayout_FreeWorkspaces(ECSILayoutTreeFunction FreeTree)
{
    for (usz i = 0; i < arrlenu(LAYOUT.workspaces); i++)
    {
        for (usz j = 0; j < arrlenu(LAYOUT.workspaces[i].roots); j++)
        {
            ECSIRoot *root = LAYOUT.workspaces[i].roots[j];

            if (root->tree != nullptr)
            {
                FreeTree(root->tree);
            }

            SDL_free(root);
        }

        arrfree(LAYOUT.workspaces[i].roots);
        SDL_free(LAYOUT.workspaces[i].name);
    }

    arrfree(LAYOUT.workspaces);

    for (usz i = 0; i < arrlenu(LAYOUT.closed); i++)
    {
        ECSValue_Destroy(&LAYOUT.closed[i].saved);
    }

    arrfree(LAYOUT.closed);
}

/// @brief Tidies a subtree after an operation: frees empty groups and splits, merges a split into a parent split of the same direction, and replaces a split that has one child by that child.
/// @return The node that takes the subtree's place, or nullptr if nothing is left.
static ECSINode *ECSILayout_TidyNode(ECSIRoot *root, ECSINode *node)
{
    if (node->type == ECSINodeType_Group)
    {
        if (arrlenu(node->panels) > 0)
        {
            return node;
        }

        if (root->maximized == node)
        {
            root->maximized = nullptr;
        }

        ECSILayout_FreeNode(node);
        return nullptr;
    }

    ECSINode **children = nullptr;

    for (usz i = 0; i < arrlenu(node->children); i++)
    {
        ECSINode *child = ECSILayout_TidyNode(root, node->children[i]);

        if (child == nullptr)
        {
            continue;
        }

        // a child split of the same direction gives its children its place; a fixed-size one keeps them, so their shares keep their meaning
        if (child->type == ECSINodeType_Split && child->vertical == node->vertical && child->fixedSize <= 0.0f)
        {
            f32 shareSum = 0.0f;

            for (usz j = 0; j < arrlenu(child->children); j++)
            {
                shareSum += child->children[j]->fixedSize > 0.0f ? 0.0f : child->children[j]->share;
            }

            for (usz j = 0; j < arrlenu(child->children); j++)
            {
                ECSINode *grandchild = child->children[j];
                grandchild->share = grandchild->fixedSize > 0.0f || shareSum <= 0.0f ? grandchild->share : grandchild->share * child->share / shareSum;
                grandchild->parent = node;
                arrput(children, grandchild);
            }

            ECSILayout_FreeNode(child);
            continue;
        }

        child->parent = node;
        arrput(children, child);
    }

    arrfree(node->children);
    node->children = children;

    if (arrlenu(children) > 1)
    {
        return node;
    }

    ECSINode *only = arrlenu(children) == 1 ? children[0] : nullptr;

    if (only != nullptr)
    {
        only->fixedSize = node->fixedSize;
        only->share = node->share;
        only->parent = node->parent;
    }

    ECSILayout_FreeNode(node);
    return only;
}

/// @brief Lets the window forget the nodes it points to, because nodes may have been freed or hidden.
static void ECSILayout_Forget(void)
{
    if (LAYOUT.Forget != nullptr)
    {
        LAYOUT.Forget();
    }
}

/// @brief Follows a change of the layout: the window forgets the nodes it points to and draws again, and ecs.layoutChanged is emitted.
static void ECSILayout_Changed(void)
{
    ECSILayout_Forget();
    LAYOUT.frameNeeded = true;
    ECSIEvents_EmitCore("ecs.layoutChanged", nullptr);
}

/// @brief Tidies a root's tree.
static void ECSILayout_TidyRoot(ECSIRoot *root)
{
    if (root->tree != nullptr)
    {
        root->tree = ECSILayout_TidyNode(root, root->tree);
    }

    if (root->tree != nullptr)
    {
        root->tree->parent = nullptr;
    }
}

/// @brief Removes the pop-out roots of a workspace that have no panels left. A main root left without panels takes the panels of the first pop-out window, so the main window never shows nothing while another window has panels.
static void ECSILayout_RemoveEmptyRoots(ECSIWorkspace *workspace)
{
    for (usz i = 1; i < arrlenu(workspace->roots);)
    {
        ECSIRoot *root = workspace->roots[i];

        if (root->tree == nullptr)
        {
            ECSILayout_FreeRoot(root);
            arrdel(workspace->roots, i);
            continue;
        }

        i++;
    }

    ECSIRoot *main = workspace->roots[0];

    if (main->tree == nullptr && arrlenu(workspace->roots) > 1)
    {
        ECSIRoot *first = workspace->roots[1];
        main->tree = first->tree;
        main->maximized = first->maximized;
        first->tree = nullptr;
        ECSILayout_FreeRoot(first);
        arrdel(workspace->roots, 1);
    }
}

/// @brief Tidies a workspace's trees after an operation, and removes the pop-out windows that have no panels left. Nodes may be freed, so the window forgets the nodes it points to.
static void ECSILayout_Tidy(ECSIWorkspace *workspace)
{
    for (usz i = 0; i < arrlenu(workspace->roots); i++)
    {
        ECSILayout_TidyRoot(workspace->roots[i]);
    }

    ECSILayout_RemoveEmptyRoots(workspace);

    ECSILayout_Changed();
}

#pragma region Moving

/// @brief Checks whether a zone splits the target group toward one of its sides.
static bool ECSILayout_IsSide(ECSIZone zone)
{
    return zone >= ECSIZone_Left && zone <= ECSIZone_Bottom;
}

/// @brief Checks whether a zone docks along a whole edge of the OS window.
static bool ECSILayout_IsEdge(ECSIZone zone)
{
    return zone >= ECSIZone_WindowLeft && zone <= ECSIZone_WindowBottom;
}

/// @brief Puts a node in another node's place in a root's tree. The other node keeps its children and sizes.
static void ECSILayout_ReplaceNode(ECSIRoot *root, ECSINode *old, ECSINode *node)
{
    ECSINode *parent = old->parent;
    node->parent = parent;
    node->fixedSize = old->fixedSize;
    node->share = old->share;

    if (parent == nullptr)
    {
        root->tree = node;
        return;
    }

    for (usz i = 0; i < arrlenu(parent->children); i++)
    {
        if (parent->children[i] == old)
        {
            parent->children[i] = node;
        }
    }
}

/// @brief Wraps a node in a new split with a new node beside it. The split takes the node's place.
/// @param first true to put the new node first: left or top.
static void ECSILayout_Wrap(ECSIRoot *root, ECSINode *split, ECSINode *node, ECSINode *added, bool first, f32 nodeShare, f32 addedShare)
{
    ECSILayout_ReplaceNode(root, node, split);
    node->fixedSize = 0.0f;
    node->share = nodeShare;
    added->fixedSize = 0.0f;
    added->share = addedShare;
    node->parent = split;
    added->parent = split;
    arrput(split->children, first ? added : node);
    arrput(split->children, first ? node : added);
}

/// @brief Takes a node out of its root's tree: out of its parent split, or out of the root if it is the whole tree. Tidying removes what is left empty.
static void ECSILayout_Unlink(ECSIRoot *root, ECSINode *node)
{
    ECSINode *parent = node->parent;

    if (parent == nullptr)
    {
        root->tree = root->tree == node ? nullptr : root->tree;
        return;
    }

    for (usz i = 0; i < arrlenu(parent->children); i++)
    {
        if (parent->children[i] == node)
        {
            arrdel(parent->children, i);
            break;
        }
    }

    node->parent = nullptr;
}

/// @brief Places a node that is out of every tree: beside a group, along an edge of a root, as the whole tree of an empty root, or as the tree of a new pop-out root.
/// @param split A new split for the side and edge zones, which it then owns; nullptr for the others.
/// @param popOutRoot A new root for a pop-out, which the workspace then owns; nullptr for the others.
static void ECSILayout_Attach(ECSIWorkspace *workspace, ECSINode *node, ECSINode *split, ECSIRoot *popOutRoot, const ECSIDrop *drop)
{
    bool first = drop->zone == ECSIZone_Left || drop->zone == ECSIZone_Top || drop->zone == ECSIZone_WindowLeft || drop->zone == ECSIZone_WindowTop;

    if (popOutRoot != nullptr)
    {
        popOutRoot->tree = node;
        node->parent = nullptr;
        arrput(workspace->roots, popOutRoot);
    }
    else if (drop->root->tree == nullptr)
    {
        // an empty main window takes the node as its whole tree
        drop->root->tree = node;
        node->parent = nullptr;
        SDL_free(split);
    }
    else if (ECSILayout_IsSide(drop->zone))
    {
        // the target and the node share the target's place
        ECSILayout_Wrap(drop->root, split, drop->group, node, first, 1.0f, 1.0f);
    }
    else
    {
        // the node docks along the whole edge, with a quarter of the window
        ECSILayout_Wrap(drop->root, split, drop->root->tree, node, first, 3.0f, 1.0f);
    }
}

/// @brief Makes the nodes a move to a new place needs before it changes anything, so a failed allocation changes nothing: a split for a side or edge zone, and a root for a pop-out.
/// @param retSplit Gets the split, or nullptr for the other zones.
/// @param retRoot Gets the root, or nullptr for the other zones.
/// @return false if memory ran out; nothing is left made then.
static bool ECSILayout_AllocateForMove(const ECSIDrop *drop, ECSINode **retSplit, ECSIRoot **retRoot)
{
    bool vertical = drop->zone == ECSIZone_Top || drop->zone == ECSIZone_Bottom || drop->zone == ECSIZone_WindowTop || drop->zone == ECSIZone_WindowBottom;
    bool failed = ((ECSILayout_IsSide(drop->zone) || ECSILayout_IsEdge(drop->zone)) && ECSILayout_SplitCreate(retSplit, vertical)) ||
                  (drop->zone == ECSIZone_PopOut && (*retRoot = ECSILayout_NewRoot()) == nullptr);

    if (failed)
    {
        SDL_free(*retSplit);
        *retSplit = nullptr;
    }

    return !failed;
}

/// @brief Puts a panel into a group at a position, before the panel there.
static void ECSILayout_InsertPanel(ECSINode *group, usz index, ECSPanel panel)
{
    // arrins of stb_ds does not compile cleanly with sign warnings, so the gap is opened by hand
    arrput(group->panels, panel);
    SDL_memmove(&group->panels[index + 1], &group->panels[index], (arrlenu(group->panels) - 1 - index) * sizeof(ECSPanel));
    group->panels[index] = panel;
}

/// @brief Takes a panel out of its group. The group shows the panel after it, or the last one; tidying removes a group that empties.
/// @return The panel's position in the group before.
static usz ECSILayout_TakePanel(ECSINode *group, ECSPanel panel)
{
    usz index = ECSILayout_IndexOf(group, panel);
    arrdel(group->panels, index);
    group->shown = arrlenu(group->panels) == 0 ? 0 : SDL_min(group->shown, arrlenu(group->panels) - 1);
    return index;
}

/// @brief Checks whether locks let the user move a group's panels to a drop place: a locked group's panels stay, and a locked group takes no panels. Locks stop the user, not code.
static bool ECSILayout_LocksAllow(const ECSINode *source, const ECSIDrop *drop)
{
    bool join = drop->zone == ECSIZone_Center || drop->zone == ECSIZone_Tabs;
    return !source->locked && !(join && drop->group != nullptr && drop->group->locked);
}

/// @brief Gives a new pop-out root its size: the drop's, or else the moved panel's.
static void ECSILayout_SizePopOut(ECSIRoot *popOutRoot, const ECSIDrop *drop, ECSPanel panel)
{
    popOutRoot->width = drop->width > 0.0f ? drop->width : SDL_max(panel->width, OPENECS_MIN_POP_OUT_SIZE);
    popOutRoot->height = drop->height > 0.0f ? drop->height : SDL_max(panel->height, OPENECS_MIN_POP_OUT_SIZE);
}

/// @brief Moves a panel to a drop place, then tidies the trees. The panel gets the focus. Locks do not stop it; ECSILayout_LocksAllow checks them for the user.
static void ECSILayout_Move(ECSIWorkspace *workspace, ECSPanel panel, const ECSIDrop *drop)
{
    ECSIRoot *sourceRoot = nullptr;
    ECSINode *source = ECSILayout_FindGroupInWorkspace(workspace, panel, &sourceRoot);
    ECSINode *target = drop->group;
    bool side = ECSILayout_IsSide(drop->zone);
    bool join = drop->zone == ECSIZone_Center || drop->zone == ECSIZone_Tabs;

    // a panel cannot join its own group again, split away from a group that holds only itself, or pop out of a window that holds only itself
    bool alone = arrlenu(source == nullptr ? nullptr : source->panels) == 1;

    if (source == nullptr || drop->zone == ECSIZone_None ||
        (source == target && ((join && drop->zone == ECSIZone_Center) || (side && alone))) ||
        (drop->zone == ECSIZone_PopOut && alone && sourceRoot->tree == source))
    {
        return;
    }

    // without a group, a panel joins only an empty window
    if (join && target == nullptr && (drop->root == nullptr || drop->root->tree != nullptr))
    {
        return;
    }

    ECSINode *group = nullptr;
    ECSINode *split = nullptr;
    ECSIRoot *popOutRoot = nullptr;

    if (!(join && target != nullptr) && (ECSILayout_GroupCreate(&group) || !ECSILayout_AllocateForMove(drop, &split, &popOutRoot)))
    {
        SDL_free(group);
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Out of memory while moving a panel.");
        return;
    }

    usz index = ECSILayout_TakePanel(source, panel);

    if (join && target != nullptr)
    {
        // the gap was counted with the panel still in its own group
        usz gap = drop->zone == ECSIZone_Center ? arrlenu(target->panels) : drop->index;
        gap = source == target && index < gap ? gap - 1 : gap;
        gap = SDL_min(gap, arrlenu(target->panels));
        ECSILayout_InsertPanel(target, gap, panel);
        target->shown = gap;
    }
    else
    {
        if (popOutRoot != nullptr)
        {
            ECSILayout_SizePopOut(popOutRoot, drop, panel);
        }

        arrput(group->panels, panel);
        ECSILayout_Attach(workspace, group, split, popOutRoot, drop);
    }

    sourceRoot->maximized = nullptr;

    if (drop->root != nullptr)
    {
        drop->root->maximized = nullptr;
    }

    ECSILayout_ChangeFocus(workspace, panel);
    ECSILayout_Tidy(workspace);
}

/// @brief Moves a whole group to a drop place, then tidies the trees. The group's shown panel gets the focus. Locks do not stop it.
static void ECSILayout_MoveGroup(ECSIWorkspace *workspace, ECSINode *group, const ECSIDrop *drop)
{
    ECSIRoot *sourceRoot = nullptr;
    ECSILayout_FindGroupInWorkspace(workspace, group->panels[group->shown], &sourceRoot);
    ECSINode *target = drop->group;
    bool join = drop->zone == ECSIZone_Center || drop->zone == ECSIZone_Tabs;

    // a group that fills its OS window cannot dock along its edges or pop out of it
    bool whole = sourceRoot->tree == group;

    if (drop->zone == ECSIZone_None || target == group || (whole && (drop->zone == ECSIZone_PopOut || (ECSILayout_IsEdge(drop->zone) && drop->root == sourceRoot))) ||
        (join && target == nullptr && (drop->root == nullptr || drop->root->tree != nullptr)))
    {
        return;
    }

    ECSPanel shown = group->panels[group->shown];

    if (join && target != nullptr)
    {
        // the panels keep their order; the emptied group is removed by tidying
        usz gap = SDL_min(drop->zone == ECSIZone_Center ? arrlenu(target->panels) : drop->index, arrlenu(target->panels));

        for (usz i = 0; i < arrlenu(group->panels); i++, gap++)
        {
            ECSILayout_InsertPanel(target, gap, group->panels[i]);
        }

        target->shown = gap - arrlenu(group->panels) + group->shown;
        arrfree(group->panels);
        group->shown = 0;
    }
    else
    {
        ECSINode *split = nullptr;
        ECSIRoot *popOutRoot = nullptr;

        if (!ECSILayout_AllocateForMove(drop, &split, &popOutRoot))
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Out of memory while moving a panel.");
            return;
        }

        if (popOutRoot != nullptr)
        {
            ECSILayout_SizePopOut(popOutRoot, drop, shown);
        }

        // the group leaves its place, which tidying removes if one child is left
        ECSILayout_Unlink(sourceRoot, group);
        ECSILayout_Attach(workspace, group, split, popOutRoot, drop);
    }

    sourceRoot->maximized = nullptr;

    if (drop->root != nullptr)
    {
        drop->root->maximized = nullptr;
    }

    ECSILayout_ChangeFocus(workspace, shown);
    ECSILayout_Tidy(workspace);
}

/// @brief Puts a panel into a group, or into a new group if the workspace has no panels, then a side zone moves it beside the group, and a pop-out into a new OS window. The panel gets the focus.
/// @param group The group, or nullptr when the workspace has no panels.
static SHUResult ECSILayout_Add(ECSIWorkspace *workspace, ECSINode *group, ECSPanel panel, ECSIZone zone)
{
    if (group == nullptr)
    {
        SHU_ReturnResult(ECSILayout_GroupCreate(&group));
        workspace->roots[0]->tree = group;
    }

    arrput(group->panels, panel);
    group->shown = arrlenu(group->panels) - 1;
    ECSIRoot *root = nullptr;
    ECSILayout_FindGroupInWorkspace(workspace, panel, &root);
    ECSIDrop drop = {.zone = zone, .root = zone == ECSIZone_PopOut ? nullptr : root, .group = group};

    if (zone != ECSIZone_Center && zone != ECSIZone_None)
    {
        ECSILayout_Move(workspace, panel, &drop);
    }

    ECSILayout_ChangeFocus(workspace, panel);
    ECSILayout_Tidy(workspace);
    return SHUResult_Ok;
}

/// @brief Takes a panel out of the layout and tidies the workspace. If the panel had the focus, the panel its group shows now gets it, or else the workspace's first panel.
static void ECSILayout_RemovePanel(ECSIWorkspace *workspace, ECSINode *group, ECSPanel panel)
{
    ECSILayout_TakePanel(group, panel);
    bool focused = workspace->focus == panel;
    bool emptied = arrlenu(group->panels) == 0;

    // tidying frees a group that empties, so its panel is chosen before
    if (focused)
    {
        ECSILayout_ChangeFocus(workspace, emptied ? nullptr : group->panels[group->shown]);
    }

    ECSILayout_Tidy(workspace);

    if (focused && emptied)
    {
        ECSILayout_ChangeFocus(workspace, ECSILayout_FirstPanelOf(workspace));
    }
}

/// @brief Saves a panel that closes, so ecs.layout.reopen can open it again next to a panel that stays in its group. The oldest saved panels go beyond ecs.reopenLimit.
static void ECSILayout_RememberClosed(const ECSINode *group, ECSPanel panel)
{
    usz index = ECSILayout_IndexOf(group, panel);
    ECSIClosedPanel closed = {.neighbour = arrlenu(group->panels) > 1 ? ECSPanel_GetID(group->panels[index == 0 ? 1 : 0]) : 0};

    // the oldest panels make room, and a limit made smaller drops more of them
    while (arrlenu(LAYOUT.closed) > 0 && (i64)arrlenu(LAYOUT.closed) >= LAYOUT.reopenLimit)
    {
        ECSValue_Destroy(&LAYOUT.closed[0].saved);
        arrdel(LAYOUT.closed, 0);
    }

    if (LAYOUT.reopenLimit <= 0 || ECSValue_Create(&closed.saved) || ECSIPanel_Save(panel, closed.saved))
    {
        ECSValue_Destroy(&closed.saved);
        return;
    }

    arrput(LAYOUT.closed, closed);
}

/// @brief Finds the group a new panel joins in a workspace: the focused group, else the first one, or nullptr if the workspace has no panels.
static ECSINode *ECSILayout_DefaultGroup(ECSIWorkspace *workspace)
{
    ECSINode *group = ECSILayout_FindGroupInWorkspace(workspace, workspace->focus, nullptr);
    return group != nullptr ? group : ECSILayout_FindGroupInWorkspace(workspace, ECSILayout_FirstPanelOf(workspace), nullptr);
}

/// @brief Converts a public zone to a drop zone.
static ECSIZone ECSILayout_ConvertZone(ECSZone zone)
{
    switch (zone)
    {
    case ECSZone_Left:
        return ECSIZone_Left;
    case ECSZone_Right:
        return ECSIZone_Right;
    case ECSZone_Top:
        return ECSIZone_Top;
    case ECSZone_Bottom:
        return ECSIZone_Bottom;
    case ECSZone_Window:
        return ECSIZone_PopOut;
    default:
        return ECSIZone_Center;
    }
}

typedef struct ECSIRecentSearch
{
    const char *typeName;
    ECSPanel best;
} ECSIRecentSearch;

/// @brief Finds the most recently focused panel of a type in a tree.
static void ECSILayout_FindRecent(const ECSINode *node, ECSIRecentSearch *search)
{
    if (node == nullptr)
    {
        return;
    }

    for (usz i = 0; i < arrlenu(node->panels); i++)
    {
        ECSPanel panel = node->panels[i];

        if (panel->focusTicks > 0 && SDL_strcmp(panel->typeName, search->typeName) == 0 && (search->best == nullptr || panel->focusTicks > search->best->focusTicks))
        {
            search->best = panel;
        }
    }

    for (usz i = 0; i < arrlenu(node->children); i++)
    {
        ECSILayout_FindRecent(node->children[i], search);
    }
}

#pragma endregion Moving

#pragma region Hit Testing

typedef struct ECSIGroupHit
{
    f32 x;
    f32 y;
    ECSINode *group;
} ECSIGroupHit;

static void ECSILayout_HitGroup(ECSINode *group, void *userData)
{
    ECSIGroupHit *hit = userData;

    if (ECSILayout_Contains((SDL_FRect){group->x, group->y, group->width, group->height}, hit->x, hit->y))
    {
        hit->group = group;
    }
}

typedef struct ECSIDividerHit
{
    f32 x;
    f32 y;
    ECSINode *split;
    usz divider;
} ECSIDividerHit;

/// @brief Finds the divider under a point.
static void ECSILayout_FindDivider(ECSINode *node, ECSIDividerHit *hit)
{
    if (node == nullptr || node->type == ECSINodeType_Group || hit->split != nullptr)
    {
        return;
    }

    for (usz i = 0; i + 1 < arrlenu(node->children); i++)
    {
        ECSINode *child = node->children[i];
        bool over = node->vertical
                        ? ECSILayout_Contains((SDL_FRect){node->x, child->y + child->height, node->width, LAYOUT.dividerSize}, hit->x, hit->y)
                        : ECSILayout_Contains((SDL_FRect){child->x + child->width, node->y, LAYOUT.dividerSize, node->height}, hit->x, hit->y);

        if (over)
        {
            hit->split = node;
            hit->divider = i;
            return;
        }
    }

    for (usz i = 0; i < arrlenu(node->children); i++)
    {
        ECSILayout_FindDivider(node->children[i], hit);
    }
}

typedef struct ECSIPanelHit
{
    f32 x;
    f32 y;
    ECSPanel panel;
} ECSIPanelHit;

static void ECSILayout_HitPanel(ECSINode *group, void *userData)
{
    ECSIPanelHit *hit = userData;

    if (arrlenu(group->panels) == 0)
    {
        return;
    }

    ECSPanel panel = group->panels[group->shown];

    if (ECSILayout_Contains((SDL_FRect){panel->x, panel->y, panel->width, panel->height}, hit->x, hit->y))
    {
        hit->panel = panel;
    }
}

typedef struct ECSINeighbourSearch
{
    ECSPanel from;
    i32 directionX;
    i32 directionY;
    ECSPanel best;
    f32 bestGap;
    f32 bestOverlap;
} ECSINeighbourSearch;

static void ECSILayout_ConsiderNeighbour(ECSINode *group, void *userData)
{
    ECSINeighbourSearch *search = userData;

    if (arrlenu(group->panels) == 0)
    {
        return;
    }

    ECSPanel from = search->from;
    ECSPanel panel = group->panels[group->shown];

    if (panel == from)
    {
        return;
    }

    f32 gap = 0.0f;
    f32 overlap = 0.0f;

    if (search->directionX != 0)
    {
        gap = search->directionX > 0 ? panel->x - (from->x + from->width) : from->x - (panel->x + panel->width);
        overlap = SDL_min(from->y + from->height, panel->y + panel->height) - SDL_max(from->y, panel->y);
    }
    else
    {
        gap = search->directionY > 0 ? panel->y - (from->y + from->height) : from->y - (panel->y + panel->height);
        overlap = SDL_min(from->x + from->width, panel->x + panel->width) - SDL_max(from->x, panel->x);
    }

    if (gap < -1.0f || overlap <= 0.0f)
    {
        return;
    }

    if (search->best == nullptr || gap < search->bestGap || (gap == search->bestGap && overlap > search->bestOverlap))
    {
        search->best = panel;
        search->bestGap = gap;
        search->bestOverlap = overlap;
    }
}

static void ECSILayout_CheckFrameWanted(ECSINode *group, void *userData)
{
    bool *wants = userData;

    if (arrlenu(group->panels) > 0 && ECSIPanel_WantsFrame(group->panels[group->shown]))
    {
        *wants = true;
    }
}

#pragma endregion Hit Testing

#pragma region Saving

/// @brief Describes a node and its children for a session. A split holds its children as list items; a group holds its panels.
static SHUResult ECSILayout_SaveNode(const ECSIRoot *root, const ECSINode *node, ECSValue *retNode)
{
    ECSValue *field = nullptr;
    ECSValue_SetTable(retNode);

    if (node->parent != nullptr)
    {
        bool fixed = node->fixedSize > 0.0f;
        SHU_ReturnResult(ECSValue_SetTableField(retNode, fixed ? "size" : "share", &field));
        ECSValue_SetNumber(field, fixed ? node->fixedSize : node->share);
    }

    if (node->type == ECSINodeType_Split)
    {
        SHU_ReturnResult(ECSValue_SetTableField(retNode, "split", &field));
        SHU_ReturnResult(ECSValue_SetString(field, node->vertical ? "vertical" : "horizontal"));

        for (usz i = 0; i < arrlenu(node->children); i++)
        {
            SHU_ReturnResult(ECSValue_AddListItem(retNode, &field));
            SHU_ReturnResult(ECSILayout_SaveNode(root, node->children[i], field));
        }

        return SHUResult_Ok;
    }

    ECSValue *panels = nullptr;
    SHU_ReturnResult(ECSValue_SetTableField(retNode, "panels", &panels));
    ECSValue_SetTable(panels);

    for (usz i = 0; i < arrlenu(node->panels); i++)
    {
        SHU_ReturnResult(ECSValue_AddListItem(panels, &field));
        SHU_ReturnResult(ECSIPanel_Save(node->panels[i], field));
    }

    SHU_ReturnResult(ECSValue_SetTableField(retNode, "shown", &field));
    ECSValue_SetInteger(field, (i64)node->shown + 1);

    if (node->locked)
    {
        SHU_ReturnResult(ECSValue_SetTableField(retNode, "locked", &field));
        ECSValue_SetBool(field, true);
    }

    if (root->maximized == node)
    {
        SHU_ReturnResult(ECSValue_SetTableField(retNode, "maximized", &field));
        ECSValue_SetBool(field, true);
    }

    return SHUResult_Ok;
}

/// @brief Adds every panel of a tree to an stb_ds array.
static void ECSILayout_CollectPanels(const ECSINode *node, ECSPanel **panels)
{
    if (node == nullptr)
    {
        return;
    }

    for (usz i = 0; i < arrlenu(node->panels); i++)
    {
        arrput(*panels, node->panels[i]);
    }

    for (usz i = 0; i < arrlenu(node->children); i++)
    {
        ECSILayout_CollectPanels(node->children[i], panels);
    }
}

#pragma endregion Saving

/// @brief Reads the layout's settings; a negative value counts as 0. Also their Changed function.
static void ECSILayout_ReadSettings(void *data)
{
    (void)data;
    LAYOUT.tabRowHeight = (f32)SDL_max(0.0, ECSValue_GetNumber(ECSSetting_Get("ecs.tabRowHeight"), 0.0));
    LAYOUT.dividerSize = (f32)SDL_max(0.0, ECSValue_GetNumber(ECSSetting_Get("ecs.dividerSize"), 0.0));
    LAYOUT.reopenLimit = SDL_max(0, ECSValue_GetInteger(ECSSetting_Get("ecs.reopenLimit"), 0));
    LAYOUT.frameNeeded = true;
}

#pragma endregion Source Only

SHUResult ECSILayout_Initialize(void)
{
    const ECSSettingDesc settings[] = {
        {.name = "ecs.tabRowHeight", .type = ECSSettingType_Number, .description = "Height of a tab row, in layout units", .Changed = ECSILayout_ReadSettings},
        {.name = "ecs.dividerSize", .type = ECSSettingType_Number, .description = "Gap between the children of a split, which dragging resizes them, in layout units", .Changed = ECSILayout_ReadSettings},
        {.name = "ecs.reopenLimit", .type = ECSSettingType_Integer, .description = "How many closed panels ecs.layout.reopen remembers", .Changed = ECSILayout_ReadSettings},
    };

    for (usz i = 0; i < SDL_arraysize(settings); i++)
    {
        SHU_ReturnResult(ECSISettings_DeclareCore(&settings[i]));
    }

    ECSILayout_ReadSettings(nullptr);
    return SHUResult_Ok;
}

bool ECSILayout_Contains(SDL_FRect rect, f32 x, f32 y)
{
    return x >= rect.x && y >= rect.y && x < rect.x + rect.w && y < rect.y + rect.h;
}

f32 ECSILayout_GetTabRowHeight(void)
{
    return LAYOUT.tabRowHeight;
}

void ECSILayout_SetForget(ECSILayoutForgetFunction function)
{
    LAYOUT.Forget = function;
}

void ECSILayout_Terminate(void)
{
    ECSILayout_FreeWorkspaces(ECSILayout_DestroyTree);
    SDL_zero(LAYOUT);
}

SHUResult ECSILayout_SplitCreate(ECSINode **retNode, bool vertical)
{
    SDL_assert(retNode != nullptr);

    *retNode = SDL_calloc(1, sizeof(ECSINode));

    if (*retNode == nullptr)
    {
        return SHUResult_ErrAllocation;
    }

    (*retNode)->type = ECSINodeType_Split;
    (*retNode)->vertical = vertical;
    (*retNode)->share = 1.0f;
    return SHUResult_Ok;
}

SHUResult ECSILayout_GroupCreate(ECSINode **retNode)
{
    SDL_assert(retNode != nullptr);

    *retNode = SDL_calloc(1, sizeof(ECSINode));

    if (*retNode == nullptr)
    {
        return SHUResult_ErrAllocation;
    }

    (*retNode)->type = ECSINodeType_Group;
    (*retNode)->share = 1.0f;
    return SHUResult_Ok;
}

void ECSILayout_SplitAdd(ECSINode *split, ECSINode *child, f32 fixedSize, f32 share)
{
    SDL_assert(split != nullptr);
    SDL_assert(child != nullptr);
    SDL_assert(split->type == ECSINodeType_Split);

    child->parent = split;
    child->fixedSize = SDL_max(0.0f, fixedSize);
    child->share = share > 0.0f ? share : 1.0f;
    arrput(split->children, child);
}

void ECSILayout_GroupAdd(ECSINode *group, ECSPanel panel)
{
    SDL_assert(group != nullptr);
    SDL_assert(panel != nullptr);
    SDL_assert(group->type == ECSINodeType_Group);

    arrput(group->panels, panel);
    ECSIPanel_Emit("ecs.panelOpened", panel);
}

void ECSILayout_GroupShow(ECSINode *group, usz index)
{
    SDL_assert(group != nullptr);
    SDL_assert(group->type == ECSINodeType_Group);

    if (index < arrlenu(group->panels))
    {
        group->shown = index;
        LAYOUT.frameNeeded = true;
    }
}

void ECSILayout_GroupSetLocked(ECSINode *group, bool locked)
{
    SDL_assert(group != nullptr);
    SDL_assert(group->type == ECSINodeType_Group);

    group->locked = locked;
}

void ECSILayout_NodeDestroy(ECSINode **node)
{
    SDL_assert(node != nullptr);
    SDL_assert(*node != nullptr);

    ECSINode *target = *node;

    for (usz i = 0; i < arrlenu(target->children); i++)
    {
        ECSILayout_NodeDestroy(&target->children[i]);
    }

    for (usz i = 0; i < arrlenu(target->panels); i++)
    {
        ECSIPanel_Destroy(&target->panels[i]);
    }

    arrfree(target->children);
    arrfree(target->panels);
    SDL_free(target);
    *node = nullptr;
}

SHUResult ECSILayout_WorkspaceAdd(const char *name, const ECSIRootDesc *roots, usz count, ECSPanel focus)
{
    SDL_assert(name != nullptr);
    SDL_assert(roots != nullptr);
    SDL_assert(count >= 1);

    ECSIWorkspace workspace = {.name = SDL_strdup(name)};
    bool failed = workspace.name == nullptr;

    // the workspace takes every tree, so each tree is freed once also when it fails
    for (usz i = 0; i < count; i++)
    {
        ECSIRoot *root = failed ? nullptr : ECSILayout_NewRoot();

        if (root == nullptr)
        {
            failed = true;

            if (roots[i].tree != nullptr)
            {
                ECSINode *tree = roots[i].tree;
                ECSILayout_NodeDestroy(&tree);
            }

            continue;
        }

        *root = (ECSIRoot){.id = root->id, .tree = roots[i].tree, .maximized = roots[i].maximized, .width = roots[i].width, .height = roots[i].height};
        ECSILayout_TidyRoot(root);
        arrput(workspace.roots, root);
    }

    // a pop-out window without panels is left out
    if (!failed)
    {
        ECSILayout_RemoveEmptyRoots(&workspace);
    }

    if (failed)
    {
        for (usz i = 0; i < arrlenu(workspace.roots); i++)
        {
            ECSILayout_FreeRoot(workspace.roots[i]);
        }

        arrfree(workspace.roots);
        SDL_free(workspace.name);
        return SHUResult_ErrAllocation;
    }

    workspace.focus = focus != nullptr ? focus : ECSILayout_FirstPanelOf(&workspace);
    arrput(LAYOUT.workspaces, workspace);
    LAYOUT.frameNeeded = true;
    return SHUResult_Ok;
}

void ECSILayout_WorkspaceSwitch(usz index)
{
    if (index >= arrlenu(LAYOUT.workspaces) || index == LAYOUT.current)
    {
        return;
    }

    // the focus moves to the other workspace's focused panel
    ECSILayout_TellFocus(LAYOUT.workspaces[LAYOUT.current].focus, LAYOUT.workspaces[index].focus);
    LAYOUT.current = index;

    ECSValue *value = nullptr;
    ECSValue *field = nullptr;

    if (ECSValue_Create(&value) == SHUResult_Ok && ECSValue_SetTableField(value, "workspace", &field) == SHUResult_Ok)
    {
        ECSValue_SetInteger(field, (i64)index + 1);
        ECSIEvents_EmitCore("ecs.workspaceSwitched", value);
    }

    ECSValue_Destroy(&value);
    ECSILayout_Forget();
    LAYOUT.frameNeeded = true;
}

void ECSILayout_Clear(void)
{
    // the panels close at once, so they hear of no change of focus
    ECSILayout_FreeWorkspaces(ECSILayout_CloseTree);
    LAYOUT.current = 0;
    ECSILayout_Forget();
    LAYOUT.frameNeeded = true;
}

void ECSILayout_ShowFirst(usz index)
{
    if (arrlenu(LAYOUT.workspaces) == 0)
    {
        return;
    }

    LAYOUT.current = index < arrlenu(LAYOUT.workspaces) ? index : 0;
    ECSILayout_TellFocus(nullptr, LAYOUT.workspaces[LAYOUT.current].focus);
    ECSILayout_Changed();
}

usz ECSILayout_GetCurrentIndex(void)
{
    return LAYOUT.current;
}

void ECSILayout_RequestFrame(void)
{
    LAYOUT.frameNeeded = true;
}

bool ECSILayout_WantsFrame(void)
{
    bool wants = LAYOUT.frameNeeded;
    usz count = 0;
    ECSIRoot *const *roots = ECSILayout_GetRoots(&count);

    for (usz i = 0; i < count; i++)
    {
        ECSILayout_ForEachGroup(roots[i], ECSILayout_CheckFrameWanted, &wants);
    }

    return wants;
}

void ECSILayout_Update(void)
{
    // requests made while the frame is drawn, such as a tab row that scrolls, get the next frame
    LAYOUT.frameNeeded = false;
    ECSIWorkspace *workspace = ECSILayout_GetCurrent();

    if (workspace == nullptr)
    {
        return;
    }

    for (usz i = 0; i < arrlenu(workspace->roots); i++)
    {
        ECSIRoot *root = workspace->roots[i];
        ECSINode *top = root->maximized != nullptr ? root->maximized : root->tree;

        if (top != nullptr)
        {
            ECSILayout_Place(top, 0.0f, root->top, root->width, root->height);
        }
    }

    // panels of other workspaces are hidden; in this one, a maximized group hides the others of its OS window
    for (usz i = 0; i < arrlenu(LAYOUT.workspaces); i++)
    {
        for (usz j = 0; j < arrlenu(LAYOUT.workspaces[i].roots); j++)
        {
            ECSIRoot *root = LAYOUT.workspaces[i].roots[j];
            ECSIVisibleGroups visibleGroups = {.current = &LAYOUT.workspaces[i] == workspace, .only = root->maximized};
            ECSILayout_ForEachGroupInTree(root->tree, ECSILayout_TellVisible, &visibleGroups);
        }
    }
}

void ECSILayout_ForEachGroup(const ECSIRoot *root, ECSILayoutGroupFunction function, void *userData)
{
    if (root == nullptr)
    {
        return;
    }

    if (root->maximized != nullptr)
    {
        function(root->maximized, userData);
    }
    else
    {
        ECSILayout_ForEachGroupInTree(root->tree, function, userData);
    }
}

usz ECSILayout_IndexOf(const ECSINode *group, ECSPanel panel)
{
    SDL_assert(group != nullptr);

    usz index = 0;

    while (group->panels[index] != panel)
    {
        index++;
    }

    return index;
}

ECSINode *ECSILayout_GroupOf(ECSPanel panel)
{
    return ECSILayout_FindGroupInWorkspace(ECSILayout_GetCurrent(), panel, nullptr);
}

ECSIRoot *const *ECSILayout_GetRoots(usz *retCount)
{
    SDL_assert(retCount != nullptr);

    ECSIWorkspace *workspace = ECSILayout_GetCurrent();
    *retCount = workspace == nullptr ? 0 : arrlenu(workspace->roots);
    return workspace == nullptr ? nullptr : workspace->roots;
}

ECSIRoot *ECSILayout_FindRoot(u32 id, bool *retShown)
{
    for (usz i = 0; i < arrlenu(LAYOUT.workspaces); i++)
    {
        for (usz j = 0; j < arrlenu(LAYOUT.workspaces[i].roots); j++)
        {
            if (LAYOUT.workspaces[i].roots[j]->id == id)
            {
                if (retShown != nullptr)
                {
                    *retShown = i == LAYOUT.current;
                }

                return LAYOUT.workspaces[i].roots[j];
            }
        }
    }

    return nullptr;
}

ECSIRoot *ECSILayout_RootOf(ECSPanel panel)
{
    ECSIRoot *root = nullptr;
    return ECSILayout_Locate(panel, nullptr, &root, nullptr) ? root : nullptr;
}

void ECSILayout_SetRootArea(ECSIRoot *root, f32 top, f32 width, f32 height)
{
    SDL_assert(root != nullptr);

    root->top = top;
    root->width = width;
    root->height = height;
}

ECSPanel *ECSILayout_GetRootPanels(const ECSIRoot *root)
{
    SDL_assert(root != nullptr);

    ECSPanel *panels = nullptr;
    ECSILayout_CollectPanels(root->tree, &panels);
    return panels;
}

void ECSILayout_FocusRoot(ECSIRoot *root)
{
    SDL_assert(root != nullptr);

    ECSIWorkspace *workspace = ECSILayout_GetCurrent();

    if (workspace == nullptr || ECSILayout_FindGroupInTree(root->tree, workspace->focus) != nullptr)
    {
        return;
    }

    // the panel focused last in the window, or else its first panel
    ECSPanel *panels = ECSILayout_GetRootPanels(root);
    ECSPanel best = ECSILayout_FirstPanel(root->tree);

    for (usz i = 0; i < arrlenu(panels); i++)
    {
        best = best == nullptr || panels[i]->focusTicks > best->focusTicks ? panels[i] : best;
    }

    arrfree(panels);

    if (best != nullptr)
    {
        ECSLayout_Focus(best);
    }
}

bool ECSILayout_CanPopOut(ECSPanel panel)
{
    ECSIRoot *root = nullptr;
    ECSINode *group = ECSILayout_FindGroupInWorkspace(ECSILayout_GetCurrent(), panel, &root);
    return group != nullptr && !group->locked && !(root->tree == group && arrlenu(group->panels) == 1);
}

void ECSILayout_PopOut(void)
{
    ECSIWorkspace *workspace = ECSILayout_GetCurrent();
    ECSPanel panel = workspace == nullptr ? nullptr : workspace->focus;

    if (panel != nullptr && ECSILayout_IsLocked(panel))
    {
        SDL_Log("'%s' is locked; unlock it to popOutRoot it out.", panel->title);
        return;
    }

    if (panel != nullptr)
    {
        ECSIDrop drop = {.zone = ECSIZone_PopOut};
        ECSILayout_Move(workspace, panel, &drop);
    }
}

ECSINode *ECSILayout_GroupAt(const ECSIRoot *root, f32 x, f32 y)
{
    ECSIGroupHit hit = {x, y, nullptr};
    ECSILayout_ForEachGroup(root, ECSILayout_HitGroup, &hit);
    return hit.group;
}

bool ECSILayout_DividerAt(const ECSIRoot *root, f32 x, f32 y, ECSINode **retSplit, usz *retDivider)
{
    SDL_assert(retSplit != nullptr);
    SDL_assert(retDivider != nullptr);

    ECSIDividerHit hit = {x, y, nullptr, 0};
    ECSILayout_FindDivider(root == nullptr || root->maximized != nullptr ? nullptr : root->tree, &hit);
    *retSplit = hit.split;
    *retDivider = hit.divider;
    return hit.split != nullptr;
}

void ECSILayout_MoveDivider(ECSINode *split, usz divider, f32 x, f32 y)
{
    SDL_assert(split != nullptr);
    SDL_assert(divider + 1 < arrlenu(split->children));

    ECSINode *first = split->children[divider];
    ECSINode *second = split->children[divider + 1];

    f32 start = split->vertical ? first->y : first->x;
    f32 end = split->vertical ? second->y + second->height : second->x + second->width;
    f32 room = end - start - LAYOUT.dividerSize;

    if (room < OPENECS_MIN_CHILD_SIZE * 2.0f)
    {
        return;
    }

    f32 position = (split->vertical ? y : x) - start - LAYOUT.dividerSize / 2.0f;
    f32 firstSize = SDL_min(SDL_max(position, OPENECS_MIN_CHILD_SIZE), room - OPENECS_MIN_CHILD_SIZE);
    f32 secondSize = room - firstSize;

    // shares are relative, so every shared child gets its current size as its share
    for (usz i = 0; i < arrlenu(split->children); i++)
    {
        ECSINode *child = split->children[i];

        if (child->fixedSize <= 0.0f)
        {
            child->share = split->vertical ? child->height : child->width;
        }
    }

    if (first->fixedSize > 0.0f)
    {
        first->fixedSize = firstSize;
    }
    else
    {
        first->share = firstSize;
    }

    if (second->fixedSize > 0.0f)
    {
        second->fixedSize = secondSize;
    }
    else
    {
        second->share = secondSize;
    }

    LAYOUT.frameNeeded = true;
}

void ECSILayout_Drop(ECSPanel panel, bool group, const ECSIDrop *drop)
{
    SDL_assert(drop != nullptr);

    ECSIWorkspace *workspace = ECSILayout_GetCurrent();
    ECSINode *source = ECSILayout_FindGroupInWorkspace(workspace, panel, nullptr);

    if (source == nullptr || !ECSILayout_LocksAllow(source, drop))
    {
        return;
    }

    if (group)
    {
        ECSILayout_MoveGroup(workspace, source, drop);
    }
    else
    {
        ECSILayout_Move(workspace, panel, drop);
    }
}

ECSPanel ECSILayout_PanelAt(const ECSIRoot *root, f32 x, f32 y)
{
    ECSIPanelHit hit = {x, y, nullptr};
    ECSILayout_ForEachGroup(root, ECSILayout_HitPanel, &hit);
    return hit.panel;
}

ECSPanel ECSILayout_GetFocus(void)
{
    ECSIWorkspace *workspace = ECSILayout_GetCurrent();
    return workspace == nullptr ? nullptr : workspace->focus;
}

void ECSILayout_SetFocus(ECSPanel panel)
{
    ECSIWorkspace *workspace = ECSILayout_GetCurrent();

    if (workspace != nullptr && panel != nullptr)
    {
        ECSILayout_ChangeFocus(workspace, panel);
    }
}

bool ECSILayout_HasPanel(ECSPanel panel)
{
    return ECSILayout_Locate(panel, nullptr, nullptr, nullptr);
}

ECSPanel ECSILayout_FindNeighbour(i32 directionX, i32 directionY)
{
    ECSINeighbourSearch search = {ECSILayout_GetFocus(), directionX, directionY, nullptr, 0.0f, 0.0f};
    ECSIRoot *root = nullptr;

    if (ECSILayout_FindGroupInWorkspace(ECSILayout_GetCurrent(), search.from, &root) == nullptr)
    {
        return nullptr;
    }

    ECSILayout_ForEachGroup(root, ECSILayout_ConsiderNeighbour, &search);
    return search.best;
}

void ECSILayout_MoveFocus(i32 directionX, i32 directionY)
{
    ECSIWorkspace *workspace = ECSILayout_GetCurrent();
    ECSIRoot *root = nullptr;
    ECSINode *source = workspace == nullptr ? nullptr : ECSILayout_FindGroupInWorkspace(workspace, workspace->focus, &root);

    if (source == nullptr)
    {
        return;
    }

    // into the neighbour's group, or else along that edge of the OS window
    ECSPanel neighbour = ECSILayout_FindNeighbour(directionX, directionY);
    ECSIDrop drop = {.root = root};

    if (neighbour != nullptr)
    {
        drop.zone = ECSIZone_Center;
        drop.group = ECSILayout_FindGroupInTree(root->tree, neighbour);
    }
    else if (directionX != 0)
    {
        drop.zone = directionX < 0 ? ECSIZone_WindowLeft : ECSIZone_WindowRight;
    }
    else
    {
        drop.zone = directionY < 0 ? ECSIZone_WindowTop : ECSIZone_WindowBottom;
    }

    if (ECSILayout_LocksAllow(source, &drop))
    {
        ECSILayout_Move(workspace, workspace->focus, &drop);
    }
}

ECSPanel ECSILayout_NextTab(void)
{
    ECSIWorkspace *workspace = ECSILayout_GetCurrent();
    ECSINode *group = workspace == nullptr ? nullptr : ECSILayout_FindGroupInWorkspace(workspace, workspace->focus, nullptr);

    if (group == nullptr || arrlenu(group->panels) < 2)
    {
        return nullptr;
    }

    group->shown = (group->shown + 1) % arrlenu(group->panels);
    LAYOUT.frameNeeded = true;
    return group->panels[group->shown];
}

bool ECSILayout_IsLocked(ECSPanel panel)
{
    ECSINode *group = ECSILayout_FindGroupInWorkspace(ECSILayout_GetCurrent(), panel, nullptr);
    return group != nullptr && group->locked;
}

bool ECSILayout_IsMaximized(ECSPanel panel)
{
    ECSIRoot *root = nullptr;
    ECSINode *group = ECSILayout_FindGroupInWorkspace(ECSILayout_GetCurrent(), panel, &root);
    return group != nullptr && root->maximized == group;
}

const ECSPanel *ECSILayout_GetGroup(ECSPanel panel, usz *retCount, usz *retShown)
{
    SDL_assert(retCount != nullptr);
    SDL_assert(retShown != nullptr);

    ECSINode *group = ECSILayout_FindGroupInWorkspace(ECSILayout_GetCurrent(), panel, nullptr);
    *retCount = group == nullptr ? 0 : arrlenu(group->panels);
    *retShown = group == nullptr ? 0 : group->shown;
    return group == nullptr ? nullptr : group->panels;
}

bool ECSILayout_CanReopen(void)
{
    return arrlenu(LAYOUT.closed) > 0;
}

void ECSILayout_ToggleLock(void)
{
    ECSIWorkspace *workspace = ECSILayout_GetCurrent();
    ECSINode *group = workspace == nullptr ? nullptr : ECSILayout_FindGroupInWorkspace(workspace, workspace->focus, nullptr);

    if (group != nullptr)
    {
        group->locked = !group->locked;
        ECSILayout_Changed();
        SDL_Log("The group of '%s' is %s.", workspace->focus->title, group->locked ? "locked" : "unlocked");
    }
}

void ECSILayout_ToggleMaximize(void)
{
    ECSIWorkspace *workspace = ECSILayout_GetCurrent();
    ECSIRoot *root = nullptr;
    ECSINode *group = workspace == nullptr ? nullptr : ECSILayout_FindGroupInWorkspace(workspace, workspace->focus, &root);

    if (group == nullptr)
    {
        return;
    }

    root->maximized = root->maximized == group ? nullptr : group;
    ECSILayout_Changed();
}

void ECSILayout_ClosePanel(ECSPanel panel)
{
    ECSIWorkspace *workspace = nullptr;
    ECSINode *group = nullptr;

    if (!ECSILayout_Locate(panel, &workspace, nullptr, &group))
    {
        return;
    }

    ECSILayout_RememberClosed(group, panel);
    ECSPanel closed = panel;
    ECSIPanel_Close(&closed);
    ECSILayout_RemovePanel(workspace, group, panel);

    // once the last panel of every workspace is closed, there is nothing left to show
    ECSPanel *left = ECSILayout_GetPanels();
    LAYOUT.emptied = arrlenu(left) == 0;
    arrfree(left);
}

bool ECSILayout_WasEmptied(void)
{
    return LAYOUT.emptied;
}

void ECSILayout_Reopen(void)
{
    ECSIWorkspace *workspace = ECSILayout_GetCurrent();

    if (workspace == nullptr || arrlenu(LAYOUT.closed) == 0)
    {
        SDL_Log("No closed panel to reopen.");
        return;
    }

    ECSIClosedPanel closed = arrpop(LAYOUT.closed);

    // the panel returns next to its neighbour, wherever that is now, or else to the focused group
    ECSINode *group = nullptr;
    ECSPanel neighbour = closed.neighbour == 0 ? nullptr : ECSLayout_FindPanel(closed.neighbour);

    if (neighbour == nullptr || !ECSILayout_Locate(neighbour, &workspace, nullptr, &group))
    {
        workspace = ECSILayout_GetCurrent();
        group = ECSILayout_DefaultGroup(workspace);
    }

    i64 version = ECSValue_GetInteger(ECSValue_GetTableField(closed.saved, "stateVersion"), 0);
    const ECSValue *state = ECSValue_GetTableField(closed.saved, "state");
    ECSPanel panel = nullptr;

    if (ECSIPanel_Create(&panel, ECSValue_GetString(ECSValue_GetTableField(closed.saved, "type"), ""), state, version >= 0 && version <= SDL_MAX_UINT32 ? (u32)version : 0) ||
        ECSILayout_Add(workspace, group, panel, ECSIZone_Center))
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Out of memory while reopening a panel.");

        if (panel != nullptr)
        {
            ECSIPanel_Destroy(&panel);
        }
    }
    else
    {
        ECSIPanel_Emit("ecs.panelOpened", panel);
        ECSILayout_WorkspaceSwitch((usz)(workspace - LAYOUT.workspaces));
    }

    ECSValue_Destroy(&closed.saved);
}

void ECSILayout_MoveToWorkspace(usz index)
{
    ECSIWorkspace *workspace = ECSILayout_GetCurrent();
    ECSPanel panel = workspace == nullptr ? nullptr : workspace->focus;
    ECSINode *group = ECSILayout_FindGroupInWorkspace(workspace, panel, nullptr);

    if (group == nullptr || index >= arrlenu(LAYOUT.workspaces) || index == LAYOUT.current)
    {
        return;
    }

    if (group->locked)
    {
        SDL_Log("'%s' is locked; unlock it to move it.", panel->title);
        return;
    }

    // the panel joins the other workspace's focused group, and becomes its focus; this workspace stays shown
    ECSIWorkspace *target = &LAYOUT.workspaces[index];
    ECSINode *targetGroup = ECSILayout_DefaultGroup(target);
    ECSILayout_RemovePanel(workspace, group, panel);

    if (ECSILayout_Add(target, targetGroup, panel, ECSIZone_Center))
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Out of memory while moving '%s'; it is closed.", panel->title);
        ECSIPanel_Close(&panel);
    }
}

ECSPanel *ECSILayout_GetPanels(void)
{
    ECSPanel *panels = nullptr;

    for (usz i = 0; i < arrlenu(LAYOUT.workspaces); i++)
    {
        for (usz j = 0; j < arrlenu(LAYOUT.workspaces[i].roots); j++)
        {
            ECSILayout_CollectPanels(LAYOUT.workspaces[i].roots[j]->tree, &panels);
        }
    }

    return panels;
}

SHUResult ECSILayout_Save(ECSValue *retWorkspaces, usz *retCurrent)
{
    SDL_assert(retWorkspaces != nullptr);
    SDL_assert(retCurrent != nullptr);

    ECSValue_SetTable(retWorkspaces);
    *retCurrent = LAYOUT.current;

    for (usz i = 0; i < arrlenu(LAYOUT.workspaces); i++)
    {
        const ECSIWorkspace *workspace = &LAYOUT.workspaces[i];
        ECSValue *saved = nullptr;
        ECSValue *field = nullptr;

        SHU_ReturnResult(ECSValue_AddListItem(retWorkspaces, &saved));
        SHU_ReturnResult(ECSValue_SetTableField(saved, "name", &field));
        SHU_ReturnResult(ECSValue_SetString(field, workspace->name));

        if (workspace->focus != nullptr)
        {
            SHU_ReturnResult(ECSValue_SetTableField(saved, "focus", &field));
            ECSValue_SetInteger(field, workspace->focus->id);
        }

        ECSValue *windows = nullptr;
        SHU_ReturnResult(ECSValue_SetTableField(saved, "windows", &windows));
        ECSValue_SetTable(windows);

        // the main window's tree first, empty if it has no panels and pop-out windows follow; a pop-out window's tree also has its size
        for (usz j = 0; j < arrlenu(workspace->roots); j++)
        {
            const ECSIRoot *root = workspace->roots[j];

            if (root->tree == nullptr && arrlenu(workspace->roots) == 1)
            {
                break;
            }

            SHU_ReturnResult(ECSValue_AddListItem(windows, &field));
            ECSValue_SetTable(field);

            if (root->tree != nullptr)
            {
                SHU_ReturnResult(ECSILayout_SaveNode(root, root->tree, field));
            }

            if (j > 0)
            {
                ECSValue *size = nullptr;
                SHU_ReturnResult(ECSValue_SetTableField(field, "width", &size));
                ECSValue_SetNumber(size, root->width);
                SHU_ReturnResult(ECSValue_SetTableField(field, "height", &size));
                ECSValue_SetNumber(size, root->height);
            }
        }
    }

    return SHUResult_Ok;
}

SHUResult ECSLayout_Open(ECSPlugin plugin, ECSPanel *retPanel, const char *type, const ECSValue *state, ECSPanel target, ECSZone zone)
{
    SDL_assert(plugin != nullptr);
    SDL_assert(retPanel != nullptr);
    SDL_assert(type != nullptr);

    ECSIWorkspace *workspace = ECSILayout_GetCurrent();
    ECSINode *group = nullptr;

    if (workspace == nullptr)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' opens a panel, but there is no workspace.", ECSIPlugin_GetName(plugin));
        return SHUResult_ErrNotFound;
    }

    if (target != nullptr)
    {
        group = ECSILayout_FindGroupInWorkspace(workspace, target, nullptr);

        if (group == nullptr)
        {
            return SHUResult_ErrNotFound;
        }
    }
    else
    {
        // the group of the most recently focused panel of the type, else the focused group, else the first
        ECSIRecentSearch search = {.typeName = type, .best = nullptr};

        for (usz i = 0; i < arrlenu(workspace->roots); i++)
        {
            ECSILayout_FindRecent(workspace->roots[i]->tree, &search);
        }

        group = ECSILayout_FindGroupInWorkspace(workspace, search.best != nullptr ? search.best : workspace->focus, nullptr);
        group = group != nullptr ? group : ECSILayout_DefaultGroup(workspace);
    }

    ECSPanel panel = nullptr;
    SHU_ReturnResult(ECSIPanel_Create(&panel, type, state, state == nullptr ? 0 : ECSIPanels_GetStateVersion(type)));
    SHU_ReturnResult(ECSILayout_Add(workspace, group, panel, ECSILayout_ConvertZone(zone)), ECSIPanel_Destroy(&panel););
    ECSIPanel_Emit("ecs.panelOpened", panel);
    *retPanel = panel;
    return SHUResult_Ok;
}

SHUResult ECSLayout_Move(ECSPanel panel, ECSPanel target, ECSZone zone)
{
    SDL_assert(panel != nullptr);
    SDL_assert(target != nullptr || zone == ECSZone_Window);

    ECSIWorkspace *workspace = nullptr;
    ECSIWorkspace *targetWorkspace = nullptr;
    ECSIRoot *root = nullptr;
    ECSINode *source = nullptr;
    ECSINode *group = nullptr;

    if (!ECSILayout_Locate(panel, &workspace, nullptr, &source))
    {
        return SHUResult_ErrNotFound;
    }

    // a pop-out window is made in the panel's own workspace
    if (target == nullptr)
    {
        ECSIDrop drop = {.zone = ECSIZone_PopOut};
        ECSILayout_Move(workspace, panel, &drop);
        return SHUResult_Ok;
    }

    if (!ECSILayout_Locate(target, &targetWorkspace, &root, &group))
    {
        return SHUResult_ErrNotFound;
    }

    ECSIDrop drop = {.zone = ECSILayout_ConvertZone(zone), .root = root, .group = group};

    if (workspace == targetWorkspace)
    {
        ECSILayout_Move(workspace, panel, &drop);
        return SHUResult_Ok;
    }

    // to another workspace: the panel leaves its group, then joins the target's group, or goes beside it
    ECSILayout_RemovePanel(workspace, source, panel);
    return ECSILayout_Add(targetWorkspace, group, panel, drop.zone);
}

bool ECSLayout_Close(ECSPanel panel)
{
    SDL_assert(panel != nullptr);

    if (!ECSILayout_HasPanel(panel) || !ECSIPanels_ConfirmClose(&panel, 1, false))
    {
        return false;
    }

    ECSILayout_ClosePanel(panel);
    return true;
}

void ECSLayout_Focus(ECSPanel panel)
{
    SDL_assert(panel != nullptr);

    ECSIWorkspace *workspace = nullptr;
    ECSINode *group = nullptr;

    if (!ECSILayout_Locate(panel, &workspace, nullptr, &group))
    {
        return;
    }

    // the workspace and the tab are shown first, so the panel is focused where the user sees it
    ECSILayout_WorkspaceSwitch((usz)(workspace - LAYOUT.workspaces));
    ECSILayout_GroupShow(group, ECSILayout_IndexOf(group, panel));
    ECSILayout_ChangeFocus(workspace, panel);
}

ECSPanel ECSLayout_FindPanel(u32 id)
{
    // every panel is in a group, so a search of every workspace finds it
    ECSPanel *panels = ECSILayout_GetPanels();
    ECSPanel found = nullptr;

    for (usz i = 0; found == nullptr && i < arrlenu(panels); i++)
    {
        found = ECSPanel_GetID(panels[i]) == id ? panels[i] : nullptr;
    }

    arrfree(panels);
    return found;
}

ECSPanel ECSLayout_GetFocus(void)
{
    return ECSILayout_GetFocus();
}

usz ECSWorkspace_GetCount(void)
{
    return arrlenu(LAYOUT.workspaces);
}

usz ECSWorkspace_GetCurrent(void)
{
    return LAYOUT.current + 1;
}

const char *ECSWorkspace_GetName(usz number)
{
    return number >= 1 && number <= arrlenu(LAYOUT.workspaces) ? LAYOUT.workspaces[number - 1].name : nullptr;
}

void ECSWorkspace_Switch(usz number)
{
    if (number >= 1)
    {
        ECSILayout_WorkspaceSwitch(number - 1);
    }
}
