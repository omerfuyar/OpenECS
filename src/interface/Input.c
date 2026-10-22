#include "interface/Input.h"

#include "interface/Keys.h"
#include "interface/Layout.h"
#include "interface/Menus.h"
#include "interface/Panels.h"
#include "interface/Popups.h"
#include "interface/Window.h"
#include "runtime/Services.h"
#include "runtime/Settings.h"

#include "SDL3/SDL.h"
#include "stb/stbSDL3.h"

#pragma region Source Only

/// @brief Starts of the names of the functions that switch workspaces and that move the focused panel to one; the workspace's number follows.
#define OPENECS_SWITCH_FUNCTION "ecs.workspace.switch"
#define OPENECS_MOVE_FUNCTION "ecs.layout.moveToWorkspace"

/// @brief Choices of the setting ecs.focus; its value comes from the core's settings file.
static const char *const OPENECS_FOCUS_CHOICES[] = {"click", "hover", nullptr};

/// @brief A section of the list of prefix keys, in the order the list shows them.
typedef enum ECSIPrefixSection
{
    ECSIPrefixSection_Navigation = 0,
    ECSIPrefixSection_Panel,
    ECSIPrefixSection_Layout,
    ECSIPrefixSection_Session,
    ECSIPrefixSection_More,
    ECSIPrefixSection_Count,
} ECSIPrefixSection;

/// @brief Headings of the sections of the list of prefix keys, in the order of ECSIPrefixSection.
static const char *const OPENECS_PREFIX_SECTIONS[ECSIPrefixSection_Count] = {"Navigation", "Panel", "Layout", "Session", "More"};

/// @brief Functions that the list of prefix keys shows on one line when they run on the four arrows: left, right, up and down.
static const struct
{
    const char *functions[4];
    const char *description;
} OPENECS_ARROW_FAMILIES[] = {
    {{"ecs.layout.focusLeft", "ecs.layout.focusRight", "ecs.layout.focusUp", "ecs.layout.focusDown"}, "Focus to panel"},
    {{"ecs.layout.moveLeft", "ecs.layout.moveRight", "ecs.layout.moveUp", "ecs.layout.moveDown"}, "Move panel to"},
};

static struct
{
    bool logInput; // --log-input: each key and text event that SDL gives is logged
    bool prefixActive;
    const char **prefixLines; // stb_ds array of the lines shown after the prefix: key text, description, and so on; a nullptr key text makes a heading
    char **prefixTexts;       // stb_ds array of the key texts made for the lines, such as "1...0"
    ECSPanel pointerPanel;    // panel that got the press; it gets pointer events until the release
    ECSPopup pointerPopup;    // popup that got the press; it gets pointer events until the release
    f32 pointerX;             // the pointer's last position in the OS window, in layout units
    f32 pointerY;
    void *clipboard;          // what a clipboard getter returned last, freed by the next call
    char *dragType;           // type of the data a panel drags, or nullptr
    ECSValue *dragValue;      // the dragged data
    char **droppedFiles;      // stb_ds array of the files another application drops, until the drop completes
    char *droppedText;        // the text another application drops, until the drop completes, or nullptr
    bool swallowText;         // the last key press ran a binding or the core's keys, so the text it types is dropped
    bool textOn;              // text input is on, for the panel below
    ECSPanel textPanel;
    SDL_Window *textWindow; // the OS window text input is on in
    SDL_Rect textArea;      // where the input method shows its window, in the OS window
    const ECSIDialogView *dialog; // the message dialog that waits for an answer, or nullptr
    ECSIDialogStepFunction TestStep; // a running test's step while a dialog waits, or nullptr
    ECSIDialogWaitFunction TestWait;
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

/// @brief A file dialog waiting for its answer, with copies of everything SDL reads until it answers.
typedef struct ECSIDialog
{
    ECSPlugin plugin; // nullptr for the core
    ECSDialogDoneFunction Done;
    void *data;
    SDL_DialogFileFilter *filters; // stb_ds array
    char **texts;                  // stb_ds array of the copied texts: filter names and patterns, and the location
    char **files;                  // stb_ds array of the answer
    bool failed;                   // a text could not be copied
} ECSIDialog;

/// @brief Typed data that the core offers on the clipboard.
typedef struct ECSIClipboardData
{
    char *mimeType;
    usz size;
    u8 bytes[]; // the data
} ECSIClipboardData;

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

/// @brief Asks about the unsaved work of every panel before the program quits.
/// @return true if the program may quit; the user may cancel to keep the work.
static bool ECSIInput_ConfirmQuit(void)
{
    ECSPanel *panels = ECSILayout_GetPanels();
    bool quit = ECSIPanels_ConfirmClose(panels, arrlenu(panels), true);
    arrfree(panels);
    return quit;
}

/// @brief Gets the panel that got the pointer press, unless code closed it since.
static ECSPanel ECSIInput_PointerPanel(void)
{
    if (INPUT.pointerPanel != nullptr && !ECSILayout_HasPanel(INPUT.pointerPanel))
    {
        INPUT.pointerPanel = nullptr;
    }

    return INPUT.pointerPanel;
}

/// @brief Gets the popup that got the pointer press, unless it has closed since. It compares pointers only, so the popup may already be freed.
static ECSPopup ECSIInput_PointerPopup(void)
{
    usz count = 0;
    ECSPopup *popups = ECSIPopups_GetOpen(&count);
    bool open = false;

    for (usz i = 0; i < count; i++)
    {
        open = open || popups[i] == INPUT.pointerPopup;
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

/// @brief Handles a pointer event for the popups: one under the pointer, or the one that got the press, gets it. A press outside every popup closes them and does nothing else.
/// @return true if the popups used the event.
static bool ECSIInput_HandlePopups(const SDL_Event *event)
{
    usz count = 0;
    ECSIPopups_GetOpen(&count);
    f32 x = 0.0f;
    f32 y = 0.0f;

    switch (event->type)
    {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    {
        ECSPopup popup = count == 0 ? nullptr : ECSIWindow_PopupAt(event->button.x, event->button.y, &x, &y);

        if (popup != nullptr)
        {
            INPUT.pointerPopup = popup;
            ECSIInput_SendPopupPointer(popup, ECSPanelEventType_PointerDown, x, y, event->button.button);
        }
        else if (count > 0)
        {
            ECSIPopups_CloseAll();
        }

        return count > 0;
    }

    case SDL_EVENT_MOUSE_MOTION:
    {
        // the popup that got the press gets the moves, also outside it
        ECSPopup pressed = ECSIInput_PointerPopup();

        if (pressed != nullptr)
        {
            ECSIWindow_ToPopup(pressed, event->motion.x, event->motion.y, &x, &y);
            ECSIInput_SendPopupPointer(pressed, ECSPanelEventType_PointerMove, x, y, 0);
            return true;
        }

        ECSPopup popup = count == 0 ? nullptr : ECSIWindow_PopupAt(event->motion.x, event->motion.y, &x, &y);

        if (popup != nullptr)
        {
            ECSIInput_SendPopupPointer(popup, ECSPanelEventType_PointerMove, x, y, 0);
        }

        return popup != nullptr;
    }

    case SDL_EVENT_MOUSE_BUTTON_UP:
    {
        ECSPopup pressed = ECSIInput_PointerPopup();
        INPUT.pointerPopup = nullptr;

        if (pressed != nullptr)
        {
            ECSIWindow_ToPopup(pressed, event->button.x, event->button.y, &x, &y);
            ECSIInput_SendPopupPointer(pressed, ECSPanelEventType_PointerUp, x, y, event->button.button);
        }

        return pressed != nullptr;
    }

    case SDL_EVENT_MOUSE_WHEEL:
    {
        ECSPopup popup = count == 0 ? nullptr : ECSIWindow_PopupAt(event->wheel.mouse_x, event->wheel.mouse_y, &x, &y);

        if (popup != nullptr)
        {
            ECSPanelEvent wheel = ECSIInput_MakeWheelEvent(&event->wheel, x, y);
            ECSIPopup_PostEvent(popup, &wheel);
        }

        return popup != nullptr;
    }

    // while a menu is open, key presses go to the newest menu; Escape closes it
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
    {
        ECSPopup menu = ECSIPopups_GetMenu();

        if (menu != nullptr && event->type == SDL_EVENT_KEY_DOWN && event->key.key == SDLK_ESCAPE)
        {
            ECSPopup_Close(&menu);
        }
        else if (menu != nullptr)
        {
            ECSPanelEvent key = ECSIInput_MakeKeyEvent(event->type == SDL_EVENT_KEY_DOWN ? ECSPanelEventType_KeyDown : ECSPanelEventType_KeyUp, &event->key);
            ECSIPopup_PostEvent(menu, &key);
        }

        // a key that goes to a menu types no text
        INPUT.swallowText = INPUT.swallowText || menu != nullptr;
        return menu != nullptr;
    }

    default:
        return false;
    }
}

/// @brief Ends dragging data, with or without dropping it.
static void ECSIInput_EndDataDrag(void)
{
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
        ECSIPanel_PostDrop(panel, x - panel->x, y - panel->y, type, value, ECSIInput_ConvertModifiers(SDL_GetModState()));
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

/// @brief Finds the section of the list of prefix keys that a function is listed in.
static ECSIPrefixSection ECSIInput_SectionOf(const char *function)
{
    // by the start of their names: Navigation, Layout and Session; the core's other functions act on the panel, and the plugins' are More
    static const struct
    {
        const char *start;
        ECSIPrefixSection section;
    } SECTIONS[] = {
        {"ecs.layout.focus", ECSIPrefixSection_Navigation},
        {"ecs.layout.move", ECSIPrefixSection_Navigation},
        {"ecs.layout.nextTab", ECSIPrefixSection_Navigation},
        {"ecs.workspace.", ECSIPrefixSection_Navigation},
        {"ecs.layout.closeGroup", ECSIPrefixSection_Layout},
        {"ecs.layout.lock", ECSIPrefixSection_Layout},
        {"ecs.layout.reset", ECSIPrefixSection_Layout},
        {"ecs.session.", ECSIPrefixSection_Session},
        {"ecs.app.", ECSIPrefixSection_Session},
    };

    for (usz i = 0; i < SDL_arraysize(SECTIONS); i++)
    {
        if (SDL_strncmp(function, SECTIONS[i].start, SDL_strlen(SECTIONS[i].start)) == 0)
        {
            return SECTIONS[i].section;
        }
    }

    return SDL_strncmp(function, "ecs.", 4) == 0 ? ECSIPrefixSection_Panel : ECSIPrefixSection_More;
}

/// @brief Finds the prefix key that runs a function with a key and modifiers, and is not listed yet.
/// @return Its position, or -1.
static i64 ECSIInput_FindPrefixKey(const char *function, u32 key, u32 modifiers, const bool *listedKeys)
{
    const ECSIKeyBinding *keys = ECSIKeys_GetPrefixKeys();

    for (usz i = 0; i < arrlenu(keys); i++)
    {
        const ECSIKeyBinding *binding = &keys[i];

        if (!listedKeys[i] && binding->key == key && binding->modifiers == modifiers && SDL_strcmp(binding->function, function) == 0)
        {
            return (i64)i;
        }
    }

    return -1;
}

/// @brief Folds a prefix key into one line with its family, when the family's four functions run on the four arrows with the same modifiers.
/// @return The line's key text, such as "Shift+Arrows", made for the list; or nullptr if the key does not fold.
static char *ECSIInput_FoldArrows(usz index, bool *listedKeys, const char **retDescription)
{
    const ECSIKeyBinding *binding = &ECSIKeys_GetPrefixKeys()[index];
    const SDL_Keycode arrows[] = {SDLK_LEFT, SDLK_RIGHT, SDLK_UP, SDLK_DOWN};

    for (usz family = 0; family < SDL_arraysize(OPENECS_ARROW_FAMILIES); family++)
    {
        i64 members[4] = {0};
        bool complete = true;

        for (usz i = 0; i < 4; i++)
        {
            members[i] = ECSIInput_FindPrefixKey(OPENECS_ARROW_FAMILIES[family].functions[i], arrows[i], binding->modifiers, listedKeys);
            complete = complete && members[i] >= 0;
        }

        if (!complete || (members[0] != (i64)index && members[1] != (i64)index && members[2] != (i64)index && members[3] != (i64)index))
        {
            continue;
        }

        // the modifiers keep the user's spelling: the text before the key's name
        const char *plus = SDL_strrchr(binding->text, '+');
        int modifiersLength = plus == nullptr ? 0 : (int)(plus - binding->text + 1);
        char *text = nullptr;

        if (SDL_asprintf(&text, "%.*sArrows", modifiersLength, binding->text) < 0)
        {
            return nullptr;
        }

        for (usz i = 0; i < 4; i++)
        {
            listedKeys[members[i]] = true;
        }

        *retDescription = OPENECS_ARROW_FAMILIES[family].description;
        return text;
    }

    return nullptr;
}

/// @brief Folds the keys of one workspace function family into one line, from the first workspace's key to the last one's, such as "1...0" or "Shift+1...0".
/// @param start Start of the family's names; the workspace's number follows.
/// @return The line's key text, made for the list.
static char *ECSIInput_FoldWorkspaces(const char *start, bool *listedKeys)
{
    const char *firstKey = nullptr;
    const char *lastKey = nullptr;
    i64 first = 0;
    i64 last = 0;

    const ECSIKeyBinding *keys = ECSIKeys_GetPrefixKeys();

    for (usz i = 0; i < arrlenu(keys); i++)
    {
        const char *function = keys[i].function;

        if (SDL_strncmp(function, start, SDL_strlen(start)) != 0)
        {
            continue;
        }

        i64 number = SDL_strtoll(function + SDL_strlen(start), nullptr, 10);
        listedKeys[i] = true;

        if (firstKey == nullptr || number < first)
        {
            firstKey = keys[i].text;
            first = number;
        }

        if (lastKey == nullptr || number > last)
        {
            lastKey = keys[i].text;
            last = number;
        }
    }

    if (first == last)
    {
        return SDL_strdup(firstKey);
    }

    // the last key leaves out the modifiers it shares with the first: "Shift+1...0"
    const char *plus = SDL_strrchr(lastKey, '+');
    usz modifiersLength = plus == nullptr ? 0 : (usz)(plus - lastKey + 1);
    const char *lastName = SDL_strncmp(firstKey, lastKey, modifiersLength) == 0 ? lastKey + modifiersLength : lastKey;
    char *text = nullptr;
    return SDL_asprintf(&text, "%s...%s", firstKey, lastName) < 0 ? nullptr : text;
}

/// @brief Orders two keys after the prefix by the order of their functions among the core's, then by their key texts. userData is the keys.
static int SDLCALL ECSIInput_CompareKeys(void *userData, const void *first, const void *second)
{
    const ECSIKeyBinding *keys = userData;
    const ECSIKeyBinding *a = &keys[*(const usz *)first];
    const ECSIKeyBinding *b = &keys[*(const usz *)second];
    usz orderA = ECSIMenus_GetOrder(a->function);
    usz orderB = ECSIMenus_GetOrder(b->function);
    return orderA != orderB ? (orderA < orderB ? -1 : 1) : SDL_strcmp(a->text, b->text);
}

/// @brief Frees the lines of the list of prefix keys.
static void ECSIInput_FreePrefixLines(void)
{
    arrfree(INPUT.prefixLines);

    for (usz i = 0; i < arrlenu(INPUT.prefixTexts); i++)
    {
        SDL_free(INPUT.prefixTexts[i]);
    }

    arrfree(INPUT.prefixTexts);
}

/// @brief Ends waiting for the key after the prefix, and hides the list of keys.
static void ECSIInput_EndPrefix(void)
{
    INPUT.prefixActive = false;
    ECSIInput_FreePrefixLines();
    ECSIWindow_ShowPrefixKeys(nullptr, 0);
}

/// @brief Starts waiting for the key after the prefix, and shows the keys with what they do now, in sections.
static void ECSIInput_StartPrefix(void)
{
    INPUT.prefixActive = true;
    ECSIInput_FreePrefixLines();

    const ECSIKeyBinding *keys = ECSIKeys_GetPrefixKeys();
    ECSPanel focus = ECSILayout_GetFocus();
    bool *listedKeys = nullptr;
    arrsetlen(listedKeys, arrlenu(keys));
    SDL_memset(listedKeys, 0, arrlenu(listedKeys) * sizeof(bool));

    // the keys are listed in the order of the core's functions, whatever order the settings files give them in
    usz *order = nullptr;
    arrsetlen(order, arrlenu(keys));

    for (usz i = 0; i < arrlenu(order); i++)
    {
        order[i] = i;
    }

    SDL_qsort_r(order, arrlenu(order), sizeof(usz), ECSIInput_CompareKeys, (void *)keys);

    for (ECSIPrefixSection section = 0; section < ECSIPrefixSection_Count; section++)
    {
        // a section without keys has no heading
        usz start = arrlenu(INPUT.prefixLines);
        arrput(INPUT.prefixLines, nullptr);
        arrput(INPUT.prefixLines, OPENECS_PREFIX_SECTIONS[section]);

        for (usz position = 0; position < arrlenu(order); position++)
        {
            usz i = order[position];
            const char *function = keys[i].function;

            if (listedKeys[i] || ECSIInput_SectionOf(function) != section || !ECSIMenus_Offers(function, focus))
            {
                continue;
            }

            const char *description = nullptr;
            char *text = nullptr;

            if (SDL_strncmp(function, OPENECS_SWITCH_FUNCTION, SDL_strlen(OPENECS_SWITCH_FUNCTION)) == 0)
            {
                text = ECSIInput_FoldWorkspaces(OPENECS_SWITCH_FUNCTION, listedKeys);
                description = "Switch to workspace";
            }
            else if (SDL_strncmp(function, OPENECS_MOVE_FUNCTION, SDL_strlen(OPENECS_MOVE_FUNCTION)) == 0)
            {
                text = ECSIInput_FoldWorkspaces(OPENECS_MOVE_FUNCTION, listedKeys);
                description = "Move panel to workspace";
            }
            else
            {
                text = ECSIInput_FoldArrows(i, listedKeys, &description);
            }

            if (text != nullptr)
            {
                arrput(INPUT.prefixTexts, text);
            }

            arrput(INPUT.prefixLines, text != nullptr ? text : keys[i].text);
            arrput(INPUT.prefixLines, description != nullptr ? description : ECSIMenus_LabelOf(function, focus));
        }

        if (arrlenu(INPUT.prefixLines) == start + 2)
        {
            arrsetlen(INPUT.prefixLines, start);
        }
    }

    arrfree(listedKeys);
    arrfree(order);
    arrput(INPUT.prefixLines, "Escape");
    arrput(INPUT.prefixLines, "Cancel");

    ECSIWindow_ShowPrefixKeys(INPUT.prefixLines, arrlenu(INPUT.prefixLines) / 2);
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

static const void *ECSIInput_ClipboardProvide(void *userData, const char *mimeType, size_t *retSize)
{
    ECSIClipboardData *data = userData;

    if (SDL_strcmp(mimeType, data->mimeType) != 0)
    {
        *retSize = 0;
        return nullptr;
    }

    *retSize = data->size;
    return data->bytes;
}

static void ECSIInput_ClipboardRelease(void *userData)
{
    ECSIClipboardData *data = userData;
    SDL_free(data->mimeType);
    SDL_free(data);
}

/// @brief Frees what a clipboard getter returned last.
static void ECSIInput_ForgetClipboard(void)
{
    SDL_free(INPUT.clipboard);
    INPUT.clipboard = nullptr;
}

static void ECSIInput_FreeDialog(ECSIDialog *dialog)
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
static const char *ECSIInput_DialogText(ECSIDialog *dialog, const char *text)
{
    char *copy = text == nullptr ? nullptr : SDL_strdup(text);

    if (copy != nullptr)
    {
        arrput(dialog->texts, copy);
    }

    dialog->failed = dialog->failed || (text != nullptr && copy == nullptr);
    return copy;
}

/// @brief Gives a dialog's answer to its plugin, on the main thread.
static void ECSIInput_DialogFinish(void *data)
{
    ECSIDialog *dialog = data;
    usz count = arrlenu(dialog->files);
    dialog->Done(dialog->data, count == 0 ? nullptr : (const char *const *)dialog->files, count);
    ECSIInput_FreeDialog(dialog);
}

/// @brief Takes SDL's answer, maybe on another thread, and sends it to the main thread.
static void SDLCALL ECSIInput_DialogAnswer(void *userData, const char *const *files, int filter)
{
    (void)filter;
    ECSIDialog *dialog = userData;

    if (files == nullptr)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "A file dialog of %s%s failed: %s", dialog->plugin == nullptr ? "the core" : "plugin ", dialog->plugin == nullptr ? "" : ECSIPlugin_GetName(dialog->plugin), SDL_GetError());
    }

    // a cancelled dialog gives an empty list, which reaches the plugin as nullptr, like a failed one
    for (usz i = 0; files != nullptr && files[i] != nullptr; i++)
    {
        char *copy = SDL_strdup(files[i]);

        if (copy != nullptr)
        {
            arrput(dialog->files, copy);
        }
    }

    if (ECS_RunOnMainThread(ECSIInput_DialogFinish, dialog))
    {
        ECSIInput_FreeDialog(dialog);
    }
}

#pragma endregion Source Only

SHUResult ECSIInput_Initialize(void)
{
    ECSSettingDesc focus = {
        .name = "ecs.focus",
        .type = ECSSettingType_Choice,
        .description = "How focus follows the pointer: click or hover",
        .choices = OPENECS_FOCUS_CHOICES,
    };

    // the questions about unsaved work are dialogs of Input, which Panels cannot call itself
    ECSIPanels_SetAsk(ECSIInput_Ask);
    return ECSISettings_DeclareCore(&focus);
}

void ECSIInput_Terminate(void)
{
    ECSIPanels_SetAsk(nullptr);
    ECSIInput_ForgetClipboard();
    ECSIInput_FreePrefixLines();
    ECSIInput_EndDataDrag();
    ECSIInput_ForgetDropped();

    for (usz i = 0; i < hmlenu(INPUT.keyNames); i++)
    {
        SDL_free(INPUT.keyNames[i].value);
    }

    hmfree(INPUT.keyNames);
    SDL_zero(INPUT);
}

void ECSIInput_SetLogInput(bool log)
{
    INPUT.logInput = log;
}

/// @brief Logs a key or text event as SDL gives it, for --log-input: which key, whether it repeats, and when, so doubled keys or text can be told apart.
static void ECSIInput_LogEvent(const SDL_Event *event)
{
    if (event->type == SDL_EVENT_KEY_DOWN || event->type == SDL_EVENT_KEY_UP)
    {
        SDL_Log("input %.3f ms: key %s '%s'%s, mod 0x%x", (double)event->key.timestamp / 1e6, event->key.down ? "down" : "up", SDL_GetKeyName(event->key.key),
                event->key.repeat ? " repeat" : "", (unsigned)event->key.mod);
    }
    else if (event->type == SDL_EVENT_TEXT_INPUT)
    {
        SDL_Log("input %.3f ms: text '%s'", (double)event->text.timestamp / 1e6, event->text.text != nullptr ? event->text.text : "");
    }
    else if (event->type == SDL_EVENT_TEXT_EDITING)
    {
        SDL_Log("input %.3f ms: editing '%s' at %d", (double)event->edit.timestamp / 1e6, event->edit.text != nullptr ? event->edit.text : "", (int)event->edit.start);
    }
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

    if (INPUT.logInput)
    {
        ECSIInput_LogEvent(event);
    }

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
    {
        // a pop-out window closes its panels, after asking about unsaved work; Cancel keeps it
        bool main = true;
        ECSIRoot *root = ECSIWindow_GetRoot(event->window.windowID, &main);

        if (!main)
        {
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

        // closing the main window quits
        return !ECSIInput_ConfirmQuit();
    }

    case SDL_EVENT_QUIT:
        return !ECSIInput_ConfirmQuit();

    case SDL_EVENT_SYSTEM_THEME_CHANGED:
        ECSISettings_ApplyTheme();
        break;

    // the panel focused last in an OS window gets the focus when the window gets the system's
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
    {
        ECSIWindow_Focused(event->window.windowID);
        ECSIRoot *root = ECSIWindow_GetRoot(event->window.windowID, nullptr);

        if (root != nullptr)
        {
            ECSILayout_FocusRoot(root);
        }

        break;
    }

    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_EXPOSED:
    case SDL_EVENT_WINDOW_SHOWN:
    case SDL_EVENT_WINDOW_RESTORED:
        ECSILayout_RequestFrame();
        break;

    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    {
        const SDL_MouseButtonEvent *button = &event->button;
        INPUT.pointerX = button->x;
        INPUT.pointerY = button->y;

        if (INPUT.prefixActive)
        {
            ECSIInput_EndPrefix();
        }

        // on a tab or grip, the middle button closes the panel and the right button opens its menu; on the rest of a tab row, the right button opens the group's menu
        ECSPanel tab = button->button == SDL_BUTTON_LEFT ? nullptr : ECSIWindow_TabAt(button->x, button->y);
        ECSPanel row = tab == nullptr && button->button == SDL_BUTTON_RIGHT ? ECSIWindow_TabRowAt(button->x, button->y) : nullptr;

        if (row != nullptr)
        {
            ECSIMenus_OpenGroupMenu(row, button->x, button->y);
            break;
        }

        if (tab != nullptr)
        {
            if (button->button == SDL_BUTTON_RIGHT)
            {
                ECSIMenus_OpenPanelMenu(tab, button->x, button->y);
            }
            else if (button->button == SDL_BUTTON_MIDDLE && !ECSILayout_IsLocked(tab))
            {
                (void)ECSLayout_Close(tab);
            }

            break;
        }

        if (button->button == SDL_BUTTON_LEFT && ECSIWindow_PointerDown(button->x, button->y))
        {
            break;
        }

        f32 x = 0.0f;
        f32 y = 0.0f;
        ECSPanel panel = ECSIWindow_PanelAt(button->x, button->y, &x, &y);

        if (panel != nullptr)
        {
            ECSILayout_SetFocus(panel);
            INPUT.pointerPanel = panel;

            ECSIInput_SendPointer(panel, ECSPanelEventType_PointerDown, x, y, button->button);
        }

        break;
    }

    case SDL_EVENT_MOUSE_MOTION:
    {
        const SDL_MouseMotionEvent *motion = &event->motion;
        INPUT.pointerX = motion->x;
        INPUT.pointerY = motion->y;

        // while a panel drags data, the core marks where it can land and the panel gets no moves
        if (INPUT.dragType != nullptr)
        {
            ECSIWindow_ShowDataDrag(INPUT.dragType, motion->x, motion->y);
            break;
        }

        if (ECSIWindow_PointerMove(motion->x, motion->y))
        {
            break;
        }

        // the panel that got the press gets the moves in the OS window of the press, even outside it
        f32 x = motion->x;
        f32 y = motion->y;
        ECSPanel panel = ECSIInput_PointerPanel() != nullptr ? INPUT.pointerPanel : ECSIWindow_PanelAt(motion->x, motion->y, &x, &y);

        if (panel == nullptr)
        {
            break;
        }

        // in hover mode, only real pointer movement over a panel moves focus; dividers and tab rows are outside every panel
        if (INPUT.pointerPanel == nullptr && SDL_strcmp(ECSValue_GetString(ECSSetting_Get("ecs.focus"), ""), "hover") == 0)
        {
            ECSILayout_SetFocus(panel);
        }

        ECSIInput_SendPointer(panel, ECSPanelEventType_PointerMove, x, y, 0);

        break;
    }

    case SDL_EVENT_MOUSE_BUTTON_UP:
    {
        const SDL_MouseButtonEvent *button = &event->button;

        // dragged data lands on the panel under the pointer; the panel it came from still gets its release
        if (INPUT.dragType != nullptr)
        {
            ECSIInput_Drop(button->x, button->y, INPUT.dragType, INPUT.dragValue);
            ECSIInput_EndDataDrag();
        }

        // a click on a grip opens the panel's menu
        ECSPanel clicked = button->button == SDL_BUTTON_LEFT ? ECSIWindow_PointerUp() : nullptr;

        if (clicked != nullptr)
        {
            ECSIMenus_OpenPanelMenu(clicked, button->x, button->y);
        }

        if (ECSIInput_PointerPanel() != nullptr)
        {
            ECSIInput_SendPointer(INPUT.pointerPanel, ECSPanelEventType_PointerUp, button->x, button->y, button->button);
            INPUT.pointerPanel = nullptr;
        }

        break;
    }

    case SDL_EVENT_MOUSE_WHEEL:
    {
        const SDL_MouseWheelEvent *wheel = &event->wheel;

        // over a tab row, the wheel scrolls the tabs; down and right go toward the last tab
        if (ECSIWindow_ScrollTabs(wheel->mouse_x, wheel->mouse_y, (wheel->x - wheel->y) * ECSIInput_GetWheelDirection(wheel)))
        {
            break;
        }

        f32 x = 0.0f;
        f32 y = 0.0f;
        ECSPanel panel = ECSIWindow_PanelAt(wheel->mouse_x, wheel->mouse_y, &x, &y);

        if (panel != nullptr)
        {
            ECSPanelEvent panelWheel = ECSIInput_MakeWheelEvent(wheel, x - panel->x, y - panel->y);
            ECSIPanel_PostEvent(panel, &panelWheel);
        }

        break;
    }

    case SDL_EVENT_KEY_DOWN:
    {
        const SDL_KeyboardEvent *key = &event->key;

        // AltGr is never part of a key combination, so a key pressed with AltGr held matches neither the prefix nor a binding, and goes to the panel as text
        u32 modifiers = ECSIInput_ConvertModifiers(key->mod);

        // a key that the core or a binding uses types no text; only a key the focused panel gets does
        INPUT.swallowText = true;

        if (INPUT.prefixActive)
        {
            if (!ECSIInput_IsModifierKey(key->key) && !key->repeat)
            {
                ECSIInput_RunPrefixKey(key->key, modifiers);
            }

            break;
        }

        if (key->key == SDLK_ESCAPE && INPUT.dragType != nullptr)
        {
            ECSIInput_EndDataDrag();
            break;
        }

        if (key->key == SDLK_ESCAPE && ECSIWindow_CancelDrag())
        {
            break;
        }

        // the prefix is read when a key is pressed, so a changed prefix always wins
        if (ECSIKeys_IsPrefix(key->key, modifiers))
        {
            ECSIInput_StartPrefix();
            break;
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

        break;
    }

    case SDL_EVENT_TEXT_INPUT:
    {
        ECSPanel focus = ECSILayout_GetFocus();

        if (!INPUT.swallowText && focus != nullptr && focus->textInput && event->text.text != nullptr)
        {
            ECSIPanel_PostText(focus, event->text.text, ECSIInput_ConvertModifiers(SDL_GetModState()));
        }

        INPUT.swallowText = false;
        break;
    }

    // files and text dropped from other applications; SDL gives each file, then completes the drop
    case SDL_EVENT_DROP_BEGIN:
        ECSIInput_ForgetDropped();
        break;

    case SDL_EVENT_DROP_FILE:
    {
        char *file = event->drop.data == nullptr ? nullptr : SDL_strdup(event->drop.data);

        if (file != nullptr)
        {
            arrput(INPUT.droppedFiles, file);
        }

        break;
    }

    case SDL_EVENT_DROP_TEXT:
        SDL_free(INPUT.droppedText);
        INPUT.droppedText = event->drop.data == nullptr ? nullptr : SDL_strdup(event->drop.data);
        break;

    case SDL_EVENT_DROP_COMPLETE:
        ECSIInput_CompleteDrop(event->drop.x, event->drop.y);
        break;

    case SDL_EVENT_KEY_UP:
    {
        ECSPanel focus = ECSILayout_GetFocus();

        // a key with Ctrl, Alt or Super types no text, so text after it is not its own, such as an input method's, which comes without a key press
        if ((ECSIInput_ConvertModifiers(event->key.mod) & (ECSModifier_Ctrl | ECSModifier_Alt | ECSModifier_Super)) != 0)
        {
            INPUT.swallowText = false;
        }

        if (focus != nullptr && !INPUT.prefixActive)
        {
            ECSPanelEvent keyUp = ECSIInput_MakeKeyEvent(ECSPanelEventType_KeyUp, &event->key);
            ECSIPanel_PostEvent(focus, &keyUp);
        }

        break;
    }

    default:
        break;
    }

    return true;
}

void ECSIInput_UpdateTextInput(void)
{
    ECSPanel focus = ECSILayout_GetFocus();
    SDL_Window *window = ECSIWindow_GetPanelWindow(focus);
    bool on = focus != nullptr && focus->textInput && focus->fault == nullptr;
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
    if (on)
    {
        area = (SDL_Rect){(int)SDL_roundf(focus->x + focus->textArea.x), (int)SDL_roundf(focus->y + focus->textArea.y), (int)SDL_roundf(focus->textArea.w), (int)SDL_roundf(focus->textArea.h)};
    }

    if (on == INPUT.textOn && (!on || (focus == INPUT.textPanel && SDL_RectsEqual(&area, &INPUT.textArea))))
    {
        return;
    }

    if (on)
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

    INPUT.textOn = on;
    INPUT.textWindow = window;
    INPUT.textPanel = on ? focus : nullptr;
    INPUT.textArea = area;
}

SHUResult ECSPanel_StartDrag(ECSPanel panel, const char *type, const ECSValue *value)
{
    SDL_assert(panel != nullptr);
    SDL_assert(type != nullptr);

    if (ECSIInput_PointerPanel() != panel)
    {
        return SHUResult_Err;
    }

    // a new drag replaces one the panel started before in the same press
    ECSIInput_EndDataDrag();
    INPUT.dragType = SDL_strdup(type);
    SHUResult result = INPUT.dragType == nullptr ? SHUResult_ErrAllocation : ECSValue_Create(&INPUT.dragValue);
    result = result || value == nullptr ? result : ECSIValue_Copy(INPUT.dragValue, value);

    if (result)
    {
        ECSIInput_EndDataDrag();
        return result;
    }

    ECSIWindow_ShowDataDrag(INPUT.dragType, INPUT.pointerX, INPUT.pointerY);
    return SHUResult_Ok;
}

#pragma region Clipboard

SHUResult ECSClipboard_SetText(const char *text)
{
    SDL_assert(text != nullptr);

    ECSIInput_ForgetClipboard();

    if (!SDL_SetClipboardText(text))
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cannot set the clipboard: %s", SDL_GetError());
        return SHUResult_ErrInternal;
    }

    return SHUResult_Ok;
}

const char *ECSClipboard_GetText(void)
{
    ECSIInput_ForgetClipboard();
    INPUT.clipboard = SDL_GetClipboardText();
    return INPUT.clipboard == nullptr ? "" : INPUT.clipboard;
}

SHUResult ECSClipboard_SetData(const char *mimeType, SHUSliceView data)
{
    SDL_assert(mimeType != nullptr);
    SDL_assert(data.data != nullptr || data.size == 0);

    ECSIInput_ForgetClipboard();
    ECSIClipboardData *copy = SDL_malloc(sizeof(ECSIClipboardData) + data.size);
    char *type = SDL_strdup(mimeType);

    if (copy == nullptr || type == nullptr)
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

    if (!SDL_SetClipboardData(ECSIInput_ClipboardProvide, ECSIInput_ClipboardRelease, copy, types, 1))
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cannot set the clipboard: %s", SDL_GetError());
        ECSIInput_ClipboardRelease(copy);
        return SHUResult_ErrInternal;
    }

    return SHUResult_Ok;
}

SHUResult ECSClipboard_GetData(const char *mimeType, SHUSlice *retData)
{
    SDL_assert(mimeType != nullptr);
    SDL_assert(retData != nullptr);

    ECSIInput_ForgetClipboard();
    usz size = 0;
    INPUT.clipboard = SDL_HasClipboardData(mimeType) ? SDL_GetClipboardData(mimeType, &size) : nullptr;
    *retData = cs(INPUT.clipboard, INPUT.clipboard == nullptr ? 0 : size);
    return INPUT.clipboard == nullptr ? SHUResult_ErrNotFound : SHUResult_Ok;
}

#pragma endregion Clipboard

#pragma region Dialogs

SHUResult ECSDialog_Show(ECSPlugin plugin, const ECSDialogDesc *desc)
{
    SDL_assert(plugin != nullptr);
    return ECSIInput_ShowDialog(plugin, desc);
}

SHUResult ECSIInput_ShowDialog(ECSPlugin plugin, const ECSDialogDesc *desc)
{
    SDL_assert(desc != nullptr && desc->Done != nullptr);
    SDL_assert(desc->filters != nullptr || desc->filterCount == 0);

    ECSIDialog *dialog = SDL_calloc(1, sizeof(ECSIDialog));

    if (dialog == nullptr)
    {
        return SHUResult_ErrAllocation;
    }

    dialog->plugin = plugin;
    dialog->Done = desc->Done;
    dialog->data = desc->data;

    for (usz i = 0; i < desc->filterCount; i++)
    {
        SDL_DialogFileFilter filter = {ECSIInput_DialogText(dialog, desc->filters[i].name), ECSIInput_DialogText(dialog, desc->filters[i].pattern)};
        arrput(dialog->filters, filter);
    }

    const char *location = ECSIInput_DialogText(dialog, desc->location);

    if (dialog->failed)
    {
        ECSIInput_FreeDialog(dialog);
        return SHUResult_ErrAllocation;
    }

    SDL_Window *window = ECSIWindow_GetMain();
    int filterCount = (int)arrlenu(dialog->filters);

    switch (desc->type)
    {
    case ECSDialogType_OpenFile:
        SDL_ShowOpenFileDialog(ECSIInput_DialogAnswer, dialog, window, dialog->filters, filterCount, location, desc->many);
        break;
    case ECSDialogType_SaveFile:
        SDL_ShowSaveFileDialog(ECSIInput_DialogAnswer, dialog, window, dialog->filters, filterCount, location);
        break;
    case ECSDialogType_OpenFolder:
        SDL_ShowOpenFolderDialog(ECSIInput_DialogAnswer, dialog, window, location, desc->many);
        break;
    }

    return SHUResult_Ok;
}

/// @brief Handles a key press in a message dialog: Left, Right and Tab choose a button, Return and Space press the chosen one, and Escape presses the last.
/// @return The pressed button, or -1 while the dialog waits.
static i32 ECSIInput_DialogKey(ECSIDialogView *dialog, const SDL_KeyboardEvent *key)
{
    bool back = key->key == SDLK_LEFT || (key->key == SDLK_TAB && (key->mod & SDL_KMOD_SHIFT) != 0);
    bool forward = key->key == SDLK_RIGHT || (key->key == SDLK_TAB && !back);

    if (key->key == SDLK_RETURN || key->key == SDLK_KP_ENTER || key->key == SDLK_SPACE)
    {
        return (i32)dialog->selected;
    }

    if (key->key == SDLK_ESCAPE)
    {
        return (i32)dialog->count - 1;
    }

    if (back || forward)
    {
        dialog->selected = (dialog->selected + (back ? dialog->count - 1 : 1)) % dialog->count;
    }

    return -1;
}

/// @brief Handles an event while a message dialog waits.
/// @param window The SDL id of the dialog's OS window.
/// @param pressedButton The button the left button was pressed on, or -1; a click is a press and a release on the same button.
/// @param keptEvents stb_ds array of the OS windows' events, such as resizes, which the main loop gets after the answer; the rest of the input is for the dialog only, so it is dropped.
/// @return The pressed button, or -1 while the dialog waits.
static i32 ECSIInput_DialogEvent(ECSIDialogView *dialog, const SDL_Event *event, SDL_WindowID window, i32 *pressedButton, SDL_Event **keptEvents)
{
    i32 last = (i32)dialog->count - 1;

    switch (event->type)
    {
    case SDL_EVENT_KEY_DOWN:
        return ECSIInput_DialogKey(dialog, &event->key);

    case SDL_EVENT_MOUSE_MOTION:
        dialog->hover = event->motion.windowID == window ? ECSIWindow_DialogButtonAt(event->motion.x, event->motion.y) : -1;
        return -1;

    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        *pressedButton = event->button.windowID == window && event->button.button == SDL_BUTTON_LEFT ? ECSIWindow_DialogButtonAt(event->button.x, event->button.y) : -1;
        return -1;

    case SDL_EVENT_MOUSE_BUTTON_UP:
    {
        bool click = event->button.windowID == window && event->button.button == SDL_BUTTON_LEFT && *pressedButton >= 0 && ECSIWindow_DialogButtonAt(event->button.x, event->button.y) == *pressedButton;
        i32 button = click ? *pressedButton : -1;
        *pressedButton = -1;
        return button;
    }

    // the system's request to quit, and closing the dialog's OS window, answer as Escape does
    case SDL_EVENT_QUIT:
        return last;

    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        return event->window.windowID == window ? last : -1;

    default:
        if (event->type >= SDL_EVENT_WINDOW_FIRST && event->type <= SDL_EVENT_WINDOW_LAST)
        {
            arrput(*keptEvents, *event);
        }

        return -1;
    }
}

void ECSIInput_SetDialogTest(ECSIDialogStepFunction Step, ECSIDialogWaitFunction Wait)
{
    INPUT.TestStep = Step;
    INPUT.TestWait = Wait;
}

const ECSIDialogView *ECSIInput_GetDialog(void)
{
    return INPUT.dialog;
}

i32 ECSIInput_Ask(const char *title, const char *message, const char *const *buttons, usz count)
{
    SDL_assert(title != nullptr && message != nullptr && buttons != nullptr);

    // one dialog shows at a time
    if (count == 0 || INPUT.dialog != nullptr)
    {
        return -1;
    }

    ECSIDialogView dialog = {.title = title, .message = message, .buttons = buttons, .count = count, .hover = -1};
    SDL_WindowID window = ECSIWindow_ShowDialog(&dialog);

    if (window == 0)
    {
        return -1;
    }

    // the core draws the dialog and waits here, as a system dialog would, but needs no other program: on Wayland, SDL's system dialogs need zenity, whose window a compositor may never show
    INPUT.dialog = &dialog;
    SDL_Event *keptEvents = nullptr; // stb_ds array of the OS windows' events, which the main loop gets after the answer
    i32 answer = -1;
    i32 pressedButton = -1;

    while (answer < 0)
    {
        ECSIWindow_RenderDialog();

        // a running test goes on, so it can read the dialog with test.dialog and answer it with keys or clicks
        if (INPUT.TestStep != nullptr && !INPUT.TestStep())
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "The test cannot answer the dialog '%s', so it is answered with '%s'.", title, buttons[count - 1]);
            answer = (i32)count - 1;
            break;
        }

        // every waiting event is handled before the next step, as in the main loop: a wait without time stops at the end of SDL's poll cycle
        SDL_Event event;

        for (bool waiting = SDL_WaitEventTimeout(&event, INPUT.TestWait != nullptr ? INPUT.TestWait() : -1); waiting && answer < 0; waiting = SDL_PollEvent(&event))
        {
            answer = ECSIInput_DialogEvent(&dialog, &event, window, &pressedButton, &keptEvents);
        }
    }

    ECSIWindow_ShowDialog(nullptr);
    INPUT.dialog = nullptr;

    for (usz i = 0; i < arrlenu(keptEvents); i++)
    {
        SDL_PushEvent(&keptEvents[i]);
    }

    arrfree(keptEvents);
    return answer;
}

SHUResult ECSDialog_ShowMessage(const char *title, const char *message, const char *const *buttons, usz buttonCount, usz *retButton)
{
    SDL_assert(title != nullptr && message != nullptr && retButton != nullptr);
    SDL_assert(buttons != nullptr && buttonCount > 0);

    i32 button = ECSIInput_Ask(title, message, buttons, buttonCount);

    if (button < 0)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cannot show the message dialog '%s'.", title);
        return SHUResult_ErrInternal;
    }

    *retButton = (usz)button;
    return SHUResult_Ok;
}

#pragma endregion Dialogs
