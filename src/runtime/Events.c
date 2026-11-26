#include "runtime/Events.h"

#include "base/Log.h"
#include "base/Timers.h"

#include "SDL3/SDL.h"
#include "stb/stbSDL3.h"

#pragma region Source Only

static void ECSIEvents_RunPluginTimer(void *owner, ECSTimerFunction function, void *data);

/// @brief An event waiting in the queue.
typedef struct ECSIQueuedEvent
{
    ECSIEventDeliverFunction Deliver;
    ECSTimerFunction Release; // frees the target of an event that is never delivered, or nullptr
    void *target;
    ECSValue *value; // a value in the target whose objects ECSIEvents_ForgetObject forgets, or nullptr
    ECSPanelEvent event;
} ECSIQueuedEvent;

/// @brief A declared named event.
typedef struct ECSINamedEvent
{
    ECSPlugin plugin; // nullptr for the core's events
    char *description;
    struct ECSISubscription **subscribers; // stb_ds array of its subscriptions, in the order they were made, so an emission reads only these
} ECSINamedEvent;

struct ECSISubscription
{
    ECSPlugin plugin;
    char *name;
    ECSEventFunction Function;
    ECSTimerFunction Release; // nullptr if the data needs no release
    void *data;
    bool cancelled; // freed when no event is being delivered
};

/// @brief A named event waiting in the queue to be delivered to its subscribers.
typedef struct ECSIEmission
{
    ECSValue *value;
    char name[]; // allocated with the emission
} ECSIEmission;

/// @brief The core's named events.
static const char *const OPENECS_CORE_EVENTS[][2] = {
    {"ecs.panelOpened", "A panel was opened: { panel = id, type = name }"},
    {"ecs.panelClosed", "A panel was closed: { panel = id, type = name }"},
    {"ecs.focusChanged", "Another panel got the focus: { panel = id }, or {} when no panel has it"},
    {"ecs.workspaceSwitched", "Another workspace is shown: { workspace = number }"},
    {"ecs.layoutChanged", "Panels were moved, grouped, closed, maximized or locked: {}"},
    {"ecs.settingChanged", "A setting's value in effect changed: { name = full name }"},
    {"ecs.pluginsChanged", "Plugins were enabled or disabled: {}"},
    {"ecs.logged", "A line was logged, by the core, a plugin or SDL, on any thread: { time = \"12:30:05.123\", level = \"debug\"|\"info\"|\"warn\"|\"error\", plugin = name or \"ecs\", message = text }; a line logged while a handler of it runs is not emitted"},
    {"ecs.prefix", "The core waits for the key after the prefix: { waiting = true, panel = id of the focused panel, keys = { { key = \"M\", function = name, text = what it does now }, ... } }; then { waiting = false }"},
};

static struct
{
    ECSIQueuedEvent *queue; // stb_ds array
    ECSIQueuedEvent *later; // stb_ds array of the named events emitted while events are delivered, which the next delivery takes
    bool delivering;
    struct
    {
        char *key;
        ECSINamedEvent value;
    } *named;                                // stb_ds hash map of the declared named events, with copied keys
    struct ECSISubscription **subscriptions; // stb_ds array; handles point to them, so each is allocated on its own
    ECSValue *const *watched[2];             // where modules keep values outside the queue (ECSIEvents_WatchValue)
    usz watchedCount;
    ECSIEventHeardFunction Heard; // hears each named event after its subscribers, or nullptr
} EVENTS = {0};

/// @brief The declared event of a name, or nullptr; its record moves when events are declared or removed, so it is found again after any of those.
static ECSINamedEvent *ECSIEvents_FindNamed(const char *name)
{
    ptrdiff_t index = EVENTS.named == nullptr ? -1 : shgeti(EVENTS.named, name);
    return index < 0 ? nullptr : &EVENTS.named[index].value;
}

static void ECSIEvents_FreeSubscription(struct ECSISubscription *subscription)
{
    if (subscription->Release != nullptr)
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
            // its event, if it is still declared, lists it too
            ECSINamedEvent *named = ECSIEvents_FindNamed(subscription->name);

            for (usz j = 0; named != nullptr && j < arrlenu(named->subscribers); j++)
            {
                if (named->subscribers[j] == subscription)
                {
                    arrdel(named->subscribers, j);
                    break;
                }
            }

            arrdel(EVENTS.subscriptions, i);
            ECSIEvents_FreeSubscription(subscription);
        }
        else
        {
            i++;
        }
    }
}

/// @brief Frees a named event that waited in the queue.
static void ECSIEvents_FreeEmission(void *data)
{
    ECSIEmission *emission = data;
    ECSValue_Destroy(&emission->value);
    SDL_free(emission);
}

/// @brief Delivers a named event to its subscribers, then frees it.
static void ECSIEvents_DeliverEmission(void *target, const ECSPanelEvent *event)
{
    (void)event;
    ECSIEmission *emission = target;

    // a subscription made while the event is delivered is added at the end, and hears the next emission; none is removed meanwhile.
    // A subscriber may declare events, which moves the event's record, so it is found again after each one
    ECSINamedEvent *named = ECSIEvents_FindNamed(emission->name);
    usz count = named == nullptr ? 0 : arrlenu(named->subscribers);

    for (usz i = 0; i < count && (named = ECSIEvents_FindNamed(emission->name)) != nullptr; i++)
    {
        struct ECSISubscription *subscription = named->subscribers[i];

        if (!subscription->cancelled)
        {
            // what a handler of ecs.logged logs is written, but not emitted again, so it cannot loop
            bool logged = SDL_strcmp(emission->name, "ecs.logged") == 0;
            ECSILog_SetQuiet(logged);
            ECSICallback callback;
            ECSIPlugin_Enter(&callback, subscription->plugin, nullptr, ECSICallbackType_Plugin);
            subscription->Function(subscription->data, emission->name, emission->value);
            (void)ECSIPlugin_Leave(&callback);
            ECSILog_SetQuiet(false);
        }
    }

    // an event that a subscriber's plugin removed while it was delivered is heard no more
    named = ECSIEvents_FindNamed(emission->name);

    if (EVENTS.Heard != nullptr && named != nullptr)
    {
        EVENTS.Heard(emission->name, named->plugin);
    }

    ECSIEvents_FreeEmission(emission);
}

/// @brief Declares a named event; plugin is nullptr for the core's.
static SHUResult ECSIEvents_Declare(ECSPlugin plugin, const char *name, const char *description)
{
    if (EVENTS.named == nullptr)
    {
        sh_new_strdup(EVENTS.named);
    }

    if (shgeti(EVENTS.named, name) >= 0)
    {
        (void)ECS_Check(nullptr, false, "Event '%s' is already declared.", name);
        return SHUResult_ErrAssertion;
    }

    ECSINamedEvent named = {.plugin = plugin, .description = SDL_strdup(description)};

    if (named.description == nullptr)
    {
        return SHUResult_ErrAllocation;
    }

    shput(EVENTS.named, name, named);
    return SHUResult_Ok;
}

/// @brief Queues a named event for its subscribers.
static SHUResult ECSIEvents_Emit(const char *name, const ECSValue *value)
{
    usz length = SDL_strlen(name);
    ECSIEmission *emission = SDL_calloc(1, sizeof(ECSIEmission) + length + 1);

    if (emission == nullptr || ECSValue_Create(&emission->value) || (value != nullptr && ECSValue_Copy(emission->value, value)))
    {
        if (emission != nullptr)
        {
            ECSValue_Destroy(&emission->value);
            SDL_free(emission);
        }

        return SHUResult_ErrAllocation;
    }

    SDL_memcpy(emission->name, name, length + 1);

    // an event emitted while events are delivered waits for the next delivery, so a subscriber that emits each time it hears one cannot hold the main loop
    ECSIQueuedEvent queued = {.Deliver = ECSIEvents_DeliverEmission, .Release = ECSIEvents_FreeEmission, .target = emission, .value = emission->value};

    if (EVENTS.delivering)
    {
        arrput(EVENTS.later, queued);
    }
    else
    {
        arrput(EVENTS.queue, queued);
    }

    return SHUResult_Ok;
}

#pragma endregion Source Only

SHUResult ECSIEvents_Initialize(void)
{
    ECSITimers_SetRunner(ECSIEvents_RunPluginTimer);
    for (usz i = 0; i < SDL_arraysize(OPENECS_CORE_EVENTS); i++)
    {
        SHU_ReturnResult(ECSIEvents_Declare(nullptr, OPENECS_CORE_EVENTS[i][0], OPENECS_CORE_EVENTS[i][1]));
    }

    return SHUResult_Ok;
}

void ECSIEvents_EmitCore(const char *name, const ECSValue *value)
{
    SDL_assert(name != nullptr);
    SDL_assert(shgeti(EVENTS.named, name) >= 0 && EVENTS.named[shgeti(EVENTS.named, name)].value.plugin == nullptr);

    if (ECSIEvents_Emit(name, value))
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Out of memory while emitting '%s'.", name);
    }
}

SHUResult ECSIEvents_Subscribe(ECSPlugin plugin, ECSSubscription *retSubscription, const char *name, ECSEventFunction function, ECSTimerFunction release, void *data)
{
    SDL_assert(plugin != nullptr);
    SDL_assert(name != nullptr && retSubscription != nullptr && function != nullptr);

    *retSubscription = nullptr;
    ptrdiff_t index = EVENTS.named == nullptr ? -1 : shgeti(EVENTS.named, name);

    if (index < 0)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' subscribes to '%s', which no plugin declared.", ECSIPlugin_GetName(plugin), name);
        return SHUResult_ErrNotFound;
    }

    // a plugin hears only the core, itself, and the plugins it depends on
    ECSPlugin owner = EVENTS.named[index].value.plugin;

    if (owner != nullptr && !ECSIPlugin_DependsOn(plugin, owner))
    {
        (void)ECS_Check(nullptr, false, "Plugin '%s' subscribes to '%s', but its manifest does not depend on '%s'.", ECSIPlugin_GetName(plugin), name, ECSIPlugin_GetName(owner));
        return SHUResult_ErrAssertion;
    }

    struct ECSISubscription *subscription = SDL_calloc(1, sizeof(struct ECSISubscription));

    if (subscription == nullptr || (subscription->name = SDL_strdup(name)) == nullptr)
    {
        SDL_free(subscription);
        return SHUResult_ErrAllocation;
    }

    subscription->plugin = plugin;
    subscription->Function = function;
    subscription->Release = release;
    subscription->data = data;
    arrput(EVENTS.subscriptions, subscription);
    arrput(EVENTS.named[index].value.subscribers, subscription);
    *retSubscription = subscription;
    return SHUResult_Ok;
}

void ECSIEvents_SetHeard(ECSIEventHeardFunction Heard)
{
    EVENTS.Heard = Heard;
}

bool ECSIEvents_FindOwner(const char *name, ECSPlugin *retOwner)
{
    SDL_assert(name != nullptr && retOwner != nullptr);

    ECSINamedEvent *named = ECSIEvents_FindNamed(name);
    *retOwner = named == nullptr ? nullptr : named->plugin;
    return named != nullptr;
}

void ECSIEvents_RemovePlugin(ECSPlugin plugin)
{
    SDL_assert(plugin != nullptr);

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
            arrfree(EVENTS.named[i - 1].value.subscribers);
            (void)shdel(EVENTS.named, EVENTS.named[i - 1].key);
        }
    }

    ECSIEvents_FreeCancelled();
}

/// @brief Makes the events of a queue that wait to be delivered give no object where their values name one.
static void ECSIEvents_ForgetObjectIn(ECSIQueuedEvent *queue, const void *object)
{
    for (usz i = 0; i < arrlenu(queue); i++)
    {
        if (queue[i].value != nullptr)
        {
            ECSIValue_ForgetObject(queue[i].value, object);
        }
    }
}

void ECSIEvents_ForgetObject(const void *object)
{
    SDL_assert(object != nullptr);

    ECSIEvents_ForgetObjectIn(EVENTS.queue, object);
    ECSIEvents_ForgetObjectIn(EVENTS.later, object);

    for (usz i = 0; i < EVENTS.watchedCount; i++)
    {
        if (*EVENTS.watched[i] != nullptr)
        {
            ECSIValue_ForgetObject(*EVENTS.watched[i], object);
        }
    }
}

void ECSIEvents_WatchValue(ECSValue *const *value)
{
    SDL_assert(value != nullptr);
    SDL_assert(EVENTS.watchedCount < SDL_arraysize(EVENTS.watched));

    EVENTS.watched[EVENTS.watchedCount++] = value;
}

SHUResult ECSEvent_Declare(ECSPlugin plugin, const char *name, const char *description)
{
    ECSILog_Check(SDL_IsMainThread(), SHUResult_ErrAssertion);
    ECSILog_Check(plugin != nullptr, SHUResult_ErrAssertion);
    ECSILog_Check(name != nullptr && description != nullptr, SHUResult_ErrAssertion);

    char *fullName = nullptr;
    SHUResult result = ECSIPlugin_MakeFullName(plugin, name, &fullName);
    result = result ? result : ECSIEvents_Declare(plugin, fullName, description);
    SDL_free(fullName);
    return result;
}

SHUResult ECSEvent_Emit(ECSPlugin plugin, const char *name, const ECSValue *value)
{
    ECSILog_Check(SDL_IsMainThread(), SHUResult_ErrAssertion);
    ECSILog_Check(plugin != nullptr, SHUResult_ErrAssertion);
    ECSILog_Check(name != nullptr, SHUResult_ErrAssertion);

    ptrdiff_t index = EVENTS.named == nullptr ? -1 : shgeti(EVENTS.named, name);

    if (index < 0 || EVENTS.named[index].value.plugin != plugin)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' emits '%s', which it did not declare.", ECSIPlugin_GetName(plugin), name);
        return SHUResult_ErrNotFound;
    }

    return ECSIEvents_Emit(name, value);
}

SHUResult ECSEvent_Subscribe(ECSPlugin plugin, ECSSubscription *retSubscription, const char *name, ECSEventFunction function, void *data)
{
    ECSILog_Check(SDL_IsMainThread(), SHUResult_ErrAssertion);
    return ECSIEvents_Subscribe(plugin, retSubscription, name, function, nullptr, data);
}

void ECSEvent_Unsubscribe(ECSSubscription *subscription)
{
    ECSILog_Check(SDL_IsMainThread());
    ECSILog_Check(subscription != nullptr);

    if (*subscription != nullptr)
    {
        (*subscription)->cancelled = true;
        *subscription = nullptr;
        ECSIEvents_FreeCancelled();
    }
}

void ECSIEvents_Post(ECSIEventDeliverFunction deliver, ECSTimerFunction release, void *target, ECSValue *value, const ECSPanelEvent *event)
{
    SDL_assert(deliver != nullptr);
    SDL_assert(event != nullptr);

    ECSIQueuedEvent queued = {.Deliver = deliver, .Release = release, .target = target, .value = value, .event = *event};
    arrput(EVENTS.queue, queued);
}

/// @brief Emits the lines of the log that wait as ecs.logged, each in its own emission, and one line for those that were left out.
static void ECSIEvents_EmitLog(void)
{
    if (!ECSILog_IsWaiting())
    {
        return;
    }

    usz dropped = 0;
    ECSILogLine *lines = ECSILog_Take(&dropped);
    char left[96];

    if (dropped > 0)
    {
        SDL_Time now = 0;
        SDL_DateTime time = {0};

        if (SDL_GetCurrentTime(&now))
        {
            SDL_TimeToDateTime(now, &time, true);
        }

        ECSILogLine line = {.level = "warn", .plugin = "ecs", .message = left};
        SDL_snprintf(line.time, sizeof(line.time), "%02d:%02d:%02d.%03d", time.hour, time.minute, time.second, time.nanosecond / 1000000);
        SDL_snprintf(left, sizeof(left), "%zu lines of the log are left out here; the log file holds them.", (size_t)dropped);
        arrput(lines, line);
    }

    for (usz i = 0; i < arrlenu(lines); i++)
    {
        ECSValue *value = nullptr;
        ECSValue *field = nullptr;
        bool made = ECSValue_Create(&value) == SHUResult_Ok;
        const char *names[] = {"time", "level", "plugin", "message"};
        const char *texts[] = {lines[i].time, lines[i].level, lines[i].plugin, lines[i].message};

        for (usz j = 0; made && j < SDL_arraysize(names); j++)
        {
            made = ECSValue_SetTableField(value, names[j], &field) == SHUResult_Ok && ECSValue_SetString(field, texts[j]) == SHUResult_Ok;
        }

        if (made)
        {
            ECSIEvents_EmitCore("ecs.logged", value);
        }

        ECSValue_Destroy(&value);
    }

    // the line of those left out is not the queue's to free
    if (dropped > 0)
    {
        arrpop(lines);
    }

    ECSILog_FreeLines(lines);
}

void ECSIEvents_Deliver(void)
{
    SDL_assert(!EVENTS.delivering); // events are never delivered inside another event handler

    ECSIEvents_EmitLog();
    EVENTS.delivering = true;

    for (usz i = 0; i < arrlenu(EVENTS.queue); i++)
    {
        // a copy, because delivering may queue more events and move the queue
        ECSIQueuedEvent queued = EVENTS.queue[i];
        queued.Deliver(queued.target, &queued.event);

        // the delivery freed the value, so a handle freed by a later event's handler does not look into it
        EVENTS.queue[i].value = nullptr;
    }

    arrfree(EVENTS.queue);
    EVENTS.queue = EVENTS.later;
    EVENTS.later = nullptr;
    EVENTS.delivering = false;
    ECSIEvents_FreeCancelled();
}

i32 ECSIEvents_GetWait(void)
{
    return arrlenu(EVENTS.queue) > 0 || ECSILog_IsWaiting() ? 0 : ECSITimers_GetWait();
}

void ECSIEvents_Terminate(void)
{
    SDL_assert(!EVENTS.delivering);

    // the events never delivered free what they carry
    for (usz i = 0; i < arrlenu(EVENTS.queue); i++)
    {
        if (EVENTS.queue[i].Release != nullptr)
        {
            EVENTS.queue[i].Release(EVENTS.queue[i].target);
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
        arrfree(EVENTS.named[i].value.subscribers);
    }

    shfree(EVENTS.named);
    SDL_zero(EVENTS);
}

/// @brief Runs a plugin's timer inside its callback.
static void ECSIEvents_RunPluginTimer(void *owner, ECSTimerFunction function, void *data)
{
    ECSICallback callback;
    ECSIPlugin_Enter(&callback, owner, nullptr, ECSICallbackType_Plugin);
    function(data);
    (void)ECSIPlugin_Leave(&callback);
}

SHUResult ECSIEvents_StartTimer(ECSPlugin plugin, ECSTimer *retTimer, f64 seconds, bool repeat, ECSTimerFunction function, ECSTimerFunction release, void *data)
{
    SDL_assert(plugin != nullptr);
    SDL_assert(retTimer != nullptr);
    SDL_assert(function != nullptr);

    // a plugin's timers stop with it, and run inside its callback
    return ECSITimers_Start(retTimer, seconds, repeat, function, release, data, plugin);
}

void ECSIEvents_StopTimersOfPlugin(ECSPlugin plugin)
{
    SDL_assert(plugin != nullptr);

    ECSITimers_StopOwnedBy(plugin);
}

SHUResult ECSTimer_Start(ECSPlugin plugin, ECSTimer *retTimer, f64 seconds, bool repeat, ECSTimerFunction function, void *data)
{
    return ECSIEvents_StartTimer(plugin, retTimer, seconds, repeat, function, nullptr, data);
}

void ECSTimer_Stop(ECSTimer *timer)
{
    ECSILog_Check(timer != nullptr);

    // the handle is cleared first, as stopping may free the memory that holds it, such as a Lua timer's; a timer that ended stops nothing
    ECSTimer stopped = *timer;
    *timer = nullptr;
    ECSITimers_Stop(stopped);
}
