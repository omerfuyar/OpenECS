#pragma once

// Settings: declarations, layers, and the value in effect of each setting.

#include "base/Values.h"
#include "runtime/Plugins.h"

#pragma region Declarations

/// @brief A settings layer. A higher layer overrides the lower ones.
typedef enum ECSISettingsLayer
{
    ECSISettingsLayer_Core = 0, // defaults of the core's settings
    ECSISettingsLayer_Plugin,   // defaults of plugins' settings
    ECSISettingsLayer_Preset,
    ECSISettingsLayer_Window, // the file that the settings window writes
    ECSISettingsLayer_User,   // the user's hand-edited file
    ECSISettingsLayer_Count,
} ECSISettingsLayer;

/// @brief Reads the settings layers: the core's settings file, the preset, and the user's files. Missing user files are fine.
/// @param corePath Path of the core's settings file, which holds the value of every core setting.
/// @param presetSettings The preset's settings table, or NULL.
/// @param presetPath Path of the preset, named where its settings are explained.
/// @param appId The tool's app id, which chooses the tool's own part of the user's files.
/// @param configFolder The user's configuration folder, ending with a separator, or NULL if there is none.
/// @return SHUResult_Ok, SHUResult_ErrFile or SHUResult_ErrBadData if the core's settings file cannot be read, or SHUResult_ErrAllocation. A user file that cannot be read is reported and skipped.
SHUWUR SHUResult ECSISettings_Initialize(const char *corePath, const ECSValue *presetSettings, const char *presetPath, const char *appId, const char *configFolder);

/// @brief Frees every declaration and layer.
void ECSISettings_Terminate(void);

/// @brief Declares a setting of the core. Its name starts with "ecs.", and its default is its value in the core's settings file; the description's default fields are not used.
/// @param desc Description of the setting.
/// @return SHUResult_Ok, SHUResult_ErrBadData if the description is invalid, the name is taken, or the core's settings file gives no value of the setting's type, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSISettings_DeclareCore(const ECSSettingDesc *desc);

/// @brief Declares a plugin's setting, like ECSSetting_Declare, with its default given as a value. The Bindings module uses it for Lua.
/// @param plugin The plugin that owns the setting.
/// @param desc Description of the setting; its default fields are ignored if defaultValue is given, except the choices.
/// @param defaultValue The default, or NULL to take it from the description.
/// @return SHUResult_Ok, SHUResult_ErrBadData if the description or the default is invalid or the name is taken, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSISettings_DeclarePlugin(ECSPlugin plugin, const ECSSettingDesc *desc, const ECSValue *defaultValue);

/// @brief Gets a declared setting's default: for a core setting, its value in the core's settings file.
/// @param name Name of the setting.
/// @return The default, or NULL if no setting has the name. Valid while the setting is declared.
const ECSValue *ECSISettings_GetDefault(const char *name);

/// @brief Gets the key bindings of a user file: its keys table for every tool, with the tool's own keys, which win.
/// @param layer ECSISettingsLayer_Window or ECSISettingsLayer_User.
/// @return A table of key texts and function names, or NULL. Valid until ECSISettings_Terminate.
const ECSValue *ECSISettings_GetKeys(ECSISettingsLayer layer);

/// @brief Describes a declared setting.
/// @param name Name of the setting.
/// @param retOwner Its owner; NULL for the core.
/// @param retType Its type.
/// @param retLayer The layer its value in effect comes from.
/// @return true if the setting is declared.
bool ECSISettings_Describe(const char *name, ECSPlugin *retOwner, ECSSettingType *retType, ECSISettingsLayer *retLayer);

/// @brief Tells owners about the settings whose value in effect changed since the last call. The main loop calls it after delivering events.
void ECSISettings_DeliverChanges(void);

/// @brief Removes every setting a plugin declared. The values in the layers are kept.
/// @param plugin The plugin.
void ECSISettings_RemovePlugin(ECSPlugin plugin);

/// @brief Reports the settings of the files that are not declared, though their owner runs (ECSIPlugins_OwnerRuns), so a misspelt name is noticed. They are kept. Call it once the plugins are loaded.
void ECSISettings_ReportUndeclared(void);

/// @brief Gets the plugins that the settings files name: the core's file, and the user's files for every tool and for this tool.
/// @return A list of plugin names, for ECSIPlugins_Load. Valid until ECSISettings_Terminate.
const ECSValue *ECSISettings_GetPlugins(void);


#pragma endregion Declarations
