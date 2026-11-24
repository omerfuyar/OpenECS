#include "interface/Menus.h"

#include "base/Log.h"
#include "interface/Keys.h"
#include "interface/Layout.h"
#include "interface/Panels.h"
#include "interface/Window.h"
#include "runtime/Plugins.h"
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

/// @brief The setting that names the title bar's menus and their functions.
#define OPENECS_TITLE_MENUS "ecs.titleMenus"

/// @brief The setting that names the title bar's buttons and their functions.
#define OPENECS_TITLE_BUTTONS "ecs.titleButtons"

/// @brief The menu of the plugins' entries for the whole tool, in the tool's menu and in the title bar.
#define OPENECS_TOOLS_LABEL "Tools"

/// @brief What marks the workspaces' menu in the title bar, after the current workspace's name.
#define OPENECS_DROPDOWN_MARK " \u25BE"

/// @brief Where the title bar's menu of the plugins' entries, and its workspaces' menu, are among the sources of its items, which are places in settings.
#define OPENECS_TOOLS_SOURCE SIZE_MAX
#define OPENECS_WORKSPACES_SOURCE (SIZE_MAX - 1)

/// @brief Most workspaces that have their own functions: keys 1 to 9 and 0.
#define OPENECS_WORKSPACE_FUNCTION_COUNT 10

/// @brief An entry of a menu: it runs a function, shows a tab or opens a submenu.
typedef struct ECSIMenuEntry
{
    char *label;
    bool heading;                  // it only names the entries below it, which a line sets apart
    char *keys;                    // text of the keys that run it, or nullptr
    char *function;                // the function it runs on the focused panel, or nullptr
    ECSPanel tab;                  // the panel whose tab it shows, or nullptr
    u32 tabID;                     // its id, as a new panel may get the address of one that closed while the menu is open
    struct ECSIMenuEntry *submenu; // stb_ds array of the entries of the submenu it opens, or nullptr
} ECSIMenuEntry;

/// @brief An open menu: the panel menu or a submenu. The window shows it and keeps its highlighted entry.
typedef struct ECSIMenuLevel
{
    ECSIMenuEntry *entries; // owned by the panel menu's entries
    const char **lines;     // stb_ds array of each entry's key text and label, which the window shows
} ECSIMenuLevel;

/// @brief An entry of ecs.titleMenus or ecs.titleButtons, as read from the setting.
typedef struct ECSIMenuSource
{
    const char *function; // the function it runs, registered by a plugin of the tool
    const char *label;    // its own label, or nullptr for the function's
    bool on;              // its setting is true
} ECSIMenuSource;

/// @brief A function that a plugin adds to the tool's menu.
typedef struct ECSIToolEntry
{
    ECSPlugin plugin;
    char *function;
} ECSIToolEntry;

static struct
{
    ECSIMenuEntry *entries;     // stb_ds array of the panel menu's entries
    ECSIToolEntry *toolEntries; // stb_ds array of the functions plugins add to the tool's menu
    char *workspaceLabel;       // the workspaces' menu's label in the title bar
    ECSIMenuLevel *levels;      // stb_ds array of the open menus, the panel menu first; while any is open, it gets the pointer and the keys

    ECSITitleItem *titleItems;  // stb_ds array of the title bar's menus and buttons, made again before each frame
    usz *titleSources;          // stb_ds array of where each item is in its setting: a menu in ecs.titleMenus, a button in ecs.titleButtons; or the menu Tools or the workspaces' menu
    i64 titleMenu;              // the item whose menu is open, or -1
    ECSIDrawnTitleItem tooltip; // the part of the title bar whose tooltip was made last
    bool tooltipMade;
} MENUS = {.titleMenu = -1};

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
        ECSIWindow_ShowMenu(level, nullptr, (SDL_FRect){0}, nullptr, 0);
    }

    if (level == 0)
    {
        arrfree(MENUS.levels);
        ECSIMenus_FreeEntries(&MENUS.entries);
        MENUS.titleMenu = -1;
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
/// @param panel For the panel menu: the panel whose OS window shows it, or nullptr for the OS window of the event that opens it.
/// @param anchor The point the panel menu opens at, or the entry a submenu opens beside.
static void ECSIMenus_OpenLevel(ECSIMenuEntry *entries, ECSPanel panel, SDL_FRect anchor)
{
    ECSIMenuLevel level = {.entries = entries};

    // an entry that opens a submenu shows an arrow instead of keys, and a heading shows no keys
    for (usz i = 0; i < arrlenu(entries); i++)
    {
        const char *keys = entries[i].submenu != nullptr ? OPENECS_SUBMENU_MARK : entries[i].keys != nullptr ? entries[i].keys
                                                                                                             : "";
        arrput(level.lines, entries[i].heading ? nullptr : keys);
        arrput(level.lines, entries[i].label != nullptr ? entries[i].label : "");
    }

    arrput(MENUS.levels, level);
    ECSIWindow_ShowMenu(arrlenu(MENUS.levels) - 1, panel, anchor, level.lines, arrlenu(entries));
}

/// @brief Adds the entries of a panel's type, with the keys its plugin bound to the same functions for the type, under a heading with the plugin's name.
static void ECSIMenus_AddTypeEntries(ECSIMenuEntry **entries, ECSPanel panel)
{
    usz count = 0;
    const char *const *functions = ECSIPanel_GetMenuEntries(panel, &count);

    if (count > 0)
    {
        ECSIMenus_Add(entries, ECSIPlugin_GetName(panel->type->plugin), nullptr, nullptr)->heading = true;
    }

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
    ECSIMenus_AddFunction(&entries, "ecs.panel.reloadPlugin", nullptr, panel);
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
        ECSIMenuEntry *entry = ECSIMenus_Add(&tabs, panels[i]->title, i == shown ? SDL_strdup(OPENECS_SHOWN_MARK) : nullptr, nullptr);
        entry->tab = panels[i];
        entry->tabID = panels[i]->id;
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
        ECSIMenus_OpenLevel(entry->submenu, nullptr, ECSIWindow_MenuItemRect(level, index));
    }
}

/// @brief Uses an entry: opens its submenu, shows its tab, or closes the menus and runs its function on the focused panel.
static void ECSIMenus_Run(usz level, usz index)
{
    const ECSIMenuEntry *entry = &MENUS.levels[level].entries[index];

    if (entry->heading)
    {
        return;
    }

    if (entry->submenu != nullptr)
    {
        ECSIMenus_SelectAndOpen(level, index);
        return;
    }

    // closing the menus frees the entry, so what it does is copied first
    ECSPanel tab = entry->tab != nullptr && ECSILayout_HasPanel(entry->tab) && entry->tab->id == entry->tabID ? entry->tab : nullptr;
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
/// @param panel The panel whose OS window shows the menu and has the point, or nullptr for the OS window of the event that opens it.
static void ECSIMenus_Show(ECSIMenuEntry *entries, ECSPanel panel, f32 x, f32 y)
{
    ECSIMenus_CloseFrom(0);
    MENUS.entries = entries;
    ECSIMenus_OpenLevel(entries, panel, (SDL_FRect){x, y, 0.0f, 0.0f});
}

/// @brief Reads an entry of ecs.titleMenus or ecs.titleButtons: a function's name, or a table with the function's name first, and its label and setting.
/// @return false if the entry names no function that a plugin of the tool registered.
static bool ECSIMenus_ReadSource(const ECSValue *entry, ECSIMenuSource *retSource)
{
    const char *setting = ECSValue_GetString(ECSValue_GetTableField(entry, "setting"), nullptr);
    retSource->function = ECSValue_GetString(entry, nullptr);
    retSource->function = retSource->function != nullptr ? retSource->function : ECSValue_GetString(ECSValue_GetListItem(entry, 0), nullptr);
    retSource->label = ECSValue_GetString(ECSValue_GetTableField(entry, "label"), nullptr);
    retSource->on = setting != nullptr && ECSValue_GetBool(ECSSetting_Get(setting), false);
    return retSource->function != nullptr && ECSIServices_GetDescription(retSource->function) != nullptr;
}

/// @brief Builds the entries of a menu of ecs.titleMenus: the functions it names that can be used now, an entry that is on marked.
/// @return stb_ds array of the entries.
static ECSIMenuEntry *ECSIMenus_BuildTitleMenu(const ECSValue *menu, ECSPanel focus)
{
    ECSIMenuEntry *entries = nullptr;

    for (usz i = 0; i < ECSValue_GetListCount(menu); i++)
    {
        ECSIMenuSource source = {0};

        if (ECSIMenus_ReadSource(ECSValue_GetListItem(menu, i), &source) && ECSIMenus_Offers(source.function, focus))
        {
            const char *label = source.label != nullptr ? source.label : ECSIMenus_LabelOf(source.function, focus);
            ECSIMenus_Add(&entries, label, source.on ? SDL_strdup(OPENECS_SHOWN_MARK) : ECSIKeys_PrefixTextOf(source.function), source.function);
        }
    }

    return entries;
}

static ECSIMenuEntry *ECSIMenus_BuildToolsMenu(ECSPanel focus);

/// @brief Opens a menu of the title bar below its part: a menu of ecs.titleMenus, the plugins' entries for the tool, or the workspaces.
/// @param item The item's place among the title bar's items.
static void ECSIMenus_OpenTitleMenu(usz item, SDL_FRect rect)
{
    usz source = MENUS.titleSources[item];
    ECSPanel focus = ECSILayout_GetFocus();
    ECSIMenuEntry *entries = source == OPENECS_TOOLS_SOURCE        ? ECSIMenus_BuildToolsMenu(focus)
                             : source == OPENECS_WORKSPACES_SOURCE ? ECSIMenus_BuildWorkspaceMenu()
                                                                   : ECSIMenus_BuildTitleMenu(ECSValue_GetListItem(ECSSetting_Get(OPENECS_TITLE_MENUS), source), focus);

    // the window keeps the menu inside it, so the workspaces' menu at the right opens to the left
    ECSIMenus_Show(entries, nullptr, rect.x, rect.y + rect.h);
    MENUS.titleMenu = (i64)item;
}

/// @brief Makes the tooltip of the part of the title bar whose tooltip is due: what its function does, and the keys that run it.
static void ECSIMenus_MakeTooltip(void)
{
    ECSIDrawnTitleItem item = {0};

    // no tooltip covers an open menu
    if (arrlenu(MENUS.levels) > 0 || !ECSIWindow_GetDueTooltip(&item))
    {
        MENUS.tooltipMade = false;
        return;
    }

    if (MENUS.tooltipMade && item.kind == MENUS.tooltip.kind && item.index == MENUS.tooltip.index)
    {
        return;
    }

    MENUS.tooltip = item;
    MENUS.tooltipMade = true;
    ECSIMenuSource source = {0};
    const char *function = nullptr;

    if (item.kind == ECSITitleKind_Name)
    {
        function = "ecs.app.menu";
    }
    else if (item.kind == ECSITitleKind_Workspace)
    {
        function = "ecs.workspace.menu";
    }
    else if (item.kind == ECSITitleKind_Button && ECSIMenus_ReadSource(ECSValue_GetListItem(ECSSetting_Get(OPENECS_TITLE_BUTTONS), MENUS.titleSources[item.index]), &source))
    {
        function = source.function;
    }

    if (function == nullptr || ECSIServices_GetDescription(function) == nullptr)
    {
        ECSIWindow_SetTooltip(nullptr, nullptr);
        return;
    }

    // the keys after the prefix, or the key bound for the focused panel's type
    ECSPanel focus = ECSILayout_GetFocus();
    char *keys = ECSIKeys_PrefixTextOf(function);
    keys = keys == nullptr && focus != nullptr ? ECSIKeys_BoundTextOf(focus->typeName, function) : keys;
    ECSIWindow_SetTooltip(ECSIMenus_LabelOf(function, focus), keys);
    SDL_free(keys);
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

/// @brief Reloads the plugin of the focused panel, or of the placeholder whose plugin a reload failed to start (DESIGN 9.11).
static void ECSIMenus_ReloadPlugin(void)
{
    ECSPanel focus = ECSILayout_GetFocus();
    char *name = focus == nullptr ? nullptr : SDL_strdup(focus->typeName);
    char *dot = name == nullptr ? nullptr : SDL_strchr(name, '.');

    // a panel type's name starts with its plugin's name
    if (dot != nullptr)
    {
        *dot = '\0';

        if (ECSPlugins_Reload(name))
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' is not reloaded: out of memory.", name);
        }
    }

    SDL_free(name);
}

/// @brief Orders two entries by their labels.
static int SDLCALL ECSIMenus_CompareLabels(const void *first, const void *second)
{
    return SDL_strcmp(((const ECSIMenuEntry *)first)->label, ((const ECSIMenuEntry *)second)->label);
}

/// @brief Builds the menu of the functions that plugins add to the tool's menu, those that can be used now, by their labels, whatever order the
/// plugins load in.
/// @return stb_ds array of the entries.
static ECSIMenuEntry *ECSIMenus_BuildToolsMenu(ECSPanel focus)
{
    ECSIMenuEntry *entries = nullptr;

    for (usz i = 0; i < arrlenu(MENUS.toolEntries); i++)
    {
        if (ECSIServices_GetDescription(MENUS.toolEntries[i].function) != nullptr)
        {
            ECSIMenus_AddFunction(&entries, MENUS.toolEntries[i].function, nullptr, focus);
        }
    }

    SDL_qsort(entries, arrlenu(entries), sizeof(ECSIMenuEntry), ECSIMenus_CompareLabels);
    return entries;
}

/// @brief Builds the tool's menu: the functions that ecs.toolMenu names and that are registered, the submenus Tools and Workspaces, then reset the layout and quit.
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

    ECSIMenus_AddSubmenu(&entries, OPENECS_TOOLS_LABEL, ECSIMenus_BuildToolsMenu(focus));
    ECSIMenus_AddSubmenu(&entries, "Workspaces", ECSIMenus_BuildWorkspaceMenu());
    ECSIMenus_AddToolEntries(&entries, focus);
    return entries;
}

/// @brief Shows the tool's menu near the top left of the focused panel, or of the window without one.
static void ECSIMenus_ShowTool(void)
{
    ECSPanel focus = ECSILayout_GetFocus();
    f32 x = (focus != nullptr ? focus->x : 0.0f) + OPENECS_MENU_OFFSET * ECSIPanels_GetScale();
    f32 y = (focus != nullptr ? focus->y : 0.0f) + OPENECS_MENU_OFFSET * ECSIPanels_GetScale();
    ECSIMenus_Show(ECSIMenus_BuildToolMenu(), focus, x, y);
}

/// @brief Shows the menu of workspaces near the top left of the focused panel, or of the window without one.
static void ECSIMenus_ShowWorkspaces(void)
{
    ECSPanel focus = ECSILayout_GetFocus();
    f32 x = (focus != nullptr ? focus->x : 0.0f) + OPENECS_MENU_OFFSET * ECSIPanels_GetScale();
    f32 y = (focus != nullptr ? focus->y : 0.0f) + OPENECS_MENU_OFFSET * ECSIPanels_GetScale();
    ECSIMenus_Show(ECSIMenus_BuildWorkspaceMenu(), focus, x, y);
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
    {"ecs.panel.reloadPlugin", ECSIMenus_ReloadPlugin, "Reload plugin"},
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
    const ECSSettingDesc settings[] = {
        {.name = OPENECS_TOOL_MENU, .type = ECSSettingType_List, .description = "The functions that the tool's menu shows first, by name, such as \"settings.open\"; the tool's name in the title bar opens the menu, and so does a right click on the title bar"},
        {.name = OPENECS_TITLE_MENUS, .type = ECSSettingType_List, .description = "The menus of the title bar, such as { label = \"File\", \"ecs.session.open\" }: each entry is a function's name, or { \"name\", label = \"Label\", setting = \"bool setting that marks it on\" }"},
        {.name = OPENECS_TITLE_BUTTONS, .type = ECSSettingType_List, .description = "The tool's buttons in the title bar, after the menus: each is a function's name, or { \"name\", label = \"Label\", setting = \"bool setting that shows it on\" }"},
    };

    for (usz i = 0; i < SDL_arraysize(settings); i++)
    {
        SHU_ReturnResult(ECSISettings_DeclareCore(&settings[i]));
    }

    for (usz i = 0; i < SDL_arraysize(OPENECS_CORE_FUNCTIONS); i++)
    {
        SHU_ReturnResult(ECSIServices_RegisterCore(OPENECS_CORE_FUNCTIONS[i].name, OPENECS_CORE_FUNCTIONS[i].Function, "void()", OPENECS_CORE_FUNCTIONS[i].description));
    }

    return SHUResult_Ok;
}

void ECSIMenus_Terminate(void)
{
    ECSIMenus_CloseFrom(0);
    arrfree(MENUS.titleItems);
    arrfree(MENUS.titleSources);
    SDL_free(MENUS.workspaceLabel);

    for (usz i = 0; i < arrlenu(MENUS.toolEntries); i++)
    {
        SDL_free(MENUS.toolEntries[i].function);
    }

    arrfree(MENUS.toolEntries);
    SDL_zero(MENUS);
    MENUS.titleMenu = -1;
}

/// @brief Adds a menu of the title bar, which is on while it is open.
/// @param source Its place in its setting, or OPENECS_TOOLS_SOURCE or OPENECS_WORKSPACES_SOURCE.
static void ECSIMenus_PutTitleItem(const char *label, ECSITitleKind kind, usz source)
{
    // arrput grows the list before it reads the item, so the item's place is read first
    ECSITitleItem item = {label, kind, MENUS.titleMenu == (i64)arrlenu(MENUS.titleItems), true};
    arrput(MENUS.titleItems, item);
    arrput(MENUS.titleSources, source);
}

void ECSIMenus_UpdateTitleBar(void)
{
    arrsetlen(MENUS.titleItems, 0);
    arrsetlen(MENUS.titleSources, 0);
    ECSPanel focus = ECSILayout_GetFocus();
    const ECSValue *menus = ECSSetting_Get(OPENECS_TITLE_MENUS);
    const ECSValue *buttons = ECSSetting_Get(OPENECS_TITLE_BUTTONS);

    // a menu shows when it has an entry that can be used now
    for (usz i = 0; i < ECSValue_GetListCount(menus); i++)
    {
        const ECSValue *menu = ECSValue_GetListItem(menus, i);
        const char *label = ECSValue_GetString(ECSValue_GetTableField(menu, "label"), nullptr);
        bool offered = false;

        for (usz j = 0; label != nullptr && !offered && j < ECSValue_GetListCount(menu); j++)
        {
            ECSIMenuSource source = {0};
            offered = ECSIMenus_ReadSource(ECSValue_GetListItem(menu, j), &source) && ECSIMenus_Offers(source.function, focus);
        }

        if (offered)
        {
            ECSIMenus_PutTitleItem(label, ECSITitleKind_Menu, i);
        }
    }

    // the plugins' entries for the tool follow, in a menu of their own
    ECSIMenuEntry *tools = ECSIMenus_BuildToolsMenu(focus);

    if (arrlenu(tools) > 0)
    {
        ECSIMenus_PutTitleItem(OPENECS_TOOLS_LABEL, ECSITitleKind_Menu, OPENECS_TOOLS_SOURCE);
    }

    ECSIMenus_FreeEntries(&tools);

    // a button stays where it is while its function cannot be used, dimmed
    for (usz i = 0; i < ECSValue_GetListCount(buttons); i++)
    {
        ECSIMenuSource source = {0};

        if (ECSIMenus_ReadSource(ECSValue_GetListItem(buttons, i), &source))
        {
            const char *label = source.label != nullptr ? source.label : ECSIMenus_LabelOf(source.function, focus);
            arrput(MENUS.titleItems, ((ECSITitleItem){label, ECSITitleKind_Button, source.on, ECSIMenus_Offers(source.function, focus)}));
            arrput(MENUS.titleSources, i);
        }
    }

    // the workspaces' menu shows the current one's name, when there are two or more
    if (ECSWorkspace_GetCount() >= 2)
    {
        const char *name = ECSWorkspace_GetName(ECSWorkspace_GetCurrent());
        SDL_free(MENUS.workspaceLabel);

        if (SDL_asprintf(&MENUS.workspaceLabel, "%s" OPENECS_DROPDOWN_MARK, name != nullptr && name[0] != '\0' ? name : "?") < 0)
        {
            MENUS.workspaceLabel = nullptr;
        }

        ECSIMenus_PutTitleItem(MENUS.workspaceLabel != nullptr ? MENUS.workspaceLabel : "?", ECSITitleKind_Workspace, OPENECS_WORKSPACES_SOURCE);
    }

    ECSIWindow_SetTitleItems(MENUS.titleItems, arrlenu(MENUS.titleItems));
    ECSIMenus_MakeTooltip();
}

bool ECSIMenus_PressTitle(f32 x, f32 y)
{
    ECSIDrawnTitleItem item = {0};

    if (!ECSIWindow_TitleItemAt(x, y, &item))
    {
        return false;
    }

    // the items are made again before each frame, so a part drawn in the last frame may be gone
    if (item.kind != ECSITitleKind_Name && item.index >= arrlenu(MENUS.titleItems))
    {
        return true;
    }

    ECSIMenuSource source = {0};

    switch (item.kind)
    {
    case ECSITitleKind_Name:
        ECSIMenus_Show(ECSIMenus_BuildToolMenu(), nullptr, item.rect.x, item.rect.y + item.rect.h);
        break;
    case ECSITitleKind_Menu:
    case ECSITitleKind_Workspace:
        ECSIMenus_OpenTitleMenu(item.index, item.rect);
        break;
    case ECSITitleKind_Button:
        if (ECSIMenus_ReadSource(ECSValue_GetListItem(ECSSetting_Get(OPENECS_TITLE_BUTTONS), MENUS.titleSources[item.index]), &source) && ECSIMenus_Offers(source.function, ECSILayout_GetFocus()))
        {
            ECSIServices_CallBound(source.function, ECSILayout_GetFocus());
        }
        break;
    }

    return true;
}

void ECSIMenus_RemovePlugin(ECSPlugin plugin)
{
    for (usz i = arrlenu(MENUS.toolEntries); i > 0; i--)
    {
        if (MENUS.toolEntries[i - 1].plugin == plugin)
        {
            SDL_free(MENUS.toolEntries[i - 1].function);
            arrdel(MENUS.toolEntries, i - 1);
        }
    }
}

SHUResult ECSApp_AddMenuEntry(ECSPlugin plugin, const char *function)
{
    ECSILog_Check(plugin != nullptr && function != nullptr, SHUResult_ErrAssertion);

    // a plugin adds only its own functions
    if (!ECSIPlugin_OwnsName(plugin, function))
    {
        (void)ECS_Check(nullptr, false, "Plugin '%s' adds '%s' to the tool's menu, which is not one of its functions.", ECSIPlugin_GetName(plugin), function);
        return SHUResult_ErrAssertion;
    }

    ECSIToolEntry entry = {plugin, SDL_strdup(function)};

    if (entry.function == nullptr)
    {
        return SHUResult_ErrAllocation;
    }

    arrput(MENUS.toolEntries, entry);
    return SHUResult_Ok;
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

    // the panel's menu offers a reload while plugins reload, as a plugin's author works then
    if (SDL_strcmp(function, "ecs.panel.reloadPlugin") == 0)
    {
        return panel != nullptr && ECSValue_GetBool(ECSSetting_Get("ecs.reloadPlugins"), false);
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
    ECSIMenus_Show(ECSIMenus_BuildPanelMenu(panel), nullptr, x, y);
}

void ECSIMenus_OpenGroupMenu(ECSPanel shownPanel, f32 x, f32 y)
{
    ECSILayout_SetFocus(shownPanel);
    ECSIMenus_Show(ECSIMenus_BuildGroupMenu(shownPanel), nullptr, x, y);
}

void ECSIMenus_OpenToolMenu(f32 x, f32 y)
{
    ECSIMenus_Show(ECSIMenus_BuildToolMenu(), nullptr, x, y);
}

/// @brief Handles a move of the pointer while a menu is open: pointing at another menu of the title bar opens it, and pointing at an entry highlights it and opens its submenu.
static void ECSIMenus_Point(f32 x, f32 y)
{
    ECSIDrawnTitleItem item = {0};

    if (MENUS.titleMenu >= 0 && ECSIWindow_TitleItemAt(x, y, &item) && (item.kind == ECSITitleKind_Menu || item.kind == ECSITitleKind_Workspace) &&
        (i64)item.index != MENUS.titleMenu && item.index < arrlenu(MENUS.titleItems))
    {
        ECSIMenus_OpenTitleMenu(item.index, item.rect);
        return;
    }

    usz level = 0;
    usz index = 0;

    if (ECSIWindow_MenuItemAt(x, y, &level, &index) && !MENUS.levels[level].entries[index].heading)
    {
        bool hasSubmenu = MENUS.levels[level].entries[index].submenu != nullptr;

        if (ECSIWindow_GetSelectedMenuItem(level) != index || arrlenu(MENUS.levels) != level + (hasSubmenu ? 2 : 1))
        {
            ECSIMenus_SelectAndOpen(level, index);
        }
    }
}

/// @brief Finds the entry a step away from another, passing over headings.
/// @param step 1 for the next entry, or the count less 1 for the one before.
static usz ECSIMenus_Step(usz level, usz from, usz step)
{
    usz count = arrlenu(MENUS.levels[level].entries);
    usz index = (from + step) % count;

    while (MENUS.levels[level].entries[index].heading && index != from)
    {
        index = (index + step) % count;
    }

    return index;
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
        ECSIMenus_Point(event->motion.x, event->motion.y);
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
            ECSIMenus_Select(deepest, ECSIMenus_Step(deepest, selected, count - 1));
            break;
        case SDLK_DOWN:
            ECSIMenus_Select(deepest, ECSIMenus_Step(deepest, selected, 1));
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
