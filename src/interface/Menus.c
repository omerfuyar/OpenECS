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
typedef struct ECSIMenuEntry
{
    char *label;
    char *keys;                     // text of the keys that run it, or NULL
    char *function;                 // the function it runs on the focused panel, or NULL
    ECSPanel tab;                   // the panel whose tab it shows, or NULL
    struct ECSIMenuEntry *submenu; // stb_ds array of the entries of the submenu it opens, or NULL
} ECSIMenuEntry;

/// @brief An open menu: the panel menu or a submenu.
typedef struct ECSIMenuLevel
{
    ECSIMenuEntry *entries; // owned by the panel menu's entries
    const char **lines;      // stb_ds array of each entry's key text and label
    usz selected;
} ECSIMenuLevel;

static struct
{
    ECSIMenuEntry *entries; // stb_ds array of the panel menu's entries
    ECSIMenuLevel *levels;  // stb_ds array of the open menus, the panel menu first; while any is open, it gets the pointer and the keys
} MENUS = {0};

/// @brief Moves the keyboard focus to a panel; the layout tells both panels.
static void ECSIMenus_Focus(ECSPanel panel)
{
    if (panel != NULL)
    {
        ECSILayout_SetFocus(panel);
    }
}

static void ECSIMenus_FreeEntries(ECSIMenuEntry **entries)
{
    for (usz i = 0; i < arrlenu(*entries); i++)
    {
        SDL_free((*entries)[i].label);
        SDL_free((*entries)[i].keys);
        SDL_free((*entries)[i].function);
        ECSIMenus_FreeEntries(&(*entries)[i].submenu);
    }

    arrfree(*entries);
}

/// @brief Closes the menus from a level on. Level 0 closes the panel menu and frees its entries.
static void ECSIMenus_CloseFrom(usz level)
{
    for (usz i = level; i < arrlenu(MENUS.levels); i++)
    {
        arrfree(MENUS.levels[i].lines);
    }

    if (level < arrlenu(MENUS.levels))
    {
        arrsetlen(MENUS.levels, level);
        ECSIWindow_ShowMenu(level, (SDL_FRect){0}, NULL, 0);
    }

    if (level == 0)
    {
        arrfree(MENUS.levels);
        ECSIMenus_FreeEntries(&MENUS.entries);
    }
}

/// @brief Adds an entry to a menu. The texts are copied.
/// @param keys Text of the keys that run it, made for the menu and taken over, or NULL.
/// @param function The function it runs, or NULL.
/// @return The entry, valid until the next entry is added.
static ECSIMenuEntry *ECSIMenus_Add(ECSIMenuEntry **entries, const char *label, char *keys, const char *function)
{
    ECSIMenuEntry entry = {.label = SDL_strdup(label), .keys = keys, .function = function == NULL ? NULL : SDL_strdup(function)};
    arrput(*entries, entry);
    return &arrlast(*entries);
}

/// @brief Adds an entry that runs a function, with the keys that run it after the prefix.
/// @param label Its label, or NULL for the function's label.
static void ECSIMenus_AddFunction(ECSIMenuEntry **entries, const char *function, const char *label, ECSPanel panel)
{
    if (ECSIMenus_Offers(function, panel))
    {
        ECSIMenus_Add(entries, label != NULL ? label : ECSIMenus_LabelOf(function, panel), ECSIKeys_PrefixTextOf(function), function);
    }
}

/// @brief Adds an entry that opens a submenu, unless the submenu is empty. The submenu is taken over.
static void ECSIMenus_AddSubmenu(ECSIMenuEntry **entries, const char *label, ECSIMenuEntry *submenu)
{
    if (arrlenu(submenu) > 0)
    {
        ECSIMenus_Add(entries, label, NULL, NULL)->submenu = submenu;
    }
}

/// @brief Shows a menu at the next level, with its first entry highlighted.
/// @param anchor The point the panel menu opens at, or the entry a submenu opens beside.
static void ECSIMenus_OpenLevel(ECSIMenuEntry *entries, SDL_FRect anchor)
{
    ECSIMenuLevel level = {.entries = entries};

    // an entry that opens a submenu shows an arrow instead of keys
    for (usz i = 0; i < arrlenu(entries); i++)
    {
        arrput(level.lines, entries[i].submenu != NULL ? OPENECS_SUBMENU_MARK : entries[i].keys != NULL ? entries[i].keys
                                                                                                        : "");
        arrput(level.lines, entries[i].label != NULL ? entries[i].label : "");
    }

    arrput(MENUS.levels, level);
    ECSIWindow_ShowMenu(arrlenu(MENUS.levels) - 1, anchor, level.lines, arrlenu(entries));
}

/// @brief Adds the entries of a panel's type, with the keys its plugin bound to the same functions for the type.
static void ECSIMenus_AddTypeEntries(ECSIMenuEntry **entries, ECSPanel panel)
{
    usz count = 0;
    const char *const *functions = ECSIPanel_GetMenuEntries(panel, &count);

    for (usz i = 0; i < count; i++)
    {
        ECSIMenus_Add(entries, ECSIMenus_LabelOf(functions[i], panel), ECSIKeys_BoundTextOf(panel->typeName, functions[i]), functions[i]);
    }
}

/// @brief Builds a panel's menu: what acts on the panel and its group, the submenus that split and move it, then its type's entries.
static void ECSIMenus_BuildPanelMenu(ECSIMenuEntry **entries, ECSPanel panel)
{
    ECSIMenuEntry *split = NULL;
    ECSIMenuEntry *move = NULL;
    ECSIMenuEntry *workspaces = NULL;

    ECSIMenus_AddFunction(entries, "ecs.close", NULL, panel);
    ECSIMenus_AddFunction(entries, "ecs.restart", NULL, panel);
    ECSIMenus_AddFunction(entries, "ecs.maximize", NULL, panel);
    ECSIMenus_AddFunction(entries, "ecs.lock", NULL, panel);
    ECSIMenus_AddFunction(entries, "ecs.reopen", NULL, panel);

    ECSIMenus_AddFunction(&split, "ecs.split_right", "Right", panel);
    ECSIMenus_AddFunction(&split, "ecs.split_down", "Down", panel);
    ECSIMenus_AddSubmenu(entries, "Split", split);

    ECSIMenus_AddFunction(&move, "ecs.move_left", "Left", panel);
    ECSIMenus_AddFunction(&move, "ecs.move_right", "Right", panel);
    ECSIMenus_AddFunction(&move, "ecs.move_up", "Up", panel);
    ECSIMenus_AddFunction(&move, "ecs.move_down", "Down", panel);
    ECSIMenus_AddSubmenu(entries, "Move", move);

    // one entry for each other workspace, by number and name
    for (usz number = 1; number <= ECSWorkspace_GetCount() && number <= OPENECS_WORKSPACE_FUNCTION_COUNT; number++)
    {
        char function[32];
        char label[256];

        if (number != ECSWorkspace_GetCurrent())
        {
            SDL_snprintf(function, sizeof(function), "ecs.move_to_workspace_%zu", number);
            SDL_snprintf(label, sizeof(label), "%zu: %s", number, ECSWorkspace_GetName(number));
            ECSIMenus_AddFunction(&workspaces, function, label, panel);
        }
    }

    ECSIMenus_AddSubmenu(entries, "Move to workspace", workspaces);
    ECSIMenus_AddTypeEntries(entries, panel);
}

/// @brief Builds a group's menu: its tabs, then what acts on the whole group.
static void ECSIMenus_BuildGroupMenu(ECSIMenuEntry **entries, ECSPanel shownPanel)
{
    ECSIMenuEntry *tabs = NULL;
    usz count = 0;
    usz shown = 0;
    const ECSPanel *panels = ECSILayout_GetGroup(shownPanel, &count, &shown);

    for (usz i = 0; i < count; i++)
    {
        ECSIMenus_Add(&tabs, panels[i]->title, i == shown ? SDL_strdup(OPENECS_SHOWN_MARK) : NULL, NULL)->tab = panels[i];
    }

    ECSIMenus_AddSubmenu(entries, "Tabs", tabs);
    ECSIMenus_AddFunction(entries, "ecs.maximize", NULL, shownPanel);
    ECSIMenus_AddFunction(entries, "ecs.lock", NULL, shownPanel);
    ECSIMenus_AddFunction(entries, "ecs.close_group", NULL, shownPanel);
    ECSIMenus_AddFunction(entries, "ecs.reopen", NULL, shownPanel);
}

/// @brief Highlights an entry, and closes the submenus of the other entries.
/// @param open Also opens the entry's submenu, if it has one.
static void ECSIMenus_Select(usz level, usz index, bool open)
{
    ECSIMenus_CloseFrom(level + 1);
    MENUS.levels[level].selected = index;
    ECSIWindow_SelectMenuItem(level, index);

    const ECSIMenuEntry *entry = &MENUS.levels[level].entries[index];

    if (open && entry->submenu != NULL)
    {
        ECSIMenus_OpenLevel(entry->submenu, ECSIWindow_MenuItemRect(level, index));
    }
}

/// @brief Uses an entry: opens its submenu, shows its tab, or closes the menus and runs its function on the focused panel.
static void ECSIMenus_Run(usz level, usz index)
{
    const ECSIMenuEntry *entry = &MENUS.levels[level].entries[index];

    if (entry->submenu != NULL)
    {
        ECSIMenus_Select(level, index, true);
        return;
    }

    // closing the menus frees the entry, so what it does is copied first
    ECSPanel tab = entry->tab;
    char *function = entry->function == NULL ? NULL : SDL_strdup(entry->function);
    ECSIMenus_CloseFrom(0);

    if (tab != NULL)
    {
        ECSILayout_ShowTab(tab);
    }

    if (function != NULL)
    {
        ECSIServices_CallBound(function, ECSILayout_GetFocus());
        SDL_free(function);
    }
}

#pragma region Core Functions

static void ECSIMenus_FocusLeft(void)
{
    ECSIMenus_Focus(ECSILayout_FindNeighbour(-1, 0));
}

static void ECSIMenus_FocusRight(void)
{
    ECSIMenus_Focus(ECSILayout_FindNeighbour(1, 0));
}

static void ECSIMenus_FocusUp(void)
{
    ECSIMenus_Focus(ECSILayout_FindNeighbour(0, -1));
}

static void ECSIMenus_FocusDown(void)
{
    ECSIMenus_Focus(ECSILayout_FindNeighbour(0, 1));
}

static void ECSIMenus_MoveLeft(void)
{
    ECSILayout_MoveFocus(-1, 0);
}

static void ECSIMenus_MoveRight(void)
{
    ECSILayout_MoveFocus(1, 0);
}

static void ECSIMenus_MoveUp(void)
{
    ECSILayout_MoveFocus(0, -1);
}

static void ECSIMenus_MoveDown(void)
{
    ECSILayout_MoveFocus(0, 1);
}

static void ECSIMenus_NextTab(void)
{
    ECSIMenus_Focus(ECSILayout_NextTab());
}

static void ECSIMenus_Maximize(void)
{
    ECSILayout_ToggleMaximize();
}

static void ECSIMenus_Close(void)
{
    ECSPanel focus = ECSILayout_GetFocus();

    if (focus != NULL && ECSILayout_IsLocked(focus))
    {
        SDL_Log("'%s' is locked; unlock it to close it.", focus->title);
        return;
    }

    if (focus == NULL || !ECSIPanels_ConfirmClose(&focus, 1, false))
    {
        return;
    }

    ECSILayout_ClosePanel(focus);
}

static void ECSIMenus_CloseGroup(void)
{
    ECSPanel focus = ECSILayout_GetFocus();
    usz count = 0;
    usz shown = 0;
    const ECSPanel *group = ECSILayout_GetGroup(focus, &count, &shown);

    if (count > 0 && ECSILayout_IsLocked(focus))
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

    if (count > 0 && ECSIPanels_ConfirmClose(panels, count, false))
    {
        for (usz i = 0; i < count; i++)
        {
            if (ECSILayout_HasPanel(panels[i]))
            {
                ECSILayout_ClosePanel(panels[i]);
            }
        }
    }

    arrfree(panels);
}

static void ECSIMenus_Lock(void)
{
    ECSILayout_ToggleLock();
}

static void ECSIMenus_Reopen(void)
{
    ECSILayout_Reopen();
}

static void ECSIMenus_Restart(void)
{
    ECSPanel focus = ECSILayout_GetFocus();

    if (focus != NULL && ECSIPanel_Restart(focus))
    {
        ECSILayout_RequestFrame();
    }
}

/// @brief Opens another panel of the focused panel's type beside it, for the type's plugin.
static void ECSIMenus_Split(ECSZone zone)
{
    ECSPanel focus = ECSILayout_GetFocus();
    ECSPanel opened = NULL;

    if (focus != NULL && focus->type != NULL && ECSLayout_Open(focus->type->plugin, &opened, focus->typeName, NULL, focus, zone))
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cannot open another '%s'.", focus->typeName);
    }
}

static void ECSIMenus_SplitRight(void)
{
    ECSIMenus_Split(ECSZone_Right);
}

static void ECSIMenus_SplitDown(void)
{
    ECSIMenus_Split(ECSZone_Bottom);
}

/// @brief Defines the core's two functions for a workspace number: switching to the workspace, and moving the focused panel to it.
#define ECSIMenus_WorkspaceFunctions(number)                 \
    static void ECSIMenus_Workspace##number(void)       \
    {                                                   \
        ECSILayout_WorkspaceSwitch(number - 1);         \
    }                                                   \
                                                        \
    static void ECSIMenus_MoveToWorkspace##number(void) \
    {                                                   \
        ECSILayout_MoveToWorkspace(number - 1);         \
    }

ECSIMenus_WorkspaceFunctions(1)
    ECSIMenus_WorkspaceFunctions(2)
        ECSIMenus_WorkspaceFunctions(3)
            ECSIMenus_WorkspaceFunctions(4)
                ECSIMenus_WorkspaceFunctions(5)
                    ECSIMenus_WorkspaceFunctions(6)
                        ECSIMenus_WorkspaceFunctions(7)
                            ECSIMenus_WorkspaceFunctions(8)
                                ECSIMenus_WorkspaceFunctions(9)
                                    ECSIMenus_WorkspaceFunctions(10)

/// @brief The entries of the core function table for a workspace number.
#define ECSIMenus_WorkspaceEntries(number)                                                                               \
    {"ecs.workspace_" #number, ECSIMenus_Workspace##number, "Switch to workspace " #number},                        \
    {                                                                                                               \
        "ecs.move_to_workspace_" #number, ECSIMenus_MoveToWorkspace##number, "Move the panel to workspace " #number \
    }

/// @brief The core's bindable functions: name, function and description.
static const struct
{
    const char *name;
    ECSFunction Function;
    const char *description;
} OPENECS_CORE_FUNCTIONS[] = {
    {"ecs.focus_left", ECSIMenus_FocusLeft, "Focus the panel on the left"},
    {"ecs.focus_right", ECSIMenus_FocusRight, "Focus the panel on the right"},
    {"ecs.focus_up", ECSIMenus_FocusUp, "Focus the panel above"},
    {"ecs.focus_down", ECSIMenus_FocusDown, "Focus the panel below"},
    {"ecs.move_left", ECSIMenus_MoveLeft, "Move the panel to the left"},
    {"ecs.move_right", ECSIMenus_MoveRight, "Move the panel to the right"},
    {"ecs.move_up", ECSIMenus_MoveUp, "Move the panel up"},
    {"ecs.move_down", ECSIMenus_MoveDown, "Move the panel down"},
    {"ecs.next_tab", ECSIMenus_NextTab, "Show the next tab"},
    {"ecs.maximize", ECSIMenus_Maximize, "Maximize or restore the group"},
    {"ecs.close", ECSIMenus_Close, "Close the panel"},
    {"ecs.close_group", ECSIMenus_CloseGroup, "Close the group's panels"},
    {"ecs.lock", ECSIMenus_Lock, "Lock or unlock the group"},
    {"ecs.reopen", ECSIMenus_Reopen, "Reopen the last closed panel"},
    {"ecs.restart", ECSIMenus_Restart, "Restart the failed panel"},
    {"ecs.split_right", ECSIMenus_SplitRight, "Open another one on the right"},
    {"ecs.split_down", ECSIMenus_SplitDown, "Open another one below"},
    ECSIMenus_WorkspaceEntries(1),
    ECSIMenus_WorkspaceEntries(2),
    ECSIMenus_WorkspaceEntries(3),
    ECSIMenus_WorkspaceEntries(4),
    ECSIMenus_WorkspaceEntries(5),
    ECSIMenus_WorkspaceEntries(6),
    ECSIMenus_WorkspaceEntries(7),
    ECSIMenus_WorkspaceEntries(8),
    ECSIMenus_WorkspaceEntries(9),
    ECSIMenus_WorkspaceEntries(10),
};

#pragma endregion Core Functions

#pragma endregion Source Only

SHUResult ECSIMenus_Initialize(void)
{
    for (usz i = 0; i < SDL_arraysize(OPENECS_CORE_FUNCTIONS); i++)
    {
        SHU_ReturnResult(ECSIServices_RegisterCore(OPENECS_CORE_FUNCTIONS[i].name, OPENECS_CORE_FUNCTIONS[i].Function, "void()", OPENECS_CORE_FUNCTIONS[i].description));
    }

    return SHUResult_Ok;
}

void ECSIMenus_Terminate(void)
{
    ECSIMenus_CloseFrom(0);
    SDL_zero(MENUS);
}

bool ECSIMenus_Offers(const char *function, ECSPanel panel)
{
    if (SDL_strcmp(function, "ecs.restart") == 0)
    {
        return panel != NULL && ECSIPanel_CanRestart(panel);
    }

    if (SDL_strcmp(function, "ecs.reopen") == 0)
    {
        return ECSILayout_CanReopen();
    }

    if (SDL_strncmp(function, "ecs.split_", SDL_strlen("ecs.split_")) == 0)
    {
        return panel != NULL && panel->type != NULL;
    }

    // the functions that move or close panels; the user cannot do that to a locked group
    if (SDL_strcmp(function, "ecs.close") == 0 || SDL_strcmp(function, "ecs.close_group") == 0 || SDL_strncmp(function, "ecs.move_", SDL_strlen("ecs.move_")) == 0)
    {
        return panel != NULL && !ECSILayout_IsLocked(panel);
    }

    return true;
}

const char *ECSIMenus_LabelOf(const char *function, ECSPanel panel)
{
    if (SDL_strcmp(function, "ecs.maximize") == 0)
    {
        return ECSILayout_IsMaximized(panel) ? "Restore the group" : "Maximize the group";
    }

    if (SDL_strcmp(function, "ecs.lock") == 0)
    {
        return ECSILayout_IsLocked(panel) ? "Unlock the group" : "Lock the group";
    }

    const char *description = ECSIServices_GetDescription(function);
    return description != NULL ? description : function;
}

usz ECSIMenus_GetOrder(const char *function)
{
    SDL_assert(function != NULL);

    usz position = 0;

    while (position < SDL_arraysize(OPENECS_CORE_FUNCTIONS) && SDL_strcmp(OPENECS_CORE_FUNCTIONS[position].name, function) != 0)
    {
        position++;
    }

    return position;
}

void ECSIMenus_Open(ECSPanel panel, f32 x, f32 y, bool group)
{
    ECSIMenus_CloseFrom(0);
    ECSILayout_SetFocus(panel);

    if (group)
    {
        ECSIMenus_BuildGroupMenu(&MENUS.entries, panel);
    }
    else
    {
        ECSIMenus_BuildPanelMenu(&MENUS.entries, panel);
    }

    ECSIMenus_OpenLevel(MENUS.entries, (SDL_FRect){x, y, 0.0f, 0.0f});
}

bool ECSIMenus_Handle(const SDL_Event *event)
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
        if (ECSIWindow_MenuItemAt(event->motion.x, event->motion.y, &level, &index))
        {
            bool hasSubmenu = MENUS.levels[level].entries[index].submenu != NULL;

            if (MENUS.levels[level].selected != index || arrlenu(MENUS.levels) != level + (hasSubmenu ? 2 : 1))
            {
                ECSIMenus_Select(level, index, true);
            }
        }

        return true;

    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        // a press outside the menus only closes them
        if (ECSIWindow_MenuItemAt(event->button.x, event->button.y, &level, &index))
        {
            if (event->button.button == SDL_BUTTON_LEFT)
            {
                ECSIMenus_Run(level, index);
            }
        }
        else if (!ECSIWindow_MenuContains(event->button.x, event->button.y))
        {
            ECSIMenus_CloseFrom(0);
        }

        return true;

    case SDL_EVENT_KEY_DOWN:
        switch (event->key.key)
        {
        case SDLK_UP:
            ECSIMenus_Select(deepest, (selected + count - 1) % count, false);
            break;
        case SDLK_DOWN:
            ECSIMenus_Select(deepest, (selected + 1) % count, false);
            break;
        case SDLK_RIGHT:
            if (MENUS.levels[deepest].entries[selected].submenu != NULL)
            {
                ECSIMenus_Run(deepest, selected);
            }
            break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_SPACE:
            ECSIMenus_Run(deepest, selected);
            break;
        case SDLK_LEFT:
            if (deepest > 0)
            {
                ECSIMenus_CloseFrom(deepest);
            }
            break;
        case SDLK_ESCAPE:
            ECSIMenus_CloseFrom(deepest);
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
        ECSIMenus_CloseFrom(0);
        return false;

    default:
        return false;
    }
}
