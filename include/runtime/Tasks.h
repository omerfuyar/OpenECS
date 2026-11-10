#pragma once

// Tasks: work on worker threads, and functions that other threads send to the main thread.

#include "runtime/Plugins.h"

#pragma region Declarations

/// @brief Prepares the lock and condition of the worker threads, which start when background work first comes.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSITasks_Initialize(void);

/// @brief Waits for running background work and drops waiting work, before the plugins are destroyed. Tasks that reach the main thread after this are dropped; those the main thread queued for itself are freed with the event queue. Background work that is asked for after it fails.
void ECSITasks_Stop(void);

/// @brief Stops the work as ECSITasks_Stop does, if it still runs, and frees the lock and conditions.
void ECSITasks_Terminate(void);

/// @brief Drops a plugin's waiting background work and waits for its running work, for a plugin that is disabled. Its done functions are dropped when they reach the main thread.
void ECSITasks_ForgetPlugin(ECSPlugin plugin);

#pragma endregion Declarations
