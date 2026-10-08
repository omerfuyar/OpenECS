#include "interface/Input.h"

#include "interface/Layout.h"
#include "interface/Panels.h"
#include "runtime/Services.h"
#include "runtime/Settings.h"

#include "SDL3/SDL.h"
#include "stb/stbSDL3.h"

#pragma region Source Only

/// @brief Start of the names of the functions that switch workspaces; the workspace's number follows.
#define OPENECS_WORKSPACE_FUNCTION "ecs.workspace_"

/// @brief Default of the setting ecs.prefix.
#define OPENECS_DEFAULT_PREFIX "Alt+W"

/// @brief Choices of the setting ecs.focus; the first is the default.
static const char *const ECSI_FOCUS_CHOICES[] = {"click", "hover", NULL};

/// @brief What a menu entry that opens a submenu shows instead of keys.
#define OPENECS_SUBMENU_MARK "\u203A"

/// @brief What the entry of the shown tab shows in a group's menu.
#define OPENECS_SHOWN_MARK "\u2022"

/// @brief Headings of the sections of the list of prefix keys, in the order ECSI_InputSectionOf numbers them.
static const char *const ECSI_PREFIX_SECTIONS[] = {"Navigation", "Panel", "More"};

/// @brief Functions that the list of prefix keys shows on one line when they run on the four arrows: left, right, up and down.
static const struct
{
    const char *functions[4];
    const char *description;
} ECSI_ARROW_FAMILIES[] = {
    {{"ecs.focus_left", "ecs.focus_right", "ecs.focus_up", "ecs.focus_down"}, "Focus the panel in that direction"},
    {{"ecs.move_left", "ecs.move_right", "ecs.move_up", "ecs.move_down"}, "Move the panel in that direction"},
};

/// @brief Most workspaces that have their own functions: keys 1 to 9 and 0.
#define OPENECS_WORKSPACE_FUNCTION_COUNT 10

/// @brief The default keys after the core prefix, with the functions they run. The setting ecs.prefix_keys adds to them and changes them.
static const char *const ECSI_PREFIX_KEYS[][2] = {
    {"Left", "ecs.focus_left"},
    {"Right", "ecs.focus_right"},
    {"Up", "ecs.focus_up"},
    {"Down", "ecs.focus_down"},
    {"Shift+Left", "ecs.move_left"},
    {"Shift+Right", "ecs.move_right"},
    {"Shift+Up", "ecs.move_up"},
    {"Shift+Down", "ecs.move_down"},
    {"Tab", "ecs.next_tab"},
    {"M", "ecs.maximize"},
    {"X", "ecs.close"},
    {"Shift+X", "ecs.close_group"},
    {"L", "ecs.lock"},
    {"1", "ecs.workspace_1"},
    {"2", "ecs.workspace_2"},
    {"3", "ecs.workspace_3"},
    {"4", "ecs.workspace_4"},
    {"5", "ecs.workspace_5"},
    {"6", "ecs.workspace_6"},
    {"7", "ecs.workspace_7"},
    {"8", "ecs.workspace_8"},
    {"9", "ecs.workspace_9"},
    {"0", "ecs.workspace_10"},
    {"T", "ecs.reopen"},
    {"R", "ecs.restart"},
};

/// @brief A key combination and the function it runs.
typedef struct ECSI_KeyBinding
{
    u32 key;
    u32 modifiers;
    char *text;     // the combination as written
    char *function; // name of the function
} ECSI_KeyBinding;

/// @brief A plugin's binding for its panel type. The key is the value of a key setting, so the user can change it.
typedef struct ECSI_PanelBinding
{
    ECSPlugin plugin;
    char *panelType;
    char *setting;
    char *function;
} ECSI_PanelBinding;

/// @brief How specific a binding is; a more specific binding wins within one settings layer.
typedef enum ECSI_BindingScope
{
    ECSI_BindingScope_Tool = 0,
    ECSI_BindingScope_Workspace,
    ECSI_BindingScope_PanelType,
} ECSI_BindingScope;

/// @brief The binding that a key press runs, found by ECSI_InputFindBinding.
typedef struct ECSI_BindingSearch
{
    u32 key;
    u32 modifiers;
    ECSI_SettingsLayer layer; // of the table being searched
    ECSI_BindingScope scope;  // of the table being searched
    const char *function;     // the best binding so far, or NULL
    ECSI_SettingsLayer bestLayer;
    ECSI_BindingScope bestScope;
} ECSI_BindingSearch;

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
    u32 prefixKey;
    u32 prefixModifiers;
    bool prefixDirty; // ecs.prefix changed and is read again at the next key press
    bool prefixActive;
    ECSI_KeyBinding *prefixKeys;      // stb_ds array of the keys after the prefix
    bool prefixKeysDirty;             // ecs.prefix_keys changed and is read again at the next key press
    const char **prefixLines;         // stb_ds array of the lines shown after the prefix: key text, description, and so on; a NULL key text makes a heading
    char **prefixTexts;               // stb_ds array of the key texts made for the lines, such as "1...0"
    ECSI_PanelBinding *panelBindings; // stb_ds array of plugins' bindings for their panel types
    ECSValue *toolKeys;               // the preset's bindings for the whole tool, or NULL
    ECSValue **workspaceKeys;         // stb_ds array of the preset's bindings for each workspace; NULL for none
    ECSPanel pointerPanel;            // panel that got the press; it gets pointer events until the release
    ECSI_MenuEntry *menu;             // stb_ds array of the panel menu's entries
    ECSI_MenuLevel *menuLevels;       // stb_ds array of the open menus, the panel menu first; while any is open, it gets the pointer and the keys
    void *clipboard;                  // what a clipboard getter returned last, freed by the next call
} INPUT = {0};

/// @brief A file dialog waiting for its answer, with copies of everything SDL reads until it answers.
typedef struct ECSI_Dialog
{
    ECSPlugin plugin;
    ECSDialogDoneFunction Done;
    void *data;
    SDL_DialogFileFilter *filters; // stb_ds array
    char **texts;                  // stb_ds array of the copied texts: filter names and patterns, and the location
    char **files;                  // stb_ds array of the answer
    bool failed;                   // a text could not be copied
} ECSI_Dialog;

/// @brief Typed data that the core offers on the clipboard.
typedef struct ECSI_ClipboardData
{
    char *mimeType;
    usz size;
    u8 bytes[]; // the data
} ECSI_ClipboardData;

/// @brief Converts SDL's modifier bits to ECSModifier bits.
static u32 ECSI_InputModifiers(SDL_Keymod modifiers)
{
    u32 result = ECSModifier_None;

    if (modifiers & SDL_KMOD_SHIFT)
    {
        result |= ECSModifier_Shift;
    }

    if (modifiers & SDL_KMOD_CTRL)
    {
        result |= ECSModifier_Ctrl;
    }

    if (modifiers & SDL_KMOD_ALT)
    {
        result |= ECSModifier_Alt;
    }

    if (modifiers & SDL_KMOD_GUI)
    {
        result |= ECSModifier_Super;
    }

    if (modifiers & SDL_KMOD_MODE)
    {
        result |= ECSModifier_AltGr;
    }

    return result;
}

/// @brief Reads the core prefix from the setting ecs.prefix if the setting changed. A key text that cannot be read is reported, and the default is used.
static void ECSI_InputReadPrefix(void)
{
    if (!INPUT.prefixDirty)
    {
        return;
    }

    INPUT.prefixDirty = false;

    if (ECSI_InputParseKey(ECSValue_GetString(ECSSetting_Get("ecs.prefix"), OPENECS_DEFAULT_PREFIX), true, &INPUT.prefixKey, &INPUT.prefixModifiers))
    {
        SHUResult result = ECSI_InputParseKey(OPENECS_DEFAULT_PREFIX, true, &INPUT.prefixKey, &INPUT.prefixModifiers);
        SDL_assert(result == SHUResult_Ok);
        (void)result;
    }
}

static void ECSI_InputFreeBindings(ECSI_KeyBinding **bindings)
{
    for (usz i = 0; i < arrlenu(*bindings); i++)
    {
        SDL_free((*bindings)[i].text);
        SDL_free((*bindings)[i].function);
    }

    arrfree(*bindings);
}

/// @brief Adds a binding to a list, or changes the binding of the same combination. A function name of NULL removes it. A key text that cannot be read is reported and skipped.
static void ECSI_InputPutBinding(ECSI_KeyBinding **bindings, const char *text, const char *function)
{
    u32 key = 0;
    u32 modifiers = 0;

    if (ECSI_InputParseKey(text, true, &key, &modifiers))
    {
        return;
    }

    for (usz i = 0; i < arrlenu(*bindings); i++)
    {
        ECSI_KeyBinding *binding = &(*bindings)[i];

        if (binding->key != key || binding->modifiers != modifiers)
        {
            continue;
        }

        char *copy = function == NULL ? NULL : SDL_strdup(function);
        SDL_free(binding->function);
        SDL_free(binding->text);
        binding->function = copy;
        binding->text = SDL_strdup(text);

        if (copy == NULL || binding->text == NULL)
        {
            SDL_free(binding->function);
            SDL_free(binding->text);
            arrdel(*bindings, i);
        }

        return;
    }

    ECSI_KeyBinding binding = {.key = key, .modifiers = modifiers, .text = SDL_strdup(text), .function = function == NULL ? NULL : SDL_strdup(function)};

    if (binding.text == NULL || binding.function == NULL)
    {
        SDL_free(binding.text);
        SDL_free(binding.function);
        return;
    }

    arrput(*bindings, binding);
}

/// @brief Adds the entries of the setting ecs.prefix_keys: key texts to function names, or false to remove a key.
static void ECSI_InputAddPrefixKey(const char *name, const ECSValue *field, void *userData)
{
    (void)userData;
    ECSI_InputPutBinding(&INPUT.prefixKeys, name, ECSValue_GetString(field, NULL));
}

/// @brief Reads the keys after the prefix: the defaults, then the setting ecs.prefix_keys, if the setting changed.
static void ECSI_InputReadPrefixKeys(void)
{
    if (!INPUT.prefixKeysDirty)
    {
        return;
    }

    INPUT.prefixKeysDirty = false;
    ECSI_InputFreeBindings(&INPUT.prefixKeys);

    for (usz i = 0; i < SDL_arraysize(ECSI_PREFIX_KEYS); i++)
    {
        ECSI_InputPutBinding(&INPUT.prefixKeys, ECSI_PREFIX_KEYS[i][0], ECSI_PREFIX_KEYS[i][1]);
    }

    ECSI_ValueTableForEachField(ECSSetting_Get("ecs.prefix_keys"), ECSI_InputAddPrefixKey, NULL);
}

/// @brief Keeps a binding that matches a key press, if it wins over the best one so far: a higher layer wins, then a more specific scope.
static void ECSI_InputConsiderBinding(ECSI_BindingSearch *search, const char *text, const char *function)
{
    u32 key = 0;
    u32 modifiers = 0;

    if (function == NULL || ECSI_InputParseKey(text, false, &key, &modifiers) || key != search->key || modifiers != search->modifiers)
    {
        return;
    }

    if (search->function == NULL || search->layer > search->bestLayer || (search->layer == search->bestLayer && search->scope > search->bestScope))
    {
        search->function = function;
        search->bestLayer = search->layer;
        search->bestScope = search->scope;
    }
}

static void ECSI_InputConsiderField(const char *name, const ECSValue *field, void *userData)
{
    ECSI_InputConsiderBinding(userData, name, ECSValue_GetString(field, NULL));
}

/// @brief Considers every binding of a table of key texts and function names.
static void ECSI_InputConsiderTable(ECSI_BindingSearch *search, const ECSValue *keys, ECSI_SettingsLayer layer, ECSI_BindingScope scope)
{
    search->layer = layer;
    search->scope = scope;
    ECSI_ValueTableForEachField(keys, ECSI_InputConsiderField, search);
}

/// @brief Finds the function that a key press runs: the binding of the highest settings layer, and within it the most specific one.
/// @return The function's name, or NULL if no binding matches.
static const char *ECSI_InputFindBinding(u32 key, u32 modifiers, ECSPanel focus)
{
    ECSI_BindingSearch search = {.key = key, .modifiers = modifiers};
    usz workspace = ECSI_LayoutGetCurrentWorkspace();

    ECSI_InputConsiderTable(&search, ECSI_SettingsGetKeys(ECSI_SettingsLayer_User), ECSI_SettingsLayer_User, ECSI_BindingScope_Tool);
    ECSI_InputConsiderTable(&search, ECSI_SettingsGetKeys(ECSI_SettingsLayer_Window), ECSI_SettingsLayer_Window, ECSI_BindingScope_Tool);
    ECSI_InputConsiderTable(&search, INPUT.toolKeys, ECSI_SettingsLayer_Preset, ECSI_BindingScope_Tool);

    if (workspace < arrlenu(INPUT.workspaceKeys))
    {
        ECSI_InputConsiderTable(&search, INPUT.workspaceKeys[workspace], ECSI_SettingsLayer_Preset, ECSI_BindingScope_Workspace);
    }

    // a plugin's binding counts in the layer that sets its key setting
    for (usz i = 0; focus != NULL && i < arrlenu(INPUT.panelBindings); i++)
    {
        ECSI_PanelBinding *binding = &INPUT.panelBindings[i];
        ECSPlugin owner = NULL;
        ECSSettingType type = ECSSettingType_Key;

        if (SDL_strcmp(binding->panelType, focus->typeName) == 0 && ECSI_SettingsDescribe(binding->setting, &owner, &type, &search.layer))
        {
            search.scope = ECSI_BindingScope_PanelType;
            ECSI_InputConsiderBinding(&search, ECSValue_GetString(ECSSetting_Get(binding->setting), ""), binding->function);
        }
    }

    return search.function;
}

static void ECSI_InputCheckKey(const char *name, const ECSValue *field, void *userData)
{
    (void)field;
    (void)userData;
    u32 key = 0;
    u32 modifiers = 0;
    (void)ECSI_InputParseKey(name, true, &key, &modifiers);
}

/// @brief Reports the key texts of a table of bindings that are not key combinations; they never match.
static void ECSI_InputCheckKeys(const ECSValue *keys)
{
    ECSI_ValueTableForEachField(keys, ECSI_InputCheckKey, NULL);
}

static void ECSI_InputFreePanelBinding(ECSI_PanelBinding *binding)
{
    SDL_free(binding->panelType);
    SDL_free(binding->setting);
    SDL_free(binding->function);
}

/// @brief Marks a setting to be read again; given as the Changed function of the input settings.
static void ECSI_InputSettingChanged(void *data)
{
    *(bool *)data = true;
}

/// @brief Sends a pointer event to a panel, with the position made relative to the panel.
static void ECSI_InputSendPointer(ECSPanel panel, ECSPanelEventType type, f32 x, f32 y, i32 button)
{
    ECSPanelEvent event = {
        .type = type,
        .modifiers = ECSI_InputModifiers(SDL_GetModState()),
        .pointer = {.x = x - panel->x, .y = y - panel->y, .button = button},
    };

    ECSI_PanelPostEvent(panel, &event);
}

/// @brief Sends a wheel event to a panel. The amount is turned back if the system flips the wheel, so positive is always away from the user.
static void ECSI_InputSendWheel(ECSPanel panel, const SDL_MouseWheelEvent *wheel)
{
    f32 direction = wheel->direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f;
    ECSPanelEvent event = {
        .type = ECSPanelEventType_Wheel,
        .modifiers = ECSI_InputModifiers(SDL_GetModState()),
        .wheel = {.x = wheel->mouse_x - panel->x, .y = wheel->mouse_y - panel->y, .amountX = wheel->x * direction, .amountY = wheel->y * direction},
    };

    ECSI_PanelPostEvent(panel, &event);
}

static void ECSI_InputSendKey(ECSPanel panel, ECSPanelEventType type, const SDL_KeyboardEvent *key)
{
    ECSPanelEvent event = {
        .type = type,
        .modifiers = ECSI_InputModifiers(key->mod),
        .key = {.code = key->key},
    };

    ECSI_PanelPostEvent(panel, &event);
}

/// @brief Moves the keyboard focus to a panel; the layout tells both panels.
static void ECSI_InputFocus(ECSPanel panel)
{
    if (panel != NULL)
    {
        ECSI_LayoutSetFocus(panel);
    }
}

/// @brief Makes the text of the keys that run a function after the prefix, such as "Alt+W, X".
/// @return The text, or NULL if no key after the prefix runs the function. Free it with SDL_free.
static char *ECSI_InputPrefixKeysOf(const char *function, const char *prefix)
{
    char *keys = NULL;

    for (usz i = 0; keys == NULL && i < arrlenu(INPUT.prefixKeys); i++)
    {
        if (SDL_strcmp(INPUT.prefixKeys[i].function, function) == 0 && SDL_asprintf(&keys, "%s, %s", prefix, INPUT.prefixKeys[i].text) < 0)
        {
            keys = NULL;
        }
    }

    return keys;
}

/// @brief Checks whether a core function can act on a panel now, so menus and the list of prefix keys offer it.
/// @param panel The focused panel, or NULL.
static bool ECSI_InputOffers(const char *function, ECSPanel panel)
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

/// @brief Gets the label of a function: what it does to a panel now, or its description.
static const char *ECSI_InputLabelOf(const char *function, ECSPanel panel)
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

static void ECSI_InputFreeMenuEntries(ECSI_MenuEntry **entries)
{
    for (usz i = 0; i < arrlenu(*entries); i++)
    {
        SDL_free((*entries)[i].label);
        SDL_free((*entries)[i].keys);
        SDL_free((*entries)[i].function);
        ECSI_InputFreeMenuEntries(&(*entries)[i].submenu);
    }

    arrfree(*entries);
}

/// @brief Closes the menus from a level on. Level 0 closes the panel menu and frees its entries.
static void ECSI_InputCloseMenus(usz level)
{
    for (usz i = level; i < arrlenu(INPUT.menuLevels); i++)
    {
        arrfree(INPUT.menuLevels[i].lines);
    }

    if (level < arrlenu(INPUT.menuLevels))
    {
        arrsetlen(INPUT.menuLevels, level);
        ECSI_LayoutShowMenu(level, (SDL_FRect){0}, NULL, 0);
    }

    if (level == 0)
    {
        arrfree(INPUT.menuLevels);
        ECSI_InputFreeMenuEntries(&INPUT.menu);
    }
}

/// @brief Adds an entry to a menu. The texts are copied.
/// @param keys Text of the keys that run it, made for the menu and taken over, or NULL.
/// @param function The function it runs, or NULL.
/// @return The entry, valid until the next entry is added.
static ECSI_MenuEntry *ECSI_InputMenuAdd(ECSI_MenuEntry **entries, const char *label, char *keys, const char *function)
{
    ECSI_MenuEntry entry = {.label = SDL_strdup(label), .keys = keys, .function = function == NULL ? NULL : SDL_strdup(function)};
    arrput(*entries, entry);
    return &arrlast(*entries);
}

/// @brief Adds an entry that runs a function, with the keys that run it after the prefix.
/// @param label Its label, or NULL for the function's label.
static void ECSI_InputMenuAddFunction(ECSI_MenuEntry **entries, const char *function, const char *label, ECSPanel panel, const char *prefix)
{
    if (ECSI_InputOffers(function, panel))
    {
        ECSI_InputMenuAdd(entries, label != NULL ? label : ECSI_InputLabelOf(function, panel), ECSI_InputPrefixKeysOf(function, prefix), function);
    }
}

/// @brief Adds an entry that opens a submenu, unless the submenu is empty. The submenu is taken over.
static void ECSI_InputMenuAddSubmenu(ECSI_MenuEntry **entries, const char *label, ECSI_MenuEntry *submenu)
{
    if (arrlenu(submenu) > 0)
    {
        ECSI_InputMenuAdd(entries, label, NULL, NULL)->submenu = submenu;
    }
}

/// @brief Shows a menu at the next level, with its first entry highlighted.
/// @param anchor The point the panel menu opens at, or the entry a submenu opens beside.
static void ECSI_InputOpenMenuLevel(ECSI_MenuEntry *entries, SDL_FRect anchor)
{
    ECSI_MenuLevel level = {.entries = entries};

    // an entry that opens a submenu shows an arrow instead of keys
    for (usz i = 0; i < arrlenu(entries); i++)
    {
        arrput(level.lines, entries[i].submenu != NULL ? OPENECS_SUBMENU_MARK : entries[i].keys != NULL ? entries[i].keys
                                                                                                        : "");
        arrput(level.lines, entries[i].label != NULL ? entries[i].label : "");
    }

    arrput(INPUT.menuLevels, level);
    ECSI_LayoutShowMenu(arrlenu(INPUT.menuLevels) - 1, anchor, level.lines, arrlenu(entries));
}

/// @brief Adds the entries of a panel's type, with the keys its plugin bound to the same functions for the type.
static void ECSI_InputMenuAddTypeEntries(ECSI_MenuEntry **entries, ECSPanel panel)
{
    usz count = 0;
    const char *const *functions = ECSI_PanelGetMenuEntries(panel, &count);

    for (usz i = 0; i < count; i++)
    {
        char *keys = NULL;

        for (usz j = 0; keys == NULL && j < arrlenu(INPUT.panelBindings); j++)
        {
            const ECSI_PanelBinding *binding = &INPUT.panelBindings[j];

            if (SDL_strcmp(binding->panelType, panel->typeName) == 0 && SDL_strcmp(binding->function, functions[i]) == 0)
            {
                const char *key = ECSValue_GetString(ECSSetting_Get(binding->setting), NULL);
                keys = key == NULL ? NULL : SDL_strdup(key);
            }
        }

        ECSI_InputMenuAdd(entries, ECSI_InputLabelOf(functions[i], panel), keys, functions[i]);
    }
}

/// @brief Builds a panel's menu: what acts on the panel and its group, the submenus that split and move it, then its type's entries.
static void ECSI_InputBuildPanelMenu(ECSI_MenuEntry **entries, ECSPanel panel, const char *prefix)
{
    ECSI_MenuEntry *split = NULL;
    ECSI_MenuEntry *move = NULL;
    ECSI_MenuEntry *workspaces = NULL;

    ECSI_InputMenuAddFunction(entries, "ecs.close", NULL, panel, prefix);
    ECSI_InputMenuAddFunction(entries, "ecs.restart", NULL, panel, prefix);
    ECSI_InputMenuAddFunction(entries, "ecs.maximize", NULL, panel, prefix);
    ECSI_InputMenuAddFunction(entries, "ecs.lock", NULL, panel, prefix);
    ECSI_InputMenuAddFunction(entries, "ecs.reopen", NULL, panel, prefix);

    ECSI_InputMenuAddFunction(&split, "ecs.split_right", "Right", panel, prefix);
    ECSI_InputMenuAddFunction(&split, "ecs.split_down", "Down", panel, prefix);
    ECSI_InputMenuAddSubmenu(entries, "Split", split);

    ECSI_InputMenuAddFunction(&move, "ecs.move_left", "Left", panel, prefix);
    ECSI_InputMenuAddFunction(&move, "ecs.move_right", "Right", panel, prefix);
    ECSI_InputMenuAddFunction(&move, "ecs.move_up", "Up", panel, prefix);
    ECSI_InputMenuAddFunction(&move, "ecs.move_down", "Down", panel, prefix);
    ECSI_InputMenuAddSubmenu(entries, "Move", move);

    // one entry for each other workspace, by number and name
    for (usz number = 1; number <= ECSWorkspace_GetCount() && number <= OPENECS_WORKSPACE_FUNCTION_COUNT; number++)
    {
        char function[32];
        char label[256];

        if (number != ECSWorkspace_GetCurrent())
        {
            SDL_snprintf(function, sizeof(function), "ecs.move_to_workspace_%zu", number);
            SDL_snprintf(label, sizeof(label), "%zu: %s", number, ECSWorkspace_GetName(number));
            ECSI_InputMenuAddFunction(&workspaces, function, label, panel, prefix);
        }
    }

    ECSI_InputMenuAddSubmenu(entries, "Move to workspace", workspaces);
    ECSI_InputMenuAddTypeEntries(entries, panel);
}

/// @brief Builds a group's menu: its tabs, then what acts on the whole group.
static void ECSI_InputBuildGroupMenu(ECSI_MenuEntry **entries, ECSPanel shownPanel, const char *prefix)
{
    ECSI_MenuEntry *tabs = NULL;
    usz count = 0;
    usz shown = 0;
    const ECSPanel *panels = ECSI_LayoutGetGroup(shownPanel, &count, &shown);

    for (usz i = 0; i < count; i++)
    {
        ECSI_InputMenuAdd(&tabs, panels[i]->title, i == shown ? SDL_strdup(OPENECS_SHOWN_MARK) : NULL, NULL)->tab = panels[i];
    }

    ECSI_InputMenuAddSubmenu(entries, "Tabs", tabs);
    ECSI_InputMenuAddFunction(entries, "ecs.maximize", NULL, shownPanel, prefix);
    ECSI_InputMenuAddFunction(entries, "ecs.lock", NULL, shownPanel, prefix);
    ECSI_InputMenuAddFunction(entries, "ecs.close_group", NULL, shownPanel, prefix);
    ECSI_InputMenuAddFunction(entries, "ecs.reopen", NULL, shownPanel, prefix);
}

/// @brief Opens a panel's menu, or the menu of the panel's group, at a point. The panel gets the focus.
static void ECSI_InputOpenMenu(ECSPanel panel, f32 x, f32 y, bool group)
{
    ECSI_InputCloseMenus(0);
    ECSI_LayoutSetFocus(panel);
    ECSI_InputReadPrefixKeys();
    const char *prefix = ECSValue_GetString(ECSSetting_Get("ecs.prefix"), OPENECS_DEFAULT_PREFIX);

    if (group)
    {
        ECSI_InputBuildGroupMenu(&INPUT.menu, panel, prefix);
    }
    else
    {
        ECSI_InputBuildPanelMenu(&INPUT.menu, panel, prefix);
    }

    ECSI_InputOpenMenuLevel(INPUT.menu, (SDL_FRect){x, y, 0.0f, 0.0f});
}

/// @brief Highlights an entry, and closes the submenus of the other entries.
/// @param open Also opens the entry's submenu, if it has one.
static void ECSI_InputSelectMenuItem(usz level, usz index, bool open)
{
    ECSI_InputCloseMenus(level + 1);
    INPUT.menuLevels[level].selected = index;
    ECSI_LayoutSelectMenuItem(level, index);

    const ECSI_MenuEntry *entry = &INPUT.menuLevels[level].entries[index];

    if (open && entry->submenu != NULL)
    {
        ECSI_InputOpenMenuLevel(entry->submenu, ECSI_LayoutMenuItemRect(level, index));
    }
}

/// @brief Uses an entry: opens its submenu, shows its tab, or closes the menus and runs its function on the focused panel.
static void ECSI_InputRunMenuItem(usz level, usz index)
{
    const ECSI_MenuEntry *entry = &INPUT.menuLevels[level].entries[index];

    if (entry->submenu != NULL)
    {
        ECSI_InputSelectMenuItem(level, index, true);
        return;
    }

    // closing the menus frees the entry, so what it does is copied first
    ECSPanel tab = entry->tab;
    char *function = entry->function == NULL ? NULL : SDL_strdup(entry->function);
    ECSI_InputCloseMenus(0);

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

/// @brief Gives an event to the open menus. Pointer and key events go to the menus only; keys go to the deepest one.
/// @return true if the menus used the event.
static bool ECSI_InputMenuHandle(const SDL_Event *event)
{
    if (arrlenu(INPUT.menuLevels) == 0)
    {
        return false;
    }

    usz deepest = arrlenu(INPUT.menuLevels) - 1;
    usz count = arrlenu(INPUT.menuLevels[deepest].entries);
    usz selected = INPUT.menuLevels[deepest].selected;
    usz level = 0;
    usz index = 0;

    switch (event->type)
    {
    case SDL_EVENT_MOUSE_MOTION:
        // pointing at an entry highlights it and opens its submenu
        if (ECSI_LayoutMenuItemAt(event->motion.x, event->motion.y, &level, &index))
        {
            bool hasSubmenu = INPUT.menuLevels[level].entries[index].submenu != NULL;

            if (INPUT.menuLevels[level].selected != index || arrlenu(INPUT.menuLevels) != level + (hasSubmenu ? 2 : 1))
            {
                ECSI_InputSelectMenuItem(level, index, true);
            }
        }

        return true;

    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        // a press outside the menus only closes them
        if (ECSI_LayoutMenuItemAt(event->button.x, event->button.y, &level, &index))
        {
            if (event->button.button == SDL_BUTTON_LEFT)
            {
                ECSI_InputRunMenuItem(level, index);
            }
        }
        else if (!ECSI_LayoutMenuContains(event->button.x, event->button.y))
        {
            ECSI_InputCloseMenus(0);
        }

        return true;

    case SDL_EVENT_KEY_DOWN:
        switch (event->key.key)
        {
        case SDLK_UP:
            ECSI_InputSelectMenuItem(deepest, (selected + count - 1) % count, false);
            break;
        case SDLK_DOWN:
            ECSI_InputSelectMenuItem(deepest, (selected + 1) % count, false);
            break;
        case SDLK_RIGHT:
            if (INPUT.menuLevels[deepest].entries[selected].submenu != NULL)
            {
                ECSI_InputRunMenuItem(deepest, selected);
            }
            break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_SPACE:
            ECSI_InputRunMenuItem(deepest, selected);
            break;
        case SDLK_LEFT:
            if (deepest > 0)
            {
                ECSI_InputCloseMenus(deepest);
            }
            break;
        case SDLK_ESCAPE:
            ECSI_InputCloseMenus(deepest);
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
        ECSI_InputCloseMenus(0);
        return false;

    default:
        return false;
    }
}

/// @brief Gets the panel that got the pointer press, unless code closed it since.
static ECSPanel ECSI_InputPointerPanel(void)
{
    if (INPUT.pointerPanel != NULL && !ECSI_LayoutHasPanel(INPUT.pointerPanel))
    {
        INPUT.pointerPanel = NULL;
    }

    return INPUT.pointerPanel;
}

/// @brief Finds the section of the list of prefix keys that a function is listed in.
static usz ECSI_InputSectionOf(const char *function)
{
    const char *navigation[] = {"ecs.focus_", "ecs.move_", "ecs.next_tab", OPENECS_WORKSPACE_FUNCTION};

    for (usz i = 0; i < SDL_arraysize(navigation); i++)
    {
        if (SDL_strncmp(function, navigation[i], SDL_strlen(navigation[i])) == 0)
        {
            return 0;
        }
    }

    return SDL_strncmp(function, "ecs.", 4) == 0 ? 1 : 2;
}

/// @brief Finds the prefix key that runs a function with a key and modifiers, and is not listed yet.
/// @return Its position, or -1.
static i64 ECSI_InputFindPrefixKey(const char *function, u32 key, u32 modifiers, const bool *listed)
{
    for (usz i = 0; i < arrlenu(INPUT.prefixKeys); i++)
    {
        const ECSI_KeyBinding *binding = &INPUT.prefixKeys[i];

        if (!listed[i] && binding->key == key && binding->modifiers == modifiers && SDL_strcmp(binding->function, function) == 0)
        {
            return (i64)i;
        }
    }

    return -1;
}

/// @brief Folds a prefix key into one line with its family, when the family's four functions run on the four arrows with the same modifiers.
/// @return The line's key text, such as "Shift+Arrows", made for the list; or NULL if the key does not fold.
static char *ECSI_InputFoldArrows(usz index, bool *listed, const char **retDescription)
{
    const ECSI_KeyBinding *binding = &INPUT.prefixKeys[index];
    const SDL_Keycode arrows[] = {SDLK_LEFT, SDLK_RIGHT, SDLK_UP, SDLK_DOWN};

    for (usz family = 0; family < SDL_arraysize(ECSI_ARROW_FAMILIES); family++)
    {
        i64 members[4] = {0};
        bool complete = true;

        for (usz i = 0; i < 4; i++)
        {
            members[i] = ECSI_InputFindPrefixKey(ECSI_ARROW_FAMILIES[family].functions[i], arrows[i], binding->modifiers, listed);
            complete = complete && members[i] >= 0;
        }

        if (!complete || (members[0] != (i64)index && members[1] != (i64)index && members[2] != (i64)index && members[3] != (i64)index))
        {
            continue;
        }

        // the modifiers keep the user's spelling: the text before the key's name
        const char *plus = SDL_strrchr(binding->text, '+');
        int modifiersLength = plus == NULL ? 0 : (int)(plus - binding->text + 1);
        char *text = NULL;

        if (SDL_asprintf(&text, "%.*sArrows", modifiersLength, binding->text) < 0)
        {
            return NULL;
        }

        for (usz i = 0; i < 4; i++)
        {
            listed[members[i]] = true;
        }

        *retDescription = ECSI_ARROW_FAMILIES[family].description;
        return text;
    }

    return NULL;
}

/// @brief Folds every key that switches workspaces into one line, from the first workspace's key to the last one's, such as "1...0".
/// @return The line's key text, made for the list.
static char *ECSI_InputFoldWorkspaces(bool *listed)
{
    const char *firstKey = NULL;
    const char *lastKey = NULL;
    i64 first = 0;
    i64 last = 0;

    for (usz i = 0; i < arrlenu(INPUT.prefixKeys); i++)
    {
        const char *function = INPUT.prefixKeys[i].function;

        if (SDL_strncmp(function, OPENECS_WORKSPACE_FUNCTION, SDL_strlen(OPENECS_WORKSPACE_FUNCTION)) != 0)
        {
            continue;
        }

        i64 number = SDL_strtoll(function + SDL_strlen(OPENECS_WORKSPACE_FUNCTION), NULL, 10);
        listed[i] = true;

        if (firstKey == NULL || number < first)
        {
            firstKey = INPUT.prefixKeys[i].text;
            first = number;
        }

        if (lastKey == NULL || number > last)
        {
            lastKey = INPUT.prefixKeys[i].text;
            last = number;
        }
    }

    char *text = NULL;

    if (first == last)
    {
        return SDL_strdup(firstKey);
    }

    return SDL_asprintf(&text, "%s...%s", firstKey, lastKey) < 0 ? NULL : text;
}

/// @brief Starts or ends waiting for the key after the prefix, and shows or hides the keys with what they do now, in sections.
static void ECSI_InputSetPrefix(bool active)
{
    INPUT.prefixActive = active;
    arrfree(INPUT.prefixLines);

    for (usz i = 0; i < arrlenu(INPUT.prefixTexts); i++)
    {
        SDL_free(INPUT.prefixTexts[i]);
    }

    arrfree(INPUT.prefixTexts);

    if (active)
    {
        ECSI_InputReadPrefixKeys();
        ECSPanel focus = ECSI_LayoutGetFocus();
        bool *listed = NULL;
        arrsetlen(listed, arrlenu(INPUT.prefixKeys));
        SDL_memset(listed, 0, arrlenu(listed) * sizeof(bool));

        for (usz section = 0; section < SDL_arraysize(ECSI_PREFIX_SECTIONS); section++)
        {
            // a section without keys has no heading
            usz start = arrlenu(INPUT.prefixLines);
            arrput(INPUT.prefixLines, NULL);
            arrput(INPUT.prefixLines, ECSI_PREFIX_SECTIONS[section]);

            for (usz i = 0; i < arrlenu(INPUT.prefixKeys); i++)
            {
                const char *function = INPUT.prefixKeys[i].function;

                if (listed[i] || ECSI_InputSectionOf(function) != section || !ECSI_InputOffers(function, focus))
                {
                    continue;
                }

                const char *description = NULL;
                bool workspace = SDL_strncmp(function, OPENECS_WORKSPACE_FUNCTION, SDL_strlen(OPENECS_WORKSPACE_FUNCTION)) == 0;
                char *text = workspace ? ECSI_InputFoldWorkspaces(listed) : ECSI_InputFoldArrows(i, listed, &description);

                if (text != NULL)
                {
                    arrput(INPUT.prefixTexts, text);
                }

                arrput(INPUT.prefixLines, text != NULL ? text : INPUT.prefixKeys[i].text);
                arrput(INPUT.prefixLines, workspace ? "Switch to workspace" : description != NULL ? description
                                                                                                  : ECSI_InputLabelOf(function, focus));
            }

            if (arrlenu(INPUT.prefixLines) == start + 2)
            {
                arrsetlen(INPUT.prefixLines, start);
            }
        }

        arrfree(listed);
        arrput(INPUT.prefixLines, "Escape");
        arrput(INPUT.prefixLines, "Cancel");
    }

    ECSI_LayoutShowPrefixKeys(INPUT.prefixLines, arrlenu(INPUT.prefixLines) / 2);
}

/// @brief Checks the modifiers of a key press against a binding's. AltGr is never part of a binding.
static bool ECSI_InputModifiersMatch(u32 modifiers, u32 expected)
{
    return (modifiers & ECSModifier_AltGr) == 0 && modifiers == expected;
}

/// @brief Runs the function of the key pressed after the core prefix. Escape cancels.
static void ECSI_InputRunPrefixKey(SDL_Keycode key, u32 modifiers)
{
    ECSI_InputSetPrefix(false);

    for (usz i = 0; key != SDLK_ESCAPE && i < arrlenu(INPUT.prefixKeys); i++)
    {
        if (INPUT.prefixKeys[i].key == key && ECSI_InputModifiersMatch(modifiers, INPUT.prefixKeys[i].modifiers))
        {
            ECSI_ServicesCallBound(INPUT.prefixKeys[i].function, ECSI_LayoutGetFocus());
            return;
        }
    }
}

#pragma region Core Functions

static void ECSI_InputFocusLeft(void)
{
    ECSI_InputFocus(ECSI_LayoutFindNeighbour(-1, 0));
}

static void ECSI_InputFocusRight(void)
{
    ECSI_InputFocus(ECSI_LayoutFindNeighbour(1, 0));
}

static void ECSI_InputFocusUp(void)
{
    ECSI_InputFocus(ECSI_LayoutFindNeighbour(0, -1));
}

static void ECSI_InputFocusDown(void)
{
    ECSI_InputFocus(ECSI_LayoutFindNeighbour(0, 1));
}

static void ECSI_InputMoveLeft(void)
{
    ECSI_LayoutMoveFocus(-1, 0);
}

static void ECSI_InputMoveRight(void)
{
    ECSI_LayoutMoveFocus(1, 0);
}

static void ECSI_InputMoveUp(void)
{
    ECSI_LayoutMoveFocus(0, -1);
}

static void ECSI_InputMoveDown(void)
{
    ECSI_LayoutMoveFocus(0, 1);
}

static void ECSI_InputNextTab(void)
{
    ECSI_InputFocus(ECSI_LayoutNextTab());
}

static void ECSI_InputMaximize(void)
{
    ECSI_LayoutToggleMaximize();
}

static void ECSI_InputClose(void)
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

static void ECSI_InputCloseGroup(void)
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

static void ECSI_InputLock(void)
{
    ECSI_LayoutToggleLock();
}

static void ECSI_InputReopen(void)
{
    ECSI_LayoutReopen();
}

static void ECSI_InputRestart(void)
{
    ECSPanel focus = ECSI_LayoutGetFocus();

    if (focus != NULL && ECSI_PanelRestart(focus))
    {
        ECSI_LayoutRequestFrame();
    }
}

/// @brief Opens another panel of the focused panel's type beside it, for the type's plugin.
static void ECSI_InputSplit(ECSZone zone)
{
    ECSPanel focus = ECSI_LayoutGetFocus();
    ECSPanel opened = NULL;

    if (focus != NULL && focus->type != NULL && ECSLayout_Open(focus->type->plugin, &opened, focus->typeName, NULL, focus, zone))
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cannot open another '%s'.", focus->typeName);
    }
}

static void ECSI_InputSplitRight(void)
{
    ECSI_InputSplit(ECSZone_Right);
}

static void ECSI_InputSplitDown(void)
{
    ECSI_InputSplit(ECSZone_Bottom);
}

/// @brief Defines the core's two functions for a workspace number: switching to the workspace, and moving the focused panel to it.
#define ECSI_WorkspaceFunctions(number)                 \
    static void ECSI_InputWorkspace##number(void)       \
    {                                                   \
        ECSI_LayoutWorkspaceSwitch(number - 1);         \
    }                                                   \
                                                        \
    static void ECSI_InputMoveToWorkspace##number(void) \
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
    {"ecs.workspace_" #number, ECSI_InputWorkspace##number, "Switch to workspace " #number},                        \
    {                                                                                                               \
        "ecs.move_to_workspace_" #number, ECSI_InputMoveToWorkspace##number, "Move the panel to workspace " #number \
    }

/// @brief The core's bindable functions: name, function and description.
static const struct
{
    const char *name;
    ECSFunction Function;
    const char *description;
} ECSI_CORE_FUNCTIONS[] = {
    {"ecs.focus_left", ECSI_InputFocusLeft, "Focus the panel on the left"},
    {"ecs.focus_right", ECSI_InputFocusRight, "Focus the panel on the right"},
    {"ecs.focus_up", ECSI_InputFocusUp, "Focus the panel above"},
    {"ecs.focus_down", ECSI_InputFocusDown, "Focus the panel below"},
    {"ecs.move_left", ECSI_InputMoveLeft, "Move the panel to the left"},
    {"ecs.move_right", ECSI_InputMoveRight, "Move the panel to the right"},
    {"ecs.move_up", ECSI_InputMoveUp, "Move the panel up"},
    {"ecs.move_down", ECSI_InputMoveDown, "Move the panel down"},
    {"ecs.next_tab", ECSI_InputNextTab, "Show the next tab"},
    {"ecs.maximize", ECSI_InputMaximize, "Maximize or restore the group"},
    {"ecs.close", ECSI_InputClose, "Close the panel"},
    {"ecs.close_group", ECSI_InputCloseGroup, "Close the group's panels"},
    {"ecs.lock", ECSI_InputLock, "Lock or unlock the group"},
    {"ecs.reopen", ECSI_InputReopen, "Reopen the last closed panel"},
    {"ecs.restart", ECSI_InputRestart, "Restart the failed panel"},
    {"ecs.split_right", ECSI_InputSplitRight, "Open another one on the right"},
    {"ecs.split_down", ECSI_InputSplitDown, "Open another one below"},
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

static const void *ECSI_InputClipboardProvide(void *userData, const char *mimeType, size_t *retSize)
{
    ECSI_ClipboardData *data = userData;

    if (SDL_strcmp(mimeType, data->mimeType) != 0)
    {
        *retSize = 0;
        return NULL;
    }

    *retSize = data->size;
    return data->bytes;
}

static void ECSI_InputClipboardRelease(void *userData)
{
    ECSI_ClipboardData *data = userData;
    SDL_free(data->mimeType);
    SDL_free(data);
}

/// @brief Frees what a clipboard getter returned last.
static void ECSI_InputForgetClipboard(void)
{
    SDL_free(INPUT.clipboard);
    INPUT.clipboard = NULL;
}

static void ECSI_InputFreeDialog(ECSI_Dialog *dialog)
{
    for (usz i = 0; i < arrlenu(dialog->texts); i++)
    {
        SDL_free(dialog->texts[i]);
    }

    for (usz i = 0; i < arrlenu(dialog->files); i++)
    {
        SDL_free(dialog->files[i]);
    }

    arrfree(dialog->filters);
    arrfree(dialog->texts);
    arrfree(dialog->files);
    SDL_free(dialog);
}

/// @brief Copies a text that a dialog keeps until it answers.
static const char *ECSI_InputDialogText(ECSI_Dialog *dialog, const char *text)
{
    char *copy = text == NULL ? NULL : SDL_strdup(text);

    if (copy != NULL)
    {
        arrput(dialog->texts, copy);
    }

    dialog->failed = dialog->failed || (text != NULL && copy == NULL);
    return copy;
}

/// @brief Gives a dialog's answer to its plugin, on the main thread.
static void ECSI_InputDialogFinish(void *data)
{
    ECSI_Dialog *dialog = data;
    usz count = arrlenu(dialog->files);
    dialog->Done(dialog->data, count == 0 ? NULL : (const char *const *)dialog->files, count);
    ECSI_InputFreeDialog(dialog);
}

/// @brief Takes SDL's answer, maybe on another thread, and sends it to the main thread.
static void SDLCALL ECSI_InputDialogAnswer(void *userData, const char *const *files, int filter)
{
    (void)filter;
    ECSI_Dialog *dialog = userData;

    if (files == NULL)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "A file dialog of plugin '%s' failed: %s", ECSI_PluginGetName(dialog->plugin), SDL_GetError());
    }

    // a cancelled dialog gives an empty list, which reaches the plugin as NULL, like a failed one
    for (usz i = 0; files != NULL && files[i] != NULL; i++)
    {
        char *copy = SDL_strdup(files[i]);

        if (copy != NULL)
        {
            arrput(dialog->files, copy);
        }
    }

    if (ECS_RunOnMainThread(ECSI_InputDialogFinish, dialog))
    {
        ECSI_InputFreeDialog(dialog);
    }
}

#pragma endregion Source Only

SHUResult ECSI_InputParseKey(const char *text, bool report, u32 *retKey, u32 *retModifiers)
{
    SDL_assert(text != NULL);
    SDL_assert(retKey != NULL);
    SDL_assert(retModifiers != NULL);

    char *copy = SDL_strdup(text);

    if (copy == NULL)
    {
        return SHUResult_ErrAllocation;
    }

    *retKey = SDLK_UNKNOWN;
    *retModifiers = ECSModifier_None;

    char *save = NULL;

    for (char *part = SDL_strtok_r(copy, "+", &save); part != NULL; part = SDL_strtok_r(NULL, "+", &save))
    {
        if (SDL_strcasecmp(part, "Ctrl") == 0)
        {
            *retModifiers |= ECSModifier_Ctrl;
        }
        else if (SDL_strcasecmp(part, "Shift") == 0)
        {
            *retModifiers |= ECSModifier_Shift;
        }
        else if (SDL_strcasecmp(part, "Alt") == 0)
        {
            *retModifiers |= ECSModifier_Alt;
        }
        else if (SDL_strcasecmp(part, "Super") == 0)
        {
            *retModifiers |= ECSModifier_Super;
        }
        else
        {
            *retKey = SDL_GetKeyFromName(part);
        }
    }

    SDL_free(copy);

    if (*retKey == SDLK_UNKNOWN)
    {
        if (report)
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "'%s' is not a key combination.", text);
        }

        return SHUResult_ErrBadData;
    }

    return SHUResult_Ok;
}

SHUResult ECSI_InputInitialize(void)
{
    ECSSettingDesc prefix = {
        .name = "ecs.prefix",
        .type = ECSSettingType_Key,
        .description = "The key combination before a core action",
        .defaultString = OPENECS_DEFAULT_PREFIX,
        .Changed = ECSI_InputSettingChanged,
        .data = &INPUT.prefixDirty,
    };

    ECSSettingDesc prefixKeys = {
        .name = "ecs.prefix_keys",
        .type = ECSSettingType_Table,
        .description = "Keys after the prefix and the functions they run, added to the core's own; false removes a key",
        .Changed = ECSI_InputSettingChanged,
        .data = &INPUT.prefixKeysDirty,
    };

    ECSSettingDesc focus = {
        .name = "ecs.focus",
        .type = ECSSettingType_Choice,
        .description = "How focus follows the pointer: click or hover",
        .choices = ECSI_FOCUS_CHOICES,
    };

    SHU_ReturnResult(ECSI_SettingsDeclareCore(&prefix));
    SHU_ReturnResult(ECSI_SettingsDeclareCore(&prefixKeys));
    SHU_ReturnResult(ECSI_SettingsDeclareCore(&focus));

    for (usz i = 0; i < SDL_arraysize(ECSI_CORE_FUNCTIONS); i++)
    {
        SHU_ReturnResult(ECSI_ServicesRegisterCore(ECSI_CORE_FUNCTIONS[i].name, ECSI_CORE_FUNCTIONS[i].Function, "void()", ECSI_CORE_FUNCTIONS[i].description));
    }

    INPUT.prefixDirty = true;
    INPUT.prefixKeysDirty = true;
    ECSI_InputReadPrefix();
    ECSI_InputCheckKeys(ECSI_SettingsGetKeys(ECSI_SettingsLayer_Window));
    ECSI_InputCheckKeys(ECSI_SettingsGetKeys(ECSI_SettingsLayer_User));
    return SHUResult_Ok;
}

SHUResult ECSI_InputSetToolKeys(const ECSValue *keys)
{
    ECSValue_Destroy(&INPUT.toolKeys);
    ECSI_InputCheckKeys(keys);
    SHU_ReturnResult(ECSValue_Create(&INPUT.toolKeys));
    return ECSI_ValueCopy(INPUT.toolKeys, keys);
}

SHUResult ECSI_InputAddWorkspaceKeys(const ECSValue *keys)
{
    ECSValue *copy = NULL;

    if (keys != NULL)
    {
        ECSI_InputCheckKeys(keys);
        SHU_ReturnResult(ECSValue_Create(&copy));
        SHU_ReturnResult(ECSI_ValueCopy(copy, keys), ECSValue_Destroy(&copy););
    }

    arrput(INPUT.workspaceKeys, copy);
    return SHUResult_Ok;
}

const ECSValue *ECSI_InputGetWorkspaceKeys(usz index)
{
    return index < arrlenu(INPUT.workspaceKeys) ? INPUT.workspaceKeys[index] : NULL;
}

void ECSI_InputRemovePlugin(ECSPlugin plugin)
{
    SDL_assert(plugin != NULL);

    for (usz i = arrlenu(INPUT.panelBindings); i > 0; i--)
    {
        if (INPUT.panelBindings[i - 1].plugin == plugin)
        {
            ECSI_InputFreePanelBinding(&INPUT.panelBindings[i - 1]);
            arrdel(INPUT.panelBindings, i - 1);
        }
    }
}

void ECSI_InputTerminate(void)
{
    ECSI_InputCloseMenus(0);
    ECSI_InputFreeBindings(&INPUT.prefixKeys);

    for (usz i = 0; i < arrlenu(INPUT.panelBindings); i++)
    {
        ECSI_InputFreePanelBinding(&INPUT.panelBindings[i]);
    }

    for (usz i = 0; i < arrlenu(INPUT.workspaceKeys); i++)
    {
        ECSValue_Destroy(&INPUT.workspaceKeys[i]);
    }

    arrfree(INPUT.panelBindings);
    arrfree(INPUT.workspaceKeys);
    ECSI_InputForgetClipboard();
    ECSValue_Destroy(&INPUT.toolKeys);
    arrfree(INPUT.prefixLines);

    for (usz i = 0; i < arrlenu(INPUT.prefixTexts); i++)
    {
        SDL_free(INPUT.prefixTexts[i]);
    }

    arrfree(INPUT.prefixTexts);
    SDL_zero(INPUT);
}

bool ECSI_InputHandle(const SDL_Event *event)
{
    SDL_assert(event != NULL);

    if (ECSI_InputMenuHandle(event))
    {
        return true;
    }

    switch (event->type)
    {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
    {
        // the user may cancel quitting to keep unsaved work
        ECSPanel *panels = ECSI_LayoutGetPanels();
        bool quit = ECSI_PanelsConfirmClose(panels, arrlenu(panels), true);
        arrfree(panels);
        return !quit;
    }

    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_EXPOSED:
    case SDL_EVENT_WINDOW_SHOWN:
    case SDL_EVENT_WINDOW_RESTORED:
        ECSI_LayoutRequestFrame();
        break;

    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    {
        const SDL_MouseButtonEvent *button = &event->button;

        if (INPUT.prefixActive)
        {
            ECSI_InputSetPrefix(false);
        }

        // on a tab or grip, the middle button closes the panel and the right button opens its menu; on the rest of a tab row, the right button opens the group's menu
        ECSPanel tab = button->button == SDL_BUTTON_LEFT ? NULL : ECSI_LayoutTabAt(button->x, button->y);
        ECSPanel row = tab == NULL && button->button == SDL_BUTTON_RIGHT ? ECSI_LayoutTabRowAt(button->x, button->y) : NULL;

        if (row != NULL)
        {
            ECSI_InputOpenMenu(row, button->x, button->y, true);
            break;
        }

        if (tab != NULL)
        {
            if (button->button == SDL_BUTTON_RIGHT)
            {
                ECSI_InputOpenMenu(tab, button->x, button->y, false);
            }
            else if (button->button == SDL_BUTTON_MIDDLE && !ECSI_LayoutIsLocked(tab))
            {
                (void)ECSLayout_Close(tab);
            }

            break;
        }

        if (button->button == SDL_BUTTON_LEFT && ECSI_LayoutPointerDown(button->x, button->y))
        {
            break;
        }

        ECSPanel panel = ECSI_LayoutPanelAt(button->x, button->y);

        if (panel != NULL)
        {
            ECSI_InputFocus(panel);
            INPUT.pointerPanel = panel;

            ECSI_InputSendPointer(panel, ECSPanelEventType_PointerDown, button->x, button->y, button->button);
        }

        break;
    }

    case SDL_EVENT_MOUSE_MOTION:
    {
        const SDL_MouseMotionEvent *motion = &event->motion;

        if (ECSI_LayoutPointerMove(motion->x, motion->y))
        {
            break;
        }

        ECSPanel panel = ECSI_InputPointerPanel() != NULL ? INPUT.pointerPanel : ECSI_LayoutPanelAt(motion->x, motion->y);

        if (panel == NULL)
        {
            break;
        }

        // in hover mode, only real pointer movement over a panel moves focus; dividers and tab rows are outside every panel
        if (INPUT.pointerPanel == NULL && SDL_strcmp(ECSValue_GetString(ECSSetting_Get("ecs.focus"), ""), "hover") == 0)
        {
            ECSI_InputFocus(panel);
        }

        ECSI_InputSendPointer(panel, ECSPanelEventType_PointerMove, motion->x, motion->y, 0);

        break;
    }

    case SDL_EVENT_MOUSE_BUTTON_UP:
    {
        const SDL_MouseButtonEvent *button = &event->button;

        // a click on a grip opens the panel's menu
        ECSPanel clicked = button->button == SDL_BUTTON_LEFT ? ECSI_LayoutPointerUp() : NULL;

        if (clicked != NULL)
        {
            ECSI_InputOpenMenu(clicked, button->x, button->y, false);
        }

        if (ECSI_InputPointerPanel() != NULL)
        {
            ECSI_InputSendPointer(INPUT.pointerPanel, ECSPanelEventType_PointerUp, button->x, button->y, button->button);
            INPUT.pointerPanel = NULL;
        }

        break;
    }

    case SDL_EVENT_MOUSE_WHEEL:
    {
        const SDL_MouseWheelEvent *wheel = &event->wheel;

        // over a tab row, the wheel scrolls the tabs; down and right go toward the last tab
        f32 direction = wheel->direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f;

        if (ECSI_LayoutScrollTabs(wheel->mouse_x, wheel->mouse_y, (wheel->x - wheel->y) * direction))
        {
            break;
        }

        ECSPanel panel = ECSI_LayoutPanelAt(wheel->mouse_x, wheel->mouse_y);

        if (panel != NULL)
        {
            ECSI_InputSendWheel(panel, wheel);
        }

        break;
    }

    case SDL_EVENT_KEY_DOWN:
    {
        const SDL_KeyboardEvent *key = &event->key;

        if (INPUT.prefixActive)
        {
            bool modifierKey = (key->key >= SDLK_LCTRL && key->key <= SDLK_RGUI) || key->key == SDLK_MODE;

            if (!modifierKey && !key->repeat)
            {
                ECSI_InputRunPrefixKey(key->key, ECSI_InputModifiers(key->mod));
            }

            break;
        }

        if (key->key == SDLK_ESCAPE && ECSI_LayoutCancelDrag())
        {
            break;
        }

        // the prefix is read when a key is pressed, so a changed prefix always wins
        ECSI_InputReadPrefix();

        if (key->key == INPUT.prefixKey && ECSI_InputModifiersMatch(ECSI_InputModifiers(key->mod), INPUT.prefixModifiers))
        {
            ECSI_InputSetPrefix(true);
            break;
        }

        // a binding wins over the focused panel's own handling of the key
        ECSPanel focus = ECSI_LayoutGetFocus();
        bool modifierKey = (key->key >= SDLK_LCTRL && key->key <= SDLK_RGUI) || key->key == SDLK_MODE;
        u32 modifiers = ECSI_InputModifiers(key->mod);
        const char *function = modifierKey || (modifiers & ECSModifier_AltGr) != 0 ? NULL : ECSI_InputFindBinding(key->key, modifiers, focus);

        if (function != NULL)
        {
            ECSI_ServicesCallBound(function, focus);
        }
        else if (focus != NULL)
        {
            ECSI_InputSendKey(focus, ECSPanelEventType_KeyDown, key);
        }

        break;
    }

    case SDL_EVENT_KEY_UP:
    {
        ECSPanel focus = ECSI_LayoutGetFocus();

        if (focus != NULL && !INPUT.prefixActive)
        {
            ECSI_InputSendKey(focus, ECSPanelEventType_KeyUp, &event->key);
        }

        break;
    }

    default:
        break;
    }

    return true;
}

SHUResult ECSKey_Bind(ECSPlugin plugin, const char *panelType, const char *setting, const char *function)
{
    SDL_assert(plugin != NULL);
    SDL_assert(panelType != NULL && setting != NULL && function != NULL);

    ECSPlugin owner = NULL;
    ECSSettingType type = ECSSettingType_Bool;
    ECSI_SettingsLayer layer = ECSI_SettingsLayer_Core;

    // a plugin binds keys only for its own panel types, with its own key settings
    if (!ECSI_PluginOwnsName(plugin, panelType))
    {
        return SHUResult_ErrBadData;
    }

    if (!ECSI_SettingsDescribe(setting, &owner, &type, &layer) || owner != plugin || type != ECSSettingType_Key)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' binds a key with '%s', which is not one of its key settings.", ECSI_PluginGetName(plugin), setting);
        return SHUResult_ErrBadData;
    }

    u32 key = 0;
    u32 modifiers = 0;
    ECSI_InputReadPrefix();

    if (ECSI_InputParseKey(ECSValue_GetString(ECSSetting_Get(setting), ""), true, &key, &modifiers) == SHUResult_Ok && key == INPUT.prefixKey && modifiers == INPUT.prefixModifiers)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "The key of '%s' is the core prefix; the binding is never triggered.", setting);
    }

    ECSI_PanelBinding binding = {.plugin = plugin, .panelType = SDL_strdup(panelType), .setting = SDL_strdup(setting), .function = SDL_strdup(function)};

    if (binding.panelType == NULL || binding.setting == NULL || binding.function == NULL)
    {
        ECSI_InputFreePanelBinding(&binding);
        return SHUResult_ErrAllocation;
    }

    arrput(INPUT.panelBindings, binding);
    return SHUResult_Ok;
}

#pragma region Clipboard

SHUResult ECSClipboard_SetText(const char *text)
{
    SDL_assert(text != NULL);

    ECSI_InputForgetClipboard();

    if (!SDL_SetClipboardText(text))
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cannot set the clipboard: %s", SDL_GetError());
        return SHUResult_ErrInternal;
    }

    return SHUResult_Ok;
}

const char *ECSClipboard_GetText(void)
{
    ECSI_InputForgetClipboard();
    INPUT.clipboard = SDL_GetClipboardText();
    return INPUT.clipboard == NULL ? "" : INPUT.clipboard;
}

SHUResult ECSClipboard_SetData(const char *mimeType, SHUSliceView data)
{
    SDL_assert(mimeType != NULL);
    SDL_assert(data.data != NULL || data.size == 0);

    ECSI_InputForgetClipboard();
    ECSI_ClipboardData *copy = SDL_malloc(sizeof(ECSI_ClipboardData) + data.size);
    char *type = SDL_strdup(mimeType);

    if (copy == NULL || type == NULL)
    {
        SDL_free(copy);
        SDL_free(type);
        return SHUResult_ErrAllocation;
    }

    copy->mimeType = type;
    copy->size = data.size;
    SDL_memcpy(copy->bytes, data.data, data.size);

    // SDL asks for the data when another program pastes it, and releases it when the clipboard changes
    const char *types[] = {copy->mimeType};

    if (!SDL_SetClipboardData(ECSI_InputClipboardProvide, ECSI_InputClipboardRelease, copy, types, 1))
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cannot set the clipboard: %s", SDL_GetError());
        ECSI_InputClipboardRelease(copy);
        return SHUResult_ErrInternal;
    }

    return SHUResult_Ok;
}

SHUResult ECSClipboard_GetData(const char *mimeType, SHUSlice *retData)
{
    SDL_assert(mimeType != NULL);
    SDL_assert(retData != NULL);

    ECSI_InputForgetClipboard();
    usz size = 0;
    INPUT.clipboard = SDL_HasClipboardData(mimeType) ? SDL_GetClipboardData(mimeType, &size) : NULL;
    *retData = cs(INPUT.clipboard, INPUT.clipboard == NULL ? 0 : size);
    return INPUT.clipboard == NULL ? SHUResult_ErrNotFound : SHUResult_Ok;
}

#pragma endregion Clipboard

#pragma region Dialogs

SHUResult ECSDialog_Show(ECSPlugin plugin, const ECSDialogDesc *desc)
{
    SDL_assert(plugin != NULL);
    SDL_assert(desc != NULL && desc->Done != NULL);
    SDL_assert(desc->filters != NULL || desc->filterCount == 0);

    ECSI_Dialog *dialog = SDL_calloc(1, sizeof(ECSI_Dialog));

    if (dialog == NULL)
    {
        return SHUResult_ErrAllocation;
    }

    dialog->plugin = plugin;
    dialog->Done = desc->Done;
    dialog->data = desc->data;

    for (usz i = 0; i < desc->filterCount; i++)
    {
        SDL_DialogFileFilter filter = {ECSI_InputDialogText(dialog, desc->filters[i].name), ECSI_InputDialogText(dialog, desc->filters[i].pattern)};
        arrput(dialog->filters, filter);
    }

    const char *location = ECSI_InputDialogText(dialog, desc->location);

    if (dialog->failed)
    {
        ECSI_InputFreeDialog(dialog);
        return SHUResult_ErrAllocation;
    }

    SDL_Window *window = ECSI_LayoutGetWindow();
    int filterCount = (int)arrlenu(dialog->filters);

    switch (desc->type)
    {
    case ECSDialogType_OpenFile:
        SDL_ShowOpenFileDialog(ECSI_InputDialogAnswer, dialog, window, dialog->filters, filterCount, location, desc->many);
        break;
    case ECSDialogType_SaveFile:
        SDL_ShowSaveFileDialog(ECSI_InputDialogAnswer, dialog, window, dialog->filters, filterCount, location);
        break;
    case ECSDialogType_OpenFolder:
        SDL_ShowOpenFolderDialog(ECSI_InputDialogAnswer, dialog, window, location, desc->many);
        break;
    }

    return SHUResult_Ok;
}

SHUResult ECSDialog_ShowMessage(const char *title, const char *message, const char *const *buttons, usz buttonCount, usz *retButton)
{
    SDL_assert(title != NULL && message != NULL && retButton != NULL);
    SDL_assert(buttons != NULL && buttonCount > 0);

    SDL_MessageBoxButtonData *data = SDL_calloc(buttonCount, sizeof(SDL_MessageBoxButtonData));

    if (data == NULL)
    {
        return SHUResult_ErrAllocation;
    }

    for (usz i = 0; i < buttonCount; i++)
    {
        data[i] = (SDL_MessageBoxButtonData){.buttonID = (int)i, .text = buttons[i]};
        data[i].flags |= i == 0 ? SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT : 0;
        data[i].flags |= i + 1 == buttonCount ? SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT : 0;
    }

    const SDL_MessageBoxData box = {
        .flags = SDL_MESSAGEBOX_INFORMATION,
        .window = ECSI_LayoutGetWindow(),
        .title = title,
        .message = message,
        .numbuttons = (int)buttonCount,
        .buttons = data,
    };

    int button = -1;
    bool shown = SDL_ShowMessageBox(&box, &button);
    SDL_free(data);

    if (!shown)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cannot show a message dialog: %s", SDL_GetError());
        return SHUResult_ErrInternal;
    }

    *retButton = (usz)SDL_max(button, 0);
    return button < 0 ? SHUResult_ErrNotFound : SHUResult_Ok;
}

#pragma endregion Dialogs
