#pragma once

// Events: named events, the event queue, and work on worker threads.

#include "runtime/Plugins.h"

#pragma region Declarations

/// @brief Function that delivers a queued event to its target.
typedef void (*ECSIEventDeliverFunction)(void *target, const ECSPanelEvent *event);

/// @brief Prepares the lock and condition of the worker threads, which start when background work first comes, and declares the core's named events.
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

/// @brief Queues an event. ECSIEvents_Deliver delivers it, after the current callback returns.
/// @param deliver Function that delivers the event.
/// @param target Passed to the function.
/// @param event The event. The queue keeps a copy.
void ECSIEvents_Post(ECSIEventDeliverFunction deliver, void *target, const ECSPanelEvent *event);

/// @brief Delivers the queued events in order, and the events queued while they are delivered, until the queue is empty.
void ECSIEvents_Deliver(void);

/// @brief Gets how long the main loop may wait for input, for SDL_WaitEventTimeout.
/// @return 0 if events are queued; otherwise the time until the next timer is due, in milliseconds rounded up, or -1 if no timer runs (base/Timers).
i32 ECSIEvents_GetWait(void);

/// @brief Waits for running background work, drops waiting work and drops the queued events. Tasks that reach the main thread after this are dropped.
void ECSIEvents_Terminate(void);

#pragma endregion Declarations
