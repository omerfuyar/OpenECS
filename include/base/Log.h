#pragma once

// Log: where SDL's log goes, and how its lines look.

#include "OpenECS.h"

#pragma region Declarations

/// @brief Sends SDL's log to standard error, with the time, the level and the plugin on each line. Debug builds also show debug messages.
void ECSILog_Initialize(void);

/// @brief Writes the log to a file too, from now on. Each start writes the file anew.
/// @param path Path of the log file.
void ECSILog_OpenFile(const char *path);

/// @brief Closes the log file. Lines logged later go to standard error only.
void ECSILog_Terminate(void);

/// @brief Gets the path of the log file.
/// @return The path, or NULL if there is no log file.
const char *ECSILog_GetPath(void);

/// @brief Gets the last error or critical message, for the dialog of a failed start.
/// @return The message, valid until the next one, or NULL if there was none.
const char *ECSILog_GetLastError(void);

#pragma endregion Declarations
