#include "interface/Input.h"

#include "interface/Keys.h"
#include "interface/Layout.h"
#include "interface/Menus.h"
#include "interface/Panels.h"
#include "interface/Window.h"
#include "runtime/Services.h"
#include "runtime/Settings.h"

#include "SDL3/SDL.h"
#include "stb/stbSDL3.h"

#pragma region Source Only

/// @brief Start of the names of the functions that switch workspaces; the workspace's number follows.
#define OPENECS_WORKSPACE_FUNCTION "ecs.workspace_"

/// @brief Choices of the setting ecs.focus; the first is the default.
static const char *const ECSI_FOCUS_CHOICES[] = {"click", "hover", NULL};

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

static struct
{
    bool prefixActive;
    const char **prefixLines; // stb_ds array of the lines shown after the prefix: key text, description, and so on; a NULL key text makes a heading
    char **prefixTexts;       // stb_ds array of the key texts made for the lines, such as "1...0"
    ECSPanel pointerPanel;    // panel that got the press; it gets pointer events until the release
    void *clipboard;          // what a clipboard getter returned last, freed by the next call
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
    const ECSI_KeyBinding *keys = ECSI_KeysGetPrefixKeys();

    for (usz i = 0; i < arrlenu(keys); i++)
    {
        const ECSI_KeyBinding *binding = &keys[i];

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
    const ECSI_KeyBinding *binding = &ECSI_KeysGetPrefixKeys()[index];
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

    const ECSI_KeyBinding *keys = ECSI_KeysGetPrefixKeys();

    for (usz i = 0; i < arrlenu(keys); i++)
    {
        const char *function = keys[i].function;

        if (SDL_strncmp(function, OPENECS_WORKSPACE_FUNCTION, SDL_strlen(OPENECS_WORKSPACE_FUNCTION)) != 0)
        {
            continue;
        }

        i64 number = SDL_strtoll(function + SDL_strlen(OPENECS_WORKSPACE_FUNCTION), NULL, 10);
        listed[i] = true;

        if (firstKey == NULL || number < first)
        {
            firstKey = keys[i].text;
            first = number;
        }

        if (lastKey == NULL || number > last)
        {
            lastKey = keys[i].text;
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
        const ECSI_KeyBinding *keys = ECSI_KeysGetPrefixKeys();
        ECSPanel focus = ECSI_LayoutGetFocus();
        bool *listed = NULL;
        arrsetlen(listed, arrlenu(keys));
        SDL_memset(listed, 0, arrlenu(listed) * sizeof(bool));

        for (usz section = 0; section < SDL_arraysize(ECSI_PREFIX_SECTIONS); section++)
        {
            // a section without keys has no heading
            usz start = arrlenu(INPUT.prefixLines);
            arrput(INPUT.prefixLines, NULL);
            arrput(INPUT.prefixLines, ECSI_PREFIX_SECTIONS[section]);

            for (usz i = 0; i < arrlenu(keys); i++)
            {
                const char *function = keys[i].function;

                if (listed[i] || ECSI_InputSectionOf(function) != section || !ECSI_MenusOffers(function, focus))
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

                arrput(INPUT.prefixLines, text != NULL ? text : keys[i].text);
                arrput(INPUT.prefixLines, workspace ? "Switch to workspace" : description != NULL ? description
                                                                                                  : ECSI_MenusLabelOf(function, focus));
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

    ECSI_WindowShowPrefixKeys(INPUT.prefixLines, arrlenu(INPUT.prefixLines) / 2);
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

    const ECSI_KeyBinding *keys = ECSI_KeysGetPrefixKeys();

    for (usz i = 0; key != SDLK_ESCAPE && i < arrlenu(keys); i++)
    {
        if (keys[i].key == key && ECSI_InputModifiersMatch(modifiers, keys[i].modifiers))
        {
            ECSI_ServicesCallBound(keys[i].function, ECSI_LayoutGetFocus());
            return;
        }
    }
}

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
    ECSSettingDesc focus = {
        .name = "ecs.focus",
        .type = ECSSettingType_Choice,
        .description = "How focus follows the pointer: click or hover",
        .choices = ECSI_FOCUS_CHOICES,
    };

    return ECSI_SettingsDeclareCore(&focus);
}

void ECSI_InputTerminate(void)
{
    ECSI_InputForgetClipboard();
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

    if (ECSI_MenusHandle(event))
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
        ECSPanel tab = button->button == SDL_BUTTON_LEFT ? NULL : ECSI_WindowTabAt(button->x, button->y);
        ECSPanel row = tab == NULL && button->button == SDL_BUTTON_RIGHT ? ECSI_WindowTabRowAt(button->x, button->y) : NULL;

        if (row != NULL)
        {
            ECSI_MenusOpen(row, button->x, button->y, true);
            break;
        }

        if (tab != NULL)
        {
            if (button->button == SDL_BUTTON_RIGHT)
            {
                ECSI_MenusOpen(tab, button->x, button->y, false);
            }
            else if (button->button == SDL_BUTTON_MIDDLE && !ECSI_LayoutIsLocked(tab))
            {
                (void)ECSLayout_Close(tab);
            }

            break;
        }

        if (button->button == SDL_BUTTON_LEFT && ECSI_WindowPointerDown(button->x, button->y))
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

        if (ECSI_WindowPointerMove(motion->x, motion->y))
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
        ECSPanel clicked = button->button == SDL_BUTTON_LEFT ? ECSI_WindowPointerUp() : NULL;

        if (clicked != NULL)
        {
            ECSI_MenusOpen(clicked, button->x, button->y, false);
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

        if (ECSI_WindowScrollTabs(wheel->mouse_x, wheel->mouse_y, (wheel->x - wheel->y) * direction))
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

        if (key->key == SDLK_ESCAPE && ECSI_WindowCancelDrag())
        {
            break;
        }

        // the prefix is read when a key is pressed, so a changed prefix always wins
        if (ECSI_KeysIsPrefix(key->key, ECSI_InputModifiers(key->mod)))
        {
            ECSI_InputSetPrefix(true);
            break;
        }

        // a binding wins over the focused panel's own handling of the key
        ECSPanel focus = ECSI_LayoutGetFocus();
        bool modifierKey = (key->key >= SDLK_LCTRL && key->key <= SDLK_RGUI) || key->key == SDLK_MODE;
        u32 modifiers = ECSI_InputModifiers(key->mod);
        const char *function = modifierKey || (modifiers & ECSModifier_AltGr) != 0 ? NULL : ECSI_KeysFind(key->key, modifiers, focus);

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

    SDL_Window *window = ECSI_WindowGetMain();
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
        .window = ECSI_WindowGetMain(),
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
