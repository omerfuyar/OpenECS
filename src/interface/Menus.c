#include "interface/Menus.h"

#include "interface/Keys.h"
#include "interface/Layout.h"
#include "interface/Panels.h"
#include "interface/Window.h"
#include "runtime/Services.h"
#include "runtime/Settings.h"

#include "SDL3/SDL.h"
#include "stb/stbSDL3.h"

#pragma region Source Only

/// @brief What a menu entry that opens a submenu shows instead of keys.
#define OPENECS_SUBMENU_MARK "\u203A"

/// @brief What the entry of the shown tab shows in a group's menu.
#define OPENECS_SHOWN_MARK "\u2022"

/// @brief How far from the focused panel's corner a menu that keys open is, in layout units.
#define OPENECS_MENU_OFFSET 24.0f

/// @brief The setting that names the functions of the tool's menu.
#define OPENECS_TOOL_MENU "ecs.toolMenu"

/// @brief Most workspaces that have their own functions: keys 1 to 9 and 0.
#define OPENECS_WORKSPACE_FUNCTION_COUNT 10

/// @brief An entry of a menu: it runs a function, shows a tab or opens a submenu.
typedef struct ECSIMenuEntry
{
    char *label;
    char *keys;                    // text of the keys that run it, or nullptr
    char *function;                // the function it runs on the focused panel, or nullptr
    ECSPanel tab;                  // the panel whose tab it shows, or nullptr
    struct ECSIMenuEntry *submenu; // stb_ds array of the entries of the submenu it opens, or nullptr
} ECSIMenuEntry;

/// @brief An open menu: the panel menu or a submenu. The window shows it and keeps its highlighted entry.
typedef struct ECSIMenuLevel
{
    ECSIMenuEntry *entries; // owned by the panel menu's entries
    const char **lines;     // stb_ds array of each entry's key text and label, which the window shows
} ECSIMenuLevel;

static struct
{
    ECSIMenuEntry *entries; // stb_ds array of the panel menu's entries
    ECSIMenuLevel *levels;  // stb_ds array of the open menus, the panel menu first; while any is open, it gets the pointer and the keys
} MENUS = {0};

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
        ECSIWindow_ShowMenu(level, (SDL_FRect){0}, nullptr, 0);
    }

    if (level == 0)
    {
        arrfree(MENUS.levels);
        ECSIMenus_FreeEntries(&MENUS.entries);
    }
}

/// @brief Adds an entry to a menu. The texts are copied.
/// @param keys Text of the keys that run it, made for the menu and taken over, or nullptr.
/// @param function The function it runs, or nullptr.
/// @return The entry, valid until the next entry is added.
static ECSIMenuEntry *ECSIMenus_Add(ECSIMenuEntry **entries, const char *label, char *keys, const char *function)
{
    ECSIMenuEntry entry = {.label = SDL_strdup(label), .keys = keys, .function = function == nullptr ? nullptr : SDL_strdup(function)};
    arrput(*entries, entry);
    return &arrlast(*entries);
}

/// @brief Adds an entry that runs a function, with the keys that run it after the prefix.
/// @param label Its label, or nullptr for the function's label.
static void ECSIMenus_AddFunction(ECSIMenuEntry **entries, const char *function, const char *label, ECSPanel panel)
{
    if (ECSIMenus_Offers(function, panel))
    {
        ECSIMenus_Add(entries, label != nullptr ? label : ECSIMenus_LabelOf(function, panel), ECSIKeys_PrefixTextOf(function), function);
    }
}

/// @brief Adds an entry that opens a submenu, unless the submenu is empty. The submenu is taken over.
static void ECSIMenus_AddSubmenu(ECSIMenuEntry **entries, const char *label, ECSIMenuEntry *submenu)
{
    if (arrlenu(submenu) > 0)
    {
        ECSIMenus_Add(entries, label, nullptr, nullptr)->submenu = submenu;
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
        arrput(level.lines, entries[i].submenu != nullptr ? OPENECS_SUBMENU_MARK : entries[i].keys != nullptr ? entries[i].keys
                                                                                                              : "");
        arrput(level.lines, entries[i].label != nullptr ? entries[i].label : "");
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

/// @brief Adds the entries that act on the whole tool: resetting the layout, and quitting.
static void ECSIMenus_AddToolEntries(ECSIMenuEntry **entries, ECSPanel panel)
{
    ECSIMenus_AddFunction(entries, "ecs.layout.reset", nullptr, panel);
    ECSIMenus_AddFunction(entries, "ecs.app.quit", nullptr, panel);
}

/// @brief Builds a panel's menu: what acts on the panel and its group, the submenus that move it, its type's entries, then what acts on the tool.
/// @return stb_ds array of the entries.
static ECSIMenuEntry *ECSIMenus_BuildPanelMenu(ECSPanel panel)
{
    ECSIMenuEntry *entries = nullptr;
    ECSIMenuEntry *move = nullptr;
    ECSIMenuEntry *workspaces = nullptr;

    ECSIMenus_AddFunction(&entries, "ecs.layout.close", nullptr, panel);
    ECSIMenus_AddFunction(&entries, "ecs.panel.restart", nullptr, panel);
    ECSIMenus_AddFunction(&entries, "ecs.layout.maximize", nullptr, panel);
    ECSIMenus_AddFunction(&entries, "ecs.layout.popOut", nullptr, panel);
    ECSIMenus_AddFunction(&entries, "ecs.layout.lock", nullptr, panel);
    ECSIMenus_AddFunction(&entries, "ecs.layout.reopen", nullptr, panel);

    ECSIMenus_AddFunction(&move, "ecs.layout.moveLeft", "Left", panel);
    ECSIMenus_AddFunction(&move, "ecs.layout.moveRight", "Right", panel);
    ECSIMenus_AddFunction(&move, "ecs.layout.moveUp", "Up", panel);
    ECSIMenus_AddFunction(&move, "ecs.layout.moveDown", "Down", panel);
    ECSIMenus_AddSubmenu(&entries, "Move", move);

    // one entry for each other workspace, by number and name
    for (usz number = 1; number <= ECSWorkspace_GetCount() && number <= OPENECS_WORKSPACE_FUNCTION_COUNT; number++)
    {
        char function[32];
        char label[256];

        if (number != ECSWorkspace_GetCurrent())
        {
            SDL_snprintf(function, sizeof(function), "ecs.layout.moveToWorkspace%zu", number);
            SDL_snprintf(label, sizeof(label), "%zu: %s", number, ECSWorkspace_GetName(number));
            ECSIMenus_AddFunction(&workspaces, function, label, panel);
        }
    }

    ECSIMenus_AddSubmenu(&entries, "To workspace", workspaces);
    ECSIMenus_AddTypeEntries(&entries, panel);
    ECSIMenus_AddToolEntries(&entries, panel);
    return entries;
}

/// @brief Builds the menu of workspaces: each one by number and name, with the current one marked; an entry switches to it.
/// @return stb_ds array of the entries.
static ECSIMenuEntry *ECSIMenus_BuildWorkspaceMenu(void)
{
    ECSIMenuEntry *entries = nullptr;

    for (usz number = 1; number <= ECSWorkspace_GetCount() && number <= OPENECS_WORKSPACE_FUNCTION_COUNT; number++)
    {
        char function[32];
        char label[256];
        SDL_snprintf(function, sizeof(function), "ecs.workspace.switch%zu", number);
        SDL_snprintf(label, sizeof(label), "%zu: %s", number, ECSWorkspace_GetName(number));
        ECSIMenus_Add(&entries, label, number == ECSWorkspace_GetCurrent() ? SDL_strdup(OPENECS_SHOWN_MARK) : ECSIKeys_PrefixTextOf(function), function);
    }

    return entries;
}

/// @brief Builds a group's menu: its tabs, what acts on the whole group, then what acts on the tool.
/// @return stb_ds array of the entries.
static ECSIMenuEntry *ECSIMenus_BuildGroupMenu(ECSPanel shownPanel)
{
    ECSIMenuEntry *entries = nullptr;
    ECSIMenuEntry *tabs = nullptr;
    usz count = 0;
    usz shown = 0;
    const ECSPanel *panels = ECSILayout_GetGroup(shownPanel, &count, &shown);

    for (usz i = 0; i < count; i++)
    {
        ECSIMenus_Add(&tabs, panels[i]->title, i == shown ? SDL_strdup(OPENECS_SHOWN_MARK) : nullptr, nullptr)->tab = panels[i];
    }

    ECSIMenus_AddSubmenu(&entries, "Tabs", tabs);
    ECSIMenus_AddSubmenu(&entries, "Workspaces", ECSIMenus_BuildWorkspaceMenu());
    ECSIMenus_AddFunction(&entries, "ecs.layout.maximize", nullptr, shownPanel);
    ECSIMenus_AddFunction(&entries, "ecs.layout.lock", nullptr, shownPanel);
    ECSIMenus_AddFunction(&entries, "ecs.layout.closeGroup", nullptr, shownPanel);
    ECSIMenus_AddFunction(&entries, "ecs.layout.reopen", nullptr, shownPanel);
    ECSIMenus_AddToolEntries(&entries, shownPanel);
    return entries;
}

/// @brief Highlights an entry, and closes the submenus of the other entries.
static void ECSIMenus_Select(usz level, usz index)
{
    ECSIMenus_CloseFrom(level + 1);
    ECSIWindow_SelectMenuItem(level, index);
}

/// @brief Highlights an entry and opens its submenu beside it, if it has one.
static void ECSIMenus_SelectAndOpen(usz level, usz index)
{
    ECSIMenus_Select(level, index);
    const ECSIMenuEntry *entry = &MENUS.levels[level].entries[index];

    if (entry->submenu != nullptr)
    {
        ECSIMenus_OpenLevel(entry->submenu, ECSIWindow_MenuItemRect(level, index));
    }
}

/// @brief Uses an entry: opens its submenu, shows its tab, or closes the menus and runs its function on the focused panel.
static void ECSIMenus_Run(usz level, usz index)
{
    const ECSIMenuEntry *entry = &MENUS.levels[level].entries[index];

    if (entry->submenu != nullptr)
    {
        ECSIMenus_SelectAndOpen(level, index);
        return;
    }

    // closing the menus frees the entry, so what it does is copied first
    ECSPanel tab = entry->tab;
    char *function = entry->function == nullptr ? nullptr : SDL_strdup(entry->function);
    ECSIMenus_CloseFrom(0);

    if (tab != nullptr)
    {
        ECSLayout_Focus(tab);
    }

    if (function != nullptr)
    {
        ECSIServices_CallBound(function, ECSILayout_GetFocus());
        SDL_free(function);
    }
}

/// @brief Closes the open menus, and shows a menu at a point with its first entry highlighted. The menu's entries are taken over.
static void ECSIMenus_Show(ECSIMenuEntry *entries, f32 x, f32 y)
{
    ECSIMenus_CloseFrom(0);
    MENUS.entries = entries;
    ECSIMenus_OpenLevel(entries, (SDL_FRect){x, y, 0.0f, 0.0f});
}

#pragma region Core Functions

static void ECSIMenus_FocusLeft(void)
{
    ECSILayout_SetFocus(ECSILayout_FindNeighbour(-1, 0));
}

static void ECSIMenus_FocusRight(void)
{
    ECSILayout_SetFocus(ECSILayout_FindNeighbour(1, 0));
}

static void ECSIMenus_FocusUp(void)
{
    ECSILayout_SetFocus(ECSILayout_FindNeighbour(0, -1));
}

static void ECSIMenus_FocusDown(void)
{
    ECSILayout_SetFocus(ECSILayout_FindNeighbour(0, 1));
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
    ECSILayout_SetFocus(ECSILayout_NextTab());
}

/// @brief The user's close: the focused panel, unless its group is locked.
static void ECSIMenus_Close(void)
{
    ECSPanel focus = ECSILayout_GetFocus();

    if (focus != nullptr && ECSILayout_IsLocked(focus))
    {
        SDL_Log("'%s' is locked; unlock it to close it.", focus->title);
    }
    else if (focus != nullptr)
    {
        ECSLayout_Close(focus);
    }
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
    ECSPanel *panels = nullptr;

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

static void ECSIMenus_Restart(void)
{
    ECSPanel focus = ECSILayout_GetFocus();

    if (focus != nullptr && ECSIPanel_Restart(focus))
    {
        ECSILayout_RequestFrame();
    }
}

/// @brief Builds the tool's menu: the functions that ecs.toolMenu names and that are registered, the submenu Workspaces, then reset the layout and quit.
static ECSIMenuEntry *ECSIMenus_BuildToolMenu(void)
{
    ECSIMenuEntry *entries = nullptr;
    ECSPanel focus = ECSILayout_GetFocus();
    const ECSValue *functions = ECSSetting_Get(OPENECS_TOOL_MENU);

    for (usz i = 0; i < ECSValue_GetListCount(functions); i++)
    {
        const char *function = ECSValue_GetString(ECSValue_GetListItem(functions, i), nullptr);

        // a function of a plugin that the tool does not load is left out
        if (function != nullptr && ECSIServices_GetDescription(function) != nullptr)
        {
            ECSIMenus_AddFunction(&entries, function, nullptr, focus);
        }
    }

    ECSIMenus_AddSubmenu(&entries, "Workspaces", ECSIMenus_BuildWorkspaceMenu());
    ECSIMenus_AddToolEntries(&entries, focus);
    return entries;
}

/// @brief Shows the tool's menu near the top left of the focused panel, or of the window without one.
static void ECSIMenus_ShowTool(void)
{
    ECSPanel focus = ECSILayout_GetFocus();
    f32 x = (focus != nullptr ? focus->x : 0.0f) + OPENECS_MENU_OFFSET;
    f32 y = (focus != nullptr ? focus->y : 0.0f) + OPENECS_MENU_OFFSET;
    ECSIMenus_Show(ECSIMenus_BuildToolMenu(), x, y);
}

/// @brief Shows the menu of workspaces near the top left of the focused panel, or of the window without one.
static void ECSIMenus_ShowWorkspaces(void)
{
    ECSPanel focus = ECSILayout_GetFocus();
    f32 x = (focus != nullptr ? focus->x : 0.0f) + OPENECS_MENU_OFFSET;
    f32 y = (focus != nullptr ? focus->y : 0.0f) + OPENECS_MENU_OFFSET;
    ECSIMenus_Show(ECSIMenus_BuildWorkspaceMenu(), x, y);
}

/// @brief Defines the core's two functions for a workspace number: switching to the workspace, and moving the focused panel to it.
#define ECSIMenus_WorkspaceFunctions(number)            \
    static void ECSIMenus_Workspace##number(void)       \
    {                                                   \
        ECSILayout_WorkspaceSwitch(number - 1);         \
    }                                                   \
                                                        \
    static void ECSIMenus_MoveToWorkspace##number(void) \
    {                                                   \
        ECSILayout_MoveToWorkspace(number - 1);         \
    }

// clang-format off
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
// clang-format on

/// @brief The entries of the core function table for a workspace number.
#define ECSIMenus_WorkspaceEntries(number)                                                               \
    {"ecs.workspace.switch" #number, ECSIMenus_Workspace##number, "Workspace " #number},                 \
    {                                                                                                    \
        "ecs.layout.moveToWorkspace" #number, ECSIMenus_MoveToWorkspace##number, "To workspace " #number \
    }

/// @brief The core's bindable functions: name, function and description.
static const struct
{
    const char *name;
    ECSFunction Function;
    const char *description;
} OPENECS_CORE_FUNCTIONS[] = {
    {"ecs.layout.focusLeft", ECSIMenus_FocusLeft, "Focus left"},
    {"ecs.layout.focusRight", ECSIMenus_FocusRight, "Focus right"},
    {"ecs.layout.focusUp", ECSIMenus_FocusUp, "Focus up"},
    {"ecs.layout.focusDown", ECSIMenus_FocusDown, "Focus down"},
    {"ecs.layout.moveLeft", ECSIMenus_MoveLeft, "Move left"},
    {"ecs.layout.moveRight", ECSIMenus_MoveRight, "Move right"},
    {"ecs.layout.moveUp", ECSIMenus_MoveUp, "Move up"},
    {"ecs.layout.moveDown", ECSIMenus_MoveDown, "Move down"},
    {"ecs.layout.nextTab", ECSIMenus_NextTab, "Next tab"},
    {"ecs.layout.maximize", ECSILayout_ToggleMaximize, "Maximize"},
    {"ecs.layout.popOut", ECSILayout_PopOut, "Pop out"},
    {"ecs.layout.close", ECSIMenus_Close, "Close"},
    {"ecs.layout.closeGroup", ECSIMenus_CloseGroup, "Close group"},
    {"ecs.layout.lock", ECSILayout_ToggleLock, "Lock"},
    {"ecs.layout.reopen", ECSILayout_Reopen, "Reopen closed"},
    {"ecs.panel.restart", ECSIMenus_Restart, "Restart"},
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
    {"ecs.workspace.menu", ECSIMenus_ShowWorkspaces, "Workspaces"},
    {"ecs.app.menu", ECSIMenus_ShowTool, "Tool menu"},
};

#pragma endregion Core Functions

#pragma endregion Source Only

SHUResult ECSIMenus_Initialize(void)
{
    const ECSSettingDesc toolMenu = {.name = OPENECS_TOOL_MENU, .type = ECSSettingType_List, .description = "The functions that the tool's menu shows first, by name, such as \"settings.open\"; the title bar opens the menu with a right click"};
    SHU_ReturnResult(ECSISettings_DeclareCore(&toolMenu));

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
    if (SDL_strcmp(function, "ecs.panel.restart") == 0)
    {
        return panel != nullptr && ECSIPanel_CanRestart(panel);
    }

    if (SDL_strcmp(function, "ecs.layout.reopen") == 0)
    {
        return ECSILayout_CanReopen();
    }

    // a panel alone in its OS window stays, and so does a locked one
    if (SDL_strcmp(function, "ecs.layout.popOut") == 0)
    {
        return panel != nullptr && ECSILayout_CanPopOut(panel);
    }

    // the functions that move or close panels; the user cannot do that to a locked group
    if (SDL_strcmp(function, "ecs.layout.close") == 0 || SDL_strcmp(function, "ecs.layout.closeGroup") == 0 || SDL_strncmp(function, "ecs.layout.move", SDL_strlen("ecs.layout.move")) == 0)
    {
        return panel != nullptr && !ECSILayout_IsLocked(panel);
    }

    return true;
}

const char *ECSIMenus_LabelOf(const char *function, ECSPanel panel)
{
    if (SDL_strcmp(function, "ecs.layout.maximize") == 0)
    {
        return ECSILayout_IsMaximized(panel) ? "Restore" : "Maximize";
    }

    if (SDL_strcmp(function, "ecs.layout.lock") == 0)
    {
        return ECSILayout_IsLocked(panel) ? "Unlock" : "Lock";
    }

    const char *description = ECSIServices_GetDescription(function);
    return description != nullptr ? description : function;
}

usz ECSIMenus_GetOrder(const char *function)
{
    SDL_assert(function != nullptr);

    usz position = 0;

    while (position < SDL_arraysize(OPENECS_CORE_FUNCTIONS) && SDL_strcmp(OPENECS_CORE_FUNCTIONS[position].name, function) != 0)
    {
        position++;
    }

    return position;
}

void ECSIMenus_OpenPanelMenu(ECSPanel panel, f32 x, f32 y)
{
    ECSILayout_SetFocus(panel);
    ECSIMenus_Show(ECSIMenus_BuildPanelMenu(panel), x, y);
}

void ECSIMenus_OpenGroupMenu(ECSPanel shownPanel, f32 x, f32 y)
{
    ECSILayout_SetFocus(shownPanel);
    ECSIMenus_Show(ECSIMenus_BuildGroupMenu(shownPanel), x, y);
}

void ECSIMenus_OpenToolMenu(f32 x, f32 y)
{
    ECSIMenus_Show(ECSIMenus_BuildToolMenu(), x, y);
}

bool ECSIMenus_Handle(const SDL_Event *event)
{
    if (arrlenu(MENUS.levels) == 0)
    {
        return false;
    }

    usz deepest = arrlenu(MENUS.levels) - 1;
    usz count = arrlenu(MENUS.levels[deepest].entries);
    usz selected = ECSIWindow_GetSelectedMenuItem(deepest);
    usz level = 0;
    usz index = 0;

    switch (event->type)
    {
    case SDL_EVENT_MOUSE_MOTION:
        // pointing at an entry highlights it and opens its submenu
        if (ECSIWindow_MenuItemAt(event->motion.x, event->motion.y, &level, &index))
        {
            bool hasSubmenu = MENUS.levels[level].entries[index].submenu != nullptr;

            if (ECSIWindow_GetSelectedMenuItem(level) != index || arrlenu(MENUS.levels) != level + (hasSubmenu ? 2 : 1))
            {
                ECSIMenus_SelectAndOpen(level, index);
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
            ECSIMenus_Select(deepest, (selected + count - 1) % count);
            break;
        case SDLK_DOWN:
            ECSIMenus_Select(deepest, (selected + 1) % count);
            break;
        case SDLK_RIGHT:
            if (MENUS.levels[deepest].entries[selected].submenu != nullptr)
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
