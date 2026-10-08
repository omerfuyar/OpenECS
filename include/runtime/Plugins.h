#pragma once

// Plugins: finding plugins, reading their manifests, and loading them in dependency order.

#include "base/Values.h"

#pragma region Declarations

/// @brief Functions of later modules that loading plugins needs. The App module provides them, because modules include only the modules before them.
typedef struct ECSI_PluginHooks
{
    /// @brief Runs a plugin's Lua code.
    /// @return SHUResult_Ok, or an error to mark the plugin failed.
    SHUResult (*StartLua)(ECSPlugin plugin, const char *path);

    /// @brief Removes everything a failed plugin registered: panel types, settings, functions and timers.
    void (*RemoveRegistrations)(ECSPlugin plugin);
} ECSI_PluginHooks;

/// @brief Sets the functions that loading plugins needs. Call it before ECSI_PluginsLoad.
/// @param hooks The functions. The core copies them.
void ECSI_PluginsSetHooks(const ECSI_PluginHooks *hooks);

/// @brief Loads plugins, after the plugins they depend on, and runs their ECSPlugin_Init.
/// @param directories Directories that hold plugin folders, searched in order. Each ends with a separator.
/// @param directoryCount Number of directories.
/// @param plugins A table of the plugins to load: named fields give a plugin's name and its minimum version, such as depends = { ui = "1.0" }; list items give only a name. NULL loads nothing.
/// @return SHUResult_Ok if every plugin loaded; otherwise the last error. Failing plugins are reported and skipped.
SHUWUR SHUResult ECSI_PluginsLoad(const char *const *directories, usz directoryCount, const ECSValue *plugins);

/// @brief Runs every plugin's ECSPlugin_Shutdown, in reverse load order, and closes their libraries.
void ECSI_PluginsUnload(void);

/// @brief Registers a plugin's state like ECSPlugin_RegisterState, with a function that releases its data when the plugin goes away. The Bindings module uses it for Lua.
/// @param release Function called with the description's data when the plugin fails or is unloaded, or NULL.
SHUWUR SHUResult ECSI_PluginRegisterState(ECSPlugin plugin, const ECSPluginStateDesc *desc, ECSTimerFunction release);

/// @brief Restores the state of every plugin that registered one, from a session's plugin_state table. A plugin whose state is missing keeps its own. Errors are reported.
/// @param states The plugin_state table: for each plugin's name, a table with its state and state_version. NULL restores nothing.
void ECSI_PluginsRestoreStates(const ECSValue *states);

/// @brief Saves the state of every plugin that registered one into a plugin_state table. Entries of other plugins are kept.
/// @param states The table to fill; a value that is not a table becomes one.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation. A plugin whose Save fails is reported and skipped.
SHUWUR SHUResult ECSI_PluginsSaveStates(ECSValue *states);

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
