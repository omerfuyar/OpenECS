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
    ECSValue *file; // the whole file; its depends field names the plugins it needs
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

/// @brief Builds the workspaces that a preset or session describes, creates their panels, and shows the current workspace. A bad part of the file is reported with its path, such as workspaces[1].windows[1], and skipped.
/// @param path Path of the file, for reports.
/// @param info What ECSI_SessionReadInfo read from the file.
/// @return SHUResult_Ok, SHUResult_ErrBadData if the file has no workspaces, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSI_SessionApply(const char *path, const ECSI_PresetInfo *info);

/// @brief Writes a session: the file it came from, with the current workspaces, panels and their saved state.
/// @param path Path of the session file.
/// @param info What the preset or session that started this tool said.
/// @return SHUResult_Ok, SHUResult_ErrFile if the file cannot be written, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSI_SessionSave(const char *path, const ECSI_PresetInfo *info);

#pragma endregion Declarations
