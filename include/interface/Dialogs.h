#pragma once

// Dialogs: the system's file dialogs, and the message dialogs that the core draws (DESIGN 7.7).

#include "interface/Window.h"

#pragma region Declarations

/// @brief Lets Panels ask about unsaved work with a message dialog.
void ECSIDialogs_Initialize(void);

/// @brief Stops Panels asking, and forgets a running test.
void ECSIDialogs_Terminate(void);

/// @brief Shows a file dialog for a plugin or for the core, as ECSDialog_Show does.
/// @param plugin The plugin that asks, or nullptr for the core.
/// @param desc Description of the dialog.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSIDialogs_Show(ECSPlugin plugin, const ECSDialogDesc *desc);

/// @brief Moves a running test on while a message dialog waits, so the test can read and answer it.
/// @return false when the test cannot answer: it has ended, or the dialog opened while the test's own code runs.
typedef bool (*ECSIDialogStepFunction)(void);

/// @brief Gives how long a waiting message dialog may wait for input before the test's next step, in milliseconds; -1 for no limit.
typedef i32 (*ECSIDialogWaitFunction)(void);

/// @brief Lets a running test go on while a message dialog waits. A test calls it as it starts.
void ECSIDialogs_SetTest(ECSIDialogStepFunction Step, ECSIDialogWaitFunction Wait);

/// @brief Asks the user with a message dialog that the core draws over an OS window, and waits for a button (DESIGN 7.7). No plugin code runs while it waits, as with a system dialog.
/// @param buttons Texts of the buttons, from left to right; Return presses the chosen one, at first the first, and Escape the last.
/// @param count Number of buttons, at least 1.
/// @return The index of the pressed button, or -1 if no dialog can be shown, such as when one is shown already.
i32 ECSIDialogs_Ask(const char *title, const char *message, const char *const *buttons, usz count);

/// @brief Gives the message dialog that waits for an answer, or nullptr; for tests.
const ECSIDialogView *ECSIDialogs_Get(void);

#pragma endregion Declarations
