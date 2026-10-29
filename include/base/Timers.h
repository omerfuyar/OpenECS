#pragma once

// Timers: functions that run on the main thread after a time, once or again and again.

#include "OpenECS.h"

#pragma region Declarations

/// @brief Starts a timer.
/// @param retTimer The new timer.
/// @param seconds Time until the first call, and between repeated calls. Must be positive for a repeating timer.
/// @param repeat true to call the function until the timer is stopped.
/// @param function Function to call.
/// @param release Function called with the data when the timer is freed, after it stops, or nullptr.
/// @param data Passed to the functions.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSITimers_Start(ECSTimer *retTimer, f64 seconds, bool repeat, ECSTimerFunction function, ECSTimerFunction release, void *data);

/// @brief Stops a timer. Its handle becomes invalid; its release runs once no timer runs.
/// @param timer The timer.
void ECSITimers_Stop(ECSTimer timer);

/// @brief Gets the time until the next timer is due.
/// @return The time in milliseconds, rounded up, or -1 if no timer runs.
i32 ECSITimers_GetWait(void);

/// @brief Runs the timers that are due. A repeating timer runs at most once per call.
void ECSITimers_Run(void);

/// @brief Stops and frees every timer.
void ECSITimers_Terminate(void);

#pragma endregion Declarations
