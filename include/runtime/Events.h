#pragma once

// Events: the event queue and timers.

#include "runtime/Plugins.h"

#pragma region Declarations

/// @brief Function that delivers a queued event to its target.
typedef void (*ECSIEventDeliverFunction)(void *target, const ECSPanelEvent *event);

/// @brief Prepares the lock and condition of the worker threads, which start when background work first comes, and declares the core's named events.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSIEvents_Initialize(void);

/// @brief Emits one of the core's named events, whose names start with "ecs.".
/// @param name Name of the event.
/// @param value What the event carries, or NULL. The core copies it.
void ECSIEvents_EmitCore(const char *name, const ECSValue *value);

/// @brief Subscribes to a named event like ECSEvent_Subscribe, with a function that releases the data when the subscription is freed. The Bindings module uses it for Lua.
/// @param release Function called with the data when the subscription is freed, or NULL.
SHUWUR SHUResult ECSIEvents_Subscribe(ECSPlugin plugin, const char *name, ECSSubscription *retSubscription, ECSEventFunction function, ECSTimerFunction release, void *data);

/// @brief Removes a plugin's named events and subscriptions, and the subscriptions to its events.
/// @param plugin The plugin.
void ECSIEvents_RemovePlugin(ECSPlugin plugin);

/// @brief Queues an event. ECSIEvents_Deliver delivers it, after the current callback returns.
/// @param deliver Function that delivers the event.
/// @param target Passed to the function.
/// @param event The event. The queue keeps a copy.
void ECSIEvents_Post(ECSIEventDeliverFunction deliver, void *target, const ECSPanelEvent *event);

/// @brief Delivers the queued events in order, and the events queued while they are delivered, until the queue is empty.
void ECSIEvents_Deliver(void);

/// @brief Starts a timer.
/// @param plugin The plugin that owns the timer.
/// @param owner What the timer belongs to, such as a panel, or NULL. ECSIEvents_StopTimersOf stops all timers of an owner.
/// @param retTimer The new timer.
/// @param seconds Time until the first call, and between repeated calls. Must be positive for a repeating timer.
/// @param repeat true to call the function until the timer is stopped.
/// @param function Function to call.
/// @param release Function called with the data when the timer is freed, after it stops, or NULL.
/// @param data Passed to the functions.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSIEvents_StartTimer(ECSPlugin plugin, const void *owner, ECSTimer *retTimer, f64 seconds, bool repeat, ECSTimerFunction function, ECSTimerFunction release, void *data);

/// @brief Stops every timer of an owner. Their handles become invalid.
/// @param owner The owner given to ECSIEvents_StartTimer.
void ECSIEvents_StopTimersOf(const void *owner);

/// @brief Stops every timer of a plugin. Their handles become invalid.
/// @param plugin The plugin.
void ECSIEvents_StopTimersOfPlugin(ECSPlugin plugin);

/// @brief Gets how long the main loop may wait for input, for SDL_WaitEventTimeout.
/// @return 0 if events are queued; otherwise the time until the next timer is due, in milliseconds rounded up, or -1 if no timer runs.
i32 ECSIEvents_GetWait(void);

/// @brief Runs the timers that are due. A repeating timer runs at most once per call.
void ECSIEvents_RunTimers(void);

/// @brief Waits for running background work, drops waiting work, stops every timer and drops the queued events. Tasks that reach the main thread after this are dropped.
void ECSIEvents_Terminate(void);

#pragma endregion Declarations
