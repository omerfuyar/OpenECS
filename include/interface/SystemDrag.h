#pragma once

// SystemDrag: a panel's drag between the program's OS windows that the system carries, where the program cannot see the pointer outside the window it was pressed in, as on Wayland.
// There, while a button is held, only the pressed window gets the pointer, and no window knows where the others are. A drag that the compositor carries is seen by every window it passes over.

#include "OpenECS.h"

#include "SDL3/SDL.h"

#pragma region Declarations

/// @brief Called while the pointer moves during a drag.
/// @param window The OS window under the pointer, or nullptr outside every one of the program's.
/// @param x Horizontal position in that window, in layout units.
/// @param y Vertical position in that window, in layout units.
typedef void (*ECSISystemDragMoveFunction)(SDL_Window *window, f32 x, f32 y);

/// @brief Called once, when the button is released or the system ends the drag.
/// @param carried Whether the compositor carried the drag. When it did not, the pressed window got the pointer all along, as without a system drag.
typedef void (*ECSISystemDragEndFunction)(bool carried);

/// @brief Gets ready for drags that the system carries, which need to see the presses they continue. Call it once the first OS window is open. It does nothing where such drags are not used.
void ECSISystemDrag_Prepare(void);

/// @brief Starts a drag that the system carries, from the OS window whose button press is still held. Only on Wayland; elsewhere the core sees the pointer over every window.
/// @param from The OS window that got the press.
/// @param move Called as the pointer moves over the program's windows or leaves them.
/// @param end Called once when the drag ends.
/// @return Whether the drag started. It does not on other systems, or when the compositor lacks what it needs.
bool ECSISystemDrag_Start(SDL_Window *from, ECSISystemDragMoveFunction move, ECSISystemDragEndFunction end);

/// @brief Checks whether a drag that the system carries is on.
bool ECSISystemDrag_IsOn(void);

/// @brief Handles what the compositor said about the drag since the last call. The main loop calls it after the SDL events.
void ECSISystemDrag_Pump(void);

/// @brief Ends the drag without calling its end function, as when a new press comes.
void ECSISystemDrag_Cancel(void);

/// @brief Frees what the drags hold. Call it before SDL closes its video.
void ECSISystemDrag_Terminate(void);

#pragma endregion Declarations
