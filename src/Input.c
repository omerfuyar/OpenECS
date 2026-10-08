#include "Input.h"

#include "Layout.h"
#include "Panels.h"
#include "Services.h"
#include "Settings.h"

#include "SDL3/SDL.h"
#include "stb/stbSDL3.h"

#pragma region Source Only

/// @brief Default of the setting ecs.prefix.
#define OPENECS_DEFAULT_PREFIX "Alt+W"

/// @brief Choices of the setting ecs.focus; the first is the default.
static const char *const ECSI_FOCUS_CHOICES[] = {"click", "hover", NULL};

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

static struct
{
    u32 prefixKey;
    u32 prefixModifiers;
    bool prefixDirty; // ecs.prefix changed and is read again at the next key press
    bool prefixActive;
    ECSI_KeyBinding *prefixKeys; // stb_ds array of the keys after the prefix
    bool prefixKeysDirty;        // ecs.prefix_keys changed and is read again at the next key press
    const char **prefixLines;    // stb_ds array of the lines shown after the prefix: key text, description, and so on
    ECSPanel pointerPanel;       // panel that got the press; it gets pointer events until the release
} INPUT = {0};

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
static SHUResult ECSI_InputParseKey(const char *text, u32 *retKey, u32 *retModifiers)
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
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "'%s' is not a key combination.", text);
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

    if (ECSI_InputParseKey(ECSValue_GetString(ECSSetting_Get("ecs.prefix"), OPENECS_DEFAULT_PREFIX), &INPUT.prefixKey, &INPUT.prefixModifiers))
    {
        SHUResult result = ECSI_InputParseKey(OPENECS_DEFAULT_PREFIX, &INPUT.prefixKey, &INPUT.prefixModifiers);
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

    if (ECSI_InputParseKey(text, &key, &modifiers))
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

    ECSI_ValueForEachField(ECSSetting_Get("ecs.prefix_keys"), ECSI_InputAddPrefixKey, NULL);
}

/// @brief Marks a setting to be read again; given as the Changed function of the input settings.
static void ECSI_InputSettingChanged(void *data)
{
    *(bool *)data = true;
}

/// @brief Sends a pointer event to a panel, with the position made relative to the panel.
static void ECSI_InputSendPointer(ECSPanel panel, ECSEventType type, f32 x, f32 y, const ECSEvent *details)
{
    ECSEvent event = details == NULL ? (ECSEvent){0} : *details;
    event.type = type;
    event.x = x - panel->x;
    event.y = y - panel->y;

    ECSI_PanelPostEvent(panel, &event);
}

static void ECSI_InputSendKey(ECSPanel panel, ECSEventType type, const SDL_KeyboardEvent *key)
{
    ECSEvent event = {
        .type = type,
        .key = key->key,
        .modifiers = ECSI_InputModifiers(key->mod),
    };

    ECSI_PanelPostEvent(panel, &event);
}

/// @brief Moves the keyboard focus to a panel and tells both panels.
static void ECSI_InputFocus(ECSPanel panel)
{
    ECSPanel old = ECSI_LayoutGetFocus();

    if (panel == NULL || panel == old)
    {
        return;
    }

    ECSEvent event = {.type = ECSEventType_Unfocused};

    if (old != NULL)
    {
        ECSI_PanelPostEvent(old, &event);
    }

    ECSI_LayoutSetFocus(panel);

    event.type = ECSEventType_Focused;
    ECSI_PanelPostEvent(panel, &event);
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

    if (focus == NULL || !ECSI_PanelsConfirmClose(&focus, 1))
    {
        return;
    }

    if (focus == INPUT.pointerPanel)
    {
        INPUT.pointerPanel = NULL;
    }

    ECSI_LayoutClosePanel(focus);
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
    return SHUResult_Ok;
}

void ECSI_InputTerminate(void)
{
    ECSI_InputFreeBindings(&INPUT.prefixKeys);
    arrfree(INPUT.prefixLines);
    SDL_zero(INPUT);
}

bool ECSI_InputHandle(const SDL_Event *event)
{
    SDL_assert(event != NULL);

    switch (event->type)
    {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
    {
        // the user may cancel quitting to keep unsaved work
        ECSPanel *panels = ECSI_LayoutGetPanels();
        bool quit = ECSI_PanelsConfirmClose(panels, arrlenu(panels));
        arrfree(panels);
        return !quit;
    }

    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_EXPOSED:
    case SDL_EVENT_WINDOW_SHOWN:
        ECSI_LayoutRequestFrame();
        break;

    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    {
        const SDL_MouseButtonEvent *button = &event->button;

        if (INPUT.prefixActive)
        {
            ECSI_InputSetPrefix(false);
        }

        if (ECSI_LayoutPointerDown(button->x, button->y))
        {
            break;
        }

        ECSPanel panel = ECSI_LayoutPanelAt(button->x, button->y);

        if (panel != NULL)
        {
            ECSI_InputFocus(panel);
            INPUT.pointerPanel = panel;

            ECSEvent details = {.button = button->button, .modifiers = ECSI_InputModifiers(SDL_GetModState())};
            ECSI_InputSendPointer(panel, ECSEventType_PointerDown, button->x, button->y, &details);
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

        ECSPanel panel = INPUT.pointerPanel != NULL ? INPUT.pointerPanel : ECSI_LayoutPanelAt(motion->x, motion->y);

        if (panel == NULL)
        {
            break;
        }

        // in hover mode, only real pointer movement over a panel moves focus; dividers and tab rows are outside every panel
        if (INPUT.pointerPanel == NULL && SDL_strcmp(ECSValue_GetString(ECSSetting_Get("ecs.focus"), ""), "hover") == 0)
        {
            ECSI_InputFocus(panel);
        }

        ECSI_InputSendPointer(panel, ECSEventType_PointerMove, motion->x, motion->y, NULL);

        break;
    }

    case SDL_EVENT_MOUSE_BUTTON_UP:
    {
        const SDL_MouseButtonEvent *button = &event->button;
        ECSI_LayoutPointerUp();

        if (INPUT.pointerPanel != NULL)
        {
            ECSEvent details = {.button = button->button, .modifiers = ECSI_InputModifiers(SDL_GetModState())};
            ECSI_InputSendPointer(INPUT.pointerPanel, ECSEventType_PointerUp, button->x, button->y, &details);
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
            ECSEvent details = {.wheelX = wheel->x, .wheelY = wheel->y};
            ECSI_InputSendPointer(panel, ECSEventType_Wheel, wheel->mouse_x, wheel->mouse_y, &details);
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

        ECSPanel focus = ECSI_LayoutGetFocus();

        if (focus != NULL)
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
