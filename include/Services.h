#pragma once

// Services: the function registry, signatures, and calls between C and Lua. The only module that calls libffi.

#include "Events.h"

#pragma region Declarations

/// @brief Prepares the table that gives each object one Lua handle, and registers the core's handle type ecs.panel.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSI_ServicesInitialize(void);

/// @brief Destroys the objects of every handle Lua still holds, while their plugins are loaded, and frees every registered function and handle type.
void ECSI_ServicesTerminate(void);

/// @brief Pushes the Lua handle of an object: the one Lua already has, or a new one.
/// @param type Name of the object's handle type.
/// @param object The object, or NULL for nil.
void ECSI_ServicesPushHandle(const char *type, void *object);

/// @brief Reads a Lua handle of a type. Raises a Lua error if the value is not a handle of that type, or its object is gone.
/// @param index Index of the handle on the Lua stack.
/// @param type Name of the handle type.
/// @return The object.
void *ECSI_ServicesCheckHandle(int index, const char *type);

/// @brief Makes an object's Lua handle invalid, without destroying the object, because the object is gone.
/// @param object The object.
void ECSI_ServicesForgetHandle(void *object);

/// @brief Pushes the metatable of a handle type, so the Bindings module can give its handles methods.
/// @param type Name of the handle type.
void ECSI_ServicesPushHandleMetatable(const char *type);

/// @brief Removes every function a plugin registered.
/// @param plugin The plugin.
void ECSI_ServicesRemovePlugin(ECSPlugin plugin);

/// @brief Registers a Lua function of a plugin's service. C gets it as a function pointer: a libffi closure that converts the arguments, calls the Lua function in a protected call, and converts the result.
/// @param plugin The plugin that provides the function.
/// @param name Name of the function.
/// @param signature The function's signature.
/// @param description One line that says what the function does.
/// @return SHUResult_Ok with the Lua function on top of the stack popped, or an error of ECSService_RegisterFunction with the stack unchanged.
SHUWUR SHUResult ECSI_ServicesRegisterLua(ECSPlugin plugin, const char *name, const char *signature, const char *description);

/// @brief Pushes a Lua function that calls a registered function, for the Bindings module. A C function is called through libffi; a Lua function is pushed as it is.
/// @param plugin The plugin that asks. It may look up its own functions and those of the plugins its manifest depends on.
/// @param name Name of the function.
/// @param signature The signature the caller expects, or NULL to take any.
/// @return SHUResult_Ok with the function pushed, or the error of ECSService_GetFunction with nothing pushed.
SHUWUR SHUResult ECSI_ServicesPushFunction(ECSPlugin plugin, const char *name, const char *signature);

#pragma endregion Declarations
