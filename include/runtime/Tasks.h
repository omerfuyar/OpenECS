#pragma once

// Tasks: work on worker threads, and functions that other threads send to the main thread.

#include "runtime/Plugins.h"

#pragma region Declarations

/// @brief Prepares the lock and condition of the worker threads, which start when background work first comes.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSITasks_Initialize(void);

/// @brief Waits for running background work and drops waiting work. Tasks that reach the main thread after this are dropped; those the main thread queued for itself are freed with the event queue.
void ECSITasks_Terminate(void);

#pragma endregion Declarations
