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
    bool version;        // print the version and exit
    bool help;           // print the options and exit
    char **files;        // the files to open, as the command line names them
    usz fileCount;
} ECSIArguments;

/// @brief Starts every module, loads the plugins and builds the layout from the session or the preset. If a step fails, it tells the user and exits the program.
/// @param arguments What the command line asks for.
void ECSIApp_Start(const ECSIArguments *arguments);

/// @brief Runs the main loop until the user quits or the test finishes.
/// @return The program's exit status: 1 if a test failed or did not finish, 0 otherwise.
int ECSIApp_Run(void);

/// @brief Saves the session for the next start, then stops every module.
/// @param retOption The command-line option that names the file returned, "--session" or "--preset".
/// @return The session or preset that ECSSession_Open or ECSSession_OpenPreset chose, which the program starts again from, or NULL. Free it with SDL_free.
char *ECSIApp_Stop(const char **retOption);

#pragma endregion Declarations
