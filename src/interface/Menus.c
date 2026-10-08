#include "interface/Menus.h"

#include "interface/Keys.h"
#include "interface/Layout.h"
#include "interface/Panels.h"
#include "interface/Window.h"
#include "runtime/Services.h"

#include "SDL3/SDL.h"
#include "stb/stbSDL3.h"

#pragma region Source Only

/// @brief What a menu entry that opens a submenu shows instead of keys.
#define OPENECS_SUBMENU_MARK "\u203A"

/// @brief What the entry of the shown tab shows in a group's menu.
#define OPENECS_SHOWN_MARK "\u2022"

/// @brief Most workspaces that have their own functions: keys 1 to 9 and 0.
#define OPENECS_WORKSPACE_FUNCTION_COUNT 10

/// @brief An entry of a menu: it runs a function, shows a tab or opens a submenu.
typedef struct ECSI_MenuEntry
{
    char *label;
    char *keys;                     // text of the keys that run it, or NULL
    char *function;                 // the function it runs on the focused panel, or NULL
    ECSPanel tab;                   // the panel whose tab it shows, or NULL
    struct ECSI_MenuEntry *submenu; // stb_ds array of the entries of the submenu it opens, or NULL
} ECSI_MenuEntry;

/// @brief An open menu: the panel menu or a submenu.
typedef struct ECSI_MenuLevel
{
    ECSI_MenuEntry *entries; // owned by the panel menu's entries
    const char **lines;      // stb_ds array of each entry's key text and label
    usz selected;
} ECSI_MenuLevel;

static struct
{
    ECSI_MenuEntry *entries; // stb_ds array of the panel menu's entries
    ECSI_MenuLevel *levels;  // stb_ds array of the open menus, the panel menu first; while any is open, it gets the pointer and the keys
} MENUS = {0};

/// @brief Moves the keyboard focus to a panel; the layout tells both panels.
static void ECSI_MenusFocus(ECSPanel panel)
{
    if (panel != NULL)
    {
        ECSI_LayoutSetFocus(panel);
    }
}

static void ECSI_MenusFreeEntries(ECSI_MenuEntry **entries)
{
    for (usz i = 0; i < arrlenu(*entries); i++)
    {
        SDL_free((*entries)[i].label);
        SDL_free((*entries)[i].keys);
        SDL_free((*entries)[i].function);
        ECSI_MenusFreeEntries(&(*entries)[i].submenu);
    }

    arrfree(*entries);
}

/// @brief Closes the menus from a level on. Level 0 closes the panel menu and frees its entries.
static void ECSI_MenusCloseFrom(usz level)
{
    for (usz i = level; i < arrlenu(MENUS.levels); i++)
    {
        arrfree(MENUS.levels[i].lines);
    }

    if (level < arrlenu(MENUS.levels))
    {
        arrsetlen(MENUS.levels, level);
        ECSI_WindowShowMenu(level, (SDL_FRect){0}, NULL, 0);
    }

    if (level == 0)
    {
        arrfree(MENUS.levels);
        ECSI_MenusFreeEntries(&MENUS.entries);
    }
}

/// @brief Adds an entry to a menu. The texts are copied.
/// @param keys Text of the keys that run it, made for the menu and taken over, or NULL.
/// @param function The function it runs, or NULL.
/// @return The entry, valid until the next entry is added.
static ECSI_MenuEntry *ECSI_MenusAdd(ECSI_MenuEntry **entries, const char *label, char *keys, const char *function)
{
    ECSI_MenuEntry entry = {.label = SDL_strdup(label), .keys = keys, .function = function == NULL ? NULL : SDL_strdup(function)};
    arrput(*entries, entry);
    return &arrlast(*entries);
}

/// @brief Adds an entry that runs a function, with the keys that run it after the prefix.
/// @param label Its label, or NULL for the function's label.
static void ECSI_MenusAddFunction(ECSI_MenuEntry **entries, const char *function, const char *label, ECSPanel panel)
{
    if (ECSI_MenusOffers(function, panel))
    {
        ECSI_MenusAdd(entries, label != NULL ? label : ECSI_MenusLabelOf(function, panel), ECSI_KeysPrefixTextOf(function), function);
    }
}

/// @brief Adds an entry that opens a submenu, unless the submenu is empty. The submenu is taken over.
static void ECSI_MenusAddSubmenu(ECSI_MenuEntry **entries, const char *label, ECSI_MenuEntry *submenu)
{
    if (arrlenu(submenu) > 0)
    {
        ECSI_MenusAdd(entries, label, NULL, NULL)->submenu = submenu;
    }
}

/// @brief Shows a menu at the next level, with its first entry highlighted.
/// @param anchor The point the panel menu opens at, or the entry a submenu opens beside.
static void ECSI_MenusOpenLevel(ECSI_MenuEntry *entries, SDL_FRect anchor)
{
    ECSI_MenuLevel level = {.entries = entries};

    // an entry that opens a submenu shows an arrow instead of keys
    for (usz i = 0; i < arrlenu(entries); i++)
    {
        arrput(level.lines, entries[i].submenu != NULL ? OPENECS_SUBMENU_MARK : entries[i].keys != NULL ? entries[i].keys
                                                                                                        : "");
        arrput(level.lines, entries[i].label != NULL ? entries[i].label : "");
    }

    arrput(MENUS.levels, level);
    ECSI_WindowShowMenu(arrlenu(MENUS.levels) - 1, anchor, level.lines, arrlenu(entries));
}

/// @brief Adds the entries of a panel's type, with the keys its plugin bound to the same functions for the type.
static void ECSI_MenusAddTypeEntries(ECSI_MenuEntry **entries, ECSPanel panel)
{
    usz count = 0;
    const char *const *functions = ECSI_PanelGetMenuEntries(panel, &count);

    for (usz i = 0; i < count; i++)
    {
        ECSI_MenusAdd(entries, ECSI_MenusLabelOf(functions[i], panel), ECSI_KeysBoundTextOf(panel->typeName, functions[i]), functions[i]);
    }
}

/// @brief Builds a panel's menu: what acts on the panel and its group, the submenus that split and move it, then its type's entries.
static void ECSI_MenusBuildPanelMenu(ECSI_MenuEntry **entries, ECSPanel panel)
{
    ECSI_MenuEntry *split = NULL;
    ECSI_MenuEntry *move = NULL;
    ECSI_MenuEntry *workspaces = NULL;

    ECSI_MenusAddFunction(entries, "ecs.close", NULL, panel);
    ECSI_MenusAddFunction(entries, "ecs.restart", NULL, panel);
    ECSI_MenusAddFunction(entries, "ecs.maximize", NULL, panel);
    ECSI_MenusAddFunction(entries, "ecs.lock", NULL, panel);
    ECSI_MenusAddFunction(entries, "ecs.reopen", NULL, panel);

    ECSI_MenusAddFunction(&split, "ecs.split_right", "Right", panel);
    ECSI_MenusAddFunction(&split, "ecs.split_down", "Down", panel);
    ECSI_MenusAddSubmenu(entries, "Split", split);

    ECSI_MenusAddFunction(&move, "ecs.move_left", "Left", panel);
    ECSI_MenusAddFunction(&move, "ecs.move_right", "Right", panel);
    ECSI_MenusAddFunction(&move, "ecs.move_up", "Up", panel);
    ECSI_MenusAddFunction(&move, "ecs.move_down", "Down", panel);
    ECSI_MenusAddSubmenu(entries, "Move", move);

    // one entry for each other workspace, by number and name
    for (usz number = 1; number <= ECSWorkspace_GetCount() && number <= OPENECS_WORKSPACE_FUNCTION_COUNT; number++)
    {
        char function[32];
        char label[256];

        if (number != ECSWorkspace_GetCurrent())
        {
            SDL_snprintf(function, sizeof(function), "ecs.move_to_workspace_%zu", number);
            SDL_snprintf(label, sizeof(label), "%zu: %s", number, ECSWorkspace_GetName(number));
            ECSI_MenusAddFunction(&workspaces, function, label, panel);
        }
    }

    ECSI_MenusAddSubmenu(entries, "Move to workspace", workspaces);
    ECSI_MenusAddTypeEntries(entries, panel);
}

/// @brief Builds a group's menu: its tabs, then what acts on the whole group.
static void ECSI_MenusBuildGroupMenu(ECSI_MenuEntry **entries, ECSPanel shownPanel)
{
    ECSI_MenuEntry *tabs = NULL;
    usz count = 0;
    usz shown = 0;
    const ECSPanel *panels = ECSI_LayoutGetGroup(shownPanel, &count, &shown);

    for (usz i = 0; i < count; i++)
    {
        ECSI_MenusAdd(&tabs, panels[i]->title, i == shown ? SDL_strdup(OPENECS_SHOWN_MARK) : NULL, NULL)->tab = panels[i];
    }

    ECSI_MenusAddSubmenu(entries, "Tabs", tabs);
    ECSI_MenusAddFunction(entries, "ecs.maximize", NULL, shownPanel);
    ECSI_MenusAddFunction(entries, "ecs.lock", NULL, shownPanel);
    ECSI_MenusAddFunction(entries, "ecs.close_group", NULL, shownPanel);
    ECSI_MenusAddFunction(entries, "ecs.reopen", NULL, shownPanel);
}

/// @brief Highlights an entry, and closes the submenus of the other entries.
/// @param open Also opens the entry's submenu, if it has one.
static void ECSI_MenusSelect(usz level, usz index, bool open)
{
    ECSI_MenusCloseFrom(level + 1);
    MENUS.levels[level].selected = index;
    ECSI_WindowSelectMenuItem(level, index);

    const ECSI_MenuEntry *entry = &MENUS.levels[level].entries[index];

    if (open && entry->submenu != NULL)
    {
        ECSI_MenusOpenLevel(entry->submenu, ECSI_WindowMenuItemRect(level, index));
    }
}

/// @brief Uses an entry: opens its submenu, shows its tab, or closes the menus and runs its function on the focused panel.
static void ECSI_MenusRun(usz level, usz index)
{
    const ECSI_MenuEntry *entry = &MENUS.levels[level].entries[index];

    if (entry->submenu != NULL)
    {
        ECSI_MenusSelect(level, index, true);
        return;
    }

    // closing the menus frees the entry, so what it does is copied first
    ECSPanel tab = entry->tab;
    char *function = entry->function == NULL ? NULL : SDL_strdup(entry->function);
    ECSI_MenusCloseFrom(0);

    if (tab != NULL)
    {
        ECSI_LayoutShowTab(tab);
    }

    if (function != NULL)
    {
        ECSI_ServicesCallBound(function, ECSI_LayoutGetFocus());
        SDL_free(function);
    }
}

#pragma region Core Functions

static void ECSI_MenusFocusLeft(void)
{
    ECSI_MenusFocus(ECSI_LayoutFindNeighbour(-1, 0));
}

static void ECSI_MenusFocusRight(void)
{
    ECSI_MenusFocus(ECSI_LayoutFindNeighbour(1, 0));
}

static void ECSI_MenusFocusUp(void)
{
    ECSI_MenusFocus(ECSI_LayoutFindNeighbour(0, -1));
}

static void ECSI_MenusFocusDown(void)
{
    ECSI_MenusFocus(ECSI_LayoutFindNeighbour(0, 1));
}

static void ECSI_MenusMoveLeft(void)
{
    ECSI_LayoutMoveFocus(-1, 0);
}

static void ECSI_MenusMoveRight(void)
{
    ECSI_LayoutMoveFocus(1, 0);
}

static void ECSI_MenusMoveUp(void)
{
    ECSI_LayoutMoveFocus(0, -1);
}

static void ECSI_MenusMoveDown(void)
{
    ECSI_LayoutMoveFocus(0, 1);
}

static void ECSI_MenusNextTab(void)
{
    ECSI_MenusFocus(ECSI_LayoutNextTab());
}

static void ECSI_MenusMaximize(void)
{
    ECSI_LayoutToggleMaximize();
}

static void ECSI_MenusClose(void)
{
    ECSPanel focus = ECSI_LayoutGetFocus();

    if (focus != NULL && ECSI_LayoutIsLocked(focus))
    {
        SDL_Log("'%s' is locked; unlock it to close it.", focus->title);
        return;
    }

    if (focus == NULL || !ECSI_PanelsConfirmClose(&focus, 1, false))
    {
        return;
    }

    ECSI_LayoutClosePanel(focus);
}

static void ECSI_MenusCloseGroup(void)
{
    ECSPanel focus = ECSI_LayoutGetFocus();
    usz count = 0;
    usz shown = 0;
    const ECSPanel *group = ECSI_LayoutGetGroup(focus, &count, &shown);

    if (count > 0 && ECSI_LayoutIsLocked(focus))
    {
        SDL_Log("The group of '%s' is locked; unlock it to close it.", focus->title);
        return;
    }

    // the group changes while its panels close, so they are copied first
    ECSPanel *panels = NULL;

    for (usz i = 0; i < count; i++)
    {
        arrput(panels, group[i]);
    }

    if (count > 0 && ECSI_PanelsConfirmClose(panels, count, false))
    {
        for (usz i = 0; i < count; i++)
        {
            if (ECSI_LayoutHasPanel(panels[i]))
            {
                ECSI_LayoutClosePanel(panels[i]);
            }
        }
    }

    arrfree(panels);
}

static void ECSI_MenusLock(void)
{
    ECSI_LayoutToggleLock();
}

static void ECSI_MenusReopen(void)
{
    ECSI_LayoutReopen();
}

static void ECSI_MenusRestart(void)
{
    ECSPanel focus = ECSI_LayoutGetFocus();

    if (focus != NULL && ECSI_PanelRestart(focus))
    {
        ECSI_LayoutRequestFrame();
    }
}

/// @brief Opens another panel of the focused panel's type beside it, for the type's plugin.
static void ECSI_MenusSplit(ECSZone zone)
{
    ECSPanel focus = ECSI_LayoutGetFocus();
    ECSPanel opened = NULL;

    if (focus != NULL && focus->type != NULL && ECSLayout_Open(focus->type->plugin, &opened, focus->typeName, NULL, focus, zone))
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cannot open another '%s'.", focus->typeName);
    }
}

static void ECSI_MenusSplitRight(void)
{
    ECSI_MenusSplit(ECSZone_Right);
}

static void ECSI_MenusSplitDown(void)
{
    ECSI_MenusSplit(ECSZone_Bottom);
}

/// @brief Defines the core's two functions for a workspace number: switching to the workspace, and moving the focused panel to it.
#define ECSI_WorkspaceFunctions(number)                 \
    static void ECSI_MenusWorkspace##number(void)       \
    {                                                   \
        ECSI_LayoutWorkspaceSwitch(number - 1);         \
    }                                                   \
                                                        \
    static void ECSI_MenusMoveToWorkspace##number(void) \
    {                                                   \
        ECSI_LayoutMoveToWorkspace(number - 1);         \
    }

ECSI_WorkspaceFunctions(1)
    ECSI_WorkspaceFunctions(2)
        ECSI_WorkspaceFunctions(3)
            ECSI_WorkspaceFunctions(4)
                ECSI_WorkspaceFunctions(5)
                    ECSI_WorkspaceFunctions(6)
                        ECSI_WorkspaceFunctions(7)
                            ECSI_WorkspaceFunctions(8)
                                ECSI_WorkspaceFunctions(9)
                                    ECSI_WorkspaceFunctions(10)

/// @brief The entries of the core function table for a workspace number.
#define ECSI_WorkspaceEntries(number)                                                                               \
    {"ecs.workspace_" #number, ECSI_MenusWorkspace##number, "Switch to workspace " #number},                        \
    {                                                                                                               \
        "ecs.move_to_workspace_" #number, ECSI_MenusMoveToWorkspace##number, "Move the panel to workspace " #number \
    }

/// @brief The core's bindable functions: name, function and description.
static const struct
{
    const char *name;
    ECSFunction Function;
    const char *description;
} ECSI_CORE_FUNCTIONS[] = {
    {"ecs.focus_left", ECSI_MenusFocusLeft, "Focus the panel on the left"},
    {"ecs.focus_right", ECSI_MenusFocusRight, "Focus the panel on the right"},
    {"ecs.focus_up", ECSI_MenusFocusUp, "Focus the panel above"},
    {"ecs.focus_down", ECSI_MenusFocusDown, "Focus the panel below"},
    {"ecs.move_left", ECSI_MenusMoveLeft, "Move the panel to the left"},
    {"ecs.move_right", ECSI_MenusMoveRight, "Move the panel to the right"},
    {"ecs.move_up", ECSI_MenusMoveUp, "Move the panel up"},
    {"ecs.move_down", ECSI_MenusMoveDown, "Move the panel down"},
    {"ecs.next_tab", ECSI_MenusNextTab, "Show the next tab"},
    {"ecs.maximize", ECSI_MenusMaximize, "Maximize or restore the group"},
    {"ecs.close", ECSI_MenusClose, "Close the panel"},
    {"ecs.close_group", ECSI_MenusCloseGroup, "Close the group's panels"},
    {"ecs.lock", ECSI_MenusLock, "Lock or unlock the group"},
    {"ecs.reopen", ECSI_MenusReopen, "Reopen the last closed panel"},
    {"ecs.restart", ECSI_MenusRestart, "Restart the failed panel"},
    {"ecs.split_right", ECSI_MenusSplitRight, "Open another one on the right"},
    {"ecs.split_down", ECSI_MenusSplitDown, "Open another one below"},
    ECSI_WorkspaceEntries(1),
    ECSI_WorkspaceEntries(2),
    ECSI_WorkspaceEntries(3),
    ECSI_WorkspaceEntries(4),
    ECSI_WorkspaceEntries(5),
    ECSI_WorkspaceEntries(6),
    ECSI_WorkspaceEntries(7),
    ECSI_WorkspaceEntries(8),
    ECSI_WorkspaceEntries(9),
    ECSI_WorkspaceEntries(10),
};

#pragma endregion Core Functions

#pragma endregion Source Only

SHUResult ECSI_MenusInitialize(void)
{
    for (usz i = 0; i < SDL_arraysize(ECSI_CORE_FUNCTIONS); i++)
    {
        SHU_ReturnResult(ECSI_ServicesRegisterCore(ECSI_CORE_FUNCTIONS[i].name, ECSI_CORE_FUNCTIONS[i].Function, "void()", ECSI_CORE_FUNCTIONS[i].description));
    }

    return SHUResult_Ok;
}

void ECSI_MenusTerminate(void)
{
    ECSI_MenusCloseFrom(0);
    SDL_zero(MENUS);
}

bool ECSI_MenusOffers(const char *function, ECSPanel panel)
{
    if (SDL_strcmp(function, "ecs.restart") == 0)
    {
        return panel != NULL && ECSI_PanelCanRestart(panel);
    }

    if (SDL_strcmp(function, "ecs.reopen") == 0)
    {
        return ECSI_LayoutCanReopen();
    }

    if (SDL_strncmp(function, "ecs.split_", SDL_strlen("ecs.split_")) == 0)
    {
        return panel != NULL && panel->type != NULL;
    }

    // the functions that move or close panels; the user cannot do that to a locked group
    if (SDL_strcmp(function, "ecs.close") == 0 || SDL_strcmp(function, "ecs.close_group") == 0 || SDL_strncmp(function, "ecs.move_", SDL_strlen("ecs.move_")) == 0)
    {
        return panel != NULL && !ECSI_LayoutIsLocked(panel);
    }

    return true;
}

const char *ECSI_MenusLabelOf(const char *function, ECSPanel panel)
{
    if (SDL_strcmp(function, "ecs.maximize") == 0)
    {
        return ECSI_LayoutIsMaximized(panel) ? "Restore the group" : "Maximize the group";
    }

    if (SDL_strcmp(function, "ecs.lock") == 0)
    {
        return ECSI_LayoutIsLocked(panel) ? "Unlock the group" : "Lock the group";
    }

    const char *description = ECSI_ServicesGetDescription(function);
    return description != NULL ? description : function;
}

void ECSI_MenusOpen(ECSPanel panel, f32 x, f32 y, bool group)
{
    ECSI_MenusCloseFrom(0);
    ECSI_LayoutSetFocus(panel);

    if (group)
    {
        ECSI_MenusBuildGroupMenu(&MENUS.entries, panel);
    }
    else
    {
        ECSI_MenusBuildPanelMenu(&MENUS.entries, panel);
    }

    ECSI_MenusOpenLevel(MENUS.entries, (SDL_FRect){x, y, 0.0f, 0.0f});
}

bool ECSI_MenusHandle(const SDL_Event *event)
{
    if (arrlenu(MENUS.levels) == 0)
    {
        return false;
    }

    usz deepest = arrlenu(MENUS.levels) - 1;
    usz count = arrlenu(MENUS.levels[deepest].entries);
    usz selected = MENUS.levels[deepest].selected;
    usz level = 0;
    usz index = 0;

    switch (event->type)
    {
    case SDL_EVENT_MOUSE_MOTION:
        // pointing at an entry highlights it and opens its submenu
        if (ECSI_WindowMenuItemAt(event->motion.x, event->motion.y, &level, &index))
        {
            bool hasSubmenu = MENUS.levels[level].entries[index].submenu != NULL;

            if (MENUS.levels[level].selected != index || arrlenu(MENUS.levels) != level + (hasSubmenu ? 2 : 1))
            {
                ECSI_MenusSelect(level, index, true);
            }
        }

        return true;

    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        // a press outside the menus only closes them
        if (ECSI_WindowMenuItemAt(event->button.x, event->button.y, &level, &index))
        {
            if (event->button.button == SDL_BUTTON_LEFT)
            {
                ECSI_MenusRun(level, index);
            }
        }
        else if (!ECSI_WindowMenuContains(event->button.x, event->button.y))
        {
            ECSI_MenusCloseFrom(0);
        }

        return true;

    case SDL_EVENT_KEY_DOWN:
        switch (event->key.key)
        {
        case SDLK_UP:
            ECSI_MenusSelect(deepest, (selected + count - 1) % count, false);
            break;
        case SDLK_DOWN:
            ECSI_MenusSelect(deepest, (selected + 1) % count, false);
            break;
        case SDLK_RIGHT:
            if (MENUS.levels[deepest].entries[selected].submenu != NULL)
            {
                ECSI_MenusRun(deepest, selected);
            }
            break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_SPACE:
            ECSI_MenusRun(deepest, selected);
            break;
        case SDLK_LEFT:
            if (deepest > 0)
            {
                ECSI_MenusCloseFrom(deepest);
            }
            break;
        case SDLK_ESCAPE:
            ECSI_MenusCloseFrom(deepest);
            break;
        default:
            break;
        }

        return true;

    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_TEXT_INPUT:
        return true;

    case SDL_EVENT_WINDOW_FOCUS_LOST:
    case SDL_EVENT_WINDOW_RESIZED:
        ECSI_MenusCloseFrom(0);
        return false;

    default:
        return false;
    }
}
