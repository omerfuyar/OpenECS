#pragma once

// Sanitizers: settings of the address and leak sanitizers that Debug builds use. In other builds its functions do nothing.

#pragma region Declarations

/// @brief Checks for leaks now, and ends the program with an error if there are any. The check at exit is skipped then.
void ECSISanitizers_CheckLeaks(void);

#pragma endregion Declarations
