#include "tools/Platform.h"

#include "SDL3/SDL.h"

#include <ctype.h>

#pragma region Source Only

/// @brief Capacity of a key combination text, such as "Ctrl+Shift+P".
#define OPENECS_KEY_TEXT_CAPACITY 64

/// @brief Converts SDL's modifier bits to ECSModifier bits.
static u32 ECSI_PlatformModifiers(SDL_Keymod modifiers)
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

/// @brief Converts an SDL event to a platform event.
/// @return false if the core does not need the event.
static bool ECSI_PlatformTranslate(const SDL_Event *event, ECSI_PlatformEvent *retEvent)
{
    *retEvent = (ECSI_PlatformEvent){0};
    retEvent->window = SDL_GetWindowFromEvent(event);

    switch (event->type)
    {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        retEvent->kind = ECSI_PlatformEventKind_Quit;
        return true;

    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_EXPOSED:
    case SDL_EVENT_WINDOW_SHOWN:
        retEvent->kind = ECSI_PlatformEventKind_WindowChanged;
        return true;

    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        retEvent->kind = ECSI_PlatformEventKind_PointerLeave;
        return true;

    case SDL_EVENT_MOUSE_MOTION:
        retEvent->kind = ECSI_PlatformEventKind_PointerMove;
        retEvent->x = event->motion.x;
        retEvent->y = event->motion.y;
        return true;

    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        retEvent->kind = event->type == SDL_EVENT_MOUSE_BUTTON_DOWN ? ECSI_PlatformEventKind_PointerDown : ECSI_PlatformEventKind_PointerUp;
        retEvent->x = event->button.x;
        retEvent->y = event->button.y;
        retEvent->button = event->button.button;
        retEvent->modifiers = ECSI_PlatformModifiers(SDL_GetModState());
        return true;

    case SDL_EVENT_MOUSE_WHEEL:
        retEvent->kind = ECSI_PlatformEventKind_Wheel;
        retEvent->x = event->wheel.mouse_x;
        retEvent->y = event->wheel.mouse_y;
        retEvent->wheelX = event->wheel.x;
        retEvent->wheelY = event->wheel.y;
        return true;

    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        retEvent->kind = event->type == SDL_EVENT_KEY_DOWN ? ECSI_PlatformEventKind_KeyDown : ECSI_PlatformEventKind_KeyUp;
        retEvent->key = event->key.key;
        retEvent->modifiers = ECSI_PlatformModifiers(event->key.mod);
        retEvent->repeat = event->key.repeat;
        retEvent->modifierKey = (event->key.key >= SDLK_LCTRL && event->key.key <= SDLK_RGUI) || event->key.key == SDLK_MODE;
        return true;

    default:
        return false;
    }
}

#pragma endregion Source Only

SHUResult ECSI_PlatformInitialize(const char *appName, const char *appId)
{
    SHU_AssertNullPointer(appName);
    SHU_AssertNullPointer(appId);

    SDL_SetAppMetadata(appName, NULL, appId);

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
    {
        SHU_LogWarning("SDL failed to start: %s", SDL_GetError());
        return SHUResult_ErrInternal;
    }

    return SHUResult_Ok;
}

void ECSI_PlatformTerminate(void)
{
    SDL_Quit();
}

const char *ECSI_PlatformGetBaseDirectory(void)
{
    const char *directory = SDL_GetBasePath();
    return directory == NULL ? "./" : directory;
}

u64 ECSI_PlatformGetTicks(void)
{
    return SDL_GetTicksNS();
}

SHUResult ECSI_PlatformWindowCreate(ECSI_OsWindow **retWindow, const char *title, i32 width, i32 height)
{
    SHU_AssertNullPointer(retWindow);
    SHU_AssertNullPointer(title);

    *retWindow = SDL_CreateWindow(title, width, height, SDL_WINDOW_RESIZABLE);

    if (*retWindow == NULL)
    {
        SHU_LogWarning("SDL failed to create a window: %s", SDL_GetError());
        return SHUResult_ErrInternal;
    }

    return SHUResult_Ok;
}

void ECSI_PlatformWindowDestroy(ECSI_OsWindow **window)
{
    SHU_AssertNullPointer(window);

    SDL_DestroyWindow(*window);
    *window = NULL;
}

void ECSI_PlatformWindowGetSize(ECSI_OsWindow *window, f32 *retWidth, f32 *retHeight)
{
    SHU_AssertNullPointer(window);

    int width = 0;
    int height = 0;
    SDL_GetWindowSize(window, &width, &height);

    *retWidth = (f32)width;
    *retHeight = (f32)height;
}

bool ECSI_PlatformEventWait(ECSI_PlatformEvent *retEvent, i32 timeoutMs)
{
    SHU_AssertNullPointer(retEvent);

    SDL_Event event;

    while (SDL_WaitEventTimeout(&event, timeoutMs))
    {
        if (ECSI_PlatformTranslate(&event, retEvent))
        {
            return true;
        }

        // after the first event, only take the events that are already queued
        timeoutMs = 0;
    }

    return false;
}

SHUResult ECSI_PlatformKeyParse(const char *text, u32 *retKey, u32 *retModifiers)
{
    SHU_AssertNullPointer(text);
    SHU_AssertNullPointer(retKey);
    SHU_AssertNullPointer(retModifiers);

    char buffer[OPENECS_KEY_TEXT_CAPACITY];
    ECSI_TextCopy(cs(buffer, sizeof(buffer)), text);

    *retKey = SDLK_UNKNOWN;
    *retModifiers = ECSModifier_None;

    char *save = NULL;

    for (char *part = strtok_r(buffer, "+", &save); part != NULL; part = strtok_r(NULL, "+", &save))
    {
        if (ECSI_TextEqualsIgnoreCase(part, "Ctrl"))
        {
            *retModifiers |= ECSModifier_Ctrl;
        }
        else if (ECSI_TextEqualsIgnoreCase(part, "Shift"))
        {
            *retModifiers |= ECSModifier_Shift;
        }
        else if (ECSI_TextEqualsIgnoreCase(part, "Alt"))
        {
            *retModifiers |= ECSModifier_Alt;
        }
        else if (ECSI_TextEqualsIgnoreCase(part, "Super"))
        {
            *retModifiers |= ECSModifier_Super;
        }
        else if (strlen(part) == 1)
        {
            // letter and digit keys use their lowercase character as key code
            *retKey = (u32)tolower((unsigned char)part[0]);
        }
        else
        {
            *retKey = SDL_GetKeyFromName(part);
        }
    }

    return *retKey == SDLK_UNKNOWN ? SHUResult_ErrBadData : SHUResult_Ok;
}

SHUResult ECSI_PlatformLibraryOpen(void **retLibrary, const char *path)
{
    SHU_AssertNullPointer(retLibrary);
    SHU_AssertNullPointer(path);

    *retLibrary = SDL_LoadObject(path);

    if (*retLibrary == NULL)
    {
        SHU_LogWarning("Cannot open library '%s': %s", path, SDL_GetError());
        return SHUResult_ErrFile;
    }

    return SHUResult_Ok;
}

ECSI_Function ECSI_PlatformLibraryGetFunction(void *library, const char *name)
{
    SHU_AssertNullPointer(library);
    SHU_AssertNullPointer(name);

    return (ECSI_Function)SDL_LoadFunction(library, name);
}

void ECSI_PlatformLibraryClose(void **library)
{
    SHU_AssertNullPointer(library);

    if (*library != NULL)
    {
        SDL_UnloadObject(*library);
        *library = NULL;
    }
}
