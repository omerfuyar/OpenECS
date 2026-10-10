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
    u32 lastRootId;
    ECSILayoutForgetFunction Forget; // the window's, or NULL

    // from the settings
    f32 tabRowHeight;
    f32 dividerSize; // gap between the children of a split; dragging it resizes them
    i64 reopenLimit; // how many closed panels ecs.layout.reopen remembers
} LAYOUT = {0};

static ECSIWorkspace *ECSILayout_Current(void)
{
    return arrlenu(LAYOUT.workspaces) == 0 ? NULL : &LAYOUT.workspaces[LAYOUT.current];
}

static bool ECSILayout_Contains(f32 x, f32 y, f32 left, f32 top, f32 width, f32 height)
{
    return x >= left && y >= top && x < left + width && y < top + height;
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
static void ECSILayout_ForEachGroupIn(ECSINode *node, ECSILayoutGroupFunction function, void *userData)
{
    if (node == NULL)
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
        ECSILayout_ForEachGroupIn(node->children[i], function, userData);
    }
}

/// @brief Tells each panel of a group whether it is visible. userData points to the group whose panel is shown, or to NULL for every group of the current workspace.
static void ECSILayout_TellVisible(ECSINode *group, void *userData)
{
    ECSINode *const *only = userData;

    for (usz i = 0; i < arrlenu(group->panels); i++)
    {
        bool visible = only != NULL && (*only == NULL || *only == group) && i == group->shown;
        ECSIPanel_SetVisible(group->panels[i], visible);
    }
}

/// @brief Finds the group that holds a panel.
static ECSINode *ECSILayout_FindGroup(ECSINode *node, ECSPanel panel)
{
    if (node == NULL)
    {
        return NULL;
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

        return NULL;
    }

    for (usz i = 0; i < arrlenu(node->children); i++)
    {
        ECSINode *group = ECSILayout_FindGroup(node->children[i], panel);

        if (group != NULL)
        {
            return group;
        }
    }

    return NULL;
}

/// @brief Finds the group that holds a panel in a workspace, and its root.
/// @param retRoot Gets the root, or NULL.
static ECSINode *ECSILayout_FindIn(const ECSIWorkspace *workspace, ECSPanel panel, ECSIRoot **retRoot)
{
    for (usz i = 0; workspace != NULL && panel != NULL && i < arrlenu(workspace->roots); i++)
    {
        ECSINode *group = ECSILayout_FindGroup(workspace->roots[i]->tree, panel);

        if (group != NULL)
        {
            if (retRoot != NULL)
            {
                *retRoot = workspace->roots[i];
            }

            return group;
        }
    }

    return NULL;
}

/// @brief Finds the workspace, root and group that hold a panel, in any workspace.
/// @param retRoot Gets the root, or NULL.
/// @return false if the panel is not in the layout.
static bool ECSILayout_Locate(ECSPanel panel, ECSIWorkspace **retWorkspace, ECSIRoot **retRoot, ECSINode **retGroup)
{
    for (usz i = 0; panel != NULL && i < arrlenu(LAYOUT.workspaces); i++)
    {
        ECSINode *group = ECSILayout_FindIn(&LAYOUT.workspaces[i], panel, retRoot);

        if (group != NULL)
        {
            *retWorkspace = &LAYOUT.workspaces[i];
            *retGroup = group;
            return true;
        }
    }

    return false;
}

/// @brief Emits ecs.focusChanged with the panel that has the focus now, or none.
static void ECSILayout_EmitFocus(ECSPanel panel)
{
    ECSValue *value = NULL;
    ECSValue *field = NULL;

    if (ECSValue_Create(&value) == SHUResult_Ok)
    {
        ECSValue_SetTable(value);

        // with no focused panel, the table stays empty
        if (panel != NULL && ECSValue_TableSetField(value, "panel", &field) == SHUResult_Ok)
        {
            ECSValue_SetInteger(field, ECSPanel_GetId(panel));
        }

        ECSIEvents_EmitCore("ecs.focusChanged", value);
    }

    ECSValue_Destroy(&value);
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

    if (panel != NULL)
    {
        panel->focusTicks = SDL_GetTicksNS();
    }

    if (workspace != ECSILayout_Current())
    {
        return;
    }

    ECSPanelEvent event = {.type = ECSPanelEventType_Unfocused};

    if (old != NULL)
    {
        ECSIPanel_PostEvent(old, &event);
    }

    if (panel != NULL)
    {
        event.type = ECSPanelEventType_Focused;
        ECSIPanel_PostEvent(panel, &event);
    }

    ECSILayout_EmitFocus(panel);
}

/// @brief Finds the first panel of a tree.
static ECSPanel ECSILayout_FirstPanel(ECSINode *node)
{
    if (node == NULL)
    {
        return NULL;
    }

    if (node->type == ECSINodeType_Group)
    {
        return arrlenu(node->panels) > 0 ? node->panels[node->shown] : NULL;
    }

    for (usz i = 0; i < arrlenu(node->children); i++)
    {
        ECSPanel panel = ECSILayout_FirstPanel(node->children[i]);

        if (panel != NULL)
        {
            return panel;
        }
    }

    return NULL;
}

/// @brief Finds the first panel of a workspace: in the main window, else in the first pop-out window.
static ECSPanel ECSILayout_FirstPanelOf(const ECSIWorkspace *workspace)
{
    for (usz i = 0; i < arrlenu(workspace->roots); i++)
    {
        ECSPanel panel = ECSILayout_FirstPanel(workspace->roots[i]->tree);

        if (panel != NULL)
        {
            return panel;
        }
    }

    return NULL;
}

/// @brief Makes an empty root with a new id.
static ECSIRoot *ECSILayout_NewRoot(void)
{
    ECSIRoot *root = SDL_calloc(1, sizeof(ECSIRoot));

    if (root != NULL)
    {
        root->id = ++LAYOUT.lastRootId;
    }

    return root;
}

/// @brief Destroys a root, its tree and its panels.
static void ECSILayout_FreeRoot(ECSIRoot *root)
{
    if (root->tree != NULL)
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

/// @brief Tidies a subtree after an operation: frees empty groups and splits, merges a split into a parent split of the same direction, and replaces a split that has one child by that child.
/// @return The node that takes the subtree's place, or NULL if nothing is left.
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
            root->maximized = NULL;
        }

        ECSILayout_FreeNode(node);
        return NULL;
    }

    ECSINode **children = NULL;

    for (usz i = 0; i < arrlenu(node->children); i++)
    {
        ECSINode *child = ECSILayout_TidyNode(root, node->children[i]);

        if (child == NULL)
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

    ECSINode *only = arrlenu(children) == 1 ? children[0] : NULL;

    if (only != NULL)
    {
        only->fixedSize = node->fixedSize;
        only->share = node->share;
        only->parent = node->parent;
    }

    ECSILayout_FreeNode(node);
    return only;
}

/// @brief Emits ecs.layoutChanged, which carries nothing.
static void ECSILayout_EmitLayoutChanged(void)
{
    ECSIEvents_EmitCore("ecs.layoutChanged", NULL);
}

/// @brief Lets the window forget the nodes it points to, because nodes may have been freed or hidden.
static void ECSILayout_Forget(void)
{
    if (LAYOUT.Forget != NULL)
    {
        LAYOUT.Forget();
    }
}

/// @brief Tidies a root's tree.
static void ECSILayout_TidyRoot(ECSIRoot *root)
{
    if (root->tree != NULL)
    {
        root->tree = ECSILayout_TidyNode(root, root->tree);
    }

    if (root->tree != NULL)
    {
        root->tree->parent = NULL;
    }
}

/// @brief Tidies a workspace's trees after an operation, and removes the pop-out windows that have no panels left. Nodes may be freed, so the window forgets the nodes it points to.
static void ECSILayout_Tidy(ECSIWorkspace *workspace)
{
    for (usz i = 0; i < arrlenu(workspace->roots);)
    {
        ECSIRoot *root = workspace->roots[i];
        ECSILayout_TidyRoot(root);

        if (i > 0 && root->tree == NULL)
        {
            ECSILayout_FreeRoot(root);
            arrdel(workspace->roots, i);
            continue;
        }

        i++;
    }

    ECSILayout_Forget();
    LAYOUT.frameNeeded = true;
    ECSILayout_EmitLayoutChanged();
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

    if (parent == NULL)
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

    if (parent == NULL)
    {
        root->tree = root->tree == node ? NULL : root->tree;
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

    node->parent = NULL;
}

/// @brief Places a node that is out of every tree: beside a group, along an edge of a root, as the whole tree of an empty root, or as the tree of a new pop-out root.
/// @param split A new split for the side and edge zones, which it then owns; NULL for the others.
/// @param pop A new root for a pop-out, which the workspace then owns; NULL for the others.
static void ECSILayout_Attach(ECSIWorkspace *workspace, ECSINode *node, ECSINode *split, ECSIRoot *pop, const ECSIDrop *drop)
{
    bool first = drop->zone == ECSIZone_Left || drop->zone == ECSIZone_Top || drop->zone == ECSIZone_WindowLeft || drop->zone == ECSIZone_WindowTop;

    if (pop != NULL)
    {
        pop->tree = node;
        node->parent = NULL;
        arrput(workspace->roots, pop);
    }
    else if (drop->root->tree == NULL)
    {
        // an empty main window takes the node as its whole tree
        drop->root->tree = node;
        node->parent = NULL;
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

/// @brief Makes what a move needs before it changes anything, so a failed allocation changes nothing: a group for a panel that leaves its group, a split for a side or edge zone, and a root for a pop-out.
/// @return false if memory ran out.
static bool ECSILayout_Prepare(const ECSIDrop *drop, ECSINode **retGroup, ECSINode **retSplit, ECSIRoot **retRoot)
{
    bool vertical = drop->zone == ECSIZone_Top || drop->zone == ECSIZone_Bottom || drop->zone == ECSIZone_WindowTop || drop->zone == ECSIZone_WindowBottom;
    bool failed = (retGroup != NULL && ECSILayout_GroupCreate(retGroup)) ||
                  ((ECSILayout_IsSide(drop->zone) || ECSILayout_IsEdge(drop->zone)) && ECSILayout_SplitCreate(retSplit, vertical)) ||
                  (drop->zone == ECSIZone_PopOut && (*retRoot = ECSILayout_NewRoot()) == NULL);

    if (failed)
    {
        SDL_free(retGroup != NULL ? *retGroup : NULL);
        SDL_free(*retSplit);
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Out of memory while moving a panel.");
    }

    return !failed;
}

/// @brief Gives a new pop-out root its size: the drop's, or else the moved panel's.
static void ECSILayout_SizePopOut(ECSIRoot *pop, const ECSIDrop *drop, ECSPanel panel)
{
    pop->width = drop->width > 0.0f ? drop->width : SDL_max(panel->width, OPENECS_MIN_POP_OUT_SIZE);
    pop->height = drop->height > 0.0f ? drop->height : SDL_max(panel->height, OPENECS_MIN_POP_OUT_SIZE);
}

/// @brief Moves a panel to a drop place, then tidies the trees. The panel gets the focus.
/// @param user true when the user moves the panel; locks stop the user but not code.
static void ECSILayout_Move(ECSIWorkspace *workspace, ECSPanel panel, const ECSIDrop *drop, bool user)
{
    ECSIRoot *sourceRoot = NULL;
    ECSINode *source = ECSILayout_FindIn(workspace, panel, &sourceRoot);
    ECSINode *target = drop->group;
    bool side = ECSILayout_IsSide(drop->zone);
    bool join = drop->zone == ECSIZone_Center || drop->zone == ECSIZone_Tabs;

    // a panel cannot join its own group again, split away from a group that holds only itself, or pop out of a window that holds only itself; locked panels stay
    bool alone = arrlenu(source == NULL ? NULL : source->panels) == 1;

    if (source == NULL || drop->zone == ECSIZone_None || (user && source->locked) ||
        (source == target && ((join && drop->zone == ECSIZone_Center) || (side && alone))) ||
        (drop->zone == ECSIZone_PopOut && alone && sourceRoot->tree == source))
    {
        return;
    }

    // without a group, a panel joins only an empty window
    if ((user && target != NULL && target->locked && join) || (join && target == NULL && (drop->root == NULL || drop->root->tree != NULL)))
    {
        return;
    }

    ECSINode *group = NULL;
    ECSINode *split = NULL;
    ECSIRoot *pop = NULL;

    if (!(join && target != NULL) && !ECSILayout_Prepare(drop, &group, &split, &pop))
    {
        return;
    }

    usz index = 0;

    while (source->panels[index] != panel)
    {
        index++;
    }

    arrdel(source->panels, index);
    source->shown = arrlenu(source->panels) == 0 ? 0 : SDL_min(source->shown, arrlenu(source->panels) - 1);

    if (join && target != NULL)
    {
        // the gap was counted with the panel still in its own group
        usz gap = drop->zone == ECSIZone_Center ? arrlenu(target->panels) : drop->index;
        gap = source == target && index < gap ? gap - 1 : gap;
        gap = SDL_min(gap, arrlenu(target->panels));
        // arrins of stb_ds does not compile cleanly with sign warnings, so the gap is opened by hand
        arrput(target->panels, panel);
        SDL_memmove(&target->panels[gap + 1], &target->panels[gap], (arrlenu(target->panels) - 1 - gap) * sizeof(ECSPanel));
        target->panels[gap] = panel;
        target->shown = gap;
    }
    else
    {
        if (pop != NULL)
        {
            ECSILayout_SizePopOut(pop, drop, panel);
        }

        arrput(group->panels, panel);
        ECSILayout_Attach(workspace, group, split, pop, drop);
    }

    sourceRoot->maximized = NULL;

    if (drop->root != NULL)
    {
        drop->root->maximized = NULL;
    }

    ECSILayout_ChangeFocus(workspace, panel);
    ECSILayout_Tidy(workspace);
}

/// @brief Moves a whole group to a drop place, then tidies the trees. The group's shown panel gets the focus.
/// @param user true when the user moves the group; locks stop the user but not code.
static void ECSILayout_MoveGroup(ECSIWorkspace *workspace, ECSINode *group, const ECSIDrop *drop, bool user)
{
    ECSIRoot *sourceRoot = NULL;
    ECSILayout_FindIn(workspace, group->panels[group->shown], &sourceRoot);
    ECSINode *target = drop->group;
    bool join = drop->zone == ECSIZone_Center || drop->zone == ECSIZone_Tabs;

    // a group that fills its OS window cannot dock along its edges or pop out of it
    bool whole = sourceRoot->tree == group;

    if (drop->zone == ECSIZone_None || target == group || (whole && (drop->zone == ECSIZone_PopOut || (ECSILayout_IsEdge(drop->zone) && drop->root == sourceRoot))) ||
        (user && (group->locked || (join && target != NULL && target->locked))) || (join && target == NULL && (drop->root == NULL || drop->root->tree != NULL)))
    {
        return;
    }

    ECSPanel shown = group->panels[group->shown];

    if (join && target != NULL)
    {
        // the panels keep their order; the emptied group is removed by tidying
        usz gap = SDL_min(drop->zone == ECSIZone_Center ? arrlenu(target->panels) : drop->index, arrlenu(target->panels));

        for (usz i = 0; i < arrlenu(group->panels); i++, gap++)
        {
            arrput(target->panels, group->panels[i]);
            SDL_memmove(&target->panels[gap + 1], &target->panels[gap], (arrlenu(target->panels) - 1 - gap) * sizeof(ECSPanel));
            target->panels[gap] = group->panels[i];
        }

        target->shown = gap - arrlenu(group->panels) + group->shown;
        arrfree(group->panels);
        group->shown = 0;
    }
    else
    {
        ECSINode *split = NULL;
        ECSIRoot *pop = NULL;

        if (!ECSILayout_Prepare(drop, NULL, &split, &pop))
        {
            return;
        }

        if (pop != NULL)
        {
            ECSILayout_SizePopOut(pop, drop, shown);
        }

        // the group leaves its place, which tidying removes if one child is left
        ECSILayout_Unlink(sourceRoot, group);
        ECSILayout_Attach(workspace, group, split, pop, drop);
    }

    sourceRoot->maximized = NULL;

    if (drop->root != NULL)
    {
        drop->root->maximized = NULL;
    }

    ECSILayout_ChangeFocus(workspace, shown);
    ECSILayout_Tidy(workspace);
}

/// @brief Puts a panel into a group, or into a new group if the workspace has no panels, then a side zone moves it beside the group, and a pop-out into a new OS window. The panel gets the focus.
/// @param group The group, or NULL when the workspace has no panels.
static SHUResult ECSILayout_Add(ECSIWorkspace *workspace, ECSINode *group, ECSPanel panel, ECSIZone zone)
{
    if (group == NULL)
    {
        SHU_ReturnResult(ECSILayout_GroupCreate(&group));
        workspace->roots[0]->tree = group;
    }

    arrput(group->panels, panel);
    group->shown = arrlenu(group->panels) - 1;
    ECSIRoot *root = NULL;
    ECSILayout_FindIn(workspace, panel, &root);
    ECSIDrop drop = {.zone = zone, .root = zone == ECSIZone_PopOut ? NULL : root, .group = group};

    if (zone != ECSIZone_Center && zone != ECSIZone_None)
    {
        ECSILayout_Move(workspace, panel, &drop, false);
    }

    ECSILayout_ChangeFocus(workspace, panel);
    ECSILayout_Tidy(workspace);
    return SHUResult_Ok;
}

/// @brief Takes a panel out of its group and tidies the workspace. If the panel had the focus, the first panel gets it.
static void ECSILayout_Detach(ECSIWorkspace *workspace, ECSINode *group, ECSPanel panel)
{
    usz index = 0;

    while (group->panels[index] != panel)
    {
        index++;
    }

    // tidying removes a group that empties, and forgets it if it was maximized
    arrdel(group->panels, index);
    group->shown = arrlenu(group->panels) == 0 ? 0 : SDL_min(group->shown, arrlenu(group->panels) - 1);
    ECSILayout_Tidy(workspace);

    if (workspace->focus == panel)
    {
        ECSILayout_ChangeFocus(workspace, ECSILayout_FirstPanelOf(workspace));
    }
}

/// @brief Finds the group a new panel joins in a workspace: the focused group, else the first one, or NULL if the workspace has no panels.
static ECSINode *ECSILayout_DefaultGroup(ECSIWorkspace *workspace)
{
    ECSINode *group = ECSILayout_FindIn(workspace, workspace->focus, NULL);
    return group != NULL ? group : ECSILayout_FindIn(workspace, ECSILayout_FirstPanelOf(workspace), NULL);
}

/// @brief Converts a public zone to a drop zone.
static ECSIZone ECSILayout_Zone(ECSZone zone)
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
    if (node == NULL)
    {
        return;
    }

    for (usz i = 0; i < arrlenu(node->panels); i++)
    {
        ECSPanel panel = node->panels[i];

        if (panel->focusTicks > 0 && SDL_strcmp(panel->typeName, search->typeName) == 0 && (search->best == NULL || panel->focusTicks > search->best->focusTicks))
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

static void ECSILayout_FindGroupAt(ECSINode *group, void *userData)
{
    ECSIGroupHit *hit = userData;

    if (ECSILayout_Contains(hit->x, hit->y, group->x, group->y, group->width, group->height))
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
    if (node == NULL || node->type == ECSINodeType_Group || hit->split != NULL)
    {
        return;
    }

    for (usz i = 0; i + 1 < arrlenu(node->children); i++)
    {
        ECSINode *child = node->children[i];
        bool over = node->vertical
                        ? ECSILayout_Contains(hit->x, hit->y, node->x, child->y + child->height, node->width, LAYOUT.dividerSize)
                        : ECSILayout_Contains(hit->x, hit->y, child->x + child->width, node->y, LAYOUT.dividerSize, node->height);

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

static void ECSILayout_FindPanel(ECSINode *group, void *userData)
{
    ECSIPanelHit *hit = userData;

    if (arrlenu(group->panels) == 0)
    {
        return;
    }

    ECSPanel panel = group->panels[group->shown];

    if (ECSILayout_Contains(hit->x, hit->y, panel->x, panel->y, panel->width, panel->height))
    {
        hit->panel = panel;
    }
}

typedef struct ECSINeighbourSearch
{
    ECSPanel from;
    i32 dx;
    i32 dy;
    ECSPanel best;
    f32 bestGap;
    f32 bestOverlap;
} ECSINeighbourSearch;

static void ECSILayout_FindNeighbourIn(ECSINode *group, void *userData)
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

    if (search->dx != 0)
    {
        gap = search->dx > 0 ? panel->x - (from->x + from->width) : from->x - (panel->x + panel->width);
        overlap = SDL_min(from->y + from->height, panel->y + panel->height) - SDL_max(from->y, panel->y);
    }
    else
    {
        gap = search->dy > 0 ? panel->y - (from->y + from->height) : from->y - (panel->y + panel->height);
        overlap = SDL_min(from->x + from->width, panel->x + panel->width) - SDL_max(from->x, panel->x);
    }

    if (gap < -1.0f || overlap <= 0.0f)
    {
        return;
    }

    if (search->best == NULL || gap < search->bestGap || (gap == search->bestGap && overlap > search->bestOverlap))
    {
        search->best = panel;
        search->bestGap = gap;
        search->bestOverlap = overlap;
    }
}

static void ECSILayout_WantsFrameIn(ECSINode *group, void *userData)
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
    ECSValue *field = NULL;
    ECSValue_SetTable(retNode);

    if (node->parent != NULL)
    {
        bool fixed = node->fixedSize > 0.0f;
        SHU_ReturnResult(ECSValue_TableSetField(retNode, fixed ? "size" : "share", &field));
        ECSValue_SetNumber(field, fixed ? node->fixedSize : node->share);
    }

    if (node->type == ECSINodeType_Split)
    {
        SHU_ReturnResult(ECSValue_TableSetField(retNode, "split", &field));
        SHU_ReturnResult(ECSValue_SetString(field, node->vertical ? "vertical" : "horizontal"));

        for (usz i = 0; i < arrlenu(node->children); i++)
        {
            SHU_ReturnResult(ECSValue_ListAddItem(retNode, &field));
            SHU_ReturnResult(ECSILayout_SaveNode(root, node->children[i], field));
        }

        return SHUResult_Ok;
    }

    ECSValue *panels = NULL;
    SHU_ReturnResult(ECSValue_TableSetField(retNode, "panels", &panels));
    ECSValue_SetTable(panels);

    for (usz i = 0; i < arrlenu(node->panels); i++)
    {
        SHU_ReturnResult(ECSValue_ListAddItem(panels, &field));
        SHU_ReturnResult(ECSIPanel_Save(node->panels[i], field));
    }

    SHU_ReturnResult(ECSValue_TableSetField(retNode, "shown", &field));
    ECSValue_SetInteger(field, (i64)node->shown + 1);

    if (node->locked)
    {
        SHU_ReturnResult(ECSValue_TableSetField(retNode, "locked", &field));
        ECSValue_SetBool(field, true);
    }

    if (root->maximized == node)
    {
        SHU_ReturnResult(ECSValue_TableSetField(retNode, "maximized", &field));
        ECSValue_SetBool(field, true);
    }

    return SHUResult_Ok;
}

/// @brief Adds every panel of a tree to an stb_ds array.
static void ECSILayout_CollectPanels(const ECSINode *node, ECSPanel **panels)
{
    if (node == NULL)
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

    ECSILayout_ReadSettings(NULL);
    return SHUResult_Ok;
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
    for (usz i = 0; i < arrlenu(LAYOUT.workspaces); i++)
    {
        for (usz j = 0; j < arrlenu(LAYOUT.workspaces[i].roots); j++)
        {
            ECSILayout_FreeRoot(LAYOUT.workspaces[i].roots[j]);
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
    SDL_zero(LAYOUT);
}

SHUResult ECSILayout_SplitCreate(ECSINode **retNode, bool vertical)
{
    SDL_assert(retNode != NULL);

    *retNode = SDL_calloc(1, sizeof(ECSINode));

    if (*retNode == NULL)
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
    SDL_assert(retNode != NULL);

    *retNode = SDL_calloc(1, sizeof(ECSINode));

    if (*retNode == NULL)
    {
        return SHUResult_ErrAllocation;
    }

    (*retNode)->type = ECSINodeType_Group;
    (*retNode)->share = 1.0f;
    return SHUResult_Ok;
}

void ECSILayout_SplitAdd(ECSINode *split, ECSINode *child, f32 fixedSize, f32 share)
{
    SDL_assert(split != NULL);
    SDL_assert(child != NULL);
    SDL_assert(split->type == ECSINodeType_Split);

    child->parent = split;
    child->fixedSize = SDL_max(0.0f, fixedSize);
    child->share = share > 0.0f ? share : 1.0f;
    arrput(split->children, child);
}

void ECSILayout_GroupAdd(ECSINode *group, ECSPanel panel)
{
    SDL_assert(group != NULL);
    SDL_assert(panel != NULL);
    SDL_assert(group->type == ECSINodeType_Group);

    arrput(group->panels, panel);
    ECSIPanel_Emit("ecs.panelOpened", panel);
}

void ECSILayout_GroupShow(ECSINode *group, usz index)
{
    SDL_assert(group != NULL);
    SDL_assert(group->type == ECSINodeType_Group);

    if (index < arrlenu(group->panels))
    {
        group->shown = index;
        LAYOUT.frameNeeded = true;
    }
}

void ECSILayout_GroupSetLocked(ECSINode *group, bool locked)
{
    SDL_assert(group != NULL);
    SDL_assert(group->type == ECSINodeType_Group);

    group->locked = locked;
}

void ECSILayout_NodeDestroy(ECSINode **node)
{
    SDL_assert(node != NULL);
    SDL_assert(*node != NULL);

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
    *node = NULL;
}

SHUResult ECSILayout_WorkspaceAdd(const char *name, const ECSIRootDesc *roots, usz count, ECSPanel focus)
{
    SDL_assert(name != NULL);
    SDL_assert(roots != NULL);
    SDL_assert(count >= 1);

    ECSIWorkspace workspace = {.name = SDL_strdup(name)};
    bool failed = workspace.name == NULL;

    // the workspace takes every tree, so each tree is freed once also when it fails
    for (usz i = 0; i < count; i++)
    {
        ECSIRoot *root = failed ? NULL : ECSILayout_NewRoot();

        if (root == NULL)
        {
            failed = true;

            if (roots[i].tree != NULL)
            {
                ECSINode *tree = roots[i].tree;
                ECSILayout_NodeDestroy(&tree);
            }

            continue;
        }

        *root = (ECSIRoot){.id = root->id, .tree = roots[i].tree, .maximized = roots[i].maximized, .width = roots[i].width, .height = roots[i].height};
        ECSILayout_TidyRoot(root);

        // a pop-out window without panels is left out
        if (i > 0 && root->tree == NULL)
        {
            ECSILayout_FreeRoot(root);
            continue;
        }

        arrput(workspace.roots, root);
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

    workspace.focus = focus != NULL ? focus : ECSILayout_FirstPanelOf(&workspace);
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
    ECSPanel old = LAYOUT.workspaces[LAYOUT.current].focus;
    ECSPanel focus = LAYOUT.workspaces[index].focus;
    ECSPanelEvent event = {.type = ECSPanelEventType_Unfocused};

    if (old != NULL)
    {
        ECSIPanel_PostEvent(old, &event);
    }

    if (focus != NULL)
    {
        event.type = ECSPanelEventType_Focused;
        ECSIPanel_PostEvent(focus, &event);
    }

    LAYOUT.current = index;
    ECSILayout_EmitFocus(focus);

    ECSValue *value = NULL;
    ECSValue *field = NULL;

    if (ECSValue_Create(&value) == SHUResult_Ok && ECSValue_TableSetField(value, "workspace", &field) == SHUResult_Ok)
    {
        ECSValue_SetInteger(field, (i64)index + 1);
        ECSIEvents_EmitCore("ecs.workspaceSwitched", value);
    }

    ECSValue_Destroy(&value);
    ECSILayout_Forget();
    LAYOUT.frameNeeded = true;
}

usz ECSILayout_GetCurrentWorkspace(void)
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
        ECSILayout_ForEachGroup(roots[i], ECSILayout_WantsFrameIn, &wants);
    }

    return wants;
}

void ECSILayout_Update(void)
{
    // requests made while the frame is drawn, such as a tab row that scrolls, get the next frame
    LAYOUT.frameNeeded = false;
    ECSIWorkspace *workspace = ECSILayout_Current();

    if (workspace == NULL)
    {
        return;
    }

    for (usz i = 0; i < arrlenu(workspace->roots); i++)
    {
        ECSIRoot *root = workspace->roots[i];
        ECSINode *top = root->maximized != NULL ? root->maximized : root->tree;

        if (top != NULL)
        {
            ECSILayout_Place(top, 0.0f, 0.0f, root->width, root->height);
        }
    }

    // panels of other workspaces are hidden; in this one, a maximized group hides the others of its OS window
    for (usz i = 0; i < arrlenu(LAYOUT.workspaces); i++)
    {
        for (usz j = 0; j < arrlenu(LAYOUT.workspaces[i].roots); j++)
        {
            ECSIRoot *root = LAYOUT.workspaces[i].roots[j];
            ECSINode *only = root->maximized;
            ECSILayout_ForEachGroupIn(root->tree, ECSILayout_TellVisible, &LAYOUT.workspaces[i] == workspace ? &only : NULL);
        }
    }
}

void ECSILayout_ForEachGroup(const ECSIRoot *root, ECSILayoutGroupFunction function, void *userData)
{
    if (root == NULL)
    {
        return;
    }

    if (root->maximized != NULL)
    {
        function(root->maximized, userData);
    }
    else
    {
        ECSILayout_ForEachGroupIn(root->tree, function, userData);
    }
}

ECSINode *ECSILayout_GroupOf(ECSPanel panel)
{
    return ECSILayout_FindIn(ECSILayout_Current(), panel, NULL);
}

ECSIRoot *const *ECSILayout_GetRoots(usz *retCount)
{
    SDL_assert(retCount != NULL);

    ECSIWorkspace *workspace = ECSILayout_Current();
    *retCount = workspace == NULL ? 0 : arrlenu(workspace->roots);
    return workspace == NULL ? NULL : workspace->roots;
}

ECSIRoot *ECSILayout_FindRoot(u32 id, bool *retShown)
{
    for (usz i = 0; i < arrlenu(LAYOUT.workspaces); i++)
    {
        for (usz j = 0; j < arrlenu(LAYOUT.workspaces[i].roots); j++)
        {
            if (LAYOUT.workspaces[i].roots[j]->id == id)
            {
                if (retShown != NULL)
                {
                    *retShown = i == LAYOUT.current;
                }

                return LAYOUT.workspaces[i].roots[j];
            }
        }
    }

    return NULL;
}

ECSIRoot *ECSILayout_RootOf(ECSPanel panel)
{
    ECSIWorkspace *workspace = NULL;
    ECSIRoot *root = NULL;
    ECSINode *group = NULL;
    return ECSILayout_Locate(panel, &workspace, &root, &group) ? root : NULL;
}

void ECSILayout_SetRootSize(ECSIRoot *root, f32 width, f32 height)
{
    SDL_assert(root != NULL);

    root->width = width;
    root->height = height;
}

ECSPanel *ECSILayout_GetRootPanels(const ECSIRoot *root)
{
    SDL_assert(root != NULL);

    ECSPanel *panels = NULL;
    ECSILayout_CollectPanels(root->tree, &panels);
    return panels;
}

void ECSILayout_FocusRoot(ECSIRoot *root)
{
    SDL_assert(root != NULL);

    ECSIWorkspace *workspace = ECSILayout_Current();

    if (workspace == NULL || ECSILayout_FindGroup(root->tree, workspace->focus) != NULL)
    {
        return;
    }

    // the panel focused last in the window, or else its first panel
    ECSPanel *panels = ECSILayout_GetRootPanels(root);
    ECSPanel best = ECSILayout_FirstPanel(root->tree);

    for (usz i = 0; i < arrlenu(panels); i++)
    {
        best = best == NULL || panels[i]->focusTicks > best->focusTicks ? panels[i] : best;
    }

    arrfree(panels);

    if (best != NULL)
    {
        ECSILayout_ShowTab(best);
    }
}

bool ECSILayout_CanPopOut(ECSPanel panel)
{
    ECSIRoot *root = NULL;
    ECSINode *group = ECSILayout_FindIn(ECSILayout_Current(), panel, &root);
    return group != NULL && !group->locked && !(root->tree == group && arrlenu(group->panels) == 1);
}

void ECSILayout_PopOut(void)
{
    ECSIWorkspace *workspace = ECSILayout_Current();
    ECSPanel panel = workspace == NULL ? NULL : workspace->focus;

    if (panel != NULL && ECSILayout_IsLocked(panel))
    {
        SDL_Log("'%s' is locked; unlock it to pop it out.", panel->title);
        return;
    }

    if (panel != NULL)
    {
        ECSIDrop drop = {.zone = ECSIZone_PopOut};
        ECSILayout_Move(workspace, panel, &drop, true);
    }
}

ECSINode *ECSILayout_GroupAt(const ECSIRoot *root, f32 x, f32 y)
{
    ECSIGroupHit hit = {x, y, NULL};
    ECSILayout_ForEachGroup(root, ECSILayout_FindGroupAt, &hit);
    return hit.group;
}

bool ECSILayout_DividerAt(const ECSIRoot *root, f32 x, f32 y, ECSINode **retSplit, usz *retDivider)
{
    SDL_assert(retSplit != NULL);
    SDL_assert(retDivider != NULL);

    ECSIDividerHit hit = {x, y, NULL, 0};
    ECSILayout_FindDivider(root == NULL || root->maximized != NULL ? NULL : root->tree, &hit);
    *retSplit = hit.split;
    *retDivider = hit.divider;
    return hit.split != NULL;
}

void ECSILayout_MoveDivider(ECSINode *split, usz divider, f32 x, f32 y)
{
    SDL_assert(split != NULL);
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
    SDL_assert(drop != NULL);

    ECSIWorkspace *workspace = ECSILayout_Current();
    ECSINode *source = ECSILayout_FindIn(workspace, panel, NULL);

    if (source == NULL)
    {
        return;
    }

    if (group)
    {
        ECSILayout_MoveGroup(workspace, source, drop, true);
    }
    else
    {
        ECSILayout_Move(workspace, panel, drop, true);
    }
}

ECSPanel ECSILayout_PanelAt(const ECSIRoot *root, f32 x, f32 y)
{
    ECSIPanelHit hit = {x, y, NULL};
    ECSILayout_ForEachGroup(root, ECSILayout_FindPanel, &hit);
    return hit.panel;
}

ECSPanel ECSILayout_GetFocus(void)
{
    ECSIWorkspace *workspace = ECSILayout_Current();
    return workspace == NULL ? NULL : workspace->focus;
}

void ECSILayout_SetFocus(ECSPanel panel)
{
    ECSIWorkspace *workspace = ECSILayout_Current();

    if (workspace != NULL)
    {
        ECSILayout_ChangeFocus(workspace, panel);
    }
}

bool ECSILayout_HasPanel(ECSPanel panel)
{
    ECSIWorkspace *workspace = NULL;
    ECSINode *group = NULL;
    return ECSILayout_Locate(panel, &workspace, NULL, &group);
}

ECSPanel ECSILayout_FindNeighbour(i32 dx, i32 dy)
{
    ECSINeighbourSearch search = {ECSILayout_GetFocus(), dx, dy, NULL, 0.0f, 0.0f};
    ECSIRoot *root = NULL;

    if (ECSILayout_FindIn(ECSILayout_Current(), search.from, &root) == NULL)
    {
        return NULL;
    }

    ECSILayout_ForEachGroup(root, ECSILayout_FindNeighbourIn, &search);
    return search.best;
}

void ECSILayout_MoveFocus(i32 dx, i32 dy)
{
    ECSIWorkspace *workspace = ECSILayout_Current();
    ECSPanel neighbour = ECSILayout_FindNeighbour(dx, dy);
    ECSIRoot *root = NULL;

    if (workspace == NULL || ECSILayout_FindIn(workspace, workspace->focus, &root) == NULL)
    {
        return;
    }

    ECSIDrop drop = {.zone = dx < 0 ? ECSIZone_WindowLeft : dx > 0 ? ECSIZone_WindowRight
                                                          : dy < 0   ? ECSIZone_WindowTop
                                                                     : ECSIZone_WindowBottom,
                     .root = root};

    if (neighbour != NULL)
    {
        drop = (ECSIDrop){.zone = ECSIZone_Center, .root = root, .group = ECSILayout_FindGroup(root->tree, neighbour)};
    }

    ECSILayout_Move(workspace, workspace->focus, &drop, true);
}

ECSPanel ECSILayout_NextTab(void)
{
    ECSIWorkspace *workspace = ECSILayout_Current();
    ECSINode *group = workspace == NULL ? NULL : ECSILayout_FindIn(workspace, workspace->focus, NULL);

    if (group == NULL || arrlenu(group->panels) < 2)
    {
        return NULL;
    }

    group->shown = (group->shown + 1) % arrlenu(group->panels);
    LAYOUT.frameNeeded = true;
    return group->panels[group->shown];
}

bool ECSILayout_IsLocked(ECSPanel panel)
{
    ECSINode *group = ECSILayout_FindIn(ECSILayout_Current(), panel, NULL);
    return group != NULL && group->locked;
}

bool ECSILayout_IsMaximized(ECSPanel panel)
{
    ECSIRoot *root = NULL;
    ECSINode *group = ECSILayout_FindIn(ECSILayout_Current(), panel, &root);
    return group != NULL && root->maximized == group;
}

const ECSPanel *ECSILayout_GetGroup(ECSPanel panel, usz *retCount, usz *retShown)
{
    SDL_assert(retCount != NULL);
    SDL_assert(retShown != NULL);

    ECSINode *group = ECSILayout_FindIn(ECSILayout_Current(), panel, NULL);
    *retCount = group == NULL ? 0 : arrlenu(group->panels);
    *retShown = group == NULL ? 0 : group->shown;
    return group == NULL ? NULL : group->panels;
}

void ECSILayout_ShowTab(ECSPanel panel)
{
    ECSIWorkspace *workspace = ECSILayout_Current();
    ECSINode *group = ECSILayout_FindIn(workspace, panel, NULL);

    for (usz i = 0; group != NULL && i < arrlenu(group->panels); i++)
    {
        if (group->panels[i] == panel)
        {
            group->shown = i;
            ECSILayout_ChangeFocus(workspace, panel);
            LAYOUT.frameNeeded = true;
        }
    }
}

bool ECSILayout_CanReopen(void)
{
    return arrlenu(LAYOUT.closed) > 0;
}

void ECSILayout_ToggleLock(void)
{
    ECSIWorkspace *workspace = ECSILayout_Current();
    ECSINode *group = workspace == NULL ? NULL : ECSILayout_FindIn(workspace, workspace->focus, NULL);

    if (group != NULL)
    {
        group->locked = !group->locked;
        ECSILayout_Forget();
        LAYOUT.frameNeeded = true;
        ECSILayout_EmitLayoutChanged();
        SDL_Log("The group of '%s' is %s.", workspace->focus->title, group->locked ? "locked" : "unlocked");
    }
}

void ECSILayout_ToggleMaximize(void)
{
    ECSIWorkspace *workspace = ECSILayout_Current();
    ECSIRoot *root = NULL;
    ECSINode *group = workspace == NULL ? NULL : ECSILayout_FindIn(workspace, workspace->focus, &root);

    if (group == NULL)
    {
        return;
    }

    root->maximized = root->maximized == group ? NULL : group;
    ECSILayout_Forget();
    LAYOUT.frameNeeded = true;
    ECSILayout_EmitLayoutChanged();
}

void ECSILayout_ClosePanel(ECSPanel panel)
{
    ECSIWorkspace *workspace = NULL;
    ECSINode *group = NULL;

    if (!ECSILayout_Locate(panel, &workspace, NULL, &group))
    {
        return;
    }

    usz index = 0;

    while (group->panels[index] != panel)
    {
        index++;
    }

    // ecs.layout.reopen opens it again next to a panel that stays in its group
    ECSIClosedPanel closed = {.neighbour = arrlenu(group->panels) > 1 ? ECSPanel_GetId(group->panels[index == 0 ? 1 : 0]) : 0};

    if (ECSValue_Create(&closed.saved) == SHUResult_Ok && ECSIPanel_Save(panel, closed.saved) == SHUResult_Ok)
    {
        // a limit made smaller drops the oldest panels too
        while (arrlenu(LAYOUT.closed) > 0 && (i64)arrlenu(LAYOUT.closed) >= LAYOUT.reopenLimit)
        {
            ECSValue_Destroy(&LAYOUT.closed[0].saved);
            arrdel(LAYOUT.closed, 0);
        }

        if (LAYOUT.reopenLimit > 0)
        {
            arrput(LAYOUT.closed, closed);
        }
        else
        {
            ECSValue_Destroy(&closed.saved);
        }
    }
    else
    {
        ECSValue_Destroy(&closed.saved);
    }

    arrdel(group->panels, index);

    // tidying frees an empty group, so the next focus is found before and after
    bool emptied = arrlenu(group->panels) == 0;
    bool focused = workspace->focus == panel;

    if (!emptied)
    {
        group->shown = SDL_min(group->shown, arrlenu(group->panels) - 1);
    }

    if (focused)
    {
        ECSILayout_ChangeFocus(workspace, emptied ? NULL : group->panels[group->shown]);
    }

    ECSIPanel_Close(&panel);
    ECSILayout_Tidy(workspace);

    if (focused && emptied)
    {
        ECSILayout_ChangeFocus(workspace, ECSILayout_FirstPanelOf(workspace));
    }
}

void ECSILayout_Reopen(void)
{
    ECSIWorkspace *workspace = ECSILayout_Current();

    if (workspace == NULL || arrlenu(LAYOUT.closed) == 0)
    {
        SDL_Log("No closed panel to reopen.");
        return;
    }

    ECSIClosedPanel closed = arrpop(LAYOUT.closed);

    // the panel returns next to its neighbour, wherever that is now, or else to the focused group
    ECSINode *group = NULL;
    ECSPanel neighbour = closed.neighbour == 0 ? NULL : ECSLayout_FindPanel(closed.neighbour);

    if (neighbour == NULL || !ECSILayout_Locate(neighbour, &workspace, NULL, &group))
    {
        workspace = ECSILayout_Current();
        group = ECSILayout_DefaultGroup(workspace);
    }

    i64 version = ECSValue_GetInteger(ECSValue_GetTableField(closed.saved, "stateVersion"), 0);
    const ECSValue *state = ECSValue_GetTableField(closed.saved, "state");
    ECSPanel panel = NULL;

    if (ECSIPanel_Create(&panel, ECSValue_GetString(ECSValue_GetTableField(closed.saved, "type"), ""), state, version >= 0 && version <= SDL_MAX_UINT32 ? (u32)version : 0) ||
        ECSILayout_Add(workspace, group, panel, ECSIZone_Center))
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Out of memory while reopening a panel.");

        if (panel != NULL)
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
    ECSIWorkspace *workspace = ECSILayout_Current();
    ECSPanel panel = workspace == NULL ? NULL : workspace->focus;
    ECSINode *group = ECSILayout_FindIn(workspace, panel, NULL);

    if (group == NULL || index >= arrlenu(LAYOUT.workspaces) || index == LAYOUT.current)
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
    ECSILayout_Detach(workspace, group, panel);

    if (ECSILayout_Add(target, targetGroup, panel, ECSIZone_Center))
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Out of memory while moving '%s'; it is closed.", panel->title);
        ECSIPanel_Close(&panel);
    }
}

ECSPanel *ECSILayout_GetPanels(void)
{
    ECSPanel *panels = NULL;

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
    SDL_assert(retWorkspaces != NULL);
    SDL_assert(retCurrent != NULL);

    ECSValue_SetTable(retWorkspaces);
    *retCurrent = LAYOUT.current;

    for (usz i = 0; i < arrlenu(LAYOUT.workspaces); i++)
    {
        const ECSIWorkspace *workspace = &LAYOUT.workspaces[i];
        ECSValue *saved = NULL;
        ECSValue *field = NULL;

        SHU_ReturnResult(ECSValue_ListAddItem(retWorkspaces, &saved));
        SHU_ReturnResult(ECSValue_TableSetField(saved, "name", &field));
        SHU_ReturnResult(ECSValue_SetString(field, workspace->name));

        if (workspace->focus != NULL)
        {
            SHU_ReturnResult(ECSValue_TableSetField(saved, "focus", &field));
            ECSValue_SetInteger(field, workspace->focus->id);
        }

        ECSValue *windows = NULL;
        SHU_ReturnResult(ECSValue_TableSetField(saved, "windows", &windows));
        ECSValue_SetTable(windows);

        // the main window's tree first, empty if it has no panels and pop-out windows follow; a pop-out window's tree also has its size
        for (usz j = 0; j < arrlenu(workspace->roots); j++)
        {
            const ECSIRoot *root = workspace->roots[j];

            if (root->tree == NULL && arrlenu(workspace->roots) == 1)
            {
                break;
            }

            SHU_ReturnResult(ECSValue_ListAddItem(windows, &field));
            ECSValue_SetTable(field);

            if (root->tree != NULL)
            {
                SHU_ReturnResult(ECSILayout_SaveNode(root, root->tree, field));
            }

            if (j > 0)
            {
                ECSValue *size = NULL;
                SHU_ReturnResult(ECSValue_TableSetField(field, "width", &size));
                ECSValue_SetNumber(size, root->width);
                SHU_ReturnResult(ECSValue_TableSetField(field, "height", &size));
                ECSValue_SetNumber(size, root->height);
            }
        }
    }

    return SHUResult_Ok;
}

SHUResult ECSLayout_Open(ECSPlugin plugin, ECSPanel *retPanel, const char *type, const ECSValue *state, ECSPanel target, ECSZone zone)
{
    SDL_assert(plugin != NULL);
    SDL_assert(retPanel != NULL);
    SDL_assert(type != NULL);

    ECSIWorkspace *workspace = ECSILayout_Current();
    ECSINode *group = NULL;

    if (workspace == NULL)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' opens a panel, but there is no workspace.", ECSIPlugin_GetName(plugin));
        return SHUResult_ErrNotFound;
    }

    if (target != NULL)
    {
        group = ECSILayout_FindIn(workspace, target, NULL);

        if (group == NULL)
        {
            return SHUResult_ErrNotFound;
        }
    }
    else
    {
        // the group of the most recently focused panel of the type, else the focused group, else the first
        ECSIRecentSearch search = {.typeName = type, .best = NULL};

        for (usz i = 0; i < arrlenu(workspace->roots); i++)
        {
            ECSILayout_FindRecent(workspace->roots[i]->tree, &search);
        }

        group = ECSILayout_FindIn(workspace, search.best != NULL ? search.best : workspace->focus, NULL);
        group = group != NULL ? group : ECSILayout_DefaultGroup(workspace);
    }

    ECSPanel panel = NULL;
    SHU_ReturnResult(ECSIPanel_Create(&panel, type, state, state == NULL ? 0 : ECSIPanels_GetStateVersion(type)));
    SHU_ReturnResult(ECSILayout_Add(workspace, group, panel, ECSILayout_Zone(zone)), ECSIPanel_Destroy(&panel););
    ECSIPanel_Emit("ecs.panelOpened", panel);
    *retPanel = panel;
    return SHUResult_Ok;
}

SHUResult ECSLayout_Move(ECSPanel panel, ECSPanel target, ECSZone zone)
{
    SDL_assert(panel != NULL);
    SDL_assert(target != NULL || zone == ECSZone_Window);

    ECSIWorkspace *workspace = NULL;
    ECSIWorkspace *targetWorkspace = NULL;
    ECSIRoot *root = NULL;
    ECSINode *source = NULL;
    ECSINode *group = NULL;

    if (!ECSILayout_Locate(panel, &workspace, NULL, &source))
    {
        return SHUResult_ErrNotFound;
    }

    // a pop-out window is made in the panel's own workspace
    if (target == NULL)
    {
        ECSIDrop drop = {.zone = ECSIZone_PopOut};
        ECSILayout_Move(workspace, panel, &drop, false);
        return SHUResult_Ok;
    }

    if (!ECSILayout_Locate(target, &targetWorkspace, &root, &group))
    {
        return SHUResult_ErrNotFound;
    }

    ECSIDrop drop = {.zone = ECSILayout_Zone(zone), .root = root, .group = group};

    if (workspace == targetWorkspace)
    {
        ECSILayout_Move(workspace, panel, &drop, false);
        return SHUResult_Ok;
    }

    // to another workspace: the panel leaves its group, then joins the target's group, or goes beside it
    ECSILayout_Detach(workspace, source, panel);
    return ECSILayout_Add(targetWorkspace, group, panel, drop.zone);
}

bool ECSLayout_Close(ECSPanel panel)
{
    // without a panel, it is the user's close, which a lock stops
    if (panel == NULL)
    {
        panel = ECSILayout_GetFocus();

        if (panel != NULL && ECSILayout_IsLocked(panel))
        {
            SDL_Log("'%s' is locked; unlock it to close it.", panel->title);
            return false;
        }
    }

    if (panel == NULL || !ECSILayout_HasPanel(panel) || !ECSIPanels_ConfirmClose(&panel, 1, false))
    {
        return false;
    }

    ECSILayout_ClosePanel(panel);
    return true;
}

void ECSLayout_Focus(ECSPanel panel)
{
    SDL_assert(panel != NULL);

    ECSIWorkspace *workspace = NULL;
    ECSINode *group = NULL;

    if (!ECSILayout_Locate(panel, &workspace, NULL, &group))
    {
        return;
    }

    // the workspace and the tab are shown first, so the panel is focused where the user sees it
    ECSILayout_WorkspaceSwitch((usz)(workspace - LAYOUT.workspaces));

    for (usz i = 0; i < arrlenu(group->panels); i++)
    {
        if (group->panels[i] == panel)
        {
            ECSILayout_GroupShow(group, i);
        }
    }

    ECSILayout_ChangeFocus(workspace, panel);
}

ECSPanel ECSLayout_FindPanel(u32 id)
{
    // every panel is in a group, so a search of every workspace finds it
    ECSPanel *panels = ECSILayout_GetPanels();
    ECSPanel found = NULL;

    for (usz i = 0; found == NULL && i < arrlenu(panels); i++)
    {
        found = ECSPanel_GetId(panels[i]) == id ? panels[i] : NULL;
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
    return number >= 1 && number <= arrlenu(LAYOUT.workspaces) ? LAYOUT.workspaces[number - 1].name : NULL;
}

void ECSWorkspace_Switch(usz number)
{
    if (number >= 1)
    {
        ECSILayout_WorkspaceSwitch(number - 1);
    }
}
