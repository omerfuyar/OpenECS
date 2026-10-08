#pragma once

// Log: where SDL's log goes, and how its lines look.

#include "OpenECS.h"

#pragma region Declarations

/// @brief Sends SDL's log to standard error, with the time, the level and the plugin on each line. Debug builds also show debug messages.
void ECSI_LogInitialize(void);

/// @brief Writes the log to a file too, from now on. Each start writes the file anew.
/// @param path Path of the log file.
void ECSI_LogOpenFile(const char *path);

/// @brief Closes the log file. Lines logged later go to standard error only.
void ECSI_LogTerminate(void);

#pragma endregion Declarations
