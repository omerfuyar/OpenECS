#pragma once

// Lua: the Lua state. Reads presets, sessions, manifests and settings files as data.

#include "shu/shu.h"

#pragma region Declarations

/// @brief Function called for each text value of a table, by ECSI_LuaDataForEachText.
typedef void (*ECSI_LuaTextFunction)(const char *key, const char *value, void *userData);

/// @brief Starts the Lua state.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation if Lua cannot start.
SHUWUR SHUResult ECSI_LuaInitialize(void);

/// @brief Stops the Lua state.
void ECSI_LuaTerminate(void);

/// @brief Runs a data file and opens the table it returns. The file runs without access to the core.
/// @param path Path of the file.
/// @return SHUResult_Ok, SHUResult_ErrFile if it cannot be read, or SHUResult_ErrBadData if it fails or does not return a table.
/// @note Close it with ECSI_LuaDataClose. Only one data file is open at a time.
SHUWUR SHUResult ECSI_LuaDataOpen(const char *path);

/// @brief Closes the open data file.
void ECSI_LuaDataClose(void);

/// @brief Enters a field of the current table, if that field is a table.
/// @param key Name of the field.
/// @return true if the field is a table; leave it with ECSI_LuaDataLeave.
bool ECSI_LuaDataEnterField(const char *key);

/// @brief Enters an item of the current table, if that item is a table.
/// @param index Position of the item, starting at 1.
/// @return true if the item is a table; leave it with ECSI_LuaDataLeave.
bool ECSI_LuaDataEnterIndex(usz index);

/// @brief Leaves the current table and returns to the one that contains it.
void ECSI_LuaDataLeave(void);

/// @brief Counts the items of the current table.
/// @return Number of items, from 1 up to the first missing one.
usz ECSI_LuaDataCount(void);

/// @brief Checks whether the current table has a field.
/// @param key Name of the field.
/// @return true if the field exists and is not nil.
bool ECSI_LuaDataHas(const char *key);

/// @brief Reads a text field of the current table.
/// @param key Name of the field.
/// @param fallback Value returned if the field is missing or not a text.
/// @return The text. Valid while the data file is open.
const char *ECSI_LuaDataGetText(const char *key, const char *fallback);

/// @brief Reads a number field of the current table.
/// @param key Name of the field.
/// @param fallback Value returned if the field is missing or not a number.
/// @return The number.
f64 ECSI_LuaDataGetNumber(const char *key, f64 fallback);

/// @brief Calls a function for every field of the current table whose key and value are texts.
/// @param function Function to call.
/// @param userData Passed to the function.
void ECSI_LuaDataForEachText(ECSI_LuaTextFunction function, void *userData);

#pragma endregion Declarations
