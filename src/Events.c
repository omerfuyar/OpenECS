#include "Events.h"

#include "SDL3/SDL.h"
#include "stb/stbSDL3.h"

#pragma region Source Only

typedef struct ECSI_Timer
{
    ECSPlugin plugin;
    const void *owner;
    u64 dueTicks;      // nanoseconds, as SDL_GetTicksNS
    u64 intervalTicks; // nanoseconds
    bool repeat;
    bool stopped; // freed when no timer is running
    ECSTimerFunction Function;
    ECSTimerFunction Release; // NULL if the data needs no release
    void *data;
} ECSI_Timer;

/// @brief An event waiting in the queue.
typedef struct ECSI_QueuedEvent
{
    ECSI_EventDeliverFunction Deliver;
    void *target;
    ECSEvent event;
} ECSI_QueuedEvent;

/// @brief Work for a worker thread.
typedef struct ECSI_Work
{
    ECSPlugin plugin;
    ECSTaskFunction Work;
    ECSTaskFunction Done;
    void *data;
} ECSI_Work;

/// @brief A function that runs on the main thread.
typedef struct ECSI_MainTask
{
    ECSTaskFunction Function;
    void *data;
} ECSI_MainTask;

/// @brief Most worker threads.
#define OPENECS_MAX_WORKERS 4

/// @brief Workers and main-thread tasks, apart from EVENTS because other threads use them.
static struct
{
    SDL_Mutex *lock;
    SDL_Condition *ready;
    ECSI_Work *queue;      // stb_ds array, guarded by lock
    SDL_Thread **workers;  // stb_ds array
    bool stopping;         // guarded by lock
    SDL_AtomicInt stopped; // tasks that reach the main thread after this are dropped, because plugins may be gone
    SDL_AtomicU32 wakeEvent;
} WORKERS = {0};

static struct
{
    ECSI_QueuedEvent *queue; // stb_ds array
    bool delivering;
    ECSI_Timer **timers; // stb_ds array; handles point to the timers, so each is allocated on its own
    bool runningTimers;
    bool freeingTimers;
} EVENTS = {0};

static void ECSI_EventsFreeTimer(ECSI_Timer *timer)
{
    if (timer->Release != NULL)
    {
        timer->Release(timer->data);
    }

    SDL_free(timer);
}

/// @brief Frees the stopped timers, unless timers are running and may still be read.
static void ECSI_EventsFreeStoppedTimers(void)
{
    if (EVENTS.runningTimers || EVENTS.freeingTimers)
    {
        return;
    }

    // a release may stop more timers, so the list is walked until nothing is freed
    EVENTS.freeingTimers = true;
    bool freed = true;

    while (freed)
    {
        freed = false;

        for (usz i = 0; i < arrlenu(EVENTS.timers);)
        {
            ECSI_Timer *timer = EVENTS.timers[i];

            if (!timer->stopped)
            {
                i++;
                continue;
            }

            arrdel(EVENTS.timers, i);
            ECSI_EventsFreeTimer(timer);
            freed = true;
        }
    }

    EVENTS.freeingTimers = false;
}

/// @brief Wakes the main loop with an event that does nothing, so it makes a pass after a task ran.
static void ECSI_EventsWake(void)
{
    u32 type = SDL_GetAtomicU32(&WORKERS.wakeEvent);

    if (type == 0)
    {
        u32 registered = SDL_RegisterEvents(1);
        type = SDL_CompareAndSwapAtomicU32(&WORKERS.wakeEvent, 0, registered) ? registered : SDL_GetAtomicU32(&WORKERS.wakeEvent);
    }

    SDL_Event event = {.type = type};
    SDL_PushEvent(&event);
}

/// @brief Runs a task that reached the main thread.
static void SDLCALL ECSI_EventsRunMainTask(void *data)
{
    ECSI_MainTask *task = data;

    if (!SDL_GetAtomicInt(&WORKERS.stopped))
    {
        task->Function(task->data);
        ECSI_EventsWake();
    }

    SDL_free(task);
}

/// @brief Runs a task that the main thread queued for itself, after the current callback.
static void ECSI_EventsDeliverMainTask(void *target, const ECSEvent *event)
{
    (void)event;
    ECSI_EventsRunMainTask(target);
}

static int SDLCALL ECSI_EventsWorker(void *unused)
{
    (void)unused;

    while (true)
    {
        SDL_LockMutex(WORKERS.lock);

        while (!WORKERS.stopping && arrlenu(WORKERS.queue) == 0)
        {
            SDL_WaitCondition(WORKERS.ready, WORKERS.lock);
        }

        if (WORKERS.stopping)
        {
            SDL_UnlockMutex(WORKERS.lock);
            return 0;
        }

        ECSI_Work work = WORKERS.queue[0];
        arrdel(WORKERS.queue, 0);
        SDL_UnlockMutex(WORKERS.lock);

        work.Work(work.data);

        if (work.Done != NULL && ECS_RunOnMainThread(work.Done, work.data))
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Background work of plugin '%s' finished, but its done function cannot be queued.", ECSI_PluginGetName(work.plugin));
        }
    }
}

/// @brief Starts the worker threads the first time background work comes. The caller holds the lock.
static SHUResult ECSI_EventsStartWorkers(void)
{
    if (arrlenu(WORKERS.workers) > 0)
    {
        return SHUResult_Ok;
    }

    int count = SDL_clamp(SDL_GetNumLogicalCPUCores() - 1, 1, OPENECS_MAX_WORKERS);

    for (int i = 0; i < count; i++)
    {
        SDL_Thread *worker = SDL_CreateThread(ECSI_EventsWorker, "OpenECS worker", NULL);

        if (worker != NULL)
        {
            arrput(WORKERS.workers, worker);
        }
    }

    return arrlenu(WORKERS.workers) > 0 ? SHUResult_Ok : SHUResult_ErrInternal;
}

#pragma endregion Source Only

SHUResult ECSI_EventsInitialize(void)
{
    WORKERS.lock = SDL_CreateMutex();
    WORKERS.ready = SDL_CreateCondition();
    return WORKERS.lock == NULL || WORKERS.ready == NULL ? SHUResult_ErrAllocation : SHUResult_Ok;
}

void ECSI_EventsPost(ECSI_EventDeliverFunction deliver, void *target, const ECSEvent *event)
{
    SDL_assert(deliver != NULL);
    SDL_assert(event != NULL);

    ECSI_QueuedEvent queued = {.Deliver = deliver, .target = target, .event = *event};
    arrput(EVENTS.queue, queued);
}

void ECSI_EventsDeliver(void)
{
    SDL_assert(!EVENTS.delivering); // events are never delivered inside another event handler

    EVENTS.delivering = true;

    for (usz i = 0; i < arrlenu(EVENTS.queue); i++)
    {
        // a copy, because delivering may queue more events and move the queue
        ECSI_QueuedEvent queued = EVENTS.queue[i];
        queued.Deliver(queued.target, &queued.event);
    }

    arrfree(EVENTS.queue);
    EVENTS.delivering = false;
}

SHUResult ECSI_EventsStartTimer(ECSPlugin plugin, const void *owner, ECSTimer *retTimer, f64 seconds, bool repeat, ECSTimerFunction function, ECSTimerFunction release, void *data)
{
    SDL_assert(plugin != NULL);
    SDL_assert(retTimer != NULL);
    SDL_assert(function != NULL);
    SDL_assert(seconds > 0.0 || (seconds == 0.0 && !repeat));

    ECSI_Timer *timer = SDL_malloc(sizeof(ECSI_Timer));

    if (timer == NULL)
    {
        return SHUResult_ErrAllocation;
    }

    u64 intervalTicks = (u64)(seconds * (f64)SDL_NS_PER_SECOND);

    *timer = (ECSI_Timer){
        .plugin = plugin,
        .owner = owner,
        .dueTicks = SDL_GetTicksNS() + intervalTicks,
        .intervalTicks = intervalTicks,
        .repeat = repeat,
        .Function = function,
        .Release = release,
        .data = data,
    };

    arrput(EVENTS.timers, timer);
    *retTimer = timer;
    return SHUResult_Ok;
}

void ECSI_EventsStopTimersOf(const void *owner)
{
    SDL_assert(owner != NULL);

    for (usz i = 0; i < arrlenu(EVENTS.timers); i++)
    {
        if (EVENTS.timers[i]->owner == owner)
        {
            EVENTS.timers[i]->stopped = true;
        }
    }

    ECSI_EventsFreeStoppedTimers();
}

void ECSI_EventsStopTimersOfPlugin(ECSPlugin plugin)
{
    SDL_assert(plugin != NULL);

    for (usz i = 0; i < arrlenu(EVENTS.timers); i++)
    {
        if (EVENTS.timers[i]->plugin == plugin)
        {
            EVENTS.timers[i]->stopped = true;
        }
    }

    ECSI_EventsFreeStoppedTimers();
}

i32 ECSI_EventsGetWait(void)
{
    if (arrlenu(EVENTS.queue) > 0)
    {
        return 0;
    }

    u64 now = SDL_GetTicksNS();
    u64 wait = UINT64_MAX;

    for (usz i = 0; i < arrlenu(EVENTS.timers); i++)
    {
        ECSI_Timer *timer = EVENTS.timers[i];

        if (!timer->stopped)
        {
            wait = SDL_min(wait, timer->dueTicks > now ? timer->dueTicks - now : 0);
        }
    }

    if (wait == UINT64_MAX)
    {
        return -1;
    }

    // rounded up, so the loop does not wake up just before the timer is due
    return (i32)SDL_min((wait + SDL_NS_PER_MS - 1) / SDL_NS_PER_MS, (u64)SDL_MAX_SINT32);
}

void ECSI_EventsRunTimers(void)
{
    u64 now = SDL_GetTicksNS();
    EVENTS.runningTimers = true;

    // timers started by the functions wait for the next call
    usz count = arrlenu(EVENTS.timers);

    for (usz i = 0; i < count; i++)
    {
        ECSI_Timer *timer = EVENTS.timers[i];

        if (timer->stopped || timer->dueTicks > now)
        {
            continue;
        }

        if (!timer->repeat)
        {
            timer->stopped = true;
        }
        else
        {
            // calls missed while the program was busy are skipped
            timer->dueTicks += timer->intervalTicks;
            timer->dueTicks = timer->dueTicks <= now ? now + timer->intervalTicks : timer->dueTicks;
        }

        timer->Function(timer->data);
    }

    EVENTS.runningTimers = false;
    ECSI_EventsFreeStoppedTimers();
}

void ECSI_EventsTerminate(void)
{
    SDL_assert(!EVENTS.runningTimers && !EVENTS.delivering);

    // running work finishes, waiting work is dropped, and no task reaches the main thread any more
    SDL_LockMutex(WORKERS.lock);
    WORKERS.stopping = true;
    SDL_BroadcastCondition(WORKERS.ready);
    SDL_UnlockMutex(WORKERS.lock);

    for (usz i = 0; i < arrlenu(WORKERS.workers); i++)
    {
        SDL_WaitThread(WORKERS.workers[i], NULL);
    }

    SDL_SetAtomicInt(&WORKERS.stopped, 1);
    arrfree(WORKERS.workers);
    arrfree(WORKERS.queue);
    SDL_DestroyCondition(WORKERS.ready);
    SDL_DestroyMutex(WORKERS.lock);
    WORKERS.lock = NULL;
    WORKERS.ready = NULL;

    // a release that stops another timer frees nothing here; every timer is freed in this loop
    EVENTS.freeingTimers = true;

    for (usz i = 0; i < arrlenu(EVENTS.timers); i++)
    {
        ECSI_EventsFreeTimer(EVENTS.timers[i]);
    }

    arrfree(EVENTS.timers);
    arrfree(EVENTS.queue);
    SDL_zero(EVENTS);
}

SHUResult ECSTimer_Start(ECSPlugin plugin, ECSTimer *retTimer, f64 seconds, bool repeat, ECSTimerFunction function, void *data)
{
    return ECSI_EventsStartTimer(plugin, NULL, retTimer, seconds, repeat, function, NULL, data);
}

void ECSTimer_Stop(ECSTimer *timer)
{
    SDL_assert(timer != NULL && *timer != NULL);

    (*timer)->stopped = true;
    *timer = NULL;
    ECSI_EventsFreeStoppedTimers();
}

SHUResult ECS_RunInBackground(ECSPlugin plugin, ECSTaskFunction work, ECSTaskFunction done, void *data)
{
    SDL_assert(plugin != NULL);
    SDL_assert(work != NULL);
    SDL_assert(WORKERS.lock != NULL);

    SDL_LockMutex(WORKERS.lock);
    SHUResult result = WORKERS.stopping ? SHUResult_ErrInternal : ECSI_EventsStartWorkers();

    if (!result)
    {
        ECSI_Work queued = {.plugin = plugin, .Work = work, .Done = done, .data = data};
        arrput(WORKERS.queue, queued);
        SDL_SignalCondition(WORKERS.ready);
    }

    SDL_UnlockMutex(WORKERS.lock);
    return result;
}

SHUResult ECS_RunOnMainThread(ECSTaskFunction function, void *data)
{
    SDL_assert(function != NULL);

    ECSI_MainTask *task = SDL_malloc(sizeof(ECSI_MainTask));

    if (task == NULL)
    {
        return SHUResult_ErrAllocation;
    }

    *task = (ECSI_MainTask){.Function = function, .data = data};

    // SDL would run it at once on the main thread, inside the caller's callback; the event queue runs it after
    if (SDL_IsMainThread())
    {
        ECSEvent event = {0};
        ECSI_EventsPost(ECSI_EventsDeliverMainTask, task, &event);
        return SHUResult_Ok;
    }

    if (!SDL_RunOnMainThread(ECSI_EventsRunMainTask, task, false))
    {
        SDL_free(task);
        return SHUResult_ErrAllocation;
    }

    return SHUResult_Ok;
}
