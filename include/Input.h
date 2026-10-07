#pragma once

// Input: focus, pointer routing, the core prefix and key dispatch.

#include "shu/shu.h"

#include "SDL3/SDL_events.h"

#pragma region Declarations

/// @brief Prepares input handling.
/// @param prefix The core prefix, written as text, such as "Alt+W".
/// @return SHUResult_Ok, or SHUResult_ErrBadData if a key text cannot be read.
SHUWUR SHUResult ECSI_InputInitialize(const char *prefix);

/// @brief Handles one SDL event.
/// @param event The event.
/// @return false if the program should quit.
bool ECSI_InputHandle(const SDL_Event *event);

#pragma endregion Declarations
