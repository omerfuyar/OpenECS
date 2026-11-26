#include "interface/Input.h"

#include "base/Log.h"
#include "interface/Keys.h"
#include "interface/Layout.h"
#include "interface/Menus.h"
#include "interface/Panels.h"
#include "interface/Popups.h"
#include "interface/Window.h"
#include "runtime/Events.h"
#include "runtime/Services.h"
#include "runtime/Settings.h"

#include "SDL3/SDL.h"
#include "stb/stbSDL3.h"

#pragma region Source Only

/// @brief Choices of the setting ecs.focus; its value comes from the core's settings file.
static const char *const OPENECS_FOCUS_CHOICES[] = {"click", "hover", nullptr};

static struct
{
    bool prefixActive;
    // a panel or popup is kept with its id, as a new one may get a freed one's address
    ECSPanel pointerPanel; // panel that got the press; it gets pointer events until the last held button is released
    u32 pointerPanelID;
    u32 pointerButtons;    // SDL's mask of the buttons whose press the panel got and that are still held
    ECSPopup pointerPopup; // popup that got the press; it gets pointer events until the release
    u32 pointerPopupID;
    f32 pointerX; // the pointer's last position in the OS window, in layout units
    f32 pointerY;
    char *dragType;         // type of the data a panel drags, or nullptr
    ECSValue *dragValue;    // the dragged data
    ECSPanel dragOverPanel; // the panel that accepts the dragged data and got DragOver last, until it gets DragLeave
    u32 dragOverPanelID;
    char **droppedFiles; // stb_ds array of the files another application drops, until the drop completes
    char *droppedText;   // the text another application drops, until the drop completes, or nullptr
    bool swallowText;    // the last key press ran a binding or the core's keys, so the text it types is dropped
    bool textOn;         // text input is on, for the panel below
    ECSPanel textPanel;
    u32 textPanelID;
    SDL_Window *textWindow; // the OS window text input is on in
    SDL_Rect textArea;      // where the input method shows its window, in the OS window
    // an input method, such as fcitx5, may repeat a held key itself: it sends a release and a press with the first press's time, again and again
    SDL_Keycode releasedKey;          // the key of the last release
    u64 releasedAt;                   // the time of the last release
    SDL_Keycode inputMethodRepeatKey; // the key that the input method repeats now, or SDLK_UNKNOWN
    struct
    {
        SDL_Keycode key;
        char *value;
    } *keyNames; // stb_ds hash map: the names of the keys that events gave, kept until exit, since SDL's own are temporary
} INPUT = {0};

/// @brief Gives a key's name, as Lua's events give it, such as "Return", in a copy kept until exit; "" if memory runs out.
static const char *ECSIInput_GetKeyName(SDL_Keycode key)
{
    char *name = hmget(INPUT.keyNames, key);

    if (name == nullptr)
    {
        name = SDL_strdup(SDL_GetKeyName(key));

        if (name == nullptr)
        {
            return "";
        }

        hmput(INPUT.keyNames, key, name);
    }

    return name;
}

/// @brief Converts SDL's modifier bits to ECSModifier bits.
static u32 ECSIInput_ConvertModifiers(SDL_Keymod modifiers)
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

/// @brief Sends a pointer event to a panel, with the position made relative to the panel.
static void ECSIInput_SendPointer(ECSPanel panel, ECSPanelEventType type, f32 x, f32 y, i32 button)
{
    ECSPanelEvent event = {
        .type = type,
        .modifiers = ECSIInput_ConvertModifiers(SDL_GetModState()),
        .pointer = {.x = x - panel->x, .y = y - panel->y, .button = button},
    };

    ECSIPanel_PostEvent(panel, &event);
}

/// @brief Gets the direction of the wheel: -1 when the system flips it, so the amounts can be turned back, else 1.
static f32 ECSIInput_GetWheelDirection(const SDL_MouseWheelEvent *wheel)
{
    return wheel->direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f;
}

/// @brief Makes a wheel event at a position in a panel's or a popup's surface. Positive amounts are always away from the user.
static ECSPanelEvent ECSIInput_MakeWheelEvent(const SDL_MouseWheelEvent *wheel, f32 x, f32 y)
{
    f32 direction = ECSIInput_GetWheelDirection(wheel);
    return (ECSPanelEvent){
        .type = ECSPanelEventType_Wheel,
        .modifiers = ECSIInput_ConvertModifiers(SDL_GetModState()),
        .wheel = {.x = x, .y = y, .scrollX = wheel->x * direction, .scrollY = wheel->y * direction},
    };
}

/// @brief Makes a key event of a type: KeyDown or KeyUp.
static ECSPanelEvent ECSIInput_MakeKeyEvent(ECSPanelEventType type, const SDL_KeyboardEvent *key)
{
    return (ECSPanelEvent){
        .type = type,
        .modifiers = ECSIInput_ConvertModifiers(key->mod),
        .key = {.code = key->key, .name = ECSIInput_GetKeyName(key->key)},
    };
}

/// @brief Checks whether a key is a modifier itself, such as Ctrl or AltGr.
static bool ECSIInput_IsModifierKey(SDL_Keycode key)
{
    return (key >= SDLK_LCTRL && key <= SDLK_RGUI) || key == SDLK_MODE;
}

/// @brief Gets the panel that got the pointer press, unless code closed it since.
static ECSPanel ECSIInput_PointerPanel(void)
{
    if (INPUT.pointerPanel != nullptr && (!ECSILayout_HasPanel(INPUT.pointerPanel) || INPUT.pointerPanel->id != INPUT.pointerPanelID))
    {
        INPUT.pointerPanel = nullptr;
    }

    return INPUT.pointerPanel;
}

/// @brief Gets the popup that got the pointer press, unless it has closed since. A popup in the open list lives, so its id can be read.
static ECSPopup ECSIInput_PointerPopup(void)
{
    usz count = 0;
    ECSPopup *popups = ECSIPopups_GetOpen(&count);
    bool open = false;

    for (usz i = 0; i < count; i++)
    {
        open = open || (popups[i] == INPUT.pointerPopup && popups[i]->id == INPUT.pointerPopupID);
    }

    INPUT.pointerPopup = open ? INPUT.pointerPopup : nullptr;
    return INPUT.pointerPopup;
}

/// @brief Sends a pointer event to a popup, at a position in its surface.
static void ECSIInput_SendPopupPointer(ECSPopup popup, ECSPanelEventType type, f32 x, f32 y, i32 button)
{
    ECSPanelEvent event = {
        .type = type,
        .modifiers = ECSIInput_ConvertModifiers(SDL_GetModState()),
        .pointer = {.x = x, .y = y, .button = button},
    };

    ECSIPopup_PostEvent(popup, &event);
}

/// @brief Gives the popup under a point of a window, and the point in its units, or nullptr when no popup is open there.
static ECSPopup ECSIInput_PopupAt(f32 windowX, f32 windowY, f32 *retX, f32 *retY)
{
    usz count = 0;
    ECSIPopups_GetOpen(&count);
    return count == 0 ? nullptr : ECSIWindow_PopupAt(windowX, windowY, retX, retY);
}

/// @brief Gives a press to the popup under the pointer. A press outside every popup, while a menu is open, closes them.
/// @return true if the popup under the pointer got the press, or a menu was open, so the press goes nowhere else.
static bool ECSIInput_PressPopups(const SDL_MouseButtonEvent *button)
{
    f32 x = 0.0f;
    f32 y = 0.0f;
    ECSPopup popup = ECSIInput_PopupAt(button->x, button->y, &x, &y);

    if (popup != nullptr)
    {
        INPUT.pointerPopup = popup;
        INPUT.pointerPopupID = popup->id;
        ECSIInput_SendPopupPointer(popup, ECSPanelEventType_PointerDown, x, y, button->button);
        return true;
    }

    // a press beside a tooltip goes on, as the tooltip takes none
    if (ECSIPopups_GetMenu() == nullptr)
    {
        return false;
    }

    ECSIPopups_CloseAll();
    return true;
}

/// @brief Gives a move of the pointer to the popup that got the press, also outside it, or else to the popup under the pointer.
/// @return true if a popup got the move.
static bool ECSIInput_MovePopupPointer(const SDL_MouseMotionEvent *motion)
{
    f32 x = 0.0f;
    f32 y = 0.0f;
    ECSPopup popup = ECSIInput_PointerPopup();

    if (popup != nullptr)
    {
        ECSIWindow_ToPopup(popup, motion->x, motion->y, &x, &y);
    }
    else
    {
        popup = ECSIInput_PopupAt(motion->x, motion->y, &x, &y);
    }

    if (popup != nullptr)
    {
        ECSIInput_SendPopupPointer(popup, ECSPanelEventType_PointerMove, x, y, 0);
    }

    return popup != nullptr;
}

/// @brief Gives a release of the pointer to the popup that got the press.
/// @return true if a popup got the release.
static bool ECSIInput_ReleasePopupPointer(const SDL_MouseButtonEvent *button)
{
    ECSPopup pressed = ECSIInput_PointerPopup();
    INPUT.pointerPopup = nullptr;

    if (pressed != nullptr)
    {
        f32 x = 0.0f;
        f32 y = 0.0f;
        ECSIWindow_ToPopup(pressed, button->x, button->y, &x, &y);
        ECSIInput_SendPopupPointer(pressed, ECSPanelEventType_PointerUp, x, y, button->button);
    }

    return pressed != nullptr;
}

/// @brief Gives a turn of the mouse wheel to the popup under the pointer.
/// @return true if a popup got the turn.
static bool ECSIInput_TurnPopupWheel(const SDL_MouseWheelEvent *wheel)
{
    f32 x = 0.0f;
    f32 y = 0.0f;
    ECSPopup popup = ECSIInput_PopupAt(wheel->mouse_x, wheel->mouse_y, &x, &y);

    if (popup != nullptr)
    {
        ECSPanelEvent event = ECSIInput_MakeWheelEvent(wheel, x, y);
        ECSIPopup_PostEvent(popup, &event);
    }

    return popup != nullptr;
}

/// @brief While a menu is open, gives a key to the newest menu; Escape closes it.
/// @return true if a menu got the key.
static bool ECSIInput_SendMenuKey(const SDL_KeyboardEvent *key)
{
    ECSPopup menu = ECSIPopups_GetMenu();

    if (menu != nullptr && key->type == SDL_EVENT_KEY_DOWN && key->key == SDLK_ESCAPE)
    {
        ECSPopup_Close(&menu);
    }
    else if (menu != nullptr)
    {
        ECSPanelEvent event = ECSIInput_MakeKeyEvent(key->type == SDL_EVENT_KEY_DOWN ? ECSPanelEventType_KeyDown : ECSPanelEventType_KeyUp, key);
        ECSIPopup_PostEvent(menu, &event);
    }

    // a key that goes to a menu types no text
    INPUT.swallowText = INPUT.swallowText || menu != nullptr;
    return menu != nullptr;
}

/// @brief Handles a pointer or key event for the popups: one under the pointer, or the one that got the press, gets it. A press outside every popup, while a menu
/// is open, closes them and does nothing else.
/// @return true if the popups used the event.
static bool ECSIInput_HandlePopups(const SDL_Event *event)
{
    switch (event->type)
    {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        return ECSIInput_PressPopups(&event->button);

    case SDL_EVENT_MOUSE_MOTION:
        return ECSIInput_MovePopupPointer(&event->motion);

    case SDL_EVENT_MOUSE_BUTTON_UP:
        return ECSIInput_ReleasePopupPointer(&event->button);

    case SDL_EVENT_MOUSE_WHEEL:
        return ECSIInput_TurnPopupWheel(&event->wheel);

    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        return ECSIInput_SendMenuKey(&event->key);

    default:
        return false;
    }
}

/// @brief Sends DragLeave to the panel that got DragOver last, unless it has closed since.
static void ECSIInput_LeaveDragOver(void)
{
    if (INPUT.dragOverPanel != nullptr && ECSILayout_HasPanel(INPUT.dragOverPanel) && INPUT.dragOverPanel->id == INPUT.dragOverPanelID)
    {
        ECSPanelEvent event = {.type = ECSPanelEventType_DragLeave, .modifiers = ECSIInput_ConvertModifiers(SDL_GetModState())};
        ECSIPanel_PostEvent(INPUT.dragOverPanel, &event);
    }

    INPUT.dragOverPanel = nullptr;
}

/// @brief Moves dragged data to a position of the event window: the core marks where it can land, the panel under the pointer gets the move unless it
/// started the drag, and gets DragOver if it accepts the type, even if it started the drag; the panel the data left gets DragLeave. The focus stays.
static void ECSIInput_MoveDataDrag(f32 x, f32 y)
{
    ECSIWindow_ShowDataDrag(INPUT.dragType, x, y);

    ECSPanel over = ECSIWindow_PanelAt(x, y, &x, &y);

    if (over != nullptr && over != ECSIInput_PointerPanel())
    {
        ECSIInput_SendPointer(over, ECSPanelEventType_PointerMove, x, y, 0);
    }

    over = over != nullptr && ECSIPanel_Accepts(over, INPUT.dragType) ? over : nullptr;

    if (over != INPUT.dragOverPanel)
    {
        ECSIInput_LeaveDragOver();
    }

    if (over != nullptr)
    {
        INPUT.dragOverPanel = over;
        INPUT.dragOverPanelID = over->id;
        ECSIPanel_PostDrop(over, ECSPanelEventType_DragOver, x - over->x, y - over->y, INPUT.dragType, INPUT.dragValue, ECSIInput_ConvertModifiers(SDL_GetModState()));
    }
}

/// @brief Ends dragging data, with or without dropping it.
static void ECSIInput_EndDataDrag(void)
{
    ECSIInput_LeaveDragOver();
    SDL_free(INPUT.dragType);
    ECSValue_Destroy(&INPUT.dragValue);
    INPUT.dragType = nullptr;
    ECSIWindow_ShowDataDrag(nullptr, 0.0f, 0.0f);
}

/// @brief Drops data on the panel at a position of the event window, or of another OS window under it, if the panel accepts its type.
static void ECSIInput_Drop(f32 x, f32 y, const char *type, const ECSValue *value)
{
    ECSPanel panel = ECSIWindow_PanelAt(x, y, &x, &y);

    if (panel != nullptr && ECSIPanel_Accepts(panel, type))
    {
        ECSIPanel_PostDrop(panel, ECSPanelEventType_Drop, x - panel->x, y - panel->y, type, value, ECSIInput_ConvertModifiers(SDL_GetModState()));
    }
    else
    {
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "No panel takes the dropped data of type '%s'.", type);
    }
}

/// @brief Forgets what another application dropped.
static void ECSIInput_ForgetDropped(void)
{
    for (usz i = 0; i < arrlenu(INPUT.droppedFiles); i++)
    {
        SDL_free(INPUT.droppedFiles[i]);
    }

    arrfree(INPUT.droppedFiles);
    SDL_free(INPUT.droppedText);
    INPUT.droppedText = nullptr;
}

/// @brief Delivers what another application dropped: its files as a file-list, and its text as text.
static void ECSIInput_CompleteDrop(f32 x, f32 y)
{
    ECSValue *value = nullptr;

    if (arrlenu(INPUT.droppedFiles) > 0 && ECSValue_Create(&value) == SHUResult_Ok)
    {
        SHUResult result = SHUResult_Ok;
        ECSValue_SetTable(value);

        for (usz i = 0; !result && i < arrlenu(INPUT.droppedFiles); i++)
        {
            ECSValue *item = nullptr;
            result = ECSValue_AddListItem(value, &item);
            result = result ? result : ECSValue_SetString(item, INPUT.droppedFiles[i]);
        }

        if (!result)
        {
            ECSIInput_Drop(x, y, "file-list", value);
        }

        ECSValue_Destroy(&value);
    }

    if (INPUT.droppedText != nullptr && ECSValue_Create(&value) == SHUResult_Ok)
    {
        if (ECSValue_SetString(value, INPUT.droppedText) == SHUResult_Ok)
        {
            ECSIInput_Drop(x, y, "text", value);
        }

        ECSValue_Destroy(&value);
    }

    ECSIInput_ForgetDropped();
}

/// @brief Orders two keys after the prefix by the order of their functions among the core's, then by their key texts. userData is the keys.
static int SDLCALL ECSIInput_CompareKeys(void *userData, const void *first, const void *second)
{
    const ECSIKeyBinding *keys = userData;
    const ECSIKeyBinding *firstBinding = &keys[*(const usz *)first];
    const ECSIKeyBinding *secondBinding = &keys[*(const usz *)second];
    usz orderA = ECSIMenus_GetOrder(firstBinding->function);
    usz orderB = ECSIMenus_GetOrder(secondBinding->function);
    return orderA != orderB ? (orderA < orderB ? -1 : 1) : SDL_strcmp(firstBinding->text, secondBinding->text);
}

/// @brief Sets a list to the keys after the prefix with what they do now, in the order of the core's functions, whatever order the settings files
/// give them in, without what cannot be done now.
static SHUResult ECSIInput_ListPrefixKeys(ECSValue *list, ECSPanel focus)
{
    const ECSIKeyBinding *keys = ECSIKeys_GetPrefixKeys();
    usz *order = nullptr;
    arrsetlen(order, arrlenu(keys));

    for (usz i = 0; i < arrlenu(order); i++)
    {
        order[i] = i;
    }

    SDL_qsort_r(order, arrlenu(order), sizeof(usz), ECSIInput_CompareKeys, (void *)keys);
    ECSValue_SetTable(list);
    SHUResult result = SHUResult_Ok;

    for (usz position = 0; !result && position < arrlenu(order); position++)
    {
        const ECSIKeyBinding *binding = &keys[order[position]];
        ECSValue *item = nullptr;
        ECSValue *field = nullptr;

        if (!ECSIMenus_Offers(binding->function, focus))
        {
            continue;
        }

        result = ECSValue_AddListItem(list, &item);
        result = result ? result : ECSValue_SetTableField(item, "key", &field);
        result = result ? result : ECSValue_SetString(field, binding->text);
        result = result ? result : ECSValue_SetTableField(item, "function", &field);
        result = result ? result : ECSValue_SetString(field, binding->function);
        result = result ? result : ECSValue_SetTableField(item, "text", &field);
        result = result ? result : ECSValue_SetString(field, ECSIMenus_LabelOf(binding->function, focus));
    }

    arrfree(order);
    return result;
}

/// @brief Emits ecs.prefix: while the core waits for the key after the prefix, the focused panel and the keys with what they do now, for a plugin
/// to list them; then that it waits no more.
static void ECSIInput_EmitPrefix(bool waiting)
{
    ECSValue *value = nullptr;
    ECSValue *field = nullptr;

    if (ECSValue_Create(&value) != SHUResult_Ok)
    {
        return;
    }

    ECSValue_SetTable(value);
    ECSPanel focus = ECSILayout_GetFocus();
    SHUResult result = ECSValue_SetTableField(value, "waiting", &field);

    if (!result)
    {
        ECSValue_SetBool(field, waiting);
    }

    if (!result && waiting && focus != nullptr)
    {
        result = ECSValue_SetTableField(value, "panel", &field);
        ECSValue_SetInteger(field, ECSPanel_GetID(focus));
    }

    if (!result && waiting)
    {
        result = ECSValue_SetTableField(value, "keys", &field);
        result = result ? result : ECSIInput_ListPrefixKeys(field, focus);
    }

    if (!result)
    {
        ECSIEvents_EmitCore("ecs.prefix", value);
    }

    ECSValue_Destroy(&value);
}

/// @brief Ends waiting for the key after the prefix.
static void ECSIInput_EndPrefix(void)
{
    INPUT.prefixActive = false;
    ECSIInput_EmitPrefix(false);
}

/// @brief Starts waiting for the key after the prefix.
static void ECSIInput_StartPrefix(void)
{
    INPUT.prefixActive = true;
    ECSIInput_EmitPrefix(true);
}

/// @brief Runs the function of the key pressed after the core prefix. Escape cancels.
static void ECSIInput_RunPrefixKey(SDL_Keycode key, u32 modifiers)
{
    ECSIInput_EndPrefix();

    const ECSIKeyBinding *keys = ECSIKeys_GetPrefixKeys();

    for (usz i = 0; key != SDLK_ESCAPE && i < arrlenu(keys); i++)
    {
        if (keys[i].key == key && keys[i].modifiers == modifiers)
        {
            ECSIServices_CallBound(keys[i].function, ECSILayout_GetFocus());
            return;
        }
    }
}

/// @brief Handles the close button of an OS window. A pop-out window closes its panels, after asking about unsaved work; Cancel keeps it. Closing the main window quits.
/// @return false if the program quits.
static bool ECSIInput_CloseWindow(SDL_WindowID windowID)
{
    bool main = true;
    ECSIRoot *root = ECSIWindow_GetRoot(windowID, &main);

    if (main)
    {
        return !ECSILayout_ConfirmCloseAll(true);
    }

    ECSPanel *panels = root == nullptr ? nullptr : ECSILayout_GetRootPanels(root);

    if (ECSIPanels_ConfirmClose(panels, arrlenu(panels), false))
    {
        for (usz i = 0; i < arrlenu(panels); i++)
        {
            ECSILayout_ClosePanel(panels[i]);
        }
    }

    arrfree(panels);
    return true;
}

/// @brief Gives the focus to the panel focused last in an OS window, when the window gets the system's focus.
static void ECSIInput_FocusWindow(SDL_WindowID windowID)
{
    ECSIWindow_Focused(windowID);
    ECSIRoot *root = ECSIWindow_GetRoot(windowID, nullptr);

    if (root != nullptr)
    {
        ECSILayout_FocusRoot(root);
    }
}

/// @brief Handles a press on a tab or a tab row: on a tab or grip, the middle button closes the panel and the right button opens its menu; on the rest of a tab row, the right button opens the shown panel's menu.
/// @return true if the press was on a tab or a tab row.
static bool ECSIInput_PressTab(const SDL_MouseButtonEvent *button)
{
    ECSPanel tab = button->button == SDL_BUTTON_LEFT ? nullptr : ECSIWindow_TabAt(button->x, button->y);
    ECSPanel row = tab == nullptr && button->button == SDL_BUTTON_RIGHT ? ECSIWindow_TabRowAt(button->x, button->y) : nullptr;

    if (row != nullptr)
    {
        ECSIMenus_OpenPanelMenu(row, button->x, button->y);
        return true;
    }

    if (tab == nullptr)
    {
        return false;
    }

    if (button->button == SDL_BUTTON_RIGHT)
    {
        ECSIMenus_OpenPanelMenu(tab, button->x, button->y);
    }
    else if (button->button == SDL_BUTTON_MIDDLE && !ECSILayout_IsLocked(tab))
    {
        (void)ECSLayout_Close(tab);
    }

    return true;
}

/// @brief Handles a press of a mouse button: on a tab, a divider or a grip, or else on the panel under the pointer, which gets the focus.
static void ECSIInput_PressPointer(const SDL_MouseButtonEvent *button)
{
    INPUT.pointerX = button->x;
    INPUT.pointerY = button->y;

    if (INPUT.prefixActive)
    {
        ECSIInput_EndPrefix();
    }

    // while a button is held on a panel, the press of another goes to that panel too
    if (ECSIInput_PointerPanel() != nullptr)
    {
        INPUT.pointerButtons |= SDL_BUTTON_MASK(button->button);
        ECSIInput_SendPointer(INPUT.pointerPanel, ECSPanelEventType_PointerDown, button->x, button->y, button->button);
        return;
    }

    if (ECSIInput_PressTab(button) || (button->button == SDL_BUTTON_LEFT && ECSIWindow_PointerDown(button->x, button->y)))
    {
        return;
    }

    // the tool's name, the menus and the buttons of the title bar
    if (button->button == SDL_BUTTON_LEFT && ECSIMenus_PressTitle(button->x, button->y))
    {
        return;
    }

    // a right click on the title bar does nothing: its menus are the tool's
    if (button->button == SDL_BUTTON_RIGHT && ECSIWindow_TitleBarAt(button->x, button->y))
    {
        return;
    }

    f32 x = 0.0f;
    f32 y = 0.0f;
    ECSPanel panel = ECSIWindow_PanelAt(button->x, button->y, &x, &y);

    if (panel != nullptr)
    {
        ECSILayout_SetFocus(panel);
        INPUT.pointerPanel = panel;
        INPUT.pointerPanelID = panel->id;
        INPUT.pointerButtons = SDL_BUTTON_MASK(button->button);

        ECSIInput_SendPointer(panel, ECSPanelEventType_PointerDown, x, y, button->button);
    }
}

/// @brief Handles a move of the pointer: it marks where dragged data can land, moves what the window drags, or goes to a panel.
static void ECSIInput_MovePointer(const SDL_MouseMotionEvent *motion)
{
    INPUT.pointerX = motion->x;
    INPUT.pointerY = motion->y;

    if (INPUT.dragType != nullptr)
    {
        ECSIInput_MoveDataDrag(motion->x, motion->y);
        return;
    }

    if (ECSIWindow_PointerMove(motion->x, motion->y))
    {
        return;
    }

    // the panel that got the press gets the moves in the OS window of the press, even outside it
    f32 x = motion->x;
    f32 y = motion->y;
    ECSPanel panel = ECSIInput_PointerPanel() != nullptr ? INPUT.pointerPanel : ECSIWindow_PanelAt(motion->x, motion->y, &x, &y);

    if (panel == nullptr)
    {
        return;
    }

    // in hover mode, only real pointer movement over a panel moves focus; dividers and tab rows are outside every panel
    if (INPUT.pointerPanel == nullptr && SDL_strcmp(ECSValue_GetString(ECSSetting_Get("ecs.focus"), ""), "hover") == 0)
    {
        ECSILayout_SetFocus(panel);
    }

    ECSIInput_SendPointer(panel, ECSPanelEventType_PointerMove, x, y, 0);
}

/// @brief Handles a release of a mouse button: it drops dragged data, opens a grip's menu, and goes to the panel that got the press.
static void ECSIInput_ReleasePointer(const SDL_MouseButtonEvent *button)
{
    // dragged data lands on the panel under the pointer, after its DragLeave; the panel it came from still gets its release
    if (INPUT.dragType != nullptr)
    {
        ECSIInput_LeaveDragOver();
        ECSIInput_Drop(button->x, button->y, INPUT.dragType, INPUT.dragValue);
        ECSIInput_EndDataDrag();
    }

    // a click on a grip opens the panel's menu
    ECSPanel clicked = button->button == SDL_BUTTON_LEFT ? ECSIWindow_PointerUp() : nullptr;

    if (clicked != nullptr)
    {
        ECSIMenus_OpenPanelMenu(clicked, button->x, button->y);
    }

    // the panel keeps the pointer until the last button it got is released
    if (ECSIInput_PointerPanel() != nullptr && (INPUT.pointerButtons & SDL_BUTTON_MASK(button->button)) != 0)
    {
        ECSIInput_SendPointer(INPUT.pointerPanel, ECSPanelEventType_PointerUp, button->x, button->y, button->button);
        INPUT.pointerButtons &= ~SDL_BUTTON_MASK(button->button);

        if (INPUT.pointerButtons == 0)
        {
            INPUT.pointerPanel = nullptr;
        }
    }
}

/// @brief Handles a turn of the mouse wheel: over a tab row it shows the next tab, down and right, or the one before; else it goes to the panel under the pointer.
static void ECSIInput_TurnWheel(const SDL_MouseWheelEvent *wheel)
{
    if (ECSIWindow_WheelTabs(wheel->mouse_x, wheel->mouse_y, (wheel->x - wheel->y) * ECSIInput_GetWheelDirection(wheel)))
    {
        return;
    }

    f32 x = 0.0f;
    f32 y = 0.0f;
    ECSPanel panel = ECSIWindow_PanelAt(wheel->mouse_x, wheel->mouse_y, &x, &y);

    if (panel != nullptr)
    {
        ECSPanelEvent panelWheel = ECSIInput_MakeWheelEvent(wheel, x - panel->x, y - panel->y);
        ECSIPanel_PostEvent(panel, &panelWheel);
    }
}

/// @brief Handles a key that the core uses: a key after the prefix, Escape while something is dragged, and the prefix itself.
/// @return true if the core used the key.
static bool ECSIInput_PressCoreKey(const SDL_KeyboardEvent *key, u32 modifiers)
{
    if (INPUT.prefixActive)
    {
        if (!ECSIInput_IsModifierKey(key->key) && !key->repeat)
        {
            ECSIInput_RunPrefixKey(key->key, modifiers);
        }

        return true;
    }

    if (key->key == SDLK_ESCAPE && INPUT.dragType != nullptr)
    {
        ECSIInput_EndDataDrag();
        return true;
    }

    if (key->key == SDLK_ESCAPE && ECSIWindow_CancelDrag())
    {
        return true;
    }

    // the prefix is read when a key is pressed, so a changed prefix always wins
    if (ECSIKeys_IsPrefix(key->key, modifiers))
    {
        ECSIInput_StartPrefix();
        return true;
    }

    return false;
}

/// @brief Handles a key press: the core's keys first, then a binding, then the focused panel.
static void ECSIInput_PressKey(const SDL_KeyboardEvent *key)
{
    // AltGr is never part of a key combination, so a key pressed with AltGr held matches neither the prefix nor a binding, and goes to the panel as text
    u32 modifiers = ECSIInput_ConvertModifiers(key->mod);

    // a key that the core or a binding uses types no text; only a key the focused panel gets does
    INPUT.swallowText = true;

    if (ECSIInput_PressCoreKey(key, modifiers))
    {
        return;
    }

    // a binding wins over the focused panel's own handling of the key, except while the panel takes text input: then a key without Ctrl, Alt or Super is the panel's, so typing never runs a binding
    ECSPanel focus = ECSILayout_GetFocus();
    bool typing = focus != nullptr && focus->textInput && (modifiers & (ECSModifier_Ctrl | ECSModifier_Alt | ECSModifier_Super)) == 0;
    const char *function = ECSIInput_IsModifierKey(key->key) || typing ? nullptr : ECSIKeys_Find(key->key, modifiers, focus);

    if (function != nullptr)
    {
        ECSIServices_CallBound(function, focus);
    }
    else if (focus != nullptr)
    {
        INPUT.swallowText = false;
        ECSPanelEvent keyDown = ECSIInput_MakeKeyEvent(ECSPanelEventType_KeyDown, key);
        ECSIPanel_PostEvent(focus, &keyDown);
    }
}

/// @brief Handles a key release, which goes to the focused panel unless the prefix is active.
static void ECSIInput_ReleaseKey(const SDL_KeyboardEvent *key)
{
    ECSPanel focus = ECSILayout_GetFocus();

    // a key's text comes between its press and its release, so text after the release is not its own, such as what is typed after a key
    // that types none, as Return in a menu, or an input method's, which comes without a key press
    INPUT.swallowText = false;

    if (focus != nullptr && !INPUT.prefixActive)
    {
        ECSPanelEvent keyUp = ECSIInput_MakeKeyEvent(ECSPanelEventType_KeyUp, key);
        ECSIPanel_PostEvent(focus, &keyUp);
    }
}

/// @brief Gives typed text to the focused panel, unless the key that typed it was used.
static void ECSIInput_TypeText(const char *text)
{
    ECSPanel focus = ECSILayout_GetFocus();

    if (!INPUT.swallowText && focus != nullptr && focus->textInput && text != nullptr)
    {
        ECSIPanel_PostText(focus, text, ECSIInput_ConvertModifiers(SDL_GetModState()));
    }

    INPUT.swallowText = false;
}

/// @brief Keeps a file that another application drops, until the drop completes.
static void ECSIInput_AddDroppedFile(const char *path)
{
    char *file = path == nullptr ? nullptr : SDL_strdup(path);

    if (file != nullptr)
    {
        arrput(INPUT.droppedFiles, file);
    }
}

#pragma endregion Source Only

SHUResult ECSIInput_Initialize(void)
{
    // a handle in the dragged data gives no object once its object is freed
    ECSIEvents_WatchValue(&INPUT.dragValue);

    ECSSettingDesc focus = {
        .name = "ecs.focus",
        .type = ECSSettingType_Choice,
        .description = "How focus follows the pointer: click or hover",
        .choices = OPENECS_FOCUS_CHOICES,
    };

    return ECSISettings_DeclareCore(&focus);
}

void ECSIInput_Terminate(void)
{
    // no panel is left to tell that the drag ends
    INPUT.dragOverPanel = nullptr;
    ECSIInput_EndDataDrag();
    ECSIInput_ForgetDropped();

    for (usz i = 0; i < hmlenu(INPUT.keyNames); i++)
    {
        SDL_free(INPUT.keyNames[i].value);
    }

    hmfree(INPUT.keyNames);
    SDL_zero(INPUT);
}

/// @brief Tells whether a key event is SDL's own repeat of a key that an input method repeats too, which would type the key twice.
/// SDL keeps the stale time of the input method's presses, so when the key is released it sends at once every repeat it thinks it missed.
static bool ECSIInput_IsDoubledRepeat(const SDL_Event *event)
{
    if (event->type == SDL_EVENT_KEY_UP)
    {
        INPUT.releasedKey = event->key.key;
        INPUT.releasedAt = event->key.timestamp;
        INPUT.inputMethodRepeatKey = SDLK_UNKNOWN;
        return false;
    }

    if (event->type != SDL_EVENT_KEY_DOWN)
    {
        return false;
    }

    // a press of the key just released, with the release's own time, is the input method's repeat; a person's new press comes later
    if (!event->key.repeat)
    {
        INPUT.inputMethodRepeatKey = event->key.key == INPUT.releasedKey && event->key.timestamp == INPUT.releasedAt ? event->key.key : SDLK_UNKNOWN;
        return false;
    }

    return INPUT.inputMethodRepeatKey != SDLK_UNKNOWN && event->key.key == INPUT.inputMethodRepeatKey;
}

bool ECSIInput_Handle(const SDL_Event *event)
{
    SDL_assert(event != nullptr);

    // the doubled repeat goes nowhere, and neither does the text that SDL sends with it
    if (ECSIInput_IsDoubledRepeat(event))
    {
        INPUT.swallowText = true;
        return true;
    }

    // pointer positions are in the layout units of the event's OS window
    SDL_Window *window = SDL_GetWindowFromEvent(event);

    if (window != nullptr)
    {
        ECSIWindow_SetEventWindow(SDL_GetWindowID(window));
    }

    if (ECSIMenus_Handle(event) || ECSIInput_HandlePopups(event))
    {
        return true;
    }

    switch (event->type)
    {
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        return ECSIInput_CloseWindow(event->window.windowID);

    case SDL_EVENT_QUIT:
        return !ECSILayout_ConfirmCloseAll(true);

    case SDL_EVENT_SYSTEM_THEME_CHANGED:
        ECSISettings_ApplyTheme();
        break;

    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        ECSIInput_FocusWindow(event->window.windowID);
        break;

    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_EXPOSED:
    case SDL_EVENT_WINDOW_SHOWN:
    case SDL_EVENT_WINDOW_RESTORED:
        ECSILayout_RequestFrame();
        break;

    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        ECSIInput_PressPointer(&event->button);
        break;

    case SDL_EVENT_MOUSE_MOTION:
        ECSIInput_MovePointer(&event->motion);
        break;

    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        ECSIWindow_PointerLeave();
        break;

    case SDL_EVENT_MOUSE_BUTTON_UP:
        ECSIInput_ReleasePointer(&event->button);
        break;

    case SDL_EVENT_MOUSE_WHEEL:
        ECSIInput_TurnWheel(&event->wheel);
        break;

    case SDL_EVENT_KEY_DOWN:
        ECSIInput_PressKey(&event->key);
        break;

    case SDL_EVENT_KEY_UP:
        ECSIInput_ReleaseKey(&event->key);
        break;

    case SDL_EVENT_TEXT_INPUT:
        ECSIInput_TypeText(event->text.text);
        break;

    // files and text dropped from other applications; SDL gives each file, then completes the drop
    case SDL_EVENT_DROP_BEGIN:
        ECSIInput_ForgetDropped();
        break;

    case SDL_EVENT_DROP_FILE:
        ECSIInput_AddDroppedFile(event->drop.data);
        break;

    case SDL_EVENT_DROP_TEXT:
        SDL_free(INPUT.droppedText);
        INPUT.droppedText = event->drop.data == nullptr ? nullptr : SDL_strdup(event->drop.data);
        break;

    case SDL_EVENT_DROP_COMPLETE:
        ECSIInput_CompleteDrop(event->drop.x, event->drop.y);
        break;

    default:
        break;
    }

    return true;
}

void ECSIInput_UpdateTextInput(void)
{
    ECSPanel focus = ECSILayout_GetFocus();
    SDL_Window *window = ECSIWindow_GetPanelWindow(focus);
    bool enable = focus != nullptr && focus->textInput && focus->fault == nullptr;
    SDL_Rect area = {0};

    if (window == nullptr)
    {
        return;
    }

    // text input follows the focus into another OS window
    if (INPUT.textOn && INPUT.textWindow != window)
    {
        SDL_StopTextInput(INPUT.textWindow);
        INPUT.textOn = false;
    }

    // the cursor is in the panel's surface pixels, and a surface has one pixel for each layout unit
    if (enable)
    {
        area = (SDL_Rect){(int)SDL_roundf(focus->x + focus->textArea.x), (int)SDL_roundf(focus->y + focus->textArea.y), (int)SDL_roundf(focus->textArea.w), (int)SDL_roundf(focus->textArea.h)};
    }

    if (enable == INPUT.textOn && (!enable || (focus == INPUT.textPanel && focus->id == INPUT.textPanelID && SDL_RectsEqual(&area, &INPUT.textArea))))
    {
        return;
    }

    if (enable)
    {
        SDL_SetTextInputArea(window, &area, 0);

        if (!INPUT.textOn)
        {
            SDL_StartTextInput(window);
        }
    }
    else
    {
        SDL_StopTextInput(window);
    }

    INPUT.textOn = enable;
    INPUT.textWindow = window;
    INPUT.textPanel = enable ? focus : nullptr;
    INPUT.textPanelID = enable ? focus->id : 0;
    INPUT.textArea = area;
}

SHUResult ECSPanel_StartDrag(ECSPanel panel, const char *type, const ECSValue *value)
{
    ECSILog_Check(panel != nullptr, SHUResult_ErrAssertion);
    ECSILog_Check(type != nullptr, SHUResult_ErrAssertion);

    if (ECSIInput_PointerPanel() != panel)
    {
        return SHUResult_Err;
    }

    // a new drag replaces one the panel started before in the same press
    ECSIInput_EndDataDrag();
    INPUT.dragType = SDL_strdup(type);
    SHUResult result = INPUT.dragType == nullptr ? SHUResult_ErrAllocation : ECSValue_Create(&INPUT.dragValue);
    result = result || value == nullptr ? result : ECSValue_Copy(INPUT.dragValue, value);

    if (result)
    {
        ECSIInput_EndDataDrag();
        return result;
    }

    ECSIInput_MoveDataDrag(INPUT.pointerX, INPUT.pointerY);
    return SHUResult_Ok;
}
