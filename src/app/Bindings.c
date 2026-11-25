#include "app/Bindings.h"

#include "app/Workers.h"
#include "base/Lua.h"
#include "interface/Input.h"
#include "interface/Panels.h"
#include "runtime/Events.h"
#include "runtime/Plugins.h"
#include "runtime/Services.h"
#include "runtime/Settings.h"

#include "lua/compat-5.3.h"

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
static const char *const OPENECS_BINDINGS_SETTING_TYPES[] = {"bool", "integer", "number", "string", "choice", "key", "list", "table", "color", "path", nullptr};

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
        ECSIServices_Raise(state, result, "the list cannot be made");
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
    ECSILua_CheckType(state, 1, LUA_TTABLE);
    ECSILuaListener *listener = nullptr;

    // the listener is made first, because the checks below may raise errors and leave nothing to free but it
    if (lua_getfield(state, 1, "changed") == LUA_TFUNCTION)
    {
        lua_getfield(state, 1, "name");
        listener = SDL_calloc(1, sizeof(ECSILuaListener));
        char *name = nullptr;
        SHUResult made = listener == nullptr ? SHUResult_ErrAllocation : ECSIPlugin_MakeFullName(ECSIBindings_GetPlugin(state), ECSILua_OptString(state, -1, ""), &name);

        if (made)
        {
            SDL_free(listener);
            ECSIServices_Raise(state, made, "the setting needs a local name, without the plugin's name");
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
        .name = ECSILua_CheckString(state, -5),
        .type = (ECSSettingType)ECSILua_CheckOption(state, -4, nullptr, OPENECS_BINDINGS_SETTING_TYPES),
        .description = ECSILua_OptString(state, -3, ""),
        .Changed = listener == nullptr ? nullptr : ECSIBindings_SettingChanged,
        .data = listener,
    };

    // the choices stay alive in the table on the stack
    const char **choices = nullptr;

    if (lua_istable(state, -2))
    {
        for (int i = 1; lua_rawgeti(state, -2, i) == LUA_TSTRING; i++)
        {
            arrput(choices, lua_tostring(state, -1));
            lua_pop(state, 1);
        }

        lua_pop(state, 1); // the value after the last choice, so the default is on top again
    }
    arrput(choices, nullptr);
    desc.choices = choices;

    ECSValue *defaultValue = nullptr;
    SHUResult result = ECSValue_Create(&defaultValue);
    result = result ? result : ECSILua_GetValue(state, -1, defaultValue);
    bool hasDefault = ECSValue_GetType(defaultValue) != ECSValueType_Nil;
    result = result ? result : ECSISettings_DeclarePlugin(ECSIBindings_GetPlugin(state), &desc, hasDefault ? defaultValue : nullptr);

    ECSValue_Destroy(&defaultValue);
    arrfree(choices);

    if (result)
    {
        ECSIServices_Raise(state, result, "setting '%s' is not declared", desc.name);
    }

    return 0;
}

static int ECSIBindings_SettingGet(lua_State *state)
{
    const ECSValue *value = ECSSetting_Get(ECSILua_CheckString(state, 1));
    ECSILua_PushValue(state, value);
    return 1;
}

static int ECSIBindings_SettingSet(lua_State *state)
{
    const char *name = ECSILua_CheckString(state, 1);
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

static int ECSIBindings_SettingSetForRun(lua_State *state)
{
    const char *name = ECSILua_CheckString(state, 1);
    ECSValue *value = nullptr;
    SHUResult result = ECSValue_Create(&value);
    result = result ? result : ECSILua_GetValue(state, 2, value);
    result = result ? result : ECSSetting_SetForRun(name, value);
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
    const char *name = ECSILua_CheckString(state, 1);
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
    {"setForRun", ECSIBindings_SettingSetForRun},
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
    f64 seconds = (f64)ECSILua_CheckNumber(state, 1);
    bool repeat = lua_toboolean(state, 2);
    ECSILua_CheckType(state, 3, LUA_TFUNCTION);
    ECSILua_ArgCheck(state, seconds > 0.0 || (seconds == 0.0 && !repeat), 1, "must be positive, or 0 for a one-shot timer");

    ECSILuaTimer *timer = SDL_malloc(sizeof(ECSILuaTimer));

    if (timer == nullptr)
    {
        return luaL_error(state, "out of memory");
    }

    ECSILuaTimer **handle = lua_newuserdata(state, sizeof(ECSILuaTimer *));
    *handle = timer;
    luaL_setmetatable(state, OPENECS_LUA_TIMER);

    lua_pushvalue(state, 3);
    timer->function = luaL_ref(state, LUA_REGISTRYINDEX);
    lua_pushvalue(state, -1);
    timer->handle = luaL_ref(state, LUA_REGISTRYINDEX);
    timer->plugin = ECSIBindings_GetPlugin(state);

    if (ECSIEvents_StartTimer(timer->plugin, &timer->timer, seconds, repeat, ECSIBindings_TimerTick, ECSIBindings_TimerRelease, timer))
    {
        ECSIBindings_TimerRelease(timer);
        return luaL_error(state, "out of memory");
    }

    return 1;
}

static int ECSIBindings_TimerStop(lua_State *state)
{
    ECSILuaTimer **handle = ECSILua_CheckUdata(state, 1, OPENECS_LUA_TIMER);

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
    const char *name = ECSILua_CheckString(state, 1);

    SHUResult result = ECSIServices_RegisterLuaHandleType(ECSIBindings_GetPlugin(state), name);

    if (result)
    {
        ECSIServices_Raise(state, result, "handle type '%s' cannot be registered", name);
    }

    return 0;
}

/// @brief Pushes the full name of one of the calling plugin's handle types, from the local name at an argument; Lua keeps the text while it is on the stack.
static const char *ECSIBindings_PushTypeName(lua_State *state, int argument)
{
    char *fullName = nullptr;
    SHUResult result = ECSIPlugin_MakeFullName(ECSIBindings_GetPlugin(state), ECSILua_CheckString(state, argument), &fullName);

    if (result)
    {
        ECSIServices_Raise(state, result, "must be the local name of one of the plugin's handle types, such as \"brush\"");
    }

    const char *pushed = lua_pushstring(state, fullName);
    SDL_free(fullName);
    return pushed;
}

static int ECSIBindings_HandleNew(lua_State *state)
{
    ECSILua_CheckAny(state, 2);
    ECSIServices_PushLuaHandle(ECSIBindings_GetPlugin(state), ECSIBindings_PushTypeName(state, 1), 2);
    return 1;
}

static int ECSIBindings_HandleValue(lua_State *state)
{
    ECSIServices_PushLuaHandleValue(ECSIBindings_GetPlugin(state), ECSIBindings_PushTypeName(state, 2), 1);
    return 1;
}

static int ECSIBindings_HandleAddMethod(lua_State *state)
{
    const char *type = ECSILua_CheckString(state, 1);
    const char *method = ECSILua_CheckString(state, 2);
    const char *function = ECSILua_CheckString(state, 3);

    SHUResult result = ECSHandle_AddMethod(ECSIBindings_GetPlugin(state), type, method, function);

    if (result)
    {
        ECSIServices_Raise(state, result, "'%s' cannot be the method '%s' of '%s'", function, method, type);
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

#pragma region Structs

static int ECSIBindings_StructRegister(lua_State *state)
{
    const char *name = ECSILua_CheckString(state, 1);

    SHUResult result = ECSStruct_Register(ECSIBindings_GetPlugin(state), name, ECSILua_CheckString(state, 2));

    if (result)
    {
        ECSIServices_Raise(state, result, "struct type '%s' cannot be registered", name);
    }

    return 0;
}

static int ECSIBindings_StructAddMethod(lua_State *state)
{
    const char *type = ECSILua_CheckString(state, 1);
    const char *method = ECSILua_CheckString(state, 2);
    const char *function = ECSILua_CheckString(state, 3);

    SHUResult result = ECSStruct_AddMethod(ECSIBindings_GetPlugin(state), type, method, function);

    if (result)
    {
        ECSIServices_Raise(state, result, "'%s' cannot be the method '%s' of '%s'", function, method, type);
    }

    return 0;
}

static int ECSIBindings_StructNew(lua_State *state)
{
    const char *type = ECSILua_CheckString(state, 1);

    // a table of the fields, or a struct of the type, which ECSIServices_PushStruct checks
    if (!lua_istable(state, 2) && lua_type(state, 2) != OPENECS_LUA_TCDATA)
    {
        ECSILua_TypeError(state, 2, "table or struct");
    }

    ECSIServices_PushStruct(2, type);
    return 1;
}

static const luaL_Reg OPENECS_BINDINGS_STRUCT[] = {
    {"register", ECSIBindings_StructRegister},
    {"addMethod", ECSIBindings_StructAddMethod},
    {"new", ECSIBindings_StructNew},
    {nullptr, nullptr},
};

#pragma endregion Structs

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
    ECSILua_CheckType(state, 1, LUA_TTABLE);
    lua_getfield(state, 1, "stateVersion");
    lua_Integer version = ECSILua_OptInteger(state, -1, 0);
    lua_pop(state, 1);
    ECSILua_ArgCheck(state, version >= 0 && version <= SDL_MAX_UINT32, 1, "stateVersion must fit in 32 bits");
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

    SHUResult result = ECSIPlugin_Register(luaPlugin->plugin, &desc, ECSIBindings_PluginRelease);

    if (result)
    {
        ECSIBindings_PluginRelease(luaPlugin);
        ECSIServices_Raise(state, result, "the plugin cannot be registered");
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
    const char *name = ECSILua_CheckString(state, 1);
    const char *description = ECSILua_OptString(state, 2, "");

    SHUResult result = ECSEvent_Declare(ECSIBindings_GetPlugin(state), name, description);

    if (result)
    {
        ECSIServices_Raise(state, result, "event '%s' cannot be declared", name);
    }

    return 0;
}

static int ECSIBindings_EventEmit(lua_State *state)
{
    const char *name = ECSILua_CheckString(state, 1);
    ECSValue *value = nullptr;

    SHUResult result = ECSValue_Create(&value);
    result = result ? result : ECSILua_GetValue(state, 2, value);
    result = result ? result : ECSEvent_Emit(ECSIBindings_GetPlugin(state), name, value);
    ECSValue_Destroy(&value);

    if (result)
    {
        ECSIServices_Raise(state, result, "event '%s' cannot be emitted", name);
    }

    return 0;
}

static int ECSIBindings_EventSubscribe(lua_State *state)
{
    const char *name = ECSILua_CheckString(state, 1);
    ECSILua_CheckType(state, 2, LUA_TFUNCTION);
    ECSPlugin plugin = ECSIBindings_GetPlugin(state);

    ECSILuaSubscription *subscription = SDL_malloc(sizeof(ECSILuaSubscription));

    if (subscription == nullptr)
    {
        return luaL_error(state, "out of memory");
    }

    ECSILuaSubscription **handle = lua_newuserdata(state, sizeof(ECSILuaSubscription *));
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
        ECSIServices_Raise(state, result, "cannot subscribe to '%s'", name);
    }

    return 1;
}

static int ECSIBindings_EventUnsubscribe(lua_State *state)
{
    ECSILuaSubscription **handle = ECSILua_CheckUdata(state, 1, OPENECS_LUA_SUBSCRIPTION);

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
    const char *type = ECSILua_CheckString(state, 1);
    ECSPanel target = ECSIBindings_CheckOptionalPanel(state, 3);
    ECSZone zone = (ECSZone)ECSILua_CheckOption(state, 4, "default", OPENECS_BINDINGS_ZONES);
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
    ECSZone zone = (ECSZone)ECSILua_CheckOption(state, 3, "center", OPENECS_BINDINGS_ZONES);

    // only a pop-out needs no target
    if (target == nullptr && zone != ECSZone_Window)
    {
        ECSILua_ArgError(state, 2, "a panel is needed, except with the zone \"window\"");
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
    lua_Integer id = ECSILua_CheckInteger(state, 1);
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
    lua_Integer number = ECSILua_CheckInteger(state, 1);
    lua_pushstring(state, number >= 1 ? ECSWorkspace_GetName((usz)number) : nullptr);
    return 1;
}

static int ECSIBindings_WorkspaceSwitch(lua_State *state)
{
    lua_Integer number = ECSILua_CheckInteger(state, 1);

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
    const char *path = ECSILua_CheckString(state, 1);
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
    const char *path = ECSILua_CheckString(state, 1);
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
    const char *nameOrPath = ECSILua_CheckString(state, 1);
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

#pragma region Plugins

static int ECSIBindings_PluginsList(lua_State *state)
{
    ECSValue *list = nullptr;

    if (ECSValue_Create(&list) || ECSPlugins_List(list))
    {
        ECSValue_Destroy(&list);
        return luaL_error(state, "the plugins cannot be listed: out of memory");
    }

    ECSILua_PushValue(state, list);
    ECSValue_Destroy(&list);
    return 1;
}

static int ECSIBindings_PluginsEnable(lua_State *state)
{
    if (ECSPlugins_Enable(ECSILua_CheckString(state, 1)))
    {
        return luaL_error(state, "out of memory");
    }

    return 0;
}

static int ECSIBindings_PluginsDisable(lua_State *state)
{
    if (ECSPlugins_Disable(ECSILua_CheckString(state, 1)))
    {
        return luaL_error(state, "out of memory");
    }

    return 0;
}

static int ECSIBindings_PluginsReload(lua_State *state)
{
    if (ECSPlugins_Reload(ECSILua_CheckString(state, 1)))
    {
        return luaL_error(state, "out of memory");
    }

    return 0;
}

static const luaL_Reg OPENECS_BINDINGS_PLUGINS[] = {
    {"list", ECSIBindings_PluginsList},
    {"enable", ECSIBindings_PluginsEnable},
    {"disable", ECSIBindings_PluginsDisable},
    {"reload", ECSIBindings_PluginsReload},
    {nullptr, nullptr},
};

#pragma endregion Plugins

#pragma region Profile

static int ECSIBindings_ProfileGet(lua_State *state)
{
    return ECSIBindings_PushFilled(state, ECSProfile_Get);
}

static const luaL_Reg OPENECS_BINDINGS_PROFILE[] = {
    {"get", ECSIBindings_ProfileGet},
    {nullptr, nullptr},
};

#pragma endregion Profile

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
    lua_pushboolean(state, ECSClipboard_SetText(ECSILua_CheckString(state, 1)) == SHUResult_Ok);
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
    const char *mimeType = ECSILua_CheckString(state, 1);
    const char *bytes = ECSILua_CheckBytes(state, 2, &size, nullptr);
    lua_pushboolean(state, ECSClipboard_SetData(mimeType, (SHUSliceView){.data = bytes, .size = size}) == SHUResult_Ok);
    return 1;
}

static int ECSIBindings_ClipboardGetData(lua_State *state)
{
    SHUSlice data = cs0;

    if (ECSClipboard_GetData(ECSILua_CheckString(state, 1), &data))
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
            lua_rawseti(state, -2, (int)i + 1);
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
    ECSILua_CheckType(state, 1, LUA_TTABLE);
    ECSILua_CheckType(state, 2, LUA_TFUNCTION);

    // the texts stay alive in the table while the dialog is described, and the core copies them
    lua_getfield(state, 1, "type");
    lua_getfield(state, 1, "location");
    lua_getfield(state, 1, "many");
    lua_getfield(state, 1, "filters");

    ECSDialogFilter *filters = nullptr;

    for (int i = 1; lua_istable(state, -1) && lua_rawgeti(state, -1, i) == LUA_TTABLE; i++)
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
        .type = (ECSDialogType)ECSILua_CheckOption(state, -4, "openFile", OPENECS_BINDINGS_DIALOG_TYPES),
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
    const char *title = ECSILua_CheckString(state, 1);
    const char *text = ECSILua_CheckString(state, 2);
    const char **buttons = nullptr;

    // the button texts stay alive in the table on the stack
    for (int i = 1; lua_istable(state, 3) && lua_rawgeti(state, 3, i) == LUA_TSTRING; i++)
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
    const char *scope = ECSILua_CheckString(state, 1);
    const char *key = ECSILua_CheckString(state, 2);
    const char *function = ECSILua_CheckString(state, 3);
    SHUResult result = ECSKey_Bind(ECSIBindings_GetPlugin(state), scope, key, function);

    if (result)
    {
        ECSIServices_Raise(state, result, "'%s' is not bound to '%s'", key, function);
    }

    return 0;
}

static int ECSIBindings_KeyList(lua_State *state)
{
    return ECSIBindings_PushFilled(state, ECSKey_List);
}

static int ECSIBindings_KeySet(lua_State *state)
{
    const char *scope = lua_isnoneornil(state, 1) ? nullptr : ECSILua_CheckString(state, 1);
    const char *key = ECSILua_CheckString(state, 2);
    ECSValue *function = nullptr;
    SHUResult result = ECSValue_Create(&function);
    result = result ? result : ECSILua_GetValue(state, 3, function);
    result = result ? result : ECSKey_Set(scope, key, function);
    ECSValue_Destroy(&function);

    // a misuse stops the Lua code at its line; a file that cannot be written is a failure, which the caller handles
    if (result == SHUResult_ErrAssertion)
    {
        ECSIServices_Raise(state, result, "key '%s' is not set", key);
    }

    if (result)
    {
        lua_pushnil(state);
        lua_pushfstring(state, "key '%s' is not set (%s)", key, SHUResult_String(result));
        return 2;
    }

    lua_pushboolean(state, true);
    return 1;
}

static int ECSIBindings_KeyFunctions(lua_State *state)
{
    return ECSIBindings_PushFilled(state, ECSKey_ListFunctions);
}

static const luaL_Reg OPENECS_BINDINGS_KEY[] = {
    {"bind", ECSIBindings_KeyBind},
    {"list", ECSIBindings_KeyList},
    {"set", ECSIBindings_KeySet},
    {"functions", ECSIBindings_KeyFunctions},
    {nullptr, nullptr},
};

#pragma endregion Keys

#pragma region Services

static int ECSIBindings_ServiceGet(lua_State *state)
{
    const char *name = ECSILua_CheckString(state, 1);
    const char *signature = ECSILua_OptString(state, 2, nullptr);
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
    ECSILua_CheckType(state, 1, LUA_TTABLE);
    ECSPlugin plugin = ECSIBindings_GetPlugin(state);
    const char *prefix = ECSIPlugin_GetName(plugin);
    SHUResult result = SHUResult_Ok;
    const char *failed = nullptr;

    lua_pushnil(state);

    while (lua_next(state, 1) != 0)
    {
        // lua_tostring would change a number key in place and break lua_next, so check the types first
        ECSILua_ArgCheck(state, lua_type(state, -2) == LUA_TSTRING && lua_istable(state, -1), 1, "must map local names to { sig, doc, fn } tables");
        lua_getfield(state, -1, "sig");
        lua_getfield(state, -2, "doc");
        lua_getfield(state, -3, "fn");
        ECSILua_CheckType(state, -1, LUA_TFUNCTION);

        // the core adds the plugin's name, so a name that has it already is a mistake, as in C
        const char *local = lua_tostring(state, -5);
        usz prefixLength = SDL_strlen(prefix);
        ECSILua_ArgCheck(state, SDL_strncmp(local, prefix, prefixLength) != 0 || local[prefixLength] != '.', 1, "must map local names, without the plugin's name");
        const char *name = lua_pushfstring(state, "%s.%s", prefix, lua_tostring(state, -5));
        lua_insert(state, -2);
        SHUResult registered = ECSIServices_RegisterLua(plugin, name, ECSILua_CheckString(state, -4), ECSILua_OptString(state, -3, ""));

        // a failed function stays on the stack; a registered one was taken
        lua_settop(state, registered ? lua_gettop(state) - 5 : lua_gettop(state) - 4);
        result = registered ? registered : result;
        failed = registered ? lua_tostring(state, -1) : failed;
    }

    if (result)
    {
        ECSIServices_Raise(state, result, "function '%s.%s' is not registered", prefix, failed == nullptr ? "?" : failed);
    }

    return 0;
}

static const luaL_Reg OPENECS_BINDINGS_SERVICE[] = {
    {"register", ECSIBindings_ServiceRegister},
    {"get", ECSIBindings_ServiceGet},
    {nullptr, nullptr},
};

#pragma endregion Services

#pragma region Panels

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
        ECS_Log(luaPanel->type->plugin, ECSLogLevel_Warning, "A panel failed to save: %s", ECSILua_OptString(state, -1, "no reason given"));
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
    ECSILua_CheckType(state, 1, LUA_TTABLE);
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
        .name = ECSILua_CheckString(state, -7),
        .title = ECSILua_OptString(state, -6, nullptr),
        .surface = SDL_strcmp(ECSILua_OptString(state, -5, "pixels"), "gpu") == 0 ? ECSSurfaceType_Gpu : ECSSurfaceType_Pixels,
        .stateVersion = (u32)ECSILua_OptInteger(state, -4, 0),
        .continuous = lua_toboolean(state, -3),
        .minWidth = (f32)ECSILua_OptNumber(state, -2, 0.0),
        .minHeight = (f32)ECSILua_OptNumber(state, -1, 0.0),
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

    if (result)
    {
        luaL_unref(state, LUA_REGISTRYINDEX, type->table);
        SDL_free(type);
        ECSIServices_Raise(state, result, "panel type '%s' is not registered", desc.name);
    }

    arrput(BINDINGS.types, type);
    return 0;
}

/// @brief A panel's fields other than its title and unsaved mark cannot be set.
static int ECSIBindings_PanelCannot(lua_State *state)
{
    ECSILua_Misuse(state, "a panel's field '%s' cannot be set; only title and unsaved can", lua_tostring(state, 2));
}

static int ECSIBindings_PanelAcceptDrops(lua_State *state)
{
    ECSPanel panel = ECSIBindings_CheckPanel(1);
    ECSILua_CheckType(state, 2, LUA_TTABLE);

    // the table keeps its strings alive while the core copies them
    const char **types = nullptr;

    for (int i = 1; lua_rawgeti(state, 2, i) != LUA_TNIL; i++)
    {
        if (lua_type(state, -1) != LUA_TSTRING)
        {
            arrfree(types);
            ECSILua_ArgError(state, 2, "the types must be a list of strings");
        }

        arrput(types, lua_tostring(state, -1));
        lua_pop(state, 1);
    }

    SHUResult result = ECSPanel_AcceptDrops(panel, types, arrlenu(types));
    arrfree(types);

    if (result)
    {
        ECSIServices_Raise(state, result, "the types are not set");
    }

    return 0;
}

static int ECSIBindings_PanelSetTextInput(lua_State *state)
{
    ECSPanel panel = ECSIBindings_CheckPanel(1);
    bool accept = lua_toboolean(state, 2);
    f32 x = (f32)ECSILua_OptNumber(state, 3, 0.0);
    f32 y = (f32)ECSILua_OptNumber(state, 4, 0.0);
    f32 width = (f32)ECSILua_OptNumber(state, 5, 0.0);
    f32 height = (f32)ECSILua_OptNumber(state, 6, 0.0);
    ECSPanel_SetTextInput(panel, accept, x, y, width, height);
    return 0;
}

static int ECSIBindings_PanelStartDrag(lua_State *state)
{
    ECSPanel panel = ECSIBindings_CheckPanel(1);
    const char *type = ECSILua_CheckString(state, 2);
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
    f32 number = lua_isnil(state, -1) ? fallback : (f32)ECSILua_CheckNumber(state, -1);
    lua_pop(state, 1);
    return number;
}

static int ECSIBindings_PanelOpenPopup(lua_State *state)
{
    ECSPanel panel = ECSIBindings_CheckPanel(1);
    ECSILua_ArgCheck(state, panel->type != nullptr, 1, "the panel has no type");
    ECSILua_CheckType(state, 2, LUA_TTABLE);

    lua_getfield(state, 2, "type");
    ECSPopupType type = (ECSPopupType)ECSILua_CheckOption(state, -1, "menu", OPENECS_BINDINGS_POPUP_TYPES);
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
    ECSILua_ArgCheck(state, lua_isfunction(state, -1), 2, "the popup needs a draw function");
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

        // a panel that is not shown has no place for a popup; anything else is a misuse or memory running out
        if (result != SHUResult_ErrNotFound)
        {
            ECSIServices_Raise(state, result, "the popup is not opened");
        }

        lua_pushnil(state);
        lua_pushstring(state, "the popup is not opened: the panel is not shown");
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
    f32 width = (f32)ECSILua_CheckNumber(state, 2);
    f32 height = (f32)ECSILua_CheckNumber(state, 3);
    ECSILua_ArgCheck(state, width > 0.0f && height > 0.0f, 2, "the size must be above 0");
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

/// @brief panel.place: where the panel is in its OS window, and the window's size, in its surface pixels.
static int ECSIBindings_PanelPlace(lua_State *state)
{
    ECSPanelPlace place = {0};
    ECSPanel_GetPlace(ECSIBindings_CheckPanel(1), &place);
    lua_createtable(state, 0, 4);
    lua_pushnumber(state, place.x);
    lua_setfield(state, -2, "x");
    lua_pushnumber(state, place.y);
    lua_setfield(state, -2, "y");
    lua_pushnumber(state, place.windowWidth);
    lua_setfield(state, -2, "windowWidth");
    lua_pushnumber(state, place.windowHeight);
    lua_setfield(state, -2, "windowHeight");
    return 1;
}

/// @brief The panel's fields that Lua builds itself, as their values are tables.
static const luaL_Reg OPENECS_BINDINGS_PANEL_TABLE_FIELDS[] = {
    {"place", ECSIBindings_PanelPlace},
    {nullptr, nullptr},
};

static const luaL_Reg OPENECS_BINDINGS_PANEL_METHODS[] = {
    {"openPopup", ECSIBindings_PanelOpenPopup},
    {"setTextInput", ECSIBindings_PanelSetTextInput},
    {"acceptDrops", ECSIBindings_PanelAcceptDrops},
    {"startDrag", ECSIBindings_PanelStartDrag},
    {nullptr, nullptr},
};

static int ECSIBindings_PanelAddMenuEntry(lua_State *state)
{
    const char *type = ECSILua_CheckString(state, 1);
    const char *function = ECSILua_CheckString(state, 2);

    SHUResult result = ECSPanelType_AddMenuEntry(ECSIBindings_GetPlugin(state), type, function);

    if (result)
    {
        ECSIServices_Raise(state, result, "'%s' is not added to the menu of '%s'", function, type);
    }

    return 0;
}

static int ECSIBindings_AppAddMenuEntry(lua_State *state)
{
    const char *function = ECSILua_CheckString(state, 1);
    SHUResult result = ECSApp_AddMenuEntry(ECSIBindings_GetPlugin(state), function);

    if (result)
    {
        ECSIServices_Raise(state, result, "'%s' is not added to the menu Tools", function);
    }

    return 0;
}

static const luaL_Reg OPENECS_BINDINGS_APP[] = {
    {"addMenuEntry", ECSIBindings_AppAddMenuEntry},
    {nullptr, nullptr},
};

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

// The methods and fields of the core's handles are C functions that Lua calls through the FFI, as it calls plugins' (DESIGN 10.6). They run
// no Lua, so the JIT compiles them with their caller, and check their arguments with ECS_Check; the handle is valid, as its wrapper checks it.

/// @brief Gets a pixel's address, or nullptr if the position is outside the surface.
static u32 *ECSIBindings_GetPixelAddress(ECSSurface *surface, i32 x, i32 y)
{
    if (x < 0 || y < 0 || x >= surface->width || y >= surface->height)
    {
        return nullptr;
    }

    return (u32 *)((u8 *)surface->pixels.data + (usz)y * (usz)surface->pitch) + x;
}

/// @brief Checks that a surface has pixels; a GPU surface has none.
static bool ECSIBindings_HasPixels(const ECSSurface *surface)
{
    return ECS_Check(nullptr, surface->type == ECSSurfaceType_Pixels, "a GPU surface has no pixels; the gpu plugin draws into it");
}

static void ECSIBindings_SurfaceSetPixel(void *surfaceHandle, i32 x, i32 y, ECSColor color)
{
    ECSSurface *surface = surfaceHandle;
    u32 *pixel = ECSIBindings_HasPixels(surface) ? ECSIBindings_GetPixelAddress(surface, x, y) : nullptr;

    // pixels outside the surface are clipped; surfaces hold premultiplied colours
    if (pixel != nullptr)
    {
        *pixel = ECSIColor_Premultiply(color);
    }
}

static const char *ECSIBindings_SurfaceGetPixel(void *surfaceHandle, i32 x, i32 y)
{
    // the text lives until the next call, and Lua copies it at once
    static char text[OPENECS_COLOR_TEXT_SIZE];
    ECSSurface *surface = surfaceHandle;

    if (!ECSIBindings_HasPixels(surface))
    {
        return nullptr;
    }

    u32 *pixel = ECSIBindings_GetPixelAddress(surface, x, y);

    if (pixel == nullptr)
    {
        (void)ECS_Check(nullptr, false, "the position %d, %d is outside the surface", x, y);
        return nullptr;
    }

    ECSIColor_Format(ECSIColor_Unpremultiply(*pixel), text);
    return text;
}

static void ECSIBindings_SurfaceSetRow(void *surfaceHandle, i32 y, SHUSliceView bytes, i32 x)
{
    ECSSurface *surface = surfaceHandle;

    // the row is clipped to the surface
    if (!ECSIBindings_HasPixels(surface) || y < 0 || y >= surface->height || x >= surface->width)
    {
        return;
    }

    i64 skip = x < 0 ? -(i64)x : 0;
    i64 count = SDL_min((i64)(bytes.size / 4) - skip, (i64)surface->width - ((i64)x + skip));
    u32 *first = ECSIBindings_GetPixelAddress(surface, (i32)(x + skip), y);

    if (count > 0 && first != nullptr)
    {
        SDL_memcpy(first, (const u8 *)bytes.data + skip * 4, (usz)count * 4);
    }
}

static void ECSIBindings_SurfaceFill(void *surfaceHandle, i32 x, i32 y, i32 width, i32 height, ECSColor color)
{
    if (ECSIBindings_HasPixels(surfaceHandle))
    {
        ECSSurface_Fill(surfaceHandle, x, y, width, height, color);
    }
}

static void ECSIBindings_SurfaceChanged(void *surfaceHandle, i32 x, i32 y, i32 width, i32 height)
{
    if (ECSIBindings_HasPixels(surfaceHandle))
    {
        ECSSurface_Changed(surfaceHandle, x, y, width, height);
    }
}

static i32 ECSIBindings_SurfaceWidth(void *surfaceHandle)
{
    return ((ECSSurface *)surfaceHandle)->width;
}

static i32 ECSIBindings_SurfaceHeight(void *surfaceHandle)
{
    return ((ECSSurface *)surfaceHandle)->height;
}

static f32 ECSIBindings_SurfaceScale(void *surfaceHandle)
{
    return ((ECSSurface *)surfaceHandle)->scale;
}

static const char *ECSIBindings_SurfaceType(void *surfaceHandle)
{
    return ((ECSSurface *)surfaceHandle)->type == ECSSurfaceType_Gpu ? "gpu" : "pixels";
}

static bool ECSIBindings_SurfaceKept(void *surfaceHandle)
{
    return ((ECSSurface *)surfaceHandle)->kept;
}

static i32 ECSIBindings_PanelID(void *panel)
{
    return (i32)ECSPanel_GetID(panel);
}

static const char *ECSIBindings_PanelTitle(void *panel)
{
    return ECSPanel_GetTitle(panel);
}

static const char *ECSIBindings_PanelType(void *panel)
{
    return ECSPanel_GetType(panel);
}

static bool ECSIBindings_PanelUnsaved(void *panel)
{
    return ECSPanel_GetUnsaved(panel);
}

static void ECSIBindings_PanelSetTitle(void *panel, const char *title)
{
    ECSPanel_SetTitle(panel, title);
}

static void ECSIBindings_PanelSetUnsaved(void *panel, bool unsaved)
{
    ECSPanel_SetUnsaved(panel, unsaved);
}

static void ECSIBindings_PanelRedrawDirect(void *panel)
{
    ECSPanel_Redraw(panel);
}

/// @brief One of the core's functions that is a method or a field of its handles.
typedef struct ECSIBindingsMember
{
    const char *key;       // the method's or field's name in Lua
    const char *name;      // the function's full name, which errors name
    ECSFunction Function;  // the C function
    const char *signature; // its signature
} ECSIBindingsMember;

static const ECSIBindingsMember OPENECS_BINDINGS_SURFACE_METHODS[] = {
    {"setPixel", "ecs.surface.setPixel", (ECSFunction)ECSIBindings_SurfaceSetPixel, "void(handle<ecs.surface> surface, int x, int y, color color)"},
    {"getPixel", "ecs.surface.getPixel", (ECSFunction)ECSIBindings_SurfaceGetPixel, "string(handle<ecs.surface> surface, int x, int y)"},
    {"setRow", "ecs.surface.setRow", (ECSFunction)ECSIBindings_SurfaceSetRow, "void(handle<ecs.surface> surface, int y, buffer bytes, int x)"},
    {"fill", "ecs.surface.fill", (ECSFunction)ECSIBindings_SurfaceFill, "void(handle<ecs.surface> surface, int x, int y, int width, int height, color color)"},
    {"changed", "ecs.surface.changed", (ECSFunction)ECSIBindings_SurfaceChanged, "void(handle<ecs.surface> surface, int x, int y, int width, int height)"},
    {nullptr, nullptr, nullptr, nullptr},
};

static const ECSIBindingsMember OPENECS_BINDINGS_SURFACE_FIELDS[] = {
    {"width", "ecs.surface.width", (ECSFunction)ECSIBindings_SurfaceWidth, "int(handle<ecs.surface> surface)"},
    {"height", "ecs.surface.height", (ECSFunction)ECSIBindings_SurfaceHeight, "int(handle<ecs.surface> surface)"},
    {"scale", "ecs.surface.scale", (ECSFunction)ECSIBindings_SurfaceScale, "float(handle<ecs.surface> surface)"},
    {"type", "ecs.surface.type", (ECSFunction)ECSIBindings_SurfaceType, "string(handle<ecs.surface> surface)"},
    {"kept", "ecs.surface.kept", (ECSFunction)ECSIBindings_SurfaceKept, "bool(handle<ecs.surface> surface)"},
    {nullptr, nullptr, nullptr, nullptr},
};

static const ECSIBindingsMember OPENECS_BINDINGS_PANEL_DIRECT[] = {
    {"redraw", "ecs.panel.redraw", (ECSFunction)ECSIBindings_PanelRedrawDirect, "void(handle<ecs.panel> panel)"},
    {nullptr, nullptr, nullptr, nullptr},
};

static const ECSIBindingsMember OPENECS_BINDINGS_PANEL_FIELDS[] = {
    {"id", "ecs.panel.id", (ECSFunction)ECSIBindings_PanelID, "int(handle<ecs.panel> panel)"},
    {"title", "ecs.panel.title", (ECSFunction)ECSIBindings_PanelTitle, "string(handle<ecs.panel> panel)"},
    {"type", "ecs.panel.type", (ECSFunction)ECSIBindings_PanelType, "string(handle<ecs.panel> panel)"},
    {"unsaved", "ecs.panel.unsaved", (ECSFunction)ECSIBindings_PanelUnsaved, "bool(handle<ecs.panel> panel)"},
    {nullptr, nullptr, nullptr, nullptr},
};

static const ECSIBindingsMember OPENECS_BINDINGS_PANEL_SETTERS[] = {
    {"title", "ecs.panel.setTitle", (ECSFunction)ECSIBindings_PanelSetTitle, "void(handle<ecs.panel> panel, string title)"},
    {"unsaved", "ecs.panel.setUnsaved", (ECSFunction)ECSIBindings_PanelSetUnsaved, "void(handle<ecs.panel> panel, bool unsaved)"},
    {nullptr, nullptr, nullptr, nullptr},
};

/// @brief The metamethods of the core's handles, which the JIT compiles: a method comes from its table, a field from its function, and a field is set
/// by its function; any other key's value is nil, and setting it calls cannot, which raises the misuse. A surface's setRow takes x as 0 when it is
/// missing; its tail call leaves no frame, so an error names the caller's line.
static const char OPENECS_BINDINGS_MEMBERS[] = "local methods, fields, setters, cannot = ...\n"
                                               "local setRow = methods.setRow\n"
                                               "if setRow ~= nil then\n"
                                               "  methods.setRow = function(surface, y, bytes, x) return setRow(surface, y, bytes, x or 0) end\n"
                                               "end\n"
                                               "local function index(object, key)\n"
                                               "  local method = methods[key]\n"
                                               "  if method ~= nil then return method end\n"
                                               "  local field = fields[key]\n"
                                               "  if field ~= nil then return field(object) end\n"
                                               "end\n"
                                               "local function newindex(object, key, value)\n"
                                               "  local setter = setters[key]\n"
                                               "  if setter == nil then return cannot(object, key) end\n"
                                               "  setter(object, value)\n"
                                               "end\n"
                                               "return index, newindex\n";

/// @brief Adds the wrappers of a list of members to the table on top of the stack, by their keys.
static SHUResult ECSIBindings_AddMembers(lua_State *state, const ECSIBindingsMember *members)
{
    for (usz i = 0; members[i].key != nullptr; i++)
    {
        SHU_ReturnResult(ECSIServices_PushCoreMethod(members[i].name, members[i].Function, members[i].signature));
        lua_setfield(state, -2, members[i].key);
    }

    return SHUResult_Ok;
}

/// @brief Sets the metamethods of one of the core's handle types: its methods, which are the given C functions and the wrapped ones, its fields and its
/// fields that can be set. A missing table of members gives nothing.
static SHUResult ECSIBindings_SetMembers(const char *type, const luaL_Reg *methods, const ECSIBindingsMember *direct, const ECSIBindingsMember *fields,
                                         const luaL_Reg *tableFields, const ECSIBindingsMember *setters, lua_CFunction Cannot)
{
    lua_State *state = ECSILua_GetState();
    int top = lua_gettop(state);
    SHUResult result = SHUResult_Ok;

    if (luaL_loadbuffer(state, OPENECS_BINDINGS_MEMBERS, sizeof(OPENECS_BINDINGS_MEMBERS) - 1, "=ecs.members") != LUA_OK)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "The handles' members do not load: %s", lua_tostring(state, -1));
        lua_settop(state, top);
        return SHUResult_Err;
    }

    lua_newtable(state);

    if (methods != nullptr)
    {
        luaL_setfuncs(state, methods, 0);
    }

    result = direct != nullptr ? ECSIBindings_AddMembers(state, direct) : result;
    lua_newtable(state);
    result = !result && fields != nullptr ? ECSIBindings_AddMembers(state, fields) : result;

    if (tableFields != nullptr)
    {
        luaL_setfuncs(state, tableFields, 0);
    }

    lua_newtable(state);
    result = !result && setters != nullptr ? ECSIBindings_AddMembers(state, setters) : result;
    lua_pushcfunction(state, Cannot);

    if (result || ECSILua_Call(state, 4, 2))
    {
        lua_settop(state, top);
        return result ? result : SHUResult_Err;
    }

    ECSIServices_PushHandleMetatable(type);
    lua_insert(state, -3);
    lua_setfield(state, -3, "__newindex");
    lua_setfield(state, -2, "__index");
    lua_settop(state, top);
    return SHUResult_Ok;
}

/// @brief A surface's fields cannot be set.
static int ECSIBindings_SurfaceCannot(lua_State *state)
{
    ECSILua_Misuse(state, "a surface's field '%s' cannot be set", lua_tostring(state, 2));
}

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

/// @brief ecs.check in Lua: a check that holds returns there, so the JIT compiles a plugin's checks with its code; one that fails, or has no message,
/// goes on to the core's check by a tail call, which names the same line.
static const char OPENECS_BINDINGS_CHECK[] =
    "local check = ...\n"
    "return function(...)\n"
    "  local condition, message = ...\n"
    "  if condition and type(message) == 'string' then return end\n"
    "  return check(...)\n"
    "end\n";

/// @brief Puts ecs.check in Lua in front of the core's, in the ecs table on the top of the stack.
static void ECSIBindings_WrapCheck(lua_State *state)
{
    if (luaL_loadbuffer(state, OPENECS_BINDINGS_CHECK, sizeof(OPENECS_BINDINGS_CHECK) - 1, "=ecs.check") != LUA_OK)
    {
        lua_pop(state, 1);
        return;
    }

    lua_getfield(state, -2, "check");
    lua_call(state, 1, 1);
    lua_setfield(state, -2, "check");
}

/// @brief Pushes a new ecs table for a plugin.
static void ECSIBindings_PushEcs(lua_State *state, ECSPlugin plugin)
{
    lua_newtable(state);
    ECSIWorkers_PushLog(state, plugin);
    lua_setfield(state, -2, "log");
    ECSIWorkers_AddErrors(state, plugin);
    ECSIBindings_WrapCheck(state);
    ECSIBindings_AddTable(state, plugin, "setting", OPENECS_BINDINGS_SETTING);
    ECSIBindings_AddTable(state, plugin, "timer", OPENECS_BINDINGS_TIMER);
    ECSIBindings_AddTable(state, plugin, "panel", OPENECS_BINDINGS_PANEL);
    ECSIBindings_AddTable(state, plugin, "service", OPENECS_BINDINGS_SERVICE);
    ECSIBindings_AddTable(state, plugin, "key", OPENECS_BINDINGS_KEY);
    ECSIBindings_AddTable(state, plugin, "layout", OPENECS_BINDINGS_LAYOUT);
    ECSIBindings_AddTable(state, plugin, "workspace", OPENECS_BINDINGS_WORKSPACE);
    ECSIBindings_AddTable(state, plugin, "session", OPENECS_BINDINGS_SESSION);
    ECSIBindings_AddTable(state, plugin, "plugins", OPENECS_BINDINGS_PLUGINS);
    ECSIBindings_AddTable(state, plugin, "profile", OPENECS_BINDINGS_PROFILE);
    ECSIBindings_AddTable(state, plugin, "clipboard", OPENECS_BINDINGS_CLIPBOARD);
    ECSIBindings_AddTable(state, plugin, "dialog", OPENECS_BINDINGS_DIALOG);
    ECSIBindings_AddTable(state, plugin, "event", OPENECS_BINDINGS_EVENT);
    ECSIBindings_AddTable(state, plugin, "handle", OPENECS_BINDINGS_HANDLE);
    ECSIBindings_AddTable(state, plugin, "struct", OPENECS_BINDINGS_STRUCT);
    ECSIBindings_AddTable(state, plugin, "app", OPENECS_BINDINGS_APP);
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
    const char *name = ECSILua_CheckString(state, 1);
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
        lua_setfenv(state, -2);
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
        if (result)
        {
            ECSIServices_Raise(state, result, "the functions of '%s' are not available", name);
        }

        return 1;
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

    ECSIServices_PushHandleMetatable(OPENECS_LUA_POPUP);
    luaL_newlib(state, OPENECS_BINDINGS_POPUP);
    lua_setfield(state, -2, "__index");
    lua_pop(state, 1);

    // a panel's and a surface's data are fields, and what they do are methods
    SHUResult result = ECSIBindings_SetMembers(OPENECS_LUA_PANEL, OPENECS_BINDINGS_PANEL_METHODS, OPENECS_BINDINGS_PANEL_DIRECT, OPENECS_BINDINGS_PANEL_FIELDS, OPENECS_BINDINGS_PANEL_TABLE_FIELDS,
                                               OPENECS_BINDINGS_PANEL_SETTERS, ECSIBindings_PanelCannot);
    result = result ? result : ECSIBindings_SetMembers(OPENECS_LUA_SURFACE, nullptr, OPENECS_BINDINGS_SURFACE_METHODS, OPENECS_BINDINGS_SURFACE_FIELDS, nullptr, nullptr, ECSIBindings_SurfaceCannot);
    SDL_assert(!result);
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
    lua_setfenv(state, -2);

    if (ECSILua_Call(state, 0, 0))
    {
        ECS_Log(plugin, ECSLogLevel_Error, "%s", lua_tostring(state, -1));
        lua_settop(state, top);
        return SHUResult_ErrBadData;
    }

    lua_settop(state, top);
    return SHUResult_Ok;
}
