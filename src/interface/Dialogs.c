#include "interface/Dialogs.h"

#include "interface/Panels.h"
#include "runtime/Plugins.h"

#include "SDL3/SDL.h"
#include "stb/stbSDL3.h"

#pragma region Source Only

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

static struct
{
    const ECSIDialogView *dialog;    // the message dialog that waits for an answer, or nullptr
    ECSIDialogStepFunction TestStep; // a running test's step while a dialog waits, or nullptr
    ECSIDialogWaitFunction TestWait;
} DIALOGS = {0};

static void ECSIDialogs_Free(ECSIDialog *dialog)
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
static const char *ECSIDialogs_KeepText(ECSIDialog *dialog, const char *text)
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
static void ECSIDialogs_Finish(void *data)
{
    ECSIDialog *dialog = data;
    usz count = arrlenu(dialog->files);
    dialog->Done(dialog->data, count == 0 ? nullptr : (const char *const *)dialog->files, count);
    ECSIDialogs_Free(dialog);
}

/// @brief Takes SDL's answer, maybe on another thread, and sends it to the main thread.
static void SDLCALL ECSIDialogs_Answer(void *userData, const char *const *files, int filter)
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

    if (ECS_RunOnMainThread(ECSIDialogs_Finish, dialog))
    {
        ECSIDialogs_Free(dialog);
    }
}

#pragma endregion Source Only

void ECSIDialogs_Initialize(void)
{
    // the questions about unsaved work are dialogs, which Panels cannot call itself
    ECSIPanels_SetAsk(ECSIDialogs_Ask);
}

void ECSIDialogs_Terminate(void)
{
    ECSIPanels_SetAsk(nullptr);
    SDL_zero(DIALOGS);
}

SHUResult ECSDialog_Show(ECSPlugin plugin, const ECSDialogDesc *desc)
{
    SDL_assert(plugin != nullptr);
    return ECSIDialogs_Show(plugin, desc);
}

SHUResult ECSIDialogs_Show(ECSPlugin plugin, const ECSDialogDesc *desc)
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
        SDL_DialogFileFilter filter = {ECSIDialogs_KeepText(dialog, desc->filters[i].name), ECSIDialogs_KeepText(dialog, desc->filters[i].pattern)};
        arrput(dialog->filters, filter);
    }

    const char *location = ECSIDialogs_KeepText(dialog, desc->location);

    if (dialog->failed)
    {
        ECSIDialogs_Free(dialog);
        return SHUResult_ErrAllocation;
    }

    SDL_Window *window = ECSIWindow_GetMain();
    int filterCount = (int)arrlenu(dialog->filters);

    switch (desc->type)
    {
    case ECSDialogType_OpenFile:
        SDL_ShowOpenFileDialog(ECSIDialogs_Answer, dialog, window, dialog->filters, filterCount, location, desc->many);
        break;
    case ECSDialogType_SaveFile:
        SDL_ShowSaveFileDialog(ECSIDialogs_Answer, dialog, window, dialog->filters, filterCount, location);
        break;
    case ECSDialogType_OpenFolder:
        SDL_ShowOpenFolderDialog(ECSIDialogs_Answer, dialog, window, location, desc->many);
        break;
    }

    return SHUResult_Ok;
}

/// @brief Handles a key press in a message dialog: Left, Right and Tab choose a button, Return and Space press the chosen one, and Escape presses the last.
/// @return The pressed button, or -1 while the dialog waits.
static i32 ECSIDialogs_Key(ECSIDialogView *dialog, const SDL_KeyboardEvent *key)
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
static i32 ECSIDialogs_Event(ECSIDialogView *dialog, const SDL_Event *event, SDL_WindowID window, i32 *pressedButton, SDL_Event **keptEvents)
{
    i32 last = (i32)dialog->count - 1;

    switch (event->type)
    {
    case SDL_EVENT_KEY_DOWN:
        return ECSIDialogs_Key(dialog, &event->key);

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

void ECSIDialogs_SetTest(ECSIDialogStepFunction Step, ECSIDialogWaitFunction Wait)
{
    DIALOGS.TestStep = Step;
    DIALOGS.TestWait = Wait;
}

const ECSIDialogView *ECSIDialogs_Get(void)
{
    return DIALOGS.dialog;
}

i32 ECSIDialogs_Ask(const char *title, const char *message, const char *const *buttons, usz count)
{
    SDL_assert(title != nullptr && message != nullptr && buttons != nullptr);

    // one dialog shows at a time
    if (count == 0 || DIALOGS.dialog != nullptr)
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
    DIALOGS.dialog = &dialog;
    SDL_Event *keptEvents = nullptr; // stb_ds array of the OS windows' events, which the main loop gets after the answer
    i32 answer = -1;
    i32 pressedButton = -1;

    while (answer < 0)
    {
        ECSIWindow_RenderDialog();

        // a running test goes on, so it can read the dialog with test.dialog and answer it with keys or clicks
        if (DIALOGS.TestStep != nullptr && !DIALOGS.TestStep())
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "The test cannot answer the dialog '%s', so it is answered with '%s'.", title, buttons[count - 1]);
            answer = (i32)count - 1;
            break;
        }

        // every waiting event is handled before the next step, as in the main loop: a wait without time stops at the end of SDL's poll cycle
        SDL_Event event;

        for (bool waiting = SDL_WaitEventTimeout(&event, DIALOGS.TestWait != nullptr ? DIALOGS.TestWait() : -1); waiting && answer < 0; waiting = SDL_PollEvent(&event))
        {
            answer = ECSIDialogs_Event(&dialog, &event, window, &pressedButton, &keptEvents);
        }
    }

    ECSIWindow_ShowDialog(nullptr);
    DIALOGS.dialog = nullptr;

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

    i32 button = ECSIDialogs_Ask(title, message, buttons, buttonCount);

    if (button < 0)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cannot show the message dialog '%s'.", title);
        return SHUResult_ErrInternal;
    }

    *retButton = (usz)button;
    return SHUResult_Ok;
}
