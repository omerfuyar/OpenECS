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

/// @brief Shows a file dialog for a plugin or for the core, as ECSDialog_Show does.
/// @param plugin The plugin that asks, or NULL for the core.
/// @param desc Description of the dialog.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation.
/// @brief Turns the system's text input on while the focused panel accepts text, and tells the input method where its text cursor is. Call it once each pass of the loop, after events are delivered.
void ECSIInput_UpdateTextInput(void);

SHUWUR SHUResult ECSIInput_ShowDialog(ECSPlugin plugin, const ECSDialogDesc *desc);

#pragma endregion Declarations
