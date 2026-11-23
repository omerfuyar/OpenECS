#pragma once

// Timers: functions that run on the main thread after a time, once or again and again.

#include "OpenECS.h"

#pragma region Declarations

/// @brief Runs a timer's function for its owner, such as inside the owner's callback.
typedef void (*ECSITimerRunFunction)(void *owner, ECSTimerFunction function, void *data);

/// @brief Sets the function that runs the timers that have an owner.
void ECSITimers_SetRunner(ECSITimerRunFunction run);

/// @brief Starts a timer. Timers live in a table of slots, and a handle names a slot and its generation, so a handle of a timer that stopped
/// or ended names nothing.
/// @param retTimer The new timer.
/// @param seconds Time until the first call, and between repeated calls. Must be positive for a repeating timer.
/// @param repeat true to call the function until the timer is stopped.
/// @param function Function to call.
/// @param release Function called with the data when the timer is freed, after it stops, or nullptr.
/// @param data Passed to the functions.
/// @param owner Given to the runner with the function, or nullptr to call the function itself.
/// @return SHUResult_Ok.
SHUWUR SHUResult ECSITimers_Start(ECSTimer *retTimer, f64 seconds, bool repeat, ECSTimerFunction function, ECSTimerFunction release, void *data, void *owner);

/// @brief Stops a timer; its release runs once no timer runs. A handle of a timer that stopped or ended does nothing.
/// @param timer The timer.
void ECSITimers_Stop(ECSTimer timer);

/// @brief Stops every timer of an owner.
void ECSITimers_StopOwnedBy(void *owner);

/// @brief Gets the time until the next timer is due.
/// @return The time in milliseconds, rounded up, or -1 if no timer runs.
i32 ECSITimers_GetWait(void);

/// @brief Runs the timers that are due. A repeating timer runs at most once per call.
void ECSITimers_Run(void);

/// @brief Stops and frees every timer.
void ECSITimers_Terminate(void);

#pragma endregion Declarations
