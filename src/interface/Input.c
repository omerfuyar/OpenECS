#include "interface/Input.h"

#include "interface/Layout.h"
#include "interface/Panels.h"
#include "runtime/Services.h"
#include "runtime/Settings.h"

#include "SDL3/SDL.h"
#include "stb/stbSDL3.h"

#pragma region Source Only

/// @brief Default of the setting ecs.prefix.
#define OPENECS_DEFAULT_PREFIX "Alt+W"

/// @brief Choices of the setting ecs.focus; the first is the default.
static const char *const ECSI_FOCUS_CHOICES[] = {"click", "hover", NULL};

/// @brief Core functions in every panel's menu. They act on the focused panel, which the menu's panel becomes.
static const char *const ECSI_MENU_FUNCTIONS[] = {"ecs.close", "ecs.maximize", "ecs.lock", "ecs.move_left", "ecs.move_right", "ecs.move_up", "ecs.move_down"};

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

static struct
{
    u32 prefixKey;
    u32 prefixModifiers;
    bool prefixDirty; // ecs.prefix changed and is read again at the next key press
    bool prefixActive;
    ECSI_KeyBinding *prefixKeys; // stb_ds array of the keys after the prefix
    bool prefixKeysDirty;        // ecs.prefix_keys changed and is read again at the next key press
    const char **prefixLines;    // stb_ds array of the lines shown after the prefix: key text, description, and so on
    ECSI_PanelBinding *panelBindings; // stb_ds array of plugins' bindings for their panel types
    ECSValue *toolKeys;               // the preset's bindings for the whole tool, or NULL
    ECSValue **workspaceKeys;         // stb_ds array of the preset's bindings for each workspace; NULL for none
    ECSPanel pointerPanel;            // panel that got the press; it gets pointer events until the release
    bool menuOpen;                    // a panel menu is shown, and gets the pointer and the keys
    const char **menuFunctions;       // stb_ds array of the menu's functions
    const char **menuLines;           // stb_ds array of the menu's lines: key text, label, and so on
    char **menuTexts;                 // stb_ds array of the key texts made for the menu
    usz menuSelected;
    f32 menuX;
    f32 menuY;
    void *clipboard;                  // what a clipboard getter returned last, freed by the next call
} INPUT = {0};

/// @brief A file dialog waiting for its answer, with copies of everything SDL reads until it answers.
typedef struct ECSI_Dialog
{
    ECSPlugin plugin;
    void (*Done)(void *data, const char *const *files, usz count);
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

/// @brief Reads a key combination written as text, such as "Ctrl+Shift+P". Key names are SDL's.
/// @param report true to report a text that is not a key combination.
static SHUResult ECSI_InputParseKey(const char *text, bool report, u32 *retKey, u32 *retModifiers)
{
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
static void ECSI_InputSendPointer(ECSPanel panel, ECSEventType type, f32 x, f32 y, i32 button)
{
    ECSEvent event = {
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
    ECSEvent event = {
        .type = ECSEventType_Wheel,
        .modifiers = ECSI_InputModifiers(SDL_GetModState()),
        .wheel = {.x = wheel->mouse_x - panel->x, .y = wheel->mouse_y - panel->y, .amountX = wheel->x * direction, .amountY = wheel->y * direction},
    };

    ECSI_PanelPostEvent(panel, &event);
}

static void ECSI_InputSendKey(ECSPanel panel, ECSEventType type, const SDL_KeyboardEvent *key)
{
    ECSEvent event = {
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

/// @brief Closes the panel menu, if it is open.
static void ECSI_InputCloseMenu(void)
{
    for (usz i = 0; i < arrlenu(INPUT.menuTexts); i++)
    {
        SDL_free(INPUT.menuTexts[i]);
    }

    arrfree(INPUT.menuTexts);
    arrfree(INPUT.menuLines);
    arrfree(INPUT.menuFunctions);

    if (INPUT.menuOpen)
    {
        INPUT.menuOpen = false;
        ECSI_LayoutShowMenu(0.0f, 0.0f, NULL, 0, 0);
    }
}

/// @brief Opens a panel's menu at a point. Each entry shows the keys that run its function after the prefix.
static void ECSI_InputOpenMenu(ECSPanel panel, f32 x, f32 y)
{
    ECSI_InputCloseMenu();
    ECSI_LayoutSetFocus(panel);
    ECSI_InputReadPrefixKeys();
    const char *prefix = ECSValue_GetString(ECSSetting_Get("ecs.prefix"), OPENECS_DEFAULT_PREFIX);

    for (usz i = 0; i < SDL_arraysize(ECSI_MENU_FUNCTIONS); i++)
    {
        const char *function = ECSI_MENU_FUNCTIONS[i];
        const char *description = ECSI_ServicesGetDescription(function);
        char *keys = NULL;

        for (usz j = 0; keys == NULL && j < arrlenu(INPUT.prefixKeys); j++)
        {
            if (SDL_strcmp(INPUT.prefixKeys[j].function, function) == 0 && SDL_asprintf(&keys, "%s, %s", prefix, INPUT.prefixKeys[j].text) < 0)
            {
                keys = NULL;
            }
        }

        if (keys != NULL)
        {
            arrput(INPUT.menuTexts, keys);
        }

        arrput(INPUT.menuFunctions, function);
        arrput(INPUT.menuLines, keys == NULL ? "" : keys);
        arrput(INPUT.menuLines, description == NULL ? function : description);
    }

    INPUT.menuOpen = true;
    INPUT.menuSelected = 0;
    INPUT.menuX = x;
    INPUT.menuY = y;
    ECSI_LayoutShowMenu(x, y, INPUT.menuLines, arrlenu(INPUT.menuFunctions), 0);
}

/// @brief Highlights a menu entry.
static void ECSI_InputSelectMenuItem(usz index)
{
    INPUT.menuSelected = index;
    ECSI_LayoutShowMenu(INPUT.menuX, INPUT.menuY, INPUT.menuLines, arrlenu(INPUT.menuFunctions), index);
}

/// @brief Closes the menu and runs the function of one of its entries on the focused panel.
static void ECSI_InputRunMenuItem(usz index)
{
    const char *function = INPUT.menuFunctions[index];
    ECSI_InputCloseMenu();
    ECSI_ServicesCallBound(function, ECSI_LayoutGetFocus());
}

/// @brief Gives an event to the open panel menu. Pointer and key events go to the menu only.
/// @return true if the menu used the event.
static bool ECSI_InputMenuHandle(const SDL_Event *event)
{
    if (!INPUT.menuOpen)
    {
        return false;
    }

    usz count = arrlenu(INPUT.menuFunctions);

    switch (event->type)
    {
    case SDL_EVENT_MOUSE_MOTION:
    {
        i32 item = ECSI_LayoutMenuItemAt(event->motion.x, event->motion.y);

        if (item >= 0 && (usz)item != INPUT.menuSelected)
        {
            ECSI_InputSelectMenuItem((usz)item);
        }

        return true;
    }

    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    {
        // a press outside the menu only closes it
        i32 item = ECSI_LayoutMenuItemAt(event->button.x, event->button.y);

        if (item >= 0 && event->button.button == SDL_BUTTON_LEFT)
        {
            ECSI_InputRunMenuItem((usz)item);
        }
        else if (!ECSI_LayoutMenuContains(event->button.x, event->button.y))
        {
            ECSI_InputCloseMenu();
        }

        return true;
    }

    case SDL_EVENT_KEY_DOWN:
        switch (event->key.key)
        {
        case SDLK_UP:
            ECSI_InputSelectMenuItem((INPUT.menuSelected + count - 1) % count);
            break;
        case SDLK_DOWN:
            ECSI_InputSelectMenuItem((INPUT.menuSelected + 1) % count);
            break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_SPACE:
            ECSI_InputRunMenuItem(INPUT.menuSelected);
            break;
        case SDLK_ESCAPE:
            ECSI_InputCloseMenu();
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
        ECSI_InputCloseMenu();
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

/// @brief Starts or ends waiting for the key after the prefix, and shows or hides the keys with what they do.
static void ECSI_InputSetPrefix(bool active)
{
    INPUT.prefixActive = active;
    arrfree(INPUT.prefixLines);

    if (active)
    {
        ECSI_InputReadPrefixKeys();

        for (usz i = 0; i < arrlenu(INPUT.prefixKeys); i++)
        {
            const char *description = ECSI_ServicesGetDescription(INPUT.prefixKeys[i].function);
            arrput(INPUT.prefixLines, INPUT.prefixKeys[i].text);
            arrput(INPUT.prefixLines, description == NULL ? INPUT.prefixKeys[i].function : description);
        }

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

static void ECSI_InputLock(void)
{
    ECSI_LayoutToggleLock();
}

static void ECSI_InputWorkspace1(void)
{
    ECSI_LayoutWorkspaceSwitch(0);
}

static void ECSI_InputWorkspace2(void)
{
    ECSI_LayoutWorkspaceSwitch(1);
}

static void ECSI_InputWorkspace3(void)
{
    ECSI_LayoutWorkspaceSwitch(2);
}

static void ECSI_InputWorkspace4(void)
{
    ECSI_LayoutWorkspaceSwitch(3);
}

static void ECSI_InputWorkspace5(void)
{
    ECSI_LayoutWorkspaceSwitch(4);
}

static void ECSI_InputWorkspace6(void)
{
    ECSI_LayoutWorkspaceSwitch(5);
}

static void ECSI_InputWorkspace7(void)
{
    ECSI_LayoutWorkspaceSwitch(6);
}

static void ECSI_InputWorkspace8(void)
{
    ECSI_LayoutWorkspaceSwitch(7);
}

static void ECSI_InputWorkspace9(void)
{
    ECSI_LayoutWorkspaceSwitch(8);
}

/// @brief The core's bindable functions: name, function and description.
static const struct
{
    const char *name;
    void (*Function)(void);
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
    {"ecs.lock", ECSI_InputLock, "Lock or unlock the group"},
    {"ecs.workspace_1", ECSI_InputWorkspace1, "Switch to workspace 1"},
    {"ecs.workspace_2", ECSI_InputWorkspace2, "Switch to workspace 2"},
    {"ecs.workspace_3", ECSI_InputWorkspace3, "Switch to workspace 3"},
    {"ecs.workspace_4", ECSI_InputWorkspace4, "Switch to workspace 4"},
    {"ecs.workspace_5", ECSI_InputWorkspace5, "Switch to workspace 5"},
    {"ecs.workspace_6", ECSI_InputWorkspace6, "Switch to workspace 6"},
    {"ecs.workspace_7", ECSI_InputWorkspace7, "Switch to workspace 7"},
    {"ecs.workspace_8", ECSI_InputWorkspace8, "Switch to workspace 8"},
    {"ecs.workspace_9", ECSI_InputWorkspace9, "Switch to workspace 9"},
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
        SHU_ReturnResult(ECSI_ServicesRegisterCore(ECSI_CORE_FUNCTIONS[i].name, (ECSFunction)ECSI_CORE_FUNCTIONS[i].Function, "void()", ECSI_CORE_FUNCTIONS[i].description));
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
    ECSI_InputCloseMenu();
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

        // on a tab or grip, the middle button closes the panel and the right button opens its menu
        ECSPanel tab = button->button == SDL_BUTTON_LEFT ? NULL : ECSI_LayoutTabAt(button->x, button->y);

        if (tab != NULL)
        {
            if (button->button == SDL_BUTTON_RIGHT)
            {
                ECSI_InputOpenMenu(tab, button->x, button->y);
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

            ECSI_InputSendPointer(panel, ECSEventType_PointerDown, button->x, button->y, button->button);
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

        ECSI_InputSendPointer(panel, ECSEventType_PointerMove, motion->x, motion->y, 0);

        break;
    }

    case SDL_EVENT_MOUSE_BUTTON_UP:
    {
        const SDL_MouseButtonEvent *button = &event->button;

        // a click on a grip opens the panel's menu
        ECSPanel clicked = button->button == SDL_BUTTON_LEFT ? ECSI_LayoutPointerUp() : NULL;

        if (clicked != NULL)
        {
            ECSI_InputOpenMenu(clicked, button->x, button->y);
        }

        if (ECSI_InputPointerPanel() != NULL)
        {
            ECSI_InputSendPointer(INPUT.pointerPanel, ECSEventType_PointerUp, button->x, button->y, button->button);
            INPUT.pointerPanel = NULL;
        }

        break;
    }

    case SDL_EVENT_MOUSE_WHEEL:
    {
        const SDL_MouseWheelEvent *wheel = &event->wheel;
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
            ECSI_InputSendKey(focus, ECSEventType_KeyDown, key);
        }

        break;
    }

    case SDL_EVENT_KEY_UP:
    {
        ECSPanel focus = ECSI_LayoutGetFocus();

        if (focus != NULL && !INPUT.prefixActive)
        {
            ECSI_InputSendKey(focus, ECSEventType_KeyUp, &event->key);
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
