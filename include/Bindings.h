#pragma once

// Bindings: the ecs table, the plugin interface for Lua plugins.

#include "Session.h"

#pragma region Declarations

/// @brief Runs a plugin's Lua code in its own environment, which holds the plugin's ecs table. Given to ECSI_PluginsSetLuaStarter.
/// @param plugin The plugin.
/// @param path Path of the plugin's Lua file.
/// @return SHUResult_Ok, SHUResult_ErrFile if the file cannot be loaded, or SHUResult_ErrBadData if it raises an error. Errors are reported.
SHUWUR SHUResult ECSI_BindingsStartPlugin(ECSPlugin plugin, const char *path);

#pragma endregion Declarations
