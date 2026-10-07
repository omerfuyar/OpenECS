#pragma once

// Session: reading presets and sessions, and building the workspaces they describe.

#include "Global.h"

#pragma region Declarations

/// @brief What a preset says about its tool before anything starts.
typedef struct ECSI_PresetInfo
{
    char name[OPENECS_NAME_CAPACITY];
    char appId[OPENECS_NAME_CAPACITY];
    char appName[OPENECS_NAME_CAPACITY];
    char pluginsDirectory[OPENECS_PATH_CAPACITY]; // empty if the preset names none
    char plugins[OPENECS_MAX_PLUGINS][OPENECS_NAME_CAPACITY];
    usz pluginCount;
} ECSI_PresetInfo;

/// @brief Finds a preset's file.
/// @param retPath Buffer for the path.
/// @param nameOrPath A preset name, looked up in the presets shipped with OpenECS, or a path to a .lua file.
void ECSI_SessionFindPreset(SHUSlice retPath, const char *nameOrPath);

/// @brief Reads a preset's identity and the plugins it needs.
/// @param path Path of the preset.
/// @param retInfo What the preset says.
/// @return SHUResult_Ok, or the error of the Lua module.
SHUWUR SHUResult ECSI_SessionReadInfo(const char *path, ECSI_PresetInfo *retInfo);

/// @brief Builds the workspaces that a preset or session describes, and creates their panels.
/// @param path Path of the preset or session.
/// @return SHUResult_Ok, SHUResult_ErrBadData if the file has no workspaces, or the error of the Lua module.
SHUWUR SHUResult ECSI_SessionApply(const char *path);

#pragma endregion Declarations
