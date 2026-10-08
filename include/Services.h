#pragma once

// Services: the function registry, signatures, and calls between C and Lua. The only module that calls libffi.

#include "Events.h"

#pragma region Declarations

/// @brief Frees every registered function.
void ECSI_ServicesTerminate(void);

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
