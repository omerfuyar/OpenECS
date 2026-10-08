#include "interface/Layout.h"

#include "runtime/Events.h"
#include "runtime/Plugins.h"
#include "runtime/Settings.h"

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
/// @brief Refresh rate assumed when the display's is unknown.
#define OPENECS_FALLBACK_FRAME_RATE 60.0f
/// @brief How far the frame limit is above the refresh rate.
#define OPENECS_FRAME_RATE_MARGIN 1.1f
/// @brief Gap between the children of a split; dragging it resizes them.
#define OPENECS_DIVIDER_SIZE 4.0f
/// @brief Height of a tab row.
#define OPENECS_TAB_ROW_HEIGHT 26.0f
/// @brief Distance from a panel's top edge within which its grip appears.
#define OPENECS_GRIP_ZONE 24.0f
/// @brief Height of a grip, which shows the panel's title.
#define OPENECS_GRIP_HEIGHT 20.0f
/// @brief Distance the pointer moves from a press on a tab or grip before the panel is dragged.
#define OPENECS_DRAG_THRESHOLD 6.0f
/// @brief Distance from an edge of the OS window within which a dragged panel docks along that edge.
#define OPENECS_DOCK_EDGE 16.0f
/// @brief Deepest edge band of a panel in which a dragged panel splits it.
#define OPENECS_SPLIT_DEPTH 80.0f
/// @brief How many closed panels ecs.reopen remembers.
#define OPENECS_REOPEN_LIMIT 20
/// @brief How far one step of the wheel scrolls a tab row.
#define OPENECS_TAB_SCROLL_STEP 40.0f
/// @brief Width of the mark between tabs where a dragged panel is inserted.
#define OPENECS_TAB_GAP_WIDTH 3.0f
/// @brief Smallest size a divider drag leaves to a child.
#define OPENECS_MIN_CHILD_SIZE 32.0f
/// @brief Padding around a panel menu and inside its entries.
#define OPENECS_MENU_PADDING 6.0f
#define OPENECS_MENU_ITEM_PADDING 10.0f
/// @brief Text that a locked group's grip shows after the title.
#define OPENECS_LOCKED_MARK "locked"
/// @brief Most menus open at once: the panel menu and its submenus.
#define OPENECS_MENU_DEPTH 4

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
#define OPENECS_COLOR_SELECTED ((Clay_Color){58, 62, 70, 255})

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
    bool locked;      // its panels cannot be moved or closed, and it accepts no dropped panels
    f32 tabScroll;    // how far the tab row is scrolled, in layout units
    f32 tabsWidth;    // width of all its tabs together, measured in the last frame
    ECSPanel scrolledTo; // the shown panel whose tab was last scrolled into view
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

/// @brief A closed panel that ecs.reopen can open again.
typedef struct ECSI_ClosedPanel
{
    ECSValue *saved; // its type, state and state_version, as ECSI_PanelSave writes them
    u32 neighbour;   // id of a panel that stayed in its group, or 0
} ECSI_ClosedPanel;

/// @brief A tab drawn in the last frame, so a click can find it.
typedef struct ECSI_TabRef
{
    ECSI_Node *group;
    usz index;
} ECSI_TabRef;

/// @brief Function called for each visible group.
typedef void (*ECSI_GroupFunction)(ECSI_Node *group, void *userData);

/// @brief A shown menu. Each line pair is an entry's key text and its label.
typedef struct ECSI_MenuView
{
    const char *const *lines; // NULL when this menu is closed
    usz count;
    usz selected;
    SDL_FRect rect;
    f32 itemHeight;
} ECSI_MenuView;

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
    ECSI_ClosedPanel *closed; // stb_ds array of the panels ecs.reopen can open again, the last closed last

    bool frameNeeded;
    i64 framePercent;     // frame rate as a percentage of the refresh rate, from ecs.vsync; 0 for no vsync and no limit
    u64 frameNanoseconds; // shortest time between frames, from the display's refresh rate; 0 for no limit
    u64 lastFrameTicks;
    const char *const *prefixLines; // keys after the prefix and what they do, in pairs, or NULL when the prefix is not pressed
    usz prefixLineCount;
    ECSI_TabRef *tabs; // stb_ds array

    ECSI_Node *gripGroup;   // lone group whose grip is shown, or NULL
    ECSI_Node *dragSplit;   // split whose divider is dragged, or NULL
    usz dragDivider;        // the dragged divider follows this child

    ECSPanel dragPanel;     // panel pressed on its tab or grip, or NULL; it is dragged once the pointer moves far enough
    bool dragGroup;         // the whole group of dragPanel is dragged, pressed on its tab row
    bool dragFromGrip;      // the press was on a grip; a click without dragging opens the panel's menu
    bool dragLocked;        // the pressed grip is a locked group's, so it is never dragged
    bool dragging;
    f32 dragStartX;
    f32 dragStartY;
    ECSI_Drop drop;         // where the dragged panel lands now
    SDL_FRect dropRect;     // the highlight of the drop place

    SDL_Cursor *cursors[SDL_SYSTEM_CURSOR_COUNT]; // made when first used
    SDL_SystemCursor cursor;                      // the pointer's shape now

    ECSI_MenuView menus[OPENECS_MENU_DEPTH]; // the panel menu, then its open submenus
} LAYOUT = {0};

/// @brief Width of the key column in the list of prefix keys.
#define OPENECS_PREFIX_KEY_COLUMN 104.0f

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

/// @brief Computes every rectangle of the current workspace for the window's current size, and tells the panels that became visible or hidden, or changed size.
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

    // panels of other workspaces are hidden; in this one, a maximized group hides the others
    for (usz i = 0; i < arrlenu(LAYOUT.workspaces); i++)
    {
        ECSI_Node *only = LAYOUT.workspaces[i].maximized;
        ECSI_LayoutForEachGroupIn(LAYOUT.workspaces[i].tree, ECSI_LayoutTellVisible, &LAYOUT.workspaces[i] == workspace ? &only : NULL);
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

/// @brief Changes a workspace's focus. In the current workspace, the panel that loses focus and the one that gets it are told.
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

/// @brief Finds the group whose tab row is at a point.
static ECSI_Node *ECSI_LayoutTabRowGroupAt(f32 x, f32 y)
{
    ECSI_GroupHit hit = {x, y, NULL};
    ECSI_LayoutForEachGroup(ECSI_LayoutFindGroupAt, &hit);
    ECSI_Node *group = hit.group;
    return group != NULL && arrlenu(group->panels) >= 2 && y < group->y + OPENECS_TAB_ROW_HEIGHT ? group : NULL;
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

    // a locked group accepts no dropped panels
    if (arrlenu(group->panels) >= 2 && y < group->y + OPENECS_TAB_ROW_HEIGHT)
    {
        return group->locked ? (ECSI_Drop){0} : ECSI_LayoutFindTabGap(group, x, retRect);
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
    if (zone == ECSI_Zone_Center && group->locked)
    {
        *retRect = (SDL_FRect){0};
        return (ECSI_Drop){0};
    }

    return (ECSI_Drop){.zone = zone, .group = group};
}

/// @brief Checks whether dropping the dragged panel, or its group, at a place would change the layout.
static bool ECSI_LayoutDropChanges(const ECSI_Drop *drop)
{
    ECSI_Workspace *workspace = ECSI_LayoutCurrent();
    ECSI_Node *source = workspace == NULL ? NULL : ECSI_LayoutFindGroup(workspace->tree, LAYOUT.dragPanel);

    if (source == NULL || drop->zone == ECSI_Zone_None)
    {
        return false;
    }

    // a whole group moves when the group is dragged or holds only the dragged panel
    bool whole = LAYOUT.dragGroup || arrlenu(source->panels) == 1;

    if (drop->zone >= ECSI_Zone_WindowLeft)
    {
        return !whole || workspace->tree != source;
    }

    if (drop->group != source)
    {
        return true;
    }

    if (whole || drop->zone == ECSI_Zone_Center)
    {
        return false;
    }

    // a gap next to the panel's own tab leaves it where it is
    if (drop->zone == ECSI_Zone_Tabs)
    {
        usz index = 0;

        while (source->panels[index] != LAYOUT.dragPanel)
        {
            index++;
        }

        return drop->index != index && drop->index != index + 1;
    }

    return true;
}

/// @brief Remembers a press on a panel's tab or grip, or on its group's tab row; the panel or group is dragged once the pointer moves far enough.
static void ECSI_LayoutArmDrag(ECSPanel panel, bool group, bool grip, f32 x, f32 y)
{
    LAYOUT.dragPanel = panel;
    LAYOUT.dragGroup = group;
    LAYOUT.dragFromGrip = grip;
    LAYOUT.dragLocked = false;
    LAYOUT.dragging = false;
    LAYOUT.dragStartX = x;
    LAYOUT.dragStartY = y;
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

/// @brief Checks whether a point is on an element that Clay laid out in the last frame.
static bool ECSI_LayoutOnElement(Clay_ElementId id, f32 x, f32 y)
{
    Clay_SetCurrentContext(LAYOUT.clay);
    Clay_ElementData element = Clay_GetElementData(id);
    return element.found && ECSI_LayoutContains(x, y, element.boundingBox.x, element.boundingBox.y, element.boundingBox.width, element.boundingBox.height);
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
            .clip = {.horizontal = true, .childOffset = {-group->tabScroll, 0.0f}},
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

                    // the mark of unsaved work
                    if (group->panels[i]->unsaved)
                    {
                        CLAY_TEXT(CLAY_STRING(" *"), CLAY_TEXT_CONFIG({.textColor = OPENECS_COLOR_ACCENT, .fontSize = OPENECS_FONT_SIZE, .wrapMode = CLAY_TEXT_WRAP_NONE}));
                    }

                    // panels of a locked group cannot be closed by the user
                    if (!group->locked)
                    {
                        CLAY(CLAY_IDI("TabClose", (u32)arrlenu(LAYOUT.tabs)), {.layout = {.padding = {8, 0, 0, 0}}})
                        {
                            CLAY_TEXT(CLAY_STRING("\u00D7"), CLAY_TEXT_CONFIG({.textColor = OPENECS_COLOR_TEXT_DIM, .fontSize = OPENECS_FONT_SIZE, .wrapMode = CLAY_TEXT_WRAP_NONE}));
                        }
                    }
                }

                arrput(LAYOUT.tabs, ((ECSI_TabRef){group, i}));
            }
        }
    }

    ECSPanel panel = group->panels[group->shown];

    if (panel->type == NULL || panel->fault != NULL)
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
            Clay_String heading = panel->fault != NULL ? CLAY_STRING("Panel failed") : CLAY_STRING("Missing panel type");
            CLAY_TEXT(heading, CLAY_TEXT_CONFIG({.textColor = OPENECS_COLOR_TEXT_DIM, .fontSize = OPENECS_FONT_SIZE}));
            CLAY_TEXT(ECSI_LayoutClayText(panel->typeName), CLAY_TEXT_CONFIG({.textColor = OPENECS_COLOR_TEXT, .fontSize = OPENECS_FONT_SIZE}));

            if (panel->fault != NULL)
            {
                CLAY_TEXT(ECSI_LayoutClayText(panel->fault), CLAY_TEXT_CONFIG({.textColor = OPENECS_COLOR_TEXT_DIM, .fontSize = OPENECS_FONT_SIZE}));
            }
        }
    }
}

/// @brief Measures each tab row from the tabs Clay just laid out, keeps its scroll inside it, and scrolls a newly shown tab into view.
static void ECSI_LayoutFitTabs(void)
{
    Clay_SetCurrentContext(LAYOUT.clay);

    for (usz i = 0; i < arrlenu(LAYOUT.tabs);)
    {
        // the tabs of one group are next to each other in the list
        ECSI_Node *group = LAYOUT.tabs[i].group;
        f32 left = 0.0f;
        f32 right = 0.0f;
        Clay_BoundingBox shown = {0};

        for (; i < arrlenu(LAYOUT.tabs) && LAYOUT.tabs[i].group == group; i++)
        {
            Clay_ElementData tab = Clay_GetElementData(CLAY_IDI("Tab", (u32)i));
            left = LAYOUT.tabs[i].index == 0 ? tab.boundingBox.x : left;
            right = tab.boundingBox.x + tab.boundingBox.width;
            shown = LAYOUT.tabs[i].index == group->shown ? tab.boundingBox : shown;
        }

        group->tabsWidth = right - left;
        f32 scroll = group->tabScroll;

        if (group->scrolledTo != group->panels[group->shown])
        {
            group->scrolledTo = group->panels[group->shown];
            scroll += shown.x < group->x ? shown.x - group->x : 0.0f;
            scroll += shown.x + shown.width > group->x + group->width ? shown.x + shown.width - group->x - group->width : 0.0f;
        }

        scroll = SDL_clamp(scroll, 0.0f, SDL_max(0.0f, group->tabsWidth - group->width));

        if (scroll != group->tabScroll)
        {
            group->tabScroll = scroll;
            LAYOUT.frameNeeded = true;
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

        // the grip shows the panel's title, centred on its top edge, and whether its group is locked
        if (LAYOUT.gripGroup != NULL)
        {
            ECSI_Node *group = LAYOUT.gripGroup;
            const char *title = group->panels[0]->title;
            int titleWidth = 0;
            int lockedWidth = 0;
            TTF_SetFontSize(LAYOUT.fonts[0], OPENECS_FONT_SIZE);
            TTF_GetStringSize(LAYOUT.fonts[0], title, 0, &titleWidth, NULL);

            if (group->locked)
            {
                TTF_GetStringSize(LAYOUT.fonts[0], OPENECS_LOCKED_MARK, 0, &lockedWidth, NULL);
                lockedWidth += (int)OPENECS_MENU_ITEM_PADDING;
            }

            f32 width = SDL_min((f32)(titleWidth + lockedWidth) + 2.0f * OPENECS_MENU_ITEM_PADDING, group->width);

            CLAY(CLAY_ID("Grip"), {
                .layout = {
                    .sizing = {CLAY_SIZING_FIXED(width), CLAY_SIZING_FIXED(OPENECS_GRIP_HEIGHT)},
                    .padding = {(u16)OPENECS_MENU_ITEM_PADDING, (u16)OPENECS_MENU_ITEM_PADDING, 0, 0},
                    .childGap = (u16)OPENECS_MENU_ITEM_PADDING,
                    .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER},
                },
                .backgroundColor = OPENECS_COLOR_OVERLAY,
                .cornerRadius = {0, 0, 6, 6},
                .border = {.color = OPENECS_COLOR_TEXT_DIM, .width = {1, 1, 0, 1, 0}},
                .clip = {.horizontal = true},
                .floating = {.attachTo = CLAY_ATTACH_TO_ROOT, .offset = {group->x + (group->width - width) / 2.0f, group->y}, .zIndex = 2},
            })
            {
                CLAY_TEXT(ECSI_LayoutClayText(title), CLAY_TEXT_CONFIG({.textColor = OPENECS_COLOR_TEXT, .fontSize = OPENECS_FONT_SIZE, .wrapMode = CLAY_TEXT_WRAP_NONE}));

                if (group->locked)
                {
                    CLAY_TEXT(CLAY_STRING(OPENECS_LOCKED_MARK), CLAY_TEXT_CONFIG({.textColor = OPENECS_COLOR_TEXT_DIM, .fontSize = OPENECS_FONT_SIZE, .wrapMode = CLAY_TEXT_WRAP_NONE}));
                }
            }
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

        for (usz level = 0; level < OPENECS_MENU_DEPTH && LAYOUT.menus[level].lines != NULL; level++)
        {
            const ECSI_MenuView *menu = &LAYOUT.menus[level];
            SDL_FRect rect = menu->rect;

            CLAY(CLAY_IDI("Menu", (u32)level), {
                .layout = {
                    .sizing = {CLAY_SIZING_FIXED(rect.w), CLAY_SIZING_FIXED(rect.h)},
                    .padding = CLAY_PADDING_ALL((u16)OPENECS_MENU_PADDING),
                    .layoutDirection = CLAY_TOP_TO_BOTTOM,
                },
                .backgroundColor = OPENECS_COLOR_OVERLAY,
                .cornerRadius = CLAY_CORNER_RADIUS(6),
                .border = {.color = OPENECS_COLOR_TEXT_DIM, .width = {1, 1, 1, 1, 0}},
                .floating = {.attachTo = CLAY_ATTACH_TO_ROOT, .offset = {rect.x, rect.y}, .zIndex = (i16)(5 + level)},
            })
            {
                // each entry has the height that ECSI_LayoutMenuItemAt counts with
                for (usz i = 0; i < menu->count; i++)
                {
                    CLAY_AUTO_ID({
                        .layout = {
                            .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(menu->itemHeight)},
                            .padding = {(u16)OPENECS_MENU_ITEM_PADDING, (u16)OPENECS_MENU_ITEM_PADDING, 0, 0},
                            .childGap = (u16)OPENECS_MENU_ITEM_PADDING,
                            .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                        },
                        .backgroundColor = i == menu->selected ? OPENECS_COLOR_SELECTED : (Clay_Color){0},
                        .cornerRadius = CLAY_CORNER_RADIUS(4),
                    })
                    {
                        CLAY_TEXT(ECSI_LayoutClayText(menu->lines[2 * i + 1]), CLAY_TEXT_CONFIG({.textColor = OPENECS_COLOR_TEXT, .fontSize = OPENECS_FONT_SIZE, .wrapMode = CLAY_TEXT_WRAP_NONE}));
                        CLAY_AUTO_ID({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)}}}) {}
                        CLAY_TEXT(ECSI_LayoutClayText(menu->lines[2 * i]), CLAY_TEXT_CONFIG({.textColor = OPENECS_COLOR_TEXT_DIM, .fontSize = OPENECS_FONT_SIZE, .wrapMode = CLAY_TEXT_WRAP_NONE}));
                    }
                }
            }
        }

        if (LAYOUT.prefixLines != NULL)
        {
            CLAY_AUTO_ID({
                .layout = {.padding = CLAY_PADDING_ALL(14), .childGap = 4, .layoutDirection = CLAY_TOP_TO_BOTTOM},
                .backgroundColor = OPENECS_COLOR_OVERLAY,
                .cornerRadius = CLAY_CORNER_RADIUS(6),
                .border = {.color = OPENECS_COLOR_ACCENT, .width = {1, 1, 1, 1, 0}},
                .floating = {.attachTo = CLAY_ATTACH_TO_ROOT, .offset = {24.0f, 24.0f}, .zIndex = 3},
            })
            {
                for (usz i = 0; i < LAYOUT.prefixLineCount; i++)
                {
                    // a line without a key is a heading
                    if (LAYOUT.prefixLines[2 * i] == NULL)
                    {
                        CLAY_AUTO_ID({.layout = {.padding = {0, 0, i == 0 ? 0 : 6, 0}}})
                        {
                            CLAY_TEXT(ECSI_LayoutClayText(LAYOUT.prefixLines[2 * i + 1]), CLAY_TEXT_CONFIG({.textColor = OPENECS_COLOR_TEXT_DIM, .fontSize = OPENECS_FONT_SIZE}));
                        }

                        continue;
                    }

                    CLAY_AUTO_ID({.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT}})
                    {
                        CLAY_AUTO_ID({.layout = {.sizing = {CLAY_SIZING_FIXED(OPENECS_PREFIX_KEY_COLUMN), CLAY_SIZING_FIT(0)}}})
                        {
                            CLAY_TEXT(ECSI_LayoutClayText(LAYOUT.prefixLines[2 * i]), CLAY_TEXT_CONFIG({.textColor = OPENECS_COLOR_ACCENT, .fontSize = OPENECS_FONT_SIZE}));
                        }

                        CLAY_TEXT(ECSI_LayoutClayText(LAYOUT.prefixLines[2 * i + 1]), CLAY_TEXT_CONFIG({.textColor = OPENECS_COLOR_TEXT, .fontSize = OPENECS_FONT_SIZE}));
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

static void ECSI_LayoutDrawGroup(ECSI_Node *group, void *userData)
{
    u64 *nowTicks = userData;

    if (arrlenu(group->panels) > 0)
    {
        ECSI_PanelDraw(group->panels[group->shown], LAYOUT.renderer, *nowTicks);
    }
}

static void ECSI_LayoutShowGroup(ECSI_Node *group, void *userData)
{
    (void)userData;

    if (arrlenu(group->panels) > 0)
    {
        ECSI_PanelShow(group->panels[group->shown], LAYOUT.renderer);
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

/// @brief Reads ecs.vsync and sets the renderer's vsync. Also the Changed function of ecs.vsync.
static void ECSI_LayoutReadVsync(void *data)
{
    (void)data;

    i64 percent = SDL_clamp(ECSValue_GetInteger(ECSSetting_Get("ecs.vsync"), 100), 0, 100);
    LAYOUT.framePercent = percent;
    LAYOUT.frameNeeded = true;

    // a whole fraction of the refresh rate, such as 50%, lets the display wait for every second refresh; other rates wait for every refresh and are limited by the core
    if (percent == 0)
    {
        SDL_SetRenderVSync(LAYOUT.renderer, SDL_RENDERER_VSYNC_DISABLED);
    }
    else if (100 % percent != 0 || !SDL_SetRenderVSync(LAYOUT.renderer, (int)(100 / percent)))
    {
        SDL_SetRenderVSync(LAYOUT.renderer, 1);
    }
}

#pragma endregion Source Only

SHUResult ECSI_LayoutInitialize(const char *title, const char *fontPath)
{
    SDL_assert(title != NULL);
    SDL_assert(fontPath != NULL);

    SHU_ReturnResult(ECSI_LayoutOpen(title, fontPath), ECSI_LayoutTerminate(););

    const ECSSettingDesc vsync = {
        .name = "ecs.vsync",
        .type = ECSSettingType_Integer,
        .description = "Frame rate in percent of the display's refresh rate: 100 waits for every refresh, 50 for every second one, 0 turns vsync off",
        .defaultInteger = 100,
        .Changed = ECSI_LayoutReadVsync,
    };

    SHU_ReturnResult(ECSI_SettingsDeclareCore(&vsync), ECSI_LayoutTerminate(););
    ECSI_LayoutReadVsync(NULL);

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

    for (usz i = 0; i < arrlenu(LAYOUT.closed); i++)
    {
        ECSValue_Destroy(&LAYOUT.closed[i].saved);
    }

    arrfree(LAYOUT.closed);

    for (usz i = 0; i < SDL_arraysize(LAYOUT.cursors); i++)
    {
        SDL_DestroyCursor(LAYOUT.cursors[i]);
    }

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
    LAYOUT.gripGroup = NULL;
    LAYOUT.dragSplit = NULL;
    LAYOUT.dragPanel = NULL;
    LAYOUT.dragging = false;
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

i32 ECSI_LayoutGetFrameWait(void)
{
    // a window that cannot be seen is not drawn; showing it again asks for a frame
    if ((SDL_GetWindowFlags(LAYOUT.window) & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED | SDL_WINDOW_OCCLUDED)) != 0)
    {
        return -1;
    }

    bool wants = LAYOUT.frameNeeded;
    ECSI_LayoutForEachGroup(ECSI_LayoutWantsFrameIn, &wants);

    if (!wants)
    {
        return -1;
    }

    u64 next = LAYOUT.lastFrameTicks + LAYOUT.frameNanoseconds;
    u64 now = SDL_GetTicksNS();
    return now >= next ? 0 : (i32)((next - now + SDL_NS_PER_MS - 1) / SDL_NS_PER_MS);
}

void ECSI_LayoutRender(u64 nowTicks)
{
    // some drivers accept vsync but do not wait for it, so frames are limited a little above the rate ecs.vsync asks for; a working vsync still sets the pace
    const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(LAYOUT.window));
    f32 rate = mode != NULL && mode->refresh_rate > 0.0f ? mode->refresh_rate : OPENECS_FALLBACK_FRAME_RATE;
    f64 limit = (f64)rate * (f64)LAYOUT.framePercent / 100.0 * (f64)OPENECS_FRAME_RATE_MARGIN;
    LAYOUT.frameNanoseconds = LAYOUT.framePercent == 0 ? 0 : (u64)((f64)SDL_NS_PER_SECOND / limit);
    LAYOUT.lastFrameTicks = nowTicks;
    ECSI_LayoutUpdate();

    // the interface's commands point to the panels' titles, so panels draw first; their Draw may change a title
    ECSI_LayoutForEachGroup(ECSI_LayoutDrawGroup, &nowTicks);
    Clay_RenderCommandArray commands = ECSI_LayoutDeclareInterface();
    ECSI_LayoutFitTabs();

    SDL_SetRenderDrawColor(LAYOUT.renderer, OPENECS_COLOR_BACKGROUND);
    SDL_RenderClear(LAYOUT.renderer);
    ECSI_LayoutForEachGroup(ECSI_LayoutShowGroup, NULL);
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

    for (usz i = 0; i < arrlenu(LAYOUT.tabs); i++)
    {
        ECSI_Node *group = LAYOUT.tabs[i].group;
        ECSPanel panel = group->panels[LAYOUT.tabs[i].index];

        // closing may ask about unsaved work; the tabs of the last frame are not used after it
        if (ECSI_LayoutOnElement(CLAY_IDI("TabClose", (u32)i), x, y))
        {
            ECSLayout_Close(panel);
            return true;
        }

        if (ECSI_LayoutOnElement(CLAY_IDI("Tab", (u32)i), x, y))
        {
            group->shown = LAYOUT.tabs[i].index;
            ECSI_LayoutSetFocus(panel);

            // a locked panel cannot be dragged, but its tab still shows it
            if (!group->locked)
            {
                ECSI_LayoutArmDrag(panel, false, false, x, y);
            }

            return true;
        }
    }

    // the empty part of a tab row drags the whole group
    ECSI_Node *group = ECSI_LayoutTabRowGroupAt(x, y);

    if (group != NULL)
    {
        ECSI_LayoutSetFocus(group->panels[group->shown]);

        if (!group->locked)
        {
            ECSI_LayoutArmDrag(group->panels[group->shown], true, false, x, y);
        }

        return true;
    }

    // a locked group's grip only opens the menu
    if (LAYOUT.gripGroup != NULL && ECSI_LayoutOnElement(CLAY_ID("Grip"), x, y))
    {
        ECSI_LayoutArmDrag(LAYOUT.gripGroup->panels[0], false, true, x, y);
        LAYOUT.dragLocked = LAYOUT.gripGroup->locked;
        return true;
    }

    return false;
}

/// @brief Sets the pointer's shape, if it changed.
static void ECSI_LayoutSetCursor(SDL_SystemCursor cursor)
{
    if (cursor == LAYOUT.cursor)
    {
        return;
    }

    if (LAYOUT.cursors[cursor] == NULL)
    {
        LAYOUT.cursors[cursor] = SDL_CreateSystemCursor(cursor);
    }

    if (LAYOUT.cursors[cursor] != NULL && SDL_SetCursor(LAYOUT.cursors[cursor]))
    {
        LAYOUT.cursor = cursor;
    }
}

bool ECSI_LayoutPointerMove(f32 x, f32 y)
{
    ECSI_Workspace *workspace = ECSI_LayoutCurrent();

    // the pointer shows a divider can be dragged, and while a panel is dragged
    ECSI_DividerHit divider = {x, y, LAYOUT.dragSplit, 0};

    if (LAYOUT.dragPanel == NULL && workspace != NULL)
    {
        ECSI_LayoutFindDivider(workspace->maximized != NULL ? NULL : workspace->tree, &divider);
    }

    ECSI_LayoutSetCursor(divider.split != NULL ? (divider.split->vertical ? SDL_SYSTEM_CURSOR_NS_RESIZE : SDL_SYSTEM_CURSOR_EW_RESIZE)
                         : LAYOUT.dragging     ? SDL_SYSTEM_CURSOR_MOVE
                                               : SDL_SYSTEM_CURSOR_DEFAULT);

    if (LAYOUT.dragSplit != NULL)
    {
        ECSI_LayoutDragDivider(x, y);
        return true;
    }

    if (LAYOUT.dragPanel != NULL)
    {
        bool wasDragging = LAYOUT.dragging;
        bool moved = SDL_fabsf(x - LAYOUT.dragStartX) + SDL_fabsf(y - LAYOUT.dragStartY) >= OPENECS_DRAG_THRESHOLD;

        // a locked grip is never dragged, and pulling it is not a click
        if (moved && LAYOUT.dragLocked)
        {
            ECSI_LayoutCancelDrag();
            return true;
        }

        LAYOUT.dragging = LAYOUT.dragging || moved;

        if (LAYOUT.dragging)
        {
            // a drop that changes nothing is not highlighted
            LAYOUT.drop = ECSI_LayoutFindDrop(x, y, &LAYOUT.dropRect);
            LAYOUT.drop = ECSI_LayoutDropChanges(&LAYOUT.drop) ? LAYOUT.drop : (ECSI_Drop){0};
            LAYOUT.frameNeeded = true;
        }

        if (LAYOUT.dragging && !wasDragging)
        {
            ECSI_LayoutSetCursor(SDL_SYSTEM_CURSOR_MOVE);
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

ECSPanel ECSI_LayoutPointerUp(void)
{
    ECSI_Workspace *workspace = ECSI_LayoutCurrent();
    ECSPanel clicked = LAYOUT.dragFromGrip && !LAYOUT.dragging ? LAYOUT.dragPanel : NULL;

    if (LAYOUT.dragging && workspace != NULL)
    {
        ECSI_Node *group = ECSI_LayoutFindGroup(workspace->tree, LAYOUT.dragPanel);

        if (LAYOUT.dragGroup && group != NULL)
        {
            ECSI_LayoutMoveGroup(workspace, group, &LAYOUT.drop, true);
        }
        else
        {
            ECSI_LayoutMove(workspace, LAYOUT.dragPanel, &LAYOUT.drop, true);
        }
    }

    ECSI_LayoutCancelDrag();
    LAYOUT.dragSplit = NULL;
    ECSI_LayoutSetCursor(SDL_SYSTEM_CURSOR_DEFAULT);
    return clicked;
}

bool ECSI_LayoutCancelDrag(void)
{
    bool dragging = LAYOUT.dragging;
    LAYOUT.dragPanel = NULL;
    LAYOUT.dragGroup = false;
    LAYOUT.dragFromGrip = false;
    LAYOUT.dragLocked = false;
    LAYOUT.dragging = false;
    LAYOUT.drop = (ECSI_Drop){0};
    LAYOUT.frameNeeded = LAYOUT.frameNeeded || dragging;
    return dragging;
}

ECSPanel ECSI_LayoutTabAt(f32 x, f32 y)
{
    for (usz i = 0; i < arrlenu(LAYOUT.tabs); i++)
    {
        if (ECSI_LayoutOnElement(CLAY_IDI("Tab", (u32)i), x, y))
        {
            return LAYOUT.tabs[i].group->panels[LAYOUT.tabs[i].index];
        }
    }

    if (LAYOUT.gripGroup != NULL && ECSI_LayoutOnElement(CLAY_ID("Grip"), x, y))
    {
        return LAYOUT.gripGroup->panels[0];
    }

    return NULL;
}

ECSPanel ECSI_LayoutTabRowAt(f32 x, f32 y)
{
    ECSI_Node *group = ECSI_LayoutTabRowGroupAt(x, y);
    return group == NULL ? NULL : group->panels[group->shown];
}

bool ECSI_LayoutScrollTabs(f32 x, f32 y, f32 steps)
{
    ECSI_Node *group = ECSI_LayoutTabRowGroupAt(x, y);

    if (group == NULL)
    {
        return false;
    }

    group->tabScroll = SDL_clamp(group->tabScroll + steps * OPENECS_TAB_SCROLL_STEP, 0.0f, SDL_max(0.0f, group->tabsWidth - group->width));
    LAYOUT.frameNeeded = true;
    return true;
}

void ECSI_LayoutShowMenu(usz level, SDL_FRect anchor, const char *const *lines, usz count)
{
    SDL_assert(level < OPENECS_MENU_DEPTH);

    // a menu that closes or changes closes its submenus
    for (usz i = level + 1; i < OPENECS_MENU_DEPTH; i++)
    {
        LAYOUT.menus[i] = (ECSI_MenuView){0};
    }

    ECSI_MenuView *menu = &LAYOUT.menus[level];
    *menu = (ECSI_MenuView){0};
    LAYOUT.frameNeeded = true;

    if (count == 0)
    {
        return;
    }

    menu->lines = lines;
    menu->count = count;

    // the menu's size is measured here, so it can be kept inside the OS window
    TTF_Font *font = LAYOUT.fonts[0];
    TTF_SetFontSize(font, OPENECS_FONT_SIZE);
    f32 width = 0.0f;

    for (usz i = 0; i < count; i++)
    {
        int labelWidth = 0;
        int keyWidth = 0;
        TTF_GetStringSize(font, lines[2 * i + 1], 0, &labelWidth, NULL);
        TTF_GetStringSize(font, lines[2 * i], 0, &keyWidth, NULL);
        width = SDL_max(width, (f32)(labelWidth + keyWidth) + 3.0f * OPENECS_MENU_ITEM_PADDING);
    }

    menu->itemHeight = (f32)TTF_GetFontHeight(font) + 6.0f;
    width += 2.0f * OPENECS_MENU_PADDING;
    f32 height = (f32)count * menu->itemHeight + 2.0f * OPENECS_MENU_PADDING;

    // it opens to the right of its anchor, or to the left if there is no room
    f32 x = anchor.x + anchor.w + width <= LAYOUT.width ? anchor.x + anchor.w : anchor.x - width;
    f32 y = anchor.y - (level == 0 ? 0.0f : OPENECS_MENU_PADDING);
    menu->rect = (SDL_FRect){
        SDL_max(0.0f, SDL_min(x, LAYOUT.width - width)),
        SDL_max(0.0f, SDL_min(y, LAYOUT.height - height)),
        width,
        height,
    };
}

void ECSI_LayoutSelectMenuItem(usz level, usz index)
{
    SDL_assert(level < OPENECS_MENU_DEPTH);

    LAYOUT.menus[level].selected = index;
    LAYOUT.frameNeeded = true;
}

SDL_FRect ECSI_LayoutMenuItemRect(usz level, usz index)
{
    SDL_assert(level < OPENECS_MENU_DEPTH);

    const ECSI_MenuView *menu = &LAYOUT.menus[level];
    return (SDL_FRect){
        menu->rect.x + OPENECS_MENU_PADDING,
        menu->rect.y + OPENECS_MENU_PADDING + (f32)index * menu->itemHeight,
        menu->rect.w - 2.0f * OPENECS_MENU_PADDING,
        menu->itemHeight,
    };
}

bool ECSI_LayoutMenuItemAt(f32 x, f32 y, usz *retLevel, usz *retIndex)
{
    SDL_assert(retLevel != NULL);
    SDL_assert(retIndex != NULL);

    // submenus are drawn over their parents, so the deepest is checked first
    for (usz level = OPENECS_MENU_DEPTH; level > 0; level--)
    {
        for (usz i = 0; i < LAYOUT.menus[level - 1].count; i++)
        {
            SDL_FRect item = ECSI_LayoutMenuItemRect(level - 1, i);

            if (ECSI_LayoutContains(x, y, item.x, item.y, item.w, item.h))
            {
                *retLevel = level - 1;
                *retIndex = i;
                return true;
            }
        }
    }

    return false;
}

bool ECSI_LayoutMenuContains(f32 x, f32 y)
{
    for (usz level = 0; level < OPENECS_MENU_DEPTH; level++)
    {
        const SDL_FRect *rect = &LAYOUT.menus[level].rect;

        if (LAYOUT.menus[level].lines != NULL && ECSI_LayoutContains(x, y, rect->x, rect->y, rect->w, rect->h))
        {
            return true;
        }
    }

    return false;
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

    ECSI_Drop drop = {.zone = dx < 0 ? ECSI_Zone_WindowLeft : dx > 0 ? ECSI_Zone_WindowRight : dy < 0 ? ECSI_Zone_WindowTop : ECSI_Zone_WindowBottom};

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
        LAYOUT.gripGroup = NULL;
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
    LAYOUT.gripGroup = NULL;
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

SDL_Window *ECSI_LayoutGetWindow(void)
{
    return LAYOUT.window;
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

void ECSI_LayoutShowPrefixKeys(const char *const *lines, usz count)
{
    LAYOUT.prefixLines = count == 0 ? NULL : lines;
    LAYOUT.prefixLineCount = count;
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
