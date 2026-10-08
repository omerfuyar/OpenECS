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
/// @return SHUResult_Ok, or SHUResult_ErrAllocation. A user file that cannot be read is reported and skipped.
SHUWUR SHUResult ECSI_SettingsInitialize(const ECSValue *presetSettings, const char *presetPath, const char *appId);

/// @brief Frees every declaration and layer.
void ECSI_SettingsTerminate(void);

/// @brief Declares a setting of the core. Its name starts with "ecs.".
/// @param desc Description of the setting.
/// @return SHUResult_Ok, SHUResult_ErrBadData if the description is invalid or the name is taken, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSI_SettingsDeclareCore(const ECSSettingDesc *desc);

/// @brief Gets the plugins that the user's files name for every tool and for this tool.
/// @return stb_ds array of plugin names. Valid until ECSI_SettingsTerminate.
const char *const *ECSI_SettingsGetPlugins(void);

#pragma endregion Declarations
