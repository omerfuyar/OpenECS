#pragma once

// Sanitizers: settings of the address and leak sanitizers that Debug builds use. In other builds its functions do nothing.

#pragma region Declarations

/// @brief Keeps every library loaded so far loaded until the program exits, so a leak inside one can be matched by its name. Graphics drivers are unloaded when their device is destroyed, before the leak check.
void ECSI_SanitizersKeepLibraries(void);

/// @brief Checks for leaks now, and ends the program with an error if there are any. The check at exit is skipped then.
void ECSI_SanitizersCheckLeaks(void);

#pragma endregion Declarations
