#include "app/Bindings.h"

#include "app/Workers.h"
#include "base/Lua.h"
#include "base/Timers.h"
#include "interface/Input.h"
#include "interface/Panels.h"
#include "runtime/Events.h"
#include "runtime/Plugins.h"
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
/// @brief Name of the handle type of popups.
#define OPENECS_LUA_POPUP "ecs.popup"

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
    int state;  // registry reference of the value that create returned
    int handle; // registry reference of the panel's handle, which keeps it the same while the panel lives
} ECSILuaPanel;

/// @brief A popup opened from Lua.
typedef struct ECSILuaPopup
{
    ECSPlugin plugin; // the plugin of its panel's type
    ECSPopup popup;
    int description; // registry reference of the description table, which holds the functions
} ECSILuaPopup;

/// @brief A timer started from Lua.
typedef struct ECSILuaTimer
{
    ECSPlugin plugin;
    ECSTimer timer;
    int function; // registry reference of the Lua function
    int handle;   // registry reference of the handle, so it lives while the timer runs
} ECSILuaTimer;

/// @brief The description a Lua plugin registers: registry references of its functions, LUA_NOREF for those it lacks.
typedef struct ECSILuaPlugin
{
    ECSPlugin plugin;
    int saveState;
    int restoreState;
    int destroy;
} ECSILuaPlugin;

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
static const char *const OPENECS_BINDINGS_EVENT_TYPES[] = {"pointerDown", "pointerUp", "pointerMove", "wheel", "keyDown", "keyUp", "focused", "unfocused", "shown", "hidden", "resized", "drop", "text"};

/// @brief Names of the setting types in Lua, in the order of ECSSettingType.
static const char *const OPENECS_BINDINGS_SETTING_TYPES[] = {"bool", "integer", "number", "string", "choice", "key", "list", "table", "color", nullptr};

/// @brief Gets the plugin that owns the ecs table a function came from. Every function of a plugin's ecs table has the plugin as its upvalue.
static ECSPlugin ECSIBindings_GetPlugin(lua_State *state)
{
    return lua_touserdata(state, lua_upvalueindex(1));
}

/// @brief Reads a panel handle. Raises a Lua error if it is not one, or its panel is gone.
static ECSPanel ECSIBindings_CheckPanel(int index)
{
    return ECSIServices_CheckHandle(index, OPENECS_LUA_PANEL);
}

/// @brief Reports the error that a failed call left on top of the stack, in a plugin's name, and pops it.
static void ECSIBindings_ReportError(lua_State *state, ECSPlugin plugin)
{
    ECSIPlugin_ReportError(plugin, lua_tostring(state, -1));
    lua_pop(state, 1);
}

/// @brief Pushes a function field of a table that the registry keeps.
/// @param tableReference The table's registry reference.
/// @return true if the field is a function; nothing is pushed otherwise.
static bool ECSIBindings_PushFunctionField(lua_State *state, int tableReference, const char *name)
{
    lua_rawgeti(state, LUA_REGISTRYINDEX, tableReference);
    bool found = lua_getfield(state, -1, name) == LUA_TFUNCTION;
    lua_remove(state, -2);

    if (!found)
    {
        lua_pop(state, 1);
    }

    return found;
}

/// @brief Pushes the value that a function of the core fills, such as a list, or raises a Lua error when it cannot be made.
static int ECSIBindings_PushFilled(lua_State *state, SHUResult (*Fill)(ECSValue *retValue))
{
    ECSValue *value = nullptr;
    SHUResult result = ECSValue_Create(&value);
    result = result ? result : Fill(value);

    if (result)
    {
        ECSValue_Destroy(&value);
        return luaL_error(state, "the list cannot be made (%s)", SHUResult_String(result));
    }

    ECSILua_PushValue(state, value);
    ECSValue_Destroy(&value);
    return 1;
}

#pragma region Settings

/// @brief Tells a Lua owner that its setting changed, with the new value.
static void ECSIBindings_SettingChanged(void *data)
{
    ECSILuaListener *listener = data;
    lua_State *state = ECSILua_GetState();

    lua_rawgeti(state, LUA_REGISTRYINDEX, listener->function);
    ECSILua_PushValue(state, ECSSetting_Get(listener->name));

    if (ECSILua_Call(state, 1, 0))
    {
        ECSIBindings_ReportError(state, listener->plugin);
    }
}

static int ECSIBindings_SettingDeclare(lua_State *state)
{
    luaL_checktype(state, 1, LUA_TTABLE);
    ECSILuaListener *listener = nullptr;

    // the listener is made first, because the checks below may raise errors and leave nothing to free but it
    if (lua_getfield(state, 1, "changed") == LUA_TFUNCTION)
    {
        lua_getfield(state, 1, "name");
        listener = SDL_calloc(1, sizeof(ECSILuaListener));
        char *name = listener == nullptr ? nullptr : ECSIPlugin_MakeFullName(ECSIBindings_GetPlugin(state), luaL_optstring(state, -1, ""));

        if (name == nullptr)
        {
            SDL_free(listener);
            return luaL_error(state, "the setting needs a local name, without the plugin's name");
        }

        lua_pop(state, 1);
        listener->plugin = ECSIBindings_GetPlugin(state);
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
        .type = (ECSSettingType)luaL_checkoption(state, -4, nullptr, OPENECS_BINDINGS_SETTING_TYPES),
        .description = luaL_optstring(state, -3, ""),
        .Changed = listener == nullptr ? nullptr : ECSIBindings_SettingChanged,
        .data = listener,
    };

    // the choices stay alive in the table on the stack
    const char **choices = nullptr;

    for (lua_Integer i = 1; lua_istable(state, -2) && lua_rawgeti(state, -2, i) == LUA_TSTRING; i++)
    {
        arrput(choices, lua_tostring(state, -1));
        lua_pop(state, 1);
    }

    lua_pop(state, lua_istable(state, -2) ? 1 : 0);
    arrput(choices, nullptr);
    desc.choices = choices;

    ECSValue *defaultValue = nullptr;
    SHUResult result = ECSValue_Create(&defaultValue);
    result = result ? result : ECSILua_GetValue(state, -1, defaultValue);
    bool hasDefault = ECSValue_GetType(defaultValue) != ECSValueType_Nil;
    result = result ? result : ECSISettings_DeclarePlugin(ECSIBindings_GetPlugin(state), &desc, hasDefault ? defaultValue : nullptr);

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

static int ECSIBindings_SettingGet(lua_State *state)
{
    const ECSValue *value = ECSSetting_Get(luaL_checkstring(state, 1));
    ECSILua_PushValue(state, value);
    return 1;
}

static int ECSIBindings_SettingSet(lua_State *state)
{
    const char *name = luaL_checkstring(state, 1);
    ECSValue *value = nullptr;
    SHUResult result = ECSValue_Create(&value);
    result = result ? result : ECSILua_GetValue(state, 2, value);
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

static int ECSIBindings_SettingList(lua_State *state)
{
    return ECSIBindings_PushFilled(state, ECSSetting_List);
}

static int ECSIBindings_SettingExplain(lua_State *state)
{
    const char *name = luaL_checkstring(state, 1);
    ECSValue *explanation = nullptr;
    SHUResult result = ECSValue_Create(&explanation);
    result = result ? result : ECSSetting_Explain(name, explanation);

    if (result)
    {
        ECSValue_Destroy(&explanation);
        lua_pushnil(state);
        lua_pushfstring(state, "no setting '%s' (%s)", name, SHUResult_String(result));
        return 2;
    }

    ECSILua_PushValue(state, explanation);
    ECSValue_Destroy(&explanation);
    return 1;
}

static const luaL_Reg OPENECS_BINDINGS_SETTING[] = {
    {"declare", ECSIBindings_SettingDeclare},
    {"get", ECSIBindings_SettingGet},
    {"set", ECSIBindings_SettingSet},
    {"list", ECSIBindings_SettingList},
    {"explain", ECSIBindings_SettingExplain},
    {nullptr, nullptr},
};

#pragma endregion Settings

#pragma region Timers

/// @brief Clears the handle a Lua value points to, so Lua cannot reach a destroyed object through it.
static void ECSIBindings_ClearHandle(lua_State *state, int reference)
{
    lua_rawgeti(state, LUA_REGISTRYINDEX, reference);
    *(void **)lua_touserdata(state, -1) = nullptr;
    lua_pop(state, 1);
}

static void ECSIBindings_TimerTick(void *data)
{
    ECSILuaTimer *timer = data;
    lua_State *state = ECSILua_GetState();

    lua_rawgeti(state, LUA_REGISTRYINDEX, timer->function);

    if (ECSILua_Call(state, 0, 0))
    {
        ECSIBindings_ReportError(state, timer->plugin);
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

static int ECSIBindings_TimerStart(lua_State *state)
{
    f64 seconds = (f64)luaL_checknumber(state, 1);
    bool repeat = lua_toboolean(state, 2);
    luaL_checktype(state, 3, LUA_TFUNCTION);
    luaL_argcheck(state, seconds > 0.0 || (seconds == 0.0 && !repeat), 1, "must be positive, or 0 for a one-shot timer");

    ECSILuaTimer *timer = SDL_malloc(sizeof(ECSILuaTimer));

    if (timer == nullptr)
    {
        return luaL_error(state, "out of memory");
    }

    ECSILuaTimer **handle = lua_newuserdatauv(state, sizeof(ECSILuaTimer *), 0);
    *handle = timer;
    luaL_setmetatable(state, OPENECS_LUA_TIMER);

    lua_pushvalue(state, 3);
    timer->function = luaL_ref(state, LUA_REGISTRYINDEX);
    lua_pushvalue(state, -1);
    timer->handle = luaL_ref(state, LUA_REGISTRYINDEX);
    timer->plugin = ECSIBindings_GetPlugin(state);

    if (ECSITimers_Start(timer->plugin, &timer->timer, seconds, repeat, ECSIBindings_TimerTick, ECSIBindings_TimerRelease, timer))
    {
        ECSIBindings_TimerRelease(timer);
        return luaL_error(state, "out of memory");
    }

    return 1;
}

static int ECSIBindings_TimerStop(lua_State *state)
{
    ECSILuaTimer **handle = luaL_checkudata(state, 1, OPENECS_LUA_TIMER);

    // a timer that already ended has no timer to stop
    if (*handle != nullptr)
    {
        ECSTimer_Stop(&(*handle)->timer);
    }

    return 0;
}

static const luaL_Reg OPENECS_BINDINGS_TIMER[] = {
    {"start", ECSIBindings_TimerStart},
    {nullptr, nullptr},
};

static const luaL_Reg OPENECS_BINDINGS_TIMER_METHODS[] = {
    {"stop", ECSIBindings_TimerStop},
    {nullptr, nullptr},
};

#pragma endregion Timers

#pragma region Handles

static int ECSIBindings_HandleRegisterType(lua_State *state)
{
    const char *name = luaL_checkstring(state, 1);

    if (ECSIServices_RegisterLuaHandleType(ECSIBindings_GetPlugin(state), name))
    {
        return luaL_error(state, "handle type '%s' cannot be registered", name);
    }

    return 0;
}

static int ECSIBindings_HandleNew(lua_State *state)
{
    const char *name = luaL_checkstring(state, 1);
    luaL_checkany(state, 2);
    ECSIServices_PushLuaHandle(ECSIBindings_GetPlugin(state), name, 2);
    return 1;
}

static int ECSIBindings_HandleValue(lua_State *state)
{
    ECSIServices_PushLuaHandleValue(ECSIBindings_GetPlugin(state), luaL_checkstring(state, 2), 1);
    return 1;
}

static int ECSIBindings_HandleAddMethod(lua_State *state)
{
    const char *type = luaL_checkstring(state, 1);
    const char *method = luaL_checkstring(state, 2);
    const char *function = luaL_checkstring(state, 3);

    if (ECSHandle_AddMethod(ECSIBindings_GetPlugin(state), type, method, function))
    {
        return luaL_error(state, "'%s' cannot be the method '%s' of '%s'", function, method, type);
    }

    return 0;
}

static const luaL_Reg OPENECS_BINDINGS_HANDLE[] = {
    {"registerType", ECSIBindings_HandleRegisterType},
    {"addMethod", ECSIBindings_HandleAddMethod},
    {"new", ECSIBindings_HandleNew},
    {"value", ECSIBindings_HandleValue},
    {nullptr, nullptr},
};

#pragma endregion Handles

#pragma region Plugin

static SHUResult ECSIBindings_PluginSaveState(void *data, ECSValue *retSavedState)
{
    ECSILuaPlugin *luaPlugin = data;
    lua_State *state = ECSILua_GetState();
    lua_rawgeti(state, LUA_REGISTRYINDEX, luaPlugin->saveState);

    if (ECSILua_Call(state, 0, 1))
    {
        ECSIBindings_ReportError(state, luaPlugin->plugin);
        return SHUResult_ErrBadData;
    }

    SHUResult result = ECSILua_GetValue(state, -1, retSavedState);
    lua_pop(state, 1);
    return result;
}

static SHUResult ECSIBindings_PluginRestoreState(void *data, const ECSValue *saved, u32 version)
{
    ECSILuaPlugin *luaPlugin = data;
    lua_State *state = ECSILua_GetState();
    lua_rawgeti(state, LUA_REGISTRYINDEX, luaPlugin->restoreState);
    ECSILua_PushValue(state, saved);
    lua_pushinteger(state, version);

    if (ECSILua_Call(state, 2, 0))
    {
        ECSIBindings_ReportError(state, luaPlugin->plugin);
        return SHUResult_ErrBadData;
    }

    return SHUResult_Ok;
}

static void ECSIBindings_PluginDestroy(void *data)
{
    ECSILuaPlugin *luaPlugin = data;
    lua_State *state = ECSILua_GetState();
    lua_rawgeti(state, LUA_REGISTRYINDEX, luaPlugin->destroy);

    if (ECSILua_Call(state, 0, 0))
    {
        ECSIBindings_ReportError(state, luaPlugin->plugin);
    }
}

static void ECSIBindings_PluginRelease(void *data)
{
    ECSILuaPlugin *luaPlugin = data;
    lua_State *state = ECSILua_GetState();
    luaL_unref(state, LUA_REGISTRYINDEX, luaPlugin->saveState);
    luaL_unref(state, LUA_REGISTRYINDEX, luaPlugin->restoreState);
    luaL_unref(state, LUA_REGISTRYINDEX, luaPlugin->destroy);
    SDL_free(luaPlugin);
}

static int ECSIBindings_PluginRegister(lua_State *state)
{
    luaL_checktype(state, 1, LUA_TTABLE);
    lua_getfield(state, 1, "stateVersion");
    lua_Integer version = luaL_optinteger(state, -1, 0);
    lua_pop(state, 1);
    luaL_argcheck(state, version >= 0 && version <= SDL_MAX_UINT32, 1, "stateVersion must fit in 32 bits");
    ECSILua_CheckFunctionField(state, 1, "saveState");
    ECSILua_CheckFunctionField(state, 1, "restoreState");
    ECSILua_CheckFunctionField(state, 1, "destroy");

    ECSILuaPlugin *luaPlugin = SDL_malloc(sizeof(ECSILuaPlugin));

    if (luaPlugin == nullptr)
    {
        return luaL_error(state, "out of memory");
    }

    *luaPlugin = (ECSILuaPlugin){
        .plugin = ECSIBindings_GetPlugin(state),
        .saveState = ECSILua_ReferenceFunctionField(state, 1, "saveState"),
        .restoreState = ECSILua_ReferenceFunctionField(state, 1, "restoreState"),
        .destroy = ECSILua_ReferenceFunctionField(state, 1, "destroy"),
    };

    ECSPluginDesc desc = {
        .stateVersion = (u32)version,
        .SaveState = luaPlugin->saveState == LUA_NOREF ? nullptr : ECSIBindings_PluginSaveState,
        .RestoreState = luaPlugin->restoreState == LUA_NOREF ? nullptr : ECSIBindings_PluginRestoreState,
        .Destroy = luaPlugin->destroy == LUA_NOREF ? nullptr : ECSIBindings_PluginDestroy,
        .data = luaPlugin,
    };

    if (ECSIPlugin_Register(luaPlugin->plugin, &desc, ECSIBindings_PluginRelease))
    {
        ECSIBindings_PluginRelease(luaPlugin);
        return luaL_error(state, "the plugin registers only one of saveState and restoreState, or registered already");
    }

    return 0;
}

#pragma endregion Plugin

#pragma region Events

/// @brief Calls a Lua subscriber with the event's name and value.
static void ECSIBindings_EventCall(void *data, const char *name, const ECSValue *value)
{
    ECSILuaSubscription *subscription = data;
    lua_State *state = ECSILua_GetState();

    lua_rawgeti(state, LUA_REGISTRYINDEX, subscription->function);
    lua_pushstring(state, name);
    ECSILua_PushValue(state, value);

    if (ECSILua_Call(state, 2, 0))
    {
        ECSIBindings_ReportError(state, subscription->plugin);
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

    if (ECSEvent_Declare(ECSIBindings_GetPlugin(state), name, description))
    {
        return luaL_error(state, "event '%s' cannot be declared", name);
    }

    return 0;
}

static int ECSIBindings_EventEmit(lua_State *state)
{
    const char *name = luaL_checkstring(state, 1);
    ECSValue *value = nullptr;

    SHUResult result = ECSValue_Create(&value);
    result = result ? result : ECSILua_GetValue(state, 2, value);
    result = result ? result : ECSEvent_Emit(ECSIBindings_GetPlugin(state), name, value);
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
    ECSPlugin plugin = ECSIBindings_GetPlugin(state);

    ECSILuaSubscription *subscription = SDL_malloc(sizeof(ECSILuaSubscription));

    if (subscription == nullptr)
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

    SHUResult result = ECSIEvents_Subscribe(plugin, &subscription->subscription, name, ECSIBindings_EventCall, ECSIBindings_EventRelease, subscription);

    if (result)
    {
        ECSIBindings_EventRelease(subscription);
        return luaL_error(state, "cannot subscribe to '%s' (%s)", name, SHUResult_String(result));
    }

    return 1;
}

static int ECSIBindings_EventUnsubscribe(lua_State *state)
{
    ECSILuaSubscription **handle = luaL_checkudata(state, 1, OPENECS_LUA_SUBSCRIPTION);

    // a subscription that already ended has nothing to cancel
    if (*handle != nullptr)
    {
        ECSEvent_Unsubscribe(&(*handle)->subscription);
    }

    return 0;
}

static const luaL_Reg OPENECS_BINDINGS_EVENT[] = {
    {"declare", ECSIBindings_EventDeclare},
    {"emit", ECSIBindings_EventEmit},
    {"subscribe", ECSIBindings_EventSubscribe},
    {nullptr, nullptr},
};

static const luaL_Reg OPENECS_BINDINGS_SUBSCRIPTION_METHODS[] = {
    {"unsubscribe", ECSIBindings_EventUnsubscribe},
    {nullptr, nullptr},
};

#pragma endregion Events

#pragma region Layout

/// @brief Names of the zones in Lua, in the order of ECSZone.
static const char *const OPENECS_BINDINGS_ZONES[] = {"default", "center", "left", "right", "top", "bottom", "window", nullptr};

/// @brief Reads an optional panel handle.
static ECSPanel ECSIBindings_CheckOptionalPanel(lua_State *state, int index)
{
    return lua_isnoneornil(state, index) ? nullptr : ECSIBindings_CheckPanel(index);
}

static int ECSIBindings_LayoutOpen(lua_State *state)
{
    const char *type = luaL_checkstring(state, 1);
    ECSPanel target = ECSIBindings_CheckOptionalPanel(state, 3);
    ECSZone zone = (ECSZone)luaL_checkoption(state, 4, "default", OPENECS_BINDINGS_ZONES);
    ECSValue *saved = nullptr;
    ECSPanel panel = nullptr;

    SHUResult result = ECSValue_Create(&saved);
    result = result ? result : ECSILua_GetValue(state, 2, saved);
    result = result ? result : ECSLayout_Open(ECSIBindings_GetPlugin(state), &panel, type, ECSValue_GetType(saved) == ECSValueType_Nil ? nullptr : saved, target, zone);
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
    ECSPanel panel = ECSIBindings_CheckPanel(1);
    ECSPanel target = ECSIBindings_CheckOptionalPanel(state, 2);
    ECSZone zone = (ECSZone)luaL_checkoption(state, 3, "center", OPENECS_BINDINGS_ZONES);

    // only a pop-out needs no target
    if (target == nullptr && zone != ECSZone_Window)
    {
        return luaL_argerror(state, 2, "a panel is needed, except with the zone \"window\"");
    }

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
    lua_pushboolean(state, ECSLayout_Close(ECSIBindings_CheckPanel(1)));
    return 1;
}

static int ECSIBindings_LayoutFocus(lua_State *state)
{
    (void)state;
    ECSLayout_Focus(ECSIBindings_CheckPanel(1));
    return 0;
}

static int ECSIBindings_LayoutGetFocus(lua_State *state)
{
    (void)state;
    ECSIServices_PushHandle(OPENECS_LUA_PANEL, ECSLayout_GetFocus());
    return 1;
}

static int ECSIBindings_LayoutFindPanel(lua_State *state)
{
    lua_Integer id = luaL_checkinteger(state, 1);
    ECSIServices_PushHandle(OPENECS_LUA_PANEL, id > 0 && id <= SDL_MAX_UINT32 ? ECSLayout_FindPanel((u32)id) : nullptr);
    return 1;
}

static const luaL_Reg OPENECS_BINDINGS_LAYOUT[] = {
    {"findPanel", ECSIBindings_LayoutFindPanel},
    {"open", ECSIBindings_LayoutOpen},
    {"move", ECSIBindings_LayoutMove},
    {"close", ECSIBindings_LayoutClose},
    {"focus", ECSIBindings_LayoutFocus},
    {"getFocus", ECSIBindings_LayoutGetFocus},
    {nullptr, nullptr},
};

static int ECSIBindings_WorkspaceGetCount(lua_State *state)
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
    lua_pushstring(state, number >= 1 ? ECSWorkspace_GetName((usz)number) : nullptr);
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
    {"getCount", ECSIBindings_WorkspaceGetCount},
    {"getCurrent", ECSIBindings_WorkspaceGetCurrent},
    {"getName", ECSIBindings_WorkspaceGetName},
    {"switch", ECSIBindings_WorkspaceSwitch},
    {nullptr, nullptr},
};

#pragma endregion Layout

#pragma region Session

static int ECSIBindings_SessionSave(lua_State *state)
{
    const char *path = luaL_checkstring(state, 1);
    SHUResult result = ECSSession_Save(path);

    if (result)
    {
        lua_pushnil(state);
        lua_pushfstring(state, "the session is not saved to '%s' (%s)", path, SHUResult_String(result));
        return 2;
    }

    lua_pushboolean(state, true);
    return 1;
}

static int ECSIBindings_SessionOpen(lua_State *state)
{
    const char *path = luaL_checkstring(state, 1);
    SHUResult result = ECSSession_Open(path);

    if (result)
    {
        lua_pushnil(state);
        lua_pushfstring(state, "the session '%s' is not opened (%s)", path, result == SHUResult_Err ? "the unsaved work is kept" : SHUResult_String(result));
        return 2;
    }

    lua_pushboolean(state, true);
    return 1;
}

static int ECSIBindings_SessionOpenPreset(lua_State *state)
{
    const char *nameOrPath = luaL_checkstring(state, 1);
    SHUResult result = ECSSession_OpenPreset(nameOrPath);

    if (result)
    {
        lua_pushnil(state);
        lua_pushfstring(state, "the preset '%s' is not opened (%s)", nameOrPath, result == SHUResult_Err ? "the unsaved work is kept" : SHUResult_String(result));
        return 2;
    }

    lua_pushboolean(state, true);
    return 1;
}

static int ECSIBindings_SessionListPresets(lua_State *state)
{
    return ECSIBindings_PushFilled(state, ECSSession_ListPresets);
}

static int ECSIBindings_SessionListSessions(lua_State *state)
{
    return ECSIBindings_PushFilled(state, ECSSession_ListSessions);
}

static const luaL_Reg OPENECS_BINDINGS_SESSION[] = {
    {"save", ECSIBindings_SessionSave},
    {"open", ECSIBindings_SessionOpen},
    {"openPreset", ECSIBindings_SessionOpenPreset},
    {"listPresets", ECSIBindings_SessionListPresets},
    {"listSessions", ECSIBindings_SessionListSessions},
    {nullptr, nullptr},
};

#pragma endregion Session

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
    {nullptr, nullptr},
};

#pragma endregion Clipboard

#pragma region Dialogs

/// @brief Names of the dialog types in Lua, in the order of ECSDialogType.
static const char *const OPENECS_BINDINGS_DIALOG_TYPES[] = {"openFile", "saveFile", "openFolder", nullptr};

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

    if (files == nullptr)
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

    if (ECSILua_Call(state, 1, 0))
    {
        ECSIBindings_ReportError(state, dialog->plugin);
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

    ECSDialogFilter *filters = nullptr;

    for (lua_Integer i = 1; lua_istable(state, -1) && lua_rawgeti(state, -1, i) == LUA_TTABLE; i++)
    {
        lua_getfield(state, -1, "name");
        lua_getfield(state, -2, "pattern");
        ECSDialogFilter filter = {lua_tostring(state, -2), lua_tostring(state, -1)};
        lua_pop(state, 3);

        if (filter.name != nullptr && filter.pattern != nullptr)
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

    if (dialog == nullptr)
    {
        arrfree(filters);
        return luaL_error(state, "out of memory");
    }

    lua_pushvalue(state, 2);
    dialog->function = luaL_ref(state, LUA_REGISTRYINDEX);
    dialog->plugin = ECSIBindings_GetPlugin(state);
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

/// @brief ecs.dialog.showMessage(title, text, { buttons }) gives the position of the pressed button, from 1, or nil.
static int ECSIBindings_DialogShowMessage(lua_State *state)
{
    const char *title = luaL_checkstring(state, 1);
    const char *text = luaL_checkstring(state, 2);
    const char **buttons = nullptr;

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
    {"showMessage", ECSIBindings_DialogShowMessage},
    {nullptr, nullptr},
};

#pragma endregion Dialogs

#pragma region Keys

static int ECSIBindings_KeyBind(lua_State *state)
{
    const char *panelType = luaL_checkstring(state, 1);
    const char *key = luaL_checkstring(state, 2);
    const char *function = luaL_checkstring(state, 3);
    SHUResult result = ECSKey_Bind(ECSIBindings_GetPlugin(state), panelType, key, function);

    // an invalid binding is reported and returned; the plugin continues
    if (result)
    {
        lua_pushnil(state);
        lua_pushfstring(state, "'%s' is not bound to '%s' (%s)", key, function, SHUResult_String(result));
        return 2;
    }

    lua_pushboolean(state, true);
    return 1;
}

static const luaL_Reg OPENECS_BINDINGS_KEY[] = {
    {"bind", ECSIBindings_KeyBind},
    {nullptr, nullptr},
};

#pragma endregion Keys

#pragma region Services

static int ECSIBindings_ServiceGet(lua_State *state)
{
    const char *name = luaL_checkstring(state, 1);
    const char *signature = luaL_optstring(state, 2, nullptr);
    SHUResult result = ECSIServices_PushFunction(ECSIBindings_GetPlugin(state), name, signature);

    // a missing function is an expected failure: nil and a message
    if (result)
    {
        lua_pushnil(state);
        lua_pushfstring(state, "function '%s' is not available (%s)", name, SHUResult_String(result));
        return 2;
    }

    return 1;
}

/// @brief ecs.service.register(functions): functions is a table of { sig = ..., doc = ..., fn = ... } by local name.
static int ECSIBindings_ServiceRegister(lua_State *state)
{
    luaL_checktype(state, 1, LUA_TTABLE);
    ECSPlugin plugin = ECSIBindings_GetPlugin(state);
    const char *prefix = ECSIPlugin_GetName(plugin);
    SHUResult result = SHUResult_Ok;
    const char *failed = nullptr;

    lua_pushnil(state);

    while (lua_next(state, 1) != 0)
    {
        // lua_tostring would change a number key in place and break lua_next, so check the types first
        luaL_argcheck(state, lua_type(state, -2) == LUA_TSTRING && lua_istable(state, -1), 1, "must map local names to { sig, doc, fn } tables");
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
        lua_pushfstring(state, "function '%s.%s' is not registered (%s)", prefix, failed == nullptr ? "?" : failed, SHUResult_String(result));
        return 2;
    }

    lua_pushboolean(state, true);
    return 1;
}

static const luaL_Reg OPENECS_BINDINGS_SERVICE[] = {
    {"register", ECSIBindings_ServiceRegister},
    {"get", ECSIBindings_ServiceGet},
    {nullptr, nullptr},
};

#pragma endregion Services

#pragma region Panels

/// @brief Reads a surface handle; one that its Draw gave is gone once Draw returns.
static ECSSurface *ECSIBindings_CheckSurface(int index)
{
    return ECSIServices_CheckHandle(index, OPENECS_LUA_SURFACE);
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
    ECSHandle_Forget(luaPanel->panel);
    luaL_unref(state, LUA_REGISTRYINDEX, luaPanel->state);
    luaL_unref(state, LUA_REGISTRYINDEX, luaPanel->handle);
    SDL_free(luaPanel);
}

static SHUResult ECSIBindings_PanelCreate(ECSPanel panel, const ECSValue *savedState, u32 version, void **retState)
{
    lua_State *state = ECSILua_GetState();
    ECSILuaPanel *luaPanel = SDL_calloc(1, sizeof(ECSILuaPanel));

    if (luaPanel == nullptr)
    {
        return SHUResult_ErrAllocation;
    }

    luaPanel->type = panel->type->typeData;
    luaPanel->panel = panel;
    luaPanel->state = LUA_NOREF;

    ECSIServices_PushHandle(OPENECS_LUA_PANEL, panel);
    luaPanel->handle = luaL_ref(state, LUA_REGISTRYINDEX);

    if (!ECSIBindings_PushFunctionField(state, luaPanel->type->table, "create"))
    {
        lua_pushnil(state);
    }
    else
    {
        lua_rawgeti(state, LUA_REGISTRYINDEX, luaPanel->handle);
        ECSILua_PushValue(state, savedState);
        lua_pushinteger(state, (lua_Integer)version);

        if (ECSILua_Call(state, 3, 1))
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

    if (ECSIBindings_PushFunctionField(state, luaPanel->type->table, "destroy"))
    {
        lua_rawgeti(state, LUA_REGISTRYINDEX, luaPanel->state);

        if (ECSILua_Call(state, 1, 0))
        {
            ECSIBindings_ReportError(state, luaPanel->type->plugin);
        }
    }

    ECSIBindings_PanelFree(state, luaPanel);
}

static void ECSIBindings_PanelDraw(void *data, ECSSurface *surface, f64 seconds)
{
    ECSILuaPanel *luaPanel = data;
    lua_State *state = ECSILua_GetState();

    if (!ECSIBindings_PushFunctionField(state, luaPanel->type->table, "draw"))
    {
        return;
    }

    // the core forgets the surface's handle once Draw returns
    lua_rawgeti(state, LUA_REGISTRYINDEX, luaPanel->state);
    ECSIServices_PushHandle(OPENECS_LUA_SURFACE, surface);
    lua_pushnumber(state, (lua_Number)seconds);

    if (ECSILua_Call(state, 3, 0))
    {
        ECSIBindings_PanelFailed(state, luaPanel);
    }
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
        lua_pushnumber(state, (lua_Number)event->wheel.scrollX);
        lua_setfield(state, -2, "scrollX");
        lua_pushnumber(state, (lua_Number)event->wheel.scrollY);
        lua_setfield(state, -2, "scrollY");
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
    case ECSPanelEventType_Drop:
        lua_pushnumber(state, (lua_Number)event->drop.x);
        lua_setfield(state, -2, "x");
        lua_pushnumber(state, (lua_Number)event->drop.y);
        lua_setfield(state, -2, "y");
        lua_pushstring(state, ECSDropData_GetType(event->drop.data));
        lua_setfield(state, -2, "dataType");
        ECSILua_PushValue(state, ECSDropData_GetValue(event->drop.data));
        lua_setfield(state, -2, "value");
        break;
    case ECSPanelEventType_Text:
        lua_pushstring(state, event->text.text);
        lua_setfield(state, -2, "text");
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

    if (!ECSIBindings_PushFunctionField(state, luaPanel->type->table, "event"))
    {
        return;
    }

    lua_rawgeti(state, LUA_REGISTRYINDEX, luaPanel->state);
    ECSIBindings_PushEvent(state, event);

    if (ECSILua_Call(state, 2, 0))
    {
        ECSIBindings_PanelFailed(state, luaPanel);
    }
}

static SHUResult ECSIBindings_PanelSaveState(void *data, ECSValue *retSavedState)
{
    ECSILuaPanel *luaPanel = data;
    lua_State *state = ECSILua_GetState();

    if (!ECSIBindings_PushFunctionField(state, luaPanel->type->table, "saveState"))
    {
        return SHUResult_ErrNotFound;
    }

    lua_rawgeti(state, LUA_REGISTRYINDEX, luaPanel->state);

    if (ECSILua_Call(state, 1, 1))
    {
        ECSIBindings_PanelFailed(state, luaPanel);
        return SHUResult_ErrBadData;
    }

    SHUResult result = ECSILua_GetValue(state, -1, retSavedState);
    lua_pop(state, 1);
    return result;
}

static SHUResult ECSIBindings_PanelSave(void *data)
{
    ECSILuaPanel *luaPanel = data;
    lua_State *state = ECSILua_GetState();

    if (!ECSIBindings_PushFunctionField(state, luaPanel->type->table, "save"))
    {
        return SHUResult_ErrNotFound;
    }

    lua_rawgeti(state, LUA_REGISTRYINDEX, luaPanel->state);

    // save returns true, or nil and a message
    if (ECSILua_Call(state, 1, 2))
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
    ECSPlugin plugin = ECSIBindings_GetPlugin(state);

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
        .title = luaL_optstring(state, -6, nullptr),
        .surface = SDL_strcmp(luaL_optstring(state, -5, "pixels"), "gpu") == 0 ? ECSSurfaceType_Gpu : ECSSurfaceType_Pixels,
        .stateVersion = (u32)luaL_optinteger(state, -4, 0),
        .continuous = lua_toboolean(state, -3),
        .minWidth = (f32)luaL_optnumber(state, -2, 0.0),
        .minHeight = (f32)luaL_optnumber(state, -1, 0.0),
        .Create = ECSIBindings_PanelCreate,
        .Destroy = ECSIBindings_PanelDestroy,
        .Draw = ECSIBindings_HasFunction(state, 1, "draw") ? ECSIBindings_PanelDraw : nullptr,
        .Event = ECSIBindings_HasFunction(state, 1, "event") ? ECSIBindings_PanelEvent : nullptr,
        .SaveState = ECSIBindings_HasFunction(state, 1, "saveState") ? ECSIBindings_PanelSaveState : nullptr,
        .Save = ECSIBindings_HasFunction(state, 1, "save") ? ECSIBindings_PanelSave : nullptr,
    };

    ECSILuaPanelType *type = SDL_malloc(sizeof(ECSILuaPanelType));

    if (type == nullptr)
    {
        return luaL_error(state, "out of memory");
    }

    lua_pushvalue(state, 1);
    type->table = luaL_ref(state, LUA_REGISTRYINDEX);
    type->plugin = plugin;
    SHUResult result = ECSIPanelType_Register(plugin, &desc, type);

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
    (void)state;
    ECSPanel_Redraw(ECSIBindings_CheckPanel(1));
    return 0;
}

/// @brief Reads a field of a panel: its title, id, type and unsaved mark, or one of its methods, which are the upvalue.
static int ECSIBindings_PanelIndex(lua_State *state)
{
    ECSPanel panel = ECSIBindings_CheckPanel(1);
    const char *key = luaL_checkstring(state, 2);

    if (SDL_strcmp(key, "title") == 0)
    {
        lua_pushstring(state, ECSPanel_GetTitle(panel));
    }
    else if (SDL_strcmp(key, "id") == 0)
    {
        lua_pushinteger(state, ECSPanel_GetID(panel));
    }
    else if (SDL_strcmp(key, "type") == 0)
    {
        lua_pushstring(state, ECSPanel_GetType(panel));
    }
    else if (SDL_strcmp(key, "unsaved") == 0)
    {
        lua_pushboolean(state, ECSPanel_GetUnsaved(panel));
    }
    else
    {
        lua_getfield(state, lua_upvalueindex(1), key);
    }

    return 1;
}

/// @brief Sets a field of a panel: its title or its unsaved mark; the others cannot be set.
static int ECSIBindings_PanelNewIndex(lua_State *state)
{
    ECSPanel panel = ECSIBindings_CheckPanel(1);
    const char *key = luaL_checkstring(state, 2);

    if (SDL_strcmp(key, "title") == 0)
    {
        ECSPanel_SetTitle(panel, luaL_checkstring(state, 3));
    }
    else if (SDL_strcmp(key, "unsaved") == 0)
    {
        ECSPanel_SetUnsaved(panel, lua_toboolean(state, 3));
    }
    else
    {
        return luaL_error(state, "a panel's field '%s' cannot be set; only title and unsaved can", key);
    }

    return 0;
}

static int ECSIBindings_PanelAcceptDrops(lua_State *state)
{
    ECSPanel panel = ECSIBindings_CheckPanel(1);
    luaL_checktype(state, 2, LUA_TTABLE);

    // the table keeps its strings alive while the core copies them
    const char **types = nullptr;

    for (lua_Integer i = 1; lua_rawgeti(state, 2, i) != LUA_TNIL; i++)
    {
        if (lua_type(state, -1) != LUA_TSTRING)
        {
            arrfree(types);
            return luaL_argerror(state, 2, "the types must be a list of strings");
        }

        arrput(types, lua_tostring(state, -1));
        lua_pop(state, 1);
    }

    SHUResult result = ECSPanel_AcceptDrops(panel, types, arrlenu(types));
    arrfree(types);

    if (result)
    {
        return luaL_error(state, "the types are not set (%s)", SHUResult_String(result));
    }

    return 0;
}

static int ECSIBindings_PanelSetTextInput(lua_State *state)
{
    ECSPanel panel = ECSIBindings_CheckPanel(1);
    bool accept = lua_toboolean(state, 2);
    f32 x = (f32)luaL_optnumber(state, 3, 0.0);
    f32 y = (f32)luaL_optnumber(state, 4, 0.0);
    f32 width = (f32)luaL_optnumber(state, 5, 0.0);
    f32 height = (f32)luaL_optnumber(state, 6, 0.0);
    ECSPanel_SetTextInput(panel, accept, x, y, width, height);
    return 0;
}

static int ECSIBindings_PanelStartDrag(lua_State *state)
{
    ECSPanel panel = ECSIBindings_CheckPanel(1);
    const char *type = luaL_checkstring(state, 2);
    ECSValue *value = nullptr;
    SHUResult result = ECSValue_Create(&value);
    result = result ? result : ECSILua_GetValue(state, 3, value);
    result = result ? result : ECSPanel_StartDrag(panel, type, value);
    ECSValue_Destroy(&value);

    if (result)
    {
        lua_pushnil(state);
        lua_pushfstring(state, "no drag of '%s' (%s)", type, result == SHUResult_Err ? "no pointer button pressed on the panel is held" : SHUResult_String(result));
        return 2;
    }

    lua_pushboolean(state, true);
    return 1;
}

#pragma region Popups

/// @brief Names of the popup types in Lua, in the order of ECSPopupType.
static const char *const OPENECS_BINDINGS_POPUP_TYPES[] = {"menu", "tooltip", nullptr};

/// @brief Reads a popup handle. Raises a Lua error if it is not one, or its popup has closed.
static ECSPopup ECSIBindings_CheckPopup(int index)
{
    return ECSIServices_CheckHandle(index, OPENECS_LUA_POPUP);
}

/// @brief Reports the error on top of the stack, pops it, and closes the popup.
static void ECSIBindings_PopupFailed(lua_State *state, const ECSILuaPopup *luaPopup)
{
    ECSIBindings_ReportError(state, luaPopup->plugin);

    // the copy is set to nullptr; the popup's handle stays until its closed function forgets it
    ECSPopup popup = luaPopup->popup;
    ECSPopup_Close(&popup);
}

static void ECSIBindings_PopupDraw(void *data, ECSSurface *surface)
{
    ECSILuaPopup *luaPopup = data;
    lua_State *state = ECSILua_GetState();

    if (!ECSIBindings_PushFunctionField(state, luaPopup->description, "draw"))
    {
        return;
    }

    // the core forgets the surface's handle once Draw returns
    ECSIServices_PushHandle(OPENECS_LUA_SURFACE, surface);

    if (ECSILua_Call(state, 1, 0))
    {
        ECSIBindings_PopupFailed(state, luaPopup);
    }
}

static void ECSIBindings_PopupEvent(void *data, const ECSPanelEvent *event)
{
    ECSILuaPopup *luaPopup = data;
    lua_State *state = ECSILua_GetState();

    if (!ECSIBindings_PushFunctionField(state, luaPopup->description, "event"))
    {
        return;
    }

    ECSIBindings_PushEvent(state, event);

    if (ECSILua_Call(state, 1, 0))
    {
        ECSIBindings_PopupFailed(state, luaPopup);
    }
}

/// @brief Runs a Lua popup's closed function, then forgets the popup.
static void ECSIBindings_PopupClosed(void *data)
{
    ECSILuaPopup *luaPopup = data;
    lua_State *state = ECSILua_GetState();

    if (ECSIBindings_PushFunctionField(state, luaPopup->description, "closed") && ECSILua_Call(state, 0, 0))
    {
        ECSIBindings_ReportError(state, luaPopup->plugin);
    }

    ECSHandle_Forget(luaPopup->popup);
    luaL_unref(state, LUA_REGISTRYINDEX, luaPopup->description);
    SDL_free(luaPopup);
}

/// @brief Reads a number field of the table at an index, or a default if it is missing.
static f32 ECSIBindings_GetOptionalNumberField(lua_State *state, int index, const char *name, f32 fallback)
{
    lua_getfield(state, index, name);
    f32 number = lua_isnil(state, -1) ? fallback : (f32)luaL_checknumber(state, -1);
    lua_pop(state, 1);
    return number;
}

static int ECSIBindings_PanelOpenPopup(lua_State *state)
{
    ECSPanel panel = ECSIBindings_CheckPanel(1);
    luaL_argcheck(state, panel->type != nullptr, 1, "the panel has no type");
    luaL_checktype(state, 2, LUA_TTABLE);

    lua_getfield(state, 2, "type");
    ECSPopupType type = (ECSPopupType)luaL_checkoption(state, -1, "menu", OPENECS_BINDINGS_POPUP_TYPES);
    lua_pop(state, 1);

    ECSPopupDesc desc = {
        .type = type,
        .width = ECSIBindings_GetOptionalNumberField(state, 2, "width", 0.0f),
        .height = ECSIBindings_GetOptionalNumberField(state, 2, "height", 0.0f),
        .Draw = ECSIBindings_PopupDraw,
        .Event = ECSIBindings_PopupEvent,
        .Closed = ECSIBindings_PopupClosed,
    };

    if (lua_getfield(state, 2, "anchor") == LUA_TTABLE)
    {
        int anchor = lua_gettop(state);
        desc.anchorX = ECSIBindings_GetOptionalNumberField(state, anchor, "x", 0.0f);
        desc.anchorY = ECSIBindings_GetOptionalNumberField(state, anchor, "y", 0.0f);
        desc.anchorWidth = ECSIBindings_GetOptionalNumberField(state, anchor, "width", 0.0f);
        desc.anchorHeight = ECSIBindings_GetOptionalNumberField(state, anchor, "height", 0.0f);
    }

    lua_pop(state, 1);
    lua_getfield(state, 2, "draw");
    luaL_argcheck(state, lua_isfunction(state, -1), 2, "the popup needs a draw function");
    lua_pop(state, 1);

    ECSILuaPopup *luaPopup = SDL_calloc(1, sizeof(ECSILuaPopup));

    if (luaPopup == nullptr)
    {
        return luaL_error(state, "out of memory");
    }

    lua_pushvalue(state, 2);
    luaPopup->description = luaL_ref(state, LUA_REGISTRYINDEX);
    luaPopup->plugin = panel->type->plugin;
    desc.data = luaPopup;
    SHUResult result = ECSPopup_Open(panel, &luaPopup->popup, &desc);

    if (result)
    {
        luaL_unref(state, LUA_REGISTRYINDEX, luaPopup->description);
        SDL_free(luaPopup);
        lua_pushnil(state);
        lua_pushfstring(state, "the popup is not opened (%s)", result == SHUResult_ErrNotFound ? "the panel is not shown" : result == SHUResult_ErrBadData ? "it needs a width and a height above 0"
                                                                                                                                                           : SHUResult_String(result));
        return 2;
    }

    ECSIServices_PushHandle(OPENECS_LUA_POPUP, luaPopup->popup);
    return 1;
}

static int ECSIBindings_PopupClose(lua_State *state)
{
    (void)state;
    ECSPopup popup = ECSIBindings_CheckPopup(1);
    ECSPopup_Close(&popup);
    return 0;
}

static int ECSIBindings_PopupRedraw(lua_State *state)
{
    (void)state;
    ECSPopup_Redraw(ECSIBindings_CheckPopup(1));
    return 0;
}

static int ECSIBindings_PopupSetSize(lua_State *state)
{
    ECSPopup popup = ECSIBindings_CheckPopup(1);
    f32 width = (f32)luaL_checknumber(state, 2);
    f32 height = (f32)luaL_checknumber(state, 3);
    luaL_argcheck(state, width > 0.0f && height > 0.0f, 2, "the size must be above 0");
    ECSPopup_SetSize(popup, width, height);
    return 0;
}

static const luaL_Reg OPENECS_BINDINGS_POPUP[] = {
    {"close", ECSIBindings_PopupClose},
    {"redraw", ECSIBindings_PopupRedraw},
    {"setSize", ECSIBindings_PopupSetSize},
    {nullptr, nullptr},
};

#pragma endregion Popups

static const luaL_Reg OPENECS_BINDINGS_PANEL_METHODS[] = {
    {"openPopup", ECSIBindings_PanelOpenPopup},
    {"setTextInput", ECSIBindings_PanelSetTextInput},
    {"acceptDrops", ECSIBindings_PanelAcceptDrops},
    {"startDrag", ECSIBindings_PanelStartDrag},
    {"redraw", ECSIBindings_PanelRedraw},
    {nullptr, nullptr},
};

static int ECSIBindings_PanelAddMenuEntry(lua_State *state)
{
    const char *type = luaL_checkstring(state, 1);
    const char *function = luaL_checkstring(state, 2);

    // an invalid entry is reported and returned; the plugin continues
    if (ECSPanelType_AddMenuEntry(ECSIBindings_GetPlugin(state), type, function))
    {
        lua_pushnil(state);
        lua_pushfstring(state, "'%s' is not added to the menu of '%s'", function, type);
        return 2;
    }

    lua_pushboolean(state, true);
    return 1;
}

static int ECSIBindings_PanelListTypes(lua_State *state)
{
    return ECSIBindings_PushFilled(state, ECSPanelType_List);
}

static const luaL_Reg OPENECS_BINDINGS_PANEL[] = {
    {"listTypes", ECSIBindings_PanelListTypes},
    {"registerType", ECSIBindings_PanelRegisterType},
    {"addMenuEntry", ECSIBindings_PanelAddMenuEntry},
    {nullptr, nullptr},
};

#pragma endregion Panels

#pragma region Surfaces

/// @brief Gets a pixel's address, or nullptr if the position is outside the surface.
static u32 *ECSIBindings_GetPixelAddress(ECSSurface *surface, lua_Integer x, lua_Integer y)
{
    if (x < 0 || y < 0 || x >= surface->width || y >= surface->height)
    {
        return nullptr;
    }

    return (u32 *)((u8 *)surface->pixels.data + (usz)y * (usz)surface->pitch) + x;
}

/// @brief Gets the surface at an index for a method that reads or writes its pixels, and raises a Lua error for a GPU surface, which has none.
static ECSSurface *ECSIBindings_CheckPixelSurface(lua_State *state, int index)
{
    ECSSurface *surface = ECSIBindings_CheckSurface(index);

    if (surface->type != ECSSurfaceType_Pixels)
    {
        luaL_error(state, "a GPU surface has no pixels; the gpu plugin draws into it");
    }

    return surface;
}

static int ECSIBindings_SurfaceSetPixel(lua_State *state)
{
    ECSSurface *surface = ECSIBindings_CheckPixelSurface(state, 1);
    u32 *pixel = ECSIBindings_GetPixelAddress(surface, luaL_checkinteger(state, 2), luaL_checkinteger(state, 3));

    ECSColor color = 0;
    luaL_argcheck(state, ECSColor_Parse(luaL_checkstring(state, 4), &color), 4, "is not a colour: \"#RRGGBB\", \"#RRGGBBAA\" or a colour of the theme");

    // pixels outside the surface are clipped; surfaces hold premultiplied colours
    if (pixel != nullptr)
    {
        *pixel = ECSIColor_Premultiply(color);
    }

    return 0;
}

static int ECSIBindings_SurfaceGetPixel(lua_State *state)
{
    ECSSurface *surface = ECSIBindings_CheckPixelSurface(state, 1);
    u32 *pixel = ECSIBindings_GetPixelAddress(surface, luaL_checkinteger(state, 2), luaL_checkinteger(state, 3));

    if (pixel == nullptr)
    {
        return luaL_argerror(state, 2, "the position is outside the surface");
    }

    char text[OPENECS_COLOR_TEXT_SIZE];
    ECSIColor_Format(ECSIColor_Unpremultiply(*pixel), text);
    lua_pushstring(state, text);
    return 1;
}

static int ECSIBindings_SurfaceSetRow(lua_State *state)
{
    ECSSurface *surface = ECSIBindings_CheckPixelSurface(state, 1);
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

    u32 *first = ECSIBindings_GetPixelAddress(surface, x + skip, y);

    if (count > 0 && first != nullptr)
    {
        SDL_memcpy(first, bytes + skip * 4, (usz)count * 4);
    }

    return 0;
}

static int ECSIBindings_SurfaceFill(lua_State *state)
{
    ECSSurface *surface = ECSIBindings_CheckPixelSurface(state, 1);
    lua_Integer values[4];

    // the rectangle is clipped to the surface, so larger numbers are clamped first
    for (int i = 0; i < 4; i++)
    {
        values[i] = SDL_clamp(luaL_checkinteger(state, i + 2), -(lua_Integer)SDL_MAX_SINT32, (lua_Integer)SDL_MAX_SINT32);
    }

    ECSColor color = 0;
    luaL_argcheck(state, ECSColor_Parse(luaL_checkstring(state, 6), &color), 6, "is not a colour: \"#RRGGBB\", \"#RRGGBBAA\" or a colour of the theme");
    ECSSurface_Fill(surface, (i32)values[0], (i32)values[1], (i32)values[2], (i32)values[3], color);
    return 0;
}

static int ECSIBindings_SurfaceIndex(lua_State *state)
{
    // the methods are the upvalue; they come first, as a panel's draw calls them for many pixels, and check the surface themselves
    if (lua_getfield(state, lua_upvalueindex(1), luaL_checkstring(state, 2)) != LUA_TNIL)
    {
        return 1;
    }

    ECSSurface *surface = ECSIBindings_CheckSurface(1);
    const char *key = lua_tostring(state, 2);

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
    else if (SDL_strcmp(key, "type") == 0)
    {
        lua_pushstring(state, surface->type == ECSSurfaceType_Gpu ? "gpu" : "pixels");
    }
    else
    {
        lua_pushnil(state);
    }

    return 1;
}

static const luaL_Reg OPENECS_BINDINGS_SURFACE_METHODS[] = {
    {"setPixel", ECSIBindings_SurfaceSetPixel},
    {"getPixel", ECSIBindings_SurfaceGetPixel},
    {"setRow", ECSIBindings_SurfaceSetRow},
    {"fill", ECSIBindings_SurfaceFill},
    {nullptr, nullptr},
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

/// @brief Runs one of the core's functions as a key runs it; its name is the upvalue.
static int ECSIBindings_RunCore(lua_State *state)
{
    ECSIServices_CallBound(lua_tostring(state, lua_upvalueindex(1)), ECSLayout_GetFocus());
    return 0;
}

/// @brief Adds one of the core's functions to the ecs table on top of the stack, at the path of its name: ecs.layout.maximize goes into the table layout.
static void ECSIBindings_AddCore(const char *name, void *data)
{
    lua_State *state = data;
    int top = lua_gettop(state);
    const char *part = name + SDL_strlen("ecs.");
    const char *dot = nullptr;

    // the tables on the way are made when they are missing
    while ((dot = SDL_strchr(part, '.')) != nullptr)
    {
        lua_pushlstring(state, part, (usz)(dot - part));

        if (lua_rawget(state, -2) != LUA_TTABLE)
        {
            lua_pop(state, 1);
            lua_newtable(state);
            lua_pushlstring(state, part, (usz)(dot - part));
            lua_pushvalue(state, -2);
            lua_rawset(state, -4);
        }

        part = dot + 1;
    }

    // a function the bindings define, such as ecs.layout.close(panel), stays
    if (lua_getfield(state, -1, part) == LUA_TNIL)
    {
        lua_pushstring(state, name);
        lua_pushcclosure(state, ECSIBindings_RunCore, 1);
        lua_setfield(state, -3, part);
    }

    lua_settop(state, top);
}

/// @brief Pushes a new ecs table for a plugin.
static void ECSIBindings_PushEcs(lua_State *state, ECSPlugin plugin)
{
    lua_newtable(state);
    ECSIWorkers_PushLog(state, plugin);
    lua_setfield(state, -2, "log");
    ECSIBindings_AddTable(state, plugin, "setting", OPENECS_BINDINGS_SETTING);
    ECSIBindings_AddTable(state, plugin, "timer", OPENECS_BINDINGS_TIMER);
    ECSIBindings_AddTable(state, plugin, "panel", OPENECS_BINDINGS_PANEL);
    ECSIBindings_AddTable(state, plugin, "service", OPENECS_BINDINGS_SERVICE);
    ECSIBindings_AddTable(state, plugin, "key", OPENECS_BINDINGS_KEY);
    ECSIBindings_AddTable(state, plugin, "layout", OPENECS_BINDINGS_LAYOUT);
    ECSIBindings_AddTable(state, plugin, "workspace", OPENECS_BINDINGS_WORKSPACE);
    ECSIBindings_AddTable(state, plugin, "session", OPENECS_BINDINGS_SESSION);
    ECSIBindings_AddTable(state, plugin, "clipboard", OPENECS_BINDINGS_CLIPBOARD);
    ECSIBindings_AddTable(state, plugin, "dialog", OPENECS_BINDINGS_DIALOG);
    ECSIBindings_AddTable(state, plugin, "event", OPENECS_BINDINGS_EVENT);
    ECSIBindings_AddTable(state, plugin, "handle", OPENECS_BINDINGS_HANDLE);
    ECSIWorkers_PushTable(state, plugin);
    lua_setfield(state, -2, "worker");

    lua_newtable(state);
    lua_pushstring(state, ECSIPlugin_GetName(plugin));
    lua_setfield(state, -2, "name");
    lua_pushstring(state, ECSIPlugin_GetVersion(plugin));
    lua_setfield(state, -2, "version");
    lua_pushstring(state, ECSPlugin_GetFolder(plugin));
    lua_setfield(state, -2, "folder");
    lua_pushlightuserdata(state, plugin);
    lua_pushcclosure(state, ECSIBindings_PluginRegister, 1);
    lua_setfield(state, -2, "register");
    lua_setfield(state, -2, "plugin");

    // every function that keys can run is a function of the module with the same name
    ECSIServices_ForEachCore(ECSIBindings_AddCore, state);
}

/// @brief The require of a plugin's environment. Its upvalues are the plugin's ecs table, its environment, its folder, the table of its loaded modules and the plugin.
/// It gives the ecs table for "ecs". It runs a module of the plugin's folder once, in the plugin's environment: "parts.shapes" is parts/shapes.lua or parts/shapes/init.lua.
/// A plugin's name gives a table of that plugin's functions (DESIGN 10.5). Lua's require gives other modules.
static int ECSIBindings_Require(lua_State *state)
{
    const char *name = luaL_checkstring(state, 1);
    lua_settop(state, 1);

    if (SDL_strcmp(name, "ecs") == 0)
    {
        lua_pushvalue(state, lua_upvalueindex(1));
        return 1;
    }

    if (lua_getfield(state, lua_upvalueindex(4), name) != LUA_TNIL)
    {
        return 1;
    }

    // paths are built on the Lua stack, so an error frees them
    const char *folder = lua_tostring(state, lua_upvalueindex(3));
    const char *module = luaL_gsub(state, name, ".", "/");
    const char *path = lua_pushfstring(state, "%s%s.lua", folder, module);

    if (!SDL_GetPathInfo(path, nullptr))
    {
        path = lua_pushfstring(state, "%s%s/init.lua", folder, module);
    }

    if (SDL_GetPathInfo(path, nullptr))
    {
        if (luaL_loadfilex(state, path, "t") != LUA_OK)
        {
            return lua_error(state);
        }

        lua_pushvalue(state, lua_upvalueindex(2));
        lua_setupvalue(state, -2, 1);
        lua_pushvalue(state, 1);
        lua_pushstring(state, path);
        lua_call(state, 2, 1);

        // like Lua's require, a module that returns nothing gives true
        if (lua_isnil(state, -1))
        {
            lua_pop(state, 1);
            lua_pushboolean(state, true);
        }

        lua_pushvalue(state, -1);
        lua_setfield(state, lua_upvalueindex(4), name);
        return 1;
    }

    // a plugin's name gives its functions, by local name
    lua_settop(state, 1);
    ECSPlugin provider = ECSIPlugins_Get(name);

    if (provider != nullptr)
    {
        SHUResult result = ECSIServices_PushPlugin(lua_touserdata(state, lua_upvalueindex(5)), provider);
        return result ? luaL_error(state, "the functions of '%s' are not available (%s)", name, SHUResult_String(result)) : 1;
    }

    lua_getglobal(state, "require");
    lua_insert(state, 1);
    lua_call(state, lua_gettop(state) - 1, LUA_MULTRET);
    return lua_gettop(state);
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

    // a panel's data are fields, and what it does are methods
    ECSIServices_PushHandleMetatable(OPENECS_LUA_PANEL);
    luaL_newlib(state, OPENECS_BINDINGS_PANEL_METHODS);
    lua_pushcclosure(state, ECSIBindings_PanelIndex, 1);
    lua_setfield(state, -2, "__index");
    lua_pushcfunction(state, ECSIBindings_PanelNewIndex);
    lua_setfield(state, -2, "__newindex");
    lua_pop(state, 1);

    ECSIServices_PushHandleMetatable(OPENECS_LUA_POPUP);
    luaL_newlib(state, OPENECS_BINDINGS_POPUP);
    lua_setfield(state, -2, "__index");
    lua_pop(state, 1);

    ECSIServices_PushHandleMetatable(OPENECS_LUA_SURFACE);
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

SHUResult ECSIBindings_StartPlugin(ECSPlugin plugin, const char *folder, const char *path)
{
    SDL_assert(plugin != nullptr);
    SDL_assert(folder != nullptr);
    SDL_assert(path != nullptr);

    lua_State *state = ECSILua_GetState();
    int top = lua_gettop(state);

    if (luaL_loadfilex(state, path, "t") != LUA_OK)
    {
        ECS_Log(plugin, ECSLogLevel_Error, "Cannot load '%s': %s", path, lua_tostring(state, -1));
        lua_settop(state, top);
        return SHUResult_ErrFile;
    }

    // the plugin's environment has its own require, which gives the plugin's ecs table and modules; it reads other globals from the shared global table
    lua_newtable(state);
    ECSIBindings_PushEcs(state, plugin);
    lua_pushvalue(state, -2);
    lua_pushstring(state, folder);
    lua_newtable(state);
    lua_pushlightuserdata(state, plugin);
    lua_pushcclosure(state, ECSIBindings_Require, 5);
    lua_setfield(state, -2, "require");
    lua_newtable(state);
    lua_pushglobaltable(state);
    lua_setfield(state, -2, "__index");
    lua_setmetatable(state, -2);
    lua_setupvalue(state, -2, 1);

    if (ECSILua_Call(state, 0, 0))
    {
        ECS_Log(plugin, ECSLogLevel_Error, "%s", lua_tostring(state, -1));
        lua_settop(state, top);
        return SHUResult_ErrBadData;
    }

    lua_settop(state, top);
    return SHUResult_Ok;
}
