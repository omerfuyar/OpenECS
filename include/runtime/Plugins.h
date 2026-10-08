#pragma once

// Plugins: finding plugins, reading their manifests, and loading them in dependency order.

#include "base/Values.h"

#pragma region Declarations

/// @brief Runs a plugin's Lua code.
/// @param plugin The plugin.
/// @param path Path of its Lua file.
/// @return SHUResult_Ok, or an error to mark the plugin failed.
typedef SHUResult (*ECSIPluginStartLuaFunction)(ECSPlugin plugin, const char *path);

/// @brief Removes everything a failed plugin registered: panel types, settings, functions and timers.
/// @param plugin The plugin.
typedef void (*ECSIPluginRemoveFunction)(ECSPlugin plugin);

/// @brief Functions of later modules that loading plugins needs. The App module provides them, because modules include only the modules before them.
typedef struct ECSIPluginHooks
{
    ECSIPluginStartLuaFunction StartLua;
    ECSIPluginRemoveFunction RemoveRegistrations;
} ECSIPluginHooks;

/// @brief Sets the functions that loading plugins needs. Call it before ECSIPlugins_Load.
/// @param hooks The functions. The core copies them.
void ECSIPlugins_SetHooks(const ECSIPluginHooks *hooks);

/// @brief Loads plugins, after the plugins they depend on, and runs their ECSPlugin_Init.
/// @param directories Directories that hold plugin folders, searched in order. Each ends with a separator.
/// @param directoryCount Number of directories.
/// @param plugins A table of the plugins to load: named fields give a plugin's name and its minimum version, such as depends = { ui = "1.0" }; list items give only a name. NULL loads nothing.
/// @param neededBy What names these plugins, for reports, such as "the preset '/path/default.lua'".
/// @return SHUResult_Ok if every plugin loaded; otherwise the last error. Failing plugins are reported and skipped.
SHUWUR SHUResult ECSIPlugins_Load(const char *const *directories, usz directoryCount, const ECSValue *plugins, const char *neededBy);

/// @brief Shuts every plugin down, in reverse load order: its Lua code's shutdown, then its ECSPlugin_Shutdown. Timers and events still work then.
void ECSIPlugins_Shutdown(void);

/// @brief Frees every plugin and closes their libraries. Call it after ECSIPlugins_Shutdown, when no worker thread runs their code.
void ECSIPlugins_Unload(void);

/// @brief Registers a plugin's state like ECSPlugin_RegisterState, with a function that releases its data when the plugin goes away. The Bindings module uses it for Lua.
/// @param release Function called with the description's data when the plugin fails or is unloaded, or NULL.
SHUWUR SHUResult ECSIPlugin_RegisterState(ECSPlugin plugin, const ECSPluginStateDesc *desc, ECSTimerFunction release);

/// @brief Sets the function that shuts down a plugin's Lua code, called before its native ECSPlugin_Shutdown. A function set before is released.
/// @param function The function, or NULL for none.
/// @param release Function called with the data when the plugin fails or is unloaded, or NULL.
/// @param data Passed to the functions.
void ECSIPlugin_SetLuaShutdown(ECSPlugin plugin, ECSTaskFunction function, ECSTaskFunction release, void *data);

/// @brief Restores the state of every plugin that registered one, from a session's plugin_state table. A plugin whose state is missing keeps its own. Errors are reported.
/// @param states The plugin_state table: for each plugin's name, a table with its state and state_version. NULL restores nothing.
void ECSIPlugins_RestoreStates(const ECSValue *states);

/// @brief Saves the state of every plugin that registered one into a plugin_state table. Entries of other plugins are kept.
/// @param states The table to fill; a value that is not a table becomes one.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation. A plugin whose Save fails is reported and skipped.
SHUWUR SHUResult ECSIPlugins_SaveStates(ECSValue *states);

/// @brief Gets a plugin's name.
/// @param plugin The plugin.
/// @return The name its manifest gives.
const char *ECSIPlugin_GetName(ECSPlugin plugin);

/// @brief Gets a plugin's version.
/// @param plugin The plugin.
/// @return The version its manifest gives.
const char *ECSIPlugin_GetVersion(ECSPlugin plugin);

/// @brief Reports an error of a plugin's callback. Repeats of the same error are counted, not reported again; ECSIPlugins_Unload reports the counts.
/// @param plugin The plugin.
/// @param message The error.
void ECSIPlugin_ReportError(ECSPlugin plugin, const char *message);

/// @brief Checks whether a plugin's manifest depends on another plugin. A plugin counts as depending on itself.
/// @param plugin The plugin.
/// @param other The other plugin.
/// @return true if the plugin may use the other plugin's services and events.
bool ECSIPlugin_DependsOn(ECSPlugin plugin, ECSPlugin other);

/// @brief Checks that a name that a plugin registers starts with the plugin's name and a dot, and reports it if not.
/// @param plugin The plugin.
/// @param name The name, such as "canvas.view".
/// @return true if the name belongs to the plugin.
bool ECSIPlugin_OwnsName(ECSPlugin plugin, const char *name);

#pragma endregion Declarations
