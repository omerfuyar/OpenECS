#pragma once

// App: start-up, the main loop and shutdown.

#include "OpenECS.h"

#pragma region Declarations

/// @brief What the command line asks for.
typedef struct ECSIArguments
{
    const char *preset;  // name or path of the preset
    const char *session; // path of a session, or NULL if the command line names none
    bool fresh;          // true to start from the preset, not from the tool's last session
    const char *test;    // path of a test to run, or NULL
} ECSIArguments;

/// @brief Starts every module, loads the plugins and builds the layout from the session or the preset. If a step fails, it tells the user and exits the program.
/// @param arguments What the command line asks for.
void ECSIApp_Start(const ECSIArguments *arguments);

/// @brief Runs the main loop until the user quits or the test finishes.
/// @return The program's exit status: 1 if a test failed or did not finish, 0 otherwise.
int ECSIApp_Run(void);

/// @brief Saves the session for the next start, then stops every module.
void ECSIApp_Stop(void);

#pragma endregion Declarations
