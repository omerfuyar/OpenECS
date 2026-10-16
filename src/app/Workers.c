#include "app/Workers.h"

#include "base/Lua.h"
#include "runtime/Events.h"
#include "runtime/Plugins.h"

#include "lua/lauxlib.h"
#include "lua/lua.h"

#include "SDL3/SDL.h"
#include "stb/stbSDL3.h"

#pragma region Source Only

/// @brief Name of the metatable of worker handles.
#define OPENECS_LUA_WORKER "ecs.worker"

/// @brief How many Lua instructions a worker runs between checks of whether it was stopped.
#define OPENECS_WORKER_CHECK_INTERVAL 1000

/// @brief A Lua script that runs on a thread of its own, in a Lua state of its own.
typedef struct ECSIWorker
{
    // set before the thread starts, and only read after
    ECSPlugin plugin;
    char *path;    // the script
    char *folder;  // the plugin's folder, where the script's require finds its modules
    char *name;    // the plugin's name and version, for the script's ecs.plugin
    char *version; //
    ECSValue *input;
    SDL_Thread *thread;

    SDL_AtomicInt stopping; // stop was asked: the script ends at its next receive, or within a few instructions

    // the lock guards the messages both ways and the end of the script
    SDL_Mutex *lock;
    SDL_Condition *arrived; // a message came to the worker, or it should stop
    ECSValue **inbox;       // stb_ds array: messages to the worker, oldest first
    ECSValue **outbox;      // stb_ds array: messages to the main thread, oldest first
    bool finished;          // the script ended, and the thread does nothing more but end
    bool succeeded;         // the script returned, rather than raised an error
    ECSValue *result;       // what the script returned, or nullptr
    char *error;            // the error it raised, or nullptr

    // main thread only: registry references in the core's state
    int message; // the function that gets each message, or LUA_NOREF
    int done;    // the function that gets the end, or LUA_NOREF
    int handle;  // the handle, so it lives while the worker does
} ECSIWorker;

static struct
{
    ECSIWorker **workers; // stb_ds array, main thread only
    SDL_AtomicInt posted; // a pump is queued on the main thread
} WORKERS = {0};

static void SDLCALL ECSIWorkers_Pump(void *data);

/// @brief Queues a pump on the main thread, unless one is queued already. Any thread.
static void ECSIWorkers_Post(void)
{
    if (SDL_CompareAndSwapAtomicInt(&WORKERS.posted, 0, 1) && ECS_RunOnMainThread(ECSIWorkers_Pump, nullptr))
    {
        SDL_SetAtomicInt(&WORKERS.posted, 0);
    }
}

/// @brief Destroys the values of a list of messages, and the list.
static void ECSIWorkers_FreeMessages(ECSValue ***messages)
{
    for (usz i = 0; i < arrlenu(*messages); i++)
    {
        ECSValue_Destroy(&(*messages)[i]);
    }

    arrfree(*messages);
}

#pragma region Worker Thread

/// @brief Gets the worker whose state runs the code.
static ECSIWorker *ECSIWorkers_Self(lua_State *state)
{
    return *(ECSIWorker **)lua_getextraspace(state);
}

/// @brief Ends the script with an error once the worker should stop, so a loop that never receives ends too.
static void ECSIWorkers_Hook(lua_State *state, lua_Debug *debug)
{
    (void)debug;

    if (SDL_GetAtomicInt(&ECSIWorkers_Self(state)->stopping))
    {
        luaL_error(state, "the worker was stopped");
    }
}

/// @brief ecs.worker.send(value): sends a copy of a value to the main thread.
static int ECSIWorkers_Send(lua_State *state)
{
    luaL_checkany(state, 1);
    ECSIWorker *worker = ECSIWorkers_Self(state);
    ECSValue *message = nullptr;
    SHUResult result = ECSValue_Create(&message);
    result = result ? result : ECSILua_GetData(state, 1, message);

    if (result)
    {
        ECSValue_Destroy(&message);
        return luaL_error(state, "the message cannot be copied (%s)", SHUResult_String(result));
    }

    SDL_LockMutex(worker->lock);
    arrput(worker->outbox, message);
    SDL_UnlockMutex(worker->lock);
    ECSIWorkers_Post();
    return 0;
}

/// @brief ecs.worker.receive(seconds): gives the oldest message from the main thread, waiting for one at most seconds, or without end if seconds is missing.
static int ECSIWorkers_Receive(lua_State *state)
{
    f64 seconds = (f64)luaL_optnumber(state, 1, -1.0);
    ECSIWorker *worker = ECSIWorkers_Self(state);
    u64 end = SDL_GetTicksNS() + (u64)(SDL_max(seconds, 0.0) * (f64)SDL_NS_PER_SECOND);
    ECSValue *message = nullptr;

    SDL_LockMutex(worker->lock);

    while (arrlenu(worker->inbox) == 0 && !SDL_GetAtomicInt(&worker->stopping))
    {
        u64 now = SDL_GetTicksNS();

        if (seconds < 0.0)
        {
            SDL_WaitCondition(worker->arrived, worker->lock);
        }
        else if (now >= end)
        {
            break;
        }
        else
        {
            // rounded up, so a short wait still waits
            SDL_WaitConditionTimeout(worker->arrived, worker->lock, (i32)SDL_min((end - now + SDL_NS_PER_MS - 1) / SDL_NS_PER_MS, (u64)SDL_MAX_SINT32));
        }
    }

    if (arrlenu(worker->inbox) > 0 && !SDL_GetAtomicInt(&worker->stopping))
    {
        message = worker->inbox[0];
        arrdel(worker->inbox, 0);
    }

    SDL_UnlockMutex(worker->lock);

    if (SDL_GetAtomicInt(&worker->stopping))
    {
        ECSValue_Destroy(&message);
        return luaL_error(state, "the worker was stopped");
    }

    ECSILua_PushData(state, message);
    ECSValue_Destroy(&message);
    return 1;
}

static int ECSIWorkers_Log(lua_State *state, ECSLogLevel level)
{
    ECS_Log(ECSIWorkers_Self(state)->plugin, level, "%s", luaL_checkstring(state, 1));
    return 0;
}

static int ECSIWorkers_LogDebug(lua_State *state)
{
    return ECSIWorkers_Log(state, ECSLogLevel_Debug);
}

static int ECSIWorkers_LogInfo(lua_State *state)
{
    return ECSIWorkers_Log(state, ECSLogLevel_Info);
}

static int ECSIWorkers_LogWarn(lua_State *state)
{
    return ECSIWorkers_Log(state, ECSLogLevel_Warning);
}

static int ECSIWorkers_LogError(lua_State *state)
{
    return ECSIWorkers_Log(state, ECSLogLevel_Error);
}

static const luaL_Reg OPENECS_WORKERS_LOG[] = {
    {"debug", ECSIWorkers_LogDebug},
    {"info", ECSIWorkers_LogInfo},
    {"warn", ECSIWorkers_LogWarn},
    {"error", ECSIWorkers_LogError},
    {nullptr, nullptr},
};

static const luaL_Reg OPENECS_WORKERS_OWN[] = {
    {"send", ECSIWorkers_Send},
    {"receive", ECSIWorkers_Receive},
    {nullptr, nullptr},
};

/// @brief Runs a worker's script in a protected call, so every error, running out of memory too, is an error of the script.
/// First it prepares the state: its ecs module, which holds log, plugin and the worker's own functions, and a require that finds the plugin's modules.
/// Then it runs the script with the input, and keeps what the script returns.
static int ECSIWorkers_Script(lua_State *state)
{
    ECSIWorker *worker = ECSIWorkers_Self(state);

    luaL_getsubtable(state, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
    lua_newtable(state);
    luaL_newlib(state, OPENECS_WORKERS_LOG);
    lua_setfield(state, -2, "log");
    luaL_newlib(state, OPENECS_WORKERS_OWN);
    lua_setfield(state, -2, "worker");
    lua_newtable(state);
    lua_pushstring(state, worker->name);
    lua_setfield(state, -2, "name");
    lua_pushstring(state, worker->version);
    lua_setfield(state, -2, "version");
    lua_pushstring(state, worker->folder);
    lua_setfield(state, -2, "folder");
    lua_setfield(state, -2, "plugin");
    lua_setfield(state, -2, "ecs");
    lua_pop(state, 1);

    // the plugin's folder comes first, as for the plugin's own require
    lua_getglobal(state, "package");
    lua_getfield(state, -1, "path");
    lua_pushfstring(state, "%s?.lua;%s?/init.lua;%s", worker->folder, worker->folder, lua_tostring(state, -1));
    lua_setfield(state, -3, "path");
    lua_pop(state, 2);

    if (luaL_loadfilex(state, worker->path, "t") != LUA_OK)
    {
        return lua_error(state);
    }

    // the input is the script's ..., as a file's arguments are
    ECSILua_PushData(state, worker->input);
    lua_call(state, 1, 1);

    if (ECSValue_Create(&worker->result) || ECSILua_GetData(state, -1, worker->result))
    {
        return luaL_error(state, "what the worker returned cannot be copied");
    }

    return 0;
}

static int SDLCALL ECSIWorkers_Run(void *data)
{
    ECSIWorker *worker = data;
    lua_State *state = ECSILua_NewState();

    if (state == nullptr)
    {
        worker->error = SDL_strdup("out of memory");
    }
    else
    {
        *(ECSIWorker **)lua_getextraspace(state) = worker;
        lua_sethook(state, ECSIWorkers_Hook, LUA_MASKCOUNT, OPENECS_WORKER_CHECK_INTERVAL);
        lua_pushcfunction(state, ECSIWorkers_Script);
        worker->succeeded = !ECSILua_CallIn(state, 0, 0);

        if (!worker->succeeded)
        {
            const char *message = lua_tostring(state, -1);
            worker->error = SDL_strdup(message == nullptr ? "the worker cannot run" : message);
            ECSValue_Destroy(&worker->result);
        }

        lua_close(state);
    }

    SDL_LockMutex(worker->lock);
    worker->finished = true;
    SDL_UnlockMutex(worker->lock);
    ECSIWorkers_Post();
    return 0;
}

#pragma endregion Worker Thread

#pragma region Main Thread

/// @brief Asks a worker to stop: its receive ends the script, and so does its next check.
static void ECSIWorkers_Stop(ECSIWorker *worker)
{
    SDL_SetAtomicInt(&worker->stopping, 1);
    SDL_LockMutex(worker->lock);
    SDL_BroadcastCondition(worker->arrived);
    SDL_UnlockMutex(worker->lock);
}

/// @brief Frees a worker whose thread ended, or never started, and clears its handle.
static void ECSIWorkers_Free(ECSIWorker *worker)
{
    lua_State *state = ECSILua_GetState();

    if (worker->handle != LUA_NOREF)
    {
        lua_rawgeti(state, LUA_REGISTRYINDEX, worker->handle);
        *(ECSIWorker **)lua_touserdata(state, -1) = nullptr;
        lua_pop(state, 1);
    }

    luaL_unref(state, LUA_REGISTRYINDEX, worker->message);
    luaL_unref(state, LUA_REGISTRYINDEX, worker->done);
    luaL_unref(state, LUA_REGISTRYINDEX, worker->handle);

    ECSIWorkers_FreeMessages(&worker->inbox);
    ECSIWorkers_FreeMessages(&worker->outbox);
    ECSValue_Destroy(&worker->input);
    ECSValue_Destroy(&worker->result);
    SDL_free(worker->error);
    SDL_free(worker->path);
    SDL_free(worker->folder);
    SDL_free(worker->name);
    SDL_free(worker->version);
    SDL_DestroyCondition(worker->arrived);
    SDL_DestroyMutex(worker->lock);
    SDL_free(worker);
}

/// @brief Calls one of a worker's functions with the values on top of the stack, and reports an error it raises.
static void ECSIWorkers_Call(ECSIWorker *worker, int function, int argumentCount)
{
    lua_State *state = ECSILua_GetState();
    lua_rawgeti(state, LUA_REGISTRYINDEX, function);
    lua_insert(state, -argumentCount - 1);

    if (ECSILua_Call(argumentCount, 0))
    {
        ECSIPlugin_ReportError(worker->plugin, lua_tostring(state, -1));
        lua_pop(state, 1);
    }
}

/// @brief Delivers what the workers sent, and the ends of their scripts, then frees the workers that ended. A worker that was stopped gets no calls.
static void SDLCALL ECSIWorkers_Pump(void *data)
{
    (void)data;

    // a message that comes while the pump runs queues another pump
    SDL_SetAtomicInt(&WORKERS.posted, 0);

    // the functions may start workers, which adds to the list, or stop them; only the pump removes them
    for (usz i = 0; i < arrlenu(WORKERS.workers);)
    {
        ECSIWorker *worker = WORKERS.workers[i];

        SDL_LockMutex(worker->lock);
        ECSValue **messages = worker->outbox;
        worker->outbox = nullptr;
        bool finished = worker->finished;
        SDL_UnlockMutex(worker->lock);

        for (usz m = 0; m < arrlenu(messages) && worker->message != LUA_NOREF; m++)
        {
            if (!SDL_GetAtomicInt(&worker->stopping))
            {
                ECSILua_PushValue(messages[m]);
                ECSIWorkers_Call(worker, worker->message, 1);
            }
        }

        ECSIWorkers_FreeMessages(&messages);

        if (!finished)
        {
            i++;
            continue;
        }

        SDL_WaitThread(worker->thread, nullptr);
        arrdel(WORKERS.workers, i);

        if (!SDL_GetAtomicInt(&worker->stopping))
        {
            if (!worker->succeeded)
            {
                ECSIPlugin_ReportError(worker->plugin, worker->error == nullptr ? "out of memory" : worker->error);
            }

            if (worker->done != LUA_NOREF)
            {
                lua_State *state = ECSILua_GetState();
                lua_pushboolean(state, worker->succeeded);

                if (worker->succeeded)
                {
                    ECSILua_PushValue(worker->result);
                }
                else
                {
                    lua_pushstring(state, worker->error == nullptr ? "out of memory" : worker->error);
                }

                ECSIWorkers_Call(worker, worker->done, 2);
            }
        }

        ECSIWorkers_Free(worker);
    }
}

/// @brief Reads a worker handle. Gives nullptr once its worker is gone.
static ECSIWorker *ECSIWorkers_Check(lua_State *state)
{
    return *(ECSIWorker **)luaL_checkudata(state, 1, OPENECS_LUA_WORKER);
}

/// @brief Raises an error if a field of a table is neither a function nor nil.
static void ECSIWorkers_CheckFunction(lua_State *state, int index, const char *name)
{
    int type = lua_getfield(state, index, name);
    luaL_argexpected(state, type == LUA_TFUNCTION || type == LUA_TNIL, index, lua_pushfstring(state, "%s as a function or nil", name));
    lua_pop(state, 1);
}

/// @brief Takes a registry reference to a function field of a table, or gives LUA_NOREF if it is nil.
static int ECSIWorkers_RefFunction(lua_State *state, int index, const char *name)
{
    if (lua_getfield(state, index, name) == LUA_TFUNCTION)
    {
        return luaL_ref(state, LUA_REGISTRYINDEX);
    }

    lua_pop(state, 1);
    return LUA_NOREF;
}

/// @brief ecs.worker.start(desc): starts a worker of a plugin, and gives its handle, or nil and a message.
static int ECSIWorkers_Start(lua_State *state)
{
    ECSPlugin plugin = lua_touserdata(state, lua_upvalueindex(1));
    luaL_checktype(state, 1, LUA_TTABLE);
    ECSIWorkers_CheckFunction(state, 1, "message");
    ECSIWorkers_CheckFunction(state, 1, "done");
    lua_getfield(state, 1, "file");
    const char *file = luaL_checkstring(state, -1);
    const char *path = lua_pushfstring(state, "%s%s", ECSPlugin_GetFolder(plugin), file);

    if (!SDL_GetPathInfo(path, nullptr))
    {
        lua_pushnil(state);
        lua_pushfstring(state, "no file '%s' in the plugin's folder", file);
        return 2;
    }

    // the handle comes first, since making it may raise an error, which would leave the worker unfreed
    ECSIWorker **handle = lua_newuserdatauv(state, sizeof(ECSIWorker *), 0);
    *handle = nullptr;
    luaL_setmetatable(state, OPENECS_LUA_WORKER);
    ECSIWorker *worker = SDL_calloc(1, sizeof(ECSIWorker));

    if (worker == nullptr)
    {
        return luaL_error(state, "out of memory");
    }

    worker->plugin = plugin;
    worker->message = LUA_NOREF;
    worker->done = LUA_NOREF;
    worker->handle = LUA_NOREF;
    worker->path = SDL_strdup(path);
    worker->folder = SDL_strdup(ECSPlugin_GetFolder(plugin));
    worker->name = SDL_strdup(ECSIPlugin_GetName(plugin));
    worker->version = SDL_strdup(ECSIPlugin_GetVersion(plugin));
    worker->lock = SDL_CreateMutex();
    worker->arrived = SDL_CreateCondition();

    lua_getfield(state, 1, "input");
    SHUResult result = ECSValue_Create(&worker->input);
    result = result ? result : ECSILua_GetData(state, -1, worker->input);
    lua_pop(state, 1);

    if (!result && (worker->path == nullptr || worker->folder == nullptr || worker->name == nullptr || worker->version == nullptr || worker->lock == nullptr || worker->arrived == nullptr))
    {
        result = SHUResult_ErrAllocation;
    }

    if (result)
    {
        ECSIWorkers_Free(worker);
        return luaL_error(state, "the worker cannot start (%s)", SHUResult_String(result));
    }

    worker->message = ECSIWorkers_RefFunction(state, 1, "message");
    worker->done = ECSIWorkers_RefFunction(state, 1, "done");

    *handle = worker;
    lua_pushvalue(state, -1);
    worker->handle = luaL_ref(state, LUA_REGISTRYINDEX);

    worker->thread = SDL_CreateThread(ECSIWorkers_Run, "OpenECS Lua worker", worker);

    if (worker->thread == nullptr)
    {
        ECSIWorkers_Free(worker);
        lua_pushnil(state);
        lua_pushfstring(state, "the worker's thread cannot start (%s)", SDL_GetError());
        return 2;
    }

    arrput(WORKERS.workers, worker);
    return 1;
}

/// @brief worker:send(value): sends a copy of a value to a worker, which its receive gives. A worker that ended or was stopped gets nothing.
static int ECSIWorkers_SendTo(lua_State *state)
{
    ECSIWorker *worker = ECSIWorkers_Check(state);
    luaL_checkany(state, 2);

    if (worker == nullptr || SDL_GetAtomicInt(&worker->stopping))
    {
        return 0;
    }

    ECSValue *message = nullptr;
    SHUResult result = ECSValue_Create(&message);
    result = result ? result : ECSILua_GetData(state, 2, message);

    if (result)
    {
        ECSValue_Destroy(&message);
        return luaL_error(state, "the message cannot be copied (%s)", SHUResult_String(result));
    }

    SDL_LockMutex(worker->lock);
    arrput(worker->inbox, message);
    SDL_SignalCondition(worker->arrived);
    SDL_UnlockMutex(worker->lock);
    return 0;
}

/// @brief worker:stop(): stops a worker. Its functions are not called again.
static int ECSIWorkers_StopHandle(lua_State *state)
{
    ECSIWorker *worker = ECSIWorkers_Check(state);

    if (worker != nullptr)
    {
        ECSIWorkers_Stop(worker);
    }

    return 0;
}

/// @brief worker.running: whether the worker's script still runs: it has not ended, and was not stopped.
static int ECSIWorkers_Index(lua_State *state)
{
    ECSIWorker *worker = ECSIWorkers_Check(state);
    const char *key = luaL_checkstring(state, 2);

    if (SDL_strcmp(key, "running") == 0)
    {
        bool running = false;

        if (worker != nullptr && !SDL_GetAtomicInt(&worker->stopping))
        {
            SDL_LockMutex(worker->lock);
            running = !worker->finished;
            SDL_UnlockMutex(worker->lock);
        }

        lua_pushboolean(state, running);
        return 1;
    }

    lua_getmetatable(state, 1);
    lua_getfield(state, -1, "methods");
    lua_getfield(state, -1, key);
    return 1;
}

static const luaL_Reg OPENECS_WORKERS_METHODS[] = {
    {"send", ECSIWorkers_SendTo},
    {"stop", ECSIWorkers_StopHandle},
    {nullptr, nullptr},
};

#pragma endregion Main Thread

#pragma endregion Source Only

void ECSIWorkers_Initialize(void)
{
    lua_State *state = ECSILua_GetState();

    luaL_newmetatable(state, OPENECS_LUA_WORKER);
    luaL_newlib(state, OPENECS_WORKERS_METHODS);
    lua_setfield(state, -2, "methods");
    lua_pushcfunction(state, ECSIWorkers_Index);
    lua_setfield(state, -2, "__index");
    lua_pop(state, 1);
}

void ECSIWorkers_Terminate(void)
{
    for (usz i = 0; i < arrlenu(WORKERS.workers); i++)
    {
        ECSIWorkers_Stop(WORKERS.workers[i]);
    }

    for (usz i = 0; i < arrlenu(WORKERS.workers); i++)
    {
        SDL_WaitThread(WORKERS.workers[i]->thread, nullptr);
        ECSIWorkers_Free(WORKERS.workers[i]);
    }

    arrfree(WORKERS.workers);
    SDL_zero(WORKERS);
}

void ECSIWorkers_PushTable(lua_State *state, ECSPlugin plugin)
{
    SDL_assert(state != nullptr);
    SDL_assert(plugin != nullptr);

    lua_newtable(state);
    lua_pushlightuserdata(state, plugin);
    lua_pushcclosure(state, ECSIWorkers_Start, 1);
    lua_setfield(state, -2, "start");
}
