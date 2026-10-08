#pragma once

// Session: reading presets and sessions, and building the workspaces they describe.

#include "Plugins.h"
#include "Values.h"

#pragma region Declarations

/// @brief What a preset says about its tool before anything starts.
typedef struct ECSI_PresetInfo
{
    char *name;
    char *appId;
    char *appName;
    char *pluginsDirectory; // NULL if the preset names none; ends with a separator
    char **plugins;     // stb_ds array
    ECSValue *settings; // the preset's settings table; nil if it has none
} ECSI_PresetInfo;

/// @brief Finds a preset's file.
/// @param retPath The path. Free it with SDL_free.
/// @param nameOrPath A preset name, looked up in the presets shipped with OpenECS, or a path to a .lua file.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSI_SessionFindPreset(char **retPath, const char *nameOrPath);

/// @brief Reads a preset's identity and the plugins it needs.
/// @param path Path of the preset.
/// @param retInfo What the preset says. Free it with ECSI_SessionFreeInfo, also after a failure.
/// @return SHUResult_Ok, SHUResult_ErrAllocation, or the error of the Lua module.
SHUWUR SHUResult ECSI_SessionReadInfo(const char *path, ECSI_PresetInfo *retInfo);

/// @brief Frees what ECSI_SessionReadInfo read.
/// @param info What the preset said.
void ECSI_SessionFreeInfo(ECSI_PresetInfo *info);

/// @brief Builds the workspaces that a preset or session describes, and creates their panels.
/// @param path Path of the preset or session.
/// @return SHUResult_Ok, SHUResult_ErrBadData if the file has no workspaces, or the error of the Lua or layout module.
SHUWUR SHUResult ECSI_SessionApply(const char *path);

#pragma endregion Declarations
