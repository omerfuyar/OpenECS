#pragma once

// Input: SDL's input events: pointer routing, focus, the core prefix and the list of keys after it, key dispatch, the clipboard and dialogs.

#include "base/Values.h"

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

#pragma endregion Declarations
