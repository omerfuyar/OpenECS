#pragma once

// Plugins: finding plugins, reading their manifests, and loading them in dependency order.

#include "base/Values.h"

#pragma region Declarations

/// @brief Runs a plugin's Lua code.
/// @param plugin The plugin.
/// @param folder The plugin's folder, ending with a separator, where its require finds its modules.
/// @param path Path of its Lua file.
/// @return SHUResult_Ok, or an error to mark the plugin failed.
typedef SHUResult (*ECSIPluginStartLuaFunction)(ECSPlugin plugin, const char *folder, const char *path);

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
/// @param plugins A table of the plugins to load: named fields give a plugin's name and its minimum version, such as depends = { ui = "1.0" }; list items give only a name. nullptr loads nothing.
/// @param neededBy What names these plugins, for reports, such as "the preset '/path/default.lua'".
/// @return SHUResult_Ok if every plugin loaded; SHUResult_ErrBadData if a manifest is not valid or plugins depend on each other in a cycle, which stops the start; or SHUResult_Err if a plugin is missing, too old or failed, which is reported and skipped.
SHUWUR SHUResult ECSIPlugins_Load(const char *const *directories, usz directoryCount, const ECSValue *plugins, const char *neededBy);

/// @brief Adds a plugin that the executable contains, such as sdl, and runs its Init. Plugins depend on it in their manifests like on any plugin. Call it before ECSIPlugins_Load; it does nothing if the plugin is there already.
/// @param name The plugin's name.
/// @param version Its version, which manifests ask for.
/// @param description What it gives.
/// @param Init Registers what it gives, as a native plugin's ECSPlugin_Init does.
/// @return SHUResult_Ok, the error of Init, which marks the plugin failed and is reported, or SHUResult_ErrAllocation.
SHUResult ECSIPlugins_AddBuiltin(const char *name, const char *version, const char *description, SHUResult (*Init)(ECSPlugin plugin));

/// @brief Destroys every plugin, in reverse load order: the Destroy function of its description runs. Timers and events still work then.
void ECSIPlugins_Destroy(void);

/// @brief Frees every plugin and closes their libraries. Call it after ECSIPlugins_Destroy, when no worker thread runs their code.
void ECSIPlugins_Unload(void);

/// @brief Registers a plugin's description like ECSPlugin_Register, with a function that releases its data when the plugin goes away. The Bindings module uses it for Lua.
/// @param release Function called with the description's data when the plugin fails or is unloaded, or nullptr.
SHUWUR SHUResult ECSIPlugin_Register(ECSPlugin plugin, const ECSPluginDesc *desc, ECSTaskFunction release);

/// @brief Restores the state of every plugin that registered one, from a session's pluginState table. A plugin whose state is missing keeps its own. Errors are reported.
/// @param states The pluginState table: for each plugin's name, a table with its state and stateVersion. nullptr restores nothing.
void ECSIPlugins_RestoreStates(const ECSValue *states);

/// @brief Saves the state of every plugin that registered one into a pluginState table. Entries of other plugins are kept.
/// @param states The table to fill; a value that is not a table becomes one.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation. A plugin whose Save fails is reported and skipped.
SHUWUR SHUResult ECSIPlugins_SaveStates(ECSValue *states);

/// @brief Gets a plugin's name.
/// @param plugin The plugin.
/// @return The name its manifest gives.
const char *ECSIPlugin_GetName(ECSPlugin plugin);

/// @brief Gets a plugin's description, from its manifest.
/// @param plugin The plugin.
/// @return The description, or an empty string. Valid while the plugin is loaded.
const char *ECSIPlugin_GetDescription(ECSPlugin plugin);

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

/// @brief Makes the name of something a plugin registers from its local name: the plugin's name, a dot and the local name, such as "canvas.view" from "view". A local name that is empty, or that starts with the plugin's name and a dot, is reported.
/// @param plugin The plugin.
/// @param localName The local name.
/// @return The name, to free with SDL_free, or nullptr if the local name is not valid or memory runs out.
char *ECSIPlugin_FullName(ECSPlugin plugin, const char *localName);

/// @brief Checks that a name of a plugin's own, such as of one of its panel types, starts with the plugin's name and a dot, and reports it if not.
/// @param plugin The plugin.
/// @param name The name, such as "canvas.view".
/// @return true if the name belongs to the plugin.
bool ECSIPlugin_OwnsName(ECSPlugin plugin, const char *name);

/// @brief Checks whether the owner of a name runs: the text before the name's first dot is "ecs", the core, or a plugin that loaded and did not fail.
/// @param name The name, such as "canvas.grid".
/// @return true if the owner runs, so the name can be checked against what it registered.
bool ECSIPlugins_OwnerRuns(const char *name);

/// @brief Gets a plugin that loaded and did not fail, by name.
/// @param name The plugin's name.
/// @return The plugin, or nullptr.
ECSPlugin ECSIPlugins_Get(const char *name);

/// @brief Gets the plugins that loaded and did not fail, in load order.
/// @param index Position, starting at 0.
/// @return The plugin, or nullptr past the last one.
ECSPlugin ECSIPlugins_GetAt(usz index);

#pragma endregion Declarations
