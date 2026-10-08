#pragma once

// Plugins: finding plugins, reading their manifests, and loading them in dependency order.

#include "Values.h"

#pragma region Declarations

/// @brief Loads plugins, after the plugins they depend on, and runs their ECSPlugin_Init.
/// @param directories Directories that hold plugin folders, searched in order. Each ends with a separator.
/// @param directoryCount Number of directories.
/// @param plugins A table of the plugins to load: named fields give a plugin's name and its minimum version, such as depends = { ui = "1.0" }; list items give only a name. NULL loads nothing.
/// @return SHUResult_Ok if every plugin loaded; otherwise the last error. Failing plugins are reported and skipped.
SHUWUR SHUResult ECSI_PluginsLoad(const char *const *directories, usz directoryCount, const ECSValue *plugins);

/// @brief Runs every plugin's ECSPlugin_Shutdown, in reverse load order, and closes their libraries.
void ECSI_PluginsUnload(void);

/// @brief Gets a plugin's name.
/// @param plugin The plugin.
/// @return The name its manifest gives.
const char *ECSI_PluginGetName(ECSPlugin plugin);

/// @brief Checks that a name that a plugin registers starts with the plugin's name and a dot, and reports it if not.
/// @param plugin The plugin.
/// @param name The name, such as "canvas.view".
/// @return true if the name belongs to the plugin.
bool ECSI_PluginOwnsName(ECSPlugin plugin, const char *name);

#pragma endregion Declarations
