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
    void *data;
} ECSI_Timer;

/// @brief An event waiting in the queue.
typedef struct ECSI_QueuedEvent
{
    ECSI_EventDeliverFunction Deliver;
    void *target;
    ECSEvent event;
} ECSI_QueuedEvent;

static struct
{
    ECSI_QueuedEvent *queue; // stb_ds array
    bool delivering;
    ECSI_Timer **timers; // stb_ds array; handles point to the timers, so each is allocated on its own
    bool runningTimers;
} EVENTS = {0};

/// @brief Frees the stopped timers, unless timers are running and may still be read.
static void ECSI_EventsFreeStoppedTimers(void)
{
    if (EVENTS.runningTimers)
    {
        return;
    }

    for (usz i = arrlenu(EVENTS.timers); i > 0; i--)
    {
        if (EVENTS.timers[i - 1]->stopped)
        {
            SDL_free(EVENTS.timers[i - 1]);
            arrdel(EVENTS.timers, i - 1);
        }
    }
}

#pragma endregion Source Only

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

SHUResult ECSI_EventsStartTimer(ECSPlugin plugin, const void *owner, ECSTimer *retTimer, f64 seconds, bool repeat, ECSTimerFunction function, void *data)
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

    for (usz i = 0; i < arrlenu(EVENTS.timers); i++)
    {
        SDL_free(EVENTS.timers[i]);
    }

    arrfree(EVENTS.timers);
    arrfree(EVENTS.queue);
    SDL_zero(EVENTS);
}

SHUResult ECSTimer_Start(ECSPlugin plugin, ECSTimer *retTimer, f64 seconds, bool repeat, ECSTimerFunction function, void *data)
{
    return ECSI_EventsStartTimer(plugin, NULL, retTimer, seconds, repeat, function, data);
}

void ECSTimer_Stop(ECSTimer *timer)
{
    SDL_assert(timer != NULL && *timer != NULL);

    (*timer)->stopped = true;
    *timer = NULL;
    ECSI_EventsFreeStoppedTimers();
}
