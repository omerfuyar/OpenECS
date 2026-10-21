#include "interface/Window.h"

#include "interface/SystemDrag.h"
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
/// @brief Size of the marks of a locked and a maximized group, and of the title bar's icons, relative to the font size.
#define OPENECS_ICON_SCALE 0.9f
/// @brief Width of a title bar button, relative to the title bar's height.
#define OPENECS_TITLE_BUTTON_WIDTH 1.5f
/// @brief How far from an OS window's edge the pointer resizes it, when OpenECS draws the title bar, in layout units.
#define OPENECS_RESIZE_EDGE 6.0f
/// @brief How long a panel dropped outside its OS window waits for the pointer to enter another one, where the system does not tell where windows are, in nanoseconds.
#define OPENECS_PENDING_DROP_NANOSECONDS (400 * SDL_NS_PER_MS)
/// @brief Longest wait of the main loop while the system carries a drag, in milliseconds.
#define OPENECS_SYSTEM_DRAG_WAIT 8
/// @brief Most menus open at once: the panel menu and its submenus.
#define OPENECS_MENU_DEPTH 4

/// @brief Widest a message dialog is, and the room it keeps from the OS window's edges, in layout units.
#define OPENECS_DIALOG_WIDTH 440.0f
#define OPENECS_DIALOG_MARGIN 24.0f

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
    {.name = "ecs.colorBackground", .type = ECSSettingType_Color, .description = "Colour between panels, such as \"#18191C\" or \"#18191CFF\""},
    {.name = "ecs.colorTabRow", .type = ECSSettingType_Color, .description = "Colour of tab rows"},
    {.name = "ecs.colorTab", .type = ECSSettingType_Color, .description = "Colour of tabs"},
    {.name = "ecs.colorTabShown", .type = ECSSettingType_Color, .description = "Colour of the shown tab"},
    {.name = "ecs.colorText", .type = ECSSettingType_Color, .description = "Colour of text"},
    {.name = "ecs.colorTextDim", .type = ECSSettingType_Color, .description = "Colour of dim text"},
    {.name = "ecs.colorAccent", .type = ECSSettingType_Color, .description = "Colour of the focus border, keys and highlights"},
    {.name = "ecs.colorPlaceholder", .type = ECSSettingType_Color, .description = "Colour of a placeholder panel"},
    {.name = "ecs.colorOverlay", .type = ECSSettingType_Color, .description = "Colour of menus, grips and the list of prefix keys"},
    {.name = "ecs.colorDrop", .type = ECSSettingType_Color, .description = "Colour of the place where a dragged panel lands"},
    {.name = "ecs.colorSelected", .type = ECSSettingType_Color, .description = "Colour of the selected menu entry"},
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
    {.name = "ecs.titleBar", .type = ECSSettingType_Bool, .description = "OpenECS draws the title bar of its OS windows, with buttons that minimize, maximize and close them, and edges that resize them; false leaves them to the system"},
    {.name = "ecs.titleBarHeight", .type = ECSSettingType_Number, .description = "Height of the title bar that OpenECS draws, in layout units"},
};

/// @brief A small picture of the core's interface, drawn with rectangles, so the font needs no symbols.
typedef enum ECSIIcon
{
    ECSIIcon_Locked = 0,
    ECSIIcon_Maximized,
    ECSIIcon_Minimize,
    ECSIIcon_Restore,
} ECSIIcon;

/// @brief A tab drawn in the last frame, so a click can find it.
typedef struct ECSITabRef
{
    ECSINode *group;
    usz index;
} ECSITabRef;

/// @brief A shown menu. Each line pair is an entry's key text and its label.
typedef struct ECSIMenuView
{
    const char *const *lines; // nullptr when this menu is closed
    usz count;
    usz selected;
    SDL_FRect rect;
    f32 itemHeight;
} ECSIMenuView;

/// @brief An OS window: the main window, or a pop-out window, with what the core draws in it.
typedef struct ECSIOSWindow
{
    u32 rootID; // the pop-out root it shows; 0 for the main window, which shows the current workspace's main root
    SDL_Window *window;
    SDL_Renderer *renderer;
    Clay_SDL3RendererData clayRenderer;
    Clay_Context *clay;
    void *clayMemory;
    f32 width; // in layout units
    f32 height;
    ECSITabRef *tabs; // stb_ds array of the tabs drawn in its last frame
    u64 raised;       // when it last opened or got the focus, counted across the OS windows; the highest is in front
} ECSIOSWindow;

/// @brief How a popup is shown: in an SDL popup window, or inside its panel's OS window when the video driver has no popup windows.
typedef struct ECSIPopupView
{
    u32 rootID;             // the root of the OS window it belongs to; it is made again if its panel moves to another one
    SDL_Window *window;     // SDL's popup window, or nullptr when it is drawn inside the OS window
    SDL_Renderer *renderer; // the popup window's
    SDL_Texture *texture;   // its pixels, made by the renderer that shows them
    SDL_FRect rect;         // where it shows, in the layout units of its panel's OS window
} ECSIPopupView;

/// @brief The GPU device that every OS window's renderer and every GPU panel shares. An offscreen renderer owns it, so it outlives the OS windows and the plugins, which release their GPU objects first.
static struct
{
    SDL_Renderer *owner;   // the offscreen GPU renderer that made the device
    SDL_GPUDevice *device; // nullptr without a GPU renderer
} GPU;

static struct
{
    ECSIOSWindow **windows; // stb_ds array: the main window, then the pop-out windows
    ECSIOSWindow *event;    // the OS window of the pointer event being handled; positions are in its layout units
    ECSPopup eventPopup;    // the popup whose SDL popup window has the pointer event being handled, or nullptr
    bool noPopupWindows;    // the video driver has no popup windows, so popups are drawn inside the OS windows
    ECSIOSWindow *gripWindow;
    ECSIOSWindow *dropWindow; // the OS window where the dragged panel lands, or nullptr for a pop-out
    ECSIOSWindow *dataWindow; // the OS window under the pointer while data is dragged
    ECSIOSWindow *menuWindow; // the OS window that shows the menus
    u32 focusRoot;            // the root of the focused panel in the last frame, so its OS window comes forward when the focus moves to it
    char *title;              // of every OS window
    char *hint;               // what the preset tells a new user, shown after the title in the title bar OpenECS draws, or nullptr
    TTF_Font *fonts[1];

    i64 framePercent;     // frame rate as a percentage of the refresh rate, from ecs.vsync; 0 for no vsync and no limit
    u64 frameNanoseconds; // shortest time between frames, from the display's refresh rate; 0 for no limit
    u64 lastFrameTicks;
    const char *const *prefixLines; // keys after the prefix and what they do, in pairs, or nullptr when the prefix is not pressed
    usz prefixLineCount;

    ECSINode *gripGroup; // lone group whose grip is shown, or nullptr
    ECSINode *dragSplit; // split whose divider is dragged, or nullptr
    usz dragDivider;      // the dragged divider follows this child

    u64 raises; // counts the OS windows' openings and focus changes, so the one in front is known (ECSIOSWindow.raised)

    ECSPanel dragPanel; // panel pressed on its tab or grip, or nullptr; it is dragged once the pointer moves far enough
    bool dragGroup;     // the whole group of dragPanel is dragged, pressed on its tab row
    bool dragFromGrip;  // the press was on a grip; a click without dragging opens the panel's menu
    bool dragLocked;    // the pressed grip is a locked group's, so it is never dragged
    bool dragging;
    f32 dragStartX;
    f32 dragStartY;
    ECSIDrop drop;     // where the dragged panel lands now
    bool dropOutside;  // the pointer is outside every OS window whose place is known, as on Wayland outside the window it was dragged from
    bool systemDrag;   // the system carries the drag, as on Wayland once the pointer leaves the window it was dragged from (SystemDrag)
    SDL_FRect dropRect; // the highlight of the drop place

    // where the system does not tell where windows are, as on Wayland, a panel released outside its OS window lands where the pointer enters next
    ECSPanel pendingPanel;   // the released panel, or nullptr
    bool pendingGroup;       // its whole group was dragged
    ECSIOSWindow *pendingFrom; // the OS window it was dragged from
    bool pendingPopOut;        // it pops out if no other OS window takes it; not when it is alone in its OS window already
    u64 pendingUntil;        // when it pops out instead

    const char *dataType; // type of the data being dragged, or nullptr
    f32 dataX;            // the pointer's position while data is dragged, in dataWindow
    f32 dataY;

    SDL_Cursor *cursors[SDL_SYSTEM_CURSOR_COUNT]; // made when first used
    SDL_SystemCursor cursor;                      // the pointer's shape now

    ECSIMenuView menus[OPENECS_MENU_DEPTH]; // the panel menu, then its open submenus

    const ECSIDialogView *dialog; // the message dialog that Input waits on, or nullptr
    ECSIOSWindow *dialogWindow;   // the OS window that shows it

    // from the settings
    Clay_Color colors[ECSIColor_Count];
    f32 fontSize;
    f32 gripHeight;
    f32 gripZone;
    f32 dragThreshold;
    f32 dockEdge;
    f32 splitDepth;
    f32 tabScrollStep;
    bool titleBar;
    f32 titleBarHeight;
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

/// @brief Gets the root an OS window shows, or nullptr if it shows none now: the main window shows the current workspace's main root, and a pop-out window its own root while its workspace is current.
static ECSIRoot *ECSIWindow_Root(const ECSIOSWindow *os)
{
    if (os == nullptr)
    {
        return nullptr;
    }

    if (os->rootID == 0)
    {
        usz count = 0;
        ECSIRoot *const *roots = ECSILayout_GetRoots(&count);
        return count > 0 ? roots[0] : nullptr;
    }

    bool shown = false;
    ECSIRoot *root = ECSILayout_FindRoot(os->rootID, &shown);
    return shown ? root : nullptr;
}

/// @brief Finds the OS window that shows a root of the current workspace.
static ECSIOSWindow *ECSIWindow_OfRoot(const ECSIRoot *root)
{
    for (usz i = 0; root != nullptr && i < arrlenu(WINDOW.windows); i++)
    {
        if (ECSIWindow_Root(WINDOW.windows[i]) == root)
        {
            return WINDOW.windows[i];
        }
    }

    return nullptr;
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

    return arrlenu(WINDOW.windows) > 0 ? WINDOW.windows[0] : nullptr;
}

/// @brief Checks whether an OS window is shown.
static bool ECSIWindow_IsShown(const ECSIOSWindow *os)
{
    return (SDL_GetWindowFlags(os->window) & SDL_WINDOW_HIDDEN) == 0 && ECSIWindow_Root(os) != nullptr;
}

/// @brief Checks whether the windows are on Wayland, which never tells where a window is on the screen, and gives a hidden window no refreshes.
static bool ECSIWindow_OnWayland(void)
{
    const char *driver = SDL_GetCurrentVideoDriver();
    return driver != nullptr && SDL_strcmp(driver, "wayland") == 0;
}

static bool ECSIWindow_Contains(f32 x, f32 y, f32 left, f32 top, f32 width, f32 height)
{
    return x >= left && y >= top && x < left + width && y < top + height;
}

/// @brief Gets the height of the title bar that OpenECS draws in its OS windows; 0 when the system draws them.
static f32 ECSIWindow_TitleHeight(void)
{
    return WINDOW.titleBar ? WINDOW.titleBarHeight : 0.0f;
}

/// @brief Gets the width of a title bar button.
static f32 ECSIWindow_TitleButtonWidth(void)
{
    return WINDOW.titleBarHeight * OPENECS_TITLE_BUTTON_WIDTH;
}

/// @brief Tells SDL which part of an OS window moves or resizes it, when OpenECS draws its title bar: the edges resize it, and the title bar moves it, apart from its buttons.
static SDL_HitTestResult SDLCALL ECSIWindow_HitTest(SDL_Window *window, const SDL_Point *point, void *data)
{
    (void)data;

    int width = 0;
    int height = 0;
    SDL_GetWindowSize(window, &width, &height);
    f32 x = (f32)point->x;
    f32 y = (f32)point->y;

    if ((SDL_GetWindowFlags(window) & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN)) == 0)
    {
        bool left = x < OPENECS_RESIZE_EDGE;
        bool right = x >= (f32)width - OPENECS_RESIZE_EDGE;
        bool top = y < OPENECS_RESIZE_EDGE;
        bool bottom = y >= (f32)height - OPENECS_RESIZE_EDGE;

        if (top || bottom || left || right)
        {
            return top && left       ? SDL_HITTEST_RESIZE_TOPLEFT
                   : top && right    ? SDL_HITTEST_RESIZE_TOPRIGHT
                   : bottom && left  ? SDL_HITTEST_RESIZE_BOTTOMLEFT
                   : bottom && right ? SDL_HITTEST_RESIZE_BOTTOMRIGHT
                   : top             ? SDL_HITTEST_RESIZE_TOP
                   : bottom          ? SDL_HITTEST_RESIZE_BOTTOM
                   : left            ? SDL_HITTEST_RESIZE_LEFT
                                     : SDL_HITTEST_RESIZE_RIGHT;
        }
    }

    return y < WINDOW.titleBarHeight && x < (f32)width - 3.0f * ECSIWindow_TitleButtonWidth() ? SDL_HITTEST_DRAGGABLE : SDL_HITTEST_NORMAL;
}

/// @brief Lets OpenECS draw an OS window's title bar, or leaves it to the system.
static void ECSIWindow_ApplyTitleBar(ECSIOSWindow *os)
{
    SDL_SetWindowBordered(os->window, !WINDOW.titleBar);
    SDL_SetWindowHitTest(os->window, WINDOW.titleBar ? ECSIWindow_HitTest : nullptr, nullptr);
}

/// @brief Finds the shown OS window under a point given in the event window's layout units: the event window itself, or another one that the point falls in on the screen.
/// @param retX Gets the point's position in the found window.
/// @param retY Gets the point's position in the found window.
/// @return The OS window, or nullptr if the point is outside every one.
static ECSIOSWindow *ECSIWindow_Under(f32 x, f32 y, f32 *retX, f32 *retY)
{
    ECSIOSWindow *event = WINDOW.event;
    *retX = x;
    *retY = y;

    // windows know their places on the screen, except on Wayland, where a point outside the event window falls in none
    if (event == nullptr || ECSIWindow_OnWayland())
    {
        return event == nullptr || ECSIWindow_Contains(x, y, 0.0f, 0.0f, event->width, event->height) ? event : nullptr;
    }

    int eventX = 0;
    int eventY = 0;
    SDL_GetWindowPosition(event->window, &eventX, &eventY);
    ECSIOSWindow *front = nullptr;

    // where windows overlap, the one in front takes the point; SDL does not tell the windows' order, so the one that last opened or got the focus counts as in front
    for (usz i = 0; i < arrlenu(WINDOW.windows); i++)
    {
        ECSIOSWindow *os = WINDOW.windows[i];
        int left = eventX;
        int top = eventY;

        if (!ECSIWindow_IsShown(os) || (os != event && !SDL_GetWindowPosition(os->window, &left, &top)))
        {
            continue;
        }

        f32 localX = x + (f32)(eventX - left);
        f32 localY = y + (f32)(eventY - top);

        if (ECSIWindow_Contains(localX, localY, 0.0f, 0.0f, os->width, os->height) && (front == nullptr || os->raised > front->raised))
        {
            front = os;
            *retX = localX;
            *retY = localY;
        }
    }

    return front;
}

/// @brief Forgets the nodes the window points to; the layout calls it after the trees change.
static void ECSIWindow_Forget(void)
{
    WINDOW.gripGroup = nullptr;
    WINDOW.dragSplit = nullptr;
    WINDOW.dragPanel = nullptr;
    WINDOW.dragging = false;
}

#pragma region Dragging

/// @brief Finds the group whose tab row is at a point of an OS window.
static ECSINode *ECSIWindow_TabRowGroupAt(const ECSIOSWindow *os, f32 x, f32 y)
{
    ECSINode *group = ECSILayout_GroupAt(ECSIWindow_Root(os), x, y);
    return group != nullptr && arrlenu(group->panels) >= 2 && y < group->y + ECSILayout_GetTabRowHeight() ? group : nullptr;
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
    if (os == nullptr || root == nullptr)
    {
        *retWindow = nullptr;
        return (ECSIDrop){.zone = ECSIZone_PopOut};
    }

    // the layout fills the OS window below the title bar
    f32 top = root->top;
    f32 width = root->width;
    f32 height = root->height;

    // an empty window takes the panel whole
    if (root->tree == nullptr)
    {
        *retRect = (SDL_FRect){0.0f, top, width, height};
        return (ECSIDrop){.zone = ECSIZone_Center, .root = root};
    }

    if (x < WINDOW.dockEdge || x >= width - WINDOW.dockEdge || y < top + WINDOW.dockEdge || y >= top + height - WINDOW.dockEdge)
    {
        ECSIZone zone = x < WINDOW.dockEdge            ? ECSIZone_WindowLeft
                        : x >= width - WINDOW.dockEdge ? ECSIZone_WindowRight
                        : y < top + WINDOW.dockEdge    ? ECSIZone_WindowTop
                                                       : ECSIZone_WindowBottom;

        *retRect = zone == ECSIZone_WindowLeft    ? (SDL_FRect){0.0f, top, width / 4.0f, height}
                   : zone == ECSIZone_WindowRight ? (SDL_FRect){width * 0.75f, top, width / 4.0f, height}
                   : zone == ECSIZone_WindowTop   ? (SDL_FRect){0.0f, top, width, height / 4.0f}
                                                  : (SDL_FRect){0.0f, top + height * 0.75f, width, height / 4.0f};
        return (ECSIDrop){.zone = zone, .root = root};
    }

    ECSINode *group = ECSILayout_GroupAt(root, x, y);

    if (group == nullptr || arrlenu(group->panels) == 0)
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

    if (source == nullptr || drop->zone == ECSIZone_None)
    {
        return false;
    }

    // a whole group moves when the group is dragged or holds only the dragged panel; a group that fills its window stays in it
    bool whole = WINDOW.dragGroup || arrlenu(source->panels) == 1;

    if (drop->zone == ECSIZone_PopOut)
    {
        return !whole || sourceRoot->tree != source;
    }

    if (drop->group == nullptr)
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

    if (WINDOW.cursors[cursor] == nullptr)
    {
        WINDOW.cursors[cursor] = SDL_CreateSystemCursor(cursor);
    }

    if (WINDOW.cursors[cursor] != nullptr && SDL_SetCursor(WINDOW.cursors[cursor]))
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

/// @brief Declares an icon for Clay, the size of the core's font, inside the open element.
static void ECSIWindow_DeclareIcon(ECSIIcon icon, Clay_Color color)
{
    f32 size = SDL_roundf(WINDOW.fontSize * OPENECS_ICON_SCALE);
    u16 line = (u16)SDL_max(1.0f, SDL_roundf(size / 8.0f));
    Clay_Color behind = WINDOW.colors[ECSIColor_Overlay];

    CLAY_AUTO_ID({.layout = {.sizing = {CLAY_SIZING_FIXED(size), CLAY_SIZING_FIXED(size)}, .layoutDirection = CLAY_TOP_TO_BOTTOM, .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_BOTTOM}}})
    {
        switch (icon)
        {
        case ECSIIcon_Locked:
            // a padlock: the shackle's arch, then the body
            CLAY_AUTO_ID({.layout = {.sizing = {CLAY_SIZING_FIXED(size * 0.56f), CLAY_SIZING_FIXED(size * 0.45f)}}, .border = {.color = color, .width = {line, line, line, 0, 0}}, .cornerRadius = {size * 0.28f, size * 0.28f, 0, 0}}) {}
            CLAY_AUTO_ID({.layout = {.sizing = {CLAY_SIZING_FIXED(size * 0.84f), CLAY_SIZING_FIXED(size * 0.55f)}}, .backgroundColor = color, .cornerRadius = CLAY_CORNER_RADIUS(size * 0.1f)}) {}
            break;
        case ECSIIcon_Maximized:
            // a window that fills the screen: a frame with a thick top
            CLAY_AUTO_ID({.layout = {.sizing = {CLAY_SIZING_FIXED(size * 0.9f), CLAY_SIZING_FIXED(size * 0.8f)}}, .border = {.color = color, .width = {line, line, (u16)(line * 2), line, 0}}}) {}
            break;
        case ECSIIcon_Minimize:
            CLAY_AUTO_ID({.layout = {.sizing = {CLAY_SIZING_FIXED(size * 0.8f), CLAY_SIZING_FIXED(line)}}, .backgroundColor = color}) {}
            break;
        case ECSIIcon_Restore:
            // two windows, one behind the other
            CLAY_AUTO_ID({.layout = {.sizing = {CLAY_SIZING_FIXED(size), CLAY_SIZING_FIXED(size)}}})
            {
                CLAY_AUTO_ID({.layout = {.sizing = {CLAY_SIZING_FIXED(size * 0.7f), CLAY_SIZING_FIXED(size * 0.6f)}}, .border = {.color = color, .width = {line, line, line, line, 0}}, .floating = {.attachTo = CLAY_ATTACH_TO_PARENT, .offset = {size * 0.3f, size * 0.1f}}}) {}
                CLAY_AUTO_ID({.layout = {.sizing = {CLAY_SIZING_FIXED(size * 0.7f), CLAY_SIZING_FIXED(size * 0.6f)}}, .backgroundColor = behind, .border = {.color = color, .width = {line, line, line, line, 0}}, .floating = {.attachTo = CLAY_ATTACH_TO_PARENT, .offset = {0.0f, size * 0.35f}}}) {}
            }
            break;
        }
    }
}

/// @brief Gets the width of the marks that show a group is locked or maximized, with the gap before each.
static f32 ECSIWindow_MarksWidth(const ECSINode *group, const ECSIRoot *root)
{
    usz marks = (group->locked ? 1u : 0u) + (root != nullptr && root->maximized == group ? 1u : 0u);
    return (f32)marks * (SDL_roundf(WINDOW.fontSize * OPENECS_ICON_SCALE) + OPENECS_MENU_ITEM_PADDING);
}

/// @brief Declares the marks that show a group is locked or maximized, inside the open element.
static void ECSIWindow_DeclareMarks(const ECSINode *group, const ECSIRoot *root)
{
    if (group->locked)
    {
        ECSIWindow_DeclareIcon(ECSIIcon_Locked, WINDOW.colors[ECSIColor_TextDim]);
    }

    if (root != nullptr && root->maximized == group)
    {
        ECSIWindow_DeclareIcon(ECSIIcon_Maximized, WINDOW.colors[ECSIColor_TextDim]);
    }
}

/// @brief Declares the title bar of an OS window for Clay: the tool's name, and buttons that minimize, maximize or restore, and close the window.
static void ECSIWindow_DeclareTitleBar(ECSIOSWindow *os)
{
    f32 buttonWidth = ECSIWindow_TitleButtonWidth();
    bool maximized = (SDL_GetWindowFlags(os->window) & SDL_WINDOW_MAXIMIZED) != 0;
    Clay_Color color = WINDOW.colors[ECSIColor_TextDim];

    CLAY(CLAY_ID("TitleBar"), {
                                  .layout = {
                                      .sizing = {CLAY_SIZING_FIXED(os->width), CLAY_SIZING_FIXED(WINDOW.titleBarHeight)},
                                      .padding = {12, 0, 0, 0},
                                      .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                  },
                                  .backgroundColor = WINDOW.colors[ECSIColor_TabRow],
                                  .floating = {.attachTo = CLAY_ATTACH_TO_ROOT, .offset = {0.0f, 0.0f}},
                              })
    {
        CLAY_TEXT(ECSIWindow_ClayText(WINDOW.title), CLAY_TEXT_CONFIG({.textColor = WINDOW.colors[ECSIColor_Text], .fontSize = (u16)WINDOW.fontSize, .wrapMode = CLAY_TEXT_WRAP_NONE}));

        // the preset's hint follows the title, cut at the buttons when the window is narrow
        CLAY_AUTO_ID({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)}, .padding = {16, 8, 0, 0}}, .clip = {.horizontal = true}})
        {
            if (WINDOW.hint != nullptr)
            {
                CLAY_TEXT(ECSIWindow_ClayText(WINDOW.hint), CLAY_TEXT_CONFIG({.textColor = color, .fontSize = (u16)WINDOW.fontSize, .wrapMode = CLAY_TEXT_WRAP_NONE}));
            }
        }

        const ECSIIcon icons[] = {ECSIIcon_Minimize, maximized ? ECSIIcon_Restore : ECSIIcon_Maximized};

        for (usz i = 0; i < SDL_arraysize(icons); i++)
        {
            CLAY_AUTO_ID({.layout = {.sizing = {CLAY_SIZING_FIXED(buttonWidth), CLAY_SIZING_GROW(0)}, .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}}})
            {
                ECSIWindow_DeclareIcon(icons[i], color);
            }
        }

        CLAY_AUTO_ID({.layout = {.sizing = {CLAY_SIZING_FIXED(buttonWidth), CLAY_SIZING_GROW(0)}, .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}}})
        {
            CLAY_TEXT(CLAY_STRING("\u00D7"), CLAY_TEXT_CONFIG({.textColor = color, .fontSize = (u16)(WINDOW.fontSize * 1.4f), .wrapMode = CLAY_TEXT_WRAP_NONE}));
        }
    }
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

        // a locked or maximized group shows its marks at the end of its tab row
        ECSIRoot *root = ECSIWindow_Root(os);
        f32 marksWidth = ECSIWindow_MarksWidth(group, root);

        if (marksWidth > 0.0f)
        {
            CLAY_AUTO_ID({
                .layout = {
                    .sizing = {CLAY_SIZING_FIXED(marksWidth + OPENECS_MENU_ITEM_PADDING), CLAY_SIZING_FIXED(ECSILayout_GetTabRowHeight())},
                    .padding = {(u16)OPENECS_MENU_ITEM_PADDING, 0, 0, 0},
                    .childGap = (u16)OPENECS_MENU_ITEM_PADDING,
                    .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                },
                .backgroundColor = WINDOW.colors[ECSIColor_TabRow],
                .floating = {.attachTo = CLAY_ATTACH_TO_ROOT, .offset = {group->x + group->width - marksWidth - OPENECS_MENU_ITEM_PADDING, group->y}, .zIndex = 1},
            })
            {
                ECSIWindow_DeclareMarks(group, root);
            }
        }
    }

    ECSPanel panel = group->panels[group->shown];

    if (panel->type == nullptr || panel->fault != nullptr)
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
            Clay_String heading = panel->fault != nullptr ? CLAY_STRING("Panel failed") : CLAY_STRING("Missing panel type");
            CLAY_TEXT(heading, CLAY_TEXT_CONFIG({.textColor = WINDOW.colors[ECSIColor_TextDim], .fontSize = (u16)WINDOW.fontSize}));
            CLAY_TEXT(ECSIWindow_ClayText(panel->typeName), CLAY_TEXT_CONFIG({.textColor = WINDOW.colors[ECSIColor_Text], .fontSize = (u16)WINDOW.fontSize}));

            if (panel->fault != nullptr)
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
/// @brief Declares the message dialog: a shade over the whole OS window, and in its middle a box with the title, the message and a row of buttons.
static void ECSIWindow_DeclareDialog(const ECSIOSWindow *os)
{
    const ECSIDialogView *dialog = WINDOW.dialog;
    f32 width = SDL_max(1.0f, SDL_min(OPENECS_DIALOG_WIDTH, os->width - 2.0f * OPENECS_DIALOG_MARGIN));
    Clay_Color shade = WINDOW.colors[ECSIColor_Background];
    shade.a = 176.0f;

    CLAY(CLAY_ID("DialogShade"), {
                                     .layout = {.sizing = {CLAY_SIZING_FIXED(os->width), CLAY_SIZING_FIXED(os->height)}, .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                     .backgroundColor = shade,
                                     .floating = {.attachTo = CLAY_ATTACH_TO_ROOT, .zIndex = 20},
                                 })
    {
        CLAY(CLAY_ID("Dialog"), {
                                    .layout = {
                                        .sizing = {CLAY_SIZING_FIXED(width), CLAY_SIZING_FIT(0)},
                                        .padding = CLAY_PADDING_ALL(16),
                                        .childGap = 12,
                                        .layoutDirection = CLAY_TOP_TO_BOTTOM,
                                    },
                                    .backgroundColor = WINDOW.colors[ECSIColor_Overlay],
                                    .cornerRadius = CLAY_CORNER_RADIUS(6),
                                    .border = {.color = WINDOW.colors[ECSIColor_Accent], .width = {1, 1, 1, 1, 0}},
                                })
        {
            CLAY_TEXT(ECSIWindow_ClayText(dialog->title), CLAY_TEXT_CONFIG({.textColor = WINDOW.colors[ECSIColor_Accent], .fontSize = (u16)WINDOW.fontSize}));
            CLAY_TEXT(ECSIWindow_ClayText(dialog->message), CLAY_TEXT_CONFIG({.textColor = WINDOW.colors[ECSIColor_Text], .fontSize = (u16)WINDOW.fontSize, .wrapMode = CLAY_TEXT_WRAP_WORDS}));

            // the buttons line up at the right; the one that Return presses has the accent's border, and the one under the pointer is lighter
            CLAY_AUTO_ID({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)}, .childGap = 8, .childAlignment = {.x = CLAY_ALIGN_X_RIGHT}}})
            {
                for (usz i = 0; i < dialog->count; i++)
                {
                    bool selected = i == dialog->selected;

                    CLAY(CLAY_IDI("DialogButton", (u32)i), {
                                                               .layout = {.padding = {12, 12, 6, 6}},
                                                               .backgroundColor = (i32)i == dialog->hover ? WINDOW.colors[ECSIColor_Selected] : WINDOW.colors[ECSIColor_Tab],
                                                               .cornerRadius = CLAY_CORNER_RADIUS(4),
                                                               .border = {.color = WINDOW.colors[selected ? ECSIColor_Accent : ECSIColor_TextDim], .width = selected ? (Clay_BorderWidth){2, 2, 2, 2, 0} : (Clay_BorderWidth){1, 1, 1, 1, 0}},
                                                           })
                    {
                        CLAY_TEXT(ECSIWindow_ClayText(dialog->buttons[i]), CLAY_TEXT_CONFIG({.textColor = WINDOW.colors[ECSIColor_Text], .fontSize = (u16)WINDOW.fontSize, .wrapMode = CLAY_TEXT_WRAP_NONE}));
                    }
                }
            }
        }
    }
}

static Clay_RenderCommandArray ECSIWindow_DeclareInterface(ECSIOSWindow *os, const ECSIRoot *root)
{
    Clay_SetCurrentContext(os->clay);
    Clay_SetLayoutDimensions((Clay_Dimensions){os->width, os->height});
    Clay_BeginLayout();

    // the focus border, the list of prefix keys and the menus show in one OS window each
    ECSPanel focus = ECSILayout_GetFocus();
    ECSIRoot *focusRoot = ECSILayout_RootOf(focus);
    bool focusWindow = focusRoot == root || (focusRoot == nullptr && os->rootID == 0);

    arrfree(os->tabs);
    CLAY(CLAY_ID("Root"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}}})
    {
        if (WINDOW.titleBar)
        {
            ECSIWindow_DeclareTitleBar(os);
        }

        ECSILayout_ForEachGroup(root, ECSIWindow_DeclareGroup, os);

        if (focus != nullptr && focusWindow)
        {
            CLAY_AUTO_ID({
                .layout = {.sizing = {CLAY_SIZING_FIXED(focus->width), CLAY_SIZING_FIXED(focus->height)}},
                .border = {.color = WINDOW.colors[ECSIColor_Accent], .width = {2, 2, 2, 2, 0}},
                .floating = {.attachTo = CLAY_ATTACH_TO_ROOT, .offset = {focus->x, focus->y}, .zIndex = 1, .pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH},
            })
            {
            }
        }

        // the grip shows the panel's title, centred on its top edge, and whether its group is locked or maximized
        if (WINDOW.gripGroup != nullptr && WINDOW.gripWindow == os)
        {
            ECSINode *group = WINDOW.gripGroup;
            const char *title = group->panels[0]->title;
            int titleWidth = 0;
            TTF_SetFontSize(WINDOW.fonts[0], WINDOW.fontSize);
            TTF_GetStringSize(WINDOW.fonts[0], title, 0, &titleWidth, nullptr);
            f32 width = SDL_min((f32)titleWidth + ECSIWindow_MarksWidth(group, root) + 2.0f * OPENECS_MENU_ITEM_PADDING, group->width);

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

                ECSIWindow_DeclareMarks(group, root);
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

        if (WINDOW.dataType != nullptr)
        {
            ECSILayout_ForEachGroup(root, ECSIWindow_DeclareDataTarget, os);
        }

        for (usz level = 0; WINDOW.menuWindow == os && level < OPENECS_MENU_DEPTH && WINDOW.menus[level].lines != nullptr; level++)
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

        if (WINDOW.dialog != nullptr && WINDOW.dialogWindow == os)
        {
            ECSIWindow_DeclareDialog(os);
        }

        if (WINDOW.prefixLines != nullptr && focusWindow)
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
                    if (WINDOW.prefixLines[2 * i] == nullptr)
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

static void ECSIWindow_ReleasePopup(ECSPopup popup);

/// @brief Closes an OS window and frees what it holds. The window pointers that named it name the main window instead.
static void ECSIWindow_CloseOS(ECSIOSWindow *os)
{
    ECSIOSWindow *main = arrlenu(WINDOW.windows) > 0 && WINDOW.windows[0] != os ? WINDOW.windows[0] : nullptr;
    ECSIOSWindow **pointers[] = {&WINDOW.event, &WINDOW.gripWindow, &WINDOW.dropWindow, &WINDOW.dataWindow, &WINDOW.menuWindow, &WINDOW.pendingFrom, &WINDOW.dialogWindow};
    WINDOW.gripGroup = WINDOW.gripWindow == os ? nullptr : WINDOW.gripGroup;

    for (usz i = 0; i < SDL_arraysize(pointers); i++)
    {
        *pointers[i] = *pointers[i] == os ? main : *pointers[i];
    }
    arrfree(os->tabs);

    // Clay keeps the context it used last; one in freed memory must never be current
    if (Clay_GetCurrentContext() == os->clay)
    {
        Clay_SetCurrentContext(main != nullptr ? main->clay : nullptr);
    }

    SDL_free(os->clayMemory);

    // the renderer frees the textures made with it, so the panels and popups shown in this window must not keep them
    if (os->renderer != nullptr)
    {
        ECSPanel *panels = ECSILayout_GetPanels();

        for (usz i = 0; i < arrlenu(panels); i++)
        {
            if (panels[i]->texture != nullptr && SDL_GetRendererFromTexture(panels[i]->texture) == os->renderer)
            {
                ECSIPanel_ReleaseTexture(panels[i]);
            }
        }

        arrfree(panels);
    }

    // SDL destroys a window's popup windows with it, so the popups shown in or over this window let go of their windows, renderers and textures first; a popup that stays open is shown again where its panel is
    usz count = 0;
    ECSPopup *popups = ECSIPopups_GetOpen(&count);

    for (usz i = 0; i < count; i++)
    {
        ECSIPopupView *view = popups[i]->window;

        if (view != nullptr && (view->rootID == os->rootID || (view->texture != nullptr && SDL_GetRendererFromTexture(view->texture) == os->renderer)))
        {
            ECSIWindow_ReleasePopup(popups[i]);
        }
    }

    if (os->clayRenderer.textEngine != nullptr)
    {
        TTF_DestroyRendererTextEngine(os->clayRenderer.textEngine);
    }

    if (os->renderer != nullptr)
    {
        SDL_DestroyRenderer(os->renderer);
    }

    if (os->window != nullptr)
    {
        SDL_DestroyWindow(os->window);
    }

    SDL_free(os);
}

/// @brief Creates a window's renderer: a GPU renderer on the shared GPU device, so all windows share one device; else SDL's default renderer.
/// The device stays without a window's GPU renderer, as when the GPU cannot present to the window: GPU panels draw on it, and that window shows copies (DESIGN 5.3).
static SDL_Renderer *ECSIWindow_CreateRenderer(SDL_Window *window, bool main)
{
    // the device comes from an offscreen renderer, which sets it up as SDL's GPU renderer needs it
    if (main && GPU.owner == nullptr)
    {
        GPU.owner = SDL_CreateGPURenderer(nullptr, nullptr);
        GPU.device = GPU.owner == nullptr ? nullptr : SDL_GetGPURendererDevice(GPU.owner);

        if (GPU.device == nullptr)
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "No GPU device (%s); GPU panels show placeholders.", SDL_GetError());
        }
    }

    SDL_Renderer *renderer = GPU.device != nullptr ? SDL_CreateGPURenderer(GPU.device, window) : nullptr;

    if (renderer == nullptr && GPU.device != nullptr)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "The GPU cannot present to a window (%s); it uses SDL's default renderer, and shows copies of GPU panels.", SDL_GetError());
    }

    return renderer != nullptr ? renderer : SDL_CreateRenderer(window, nullptr);
}

/// @brief Opens an OS window with its renderer and Clay context, and adds it to the list.
/// @param rootID The pop-out root it shows, or 0 for the main window.
/// @param flags SDL's window flags; a pop-out window opens hidden, so it can be placed first.
/// @return The window, or nullptr if it cannot be opened.
static ECSIOSWindow *ECSIWindow_OpenOS(u32 rootID, i32 width, i32 height, SDL_WindowFlags flags)
{
    ECSIOSWindow *os = SDL_calloc(1, sizeof(ECSIOSWindow));

    if (os == nullptr)
    {
        return nullptr;
    }

    os->rootID = rootID;
    os->raised = ++WINDOW.raises;
    os->window = SDL_CreateWindow(WINDOW.title, width, height, SDL_WINDOW_RESIZABLE | (WINDOW.titleBar ? SDL_WINDOW_BORDERLESS : 0) | flags);

    if (os->window == nullptr)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot open a window: %s", SDL_GetError());
        ECSIWindow_CloseOS(os);
        return nullptr;
    }

    ECSIWindow_ApplyTitleBar(os);

    os->renderer = ECSIWindow_CreateRenderer(os->window, rootID == 0);

    if (os->renderer == nullptr)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot create a renderer: %s", SDL_GetError());
        ECSIWindow_CloseOS(os);
        return nullptr;
    }

    os->clayRenderer = (Clay_SDL3RendererData){
        .renderer = os->renderer,
        .textEngine = TTF_CreateRendererTextEngine(os->renderer),
        .fonts = WINDOW.fonts,
    };

    u32 claySize = Clay_MinMemorySize();
    os->clayMemory = os->clayRenderer.textEngine == nullptr ? nullptr : SDL_malloc(claySize);

    if (os->clayMemory == nullptr)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot create a text engine: %s", SDL_GetError());
        ECSIWindow_CloseOS(os);
        return nullptr;
    }

    ECSIWindow_ReadSize(os);
    Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(claySize, os->clayMemory);
    os->clay = Clay_Initialize(arena, (Clay_Dimensions){os->width, os->height}, (Clay_ErrorHandler){ECSIWindow_ClayError, nullptr});
    Clay_SetMeasureTextFunction(ECSIWindow_MeasureText, nullptr);

    // only the main window waits for the display, so presenting several windows does not wait once for each
    if (rootID != 0)
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
    char *fontPath = nullptr;

    if (SDL_asprintf(&fontPath, "%s%s", ECSPath_IsAbsolute(font) ? "" : SDL_GetBasePath(), font) < 0)
    {
        return SHUResult_ErrAllocation;
    }

    WINDOW.fonts[0] = TTF_OpenFont(fontPath, WINDOW.fontSize);

    if (WINDOW.fonts[0] == nullptr)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot load the font '%s': %s", fontPath, SDL_GetError());
        SDL_free(fontPath);
        return SHUResult_ErrFile;
    }

    SDL_free(fontPath);
    WINDOW.title = SDL_strdup(title);

    if (WINDOW.title == nullptr)
    {
        return SHUResult_ErrAllocation;
    }

    i64 width = SDL_clamp(ECSValue_GetInteger(ECSSetting_Get("ecs.windowWidth"), 0), 1, SDL_MAX_SINT32);
    i64 height = SDL_clamp(ECSValue_GetInteger(ECSSetting_Get("ecs.windowHeight"), 0), 1, SDL_MAX_SINT32);
    WINDOW.event = ECSIWindow_OpenOS(0, (i32)width, (i32)height, 0);
    WINDOW.menuWindow = WINDOW.event;

    // a drag that the system carries continues a press, which it must have seen
    ECSISystemDrag_Prepare();
    return WINDOW.event == nullptr ? SHUResult_ErrInternal : SHUResult_Ok;
}

/// @brief Matches the OS windows to the current workspace's roots: a new pop-out root gets its OS window at the pointer, a gone one's window closes, and the windows of other workspaces hide. Each shown root gets its window's size.
static void ECSIWindow_Sync(void)
{
    for (usz i = 1; i < arrlenu(WINDOW.windows);)
    {
        ECSIOSWindow *os = WINDOW.windows[i];
        bool shown = false;

        if (ECSILayout_FindRoot(os->rootID, &shown) == nullptr)
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
        if (ECSIWindow_OfRoot(roots[i]) != nullptr)
        {
            continue;
        }

        ECSIOSWindow *os = ECSIWindow_OpenOS(roots[i]->id, (i32)SDL_max(1.0f, roots[i]->width), (i32)SDL_max(1.0f, roots[i]->height + ECSIWindow_TitleHeight()), SDL_WINDOW_HIDDEN);

        if (os != nullptr)
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

        if (root != nullptr)
        {
            f32 title = SDL_min(ECSIWindow_TitleHeight(), os->height);
            ECSILayout_SetRootArea(root, title, os->width, os->height - title);
        }
    }

    // the OS window of the focused panel comes forward when the focus moves into it
    ECSIRoot *focusRoot = ECSILayout_RootOf(ECSILayout_GetFocus());
    u32 focusID = focusRoot == nullptr ? 0 : focusRoot->id;
    ECSIOSWindow *focusWindow = ECSIWindow_OfRoot(focusRoot);

    if (focusID != WINDOW.focusRoot && focusWindow != nullptr && (SDL_GetWindowFlags(focusWindow->window) & SDL_WINDOW_INPUT_FOCUS) == 0)
    {
        SDL_RaiseWindow(focusWindow->window);
    }

    WINDOW.focusRoot = focusID;
}

#pragma region Popups

/// @brief Frees what a popup is shown with; the Popups module calls it before it frees the popup.
static void ECSIWindow_ReleasePopup(ECSPopup popup)
{
    ECSIPopupView *view = popup->window;

    if (view == nullptr)
    {
        return;
    }

    WINDOW.eventPopup = WINDOW.eventPopup == popup ? nullptr : WINDOW.eventPopup;

    if (view->texture != nullptr)
    {
        SDL_DestroyTexture(view->texture);
    }

    if (view->renderer != nullptr)
    {
        SDL_DestroyRenderer(view->renderer);
    }

    if (view->window != nullptr)
    {
        SDL_DestroyWindow(view->window);
    }

    SDL_free(view);
    popup->window = nullptr;
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

    if (view == nullptr)
    {
        return nullptr;
    }

    view->rootID = os->rootID;
    popup->window = view;

    if (WINDOW.noPopupWindows)
    {
        return view;
    }

    SDL_WindowFlags flags = popup->desc.type == ECSPopupType_Menu ? SDL_WINDOW_POPUP_MENU : SDL_WINDOW_TOOLTIP;
    view->window = SDL_CreatePopupWindow(os->window, (int)rect.x, (int)rect.y, (int)rect.w, (int)rect.h, flags | SDL_WINDOW_TRANSPARENT);
    view->renderer = view->window == nullptr ? nullptr : ECSIWindow_CreateRenderer(view->window, false);

    // a driver without popup windows, such as the offscreen driver, gets them drawn inside the OS windows from now on
    if (view->renderer == nullptr)
    {
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Popups are drawn inside the OS windows: %s", SDL_GetError());
        WINDOW.noPopupWindows = view->window == nullptr;

        if (view->window != nullptr)
        {
            SDL_DestroyWindow(view->window);
            view->window = nullptr;
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

        if (os == nullptr)
        {
            continue;
        }

        // a popup whose panel moved to another OS window is shown there anew
        if (view != nullptr && view->rootID != os->rootID)
        {
            ECSIWindow_ReleasePopup(popup);
            view = nullptr;
        }

        SDL_FRect rect = ECSIWindow_PlacePopup(popup, os, view == nullptr ? WINDOW.noPopupWindows : view->window == nullptr);
        view = view != nullptr ? view : ECSIWindow_MakePopupView(popup, os, rect);

        if (view == nullptr)
        {
            continue;
        }

        if (view->window != nullptr && (rect.x != view->rect.x || rect.y != view->rect.y || rect.w != view->rect.w || rect.h != view->rect.h))
        {
            SDL_SetWindowPosition(view->window, (int)rect.x, (int)rect.y);
            SDL_SetWindowSize(view->window, (int)rect.w, (int)rect.h);
        }

        view->rect = rect;
        SDL_Renderer *renderer = view->renderer != nullptr ? view->renderer : os->renderer;
        bool drawn = ECSIPopup_Draw(popup);
        SDL_Surface *pixels = popup->pixels;

        if (pixels == nullptr)
        {
            continue;
        }

        if (view->texture != nullptr && (view->texture->w != pixels->w || view->texture->h != pixels->h || SDL_GetRendererFromTexture(view->texture) != renderer))
        {
            SDL_DestroyTexture(view->texture);
            view->texture = nullptr;
        }

        if (view->texture == nullptr)
        {
            // a popup's pixels have premultiplied alpha, so what it leaves out shows what is below
            view->texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, pixels->w, pixels->h);
            SDL_SetTextureBlendMode(view->texture, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
            drawn = true;
        }

        if (drawn && view->texture != nullptr)
        {
            SDL_UpdateTexture(view->texture, nullptr, pixels->pixels, pixels->pitch);
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

        if (view != nullptr && view->window == nullptr && view->texture != nullptr && view->rootID == os->rootID && ECSIWindow_OfRoot(ECSILayout_RootOf(popups[i]->panel)) == os)
        {
            SDL_RenderTexture(os->renderer, view->texture, nullptr, &view->rect);
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

        if (view != nullptr && view->renderer != nullptr)
        {
            SDL_SetRenderDrawColor(view->renderer, 0, 0, 0, 0);
            SDL_RenderClear(view->renderer);
            SDL_RenderTexture(view->renderer, view->texture, nullptr, nullptr);
            SDL_RenderPresent(view->renderer);
        }
    }
}

#pragma endregion Popups

#pragma endregion OS Windows

/// @brief Reads a colour setting. Its value in effect is always a colour, because colour settings take nothing else.
static void ECSIWindow_ReadColor(const char *name, Clay_Color *retColor)
{
    u32 argb = 0;
    bool parsed = ECSISettings_ParseColor(ECSValue_GetString(ECSSetting_Get(name), ""), &argb);
    SDL_assert(parsed);
    (void)parsed;

    *retColor = (Clay_Color){(f32)((argb >> 16) & 0xFF), (f32)((argb >> 8) & 0xFF), (f32)(argb & 0xFF), (f32)(argb >> 24)};
}

/// @brief Reads a number setting of the window, in layout units; a negative value counts as 0.
static f32 ECSIWindow_ReadNumber(const char *name)
{
    return (f32)SDL_max(0.0, ECSValue_GetNumber(ECSSetting_Get(name), 0.0));
}

/// @brief Reads the window's settings, except ecs.vsync and the ones read when the window opens.
static void ECSIWindow_Read(void)
{
    for (usz i = 0; i < ECSIColor_Count; i++)
    {
        ECSIWindow_ReadColor(OPENECS_WINDOW_SETTINGS[i].name, &WINDOW.colors[i]);
    }

    WINDOW.fontSize = ECSIWindow_ReadNumber("ecs.fontSize");
    WINDOW.gripHeight = ECSIWindow_ReadNumber("ecs.gripHeight");
    WINDOW.gripZone = ECSIWindow_ReadNumber("ecs.gripZone");
    WINDOW.dragThreshold = ECSIWindow_ReadNumber("ecs.dragThreshold");
    WINDOW.dockEdge = ECSIWindow_ReadNumber("ecs.dockEdge");
    WINDOW.splitDepth = ECSIWindow_ReadNumber("ecs.splitDepth");
    WINDOW.tabScrollStep = ECSIWindow_ReadNumber("ecs.tabScrollStep");
    WINDOW.titleBarHeight = ECSIWindow_ReadNumber("ecs.titleBarHeight");
    bool titleBar = ECSValue_GetBool(ECSSetting_Get("ecs.titleBar"), true);

    if (titleBar != WINDOW.titleBar)
    {
        WINDOW.titleBar = titleBar;

        for (usz i = 0; i < arrlenu(WINDOW.windows); i++)
        {
            ECSIWindow_ApplyTitleBar(WINDOW.windows[i]);
        }
    }
}

/// @brief The Changed function of the window's settings.
static void ECSIWindow_ReadSettings(void *data)
{
    (void)data;
    ECSIWindow_Read();
    ECSILayout_RequestFrame();
}

/// @brief The Changed function of ecs.theme.
static void ECSIWindow_ApplyTheme(void *data)
{
    (void)data;
    ECSISettings_ApplyTheme();
}

/// @brief Reads ecs.vsync and sets the main renderer's vsync. Also the Changed function of ecs.vsync.
static void ECSIWindow_ReadVsync(void *data)
{
    (void)data;

    SDL_Renderer *renderer = WINDOW.windows[0]->renderer;
    i64 percent = SDL_clamp(ECSValue_GetInteger(ECSSetting_Get("ecs.vsync"), 0), 0, 100);
    WINDOW.framePercent = percent;
    ECSILayout_RequestFrame();

    // on Wayland, the compositor shows whole frames already, and a window it hides gets no refreshes, so waiting for one would stop the program; the core limits the frame rate
    // elsewhere, a whole fraction of the refresh rate, such as 50%, lets the display wait for every second refresh; other rates wait for every refresh and are limited by the core
    if (percent == 0 || ECSIWindow_OnWayland())
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
    if (root != nullptr)
    {
        ECSILayout_ForEachGroup(root, ECSIWindow_DrawGroup, &context);
    }

    Clay_RenderCommandArray commands = ECSIWindow_DeclareInterface(os, root);
    ECSIWindow_FitTabs(os);

    Clay_Color background = WINDOW.colors[ECSIColor_Background];
    SDL_SetRenderDrawColor(os->renderer, (u8)background.r, (u8)background.g, (u8)background.b, (u8)background.a);
    SDL_RenderClear(os->renderer);

    if (root != nullptr)
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
    f32 rate = mode != nullptr && mode->refresh_rate > 0.0f ? mode->refresh_rate : OPENECS_FALLBACK_FRAME_RATE;
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

void ECSIWindow_SetHint(const char *hint)
{
    SDL_free(WINDOW.hint);
    WINDOW.hint = hint == nullptr || hint[0] == '\0' ? nullptr : SDL_strdup(hint);
}

SHUResult ECSIWindow_Initialize(const char *title)
{
    SDL_assert(title != nullptr);

    for (usz i = 0; i < SDL_arraysize(OPENECS_WINDOW_SETTINGS); i++)
    {
        ECSSettingDesc desc = OPENECS_WINDOW_SETTINGS[i];
        desc.Changed = ECSIWindow_ReadSettings;
        SHU_ReturnResult(ECSISettings_DeclareCore(&desc));
    }

    ECSIWindow_Read();

    SHU_ReturnResult(ECSIWindow_Open(title), ECSIWindow_Terminate(););

    // the theme gives the colours their values; it is applied once SDL has started, which tells the system's theme
    static const char *const THEMES[] = {"system", "dark", "light", nullptr};
    const ECSSettingDesc theme = {
        .name = "ecs.theme",
        .type = ECSSettingType_Choice,
        .description = "Colours of the interface: the system's choice of dark or light, dark, or light",
        .choices = THEMES,
        .Changed = ECSIWindow_ApplyTheme,
    };

    SHU_ReturnResult(ECSISettings_DeclareCore(&theme), ECSIWindow_Terminate(););

    const ECSSettingDesc vsync = {
        .name = "ecs.vsync",
        .type = ECSSettingType_Integer,
        .description = "Frame rate in percent of the display's refresh rate: 100 waits for every refresh, 50 for every second one, 0 turns vsync off",
        .Changed = ECSIWindow_ReadVsync,
    };

    SHU_ReturnResult(ECSISettings_DeclareCore(&vsync), ECSIWindow_Terminate(););
    ECSIWindow_ReadVsync(nullptr);
    ECSILayout_SetForget(ECSIWindow_Forget);
    ECSIPopups_SetRelease(ECSIWindow_ReleasePopup);
    return SHUResult_Ok;
}

void ECSIWindow_Terminate(void)
{
    ECSISystemDrag_Terminate();
    WINDOW.systemDrag = false;
    ECSILayout_SetForget(nullptr);
    ECSIPopups_SetRelease(nullptr);

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

    if (WINDOW.fonts[0] != nullptr)
    {
        TTF_CloseFont(WINDOW.fonts[0]);
    }

    if (TTF_WasInit() > 0)
    {
        TTF_Quit();
    }

    SDL_free(WINDOW.title);
    SDL_free(WINDOW.hint);
    SDL_zero(WINDOW);
}

void *ECSGpu_GetDevice(void)
{
    return GPU.device;
}

void ECSIWindow_DestroyDevice(void)
{
    if (GPU.owner != nullptr)
    {
        SDL_DestroyRenderer(GPU.owner);
    }

    SDL_zero(GPU);
}

SDL_Window *ECSIWindow_GetMain(void)
{
    return arrlenu(WINDOW.windows) > 0 ? WINDOW.windows[0]->window : nullptr;
}

SDL_Window *ECSIWindow_Get(usz number)
{
    // the windows are synced first, so a root made since the last frame has its window
    ECSIWindow_Sync();
    usz count = 0;
    ECSIRoot *const *roots = ECSILayout_GetRoots(&count);
    ECSIOSWindow *os = number >= 1 && number <= count ? ECSIWindow_OfRoot(roots[number - 1]) : nullptr;
    return os == nullptr ? nullptr : os->window;
}

usz ECSIWindow_NumberOf(ECSPanel panel)
{
    usz count = 0;
    ECSIRoot *const *roots = ECSILayout_GetRoots(&count);
    ECSIRoot *root = ECSILayout_RootOf(panel);

    for (usz i = 0; root != nullptr && i < count; i++)
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
    return os != nullptr ? os->window : ECSIWindow_GetMain();
}

ECSIRoot *ECSIWindow_GetRoot(SDL_WindowID id, bool *retMain)
{
    ECSIOSWindow *os = ECSIWindow_Find(id);

    if (retMain != nullptr)
    {
        *retMain = os == nullptr || os->rootID == 0;
    }

    return ECSIWindow_Root(os);
}

void ECSIWindow_Focused(SDL_WindowID id)
{
    ECSIOSWindow *os = ECSIWindow_Find(id);

    if (os != nullptr)
    {
        os->raised = ++WINDOW.raises;
    }
}

void ECSIWindow_SetEventWindow(SDL_WindowID id)
{
    WINDOW.event = ECSIWindow_Find(id);
    WINDOW.eventPopup = nullptr;

    // an SDL popup window's events go to its popup, with positions in it
    usz count = 0;
    ECSPopup *popups = ECSIPopups_GetOpen(&count);

    for (usz i = 0; i < count; i++)
    {
        ECSIPopupView *view = popups[i]->window;

        if (view != nullptr && view->window != nullptr && SDL_GetWindowID(view->window) == id)
        {
            WINDOW.eventPopup = popups[i];
        }
    }
}

ECSPopup ECSIWindow_PopupAt(f32 x, f32 y, f32 *retX, f32 *retY)
{
    SDL_assert(retX != nullptr);
    SDL_assert(retY != nullptr);

    *retX = x;
    *retY = y;

    if (WINDOW.eventPopup != nullptr)
    {
        return WINDOW.eventPopup;
    }

    // the newest popup drawn inside the event window that holds the point
    usz count = 0;
    ECSPopup *popups = ECSIPopups_GetOpen(&count);

    for (usz i = count; i > 0; i--)
    {
        ECSIPopupView *view = popups[i - 1]->window;

        if (view != nullptr && view->window == nullptr && WINDOW.event != nullptr && view->rootID == WINDOW.event->rootID &&
            ECSIWindow_Contains(x, y, view->rect.x, view->rect.y, view->rect.w, view->rect.h))
        {
            *retX = x - view->rect.x;
            *retY = y - view->rect.y;
            return popups[i - 1];
        }
    }

    return nullptr;
}

void ECSIWindow_ToPopup(ECSPopup popup, f32 x, f32 y, f32 *retX, f32 *retY)
{
    SDL_assert(popup != nullptr);
    SDL_assert(retX != nullptr);
    SDL_assert(retY != nullptr);

    // an SDL popup window's own events are in its positions already
    ECSIPopupView *view = popup->window;
    bool inside = view != nullptr && view->window == nullptr && popup != WINDOW.eventPopup;
    *retX = inside ? x - view->rect.x : x;
    *retY = inside ? y - view->rect.y : y;
}

bool ECSIWindow_PopupRect(ECSPopup popup, SDL_FRect *retRect, usz *retWindow)
{
    SDL_assert(popup != nullptr);
    SDL_assert(retRect != nullptr);
    SDL_assert(retWindow != nullptr);

    ECSIPopupView *view = popup->window;

    if (view == nullptr)
    {
        return false;
    }

    *retRect = view->rect;
    *retWindow = ECSIWindow_NumberOf(popup->panel);
    return true;
}

SDL_Window *ECSIWindow_PopupWindowAt(SDL_Window *window, f32 x, f32 y, SDL_FRect *retRect)
{
    SDL_assert(retRect != nullptr);

    usz count = 0;
    ECSPopup *popups = ECSIPopups_GetOpen(&count);

    // the newest popup on top
    for (usz i = count; i > 0; i--)
    {
        ECSIPopupView *view = popups[i - 1]->window;

        if (view != nullptr && view->window != nullptr && SDL_GetWindowParent(view->window) == window &&
            (SDL_GetWindowFlags(view->window) & SDL_WINDOW_HIDDEN) == 0 &&
            ECSIWindow_Contains(x, y, view->rect.x, view->rect.y, view->rect.w, view->rect.h))
        {
            *retRect = view->rect;
            return view->window;
        }
    }

    return nullptr;
}

ECSPanel ECSIWindow_PanelAt(f32 x, f32 y, f32 *retX, f32 *retY)
{
    SDL_assert(retX != nullptr);
    SDL_assert(retY != nullptr);

    ECSIOSWindow *os = ECSIWindow_Under(x, y, retX, retY);
    return ECSILayout_PanelAt(ECSIWindow_Root(os), *retX, *retY);
}

/// @brief Gets how long the main loop may wait for the next frame, in milliseconds: 0 when one is due, -1 when none is wanted.
static i32 ECSIWindow_NextFrameWait(void)
{
    // a panel waiting to land wakes the loop when its wait ends
    if (WINDOW.pendingPanel != nullptr)
    {
        u64 now = SDL_GetTicksNS();
        return now >= WINDOW.pendingUntil ? 0 : (i32)((WINDOW.pendingUntil - now + SDL_NS_PER_MS - 1) / SDL_NS_PER_MS);
    }

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

i32 ECSIWindow_GetFrameWait(void)
{
    i32 wait = ECSIWindow_NextFrameWait();

    // while the system carries a drag, its news come apart from SDL's events, so the loop looks for them often
    return WINDOW.systemDrag && (wait < 0 || wait > OPENECS_SYSTEM_DRAG_WAIT) ? OPENECS_SYSTEM_DRAG_WAIT : wait;
}

void ECSIWindow_Render(u64 nowTicks)
{
    // a panel that no other OS window took pops out
    if (WINDOW.pendingPanel != nullptr && nowTicks >= WINDOW.pendingUntil)
    {
        ECSPanel panel = WINDOW.pendingPanel;
        WINDOW.pendingPanel = nullptr;
        ECSIDrop drop = {.zone = ECSIZone_PopOut};

        if (WINDOW.pendingPopOut && ECSILayout_HasPanel(panel))
        {
            ECSILayout_Drop(panel, WINDOW.pendingGroup, &drop);
        }
    }

    ECSIWindow_DrawFrames(nowTicks);
    ECSIWindow_Present();
}

/// @brief Draws a frame and reads what an OS window shows, before it is presented, because presenting may discard it. Present it after.
/// @return The picture, to destroy with SDL_DestroySurface, or nullptr if there is no such window or it cannot be read.
static SDL_Surface *ECSIWindow_ReadFrame(usz number, f32 *retScale)
{
    ECSIWindow_DrawFrames(SDL_GetTicksNS());
    usz count = 0;
    ECSIRoot *const *roots = ECSILayout_GetRoots(&count);
    ECSIOSWindow *os = number >= 1 && number <= count ? ECSIWindow_OfRoot(roots[number - 1]) : nullptr;

    if (retScale != nullptr)
    {
        *retScale = os == nullptr ? 1.0f : SDL_GetWindowPixelDensity(os->window);
    }

    return os == nullptr ? nullptr : SDL_RenderReadPixels(os->renderer, nullptr);
}

SHUResult ECSIWindow_Screenshot(const char *path, usz number)
{
    SDL_assert(path != nullptr);

    SDL_Surface *picture = ECSIWindow_ReadFrame(number, nullptr);
    bool saved = picture != nullptr && SDL_SavePNG(picture, path);

    if (!saved)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cannot save a screenshot of window %zu to '%s': %s", number, path, SDL_GetError());
    }

    SDL_DestroySurface(picture);
    ECSIWindow_Present();
    return saved ? SHUResult_Ok : SHUResult_ErrFile;
}

SHUResult ECSIWindow_ReadPixel(usz number, f32 x, f32 y, ECSColor *retColor)
{
    SDL_assert(retColor != nullptr);

    f32 scale = 1.0f;
    SDL_Surface *picture = ECSIWindow_ReadFrame(number, &scale);
    Uint8 red = 0;
    Uint8 green = 0;
    Uint8 blue = 0;
    Uint8 alpha = 0;
    bool read = picture != nullptr && SDL_ReadSurfacePixel(picture, (int)(x * scale), (int)(y * scale), &red, &green, &blue, &alpha);
    *retColor = ((ECSColor)alpha << 24) | ((ECSColor)red << 16) | ((ECSColor)green << 8) | blue;

    if (!read)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cannot read the pixel at %g, %g of window %zu: %s", (double)x, (double)y, number, SDL_GetError());
    }

    SDL_DestroySurface(picture);
    ECSIWindow_Present();
    return read ? SHUResult_Ok : SHUResult_ErrBadData;
}

bool ECSIWindow_PointerDown(f32 x, f32 y)
{
    ECSIOSWindow *os = WINDOW.event;

    // a press while the system carries a drag ends it where it is, as the release was missed
    if (WINDOW.systemDrag)
    {
        ECSISystemDrag_Cancel();
        WINDOW.systemDrag = false;
        ECSIWindow_CancelDrag();
    }

    // the title bar's buttons, from the right: close, maximize or restore, and minimize; the rest of it moves the window, which the system does
    if (WINDOW.titleBar && y < WINDOW.titleBarHeight)
    {
        f32 button = (os->width - x) / ECSIWindow_TitleButtonWidth();

        if (button < 1.0f)
        {
            SDL_Event close = {.window = {.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED, .windowID = SDL_GetWindowID(os->window), .timestamp = SDL_GetTicksNS()}};
            SDL_PushEvent(&close);
        }
        else if (button < 2.0f && (SDL_GetWindowFlags(os->window) & SDL_WINDOW_MAXIMIZED) != 0)
        {
            SDL_RestoreWindow(os->window);
        }
        else if (button < 2.0f)
        {
            SDL_MaximizeWindow(os->window);
        }
        else if (button < 3.0f)
        {
            SDL_MinimizeWindow(os->window);
        }

        return true;
    }

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

    if (group != nullptr)
    {
        ECSILayout_SetFocus(group->panels[group->shown]);

        if (!group->locked)
        {
            ECSIWindow_ArmDrag(group->panels[group->shown], true, false, x, y);
        }

        return true;
    }

    // a locked group's grip only opens the menu
    if (WINDOW.gripGroup != nullptr && WINDOW.gripWindow == os && ECSIWindow_OnElement(os, CLAY_ID("Grip"), x, y))
    {
        ECSIWindow_ArmDrag(WINDOW.gripGroup->panels[0], false, true, x, y);
        WINDOW.dragLocked = WINDOW.gripGroup->locked;
        return true;
    }

    return false;
}

/// @brief Ends a press on a tab, a grip or a divider when the button is released: a dragged panel lands, and a grip that was not dragged is clicked.
/// @return The panel whose grip was clicked, or nullptr.
static ECSPanel ECSIWindow_Release(void)
{
    ECSPanel clicked = WINDOW.dragFromGrip && !WINDOW.dragging ? WINDOW.dragPanel : nullptr;

    // released outside every OS window it knows the place of, the panel waits briefly for the pointer to enter another one;
    // so does a panel alone in its OS window, which has nowhere to pop out to
    if (WINDOW.dragging && WINDOW.dropOutside && ECSIWindow_OnWayland())
    {
        WINDOW.pendingPanel = WINDOW.dragPanel;
        WINDOW.pendingGroup = WINDOW.dragGroup;
        WINDOW.pendingPopOut = WINDOW.drop.zone == ECSIZone_PopOut;
        WINDOW.pendingFrom = WINDOW.event;
        WINDOW.pendingUntil = SDL_GetTicksNS() + OPENECS_PENDING_DROP_NANOSECONDS;
    }
    else if (WINDOW.dragging)
    {
        ECSILayout_Drop(WINDOW.dragPanel, WINDOW.dragGroup, &WINDOW.drop);
    }

    ECSIWindow_CancelDrag();
    WINDOW.dragSplit = nullptr;
    ECSIWindow_SetCursor(SDL_SYSTEM_CURSOR_DEFAULT);
    return clicked;
}

/// @brief Finds where the dragged panel lands with the pointer at a point of the event window, and marks it. A drop that changes nothing is not marked.
static void ECSIWindow_FindDragDrop(f32 x, f32 y)
{
    WINDOW.drop = ECSIWindow_FindDrop(x, y, &WINDOW.dropWindow, &WINDOW.dropRect);
    WINDOW.dropOutside = WINDOW.drop.zone == ECSIZone_PopOut;
    WINDOW.drop = ECSIWindow_DropChanges(&WINDOW.drop) ? WINDOW.drop : (ECSIDrop){0};
    ECSILayout_RequestFrame();
}

/// @brief The pointer moved during a drag that the system carries: over one of the program's OS windows, or outside them.
static void ECSIWindow_SystemDragMove(SDL_Window *window, f32 x, f32 y)
{
    if (!WINDOW.dragging || WINDOW.dragPanel == nullptr)
    {
        return;
    }

    ECSIOSWindow *os = window == nullptr ? nullptr : ECSIWindow_Find(SDL_GetWindowID(window));

    if (os != nullptr)
    {
        WINDOW.event = os;
        ECSIWindow_FindDragDrop(x, y);
        return;
    }

    // outside the program's windows, or over a popup, the panel pops out
    WINDOW.drop = (ECSIDrop){.zone = ECSIZone_PopOut};
    WINDOW.dropWindow = nullptr;
    WINDOW.dropRect = (SDL_FRect){0};
    WINDOW.dropOutside = true;
    WINDOW.drop = ECSIWindow_DropChanges(&WINDOW.drop) ? WINDOW.drop : (ECSIDrop){0};
    ECSILayout_RequestFrame();
}

/// @brief A drag that the system carries ended: the panel lands where the pointer was last.
static void ECSIWindow_SystemDragEnd(bool carried)
{
    WINDOW.systemDrag = false;

    // a drag that the compositor did not take ends as the release of any drag does
    if (!carried)
    {
        ECSIWindow_Release();
        return;
    }

    if (WINDOW.dragging && WINDOW.drop.zone != ECSIZone_None && ECSILayout_HasPanel(WINDOW.dragPanel))
    {
        ECSILayout_Drop(WINDOW.dragPanel, WINDOW.dragGroup, &WINDOW.drop);
    }

    ECSIWindow_CancelDrag();
    ECSIWindow_SetCursor(SDL_SYSTEM_CURSOR_DEFAULT);
}

bool ECSIWindow_PointerMove(f32 x, f32 y)
{
    // a panel released outside its OS window lands where the pointer enters another one
    if (WINDOW.pendingPanel != nullptr && WINDOW.event != WINDOW.pendingFrom)
    {
        ECSPanel panel = WINDOW.pendingPanel;
        WINDOW.pendingPanel = nullptr;
        SDL_FRect rect = {0};
        ECSIOSWindow *os = nullptr;
        ECSIDrop drop = ECSIWindow_FindDrop(x, y, &os, &rect);

        if (ECSILayout_HasPanel(panel))
        {
            ECSILayout_Drop(panel, WINDOW.pendingGroup, &drop);
        }

        return true;
    }

    // the pointer shows a divider can be dragged, and while a panel is dragged
    ECSINode *split = WINDOW.dragSplit;
    usz divider = 0;

    if (split == nullptr && WINDOW.dragPanel == nullptr)
    {
        ECSILayout_DividerAt(ECSIWindow_Root(WINDOW.event), x, y, &split, &divider);
    }

    ECSIWindow_SetCursor(split != nullptr ? (split->vertical ? SDL_SYSTEM_CURSOR_NS_RESIZE : SDL_SYSTEM_CURSOR_EW_RESIZE)
                         : WINDOW.dragging     ? SDL_SYSTEM_CURSOR_MOVE
                                               : SDL_SYSTEM_CURSOR_DEFAULT);

    if (WINDOW.dragSplit != nullptr)
    {
        ECSILayout_MoveDivider(WINDOW.dragSplit, WINDOW.dragDivider, x, y);
        return true;
    }

    if (WINDOW.dragPanel != nullptr)
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
            ECSIWindow_FindDragDrop(x, y);

            // on Wayland, the drag goes on as one the compositor carries once the pointer leaves the window, so the other windows see the pointer and mark the drop
            if (WINDOW.dropOutside && ECSIWindow_OnWayland() && !WINDOW.systemDrag)
            {
                WINDOW.systemDrag = ECSISystemDrag_Start(WINDOW.event->window, ECSIWindow_SystemDragMove, ECSIWindow_SystemDragEnd);
            }
        }

        if (WINDOW.dragging && !wasDragging)
        {
            ECSIWindow_SetCursor(SDL_SYSTEM_CURSOR_MOVE);
        }

        return true;
    }

    ECSIGripHit hit = {x, y, nullptr};
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
    // when the system takes the drag, SDL releases the button; the drag goes on until the system ends it
    return WINDOW.systemDrag ? nullptr : ECSIWindow_Release();
}

bool ECSIWindow_CancelDrag(void)
{
    bool dragging = WINDOW.dragging;
    WINDOW.dragPanel = nullptr;
    WINDOW.dragGroup = false;
    WINDOW.dragFromGrip = false;
    WINDOW.dragLocked = false;
    WINDOW.dragging = false;
    WINDOW.drop = (ECSIDrop){0};
    WINDOW.dropOutside = false;
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

    if (WINDOW.gripGroup != nullptr && WINDOW.gripWindow == os && ECSIWindow_OnElement(os, CLAY_ID("Grip"), x, y))
    {
        return WINDOW.gripGroup->panels[0];
    }

    return nullptr;
}

ECSPanel ECSIWindow_TabRowAt(f32 x, f32 y)
{
    ECSINode *group = ECSIWindow_TabRowGroupAt(WINDOW.event, x, y);
    return group == nullptr ? nullptr : group->panels[group->shown];
}

bool ECSIWindow_ScrollTabs(f32 x, f32 y, f32 steps)
{
    ECSINode *group = ECSIWindow_TabRowGroupAt(WINDOW.event, x, y);

    if (group == nullptr)
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
        TTF_GetStringSize(font, lines[2 * i + 1], 0, &labelWidth, nullptr);
        TTF_GetStringSize(font, lines[2 * i], 0, &keyWidth, nullptr);
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
    SDL_assert(retLevel != nullptr);
    SDL_assert(retIndex != nullptr);

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

        if (WINDOW.menus[level].lines != nullptr && ECSIWindow_Contains(x, y, rect->x, rect->y, rect->w, rect->h))
        {
            return true;
        }
    }

    return false;
}

void ECSIWindow_ShowDataDrag(const char *type, f32 x, f32 y)
{
    WINDOW.dataType = type;
    WINDOW.dataWindow = type == nullptr ? nullptr : ECSIWindow_Under(x, y, &WINDOW.dataX, &WINDOW.dataY);
    ECSILayout_RequestFrame();
}

void ECSIWindow_ShowPrefixKeys(const char *const *lines, usz count)
{
    WINDOW.prefixLines = count == 0 ? nullptr : lines;
    WINDOW.prefixLineCount = count;
    ECSILayout_RequestFrame();
}

SDL_WindowID ECSIWindow_ShowDialog(const ECSIDialogView *dialog)
{
    ECSILayout_RequestFrame();
    WINDOW.dialog = dialog;
    WINDOW.dialogWindow = nullptr;

    if (dialog == nullptr)
    {
        return 0;
    }

    // the dialog shows over the OS window of the event that asks, such as a pop-out window's close button, and comes forward
    ECSIOSWindow *os = WINDOW.event != nullptr && ECSIWindow_IsShown(WINDOW.event) ? WINDOW.event : arrlenu(WINDOW.windows) > 0 ? WINDOW.windows[0] : nullptr;

    if (os == nullptr)
    {
        WINDOW.dialog = nullptr;
        return 0;
    }

    WINDOW.dialogWindow = os;
    SDL_RaiseWindow(os->window);
    return SDL_GetWindowID(os->window);
}

void ECSIWindow_RenderDialog(void)
{
    ECSIOSWindow *os = WINDOW.dialogWindow;

    if (WINDOW.dialog == nullptr || os == nullptr)
    {
        return;
    }

    // the panels show the pixels of their last frame, and the core's interface is laid out again with the dialog over it
    ECSIWindow_ReadSize(os);
    ECSIRoot *root = ECSIWindow_Root(os);
    ECSIDrawContext context = {os->renderer, SDL_GetTicksNS()};
    Clay_RenderCommandArray commands = ECSIWindow_DeclareInterface(os, root);

    Clay_Color background = WINDOW.colors[ECSIColor_Background];
    SDL_SetRenderDrawColor(os->renderer, (u8)background.r, (u8)background.g, (u8)background.b, (u8)background.a);
    SDL_RenderClear(os->renderer);

    if (root != nullptr)
    {
        ECSILayout_ForEachGroup(root, ECSIWindow_ShowGroup, &context);
    }

    SDL_Clay_RenderClayCommands(&os->clayRenderer, &commands);
    ECSIWindow_ShowPopupsIn(os);
    SDL_RenderPresent(os->renderer);
}

i32 ECSIWindow_DialogButtonAt(f32 x, f32 y)
{
    for (usz i = 0; WINDOW.dialog != nullptr && WINDOW.dialogWindow != nullptr && i < WINDOW.dialog->count; i++)
    {
        if (ECSIWindow_OnElement(WINDOW.dialogWindow, CLAY_IDI("DialogButton", (u32)i), x, y))
        {
            return (i32)i;
        }
    }

    return -1;
}

bool ECSIWindow_DialogButtonRect(usz index, SDL_FRect *retRect)
{
    SDL_assert(retRect != nullptr);

    if (WINDOW.dialog == nullptr || WINDOW.dialogWindow == nullptr || index >= WINDOW.dialog->count)
    {
        return false;
    }

    Clay_SetCurrentContext(WINDOW.dialogWindow->clay);
    Clay_ElementData element = Clay_GetElementData(CLAY_IDI("DialogButton", (u32)index));
    *retRect = (SDL_FRect){element.boundingBox.x, element.boundingBox.y, element.boundingBox.width, element.boundingBox.height};
    return element.found;
}
