#include "runtime/Tasks.h"

#include "runtime/Events.h"

#include "SDL3/SDL.h"
#include "stb/stbSDL3.h"

#pragma region Source Only

/// @brief Work for a worker thread.
typedef struct ECSIWork
{
    ECSPlugin plugin;
    u32 generation; // the plugin's load whose code the functions are
    ECSTaskFunction Work;
    ECSTaskFunction Done;
    void *data;
} ECSIWork;

/// @brief A function that runs on the main thread.
typedef struct ECSIMainTask
{
    ECSPlugin plugin; // the plugin whose background work ends with it, or nullptr
    u32 generation;   // the plugin's load whose code the function is; a task of an earlier load is dropped
    ECSTaskFunction Function;
    void *data;
} ECSIMainTask;

/// @brief Most worker threads.
#define OPENECS_MAX_WORKER_THREADS 4

/// @brief The worker threads and their queue, which other threads use.
static struct
{
    SDL_Mutex *lock;
    SDL_Condition *ready;
    ECSIWork *queue;       // stb_ds array, guarded by lock
    ECSPlugin *busy;       // stb_ds array of the plugins whose work runs now, one item for each, guarded by lock
    SDL_Condition *idle;   // work finished
    SDL_Thread **workers;  // stb_ds array
    bool stopping;         // guarded by lock
    SDL_AtomicInt stopped; // tasks that reach the main thread after this are dropped, because plugins may be gone
    SDL_AtomicU32 wakeEvent;
} TASKS = {0};

/// @brief Wakes the main loop with an event that does nothing, so it makes a pass after a task ran.
static void ECSITasks_Wake(void)
{
    u32 type = SDL_GetAtomicU32(&TASKS.wakeEvent);

    if (type == 0)
    {
        u32 registered = SDL_RegisterEvents(1);
        type = SDL_CompareAndSwapAtomicU32(&TASKS.wakeEvent, 0, registered) ? registered : SDL_GetAtomicU32(&TASKS.wakeEvent);
    }

    SDL_Event event = {.type = type};
    SDL_PushEvent(&event);
}

/// @brief Runs a task that reached the main thread.
static void SDLCALL ECSITasks_RunMainTask(void *data)
{
    ECSIMainTask *task = data;

    // a plugin disabled since then has unloaded the code of its done function
    if (!SDL_GetAtomicInt(&TASKS.stopped) && (task->plugin == nullptr || ECSIPlugin_GetGeneration(task->plugin) == task->generation))
    {
        task->Function(task->data);
        ECSITasks_Wake();
    }

    SDL_free(task);
}

/// @brief Frees a task that the main thread queued for itself and that never ran.
static void ECSITasks_FreeMainTask(void *data)
{
    SDL_free(data);
}

/// @brief Runs a task that the main thread queued for itself, after the current callback.
static void ECSITasks_DeliverMainTask(void *target, const ECSPanelEvent *event)
{
    (void)event;
    ECSITasks_RunMainTask(target);
}

/// @brief Sends a function to the main thread, for a plugin's background work or for anyone.
/// @param plugin The plugin whose work ends with the function, or nullptr; the function is dropped if that plugin is disabled first.
/// @param generation The plugin's load whose code the function is.
static SHUResult ECSITasks_Post(ECSPlugin plugin, u32 generation, ECSTaskFunction function, void *data)
{
    SDL_assert(function != nullptr);

    ECSIMainTask *task = SDL_malloc(sizeof(ECSIMainTask));

    if (task == nullptr)
    {
        return SHUResult_ErrAllocation;
    }

    *task = (ECSIMainTask){.plugin = plugin, .generation = generation, .Function = function, .data = data};

    // SDL would run it at once on the main thread, inside the caller's callback; the event queue runs it after
    if (SDL_IsMainThread())
    {
        ECSPanelEvent event = {0};
        ECSIEvents_Post(ECSITasks_DeliverMainTask, ECSITasks_FreeMainTask, task, &event);
        return SHUResult_Ok;
    }

    if (!SDL_RunOnMainThread(ECSITasks_RunMainTask, task, false))
    {
        SDL_free(task);
        return SHUResult_ErrAllocation;
    }

    return SHUResult_Ok;
}

static int SDLCALL ECSITasks_Worker(void *unused)
{
    (void)unused;

    while (true)
    {
        SDL_LockMutex(TASKS.lock);

        while (!TASKS.stopping && arrlenu(TASKS.queue) == 0)
        {
            SDL_WaitCondition(TASKS.ready, TASKS.lock);
        }

        if (TASKS.stopping)
        {
            SDL_UnlockMutex(TASKS.lock);
            return 0;
        }

        ECSIWork work = TASKS.queue[0];
        arrdel(TASKS.queue, 0);
        arrput(TASKS.busy, work.plugin);
        SDL_UnlockMutex(TASKS.lock);

        work.Work(work.data);

        if (work.Done != nullptr && ECSITasks_Post(work.plugin, work.generation, work.Done, work.data))
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Background work of plugin '%s' finished, but its done function cannot be queued.", ECSIPlugin_GetName(work.plugin));
        }

        SDL_LockMutex(TASKS.lock);

        for (usz i = 0; i < arrlenu(TASKS.busy); i++)
        {
            if (TASKS.busy[i] == work.plugin)
            {
                arrdelswap(TASKS.busy, i);
                break;
            }
        }

        SDL_BroadcastCondition(TASKS.idle);
        SDL_UnlockMutex(TASKS.lock);
    }
}

/// @brief Starts the worker threads the first time background work comes. The caller holds the lock.
static SHUResult ECSITasks_StartWorkers(void)
{
    if (arrlenu(TASKS.workers) > 0)
    {
        return SHUResult_Ok;
    }

    int count = SDL_clamp(SDL_GetNumLogicalCPUCores() - 1, 1, OPENECS_MAX_WORKER_THREADS);

    for (int i = 0; i < count; i++)
    {
        SDL_Thread *worker = SDL_CreateThread(ECSITasks_Worker, "OpenECS worker", nullptr);

        if (worker != nullptr)
        {
            arrput(TASKS.workers, worker);
        }
    }

    return arrlenu(TASKS.workers) > 0 ? SHUResult_Ok : SHUResult_ErrInternal;
}

#pragma endregion Source Only

SHUResult ECSITasks_Initialize(void)
{
    TASKS.lock = SDL_CreateMutex();
    TASKS.ready = SDL_CreateCondition();
    TASKS.idle = SDL_CreateCondition();
    return TASKS.lock == nullptr || TASKS.ready == nullptr || TASKS.idle == nullptr ? SHUResult_ErrAllocation : SHUResult_Ok;
}

void ECSITasks_ForgetPlugin(ECSPlugin plugin)
{
    SDL_assert(plugin != nullptr);

    SDL_LockMutex(TASKS.lock);

    for (usz i = 0; i < arrlenu(TASKS.queue);)
    {
        if (TASKS.queue[i].plugin == plugin)
        {
            arrdel(TASKS.queue, i);
        }
        else
        {
            i++;
        }
    }

    for (bool running = true; running;)
    {
        running = false;

        for (usz i = 0; i < arrlenu(TASKS.busy) && !running; i++)
        {
            running = TASKS.busy[i] == plugin;
        }

        if (running)
        {
            SDL_WaitCondition(TASKS.idle, TASKS.lock);
        }
    }

    SDL_UnlockMutex(TASKS.lock);
}

void ECSITasks_Stop(void)
{
    // running work finishes, waiting work is dropped, and no task reaches the main thread any more
    SDL_LockMutex(TASKS.lock);
    TASKS.stopping = true;
    SDL_BroadcastCondition(TASKS.ready);
    SDL_UnlockMutex(TASKS.lock);

    for (usz i = 0; i < arrlenu(TASKS.workers); i++)
    {
        SDL_WaitThread(TASKS.workers[i], nullptr);
    }

    SDL_SetAtomicInt(&TASKS.stopped, 1);
    arrfree(TASKS.workers);
}

void ECSITasks_Terminate(void)
{
    ECSITasks_Stop();
    arrfree(TASKS.queue);
    arrfree(TASKS.busy);
    SDL_DestroyCondition(TASKS.idle);
    SDL_DestroyCondition(TASKS.ready);
    SDL_DestroyMutex(TASKS.lock);
    TASKS.lock = nullptr;
    TASKS.ready = nullptr;
}

SHUResult ECS_RunInBackground(ECSPlugin plugin, ECSTaskFunction work, ECSTaskFunction done, void *data)
{
    SDL_assert(plugin != nullptr);
    SDL_assert(work != nullptr);
    SDL_assert(TASKS.lock != nullptr);

    SDL_LockMutex(TASKS.lock);
    SHUResult result = TASKS.stopping ? SHUResult_ErrInternal : ECSITasks_StartWorkers();

    if (!result)
    {
        ECSIWork queued = {.plugin = plugin, .generation = ECSIPlugin_GetGeneration(plugin), .Work = work, .Done = done, .data = data};
        arrput(TASKS.queue, queued);
        SDL_SignalCondition(TASKS.ready);
    }

    SDL_UnlockMutex(TASKS.lock);
    return result;
}

SHUResult ECS_RunOnMainThread(ECSTaskFunction function, void *data)
{
    SDL_assert(function != nullptr);
    return ECSITasks_Post(nullptr, 0, function, data);
}
