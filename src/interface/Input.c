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
#define OPENECS_WORKSPACE_FUNCTION "ecs.workspace.switch"

/// @brief Choices of the setting ecs.focus; its value comes from the core's settings file.
static const char *const OPENECS_FOCUS_CHOICES[] = {"click", "hover", NULL};

/// @brief Headings of the sections of the list of prefix keys, in the order ECSIInput_SectionOf numbers them.
static const char *const OPENECS_PREFIX_SECTIONS[] = {"Navigation", "Panel", "More"};

/// @brief Functions that the list of prefix keys shows on one line when they run on the four arrows: left, right, up and down.
static const struct
{
    const char *functions[4];
    const char *description;
} OPENECS_ARROW_FAMILIES[] = {
    {{"ecs.layout.focusLeft", "ecs.layout.focusRight", "ecs.layout.focusUp", "ecs.layout.focusDown"}, "Focus the panel in that direction"},
    {{"ecs.layout.moveLeft", "ecs.layout.moveRight", "ecs.layout.moveUp", "ecs.layout.moveDown"}, "Move the panel in that direction"},
};

static struct
{
    bool prefixActive;
    const char **prefixLines; // stb_ds array of the lines shown after the prefix: key text, description, and so on; a NULL key text makes a heading
    char **prefixTexts;       // stb_ds array of the key texts made for the lines, such as "1...0"
    ECSPanel pointerPanel;    // panel that got the press; it gets pointer events until the release
    f32 pointerX;             // the pointer's last position in the OS window, in layout units
    f32 pointerY;
    void *clipboard;          // what a clipboard getter returned last, freed by the next call
    char *dragType;           // type of the data a panel drags, or NULL
    ECSValue *dragValue;      // the dragged data
    char **droppedFiles;      // stb_ds array of the files another application drops, until the drop completes
    char *droppedText;        // the text another application drops, until the drop completes, or NULL
} INPUT = {0};

/// @brief A file dialog waiting for its answer, with copies of everything SDL reads until it answers.
typedef struct ECSIDialog
{
    ECSPlugin plugin; // NULL for the core
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
static u32 ECSIInput_Modifiers(SDL_Keymod modifiers)
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
        .modifiers = ECSIInput_Modifiers(SDL_GetModState()),
        .pointer = {.x = x - panel->x, .y = y - panel->y, .button = button},
    };

    ECSIPanel_PostEvent(panel, &event);
}

/// @brief Sends a wheel event to a panel. The amount is turned back if the system flips the wheel, so positive is always away from the user.
static void ECSIInput_SendWheel(ECSPanel panel, const SDL_MouseWheelEvent *wheel)
{
    f32 direction = wheel->direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f;
    ECSPanelEvent event = {
        .type = ECSPanelEventType_Wheel,
        .modifiers = ECSIInput_Modifiers(SDL_GetModState()),
        .wheel = {.x = wheel->mouse_x - panel->x, .y = wheel->mouse_y - panel->y, .amountX = wheel->x * direction, .amountY = wheel->y * direction},
    };

    ECSIPanel_PostEvent(panel, &event);
}

static void ECSIInput_SendKey(ECSPanel panel, ECSPanelEventType type, const SDL_KeyboardEvent *key)
{
    ECSPanelEvent event = {
        .type = type,
        .modifiers = ECSIInput_Modifiers(key->mod),
        .key = {.code = key->key},
    };

    ECSIPanel_PostEvent(panel, &event);
}

/// @brief Moves the keyboard focus to a panel; the layout tells both panels.
static void ECSIInput_Focus(ECSPanel panel)
{
    if (panel != NULL)
    {
        ECSILayout_SetFocus(panel);
    }
}

/// @brief Gets the panel that got the pointer press, unless code closed it since.
static ECSPanel ECSIInput_PointerPanel(void)
{
    if (INPUT.pointerPanel != NULL && !ECSILayout_HasPanel(INPUT.pointerPanel))
    {
        INPUT.pointerPanel = NULL;
    }

    return INPUT.pointerPanel;
}

/// @brief Ends dragging data, with or without dropping it.
static void ECSIInput_EndDataDrag(void)
{
    SDL_free(INPUT.dragType);
    ECSValue_Destroy(&INPUT.dragValue);
    INPUT.dragType = NULL;
    ECSIWindow_ShowDataDrag(NULL, 0.0f, 0.0f);
}

/// @brief Drops data on the panel at a position, if the panel accepts its type.
static void ECSIInput_Drop(f32 x, f32 y, const char *type, const ECSValue *value)
{
    ECSPanel panel = ECSILayout_PanelAt(x, y);

    if (panel != NULL && ECSIPanel_Accepts(panel, type))
    {
        ECSIPanel_PostDrop(panel, x - panel->x, y - panel->y, type, value, ECSIInput_Modifiers(SDL_GetModState()));
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
    INPUT.droppedText = NULL;
}

/// @brief Delivers what another application dropped: its files as a file-list, and its text as text.
static void ECSIInput_CompleteDrop(f32 x, f32 y)
{
    ECSValue *value = NULL;

    if (arrlenu(INPUT.droppedFiles) > 0 && ECSValue_Create(&value) == SHUResult_Ok)
    {
        SHUResult result = SHUResult_Ok;
        ECSValue_SetTable(value);

        for (usz i = 0; !result && i < arrlenu(INPUT.droppedFiles); i++)
        {
            ECSValue *item = NULL;
            result = ECSValue_ListAddItem(value, &item);
            result = result ? result : ECSValue_SetString(item, INPUT.droppedFiles[i]);
        }

        if (!result)
        {
            ECSIInput_Drop(x, y, "file-list", value);
        }

        ECSValue_Destroy(&value);
    }

    if (INPUT.droppedText != NULL && ECSValue_Create(&value) == SHUResult_Ok)
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
static usz ECSIInput_SectionOf(const char *function)
{
    const char *navigation[] = {"ecs.layout.focus", "ecs.layout.move", "ecs.layout.nextTab", OPENECS_WORKSPACE_FUNCTION};

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
static i64 ECSIInput_FindPrefixKey(const char *function, u32 key, u32 modifiers, const bool *listed)
{
    const ECSIKeyBinding *keys = ECSIKeys_GetPrefixKeys();

    for (usz i = 0; i < arrlenu(keys); i++)
    {
        const ECSIKeyBinding *binding = &keys[i];

        if (!listed[i] && binding->key == key && binding->modifiers == modifiers && SDL_strcmp(binding->function, function) == 0)
        {
            return (i64)i;
        }
    }

    return -1;
}

/// @brief Folds a prefix key into one line with its family, when the family's four functions run on the four arrows with the same modifiers.
/// @return The line's key text, such as "Shift+Arrows", made for the list; or NULL if the key does not fold.
static char *ECSIInput_FoldArrows(usz index, bool *listed, const char **retDescription)
{
    const ECSIKeyBinding *binding = &ECSIKeys_GetPrefixKeys()[index];
    const SDL_Keycode arrows[] = {SDLK_LEFT, SDLK_RIGHT, SDLK_UP, SDLK_DOWN};

    for (usz family = 0; family < SDL_arraysize(OPENECS_ARROW_FAMILIES); family++)
    {
        i64 members[4] = {0};
        bool complete = true;

        for (usz i = 0; i < 4; i++)
        {
            members[i] = ECSIInput_FindPrefixKey(OPENECS_ARROW_FAMILIES[family].functions[i], arrows[i], binding->modifiers, listed);
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

        *retDescription = OPENECS_ARROW_FAMILIES[family].description;
        return text;
    }

    return NULL;
}

/// @brief Folds every key that switches workspaces into one line, from the first workspace's key to the last one's, such as "1...0".
/// @return The line's key text, made for the list.
static char *ECSIInput_FoldWorkspaces(bool *listed)
{
    const char *firstKey = NULL;
    const char *lastKey = NULL;
    i64 first = 0;
    i64 last = 0;

    const ECSIKeyBinding *keys = ECSIKeys_GetPrefixKeys();

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

/// @brief Starts or ends waiting for the key after the prefix, and shows or hides the keys with what they do now, in sections.
static void ECSIInput_SetPrefix(bool active)
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
        const ECSIKeyBinding *keys = ECSIKeys_GetPrefixKeys();
        ECSPanel focus = ECSILayout_GetFocus();
        bool *listed = NULL;
        arrsetlen(listed, arrlenu(keys));
        SDL_memset(listed, 0, arrlenu(listed) * sizeof(bool));

        // the keys are listed in the order of the core's functions, whatever order the settings files give them in
        usz *order = NULL;
        arrsetlen(order, arrlenu(keys));

        for (usz i = 0; i < arrlenu(order); i++)
        {
            order[i] = i;
        }

        SDL_qsort_r(order, arrlenu(order), sizeof(usz), ECSIInput_CompareKeys, (void *)keys);

        for (usz section = 0; section < SDL_arraysize(OPENECS_PREFIX_SECTIONS); section++)
        {
            // a section without keys has no heading
            usz start = arrlenu(INPUT.prefixLines);
            arrput(INPUT.prefixLines, NULL);
            arrput(INPUT.prefixLines, OPENECS_PREFIX_SECTIONS[section]);

            for (usz position = 0; position < arrlenu(order); position++)
            {
                usz i = order[position];
                const char *function = keys[i].function;

                if (listed[i] || ECSIInput_SectionOf(function) != section || !ECSIMenus_Offers(function, focus))
                {
                    continue;
                }

                const char *description = NULL;
                bool workspace = SDL_strncmp(function, OPENECS_WORKSPACE_FUNCTION, SDL_strlen(OPENECS_WORKSPACE_FUNCTION)) == 0;
                char *text = workspace ? ECSIInput_FoldWorkspaces(listed) : ECSIInput_FoldArrows(i, listed, &description);

                if (text != NULL)
                {
                    arrput(INPUT.prefixTexts, text);
                }

                arrput(INPUT.prefixLines, text != NULL ? text : keys[i].text);
                arrput(INPUT.prefixLines, workspace ? "Switch to workspace" : description != NULL ? description
                                                                                                  : ECSIMenus_LabelOf(function, focus));
            }

            if (arrlenu(INPUT.prefixLines) == start + 2)
            {
                arrsetlen(INPUT.prefixLines, start);
            }
        }

        arrfree(listed);
        arrfree(order);
        arrput(INPUT.prefixLines, "Escape");
        arrput(INPUT.prefixLines, "Cancel");
    }

    ECSIWindow_ShowPrefixKeys(INPUT.prefixLines, arrlenu(INPUT.prefixLines) / 2);
}

/// @brief Checks the modifiers of a key press against a binding's. AltGr is never part of a binding.
static bool ECSIInput_ModifiersMatch(u32 modifiers, u32 expected)
{
    return (modifiers & ECSModifier_AltGr) == 0 && modifiers == expected;
}

/// @brief Runs the function of the key pressed after the core prefix. Escape cancels.
static void ECSIInput_RunPrefixKey(SDL_Keycode key, u32 modifiers)
{
    ECSIInput_SetPrefix(false);

    const ECSIKeyBinding *keys = ECSIKeys_GetPrefixKeys();

    for (usz i = 0; key != SDLK_ESCAPE && i < arrlenu(keys); i++)
    {
        if (keys[i].key == key && ECSIInput_ModifiersMatch(modifiers, keys[i].modifiers))
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
        return NULL;
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
    INPUT.clipboard = NULL;
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
    char *copy = text == NULL ? NULL : SDL_strdup(text);

    if (copy != NULL)
    {
        arrput(dialog->texts, copy);
    }

    dialog->failed = dialog->failed || (text != NULL && copy == NULL);
    return copy;
}

/// @brief Gives a dialog's answer to its plugin, on the main thread.
static void ECSIInput_DialogFinish(void *data)
{
    ECSIDialog *dialog = data;
    usz count = arrlenu(dialog->files);
    dialog->Done(dialog->data, count == 0 ? NULL : (const char *const *)dialog->files, count);
    ECSIInput_FreeDialog(dialog);
}

/// @brief Takes SDL's answer, maybe on another thread, and sends it to the main thread.
static void SDLCALL ECSIInput_DialogAnswer(void *userData, const char *const *files, int filter)
{
    (void)filter;
    ECSIDialog *dialog = userData;

    if (files == NULL)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "A file dialog of %s%s failed: %s", dialog->plugin == NULL ? "the core" : "plugin ", dialog->plugin == NULL ? "" : ECSIPlugin_GetName(dialog->plugin), SDL_GetError());
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

    return ECSISettings_DeclareCore(&focus);
}

void ECSIInput_Terminate(void)
{
    ECSIInput_ForgetClipboard();
    arrfree(INPUT.prefixLines);

    for (usz i = 0; i < arrlenu(INPUT.prefixTexts); i++)
    {
        SDL_free(INPUT.prefixTexts[i]);
    }

    arrfree(INPUT.prefixTexts);
    ECSIInput_EndDataDrag();
    ECSIInput_ForgetDropped();
    SDL_zero(INPUT);
}

bool ECSIInput_Handle(const SDL_Event *event)
{
    SDL_assert(event != NULL);

    if (ECSIMenus_Handle(event))
    {
        return true;
    }

    switch (event->type)
    {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
    {
        // the user may cancel quitting to keep unsaved work
        ECSPanel *panels = ECSILayout_GetPanels();
        bool quit = ECSIPanels_ConfirmClose(panels, arrlenu(panels), true);
        arrfree(panels);
        return !quit;
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
            ECSIInput_SetPrefix(false);
        }

        // on a tab or grip, the middle button closes the panel and the right button opens its menu; on the rest of a tab row, the right button opens the group's menu
        ECSPanel tab = button->button == SDL_BUTTON_LEFT ? NULL : ECSIWindow_TabAt(button->x, button->y);
        ECSPanel row = tab == NULL && button->button == SDL_BUTTON_RIGHT ? ECSIWindow_TabRowAt(button->x, button->y) : NULL;

        if (row != NULL)
        {
            ECSIMenus_Open(row, button->x, button->y, true);
            break;
        }

        if (tab != NULL)
        {
            if (button->button == SDL_BUTTON_RIGHT)
            {
                ECSIMenus_Open(tab, button->x, button->y, false);
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

        ECSPanel panel = ECSILayout_PanelAt(button->x, button->y);

        if (panel != NULL)
        {
            ECSIInput_Focus(panel);
            INPUT.pointerPanel = panel;

            ECSIInput_SendPointer(panel, ECSPanelEventType_PointerDown, button->x, button->y, button->button);
        }

        break;
    }

    case SDL_EVENT_MOUSE_MOTION:
    {
        const SDL_MouseMotionEvent *motion = &event->motion;
        INPUT.pointerX = motion->x;
        INPUT.pointerY = motion->y;

        // while a panel drags data, the core marks where it can land and the panel gets no moves
        if (INPUT.dragType != NULL)
        {
            ECSIWindow_ShowDataDrag(INPUT.dragType, motion->x, motion->y);
            break;
        }

        if (ECSIWindow_PointerMove(motion->x, motion->y))
        {
            break;
        }

        ECSPanel panel = ECSIInput_PointerPanel() != NULL ? INPUT.pointerPanel : ECSILayout_PanelAt(motion->x, motion->y);

        if (panel == NULL)
        {
            break;
        }

        // in hover mode, only real pointer movement over a panel moves focus; dividers and tab rows are outside every panel
        if (INPUT.pointerPanel == NULL && SDL_strcmp(ECSValue_GetString(ECSSetting_Get("ecs.focus"), ""), "hover") == 0)
        {
            ECSIInput_Focus(panel);
        }

        ECSIInput_SendPointer(panel, ECSPanelEventType_PointerMove, motion->x, motion->y, 0);

        break;
    }

    case SDL_EVENT_MOUSE_BUTTON_UP:
    {
        const SDL_MouseButtonEvent *button = &event->button;

        // dragged data lands on the panel under the pointer; the panel it came from still gets its release
        if (INPUT.dragType != NULL)
        {
            ECSIInput_Drop(button->x, button->y, INPUT.dragType, INPUT.dragValue);
            ECSIInput_EndDataDrag();
        }

        // a click on a grip opens the panel's menu
        ECSPanel clicked = button->button == SDL_BUTTON_LEFT ? ECSIWindow_PointerUp() : NULL;

        if (clicked != NULL)
        {
            ECSIMenus_Open(clicked, button->x, button->y, false);
        }

        if (ECSIInput_PointerPanel() != NULL)
        {
            ECSIInput_SendPointer(INPUT.pointerPanel, ECSPanelEventType_PointerUp, button->x, button->y, button->button);
            INPUT.pointerPanel = NULL;
        }

        break;
    }

    case SDL_EVENT_MOUSE_WHEEL:
    {
        const SDL_MouseWheelEvent *wheel = &event->wheel;

        // over a tab row, the wheel scrolls the tabs; down and right go toward the last tab
        f32 direction = wheel->direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f;

        if (ECSIWindow_ScrollTabs(wheel->mouse_x, wheel->mouse_y, (wheel->x - wheel->y) * direction))
        {
            break;
        }

        ECSPanel panel = ECSILayout_PanelAt(wheel->mouse_x, wheel->mouse_y);

        if (panel != NULL)
        {
            ECSIInput_SendWheel(panel, wheel);
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
                ECSIInput_RunPrefixKey(key->key, ECSIInput_Modifiers(key->mod));
            }

            break;
        }

        if (key->key == SDLK_ESCAPE && INPUT.dragType != NULL)
        {
            ECSIInput_EndDataDrag();
            break;
        }

        if (key->key == SDLK_ESCAPE && ECSIWindow_CancelDrag())
        {
            break;
        }

        // the prefix is read when a key is pressed, so a changed prefix always wins
        if (ECSIKeys_IsPrefix(key->key, ECSIInput_Modifiers(key->mod)))
        {
            ECSIInput_SetPrefix(true);
            break;
        }

        // a binding wins over the focused panel's own handling of the key
        ECSPanel focus = ECSILayout_GetFocus();
        bool modifierKey = (key->key >= SDLK_LCTRL && key->key <= SDLK_RGUI) || key->key == SDLK_MODE;
        u32 modifiers = ECSIInput_Modifiers(key->mod);
        const char *function = modifierKey || (modifiers & ECSModifier_AltGr) != 0 ? NULL : ECSIKeys_Find(key->key, modifiers, focus);

        if (function != NULL)
        {
            ECSIServices_CallBound(function, focus);
        }
        else if (focus != NULL)
        {
            ECSIInput_SendKey(focus, ECSPanelEventType_KeyDown, key);
        }

        break;
    }

    // files and text dropped from other applications; SDL gives each file, then completes the drop
    case SDL_EVENT_DROP_BEGIN:
        ECSIInput_ForgetDropped();
        break;

    case SDL_EVENT_DROP_FILE:
    {
        char *file = event->drop.data == NULL ? NULL : SDL_strdup(event->drop.data);

        if (file != NULL)
        {
            arrput(INPUT.droppedFiles, file);
        }

        break;
    }

    case SDL_EVENT_DROP_TEXT:
        SDL_free(INPUT.droppedText);
        INPUT.droppedText = event->drop.data == NULL ? NULL : SDL_strdup(event->drop.data);
        break;

    case SDL_EVENT_DROP_COMPLETE:
        ECSIInput_CompleteDrop(event->drop.x, event->drop.y);
        break;

    case SDL_EVENT_KEY_UP:
    {
        ECSPanel focus = ECSILayout_GetFocus();

        if (focus != NULL && !INPUT.prefixActive)
        {
            ECSIInput_SendKey(focus, ECSPanelEventType_KeyUp, &event->key);
        }

        break;
    }

    default:
        break;
    }

    return true;
}

SHUResult ECSPanel_StartDrag(ECSPanel panel, const char *type, const ECSValue *value)
{
    SDL_assert(panel != NULL);
    SDL_assert(type != NULL);

    if (ECSIInput_PointerPanel() != panel)
    {
        return SHUResult_Err;
    }

    // a new drag replaces one the panel started before in the same press
    ECSIInput_EndDataDrag();
    INPUT.dragType = SDL_strdup(type);
    SHUResult result = INPUT.dragType == NULL ? SHUResult_ErrAllocation : ECSValue_Create(&INPUT.dragValue);
    result = result || value == NULL ? result : ECSIValue_Copy(INPUT.dragValue, value);

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
    SDL_assert(text != NULL);

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
    return INPUT.clipboard == NULL ? "" : INPUT.clipboard;
}

SHUResult ECSClipboard_SetData(const char *mimeType, SHUSliceView data)
{
    SDL_assert(mimeType != NULL);
    SDL_assert(data.data != NULL || data.size == 0);

    ECSIInput_ForgetClipboard();
    ECSIClipboardData *copy = SDL_malloc(sizeof(ECSIClipboardData) + data.size);
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
    SDL_assert(mimeType != NULL);
    SDL_assert(retData != NULL);

    ECSIInput_ForgetClipboard();
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
    return ECSIInput_ShowDialog(plugin, desc);
}

SHUResult ECSIInput_ShowDialog(ECSPlugin plugin, const ECSDialogDesc *desc)
{
    SDL_assert(desc != NULL && desc->Done != NULL);
    SDL_assert(desc->filters != NULL || desc->filterCount == 0);

    ECSIDialog *dialog = SDL_calloc(1, sizeof(ECSIDialog));

    if (dialog == NULL)
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
        .window = ECSIWindow_GetMain(),
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
