// Named counters, in C. Other plugins, in any language, call its functions by name and signature.
// A function registered by name is a service: keys, menus, presets and other plugins can run it.

#include "OpenECS.h"

#include <string.h>

#define COUNTER_MAX 16
#define COUNTER_NAME_SIZE 32

static struct
{
    char names[COUNTER_MAX][COUNTER_NAME_SIZE];
    i64 values[COUNTER_MAX];
    usz count;
} COUNTERS = {0};

/// @brief Finds a counter, and makes it if it is new; NULL if there is no room.
static i64 *CounterFind(const char *name)
{
    for (usz i = 0; i < COUNTERS.count; i++)
    {
        if (strcmp(COUNTERS.names[i], name) == 0)
        {
            return &COUNTERS.values[i];
        }
    }

    if (COUNTERS.count == COUNTER_MAX || strlen(name) >= COUNTER_NAME_SIZE)
    {
        return NULL;
    }

    strcpy(COUNTERS.names[COUNTERS.count], name);
    return &COUNTERS.values[COUNTERS.count++];
}

// counter.add, "int(string, int)": the string is valid only during the call, so it is copied if kept
static i32 CounterAdd(const char *name, i32 amount)
{
    i64 *value = CounterFind(name);

    if (value == NULL)
    {
        return 0;
    }

    *value += amount;
    return (i32)*value;
}

// counter.get, "int(string)"
static i32 CounterGet(const char *name)
{
    i64 *value = CounterFind(name);
    return value == NULL ? 0 : (i32)*value;
}

SHUResult ECSPlugin_Init(ECSPlugin plugin)
{
    // the signature tells the core how to pass the arguments; callers must ask for the same signature
    // the function is cast to ECSFunction, the type of any function; the signature says its real type
    SHU_ReturnResult(ECSService_RegisterFunction(plugin, "counter.add", (ECSFunction)CounterAdd, "int(string, int)", "Adds to a named counter, and gives its new value"));
    SHU_ReturnResult(ECSService_RegisterFunction(plugin, "counter.get", (ECSFunction)CounterGet, "int(string)", "Gives a named counter's value"));
    return SHUResult_Ok;
}
