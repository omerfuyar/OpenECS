#pragma once

// Lua: the Lua state. Reads presets, sessions, manifests and settings files as data, and runs Lua code in protected calls.

#include "base/Values.h"

#pragma region Declarations

/// @brief The Lua state, as lua.h declares it, so a header can name it without including Lua.
typedef struct lua_State lua_State;

/// @brief LuaJIT's type of cdata, which lua.h does not name.
#define OPENECS_LUA_TCDATA 10

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

/// @brief Gets the samples of LuaJIT's profiler of the core's state: the Lua function that ran, as its file's full path, then its name or the line where it
/// starts, such as "/plugins/ui/init.lua:element"; not followed by a zero byte.
typedef void (*ECSILuaSampleFunction)(const char *function, usz length, int samples);

/// @brief Starts LuaJIT's sampling profiler on the core's state, which samples every millisecond, or stops it (DESIGN 3.5).
/// @param Sample Gets each sample of Lua code; nullptr stops the profiler.
void ECSILua_SetSampling(ECSILuaSampleFunction Sample);

/// @brief Pushes the table of the fields of the struct at an index of the stack, for values. Gives false, and pushes nothing, for a value that is not a
/// struct.
typedef bool (*ECSILuaPushStructFieldsFunction)(int index);

/// @brief Sets the functions that turn handles and structs into values, and handles back. The Services module, which owns them, sets them; without
/// them, handles and structs are skipped.
/// @param Read Reads a handle.
/// @param Push Pushes a handle.
/// @param PushStructFields Pushes a struct's fields.
void ECSILua_SetValueFunctions(ECSILuaReadHandleFunction Read, ECSILuaPushHandleFunction Push, ECSILuaPushStructFieldsFunction PushStructFields);

/// @brief Copies the Lua value at an index of a state's stack into a value, by the rules of ECSILua_ReadData. A string with a zero byte becomes a buffer.
/// In the core's state a handle stays a handle; another state's value goes to another thread, so a handle is skipped there.
/// @param state The core's state, or one of ECSILua_NewState.
/// @param index Index of the Lua value.
/// @param retValue The value to set.
/// @return SHUResult_Ok, SHUResult_ErrBadData if tables are nested too deeply, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSILua_GetValue(lua_State *state, int index, ECSValue *retValue);

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

/// @brief ECSILua_Misuse for a Lua function that a C function checks for: the error names the line at a level of the stack, where 1 is the function that called the C function.
/// @param state The state whose code called.
/// @param level The level of the stack whose line the error names.
/// @param format printf-style format, as lua_pushfstring takes it.
[[noreturn]] void ECSILua_MisuseAt(lua_State *state, int level, const char *format, ...);

/// @brief luaL_argerror, which notes the misuse first.
[[noreturn]] void ECSILua_ArgError(lua_State *state, int argument, const char *message);

/// @brief ECSILua_ArgError for the function at a level of the stack, where 0 is the C function that runs, such as a Lua wrapper of a C function that calls a check.
/// @param state The state whose code called.
/// @param level The level of the function whose argument is bad.
/// @param argument The argument's number.
/// @param message What is wrong with it.
[[noreturn]] void ECSILua_ArgErrorAt(lua_State *state, int level, int argument, const char *message);

/// @brief luaL_typeerror, which notes the misuse first.
[[noreturn]] void ECSILua_TypeError(lua_State *state, int argument, const char *typeName);

/// @brief ECSILua_TypeError for the function at a level of the stack, of a value at an index.
/// @param state The state whose code called.
/// @param level The level of the function whose argument is bad, as ECSILua_ArgErrorAt takes it.
/// @param argument The argument's number.
/// @param index Index of the argument's value on the stack.
/// @param typeName The type it should have.
[[noreturn]] void ECSILua_TypeErrorAt(lua_State *state, int level, int argument, int index, const char *typeName);

/// @brief Pushes the Lua function that gives the bytes of a buffer that is not a string (DESIGN 10.7): a string.buffer's, or a cdata array's or struct's,
/// as a uint8_t pointer and a length; or nothing for any other value.
void ECSILua_PushBytesOf(lua_State *state);

/// @brief Gives the bytes of a buffer at an index of the stack (DESIGN 10.7): a string's, a string.buffer's, or a cdata array's or struct's, valid while
/// the value is.
/// @param retText Set to whether they are a string's, which must not change; may be nullptr.
/// @return nullptr if the value is none of them.
const char *ECSILua_ToBytes(lua_State *state, int index, usz *retLength, bool *retText);

/// @brief ECSILua_ToBytes of an argument, which raises a type error if the argument is no buffer.
const char *ECSILua_CheckBytes(lua_State *state, int argument, usz *retLength, bool *retText);

/// @brief The checks of luaL_check..., luaL_opt... and luaL_argcheck, which note the misuse before they raise an error.
const char *ECSILua_CheckLString(lua_State *state, int argument, usz *retLength);
const char *ECSILua_CheckString(lua_State *state, int argument);
const char *ECSILua_OptString(lua_State *state, int argument, const char *fallback);
i64 ECSILua_CheckInteger(lua_State *state, int argument);

/// @brief Reads a 64-bit integer at an index of the core's state: a whole number, or an int64_t or uint64_t cdata of LuaJIT's FFI, such as 123LL,
/// for one past 2^53. Another state reads a whole number only.
/// @param state The state.
/// @param index Index of the value.
/// @param retValue The integer, or 0 when the value is not one.
/// @return Whether the value is an integer.
bool ECSILua_ToInt64(lua_State *state, int index, i64 *retValue);
i64 ECSILua_OptInteger(lua_State *state, int argument, i64 fallback);
f64 ECSILua_CheckNumber(lua_State *state, int argument);
f64 ECSILua_OptNumber(lua_State *state, int argument, f64 fallback);
void ECSILua_CheckType(lua_State *state, int argument, int type);
void ECSILua_CheckAny(lua_State *state, int argument);
void *ECSILua_CheckUdata(lua_State *state, int argument, const char *typeName);
int ECSILua_CheckOption(lua_State *state, int argument, const char *fallback, const char *const options[]);
#define ECSILua_ArgCheck(state, condition, argument, message) ((void)((condition) || (ECSILua_ArgError((state), (argument), (message)), 0)))

/// @brief Gives where Lua's wrappers of C functions write the function they call through the FFI, or nullptr between their calls (DESIGN 10.4).
/// Lua cannot run while it is set, as the call may be compiled code.
/// @return The place, which lives as long as the program.
void **ECSILua_GetFfiCall(void);

/// @brief Tells whether a C function that Lua called through the FFI runs, so the core must not use Lua.
/// @return true during such a call.
bool ECSILua_InFfiCall(void);

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
