#include "Input.h"

#include "Layout.h"
#include "Panels.h"
#include "Settings.h"

#include "SDL3/SDL.h"
#include "stb/stbSDL3.h"

#pragma region Source Only

/// @brief An action of the core that follows the core prefix.
typedef enum ECSI_CoreAction
{
    ECSI_CoreAction_FocusLeft = 0,
    ECSI_CoreAction_FocusRight,
    ECSI_CoreAction_FocusUp,
    ECSI_CoreAction_FocusDown,
    ECSI_CoreAction_MoveLeft,
    ECSI_CoreAction_MoveRight,
    ECSI_CoreAction_MoveUp,
    ECSI_CoreAction_MoveDown,
    ECSI_CoreAction_NextTab,
    ECSI_CoreAction_Maximize,
    ECSI_CoreAction_Close,
    ECSI_CoreAction_Cancel,
    ECSI_CoreAction_Count,
} ECSI_CoreAction;

/// @brief The keys that follow the core prefix: the default of the setting ecs.prefix_keys. Digits 1 to 9 switch workspace.
static const char *const ECSI_PREFIX_KEYS[ECSI_CoreAction_Count] = {
    [ECSI_CoreAction_FocusLeft] = "Left",
    [ECSI_CoreAction_FocusRight] = "Right",
    [ECSI_CoreAction_FocusUp] = "Up",
    [ECSI_CoreAction_FocusDown] = "Down",
    [ECSI_CoreAction_MoveLeft] = "Shift+Left",
    [ECSI_CoreAction_MoveRight] = "Shift+Right",
    [ECSI_CoreAction_MoveUp] = "Shift+Up",
    [ECSI_CoreAction_MoveDown] = "Shift+Down",
    [ECSI_CoreAction_NextTab] = "Tab",
    [ECSI_CoreAction_Maximize] = "M",
    [ECSI_CoreAction_Close] = "X",
    [ECSI_CoreAction_Cancel] = "Escape",
};

/// @brief Default of the setting ecs.prefix.
#define OPENECS_DEFAULT_PREFIX "Alt+W"

/// @brief Choices of the setting ecs.focus; the first is the default.
static const char *const ECSI_FOCUS_CHOICES[] = {"click", "hover", NULL};

static struct
{
    const ECSValue *prefixSetting; // the value of ecs.prefix that prefixKey was read from
    u32 prefixKey;
    u32 prefixModifiers;
    bool prefixActive;
    u32 actionKeys[ECSI_CoreAction_Count];
    u32 actionModifiers[ECSI_CoreAction_Count];
    ECSPanel pointerPanel; // panel that got the press; it gets pointer events until the release
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
    const ECSValue *setting = ECSSetting_Get("ecs.prefix");

    if (setting == INPUT.prefixSetting)
    {
        return;
    }

    INPUT.prefixSetting = setting;

    if (ECSI_InputParseKey(ECSValue_GetString(setting, OPENECS_DEFAULT_PREFIX), &INPUT.prefixKey, &INPUT.prefixModifiers))
    {
        SHUResult result = ECSI_InputParseKey(OPENECS_DEFAULT_PREFIX, &INPUT.prefixKey, &INPUT.prefixModifiers);
        SDL_assert(result == SHUResult_Ok);
        (void)result;
    }
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

static void ECSI_InputSetPrefix(bool active)
{
    INPUT.prefixActive = active;
    ECSI_LayoutShowPrefixKeys(active);
}

/// @brief Checks the modifiers of a key press against a binding's. AltGr is never part of a binding.
static bool ECSI_InputModifiersMatch(u32 modifiers, u32 expected)
{
    return (modifiers & ECSModifier_AltGr) == 0 && modifiers == expected;
}

/// @brief Runs the core action chosen by the key pressed after the core prefix.
static void ECSI_InputRunAction(SDL_Keycode key, u32 modifiers)
{
    ECSI_InputSetPrefix(false);

    if (key >= SDLK_1 && key <= SDLK_9)
    {
        ECSI_LayoutWorkspaceSwitch(key - SDLK_1);
        return;
    }

    ECSI_CoreAction action = ECSI_CoreAction_Count;

    for (u32 i = 0; i < ECSI_CoreAction_Count; i++)
    {
        if (INPUT.actionKeys[i] == key && ECSI_InputModifiersMatch(modifiers, INPUT.actionModifiers[i]))
        {
            action = (ECSI_CoreAction)i;
        }
    }

    ECSPanel focus = ECSI_LayoutGetFocus();

    switch (action)
    {
    case ECSI_CoreAction_FocusLeft:
        ECSI_InputFocus(ECSI_LayoutFindNeighbour(-1, 0));
        break;
    case ECSI_CoreAction_FocusRight:
        ECSI_InputFocus(ECSI_LayoutFindNeighbour(1, 0));
        break;
    case ECSI_CoreAction_FocusUp:
        ECSI_InputFocus(ECSI_LayoutFindNeighbour(0, -1));
        break;
    case ECSI_CoreAction_FocusDown:
        ECSI_InputFocus(ECSI_LayoutFindNeighbour(0, 1));
        break;
    case ECSI_CoreAction_MoveLeft:
        ECSI_LayoutMoveFocus(-1, 0);
        break;
    case ECSI_CoreAction_MoveRight:
        ECSI_LayoutMoveFocus(1, 0);
        break;
    case ECSI_CoreAction_MoveUp:
        ECSI_LayoutMoveFocus(0, -1);
        break;
    case ECSI_CoreAction_MoveDown:
        ECSI_LayoutMoveFocus(0, 1);
        break;
    case ECSI_CoreAction_NextTab:
        ECSI_InputFocus(ECSI_LayoutNextTab());
        break;
    case ECSI_CoreAction_Maximize:
        ECSI_LayoutToggleMaximize();
        break;
    case ECSI_CoreAction_Close:
        if (focus != NULL && focus == INPUT.pointerPanel)
        {
            INPUT.pointerPanel = NULL;
        }

        if (focus != NULL && ECSI_PanelsConfirmClose(&focus, 1))
        {
            ECSI_LayoutClosePanel(focus);
        }

        break;
    default:
        break;
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
    };

    ECSSettingDesc focus = {
        .name = "ecs.focus",
        .type = ECSSettingType_Choice,
        .description = "How focus follows the pointer: click or hover",
        .choices = ECSI_FOCUS_CHOICES,
    };

    SHU_ReturnResult(ECSI_SettingsDeclareCore(&prefix));
    SHU_ReturnResult(ECSI_SettingsDeclareCore(&focus));
    ECSI_InputReadPrefix();

    for (u32 i = 0; i < ECSI_CoreAction_Count; i++)
    {
        SHU_ReturnResult(ECSI_InputParseKey(ECSI_PREFIX_KEYS[i], &INPUT.actionKeys[i], &INPUT.actionModifiers[i]));
    }

    return SHUResult_Ok;
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
                ECSI_InputRunAction(key->key, ECSI_InputModifiers(key->mod));
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
