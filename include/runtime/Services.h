#pragma once

// Services: the function registry, signatures, and calls between C and Lua. The only module that calls libffi.

#include "runtime/Events.h"

#pragma region Declarations

/// @brief Prepares the table that gives each object one Lua handle, and registers the core's handle types ecs.panel and ecs.surface.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSIServices_Initialize(void);

/// @brief Destroys the objects of every handle Lua still holds, while their plugins are loaded, and frees every registered function and handle type.
void ECSIServices_Terminate(void);

/// @brief Pushes the Lua handle of an object: the one Lua already has, or a new one.
/// @param type Name of the object's handle type.
/// @param object The object, or NULL for nil. Nil is pushed too if the type is gone, because its plugin was removed.
void ECSIServices_PushHandle(const char *type, void *object);

/// @brief Reads a Lua handle of a type. Raises a Lua error if the value is not a handle of that type, or its object or type is gone.
/// @param index Index of the handle on the Lua stack.
/// @param type Name of the handle type.
/// @return The object.
void *ECSIServices_CheckHandle(int index, const char *type);

/// @brief Makes an object's Lua handle invalid, without destroying the object, because the object is gone.
/// @param object The object.
void ECSIServices_ForgetHandle(void *object);

/// @brief Pushes the metatable of a handle type, so the Bindings module can give its handles methods.
/// @param type Name of the handle type. Nil is pushed if the type is gone.
void ECSIServices_PushHandleMetatable(const char *type);

/// @brief Registers a handle type that a Lua plugin provides. Its objects are Lua values, which the core keeps until the handle is collected.
/// @param plugin The plugin.
/// @param name Name of the type; it starts with the plugin's name.
/// @return SHUResult_Ok, SHUResult_ErrBadData if the name is taken or not the plugin's, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSIServices_RegisterLuaHandleType(ECSPlugin plugin, const char *name);

/// @brief Pushes a new handle of a type that a Lua plugin provides, standing for a Lua value. Raises a Lua error if the type is not the plugin's.
/// @param plugin The plugin that calls.
/// @param name Name of the handle type.
/// @param index Index of the Lua value on the stack.
void ECSIServices_PushLuaHandle(ECSPlugin plugin, const char *name, int index);

/// @brief Pushes the Lua value that a handle of a Lua plugin's type stands for. Raises a Lua error if the handle is not of the type, its object is gone, or the type is not the plugin's.
/// @param plugin The plugin that calls.
/// @param name Name of the handle type.
/// @param index Index of the handle on the stack.
void ECSIServices_PushLuaHandleValue(ECSPlugin plugin, const char *name, int index);

/// @brief Removes every function a plugin registered.
/// @param plugin The plugin.
void ECSIServices_RemovePlugin(ECSPlugin plugin);

/// @brief Registers one of the core's own functions, such as ecs.layout.maximize, so keys and plugins can call it.
/// @param name Name of the function. It starts with "ecs.".
/// @param function The function.
/// @param signature The function's signature.
/// @param description One line that says what the function does.
/// @return SHUResult_Ok, SHUResult_ErrBadData if the name or signature is invalid or the name is taken, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSIServices_RegisterCore(const char *name, ECSFunction function, const char *signature, const char *description);

/// @brief Calls a function that a key is bound to. It takes no arguments, or the focused panel.
/// @param name Name of the function.
/// @param focus The focused panel, or NULL.
/// @return SHUResult_Ok, SHUResult_ErrNotFound if no function has the name, or SHUResult_ErrBadData if its signature is neither void() nor void(handle<ecs.panel>). Errors are reported.
SHUResult ECSIServices_CallBound(const char *name, ECSPanel focus);

/// @brief Calls the function that a preset names in open, with the path of a file. It reports a function that is missing or has another signature.
/// @param name Name of the function. Its signature must be void(string).
/// @param path The file's path.
/// @return SHUResult_Ok, SHUResult_ErrNotFound if no function has the name, or SHUResult_ErrBadData if its signature is another.
SHUResult ECSIServices_CallOpen(const char *name, const char *path);

/// @brief A function that ECSIServices_ForEachCore calls with the name of one of the core's functions.
typedef void (*ECSIServicesCoreFunction)(const char *name, void *data);

/// @brief Calls a function with the name of each function the core registered, in the order they were registered.
/// @param function The function to call.
/// @param data Passed to it.
void ECSIServices_ForEachCore(ECSIServicesCoreFunction function, void *data);

/// @brief Gets a function's one-line description.
/// @param name Name of the function.
/// @return The description, or NULL if no function has the name. Valid while the function is registered.
const char *ECSIServices_GetDescription(const char *name);

/// @brief Registers a Lua function of a plugin's service. C gets it as a function pointer: a libffi closure that converts the arguments, calls the Lua function in a protected call, and converts the result.
/// @param plugin The plugin that provides the function.
/// @param name Name of the function.
/// @param signature The function's signature.
/// @param description One line that says what the function does.
/// @return SHUResult_Ok with the Lua function on top of the stack popped, or an error of ECSService_RegisterFunction with the stack unchanged.
SHUWUR SHUResult ECSIServices_RegisterLua(ECSPlugin plugin, const char *name, const char *signature, const char *description);

/// @brief Pushes a Lua function that calls a registered function, for the Bindings module. A C function is called through libffi; a Lua function is pushed as it is.
/// @param plugin The plugin that asks. It may look up its own functions and those of the plugins its manifest depends on.
/// @param name Name of the function.
/// @param signature The signature the caller expects, or NULL to take any.
/// @return SHUResult_Ok with the function pushed, or the error of ECSService_GetFunction with nothing pushed.
SHUWUR SHUResult ECSIServices_PushFunction(ECSPlugin plugin, const char *name, const char *signature);

/// @brief Pushes a table of every function a plugin registered, by local name, for the Bindings module's require.
/// @param plugin The plugin that asks. It may ask for itself and for the plugins its manifest depends on.
/// @param provider The plugin whose functions are pushed.
/// @return SHUResult_Ok with the table pushed, or SHUResult_ErrPrivileges or SHUResult_ErrAllocation with nothing pushed.
SHUWUR SHUResult ECSIServices_PushPlugin(ECSPlugin plugin, ECSPlugin provider);

/// @brief Writes the definition files of a plugin's functions (DESIGN 10.10): NAME.lua for editors of Lua plugins, and NAME.h for plugins in C.
/// @param provider The plugin.
/// @param folder The folder to write into, ending with a separator.
/// @return SHUResult_Ok, SHUResult_ErrFile if a file cannot be written, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSIServices_WriteDefinitions(ECSPlugin provider, const char *folder);

#pragma endregion Declarations
