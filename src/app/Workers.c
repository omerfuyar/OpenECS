#include "app/Workers.h"

#include "base/Lua.h"
#include "runtime/Tasks.h"
#include "runtime/Plugins.h"
#include "runtime/Services.h"

#include "lua/compat-5.3.h"

#include "SDL3/SDL.h"
#include "stb/stbSDL3.h"

#pragma region Source Only

/// @brief Name of the metatable of worker handles.
#define OPENECS_LUA_WORKER "ecs.worker"

/// @brief The registry key of a worker's state under which its worker is: the address of this constant, as each state has its own registry.
static const char OPENECS_WORKER_KEY = 0;

/// @brief A value that goes between a worker's thread and the main thread: the bytes that LuaJIT's string.buffer encodes it to (DESIGN 3.4).
typedef struct ECSIMessage
{
    usz size;
    char bytes[];
} ECSIMessage;

/// @brief A Lua script that runs on a thread of its own, in a Lua state of its own.
typedef struct ECSIWorker
{
    // set before the thread starts, and only read after
    ECSPlugin plugin;
    char *path;    // the script
    char *folder;  // the plugin's folder, where the script's require finds its modules
    char *name;    // the plugin's name, for the script's ecs.plugin
    char *version; // the plugin's version, for the script's ecs.plugin
    ECSIMessage *input;
    SDL_Thread *thread;

    SDL_AtomicInt stopping; // stop was asked: the script ends at its next receive or call of ecs

    // the lock guards the messages both ways and the end of the script
    SDL_Mutex *lock;
    SDL_Condition *arrived; // a message came to the worker, or it should stop
    ECSIMessage **inbox;    // stb_ds array: messages to the worker, oldest first
    ECSIMessage **outbox;   // stb_ds array: messages to the main thread, oldest first
    bool finished;          // the script ended, and the thread does nothing more but end
    bool succeeded;         // the script returned, rather than raised an error
    ECSIMessage *result;    // what the script returned, or nullptr
    char *error;            // the error it raised, or nullptr

    // main thread only: registry references in the core's state
    int messageReference; // the function that gets each message, or LUA_NOREF
    int doneReference;    // the function that gets the end, or LUA_NOREF
    int handleReference;  // the handle, so it lives while the worker does
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
    if (SDL_CompareAndSwapAtomicInt(&WORKERS.posted, 0, 1))
    {
        ECSITasks_RunOnMainThread(ECSIWorkers_Pump, nullptr);
    }
}

/// @brief The registry keys of a state's string.buffer encode and decode functions, which each state keeps once it used them.
static const char OPENECS_WORKER_ENCODE = 0;
static const char OPENECS_WORKER_DECODE = 0;

/// @brief Pushes a state's string.buffer encode or decode function, by its registry key.
static void ECSIWorkers_PushCoder(lua_State *state, const char *key)
{
    lua_rawgetp(state, LUA_REGISTRYINDEX, key);

    if (!lua_isnil(state, -1))
    {
        return;
    }

    lua_pop(state, 1);
    lua_getglobal(state, "require");
    lua_pushliteral(state, "string.buffer");
    lua_call(state, 1, 1);
    lua_getfield(state, -1, "encode");
    lua_rawsetp(state, LUA_REGISTRYINDEX, &OPENECS_WORKER_ENCODE);
    lua_getfield(state, -1, "decode");
    lua_rawsetp(state, LUA_REGISTRYINDEX, &OPENECS_WORKER_DECODE);
    lua_pop(state, 1);
    lua_rawgetp(state, LUA_REGISTRYINDEX, key);
}

/// @brief Replaces the Lua value at an index with the string that string.buffer encodes it to. It raises the encoder's error for a value that it cannot
/// encode, such as a function, a handle or a struct, before anything is allocated.
static void ECSIWorkers_Encode(lua_State *state, int index)
{
    index = lua_absindex(state, index);
    ECSIWorkers_PushCoder(state, &OPENECS_WORKER_ENCODE);
    lua_pushvalue(state, index);
    lua_call(state, 1, 1);
    lua_replace(state, index);
}

/// @brief Copies the encoded string at an index into a new message.
/// @return nullptr if there is no memory for it.
static ECSIMessage *ECSIWorkers_TakeMessage(lua_State *state, int index)
{
    usz size = 0;
    const char *bytes = lua_tolstring(state, index, &size);
    ECSIMessage *message = SDL_malloc(sizeof(ECSIMessage) + size);

    if (message != nullptr)
    {
        message->size = size;
        SDL_memcpy(message->bytes, bytes, size);
    }

    return message;
}

/// @brief Encodes the Lua value at an index into a new message, or raises a Lua error.
static ECSIMessage *ECSIWorkers_CopyMessage(lua_State *state, int index)
{
    lua_pushvalue(state, index);
    ECSIWorkers_Encode(state, -1);
    ECSIMessage *message = ECSIWorkers_TakeMessage(state, -1);
    lua_pop(state, 1);

    if (message == nullptr)
    {
        luaL_error(state, "out of memory");
    }

    return message;
}

/// @brief Pushes the value that a message holds, which string.buffer decodes in the state that receives it; nil for no message.
static void ECSIWorkers_PushMessage(lua_State *state, const ECSIMessage *message)
{
    if (message == nullptr)
    {
        lua_pushnil(state);
        return;
    }

    ECSIWorkers_PushCoder(state, &OPENECS_WORKER_DECODE);
    lua_pushlstring(state, message->bytes, message->size);
    lua_call(state, 1, 1);
}

/// @brief Pushes the value of the message that a light userdata names: the function of a protected call, after which the caller frees the message.
static int ECSIWorkers_Decode(lua_State *state)
{
    ECSIWorkers_PushMessage(state, lua_touserdata(state, 1));
    return 1;
}

/// @brief The key of each state's ECSIWorkers_Decode in its registry.
static const char OPENECS_WORKERS_DECODE_KEY = 0;

/// @brief Pushes ECSIWorkers_Decode. LuaJIT has no light C functions, so it is made once for each state and kept in its registry.
static void ECSIWorkers_PushDecode(lua_State *state)
{
    lua_rawgetp(state, LUA_REGISTRYINDEX, &OPENECS_WORKERS_DECODE_KEY);

    if (lua_isnil(state, -1))
    {
        lua_pop(state, 1);
        lua_pushcfunction(state, ECSIWorkers_Decode);
        lua_pushvalue(state, -1);
        lua_rawsetp(state, LUA_REGISTRYINDEX, &OPENECS_WORKERS_DECODE_KEY);
    }
}

/// @brief Frees a list of messages.
static void ECSIWorkers_FreeMessages(ECSIMessage ***messages)
{
    for (usz i = 0; i < arrlenu(*messages); i++)
    {
        SDL_free((*messages)[i]);
    }

    arrfree(*messages);
}

/// @brief Gets the worker whose state runs the code.
static ECSIWorker *ECSIWorkers_GetCurrent(lua_State *state)
{
    lua_rawgetp(state, LUA_REGISTRYINDEX, &OPENECS_WORKER_KEY);
    ECSIWorker *worker = lua_touserdata(state, -1);
    lua_pop(state, 1);
    return worker;
}

/// @brief Ends a worker's script with an error once the worker should stop. Each call of ecs checks it, as LuaJIT runs no hooks in the code it compiles.
/// The log and error functions are the main state's too, which has no worker.
static void ECSIWorkers_CheckStopped(lua_State *state)
{
    ECSIWorker *worker = ECSIWorkers_GetCurrent(state);

    if (worker != nullptr && SDL_GetAtomicInt(&worker->stopping))
    {
        luaL_error(state, "the worker was stopped");
    }
}

#pragma region Log

static int ECSIWorkers_Log(lua_State *state, ECSLogLevel level)
{
    ECSIWorkers_CheckStopped(state);
    ECS_Log(lua_touserdata(state, lua_upvalueindex(1)), level, "%s", ECSILua_CheckString(state, 1));
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

/// @brief ecs.fail(message): logs a failure the plugin returns, while ecs.logServiceErrors is on, and gives the message, for return nil, ecs.fail(message).
static int ECSIWorkers_Fail(lua_State *state)
{
    ECSIWorkers_CheckStopped(state);
    const char *message = ECSILua_CheckString(state, 1);
    const char *error = nullptr;
    ECS_Fail(lua_touserdata(state, lua_upvalueindex(1)), &error, "%s", message);
    lua_pushstring(state, error);
    return 1;
}

/// @brief ecs.check(condition, message): a misuse when the condition is false or nil. It stops the Lua code, with the line that called the checking function.
static int ECSIWorkers_Check(lua_State *state)
{
    ECSIWorkers_CheckStopped(state);
    ECSILua_CheckAny(state, 1);
    const char *message = ECSILua_CheckString(state, 2);

    if (lua_toboolean(state, 1))
    {
        return 0;
    }

    lua_State *outer = ECSILua_SetCaller(state);
    ECSIPlugin_NoteMisuse(lua_touserdata(state, lua_upvalueindex(1)), message);
    (void)ECSILua_SetCaller(outer);

    // as error(message, 2): the line to fix is the one that called the function which checked
    luaL_where(state, 2);
    lua_pushstring(state, message);
    lua_concat(state, 2);
    return lua_error(state);
}

static const luaL_Reg OPENECS_WORKERS_ERRORS[] = {
    {"fail", ECSIWorkers_Fail},
    {"check", ECSIWorkers_Check},
    {nullptr, nullptr},
};

#pragma endregion Log

#pragma region Worker Thread

/// @brief ecs.worker.send(value): sends a copy of a value to the main thread.
static int ECSIWorkers_SendToMain(lua_State *state)
{
    ECSIWorkers_CheckStopped(state);
    ECSILua_CheckAny(state, 1);
    ECSIWorker *worker = ECSIWorkers_GetCurrent(state);
    ECSIMessage *message = ECSIWorkers_CopyMessage(state, 1);

    SDL_LockMutex(worker->lock);
    arrput(worker->outbox, message);
    SDL_UnlockMutex(worker->lock);
    ECSIWorkers_Post();
    return 0;
}

/// @brief ecs.worker.receive(seconds): gives the oldest message from the main thread, waiting for one at most seconds, or without end if seconds is missing.
static int ECSIWorkers_Receive(lua_State *state)
{
    f64 seconds = (f64)ECSILua_OptNumber(state, 1, -1.0);
    ECSIWorker *worker = ECSIWorkers_GetCurrent(state);
    u64 end = SDL_GetTicksNS() + (u64)(SDL_max(seconds, 0.0) * (f64)SDL_NS_PER_SECOND);
    ECSIMessage *message = nullptr;

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
        SDL_free(message);
        return luaL_error(state, "the worker was stopped");
    }

    // the message is freed even if decoding it fails, which only running out of memory does
    ECSIWorkers_PushDecode(state);
    lua_pushlightuserdata(state, message);
    int failed = lua_pcall(state, 1, 1, 0);
    SDL_free(message);
    return failed ? lua_error(state) : 1;
}

static const luaL_Reg OPENECS_WORKERS_OWN[] = {
    {"send", ECSIWorkers_SendToMain},
    {"receive", ECSIWorkers_Receive},
    {nullptr, nullptr},
};

/// @brief Runs a worker's script in a protected call, so every error, running out of memory too, is an error of the script.
/// First it prepares the state: its ecs module, which holds log, plugin and the worker's own functions, and a require that finds the plugin's modules.
/// Then it runs the script with the input, and keeps what the script returns.
static int ECSIWorkers_Script(lua_State *state)
{
    ECSIWorker *worker = ECSIWorkers_GetCurrent(state);

    luaL_getsubtable(state, LUA_REGISTRYINDEX, "_LOADED");
    lua_newtable(state);
    ECSIWorkers_PushLog(state, worker->plugin);
    lua_setfield(state, -2, "log");
    ECSIWorkers_AddErrors(state, worker->plugin);
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
    ECSIWorkers_PushMessage(state, worker->input);
    lua_call(state, 1, 1);
    ECSIWorkers_Encode(state, -1);
    worker->result = ECSIWorkers_TakeMessage(state, -1);

    if (worker->result == nullptr)
    {
        return luaL_error(state, "out of memory");
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
        lua_pushlightuserdata(state, worker);
        lua_rawsetp(state, LUA_REGISTRYINDEX, &OPENECS_WORKER_KEY);
        lua_pushcfunction(state, ECSIWorkers_Script);
        ECSICallback callback;
        ECSIPlugin_Enter(&callback, worker->plugin, nullptr, ECSICallbackType_Plugin);
        worker->succeeded = !ECSILua_Call(state, 0, 0);
        (void)ECSIPlugin_Leave(&callback);

        if (!worker->succeeded)
        {
            const char *message = lua_tostring(state, -1);
            worker->error = SDL_strdup(message == nullptr ? "the worker cannot run" : message);
            SDL_free(worker->result);
            worker->result = nullptr;
        }

        lua_close(state);
    }

    ECSIServices_EndThread();
    ECSIPlugins_EndThread();
    SDL_LockMutex(worker->lock);
    worker->finished = true;
    SDL_UnlockMutex(worker->lock);
    ECSIWorkers_Post();
    return 0;
}

#pragma endregion Worker Thread

#pragma region Main Thread

/// @brief Asks a worker to stop: its next receive or call of ecs ends the script.
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

    if (worker->handleReference != LUA_NOREF)
    {
        lua_rawgeti(state, LUA_REGISTRYINDEX, worker->handleReference);
        *(ECSIWorker **)lua_touserdata(state, -1) = nullptr;
        lua_pop(state, 1);
    }

    luaL_unref(state, LUA_REGISTRYINDEX, worker->messageReference);
    luaL_unref(state, LUA_REGISTRYINDEX, worker->doneReference);
    luaL_unref(state, LUA_REGISTRYINDEX, worker->handleReference);

    ECSIWorkers_FreeMessages(&worker->inbox);
    ECSIWorkers_FreeMessages(&worker->outbox);
    SDL_free(worker->input);
    SDL_free(worker->result);
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

    ECSICallback callback;
    ECSIPlugin_Enter(&callback, worker->plugin, nullptr, ECSICallbackType_Plugin);

    if (ECSILua_Call(state, argumentCount, 0))
    {
        ECSIPlugin_ReportError(worker->plugin, lua_tostring(state, -1));
        lua_pop(state, 1);
    }

    (void)ECSIPlugin_Leave(&callback);
}

/// @brief Calls one of a worker's functions with the values on top of the stack and then the value of a message, and reports an error.
/// @param count How many values are on the stack before the message's.
static void ECSIWorkers_Deliver(ECSIWorker *worker, int function, const ECSIMessage *message, int count)
{
    lua_State *state = ECSILua_GetState();
    ECSIWorkers_PushDecode(state);
    lua_pushlightuserdata(state, (void *)message);

    if (lua_pcall(state, 1, 1, 0) != LUA_OK)
    {
        ECSIPlugin_ReportError(worker->plugin, lua_tostring(state, -1));
        lua_pop(state, count + 1);
        return;
    }

    ECSIWorkers_Call(worker, function, count + 1);
}

/// @brief Takes the messages a worker sent and delivers them to its message function, unless the worker was stopped.
/// @return Whether the worker's script had ended when its messages were taken.
static bool ECSIWorkers_DeliverMessages(ECSIWorker *worker)
{
    SDL_LockMutex(worker->lock);
    ECSIMessage **messages = worker->outbox;
    worker->outbox = nullptr;
    bool finished = worker->finished;
    SDL_UnlockMutex(worker->lock);

    for (usz i = 0; i < arrlenu(messages) && worker->messageReference != LUA_NOREF; i++)
    {
        if (!SDL_GetAtomicInt(&worker->stopping))
        {
            ECSIWorkers_Deliver(worker, worker->messageReference, messages[i], 0);
        }
    }

    ECSIWorkers_FreeMessages(&messages);
    return finished;
}

/// @brief Reports the end of a worker's script to its plugin: an error to the plugin's log, and the result to its done function.
static void ECSIWorkers_DeliverEnd(ECSIWorker *worker)
{
    const char *error = worker->error == nullptr ? "out of memory" : worker->error;

    if (!worker->succeeded)
    {
        ECSIPlugin_ReportError(worker->plugin, error);
    }

    if (worker->doneReference == LUA_NOREF)
    {
        return;
    }

    lua_State *state = ECSILua_GetState();
    lua_pushboolean(state, worker->succeeded);

    if (worker->succeeded)
    {
        ECSIWorkers_Deliver(worker, worker->doneReference, worker->result, 1);
        return;
    }

    lua_pushstring(state, error);
    ECSIWorkers_Call(worker, worker->doneReference, 2);
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

        if (!ECSIWorkers_DeliverMessages(worker))
        {
            i++;
            continue;
        }

        SDL_WaitThread(worker->thread, nullptr);
        arrdel(WORKERS.workers, i);

        if (!SDL_GetAtomicInt(&worker->stopping))
        {
            ECSIWorkers_DeliverEnd(worker);
        }

        ECSIWorkers_Free(worker);
    }
}

/// @brief Reads a worker handle. Gives nullptr once its worker is gone.
static ECSIWorker *ECSIWorkers_CheckHandle(lua_State *state)
{
    return *(ECSIWorker **)ECSILua_CheckUdata(state, 1, OPENECS_LUA_WORKER);
}

/// @brief ecs.worker.start(desc): starts a worker of a plugin, and gives its handle, or nil and a message.
static int ECSIWorkers_Start(lua_State *state)
{
    ECSPlugin plugin = lua_touserdata(state, lua_upvalueindex(1));
    ECSILua_CheckType(state, 1, LUA_TTABLE);
    ECSILua_CheckFunctionField(state, 1, "message");
    ECSILua_CheckFunctionField(state, 1, "done");
    lua_getfield(state, 1, "file");
    const char *file = ECSILua_CheckString(state, -1);
    const char *path = lua_pushfstring(state, "%s%s", ECSPlugin_GetFolder(plugin), file);

    if (!SDL_GetPathInfo(path, nullptr))
    {
        lua_pushnil(state);
        lua_pushfstring(state, "no file '%s' in the plugin's folder", file);
        return 2;
    }

    // the input is encoded first, as an input that cannot be is the caller's error, raised before anything is allocated
    lua_getfield(state, 1, "input");
    ECSIWorkers_Encode(state, -1);
    int input = lua_gettop(state);

    // the handle comes next, since making it may raise an error, which would leave the worker unfreed
    ECSIWorker **handle = lua_newuserdata(state, sizeof(ECSIWorker *));
    *handle = nullptr;
    luaL_setmetatable(state, OPENECS_LUA_WORKER);
    ECSIWorker *worker = SDL_calloc(1, sizeof(ECSIWorker));

    if (worker == nullptr)
    {
        return luaL_error(state, "out of memory");
    }

    worker->plugin = plugin;
    worker->messageReference = LUA_NOREF;
    worker->doneReference = LUA_NOREF;
    worker->handleReference = LUA_NOREF;
    worker->path = SDL_strdup(path);
    worker->folder = SDL_strdup(ECSPlugin_GetFolder(plugin));
    worker->name = SDL_strdup(ECSIPlugin_GetName(plugin));
    worker->version = SDL_strdup(ECSIPlugin_GetVersion(plugin));
    worker->lock = SDL_CreateMutex();
    worker->arrived = SDL_CreateCondition();

    worker->input = ECSIWorkers_TakeMessage(state, input);
    SHUResult result = SHUResult_Ok;

    if ((worker->input == nullptr || worker->path == nullptr || worker->folder == nullptr || worker->name == nullptr || worker->version == nullptr || worker->lock == nullptr || worker->arrived == nullptr))
    {
        result = SHUResult_ErrAllocation;
    }

    if (result)
    {
        ECSIWorkers_Free(worker);
        ECSIServices_Raise(state, result, "the worker cannot start");
    }

    worker->messageReference = ECSILua_ReferenceFunctionField(state, 1, "message");
    worker->doneReference = ECSILua_ReferenceFunctionField(state, 1, "done");

    *handle = worker;
    lua_pushvalue(state, -1);
    worker->handleReference = luaL_ref(state, LUA_REGISTRYINDEX);

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
static int ECSIWorkers_SendToWorker(lua_State *state)
{
    ECSIWorker *worker = ECSIWorkers_CheckHandle(state);
    ECSILua_CheckAny(state, 2);

    if (worker == nullptr || SDL_GetAtomicInt(&worker->stopping))
    {
        return 0;
    }

    ECSIMessage *message = ECSIWorkers_CopyMessage(state, 2);

    SDL_LockMutex(worker->lock);
    arrput(worker->inbox, message);
    SDL_SignalCondition(worker->arrived);
    SDL_UnlockMutex(worker->lock);
    return 0;
}

/// @brief worker:stop(): stops a worker. Its functions are not called again.
static int ECSIWorkers_StopHandle(lua_State *state)
{
    ECSIWorker *worker = ECSIWorkers_CheckHandle(state);

    if (worker != nullptr)
    {
        ECSIWorkers_Stop(worker);
    }

    return 0;
}

/// @brief worker.running: whether the worker's script still runs: it has not ended, and was not stopped.
static int ECSIWorkers_Index(lua_State *state)
{
    ECSIWorker *worker = ECSIWorkers_CheckHandle(state);
    const char *key = ECSILua_CheckString(state, 2);

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
    {"send", ECSIWorkers_SendToWorker},
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

void ECSIWorkers_StopPlugin(ECSPlugin plugin)
{
    SDL_assert(plugin != nullptr);

    for (usz i = 0; i < arrlenu(WORKERS.workers);)
    {
        ECSIWorker *worker = WORKERS.workers[i];

        if (worker->plugin != plugin)
        {
            i++;
            continue;
        }

        ECSIWorkers_Stop(worker);
        SDL_WaitThread(worker->thread, nullptr);
        arrdel(WORKERS.workers, i);
        ECSIWorkers_Free(worker);
    }
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

void ECSIWorkers_PushLog(lua_State *state, ECSPlugin plugin)
{
    SDL_assert(state != nullptr);
    SDL_assert(plugin != nullptr);

    lua_newtable(state);
    lua_pushlightuserdata(state, plugin);
    luaL_setfuncs(state, OPENECS_WORKERS_LOG, 1);
}

void ECSIWorkers_AddErrors(lua_State *state, ECSPlugin plugin)
{
    SDL_assert(state != nullptr);
    SDL_assert(plugin != nullptr);

    lua_pushlightuserdata(state, plugin);
    luaL_setfuncs(state, OPENECS_WORKERS_ERRORS, 1);
}
