#pragma once

// Platform: OS windows, input events, time and shared libraries. The only module that talks to the operating system.

#include "Global.h"

#pragma region Declarations

/// @brief An OS window. Its contents belong to SDL.
typedef struct SDL_Window ECSI_OsWindow;

/// @brief A function loaded from a shared library. Cast it to its real type before calling it.
typedef void (*ECSI_Function)(void);

/// @brief Kind of a platform event.
typedef enum ECSI_PlatformEventKind
{
    ECSI_PlatformEventKind_None = 0,
    ECSI_PlatformEventKind_Quit,
    ECSI_PlatformEventKind_WindowChanged, // resized, shown or exposed: the window must be drawn again
    ECSI_PlatformEventKind_PointerMove,
    ECSI_PlatformEventKind_PointerDown,
    ECSI_PlatformEventKind_PointerUp,
    ECSI_PlatformEventKind_PointerLeave,
    ECSI_PlatformEventKind_Wheel,
    ECSI_PlatformEventKind_KeyDown,
    ECSI_PlatformEventKind_KeyUp,
} ECSI_PlatformEventKind;

/// @brief An event from the operating system, in the core's terms.
typedef struct ECSI_PlatformEvent
{
    ECSI_PlatformEventKind kind;
    ECSI_OsWindow *window;
    f32 x; // pointer position in layout units
    f32 y;
    f32 wheelX;
    f32 wheelY;
    i32 button;    // 1 left, 2 middle, 3 right
    u32 key;       // SDL key code
    u32 modifiers; // ECSModifier bits
    bool repeat;      // key events: the key is held down
    bool modifierKey; // key events: the key itself is a modifier, such as Shift
} ECSI_PlatformEvent;

/// @brief Starts SDL. The tool's identity must be known here, because SDL needs it before any OS window exists.
/// @param appName Name of the tool, shown by the desktop.
/// @param appId Application id of the tool. Matches the name of its .desktop file.
/// @return SHUResult_Ok, or SHUResult_ErrInternal if SDL fails to start.
SHUWUR SHUResult ECSI_PlatformInitialize(const char *appName, const char *appId);

/// @brief Stops SDL.
void ECSI_PlatformTerminate(void);

/// @brief Gets the directory of the executable.
/// @return Path that ends with a separator.
const char *ECSI_PlatformGetBaseDirectory(void);

/// @brief Gets the time since SDL started.
/// @return Nanoseconds.
u64 ECSI_PlatformGetTicks(void);

/// @brief Creates an OS window.
/// @param retWindow The new window.
/// @param title Title of the window.
/// @param width Width in layout units.
/// @param height Height in layout units.
/// @return SHUResult_Ok, or SHUResult_ErrInternal if SDL fails to create the window.
SHUWUR SHUResult ECSI_PlatformWindowCreate(ECSI_OsWindow **retWindow, const char *title, i32 width, i32 height);

/// @brief Destroys an OS window and sets the handle to NULL.
/// @param window Window to destroy.
void ECSI_PlatformWindowDestroy(ECSI_OsWindow **window);

/// @brief Gets the size of an OS window.
/// @param window Window to measure.
/// @param retWidth Width in layout units.
/// @param retHeight Height in layout units.
void ECSI_PlatformWindowGetSize(ECSI_OsWindow *window, f32 *retWidth, f32 *retHeight);

/// @brief Waits for the next event that matters to the core.
/// @param retEvent The event.
/// @param timeoutMs Longest wait in milliseconds; -1 waits without limit, 0 only takes events already queued.
/// @return true if an event was written, false if the time ran out.
bool ECSI_PlatformEventWait(ECSI_PlatformEvent *retEvent, i32 timeoutMs);

/// @brief Reads a key combination written as text, such as "Ctrl+Shift+P".
/// @param text The key combination.
/// @param retKey The key's SDL key code.
/// @param retModifiers The modifiers, as ECSModifier bits.
/// @return SHUResult_Ok, or SHUResult_ErrBadData if the text is not a key combination.
SHUWUR SHUResult ECSI_PlatformKeyParse(const char *text, u32 *retKey, u32 *retModifiers);

/// @brief Opens a shared library.
/// @param retLibrary The opened library.
/// @param path Path of the library file.
/// @return SHUResult_Ok, or SHUResult_ErrFile if the library cannot be opened.
SHUWUR SHUResult ECSI_PlatformLibraryOpen(void **retLibrary, const char *path);

/// @brief Finds a function exported by a shared library.
/// @param library The library.
/// @param name Name of the function.
/// @return The function, or NULL if the library does not export it.
ECSI_Function ECSI_PlatformLibraryGetFunction(void *library, const char *name);

/// @brief Closes a shared library and sets the handle to NULL.
/// @param library Library to close.
void ECSI_PlatformLibraryClose(void **library);

#pragma endregion Declarations
