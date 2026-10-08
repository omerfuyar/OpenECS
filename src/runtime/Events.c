#include "runtime/Events.h"

#include "SDL3/SDL.h"
#include "stb/stbSDL3.h"

#pragma region Source Only

typedef struct ECSITimer
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
} ECSITimer;

/// @brief An event waiting in the queue.
typedef struct ECSIQueuedEvent
{
    ECSIEventDeliverFunction Deliver;
    void *target;
    ECSPanelEvent event;
} ECSIQueuedEvent;

/// @brief A declared named event.
typedef struct ECSINamedEvent
{
    ECSPlugin plugin; // NULL for the core's events
    char *description;
} ECSINamedEvent;

struct ECSISubscription
{
    ECSPlugin plugin;
    char *name;
    ECSEventFunction Function;
    ECSTimerFunction Release; // NULL if the data needs no release
    void *data;
    bool cancelled; // freed when no event is being delivered
};

/// @brief A named event waiting in the queue to be delivered to its subscribers.
typedef struct ECSIEmission
{
    char *name;
    ECSValue *value;
} ECSIEmission;

/// @brief The core's named events.
static const char *const OPENECS_CORE_EVENTS[][2] = {
    {"ecs.panel_opened", "A panel was opened: { panel = id, type = name }"},
    {"ecs.panel_closed", "A panel was closed: { panel = id, type = name }"},
    {"ecs.focus_changed", "Another panel got the focus: { panel = id }, or {} when no panel has it"},
    {"ecs.workspace_switched", "Another workspace is shown: { workspace = number }"},
    {"ecs.layout_changed", "Panels were moved, grouped, closed, maximized or locked: {}"},
};

/// @brief Work for a worker thread.
typedef struct ECSIWork
{
    ECSPlugin plugin;
    ECSTaskFunction Work;
    ECSTaskFunction Done;
    void *data;
} ECSIWork;

/// @brief A function that runs on the main thread.
typedef struct ECSIMainTask
{
    ECSTaskFunction Function;
    void *data;
} ECSIMainTask;

/// @brief Most worker threads.
#define OPENECS_MAX_WORKERS 4

/// @brief Workers and main-thread tasks, apart from EVENTS because other threads use them.
static struct
{
    SDL_Mutex *lock;
    SDL_Condition *ready;
    ECSIWork *queue;      // stb_ds array, guarded by lock
    SDL_Thread **workers;  // stb_ds array
    bool stopping;         // guarded by lock
    SDL_AtomicInt stopped; // tasks that reach the main thread after this are dropped, because plugins may be gone
    SDL_AtomicU32 wakeEvent;
} WORKERS = {0};

static struct
{
    ECSIQueuedEvent *queue; // stb_ds array
    bool delivering;
    ECSITimer **timers; // stb_ds array; handles point to the timers, so each is allocated on its own
    bool runningTimers;
    bool freeingTimers;
    struct
    {
        char *key;
        ECSINamedEvent value;
    } *named;                                 // stb_ds hash map of the declared named events, with copied keys
    struct ECSISubscription **subscriptions; // stb_ds array; handles point to them, so each is allocated on its own
} EVENTS = {0};

static void ECSIEvents_FreeSubscription(struct ECSISubscription *subscription)
{
    if (subscription->Release != NULL)
    {
        subscription->Release(subscription->data);
    }

    SDL_free(subscription->name);
    SDL_free(subscription);
}

/// @brief Frees the cancelled subscriptions, unless events are being delivered and they may still be read.
static void ECSIEvents_FreeCancelled(void)
{
    if (EVENTS.delivering)
    {
        return;
    }

    for (usz i = 0; i < arrlenu(EVENTS.subscriptions);)
    {
        struct ECSISubscription *subscription = EVENTS.subscriptions[i];

        if (subscription->cancelled)
        {
            arrdel(EVENTS.subscriptions, i);
            ECSIEvents_FreeSubscription(subscription);
        }
        else
        {
            i++;
        }
    }
}

/// @brief Delivers a named event to its subscribers, then frees it.
static void ECSIEvents_DeliverEmission(void *target, const ECSPanelEvent *event)
{
    (void)event;
    ECSIEmission *emission = target;

    // a subscriber may subscribe or cancel while it runs, so the list is read again at each step
    for (usz i = 0; i < arrlenu(EVENTS.subscriptions); i++)
    {
        struct ECSISubscription *subscription = EVENTS.subscriptions[i];

        if (!subscription->cancelled && SDL_strcmp(subscription->name, emission->name) == 0)
        {
            subscription->Function(subscription->data, emission->name, emission->value);
        }
    }

    SDL_free(emission->name);
    ECSValue_Destroy(&emission->value);
    SDL_free(emission);
}

/// @brief Declares a named event; plugin is NULL for the core's.
static SHUResult ECSIEvents_Declare(ECSPlugin plugin, const char *name, const char *description)
{
    if (EVENTS.named == NULL)
    {
        sh_new_strdup(EVENTS.named);
    }

    if (shgeti(EVENTS.named, name) >= 0)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Event '%s' is already declared.", name);
        return SHUResult_ErrBadData;
    }

    ECSINamedEvent named = {.plugin = plugin, .description = SDL_strdup(description)};

    if (named.description == NULL)
    {
        return SHUResult_ErrAllocation;
    }

    shput(EVENTS.named, name, named);
    return SHUResult_Ok;
}

/// @brief Queues a named event for its subscribers.
static SHUResult ECSIEvents_Emit(const char *name, const ECSValue *value)
{
    ECSIEmission *emission = SDL_calloc(1, sizeof(ECSIEmission));

    if (emission == NULL || (emission->name = SDL_strdup(name)) == NULL || ECSValue_Create(&emission->value) || (value != NULL && ECSIValue_Copy(emission->value, value)))
    {
        if (emission != NULL)
        {
            SDL_free(emission->name);
            ECSValue_Destroy(&emission->value);
            SDL_free(emission);
        }

        return SHUResult_ErrAllocation;
    }

    ECSIEvents_Post(ECSIEvents_DeliverEmission, emission, &(ECSPanelEvent){0});
    return SHUResult_Ok;
}

static void ECSIEvents_FreeTimer(ECSITimer *timer)
{
    if (timer->Release != NULL)
    {
        timer->Release(timer->data);
    }

    SDL_free(timer);
}

/// @brief Frees the stopped timers, unless timers are running and may still be read.
static void ECSIEvents_FreeStoppedTimers(void)
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
            ECSITimer *timer = EVENTS.timers[i];

            if (!timer->stopped)
            {
                i++;
                continue;
            }

            arrdel(EVENTS.timers, i);
            ECSIEvents_FreeTimer(timer);
            freed = true;
        }
    }

    EVENTS.freeingTimers = false;
}

/// @brief Wakes the main loop with an event that does nothing, so it makes a pass after a task ran.
static void ECSIEvents_Wake(void)
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
static void SDLCALL ECSIEvents_RunMainTask(void *data)
{
    ECSIMainTask *task = data;

    if (!SDL_GetAtomicInt(&WORKERS.stopped))
    {
        task->Function(task->data);
        ECSIEvents_Wake();
    }

    SDL_free(task);
}

/// @brief Runs a task that the main thread queued for itself, after the current callback.
static void ECSIEvents_DeliverMainTask(void *target, const ECSPanelEvent *event)
{
    (void)event;
    ECSIEvents_RunMainTask(target);
}

static int SDLCALL ECSIEvents_Worker(void *unused)
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

        ECSIWork work = WORKERS.queue[0];
        arrdel(WORKERS.queue, 0);
        SDL_UnlockMutex(WORKERS.lock);

        work.Work(work.data);

        if (work.Done != NULL && ECS_RunOnMainThread(work.Done, work.data))
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Background work of plugin '%s' finished, but its done function cannot be queued.", ECSIPlugin_GetName(work.plugin));
        }
    }
}

/// @brief Starts the worker threads the first time background work comes. The caller holds the lock.
static SHUResult ECSIEvents_StartWorkers(void)
{
    if (arrlenu(WORKERS.workers) > 0)
    {
        return SHUResult_Ok;
    }

    int count = SDL_clamp(SDL_GetNumLogicalCPUCores() - 1, 1, OPENECS_MAX_WORKERS);

    for (int i = 0; i < count; i++)
    {
        SDL_Thread *worker = SDL_CreateThread(ECSIEvents_Worker, "OpenECS worker", NULL);

        if (worker != NULL)
        {
            arrput(WORKERS.workers, worker);
        }
    }

    return arrlenu(WORKERS.workers) > 0 ? SHUResult_Ok : SHUResult_ErrInternal;
}

#pragma endregion Source Only

SHUResult ECSIEvents_Initialize(void)
{
    WORKERS.lock = SDL_CreateMutex();
    WORKERS.ready = SDL_CreateCondition();

    if (WORKERS.lock == NULL || WORKERS.ready == NULL)
    {
        return SHUResult_ErrAllocation;
    }

    for (usz i = 0; i < SDL_arraysize(OPENECS_CORE_EVENTS); i++)
    {
        SHU_ReturnResult(ECSIEvents_Declare(NULL, OPENECS_CORE_EVENTS[i][0], OPENECS_CORE_EVENTS[i][1]));
    }

    return SHUResult_Ok;
}

void ECSIEvents_EmitCore(const char *name, const ECSValue *value)
{
    SDL_assert(name != NULL);
    SDL_assert(shgeti(EVENTS.named, name) >= 0 && EVENTS.named[shgeti(EVENTS.named, name)].value.plugin == NULL);

    if (ECSIEvents_Emit(name, value))
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Out of memory while emitting '%s'.", name);
    }
}

SHUResult ECSIEvents_Subscribe(ECSPlugin plugin, const char *name, ECSSubscription *retSubscription, ECSEventFunction function, ECSTimerFunction release, void *data)
{
    SDL_assert(plugin != NULL);
    SDL_assert(name != NULL && retSubscription != NULL && function != NULL);

    *retSubscription = NULL;
    ptrdiff_t index = EVENTS.named == NULL ? -1 : shgeti(EVENTS.named, name);

    if (index < 0)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' subscribes to '%s', which no plugin declared.", ECSIPlugin_GetName(plugin), name);
        return SHUResult_ErrNotFound;
    }

    // a plugin hears only the core, itself, and the plugins it depends on
    ECSPlugin owner = EVENTS.named[index].value.plugin;

    if (owner != NULL && !ECSIPlugin_DependsOn(plugin, owner))
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' subscribes to '%s', but its manifest does not depend on '%s'.", ECSIPlugin_GetName(plugin), name, ECSIPlugin_GetName(owner));
        return SHUResult_ErrBadData;
    }

    struct ECSISubscription *subscription = SDL_calloc(1, sizeof(struct ECSISubscription));

    if (subscription == NULL || (subscription->name = SDL_strdup(name)) == NULL)
    {
        SDL_free(subscription);
        return SHUResult_ErrAllocation;
    }

    subscription->plugin = plugin;
    subscription->Function = function;
    subscription->Release = release;
    subscription->data = data;
    arrput(EVENTS.subscriptions, subscription);
    *retSubscription = subscription;
    return SHUResult_Ok;
}

void ECSIEvents_RemovePlugin(ECSPlugin plugin)
{
    SDL_assert(plugin != NULL);

    for (usz i = 0; i < arrlenu(EVENTS.subscriptions); i++)
    {
        struct ECSISubscription *subscription = EVENTS.subscriptions[i];
        ptrdiff_t index = shgeti(EVENTS.named, subscription->name);

        if (subscription->plugin == plugin || (index >= 0 && EVENTS.named[index].value.plugin == plugin))
        {
            subscription->cancelled = true;
        }
    }

    // backwards, because shdel moves the last entry into the hole
    for (usz i = shlenu(EVENTS.named); i > 0; i--)
    {
        if (EVENTS.named[i - 1].value.plugin == plugin)
        {
            SDL_free(EVENTS.named[i - 1].value.description);
            (void)shdel(EVENTS.named, EVENTS.named[i - 1].key);
        }
    }

    ECSIEvents_FreeCancelled();
}

SHUResult ECSEvent_Declare(ECSPlugin plugin, const char *name, const char *description)
{
    SDL_assert(plugin != NULL);
    SDL_assert(name != NULL && description != NULL);

    return ECSIPlugin_OwnsName(plugin, name) ? ECSIEvents_Declare(plugin, name, description) : SHUResult_ErrBadData;
}

SHUResult ECSEvent_Emit(ECSPlugin plugin, const char *name, const ECSValue *value)
{
    SDL_assert(plugin != NULL);
    SDL_assert(name != NULL);

    ptrdiff_t index = EVENTS.named == NULL ? -1 : shgeti(EVENTS.named, name);

    if (index < 0 || EVENTS.named[index].value.plugin != plugin)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' emits '%s', which it did not declare.", ECSIPlugin_GetName(plugin), name);
        return SHUResult_ErrNotFound;
    }

    return ECSIEvents_Emit(name, value);
}

SHUResult ECSEvent_Subscribe(ECSPlugin plugin, const char *name, ECSSubscription *retSubscription, ECSEventFunction function, void *data)
{
    return ECSIEvents_Subscribe(plugin, name, retSubscription, function, NULL, data);
}

void ECSEvent_Unsubscribe(ECSSubscription *subscription)
{
    SDL_assert(subscription != NULL);

    if (*subscription != NULL)
    {
        (*subscription)->cancelled = true;
        *subscription = NULL;
        ECSIEvents_FreeCancelled();
    }
}

void ECSIEvents_Post(ECSIEventDeliverFunction deliver, void *target, const ECSPanelEvent *event)
{
    SDL_assert(deliver != NULL);
    SDL_assert(event != NULL);

    ECSIQueuedEvent queued = {.Deliver = deliver, .target = target, .event = *event};
    arrput(EVENTS.queue, queued);
}

void ECSIEvents_Deliver(void)
{
    SDL_assert(!EVENTS.delivering); // events are never delivered inside another event handler

    EVENTS.delivering = true;

    for (usz i = 0; i < arrlenu(EVENTS.queue); i++)
    {
        // a copy, because delivering may queue more events and move the queue
        ECSIQueuedEvent queued = EVENTS.queue[i];
        queued.Deliver(queued.target, &queued.event);
    }

    arrfree(EVENTS.queue);
    EVENTS.delivering = false;
    ECSIEvents_FreeCancelled();
}

SHUResult ECSIEvents_StartTimer(ECSPlugin plugin, const void *owner, ECSTimer *retTimer, f64 seconds, bool repeat, ECSTimerFunction function, ECSTimerFunction release, void *data)
{
    SDL_assert(plugin != NULL);
    SDL_assert(retTimer != NULL);
    SDL_assert(function != NULL);
    SDL_assert(seconds > 0.0 || (seconds == 0.0 && !repeat));

    ECSITimer *timer = SDL_malloc(sizeof(ECSITimer));

    if (timer == NULL)
    {
        return SHUResult_ErrAllocation;
    }

    u64 intervalTicks = (u64)(seconds * (f64)SDL_NS_PER_SECOND);

    *timer = (ECSITimer){
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

void ECSIEvents_StopTimersOf(const void *owner)
{
    SDL_assert(owner != NULL);

    for (usz i = 0; i < arrlenu(EVENTS.timers); i++)
    {
        if (EVENTS.timers[i]->owner == owner)
        {
            EVENTS.timers[i]->stopped = true;
        }
    }

    ECSIEvents_FreeStoppedTimers();
}

void ECSIEvents_StopTimersOfPlugin(ECSPlugin plugin)
{
    SDL_assert(plugin != NULL);

    for (usz i = 0; i < arrlenu(EVENTS.timers); i++)
    {
        if (EVENTS.timers[i]->plugin == plugin)
        {
            EVENTS.timers[i]->stopped = true;
        }
    }

    ECSIEvents_FreeStoppedTimers();
}

i32 ECSIEvents_GetWait(void)
{
    if (arrlenu(EVENTS.queue) > 0)
    {
        return 0;
    }

    u64 now = SDL_GetTicksNS();
    u64 wait = UINT64_MAX;

    for (usz i = 0; i < arrlenu(EVENTS.timers); i++)
    {
        ECSITimer *timer = EVENTS.timers[i];

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

void ECSIEvents_RunTimers(void)
{
    u64 now = SDL_GetTicksNS();
    EVENTS.runningTimers = true;

    // timers started by the functions wait for the next call
    usz count = arrlenu(EVENTS.timers);

    for (usz i = 0; i < count; i++)
    {
        ECSITimer *timer = EVENTS.timers[i];

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
    ECSIEvents_FreeStoppedTimers();
}

void ECSIEvents_Terminate(void)
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
        ECSIEvents_FreeTimer(EVENTS.timers[i]);
    }

    arrfree(EVENTS.timers);

    // named events still queued hold their copies
    for (usz i = 0; i < arrlenu(EVENTS.queue); i++)
    {
        if (EVENTS.queue[i].Deliver == ECSIEvents_DeliverEmission)
        {
            ECSIEmission *emission = EVENTS.queue[i].target;
            SDL_free(emission->name);
            ECSValue_Destroy(&emission->value);
            SDL_free(emission);
        }
    }

    arrfree(EVENTS.queue);

    for (usz i = 0; i < arrlenu(EVENTS.subscriptions); i++)
    {
        ECSIEvents_FreeSubscription(EVENTS.subscriptions[i]);
    }

    arrfree(EVENTS.subscriptions);

    for (usz i = 0; i < shlenu(EVENTS.named); i++)
    {
        SDL_free(EVENTS.named[i].value.description);
    }

    shfree(EVENTS.named);
    SDL_zero(EVENTS);
}

SHUResult ECSTimer_Start(ECSPlugin plugin, ECSTimer *retTimer, f64 seconds, bool repeat, ECSTimerFunction function, void *data)
{
    return ECSIEvents_StartTimer(plugin, NULL, retTimer, seconds, repeat, function, NULL, data);
}

void ECSTimer_Stop(ECSTimer *timer)
{
    SDL_assert(timer != NULL && *timer != NULL);

    (*timer)->stopped = true;
    *timer = NULL;
    ECSIEvents_FreeStoppedTimers();
}

SHUResult ECS_RunInBackground(ECSPlugin plugin, ECSTaskFunction work, ECSTaskFunction done, void *data)
{
    SDL_assert(plugin != NULL);
    SDL_assert(work != NULL);
    SDL_assert(WORKERS.lock != NULL);

    SDL_LockMutex(WORKERS.lock);
    SHUResult result = WORKERS.stopping ? SHUResult_ErrInternal : ECSIEvents_StartWorkers();

    if (!result)
    {
        ECSIWork queued = {.plugin = plugin, .Work = work, .Done = done, .data = data};
        arrput(WORKERS.queue, queued);
        SDL_SignalCondition(WORKERS.ready);
    }

    SDL_UnlockMutex(WORKERS.lock);
    return result;
}

SHUResult ECS_RunOnMainThread(ECSTaskFunction function, void *data)
{
    SDL_assert(function != NULL);

    ECSIMainTask *task = SDL_malloc(sizeof(ECSIMainTask));

    if (task == NULL)
    {
        return SHUResult_ErrAllocation;
    }

    *task = (ECSIMainTask){.Function = function, .data = data};

    // SDL would run it at once on the main thread, inside the caller's callback; the event queue runs it after
    if (SDL_IsMainThread())
    {
        ECSPanelEvent event = {0};
        ECSIEvents_Post(ECSIEvents_DeliverMainTask, task, &event);
        return SHUResult_Ok;
    }

    if (!SDL_RunOnMainThread(ECSIEvents_RunMainTask, task, false))
    {
        SDL_free(task);
        return SHUResult_ErrAllocation;
    }

    return SHUResult_Ok;
}
