#pragma once

// Input: SDL's input events: pointer routing, focus, the core prefix and the list of keys after it, key dispatch, the clipboard and dialogs.

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

/// @brief Shows a file dialog for a plugin or for the core, as ECSDialog_Show does.
/// @param plugin The plugin that asks, or nullptr for the core.
/// @param desc Description of the dialog.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSIInput_ShowDialog(ECSPlugin plugin, const ECSDialogDesc *desc);

/// @brief Moves a running test on while a message dialog waits, so the test can read and answer it.
/// @return false when the test cannot answer: it has ended, or the dialog opened while the test's own code runs.
typedef bool (*ECSIDialogStepFunction)(void);

/// @brief Gives how long a waiting message dialog may wait for input before the test's next step, in milliseconds; -1 for no limit.
typedef i32 (*ECSIDialogWaitFunction)(void);

/// @brief Lets a running test go on while a message dialog waits. A test calls it as it starts.
void ECSIInput_SetDialogTest(ECSIDialogStepFunction Step, ECSIDialogWaitFunction Wait);

/// @brief Asks the user with a message dialog that the core draws over an OS window, and waits for a button (DESIGN 7.7). No plugin code runs while it waits, as with a system dialog.
/// @param buttons Texts of the buttons, from left to right; Return presses the chosen one, at first the first, and Escape the last.
/// @param count Number of buttons, at least 1.
/// @return The index of the pressed button, or -1 if no dialog can be shown, such as when one is shown already.
i32 ECSIInput_Ask(const char *title, const char *message, const char *const *buttons, usz count);

/// @brief Gives the message dialog that waits for an answer, or nullptr; for tests.
const ECSIDialogView *ECSIInput_GetDialog(void);

#pragma endregion Declarations
