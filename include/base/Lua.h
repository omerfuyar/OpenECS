#pragma once

// Lua: the Lua state. Reads presets, sessions, manifests and settings files as data, and runs Lua code in protected calls.

#include "base/Values.h"

#pragma region Declarations

/// @brief The Lua state, as lua.h declares it. Only the Bindings, Workers and Test modules use it.
typedef struct lua_State lua_State;

/// @brief Starts the Lua state.
/// @return SHUResult_Ok, SHUResult_ErrAllocation if Lua cannot start, or SHUResult_ErrInternal if the data file writer does not load.
SHUWUR SHUResult ECSILua_Initialize(void);

/// @brief Stops the Lua state.
void ECSILua_Terminate(void);

/// @brief Gets the Lua state, for the Bindings and Test modules.
/// @return The state.
lua_State *ECSILua_GetState(void);

/// @brief Reads the handle at an index of the stack, for values: the name of its type and its object. Gives false for a value that is not a handle, or whose object is gone.
typedef bool (*ECSILuaReadHandleFunction)(int index, const char **retType, void **retObject);

/// @brief Pushes the handle of an object of a type, for values.
typedef void (*ECSILuaPushHandleFunction)(const char *type, void *object);

/// @brief Sets the functions that turn handles into values and back. The Services module, which owns handles, sets them; without them, handles are skipped.
/// @param Read Reads a handle.
/// @param Push Pushes a handle.
void ECSILua_SetHandleFunctions(ECSILuaReadHandleFunction Read, ECSILuaPushHandleFunction Push);

/// @brief Copies the Lua value at an index of the stack into a value, by the rules of ECSILua_ReadData. A string with a zero byte becomes a buffer, and a handle stays a handle.
/// @param index Index of the Lua value.
/// @param value The value to set.
/// @return SHUResult_Ok, SHUResult_ErrBadData if tables are nested too deeply, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSILua_GetValue(int index, ECSValue *value);

/// @brief Pushes a value onto the Lua stack. A table becomes a Lua table with its list items and named fields.
/// @param value The value, or nullptr for nil.
void ECSILua_PushValue(const ECSValue *value);

/// @brief Starts a Lua state of its own, with every library open, for a worker's thread (DESIGN 3.4). Every function may use it on the thread that made it.
/// @return The state, to close with lua_close, or nullptr if out of memory.
lua_State *ECSILua_NewState(void);

/// @brief Copies the Lua value at an index of a state's stack into a value, as ECSILua_GetValue does, but skips handles, so the value can go to another thread.
/// @param state The state, the core's or one of ECSILua_NewState.
/// @param index Index of the Lua value.
/// @param value The value to set.
/// @return SHUResult_Ok, SHUResult_ErrBadData if tables are nested too deeply, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSILua_GetData(lua_State *state, int index, ECSValue *value);

/// @brief Pushes a value onto a state's stack, as ECSILua_PushValue does. On a state other than the core's, a handle becomes nil.
/// @param state The state.
/// @param value The value, or nullptr for nil.
void ECSILua_PushData(lua_State *state, const ECSValue *value);

/// @brief Calls the Lua function below its arguments on the stack, in a protected call.
/// @param argumentCount Number of arguments on top of the function.
/// @param resultCount Number of results to leave on the stack.
/// @return SHUResult_Ok with the results pushed, or SHUResult_Err with the error message and a stack trace pushed instead.
SHUWUR SHUResult ECSILua_Call(int argumentCount, int resultCount);

/// @brief Calls a Lua function on a state's stack in a protected call, as ECSILua_Call does on the core's state.
/// @param state The state.
/// @param argumentCount Number of arguments on top of the function.
/// @param resultCount Number of results to leave on the stack.
/// @return SHUResult_Ok with the results pushed, or SHUResult_Err with the error message and a stack trace pushed instead.
SHUWUR SHUResult ECSILua_CallIn(lua_State *state, int argumentCount, int resultCount);

/// @brief Runs a data file and copies the table it returns into a value. The file runs without access to the core. Integer keys from 1 up to the first missing one become list items, and text keys named fields, in the order of their names. Other keys, and functions and other values that are not data, are skipped and reported.
/// @param path Path of the file.
/// @param retValue The value to set to the table.
/// @return SHUResult_Ok, SHUResult_ErrFile if it cannot be read, SHUResult_ErrBadData if it fails, does not return a table or nests tables too deeply, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSILua_ReadData(const char *path, ECSValue *retValue);

/// @brief Writes a value as a data file that returns it. The file is written to a temporary file first, then renamed over the old one, so it is never left half-written. Missing folders are created.
/// @param path Path of the file.
/// @param value The value to write.
/// @return SHUResult_Ok, SHUResult_ErrFile if the file cannot be written, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSILua_WriteData(const char *path, const ECSValue *value);

#pragma endregion Declarations
