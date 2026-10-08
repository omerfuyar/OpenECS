#include "app/Bindings.h"

#include "base/Lua.h"
#include "interface/Input.h"
#include "interface/Panels.h"
#include "runtime/Events.h"
#include "runtime/Services.h"
#include "runtime/Settings.h"

#include "lua/lauxlib.h"
#include "lua/lua.h"

#include "SDL3/SDL.h"
#include "stb/stbSDL3.h"

#pragma region Source Only

/// @brief Name of the metatable of timer handles.
#define OPENECS_LUA_TIMER "ecs.timer"
/// @brief Name of the metatable of subscription handles.
#define OPENECS_LUA_SUBSCRIPTION "ecs.subscription"
/// @brief Name of the handle type of panels.
#define OPENECS_LUA_PANEL "ecs.panel"
/// @brief Name of the metatable of surfaces given to draw.
#define OPENECS_LUA_SURFACE "ecs.surface"

/// @brief A Lua function that a setting's owner gave to be told about changes.
typedef struct ECSILuaListener
{
    ECSPlugin plugin;
    char *name;   // the setting's name
    int function; // registry reference of the Lua function
} ECSILuaListener;

/// @brief A panel type registered from Lua.
typedef struct ECSILuaPanelType
{
    ECSPlugin plugin;
    int table; // registry reference of the description table, which holds the callbacks
} ECSILuaPanelType;

/// @brief The state of a panel of a Lua panel type.
typedef struct ECSILuaPanel
{
    ECSILuaPanelType *type;
    ECSPanel panel;
    int state;   // registry reference of the value that create returned
    int handle;  // registry reference of the panel's handle, which keeps it the same while the panel lives
    int surface; // registry reference of the surface handle given to draw
} ECSILuaPanel;

/// @brief A timer started from Lua.
typedef struct ECSILuaTimer
{
    ECSPlugin plugin;
    ECSTimer timer;
    int function; // registry reference of the Lua function
    int handle;   // registry reference of the handle, so it lives while the timer runs
} ECSILuaTimer;

/// @brief A Lua plugin's own state: the functions that save and restore it.
typedef struct ECSILuaPluginState
{
    ECSPlugin plugin;
    int save;    // registry reference of the save function
    int restore; // registry reference of the restore function
} ECSILuaPluginState;

/// @brief A subscription made from Lua.
typedef struct ECSILuaSubscription
{
    ECSPlugin plugin;
    ECSSubscription subscription;
    int function; // registry reference of the Lua function
    int handle;   // registry reference of the handle, so it lives while the subscription does
} ECSILuaSubscription;

static struct
{
    ECSILuaPanelType **types;    // stb_ds array
    ECSILuaListener **listeners; // stb_ds array
} BINDINGS = {0};

/// @brief Names of the event types in Lua, in the order of ECSPanelEventType.
static const char *const OPENECS_BINDINGS_EVENT_TYPES[] = {"pointerDown", "pointerUp", "pointerMove", "wheel", "keyDown", "keyUp", "focused", "unfocused", "shown", "hidden", "resized"};

/// @brief Names of the setting types in Lua, in the order of ECSSettingType.
static const char *const OPENECS_BINDINGS_SETTING_TYPES[] = {"bool", "integer", "number", "string", "choice", "key", "list", "table", NULL};

/// @brief Gets the plugin that owns the ecs table a function came from. Every function of a plugin's ecs table has the plugin as its upvalue.
static ECSPlugin ECSIBindings_Plugin(lua_State *state)
{
    return lua_touserdata(state, lua_upvalueindex(1));
}

/// @brief Reads a panel handle. Raises a Lua error if it is not one, or its panel is gone.
static ECSPanel ECSIBindings_CheckPanel(lua_State *state, int index)
{
    (void)state;
    return ECSIServices_CheckHandle(index, OPENECS_LUA_PANEL);
}

#pragma region Log

static int ECSIBindings_Log(lua_State *state, ECSLogLevel level)
{
    ECS_Log(ECSIBindings_Plugin(state), level, "%s", luaL_checkstring(state, 1));
    return 0;
}

static int ECSIBindings_LogDebug(lua_State *state)
{
    return ECSIBindings_Log(state, ECSLogLevel_Debug);
}

static int ECSIBindings_LogInfo(lua_State *state)
{
    return ECSIBindings_Log(state, ECSLogLevel_Info);
}

static int ECSIBindings_LogWarn(lua_State *state)
{
    return ECSIBindings_Log(state, ECSLogLevel_Warning);
}

static int ECSIBindings_LogError(lua_State *state)
{
    return ECSIBindings_Log(state, ECSLogLevel_Error);
}

static const luaL_Reg OPENECS_BINDINGS_LOG[] = {
    {"debug", ECSIBindings_LogDebug},
    {"info", ECSIBindings_LogInfo},
    {"warn", ECSIBindings_LogWarn},
    {"error", ECSIBindings_LogError},
    {NULL, NULL},
};

#pragma endregion Log

#pragma region Settings

/// @brief Tells a Lua owner that its setting changed, with the new value.
static void ECSIBindings_SettingChanged(void *data)
{
    ECSILuaListener *listener = data;
    lua_State *state = ECSILua_GetState();

    lua_rawgeti(state, LUA_REGISTRYINDEX, listener->function);
    ECSILua_PushValue(ECSSetting_Get(listener->name));

    if (ECSILua_Call(1, 0))
    {
        ECSIPlugin_ReportError(listener->plugin, lua_tostring(state, -1));
        lua_pop(state, 1);
    }
}

static int ECSIBindings_SettingsDeclare(lua_State *state)
{
    luaL_checktype(state, 1, LUA_TTABLE);
    ECSILuaListener *listener = NULL;

    // the listener is made first, because the checks below may raise errors and leave nothing to free but it
    if (lua_getfield(state, 1, "changed") == LUA_TFUNCTION)
    {
        lua_getfield(state, 1, "name");
        listener = SDL_calloc(1, sizeof(ECSILuaListener));
        char *name = listener == NULL ? NULL : SDL_strdup(luaL_optstring(state, -1, ""));

        if (name == NULL)
        {
            SDL_free(listener);
            return luaL_error(state, "out of memory");
        }

        lua_pop(state, 1);
        listener->plugin = ECSIBindings_Plugin(state);
        listener->name = name;
        listener->function = luaL_ref(state, LUA_REGISTRYINDEX);
        arrput(BINDINGS.listeners, listener);
    }
    else
    {
        lua_pop(state, 1);
    }

    lua_getfield(state, 1, "name");
    lua_getfield(state, 1, "type");
    lua_getfield(state, 1, "description");
    lua_getfield(state, 1, "choices");
    lua_getfield(state, 1, "default");

    ECSSettingDesc desc = {
        .name = luaL_checkstring(state, -5),
        .type = (ECSSettingType)luaL_checkoption(state, -4, NULL, OPENECS_BINDINGS_SETTING_TYPES),
        .description = luaL_optstring(state, -3, ""),
        .Changed = listener == NULL ? NULL : ECSIBindings_SettingChanged,
        .data = listener,
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
    SHUResult result = ECSValue_Create(&defaultValue);
    result = result ? result : ECSILua_GetValue(-1, defaultValue);
    bool hasDefault = ECSValue_GetType(defaultValue) != ECSValueType_Nil;
    result = result ? result : ECSISettings_DeclarePlugin(ECSIBindings_Plugin(state), &desc, hasDefault ? defaultValue : NULL);

    ECSValue_Destroy(&defaultValue);
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

static int ECSIBindings_SettingsGet(lua_State *state)
{
    const ECSValue *value = ECSSetting_Get(luaL_checkstring(state, 1));
    ECSILua_PushValue(value);
    return 1;
}

static int ECSIBindings_SettingsSet(lua_State *state)
{
    const char *name = luaL_checkstring(state, 1);
    ECSValue *value = NULL;
    SHUResult result = ECSValue_Create(&value);
    result = result ? result : ECSILua_GetValue(2, value);
    result = result ? result : ECSSetting_Set(name, value);
    ECSValue_Destroy(&value);

    if (result)
    {
        lua_pushnil(state);
        lua_pushfstring(state, "setting '%s' is not set (%s)", name, SHUResult_String(result));
        return 2;
    }

    lua_pushboolean(state, true);
    return 1;
}

/// @brief A settings function that fills a value: ECSSetting_Explain, or the list of settings.
typedef SHUResult (*ECSIBindingsFillFunction)(const char *name, ECSValue *retValue);

/// @brief Pushes a value that a settings function fills, or nil and a message.
static int ECSIBindings_SettingsPush(lua_State *state, ECSIBindingsFillFunction fill, const char *name)
{
    ECSValue *value = NULL;
    SHUResult result = ECSValue_Create(&value);
    result = result ? result : fill(name, value);

    if (result)
    {
        ECSValue_Destroy(&value);
        lua_pushnil(state);
        lua_pushfstring(state, "no setting '%s' (%s)", name == NULL ? "" : name, SHUResult_String(result));
        return 2;
    }

    ECSILua_PushValue(value);
    ECSValue_Destroy(&value);
    return 1;
}

static SHUResult ECSIBindings_SettingsFillList(const char *name, ECSValue *retValue)
{
    (void)name;
    return ECSSetting_List(retValue);
}

static int ECSIBindings_SettingsList(lua_State *state)
{
    return ECSIBindings_SettingsPush(state, ECSIBindings_SettingsFillList, NULL);
}

static int ECSIBindings_SettingsExplain(lua_State *state)
{
    return ECSIBindings_SettingsPush(state, ECSSetting_Explain, luaL_checkstring(state, 1));
}

static const luaL_Reg OPENECS_BINDINGS_SETTINGS[] = {
    {"declare", ECSIBindings_SettingsDeclare},
    {"get", ECSIBindings_SettingsGet},
    {"set", ECSIBindings_SettingsSet},
    {"list", ECSIBindings_SettingsList},
    {"explain", ECSIBindings_SettingsExplain},
    {NULL, NULL},
};

#pragma endregion Settings

#pragma region Timers

/// @brief Clears the handle a Lua value points to, so Lua cannot reach a destroyed object through it.
static void ECSIBindings_ClearHandle(lua_State *state, int reference)
{
    lua_rawgeti(state, LUA_REGISTRYINDEX, reference);
    *(void **)lua_touserdata(state, -1) = NULL;
    lua_pop(state, 1);
}

static void ECSIBindings_TimerTick(void *data)
{
    ECSILuaTimer *timer = data;
    lua_State *state = ECSILua_GetState();

    lua_rawgeti(state, LUA_REGISTRYINDEX, timer->function);

    if (ECSILua_Call(0, 0))
    {
        ECSIPlugin_ReportError(timer->plugin, lua_tostring(state, -1));
        lua_pop(state, 1);
    }
}

/// @brief Frees a Lua timer when the core frees its timer, and clears its handle.
static void ECSIBindings_TimerRelease(void *data)
{
    ECSILuaTimer *timer = data;
    lua_State *state = ECSILua_GetState();

    ECSIBindings_ClearHandle(state, timer->handle);

    luaL_unref(state, LUA_REGISTRYINDEX, timer->function);
    luaL_unref(state, LUA_REGISTRYINDEX, timer->handle);
    SDL_free(timer);
}

/// @brief Starts a timer for a plugin, and pushes its handle.
/// @param owner The panel the timer belongs to, or NULL.
static int ECSIBindings_StartTimer(lua_State *state, ECSPlugin plugin, const void *owner, int first)
{
    f64 seconds = (f64)luaL_checknumber(state, first);
    bool repeat = lua_toboolean(state, first + 1);
    luaL_checktype(state, first + 2, LUA_TFUNCTION);
    luaL_argcheck(state, seconds > 0.0 || (seconds == 0.0 && !repeat), first, "must be positive, or 0 for a one-shot timer");

    ECSILuaTimer *timer = SDL_malloc(sizeof(ECSILuaTimer));

    if (timer == NULL)
    {
        return luaL_error(state, "out of memory");
    }

    ECSILuaTimer **handle = lua_newuserdatauv(state, sizeof(ECSILuaTimer *), 0);
    *handle = timer;
    luaL_setmetatable(state, OPENECS_LUA_TIMER);

    lua_pushvalue(state, first + 2);
    timer->function = luaL_ref(state, LUA_REGISTRYINDEX);
    lua_pushvalue(state, -1);
    timer->handle = luaL_ref(state, LUA_REGISTRYINDEX);
    timer->plugin = plugin;

    if (ECSIEvents_StartTimer(plugin, owner, &timer->timer, seconds, repeat, ECSIBindings_TimerTick, ECSIBindings_TimerRelease, timer))
    {
        ECSIBindings_TimerRelease(timer);
        return luaL_error(state, "out of memory");
    }

    return 1;
}

static int ECSIBindings_TimerStart(lua_State *state)
{
    return ECSIBindings_StartTimer(state, ECSIBindings_Plugin(state), NULL, 1);
}

static int ECSIBindings_TimerStop(lua_State *state)
{
    ECSILuaTimer **handle = luaL_checkudata(state, 1, OPENECS_LUA_TIMER);

    // a timer that already ended has no timer to stop
    if (*handle != NULL)
    {
        ECSTimer_Stop(&(*handle)->timer);
    }

    return 0;
}

static const luaL_Reg OPENECS_BINDINGS_TIMER[] = {
    {"start", ECSIBindings_TimerStart},
    {NULL, NULL},
};

static const luaL_Reg OPENECS_BINDINGS_TIMER_METHODS[] = {
    {"stop", ECSIBindings_TimerStop},
    {NULL, NULL},
};

#pragma endregion Timers

#pragma region Handles

static int ECSIBindings_HandleRegisterType(lua_State *state)
{
    const char *name = luaL_checkstring(state, 1);

    if (ECSIServices_RegisterLuaHandleType(ECSIBindings_Plugin(state), name))
    {
        return luaL_error(state, "handle type '%s' cannot be registered", name);
    }

    return 0;
}

static int ECSIBindings_HandleNew(lua_State *state)
{
    const char *name = luaL_checkstring(state, 1);
    luaL_checkany(state, 2);
    ECSIServices_PushLuaHandle(ECSIBindings_Plugin(state), name, 2);
    return 1;
}

static int ECSIBindings_HandleValue(lua_State *state)
{
    ECSIServices_PushLuaHandleValue(ECSIBindings_Plugin(state), luaL_checkstring(state, 2), 1);
    return 1;
}

static const luaL_Reg OPENECS_BINDINGS_HANDLE[] = {
    {"registerType", ECSIBindings_HandleRegisterType},
    {"new", ECSIBindings_HandleNew},
    {"value", ECSIBindings_HandleValue},
    {NULL, NULL},
};

#pragma endregion Handles

#pragma region Plugin state

static SHUResult ECSIBindings_StateSave(void *data, ECSValue *retState)
{
    ECSILuaPluginState *luaState = data;
    lua_State *state = ECSILua_GetState();
    lua_rawgeti(state, LUA_REGISTRYINDEX, luaState->save);

    if (ECSILua_Call(0, 1))
    {
        ECSIPlugin_ReportError(luaState->plugin, lua_tostring(state, -1));
        lua_pop(state, 1);
        return SHUResult_ErrBadData;
    }

    SHUResult result = ECSILua_GetValue(-1, retState);
    lua_pop(state, 1);
    return result;
}

static SHUResult ECSIBindings_StateRestore(void *data, const ECSValue *saved, u32 version)
{
    ECSILuaPluginState *luaState = data;
    lua_State *state = ECSILua_GetState();
    lua_rawgeti(state, LUA_REGISTRYINDEX, luaState->restore);
    ECSILua_PushValue(saved);
    lua_pushinteger(state, version);

    if (ECSILua_Call(2, 0))
    {
        ECSIPlugin_ReportError(luaState->plugin, lua_tostring(state, -1));
        lua_pop(state, 1);
        return SHUResult_ErrBadData;
    }

    return SHUResult_Ok;
}

static void ECSIBindings_StateRelease(void *data)
{
    ECSILuaPluginState *luaState = data;
    lua_State *state = ECSILua_GetState();
    luaL_unref(state, LUA_REGISTRYINDEX, luaState->save);
    luaL_unref(state, LUA_REGISTRYINDEX, luaState->restore);
    SDL_free(luaState);
}

/// @brief Calls a Lua plugin's shutdown function; the data is its registry reference.
static void ECSIBindings_ShutdownCall(void *data)
{
    lua_State *state = ECSILua_GetState();
    lua_rawgeti(state, LUA_REGISTRYINDEX, (int)(intptr_t)data);

    if (ECSILua_Call(0, 0))
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "A Lua plugin failed to shut down: %s", lua_tostring(state, -1));
        lua_pop(state, 1);
    }
}

static void ECSIBindings_ShutdownRelease(void *data)
{
    luaL_unref(ECSILua_GetState(), LUA_REGISTRYINDEX, (int)(intptr_t)data);
}

static int ECSIBindings_PluginOnShutdown(lua_State *state)
{
    luaL_checktype(state, 1, LUA_TFUNCTION);
    lua_pushvalue(state, 1);
    int function = luaL_ref(state, LUA_REGISTRYINDEX);
    ECSIPlugin_SetLuaShutdown(ECSIBindings_Plugin(state), ECSIBindings_ShutdownCall, ECSIBindings_ShutdownRelease, (void *)(intptr_t)function);
    return 0;
}

static int ECSIBindings_PluginRegisterState(lua_State *state)
{
    luaL_checktype(state, 1, LUA_TTABLE);
    lua_getfield(state, 1, "version");
    lua_Integer version = luaL_optinteger(state, -1, 0);
    lua_getfield(state, 1, "save");
    lua_getfield(state, 1, "restore");
    luaL_argcheck(state, lua_isfunction(state, -2) && lua_isfunction(state, -1), 1, "save and restore must be functions");
    luaL_argcheck(state, version >= 0 && version <= SDL_MAX_UINT32, 1, "version must fit in 32 bits");

    ECSILuaPluginState *luaState = SDL_malloc(sizeof(ECSILuaPluginState));

    if (luaState == NULL)
    {
        return luaL_error(state, "out of memory");
    }

    luaState->plugin = ECSIBindings_Plugin(state);
    luaState->restore = luaL_ref(state, LUA_REGISTRYINDEX);
    luaState->save = luaL_ref(state, LUA_REGISTRYINDEX);

    ECSPluginStateDesc desc = {.version = (u32)version, .Save = ECSIBindings_StateSave, .Restore = ECSIBindings_StateRestore, .data = luaState};

    if (ECSIPlugin_RegisterState(luaState->plugin, &desc, ECSIBindings_StateRelease))
    {
        ECSIBindings_StateRelease(luaState);
        return luaL_error(state, "the plugin's state is registered already");
    }

    return 0;
}

#pragma endregion Plugin state

#pragma region Events

/// @brief Calls a Lua subscriber with the event's name and value.
static void ECSIBindings_EventCall(void *data, const char *name, const ECSValue *value)
{
    ECSILuaSubscription *subscription = data;
    lua_State *state = ECSILua_GetState();

    lua_rawgeti(state, LUA_REGISTRYINDEX, subscription->function);
    lua_pushstring(state, name);
    ECSILua_PushValue(value);

    if (ECSILua_Call(2, 0))
    {
        ECSIPlugin_ReportError(subscription->plugin, lua_tostring(state, -1));
        lua_pop(state, 1);
    }
}

/// @brief Frees a Lua subscription when the core frees its subscription, and clears its handle.
static void ECSIBindings_EventRelease(void *data)
{
    ECSILuaSubscription *subscription = data;
    lua_State *state = ECSILua_GetState();

    ECSIBindings_ClearHandle(state, subscription->handle);
    luaL_unref(state, LUA_REGISTRYINDEX, subscription->function);
    luaL_unref(state, LUA_REGISTRYINDEX, subscription->handle);
    SDL_free(subscription);
}

static int ECSIBindings_EventDeclare(lua_State *state)
{
    const char *name = luaL_checkstring(state, 1);
    const char *description = luaL_optstring(state, 2, "");

    if (ECSEvent_Declare(ECSIBindings_Plugin(state), name, description))
    {
        return luaL_error(state, "event '%s' cannot be declared", name);
    }

    return 0;
}

static int ECSIBindings_EventEmit(lua_State *state)
{
    const char *name = luaL_checkstring(state, 1);
    ECSValue *value = NULL;

    SHUResult result = ECSValue_Create(&value);
    result = result ? result : ECSILua_GetValue(2, value);
    result = result ? result : ECSEvent_Emit(ECSIBindings_Plugin(state), name, value);
    ECSValue_Destroy(&value);

    if (result)
    {
        return luaL_error(state, "event '%s' cannot be emitted (%s)", name, SHUResult_String(result));
    }

    return 0;
}

static int ECSIBindings_EventSubscribe(lua_State *state)
{
    const char *name = luaL_checkstring(state, 1);
    luaL_checktype(state, 2, LUA_TFUNCTION);
    ECSPlugin plugin = ECSIBindings_Plugin(state);

    ECSILuaSubscription *subscription = SDL_malloc(sizeof(ECSILuaSubscription));

    if (subscription == NULL)
    {
        return luaL_error(state, "out of memory");
    }

    ECSILuaSubscription **handle = lua_newuserdatauv(state, sizeof(ECSILuaSubscription *), 0);
    *handle = subscription;
    luaL_setmetatable(state, OPENECS_LUA_SUBSCRIPTION);

    lua_pushvalue(state, 2);
    subscription->function = luaL_ref(state, LUA_REGISTRYINDEX);
    lua_pushvalue(state, -1);
    subscription->handle = luaL_ref(state, LUA_REGISTRYINDEX);
    subscription->plugin = plugin;

    SHUResult result = ECSIEvents_Subscribe(plugin, name, &subscription->subscription, ECSIBindings_EventCall, ECSIBindings_EventRelease, subscription);

    if (result)
    {
        ECSIBindings_EventRelease(subscription);
        return luaL_error(state, "cannot subscribe to '%s' (%s)", name, SHUResult_String(result));
    }

    return 1;
}

static int ECSIBindings_EventCancel(lua_State *state)
{
    ECSILuaSubscription **handle = luaL_checkudata(state, 1, OPENECS_LUA_SUBSCRIPTION);

    // a subscription that already ended has nothing to cancel
    if (*handle != NULL)
    {
        ECSEvent_Unsubscribe(&(*handle)->subscription);
    }

    return 0;
}

static const luaL_Reg OPENECS_BINDINGS_EVENT[] = {
    {"declare", ECSIBindings_EventDeclare},
    {"emit", ECSIBindings_EventEmit},
    {"subscribe", ECSIBindings_EventSubscribe},
    {NULL, NULL},
};

static const luaL_Reg OPENECS_BINDINGS_SUBSCRIPTION_METHODS[] = {
    {"cancel", ECSIBindings_EventCancel},
    {NULL, NULL},
};

#pragma endregion Events

#pragma region Layout

/// @brief Names of the zones in Lua, in the order of ECSZone.
static const char *const OPENECS_BINDINGS_ZONES[] = {"default", "center", "left", "right", "top", "bottom", NULL};

/// @brief Reads an optional panel handle.
static ECSPanel ECSIBindings_OptPanel(lua_State *state, int index)
{
    return lua_isnoneornil(state, index) ? NULL : ECSIBindings_CheckPanel(state, index);
}

static int ECSIBindings_LayoutOpen(lua_State *state)
{
    const char *type = luaL_checkstring(state, 1);
    ECSPanel target = ECSIBindings_OptPanel(state, 3);
    ECSZone zone = (ECSZone)luaL_checkoption(state, 4, "default", OPENECS_BINDINGS_ZONES);
    ECSValue *saved = NULL;
    ECSPanel panel = NULL;

    SHUResult result = ECSValue_Create(&saved);
    result = result ? result : ECSILua_GetValue(2, saved);
    result = result ? result : ECSLayout_Open(ECSIBindings_Plugin(state), &panel, type, ECSValue_GetType(saved) == ECSValueType_Nil ? NULL : saved, target, zone);
    ECSValue_Destroy(&saved);

    if (result)
    {
        lua_pushnil(state);
        lua_pushfstring(state, "panel '%s' is not opened (%s)", type, SHUResult_String(result));
        return 2;
    }

    ECSIServices_PushHandle(OPENECS_LUA_PANEL, panel);
    return 1;
}

static int ECSIBindings_LayoutMove(lua_State *state)
{
    ECSPanel panel = ECSIBindings_CheckPanel(state, 1);
    ECSPanel target = ECSIBindings_CheckPanel(state, 2);
    ECSZone zone = (ECSZone)luaL_checkoption(state, 3, "center", OPENECS_BINDINGS_ZONES);

    if (ECSLayout_Move(panel, target, zone))
    {
        lua_pushnil(state);
        lua_pushliteral(state, "the panels are not in one workspace");
        return 2;
    }

    lua_pushboolean(state, true);
    return 1;
}

static int ECSIBindings_LayoutClose(lua_State *state)
{
    lua_pushboolean(state, ECSLayout_Close(ECSIBindings_CheckPanel(state, 1)));
    return 1;
}

static int ECSIBindings_LayoutFocus(lua_State *state)
{
    ECSLayout_Focus(ECSIBindings_CheckPanel(state, 1));
    return 0;
}

static int ECSIBindings_LayoutGetFocus(lua_State *state)
{
    (void)state;
    ECSIServices_PushHandle(OPENECS_LUA_PANEL, ECSLayout_GetFocus());
    return 1;
}

static int ECSIBindings_LayoutFind(lua_State *state)
{
    lua_Integer id = luaL_checkinteger(state, 1);
    ECSIServices_PushHandle(OPENECS_LUA_PANEL, id > 0 && id <= SDL_MAX_UINT32 ? ECSLayout_FindPanel((u32)id) : NULL);
    return 1;
}

static const luaL_Reg OPENECS_BINDINGS_LAYOUT[] = {
    {"find", ECSIBindings_LayoutFind},
    {"open", ECSIBindings_LayoutOpen},
    {"move", ECSIBindings_LayoutMove},
    {"close", ECSIBindings_LayoutClose},
    {"focus", ECSIBindings_LayoutFocus},
    {"getFocus", ECSIBindings_LayoutGetFocus},
    {NULL, NULL},
};

static int ECSIBindings_WorkspaceCount(lua_State *state)
{
    lua_pushinteger(state, (lua_Integer)ECSWorkspace_GetCount());
    return 1;
}

static int ECSIBindings_WorkspaceGetCurrent(lua_State *state)
{
    lua_pushinteger(state, (lua_Integer)ECSWorkspace_GetCurrent());
    return 1;
}

static int ECSIBindings_WorkspaceGetName(lua_State *state)
{
    lua_Integer number = luaL_checkinteger(state, 1);
    lua_pushstring(state, number >= 1 ? ECSWorkspace_GetName((usz)number) : NULL);
    return 1;
}

static int ECSIBindings_WorkspaceSwitch(lua_State *state)
{
    lua_Integer number = luaL_checkinteger(state, 1);

    if (number >= 1)
    {
        ECSWorkspace_Switch((usz)number);
    }

    return 0;
}

static const luaL_Reg OPENECS_BINDINGS_WORKSPACE[] = {
    {"count", ECSIBindings_WorkspaceCount},
    {"getCurrent", ECSIBindings_WorkspaceGetCurrent},
    {"getName", ECSIBindings_WorkspaceGetName},
    {"switch", ECSIBindings_WorkspaceSwitch},
    {NULL, NULL},
};

#pragma endregion Layout

#pragma region Clipboard

static int ECSIBindings_ClipboardSetText(lua_State *state)
{
    lua_pushboolean(state, ECSClipboard_SetText(luaL_checkstring(state, 1)) == SHUResult_Ok);
    return 1;
}

static int ECSIBindings_ClipboardGetText(lua_State *state)
{
    lua_pushstring(state, ECSClipboard_GetText());
    return 1;
}

static int ECSIBindings_ClipboardSetData(lua_State *state)
{
    usz size = 0;
    const char *mimeType = luaL_checkstring(state, 1);
    const char *bytes = luaL_checklstring(state, 2, &size);
    lua_pushboolean(state, ECSClipboard_SetData(mimeType, (SHUSliceView){.data = bytes, .size = size}) == SHUResult_Ok);
    return 1;
}

static int ECSIBindings_ClipboardGetData(lua_State *state)
{
    SHUSlice data = cs0;

    if (ECSClipboard_GetData(luaL_checkstring(state, 1), &data))
    {
        lua_pushnil(state);
        return 1;
    }

    lua_pushlstring(state, data.data, data.size);
    return 1;
}

static const luaL_Reg OPENECS_BINDINGS_CLIPBOARD[] = {
    {"setText", ECSIBindings_ClipboardSetText},
    {"getText", ECSIBindings_ClipboardGetText},
    {"setData", ECSIBindings_ClipboardSetData},
    {"getData", ECSIBindings_ClipboardGetData},
    {NULL, NULL},
};

#pragma endregion Clipboard

#pragma region Dialogs

/// @brief Names of the dialog types in Lua, in the order of ECSDialogType.
static const char *const OPENECS_BINDINGS_DIALOG_TYPES[] = {"openFile", "saveFile", "openFolder", NULL};

/// @brief A Lua function waiting for a file dialog's answer.
typedef struct ECSILuaDialog
{
    ECSPlugin plugin;
    int function; // registry reference
} ECSILuaDialog;

static void ECSIBindings_DialogDone(void *data, const char *const *files, usz count)
{
    ECSILuaDialog *dialog = data;
    lua_State *state = ECSILua_GetState();

    lua_rawgeti(state, LUA_REGISTRYINDEX, dialog->function);

    if (files == NULL)
    {
        lua_pushnil(state);
    }
    else
    {
        lua_createtable(state, (int)count, 0);

        for (usz i = 0; i < count; i++)
        {
            lua_pushstring(state, files[i]);
            lua_rawseti(state, -2, (lua_Integer)i + 1);
        }
    }

    if (ECSILua_Call(1, 0))
    {
        ECSIPlugin_ReportError(dialog->plugin, lua_tostring(state, -1));
        lua_pop(state, 1);
    }

    luaL_unref(state, LUA_REGISTRYINDEX, dialog->function);
    SDL_free(dialog);
}

/// @brief ecs.dialog.show({ type, filters = { { name, pattern } }, location, many }, function(files) end)
static int ECSIBindings_DialogShow(lua_State *state)
{
    luaL_checktype(state, 1, LUA_TTABLE);
    luaL_checktype(state, 2, LUA_TFUNCTION);

    // the texts stay alive in the table while the dialog is described, and the core copies them
    lua_getfield(state, 1, "type");
    lua_getfield(state, 1, "location");
    lua_getfield(state, 1, "many");
    lua_getfield(state, 1, "filters");

    ECSDialogFilter *filters = NULL;

    for (lua_Integer i = 1; lua_istable(state, -1) && lua_rawgeti(state, -1, i) == LUA_TTABLE; i++)
    {
        lua_getfield(state, -1, "name");
        lua_getfield(state, -2, "pattern");
        ECSDialogFilter filter = {lua_tostring(state, -2), lua_tostring(state, -1)};
        lua_pop(state, 3);

        if (filter.name != NULL && filter.pattern != NULL)
        {
            arrput(filters, filter);
        }
    }

    // the loop leaves the first value that is not a filter on the stack
    lua_pop(state, lua_istable(state, -2) ? 1 : 0);

    ECSDialogDesc desc = {
        .type = (ECSDialogType)luaL_checkoption(state, -4, "openFile", OPENECS_BINDINGS_DIALOG_TYPES),
        .filters = filters,
        .filterCount = arrlenu(filters),
        .location = lua_tostring(state, -3),
        .many = lua_toboolean(state, -2),
        .Done = ECSIBindings_DialogDone,
    };

    ECSILuaDialog *dialog = SDL_malloc(sizeof(ECSILuaDialog));

    if (dialog == NULL)
    {
        arrfree(filters);
        return luaL_error(state, "out of memory");
    }

    lua_pushvalue(state, 2);
    dialog->function = luaL_ref(state, LUA_REGISTRYINDEX);
    dialog->plugin = ECSIBindings_Plugin(state);
    desc.data = dialog;

    SHUResult result = ECSDialog_Show(dialog->plugin, &desc);
    arrfree(filters);

    if (result)
    {
        luaL_unref(state, LUA_REGISTRYINDEX, dialog->function);
        SDL_free(dialog);
        lua_pushnil(state);
        lua_pushfstring(state, "the dialog is not shown (%s)", SHUResult_String(result));
        return 2;
    }

    lua_pushboolean(state, true);
    return 1;
}

/// @brief ecs.dialog.message(title, text, { buttons }) gives the position of the pressed button, from 1, or nil.
static int ECSIBindings_DialogMessage(lua_State *state)
{
    const char *title = luaL_checkstring(state, 1);
    const char *text = luaL_checkstring(state, 2);
    const char **buttons = NULL;

    // the button texts stay alive in the table on the stack
    for (lua_Integer i = 1; lua_istable(state, 3) && lua_rawgeti(state, 3, i) == LUA_TSTRING; i++)
    {
        arrput(buttons, lua_tostring(state, -1));
        lua_pop(state, 1);
    }

    if (arrlenu(buttons) == 0)
    {
        arrput(buttons, "OK");
    }

    usz button = 0;
    SHUResult result = ECSDialog_ShowMessage(title, text, buttons, arrlenu(buttons), &button);
    arrfree(buttons);

    if (result)
    {
        lua_pushnil(state);
        return 1;
    }

    lua_pushinteger(state, (lua_Integer)button + 1);
    return 1;
}

static const luaL_Reg OPENECS_BINDINGS_DIALOG[] = {
    {"show", ECSIBindings_DialogShow},
    {"message", ECSIBindings_DialogMessage},
    {NULL, NULL},
};

#pragma endregion Dialogs

#pragma region Input

static int ECSIBindings_InputBind(lua_State *state)
{
    const char *panelType = luaL_checkstring(state, 1);
    const char *setting = luaL_checkstring(state, 2);
    const char *function = luaL_checkstring(state, 3);
    SHUResult result = ECSKey_Bind(ECSIBindings_Plugin(state), panelType, setting, function);

    // an invalid binding is reported and returned; the plugin continues
    if (result)
    {
        lua_pushnil(state);
        lua_pushfstring(state, "the key of '%s' is not bound (%s)", setting, SHUResult_String(result));
        return 2;
    }

    lua_pushboolean(state, true);
    return 1;
}

static const luaL_Reg OPENECS_BINDINGS_INPUT[] = {
    {"bind", ECSIBindings_InputBind},
    {NULL, NULL},
};

#pragma endregion Input

#pragma region Services

static int ECSIBindings_ServiceGet(lua_State *state)
{
    const char *name = luaL_checkstring(state, 1);
    const char *signature = luaL_optstring(state, 2, NULL);
    SHUResult result = ECSIServices_PushFunction(ECSIBindings_Plugin(state), name, signature);

    // a missing function is an expected failure: nil and a message
    if (result)
    {
        lua_pushnil(state);
        lua_pushfstring(state, "function '%s' is not available (%s)", name, SHUResult_String(result));
        return 2;
    }

    return 1;
}

/// @brief ecs.service.register(prefix, functions): functions is a table of { sig = ..., doc = ..., fn = ... } by local name.
static int ECSIBindings_ServiceRegister(lua_State *state)
{
    const char *prefix = luaL_checkstring(state, 1);
    luaL_checktype(state, 2, LUA_TTABLE);
    ECSPlugin plugin = ECSIBindings_Plugin(state);
    SHUResult result = SHUResult_Ok;
    const char *failed = NULL;

    lua_pushnil(state);

    while (lua_next(state, 2) != 0)
    {
        // lua_tostring would change a number key in place and break lua_next, so check the types first
        luaL_argcheck(state, lua_type(state, -2) == LUA_TSTRING && lua_istable(state, -1), 2, "must map names to { sig, doc, fn } tables");
        lua_getfield(state, -1, "sig");
        lua_getfield(state, -2, "doc");
        lua_getfield(state, -3, "fn");
        luaL_checktype(state, -1, LUA_TFUNCTION);

        const char *name = lua_pushfstring(state, "%s.%s", prefix, lua_tostring(state, -5));
        lua_insert(state, -2);
        SHUResult registered = ECSIServices_RegisterLua(plugin, name, luaL_checkstring(state, -4), luaL_optstring(state, -3, ""));

        // a failed function stays on the stack; a registered one was taken
        lua_settop(state, registered ? lua_gettop(state) - 5 : lua_gettop(state) - 4);
        result = registered ? registered : result;
        failed = registered ? lua_tostring(state, -1) : failed;
    }

    // an invalid registration is reported and returned; the plugin continues with the others
    if (result)
    {
        lua_pushnil(state);
        lua_pushfstring(state, "function '%s.%s' is not registered (%s)", prefix, failed == NULL ? "?" : failed, SHUResult_String(result));
        return 2;
    }

    lua_pushboolean(state, true);
    return 1;
}

static const luaL_Reg OPENECS_BINDINGS_SERVICE[] = {
    {"register", ECSIBindings_ServiceRegister},
    {"get", ECSIBindings_ServiceGet},
    {NULL, NULL},
};

#pragma endregion Services

#pragma region Panels

static ECSSurface *ECSIBindings_CheckSurface(lua_State *state, int index)
{
    ECSSurface **handle = luaL_checkudata(state, index, OPENECS_LUA_SURFACE);

    if (*handle == NULL)
    {
        luaL_error(state, "a surface is valid only while draw runs");
    }

    return *handle;
}

/// @brief Pushes a callback of a Lua panel type.
/// @return true if the type has the callback; nothing is pushed otherwise.
static bool ECSIBindings_PushCallback(lua_State *state, const ECSILuaPanelType *type, const char *name)
{
    lua_rawgeti(state, LUA_REGISTRYINDEX, type->table);
    bool found = lua_getfield(state, -1, name) == LUA_TFUNCTION;
    lua_remove(state, -2);

    if (!found)
    {
        lua_pop(state, 1);
    }

    return found;
}

/// @brief Reports the error on top of the stack, pops it, and makes the panel faulted.
static void ECSIBindings_PanelFailed(lua_State *state, const ECSILuaPanel *luaPanel)
{
    const char *message = lua_tostring(state, -1);
    ECSIPlugin_ReportError(luaPanel->type->plugin, message);
    ECSIPanel_Fault(luaPanel->panel, message);
    lua_pop(state, 1);
}

static void ECSIBindings_PanelFree(lua_State *state, ECSILuaPanel *luaPanel)
{
    ECSIServices_ForgetHandle(luaPanel->panel);
    luaL_unref(state, LUA_REGISTRYINDEX, luaPanel->state);
    luaL_unref(state, LUA_REGISTRYINDEX, luaPanel->handle);
    luaL_unref(state, LUA_REGISTRYINDEX, luaPanel->surface);
    SDL_free(luaPanel);
}

static SHUResult ECSIBindings_PanelCreate(ECSPanel panel, const ECSValue *savedState, u32 version, void **retState)
{
    lua_State *state = ECSILua_GetState();
    ECSILuaPanel *luaPanel = SDL_calloc(1, sizeof(ECSILuaPanel));

    if (luaPanel == NULL)
    {
        return SHUResult_ErrAllocation;
    }

    luaPanel->type = panel->type->typeData;
    luaPanel->panel = panel;
    luaPanel->state = LUA_NOREF;

    ECSIServices_PushHandle(OPENECS_LUA_PANEL, panel);
    luaPanel->handle = luaL_ref(state, LUA_REGISTRYINDEX);

    ECSSurface **surface = lua_newuserdatauv(state, sizeof(ECSSurface *), 0);
    *surface = NULL;
    luaL_setmetatable(state, OPENECS_LUA_SURFACE);
    luaPanel->surface = luaL_ref(state, LUA_REGISTRYINDEX);

    if (!ECSIBindings_PushCallback(state, luaPanel->type, "create"))
    {
        lua_pushnil(state);
    }
    else
    {
        lua_rawgeti(state, LUA_REGISTRYINDEX, luaPanel->handle);
        ECSILua_PushValue(savedState);
        lua_pushinteger(state, (lua_Integer)version);

        if (ECSILua_Call(3, 1))
        {
            ECSIBindings_PanelFailed(state, luaPanel);
            ECSIBindings_PanelFree(state, luaPanel);
            return SHUResult_ErrBadData;
        }
    }

    luaPanel->state = luaL_ref(state, LUA_REGISTRYINDEX);
    *retState = luaPanel;
    return SHUResult_Ok;
}

static void ECSIBindings_PanelDestroy(void *data)
{
    ECSILuaPanel *luaPanel = data;
    lua_State *state = ECSILua_GetState();

    if (ECSIBindings_PushCallback(state, luaPanel->type, "destroy"))
    {
        lua_rawgeti(state, LUA_REGISTRYINDEX, luaPanel->state);

        if (ECSILua_Call(1, 0))
        {
            ECSIPlugin_ReportError(luaPanel->type->plugin, lua_tostring(state, -1));
            lua_pop(state, 1);
        }
    }

    ECSIBindings_PanelFree(state, luaPanel);
}

static void ECSIBindings_PanelDraw(void *data, ECSSurface *surface, f64 seconds)
{
    ECSILuaPanel *luaPanel = data;
    lua_State *state = ECSILua_GetState();

    if (!ECSIBindings_PushCallback(state, luaPanel->type, "draw"))
    {
        return;
    }

    lua_rawgeti(state, LUA_REGISTRYINDEX, luaPanel->state);
    lua_rawgeti(state, LUA_REGISTRYINDEX, luaPanel->surface);
    ECSSurface **handle = lua_touserdata(state, -1);
    *handle = surface;
    lua_pushnumber(state, (lua_Number)seconds);

    if (ECSILua_Call(3, 0))
    {
        ECSIBindings_PanelFailed(state, luaPanel);
    }

    *handle = NULL;
}

static void ECSIBindings_PushEvent(lua_State *state, const ECSPanelEvent *event)
{
    lua_createtable(state, 0, 8);
    lua_pushstring(state, OPENECS_BINDINGS_EVENT_TYPES[event->type]);
    lua_setfield(state, -2, "type");

    switch (event->type)
    {
    case ECSPanelEventType_PointerDown:
    case ECSPanelEventType_PointerUp:
        lua_pushinteger(state, event->pointer.button);
        lua_setfield(state, -2, "button");
        // fall through
    case ECSPanelEventType_PointerMove:
        lua_pushnumber(state, (lua_Number)event->pointer.x);
        lua_setfield(state, -2, "x");
        lua_pushnumber(state, (lua_Number)event->pointer.y);
        lua_setfield(state, -2, "y");
        break;
    case ECSPanelEventType_Wheel:
        lua_pushnumber(state, (lua_Number)event->wheel.x);
        lua_setfield(state, -2, "x");
        lua_pushnumber(state, (lua_Number)event->wheel.y);
        lua_setfield(state, -2, "y");
        lua_pushnumber(state, (lua_Number)event->wheel.amountX);
        lua_setfield(state, -2, "wheelX");
        lua_pushnumber(state, (lua_Number)event->wheel.amountY);
        lua_setfield(state, -2, "wheelY");
        break;
    case ECSPanelEventType_KeyDown:
    case ECSPanelEventType_KeyUp:
        lua_pushstring(state, SDL_GetKeyName(event->key.code));
        lua_setfield(state, -2, "key");
        break;
    case ECSPanelEventType_Shown:
    case ECSPanelEventType_Resized:
        lua_pushnumber(state, (lua_Number)event->size.width);
        lua_setfield(state, -2, "width");
        lua_pushnumber(state, (lua_Number)event->size.height);
        lua_setfield(state, -2, "height");
        break;
    default:
        break;
    }

    lua_pushboolean(state, (event->modifiers & ECSModifier_Shift) != 0);
    lua_setfield(state, -2, "shift");
    lua_pushboolean(state, (event->modifiers & ECSModifier_Ctrl) != 0);
    lua_setfield(state, -2, "ctrl");
    lua_pushboolean(state, (event->modifiers & ECSModifier_Alt) != 0);
    lua_setfield(state, -2, "alt");
    lua_pushboolean(state, (event->modifiers & ECSModifier_Super) != 0);
    lua_setfield(state, -2, "super");
}

static void ECSIBindings_PanelEvent(void *data, const ECSPanelEvent *event)
{
    ECSILuaPanel *luaPanel = data;
    lua_State *state = ECSILua_GetState();

    if (!ECSIBindings_PushCallback(state, luaPanel->type, "event"))
    {
        return;
    }

    lua_rawgeti(state, LUA_REGISTRYINDEX, luaPanel->state);
    ECSIBindings_PushEvent(state, event);

    if (ECSILua_Call(2, 0))
    {
        ECSIBindings_PanelFailed(state, luaPanel);
    }
}

static SHUResult ECSIBindings_PanelSaveState(void *data, ECSValue *retState)
{
    ECSILuaPanel *luaPanel = data;
    lua_State *state = ECSILua_GetState();

    if (!ECSIBindings_PushCallback(state, luaPanel->type, "saveState"))
    {
        return SHUResult_ErrNotFound;
    }

    lua_rawgeti(state, LUA_REGISTRYINDEX, luaPanel->state);

    if (ECSILua_Call(1, 1))
    {
        ECSIBindings_PanelFailed(state, luaPanel);
        return SHUResult_ErrBadData;
    }

    SHUResult result = ECSILua_GetValue(-1, retState);
    lua_pop(state, 1);
    return result;
}

static SHUResult ECSIBindings_PanelSave(void *data)
{
    ECSILuaPanel *luaPanel = data;
    lua_State *state = ECSILua_GetState();

    if (!ECSIBindings_PushCallback(state, luaPanel->type, "save"))
    {
        return SHUResult_ErrNotFound;
    }

    lua_rawgeti(state, LUA_REGISTRYINDEX, luaPanel->state);

    // save returns true, or nil and a message
    if (ECSILua_Call(1, 2))
    {
        ECSIBindings_PanelFailed(state, luaPanel);
        return SHUResult_ErrBadData;
    }

    bool saved = lua_toboolean(state, -2);

    if (!saved)
    {
        ECS_Log(luaPanel->type->plugin, ECSLogLevel_Warning, "A panel failed to save: %s", luaL_optstring(state, -1, "no reason given"));
    }

    lua_pop(state, 2);
    return saved ? SHUResult_Ok : SHUResult_ErrFile;
}

/// @brief Checks whether the table at an index has a function field.
static bool ECSIBindings_HasFunction(lua_State *state, int index, const char *name)
{
    bool found = lua_getfield(state, index, name) == LUA_TFUNCTION;
    lua_pop(state, 1);
    return found;
}

static int ECSIBindings_PanelRegisterType(lua_State *state)
{
    luaL_checktype(state, 1, LUA_TTABLE);
    ECSPlugin plugin = ECSIBindings_Plugin(state);

    // the texts stay alive in the table while the type is registered, which copies them
    lua_getfield(state, 1, "name");
    lua_getfield(state, 1, "title");
    lua_getfield(state, 1, "surface");
    lua_getfield(state, 1, "stateVersion");
    lua_getfield(state, 1, "continuous");
    lua_getfield(state, 1, "minWidth");
    lua_getfield(state, 1, "minHeight");

    ECSPanelTypeDesc desc = {
        .name = luaL_checkstring(state, -7),
        .title = luaL_optstring(state, -6, NULL),
        .surface = SDL_strcmp(luaL_optstring(state, -5, "pixels"), "gpu") == 0 ? ECSSurfaceType_Gpu : ECSSurfaceType_Pixels,
        .stateVersion = (u32)luaL_optinteger(state, -4, 0),
        .continuous = lua_toboolean(state, -3),
        .minWidth = (f32)luaL_optnumber(state, -2, 0.0),
        .minHeight = (f32)luaL_optnumber(state, -1, 0.0),
        .Create = ECSIBindings_PanelCreate,
        .Destroy = ECSIBindings_PanelDestroy,
        .Draw = ECSIBindings_HasFunction(state, 1, "draw") ? ECSIBindings_PanelDraw : NULL,
        .Event = ECSIBindings_HasFunction(state, 1, "event") ? ECSIBindings_PanelEvent : NULL,
        .SaveState = ECSIBindings_HasFunction(state, 1, "saveState") ? ECSIBindings_PanelSaveState : NULL,
        .Save = ECSIBindings_HasFunction(state, 1, "save") ? ECSIBindings_PanelSave : NULL,
    };

    ECSILuaPanelType *type = SDL_malloc(sizeof(ECSILuaPanelType));

    if (type == NULL)
    {
        return luaL_error(state, "out of memory");
    }

    lua_pushvalue(state, 1);
    type->table = luaL_ref(state, LUA_REGISTRYINDEX);
    type->plugin = plugin;
    SHUResult result = ECSIPanel_TypeRegister(plugin, &desc, type);

    // an invalid registration is reported and returned; the plugin continues
    if (result)
    {
        luaL_unref(state, LUA_REGISTRYINDEX, type->table);
        SDL_free(type);
        lua_pushnil(state);
        lua_pushfstring(state, "panel type '%s' is not registered (%s)", desc.name, SHUResult_String(result));
        return 2;
    }

    arrput(BINDINGS.types, type);
    lua_pushboolean(state, true);
    return 1;
}

static int ECSIBindings_PanelRedraw(lua_State *state)
{
    ECSPanel_Redraw(ECSIBindings_CheckPanel(state, 1));
    return 0;
}

static int ECSIBindings_PanelGetTitle(lua_State *state)
{
    lua_pushstring(state, ECSPanel_GetTitle(ECSIBindings_CheckPanel(state, 1)));
    return 1;
}

static int ECSIBindings_PanelSetTitle(lua_State *state)
{
    ECSPanel_SetTitle(ECSIBindings_CheckPanel(state, 1), luaL_checkstring(state, 2));
    return 0;
}

static int ECSIBindings_PanelGetId(lua_State *state)
{
    lua_pushinteger(state, ECSPanel_GetId(ECSIBindings_CheckPanel(state, 1)));
    return 1;
}

static int ECSIBindings_PanelGetType(lua_State *state)
{
    lua_pushstring(state, ECSPanel_GetType(ECSIBindings_CheckPanel(state, 1)));
    return 1;
}

static int ECSIBindings_PanelSetUnsaved(lua_State *state)
{
    ECSPanel_SetUnsaved(ECSIBindings_CheckPanel(state, 1), lua_toboolean(state, 2));
    return 0;
}

static int ECSIBindings_PanelStartTimer(lua_State *state)
{
    ECSPanel panel = ECSIBindings_CheckPanel(state, 1);
    luaL_argcheck(state, panel->type != NULL, 1, "the panel has no type");
    return ECSIBindings_StartTimer(state, panel->type->plugin, panel, 2);
}

static const luaL_Reg OPENECS_BINDINGS_PANEL_METHODS[] = {
    {"redraw", ECSIBindings_PanelRedraw},
    {"getTitle", ECSIBindings_PanelGetTitle},
    {"setTitle", ECSIBindings_PanelSetTitle},
    {"getId", ECSIBindings_PanelGetId},
    {"getType", ECSIBindings_PanelGetType},
    {"setUnsaved", ECSIBindings_PanelSetUnsaved},
    {"startTimer", ECSIBindings_PanelStartTimer},
    {NULL, NULL},
};

static int ECSIBindings_PanelAddMenuEntry(lua_State *state)
{
    const char *type = luaL_checkstring(state, 1);
    const char *function = luaL_checkstring(state, 2);

    // an invalid entry is reported and returned; the plugin continues
    if (ECSPanelType_AddMenuEntry(ECSIBindings_Plugin(state), type, function))
    {
        lua_pushnil(state);
        lua_pushfstring(state, "'%s' is not added to the menu of '%s'", function, type);
        return 2;
    }

    lua_pushboolean(state, true);
    return 1;
}

static const luaL_Reg OPENECS_BINDINGS_PANEL[] = {
    {"registerType", ECSIBindings_PanelRegisterType},
    {"addMenuEntry", ECSIBindings_PanelAddMenuEntry},
    {"redraw", ECSIBindings_PanelRedraw},
    {"getTitle", ECSIBindings_PanelGetTitle},
    {"setTitle", ECSIBindings_PanelSetTitle},
    {"setUnsaved", ECSIBindings_PanelSetUnsaved},
    {"startTimer", ECSIBindings_PanelStartTimer},
    {NULL, NULL},
};

#pragma endregion Panels

#pragma region Surfaces

/// @brief Gets a pixel's address, or NULL if the position is outside the surface.
static u32 *ECSIBindings_Pixel(ECSSurface *surface, lua_Integer x, lua_Integer y)
{
    if (x < 0 || y < 0 || x >= surface->width || y >= surface->height)
    {
        return NULL;
    }

    return (u32 *)((u8 *)surface->pixels.data + (usz)y * (usz)surface->pitch) + x;
}

static int ECSIBindings_SurfaceSetPixel(lua_State *state)
{
    ECSSurface *surface = ECSIBindings_CheckSurface(state, 1);
    u32 *pixel = ECSIBindings_Pixel(surface, luaL_checkinteger(state, 2), luaL_checkinteger(state, 3));

    // pixels outside the surface are clipped
    if (pixel != NULL)
    {
        *pixel = (u32)luaL_checkinteger(state, 4);
    }

    return 0;
}

static int ECSIBindings_SurfaceGetPixel(lua_State *state)
{
    ECSSurface *surface = ECSIBindings_CheckSurface(state, 1);
    u32 *pixel = ECSIBindings_Pixel(surface, luaL_checkinteger(state, 2), luaL_checkinteger(state, 3));

    if (pixel == NULL)
    {
        return luaL_argerror(state, 2, "the position is outside the surface");
    }

    lua_pushinteger(state, (lua_Integer)*pixel);
    return 1;
}

static int ECSIBindings_SurfaceSetRow(lua_State *state)
{
    ECSSurface *surface = ECSIBindings_CheckSurface(state, 1);
    lua_Integer y = luaL_checkinteger(state, 2);
    usz length = 0;
    const char *bytes = luaL_checklstring(state, 3, &length);
    lua_Integer x = luaL_optinteger(state, 4, 0);

    // the row is clipped to the surface
    if (y < 0 || y >= surface->height || x >= surface->width)
    {
        return 0;
    }

    lua_Integer skip = x < 0 ? -x : 0;
    lua_Integer count = SDL_min((lua_Integer)(length / 4) - skip, (lua_Integer)surface->width - (x + skip));

    u32 *first = ECSIBindings_Pixel(surface, x + skip, y);

    if (count > 0 && first != NULL)
    {
        SDL_memcpy(first, bytes + skip * 4, (usz)count * 4);
    }

    return 0;
}

static int ECSIBindings_SurfaceIndex(lua_State *state)
{
    ECSSurface *surface = ECSIBindings_CheckSurface(state, 1);
    const char *key = luaL_checkstring(state, 2);

    if (SDL_strcmp(key, "width") == 0)
    {
        lua_pushinteger(state, surface->width);
    }
    else if (SDL_strcmp(key, "height") == 0)
    {
        lua_pushinteger(state, surface->height);
    }
    else if (SDL_strcmp(key, "scale") == 0)
    {
        lua_pushnumber(state, (lua_Number)surface->scale);
    }
    else
    {
        // the methods are the upvalue
        lua_getfield(state, lua_upvalueindex(1), key);
    }

    return 1;
}

static const luaL_Reg OPENECS_BINDINGS_SURFACE_METHODS[] = {
    {"setPixel", ECSIBindings_SurfaceSetPixel},
    {"getPixel", ECSIBindings_SurfaceGetPixel},
    {"setRow", ECSIBindings_SurfaceSetRow},
    {NULL, NULL},
};

#pragma endregion Surfaces

/// @brief Adds a table of functions to the ecs table on top of the stack. Each function gets the plugin as its upvalue.
static void ECSIBindings_AddTable(lua_State *state, ECSPlugin plugin, const char *name, const luaL_Reg *functions)
{
    lua_newtable(state);
    lua_pushlightuserdata(state, plugin);
    luaL_setfuncs(state, functions, 1);
    lua_setfield(state, -2, name);
}

/// @brief Pushes a new ecs table for a plugin.
static void ECSIBindings_PushEcs(lua_State *state, ECSPlugin plugin)
{
    lua_newtable(state);
    ECSIBindings_AddTable(state, plugin, "log", OPENECS_BINDINGS_LOG);
    ECSIBindings_AddTable(state, plugin, "settings", OPENECS_BINDINGS_SETTINGS);
    ECSIBindings_AddTable(state, plugin, "timer", OPENECS_BINDINGS_TIMER);
    ECSIBindings_AddTable(state, plugin, "panel", OPENECS_BINDINGS_PANEL);
    ECSIBindings_AddTable(state, plugin, "service", OPENECS_BINDINGS_SERVICE);
    ECSIBindings_AddTable(state, plugin, "input", OPENECS_BINDINGS_INPUT);
    ECSIBindings_AddTable(state, plugin, "layout", OPENECS_BINDINGS_LAYOUT);
    ECSIBindings_AddTable(state, plugin, "workspace", OPENECS_BINDINGS_WORKSPACE);
    ECSIBindings_AddTable(state, plugin, "clipboard", OPENECS_BINDINGS_CLIPBOARD);
    ECSIBindings_AddTable(state, plugin, "dialog", OPENECS_BINDINGS_DIALOG);
    ECSIBindings_AddTable(state, plugin, "event", OPENECS_BINDINGS_EVENT);
    ECSIBindings_AddTable(state, plugin, "handle", OPENECS_BINDINGS_HANDLE);

    lua_newtable(state);
    lua_pushstring(state, ECSIPlugin_GetName(plugin));
    lua_setfield(state, -2, "name");
    lua_pushstring(state, ECSIPlugin_GetVersion(plugin));
    lua_setfield(state, -2, "version");
    lua_pushlightuserdata(state, plugin);
    lua_pushcclosure(state, ECSIBindings_PluginRegisterState, 1);
    lua_setfield(state, -2, "registerState");
    lua_pushlightuserdata(state, plugin);
    lua_pushcclosure(state, ECSIBindings_PluginOnShutdown, 1);
    lua_setfield(state, -2, "onShutdown");
    lua_setfield(state, -2, "plugin");
}

#pragma endregion Source Only

void ECSIBindings_Initialize(void)
{
    lua_State *state = ECSILua_GetState();

    luaL_newmetatable(state, OPENECS_LUA_TIMER);
    luaL_newlib(state, OPENECS_BINDINGS_TIMER_METHODS);
    lua_setfield(state, -2, "__index");
    lua_pop(state, 1);

    luaL_newmetatable(state, OPENECS_LUA_SUBSCRIPTION);
    luaL_newlib(state, OPENECS_BINDINGS_SUBSCRIPTION_METHODS);
    lua_setfield(state, -2, "__index");
    lua_pop(state, 1);

    ECSIServices_PushHandleMetatable(OPENECS_LUA_PANEL);
    luaL_newlib(state, OPENECS_BINDINGS_PANEL_METHODS);
    lua_setfield(state, -2, "__index");
    lua_pop(state, 1);

    luaL_newmetatable(state, OPENECS_LUA_SURFACE);
    luaL_newlib(state, OPENECS_BINDINGS_SURFACE_METHODS);
    lua_pushcclosure(state, ECSIBindings_SurfaceIndex, 1);
    lua_setfield(state, -2, "__index");
    lua_pop(state, 1);
}

void ECSIBindings_Terminate(void)
{
    // the types' tables go with the Lua state
    for (usz i = 0; i < arrlenu(BINDINGS.types); i++)
    {
        SDL_free(BINDINGS.types[i]);
    }

    for (usz i = 0; i < arrlenu(BINDINGS.listeners); i++)
    {
        SDL_free(BINDINGS.listeners[i]->name);
        SDL_free(BINDINGS.listeners[i]);
    }

    arrfree(BINDINGS.types);
    arrfree(BINDINGS.listeners);
    SDL_zero(BINDINGS);
}

SHUResult ECSIBindings_StartPlugin(ECSPlugin plugin, const char *path)
{
    SDL_assert(plugin != NULL);
    SDL_assert(path != NULL);

    lua_State *state = ECSILua_GetState();
    int top = lua_gettop(state);

    if (luaL_loadfilex(state, path, "t") != LUA_OK)
    {
        ECS_Log(plugin, ECSLogLevel_Error, "Cannot load '%s': %s", path, lua_tostring(state, -1));
        lua_settop(state, top);
        return SHUResult_ErrFile;
    }

    // the plugin's environment holds its own ecs table, and reads other globals from the shared global table
    lua_newtable(state);
    ECSIBindings_PushEcs(state, plugin);
    lua_setfield(state, -2, "ecs");
    lua_newtable(state);
    lua_pushglobaltable(state);
    lua_setfield(state, -2, "__index");
    lua_setmetatable(state, -2);
    lua_setupvalue(state, -2, 1);

    if (ECSILua_Call(0, 0))
    {
        ECS_Log(plugin, ECSLogLevel_Error, "%s", lua_tostring(state, -1));
        lua_settop(state, top);
        return SHUResult_ErrBadData;
    }

    lua_settop(state, top);
    return SHUResult_Ok;
}
