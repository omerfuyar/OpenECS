#include "interface/Layout.h"

#include "runtime/Events.h"
#include "runtime/Plugins.h"

#include "SDL3/SDL.h"
#include "stb/stbSDL3.h"

#pragma region Source Only

/// @brief Gap between the children of a split; dragging it resizes them.
#define OPENECS_DIVIDER_SIZE 4.0f
/// @brief How many closed panels ecs.reopen remembers.
#define OPENECS_REOPEN_LIMIT 20
/// @brief Smallest size a divider drag leaves to a child.
#define OPENECS_MIN_CHILD_SIZE 32.0f

/// @brief A named arrangement of panels.
typedef struct ECSI_Workspace
{
    char *name;
    ECSI_Node *tree;      // NULL when the workspace has no panels
    ECSI_Node *maximized; // group that fills the window, or NULL
    ECSPanel focus;
} ECSI_Workspace;

/// @brief A closed panel that ecs.reopen can open again.
typedef struct ECSI_ClosedPanel
{
    ECSValue *saved; // its type, state and state_version, as ECSI_PanelSave writes them
    u32 neighbour;   // id of a panel that stayed in its group, or 0
} ECSI_ClosedPanel;

static struct
{
    ECSI_Workspace *workspaces; // stb_ds array
    usz current;
    ECSI_ClosedPanel *closed; // stb_ds array of the panels ecs.reopen can open again, the last closed last
    bool frameNeeded;
    ECSI_LayoutForgetFunction Forget; // the window's, or NULL
} LAYOUT = {0};

static ECSI_Workspace *ECSI_LayoutCurrent(void)
{
    return arrlenu(LAYOUT.workspaces) == 0 ? NULL : &LAYOUT.workspaces[LAYOUT.current];
}

static bool ECSI_LayoutContains(f32 x, f32 y, f32 left, f32 top, f32 width, f32 height)
{
    return x >= left && y >= top && x < left + width && y < top + height;
}

/// @brief Computes the rectangles of a node and its children, and places the shown panel of each group.
static void ECSI_LayoutPlace(ECSI_Node *node, f32 x, f32 y, f32 width, f32 height)
{
    node->x = x;
    node->y = y;
    node->width = width;
    node->height = height;

    if (node->type == ECSI_NodeType_Group)
    {
        f32 top = arrlenu(node->panels) >= 2 ? OPENECS_TAB_ROW_HEIGHT : 0.0f;

        if (arrlenu(node->panels) > 0)
        {
            ECSI_PanelSetRect(node->panels[node->shown], x, y + top, width, SDL_max(0.0f, height - top));
        }

        return;
    }

    f32 total = node->vertical ? height : width;
    f32 available = total - OPENECS_DIVIDER_SIZE * (f32)(arrlenu(node->children) - 1);
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
        ECSI_Node *child = node->children[i];
        f32 size = child->fixedSize > 0.0f ? child->fixedSize : (shareSum > 0.0f ? remaining * child->share / shareSum : 0.0f);

        // the last child takes what is left, so rounding leaves no gap
        size = i + 1 == arrlenu(node->children) ? end - position : SDL_min(size, end - position);
        size = SDL_max(0.0f, size);

        if (node->vertical)
        {
            ECSI_LayoutPlace(child, x, position, width, size);
        }
        else
        {
            ECSI_LayoutPlace(child, position, y, size, height);
        }

        position += size + OPENECS_DIVIDER_SIZE;
    }
}

/// @brief Calls a function for every group of a tree.
static void ECSI_LayoutForEachGroupIn(ECSI_Node *node, ECSI_LayoutGroupFunction function, void *userData)
{
    if (node == NULL)
    {
        return;
    }

    if (node->type == ECSI_NodeType_Group)
    {
        function(node, userData);
        return;
    }

    for (usz i = 0; i < arrlenu(node->children); i++)
    {
        ECSI_LayoutForEachGroupIn(node->children[i], function, userData);
    }
}

/// @brief Tells each panel of a group whether it is visible. userData points to the group whose panel is shown, or to NULL for every group of the current workspace.
static void ECSI_LayoutTellVisible(ECSI_Node *group, void *userData)
{
    ECSI_Node *const *only = userData;

    for (usz i = 0; i < arrlenu(group->panels); i++)
    {
        bool visible = only != NULL && (*only == NULL || *only == group) && i == group->shown;
        ECSI_PanelSetVisible(group->panels[i], visible);
    }
}

/// @brief Finds the group that holds a panel.
static ECSI_Node *ECSI_LayoutFindGroup(ECSI_Node *node, ECSPanel panel)
{
    if (node == NULL)
    {
        return NULL;
    }

    if (node->type == ECSI_NodeType_Group)
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
        ECSI_Node *group = ECSI_LayoutFindGroup(node->children[i], panel);

        if (group != NULL)
        {
            return group;
        }
    }

    return NULL;
}

/// @brief Finds the workspace and group that hold a panel, in any workspace.
/// @return false if the panel is not in the layout.
static bool ECSI_LayoutLocate(ECSPanel panel, ECSI_Workspace **retWorkspace, ECSI_Node **retGroup)
{
    for (usz i = 0; panel != NULL && i < arrlenu(LAYOUT.workspaces); i++)
    {
        ECSI_Node *group = ECSI_LayoutFindGroup(LAYOUT.workspaces[i].tree, panel);

        if (group != NULL)
        {
            *retWorkspace = &LAYOUT.workspaces[i];
            *retGroup = group;
            return true;
        }
    }

    return false;
}

/// @brief Emits ecs.focus_changed with the panel that has the focus now, or none.
static void ECSI_LayoutEmitFocus(ECSPanel panel)
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

        ECSI_EventsEmitCore("ecs.focus_changed", value);
    }

    ECSValue_Destroy(&value);
}

/// @brief Changes a workspace's focus. In the current workspace, the panel that loses focus and the one that gets it are told.
static void ECSI_LayoutChangeFocus(ECSI_Workspace *workspace, ECSPanel panel)
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

    if (workspace != ECSI_LayoutCurrent())
    {
        return;
    }

    ECSPanelEvent event = {.type = ECSPanelEventType_Unfocused};

    if (old != NULL)
    {
        ECSI_PanelPostEvent(old, &event);
    }

    if (panel != NULL)
    {
        event.type = ECSPanelEventType_Focused;
        ECSI_PanelPostEvent(panel, &event);
    }

    ECSI_LayoutEmitFocus(panel);
}

/// @brief Finds the first panel of a tree.
static ECSPanel ECSI_LayoutFirstPanel(ECSI_Node *node)
{
    if (node == NULL)
    {
        return NULL;
    }

    if (node->type == ECSI_NodeType_Group)
    {
        return arrlenu(node->panels) > 0 ? node->panels[node->shown] : NULL;
    }

    for (usz i = 0; i < arrlenu(node->children); i++)
    {
        ECSPanel panel = ECSI_LayoutFirstPanel(node->children[i]);

        if (panel != NULL)
        {
            return panel;
        }
    }

    return NULL;
}

/// @brief Frees one node, without its children or panels.
static void ECSI_LayoutFreeNode(ECSI_Node *node)
{
    arrfree(node->children);
    arrfree(node->panels);
    SDL_free(node);
}

/// @brief Tidies a subtree after an operation: frees empty groups and splits, merges a split into a parent split of the same direction, and replaces a split that has one child by that child.
/// @return The node that takes the subtree's place, or NULL if nothing is left.
static ECSI_Node *ECSI_LayoutTidyNode(ECSI_Workspace *workspace, ECSI_Node *node)
{
    if (node->type == ECSI_NodeType_Group)
    {
        if (arrlenu(node->panels) > 0)
        {
            return node;
        }

        if (workspace->maximized == node)
        {
            workspace->maximized = NULL;
        }

        ECSI_LayoutFreeNode(node);
        return NULL;
    }

    ECSI_Node **children = NULL;

    for (usz i = 0; i < arrlenu(node->children); i++)
    {
        ECSI_Node *child = ECSI_LayoutTidyNode(workspace, node->children[i]);

        if (child == NULL)
        {
            continue;
        }

        // a child split of the same direction gives its children its place; a fixed-size one keeps them, so their shares keep their meaning
        if (child->type == ECSI_NodeType_Split && child->vertical == node->vertical && child->fixedSize <= 0.0f)
        {
            f32 shareSum = 0.0f;

            for (usz j = 0; j < arrlenu(child->children); j++)
            {
                shareSum += child->children[j]->fixedSize > 0.0f ? 0.0f : child->children[j]->share;
            }

            for (usz j = 0; j < arrlenu(child->children); j++)
            {
                ECSI_Node *grandchild = child->children[j];
                grandchild->share = grandchild->fixedSize > 0.0f || shareSum <= 0.0f ? grandchild->share : grandchild->share * child->share / shareSum;
                grandchild->parent = node;
                arrput(children, grandchild);
            }

            ECSI_LayoutFreeNode(child);
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

    ECSI_Node *only = arrlenu(children) == 1 ? children[0] : NULL;

    if (only != NULL)
    {
        only->fixedSize = node->fixedSize;
        only->share = node->share;
        only->parent = node->parent;
    }

    ECSI_LayoutFreeNode(node);
    return only;
}

/// @brief Emits ecs.layout_changed, which carries nothing.
static void ECSI_LayoutEmitLayoutChanged(void)
{
    ECSI_EventsEmitCore("ecs.layout_changed", NULL);
}

/// @brief Lets the window forget the nodes it points to, because nodes may have been freed or hidden.
static void ECSI_LayoutForget(void)
{
    if (LAYOUT.Forget != NULL)
    {
        LAYOUT.Forget();
    }
}

/// @brief Tidies a workspace's tree after an operation. Nodes may be freed, so the window forgets the nodes it points to.
static void ECSI_LayoutTidy(ECSI_Workspace *workspace)
{
    if (workspace->tree != NULL)
    {
        workspace->tree = ECSI_LayoutTidyNode(workspace, workspace->tree);
    }

    if (workspace->tree != NULL)
    {
        workspace->tree->parent = NULL;
    }

    ECSI_LayoutForget();
    LAYOUT.frameNeeded = true;
    ECSI_LayoutEmitLayoutChanged();
}

#pragma region Moving

/// @brief Puts a node in another node's place in the tree. The other node keeps its children and sizes.
static void ECSI_LayoutReplaceNode(ECSI_Workspace *workspace, ECSI_Node *old, ECSI_Node *node)
{
    ECSI_Node *parent = old->parent;
    node->parent = parent;
    node->fixedSize = old->fixedSize;
    node->share = old->share;

    if (parent == NULL)
    {
        workspace->tree = node;
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
static void ECSI_LayoutWrap(ECSI_Workspace *workspace, ECSI_Node *split, ECSI_Node *node, ECSI_Node *added, bool first, f32 nodeShare, f32 addedShare)
{
    ECSI_LayoutReplaceNode(workspace, node, split);
    node->fixedSize = 0.0f;
    node->share = nodeShare;
    added->fixedSize = 0.0f;
    added->share = addedShare;
    node->parent = split;
    added->parent = split;
    arrput(split->children, first ? added : node);
    arrput(split->children, first ? node : added);
}

/// @brief Moves a panel to a drop place, then tidies the tree. The panel gets the focus.
/// @param user true when the user moves the panel; locks stop the user but not code.
static void ECSI_LayoutMove(ECSI_Workspace *workspace, ECSPanel panel, const ECSI_Drop *drop, bool user)
{
    ECSI_Node *source = ECSI_LayoutFindGroup(workspace->tree, panel);
    ECSI_Node *target = drop->group;
    bool side = drop->zone >= ECSI_Zone_Left && drop->zone <= ECSI_Zone_Bottom;
    bool edge = drop->zone >= ECSI_Zone_WindowLeft;

    // a panel cannot join its own group again, or split away from a group that holds only itself; locked panels stay
    if (source == NULL || drop->zone == ECSI_Zone_None || (user && source->locked) || (source == target && (drop->zone == ECSI_Zone_Center || (side && arrlenu(source->panels) == 1))))
    {
        return;
    }

    if (user && target != NULL && target->locked && (drop->zone == ECSI_Zone_Center || drop->zone == ECSI_Zone_Tabs))
    {
        return;
    }

    // the new nodes are made first, so a failed allocation changes nothing
    ECSI_Node *group = NULL;
    ECSI_Node *split = NULL;

    if (side || edge)
    {
        bool vertical = drop->zone == ECSI_Zone_Top || drop->zone == ECSI_Zone_Bottom || drop->zone == ECSI_Zone_WindowTop || drop->zone == ECSI_Zone_WindowBottom;

        if (ECSI_LayoutGroupCreate(&group) || ECSI_LayoutSplitCreate(&split, vertical))
        {
            SDL_free(group);
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Out of memory while moving a panel.");
            return;
        }
    }

    usz index = 0;

    while (source->panels[index] != panel)
    {
        index++;
    }

    arrdel(source->panels, index);
    source->shown = arrlenu(source->panels) == 0 ? 0 : SDL_min(source->shown, arrlenu(source->panels) - 1);
    bool first = drop->zone == ECSI_Zone_Left || drop->zone == ECSI_Zone_Top || drop->zone == ECSI_Zone_WindowLeft || drop->zone == ECSI_Zone_WindowTop;

    if (drop->zone == ECSI_Zone_Center || drop->zone == ECSI_Zone_Tabs)
    {
        // the gap was counted with the panel still in its own group
        usz gap = drop->zone == ECSI_Zone_Center ? arrlenu(target->panels) : drop->index;
        gap = source == target && index < gap ? gap - 1 : gap;
        gap = SDL_min(gap, arrlenu(target->panels));
        // arrins of stb_ds does not compile cleanly with sign warnings, so the gap is opened by hand
        arrput(target->panels, panel);
        SDL_memmove(&target->panels[gap + 1], &target->panels[gap], (arrlenu(target->panels) - 1 - gap) * sizeof(ECSPanel));
        target->panels[gap] = panel;
        target->shown = gap;
    }
    else if (side)
    {
        // the target and the panel share the target's place
        arrput(group->panels, panel);
        ECSI_LayoutWrap(workspace, split, target, group, first, 1.0f, 1.0f);
    }
    else
    {
        // the panel docks along the whole edge, with a quarter of the window
        arrput(group->panels, panel);
        ECSI_LayoutWrap(workspace, split, workspace->tree, group, first, 3.0f, 1.0f);
    }

    workspace->maximized = NULL;
    ECSI_LayoutChangeFocus(workspace, panel);
    ECSI_LayoutTidy(workspace);
}

/// @brief Moves a whole group to a drop place, then tidies the tree. The group's shown panel gets the focus.
/// @param user true when the user moves the group; locks stop the user but not code.
static void ECSI_LayoutMoveGroup(ECSI_Workspace *workspace, ECSI_Node *group, const ECSI_Drop *drop, bool user)
{
    ECSI_Node *target = drop->group;
    bool side = drop->zone >= ECSI_Zone_Left && drop->zone <= ECSI_Zone_Bottom;
    bool edge = drop->zone >= ECSI_Zone_WindowLeft;
    bool join = drop->zone == ECSI_Zone_Center || drop->zone == ECSI_Zone_Tabs;

    if (drop->zone == ECSI_Zone_None || target == group || (edge && workspace->tree == group) || (user && (group->locked || (join && target->locked))))
    {
        return;
    }

    ECSPanel shown = group->panels[group->shown];

    if (join)
    {
        // the panels keep their order; the emptied group is removed by tidying
        usz gap = SDL_min(drop->zone == ECSI_Zone_Center ? arrlenu(target->panels) : drop->index, arrlenu(target->panels));

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
        bool vertical = drop->zone == ECSI_Zone_Top || drop->zone == ECSI_Zone_Bottom || drop->zone == ECSI_Zone_WindowTop || drop->zone == ECSI_Zone_WindowBottom;
        bool first = drop->zone == ECSI_Zone_Left || drop->zone == ECSI_Zone_Top || drop->zone == ECSI_Zone_WindowLeft || drop->zone == ECSI_Zone_WindowTop;
        ECSI_Node *split = NULL;

        if (ECSI_LayoutSplitCreate(&split, vertical))
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Out of memory while moving a group.");
            return;
        }

        // the group leaves its parent split, which tidying removes if one child is left
        ECSI_Node *parent = group->parent;

        for (usz i = 0; parent != NULL && i < arrlenu(parent->children); i++)
        {
            if (parent->children[i] == group)
            {
                arrdel(parent->children, i);
                break;
            }
        }

        group->parent = NULL;

        if (side)
        {
            ECSI_LayoutWrap(workspace, split, target, group, first, 1.0f, 1.0f);
        }
        else
        {
            ECSI_LayoutWrap(workspace, split, workspace->tree, group, first, 3.0f, 1.0f);
        }
    }

    workspace->maximized = NULL;
    ECSI_LayoutChangeFocus(workspace, shown);
    ECSI_LayoutTidy(workspace);
}

/// @brief Puts a panel into a group, or into a new group if the workspace has no panels, then a side zone moves it beside the group. The panel gets the focus.
/// @param group The group, or NULL when the workspace has no panels.
static SHUResult ECSI_LayoutAdd(ECSI_Workspace *workspace, ECSI_Node *group, ECSPanel panel, ECSI_Zone zone)
{
    if (group == NULL)
    {
        SHU_ReturnResult(ECSI_LayoutGroupCreate(&group));
        workspace->tree = group;
    }

    arrput(group->panels, panel);
    group->shown = arrlenu(group->panels) - 1;
    ECSI_Drop drop = {.zone = zone, .group = group};

    if (zone != ECSI_Zone_Center && zone != ECSI_Zone_None)
    {
        ECSI_LayoutMove(workspace, panel, &drop, false);
    }

    ECSI_LayoutChangeFocus(workspace, panel);
    ECSI_LayoutTidy(workspace);
    return SHUResult_Ok;
}

/// @brief Takes a panel out of its group and tidies the workspace. If the panel had the focus, the first panel gets it.
static void ECSI_LayoutDetach(ECSI_Workspace *workspace, ECSI_Node *group, ECSPanel panel)
{
    usz index = 0;

    while (group->panels[index] != panel)
    {
        index++;
    }

    // tidying removes a group that empties, and forgets it if it was maximized
    arrdel(group->panels, index);
    group->shown = arrlenu(group->panels) == 0 ? 0 : SDL_min(group->shown, arrlenu(group->panels) - 1);
    ECSI_LayoutTidy(workspace);

    if (workspace->focus == panel)
    {
        ECSI_LayoutChangeFocus(workspace, ECSI_LayoutFirstPanel(workspace->tree));
    }
}

/// @brief Finds the group a new panel joins in a workspace: the focused group, else the first one, or NULL if the workspace has no panels.
static ECSI_Node *ECSI_LayoutDefaultGroup(ECSI_Workspace *workspace)
{
    ECSI_Node *group = ECSI_LayoutFindGroup(workspace->tree, workspace->focus);
    return group != NULL ? group : ECSI_LayoutFindGroup(workspace->tree, ECSI_LayoutFirstPanel(workspace->tree));
}

/// @brief Converts a public zone to a drop zone.
static ECSI_Zone ECSI_LayoutZone(ECSZone zone)
{
    switch (zone)
    {
    case ECSZone_Left:
        return ECSI_Zone_Left;
    case ECSZone_Right:
        return ECSI_Zone_Right;
    case ECSZone_Top:
        return ECSI_Zone_Top;
    case ECSZone_Bottom:
        return ECSI_Zone_Bottom;
    default:
        return ECSI_Zone_Center;
    }
}

typedef struct ECSI_RecentSearch
{
    const char *typeName;
    ECSPanel best;
} ECSI_RecentSearch;

/// @brief Finds the most recently focused panel of a type in a tree.
static void ECSI_LayoutFindRecent(const ECSI_Node *node, ECSI_RecentSearch *search)
{
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
        ECSI_LayoutFindRecent(node->children[i], search);
    }
}

#pragma endregion Moving

#pragma region Hit Testing

typedef struct ECSI_GroupHit
{
    f32 x;
    f32 y;
    ECSI_Node *group;
} ECSI_GroupHit;

static void ECSI_LayoutFindGroupAt(ECSI_Node *group, void *userData)
{
    ECSI_GroupHit *hit = userData;

    if (ECSI_LayoutContains(hit->x, hit->y, group->x, group->y, group->width, group->height))
    {
        hit->group = group;
    }
}

typedef struct ECSI_DividerHit
{
    f32 x;
    f32 y;
    ECSI_Node *split;
    usz divider;
} ECSI_DividerHit;

/// @brief Finds the divider under a point.
static void ECSI_LayoutFindDivider(ECSI_Node *node, ECSI_DividerHit *hit)
{
    if (node == NULL || node->type == ECSI_NodeType_Group || hit->split != NULL)
    {
        return;
    }

    for (usz i = 0; i + 1 < arrlenu(node->children); i++)
    {
        ECSI_Node *child = node->children[i];
        bool over = node->vertical
                        ? ECSI_LayoutContains(hit->x, hit->y, node->x, child->y + child->height, node->width, OPENECS_DIVIDER_SIZE)
                        : ECSI_LayoutContains(hit->x, hit->y, child->x + child->width, node->y, OPENECS_DIVIDER_SIZE, node->height);

        if (over)
        {
            hit->split = node;
            hit->divider = i;
            return;
        }
    }

    for (usz i = 0; i < arrlenu(node->children); i++)
    {
        ECSI_LayoutFindDivider(node->children[i], hit);
    }
}

typedef struct ECSI_PanelHit
{
    f32 x;
    f32 y;
    ECSPanel panel;
} ECSI_PanelHit;

static void ECSI_LayoutFindPanel(ECSI_Node *group, void *userData)
{
    ECSI_PanelHit *hit = userData;

    if (arrlenu(group->panels) == 0)
    {
        return;
    }

    ECSPanel panel = group->panels[group->shown];

    if (ECSI_LayoutContains(hit->x, hit->y, panel->x, panel->y, panel->width, panel->height))
    {
        hit->panel = panel;
    }
}

typedef struct ECSI_NeighbourSearch
{
    ECSPanel from;
    i32 dx;
    i32 dy;
    ECSPanel best;
    f32 bestGap;
    f32 bestOverlap;
} ECSI_NeighbourSearch;

static void ECSI_LayoutFindNeighbourIn(ECSI_Node *group, void *userData)
{
    ECSI_NeighbourSearch *search = userData;

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

static void ECSI_LayoutWantsFrameIn(ECSI_Node *group, void *userData)
{
    bool *wants = userData;

    if (arrlenu(group->panels) > 0 && ECSI_PanelWantsFrame(group->panels[group->shown]))
    {
        *wants = true;
    }
}

#pragma endregion Hit Testing

#pragma region Saving

/// @brief Describes a node and its children for a session. A split holds its children as list items; a group holds its panels.
static SHUResult ECSI_LayoutSaveNode(const ECSI_Workspace *workspace, const ECSI_Node *node, ECSValue *retNode)
{
    ECSValue *field = NULL;
    ECSValue_SetTable(retNode);

    if (node->parent != NULL)
    {
        bool fixed = node->fixedSize > 0.0f;
        SHU_ReturnResult(ECSValue_TableSetField(retNode, fixed ? "size" : "share", &field));
        ECSValue_SetNumber(field, fixed ? node->fixedSize : node->share);
    }

    if (node->type == ECSI_NodeType_Split)
    {
        SHU_ReturnResult(ECSValue_TableSetField(retNode, "split", &field));
        SHU_ReturnResult(ECSValue_SetString(field, node->vertical ? "vertical" : "horizontal"));

        for (usz i = 0; i < arrlenu(node->children); i++)
        {
            SHU_ReturnResult(ECSValue_ListAddItem(retNode, &field));
            SHU_ReturnResult(ECSI_LayoutSaveNode(workspace, node->children[i], field));
        }

        return SHUResult_Ok;
    }

    ECSValue *panels = NULL;
    SHU_ReturnResult(ECSValue_TableSetField(retNode, "panels", &panels));
    ECSValue_SetTable(panels);

    for (usz i = 0; i < arrlenu(node->panels); i++)
    {
        SHU_ReturnResult(ECSValue_ListAddItem(panels, &field));
        SHU_ReturnResult(ECSI_PanelSave(node->panels[i], field));
    }

    SHU_ReturnResult(ECSValue_TableSetField(retNode, "shown", &field));
    ECSValue_SetInteger(field, (i64)node->shown + 1);

    if (node->locked)
    {
        SHU_ReturnResult(ECSValue_TableSetField(retNode, "locked", &field));
        ECSValue_SetBool(field, true);
    }

    if (workspace->maximized == node)
    {
        SHU_ReturnResult(ECSValue_TableSetField(retNode, "maximized", &field));
        ECSValue_SetBool(field, true);
    }

    return SHUResult_Ok;
}

/// @brief Adds every panel of a tree to an stb_ds array.
static void ECSI_LayoutCollectPanels(const ECSI_Node *node, ECSPanel **panels)
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
        ECSI_LayoutCollectPanels(node->children[i], panels);
    }
}

#pragma endregion Saving

#pragma endregion Source Only

void ECSI_LayoutSetForget(ECSI_LayoutForgetFunction function)
{
    LAYOUT.Forget = function;
}

void ECSI_LayoutTerminate(void)
{
    for (usz i = 0; i < arrlenu(LAYOUT.workspaces); i++)
    {
        if (LAYOUT.workspaces[i].tree != NULL)
        {
            ECSI_LayoutNodeDestroy(&LAYOUT.workspaces[i].tree);
        }

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

SHUResult ECSI_LayoutSplitCreate(ECSI_Node **retNode, bool vertical)
{
    SDL_assert(retNode != NULL);

    *retNode = SDL_calloc(1, sizeof(ECSI_Node));

    if (*retNode == NULL)
    {
        return SHUResult_ErrAllocation;
    }

    (*retNode)->type = ECSI_NodeType_Split;
    (*retNode)->vertical = vertical;
    (*retNode)->share = 1.0f;
    return SHUResult_Ok;
}

SHUResult ECSI_LayoutGroupCreate(ECSI_Node **retNode)
{
    SDL_assert(retNode != NULL);

    *retNode = SDL_calloc(1, sizeof(ECSI_Node));

    if (*retNode == NULL)
    {
        return SHUResult_ErrAllocation;
    }

    (*retNode)->type = ECSI_NodeType_Group;
    (*retNode)->share = 1.0f;
    return SHUResult_Ok;
}

void ECSI_LayoutSplitAdd(ECSI_Node *split, ECSI_Node *child, f32 fixedSize, f32 share)
{
    SDL_assert(split != NULL);
    SDL_assert(child != NULL);
    SDL_assert(split->type == ECSI_NodeType_Split);

    child->parent = split;
    child->fixedSize = SDL_max(0.0f, fixedSize);
    child->share = share > 0.0f ? share : 1.0f;
    arrput(split->children, child);
}

void ECSI_LayoutGroupAdd(ECSI_Node *group, ECSPanel panel)
{
    SDL_assert(group != NULL);
    SDL_assert(panel != NULL);
    SDL_assert(group->type == ECSI_NodeType_Group);

    arrput(group->panels, panel);
    ECSI_PanelEmit("ecs.panel_opened", panel);
}

void ECSI_LayoutGroupShow(ECSI_Node *group, usz index)
{
    SDL_assert(group != NULL);
    SDL_assert(group->type == ECSI_NodeType_Group);

    if (index < arrlenu(group->panels))
    {
        group->shown = index;
        LAYOUT.frameNeeded = true;
    }
}

void ECSI_LayoutGroupSetLocked(ECSI_Node *group, bool locked)
{
    SDL_assert(group != NULL);
    SDL_assert(group->type == ECSI_NodeType_Group);

    group->locked = locked;
}

void ECSI_LayoutNodeDestroy(ECSI_Node **node)
{
    SDL_assert(node != NULL);
    SDL_assert(*node != NULL);

    ECSI_Node *target = *node;

    for (usz i = 0; i < arrlenu(target->children); i++)
    {
        ECSI_LayoutNodeDestroy(&target->children[i]);
    }

    for (usz i = 0; i < arrlenu(target->panels); i++)
    {
        ECSI_PanelDestroy(&target->panels[i]);
    }

    arrfree(target->children);
    arrfree(target->panels);
    SDL_free(target);
    *node = NULL;
}

SHUResult ECSI_LayoutWorkspaceAdd(const char *name, ECSI_Node *tree, ECSPanel focus, ECSI_Node *maximized)
{
    SDL_assert(name != NULL);

    char *copy = SDL_strdup(name);

    if (copy == NULL)
    {
        return SHUResult_ErrAllocation;
    }

    ECSI_Workspace workspace = {
        .name = copy,
        .tree = tree,
        .maximized = maximized,
        .focus = focus != NULL ? focus : ECSI_LayoutFirstPanel(tree),
    };

    arrput(LAYOUT.workspaces, workspace);

    LAYOUT.frameNeeded = true;
    return SHUResult_Ok;
}

void ECSI_LayoutWorkspaceSwitch(usz index)
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
        ECSI_PanelPostEvent(old, &event);
    }

    if (focus != NULL)
    {
        event.type = ECSPanelEventType_Focused;
        ECSI_PanelPostEvent(focus, &event);
    }

    LAYOUT.current = index;
    ECSI_LayoutEmitFocus(focus);

    ECSValue *value = NULL;
    ECSValue *field = NULL;

    if (ECSValue_Create(&value) == SHUResult_Ok && ECSValue_TableSetField(value, "workspace", &field) == SHUResult_Ok)
    {
        ECSValue_SetInteger(field, (i64)index + 1);
        ECSI_EventsEmitCore("ecs.workspace_switched", value);
    }

    ECSValue_Destroy(&value);
    ECSI_LayoutForget();
    LAYOUT.frameNeeded = true;
}

usz ECSI_LayoutGetCurrentWorkspace(void)
{
    return LAYOUT.current;
}

void ECSI_LayoutRequestFrame(void)
{
    LAYOUT.frameNeeded = true;
}

bool ECSI_LayoutWantsFrame(void)
{
    bool wants = LAYOUT.frameNeeded;
    ECSI_LayoutForEachGroup(ECSI_LayoutWantsFrameIn, &wants);
    return wants;
}

void ECSI_LayoutUpdate(f32 width, f32 height)
{
    // requests made while the frame is drawn, such as a tab row that scrolls, get the next frame
    LAYOUT.frameNeeded = false;
    ECSI_Workspace *workspace = ECSI_LayoutCurrent();

    if (workspace == NULL)
    {
        return;
    }

    if (workspace->maximized != NULL)
    {
        ECSI_LayoutPlace(workspace->maximized, 0.0f, 0.0f, width, height);
    }
    else if (workspace->tree != NULL)
    {
        ECSI_LayoutPlace(workspace->tree, 0.0f, 0.0f, width, height);
    }

    // panels of other workspaces are hidden; in this one, a maximized group hides the others
    for (usz i = 0; i < arrlenu(LAYOUT.workspaces); i++)
    {
        ECSI_Node *only = LAYOUT.workspaces[i].maximized;
        ECSI_LayoutForEachGroupIn(LAYOUT.workspaces[i].tree, ECSI_LayoutTellVisible, &LAYOUT.workspaces[i] == workspace ? &only : NULL);
    }
}

void ECSI_LayoutForEachGroup(ECSI_LayoutGroupFunction function, void *userData)
{
    ECSI_Workspace *workspace = ECSI_LayoutCurrent();

    if (workspace == NULL)
    {
        return;
    }

    if (workspace->maximized != NULL)
    {
        function(workspace->maximized, userData);
    }
    else
    {
        ECSI_LayoutForEachGroupIn(workspace->tree, function, userData);
    }
}

ECSI_Node *ECSI_LayoutGroupOf(ECSPanel panel)
{
    ECSI_Workspace *workspace = ECSI_LayoutCurrent();
    return workspace == NULL || panel == NULL ? NULL : ECSI_LayoutFindGroup(workspace->tree, panel);
}

ECSI_Node *ECSI_LayoutGetTree(void)
{
    ECSI_Workspace *workspace = ECSI_LayoutCurrent();
    return workspace == NULL ? NULL : workspace->tree;
}

ECSI_Node *ECSI_LayoutGroupAt(f32 x, f32 y)
{
    ECSI_GroupHit hit = {x, y, NULL};
    ECSI_LayoutForEachGroup(ECSI_LayoutFindGroupAt, &hit);
    return hit.group;
}

bool ECSI_LayoutDividerAt(f32 x, f32 y, ECSI_Node **retSplit, usz *retDivider)
{
    SDL_assert(retSplit != NULL);
    SDL_assert(retDivider != NULL);

    ECSI_Workspace *workspace = ECSI_LayoutCurrent();
    ECSI_DividerHit hit = {x, y, NULL, 0};
    ECSI_LayoutFindDivider(workspace == NULL || workspace->maximized != NULL ? NULL : workspace->tree, &hit);
    *retSplit = hit.split;
    *retDivider = hit.divider;
    return hit.split != NULL;
}

void ECSI_LayoutMoveDivider(ECSI_Node *split, usz divider, f32 x, f32 y)
{
    SDL_assert(split != NULL);
    SDL_assert(divider + 1 < arrlenu(split->children));

    ECSI_Node *first = split->children[divider];
    ECSI_Node *second = split->children[divider + 1];

    f32 start = split->vertical ? first->y : first->x;
    f32 end = split->vertical ? second->y + second->height : second->x + second->width;
    f32 room = end - start - OPENECS_DIVIDER_SIZE;

    if (room < OPENECS_MIN_CHILD_SIZE * 2.0f)
    {
        return;
    }

    f32 position = (split->vertical ? y : x) - start - OPENECS_DIVIDER_SIZE / 2.0f;
    f32 firstSize = SDL_min(SDL_max(position, OPENECS_MIN_CHILD_SIZE), room - OPENECS_MIN_CHILD_SIZE);
    f32 secondSize = room - firstSize;

    // shares are relative, so every shared child gets its current size as its share
    for (usz i = 0; i < arrlenu(split->children); i++)
    {
        ECSI_Node *child = split->children[i];

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

void ECSI_LayoutDrop(ECSPanel panel, bool group, const ECSI_Drop *drop)
{
    SDL_assert(drop != NULL);

    ECSI_Workspace *workspace = ECSI_LayoutCurrent();
    ECSI_Node *source = workspace == NULL ? NULL : ECSI_LayoutFindGroup(workspace->tree, panel);

    if (source == NULL)
    {
        return;
    }

    if (group)
    {
        ECSI_LayoutMoveGroup(workspace, source, drop, true);
    }
    else
    {
        ECSI_LayoutMove(workspace, panel, drop, true);
    }
}

ECSPanel ECSI_LayoutPanelAt(f32 x, f32 y)
{
    ECSI_PanelHit hit = {x, y, NULL};
    ECSI_LayoutForEachGroup(ECSI_LayoutFindPanel, &hit);
    return hit.panel;
}

ECSPanel ECSI_LayoutGetFocus(void)
{
    ECSI_Workspace *workspace = ECSI_LayoutCurrent();
    return workspace == NULL ? NULL : workspace->focus;
}

void ECSI_LayoutSetFocus(ECSPanel panel)
{
    ECSI_Workspace *workspace = ECSI_LayoutCurrent();

    if (workspace != NULL)
    {
        ECSI_LayoutChangeFocus(workspace, panel);
    }
}

bool ECSI_LayoutHasPanel(ECSPanel panel)
{
    ECSI_Workspace *workspace = NULL;
    ECSI_Node *group = NULL;
    return ECSI_LayoutLocate(panel, &workspace, &group);
}

ECSPanel ECSI_LayoutFindNeighbour(i32 dx, i32 dy)
{
    ECSI_NeighbourSearch search = {ECSI_LayoutGetFocus(), dx, dy, NULL, 0.0f, 0.0f};

    if (search.from == NULL)
    {
        return NULL;
    }

    ECSI_LayoutForEachGroup(ECSI_LayoutFindNeighbourIn, &search);
    return search.best;
}

void ECSI_LayoutMoveFocus(i32 dx, i32 dy)
{
    ECSI_Workspace *workspace = ECSI_LayoutCurrent();
    ECSPanel neighbour = ECSI_LayoutFindNeighbour(dx, dy);

    if (workspace == NULL || workspace->focus == NULL)
    {
        return;
    }

    ECSI_Drop drop = {.zone = dx < 0 ? ECSI_Zone_WindowLeft : dx > 0 ? ECSI_Zone_WindowRight
                                                          : dy < 0   ? ECSI_Zone_WindowTop
                                                                     : ECSI_Zone_WindowBottom};

    if (neighbour != NULL)
    {
        drop = (ECSI_Drop){.zone = ECSI_Zone_Center, .group = ECSI_LayoutFindGroup(workspace->tree, neighbour)};
    }

    ECSI_LayoutMove(workspace, workspace->focus, &drop, true);
}

ECSPanel ECSI_LayoutNextTab(void)
{
    ECSI_Workspace *workspace = ECSI_LayoutCurrent();
    ECSI_Node *group = workspace == NULL ? NULL : ECSI_LayoutFindGroup(workspace->tree, workspace->focus);

    if (group == NULL || arrlenu(group->panels) < 2)
    {
        return NULL;
    }

    group->shown = (group->shown + 1) % arrlenu(group->panels);
    LAYOUT.frameNeeded = true;
    return group->panels[group->shown];
}

bool ECSI_LayoutIsLocked(ECSPanel panel)
{
    ECSI_Workspace *workspace = ECSI_LayoutCurrent();
    ECSI_Node *group = workspace == NULL || panel == NULL ? NULL : ECSI_LayoutFindGroup(workspace->tree, panel);
    return group != NULL && group->locked;
}

bool ECSI_LayoutIsMaximized(ECSPanel panel)
{
    ECSI_Workspace *workspace = ECSI_LayoutCurrent();
    return workspace != NULL && panel != NULL && workspace->maximized != NULL && workspace->maximized == ECSI_LayoutFindGroup(workspace->tree, panel);
}

const ECSPanel *ECSI_LayoutGetGroup(ECSPanel panel, usz *retCount, usz *retShown)
{
    SDL_assert(retCount != NULL);
    SDL_assert(retShown != NULL);

    ECSI_Workspace *workspace = ECSI_LayoutCurrent();
    ECSI_Node *group = workspace == NULL || panel == NULL ? NULL : ECSI_LayoutFindGroup(workspace->tree, panel);
    *retCount = group == NULL ? 0 : arrlenu(group->panels);
    *retShown = group == NULL ? 0 : group->shown;
    return group == NULL ? NULL : group->panels;
}

void ECSI_LayoutShowTab(ECSPanel panel)
{
    ECSI_Workspace *workspace = ECSI_LayoutCurrent();
    ECSI_Node *group = workspace == NULL || panel == NULL ? NULL : ECSI_LayoutFindGroup(workspace->tree, panel);

    for (usz i = 0; group != NULL && i < arrlenu(group->panels); i++)
    {
        if (group->panels[i] == panel)
        {
            group->shown = i;
            ECSI_LayoutChangeFocus(workspace, panel);
            LAYOUT.frameNeeded = true;
        }
    }
}

bool ECSI_LayoutCanReopen(void)
{
    return arrlenu(LAYOUT.closed) > 0;
}

void ECSI_LayoutToggleLock(void)
{
    ECSI_Workspace *workspace = ECSI_LayoutCurrent();
    ECSI_Node *group = workspace == NULL ? NULL : ECSI_LayoutFindGroup(workspace->tree, workspace->focus);

    if (group != NULL)
    {
        group->locked = !group->locked;
        ECSI_LayoutForget();
        LAYOUT.frameNeeded = true;
        ECSI_LayoutEmitLayoutChanged();
        SDL_Log("The group of '%s' is %s.", workspace->focus->title, group->locked ? "locked" : "unlocked");
    }
}

void ECSI_LayoutToggleMaximize(void)
{
    ECSI_Workspace *workspace = ECSI_LayoutCurrent();
    ECSI_Node *group = workspace == NULL ? NULL : ECSI_LayoutFindGroup(workspace->tree, workspace->focus);

    if (group == NULL)
    {
        return;
    }

    workspace->maximized = workspace->maximized == group ? NULL : group;
    ECSI_LayoutForget();
    LAYOUT.frameNeeded = true;
    ECSI_LayoutEmitLayoutChanged();
}

void ECSI_LayoutClosePanel(ECSPanel panel)
{
    ECSI_Workspace *workspace = NULL;
    ECSI_Node *group = NULL;

    if (!ECSI_LayoutLocate(panel, &workspace, &group))
    {
        return;
    }

    usz index = 0;

    while (group->panels[index] != panel)
    {
        index++;
    }

    // ecs.reopen opens it again next to a panel that stays in its group
    ECSI_ClosedPanel closed = {.neighbour = arrlenu(group->panels) > 1 ? ECSPanel_GetId(group->panels[index == 0 ? 1 : 0]) : 0};

    if (ECSValue_Create(&closed.saved) == SHUResult_Ok && ECSI_PanelSave(panel, closed.saved) == SHUResult_Ok)
    {
        if (arrlenu(LAYOUT.closed) == OPENECS_REOPEN_LIMIT)
        {
            ECSValue_Destroy(&LAYOUT.closed[0].saved);
            arrdel(LAYOUT.closed, 0);
        }

        arrput(LAYOUT.closed, closed);
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
        ECSI_LayoutChangeFocus(workspace, emptied ? NULL : group->panels[group->shown]);
    }

    ECSI_PanelClose(&panel);
    ECSI_LayoutTidy(workspace);

    if (focused && emptied)
    {
        ECSI_LayoutChangeFocus(workspace, ECSI_LayoutFirstPanel(workspace->tree));
    }
}

void ECSI_LayoutReopen(void)
{
    ECSI_Workspace *workspace = ECSI_LayoutCurrent();

    if (workspace == NULL || arrlenu(LAYOUT.closed) == 0)
    {
        SDL_Log("No closed panel to reopen.");
        return;
    }

    ECSI_ClosedPanel closed = arrpop(LAYOUT.closed);

    // the panel returns next to its neighbour, wherever that is now, or else to the focused group
    ECSI_Node *group = NULL;
    ECSPanel neighbour = closed.neighbour == 0 ? NULL : ECSLayout_FindPanel(closed.neighbour);

    if (neighbour == NULL || !ECSI_LayoutLocate(neighbour, &workspace, &group))
    {
        workspace = ECSI_LayoutCurrent();
        group = ECSI_LayoutDefaultGroup(workspace);
    }

    i64 version = ECSValue_GetInteger(ECSValue_GetTableField(closed.saved, "state_version"), 0);
    const ECSValue *state = ECSValue_GetTableField(closed.saved, "state");
    ECSPanel panel = NULL;

    if (ECSI_PanelCreate(&panel, ECSValue_GetString(ECSValue_GetTableField(closed.saved, "type"), ""), state, version >= 0 && version <= SDL_MAX_UINT32 ? (u32)version : 0) ||
        ECSI_LayoutAdd(workspace, group, panel, ECSI_Zone_Center))
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Out of memory while reopening a panel.");

        if (panel != NULL)
        {
            ECSI_PanelDestroy(&panel);
        }
    }
    else
    {
        ECSI_PanelEmit("ecs.panel_opened", panel);
        ECSI_LayoutWorkspaceSwitch((usz)(workspace - LAYOUT.workspaces));
    }

    ECSValue_Destroy(&closed.saved);
}

void ECSI_LayoutMoveToWorkspace(usz index)
{
    ECSI_Workspace *workspace = ECSI_LayoutCurrent();
    ECSPanel panel = workspace == NULL ? NULL : workspace->focus;
    ECSI_Node *group = panel == NULL ? NULL : ECSI_LayoutFindGroup(workspace->tree, panel);

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
    ECSI_Workspace *target = &LAYOUT.workspaces[index];
    ECSI_Node *targetGroup = ECSI_LayoutDefaultGroup(target);
    ECSI_LayoutDetach(workspace, group, panel);

    if (ECSI_LayoutAdd(target, targetGroup, panel, ECSI_Zone_Center))
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Out of memory while moving '%s'; it is closed.", panel->title);
        ECSI_PanelClose(&panel);
    }
}

ECSPanel *ECSI_LayoutGetPanels(void)
{
    ECSPanel *panels = NULL;

    for (usz i = 0; i < arrlenu(LAYOUT.workspaces); i++)
    {
        ECSI_LayoutCollectPanels(LAYOUT.workspaces[i].tree, &panels);
    }

    return panels;
}

SHUResult ECSI_LayoutSave(ECSValue *retWorkspaces, usz *retCurrent)
{
    SDL_assert(retWorkspaces != NULL);
    SDL_assert(retCurrent != NULL);

    ECSValue_SetTable(retWorkspaces);
    *retCurrent = LAYOUT.current;

    for (usz i = 0; i < arrlenu(LAYOUT.workspaces); i++)
    {
        const ECSI_Workspace *workspace = &LAYOUT.workspaces[i];
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

        SHU_ReturnResult(ECSValue_TableSetField(saved, "windows", &field));
        ECSValue_SetTable(field);

        if (workspace->tree != NULL)
        {
            SHU_ReturnResult(ECSValue_ListAddItem(field, &field));
            SHU_ReturnResult(ECSI_LayoutSaveNode(workspace, workspace->tree, field));
        }
    }

    return SHUResult_Ok;
}

SHUResult ECSLayout_Open(ECSPlugin plugin, ECSPanel *retPanel, const char *type, const ECSValue *state, ECSPanel target, ECSZone zone)
{
    SDL_assert(plugin != NULL);
    SDL_assert(retPanel != NULL);
    SDL_assert(type != NULL);

    ECSI_Workspace *workspace = ECSI_LayoutCurrent();
    ECSI_Node *group = NULL;

    if (workspace == NULL)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' opens a panel, but there is no workspace.", ECSI_PluginGetName(plugin));
        return SHUResult_ErrNotFound;
    }

    if (target != NULL)
    {
        group = ECSI_LayoutFindGroup(workspace->tree, target);

        if (group == NULL)
        {
            return SHUResult_ErrNotFound;
        }
    }
    else if (workspace->tree != NULL)
    {
        // the group of the most recently focused panel of the type, else the focused group, else the first
        ECSI_RecentSearch search = {.typeName = type, .best = NULL};
        ECSI_LayoutFindRecent(workspace->tree, &search);
        group = ECSI_LayoutFindGroup(workspace->tree, search.best != NULL ? search.best : workspace->focus);
        group = group != NULL ? group : ECSI_LayoutFindGroup(workspace->tree, ECSI_LayoutFirstPanel(workspace->tree));
    }

    ECSPanel panel = NULL;
    SHU_ReturnResult(ECSI_PanelCreate(&panel, type, state, state == NULL ? 0 : ECSI_PanelsGetStateVersion(type)));
    SHU_ReturnResult(ECSI_LayoutAdd(workspace, group, panel, ECSI_LayoutZone(zone)), ECSI_PanelDestroy(&panel););
    ECSI_PanelEmit("ecs.panel_opened", panel);
    *retPanel = panel;
    return SHUResult_Ok;
}

SHUResult ECSLayout_Move(ECSPanel panel, ECSPanel target, ECSZone zone)
{
    SDL_assert(panel != NULL);
    SDL_assert(target != NULL);

    ECSI_Workspace *workspace = NULL;
    ECSI_Workspace *targetWorkspace = NULL;
    ECSI_Node *source = NULL;
    ECSI_Node *group = NULL;

    if (!ECSI_LayoutLocate(panel, &workspace, &source) || !ECSI_LayoutLocate(target, &targetWorkspace, &group))
    {
        return SHUResult_ErrNotFound;
    }

    ECSI_Drop drop = {.zone = ECSI_LayoutZone(zone), .group = group};

    if (workspace == targetWorkspace)
    {
        ECSI_LayoutMove(workspace, panel, &drop, false);
        return SHUResult_Ok;
    }

    // to another workspace: the panel leaves its group, then joins the target's group, or goes beside it
    ECSI_LayoutDetach(workspace, source, panel);
    return ECSI_LayoutAdd(targetWorkspace, group, panel, drop.zone);
}

bool ECSLayout_Close(ECSPanel panel)
{
    SDL_assert(panel != NULL);

    if (!ECSI_LayoutHasPanel(panel) || !ECSI_PanelsConfirmClose(&panel, 1, false))
    {
        return false;
    }

    ECSI_LayoutClosePanel(panel);
    return true;
}

void ECSLayout_Focus(ECSPanel panel)
{
    SDL_assert(panel != NULL);

    ECSI_Workspace *workspace = NULL;
    ECSI_Node *group = NULL;

    if (!ECSI_LayoutLocate(panel, &workspace, &group))
    {
        return;
    }

    // the workspace and the tab are shown first, so the panel is focused where the user sees it
    ECSI_LayoutWorkspaceSwitch((usz)(workspace - LAYOUT.workspaces));

    for (usz i = 0; i < arrlenu(group->panels); i++)
    {
        if (group->panels[i] == panel)
        {
            ECSI_LayoutGroupShow(group, i);
        }
    }

    ECSI_LayoutChangeFocus(workspace, panel);
}

ECSPanel ECSLayout_FindPanel(u32 id)
{
    // every panel is in a group, so a search of every workspace finds it
    ECSPanel *panels = ECSI_LayoutGetPanels();
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
    return ECSI_LayoutGetFocus();
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
        ECSI_LayoutWorkspaceSwitch(number - 1);
    }
}
