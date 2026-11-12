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

/// @brief What a callback's misuse costs (DESIGN 14.2).
typedef enum ECSICallbackType : u8
{
    ECSICallbackType_Plugin, // the plugin is disabled, with the plugins that depend on it
    ECSICallbackType_Panel,  // the panel faults
    ECSICallbackType_Quiet,  // the misuse is only reported, as the plugin or panel goes away already, or the caller deals with it
} ECSICallbackType;

/// @brief A plugin's callback while it runs on a thread, from ECSIPlugin_Enter to ECSIPlugin_Leave. Callbacks inside it, such as a panel that a key's function opens, are entered too; a misuse belongs to the innermost.
typedef struct ECSICallback
{
    ECSPlugin plugin;
    ECSPanel panel; // the panel of a panel's or popup's callback, or nullptr
    ECSICallbackType type;
    char *misuse;               // the first misuse in it, or nullptr
    struct ECSICallback *outer; // the callback it runs inside, or nullptr
} ECSICallback;

/// @brief Deals with a callback that misused a function, once it returned: faults its panel, or disables its plugin. Any thread; a panel's callback runs on the main thread.
/// @param callback The callback; its misuse is valid during the call.
typedef void (*ECSIPluginMisusedFunction)(const ECSICallback *callback);

/// @brief Notes a misuse as it happens, such as to fail a test. Any thread.
/// @param message What the check says.
typedef void (*ECSIPluginMisuseFunction)(const char *message);

/// @brief Functions of later modules that loading plugins needs. The App module provides them, because modules include only the modules before them.
typedef struct ECSIPluginHooks
{
    ECSIPluginStartLuaFunction StartLua;
    ECSIPluginRemoveFunction RemoveRegistrations;
    ECSIPluginMisusedFunction Misused;
    ECSIPluginMisuseFunction Misuse;
} ECSIPluginHooks;

/// @brief Sets the functions that loading plugins needs. Call it before ECSIPlugins_Load.
/// @param hooks The functions. The core copies them.
void ECSIPlugins_SetHooks(const ECSIPluginHooks *hooks);

/// @brief Loads plugins, after the plugins they depend on, and runs their ECSPlugin_Init.
/// @param folders Folders that hold plugins, searched in order. Each ends with a separator.
/// @param folderCount Number of folders.
/// @param plugins A table of the plugins to load: named fields give a plugin's name and its minimum version, such as depends = { ui = "1.0" }; list items give only a name. nullptr loads nothing.
/// @param neededBy What names these plugins, for reports, such as "the preset '/path/default.lua'".
/// @return SHUResult_Ok if every plugin loaded; SHUResult_ErrBadData if a manifest is not valid or plugins depend on each other in a cycle, which stops the start; or SHUResult_Err if a plugin is missing, too old or failed, which is reported and skipped.
SHUWUR SHUResult ECSIPlugins_Load(const char *const *folders, usz folderCount, const ECSValue *plugins, const char *neededBy);

/// @brief Destroys every plugin, in reverse load order: the Destroy function of its description runs. Timers and events still work then.
void ECSIPlugins_Destroy(void);

/// @brief Reports each error of a plugin's callback that happened more than once, with how often it happened.
void ECSIPlugins_ReportRepeatedErrors(void);

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
/// @return SHUResult_Ok, or SHUResult_ErrAllocation. A plugin whose Save fails is reported, and its entry is kept.
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

/// @brief Reports an error of a plugin's callback. Repeats of the same error are counted, not reported again; ECSIPlugins_ReportRepeatedErrors reports the counts.
/// @param plugin The plugin.
/// @param message The error.
void ECSIPlugin_ReportError(ECSPlugin plugin, const char *message);

/// @brief Checks whether a plugin's manifest depends on another plugin. A plugin counts as depending on itself.
/// @param plugin The plugin.
/// @param other The other plugin.
/// @return true if the plugin may use the other plugin's services and events.
bool ECSIPlugin_DependsOn(ECSPlugin plugin, ECSPlugin other);

/// @brief Makes the name of something a plugin registers from its local name: the plugin's name, a dot and the local name, such as "canvas.view" from "view". A local name that is empty, or that starts with the plugin's name and a dot, is a misuse (DESIGN 1.4).
/// @param plugin The plugin.
/// @param localName The local name.
/// @param retName The name, to free with SDL_free, or nullptr if it is not made.
/// @return SHUResult_Ok, SHUResult_ErrAssertion if the local name is not valid, or SHUResult_ErrAllocation.
SHUResult ECSIPlugin_MakeFullName(ECSPlugin plugin, const char *localName, char **retName);

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

/// @brief Visits a plugin, for ECSIPlugins_ForEach.
typedef SHUResult (*ECSIPluginsVisitFunction)(ECSPlugin plugin, void *data);

/// @brief Calls a function for each plugin that loaded and did not fail, in load order.
/// @param Visit The function.
/// @param data Passed to the function.
/// @return SHUResult_Ok, or the first error that the function gave; every plugin is visited still.
SHUResult ECSIPlugins_ForEach(ECSIPluginsVisitFunction Visit, void *data);

/// @brief Tells whether a plugin's code runs: it loaded, did not fail and is not disabled.
bool ECSIPlugin_IsEnabled(ECSPlugin plugin);

/// @brief Marks a plugin that is about to be disabled: it takes no new background work until it is loaded again.
void ECSIPlugin_StopWork(ECSPlugin plugin);

/// @brief Tells whether a plugin may start background work: its code runs and it is not being disabled.
bool ECSIPlugin_TakesWork(ECSPlugin plugin);

/// @brief Gets the number of a plugin's load: it grows each time the plugin's code is loaded or unloaded, so work that an earlier load sent is dropped.
u32 ECSIPlugin_GetGeneration(ECSPlugin plugin);

/// @brief Names the plugins that a session disabled, before ECSIPlugins_Load: they, and the plugins that depend on them, are recorded without loading their code.
/// @param names The names; the caller keeps them while plugins load.
void ECSIPlugins_SetDisabled(const char *const *names, usz count);

/// @brief Gets a plugin and every enabled plugin that depends on it, through others too, the last loaded first: the plugins that disabling it disables.
/// @return stb_ds array of the plugins. Free it with arrfree.
ECSPlugin *ECSIPlugins_GetDependents(ECSPlugin plugin);

/// @brief Disables an enabled plugin: keeps the state its SaveState gives, runs its Destroy, removes what it registered and unloads its library. The caller first deals with its panels, workers, background work and handles.
/// @param misuse What the misuse said, for a plugin disabled because it, or a plugin it depends on, misused a function; nullptr when the disable was asked for. Its placeholders show it.
void ECSIPlugins_Disable(ECSPlugin plugin, const char *misuse);

/// @brief Gets why a disabled plugin was disabled, by its name or by a name it owns, such as a panel type's.
/// @return What the misuse said, or nullptr if the plugin is not disabled or its disable was asked for. Valid until the plugin is enabled.
const char *ECSIPlugins_GetMisuse(const char *name);

/// @brief Enables a plugin, and the plugins it depends on, as at start: a disabled plugin or one that never loaded. It gets back the state it kept.
/// @param folders Folders that hold plugins, as for ECSIPlugins_Load.
/// @return SHUResult_Ok, or the error of the plugin or of a plugin it needs, which is reported.
SHUWUR SHUResult ECSIPlugins_Enable(const char *const *folders, usz folderCount, const char *name);

/// @brief Lists every plugin in some folders of plugins, as ECSPlugins_List gives them.
SHUWUR SHUResult ECSIPlugins_List(const char *const *folders, usz folderCount, ECSValue *retList);

/// @brief Tells whether a plugin is disabled, by its name or by a name it owns, such as a panel type's.
bool ECSIPlugins_IsDisabled(const char *name);

/// @brief Marks the start of a plugin's callback on this thread, so a misuse in it is charged to it (DESIGN 14.2). Every call of a plugin's code goes between ECSIPlugin_Enter and ECSIPlugin_Leave.
/// @param callback Kept by the caller until ECSIPlugin_Leave, such as on its stack.
/// @param plugin The plugin whose code runs, or nullptr for a callback of the core's own, whose misuse is only reported.
/// @param panel The panel of a panel's or popup's callback, or nullptr.
/// @param type What a misuse costs.
void ECSIPlugin_Enter(ECSICallback *callback, ECSPlugin plugin, ECSPanel panel, ECSICallbackType type);

/// @brief Marks the end of the callback that ECSIPlugin_Enter started. If it misused a function, its panel faults or its plugin is disabled, as its type says.
/// @param callback The callback.
/// @return true if it misused a function.
bool ECSIPlugin_Leave(ECSICallback *callback);

/// @brief Notes a misuse: logs it in the name of the plugin whose callback runs on this thread, with the Lua stack trace when Lua made the call, and charges it to that callback.
/// @param checker The plugin whose check failed, or nullptr for the core.
/// @param message What the check says.
void ECSIPlugin_NoteMisuse(ECSPlugin checker, const char *message);

/// @brief Counts the misuses on this thread, so a caller sees whether a call misused a function.
u64 ECSIPlugin_GetMisuseCount(void);

/// @brief Gets what the last misuse on this thread said, without a stack trace, for the error that Lua raises for it.
/// @return The message, valid until the next misuse on this thread.
const char *ECSIPlugin_GetLastMisuse(void);

/// @brief Tells whether the callback that runs on this thread misused a function, so its error is not reported twice.
bool ECSIPlugin_IsMisusing(void);

/// @brief Lists the names of the disabled plugins, for the session.
SHUWUR SHUResult ECSIPlugins_GetDisabled(ECSValue *retList);

#pragma endregion Declarations
