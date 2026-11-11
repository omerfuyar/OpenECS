#pragma once

// Log: where SDL's log goes, and how its lines look.

#include "OpenECS.h"

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

/// @brief Sends SDL's log to standard error, with the time, the level and the plugin on each line. Debug builds also show debug messages.
void ECSILog_Initialize(void);

/// @brief Writes the log to a file too, from now on. Each start writes the file anew.
/// @param path Path of the log file.
void ECSILog_OpenFile(const char *path);

/// @brief Closes the log file. Lines logged later go to standard error only.
void ECSILog_CloseFile(void);

/// @brief Gets the path of the log file.
/// @return The path, or nullptr if there is no log file.
const char *ECSILog_GetPath(void);

/// @brief Gets the last error or critical message, for the dialog of a failed start.
/// @return The message, valid until the next one, or nullptr if there was none.
const char *ECSILog_GetLastError(void);

#pragma endregion Declarations
