#pragma once

// Test: runs a test in Debug builds (DESIGN 17.5). In other builds it refuses tests.

#include "app/Session.h"

#pragma region Declarations

/// @brief Reads a test file and prepares its run function. Builds other than Debug refuse it.
/// @param path Path of the test file.
/// @param info What the preset said. The test reads it when it asks for the session, so it may be filled later.
/// @param retPreset The preset the test starts from: a name, or a path. Free it with SDL_free.
/// @return SHUResult_Ok, SHUResult_ErrFile if the file cannot be read, SHUResult_ErrBadData if it fails or returns no test, SHUResult_ErrAllocation, or SHUResult_ErrPrivileges in builds other than Debug.
SHUWUR SHUResult ECSI_TestLoad(const char *path, const ECSI_PresetInfo *info, char **retPreset);

/// @brief Frees the test.
void ECSI_TestTerminate(void);

/// @brief Tells whether a test runs.
/// @return true while a test runs.
bool ECSI_TestIsRunning(void);

/// @brief Gets the time until the test's next step.
/// @return -1 if no test runs, 0 if the next step is due, or the milliseconds until it is due.
i32 ECSI_TestGetWait(void);

/// @brief Runs the test's next step: sends its next input event, or lets its run function go on. Call it at the end of each pass of the loop, once the window is drawn.
/// @return false when the test has finished, so the program quits; true otherwise.
bool ECSI_TestStep(void);

/// @brief Gets the program's exit status.
/// @return 1 if a test failed or did not finish, 0 otherwise.
int ECSI_TestGetStatus(void);

#pragma endregion Declarations
