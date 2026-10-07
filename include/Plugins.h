#pragma once

// Plugins: finding plugins, reading their manifests, and loading them in dependency order.

#include "OpenECS.h"

#pragma region Declarations

/// @brief Most plugins that can be loaded.
#define OPENECS_MAX_PLUGINS 64

/// @brief Loads plugins, after the plugins they depend on, and runs their ECSPlugin_Init.
/// @param directories Directories that hold plugin folders, searched in order. Each ends with a separator.
/// @param directoryCount Number of directories.
/// @param names Names of the plugins to load.
/// @param nameCount Number of names.
/// @return SHUResult_Ok if every plugin loaded; otherwise the last error. Failing plugins are reported and skipped.
SHUWUR SHUResult ECSI_PluginsLoad(const char *const *directories, usz directoryCount, const char *const *names, usz nameCount);

/// @brief Runs every plugin's ECSPlugin_Shutdown, in reverse load order, and closes their libraries.
void ECSI_PluginsUnload(void);

/// @brief Gets a plugin's name.
/// @param plugin The plugin.
/// @return The name its manifest gives.
const char *ECSI_PluginGetName(ECSPlugin plugin);

#pragma endregion Declarations
