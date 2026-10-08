#include "Bindings.h"

#include "Events.h"
#include "Lua.h"
#include "Settings.h"

#include "lua/lauxlib.h"
#include "lua/lua.h"

#include "SDL3/SDL.h"
#include "stb/stbSDL3.h"

#pragma region Source Only

/// @brief Name of the metatable of timer handles.
#define OPENECS_LUA_TIMER "ecs.timer"

/// @brief A timer started from Lua.
typedef struct ECSI_LuaTimer
{
    ECSPlugin plugin;
    ECSTimer timer;
    int function; // registry reference of the Lua function
    int handle;   // registry reference of the handle, so it lives while the timer runs
} ECSI_LuaTimer;

static struct
{
    struct
    {
        char *key; // "plugin: message"
        u64 value; // how often it happened
    } *errors; // stb_ds hash map with copied keys
} BINDINGS = {0};

/// @brief Names of the setting types in Lua, in the order of ECSSettingType.
static const char *const ECSI_BINDINGS_SETTING_TYPES[] = {"bool", "integer", "number", "string", "choice", "key", "list", "table", NULL};

/// @brief Reports an error of a plugin's Lua callback. Repeats of the same error are counted, not reported again.
static void ECSI_BindingsReport(ECSPlugin plugin, const char *message)
{
    char *key = NULL;

    if (SDL_asprintf(&key, "%s: %s", ECSI_PluginGetName(plugin), message) < 0)
    {
        return;
    }

    if (BINDINGS.errors == NULL)
    {
        sh_new_strdup(BINDINGS.errors);
    }

    u64 count = shget(BINDINGS.errors, key);
    shput(BINDINGS.errors, key, count + 1);
    SDL_free(key);

    if (count == 0)
    {
        ECS_Log(plugin, ECSLogLevel_Error, "%s", message);
    }
}

/// @brief Gets the plugin that owns the ecs table a function came from. Every function of a plugin's ecs table has the plugin as its upvalue.
static ECSPlugin ECSI_BindingsPlugin(lua_State *state)
{
    return lua_touserdata(state, lua_upvalueindex(1));
}

#pragma region Log

static int ECSI_BindingsLog(lua_State *state, ECSLogLevel level)
{
    ECS_Log(ECSI_BindingsPlugin(state), level, "%s", luaL_checkstring(state, 1));
    return 0;
}

static int ECSI_BindingsLogDebug(lua_State *state)
{
    return ECSI_BindingsLog(state, ECSLogLevel_Debug);
}

static int ECSI_BindingsLogInfo(lua_State *state)
{
    return ECSI_BindingsLog(state, ECSLogLevel_Info);
}

static int ECSI_BindingsLogWarn(lua_State *state)
{
    return ECSI_BindingsLog(state, ECSLogLevel_Warning);
}

static int ECSI_BindingsLogError(lua_State *state)
{
    return ECSI_BindingsLog(state, ECSLogLevel_Error);
}

static const luaL_Reg ECSI_BINDINGS_LOG[] = {
    {"debug", ECSI_BindingsLogDebug},
    {"info", ECSI_BindingsLogInfo},
    {"warn", ECSI_BindingsLogWarn},
    {"error", ECSI_BindingsLogError},
    {NULL, NULL},
};

#pragma endregion Log

#pragma region Settings

static int ECSI_BindingsSettingsDeclare(lua_State *state)
{
    luaL_checktype(state, 1, LUA_TTABLE);

    lua_getfield(state, 1, "name");
    lua_getfield(state, 1, "type");
    lua_getfield(state, 1, "description");
    lua_getfield(state, 1, "choices");
    lua_getfield(state, 1, "default");

    ECSSettingDesc desc = {
        .name = luaL_checkstring(state, -5),
        .type = (ECSSettingType)luaL_checkoption(state, -4, NULL, ECSI_BINDINGS_SETTING_TYPES),
        .description = luaL_optstring(state, -3, ""),
    };

    // the choices stay alive in the table on the stack
    const char **choices = NULL;

    for (lua_Integer i = 1; lua_istable(state, -2) && lua_rawgeti(state, -2, i) == LUA_TSTRING; i++)
    {
        arrput(choices, lua_tostring(state, -1));
        lua_pop(state, 1);
    }

    lua_pop(state, lua_istable(state, -2) ? 1 : 0);
    arrput(choices, NULL);
    desc.choices = choices;

    ECSValue *defaultValue = NULL;
    SHUResult result = ECSI_ValueCreate(&defaultValue);
    result = result ? result : ECSI_LuaGetValue(-1, defaultValue);
    bool hasDefault = ECSValue_GetType(defaultValue) != ECSValueType_Nil;
    result = result ? result : ECSI_SettingsDeclarePlugin(ECSI_BindingsPlugin(state), &desc, hasDefault ? defaultValue : NULL);

    ECSI_ValueDestroy(&defaultValue);
    arrfree(choices);

    // an invalid declaration is reported and returned; the plugin continues
    if (result)
    {
        lua_pushnil(state);
        lua_pushfstring(state, "setting '%s' is not declared (%s)", desc.name, SHUResult_String(result));
        return 2;
    }

    lua_pushboolean(state, true);
    return 1;
}

static int ECSI_BindingsSettingsGet(lua_State *state)
{
    const ECSValue *value = ECSSetting_Get(luaL_checkstring(state, 1));
    ECSI_LuaPushValue(value);
    return 1;
}

static const luaL_Reg ECSI_BINDINGS_SETTINGS[] = {
    {"declare", ECSI_BindingsSettingsDeclare},
    {"get", ECSI_BindingsSettingsGet},
    {NULL, NULL},
};

#pragma endregion Settings

#pragma region Timers

static void ECSI_BindingsTimerTick(void *data)
{
    ECSI_LuaTimer *timer = data;
    lua_State *state = ECSI_LuaGetState();

    lua_rawgeti(state, LUA_REGISTRYINDEX, timer->function);

    if (ECSI_LuaCall(0, 0))
    {
        ECSI_BindingsReport(timer->plugin, lua_tostring(state, -1));
        lua_pop(state, 1);
    }
}

/// @brief Frees a Lua timer when the core frees its timer, and clears its handle.
static void ECSI_BindingsTimerRelease(void *data)
{
    ECSI_LuaTimer *timer = data;
    lua_State *state = ECSI_LuaGetState();

    lua_rawgeti(state, LUA_REGISTRYINDEX, timer->handle);
    *(ECSI_LuaTimer **)lua_touserdata(state, -1) = NULL;
    lua_pop(state, 1);

    luaL_unref(state, LUA_REGISTRYINDEX, timer->function);
    luaL_unref(state, LUA_REGISTRYINDEX, timer->handle);
    SDL_free(timer);
}

/// @brief Starts a timer for a plugin, and pushes its handle.
/// @param owner The panel the timer belongs to, or NULL.
static int ECSI_BindingsStartTimer(lua_State *state, ECSPlugin plugin, const void *owner, int first)
{
    f64 seconds = (f64)luaL_checknumber(state, first);
    bool repeat = lua_toboolean(state, first + 1);
    luaL_checktype(state, first + 2, LUA_TFUNCTION);
    luaL_argcheck(state, seconds > 0.0 || (seconds == 0.0 && !repeat), first, "must be positive, or 0 for a one-shot timer");

    ECSI_LuaTimer *timer = SDL_malloc(sizeof(ECSI_LuaTimer));

    if (timer == NULL)
    {
        return luaL_error(state, "out of memory");
    }

    ECSI_LuaTimer **handle = lua_newuserdatauv(state, sizeof(ECSI_LuaTimer *), 0);
    *handle = timer;
    luaL_setmetatable(state, OPENECS_LUA_TIMER);

    lua_pushvalue(state, first + 2);
    timer->function = luaL_ref(state, LUA_REGISTRYINDEX);
    lua_pushvalue(state, -1);
    timer->handle = luaL_ref(state, LUA_REGISTRYINDEX);
    timer->plugin = plugin;

    if (ECSI_EventsStartTimer(plugin, owner, &timer->timer, seconds, repeat, ECSI_BindingsTimerTick, ECSI_BindingsTimerRelease, timer))
    {
        ECSI_BindingsTimerRelease(timer);
        return luaL_error(state, "out of memory");
    }

    return 1;
}

static int ECSI_BindingsTimerStart(lua_State *state)
{
    return ECSI_BindingsStartTimer(state, ECSI_BindingsPlugin(state), NULL, 1);
}

static int ECSI_BindingsTimerStop(lua_State *state)
{
    ECSI_LuaTimer **handle = luaL_checkudata(state, 1, OPENECS_LUA_TIMER);

    // a timer that already ended has no timer to stop
    if (*handle != NULL)
    {
        ECSTimer_Stop(&(*handle)->timer);
    }

    return 0;
}

static const luaL_Reg ECSI_BINDINGS_TIMER[] = {
    {"start", ECSI_BindingsTimerStart},
    {NULL, NULL},
};

static const luaL_Reg ECSI_BINDINGS_TIMER_METHODS[] = {
    {"stop", ECSI_BindingsTimerStop},
    {NULL, NULL},
};

#pragma endregion Timers

/// @brief Adds a table of functions to the ecs table on top of the stack. Each function gets the plugin as its upvalue.
static void ECSI_BindingsAddTable(lua_State *state, ECSPlugin plugin, const char *name, const luaL_Reg *functions)
{
    lua_newtable(state);
    lua_pushlightuserdata(state, plugin);
    luaL_setfuncs(state, functions, 1);
    lua_setfield(state, -2, name);
}

/// @brief Pushes a new ecs table for a plugin.
static void ECSI_BindingsPushEcs(lua_State *state, ECSPlugin plugin)
{
    lua_newtable(state);
    ECSI_BindingsAddTable(state, plugin, "log", ECSI_BINDINGS_LOG);
    ECSI_BindingsAddTable(state, plugin, "settings", ECSI_BINDINGS_SETTINGS);
    ECSI_BindingsAddTable(state, plugin, "timer", ECSI_BINDINGS_TIMER);

    lua_newtable(state);
    lua_pushstring(state, ECSI_PluginGetName(plugin));
    lua_setfield(state, -2, "name");
    lua_pushstring(state, ECSI_PluginGetVersion(plugin));
    lua_setfield(state, -2, "version");
    lua_setfield(state, -2, "plugin");
}

#pragma endregion Source Only

void ECSI_BindingsInitialize(void)
{
    lua_State *state = ECSI_LuaGetState();

    luaL_newmetatable(state, OPENECS_LUA_TIMER);
    luaL_newlib(state, ECSI_BINDINGS_TIMER_METHODS);
    lua_setfield(state, -2, "__index");
    lua_pop(state, 1);
}

void ECSI_BindingsTerminate(void)
{
    for (usz i = 0; i < shlenu(BINDINGS.errors); i++)
    {
        if (BINDINGS.errors[i].value > 1)
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "This error happened %" SDL_PRIu64 " times: %s", BINDINGS.errors[i].value, BINDINGS.errors[i].key);
        }
    }

    shfree(BINDINGS.errors);
    SDL_zero(BINDINGS);
}

SHUResult ECSI_BindingsStartPlugin(ECSPlugin plugin, const char *path)
{
    SDL_assert(plugin != NULL);
    SDL_assert(path != NULL);

    lua_State *state = ECSI_LuaGetState();
    int top = lua_gettop(state);

    if (luaL_loadfilex(state, path, "t") != LUA_OK)
    {
        ECS_Log(plugin, ECSLogLevel_Error, "Cannot load '%s': %s", path, lua_tostring(state, -1));
        lua_settop(state, top);
        return SHUResult_ErrFile;
    }

    // the plugin's environment holds its own ecs table, and reads other globals from the shared global table
    lua_newtable(state);
    ECSI_BindingsPushEcs(state, plugin);
    lua_setfield(state, -2, "ecs");
    lua_newtable(state);
    lua_pushglobaltable(state);
    lua_setfield(state, -2, "__index");
    lua_setmetatable(state, -2);
    lua_setupvalue(state, -2, 1);

    if (ECSI_LuaCall(0, 0))
    {
        ECS_Log(plugin, ECSLogLevel_Error, "%s", lua_tostring(state, -1));
        lua_settop(state, top);
        return SHUResult_ErrBadData;
    }

    lua_settop(state, top);
    return SHUResult_Ok;
}
