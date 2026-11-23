#include "base/Timers.h"

#include "SDL3/SDL.h"
#include "stb/stbSDL3.h"

#pragma region Source Only

/// @brief A slot of the timers' table. A handle names its slot and its generation, which grows when the timer stops, so a handle of a timer
/// that ended names nothing.
typedef struct ECSITimer
{
    u64 dueNanoseconds; // as SDL_GetTicksNS
    u64 intervalNanoseconds;
    u32 generation;
    bool used;    // the slot holds a timer, running or stopped
    bool stopped; // its release runs, and the slot is free again, once no timer runs
    bool repeat;
    void *owner; // what the runner gets, or nullptr
    ECSTimerFunction Function;
    ECSTimerFunction Release; // nullptr if the data needs no release
    void *data;
} ECSITimer;

// a handle holds the slot's index and its generation in one pointer-sized number
static_assert(sizeof(uintptr_t) >= sizeof(u64), "a timer's handle holds a 32-bit index and a 32-bit generation");

static struct
{
    ECSITimer *slots; // stb_ds array
    u32 *free;        // stb_ds array of the indices of free slots
    ECSITimerRunFunction Run;
    bool running;
    bool freeing;
} TIMERS = {0};

static ECSTimer ECSITimers_Handle(usz index)
{
    return (ECSTimer)(uintptr_t)(((u64)TIMERS.slots[index].generation << 32U) | (u64)(index + 1));
}

/// @brief Finds the running timer that a handle names.
/// @return Its slot's index, or -1 if it stopped or ended.
static i64 ECSITimers_Find(ECSTimer timer)
{
    u64 value = (u64)(uintptr_t)timer;
    u64 index = (value & 0xFFFFFFFFU) - 1;

    if (value == 0 || index >= arrlenu(TIMERS.slots))
    {
        return -1;
    }

    const ECSITimer *slot = &TIMERS.slots[index];
    return slot->used && !slot->stopped && slot->generation == (u32)(value >> 32U) ? (i64)index : -1;
}

/// @brief Stops a timer's slot: its handle names nothing from now.
static void ECSITimers_StopSlot(usz index)
{
    TIMERS.slots[index].stopped = true;
    TIMERS.slots[index].generation++;
}

/// @brief Frees the stopped timers, unless timers are running and may still be read.
static void ECSITimers_FreeStopped(void)
{
    if (TIMERS.running || TIMERS.freeing)
    {
        return;
    }

    // a release may stop more timers, or start some, so the table is walked until nothing is freed
    TIMERS.freeing = true;
    bool freed = true;

    while (freed)
    {
        freed = false;

        for (usz i = 0; i < arrlenu(TIMERS.slots); i++)
        {
            if (!TIMERS.slots[i].used || !TIMERS.slots[i].stopped)
            {
                continue;
            }

            ECSTimerFunction Release = TIMERS.slots[i].Release;
            void *data = TIMERS.slots[i].data;
            TIMERS.slots[i].used = false;
            arrput(TIMERS.free, (u32)i);
            freed = true;

            if (Release != nullptr)
            {
                Release(data);
            }
        }
    }

    TIMERS.freeing = false;
}

#pragma endregion Source Only

void ECSITimers_SetRunner(ECSITimerRunFunction run)
{
    TIMERS.Run = run;
}

SHUResult ECSITimers_Start(ECSTimer *retTimer, f64 seconds, bool repeat, ECSTimerFunction function, ECSTimerFunction release, void *data, void *owner)
{
    SDL_assert(retTimer != nullptr);
    SDL_assert(function != nullptr);
    SDL_assert(seconds > 0.0 || (seconds == 0.0 && !repeat));

    usz index = arrlenu(TIMERS.slots);

    if (arrlenu(TIMERS.free) > 0)
    {
        index = arrpop(TIMERS.free);
    }
    else
    {
        ECSITimer empty = {.generation = 1};
        arrput(TIMERS.slots, empty);
    }

    u64 intervalNanoseconds = (u64)(seconds * (f64)SDL_NS_PER_SECOND);
    ECSITimer *timer = &TIMERS.slots[index];

    *timer = (ECSITimer){
        .dueNanoseconds = SDL_GetTicksNS() + intervalNanoseconds,
        .intervalNanoseconds = intervalNanoseconds,
        .generation = timer->generation,
        .used = true,
        .repeat = repeat,
        .owner = owner,
        .Function = function,
        .Release = release,
        .data = data,
    };

    *retTimer = ECSITimers_Handle(index);
    return SHUResult_Ok;
}

void ECSITimers_Stop(ECSTimer timer)
{
    i64 index = ECSITimers_Find(timer);

    if (index >= 0)
    {
        ECSITimers_StopSlot((usz)index);
        ECSITimers_FreeStopped();
    }
}

void ECSITimers_StopOwnedBy(void *owner)
{
    SDL_assert(owner != nullptr);

    for (usz i = 0; i < arrlenu(TIMERS.slots); i++)
    {
        if (TIMERS.slots[i].used && !TIMERS.slots[i].stopped && TIMERS.slots[i].owner == owner)
        {
            ECSITimers_StopSlot(i);
        }
    }

    ECSITimers_FreeStopped();
}

i32 ECSITimers_GetWait(void)
{
    u64 now = SDL_GetTicksNS();
    u64 wait = UINT64_MAX;

    for (usz i = 0; i < arrlenu(TIMERS.slots); i++)
    {
        const ECSITimer *timer = &TIMERS.slots[i];

        if (timer->used && !timer->stopped)
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

    // timers started by the functions wait for the next call; the table may grow during a call, so a slot is read again by its index
    usz count = arrlenu(TIMERS.slots);

    for (usz i = 0; i < count; i++)
    {
        ECSITimer *timer = &TIMERS.slots[i];

        if (!timer->used || timer->stopped || timer->dueNanoseconds > now)
        {
            continue;
        }

        // a one-shot timer ends before its function runs, so its handle names nothing from then
        if (!timer->repeat)
        {
            ECSITimers_StopSlot(i);
        }
        else
        {
            // calls missed while the program was busy are skipped
            timer->dueNanoseconds += timer->intervalNanoseconds;
            timer->dueNanoseconds = timer->dueNanoseconds <= now ? now + timer->intervalNanoseconds : timer->dueNanoseconds;
        }

        void *owner = timer->owner;
        ECSTimerFunction Function = timer->Function;
        void *data = timer->data;

        if (owner != nullptr && TIMERS.Run != nullptr)
        {
            TIMERS.Run(owner, Function, data);
        }
        else
        {
            Function(data);
        }
    }

    TIMERS.running = false;
    ECSITimers_FreeStopped();
}

void ECSITimers_Terminate(void)
{
    SDL_assert(!TIMERS.running);

    // a release that stops another timer frees nothing here; every timer is released in this loop
    TIMERS.freeing = true;

    for (usz i = 0; i < arrlenu(TIMERS.slots); i++)
    {
        if (TIMERS.slots[i].used && TIMERS.slots[i].Release != nullptr)
        {
            TIMERS.slots[i].Release(TIMERS.slots[i].data);
        }
    }

    arrfree(TIMERS.slots);
    arrfree(TIMERS.free);
    SDL_zero(TIMERS);
}
