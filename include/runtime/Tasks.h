#pragma once

// Tasks: work on worker threads, and functions that other threads send to the main thread.

#include "runtime/Plugins.h"

#pragma region Declarations

/// @brief Function that frees what a thread kept, as a pool thread ends.
typedef void (*ECSITasksEndThreadFunction)(void);

/// @brief Prepares the lock and condition of the worker threads, which start when background work first comes.
/// @param EndThread Frees what each pool thread kept, as it ends; it frees what later modules keep, such as Services.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSITasks_Initialize(ECSITasksEndThreadFunction EndThread);

/// @brief Waits for running background work and drops waiting work, before the plugins are destroyed, then runs the functions that wait for the main thread. Tasks that reach the main thread after this are dropped; those the main thread queued for itself are freed with the event queue. Background work that is asked for after it fails.
void ECSITasks_Stop(void);

/// @brief Stops the work as ECSITasks_Stop does, if it still runs, and frees the lock and conditions.
void ECSITasks_Terminate(void);

/// @brief Drops a plugin's waiting background work and waits for its running work, for a plugin that is disabled, then runs the done functions of
/// its work that finished, which it takes out of the main thread's queue. Main thread only, before the plugin's Destroy.
void ECSITasks_ForgetPlugin(ECSPlugin plugin);

/// @brief Runs the functions that other threads sent to the main thread, at one step of the loop; the functions that come meanwhile wait for the
/// next pass. Dialogs and disabling plugins never run them (DESIGN 3.3). Main thread only.
void ECSITasks_RunMain(void);

/// @brief Gives how long the loop may wait: 0 while functions wait for the main thread, as a dialog may have taken their wake event, else -1.
i32 ECSITasks_GetWait(void);

/// @brief Sends one of the core's functions to the main thread's queue, from any thread; no plugin owns it, so it runs even after a plugin is disabled.
void ECSITasks_RunOnMainThread(ECSTaskFunction function, void *data);

#pragma endregion Declarations
