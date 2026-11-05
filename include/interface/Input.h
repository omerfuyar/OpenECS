#pragma once

// Input: SDL's input events: pointer routing, focus, the core prefix and the list of keys after it, and key dispatch.

#include "base/Values.h"
#include "interface/Window.h"

#include "SDL3/SDL_events.h"

#pragma region Declarations

/// @brief Declares the setting ecs.focus.
/// @return SHUResult_Ok, SHUResult_ErrAllocation, or SHUResult_ErrBadData if a name of the core is taken.
SHUWUR SHUResult ECSIInput_Initialize(void);

/// @brief Frees what input handling holds.
void ECSIInput_Terminate(void);

/// @brief Handles one SDL event.
/// @param event The event.
/// @return false if the program should quit.
bool ECSIInput_Handle(const SDL_Event *event);

/// @brief Turns the system's text input on while the focused panel accepts text, and tells the input method where its text cursor is. Call it once each pass of the loop, after events are delivered.
void ECSIInput_UpdateTextInput(void);

#pragma endregion Declarations
