#pragma once

// Log: where SDL's log goes, and how its lines look.

#include "OpenECS.h"

#include <stdio.h>

#pragma region Declarations

/// @brief Checks a rule that a public function's caller must keep (DESIGN 1.4). A broken rule is a misuse: it is reported, and the function returns the value given after the condition, or nothing.
#define ECSILog_Check(condition, ...)                                              \
    do                                                                             \
    {                                                                              \
        if (!ECS_Check(nullptr, (condition), "%s needs %s", __func__, #condition)) \
        {                                                                          \
            return __VA_ARGS__;                                                    \
        }                                                                          \
    } while (0)

/// @brief A line of the log that waits for the main loop, which emits it as ecs.logged.
typedef struct ECSILogLine
{
    char time[16];     // as the line shows it, such as "12:30:05.123"
    const char *level; // as ecs.log names it: "debug", "info", "warn" or "error"
    char *plugin;      // the plugin that logged it, or "ecs"
    char *message;
} ECSILogLine;

/// @brief Sends SDL's log to standard error, with the time, the level and the plugin on each line. Debug builds also show debug messages.
void ECSILog_Initialize(void);

/// @brief Checks whether a stream shows colours: a terminal, unless NO_COLOR is set (no-color.org). On Windows it turns the console's colours on.
/// @param stream stdout or stderr.
/// @return true if the stream shows colours.
bool ECSILog_HasColors(FILE *stream);

/// @brief Writes the log to a file too, from now on. Each start writes the file anew.
/// @param path Path of the log file.
void ECSILog_OpenFile(const char *path);

/// @brief Closes the log file. Lines logged later go to standard error only.
void ECSILog_CloseFile(void);

/// @brief Gets the path of the log file.
/// @return The path, or nullptr if there is no log file.
const char *ECSILog_GetPath(void);

/// @brief Tells whether lines wait for ECSILog_Take, from any thread.
bool ECSILog_IsWaiting(void);

/// @brief Takes the lines that wait, on the main thread.
/// @param retDropped How many lines were left out since the last take, as more than the queue holds waited.
/// @return A stb_ds array of the lines, or nullptr, to free with ECSILog_FreeLines.
ECSILogLine *ECSILog_Take(usz *retDropped);

/// @brief Frees lines that ECSILog_Take gave.
void ECSILog_FreeLines(ECSILogLine *lines);

/// @brief While quiet, lines that the main thread logs are written but not queued, so a handler of ecs.logged that logs does not loop.
void ECSILog_SetQuiet(bool quiet);

/// @brief Frees the lines that wait, and stops queuing. Lines logged later are still written.
void ECSILog_Terminate(void);

/// @brief Gets the last error or critical message, for the dialog of a failed start.
/// @return The message, valid until the next one, or nullptr if there was none.
const char *ECSILog_GetLastError(void);

#pragma endregion Declarations
