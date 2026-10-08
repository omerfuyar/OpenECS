#pragma once

// Input: focus, pointer routing, the core prefix and key dispatch.

#include "shu/shu.h"

#include "SDL3/SDL_events.h"

#pragma region Declarations

/// @brief Declares the input settings, ecs.prefix and ecs.focus, and prepares input handling.
/// @return SHUResult_Ok, SHUResult_ErrAllocation, or SHUResult_ErrBadData if a key text of the core cannot be read.
SHUWUR SHUResult ECSI_InputInitialize(void);

/// @brief Handles one SDL event.
/// @param event The event.
/// @return false if the program should quit.
bool ECSI_InputHandle(const SDL_Event *event);

#pragma endregion Declarations
