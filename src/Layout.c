#include "Layout.h"

#include "SDL3/SDL.h"
#include "SDL3_ttf/SDL_ttf.h"
#include "clay/clay.h"
#include "clay/claySDL3.h"
#include "stb/stbSDL3.h"

#pragma region Source Only

/// @brief Size of the OS window when it opens, in layout units.
#define OPENECS_WINDOW_WIDTH 1280
#define OPENECS_WINDOW_HEIGHT 800
/// @brief Size of the core's font when it is loaded, in layout units.
#define OPENECS_FONT_SIZE 14
/// @brief Gap between the children of a split; dragging it resizes them.
#define OPENECS_DIVIDER_SIZE 4.0f
/// @brief Height of a tab row.
#define OPENECS_TAB_ROW_HEIGHT 26.0f
/// @brief Distance from a panel's top edge within which its grip appears.
#define OPENECS_GRIP_ZONE 12.0f
/// @brief Size of a grip.
#define OPENECS_GRIP_WIDTH 48.0f
#define OPENECS_GRIP_HEIGHT 6.0f
/// @brief Distance the pointer moves from a press on a tab or grip before the panel is dragged.
#define OPENECS_DRAG_THRESHOLD 6.0f
/// @brief Distance from an edge of the OS window within which a dragged panel docks along that edge.
#define OPENECS_DOCK_EDGE 16.0f
/// @brief Deepest edge band of a panel in which a dragged panel splits it.
#define OPENECS_SPLIT_DEPTH 80.0f
/// @brief Width of the mark between tabs where a dragged panel is inserted.
#define OPENECS_TAB_GAP_WIDTH 3.0f
/// @brief Smallest size a divider drag leaves to a child.
#define OPENECS_MIN_CHILD_SIZE 32.0f

/// @brief Background colour; it shows through the gaps between panels.
#define OPENECS_COLOR_BACKGROUND 0x18, 0x19, 0x1C, 0xFF
#define OPENECS_COLOR_TAB_ROW ((Clay_Color){32, 34, 38, 255})
#define OPENECS_COLOR_TAB ((Clay_Color){40, 43, 48, 255})
#define OPENECS_COLOR_TAB_SHOWN ((Clay_Color){58, 62, 70, 255})
#define OPENECS_COLOR_TEXT ((Clay_Color){220, 222, 226, 255})
#define OPENECS_COLOR_TEXT_DIM ((Clay_Color){150, 154, 160, 255})
#define OPENECS_COLOR_ACCENT ((Clay_Color){76, 139, 245, 255})
#define OPENECS_COLOR_PLACEHOLDER ((Clay_Color){44, 30, 34, 255})
#define OPENECS_COLOR_OVERLAY ((Clay_Color){28, 30, 34, 245})
#define OPENECS_COLOR_DROP ((Clay_Color){76, 139, 245, 70})

/// @brief Type of a layout node.
typedef enum ECSI_NodeType
{
    ECSI_NodeType_Split = 0,
    ECSI_NodeType_Group,
} ECSI_NodeType;

struct ECSI_Node
{
    ECSI_NodeType type;
    ECSI_Node *parent;
    f32 fixedSize; // size along the parent split, in layout units; 0 to use the share
    f32 share;     // share of the parent split's remaining space
    f32 x;         // rectangle in layout units, computed
    f32 y;
    f32 width;
    f32 height;

    // split
    bool vertical; // children are stacked from top to bottom
    ECSI_Node **children; // stb_ds array

    // group
    ECSPanel *panels; // stb_ds array
    usz shown;        // index of the panel shown
};

/// @brief Where a moved panel lands.
typedef enum ECSI_Zone
{
    ECSI_Zone_None = 0,
    ECSI_Zone_Center, // group with the target
    ECSI_Zone_Tabs,   // insert between the target's tabs
    ECSI_Zone_Left,   // split the target toward a side
    ECSI_Zone_Right,
    ECSI_Zone_Top,
    ECSI_Zone_Bottom,
    ECSI_Zone_WindowLeft, // dock along a whole edge of the OS window
    ECSI_Zone_WindowRight,
    ECSI_Zone_WindowTop,
    ECSI_Zone_WindowBottom,
} ECSI_Zone;

/// @brief A place to drop a panel.
typedef struct ECSI_Drop
{
    ECSI_Zone zone;
    ECSI_Node *group; // the target group; NULL for the window's edges
    usz index;        // tabs: the gap between tabs, starting at 0
} ECSI_Drop;

/// @brief A named arrangement of panels.
typedef struct ECSI_Workspace
{
    char *name;
    ECSI_Node *tree;      // NULL when the workspace has no panels
    ECSI_Node *maximized; // group that fills the window, or NULL
    ECSPanel focus;
} ECSI_Workspace;

/// @brief A tab drawn in the last frame, so a click can find it.
typedef struct ECSI_TabRef
{
    ECSI_Node *group;
    usz index;
} ECSI_TabRef;

/// @brief Function called for each visible group.
typedef void (*ECSI_GroupFunction)(ECSI_Node *group, void *userData);

static struct
{
    SDL_Window *window;
    SDL_Renderer *renderer;
    TTF_Font *fonts[1];
    Clay_SDL3RendererData clayRenderer;
    Clay_Context *clay;
    void *clayMemory;
    f32 width;
    f32 height;

    ECSI_Workspace *workspaces; // stb_ds array
    usz current;

    bool frameNeeded;
    bool showPrefixKeys;
    ECSI_TabRef *tabs; // stb_ds array

    ECSI_Node *gripGroup;   // lone group whose grip is shown, or NULL
    ECSI_Node *dragSplit;   // split whose divider is dragged, or NULL
    usz dragDivider;        // the dragged divider follows this child

    ECSPanel dragPanel;     // panel pressed on its tab or grip, or NULL; it is dragged once the pointer moves far enough
    bool dragging;
    f32 dragStartX;
    f32 dragStartY;
    ECSI_Drop drop;         // where the dragged panel lands now
    SDL_FRect dropRect;     // the highlight of the drop place
} LAYOUT = {0};

/// @brief Width of the key column in the list of prefix keys.
#define OPENECS_PREFIX_KEY_COLUMN 104.0f

/// @brief The keys shown after the core prefix, as key and action. Mirrors the default of the setting ecs.prefix_keys.
static const char *const ECSI_PREFIX_KEY_LINES[][2] = {
    {"Arrows", "move focus"},
    {"Shift+Arrows", "move the panel"},
    {"Tab", "next tab"},
    {"1 to 9", "switch workspace"},
    {"M", "maximize or restore"},
    {"X", "close the panel"},
    {"Escape", "cancel"},
};

static ECSI_Workspace *ECSI_LayoutCurrent(void)
{
    return arrlenu(LAYOUT.workspaces) == 0 ? NULL : &LAYOUT.workspaces[LAYOUT.current];
}

static Clay_String ECSI_LayoutClayText(const char *text)
{
    return (Clay_String){.isStaticallyAllocated = false, .length = (int32_t)SDL_strlen(text), .chars = text};
}

static void ECSI_LayoutClayError(Clay_ErrorData error)
{
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Clay: %.*s", (int)error.errorText.length, error.errorText.chars);
}

/// @brief Measures text for Clay, with the core's font.
static Clay_Dimensions ECSI_LayoutMeasureText(Clay_StringSlice text, Clay_TextElementConfig *config, void *userData)
{
    (void)userData;

    TTF_Font *font = LAYOUT.fonts[config->fontId];
    int width = 0;
    int height = 0;

    TTF_SetFontSize(font, config->fontSize);
    TTF_GetStringSize(font, text.chars, (size_t)text.length, &width, &height);

    return (Clay_Dimensions){(f32)width, (f32)height};
}

/// @brief Reads the size of the OS window.
static void ECSI_LayoutReadSize(void)
{
    int width = 0;
    int height = 0;
    SDL_GetWindowSize(LAYOUT.window, &width, &height);

    LAYOUT.width = (f32)width;
    LAYOUT.height = (f32)height;
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

/// @brief Calls a function for every visible group of the current workspace.
static void ECSI_LayoutForEachGroupIn(ECSI_Node *node, ECSI_GroupFunction function, void *userData)
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

static void ECSI_LayoutForEachGroup(ECSI_GroupFunction function, void *userData)
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

/// @brief Computes every rectangle of the current workspace for the window's current size.
static void ECSI_LayoutUpdate(void)
{
    ECSI_LayoutReadSize();

    ECSI_Workspace *workspace = ECSI_LayoutCurrent();

    if (workspace == NULL)
    {
        return;
    }

    if (workspace->maximized != NULL)
    {
        ECSI_LayoutPlace(workspace->maximized, 0.0f, 0.0f, LAYOUT.width, LAYOUT.height);
    }
    else if (workspace->tree != NULL)
    {
        ECSI_LayoutPlace(workspace->tree, 0.0f, 0.0f, LAYOUT.width, LAYOUT.height);
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

/// @brief Tidies a workspace's tree after an operation. Nodes may be freed, so the core's interface forgets the nodes it pointed to.
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

    LAYOUT.gripGroup = NULL;
    LAYOUT.dragSplit = NULL;
    LAYOUT.dragPanel = NULL;
    LAYOUT.dragging = false;
    LAYOUT.frameNeeded = true;
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
static void ECSI_LayoutMove(ECSI_Workspace *workspace, ECSPanel panel, const ECSI_Drop *drop)
{
    ECSI_Node *source = ECSI_LayoutFindGroup(workspace->tree, panel);
    ECSI_Node *target = drop->group;
    bool side = drop->zone >= ECSI_Zone_Left && drop->zone <= ECSI_Zone_Bottom;
    bool edge = drop->zone >= ECSI_Zone_WindowLeft;

    // a panel cannot join its own group again, or split away from a group that holds only itself
    if (source == NULL || drop->zone == ECSI_Zone_None || (source == target && (drop->zone == ECSI_Zone_Center || (side && arrlenu(source->panels) == 1))))
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
    workspace->focus = panel;
    ECSI_LayoutTidy(workspace);
}

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

/// @brief Finds the gap between a group's tabs nearest to a point, from the tabs drawn in the last frame.
static ECSI_Drop ECSI_LayoutFindTabGap(ECSI_Node *group, f32 x, SDL_FRect *retRect)
{
    ECSI_Drop drop = {.zone = ECSI_Zone_Tabs, .group = group, .index = 0};
    f32 gapX = group->x;
    Clay_SetCurrentContext(LAYOUT.clay);

    for (usz i = 0; i < arrlenu(LAYOUT.tabs); i++)
    {
        Clay_ElementData tab = Clay_GetElementData(CLAY_IDI("Tab", (u32)i));

        if (LAYOUT.tabs[i].group != group || !tab.found)
        {
            continue;
        }

        if (x > tab.boundingBox.x + tab.boundingBox.width / 2.0f)
        {
            drop.index = LAYOUT.tabs[i].index + 1;
            gapX = tab.boundingBox.x + tab.boundingBox.width;
        }
        else if (LAYOUT.tabs[i].index == drop.index)
        {
            gapX = tab.boundingBox.x;
        }
    }

    *retRect = (SDL_FRect){gapX - OPENECS_TAB_GAP_WIDTH / 2.0f, group->y, OPENECS_TAB_GAP_WIDTH, OPENECS_TAB_ROW_HEIGHT};
    return drop;
}

/// @brief Finds where a panel dragged to a point lands, and the rectangle to highlight. The checks follow DESIGN 6.7.
static ECSI_Drop ECSI_LayoutFindDrop(f32 x, f32 y, SDL_FRect *retRect)
{
    f32 width = LAYOUT.width;
    f32 height = LAYOUT.height;
    *retRect = (SDL_FRect){0};

    // outside the window, the panel would pop out; pop-out windows are not implemented yet
    if (!ECSI_LayoutContains(x, y, 0.0f, 0.0f, width, height))
    {
        return (ECSI_Drop){0};
    }

    if (x < OPENECS_DOCK_EDGE || x >= width - OPENECS_DOCK_EDGE || y < OPENECS_DOCK_EDGE || y >= height - OPENECS_DOCK_EDGE)
    {
        ECSI_Zone zone = x < OPENECS_DOCK_EDGE            ? ECSI_Zone_WindowLeft
                         : x >= width - OPENECS_DOCK_EDGE ? ECSI_Zone_WindowRight
                         : y < OPENECS_DOCK_EDGE          ? ECSI_Zone_WindowTop
                                                          : ECSI_Zone_WindowBottom;

        *retRect = zone == ECSI_Zone_WindowLeft    ? (SDL_FRect){0.0f, 0.0f, width / 4.0f, height}
                   : zone == ECSI_Zone_WindowRight ? (SDL_FRect){width * 0.75f, 0.0f, width / 4.0f, height}
                   : zone == ECSI_Zone_WindowTop   ? (SDL_FRect){0.0f, 0.0f, width, height / 4.0f}
                                                   : (SDL_FRect){0.0f, height * 0.75f, width, height / 4.0f};
        return (ECSI_Drop){.zone = zone};
    }

    ECSI_GroupHit hit = {x, y, NULL};
    ECSI_LayoutForEachGroup(ECSI_LayoutFindGroupAt, &hit);
    ECSI_Node *group = hit.group;

    if (group == NULL || arrlenu(group->panels) == 0)
    {
        return (ECSI_Drop){0};
    }

    if (arrlenu(group->panels) >= 2 && y < group->y + OPENECS_TAB_ROW_HEIGHT)
    {
        return ECSI_LayoutFindTabGap(group, x, retRect);
    }

    // edge bands are a quarter of the panel deep at most, so the centre keeps at least half of it
    ECSPanel panel = group->panels[group->shown];
    f32 bandX = SDL_min(panel->width / 4.0f, OPENECS_SPLIT_DEPTH);
    f32 bandY = SDL_min(panel->height / 4.0f, OPENECS_SPLIT_DEPTH);
    const f32 distances[] = {
        (x - panel->x) / bandX,
        (panel->x + panel->width - x) / bandX,
        (y - panel->y) / bandY,
        (panel->y + panel->height - y) / bandY,
    };

    // the nearest band wins; distances are relative to the band, so 1 is its inner edge
    ECSI_Zone zone = ECSI_Zone_Center;
    f32 nearest = 1.0f;

    for (usz i = 0; i < SDL_arraysize(distances); i++)
    {
        if (distances[i] < nearest)
        {
            nearest = distances[i];
            zone = (ECSI_Zone)(ECSI_Zone_Left + (i32)i);
        }
    }

    f32 halfWidth = panel->width / 2.0f;
    f32 halfHeight = panel->height / 2.0f;
    *retRect = zone == ECSI_Zone_Left     ? (SDL_FRect){panel->x, panel->y, halfWidth, panel->height}
               : zone == ECSI_Zone_Right  ? (SDL_FRect){panel->x + halfWidth, panel->y, halfWidth, panel->height}
               : zone == ECSI_Zone_Top    ? (SDL_FRect){panel->x, panel->y, panel->width, halfHeight}
               : zone == ECSI_Zone_Bottom ? (SDL_FRect){panel->x, panel->y + halfHeight, panel->width, halfHeight}
                                          : (SDL_FRect){panel->x, panel->y, panel->width, panel->height};
    return (ECSI_Drop){.zone = zone, .group = group};
}

/// @brief Remembers a press on a panel's tab or grip; the panel is dragged once the pointer moves far enough.
static void ECSI_LayoutArmDrag(ECSPanel panel, f32 x, f32 y)
{
    LAYOUT.dragPanel = panel;
    LAYOUT.dragging = false;
    LAYOUT.dragStartX = x;
    LAYOUT.dragStartY = y;
}

#pragma endregion Moving

#pragma region Dividers

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

/// @brief Moves the dragged divider to a point. Fixed children keep fixed sizes; shared children turn their sizes into shares.
static void ECSI_LayoutDragDivider(f32 x, f32 y)
{
    ECSI_Node *split = LAYOUT.dragSplit;
    ECSI_Node *first = split->children[LAYOUT.dragDivider];
    ECSI_Node *second = split->children[LAYOUT.dragDivider + 1];

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

#pragma endregion Dividers

#pragma region Interface

typedef struct ECSI_GripHit
{
    f32 x;
    f32 y;
    ECSI_Node *group;
} ECSI_GripHit;

static void ECSI_LayoutFindGripGroup(ECSI_Node *group, void *userData)
{
    ECSI_GripHit *hit = userData;

    if (arrlenu(group->panels) == 1 && ECSI_LayoutContains(hit->x, hit->y, group->x, group->y, group->width, OPENECS_GRIP_ZONE))
    {
        hit->group = group;
    }
}

/// @brief Declares a group's tab row, or its placeholder text, for Clay.
static void ECSI_LayoutDeclareGroup(ECSI_Node *group, void *userData)
{
    (void)userData;

    if (arrlenu(group->panels) == 0)
    {
        return;
    }

    if (arrlenu(group->panels) >= 2)
    {
        CLAY_AUTO_ID({
            .layout = {
                .sizing = {CLAY_SIZING_FIXED(group->width), CLAY_SIZING_FIXED(OPENECS_TAB_ROW_HEIGHT)},
                .childGap = 1,
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
            },
            .backgroundColor = OPENECS_COLOR_TAB_ROW,
            .clip = {.horizontal = true},
            .floating = {.attachTo = CLAY_ATTACH_TO_ROOT, .offset = {group->x, group->y}},
        })
        {
            for (usz i = 0; i < arrlenu(group->panels); i++)
            {
                CLAY(CLAY_IDI("Tab", (u32)arrlenu(LAYOUT.tabs)), {
                    .layout = {
                        .sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_GROW(0)},
                        .padding = {12, 12, 0, 0},
                        .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                    },
                    .backgroundColor = i == group->shown ? OPENECS_COLOR_TAB_SHOWN : OPENECS_COLOR_TAB,
                })
                {
                    CLAY_TEXT(ECSI_LayoutClayText(group->panels[i]->title),
                              CLAY_TEXT_CONFIG({
                                  .textColor = i == group->shown ? OPENECS_COLOR_TEXT : OPENECS_COLOR_TEXT_DIM,
                                  .fontSize = OPENECS_FONT_SIZE,
                                  .wrapMode = CLAY_TEXT_WRAP_NONE,
                              }));
                }

                arrput(LAYOUT.tabs, ((ECSI_TabRef){group, i}));
            }
        }
    }

    ECSPanel panel = group->panels[group->shown];

    if (panel->type == NULL)
    {
        CLAY_AUTO_ID({
            .layout = {
                .sizing = {CLAY_SIZING_FIXED(panel->width), CLAY_SIZING_FIXED(panel->height)},
                .padding = CLAY_PADDING_ALL(16),
                .childGap = 6,
                .layoutDirection = CLAY_TOP_TO_BOTTOM,
            },
            .backgroundColor = OPENECS_COLOR_PLACEHOLDER,
            .clip = {.horizontal = true, .vertical = true},
            .floating = {.attachTo = CLAY_ATTACH_TO_ROOT, .offset = {panel->x, panel->y}},
        })
        {
            CLAY_TEXT(CLAY_STRING("Missing panel type"), CLAY_TEXT_CONFIG({.textColor = OPENECS_COLOR_TEXT_DIM, .fontSize = OPENECS_FONT_SIZE}));
            CLAY_TEXT(ECSI_LayoutClayText(panel->typeName), CLAY_TEXT_CONFIG({.textColor = OPENECS_COLOR_TEXT, .fontSize = OPENECS_FONT_SIZE}));
        }
    }
}

/// @brief Declares the core's own interface for Clay and returns what to draw.
static Clay_RenderCommandArray ECSI_LayoutDeclareInterface(void)
{
    Clay_SetCurrentContext(LAYOUT.clay);
    Clay_SetLayoutDimensions((Clay_Dimensions){LAYOUT.width, LAYOUT.height});
    Clay_BeginLayout();

    arrfree(LAYOUT.tabs);
    ECSI_Workspace *workspace = ECSI_LayoutCurrent();

    CLAY(CLAY_ID("Root"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}}})
    {
        ECSI_LayoutForEachGroup(ECSI_LayoutDeclareGroup, NULL);

        ECSPanel focus = workspace == NULL ? NULL : workspace->focus;

        if (focus != NULL)
        {
            CLAY_AUTO_ID({
                .layout = {.sizing = {CLAY_SIZING_FIXED(focus->width), CLAY_SIZING_FIXED(focus->height)}},
                .border = {.color = OPENECS_COLOR_ACCENT, .width = {2, 2, 2, 2, 0}},
                .floating = {.attachTo = CLAY_ATTACH_TO_ROOT, .offset = {focus->x, focus->y}, .zIndex = 1, .pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH},
            }) {}
        }

        if (LAYOUT.gripGroup != NULL)
        {
            ECSI_Node *group = LAYOUT.gripGroup;

            CLAY_AUTO_ID({
                .layout = {.sizing = {CLAY_SIZING_FIXED(OPENECS_GRIP_WIDTH), CLAY_SIZING_FIXED(OPENECS_GRIP_HEIGHT)}},
                .backgroundColor = OPENECS_COLOR_TEXT_DIM,
                .cornerRadius = CLAY_CORNER_RADIUS(3),
                .floating = {.attachTo = CLAY_ATTACH_TO_ROOT, .offset = {group->x + (group->width - OPENECS_GRIP_WIDTH) / 2.0f, group->y + 3.0f}, .zIndex = 2},
            }) {}
        }

        if (LAYOUT.dragging && LAYOUT.drop.zone != ECSI_Zone_None)
        {
            SDL_FRect rect = LAYOUT.dropRect;

            CLAY_AUTO_ID({
                .layout = {.sizing = {CLAY_SIZING_FIXED(rect.w), CLAY_SIZING_FIXED(rect.h)}},
                .backgroundColor = OPENECS_COLOR_DROP,
                .border = {.color = OPENECS_COLOR_ACCENT, .width = {2, 2, 2, 2, 0}},
                .floating = {.attachTo = CLAY_ATTACH_TO_ROOT, .offset = {rect.x, rect.y}, .zIndex = 4, .pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH},
            }) {}
        }

        if (LAYOUT.showPrefixKeys)
        {
            CLAY_AUTO_ID({
                .layout = {.padding = CLAY_PADDING_ALL(14), .childGap = 4, .layoutDirection = CLAY_TOP_TO_BOTTOM},
                .backgroundColor = OPENECS_COLOR_OVERLAY,
                .cornerRadius = CLAY_CORNER_RADIUS(6),
                .border = {.color = OPENECS_COLOR_ACCENT, .width = {1, 1, 1, 1, 0}},
                .floating = {.attachTo = CLAY_ATTACH_TO_ROOT, .offset = {24.0f, 24.0f}, .zIndex = 3},
            })
            {
                for (usz i = 0; i < SDL_arraysize(ECSI_PREFIX_KEY_LINES); i++)
                {
                    CLAY_AUTO_ID({.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT}})
                    {
                        CLAY_AUTO_ID({.layout = {.sizing = {CLAY_SIZING_FIXED(OPENECS_PREFIX_KEY_COLUMN), CLAY_SIZING_FIT(0)}}})
                        {
                            CLAY_TEXT(ECSI_LayoutClayText(ECSI_PREFIX_KEY_LINES[i][0]), CLAY_TEXT_CONFIG({.textColor = OPENECS_COLOR_ACCENT, .fontSize = OPENECS_FONT_SIZE}));
                        }

                        CLAY_TEXT(ECSI_LayoutClayText(ECSI_PREFIX_KEY_LINES[i][1]), CLAY_TEXT_CONFIG({.textColor = OPENECS_COLOR_TEXT, .fontSize = OPENECS_FONT_SIZE}));
                    }
                }
            }
        }
    }

    return Clay_EndLayout(0.0f);
}

#pragma endregion Interface

#pragma region Drawing

static void ECSI_LayoutWantsFrameIn(ECSI_Node *group, void *userData)
{
    bool *wants = userData;

    if (arrlenu(group->panels) > 0 && ECSI_PanelWantsFrame(group->panels[group->shown]))
    {
        *wants = true;
    }
}

static void ECSI_LayoutRenderGroup(ECSI_Node *group, void *userData)
{
    u64 *nowTicks = userData;

    if (arrlenu(group->panels) > 0)
    {
        ECSI_PanelRender(group->panels[group->shown], LAYOUT.renderer, *nowTicks);
    }
}

#pragma endregion Drawing

#pragma region Panels

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

#pragma endregion Panels

#pragma region Saving

/// @brief Describes a node and its children for a session. A split holds its children as list items; a group holds its panels.
static SHUResult ECSI_LayoutSaveNode(const ECSI_Workspace *workspace, const ECSI_Node *node, ECSValue *retNode)
{
    ECSValue *field = NULL;
    ECSValue_SetTable(retNode);

    if (node->parent != NULL)
    {
        bool fixed = node->fixedSize > 0.0f;
        SHU_ReturnResult(ECSValue_SetField(retNode, fixed ? "size" : "share", &field));
        ECSValue_SetNumber(field, fixed ? node->fixedSize : node->share);
    }

    if (node->type == ECSI_NodeType_Split)
    {
        SHU_ReturnResult(ECSValue_SetField(retNode, "split", &field));
        SHU_ReturnResult(ECSValue_SetString(field, node->vertical ? "vertical" : "horizontal"));

        for (usz i = 0; i < arrlenu(node->children); i++)
        {
            SHU_ReturnResult(ECSValue_AddItem(retNode, &field));
            SHU_ReturnResult(ECSI_LayoutSaveNode(workspace, node->children[i], field));
        }

        return SHUResult_Ok;
    }

    ECSValue *panels = NULL;
    SHU_ReturnResult(ECSValue_SetField(retNode, "panels", &panels));
    ECSValue_SetTable(panels);

    for (usz i = 0; i < arrlenu(node->panels); i++)
    {
        SHU_ReturnResult(ECSValue_AddItem(panels, &field));
        SHU_ReturnResult(ECSI_PanelSave(node->panels[i], field));
    }

    SHU_ReturnResult(ECSValue_SetField(retNode, "shown", &field));
    ECSValue_SetInteger(field, (i64)node->shown + 1);

    if (workspace->maximized == node)
    {
        SHU_ReturnResult(ECSValue_SetField(retNode, "maximized", &field));
        ECSValue_SetBool(field, true);
    }

    return SHUResult_Ok;
}

#pragma endregion Saving

/// @brief Opens the OS window, its renderer and the core's font.
static SHUResult ECSI_LayoutOpen(const char *title, const char *fontPath)
{
    if (!TTF_Init())
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_ttf failed to start: %s", SDL_GetError());
        return SHUResult_ErrInternal;
    }

    LAYOUT.fonts[0] = TTF_OpenFont(fontPath, OPENECS_FONT_SIZE);

    if (LAYOUT.fonts[0] == NULL)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot load the font '%s': %s", fontPath, SDL_GetError());
        return SHUResult_ErrFile;
    }

    LAYOUT.window = SDL_CreateWindow(title, OPENECS_WINDOW_WIDTH, OPENECS_WINDOW_HEIGHT, SDL_WINDOW_RESIZABLE);

    if (LAYOUT.window == NULL)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot open a window: %s", SDL_GetError());
        return SHUResult_ErrInternal;
    }

    LAYOUT.renderer = SDL_CreateGPURenderer(NULL, LAYOUT.window);

    if (LAYOUT.renderer == NULL)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "GPU renderer not available (%s); using SDL's default renderer.", SDL_GetError());
        LAYOUT.renderer = SDL_CreateRenderer(LAYOUT.window, NULL);
    }

    if (LAYOUT.renderer == NULL)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot create a renderer: %s", SDL_GetError());
        return SHUResult_ErrInternal;
    }

    SDL_SetRenderVSync(LAYOUT.renderer, 1);

    LAYOUT.clayRenderer = (Clay_SDL3RendererData){
        .renderer = LAYOUT.renderer,
        .textEngine = TTF_CreateRendererTextEngine(LAYOUT.renderer),
        .fonts = LAYOUT.fonts,
    };

    if (LAYOUT.clayRenderer.textEngine == NULL)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot create a text engine: %s", SDL_GetError());
        return SHUResult_ErrInternal;
    }

    ECSI_LayoutReadSize();

    u32 claySize = Clay_MinMemorySize();
    LAYOUT.clayMemory = SDL_malloc(claySize);

    if (LAYOUT.clayMemory == NULL)
    {
        return SHUResult_ErrAllocation;
    }

    Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(claySize, LAYOUT.clayMemory);
    LAYOUT.clay = Clay_Initialize(arena, (Clay_Dimensions){LAYOUT.width, LAYOUT.height}, (Clay_ErrorHandler){ECSI_LayoutClayError, NULL});
    Clay_SetMeasureTextFunction(ECSI_LayoutMeasureText, NULL);

    return SHUResult_Ok;
}

#pragma endregion Source Only

SHUResult ECSI_LayoutInitialize(const char *title, const char *fontPath)
{
    SDL_assert(title != NULL);
    SDL_assert(fontPath != NULL);

    SHU_ReturnResult(ECSI_LayoutOpen(title, fontPath), ECSI_LayoutTerminate(););

    LAYOUT.frameNeeded = true;
    return SHUResult_Ok;
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
    arrfree(LAYOUT.tabs);

    SDL_free(LAYOUT.clayMemory);

    if (LAYOUT.clayRenderer.textEngine != NULL)
    {
        TTF_DestroyRendererTextEngine(LAYOUT.clayRenderer.textEngine);
    }

    if (LAYOUT.renderer != NULL)
    {
        SDL_DestroyRenderer(LAYOUT.renderer);
    }

    if (LAYOUT.window != NULL)
    {
        SDL_DestroyWindow(LAYOUT.window);
    }

    if (LAYOUT.fonts[0] != NULL)
    {
        TTF_CloseFont(LAYOUT.fonts[0]);
    }

    if (TTF_WasInit() > 0)
    {
        TTF_Quit();
    }

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

    LAYOUT.current = index;
    LAYOUT.gripGroup = NULL;
    LAYOUT.dragSplit = NULL;
    LAYOUT.dragPanel = NULL;
    LAYOUT.dragging = false;
    LAYOUT.frameNeeded = true;
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

void ECSI_LayoutRender(u64 nowTicks)
{
    ECSI_LayoutUpdate();
    Clay_RenderCommandArray commands = ECSI_LayoutDeclareInterface();

    SDL_SetRenderDrawColor(LAYOUT.renderer, OPENECS_COLOR_BACKGROUND);
    SDL_RenderClear(LAYOUT.renderer);
    ECSI_LayoutForEachGroup(ECSI_LayoutRenderGroup, &nowTicks);
    SDL_Clay_RenderClayCommands(&LAYOUT.clayRenderer, &commands);
    SDL_RenderPresent(LAYOUT.renderer);

    LAYOUT.frameNeeded = false;
}

ECSPanel ECSI_LayoutPanelAt(f32 x, f32 y)
{
    ECSI_PanelHit hit = {x, y, NULL};
    ECSI_LayoutForEachGroup(ECSI_LayoutFindPanel, &hit);
    return hit.panel;
}

bool ECSI_LayoutPointerDown(f32 x, f32 y)
{
    ECSI_Workspace *workspace = ECSI_LayoutCurrent();

    if (workspace == NULL)
    {
        return false;
    }

    ECSI_DividerHit divider = {x, y, NULL, 0};
    ECSI_LayoutFindDivider(workspace->maximized != NULL ? NULL : workspace->tree, &divider);

    if (divider.split != NULL)
    {
        LAYOUT.dragSplit = divider.split;
        LAYOUT.dragDivider = divider.divider;
        return true;
    }

    Clay_SetCurrentContext(LAYOUT.clay);

    for (usz i = 0; i < arrlenu(LAYOUT.tabs); i++)
    {
        Clay_ElementData tab = Clay_GetElementData(CLAY_IDI("Tab", (u32)i));

        if (tab.found && ECSI_LayoutContains(x, y, tab.boundingBox.x, tab.boundingBox.y, tab.boundingBox.width, tab.boundingBox.height))
        {
            ECSI_Node *group = LAYOUT.tabs[i].group;
            group->shown = LAYOUT.tabs[i].index;
            ECSI_LayoutSetFocus(group->panels[group->shown]);
            ECSI_LayoutArmDrag(group->panels[group->shown], x, y);
            return true;
        }
    }

    if (LAYOUT.gripGroup != NULL && y < LAYOUT.gripGroup->y + OPENECS_GRIP_ZONE)
    {
        ECSI_LayoutArmDrag(LAYOUT.gripGroup->panels[0], x, y);
        return true;
    }

    return false;
}

bool ECSI_LayoutPointerMove(f32 x, f32 y)
{
    if (LAYOUT.dragSplit != NULL)
    {
        ECSI_LayoutDragDivider(x, y);
        return true;
    }

    if (LAYOUT.dragPanel != NULL)
    {
        LAYOUT.dragging = LAYOUT.dragging || SDL_fabsf(x - LAYOUT.dragStartX) + SDL_fabsf(y - LAYOUT.dragStartY) >= OPENECS_DRAG_THRESHOLD;

        if (LAYOUT.dragging)
        {
            LAYOUT.drop = ECSI_LayoutFindDrop(x, y, &LAYOUT.dropRect);
            LAYOUT.frameNeeded = true;
        }

        return true;
    }

    ECSI_GripHit hit = {x, y, NULL};
    ECSI_LayoutForEachGroup(ECSI_LayoutFindGripGroup, &hit);

    if (hit.group != LAYOUT.gripGroup)
    {
        LAYOUT.gripGroup = hit.group;
        LAYOUT.frameNeeded = true;
    }

    return false;
}

void ECSI_LayoutPointerUp(void)
{
    ECSI_Workspace *workspace = ECSI_LayoutCurrent();

    if (LAYOUT.dragging && workspace != NULL)
    {
        ECSI_LayoutMove(workspace, LAYOUT.dragPanel, &LAYOUT.drop);
    }

    ECSI_LayoutCancelDrag();
    LAYOUT.dragSplit = NULL;
}

bool ECSI_LayoutCancelDrag(void)
{
    bool dragging = LAYOUT.dragging;
    LAYOUT.dragPanel = NULL;
    LAYOUT.dragging = false;
    LAYOUT.drop = (ECSI_Drop){0};
    LAYOUT.frameNeeded = LAYOUT.frameNeeded || dragging;
    return dragging;
}

ECSPanel ECSI_LayoutGetFocus(void)
{
    ECSI_Workspace *workspace = ECSI_LayoutCurrent();
    return workspace == NULL ? NULL : workspace->focus;
}

void ECSI_LayoutSetFocus(ECSPanel panel)
{
    ECSI_Workspace *workspace = ECSI_LayoutCurrent();

    if (workspace != NULL && workspace->focus != panel)
    {
        workspace->focus = panel;
        LAYOUT.frameNeeded = true;
    }
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

    ECSI_Drop drop = {.zone = dx < 0 ? ECSI_Zone_WindowLeft : dx > 0 ? ECSI_Zone_WindowRight : dy < 0 ? ECSI_Zone_WindowTop : ECSI_Zone_WindowBottom};

    if (neighbour != NULL)
    {
        drop = (ECSI_Drop){.zone = ECSI_Zone_Center, .group = ECSI_LayoutFindGroup(workspace->tree, neighbour)};
    }

    ECSI_LayoutMove(workspace, workspace->focus, &drop);
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

void ECSI_LayoutToggleMaximize(void)
{
    ECSI_Workspace *workspace = ECSI_LayoutCurrent();
    ECSI_Node *group = workspace == NULL ? NULL : ECSI_LayoutFindGroup(workspace->tree, workspace->focus);

    if (group == NULL)
    {
        return;
    }

    workspace->maximized = workspace->maximized == group ? NULL : group;
    LAYOUT.gripGroup = NULL;
    LAYOUT.frameNeeded = true;
}

void ECSI_LayoutClosePanel(ECSPanel panel)
{
    ECSI_Workspace *workspace = ECSI_LayoutCurrent();
    ECSI_Node *group = workspace == NULL || panel == NULL ? NULL : ECSI_LayoutFindGroup(workspace->tree, panel);

    if (group == NULL)
    {
        return;
    }

    usz index = 0;

    while (group->panels[index] != panel)
    {
        index++;
    }

    arrdel(group->panels, index);
    ECSI_PanelClose(&panel);

    // tidying frees an empty group, so its focus is found before and after
    bool emptied = arrlenu(group->panels) == 0;

    if (!emptied)
    {
        group->shown = SDL_min(group->shown, arrlenu(group->panels) - 1);
        workspace->focus = group->panels[group->shown];
    }

    ECSI_LayoutTidy(workspace);

    if (emptied)
    {
        workspace->focus = ECSI_LayoutFirstPanel(workspace->tree);
    }
}

void ECSI_LayoutShowPrefixKeys(bool show)
{
    LAYOUT.showPrefixKeys = show;
    LAYOUT.frameNeeded = true;
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

        SHU_ReturnResult(ECSValue_AddItem(retWorkspaces, &saved));
        SHU_ReturnResult(ECSValue_SetField(saved, "name", &field));
        SHU_ReturnResult(ECSValue_SetString(field, workspace->name));

        if (workspace->focus != NULL)
        {
            SHU_ReturnResult(ECSValue_SetField(saved, "focus", &field));
            ECSValue_SetInteger(field, workspace->focus->id);
        }

        SHU_ReturnResult(ECSValue_SetField(saved, "windows", &field));
        ECSValue_SetTable(field);

        if (workspace->tree != NULL)
        {
            SHU_ReturnResult(ECSValue_AddItem(field, &field));
            SHU_ReturnResult(ECSI_LayoutSaveNode(workspace, workspace->tree, field));
        }
    }

    return SHUResult_Ok;
}
