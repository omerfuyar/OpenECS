#pragma once

// App: start-up, the main loop and shutdown.

#include "OpenECS.h"

#pragma region Declarations

/// @brief What the command line asks for.
typedef struct ECSIArguments
{
    const char *preset;      // name or path of the preset
    const char *session;     // path of a session, or nullptr if the command line names none
    bool fresh;              // true to start from the preset, not from the tool's last session
    const char *test;        // path of a test to run, or nullptr
    const char *definitions; // folder to write the definition files of the plugins' functions into, or nullptr
    bool version;            // print the version and exit
    bool help;               // print the options and exit
    char **files;            // the files to open, as the command line names them
    usz fileCount;
} ECSIArguments;

/// @brief Writes the plugin interface, and the definition files of every loaded plugin's functions (DESIGN 10.10). Call it after ECSIApp_Start, in place of ECSIApp_Run.
/// @param folder The folder to write into; it is made if it is missing.
/// @return SHUResult_Ok, or the first error; each is reported.
SHUWUR SHUResult ECSIApp_WriteDefinitions(const char *folder);

/// @brief Starts every module, loads the plugins and builds the layout from the session or the preset. If a step fails, it tells the user and exits the program.
/// @param arguments What the command line asks for.
void ECSIApp_Start(const ECSIArguments *arguments);

/// @brief Runs the main loop until the user quits or the test finishes.
/// @return The program's exit status: 1 if a test failed or did not finish, 0 otherwise.
int ECSIApp_Run(void);

/// @brief What the program starts again from once it stops: the session or preset that ECSSession_Open or ECSSession_OpenPreset chose.
typedef struct ECSIRestart
{
    char *path;         // the file, or nullptr to exit; free it with SDL_free
    const char *option; // the command-line option that names it, "--session" or "--preset"
} ECSIRestart;

/// @brief Saves the session for the next start, then stops every module.
/// @return What the program starts again from.
ECSIRestart ECSIApp_Stop(void);

#pragma endregion Declarations
