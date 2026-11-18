#pragma once

// Lua: the Lua state. Reads presets, sessions, manifests and settings files as data, and runs Lua code in protected calls.

#include "base/Values.h"

#pragma region Declarations

/// @brief The Lua state, as lua.h declares it, so a header can name it without including Lua.
typedef struct lua_State lua_State;

/// @brief Starts the Lua state.
/// @return SHUResult_Ok, SHUResult_ErrAllocation if Lua cannot start, or SHUResult_ErrInternal if the data file writer does not load.
SHUWUR SHUResult ECSILua_Initialize(void);

/// @brief Stops the Lua state.
void ECSILua_Terminate(void);

/// @brief Gets the Lua state that runs the plugins' Lua code.
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

/// @brief Copies the Lua value at an index of a state's stack into a value, by the rules of ECSILua_ReadData. A string with a zero byte becomes a buffer.
/// In the core's state a handle stays a handle; another state's value goes to another thread, so a handle is skipped there.
/// @param state The core's state, or one of ECSILua_NewState.
/// @param index Index of the Lua value.
/// @param retValue The value to set.
/// @return SHUResult_Ok, SHUResult_ErrBadData if tables are nested too deeply, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSILua_GetValue(lua_State *state, int index, ECSValue *retValue);

/// @brief Copies the Lua value at an index of a state's stack into a value that goes to another thread: as ECSILua_GetValue does, but a handle is skipped with a
/// warning in the core's state too, as only the main thread may use the Lua handle that a value keeps.
/// @return SHUResult_Ok, SHUResult_ErrBadData if tables are nested too deeply, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSILua_GetMessage(lua_State *state, int index, ECSValue *retValue);

/// @brief Pushes a value onto a state's stack. A table becomes a Lua table with its list items and named fields. On a state other than the core's, a handle becomes nil.
/// @param state The state.
/// @param value The value, or nullptr for nil.
void ECSILua_PushValue(lua_State *state, const ECSValue *value);

/// @brief Raises a Lua argument error if a field of a table is neither a function nor nil. Check every field before allocating, then reference them.
/// @param state The state.
/// @param index Index of the table.
/// @param name Name of the field.
void ECSILua_CheckFunctionField(lua_State *state, int index, const char *name);

/// @brief Takes a registry reference to a function field of a table, which ECSILua_CheckFunctionField checked.
/// @param state The state.
/// @param index Index of the table.
/// @param name Name of the field.
/// @return The reference, or LUA_NOREF if the field is not a function.
int ECSILua_ReferenceFunctionField(lua_State *state, int index, const char *name);

/// @brief Starts a Lua state of its own, with every library open, for a worker's thread (DESIGN 3.4). Every function may use it on the thread that made it.
/// @return The state, to close with lua_close, or nullptr if out of memory.
lua_State *ECSILua_NewState(void);

/// @brief Notes a misuse of a core function from Lua, before Lua raises its error (DESIGN 1.4). The Plugins module provides it, through the App module.
/// @param message What the check says.
typedef void (*ECSILuaMisuseFunction)(const char *message);

/// @brief Sets the function that notes misuses. Call it before Lua code runs.
void ECSILua_SetMisuseFunction(ECSILuaMisuseFunction Misuse);

/// @brief Notes a misuse of the C function that Lua called, and raises an error at the Lua line that called it, as luaL_error does. For every check of a core function's arguments from Lua.
/// @param state The state whose code called.
/// @param format printf-style format, as lua_pushfstring takes it.
[[noreturn]] void ECSILua_Misuse(lua_State *state, const char *format, ...);

/// @brief luaL_argerror, which notes the misuse first.
[[noreturn]] void ECSILua_ArgError(lua_State *state, int argument, const char *message);

/// @brief luaL_typeerror, which notes the misuse first.
[[noreturn]] void ECSILua_TypeError(lua_State *state, int argument, const char *typeName);

/// @brief The checks of luaL_check..., luaL_opt... and luaL_argcheck, which note the misuse before they raise an error.
const char *ECSILua_CheckLString(lua_State *state, int argument, usz *retLength);
const char *ECSILua_CheckString(lua_State *state, int argument);
const char *ECSILua_OptString(lua_State *state, int argument, const char *fallback);
i64 ECSILua_CheckInteger(lua_State *state, int argument);
i64 ECSILua_OptInteger(lua_State *state, int argument, i64 fallback);
f64 ECSILua_CheckNumber(lua_State *state, int argument);
f64 ECSILua_OptNumber(lua_State *state, int argument, f64 fallback);
void ECSILua_CheckType(lua_State *state, int argument, int type);
void ECSILua_CheckAny(lua_State *state, int argument);
void *ECSILua_CheckUdata(lua_State *state, int argument, const char *typeName);
int ECSILua_CheckOption(lua_State *state, int argument, const char *fallback, const char *const options[]);
#define ECSILua_ArgCheck(state, condition, argument, message) ((void)((condition) || (ECSILua_ArgError((state), (argument), (message)), 0)))

/// @brief Sets the Lua state whose code calls C on this thread, while C runs for it, so a misuse can name the Lua line that made it.
/// @param state The state, or nullptr when the call returns.
/// @return The state set before, to set again when the call returns.
lua_State *ECSILua_SetCaller(lua_State *state);

/// @brief Gets the Lua state whose code calls C on this thread.
/// @return The state, or nullptr if C was not called from Lua.
lua_State *ECSILua_GetCaller(void);

/// @brief Calls the Lua function below its arguments on a state's stack, in a protected call.
/// @param state The state.
/// @param argumentCount Number of arguments on top of the function.
/// @param resultCount Number of results to leave on the stack.
/// @return SHUResult_Ok with the results pushed, or SHUResult_Err with the error message and a stack trace pushed instead.
SHUWUR SHUResult ECSILua_Call(lua_State *state, int argumentCount, int resultCount);

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
