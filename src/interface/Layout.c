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
typedef struct ECSIWorkspace
{
    char *name;
    ECSINode *tree;      // NULL when the workspace has no panels
    ECSINode *maximized; // group that fills the window, or NULL
    ECSPanel focus;
} ECSIWorkspace;

/// @brief A closed panel that ecs.reopen can open again.
typedef struct ECSIClosedPanel
{
    ECSValue *saved; // its type, state and state_version, as ECSIPanel_Save writes them
    u32 neighbour;   // id of a panel that stayed in its group, or 0
} ECSIClosedPanel;

static struct
{
    ECSIWorkspace *workspaces; // stb_ds array
    usz current;
    ECSIClosedPanel *closed; // stb_ds array of the panels ecs.reopen can open again, the last closed last
    bool frameNeeded;
    ECSILayoutForgetFunction Forget; // the window's, or NULL
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
        f32 top = arrlenu(node->panels) >= 2 ? OPENECS_TAB_ROW_HEIGHT : 0.0f;

        if (arrlenu(node->panels) > 0)
        {
            ECSIPanel_SetRect(node->panels[node->shown], x, y + top, width, SDL_max(0.0f, height - top));
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

        position += size + OPENECS_DIVIDER_SIZE;
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

/// @brief Finds the workspace and group that hold a panel, in any workspace.
/// @return false if the panel is not in the layout.
static bool ECSILayout_Locate(ECSPanel panel, ECSIWorkspace **retWorkspace, ECSINode **retGroup)
{
    for (usz i = 0; panel != NULL && i < arrlenu(LAYOUT.workspaces); i++)
    {
        ECSINode *group = ECSILayout_FindGroup(LAYOUT.workspaces[i].tree, panel);

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

        ECSIEvents_EmitCore("ecs.focus_changed", value);
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

/// @brief Frees one node, without its children or panels.
static void ECSILayout_FreeNode(ECSINode *node)
{
    arrfree(node->children);
    arrfree(node->panels);
    SDL_free(node);
}

/// @brief Tidies a subtree after an operation: frees empty groups and splits, merges a split into a parent split of the same direction, and replaces a split that has one child by that child.
/// @return The node that takes the subtree's place, or NULL if nothing is left.
static ECSINode *ECSILayout_TidyNode(ECSIWorkspace *workspace, ECSINode *node)
{
    if (node->type == ECSINodeType_Group)
    {
        if (arrlenu(node->panels) > 0)
        {
            return node;
        }

        if (workspace->maximized == node)
        {
            workspace->maximized = NULL;
        }

        ECSILayout_FreeNode(node);
        return NULL;
    }

    ECSINode **children = NULL;

    for (usz i = 0; i < arrlenu(node->children); i++)
    {
        ECSINode *child = ECSILayout_TidyNode(workspace, node->children[i]);

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

/// @brief Emits ecs.layout_changed, which carries nothing.
static void ECSILayout_EmitLayoutChanged(void)
{
    ECSIEvents_EmitCore("ecs.layout_changed", NULL);
}

/// @brief Lets the window forget the nodes it points to, because nodes may have been freed or hidden.
static void ECSILayout_Forget(void)
{
    if (LAYOUT.Forget != NULL)
    {
        LAYOUT.Forget();
    }
}

/// @brief Tidies a workspace's tree after an operation. Nodes may be freed, so the window forgets the nodes it points to.
static void ECSILayout_Tidy(ECSIWorkspace *workspace)
{
    if (workspace->tree != NULL)
    {
        workspace->tree = ECSILayout_TidyNode(workspace, workspace->tree);
    }

    if (workspace->tree != NULL)
    {
        workspace->tree->parent = NULL;
    }

    ECSILayout_Forget();
    LAYOUT.frameNeeded = true;
    ECSILayout_EmitLayoutChanged();
}

#pragma region Moving

/// @brief Puts a node in another node's place in the tree. The other node keeps its children and sizes.
static void ECSILayout_ReplaceNode(ECSIWorkspace *workspace, ECSINode *old, ECSINode *node)
{
    ECSINode *parent = old->parent;
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
static void ECSILayout_Wrap(ECSIWorkspace *workspace, ECSINode *split, ECSINode *node, ECSINode *added, bool first, f32 nodeShare, f32 addedShare)
{
    ECSILayout_ReplaceNode(workspace, node, split);
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
static void ECSILayout_Move(ECSIWorkspace *workspace, ECSPanel panel, const ECSIDrop *drop, bool user)
{
    ECSINode *source = ECSILayout_FindGroup(workspace->tree, panel);
    ECSINode *target = drop->group;
    bool side = drop->zone >= ECSIZone_Left && drop->zone <= ECSIZone_Bottom;
    bool edge = drop->zone >= ECSIZone_WindowLeft;

    // a panel cannot join its own group again, or split away from a group that holds only itself; locked panels stay
    if (source == NULL || drop->zone == ECSIZone_None || (user && source->locked) || (source == target && (drop->zone == ECSIZone_Center || (side && arrlenu(source->panels) == 1))))
    {
        return;
    }

    if (user && target != NULL && target->locked && (drop->zone == ECSIZone_Center || drop->zone == ECSIZone_Tabs))
    {
        return;
    }

    // the new nodes are made first, so a failed allocation changes nothing
    ECSINode *group = NULL;
    ECSINode *split = NULL;

    if (side || edge)
    {
        bool vertical = drop->zone == ECSIZone_Top || drop->zone == ECSIZone_Bottom || drop->zone == ECSIZone_WindowTop || drop->zone == ECSIZone_WindowBottom;

        if (ECSILayout_GroupCreate(&group) || ECSILayout_SplitCreate(&split, vertical))
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
    bool first = drop->zone == ECSIZone_Left || drop->zone == ECSIZone_Top || drop->zone == ECSIZone_WindowLeft || drop->zone == ECSIZone_WindowTop;

    if (drop->zone == ECSIZone_Center || drop->zone == ECSIZone_Tabs)
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
    else if (side)
    {
        // the target and the panel share the target's place
        arrput(group->panels, panel);
        ECSILayout_Wrap(workspace, split, target, group, first, 1.0f, 1.0f);
    }
    else
    {
        // the panel docks along the whole edge, with a quarter of the window
        arrput(group->panels, panel);
        ECSILayout_Wrap(workspace, split, workspace->tree, group, first, 3.0f, 1.0f);
    }

    workspace->maximized = NULL;
    ECSILayout_ChangeFocus(workspace, panel);
    ECSILayout_Tidy(workspace);
}

/// @brief Moves a whole group to a drop place, then tidies the tree. The group's shown panel gets the focus.
/// @param user true when the user moves the group; locks stop the user but not code.
static void ECSILayout_MoveGroup(ECSIWorkspace *workspace, ECSINode *group, const ECSIDrop *drop, bool user)
{
    ECSINode *target = drop->group;
    bool side = drop->zone >= ECSIZone_Left && drop->zone <= ECSIZone_Bottom;
    bool edge = drop->zone >= ECSIZone_WindowLeft;
    bool join = drop->zone == ECSIZone_Center || drop->zone == ECSIZone_Tabs;

    if (drop->zone == ECSIZone_None || target == group || (edge && workspace->tree == group) || (user && (group->locked || (join && target->locked))))
    {
        return;
    }

    ECSPanel shown = group->panels[group->shown];

    if (join)
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
        bool vertical = drop->zone == ECSIZone_Top || drop->zone == ECSIZone_Bottom || drop->zone == ECSIZone_WindowTop || drop->zone == ECSIZone_WindowBottom;
        bool first = drop->zone == ECSIZone_Left || drop->zone == ECSIZone_Top || drop->zone == ECSIZone_WindowLeft || drop->zone == ECSIZone_WindowTop;
        ECSINode *split = NULL;

        if (ECSILayout_SplitCreate(&split, vertical))
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Out of memory while moving a group.");
            return;
        }

        // the group leaves its parent split, which tidying removes if one child is left
        ECSINode *parent = group->parent;

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
            ECSILayout_Wrap(workspace, split, target, group, first, 1.0f, 1.0f);
        }
        else
        {
            ECSILayout_Wrap(workspace, split, workspace->tree, group, first, 3.0f, 1.0f);
        }
    }

    workspace->maximized = NULL;
    ECSILayout_ChangeFocus(workspace, shown);
    ECSILayout_Tidy(workspace);
}

/// @brief Puts a panel into a group, or into a new group if the workspace has no panels, then a side zone moves it beside the group. The panel gets the focus.
/// @param group The group, or NULL when the workspace has no panels.
static SHUResult ECSILayout_Add(ECSIWorkspace *workspace, ECSINode *group, ECSPanel panel, ECSIZone zone)
{
    if (group == NULL)
    {
        SHU_ReturnResult(ECSILayout_GroupCreate(&group));
        workspace->tree = group;
    }

    arrput(group->panels, panel);
    group->shown = arrlenu(group->panels) - 1;
    ECSIDrop drop = {.zone = zone, .group = group};

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
        ECSILayout_ChangeFocus(workspace, ECSILayout_FirstPanel(workspace->tree));
    }
}

/// @brief Finds the group a new panel joins in a workspace: the focused group, else the first one, or NULL if the workspace has no panels.
static ECSINode *ECSILayout_DefaultGroup(ECSIWorkspace *workspace)
{
    ECSINode *group = ECSILayout_FindGroup(workspace->tree, workspace->focus);
    return group != NULL ? group : ECSILayout_FindGroup(workspace->tree, ECSILayout_FirstPanel(workspace->tree));
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
                        ? ECSILayout_Contains(hit->x, hit->y, node->x, child->y + child->height, node->width, OPENECS_DIVIDER_SIZE)
                        : ECSILayout_Contains(hit->x, hit->y, child->x + child->width, node->y, OPENECS_DIVIDER_SIZE, node->height);

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
static SHUResult ECSILayout_SaveNode(const ECSIWorkspace *workspace, const ECSINode *node, ECSValue *retNode)
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
            SHU_ReturnResult(ECSILayout_SaveNode(workspace, node->children[i], field));
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

    if (workspace->maximized == node)
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

#pragma endregion Source Only

void ECSILayout_SetForget(ECSILayoutForgetFunction function)
{
    LAYOUT.Forget = function;
}

void ECSILayout_Terminate(void)
{
    for (usz i = 0; i < arrlenu(LAYOUT.workspaces); i++)
    {
        if (LAYOUT.workspaces[i].tree != NULL)
        {
            ECSILayout_NodeDestroy(&LAYOUT.workspaces[i].tree);
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
    ECSIPanel_Emit("ecs.panel_opened", panel);
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

SHUResult ECSILayout_WorkspaceAdd(const char *name, ECSINode *tree, ECSPanel focus, ECSINode *maximized)
{
    SDL_assert(name != NULL);

    char *copy = SDL_strdup(name);

    if (copy == NULL)
    {
        return SHUResult_ErrAllocation;
    }

    ECSIWorkspace workspace = {
        .name = copy,
        .tree = tree,
        .maximized = maximized,
        .focus = focus != NULL ? focus : ECSILayout_FirstPanel(tree),
    };

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
        ECSIEvents_EmitCore("ecs.workspace_switched", value);
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
    ECSILayout_ForEachGroup(ECSILayout_WantsFrameIn, &wants);
    return wants;
}

void ECSILayout_Update(f32 width, f32 height)
{
    // requests made while the frame is drawn, such as a tab row that scrolls, get the next frame
    LAYOUT.frameNeeded = false;
    ECSIWorkspace *workspace = ECSILayout_Current();

    if (workspace == NULL)
    {
        return;
    }

    if (workspace->maximized != NULL)
    {
        ECSILayout_Place(workspace->maximized, 0.0f, 0.0f, width, height);
    }
    else if (workspace->tree != NULL)
    {
        ECSILayout_Place(workspace->tree, 0.0f, 0.0f, width, height);
    }

    // panels of other workspaces are hidden; in this one, a maximized group hides the others
    for (usz i = 0; i < arrlenu(LAYOUT.workspaces); i++)
    {
        ECSINode *only = LAYOUT.workspaces[i].maximized;
        ECSILayout_ForEachGroupIn(LAYOUT.workspaces[i].tree, ECSILayout_TellVisible, &LAYOUT.workspaces[i] == workspace ? &only : NULL);
    }
}

void ECSILayout_ForEachGroup(ECSILayoutGroupFunction function, void *userData)
{
    ECSIWorkspace *workspace = ECSILayout_Current();

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
        ECSILayout_ForEachGroupIn(workspace->tree, function, userData);
    }
}

ECSINode *ECSILayout_GroupOf(ECSPanel panel)
{
    ECSIWorkspace *workspace = ECSILayout_Current();
    return workspace == NULL || panel == NULL ? NULL : ECSILayout_FindGroup(workspace->tree, panel);
}

ECSINode *ECSILayout_GetTree(void)
{
    ECSIWorkspace *workspace = ECSILayout_Current();
    return workspace == NULL ? NULL : workspace->tree;
}

ECSINode *ECSILayout_GroupAt(f32 x, f32 y)
{
    ECSIGroupHit hit = {x, y, NULL};
    ECSILayout_ForEachGroup(ECSILayout_FindGroupAt, &hit);
    return hit.group;
}

bool ECSILayout_DividerAt(f32 x, f32 y, ECSINode **retSplit, usz *retDivider)
{
    SDL_assert(retSplit != NULL);
    SDL_assert(retDivider != NULL);

    ECSIWorkspace *workspace = ECSILayout_Current();
    ECSIDividerHit hit = {x, y, NULL, 0};
    ECSILayout_FindDivider(workspace == NULL || workspace->maximized != NULL ? NULL : workspace->tree, &hit);
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
    ECSINode *source = workspace == NULL ? NULL : ECSILayout_FindGroup(workspace->tree, panel);

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

ECSPanel ECSILayout_PanelAt(f32 x, f32 y)
{
    ECSIPanelHit hit = {x, y, NULL};
    ECSILayout_ForEachGroup(ECSILayout_FindPanel, &hit);
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
    return ECSILayout_Locate(panel, &workspace, &group);
}

ECSPanel ECSILayout_FindNeighbour(i32 dx, i32 dy)
{
    ECSINeighbourSearch search = {ECSILayout_GetFocus(), dx, dy, NULL, 0.0f, 0.0f};

    if (search.from == NULL)
    {
        return NULL;
    }

    ECSILayout_ForEachGroup(ECSILayout_FindNeighbourIn, &search);
    return search.best;
}

void ECSILayout_MoveFocus(i32 dx, i32 dy)
{
    ECSIWorkspace *workspace = ECSILayout_Current();
    ECSPanel neighbour = ECSILayout_FindNeighbour(dx, dy);

    if (workspace == NULL || workspace->focus == NULL)
    {
        return;
    }

    ECSIDrop drop = {.zone = dx < 0 ? ECSIZone_WindowLeft : dx > 0 ? ECSIZone_WindowRight
                                                          : dy < 0   ? ECSIZone_WindowTop
                                                                     : ECSIZone_WindowBottom};

    if (neighbour != NULL)
    {
        drop = (ECSIDrop){.zone = ECSIZone_Center, .group = ECSILayout_FindGroup(workspace->tree, neighbour)};
    }

    ECSILayout_Move(workspace, workspace->focus, &drop, true);
}

ECSPanel ECSILayout_NextTab(void)
{
    ECSIWorkspace *workspace = ECSILayout_Current();
    ECSINode *group = workspace == NULL ? NULL : ECSILayout_FindGroup(workspace->tree, workspace->focus);

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
    ECSIWorkspace *workspace = ECSILayout_Current();
    ECSINode *group = workspace == NULL || panel == NULL ? NULL : ECSILayout_FindGroup(workspace->tree, panel);
    return group != NULL && group->locked;
}

bool ECSILayout_IsMaximized(ECSPanel panel)
{
    ECSIWorkspace *workspace = ECSILayout_Current();
    return workspace != NULL && panel != NULL && workspace->maximized != NULL && workspace->maximized == ECSILayout_FindGroup(workspace->tree, panel);
}

const ECSPanel *ECSILayout_GetGroup(ECSPanel panel, usz *retCount, usz *retShown)
{
    SDL_assert(retCount != NULL);
    SDL_assert(retShown != NULL);

    ECSIWorkspace *workspace = ECSILayout_Current();
    ECSINode *group = workspace == NULL || panel == NULL ? NULL : ECSILayout_FindGroup(workspace->tree, panel);
    *retCount = group == NULL ? 0 : arrlenu(group->panels);
    *retShown = group == NULL ? 0 : group->shown;
    return group == NULL ? NULL : group->panels;
}

void ECSILayout_ShowTab(ECSPanel panel)
{
    ECSIWorkspace *workspace = ECSILayout_Current();
    ECSINode *group = workspace == NULL || panel == NULL ? NULL : ECSILayout_FindGroup(workspace->tree, panel);

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
    ECSINode *group = workspace == NULL ? NULL : ECSILayout_FindGroup(workspace->tree, workspace->focus);

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
    ECSINode *group = workspace == NULL ? NULL : ECSILayout_FindGroup(workspace->tree, workspace->focus);

    if (group == NULL)
    {
        return;
    }

    workspace->maximized = workspace->maximized == group ? NULL : group;
    ECSILayout_Forget();
    LAYOUT.frameNeeded = true;
    ECSILayout_EmitLayoutChanged();
}

void ECSILayout_ClosePanel(ECSPanel panel)
{
    ECSIWorkspace *workspace = NULL;
    ECSINode *group = NULL;

    if (!ECSILayout_Locate(panel, &workspace, &group))
    {
        return;
    }

    usz index = 0;

    while (group->panels[index] != panel)
    {
        index++;
    }

    // ecs.reopen opens it again next to a panel that stays in its group
    ECSIClosedPanel closed = {.neighbour = arrlenu(group->panels) > 1 ? ECSPanel_GetId(group->panels[index == 0 ? 1 : 0]) : 0};

    if (ECSValue_Create(&closed.saved) == SHUResult_Ok && ECSIPanel_Save(panel, closed.saved) == SHUResult_Ok)
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
        ECSILayout_ChangeFocus(workspace, emptied ? NULL : group->panels[group->shown]);
    }

    ECSIPanel_Close(&panel);
    ECSILayout_Tidy(workspace);

    if (focused && emptied)
    {
        ECSILayout_ChangeFocus(workspace, ECSILayout_FirstPanel(workspace->tree));
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

    if (neighbour == NULL || !ECSILayout_Locate(neighbour, &workspace, &group))
    {
        workspace = ECSILayout_Current();
        group = ECSILayout_DefaultGroup(workspace);
    }

    i64 version = ECSValue_GetInteger(ECSValue_GetTableField(closed.saved, "state_version"), 0);
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
        ECSIPanel_Emit("ecs.panel_opened", panel);
        ECSILayout_WorkspaceSwitch((usz)(workspace - LAYOUT.workspaces));
    }

    ECSValue_Destroy(&closed.saved);
}

void ECSILayout_MoveToWorkspace(usz index)
{
    ECSIWorkspace *workspace = ECSILayout_Current();
    ECSPanel panel = workspace == NULL ? NULL : workspace->focus;
    ECSINode *group = panel == NULL ? NULL : ECSILayout_FindGroup(workspace->tree, panel);

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
        ECSILayout_CollectPanels(LAYOUT.workspaces[i].tree, &panels);
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

        SHU_ReturnResult(ECSValue_TableSetField(saved, "windows", &field));
        ECSValue_SetTable(field);

        if (workspace->tree != NULL)
        {
            SHU_ReturnResult(ECSValue_ListAddItem(field, &field));
            SHU_ReturnResult(ECSILayout_SaveNode(workspace, workspace->tree, field));
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
        group = ECSILayout_FindGroup(workspace->tree, target);

        if (group == NULL)
        {
            return SHUResult_ErrNotFound;
        }
    }
    else if (workspace->tree != NULL)
    {
        // the group of the most recently focused panel of the type, else the focused group, else the first
        ECSIRecentSearch search = {.typeName = type, .best = NULL};
        ECSILayout_FindRecent(workspace->tree, &search);
        group = ECSILayout_FindGroup(workspace->tree, search.best != NULL ? search.best : workspace->focus);
        group = group != NULL ? group : ECSILayout_FindGroup(workspace->tree, ECSILayout_FirstPanel(workspace->tree));
    }

    ECSPanel panel = NULL;
    SHU_ReturnResult(ECSIPanel_Create(&panel, type, state, state == NULL ? 0 : ECSIPanels_GetStateVersion(type)));
    SHU_ReturnResult(ECSILayout_Add(workspace, group, panel, ECSILayout_Zone(zone)), ECSIPanel_Destroy(&panel););
    ECSIPanel_Emit("ecs.panel_opened", panel);
    *retPanel = panel;
    return SHUResult_Ok;
}

SHUResult ECSLayout_Move(ECSPanel panel, ECSPanel target, ECSZone zone)
{
    SDL_assert(panel != NULL);
    SDL_assert(target != NULL);

    ECSIWorkspace *workspace = NULL;
    ECSIWorkspace *targetWorkspace = NULL;
    ECSINode *source = NULL;
    ECSINode *group = NULL;

    if (!ECSILayout_Locate(panel, &workspace, &source) || !ECSILayout_Locate(target, &targetWorkspace, &group))
    {
        return SHUResult_ErrNotFound;
    }

    ECSIDrop drop = {.zone = ECSILayout_Zone(zone), .group = group};

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
    SDL_assert(panel != NULL);

    if (!ECSILayout_HasPanel(panel) || !ECSIPanels_ConfirmClose(&panel, 1, false))
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

    if (!ECSILayout_Locate(panel, &workspace, &group))
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
