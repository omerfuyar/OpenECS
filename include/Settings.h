#pragma once

// Settings: declarations, layers, and the value in effect of each setting.

#include "Plugins.h"
#include "Values.h"

#pragma region Declarations

/// @brief A settings layer. A higher layer overrides the lower ones.
typedef enum ECSI_SettingsLayer
{
    ECSI_SettingsLayer_Core = 0, // defaults of the core's settings
    ECSI_SettingsLayer_Plugin,   // defaults of plugins' settings
    ECSI_SettingsLayer_Preset,
    ECSI_SettingsLayer_Window, // the file that the settings window writes
    ECSI_SettingsLayer_User,   // the user's hand-edited file
    ECSI_SettingsLayer_Count,
} ECSI_SettingsLayer;

/// @brief Reads the settings layers of the preset and the user's files. Missing user files are fine.
/// @param presetSettings The preset's settings table, or NULL.
/// @param presetPath Path of the preset, named where its settings are explained.
/// @param appId The tool's app id, which chooses the tool's own part of the user's files.
/// @param configFolder The user's configuration folder, ending with a separator, or NULL if there is none.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation. A user file that cannot be read is reported and skipped.
SHUWUR SHUResult ECSI_SettingsInitialize(const ECSValue *presetSettings, const char *presetPath, const char *appId, const char *configFolder);

/// @brief Frees every declaration and layer.
void ECSI_SettingsTerminate(void);

/// @brief Declares a setting of the core. Its name starts with "ecs.".
/// @param desc Description of the setting.
/// @return SHUResult_Ok, SHUResult_ErrBadData if the description is invalid or the name is taken, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSI_SettingsDeclareCore(const ECSSettingDesc *desc);

/// @brief Declares a plugin's setting, like ECSSetting_Declare, with its default given as a value. The Bindings module uses it for Lua.
/// @param plugin The plugin that owns the setting.
/// @param desc Description of the setting; its default fields are ignored if defaultValue is given, except the choices.
/// @param defaultValue The default, or NULL to take it from the description.
/// @return SHUResult_Ok, SHUResult_ErrBadData if the description or the default is invalid or the name is taken, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSI_SettingsDeclarePlugin(ECSPlugin plugin, const ECSSettingDesc *desc, const ECSValue *defaultValue);

/// @brief Gets the plugins that the user's files name for every tool and for this tool.
/// @return A list of plugin names, for ECSI_PluginsLoad. Valid until ECSI_SettingsTerminate.
const ECSValue *ECSI_SettingsGetPlugins(void);

#pragma endregion Declarations
