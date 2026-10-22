#pragma once

// Paths: what a path looks like on each system, and paths relative to a file.

#include "OpenECS.h"

#pragma region Declarations

/// @brief Gets how much of a file's path is its folder, to put before a path that is relative to that folder: printf's "%.*s%s" with the file's path and the path.
/// @param file Path of a file, such as a preset.
/// @param path A path relative to the file's folder, or an absolute path.
/// @return The length of the file's folder, with its last separator; 0 if the path is absolute or the file's path has no folder.
int ECSIPath_GetFolderLength(const char *file, const char *path);

#pragma endregion Declarations
