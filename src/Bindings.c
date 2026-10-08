#include "Bindings.h"

#include "Events.h"
#include "Lua.h"
#include "Panels.h"
#include "Settings.h"

#include "lua/lauxlib.h"
#include "lua/lua.h"

#include "SDL3/SDL.h"
#include "stb/stbSDL3.h"

#pragma region Source Only

/// @brief Name of the metatable of timer handles.
#define OPENECS_LUA_TIMER "ecs.timer"
/// @brief Name of the metatable of panel handles.
#define OPENECS_LUA_PANEL "ecs.panel"
/// @brief Name of the metatable of surfaces given to draw.
#define OPENECS_LUA_SURFACE "ecs.surface"

/// @brief A panel type registered from Lua.
typedef struct ECSI_LuaPanelType
{
    ECSPlugin plugin;
    int table; // registry reference of the description table, which holds the callbacks
} ECSI_LuaPanelType;

/// @brief The state of a panel of a Lua panel type.
typedef struct ECSI_LuaPanel
{
    ECSI_LuaPanelType *type;
    ECSPanel panel;
    int state;   // registry reference of the value that create returned
    int handle;  // registry reference of the panel's handle
    int surface; // registry reference of the surface handle given to draw
} ECSI_LuaPanel;

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
    } *errors;                  // stb_ds hash map with copied keys
    ECSI_LuaPanelType **types; // stb_ds array
} BINDINGS = {0};

/// @brief Names of the event types in Lua, in the order of ECSEventType.
static const char *const ECSI_BINDINGS_EVENT_TYPES[] = {"pointer_down", "pointer_up", "pointer_move", "wheel", "key_down", "key_up", "focused", "unfocused"};

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

#pragma region Panels

static ECSPanel ECSI_BindingsCheckPanel(lua_State *state, int index)
{
    ECSPanel *handle = luaL_checkudata(state, index, OPENECS_LUA_PANEL);

    if (*handle == NULL)
    {
        luaL_error(state, "the panel is closed");
    }

    return *handle;
}

static ECSSurface *ECSI_BindingsCheckSurface(lua_State *state, int index)
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
static bool ECSI_BindingsPushCallback(lua_State *state, const ECSI_LuaPanelType *type, const char *name)
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
static void ECSI_BindingsPanelFailed(lua_State *state, const ECSI_LuaPanel *luaPanel)
{
    const char *message = lua_tostring(state, -1);
    ECSI_BindingsReport(luaPanel->type->plugin, message);
    ECSI_PanelFault(luaPanel->panel, message);
    lua_pop(state, 1);
}

/// @brief Clears the handle a Lua value points to, so Lua cannot reach a destroyed object through it.
static void ECSI_BindingsClearHandle(lua_State *state, int reference)
{
    lua_rawgeti(state, LUA_REGISTRYINDEX, reference);
    *(void **)lua_touserdata(state, -1) = NULL;
    lua_pop(state, 1);
}

static void ECSI_BindingsPanelFree(lua_State *state, ECSI_LuaPanel *luaPanel)
{
    ECSI_BindingsClearHandle(state, luaPanel->handle);
    luaL_unref(state, LUA_REGISTRYINDEX, luaPanel->state);
    luaL_unref(state, LUA_REGISTRYINDEX, luaPanel->handle);
    luaL_unref(state, LUA_REGISTRYINDEX, luaPanel->surface);
    SDL_free(luaPanel);
}

static SHUResult ECSI_BindingsPanelCreate(ECSPanel panel, const ECSValue *savedState, u32 version, void **retState)
{
    lua_State *state = ECSI_LuaGetState();
    ECSI_LuaPanel *luaPanel = SDL_calloc(1, sizeof(ECSI_LuaPanel));

    if (luaPanel == NULL)
    {
        return SHUResult_ErrAllocation;
    }

    luaPanel->type = panel->type->typeData;
    luaPanel->panel = panel;
    luaPanel->state = LUA_NOREF;

    ECSPanel *handle = lua_newuserdatauv(state, sizeof(ECSPanel), 0);
    *handle = panel;
    luaL_setmetatable(state, OPENECS_LUA_PANEL);
    luaPanel->handle = luaL_ref(state, LUA_REGISTRYINDEX);

    ECSSurface **surface = lua_newuserdatauv(state, sizeof(ECSSurface *), 0);
    *surface = NULL;
    luaL_setmetatable(state, OPENECS_LUA_SURFACE);
    luaPanel->surface = luaL_ref(state, LUA_REGISTRYINDEX);

    if (!ECSI_BindingsPushCallback(state, luaPanel->type, "create"))
    {
        lua_pushnil(state);
    }
    else
    {
        lua_rawgeti(state, LUA_REGISTRYINDEX, luaPanel->handle);
        ECSI_LuaPushValue(savedState);
        lua_pushinteger(state, (lua_Integer)version);

        if (ECSI_LuaCall(3, 1))
        {
            ECSI_BindingsPanelFailed(state, luaPanel);
            ECSI_BindingsPanelFree(state, luaPanel);
            return SHUResult_ErrBadData;
        }
    }

    luaPanel->state = luaL_ref(state, LUA_REGISTRYINDEX);
    *retState = luaPanel;
    return SHUResult_Ok;
}

static void ECSI_BindingsPanelDestroy(void *data)
{
    ECSI_LuaPanel *luaPanel = data;
    lua_State *state = ECSI_LuaGetState();

    if (ECSI_BindingsPushCallback(state, luaPanel->type, "destroy"))
    {
        lua_rawgeti(state, LUA_REGISTRYINDEX, luaPanel->state);

        if (ECSI_LuaCall(1, 0))
        {
            ECSI_BindingsReport(luaPanel->type->plugin, lua_tostring(state, -1));
            lua_pop(state, 1);
        }
    }

    ECSI_BindingsPanelFree(state, luaPanel);
}

static void ECSI_BindingsPanelDraw(void *data, ECSSurface *surface, f64 seconds)
{
    ECSI_LuaPanel *luaPanel = data;
    lua_State *state = ECSI_LuaGetState();

    if (!ECSI_BindingsPushCallback(state, luaPanel->type, "draw"))
    {
        return;
    }

    lua_rawgeti(state, LUA_REGISTRYINDEX, luaPanel->state);
    lua_rawgeti(state, LUA_REGISTRYINDEX, luaPanel->surface);
    ECSSurface **handle = lua_touserdata(state, -1);
    *handle = surface;
    lua_pushnumber(state, (lua_Number)seconds);

    if (ECSI_LuaCall(3, 0))
    {
        ECSI_BindingsPanelFailed(state, luaPanel);
    }

    *handle = NULL;
}

static void ECSI_BindingsPushEvent(lua_State *state, const ECSEvent *event)
{
    lua_createtable(state, 0, 8);
    lua_pushstring(state, ECSI_BINDINGS_EVENT_TYPES[event->type]);
    lua_setfield(state, -2, "type");

    switch (event->type)
    {
    case ECSEventType_PointerDown:
    case ECSEventType_PointerUp:
        lua_pushinteger(state, event->button);
        lua_setfield(state, -2, "button");
        // fall through
    case ECSEventType_PointerMove:
    case ECSEventType_Wheel:
        lua_pushnumber(state, (lua_Number)event->x);
        lua_setfield(state, -2, "x");
        lua_pushnumber(state, (lua_Number)event->y);
        lua_setfield(state, -2, "y");
        lua_pushnumber(state, (lua_Number)event->wheelX);
        lua_setfield(state, -2, "wheel_x");
        lua_pushnumber(state, (lua_Number)event->wheelY);
        lua_setfield(state, -2, "wheel_y");
        break;
    case ECSEventType_KeyDown:
    case ECSEventType_KeyUp:
        lua_pushstring(state, SDL_GetKeyName(event->key));
        lua_setfield(state, -2, "key");
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

static void ECSI_BindingsPanelEvent(void *data, const ECSEvent *event)
{
    ECSI_LuaPanel *luaPanel = data;
    lua_State *state = ECSI_LuaGetState();

    if (!ECSI_BindingsPushCallback(state, luaPanel->type, "event"))
    {
        return;
    }

    lua_rawgeti(state, LUA_REGISTRYINDEX, luaPanel->state);
    ECSI_BindingsPushEvent(state, event);

    if (ECSI_LuaCall(2, 0))
    {
        ECSI_BindingsPanelFailed(state, luaPanel);
    }
}

static SHUResult ECSI_BindingsPanelSaveState(void *data, ECSValue *retState)
{
    ECSI_LuaPanel *luaPanel = data;
    lua_State *state = ECSI_LuaGetState();

    if (!ECSI_BindingsPushCallback(state, luaPanel->type, "save_state"))
    {
        return SHUResult_ErrNotFound;
    }

    lua_rawgeti(state, LUA_REGISTRYINDEX, luaPanel->state);

    if (ECSI_LuaCall(1, 1))
    {
        ECSI_BindingsPanelFailed(state, luaPanel);
        return SHUResult_ErrBadData;
    }

    SHUResult result = ECSI_LuaGetValue(-1, retState);
    lua_pop(state, 1);
    return result;
}

static SHUResult ECSI_BindingsPanelSave(void *data)
{
    ECSI_LuaPanel *luaPanel = data;
    lua_State *state = ECSI_LuaGetState();

    if (!ECSI_BindingsPushCallback(state, luaPanel->type, "save"))
    {
        return SHUResult_ErrNotFound;
    }

    lua_rawgeti(state, LUA_REGISTRYINDEX, luaPanel->state);

    // save returns true, or nil and a message
    if (ECSI_LuaCall(1, 2))
    {
        ECSI_BindingsPanelFailed(state, luaPanel);
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
static bool ECSI_BindingsHasFunction(lua_State *state, int index, const char *name)
{
    bool found = lua_getfield(state, index, name) == LUA_TFUNCTION;
    lua_pop(state, 1);
    return found;
}

static int ECSI_BindingsPanelRegisterType(lua_State *state)
{
    luaL_checktype(state, 1, LUA_TTABLE);
    ECSPlugin plugin = ECSI_BindingsPlugin(state);

    // the texts stay alive in the table while the type is registered, which copies them
    lua_getfield(state, 1, "name");
    lua_getfield(state, 1, "title");
    lua_getfield(state, 1, "surface");
    lua_getfield(state, 1, "state_version");
    lua_getfield(state, 1, "continuous");
    lua_getfield(state, 1, "min_width");
    lua_getfield(state, 1, "min_height");

    ECSPanelTypeDesc desc = {
        .name = luaL_checkstring(state, -7),
        .title = luaL_optstring(state, -6, NULL),
        .surface = SDL_strcmp(luaL_optstring(state, -5, "pixels"), "gpu") == 0 ? ECSSurfaceType_Gpu : ECSSurfaceType_Pixels,
        .stateVersion = (u32)luaL_optinteger(state, -4, 0),
        .continuous = lua_toboolean(state, -3),
        .minWidth = (f32)luaL_optnumber(state, -2, 0.0),
        .minHeight = (f32)luaL_optnumber(state, -1, 0.0),
        .Create = ECSI_BindingsPanelCreate,
        .Destroy = ECSI_BindingsPanelDestroy,
        .Draw = ECSI_BindingsHasFunction(state, 1, "draw") ? ECSI_BindingsPanelDraw : NULL,
        .Event = ECSI_BindingsHasFunction(state, 1, "event") ? ECSI_BindingsPanelEvent : NULL,
        .SaveState = ECSI_BindingsHasFunction(state, 1, "save_state") ? ECSI_BindingsPanelSaveState : NULL,
        .Save = ECSI_BindingsHasFunction(state, 1, "save") ? ECSI_BindingsPanelSave : NULL,
    };

    ECSI_LuaPanelType *type = SDL_malloc(sizeof(ECSI_LuaPanelType));

    if (type == NULL)
    {
        return luaL_error(state, "out of memory");
    }

    lua_pushvalue(state, 1);
    type->table = luaL_ref(state, LUA_REGISTRYINDEX);
    type->plugin = plugin;
    SHUResult result = ECSI_PanelTypeRegister(plugin, &desc, type);

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

static int ECSI_BindingsPanelRedraw(lua_State *state)
{
    ECSPanel_Redraw(ECSI_BindingsCheckPanel(state, 1));
    return 0;
}

static int ECSI_BindingsPanelGetTitle(lua_State *state)
{
    lua_pushstring(state, ECSPanel_GetTitle(ECSI_BindingsCheckPanel(state, 1)));
    return 1;
}

static int ECSI_BindingsPanelSetTitle(lua_State *state)
{
    ECSPanel_SetTitle(ECSI_BindingsCheckPanel(state, 1), luaL_checkstring(state, 2));
    return 0;
}

static int ECSI_BindingsPanelStartTimer(lua_State *state)
{
    ECSPanel panel = ECSI_BindingsCheckPanel(state, 1);
    luaL_argcheck(state, panel->type != NULL, 1, "the panel has no type");
    return ECSI_BindingsStartTimer(state, panel->type->plugin, panel, 2);
}

static const luaL_Reg ECSI_BINDINGS_PANEL_METHODS[] = {
    {"redraw", ECSI_BindingsPanelRedraw},
    {"get_title", ECSI_BindingsPanelGetTitle},
    {"set_title", ECSI_BindingsPanelSetTitle},
    {"start_timer", ECSI_BindingsPanelStartTimer},
    {NULL, NULL},
};

static const luaL_Reg ECSI_BINDINGS_PANEL[] = {
    {"register_type", ECSI_BindingsPanelRegisterType},
    {"redraw", ECSI_BindingsPanelRedraw},
    {"get_title", ECSI_BindingsPanelGetTitle},
    {"set_title", ECSI_BindingsPanelSetTitle},
    {"start_timer", ECSI_BindingsPanelStartTimer},
    {NULL, NULL},
};

#pragma endregion Panels

#pragma region Surfaces

/// @brief Gets a pixel's address, or NULL if the position is outside the surface.
static u32 *ECSI_BindingsPixel(ECSSurface *surface, lua_Integer x, lua_Integer y)
{
    if (x < 0 || y < 0 || x >= surface->width || y >= surface->height)
    {
        return NULL;
    }

    return (u32 *)((u8 *)surface->pixels.data + (usz)y * (usz)surface->pitch) + x;
}

static int ECSI_BindingsSurfaceSetPixel(lua_State *state)
{
    ECSSurface *surface = ECSI_BindingsCheckSurface(state, 1);
    u32 *pixel = ECSI_BindingsPixel(surface, luaL_checkinteger(state, 2), luaL_checkinteger(state, 3));

    // pixels outside the surface are clipped
    if (pixel != NULL)
    {
        *pixel = (u32)luaL_checkinteger(state, 4);
    }

    return 0;
}

static int ECSI_BindingsSurfaceGetPixel(lua_State *state)
{
    ECSSurface *surface = ECSI_BindingsCheckSurface(state, 1);
    u32 *pixel = ECSI_BindingsPixel(surface, luaL_checkinteger(state, 2), luaL_checkinteger(state, 3));
    luaL_argcheck(state, pixel != NULL, 2, "the position is outside the surface");
    lua_pushinteger(state, (lua_Integer)*pixel);
    return 1;
}

static int ECSI_BindingsSurfaceSetRow(lua_State *state)
{
    ECSSurface *surface = ECSI_BindingsCheckSurface(state, 1);
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

    if (count > 0)
    {
        SDL_memcpy(ECSI_BindingsPixel(surface, x + skip, y), bytes + skip * 4, (usz)count * 4);
    }

    return 0;
}

static int ECSI_BindingsSurfaceIndex(lua_State *state)
{
    ECSSurface *surface = ECSI_BindingsCheckSurface(state, 1);
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

static const luaL_Reg ECSI_BINDINGS_SURFACE_METHODS[] = {
    {"set_pixel", ECSI_BindingsSurfaceSetPixel},
    {"get_pixel", ECSI_BindingsSurfaceGetPixel},
    {"set_row", ECSI_BindingsSurfaceSetRow},
    {NULL, NULL},
};

#pragma endregion Surfaces

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
    ECSI_BindingsAddTable(state, plugin, "panel", ECSI_BINDINGS_PANEL);

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

    luaL_newmetatable(state, OPENECS_LUA_PANEL);
    luaL_newlib(state, ECSI_BINDINGS_PANEL_METHODS);
    lua_setfield(state, -2, "__index");
    lua_pop(state, 1);

    luaL_newmetatable(state, OPENECS_LUA_SURFACE);
    luaL_newlib(state, ECSI_BINDINGS_SURFACE_METHODS);
    lua_pushcclosure(state, ECSI_BindingsSurfaceIndex, 1);
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

    // the types' tables go with the Lua state
    for (usz i = 0; i < arrlenu(BINDINGS.types); i++)
    {
        SDL_free(BINDINGS.types[i]);
    }

    shfree(BINDINGS.errors);
    arrfree(BINDINGS.types);
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
