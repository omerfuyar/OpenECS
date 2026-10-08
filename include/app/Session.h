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

/// @brief Folders of the files that presets and sessions are read from and written to. Each ends with a separator, or is NULL if there is none.
typedef struct ECSISessionFolders
{
    const char *presets;  // the user's presets
    const char *sessions; // saved sessions, where the dialogs of sessions start
    const char *state;    // the state folder, which holds each tool's last session
} ECSISessionFolders;

/// @brief Finds a preset's file.
/// @param retPath The path. Free it with SDL_free.
/// @param nameOrPath A preset name, looked up in the user's presets, then in the presets shipped with OpenECS; or a path, which has a '/' or ends with .lua.
/// @param userFolder The user's presets folder, ending with a separator, or NULL.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSISession_FindPreset(char **retPath, const char *nameOrPath, const char *userFolder);

/// @brief Finds a tool's last session file.
/// @param stateFolder The state folder, ending with a separator, or NULL.
/// @param appId The tool's app id.
/// @return The path, or NULL if there is no state folder. Free it with SDL_free.
char *ECSISession_GetLastPath(const char *stateFolder, const char *appId);

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

/// @brief Keeps the folders, and registers the core's functions ecs.session.save and ecs.session.open for keys and menus. They ask for the file.
/// @param folders The folders. The sessions folder is where the dialogs start, or the system chooses if it is NULL; it is created when a dialog opens.
/// @param canOpen false during a test, which starts from its preset alone and cannot restart; ECSSession_Open and ECSSession_OpenPreset then refuse.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSISession_Initialize(const ECSISessionFolders *folders, bool canOpen);

/// @brief Gets the session or preset that ECSSession_Open or ECSSession_OpenPreset chose, which OpenECS starts again from once it stops.
/// @param retOption The command-line option that names it, "--session" or "--preset"; NULL if not needed.
/// @return The file's path, or NULL. Valid until ECSISession_Terminate.
const char *ECSISession_GetNext(const char **retOption);

/// @brief Frees what the Session module holds.
void ECSISession_Terminate(void);

#pragma endregion Declarations
