#include "systems/Input.h"

#include "systems/Layout.h"
#include "systems/Panels.h"

#pragma region Source Only

/// @brief An action of the core that follows the core prefix.
typedef enum ECSI_CoreAction
{
    ECSI_CoreAction_FocusLeft = 0,
    ECSI_CoreAction_FocusRight,
    ECSI_CoreAction_FocusUp,
    ECSI_CoreAction_FocusDown,
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
    [ECSI_CoreAction_NextTab] = "Tab",
    [ECSI_CoreAction_Maximize] = "M",
    [ECSI_CoreAction_Close] = "X",
    [ECSI_CoreAction_Cancel] = "Escape",
};

static struct
{
    u32 prefixKey;
    u32 prefixModifiers;
    bool prefixActive;
    u32 actionKeys[ECSI_CoreAction_Count];
    ECSPanel pointerPanel; // panel that got the press; it gets pointer events until the release
} INPUT = {0};

/// @brief Sends a pointer event to a panel, with the position made relative to the panel.
static void ECSI_InputSendPointer(ECSPanel panel, ECSEventKind kind, const ECSI_PlatformEvent *event)
{
    ECSEvent sent = {
        .structSize = sizeof(ECSEvent),
        .kind = kind,
        .x = event->x - panel->x,
        .y = event->y - panel->y,
        .wheelX = event->wheelX,
        .wheelY = event->wheelY,
        .button = event->button,
        .modifiers = event->modifiers,
    };

    ECSI_PanelSendEvent(panel, &sent);
}

static void ECSI_InputSendKey(ECSPanel panel, ECSEventKind kind, const ECSI_PlatformEvent *event)
{
    ECSEvent sent = {
        .structSize = sizeof(ECSEvent),
        .kind = kind,
        .key = event->key,
        .modifiers = event->modifiers,
    };

    ECSI_PanelSendEvent(panel, &sent);
}

/// @brief Moves the keyboard focus to a panel and tells both panels.
static void ECSI_InputFocus(ECSPanel panel)
{
    ECSPanel old = ECSI_LayoutGetFocus();

    if (panel == NULL || panel == old)
    {
        return;
    }

    ECSEvent event = {.structSize = sizeof(ECSEvent), .kind = ECSEventKind_Unfocused};

    if (old != NULL)
    {
        ECSI_PanelSendEvent(old, &event);
    }

    ECSI_LayoutSetFocus(panel);

    event.kind = ECSEventKind_Focused;
    ECSI_PanelSendEvent(panel, &event);
}

static void ECSI_InputSetPrefix(bool active)
{
    INPUT.prefixActive = active;
    ECSI_LayoutShowPrefixKeys(active);
}

/// @brief Runs the core action chosen by the key pressed after the core prefix.
static void ECSI_InputRunAction(const ECSI_PlatformEvent *event)
{
    ECSI_InputSetPrefix(false);

    if (event->key >= '1' && event->key <= '9')
    {
        ECSI_LayoutWorkspaceSwitch(event->key - '1');
        return;
    }

    ECSI_CoreAction action = ECSI_CoreAction_Count;

    for (u32 i = 0; i < ECSI_CoreAction_Count; i++)
    {
        if (INPUT.actionKeys[i] == event->key)
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

        ECSI_LayoutClosePanel(focus);
        break;
    default:
        break;
    }
}

/// @brief Checks the modifiers of a key press against a binding's. AltGr is never part of a binding.
static bool ECSI_InputModifiersMatch(u32 modifiers, u32 expected)
{
    return (modifiers & ECSModifier_AltGr) == 0 && modifiers == expected;
}

#pragma endregion Source Only

SHUResult ECSI_InputInitialize(const char *prefix)
{
    SHU_AssertNullPointer(prefix);

    SHU_ReturnResult(ECSI_PlatformKeyParse(prefix, &INPUT.prefixKey, &INPUT.prefixModifiers));

    for (u32 i = 0; i < ECSI_CoreAction_Count; i++)
    {
        u32 modifiers = 0;
        SHU_ReturnResult(ECSI_PlatformKeyParse(ECSI_PREFIX_KEYS[i], &INPUT.actionKeys[i], &modifiers));
    }

    return SHUResult_Ok;
}

bool ECSI_InputHandle(const ECSI_PlatformEvent *event)
{
    SHU_AssertNullPointer(event);

    switch (event->kind)
    {
    case ECSI_PlatformEventKind_Quit:
        return false;

    case ECSI_PlatformEventKind_WindowChanged:
        ECSI_LayoutRequestFrame();
        break;

    case ECSI_PlatformEventKind_PointerDown:
    {
        if (INPUT.prefixActive)
        {
            ECSI_InputSetPrefix(false);
        }

        if (ECSI_LayoutPointerDown(event->x, event->y))
        {
            break;
        }

        ECSPanel panel = ECSI_LayoutPanelAt(event->x, event->y);

        if (panel != NULL)
        {
            ECSI_InputFocus(panel);
            INPUT.pointerPanel = panel;
            ECSI_InputSendPointer(panel, ECSEventKind_PointerDown, event);
        }

        break;
    }

    case ECSI_PlatformEventKind_PointerMove:
    {
        if (ECSI_LayoutPointerMove(event->x, event->y))
        {
            break;
        }

        ECSPanel panel = INPUT.pointerPanel != NULL ? INPUT.pointerPanel : ECSI_LayoutPanelAt(event->x, event->y);

        if (panel != NULL)
        {
            ECSI_InputSendPointer(panel, ECSEventKind_PointerMove, event);
        }

        break;
    }

    case ECSI_PlatformEventKind_PointerUp:
        ECSI_LayoutPointerUp();

        if (INPUT.pointerPanel != NULL)
        {
            ECSI_InputSendPointer(INPUT.pointerPanel, ECSEventKind_PointerUp, event);
            INPUT.pointerPanel = NULL;
        }

        break;

    case ECSI_PlatformEventKind_Wheel:
    {
        ECSPanel panel = ECSI_LayoutPanelAt(event->x, event->y);

        if (panel != NULL)
        {
            ECSI_InputSendPointer(panel, ECSEventKind_Wheel, event);
        }

        break;
    }

    case ECSI_PlatformEventKind_KeyDown:
    {
        if (INPUT.prefixActive)
        {
            if (!event->modifierKey && !event->repeat)
            {
                ECSI_InputRunAction(event);
            }

            break;
        }

        if (event->key == INPUT.prefixKey && ECSI_InputModifiersMatch(event->modifiers, INPUT.prefixModifiers))
        {
            ECSI_InputSetPrefix(true);
            break;
        }

        ECSPanel focus = ECSI_LayoutGetFocus();

        if (focus != NULL)
        {
            ECSI_InputSendKey(focus, ECSEventKind_KeyDown, event);
        }

        break;
    }

    case ECSI_PlatformEventKind_KeyUp:
    {
        ECSPanel focus = ECSI_LayoutGetFocus();

        if (focus != NULL && !INPUT.prefixActive)
        {
            ECSI_InputSendKey(focus, ECSEventKind_KeyUp, event);
        }

        break;
    }

    default:
        break;
    }

    return true;
}
