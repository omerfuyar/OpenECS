#include "base/Timers.h"

#include "SDL3/SDL.h"
#include "stb/stbSDL3.h"

#pragma region Source Only

typedef struct ECSITimer
{
    ECSPlugin plugin;
    u64 dueNanoseconds; // as SDL_GetTicksNS
    u64 intervalNanoseconds;
    bool repeat;
    bool stopped; // freed when no timer is running
    ECSTimerFunction Function;
    ECSTimerFunction Release; // nullptr if the data needs no release
    void *data;
} ECSITimer;

static struct
{
    ECSITimer **timers; // stb_ds array; handles point to the timers, so each is allocated on its own
    bool running;
    bool freeing;
} TIMERS = {0};

static void ECSITimers_Free(ECSITimer *timer)
{
    if (timer->Release != nullptr)
    {
        timer->Release(timer->data);
    }

    SDL_free(timer);
}

/// @brief Frees the stopped timers, unless timers are running and may still be read.
static void ECSITimers_FreeStopped(void)
{
    if (TIMERS.running || TIMERS.freeing)
    {
        return;
    }

    // a release may stop more timers, so the list is walked until nothing is freed
    TIMERS.freeing = true;
    bool freed = true;

    while (freed)
    {
        freed = false;

        for (usz i = 0; i < arrlenu(TIMERS.timers);)
        {
            ECSITimer *timer = TIMERS.timers[i];

            if (!timer->stopped)
            {
                i++;
                continue;
            }

            arrdel(TIMERS.timers, i);
            ECSITimers_Free(timer);
            freed = true;
        }
    }

    TIMERS.freeing = false;
}

#pragma endregion Source Only

SHUResult ECSITimers_Start(ECSPlugin plugin, ECSTimer *retTimer, f64 seconds, bool repeat, ECSTimerFunction function, ECSTimerFunction release, void *data)
{
    SDL_assert(plugin != nullptr);
    SDL_assert(retTimer != nullptr);
    SDL_assert(function != nullptr);
    SDL_assert(seconds > 0.0 || (seconds == 0.0 && !repeat));

    ECSITimer *timer = SDL_malloc(sizeof(ECSITimer));

    if (timer == nullptr)
    {
        return SHUResult_ErrAllocation;
    }

    u64 intervalNanoseconds = (u64)(seconds * (f64)SDL_NS_PER_SECOND);

    *timer = (ECSITimer){
        .plugin = plugin,
        .dueNanoseconds = SDL_GetTicksNS() + intervalNanoseconds,
        .intervalNanoseconds = intervalNanoseconds,
        .repeat = repeat,
        .Function = function,
        .Release = release,
        .data = data,
    };

    arrput(TIMERS.timers, timer);
    *retTimer = timer;
    return SHUResult_Ok;
}

void ECSITimers_StopPlugin(ECSPlugin plugin)
{
    SDL_assert(plugin != nullptr);

    for (usz i = 0; i < arrlenu(TIMERS.timers); i++)
    {
        if (TIMERS.timers[i]->plugin == plugin)
        {
            TIMERS.timers[i]->stopped = true;
        }
    }

    ECSITimers_FreeStopped();
}

i32 ECSITimers_GetWait(void)
{
    u64 now = SDL_GetTicksNS();
    u64 wait = UINT64_MAX;

    for (usz i = 0; i < arrlenu(TIMERS.timers); i++)
    {
        ECSITimer *timer = TIMERS.timers[i];

        if (!timer->stopped)
        {
            wait = SDL_min(wait, timer->dueNanoseconds > now ? timer->dueNanoseconds - now : 0);
        }
    }

    if (wait == UINT64_MAX)
    {
        return -1;
    }

    // rounded up, so the loop does not wake up just before the timer is due
    return (i32)SDL_min((wait + SDL_NS_PER_MS - 1) / SDL_NS_PER_MS, (u64)SDL_MAX_SINT32);
}

void ECSITimers_Run(void)
{
    u64 now = SDL_GetTicksNS();
    TIMERS.running = true;

    // timers started by the functions wait for the next call
    usz count = arrlenu(TIMERS.timers);

    for (usz i = 0; i < count; i++)
    {
        ECSITimer *timer = TIMERS.timers[i];

        if (timer->stopped || timer->dueNanoseconds > now)
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
            timer->dueNanoseconds += timer->intervalNanoseconds;
            timer->dueNanoseconds = timer->dueNanoseconds <= now ? now + timer->intervalNanoseconds : timer->dueNanoseconds;
        }

        timer->Function(timer->data);
    }

    TIMERS.running = false;
    ECSITimers_FreeStopped();
}

void ECSITimers_Terminate(void)
{
    SDL_assert(!TIMERS.running);

    // a release that stops another timer frees nothing here; every timer is freed in this loop
    TIMERS.freeing = true;

    for (usz i = 0; i < arrlenu(TIMERS.timers); i++)
    {
        ECSITimers_Free(TIMERS.timers[i]);
    }

    arrfree(TIMERS.timers);
    SDL_zero(TIMERS);
}

SHUResult ECSTimer_Start(ECSPlugin plugin, ECSTimer *retTimer, f64 seconds, bool repeat, ECSTimerFunction function, void *data)
{
    return ECSITimers_Start(plugin, retTimer, seconds, repeat, function, nullptr, data);
}

void ECSTimer_Stop(ECSTimer *timer)
{
    SDL_assert(timer != nullptr);

    if (*timer == nullptr)
    {
        return;
    }

    (*timer)->stopped = true;
    *timer = nullptr;
    ECSITimers_FreeStopped();
}
