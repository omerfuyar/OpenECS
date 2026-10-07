#pragma once

// Definitions shared by every module of the core. Every core header includes this file first.

#ifdef OPENECS_NO_ASSERT
#define SHU_NO_ASSERT
#endif

#include "shu/shu.h"
#include "OpenECS.h"

#pragma region Constants

/// @brief Capacity of a name: a plugin, a panel type, a title or a workspace, including the zero byte.
#define OPENECS_NAME_CAPACITY 64

/// @brief Capacity of a file path, including the zero byte.
#define OPENECS_PATH_CAPACITY 1024

/// @brief Most plugins that can be loaded.
#define OPENECS_MAX_PLUGINS 64

/// @brief Most panel types that can be registered.
#define OPENECS_MAX_PANEL_TYPES 256

/// @brief Most children of a split.
#define OPENECS_MAX_SPLIT_CHILDREN 16

/// @brief Most panels in a group.
#define OPENECS_MAX_GROUP_PANELS 32

/// @brief Most workspaces.
#define OPENECS_MAX_WORKSPACES 9

#pragma endregion Constants

#pragma region Declarations

/// @brief Copies text into a buffer, and cuts it if it does not fit. The result always ends with a zero byte.
/// @param buffer Buffer to write into. Its size includes the zero byte.
/// @param text Text to copy. NULL copies an empty text.
void ECSI_TextCopy(SHUSlice buffer, const char *text);

/// @brief Compares two texts, ignoring letter case.
/// @param text First text.
/// @param other Second text.
/// @return true if they are equal.
bool ECSI_TextEqualsIgnoreCase(const char *text, const char *other);

/// @brief Builds the path of a directory in the user's data directory ($XDG_DATA_HOME, or ~/.local/share).
/// @param buffer Buffer to write the path into.
/// @param name Directory inside the data directory, such as "openecs/plugins/".
void ECSI_PathUserData(SHUSlice buffer, const char *name);

/// @brief Checks whether a text starts with another text.
/// @param text Text to check.
/// @param prefix Expected start.
/// @return true if text starts with prefix.
bool ECSI_TextStartsWith(const char *text, const char *prefix);

#pragma endregion Declarations
