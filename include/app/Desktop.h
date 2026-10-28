#pragma once

// Desktop: the entries of tools in the system's application launcher (DESIGN 13.9).

#include "app/Session.h"

#pragma region Declarations

/// @brief Writes the desktop entry of a preset's tool, so the system's launcher lists it and offers it to open the file types it takes.
/// @param info What ECSISession_ReadInfo read from the preset.
/// @param presetPath The preset's file.
/// @param presetName The name the command line gave the preset, or nullptr if it gave a path; the entry starts the tool by that name.
/// @return SHUResult_Ok, SHUResult_ErrNotFound if there is no folder for desktop entries, SHUResult_ErrAllocation, SHUResult_ErrFile, or SHUResult_Err on Windows, which has no desktop entries yet.
SHUWUR SHUResult ECSIDesktop_Install(const ECSIPresetInfo *info, const char *presetPath, const char *presetName);

/// @brief Removes the desktop entry of a preset's tool.
/// @param info What ECSISession_ReadInfo read from the preset.
/// @return SHUResult_Ok, SHUResult_ErrNotFound if the tool has no entry, SHUResult_ErrAllocation, SHUResult_ErrFile, or SHUResult_Err on Windows, which has no desktop entries yet.
SHUWUR SHUResult ECSIDesktop_Uninstall(const ECSIPresetInfo *info);

#pragma endregion Declarations
