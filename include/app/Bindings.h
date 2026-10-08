#pragma once

// Bindings: the ecs table, the plugin interface for Lua plugins.

#include "app/Session.h"

#pragma region Declarations

/// @brief Prepares the types of values that the ecs table gives to Lua, such as timers.
void ECSIBindings_Initialize(void);

/// @brief Frees what the bindings keep. Call it while Lua still runs.
void ECSIBindings_Terminate(void);

/// @brief Runs a plugin's Lua code in its own environment, which holds the plugin's ecs table. Given to ECSIPlugins_SetLuaStarter.
/// @param plugin The plugin.
/// @param path Path of the plugin's Lua file.
/// @return SHUResult_Ok, SHUResult_ErrFile if the file cannot be loaded, or SHUResult_ErrBadData if it raises an error. Errors are reported.
SHUWUR SHUResult ECSIBindings_StartPlugin(ECSPlugin plugin, const char *path);

#pragma endregion Declarations
