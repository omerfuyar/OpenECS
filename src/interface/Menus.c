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

/// @brief How far from the focused panel's corner a menu that keys open is, in layout units.
#define OPENECS_MENU_OFFSET 24.0f

/// @brief Most workspaces that have their own functions: keys 1 to 9 and 0.
#define OPENECS_WORKSPACE_FUNCTION_COUNT 10

/// @brief An entry of a menu: it runs a function, shows a tab or opens a submenu.
typedef struct ECSIMenuEntry
{
    char *label;
    char *keys;                     // text of the keys that run it, or nullptr
    char *function;                 // the function it runs on the focused panel, or nullptr
    ECSPanel tab;                   // the panel whose tab it shows, or nullptr
    struct ECSIMenuEntry *submenu; // stb_ds array of the entries of the submenu it opens, or nullptr
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
    if (panel != nullptr)
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
static void ECSIMenus_BuildPanelMenu(ECSIMenuEntry **entries, ECSPanel panel)
{
    ECSIMenuEntry *move = nullptr;
    ECSIMenuEntry *workspaces = nullptr;

    ECSIMenus_AddFunction(entries, "ecs.layout.close", nullptr, panel);
    ECSIMenus_AddFunction(entries, "ecs.panel.restart", nullptr, panel);
    ECSIMenus_AddFunction(entries, "ecs.layout.maximize", nullptr, panel);
    ECSIMenus_AddFunction(entries, "ecs.layout.popOut", nullptr, panel);
    ECSIMenus_AddFunction(entries, "ecs.layout.lock", nullptr, panel);
    ECSIMenus_AddFunction(entries, "ecs.layout.reopen", nullptr, panel);

    ECSIMenus_AddFunction(&move, "ecs.layout.moveLeft", "Left", panel);
    ECSIMenus_AddFunction(&move, "ecs.layout.moveRight", "Right", panel);
    ECSIMenus_AddFunction(&move, "ecs.layout.moveUp", "Up", panel);
    ECSIMenus_AddFunction(&move, "ecs.layout.moveDown", "Down", panel);
    ECSIMenus_AddSubmenu(entries, "Move", move);

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

    ECSIMenus_AddSubmenu(entries, "To workspace", workspaces);
    ECSIMenus_AddTypeEntries(entries, panel);
    ECSIMenus_AddToolEntries(entries, panel);
}

/// @brief Builds the menu of workspaces: each one by number and name, with the current one marked; an entry switches to it.
static void ECSIMenus_BuildWorkspaceMenu(ECSIMenuEntry **entries)
{
    for (usz number = 1; number <= ECSWorkspace_GetCount() && number <= OPENECS_WORKSPACE_FUNCTION_COUNT; number++)
    {
        char function[32];
        char label[256];
        SDL_snprintf(function, sizeof(function), "ecs.workspace.switch%zu", number);
        SDL_snprintf(label, sizeof(label), "%zu: %s", number, ECSWorkspace_GetName(number));
        ECSIMenus_Add(entries, label, number == ECSWorkspace_GetCurrent() ? SDL_strdup(OPENECS_SHOWN_MARK) : ECSIKeys_PrefixTextOf(function), function);
    }
}

/// @brief Builds a group's menu: its tabs, what acts on the whole group, then what acts on the tool.
static void ECSIMenus_BuildGroupMenu(ECSIMenuEntry **entries, ECSPanel shownPanel)
{
    ECSIMenuEntry *tabs = nullptr;
    usz count = 0;
    usz shown = 0;
    const ECSPanel *panels = ECSILayout_GetGroup(shownPanel, &count, &shown);

    for (usz i = 0; i < count; i++)
    {
        ECSIMenus_Add(&tabs, panels[i]->title, i == shown ? SDL_strdup(OPENECS_SHOWN_MARK) : nullptr, nullptr)->tab = panels[i];
    }

    ECSIMenus_AddSubmenu(entries, "Tabs", tabs);
    ECSIMenuEntry *workspaces = nullptr;
    ECSIMenus_BuildWorkspaceMenu(&workspaces);
    ECSIMenus_AddSubmenu(entries, "Workspaces", workspaces);
    ECSIMenus_AddFunction(entries, "ecs.layout.maximize", nullptr, shownPanel);
    ECSIMenus_AddFunction(entries, "ecs.layout.lock", nullptr, shownPanel);
    ECSIMenus_AddFunction(entries, "ecs.layout.closeGroup", nullptr, shownPanel);
    ECSIMenus_AddFunction(entries, "ecs.layout.reopen", nullptr, shownPanel);
    ECSIMenus_AddToolEntries(entries, shownPanel);
}

/// @brief Highlights an entry, and closes the submenus of the other entries.
/// @param open Also opens the entry's submenu, if it has one.
static void ECSIMenus_Select(usz level, usz index, bool open)
{
    ECSIMenus_CloseFrom(level + 1);
    MENUS.levels[level].selected = index;
    ECSIWindow_SelectMenuItem(level, index);

    const ECSIMenuEntry *entry = &MENUS.levels[level].entries[index];

    if (open && entry->submenu != nullptr)
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
        ECSIMenus_Select(level, index, true);
        return;
    }

    // closing the menus frees the entry, so what it does is copied first
    ECSPanel tab = entry->tab;
    char *function = entry->function == nullptr ? nullptr : SDL_strdup(entry->function);
    ECSIMenus_CloseFrom(0);

    if (tab != nullptr)
    {
        ECSILayout_ShowTab(tab);
    }

    if (function != nullptr)
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

static void ECSIMenus_PopOut(void)
{
    ECSILayout_PopOut();
}

static void ECSIMenus_Close(void)
{
    ECSLayout_Close(nullptr);
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

    if (focus != nullptr && ECSIPanel_Restart(focus))
    {
        ECSILayout_RequestFrame();
    }
}

/// @brief Shows the menu of workspaces near the top left of the focused panel, or of the window without one.
static void ECSIMenus_ShowWorkspaces(void)
{
    ECSPanel focus = ECSILayout_GetFocus();
    ECSIMenus_CloseFrom(0);
    ECSIMenus_BuildWorkspaceMenu(&MENUS.entries);
    ECSIMenus_OpenLevel(MENUS.entries, (SDL_FRect){(focus != nullptr ? focus->x : 0.0f) + OPENECS_MENU_OFFSET, (focus != nullptr ? focus->y : 0.0f) + OPENECS_MENU_OFFSET, 0.0f, 0.0f});
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
    {"ecs.workspace.switch" #number, ECSIMenus_Workspace##number, "Workspace " #number},                        \
    {                                                                                                               \
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
    {"ecs.layout.maximize", ECSIMenus_Maximize, "Maximize"},
    {"ecs.layout.popOut", ECSIMenus_PopOut, "Pop out"},
    {"ecs.layout.close", ECSIMenus_Close, "Close"},
    {"ecs.layout.closeGroup", ECSIMenus_CloseGroup, "Close group"},
    {"ecs.layout.lock", ECSIMenus_Lock, "Lock"},
    {"ecs.layout.reopen", ECSIMenus_Reopen, "Reopen closed"},
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
            bool hasSubmenu = MENUS.levels[level].entries[index].submenu != nullptr;

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
