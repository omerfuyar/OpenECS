#pragma once

// Plugins: finding plugins, reading their manifests, and loading them in dependency order.

#include "Values.h"

#pragma region Declarations

/// @brief Runs a plugin's Lua code. The Bindings module provides it, because it comes after this module.
/// @param plugin The plugin.
/// @param path Path of the plugin's Lua file.
/// @return SHUResult_Ok, or an error to mark the plugin failed.
typedef SHUResult (*ECSI_PluginLuaStarter)(ECSPlugin plugin, const char *path);

/// @brief Sets the function that runs plugins' Lua code. Call it before ECSI_PluginsLoad.
/// @param starter The function.
void ECSI_PluginsSetLuaStarter(ECSI_PluginLuaStarter starter);

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

/// @brief Gets a plugin's version.
/// @param plugin The plugin.
/// @return The version its manifest gives.
const char *ECSI_PluginGetVersion(ECSPlugin plugin);

/// @brief Reports an error of a plugin's callback. Repeats of the same error are counted, not reported again; ECSI_PluginsUnload reports the counts.
/// @param plugin The plugin.
/// @param message The error.
void ECSI_PluginReportError(ECSPlugin plugin, const char *message);

/// @brief Checks whether a plugin's manifest depends on another plugin. A plugin counts as depending on itself.
/// @param plugin The plugin.
/// @param other The other plugin.
/// @return true if the plugin may use the other plugin's services and events.
bool ECSI_PluginDependsOn(ECSPlugin plugin, ECSPlugin other);

/// @brief Checks that a name that a plugin registers starts with the plugin's name and a dot, and reports it if not.
/// @param plugin The plugin.
/// @param name The name, such as "canvas.view".
/// @return true if the name belongs to the plugin.
bool ECSI_PluginOwnsName(ECSPlugin plugin, const char *name);

#pragma endregion Declarations
