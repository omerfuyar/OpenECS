#pragma once

// Events: the event queue and timers.

#include "Plugins.h"

#pragma region Declarations

/// @brief Function that delivers a queued event to its target.
typedef void (*ECSI_EventDeliverFunction)(void *target, const ECSEvent *event);

/// @brief Queues an event. ECSI_EventsDeliver delivers it, after the current callback returns.
/// @param deliver Function that delivers the event.
/// @param target Passed to the function.
/// @param event The event. The queue keeps a copy.
void ECSI_EventsPost(ECSI_EventDeliverFunction deliver, void *target, const ECSEvent *event);

/// @brief Delivers the queued events in order, and the events queued while they are delivered, until the queue is empty.
void ECSI_EventsDeliver(void);

/// @brief Starts a timer.
/// @param plugin The plugin that owns the timer.
/// @param owner What the timer belongs to, such as a panel, or NULL. ECSI_EventsStopTimersOf stops all timers of an owner.
/// @param retTimer The new timer.
/// @param seconds Time until the first call, and between repeated calls. Must be positive for a repeating timer.
/// @param repeat true to call the function until the timer is stopped.
/// @param function Function to call.
/// @param data Passed to the function.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSI_EventsStartTimer(ECSPlugin plugin, const void *owner, ECSTimer *retTimer, f64 seconds, bool repeat, ECSTimerFunction function, void *data);

/// @brief Stops every timer of an owner. Their handles become invalid.
/// @param owner The owner given to ECSI_EventsStartTimer.
void ECSI_EventsStopTimersOf(const void *owner);

/// @brief Gets the time until the next timer is due, for SDL_WaitEventTimeout.
/// @return Milliseconds, rounded up, or -1 if no timer runs.
i32 ECSI_EventsGetTimerWait(void);

/// @brief Runs the timers that are due. A repeating timer runs at most once per call.
void ECSI_EventsRunTimers(void);

/// @brief Stops every timer and drops the queued events.
void ECSI_EventsTerminate(void);

#pragma endregion Declarations
