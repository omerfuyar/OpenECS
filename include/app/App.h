#pragma once

// App: start-up, the main loop and shutdown.

#include "OpenECS.h"

#pragma region Declarations

/// @brief What the command line asks for.
typedef struct ECSI_Arguments
{
    const char *preset;  // name or path of the preset
    const char *session; // path of a session, or NULL if the command line names none
    bool fresh;          // true to start from the preset, not from the tool's last session
} ECSI_Arguments;

/// @brief Starts every module, loads the plugins and builds the layout from the session or the preset. If a step fails, it tells the user and exits the program.
/// @param arguments What the command line asks for.
void ECSI_AppStart(const ECSI_Arguments *arguments);

/// @brief Runs the main loop until the user quits.
void ECSI_AppRun(void);

/// @brief Saves the session for the next start, then stops every module.
void ECSI_AppStop(void);

#pragma endregion Declarations
