#include "base/Profile.h"

#include "base/Log.h"

#include "SDL3/SDL.h"
#include "stb/stbSDL3.h"

#pragma region Source Only

#define OPENECS_PROFILE_PASSES 240 // the passes of the loop that the profile keeps, a few seconds of frames

static const char *const OPENECS_PROFILE_STEPS[ECSIProfileStep_Count] = {"wait", "input", "timers", "events", "layout", "panels", "upload", "interface", "present"};

/// @brief What a plugin or a panel took in a second.
typedef struct ECSIProfileTime
{
    char *name;  // the plugin's name, or the panel's type
    u64 ticks;   // performance counter ticks in all
    u64 longest; // the longest single call
    u32 calls;
} ECSIProfileTime;

typedef struct
{
    ECSPlugin key;
    ECSIProfileTime value;
} ECSIProfilePluginTimes;

typedef struct
{
    u32 key;
    ECSIProfileTime value;
} ECSIProfilePanelTimes;

static struct
{
    bool on;
    f64 frequency; // performance counter ticks per second

    ECSIProfileStep step; // the step the pass is in
    u64 stepStart;
    u64 steps[ECSIProfileStep_Count]; // this pass's ticks in each step

    u64 passes[OPENECS_PROFILE_PASSES][ECSIProfileStep_Count]; // a ring of the last passes' steps
    usz nextPass;
    usz passCount;

    ECSIProfileScope *scope; // the innermost plugin scope on the main thread

    u64 secondStart;
    ECSIProfilePluginTimes *plugins; // stb_ds hash maps of this second
    ECSIProfilePanelTimes *panels;
    ECSIProfilePluginTimes *lastPlugins; // and of the last whole second
    ECSIProfilePanelTimes *lastPanels;
    f64 lastSeconds; // how long that second was, in seconds
} PROFILE = {0};

static void ECSIProfile_FreePlugins(ECSIProfilePluginTimes **times)
{
    for (usz i = 0; i < hmlenu(*times); i++)
    {
        SDL_free((*times)[i].value.name);
    }

    hmfree(*times);
}

static void ECSIProfile_FreePanels(ECSIProfilePanelTimes **times)
{
    for (usz i = 0; i < hmlenu(*times); i++)
    {
        SDL_free((*times)[i].value.name);
    }

    hmfree(*times);
}

static void ECSIProfile_Clear(void)
{
    ECSIProfile_FreePlugins(&PROFILE.plugins);
    ECSIProfile_FreePlugins(&PROFILE.lastPlugins);
    ECSIProfile_FreePanels(&PROFILE.panels);
    ECSIProfile_FreePanels(&PROFILE.lastPanels);
    PROFILE.nextPass = 0;
    PROFILE.passCount = 0;
    PROFILE.lastSeconds = 0.0;
    SDL_zeroa(PROFILE.steps);
}

/// @brief Adds a call's ticks to a time, and names it the first time.
static void ECSIProfile_Add(ECSIProfileTime *time, const char *name, u64 ticks)
{
    if (time->name == nullptr)
    {
        time->name = SDL_strdup(name);
    }

    time->ticks += ticks;
    time->longest = SDL_max(time->longest, ticks);
    time->calls++;
}

/// @brief Fills a table's fields with a time, in seconds per second of the window it was measured in.
static SHUResult ECSIProfile_SetTime(ECSValue *item, const ECSIProfileTime *time)
{
    ECSValue *field = nullptr;
    SHU_ReturnResult(ECSValue_SetTableField(item, "seconds", &field));
    ECSValue_SetNumber(field, (f64)time->ticks / PROFILE.frequency / SDL_max(PROFILE.lastSeconds, 1e-9));
    SHU_ReturnResult(ECSValue_SetTableField(item, "longest", &field));
    ECSValue_SetNumber(field, (f64)time->longest / PROFILE.frequency);
    SHU_ReturnResult(ECSValue_SetTableField(item, "calls", &field));
    ECSValue_SetInteger(field, time->calls);
    return SHUResult_Ok;
}

/// @brief Orders times by their ticks, the most first.
static int ECSIProfile_CompareTimes(const void *first, const void *second)
{
    u64 firstTicks = (*(const ECSIProfileTime *const *)first)->ticks;
    u64 secondTicks = (*(const ECSIProfileTime *const *)second)->ticks;
    return firstTicks < secondTicks ? 1 : firstTicks > secondTicks ? -1
                                                                   : 0;
}

/// @brief Lists the last second's times, the most first: each a table of its name in nameField, its seconds, longest and calls, and a panel's id.
static SHUResult ECSIProfile_ListTimes(ECSValue *list, ECSIProfileTime **times, usz count, const char *nameField, bool panels)
{
    ECSValue_SetTable(list);
    SDL_qsort((void *)times, count, sizeof(ECSIProfileTime *), ECSIProfile_CompareTimes);

    for (usz i = 0; i < count; i++)
    {
        ECSValue *item = nullptr;
        ECSValue *field = nullptr;
        SHU_ReturnResult(ECSValue_AddListItem(list, &item));
        SHU_ReturnResult(ECSValue_SetTableField(item, nameField, &field));
        SHU_ReturnResult(ECSValue_SetString(field, times[i]->name != nullptr ? times[i]->name : ""));
        SHU_ReturnResult(ECSIProfile_SetTime(item, times[i]));

        if (panels)
        {
            // the panel's id is found from its time, which the map keeps beside it
            const ECSIProfilePanelTimes *entry = (const ECSIProfilePanelTimes *)((const u8 *)times[i] - offsetof(ECSIProfilePanelTimes, value));
            SHU_ReturnResult(ECSValue_SetTableField(item, "id", &field));
            ECSValue_SetInteger(field, entry->key);
        }
    }

    return SHUResult_Ok;
}

static SHUResult ECSIProfile_ListPasses(ECSValue *list)
{
    ECSValue_SetTable(list);

    // the ring's oldest pass first
    for (usz i = 0; i < PROFILE.passCount; i++)
    {
        const u64 *steps = PROFILE.passes[(PROFILE.nextPass + OPENECS_PROFILE_PASSES - PROFILE.passCount + i) % OPENECS_PROFILE_PASSES];
        ECSValue *item = nullptr;
        ECSValue *field = nullptr;
        SHU_ReturnResult(ECSValue_AddListItem(list, &item));
        ECSValue_SetTable(item);

        for (usz j = 0; j < ECSIProfileStep_Count; j++)
        {
            SHU_ReturnResult(ECSValue_SetTableField(item, OPENECS_PROFILE_STEPS[j], &field));
            ECSValue_SetNumber(field, (f64)steps[j] / PROFILE.frequency);
        }
    }

    return SHUResult_Ok;
}

static SHUResult ECSIProfile_Fill(ECSValue *profile)
{
    ECSValue_SetTable(profile);
    ECSValue *field = nullptr;
    SHU_ReturnResult(ECSValue_SetTableField(profile, "passes", &field));
    SHU_ReturnResult(ECSIProfile_ListPasses(field));

    usz pluginCount = hmlenu(PROFILE.lastPlugins);
    usz panelCount = hmlenu(PROFILE.lastPanels);
    ECSIProfileTime **times = SDL_malloc(sizeof(ECSIProfileTime *) * (SDL_max(pluginCount, panelCount) + 1));

    if (times == nullptr)
    {
        return SHUResult_ErrAllocation;
    }

    for (usz i = 0; i < pluginCount; i++)
    {
        times[i] = &PROFILE.lastPlugins[i].value;
    }

    SHUResult result = ECSValue_SetTableField(profile, "plugins", &field);
    result = result ? result : ECSIProfile_ListTimes(field, times, pluginCount, "name", false);

    for (usz i = 0; i < panelCount; i++)
    {
        times[i] = &PROFILE.lastPanels[i].value;
    }

    result = result ? result : ECSValue_SetTableField(profile, "panels", &field);
    result = result ? result : ECSIProfile_ListTimes(field, times, panelCount, "type", true);
    SDL_free(times);
    return result;
}

#pragma endregion Source Only

void ECSIProfile_SetOn(bool measure)
{
    if (measure == PROFILE.on)
    {
        return;
    }

    ECSIProfile_Clear();
    PROFILE.on = measure;
    PROFILE.frequency = (f64)SDL_GetPerformanceFrequency();
    PROFILE.stepStart = SDL_GetPerformanceCounter();
    PROFILE.secondStart = PROFILE.stepStart;
}

bool ECSIProfile_IsOn(void)
{
    return PROFILE.on;
}

ECSIProfileStep ECSIProfile_SetStep(ECSIProfileStep step)
{
    ECSIProfileStep left = PROFILE.step;

    if (PROFILE.on)
    {
        u64 now = SDL_GetPerformanceCounter();
        PROFILE.steps[left] += now - PROFILE.stepStart;
        PROFILE.stepStart = now;
    }

    PROFILE.step = step;
    return left;
}

void ECSIProfile_Enter(ECSIProfileScope *scope, ECSPlugin plugin, const char *name)
{
    SDL_assert(scope != nullptr);

    // the time of other threads runs beside the main thread's, so it is no part of a frame
    scope->active = PROFILE.on && SDL_IsMainThread();

    if (!scope->active)
    {
        return;
    }

    scope->plugin = plugin;
    scope->name = name;
    scope->inner = 0;
    scope->outer = PROFILE.scope;
    PROFILE.scope = scope;
    scope->start = SDL_GetPerformanceCounter();
}

void ECSIProfile_Leave(ECSIProfileScope *scope)
{
    SDL_assert(scope != nullptr);

    if (!scope->active)
    {
        return;
    }

    u64 elapsed = SDL_GetPerformanceCounter() - scope->start;
    PROFILE.scope = scope->outer;

    if (scope->outer != nullptr)
    {
        scope->outer->inner += elapsed;
    }

    // measuring may have stopped inside the scope, which dropped the measures
    if (!PROFILE.on)
    {
        return;
    }

    ptrdiff_t index = hmgeti(PROFILE.plugins, scope->plugin);

    if (index < 0)
    {
        ECSIProfileTime time = {0};
        hmput(PROFILE.plugins, scope->plugin, time);
        index = hmgeti(PROFILE.plugins, scope->plugin);
    }

    ECSIProfile_Add(&PROFILE.plugins[index].value, scope->name != nullptr ? scope->name : "ecs", elapsed - SDL_min(scope->inner, elapsed));
}

void ECSIProfile_ForgetPlugin(ECSPlugin plugin)
{
    ptrdiff_t index = hmgeti(PROFILE.plugins, plugin);

    if (index >= 0)
    {
        SDL_free(PROFILE.plugins[index].value.name);
        (void)hmdel(PROFILE.plugins, plugin);
    }

    // the last second's times keep their names, and their handles are only keys
}

u64 ECSIProfile_BeginDraw(void)
{
    return PROFILE.on ? SDL_GetPerformanceCounter() : 0;
}

void ECSIProfile_EndDraw(u64 start, u32 id, const char *type)
{
    if (!PROFILE.on || start == 0)
    {
        return;
    }

    u64 elapsed = SDL_GetPerformanceCounter() - start;
    ptrdiff_t index = hmgeti(PROFILE.panels, id);

    if (index < 0)
    {
        ECSIProfileTime time = {0};
        hmput(PROFILE.panels, id, time);
        index = hmgeti(PROFILE.panels, id);
    }

    ECSIProfile_Add(&PROFILE.panels[index].value, type != nullptr ? type : "", elapsed);
}

void ECSIProfile_EndPass(void)
{
    if (!PROFILE.on)
    {
        return;
    }

    (void)ECSIProfile_SetStep(PROFILE.step);
    SDL_memcpy(PROFILE.passes[PROFILE.nextPass], PROFILE.steps, sizeof(PROFILE.steps));
    PROFILE.nextPass = (PROFILE.nextPass + 1) % OPENECS_PROFILE_PASSES;
    PROFILE.passCount = SDL_min(PROFILE.passCount + 1, OPENECS_PROFILE_PASSES);
    SDL_zeroa(PROFILE.steps);

    // once a second, this second's times become the last second's, which the profile gives
    u64 now = SDL_GetPerformanceCounter();
    f64 seconds = (f64)(now - PROFILE.secondStart) / PROFILE.frequency;

    if (seconds >= 1.0)
    {
        ECSIProfile_FreePlugins(&PROFILE.lastPlugins);
        ECSIProfile_FreePanels(&PROFILE.lastPanels);
        PROFILE.lastPlugins = PROFILE.plugins;
        PROFILE.lastPanels = PROFILE.panels;
        PROFILE.lastSeconds = seconds;
        PROFILE.plugins = nullptr;
        PROFILE.panels = nullptr;
        PROFILE.secondStart = now;
    }
}

void ECSIProfile_Terminate(void)
{
    ECSIProfile_Clear();
    PROFILE.on = false;
}

SHUResult ECSProfile_Get(ECSValue *retProfile)
{
    ECSILog_Check(retProfile != nullptr, SHUResult_ErrAssertion);
    ECSILog_Check(SDL_IsMainThread(), SHUResult_ErrAssertion);

    if (!PROFILE.on)
    {
        ECSValue_SetNil(retProfile);
        return SHUResult_Ok;
    }

    return ECSIProfile_Fill(retProfile);
}
