#pragma once

// Plugins: finding plugins, reading their manifests, loading them in dependency order, and the context table.

#include "Global.h"

#pragma region Declarations

/// @brief Loads plugins and the plugins they depend on, in dependency order, and runs their Init.
/// @param directories Directories that hold plugin folders, searched in order.
/// @param directoryCount Number of directories.
/// @param names Names of the plugins to load.
/// @param nameCount Number of names.
/// @return SHUResult_Ok if every plugin loaded; otherwise the last error. Failing plugins are reported and skipped.
SHUWUR SHUResult ECSI_PluginsLoad(const char *const *directories, usz directoryCount, const char *const *names, usz nameCount);

/// @brief Runs every plugin's Shutdown, in reverse load order, and closes their libraries.
void ECSI_PluginsUnload(void);

#pragma endregion Declarations
