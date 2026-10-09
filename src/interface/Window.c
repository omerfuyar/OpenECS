#include "interface/Window.h"

#include "runtime/Settings.h"

#include "SDL3/SDL.h"
#include "SDL3_ttf/SDL_ttf.h"
#include "clay/clay.h"
#include "clay/claySDL3.h"
#include "stb/stbSDL3.h"

#pragma region Source Only

/// @brief Refresh rate assumed when the display's is unknown.
#define OPENECS_FALLBACK_FRAME_RATE 60.0f
/// @brief How far the frame limit is above the refresh rate.
#define OPENECS_FRAME_RATE_MARGIN 1.1f
/// @brief Width of the mark between tabs where a dragged panel is inserted.
#define OPENECS_TAB_GAP_WIDTH 3.0f
/// @brief Padding around a panel menu and inside its entries.
#define OPENECS_MENU_PADDING 6.0f
#define OPENECS_MENU_ITEM_PADDING 10.0f
/// @brief Text that a locked group's grip shows after the title.
#define OPENECS_LOCKED_MARK "locked"
/// @brief Most menus open at once: the panel menu and its submenus.
#define OPENECS_MENU_DEPTH 4

/// @brief A colour of the core's interface. Each is a setting, in OPENECS_WINDOW_SETTINGS.
typedef enum ECSIColor
{
    ECSIColor_Background = 0,
    ECSIColor_TabRow,
    ECSIColor_Tab,
    ECSIColor_TabShown,
    ECSIColor_Text,
    ECSIColor_TextDim,
    ECSIColor_Accent,
    ECSIColor_Placeholder,
    ECSIColor_Overlay,
    ECSIColor_Drop,
    ECSIColor_Selected,
    ECSIColor_Count,
} ECSIColor;

/// @brief The window's core settings; their values are in the core's settings file. The colours come first, in the order of ECSIColor.
static const ECSSettingDesc OPENECS_WINDOW_SETTINGS[] = {
    {.name = "ecs.colorBackground", .type = ECSSettingType_String, .description = "Colour between panels, such as \"#18191C\" or \"#18191CFF\""},
    {.name = "ecs.colorTabRow", .type = ECSSettingType_String, .description = "Colour of tab rows"},
    {.name = "ecs.colorTab", .type = ECSSettingType_String, .description = "Colour of tabs"},
    {.name = "ecs.colorTabShown", .type = ECSSettingType_String, .description = "Colour of the shown tab"},
    {.name = "ecs.colorText", .type = ECSSettingType_String, .description = "Colour of text"},
    {.name = "ecs.colorTextDim", .type = ECSSettingType_String, .description = "Colour of dim text"},
    {.name = "ecs.colorAccent", .type = ECSSettingType_String, .description = "Colour of the focus border, keys and highlights"},
    {.name = "ecs.colorPlaceholder", .type = ECSSettingType_String, .description = "Colour of a placeholder panel"},
    {.name = "ecs.colorOverlay", .type = ECSSettingType_String, .description = "Colour of menus, grips and the list of prefix keys"},
    {.name = "ecs.colorDrop", .type = ECSSettingType_String, .description = "Colour of the place where a dragged panel lands"},
    {.name = "ecs.colorSelected", .type = ECSSettingType_String, .description = "Colour of the selected menu entry"},
    {.name = "ecs.windowWidth", .type = ECSSettingType_Integer, .description = "Width of the OS window when it opens, in layout units"},
    {.name = "ecs.windowHeight", .type = ECSSettingType_Integer, .description = "Height of the OS window when it opens, in layout units"},
    {.name = "ecs.font", .type = ECSSettingType_String, .description = "TrueType font of the core's interface, read when the window opens; a relative path starts at the executable's folder"},
    {.name = "ecs.fontSize", .type = ECSSettingType_Number, .description = "Size of the core's font, in layout units"},
    {.name = "ecs.gripHeight", .type = ECSSettingType_Number, .description = "Height of a grip, in layout units"},
    {.name = "ecs.gripZone", .type = ECSSettingType_Number, .description = "Distance from a panel's top edge within which its grip shows, in layout units"},
    {.name = "ecs.dragThreshold", .type = ECSSettingType_Number, .description = "How far the pointer moves from a press on a tab or grip before the panel is dragged, in layout units"},
    {.name = "ecs.dockEdge", .type = ECSSettingType_Number, .description = "Distance from an edge of the OS window within which a dragged panel docks along that edge, in layout units"},
    {.name = "ecs.splitDepth", .type = ECSSettingType_Number, .description = "Deepest edge band of a panel in which a dragged panel splits it, in layout units"},
    {.name = "ecs.tabScrollStep", .type = ECSSettingType_Number, .description = "How far one step of the wheel scrolls a tab row, in layout units"},
};

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

/// @brief An OS window: the main window, or a pop-out window, with what the core draws in it.
typedef struct ECSIOSWindow
{
    u32 rootId; // the pop-out root it shows; 0 for the main window, which shows the current workspace's main root
    SDL_Window *window;
    SDL_Renderer *renderer;
    Clay_SDL3RendererData clayRenderer;
    Clay_Context *clay;
    void *clayMemory;
    f32 width; // in layout units
    f32 height;
    ECSITabRef *tabs; // stb_ds array of the tabs drawn in its last frame
} ECSIOSWindow;

/// @brief How a popup is shown: in an SDL popup window, or inside its panel's OS window when the video driver has no popup windows.
typedef struct ECSIPopupView
{
    u32 rootId;             // the root of the OS window it belongs to; it is made again if its panel moves to another one
    SDL_Window *window;     // SDL's popup window, or NULL when it is drawn inside the OS window
    SDL_Renderer *renderer; // the popup window's
    SDL_Texture *texture;   // its pixels, made by the renderer that shows them
    SDL_FRect rect;         // where it shows, in the layout units of its panel's OS window
} ECSIPopupView;

static struct
{
    ECSIOSWindow **windows; // stb_ds array: the main window, then the pop-out windows
    ECSIOSWindow *event;    // the OS window of the pointer event being handled; positions are in its layout units
    ECSPopup eventPopup;    // the popup whose SDL popup window has the pointer event being handled, or NULL
    bool noPopupWindows;    // the video driver has no popup windows, so popups are drawn inside the OS windows
    ECSIOSWindow *gripWindow;
    ECSIOSWindow *dropWindow; // the OS window where the dragged panel lands, or NULL for a pop-out
    ECSIOSWindow *dataWindow; // the OS window under the pointer while data is dragged
    ECSIOSWindow *menuWindow; // the OS window that shows the menus
    u32 focusRoot;            // the root of the focused panel in the last frame, so its OS window comes forward when the focus moves to it
    char *title;              // of every OS window
    TTF_Font *fonts[1];

    i64 framePercent;     // frame rate as a percentage of the refresh rate, from ecs.vsync; 0 for no vsync and no limit
    u64 frameNanoseconds; // shortest time between frames, from the display's refresh rate; 0 for no limit
    u64 lastFrameTicks;
    const char *const *prefixLines; // keys after the prefix and what they do, in pairs, or NULL when the prefix is not pressed
    usz prefixLineCount;

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

    const char *dataType; // type of the data being dragged, or NULL
    f32 dataX;            // the pointer's position while data is dragged, in dataWindow
    f32 dataY;

    SDL_Cursor *cursors[SDL_SYSTEM_CURSOR_COUNT]; // made when first used
    SDL_SystemCursor cursor;                      // the pointer's shape now

    ECSIMenuView menus[OPENECS_MENU_DEPTH]; // the panel menu, then its open submenus

    // from the settings
    Clay_Color colors[ECSIColor_Count];
    f32 fontSize;
    f32 gripHeight;
    f32 gripZone;
    f32 dragThreshold;
    f32 dockEdge;
    f32 splitDepth;
    f32 tabScrollStep;
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

/// @brief Reads the size of an OS window.
static void ECSIWindow_ReadSize(ECSIOSWindow *os)
{
    int width = 0;
    int height = 0;
    SDL_GetWindowSize(os->window, &width, &height);

    os->width = (f32)width;
    os->height = (f32)height;
}

/// @brief Gets the root an OS window shows, or NULL if it shows none now: the main window shows the current workspace's main root, and a pop-out window its own root while its workspace is current.
static ECSIRoot *ECSIWindow_Root(const ECSIOSWindow *os)
{
    if (os == NULL)
    {
        return NULL;
    }

    if (os->rootId == 0)
    {
        usz count = 0;
        ECSIRoot *const *roots = ECSILayout_GetRoots(&count);
        return count > 0 ? roots[0] : NULL;
    }

    bool shown = false;
    ECSIRoot *root = ECSILayout_FindRoot(os->rootId, &shown);
    return shown ? root : NULL;
}

/// @brief Finds the OS window that shows a root of the current workspace.
static ECSIOSWindow *ECSIWindow_OfRoot(const ECSIRoot *root)
{
    for (usz i = 0; root != NULL && i < arrlenu(WINDOW.windows); i++)
    {
        if (ECSIWindow_Root(WINDOW.windows[i]) == root)
        {
            return WINDOW.windows[i];
        }
    }

    return NULL;
}

/// @brief Finds an OS window by SDL's id; the main window if it is none of them.
static ECSIOSWindow *ECSIWindow_Find(SDL_WindowID id)
{
    for (usz i = 0; i < arrlenu(WINDOW.windows); i++)
    {
        if (SDL_GetWindowID(WINDOW.windows[i]->window) == id)
        {
            return WINDOW.windows[i];
        }
    }

    return arrlenu(WINDOW.windows) > 0 ? WINDOW.windows[0] : NULL;
}

/// @brief Checks whether an OS window is shown.
static bool ECSIWindow_IsShown(const ECSIOSWindow *os)
{
    return (SDL_GetWindowFlags(os->window) & SDL_WINDOW_HIDDEN) == 0 && ECSIWindow_Root(os) != NULL;
}

static bool ECSIWindow_Contains(f32 x, f32 y, f32 left, f32 top, f32 width, f32 height)
{
    return x >= left && y >= top && x < left + width && y < top + height;
}

/// @brief Finds the shown OS window under a point given in the event window's layout units: the event window itself, or another one that the point falls in on the screen.
/// @param retX Gets the point's position in the found window.
/// @param retY Gets the point's position in the found window.
/// @return The OS window, or NULL if the point is outside every one.
static ECSIOSWindow *ECSIWindow_Under(f32 x, f32 y, f32 *retX, f32 *retY)
{
    ECSIOSWindow *event = WINDOW.event;
    *retX = x;
    *retY = y;

    if (event == NULL || ECSIWindow_Contains(x, y, 0.0f, 0.0f, event->width, event->height))
    {
        return event;
    }

    // windows know their places on the screen, except on Wayland, where a point outside the event window falls in none
    int eventX = 0;
    int eventY = 0;
    SDL_GetWindowPosition(event->window, &eventX, &eventY);

    for (usz i = 0; i < arrlenu(WINDOW.windows); i++)
    {
        ECSIOSWindow *os = WINDOW.windows[i];
        int left = 0;
        int top = 0;

        if (os == event || !ECSIWindow_IsShown(os) || !SDL_GetWindowPosition(os->window, &left, &top))
        {
            continue;
        }

        f32 localX = x + (f32)(eventX - left);
        f32 localY = y + (f32)(eventY - top);

        if (ECSIWindow_Contains(localX, localY, 0.0f, 0.0f, os->width, os->height))
        {
            *retX = localX;
            *retY = localY;
            return os;
        }
    }

    return NULL;
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

/// @brief Finds the group whose tab row is at a point of an OS window.
static ECSINode *ECSIWindow_TabRowGroupAt(const ECSIOSWindow *os, f32 x, f32 y)
{
    ECSINode *group = ECSILayout_GroupAt(ECSIWindow_Root(os), x, y);
    return group != NULL && arrlenu(group->panels) >= 2 && y < group->y + ECSILayout_GetTabRowHeight() ? group : NULL;
}

/// @brief Finds the gap between a group's tabs nearest to a point, from the tabs drawn in the OS window's last frame.
static ECSIDrop ECSIWindow_FindTabGap(ECSIOSWindow *os, ECSINode *group, f32 x, SDL_FRect *retRect)
{
    ECSIDrop drop = {.zone = ECSIZone_Tabs, .root = ECSIWindow_Root(os), .group = group, .index = 0};
    f32 gapX = group->x;
    Clay_SetCurrentContext(os->clay);

    for (usz i = 0; i < arrlenu(os->tabs); i++)
    {
        Clay_ElementData tab = Clay_GetElementData(CLAY_IDI("Tab", (u32)i));

        if (os->tabs[i].group != group || !tab.found)
        {
            continue;
        }

        if (x > tab.boundingBox.x + tab.boundingBox.width / 2.0f)
        {
            drop.index = os->tabs[i].index + 1;
            gapX = tab.boundingBox.x + tab.boundingBox.width;
        }
        else if (os->tabs[i].index == drop.index)
        {
            gapX = tab.boundingBox.x;
        }
    }

    *retRect = (SDL_FRect){gapX - OPENECS_TAB_GAP_WIDTH / 2.0f, group->y, OPENECS_TAB_GAP_WIDTH, ECSILayout_GetTabRowHeight()};
    return drop;
}

/// @brief Finds where a panel dragged to a point lands, the OS window it lands in, and the rectangle to highlight there. The checks follow DESIGN 6.7.
/// @param x Horizontal position in the event window, in layout units.
/// @param y Vertical position in the event window, in layout units.
static ECSIDrop ECSIWindow_FindDrop(f32 x, f32 y, ECSIOSWindow **retWindow, SDL_FRect *retRect)
{
    *retRect = (SDL_FRect){0};
    ECSIOSWindow *os = ECSIWindow_Under(x, y, &x, &y);
    ECSIRoot *root = ECSIWindow_Root(os);
    *retWindow = os;

    // outside every OS window, the panel pops out into a new one
    if (os == NULL || root == NULL)
    {
        *retWindow = NULL;
        return (ECSIDrop){.zone = ECSIZone_PopOut};
    }

    f32 width = os->width;
    f32 height = os->height;

    // an empty window takes the panel whole
    if (root->tree == NULL)
    {
        *retRect = (SDL_FRect){0.0f, 0.0f, width, height};
        return (ECSIDrop){.zone = ECSIZone_Center, .root = root};
    }

    if (x < WINDOW.dockEdge || x >= width - WINDOW.dockEdge || y < WINDOW.dockEdge || y >= height - WINDOW.dockEdge)
    {
        ECSIZone zone = x < WINDOW.dockEdge            ? ECSIZone_WindowLeft
                        : x >= width - WINDOW.dockEdge ? ECSIZone_WindowRight
                        : y < WINDOW.dockEdge          ? ECSIZone_WindowTop
                                                       : ECSIZone_WindowBottom;

        *retRect = zone == ECSIZone_WindowLeft    ? (SDL_FRect){0.0f, 0.0f, width / 4.0f, height}
                   : zone == ECSIZone_WindowRight ? (SDL_FRect){width * 0.75f, 0.0f, width / 4.0f, height}
                   : zone == ECSIZone_WindowTop   ? (SDL_FRect){0.0f, 0.0f, width, height / 4.0f}
                                                  : (SDL_FRect){0.0f, height * 0.75f, width, height / 4.0f};
        return (ECSIDrop){.zone = zone, .root = root};
    }

    ECSINode *group = ECSILayout_GroupAt(root, x, y);

    if (group == NULL || arrlenu(group->panels) == 0)
    {
        return (ECSIDrop){0};
    }

    // a locked group accepts no dropped panels
    if (arrlenu(group->panels) >= 2 && y < group->y + ECSILayout_GetTabRowHeight())
    {
        return group->locked ? (ECSIDrop){0} : ECSIWindow_FindTabGap(os, group, x, retRect);
    }

    // edge bands are a quarter of the panel deep at most, so the centre keeps at least half of it
    ECSPanel panel = group->panels[group->shown];
    f32 bandX = SDL_min(panel->width / 4.0f, WINDOW.splitDepth);
    f32 bandY = SDL_min(panel->height / 4.0f, WINDOW.splitDepth);
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

    return (ECSIDrop){.zone = zone, .root = root, .group = group};
}

/// @brief Checks whether dropping the dragged panel, or its group, at a place would change the layout.
static bool ECSIWindow_DropChanges(const ECSIDrop *drop)
{
    ECSINode *source = ECSILayout_GroupOf(WINDOW.dragPanel);
    ECSIRoot *sourceRoot = ECSILayout_RootOf(WINDOW.dragPanel);

    if (source == NULL || drop->zone == ECSIZone_None)
    {
        return false;
    }

    // a whole group moves when the group is dragged or holds only the dragged panel; a group that fills its window stays in it
    bool whole = WINDOW.dragGroup || arrlenu(source->panels) == 1;

    if (drop->zone == ECSIZone_PopOut)
    {
        return !whole || sourceRoot->tree != source;
    }

    if (drop->group == NULL)
    {
        return !whole || drop->root->tree != source;
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

    if (arrlenu(group->panels) == 1 && ECSIWindow_Contains(hit->x, hit->y, group->x, group->y, group->width, WINDOW.gripZone))
    {
        hit->group = group;
    }
}

/// @brief Checks whether a point is on an element that Clay laid out in an OS window's last frame.
static bool ECSIWindow_OnElement(const ECSIOSWindow *os, Clay_ElementId id, f32 x, f32 y)
{
    Clay_SetCurrentContext(os->clay);
    Clay_ElementData element = Clay_GetElementData(id);
    return element.found && ECSIWindow_Contains(x, y, element.boundingBox.x, element.boundingBox.y, element.boundingBox.width, element.boundingBox.height);
}

/// @brief Declares a group's tab row, or its placeholder text, for Clay. userData is the OS window.
static void ECSIWindow_DeclareGroup(ECSINode *group, void *userData)
{
    ECSIOSWindow *os = userData;

    if (arrlenu(group->panels) == 0)
    {
        return;
    }

    if (arrlenu(group->panels) >= 2)
    {
        CLAY_AUTO_ID({
            .layout = {
                .sizing = {CLAY_SIZING_FIXED(group->width), CLAY_SIZING_FIXED(ECSILayout_GetTabRowHeight())},
                .childGap = 1,
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
            },
            .backgroundColor = WINDOW.colors[ECSIColor_TabRow],
            .clip = {.horizontal = true, .childOffset = {-group->tabScroll, 0.0f}},
            .floating = {.attachTo = CLAY_ATTACH_TO_ROOT, .offset = {group->x, group->y}},
        })
        {
            for (usz i = 0; i < arrlenu(group->panels); i++)
            {
                CLAY(CLAY_IDI("Tab", (u32)arrlenu(os->tabs)), {
                                                                     .layout = {
                                                                         .sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_GROW(0)},
                                                                         .padding = {12, 12, 0, 0},
                                                                         .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                                                     },
                                                                     .backgroundColor = i == group->shown ? WINDOW.colors[ECSIColor_TabShown] : WINDOW.colors[ECSIColor_Tab],
                                                                 })
                {
                    CLAY_TEXT(ECSIWindow_ClayText(group->panels[i]->title),
                              CLAY_TEXT_CONFIG({
                                  .textColor = i == group->shown ? WINDOW.colors[ECSIColor_Text] : WINDOW.colors[ECSIColor_TextDim],
                                  .fontSize = (u16)WINDOW.fontSize,
                                  .wrapMode = CLAY_TEXT_WRAP_NONE,
                              }));

                    // the mark of unsaved work
                    if (group->panels[i]->unsaved)
                    {
                        CLAY_TEXT(CLAY_STRING(" *"), CLAY_TEXT_CONFIG({.textColor = WINDOW.colors[ECSIColor_Accent], .fontSize = (u16)WINDOW.fontSize, .wrapMode = CLAY_TEXT_WRAP_NONE}));
                    }

                    // panels of a locked group cannot be closed by the user
                    if (!group->locked)
                    {
                        CLAY(CLAY_IDI("TabClose", (u32)arrlenu(os->tabs)), {.layout = {.padding = {8, 0, 0, 0}}})
                        {
                            CLAY_TEXT(CLAY_STRING("\u00D7"), CLAY_TEXT_CONFIG({.textColor = WINDOW.colors[ECSIColor_TextDim], .fontSize = (u16)WINDOW.fontSize, .wrapMode = CLAY_TEXT_WRAP_NONE}));
                        }
                    }
                }

                arrput(os->tabs, ((ECSITabRef){group, i}));
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
            .backgroundColor = WINDOW.colors[ECSIColor_Placeholder],
            .clip = {.horizontal = true, .vertical = true},
            .floating = {.attachTo = CLAY_ATTACH_TO_ROOT, .offset = {panel->x, panel->y}},
        })
        {
            Clay_String heading = panel->fault != NULL ? CLAY_STRING("Panel failed") : CLAY_STRING("Missing panel type");
            CLAY_TEXT(heading, CLAY_TEXT_CONFIG({.textColor = WINDOW.colors[ECSIColor_TextDim], .fontSize = (u16)WINDOW.fontSize}));
            CLAY_TEXT(ECSIWindow_ClayText(panel->typeName), CLAY_TEXT_CONFIG({.textColor = WINDOW.colors[ECSIColor_Text], .fontSize = (u16)WINDOW.fontSize}));

            if (panel->fault != NULL)
            {
                CLAY_TEXT(ECSIWindow_ClayText(panel->fault), CLAY_TEXT_CONFIG({.textColor = WINDOW.colors[ECSIColor_TextDim], .fontSize = (u16)WINDOW.fontSize}));
            }
        }
    }
}

/// @brief Measures each tab row of an OS window from the tabs Clay just laid out, keeps its scroll inside it, and scrolls a newly shown tab into view.
static void ECSIWindow_FitTabs(ECSIOSWindow *os)
{
    Clay_SetCurrentContext(os->clay);

    for (usz i = 0; i < arrlenu(os->tabs);)
    {
        // the tabs of one group are next to each other in the list
        ECSINode *group = os->tabs[i].group;
        f32 left = 0.0f;
        f32 right = 0.0f;
        Clay_BoundingBox shown = {0};

        for (; i < arrlenu(os->tabs) && os->tabs[i].group == group; i++)
        {
            Clay_ElementData tab = Clay_GetElementData(CLAY_IDI("Tab", (u32)i));
            left = os->tabs[i].index == 0 ? tab.boundingBox.x : left;
            right = tab.boundingBox.x + tab.boundingBox.width;
            shown = os->tabs[i].index == group->shown ? tab.boundingBox : shown;
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

/// @brief Marks a visible group's shown panel if it accepts the dragged data, and fills it if the pointer is over it. userData is the OS window.
static void ECSIWindow_DeclareDataTarget(ECSINode *group, void *userData)
{
    const ECSIOSWindow *os = userData;

    if (arrlenu(group->panels) == 0)
    {
        return;
    }

    ECSPanel panel = group->panels[group->shown];

    if (!ECSIPanel_Accepts(panel, WINDOW.dataType))
    {
        return;
    }

    bool under = os == WINDOW.dataWindow && WINDOW.dataX >= panel->x && WINDOW.dataX < panel->x + panel->width && WINDOW.dataY >= panel->y && WINDOW.dataY < panel->y + panel->height;

    CLAY_AUTO_ID({
        .layout = {.sizing = {CLAY_SIZING_FIXED(panel->width), CLAY_SIZING_FIXED(panel->height)}},
        .backgroundColor = under ? WINDOW.colors[ECSIColor_Drop] : (Clay_Color){0},
        .border = {.color = WINDOW.colors[ECSIColor_Accent], .width = {2, 2, 2, 2, 0}},
        .floating = {.attachTo = CLAY_ATTACH_TO_ROOT, .offset = {panel->x, panel->y}, .zIndex = 4, .pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH},
    })
    {
    }
}

/// @brief Declares the core's own interface in an OS window for Clay and returns what to draw.
static Clay_RenderCommandArray ECSIWindow_DeclareInterface(ECSIOSWindow *os, const ECSIRoot *root)
{
    Clay_SetCurrentContext(os->clay);
    Clay_SetLayoutDimensions((Clay_Dimensions){os->width, os->height});
    Clay_BeginLayout();

    // the focus border, the list of prefix keys and the menus show in one OS window each
    ECSPanel focus = ECSILayout_GetFocus();
    ECSIRoot *focusRoot = ECSILayout_RootOf(focus);
    bool focusWindow = focusRoot == root || (focusRoot == NULL && os->rootId == 0);

    arrfree(os->tabs);
    CLAY(CLAY_ID("Root"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}}})
    {
        ECSILayout_ForEachGroup(root, ECSIWindow_DeclareGroup, os);

        if (focus != NULL && focusWindow)
        {
            CLAY_AUTO_ID({
                .layout = {.sizing = {CLAY_SIZING_FIXED(focus->width), CLAY_SIZING_FIXED(focus->height)}},
                .border = {.color = WINDOW.colors[ECSIColor_Accent], .width = {2, 2, 2, 2, 0}},
                .floating = {.attachTo = CLAY_ATTACH_TO_ROOT, .offset = {focus->x, focus->y}, .zIndex = 1, .pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH},
            })
            {
            }
        }

        // the grip shows the panel's title, centred on its top edge, and whether its group is locked
        if (WINDOW.gripGroup != NULL && WINDOW.gripWindow == os)
        {
            ECSINode *group = WINDOW.gripGroup;
            const char *title = group->panels[0]->title;
            int titleWidth = 0;
            int lockedWidth = 0;
            TTF_SetFontSize(WINDOW.fonts[0], WINDOW.fontSize);
            TTF_GetStringSize(WINDOW.fonts[0], title, 0, &titleWidth, NULL);

            if (group->locked)
            {
                TTF_GetStringSize(WINDOW.fonts[0], OPENECS_LOCKED_MARK, 0, &lockedWidth, NULL);
                lockedWidth += (int)OPENECS_MENU_ITEM_PADDING;
            }

            f32 width = SDL_min((f32)(titleWidth + lockedWidth) + 2.0f * OPENECS_MENU_ITEM_PADDING, group->width);

            CLAY(CLAY_ID("Grip"), {
                                      .layout = {
                                          .sizing = {CLAY_SIZING_FIXED(width), CLAY_SIZING_FIXED(WINDOW.gripHeight)},
                                          .padding = {(u16)OPENECS_MENU_ITEM_PADDING, (u16)OPENECS_MENU_ITEM_PADDING, 0, 0},
                                          .childGap = (u16)OPENECS_MENU_ITEM_PADDING,
                                          .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER},
                                      },
                                      .backgroundColor = WINDOW.colors[ECSIColor_Overlay],
                                      .cornerRadius = {0, 0, 6, 6},
                                      .border = {.color = WINDOW.colors[ECSIColor_TextDim], .width = {1, 1, 0, 1, 0}},
                                      .clip = {.horizontal = true},
                                      .floating = {.attachTo = CLAY_ATTACH_TO_ROOT, .offset = {group->x + (group->width - width) / 2.0f, group->y}, .zIndex = 2},
                                  })
            {
                CLAY_TEXT(ECSIWindow_ClayText(title), CLAY_TEXT_CONFIG({.textColor = WINDOW.colors[ECSIColor_Text], .fontSize = (u16)WINDOW.fontSize, .wrapMode = CLAY_TEXT_WRAP_NONE}));

                if (group->locked)
                {
                    CLAY_TEXT(CLAY_STRING(OPENECS_LOCKED_MARK), CLAY_TEXT_CONFIG({.textColor = WINDOW.colors[ECSIColor_TextDim], .fontSize = (u16)WINDOW.fontSize, .wrapMode = CLAY_TEXT_WRAP_NONE}));
                }
            }
        }

        if (WINDOW.dragging && WINDOW.drop.zone != ECSIZone_None && WINDOW.dropWindow == os)
        {
            SDL_FRect rect = WINDOW.dropRect;

            CLAY_AUTO_ID({
                .layout = {.sizing = {CLAY_SIZING_FIXED(rect.w), CLAY_SIZING_FIXED(rect.h)}},
                .backgroundColor = WINDOW.colors[ECSIColor_Drop],
                .border = {.color = WINDOW.colors[ECSIColor_Accent], .width = {2, 2, 2, 2, 0}},
                .floating = {.attachTo = CLAY_ATTACH_TO_ROOT, .offset = {rect.x, rect.y}, .zIndex = 4, .pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH},
            })
            {
            }
        }

        if (WINDOW.dataType != NULL)
        {
            ECSILayout_ForEachGroup(root, ECSIWindow_DeclareDataTarget, os);
        }

        for (usz level = 0; WINDOW.menuWindow == os && level < OPENECS_MENU_DEPTH && WINDOW.menus[level].lines != NULL; level++)
        {
            const ECSIMenuView *menu = &WINDOW.menus[level];
            SDL_FRect rect = menu->rect;

            CLAY(CLAY_IDI("Menu", (u32)level), {
                                                   .layout = {
                                                       .sizing = {CLAY_SIZING_FIXED(rect.w), CLAY_SIZING_FIXED(rect.h)},
                                                       .padding = CLAY_PADDING_ALL((u16)OPENECS_MENU_PADDING),
                                                       .layoutDirection = CLAY_TOP_TO_BOTTOM,
                                                   },
                                                   .backgroundColor = WINDOW.colors[ECSIColor_Overlay],
                                                   .cornerRadius = CLAY_CORNER_RADIUS(6),
                                                   .border = {.color = WINDOW.colors[ECSIColor_TextDim], .width = {1, 1, 1, 1, 0}},
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
                        .backgroundColor = i == menu->selected ? WINDOW.colors[ECSIColor_Selected] : (Clay_Color){0},
                        .cornerRadius = CLAY_CORNER_RADIUS(4),
                    })
                    {
                        CLAY_TEXT(ECSIWindow_ClayText(menu->lines[2 * i + 1]), CLAY_TEXT_CONFIG({.textColor = WINDOW.colors[ECSIColor_Text], .fontSize = (u16)WINDOW.fontSize, .wrapMode = CLAY_TEXT_WRAP_NONE}));
                        CLAY_AUTO_ID({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)}}}) {}
                        CLAY_TEXT(ECSIWindow_ClayText(menu->lines[2 * i]), CLAY_TEXT_CONFIG({.textColor = WINDOW.colors[ECSIColor_TextDim], .fontSize = (u16)WINDOW.fontSize, .wrapMode = CLAY_TEXT_WRAP_NONE}));
                    }
                }
            }
        }

        if (WINDOW.prefixLines != NULL && focusWindow)
        {
            CLAY_AUTO_ID({
                .layout = {.padding = CLAY_PADDING_ALL(14), .childGap = 4, .layoutDirection = CLAY_TOP_TO_BOTTOM},
                .backgroundColor = WINDOW.colors[ECSIColor_Overlay],
                .cornerRadius = CLAY_CORNER_RADIUS(6),
                .border = {.color = WINDOW.colors[ECSIColor_Accent], .width = {1, 1, 1, 1, 0}},
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
                            CLAY_TEXT(ECSIWindow_ClayText(WINDOW.prefixLines[2 * i + 1]), CLAY_TEXT_CONFIG({.textColor = WINDOW.colors[ECSIColor_TextDim], .fontSize = (u16)WINDOW.fontSize}));
                        }

                        continue;
                    }

                    CLAY_AUTO_ID({.layout = {.layoutDirection = CLAY_LEFT_TO_RIGHT}})
                    {
                        CLAY_AUTO_ID({.layout = {.sizing = {CLAY_SIZING_FIXED(OPENECS_PREFIX_KEY_COLUMN), CLAY_SIZING_FIT(0)}}})
                        {
                            CLAY_TEXT(ECSIWindow_ClayText(WINDOW.prefixLines[2 * i]), CLAY_TEXT_CONFIG({.textColor = WINDOW.colors[ECSIColor_Accent], .fontSize = (u16)WINDOW.fontSize}));
                        }

                        CLAY_TEXT(ECSIWindow_ClayText(WINDOW.prefixLines[2 * i + 1]), CLAY_TEXT_CONFIG({.textColor = WINDOW.colors[ECSIColor_Text], .fontSize = (u16)WINDOW.fontSize}));
                    }
                }
            }
        }
    }

    return Clay_EndLayout(0.0f);
}

#pragma endregion Interface

#pragma region Drawing

/// @brief What drawing a group needs: the OS window's renderer and the time.
typedef struct ECSIDrawContext
{
    SDL_Renderer *renderer;
    u64 nowTicks;
} ECSIDrawContext;

static void ECSIWindow_DrawGroup(ECSINode *group, void *userData)
{
    ECSIDrawContext *context = userData;

    if (arrlenu(group->panels) > 0)
    {
        ECSIPanel_Draw(group->panels[group->shown], context->renderer, context->nowTicks);
    }
}

static void ECSIWindow_ShowGroup(ECSINode *group, void *userData)
{
    ECSIDrawContext *context = userData;

    if (arrlenu(group->panels) > 0)
    {
        ECSIPanel_Show(group->panels[group->shown], context->renderer);
    }
}

#pragma endregion Drawing

#pragma region OS Windows

/// @brief Closes an OS window and frees what it holds. The window pointers that named it name the main window instead.
static void ECSIWindow_CloseOS(ECSIOSWindow *os)
{
    ECSIOSWindow *main = arrlenu(WINDOW.windows) > 0 && WINDOW.windows[0] != os ? WINDOW.windows[0] : NULL;
    ECSIOSWindow **pointers[] = {&WINDOW.event, &WINDOW.gripWindow, &WINDOW.dropWindow, &WINDOW.dataWindow, &WINDOW.menuWindow};
    WINDOW.gripGroup = WINDOW.gripWindow == os ? NULL : WINDOW.gripGroup;

    for (usz i = 0; i < SDL_arraysize(pointers); i++)
    {
        *pointers[i] = *pointers[i] == os ? main : *pointers[i];
    }
    arrfree(os->tabs);
    SDL_free(os->clayMemory);

    if (os->clayRenderer.textEngine != NULL)
    {
        TTF_DestroyRendererTextEngine(os->clayRenderer.textEngine);
    }

    if (os->renderer != NULL)
    {
        SDL_DestroyRenderer(os->renderer);
    }

    if (os->window != NULL)
    {
        SDL_DestroyWindow(os->window);
    }

    SDL_free(os);
}

/// @brief Opens an OS window with its renderer and Clay context, and adds it to the list.
/// @param rootId The pop-out root it shows, or 0 for the main window.
/// @param flags SDL's window flags; a pop-out window opens hidden, so it can be placed first.
/// @return The window, or NULL if it cannot be opened.
static ECSIOSWindow *ECSIWindow_OpenOS(u32 rootId, i32 width, i32 height, SDL_WindowFlags flags)
{
    ECSIOSWindow *os = SDL_calloc(1, sizeof(ECSIOSWindow));

    if (os == NULL)
    {
        return NULL;
    }

    os->rootId = rootId;
    os->window = SDL_CreateWindow(WINDOW.title, width, height, SDL_WINDOW_RESIZABLE | flags);

    if (os->window == NULL)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot open a window: %s", SDL_GetError());
        ECSIWindow_CloseOS(os);
        return NULL;
    }

    os->renderer = SDL_CreateGPURenderer(NULL, os->window);

    if (os->renderer == NULL)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "GPU renderer not available (%s); using SDL's default renderer.", SDL_GetError());
        os->renderer = SDL_CreateRenderer(os->window, NULL);
    }

    if (os->renderer == NULL)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot create a renderer: %s", SDL_GetError());
        ECSIWindow_CloseOS(os);
        return NULL;
    }

    os->clayRenderer = (Clay_SDL3RendererData){
        .renderer = os->renderer,
        .textEngine = TTF_CreateRendererTextEngine(os->renderer),
        .fonts = WINDOW.fonts,
    };

    u32 claySize = Clay_MinMemorySize();
    os->clayMemory = os->clayRenderer.textEngine == NULL ? NULL : SDL_malloc(claySize);

    if (os->clayMemory == NULL)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot create a text engine: %s", SDL_GetError());
        ECSIWindow_CloseOS(os);
        return NULL;
    }

    ECSIWindow_ReadSize(os);
    Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(claySize, os->clayMemory);
    os->clay = Clay_Initialize(arena, (Clay_Dimensions){os->width, os->height}, (Clay_ErrorHandler){ECSIWindow_ClayError, NULL});
    Clay_SetMeasureTextFunction(ECSIWindow_MeasureText, NULL);

    // only the main window waits for the display, so presenting several windows does not wait once for each
    if (rootId != 0)
    {
        SDL_SetRenderVSync(os->renderer, SDL_RENDERER_VSYNC_DISABLED);
    }

    arrput(WINDOW.windows, os);
    return os;
}

/// @brief Loads the core's font and opens the main OS window.
static SHUResult ECSIWindow_Open(const char *title)
{
    if (!TTF_Init())
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_ttf failed to start: %s", SDL_GetError());
        return SHUResult_ErrInternal;
    }

    // a relative font path starts at the executable's folder
    const char *font = ECSValue_GetString(ECSSetting_Get("ecs.font"), "");
    char *fontPath = NULL;

    if (SDL_asprintf(&fontPath, "%s%s", font[0] == '/' ? "" : SDL_GetBasePath(), font) < 0)
    {
        return SHUResult_ErrAllocation;
    }

    WINDOW.fonts[0] = TTF_OpenFont(fontPath, WINDOW.fontSize);

    if (WINDOW.fonts[0] == NULL)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot load the font '%s': %s", fontPath, SDL_GetError());
        SDL_free(fontPath);
        return SHUResult_ErrFile;
    }

    SDL_free(fontPath);
    WINDOW.title = SDL_strdup(title);

    if (WINDOW.title == NULL)
    {
        return SHUResult_ErrAllocation;
    }

    i64 width = SDL_clamp(ECSValue_GetInteger(ECSSetting_Get("ecs.windowWidth"), 0), 1, SDL_MAX_SINT32);
    i64 height = SDL_clamp(ECSValue_GetInteger(ECSSetting_Get("ecs.windowHeight"), 0), 1, SDL_MAX_SINT32);
    WINDOW.event = ECSIWindow_OpenOS(0, (i32)width, (i32)height, 0);
    WINDOW.menuWindow = WINDOW.event;
    return WINDOW.event == NULL ? SHUResult_ErrInternal : SHUResult_Ok;
}

/// @brief Matches the OS windows to the current workspace's roots: a new pop-out root gets its OS window at the pointer, a gone one's window closes, and the windows of other workspaces hide. Each shown root gets its window's size.
static void ECSIWindow_Sync(void)
{
    for (usz i = 1; i < arrlenu(WINDOW.windows);)
    {
        ECSIOSWindow *os = WINDOW.windows[i];
        bool shown = false;

        if (ECSILayout_FindRoot(os->rootId, &shown) == NULL)
        {
            arrdel(WINDOW.windows, i);
            ECSIWindow_CloseOS(os);
            continue;
        }

        bool hidden = (SDL_GetWindowFlags(os->window) & SDL_WINDOW_HIDDEN) != 0;

        if (shown && hidden)
        {
            SDL_ShowWindow(os->window);
        }
        else if (!shown && !hidden)
        {
            SDL_HideWindow(os->window);
        }

        i++;
    }

    usz count = 0;
    ECSIRoot *const *roots = ECSILayout_GetRoots(&count);

    for (usz i = 1; i < count; i++)
    {
        if (ECSIWindow_OfRoot(roots[i]) != NULL)
        {
            continue;
        }

        ECSIOSWindow *os = ECSIWindow_OpenOS(roots[i]->id, (i32)SDL_max(1.0f, roots[i]->width), (i32)SDL_max(1.0f, roots[i]->height), SDL_WINDOW_HIDDEN);

        if (os != NULL)
        {
            // it opens at the pointer; on Wayland, the compositor places it
            f32 x = 0.0f;
            f32 y = 0.0f;
            SDL_GetGlobalMouseState(&x, &y);
            SDL_SetWindowPosition(os->window, (int)x, (int)y);
            SDL_ShowWindow(os->window);
        }
    }

    for (usz i = 0; i < arrlenu(WINDOW.windows); i++)
    {
        ECSIOSWindow *os = WINDOW.windows[i];
        ECSIRoot *root = ECSIWindow_Root(os);
        ECSIWindow_ReadSize(os);

        if (root != NULL)
        {
            ECSILayout_SetRootSize(root, os->width, os->height);
        }
    }

    // the OS window of the focused panel comes forward when the focus moves into it
    ECSIRoot *focusRoot = ECSILayout_RootOf(ECSILayout_GetFocus());
    u32 focusId = focusRoot == NULL ? 0 : focusRoot->id;
    ECSIOSWindow *focusWindow = ECSIWindow_OfRoot(focusRoot);

    if (focusId != WINDOW.focusRoot && focusWindow != NULL && (SDL_GetWindowFlags(focusWindow->window) & SDL_WINDOW_INPUT_FOCUS) == 0)
    {
        SDL_RaiseWindow(focusWindow->window);
    }

    WINDOW.focusRoot = focusId;
}

#pragma region Popups

/// @brief Frees what a popup is shown with; the Popups module calls it before it frees the popup.
static void ECSIWindow_ReleasePopup(ECSPopup popup)
{
    ECSIPopupView *view = popup->window;

    if (view == NULL)
    {
        return;
    }

    WINDOW.eventPopup = WINDOW.eventPopup == popup ? NULL : WINDOW.eventPopup;

    if (view->texture != NULL)
    {
        SDL_DestroyTexture(view->texture);
    }

    if (view->renderer != NULL)
    {
        SDL_DestroyRenderer(view->renderer);
    }

    if (view->window != NULL)
    {
        SDL_DestroyWindow(view->window);
    }

    SDL_free(view);
    popup->window = NULL;
}

/// @brief Finds where a popup shows in its panel's OS window: below its anchor, or above it if there is no room below. Drawn inside the OS window, it is also kept inside it.
static SDL_FRect ECSIWindow_PlacePopup(ECSPopup popup, const ECSIOSWindow *os, bool inside)
{
    ECSPanel panel = popup->panel;
    const ECSPopupDesc *desc = &popup->desc;
    SDL_FRect rect = {panel->x + desc->anchorX, panel->y + desc->anchorY + desc->anchorHeight, desc->width, desc->height};
    f32 above = panel->y + desc->anchorY - desc->height;

    if (rect.y + rect.h > os->height && above >= 0.0f)
    {
        rect.y = above;
    }

    if (inside)
    {
        rect.x = SDL_max(0.0f, SDL_min(rect.x, os->width - rect.w));
        rect.y = SDL_max(0.0f, SDL_min(rect.y, os->height - rect.h));
    }

    return rect;
}

/// @brief Makes how a popup is shown: an SDL popup window with its renderer, or nothing more when the video driver has none.
static ECSIPopupView *ECSIWindow_MakePopupView(ECSPopup popup, const ECSIOSWindow *os, SDL_FRect rect)
{
    ECSIPopupView *view = SDL_calloc(1, sizeof(ECSIPopupView));

    if (view == NULL)
    {
        return NULL;
    }

    view->rootId = os->rootId;
    popup->window = view;

    if (WINDOW.noPopupWindows)
    {
        return view;
    }

    SDL_WindowFlags flags = popup->desc.kind == ECSPopupKind_Menu ? SDL_WINDOW_POPUP_MENU : SDL_WINDOW_TOOLTIP;
    view->window = SDL_CreatePopupWindow(os->window, (int)rect.x, (int)rect.y, (int)rect.w, (int)rect.h, flags | SDL_WINDOW_TRANSPARENT);
    view->renderer = view->window == NULL ? NULL : SDL_CreateRenderer(view->window, NULL);

    // a driver without popup windows, such as the offscreen driver, gets them drawn inside the OS windows from now on
    if (view->renderer == NULL)
    {
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Popups are drawn inside the OS windows: %s", SDL_GetError());
        WINDOW.noPopupWindows = view->window == NULL;

        if (view->window != NULL)
        {
            SDL_DestroyWindow(view->window);
            view->window = NULL;
        }
    }
    else
    {
        SDL_SetRenderVSync(view->renderer, SDL_RENDERER_VSYNC_DISABLED);
    }

    return view;
}

/// @brief Places each open popup, draws its pixels if it needs it, and puts them in its texture. A popup window is drawn and presented with ECSIWindow_PresentPopups.
static void ECSIWindow_UpdatePopups(void)
{
    // a popup's Draw may open or close popups, so the list is read again for each one
    for (usz i = 0;; i++)
    {
        usz count = 0;
        ECSPopup *popups = ECSIPopups_GetOpen(&count);

        if (i >= count)
        {
            break;
        }

        ECSPopup popup = popups[i];
        ECSIOSWindow *os = ECSIWindow_OfRoot(ECSILayout_RootOf(popup->panel));
        ECSIPopupView *view = popup->window;

        if (os == NULL)
        {
            continue;
        }

        // a popup whose panel moved to another OS window is shown there anew
        if (view != NULL && view->rootId != os->rootId)
        {
            ECSIWindow_ReleasePopup(popup);
            view = NULL;
        }

        SDL_FRect rect = ECSIWindow_PlacePopup(popup, os, view == NULL ? WINDOW.noPopupWindows : view->window == NULL);
        view = view != NULL ? view : ECSIWindow_MakePopupView(popup, os, rect);

        if (view == NULL)
        {
            continue;
        }

        if (view->window != NULL && (rect.x != view->rect.x || rect.y != view->rect.y || rect.w != view->rect.w || rect.h != view->rect.h))
        {
            SDL_SetWindowPosition(view->window, (int)rect.x, (int)rect.y);
            SDL_SetWindowSize(view->window, (int)rect.w, (int)rect.h);
        }

        view->rect = rect;
        SDL_Renderer *renderer = view->renderer != NULL ? view->renderer : os->renderer;
        bool drawn = ECSIPopup_Draw(popup);
        SDL_Surface *pixels = popup->pixels;

        if (pixels == NULL)
        {
            continue;
        }

        if (view->texture != NULL && (view->texture->w != pixels->w || view->texture->h != pixels->h || SDL_GetRendererFromTexture(view->texture) != renderer))
        {
            SDL_DestroyTexture(view->texture);
            view->texture = NULL;
        }

        if (view->texture == NULL)
        {
            // a popup's pixels have premultiplied alpha, so what it leaves out shows what is below
            view->texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, pixels->w, pixels->h);
            SDL_SetTextureBlendMode(view->texture, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
            drawn = true;
        }

        if (drawn && view->texture != NULL)
        {
            SDL_UpdateTexture(view->texture, NULL, pixels->pixels, pixels->pitch);
        }
    }
}

/// @brief Draws the popups that are drawn inside an OS window, over everything else, the oldest first.
static void ECSIWindow_ShowPopupsIn(const ECSIOSWindow *os)
{
    usz count = 0;
    ECSPopup *popups = ECSIPopups_GetOpen(&count);

    for (usz i = 0; i < count; i++)
    {
        ECSIPopupView *view = popups[i]->window;

        if (view != NULL && view->window == NULL && view->texture != NULL && view->rootId == os->rootId && ECSIWindow_OfRoot(ECSILayout_RootOf(popups[i]->panel)) == os)
        {
            SDL_RenderTexture(os->renderer, view->texture, NULL, &view->rect);
        }
    }
}

/// @brief Draws and presents the SDL popup windows.
static void ECSIWindow_PresentPopups(void)
{
    usz count = 0;
    ECSPopup *popups = ECSIPopups_GetOpen(&count);

    for (usz i = 0; i < count; i++)
    {
        ECSIPopupView *view = popups[i]->window;

        if (view != NULL && view->renderer != NULL)
        {
            SDL_SetRenderDrawColor(view->renderer, 0, 0, 0, 0);
            SDL_RenderClear(view->renderer);
            SDL_RenderTexture(view->renderer, view->texture, NULL, NULL);
            SDL_RenderPresent(view->renderer);
        }
    }
}

#pragma endregion Popups

#pragma endregion OS Windows

/// @brief Reads a colour written as "#RRGGBB" or "#RRGGBBAA".
static bool ECSIWindow_ParseColor(const char *text, Clay_Color *retColor)
{
    usz length = SDL_strlen(text);
    u8 parts[4] = {0, 0, 0, 255};

    if ((length != 7 && length != 9) || text[0] != '#')
    {
        return false;
    }

    for (usz i = 1; i < length; i++)
    {
        if (!SDL_isxdigit((unsigned char)text[i]))
        {
            return false;
        }
    }

    for (usz i = 0; 2 * i + 1 < length; i++)
    {
        char pair[3] = {text[2 * i + 1], text[2 * i + 2], '\0'};
        parts[i] = (u8)SDL_strtoul(pair, NULL, 16);
    }

    *retColor = (Clay_Color){parts[0], parts[1], parts[2], parts[3]};
    return true;
}

/// @brief Reads a colour setting. A value that is not a colour is reported, and the core's settings file's colour is used.
/// @return false if neither is a colour.
static bool ECSIWindow_ReadColor(const char *name, Clay_Color *retColor)
{
    if (ECSIWindow_ParseColor(ECSValue_GetString(ECSSetting_Get(name), ""), retColor))
    {
        return true;
    }

    if (ECSIWindow_ParseColor(ECSValue_GetString(ECSISettings_GetDefault(name), ""), retColor))
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Setting '%s' must be a colour such as \"#18191C\" or \"#18191CFF\"; the default is used.", name);
        return true;
    }

    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "The core's settings file gives '%s' no colour such as \"#18191C\" or \"#18191CFF\".", name);
    return false;
}

/// @brief Reads a number setting of the window, in layout units; a negative value counts as 0.
static f32 ECSIWindow_ReadNumber(const char *name)
{
    return (f32)SDL_max(0.0, ECSValue_GetNumber(ECSSetting_Get(name), 0.0));
}

/// @brief Reads the window's settings, except ecs.vsync and the ones read when the window opens.
/// @return false if a colour setting and its default are not colours.
static bool ECSIWindow_Read(void)
{
    bool valid = true;

    for (usz i = 0; i < ECSIColor_Count; i++)
    {
        valid = ECSIWindow_ReadColor(OPENECS_WINDOW_SETTINGS[i].name, &WINDOW.colors[i]) && valid;
    }

    WINDOW.fontSize = ECSIWindow_ReadNumber("ecs.fontSize");
    WINDOW.gripHeight = ECSIWindow_ReadNumber("ecs.gripHeight");
    WINDOW.gripZone = ECSIWindow_ReadNumber("ecs.gripZone");
    WINDOW.dragThreshold = ECSIWindow_ReadNumber("ecs.dragThreshold");
    WINDOW.dockEdge = ECSIWindow_ReadNumber("ecs.dockEdge");
    WINDOW.splitDepth = ECSIWindow_ReadNumber("ecs.splitDepth");
    WINDOW.tabScrollStep = ECSIWindow_ReadNumber("ecs.tabScrollStep");
    return valid;
}

/// @brief The Changed function of the window's settings.
static void ECSIWindow_ReadSettings(void *data)
{
    (void)data;
    (void)ECSIWindow_Read();
    ECSILayout_RequestFrame();
}

/// @brief Reads ecs.vsync and sets the main renderer's vsync. Also the Changed function of ecs.vsync.
static void ECSIWindow_ReadVsync(void *data)
{
    (void)data;

    SDL_Renderer *renderer = WINDOW.windows[0]->renderer;
    i64 percent = SDL_clamp(ECSValue_GetInteger(ECSSetting_Get("ecs.vsync"), 0), 0, 100);
    WINDOW.framePercent = percent;
    ECSILayout_RequestFrame();

    // a whole fraction of the refresh rate, such as 50%, lets the display wait for every second refresh; other rates wait for every refresh and are limited by the core
    if (percent == 0)
    {
        SDL_SetRenderVSync(renderer, SDL_RENDERER_VSYNC_DISABLED);
    }
    else if (100 % percent != 0 || !SDL_SetRenderVSync(renderer, (int)(100 / percent)))
    {
        SDL_SetRenderVSync(renderer, 1);
    }
}

/// @brief Draws a frame of an OS window without presenting it. The layout is updated before.
static void ECSIWindow_DrawFrame(ECSIOSWindow *os, u64 nowTicks)
{
    ECSIRoot *root = ECSIWindow_Root(os);
    ECSIDrawContext context = {os->renderer, nowTicks};

    // the interface's commands point to the panels' titles, so panels draw first; their Draw may change a title
    if (root != NULL)
    {
        ECSILayout_ForEachGroup(root, ECSIWindow_DrawGroup, &context);
    }

    Clay_RenderCommandArray commands = ECSIWindow_DeclareInterface(os, root);
    ECSIWindow_FitTabs(os);

    Clay_Color background = WINDOW.colors[ECSIColor_Background];
    SDL_SetRenderDrawColor(os->renderer, (u8)background.r, (u8)background.g, (u8)background.b, (u8)background.a);
    SDL_RenderClear(os->renderer);

    if (root != NULL)
    {
        ECSILayout_ForEachGroup(root, ECSIWindow_ShowGroup, &context);
    }

    SDL_Clay_RenderClayCommands(&os->clayRenderer, &commands);
    ECSIWindow_ShowPopupsIn(os);
}

/// @brief Updates the layout and draws a frame of every shown OS window, without presenting them.
static void ECSIWindow_DrawFrames(u64 nowTicks)
{
    // some drivers accept vsync but do not wait for it, so frames are limited a little above the rate ecs.vsync asks for; a working vsync still sets the pace
    const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(WINDOW.windows[0]->window));
    f32 rate = mode != NULL && mode->refresh_rate > 0.0f ? mode->refresh_rate : OPENECS_FALLBACK_FRAME_RATE;
    f64 limit = (f64)rate * (f64)WINDOW.framePercent / 100.0 * (f64)OPENECS_FRAME_RATE_MARGIN;
    WINDOW.frameNanoseconds = WINDOW.framePercent == 0 ? 0 : (u64)((f64)SDL_NS_PER_SECOND / limit);
    WINDOW.lastFrameTicks = nowTicks;
    ECSIWindow_Sync();
    ECSILayout_Update();
    ECSIWindow_UpdatePopups();

    for (usz i = 0; i < arrlenu(WINDOW.windows); i++)
    {
        if (ECSIWindow_IsShown(WINDOW.windows[i]))
        {
            ECSIWindow_DrawFrame(WINDOW.windows[i], nowTicks);
        }
    }
}

/// @brief Presents the frame of every shown OS window and popup window.
static void ECSIWindow_Present(void)
{
    ECSIWindow_PresentPopups();

    for (usz i = 0; i < arrlenu(WINDOW.windows); i++)
    {
        if (ECSIWindow_IsShown(WINDOW.windows[i]))
        {
            SDL_RenderPresent(WINDOW.windows[i]->renderer);
        }
    }
}

#pragma endregion Source Only

SHUResult ECSIWindow_Initialize(const char *title)
{
    SDL_assert(title != NULL);

    for (usz i = 0; i < SDL_arraysize(OPENECS_WINDOW_SETTINGS); i++)
    {
        ECSSettingDesc desc = OPENECS_WINDOW_SETTINGS[i];
        desc.Changed = ECSIWindow_ReadSettings;
        SHU_ReturnResult(ECSISettings_DeclareCore(&desc));
    }

    // the core's settings file must give colours, because a colour that cannot be read falls back to it
    if (!ECSIWindow_Read())
    {
        return SHUResult_ErrBadData;
    }

    SHU_ReturnResult(ECSIWindow_Open(title), ECSIWindow_Terminate(););

    const ECSSettingDesc vsync = {
        .name = "ecs.vsync",
        .type = ECSSettingType_Integer,
        .description = "Frame rate in percent of the display's refresh rate: 100 waits for every refresh, 50 for every second one, 0 turns vsync off",
        .Changed = ECSIWindow_ReadVsync,
    };

    SHU_ReturnResult(ECSISettings_DeclareCore(&vsync), ECSIWindow_Terminate(););
    ECSIWindow_ReadVsync(NULL);
    ECSILayout_SetForget(ECSIWindow_Forget);
    ECSIPopups_SetRelease(ECSIWindow_ReleasePopup);
    return SHUResult_Ok;
}

void ECSIWindow_Terminate(void)
{
    ECSILayout_SetForget(NULL);
    ECSIPopups_SetRelease(NULL);

    // pop-out windows close before the main window
    while (arrlenu(WINDOW.windows) > 0)
    {
        ECSIWindow_CloseOS(arrpop(WINDOW.windows));
    }

    arrfree(WINDOW.windows);

    for (usz i = 0; i < SDL_arraysize(WINDOW.cursors); i++)
    {
        SDL_DestroyCursor(WINDOW.cursors[i]);
    }

    if (WINDOW.fonts[0] != NULL)
    {
        TTF_CloseFont(WINDOW.fonts[0]);
    }

    if (TTF_WasInit() > 0)
    {
        TTF_Quit();
    }

    SDL_free(WINDOW.title);
    SDL_zero(WINDOW);
}

SDL_Window *ECSIWindow_GetMain(void)
{
    return arrlenu(WINDOW.windows) > 0 ? WINDOW.windows[0]->window : NULL;
}

SDL_Window *ECSIWindow_Get(usz number)
{
    // the windows are synced first, so a root made since the last frame has its window
    ECSIWindow_Sync();
    usz count = 0;
    ECSIRoot *const *roots = ECSILayout_GetRoots(&count);
    ECSIOSWindow *os = number >= 1 && number <= count ? ECSIWindow_OfRoot(roots[number - 1]) : NULL;
    return os == NULL ? NULL : os->window;
}

usz ECSIWindow_NumberOf(ECSPanel panel)
{
    usz count = 0;
    ECSIRoot *const *roots = ECSILayout_GetRoots(&count);
    ECSIRoot *root = ECSILayout_RootOf(panel);

    for (usz i = 0; root != NULL && i < count; i++)
    {
        if (roots[i] == root)
        {
            return i + 1;
        }
    }

    return 0;
}

SDL_Window *ECSIWindow_OfPanel(ECSPanel panel)
{
    ECSIOSWindow *os = ECSIWindow_OfRoot(ECSILayout_RootOf(panel));
    return os != NULL ? os->window : ECSIWindow_GetMain();
}

ECSIRoot *ECSIWindow_GetRoot(SDL_WindowID id, bool *retMain)
{
    ECSIOSWindow *os = ECSIWindow_Find(id);

    if (retMain != NULL)
    {
        *retMain = os == NULL || os->rootId == 0;
    }

    return ECSIWindow_Root(os);
}

void ECSIWindow_SetEventWindow(SDL_WindowID id)
{
    WINDOW.event = ECSIWindow_Find(id);
    WINDOW.eventPopup = NULL;

    // an SDL popup window's events go to its popup, with positions in it
    usz count = 0;
    ECSPopup *popups = ECSIPopups_GetOpen(&count);

    for (usz i = 0; i < count; i++)
    {
        ECSIPopupView *view = popups[i]->window;

        if (view != NULL && view->window != NULL && SDL_GetWindowID(view->window) == id)
        {
            WINDOW.eventPopup = popups[i];
        }
    }
}

ECSPopup ECSIWindow_PopupAt(f32 x, f32 y, f32 *retX, f32 *retY)
{
    SDL_assert(retX != NULL);
    SDL_assert(retY != NULL);

    *retX = x;
    *retY = y;

    if (WINDOW.eventPopup != NULL)
    {
        return WINDOW.eventPopup;
    }

    // the newest popup drawn inside the event window that holds the point
    usz count = 0;
    ECSPopup *popups = ECSIPopups_GetOpen(&count);

    for (usz i = count; i > 0; i--)
    {
        ECSIPopupView *view = popups[i - 1]->window;

        if (view != NULL && view->window == NULL && WINDOW.event != NULL && view->rootId == WINDOW.event->rootId &&
            ECSIWindow_Contains(x, y, view->rect.x, view->rect.y, view->rect.w, view->rect.h))
        {
            *retX = x - view->rect.x;
            *retY = y - view->rect.y;
            return popups[i - 1];
        }
    }

    return NULL;
}

void ECSIWindow_ToPopup(ECSPopup popup, f32 x, f32 y, f32 *retX, f32 *retY)
{
    SDL_assert(popup != NULL);
    SDL_assert(retX != NULL);
    SDL_assert(retY != NULL);

    // an SDL popup window's own events are in its positions already
    ECSIPopupView *view = popup->window;
    bool inside = view != NULL && view->window == NULL && popup != WINDOW.eventPopup;
    *retX = inside ? x - view->rect.x : x;
    *retY = inside ? y - view->rect.y : y;
}

bool ECSIWindow_PopupRect(ECSPopup popup, SDL_FRect *retRect, usz *retWindow)
{
    SDL_assert(popup != NULL);
    SDL_assert(retRect != NULL);
    SDL_assert(retWindow != NULL);

    ECSIPopupView *view = popup->window;

    if (view == NULL)
    {
        return false;
    }

    *retRect = view->rect;
    *retWindow = ECSIWindow_NumberOf(popup->panel);
    return true;
}

ECSPanel ECSIWindow_PanelAt(f32 x, f32 y, f32 *retX, f32 *retY)
{
    SDL_assert(retX != NULL);
    SDL_assert(retY != NULL);

    ECSIOSWindow *os = ECSIWindow_Under(x, y, retX, retY);
    return ECSILayout_PanelAt(ECSIWindow_Root(os), *retX, *retY);
}

i32 ECSIWindow_GetFrameWait(void)
{
    // windows that cannot be seen are not drawn; showing one again asks for a frame
    bool visible = false;

    for (usz i = 0; i < arrlenu(WINDOW.windows); i++)
    {
        visible = visible || (SDL_GetWindowFlags(WINDOW.windows[i]->window) & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED | SDL_WINDOW_OCCLUDED)) == 0;
    }

    if (!visible || (!ECSILayout_WantsFrame() && !ECSIPopups_WantsFrame()))
    {
        return -1;
    }

    u64 next = WINDOW.lastFrameTicks + WINDOW.frameNanoseconds;
    u64 now = SDL_GetTicksNS();
    return now >= next ? 0 : (i32)((next - now + SDL_NS_PER_MS - 1) / SDL_NS_PER_MS);
}

void ECSIWindow_Render(u64 nowTicks)
{
    ECSIWindow_DrawFrames(nowTicks);
    ECSIWindow_Present();
}

SHUResult ECSIWindow_Screenshot(const char *path, usz number)
{
    SDL_assert(path != NULL);

    // the picture is read before it is presented, because presenting may discard it
    ECSIWindow_DrawFrames(SDL_GetTicksNS());
    usz count = 0;
    ECSIRoot *const *roots = ECSILayout_GetRoots(&count);
    ECSIOSWindow *os = number >= 1 && number <= count ? ECSIWindow_OfRoot(roots[number - 1]) : NULL;
    SDL_Surface *picture = os == NULL ? NULL : SDL_RenderReadPixels(os->renderer, NULL);
    bool saved = picture != NULL && SDL_SavePNG(picture, path);

    if (!saved)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cannot save a screenshot of window %zu to '%s': %s", number, path, os == NULL ? "there is no such window" : SDL_GetError());
    }

    SDL_DestroySurface(picture);
    ECSIWindow_Present();
    return saved ? SHUResult_Ok : SHUResult_ErrFile;
}

bool ECSIWindow_PointerDown(f32 x, f32 y)
{
    ECSIOSWindow *os = WINDOW.event;

    if (ECSILayout_DividerAt(ECSIWindow_Root(os), x, y, &WINDOW.dragSplit, &WINDOW.dragDivider))
    {
        return true;
    }

    for (usz i = 0; i < arrlenu(os->tabs); i++)
    {
        ECSINode *group = os->tabs[i].group;
        ECSPanel panel = group->panels[os->tabs[i].index];

        // closing may ask about unsaved work; the tabs of the last frame are not used after it
        if (ECSIWindow_OnElement(os, CLAY_IDI("TabClose", (u32)i), x, y))
        {
            ECSLayout_Close(panel);
            return true;
        }

        if (ECSIWindow_OnElement(os, CLAY_IDI("Tab", (u32)i), x, y))
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
    ECSINode *group = ECSIWindow_TabRowGroupAt(os, x, y);

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
    if (WINDOW.gripGroup != NULL && WINDOW.gripWindow == os && ECSIWindow_OnElement(os, CLAY_ID("Grip"), x, y))
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
        ECSILayout_DividerAt(ECSIWindow_Root(WINDOW.event), x, y, &split, &divider);
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
        bool moved = SDL_fabsf(x - WINDOW.dragStartX) + SDL_fabsf(y - WINDOW.dragStartY) >= WINDOW.dragThreshold;

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
            WINDOW.drop = ECSIWindow_FindDrop(x, y, &WINDOW.dropWindow, &WINDOW.dropRect);
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
    ECSILayout_ForEachGroup(ECSIWindow_Root(WINDOW.event), ECSIWindow_FindGripGroup, &hit);

    if (hit.group != WINDOW.gripGroup || WINDOW.gripWindow != WINDOW.event)
    {
        WINDOW.gripGroup = hit.group;
        WINDOW.gripWindow = WINDOW.event;
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
    ECSIOSWindow *os = WINDOW.event;

    for (usz i = 0; i < arrlenu(os->tabs); i++)
    {
        if (ECSIWindow_OnElement(os, CLAY_IDI("Tab", (u32)i), x, y))
        {
            return os->tabs[i].group->panels[os->tabs[i].index];
        }
    }

    if (WINDOW.gripGroup != NULL && WINDOW.gripWindow == os && ECSIWindow_OnElement(os, CLAY_ID("Grip"), x, y))
    {
        return WINDOW.gripGroup->panels[0];
    }

    return NULL;
}

ECSPanel ECSIWindow_TabRowAt(f32 x, f32 y)
{
    ECSINode *group = ECSIWindow_TabRowGroupAt(WINDOW.event, x, y);
    return group == NULL ? NULL : group->panels[group->shown];
}

bool ECSIWindow_ScrollTabs(f32 x, f32 y, f32 steps)
{
    ECSINode *group = ECSIWindow_TabRowGroupAt(WINDOW.event, x, y);

    if (group == NULL)
    {
        return false;
    }

    group->tabScroll = SDL_clamp(group->tabScroll + steps * WINDOW.tabScrollStep, 0.0f, SDL_max(0.0f, group->tabsWidth - group->width));
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

    // the panel menu opens in the OS window of the event that opens it, and its submenus open beside it
    if (level == 0)
    {
        WINDOW.menuWindow = WINDOW.event;
    }

    f32 windowWidth = WINDOW.menuWindow->width;
    f32 windowHeight = WINDOW.menuWindow->height;

    if (count == 0)
    {
        return;
    }

    menu->lines = lines;
    menu->count = count;

    // the menu's size is measured here, so it can be kept inside the OS window
    TTF_Font *font = WINDOW.fonts[0];
    TTF_SetFontSize(font, WINDOW.fontSize);
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
    f32 x = anchor.x + anchor.w + width <= windowWidth ? anchor.x + anchor.w : anchor.x - width;
    f32 y = anchor.y - (level == 0 ? 0.0f : OPENECS_MENU_PADDING);
    menu->rect = (SDL_FRect){
        SDL_max(0.0f, SDL_min(x, windowWidth - width)),
        SDL_max(0.0f, SDL_min(y, windowHeight - height)),
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

    // a point in another OS window is on no menu
    if (WINDOW.event != WINDOW.menuWindow)
    {
        return false;
    }

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
    if (WINDOW.event != WINDOW.menuWindow)
    {
        return false;
    }

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

void ECSIWindow_ShowDataDrag(const char *type, f32 x, f32 y)
{
    WINDOW.dataType = type;
    WINDOW.dataWindow = type == NULL ? NULL : ECSIWindow_Under(x, y, &WINDOW.dataX, &WINDOW.dataY);
    ECSILayout_RequestFrame();
}

void ECSIWindow_ShowPrefixKeys(const char *const *lines, usz count)
{
    WINDOW.prefixLines = count == 0 ? NULL : lines;
    WINDOW.prefixLineCount = count;
    ECSILayout_RequestFrame();
}
