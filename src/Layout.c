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
} LAYOUT = {0};

/// @brief Width of the key column in the list of prefix keys.
#define OPENECS_PREFIX_KEY_COLUMN 64.0f

/// @brief The keys shown after the core prefix, as key and action. Mirrors the default of the setting ecs.prefix_keys.
static const char *const ECSI_PREFIX_KEY_LINES[][2] = {
    {"Arrows", "move focus"},
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

/// @brief Removes a node from its workspace tree and frees it, then replaces a split left with one child by that child.
static void ECSI_LayoutRemoveNode(ECSI_Workspace *workspace, ECSI_Node *node)
{
    ECSI_Node *parent = node->parent;
    arrfree(node->children);
    arrfree(node->panels);
    SDL_free(node);

    if (parent == NULL)
    {
        workspace->tree = NULL;
        return;
    }

    usz index = 0;

    while (parent->children[index] != node)
    {
        index++;
    }

    arrdel(parent->children, index);

    if (arrlenu(parent->children) != 1)
    {
        return;
    }

    ECSI_Node *only = parent->children[0];
    ECSI_Node *grandparent = parent->parent;
    only->fixedSize = parent->fixedSize;
    only->share = parent->share;
    only->parent = grandparent;

    if (grandparent == NULL)
    {
        workspace->tree = only;
    }
    else
    {
        for (usz i = 0; i < arrlenu(grandparent->children); i++)
        {
            if (grandparent->children[i] == parent)
            {
                grandparent->children[i] = only;
            }
        }
    }

    arrfree(parent->children);
    SDL_free(parent);
}

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

SHUResult ECSI_LayoutWorkspaceAdd(const char *name, ECSI_Node *tree)
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
        .maximized = NULL,
        .focus = ECSI_LayoutFirstPanel(tree),
    };

    arrput(LAYOUT.workspaces, workspace);

    LAYOUT.frameNeeded = true;
    return SHUResult_Ok;
}

void ECSI_LayoutWorkspaceSwitch(u32 index)
{
    if (index >= arrlenu(LAYOUT.workspaces) || index == LAYOUT.current)
    {
        return;
    }

    LAYOUT.current = index;
    LAYOUT.gripGroup = NULL;
    LAYOUT.dragSplit = NULL;
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
            return true;
        }
    }

    // the grip will move the panel when docking by dragging is implemented
    return LAYOUT.gripGroup != NULL && y < LAYOUT.gripGroup->y + OPENECS_GRIP_ZONE;
}

bool ECSI_LayoutPointerMove(f32 x, f32 y)
{
    if (LAYOUT.dragSplit != NULL)
    {
        ECSI_LayoutDragDivider(x, y);
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
    LAYOUT.dragSplit = NULL;
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
    ECSI_PanelDestroy(&panel);

    if (arrlenu(group->panels) == 0)
    {
        if (workspace->maximized == group)
        {
            workspace->maximized = NULL;
        }

        if (LAYOUT.gripGroup == group)
        {
            LAYOUT.gripGroup = NULL;
        }

        ECSI_LayoutRemoveNode(workspace, group);
        workspace->focus = ECSI_LayoutFirstPanel(workspace->tree);
    }
    else
    {
        group->shown = group->shown >= arrlenu(group->panels) ? arrlenu(group->panels) - 1 : group->shown;
        workspace->focus = group->panels[group->shown];
    }

    LAYOUT.dragSplit = NULL;
    LAYOUT.frameNeeded = true;
}

void ECSI_LayoutShowPrefixKeys(bool show)
{
    LAYOUT.showPrefixKeys = show;
    LAYOUT.frameNeeded = true;
}
