#pragma once

// Session: reading presets and sessions, building the workspaces they describe, and saving sessions.

#include "base/Values.h"
#include "runtime/Plugins.h"

#pragma region Declarations

/// @brief What a preset says about its tool before anything starts.
typedef struct ECSIPresetInfo
{
    char *name;
    char *appId;
    char *appName;
    char *pluginsDirectory; // NULL if the preset names none; ends with a separator
    ECSValue *file;         // the whole file; its depends field names the plugins it needs
} ECSIPresetInfo;

/// @brief Finds a preset's file.
/// @param retPath The path. Free it with SDL_free.
/// @param nameOrPath A preset name, looked up in the presets shipped with OpenECS, or a path to a .lua file.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSISession_FindPreset(char **retPath, const char *nameOrPath);

/// @brief Reads a preset's identity and the plugins it needs.
/// @param path Path of the preset.
/// @param retInfo What the preset says. Free it with ECSISession_FreeInfo, also after a failure.
/// @return SHUResult_Ok, SHUResult_ErrAllocation, or the error of the Lua module.
SHUWUR SHUResult ECSISession_ReadInfo(const char *path, ECSIPresetInfo *retInfo);

/// @brief Frees what ECSISession_ReadInfo read.
/// @param info What the preset said.
void ECSISession_FreeInfo(ECSIPresetInfo *info);

/// @brief Builds the workspaces that a preset or session describes, creates their panels, and shows the current workspace. A bad part of the file is reported with its path, such as workspaces[1].windows[1], and skipped.
/// @param path Path of the file, for reports.
/// @param info What ECSISession_ReadInfo read from the file.
/// @return SHUResult_Ok, SHUResult_ErrBadData if the file has no workspaces, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSISession_Apply(const char *path, const ECSIPresetInfo *info);

/// @brief Builds a session: the file it came from, with the current workspaces, panels and their saved state.
/// @param info What the preset or session that started this tool said.
/// @param retSession The value to fill.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSISession_Build(const ECSIPresetInfo *info, ECSValue *retSession);

/// @brief Registers the core's function ecs.saveSession, which saves the session to a file the user chooses.
/// @param folder The folder where its dialog starts, ending with a separator, or NULL to let the system choose. It is created when the dialog opens.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSISession_Initialize(const char *folder);

/// @brief Frees what the Session module holds.
void ECSISession_Terminate(void);

#pragma endregion Declarations
