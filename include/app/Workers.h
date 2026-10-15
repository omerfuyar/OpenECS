#pragma once

// Workers: Lua code that runs on threads of its own, each in a Lua state of its own, and the messages that go between them and the main thread (DESIGN 3.4).

#include "app/Session.h"

#pragma region Declarations

/// @brief The Lua state, as lua.h declares it.
typedef struct lua_State lua_State;

/// @brief Prepares the type of worker handles. Call it after Lua starts.
void ECSIWorkers_Initialize(void);

/// @brief Stops every worker and waits for its thread, then frees it, without calling its functions. Call it while Lua still runs.
void ECSIWorkers_Terminate(void);

/// @brief Pushes the table worker of a plugin's ecs table, whose start function starts workers for the plugin.
/// @param state The core's Lua state.
/// @param plugin The plugin.
void ECSIWorkers_PushTable(lua_State *state, ECSPlugin plugin);

#pragma endregion Declarations
