#include "interface/Window.h"

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
/// @brief How far one step of the wheel scrolls a tab row.
#define OPENECS_TAB_SCROLL_STEP 40.0f
/// @brief Width of the mark between tabs where a dragged panel is inserted.
#define OPENECS_TAB_GAP_WIDTH 3.0f
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

/// @brief A tab drawn in the last frame, so a click can find it.
typedef struct ECSITabRef
{
    ECSINode *group;
    usz index;
} ECSITabRef;

/// @brief A shown menu. Each line pair is an entry's key text and its label.
typedef struct ECSIMenuView
{
    const char *const *lines; // NULL when this menu is closed
    usz count;
    usz selected;
    SDL_FRect rect;
    f32 itemHeight;
} ECSIMenuView;

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

    i64 framePercent;     // frame rate as a percentage of the refresh rate, from ecs.vsync; 0 for no vsync and no limit
    u64 frameNanoseconds; // shortest time between frames, from the display's refresh rate; 0 for no limit
    u64 lastFrameTicks;
    const char *const *prefixLines; // keys after the prefix and what they do, in pairs, or NULL when the prefix is not pressed
    usz prefixLineCount;
    ECSITabRef *tabs; // stb_ds array

    ECSINode *gripGroup; // lone group whose grip is shown, or NULL
    ECSINode *dragSplit; // split whose divider is dragged, or NULL
    usz dragDivider;      // the dragged divider follows this child

    ECSPanel dragPanel; // panel pressed on its tab or grip, or NULL; it is dragged once the pointer moves far enough
    bool dragGroup;     // the whole group of dragPanel is dragged, pressed on its tab row
    bool dragFromGrip;  // the press was on a grip; a click without dragging opens the panel's menu
    bool dragLocked;    // the pressed grip is a locked group's, so it is never dragged
    bool dragging;
    f32 dragStartX;
    f32 dragStartY;
    ECSIDrop drop;     // where the dragged panel lands now
    SDL_FRect dropRect; // the highlight of the drop place

    SDL_Cursor *cursors[SDL_SYSTEM_CURSOR_COUNT]; // made when first used
    SDL_SystemCursor cursor;                      // the pointer's shape now

    ECSIMenuView menus[OPENECS_MENU_DEPTH]; // the panel menu, then its open submenus
} WINDOW = {0};

/// @brief Width of the key column in the list of prefix keys.
#define OPENECS_PREFIX_KEY_COLUMN 104.0f

static Clay_String ECSIWindow_ClayText(const char *text)
{
    return (Clay_String){.isStaticallyAllocated = false, .length = (int32_t)SDL_strlen(text), .chars = text};
}

static void ECSIWindow_ClayError(Clay_ErrorData error)
{
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Clay: %.*s", (int)error.errorText.length, error.errorText.chars);
}

/// @brief Measures text for Clay, with the core's font.
static Clay_Dimensions ECSIWindow_MeasureText(Clay_StringSlice text, Clay_TextElementConfig *config, void *userData)
{
    (void)userData;

    TTF_Font *font = WINDOW.fonts[config->fontId];
    int width = 0;
    int height = 0;

    TTF_SetFontSize(font, config->fontSize);
    TTF_GetStringSize(font, text.chars, (size_t)text.length, &width, &height);

    return (Clay_Dimensions){(f32)width, (f32)height};
}

/// @brief Reads the size of the OS window.
static void ECSIWindow_ReadSize(void)
{
    int width = 0;
    int height = 0;
    SDL_GetWindowSize(WINDOW.window, &width, &height);

    WINDOW.width = (f32)width;
    WINDOW.height = (f32)height;
}

static bool ECSIWindow_Contains(f32 x, f32 y, f32 left, f32 top, f32 width, f32 height)
{
    return x >= left && y >= top && x < left + width && y < top + height;
}

/// @brief Forgets the nodes the window points to; the layout calls it after the trees change.
static void ECSIWindow_Forget(void)
{
    WINDOW.gripGroup = NULL;
    WINDOW.dragSplit = NULL;
    WINDOW.dragPanel = NULL;
    WINDOW.dragging = false;
}

#pragma region Dragging

/// @brief Finds the group whose tab row is at a point.
static ECSINode *ECSIWindow_TabRowGroupAt(f32 x, f32 y)
{
    ECSINode *group = ECSILayout_GroupAt(x, y);
    return group != NULL && arrlenu(group->panels) >= 2 && y < group->y + OPENECS_TAB_ROW_HEIGHT ? group : NULL;
}

/// @brief Finds the gap between a group's tabs nearest to a point, from the tabs drawn in the last frame.
static ECSIDrop ECSIWindow_FindTabGap(ECSINode *group, f32 x, SDL_FRect *retRect)
{
    ECSIDrop drop = {.zone = ECSIZone_Tabs, .group = group, .index = 0};
    f32 gapX = group->x;
    Clay_SetCurrentContext(WINDOW.clay);

    for (usz i = 0; i < arrlenu(WINDOW.tabs); i++)
    {
        Clay_ElementData tab = Clay_GetElementData(CLAY_IDI("Tab", (u32)i));

        if (WINDOW.tabs[i].group != group || !tab.found)
        {
            continue;
        }

        if (x > tab.boundingBox.x + tab.boundingBox.width / 2.0f)
        {
            drop.index = WINDOW.tabs[i].index + 1;
            gapX = tab.boundingBox.x + tab.boundingBox.width;
        }
        else if (WINDOW.tabs[i].index == drop.index)
        {
            gapX = tab.boundingBox.x;
        }
    }

    *retRect = (SDL_FRect){gapX - OPENECS_TAB_GAP_WIDTH / 2.0f, group->y, OPENECS_TAB_GAP_WIDTH, OPENECS_TAB_ROW_HEIGHT};
    return drop;
}

/// @brief Finds where a panel dragged to a point lands, and the rectangle to highlight. The checks follow DESIGN 6.7.
static ECSIDrop ECSIWindow_FindDrop(f32 x, f32 y, SDL_FRect *retRect)
{
    f32 width = WINDOW.width;
    f32 height = WINDOW.height;
    *retRect = (SDL_FRect){0};

    // outside the window, the panel would pop out; pop-out windows are not implemented yet
    if (!ECSIWindow_Contains(x, y, 0.0f, 0.0f, width, height))
    {
        return (ECSIDrop){0};
    }

    if (x < OPENECS_DOCK_EDGE || x >= width - OPENECS_DOCK_EDGE || y < OPENECS_DOCK_EDGE || y >= height - OPENECS_DOCK_EDGE)
    {
        ECSIZone zone = x < OPENECS_DOCK_EDGE            ? ECSIZone_WindowLeft
                         : x >= width - OPENECS_DOCK_EDGE ? ECSIZone_WindowRight
                         : y < OPENECS_DOCK_EDGE          ? ECSIZone_WindowTop
                                                          : ECSIZone_WindowBottom;

        *retRect = zone == ECSIZone_WindowLeft    ? (SDL_FRect){0.0f, 0.0f, width / 4.0f, height}
                   : zone == ECSIZone_WindowRight ? (SDL_FRect){width * 0.75f, 0.0f, width / 4.0f, height}
                   : zone == ECSIZone_WindowTop   ? (SDL_FRect){0.0f, 0.0f, width, height / 4.0f}
                                                   : (SDL_FRect){0.0f, height * 0.75f, width, height / 4.0f};
        return (ECSIDrop){.zone = zone};
    }

    ECSINode *group = ECSILayout_GroupAt(x, y);

    if (group == NULL || arrlenu(group->panels) == 0)
    {
        return (ECSIDrop){0};
    }

    // a locked group accepts no dropped panels
    if (arrlenu(group->panels) >= 2 && y < group->y + OPENECS_TAB_ROW_HEIGHT)
    {
        return group->locked ? (ECSIDrop){0} : ECSIWindow_FindTabGap(group, x, retRect);
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
    ECSIZone zone = ECSIZone_Center;
    f32 nearest = 1.0f;

    for (usz i = 0; i < SDL_arraysize(distances); i++)
    {
        if (distances[i] < nearest)
        {
            nearest = distances[i];
            zone = (ECSIZone)(ECSIZone_Left + (i32)i);
        }
    }

    f32 halfWidth = panel->width / 2.0f;
    f32 halfHeight = panel->height / 2.0f;
    *retRect = zone == ECSIZone_Left     ? (SDL_FRect){panel->x, panel->y, halfWidth, panel->height}
               : zone == ECSIZone_Right  ? (SDL_FRect){panel->x + halfWidth, panel->y, halfWidth, panel->height}
               : zone == ECSIZone_Top    ? (SDL_FRect){panel->x, panel->y, panel->width, halfHeight}
               : zone == ECSIZone_Bottom ? (SDL_FRect){panel->x, panel->y + halfHeight, panel->width, halfHeight}
                                          : (SDL_FRect){panel->x, panel->y, panel->width, panel->height};
    if (zone == ECSIZone_Center && group->locked)
    {
        *retRect = (SDL_FRect){0};
        return (ECSIDrop){0};
    }

    return (ECSIDrop){.zone = zone, .group = group};
}

/// @brief Checks whether dropping the dragged panel, or its group, at a place would change the layout.
static bool ECSIWindow_DropChanges(const ECSIDrop *drop)
{
    ECSINode *source = ECSILayout_GroupOf(WINDOW.dragPanel);

    if (source == NULL || drop->zone == ECSIZone_None)
    {
        return false;
    }

    // a whole group moves when the group is dragged or holds only the dragged panel
    bool whole = WINDOW.dragGroup || arrlenu(source->panels) == 1;

    if (drop->zone >= ECSIZone_WindowLeft)
    {
        return !whole || ECSILayout_GetTree() != source;
    }

    if (drop->group != source)
    {
        return true;
    }

    if (whole || drop->zone == ECSIZone_Center)
    {
        return false;
    }

    // a gap next to the panel's own tab leaves it where it is
    if (drop->zone == ECSIZone_Tabs)
    {
        usz index = 0;

        while (source->panels[index] != WINDOW.dragPanel)
        {
            index++;
        }

        return drop->index != index && drop->index != index + 1;
    }

    return true;
}

/// @brief Remembers a press on a panel's tab or grip, or on its group's tab row; the panel or group is dragged once the pointer moves far enough.
static void ECSIWindow_ArmDrag(ECSPanel panel, bool group, bool grip, f32 x, f32 y)
{
    WINDOW.dragPanel = panel;
    WINDOW.dragGroup = group;
    WINDOW.dragFromGrip = grip;
    WINDOW.dragLocked = false;
    WINDOW.dragging = false;
    WINDOW.dragStartX = x;
    WINDOW.dragStartY = y;
}

/// @brief Sets the pointer's shape, if it changed.
static void ECSIWindow_SetCursor(SDL_SystemCursor cursor)
{
    if (cursor == WINDOW.cursor)
    {
        return;
    }

    if (WINDOW.cursors[cursor] == NULL)
    {
        WINDOW.cursors[cursor] = SDL_CreateSystemCursor(cursor);
    }

    if (WINDOW.cursors[cursor] != NULL && SDL_SetCursor(WINDOW.cursors[cursor]))
    {
        WINDOW.cursor = cursor;
    }
}

#pragma endregion Dragging

#pragma region Interface

typedef struct ECSIGripHit
{
    f32 x;
    f32 y;
    ECSINode *group;
} ECSIGripHit;

static void ECSIWindow_FindGripGroup(ECSINode *group, void *userData)
{
    ECSIGripHit *hit = userData;

    if (arrlenu(group->panels) == 1 && ECSIWindow_Contains(hit->x, hit->y, group->x, group->y, group->width, OPENECS_GRIP_ZONE))
    {
        hit->group = group;
    }
}

/// @brief Checks whether a point is on an element that Clay laid out in the last frame.
static bool ECSIWindow_OnElement(Clay_ElementId id, f32 x, f32 y)
{
    Clay_SetCurrentContext(WINDOW.clay);
    Clay_ElementData element = Clay_GetElementData(id);
    return element.found && ECSIWindow_Contains(x, y, element.boundingBox.x, element.boundingBox.y, element.boundingBox.width, element.boundingBox.height);
}

/// @brief Declares a group's tab row, or its placeholder text, for Clay.
static void ECSIWindow_DeclareGroup(ECSINode *group, void *userData)
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
                CLAY(CLAY_IDI("Tab", (u32)arrlenu(WINDOW.tabs)), {
                                                                     .layout = {
                                                                         .sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_GROW(0)},
                                                                         .padding = {12, 12, 0, 0},
                                                                         .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                                                     },
                                                                     .backgroundColor = i == group->shown ? OPENECS_COLOR_TAB_SHOWN : OPENECS_COLOR_TAB,
                                                                 })
                {
                    CLAY_TEXT(ECSIWindow_ClayText(group->panels[i]->title),
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
                        CLAY(CLAY_IDI("TabClose", (u32)arrlenu(WINDOW.tabs)), {.layout = {.padding = {8, 0, 0, 0}}})
                        {
                            CLAY_TEXT(CLAY_STRING("\u00D7"), CLAY_TEXT_CONFIG({.textColor = OPENECS_COLOR_TEXT_DIM, .fontSize = OPENECS_FONT_SIZE, .wrapMode = CLAY_TEXT_WRAP_NONE}));
                        }
                    }
                }

                arrput(WINDOW.tabs, ((ECSITabRef){group, i}));
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
            CLAY_TEXT(ECSIWindow_ClayText(panel->typeName), CLAY_TEXT_CONFIG({.textColor = OPENECS_COLOR_TEXT, .fontSize = OPENECS_FONT_SIZE}));

            if (panel->fault != NULL)
            {
                CLAY_TEXT(ECSIWindow_ClayText(panel->fault), CLAY_TEXT_CONFIG({.textColor = OPENECS_COLOR_TEXT_DIM, .fontSize = OPENECS_FONT_SIZE}));
            }
        }
    }
}

/// @brief Measures each tab row from the tabs Clay just laid out, keeps its scroll inside it, and scrolls a newly shown tab into view.
static void ECSIWindow_FitTabs(void)
{
    Clay_SetCurrentContext(WINDOW.clay);

    for (usz i = 0; i < arrlenu(WINDOW.tabs);)
    {
        // the tabs of one group are next to each other in the list
        ECSINode *group = WINDOW.tabs[i].group;
        f32 left = 0.0f;
        f32 right = 0.0f;
        Clay_BoundingBox shown = {0};

        for (; i < arrlenu(WINDOW.tabs) && WINDOW.tabs[i].group == group; i++)
        {
            Clay_ElementData tab = Clay_GetElementData(CLAY_IDI("Tab", (u32)i));
            left = WINDOW.tabs[i].index == 0 ? tab.boundingBox.x : left;
            right = tab.boundingBox.x + tab.boundingBox.width;
            shown = WINDOW.tabs[i].index == group->shown ? tab.boundingBox : shown;
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
            ECSILayout_RequestFrame();
        }
    }
}

/// @brief Declares the core's own interface for Clay and returns what to draw.
static Clay_RenderCommandArray ECSIWindow_DeclareInterface(void)
{
    Clay_SetCurrentContext(WINDOW.clay);
    Clay_SetLayoutDimensions((Clay_Dimensions){WINDOW.width, WINDOW.height});
    Clay_BeginLayout();

    arrfree(WINDOW.tabs);
    CLAY(CLAY_ID("Root"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}}})
    {
        ECSILayout_ForEachGroup(ECSIWindow_DeclareGroup, NULL);

        ECSPanel focus = ECSILayout_GetFocus();

        if (focus != NULL)
        {
            CLAY_AUTO_ID({
                .layout = {.sizing = {CLAY_SIZING_FIXED(focus->width), CLAY_SIZING_FIXED(focus->height)}},
                .border = {.color = OPENECS_COLOR_ACCENT, .width = {2, 2, 2, 2, 0}},
                .floating = {.attachTo = CLAY_ATTACH_TO_ROOT, .offset = {focus->x, focus->y}, .zIndex = 1, .pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH},
            })
            {
            }
        }

        // the grip shows the panel's title, centred on its top edge, and whether its group is locked
        if (WINDOW.gripGroup != NULL)
        {
            ECSINode *group = WINDOW.gripGroup;
            const char *title = group->panels[0]->title;
            int titleWidth = 0;
            int lockedWidth = 0;
            TTF_SetFontSize(WINDOW.fonts[0], OPENECS_FONT_SIZE);
            TTF_GetStringSize(WINDOW.fonts[0], title, 0, &titleWidth, NULL);

            if (group->locked)
            {
                TTF_GetStringSize(WINDOW.fonts[0], OPENECS_LOCKED_MARK, 0, &lockedWidth, NULL);
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
                CLAY_TEXT(ECSIWindow_ClayText(title), CLAY_TEXT_CONFIG({.textColor = OPENECS_COLOR_TEXT, .fontSize = OPENECS_FONT_SIZE, .wrapMode = CLAY_TEXT_WRAP_NONE}));

                if (group->locked)
                {
                    CLAY_TEXT(CLAY_STRING(OPENECS_LOCKED_MARK), CLAY_TEXT_CONFIG({.textColor = OPENECS_COLOR_TEXT_DIM, .fontSize = OPENECS_FONT_SIZE, .wrapMode = CLAY_TEXT_WRAP_NONE}));
                }
            }
        }

        if (WINDOW.dragging && WINDOW.drop.zone != ECSIZone_None)
        {
            SDL_FRect rect = WINDOW.dropRect;

            CLAY_AUTO_ID({
                .layout = {.sizing = {CLAY_SIZING_FIXED(rect.w), CLAY_SIZING_FIXED(rect.h)}},
                .backgroundColor = OPENECS_COLOR_DROP,
                .border = {.color = OPENECS_COLOR_ACCENT, .width = {2, 2, 2, 2, 0}},
                .floating = {.attachTo = CLAY_ATTACH_TO_ROOT, .offset = {rect.x, rect.y}, .zIndex = 4, .pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH},
            })
            {
            }
        }

        for (usz level = 0; level < OPENECS_MENU_DEPTH && WINDOW.menus[level].lines != NULL; level++)
        {
            const ECSIMenuView *menu = &WINDOW.menus[level];
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
                // each entry has the height that ECSIWindow_MenuItemAt counts with
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
                        CLAY_TEXT(ECSIWindow_ClayText(menu->lines[2 * i + 1]), CLAY_TEXT_CONFIG({.textColor = OPENECS_COLOR_TEXT, .fontSize = OPENECS_FONT_SIZE, .wrapMode = CLAY_TEXT_WRAP_NONE}));
                        CLAY_AUTO_ID({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)}}}) {}
                        CLAY_TEXT(ECSIWindow_ClayText(menu->lines[2 * i]), CLAY_TEXT_CONFIG({.textColor = OPENECS_COLOR_TEXT_DIM, .fontSize = OPENECS_FONT_SIZE, .wrapMode = CLAY_TEXT_WRAP_NONE}));
                    }
                }
            }
        }

        if (WINDOW.prefixLines != NULL)
        {
            CLAY_AUTO_ID({
                .layout = {.padding = CLAY_PADDING_ALL(14), .childGap = 4, .layoutDirection = CLAY_TOP_TO_BOTTOM},
                .backgroundColor = OPENECS_COLOR_OVERLAY,
                .cornerRadius = CLAY_CORNER_RADIUS(6),
                .border = {.color = OPENECS_COLOR_ACCENT, .width = {1, 1, 1, 1, 0}},
                .floating = {.attachTo = CLAY_ATTACH_TO_ROOT, .offset = {24.0f, 24.0f}, .zIndex = 3},
            })
            {
                for (usz i = 0; i < WINDOW.prefixLineCount; i++)
                {
                    // a line without a key is a heading
                    if (WINDOW.prefixLines[2 * i] == NULL)
                    {
                        CLAY_AUTO_ID({.layout = {.padding = {0, 0, i == 0 ? 0 : 6, 0}}})
                        {
                            CLAY_TEXT(ECSIWindow_ClayText(WINDOW.prefixLines[2 * i + 1]), CLAY_TEXT_CONFIG({.textColor = OPENECS_COLOR_TEXT_DIM, .fontSize = OPENECS_FONT_SIZE}));
                        }

                        continue;
                    }

                    CLAY_AUTO_ID({.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT}})
                    {
                        CLAY_AUTO_ID({.layout = {.sizing = {CLAY_SIZING_FIXED(OPENECS_PREFIX_KEY_COLUMN), CLAY_SIZING_FIT(0)}}})
                        {
                            CLAY_TEXT(ECSIWindow_ClayText(WINDOW.prefixLines[2 * i]), CLAY_TEXT_CONFIG({.textColor = OPENECS_COLOR_ACCENT, .fontSize = OPENECS_FONT_SIZE}));
                        }

                        CLAY_TEXT(ECSIWindow_ClayText(WINDOW.prefixLines[2 * i + 1]), CLAY_TEXT_CONFIG({.textColor = OPENECS_COLOR_TEXT, .fontSize = OPENECS_FONT_SIZE}));
                    }
                }
            }
        }
    }

    return Clay_EndLayout(0.0f);
}

#pragma endregion Interface

#pragma region Drawing

static void ECSIWindow_DrawGroup(ECSINode *group, void *userData)
{
    u64 *nowTicks = userData;

    if (arrlenu(group->panels) > 0)
    {
        ECSIPanel_Draw(group->panels[group->shown], WINDOW.renderer, *nowTicks);
    }
}

static void ECSIWindow_ShowGroup(ECSINode *group, void *userData)
{
    (void)userData;

    if (arrlenu(group->panels) > 0)
    {
        ECSIPanel_Show(group->panels[group->shown], WINDOW.renderer);
    }
}

#pragma endregion Drawing

/// @brief Opens the OS window, its renderer and the core's font.
static SHUResult ECSIWindow_Open(const char *title, const char *fontPath)
{
    if (!TTF_Init())
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_ttf failed to start: %s", SDL_GetError());
        return SHUResult_ErrInternal;
    }

    WINDOW.fonts[0] = TTF_OpenFont(fontPath, OPENECS_FONT_SIZE);

    if (WINDOW.fonts[0] == NULL)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot load the font '%s': %s", fontPath, SDL_GetError());
        return SHUResult_ErrFile;
    }

    WINDOW.window = SDL_CreateWindow(title, OPENECS_WINDOW_WIDTH, OPENECS_WINDOW_HEIGHT, SDL_WINDOW_RESIZABLE);

    if (WINDOW.window == NULL)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot open a window: %s", SDL_GetError());
        return SHUResult_ErrInternal;
    }

    WINDOW.renderer = SDL_CreateGPURenderer(NULL, WINDOW.window);

    if (WINDOW.renderer == NULL)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "GPU renderer not available (%s); using SDL's default renderer.", SDL_GetError());
        WINDOW.renderer = SDL_CreateRenderer(WINDOW.window, NULL);
    }

    if (WINDOW.renderer == NULL)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot create a renderer: %s", SDL_GetError());
        return SHUResult_ErrInternal;
    }

    WINDOW.clayRenderer = (Clay_SDL3RendererData){
        .renderer = WINDOW.renderer,
        .textEngine = TTF_CreateRendererTextEngine(WINDOW.renderer),
        .fonts = WINDOW.fonts,
    };

    if (WINDOW.clayRenderer.textEngine == NULL)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot create a text engine: %s", SDL_GetError());
        return SHUResult_ErrInternal;
    }

    ECSIWindow_ReadSize();

    u32 claySize = Clay_MinMemorySize();
    WINDOW.clayMemory = SDL_malloc(claySize);

    if (WINDOW.clayMemory == NULL)
    {
        return SHUResult_ErrAllocation;
    }

    Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(claySize, WINDOW.clayMemory);
    WINDOW.clay = Clay_Initialize(arena, (Clay_Dimensions){WINDOW.width, WINDOW.height}, (Clay_ErrorHandler){ECSIWindow_ClayError, NULL});
    Clay_SetMeasureTextFunction(ECSIWindow_MeasureText, NULL);

    return SHUResult_Ok;
}

/// @brief Reads ecs.vsync and sets the renderer's vsync. Also the Changed function of ecs.vsync.
static void ECSIWindow_ReadVsync(void *data)
{
    (void)data;

    i64 percent = SDL_clamp(ECSValue_GetInteger(ECSSetting_Get("ecs.vsync"), 0), 0, 100);
    WINDOW.framePercent = percent;
    ECSILayout_RequestFrame();

    // a whole fraction of the refresh rate, such as 50%, lets the display wait for every second refresh; other rates wait for every refresh and are limited by the core
    if (percent == 0)
    {
        SDL_SetRenderVSync(WINDOW.renderer, SDL_RENDERER_VSYNC_DISABLED);
    }
    else if (100 % percent != 0 || !SDL_SetRenderVSync(WINDOW.renderer, (int)(100 / percent)))
    {
        SDL_SetRenderVSync(WINDOW.renderer, 1);
    }
}

/// @brief Draws a frame without presenting it.
static void ECSIWindow_DrawFrame(u64 nowTicks)
{
    // some drivers accept vsync but do not wait for it, so frames are limited a little above the rate ecs.vsync asks for; a working vsync still sets the pace
    const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(WINDOW.window));
    f32 rate = mode != NULL && mode->refresh_rate > 0.0f ? mode->refresh_rate : OPENECS_FALLBACK_FRAME_RATE;
    f64 limit = (f64)rate * (f64)WINDOW.framePercent / 100.0 * (f64)OPENECS_FRAME_RATE_MARGIN;
    WINDOW.frameNanoseconds = WINDOW.framePercent == 0 ? 0 : (u64)((f64)SDL_NS_PER_SECOND / limit);
    WINDOW.lastFrameTicks = nowTicks;
    ECSIWindow_ReadSize();
    ECSILayout_Update(WINDOW.width, WINDOW.height);

    // the interface's commands point to the panels' titles, so panels draw first; their Draw may change a title
    ECSILayout_ForEachGroup(ECSIWindow_DrawGroup, &nowTicks);
    Clay_RenderCommandArray commands = ECSIWindow_DeclareInterface();
    ECSIWindow_FitTabs();

    SDL_SetRenderDrawColor(WINDOW.renderer, OPENECS_COLOR_BACKGROUND);
    SDL_RenderClear(WINDOW.renderer);
    ECSILayout_ForEachGroup(ECSIWindow_ShowGroup, NULL);
    SDL_Clay_RenderClayCommands(&WINDOW.clayRenderer, &commands);
}

#pragma endregion Source Only

SHUResult ECSIWindow_Initialize(const char *title, const char *fontPath)
{
    SDL_assert(title != NULL);
    SDL_assert(fontPath != NULL);

    SHU_ReturnResult(ECSIWindow_Open(title, fontPath), ECSIWindow_Terminate(););

    const ECSSettingDesc vsync = {
        .name = "ecs.vsync",
        .type = ECSSettingType_Integer,
        .description = "Frame rate in percent of the display's refresh rate: 100 waits for every refresh, 50 for every second one, 0 turns vsync off",
        .Changed = ECSIWindow_ReadVsync,
    };

    SHU_ReturnResult(ECSISettings_DeclareCore(&vsync), ECSIWindow_Terminate(););
    ECSIWindow_ReadVsync(NULL);
    ECSILayout_SetForget(ECSIWindow_Forget);
    return SHUResult_Ok;
}

void ECSIWindow_Terminate(void)
{
    ECSILayout_SetForget(NULL);
    arrfree(WINDOW.tabs);

    for (usz i = 0; i < SDL_arraysize(WINDOW.cursors); i++)
    {
        SDL_DestroyCursor(WINDOW.cursors[i]);
    }

    SDL_free(WINDOW.clayMemory);

    if (WINDOW.clayRenderer.textEngine != NULL)
    {
        TTF_DestroyRendererTextEngine(WINDOW.clayRenderer.textEngine);
    }

    if (WINDOW.renderer != NULL)
    {
        SDL_DestroyRenderer(WINDOW.renderer);
    }

    if (WINDOW.window != NULL)
    {
        SDL_DestroyWindow(WINDOW.window);
    }

    if (WINDOW.fonts[0] != NULL)
    {
        TTF_CloseFont(WINDOW.fonts[0]);
    }

    if (TTF_WasInit() > 0)
    {
        TTF_Quit();
    }

    SDL_zero(WINDOW);
}

SDL_Window *ECSIWindow_GetMain(void)
{
    return WINDOW.window;
}

i32 ECSIWindow_GetFrameWait(void)
{
    // a window that cannot be seen is not drawn; showing it again asks for a frame
    if ((SDL_GetWindowFlags(WINDOW.window) & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED | SDL_WINDOW_OCCLUDED)) != 0)
    {
        return -1;
    }

    if (!ECSILayout_WantsFrame())
    {
        return -1;
    }

    u64 next = WINDOW.lastFrameTicks + WINDOW.frameNanoseconds;
    u64 now = SDL_GetTicksNS();
    return now >= next ? 0 : (i32)((next - now + SDL_NS_PER_MS - 1) / SDL_NS_PER_MS);
}

void ECSIWindow_Render(u64 nowTicks)
{
    ECSIWindow_DrawFrame(nowTicks);
    SDL_RenderPresent(WINDOW.renderer);
}

SHUResult ECSIWindow_Screenshot(const char *path)
{
    SDL_assert(path != NULL);

    // the picture is read before it is presented, because presenting may discard it
    ECSIWindow_DrawFrame(SDL_GetTicksNS());
    SDL_Surface *picture = SDL_RenderReadPixels(WINDOW.renderer, NULL);
    bool saved = picture != NULL && SDL_SavePNG(picture, path);

    if (!saved)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cannot save a screenshot to '%s': %s", path, SDL_GetError());
    }

    SDL_DestroySurface(picture);
    SDL_RenderPresent(WINDOW.renderer);
    return saved ? SHUResult_Ok : SHUResult_ErrFile;
}

bool ECSIWindow_PointerDown(f32 x, f32 y)
{
    if (ECSILayout_DividerAt(x, y, &WINDOW.dragSplit, &WINDOW.dragDivider))
    {
        return true;
    }

    for (usz i = 0; i < arrlenu(WINDOW.tabs); i++)
    {
        ECSINode *group = WINDOW.tabs[i].group;
        ECSPanel panel = group->panels[WINDOW.tabs[i].index];

        // closing may ask about unsaved work; the tabs of the last frame are not used after it
        if (ECSIWindow_OnElement(CLAY_IDI("TabClose", (u32)i), x, y))
        {
            ECSLayout_Close(panel);
            return true;
        }

        if (ECSIWindow_OnElement(CLAY_IDI("Tab", (u32)i), x, y))
        {
            ECSILayout_ShowTab(panel);

            // a locked panel cannot be dragged, but its tab still shows it
            if (!group->locked)
            {
                ECSIWindow_ArmDrag(panel, false, false, x, y);
            }

            return true;
        }
    }

    // the empty part of a tab row drags the whole group
    ECSINode *group = ECSIWindow_TabRowGroupAt(x, y);

    if (group != NULL)
    {
        ECSILayout_SetFocus(group->panels[group->shown]);

        if (!group->locked)
        {
            ECSIWindow_ArmDrag(group->panels[group->shown], true, false, x, y);
        }

        return true;
    }

    // a locked group's grip only opens the menu
    if (WINDOW.gripGroup != NULL && ECSIWindow_OnElement(CLAY_ID("Grip"), x, y))
    {
        ECSIWindow_ArmDrag(WINDOW.gripGroup->panels[0], false, true, x, y);
        WINDOW.dragLocked = WINDOW.gripGroup->locked;
        return true;
    }

    return false;
}

bool ECSIWindow_PointerMove(f32 x, f32 y)
{
    // the pointer shows a divider can be dragged, and while a panel is dragged
    ECSINode *split = WINDOW.dragSplit;
    usz divider = 0;

    if (split == NULL && WINDOW.dragPanel == NULL)
    {
        ECSILayout_DividerAt(x, y, &split, &divider);
    }

    ECSIWindow_SetCursor(split != NULL ? (split->vertical ? SDL_SYSTEM_CURSOR_NS_RESIZE : SDL_SYSTEM_CURSOR_EW_RESIZE)
                         : WINDOW.dragging     ? SDL_SYSTEM_CURSOR_MOVE
                                               : SDL_SYSTEM_CURSOR_DEFAULT);

    if (WINDOW.dragSplit != NULL)
    {
        ECSILayout_MoveDivider(WINDOW.dragSplit, WINDOW.dragDivider, x, y);
        return true;
    }

    if (WINDOW.dragPanel != NULL)
    {
        bool wasDragging = WINDOW.dragging;
        bool moved = SDL_fabsf(x - WINDOW.dragStartX) + SDL_fabsf(y - WINDOW.dragStartY) >= OPENECS_DRAG_THRESHOLD;

        // a locked grip is never dragged, and pulling it is not a click
        if (moved && WINDOW.dragLocked)
        {
            ECSIWindow_CancelDrag();
            return true;
        }

        WINDOW.dragging = WINDOW.dragging || moved;

        if (WINDOW.dragging)
        {
            // a drop that changes nothing is not highlighted
            WINDOW.drop = ECSIWindow_FindDrop(x, y, &WINDOW.dropRect);
            WINDOW.drop = ECSIWindow_DropChanges(&WINDOW.drop) ? WINDOW.drop : (ECSIDrop){0};
            ECSILayout_RequestFrame();
        }

        if (WINDOW.dragging && !wasDragging)
        {
            ECSIWindow_SetCursor(SDL_SYSTEM_CURSOR_MOVE);
        }

        return true;
    }

    ECSIGripHit hit = {x, y, NULL};
    ECSILayout_ForEachGroup(ECSIWindow_FindGripGroup, &hit);

    if (hit.group != WINDOW.gripGroup)
    {
        WINDOW.gripGroup = hit.group;
        ECSILayout_RequestFrame();
    }

    return false;
}

ECSPanel ECSIWindow_PointerUp(void)
{
    ECSPanel clicked = WINDOW.dragFromGrip && !WINDOW.dragging ? WINDOW.dragPanel : NULL;

    if (WINDOW.dragging)
    {
        ECSILayout_Drop(WINDOW.dragPanel, WINDOW.dragGroup, &WINDOW.drop);
    }

    ECSIWindow_CancelDrag();
    WINDOW.dragSplit = NULL;
    ECSIWindow_SetCursor(SDL_SYSTEM_CURSOR_DEFAULT);
    return clicked;
}

bool ECSIWindow_CancelDrag(void)
{
    bool dragging = WINDOW.dragging;
    WINDOW.dragPanel = NULL;
    WINDOW.dragGroup = false;
    WINDOW.dragFromGrip = false;
    WINDOW.dragLocked = false;
    WINDOW.dragging = false;
    WINDOW.drop = (ECSIDrop){0};
    if (dragging)
    {
        ECSILayout_RequestFrame();
    }

    return dragging;
}

ECSPanel ECSIWindow_TabAt(f32 x, f32 y)
{
    for (usz i = 0; i < arrlenu(WINDOW.tabs); i++)
    {
        if (ECSIWindow_OnElement(CLAY_IDI("Tab", (u32)i), x, y))
        {
            return WINDOW.tabs[i].group->panels[WINDOW.tabs[i].index];
        }
    }

    if (WINDOW.gripGroup != NULL && ECSIWindow_OnElement(CLAY_ID("Grip"), x, y))
    {
        return WINDOW.gripGroup->panels[0];
    }

    return NULL;
}

ECSPanel ECSIWindow_TabRowAt(f32 x, f32 y)
{
    ECSINode *group = ECSIWindow_TabRowGroupAt(x, y);
    return group == NULL ? NULL : group->panels[group->shown];
}

bool ECSIWindow_ScrollTabs(f32 x, f32 y, f32 steps)
{
    ECSINode *group = ECSIWindow_TabRowGroupAt(x, y);

    if (group == NULL)
    {
        return false;
    }

    group->tabScroll = SDL_clamp(group->tabScroll + steps * OPENECS_TAB_SCROLL_STEP, 0.0f, SDL_max(0.0f, group->tabsWidth - group->width));
    ECSILayout_RequestFrame();
    return true;
}

void ECSIWindow_ShowMenu(usz level, SDL_FRect anchor, const char *const *lines, usz count)
{
    SDL_assert(level < OPENECS_MENU_DEPTH);

    // a menu that closes or changes closes its submenus
    for (usz i = level + 1; i < OPENECS_MENU_DEPTH; i++)
    {
        WINDOW.menus[i] = (ECSIMenuView){0};
    }

    ECSIMenuView *menu = &WINDOW.menus[level];
    *menu = (ECSIMenuView){0};
    ECSILayout_RequestFrame();

    if (count == 0)
    {
        return;
    }

    menu->lines = lines;
    menu->count = count;

    // the menu's size is measured here, so it can be kept inside the OS window
    TTF_Font *font = WINDOW.fonts[0];
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
    f32 x = anchor.x + anchor.w + width <= WINDOW.width ? anchor.x + anchor.w : anchor.x - width;
    f32 y = anchor.y - (level == 0 ? 0.0f : OPENECS_MENU_PADDING);
    menu->rect = (SDL_FRect){
        SDL_max(0.0f, SDL_min(x, WINDOW.width - width)),
        SDL_max(0.0f, SDL_min(y, WINDOW.height - height)),
        width,
        height,
    };
}

void ECSIWindow_SelectMenuItem(usz level, usz index)
{
    SDL_assert(level < OPENECS_MENU_DEPTH);

    WINDOW.menus[level].selected = index;
    ECSILayout_RequestFrame();
}

SDL_FRect ECSIWindow_MenuItemRect(usz level, usz index)
{
    SDL_assert(level < OPENECS_MENU_DEPTH);

    const ECSIMenuView *menu = &WINDOW.menus[level];
    return (SDL_FRect){
        menu->rect.x + OPENECS_MENU_PADDING,
        menu->rect.y + OPENECS_MENU_PADDING + (f32)index * menu->itemHeight,
        menu->rect.w - 2.0f * OPENECS_MENU_PADDING,
        menu->itemHeight,
    };
}

bool ECSIWindow_MenuItemAt(f32 x, f32 y, usz *retLevel, usz *retIndex)
{
    SDL_assert(retLevel != NULL);
    SDL_assert(retIndex != NULL);

    // submenus are drawn over their parents, so the deepest is checked first
    for (usz level = OPENECS_MENU_DEPTH; level > 0; level--)
    {
        for (usz i = 0; i < WINDOW.menus[level - 1].count; i++)
        {
            SDL_FRect item = ECSIWindow_MenuItemRect(level - 1, i);

            if (ECSIWindow_Contains(x, y, item.x, item.y, item.w, item.h))
            {
                *retLevel = level - 1;
                *retIndex = i;
                return true;
            }
        }
    }

    return false;
}

bool ECSIWindow_MenuContains(f32 x, f32 y)
{
    for (usz level = 0; level < OPENECS_MENU_DEPTH; level++)
    {
        const SDL_FRect *rect = &WINDOW.menus[level].rect;

        if (WINDOW.menus[level].lines != NULL && ECSIWindow_Contains(x, y, rect->x, rect->y, rect->w, rect->h))
        {
            return true;
        }
    }

    return false;
}

void ECSIWindow_ShowPrefixKeys(const char *const *lines, usz count)
{
    WINDOW.prefixLines = count == 0 ? NULL : lines;
    WINDOW.prefixLineCount = count;
    ECSILayout_RequestFrame();
}
