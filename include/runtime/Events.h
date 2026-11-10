#pragma once

// Events: named events, the event queue, and the plugins' timers.

#include "runtime/Plugins.h"

#pragma region Declarations

/// @brief Function that delivers a queued event to its target.
typedef void (*ECSIEventDeliverFunction)(void *target, const ECSPanelEvent *event);

/// @brief Declares the core's named events.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSIEvents_Initialize(void);

/// @brief Emits one of the core's named events, whose names start with "ecs.".
/// @param name Name of the event.
/// @param value What the event carries, or nullptr. The core copies it.
void ECSIEvents_EmitCore(const char *name, const ECSValue *value);

/// @brief Subscribes to a named event like ECSEvent_Subscribe, with a function that releases the data when the subscription is freed.
/// @param release Function called with the data when the subscription is freed, or nullptr.
SHUWUR SHUResult ECSIEvents_Subscribe(ECSPlugin plugin, ECSSubscription *retSubscription, const char *name, ECSEventFunction function, ECSTimerFunction release, void *data);

/// @brief Removes a plugin's named events and subscriptions, and the subscriptions to its events.
/// @param plugin The plugin.
void ECSIEvents_RemovePlugin(ECSPlugin plugin);

/// @brief Tells the named events that wait to be delivered that an object of a handle type is freed, so a value that names it gives no object.
void ECSIEvents_ForgetObject(const void *object);

/// @brief Queues an event. ECSIEvents_Deliver delivers it, after the current callback returns.
/// @param deliver Function that delivers the event.
/// @param release Function that frees the target if the event is never delivered, as at exit, or nullptr.
/// @param target Passed to the functions.
/// @param event The event. The queue keeps a copy.
void ECSIEvents_Post(ECSIEventDeliverFunction deliver, ECSTimerFunction release, void *target, const ECSPanelEvent *event);

/// @brief Delivers the queued events in order, and the events queued while they are delivered, until the queue is empty. A named event emitted meanwhile
/// waits for the next call, and a subscription made meanwhile hears the next emission.
void ECSIEvents_Deliver(void);

/// @brief Starts a plugin's timer on base/Timers, which stops when the plugin's timers stop.
/// @param plugin The plugin that owns the timer.
/// @param retTimer The new timer.
/// @param seconds Time until the first call, and between repeated calls. Must be positive for a repeating timer.
/// @param repeat true to call the function until the timer is stopped.
/// @param function Function to call.
/// @param release Function called with the data when the timer is freed, after it stops, or nullptr.
/// @param data Passed to the functions.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSIEvents_StartTimer(ECSPlugin plugin, ECSTimer *retTimer, f64 seconds, bool repeat, ECSTimerFunction function, ECSTimerFunction release, void *data);

/// @brief Stops every timer of a plugin. Their handles become invalid.
/// @param plugin The plugin.
void ECSIEvents_StopTimersOfPlugin(ECSPlugin plugin);

/// @brief Gets how long the main loop may wait for input, for SDL_WaitEventTimeout.
/// @return 0 if events are queued; otherwise the time until the next timer is due, in milliseconds rounded up, or -1 if no timer runs (base/Timers).
i32 ECSIEvents_GetWait(void);

/// @brief Frees the queued events, which are never delivered, the subscriptions and the named events.
void ECSIEvents_Terminate(void);

#pragma endregion Declarations
