#pragma once

// Lua: the Lua state. Reads presets, sessions, manifests and settings files as data, and runs Lua code in protected calls.

#include "Values.h"

#pragma region Declarations

/// @brief The Lua state, as lua.h declares it. Only the Bindings module uses it.
typedef struct lua_State lua_State;

/// @brief Starts the Lua state.
/// @return SHUResult_Ok, SHUResult_ErrAllocation if Lua cannot start, or SHUResult_ErrInternal if the data file writer does not load.
SHUWUR SHUResult ECSI_LuaInitialize(void);

/// @brief Stops the Lua state.
void ECSI_LuaTerminate(void);

/// @brief Gets the Lua state, for the Bindings module.
/// @return The state.
lua_State *ECSI_LuaGetState(void);

/// @brief Copies the Lua value at an index of the stack into a value, by the rules of ECSI_LuaReadData.
/// @param index Index of the Lua value.
/// @param value The value to set.
/// @return SHUResult_Ok, SHUResult_ErrBadData if tables are nested too deeply, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSI_LuaGetValue(int index, ECSValue *value);

/// @brief Pushes a value onto the Lua stack. A table becomes a Lua table with its list items and named fields.
/// @param value The value, or NULL for nil.
void ECSI_LuaPushValue(const ECSValue *value);

/// @brief Calls the Lua function below its arguments on the stack, in a protected call.
/// @param argumentCount Number of arguments on top of the function.
/// @param resultCount Number of results to leave on the stack.
/// @return SHUResult_Ok with the results pushed, or SHUResult_Err with the error message and a stack trace pushed instead.
SHUWUR SHUResult ECSI_LuaCall(int argumentCount, int resultCount);

/// @brief Runs a data file and copies the table it returns into a value. The file runs without access to the core. Integer keys from 1 up to the first missing one become list items, and text keys named fields, in the order of their names. Other keys, and functions and other values that are not data, are skipped and reported.
/// @param path Path of the file.
/// @param retValue The value to set to the table.
/// @return SHUResult_Ok, SHUResult_ErrFile if it cannot be read, SHUResult_ErrBadData if it fails, does not return a table or nests tables too deeply, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSI_LuaReadData(const char *path, ECSValue *retValue);

/// @brief Writes a value as a data file that returns it. The file is written to a temporary file first, then renamed over the old one, so it is never left half-written. Missing folders are created.
/// @param path Path of the file.
/// @param value The value to write.
/// @return SHUResult_Ok, SHUResult_ErrFile if the file cannot be written, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSI_LuaWriteData(const char *path, const ECSValue *value);

#pragma endregion Declarations
