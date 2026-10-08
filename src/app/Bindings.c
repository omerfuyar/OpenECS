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
/// @brief Name of the handle type of panels.
#define OPENECS_LUA_PANEL "ecs.panel"
/// @brief Name of the metatable of surfaces given to draw.
#define OPENECS_LUA_SURFACE "ecs.surface"

/// @brief A Lua function that a setting's owner gave to be told about changes.
typedef struct ECSI_LuaListener
{
    ECSPlugin plugin;
    char *name;   // the setting's name
    int function; // registry reference of the Lua function
} ECSI_LuaListener;

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
    int handle;  // registry reference of the panel's handle, which keeps it the same while the panel lives
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
    ECSI_LuaPanelType **types;     // stb_ds array
    ECSI_LuaListener **listeners; // stb_ds array
} BINDINGS = {0};

/// @brief Names of the event types in Lua, in the order of ECSEventType.
static const char *const ECSI_BINDINGS_EVENT_TYPES[] = {"pointer_down", "pointer_up", "pointer_move", "wheel", "key_down", "key_up", "focused", "unfocused"};

/// @brief Names of the setting types in Lua, in the order of ECSSettingType.
static const char *const ECSI_BINDINGS_SETTING_TYPES[] = {"bool", "integer", "number", "string", "choice", "key", "list", "table", NULL};

/// @brief Gets the plugin that owns the ecs table a function came from. Every function of a plugin's ecs table has the plugin as its upvalue.
static ECSPlugin ECSI_BindingsPlugin(lua_State *state)
{
    return lua_touserdata(state, lua_upvalueindex(1));
}

/// @brief Reads a panel handle. Raises a Lua error if it is not one, or its panel is gone.
static ECSPanel ECSI_BindingsCheckPanel(lua_State *state, int index)
{
    (void)state;
    return ECSI_ServicesCheckHandle(index, OPENECS_LUA_PANEL);
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

/// @brief Tells a Lua owner that its setting changed, with the new value.
static void ECSI_BindingsSettingChanged(void *data)
{
    ECSI_LuaListener *listener = data;
    lua_State *state = ECSI_LuaGetState();

    lua_rawgeti(state, LUA_REGISTRYINDEX, listener->function);
    ECSI_LuaPushValue(ECSSetting_Get(listener->name));

    if (ECSI_LuaCall(1, 0))
    {
        ECSI_PluginReportError(listener->plugin, lua_tostring(state, -1));
        lua_pop(state, 1);
    }
}

static int ECSI_BindingsSettingsDeclare(lua_State *state)
{
    luaL_checktype(state, 1, LUA_TTABLE);
    ECSI_LuaListener *listener = NULL;

    // the listener is made first, because the checks below may raise errors and leave nothing to free but it
    if (lua_getfield(state, 1, "changed") == LUA_TFUNCTION)
    {
        lua_getfield(state, 1, "name");
        listener = SDL_calloc(1, sizeof(ECSI_LuaListener));
        char *name = listener == NULL ? NULL : SDL_strdup(luaL_optstring(state, -1, ""));

        if (name == NULL)
        {
            SDL_free(listener);
            return luaL_error(state, "out of memory");
        }

        lua_pop(state, 1);
        listener->plugin = ECSI_BindingsPlugin(state);
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
        .type = (ECSSettingType)luaL_checkoption(state, -4, NULL, ECSI_BINDINGS_SETTING_TYPES),
        .description = luaL_optstring(state, -3, ""),
        .Changed = listener == NULL ? NULL : ECSI_BindingsSettingChanged,
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
    result = result ? result : ECSI_LuaGetValue(-1, defaultValue);
    bool hasDefault = ECSValue_GetType(defaultValue) != ECSValueType_Nil;
    result = result ? result : ECSI_SettingsDeclarePlugin(ECSI_BindingsPlugin(state), &desc, hasDefault ? defaultValue : NULL);

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

static int ECSI_BindingsSettingsGet(lua_State *state)
{
    const ECSValue *value = ECSSetting_Get(luaL_checkstring(state, 1));
    ECSI_LuaPushValue(value);
    return 1;
}

static int ECSI_BindingsSettingsSet(lua_State *state)
{
    const char *name = luaL_checkstring(state, 1);
    ECSValue *value = NULL;
    SHUResult result = ECSValue_Create(&value);
    result = result ? result : ECSI_LuaGetValue(2, value);
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

/// @brief Pushes a value that a settings function fills, or nil and a message.
static int ECSI_BindingsSettingsPush(lua_State *state, SHUResult (*fill)(const char *name, ECSValue *retValue), const char *name)
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

    ECSI_LuaPushValue(value);
    ECSValue_Destroy(&value);
    return 1;
}

static SHUResult ECSI_BindingsSettingsFillList(const char *name, ECSValue *retValue)
{
    (void)name;
    return ECSSetting_List(retValue);
}

static int ECSI_BindingsSettingsList(lua_State *state)
{
    return ECSI_BindingsSettingsPush(state, ECSI_BindingsSettingsFillList, NULL);
}

static int ECSI_BindingsSettingsExplain(lua_State *state)
{
    return ECSI_BindingsSettingsPush(state, ECSSetting_Explain, luaL_checkstring(state, 1));
}

static const luaL_Reg ECSI_BINDINGS_SETTINGS[] = {
    {"declare", ECSI_BindingsSettingsDeclare},
    {"get", ECSI_BindingsSettingsGet},
    {"set", ECSI_BindingsSettingsSet},
    {"list", ECSI_BindingsSettingsList},
    {"explain", ECSI_BindingsSettingsExplain},
    {NULL, NULL},
};

#pragma endregion Settings

#pragma region Timers

/// @brief Clears the handle a Lua value points to, so Lua cannot reach a destroyed object through it.
static void ECSI_BindingsClearHandle(lua_State *state, int reference)
{
    lua_rawgeti(state, LUA_REGISTRYINDEX, reference);
    *(void **)lua_touserdata(state, -1) = NULL;
    lua_pop(state, 1);
}

static void ECSI_BindingsTimerTick(void *data)
{
    ECSI_LuaTimer *timer = data;
    lua_State *state = ECSI_LuaGetState();

    lua_rawgeti(state, LUA_REGISTRYINDEX, timer->function);

    if (ECSI_LuaCall(0, 0))
    {
        ECSI_PluginReportError(timer->plugin, lua_tostring(state, -1));
        lua_pop(state, 1);
    }
}

/// @brief Frees a Lua timer when the core frees its timer, and clears its handle.
static void ECSI_BindingsTimerRelease(void *data)
{
    ECSI_LuaTimer *timer = data;
    lua_State *state = ECSI_LuaGetState();

    ECSI_BindingsClearHandle(state, timer->handle);

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

#pragma region Layout

/// @brief Names of the zones in Lua, in the order of ECSZone.
static const char *const ECSI_BINDINGS_ZONES[] = {"default", "center", "left", "right", "top", "bottom", NULL};

/// @brief Reads an optional panel handle.
static ECSPanel ECSI_BindingsOptPanel(lua_State *state, int index)
{
    return lua_isnoneornil(state, index) ? NULL : ECSI_BindingsCheckPanel(state, index);
}

static int ECSI_BindingsLayoutOpen(lua_State *state)
{
    const char *type = luaL_checkstring(state, 1);
    ECSPanel target = ECSI_BindingsOptPanel(state, 3);
    ECSZone zone = (ECSZone)luaL_checkoption(state, 4, "default", ECSI_BINDINGS_ZONES);
    ECSValue *saved = NULL;
    ECSPanel panel = NULL;

    SHUResult result = ECSValue_Create(&saved);
    result = result ? result : ECSI_LuaGetValue(2, saved);
    result = result ? result : ECSLayout_Open(ECSI_BindingsPlugin(state), &panel, type, ECSValue_GetType(saved) == ECSValueType_Nil ? NULL : saved, target, zone);
    ECSValue_Destroy(&saved);

    if (result)
    {
        lua_pushnil(state);
        lua_pushfstring(state, "panel '%s' is not opened (%s)", type, SHUResult_String(result));
        return 2;
    }

    ECSI_ServicesPushHandle(OPENECS_LUA_PANEL, panel);
    return 1;
}

static int ECSI_BindingsLayoutMove(lua_State *state)
{
    ECSPanel panel = ECSI_BindingsCheckPanel(state, 1);
    ECSPanel target = ECSI_BindingsCheckPanel(state, 2);
    ECSZone zone = (ECSZone)luaL_checkoption(state, 3, "center", ECSI_BINDINGS_ZONES);

    if (ECSLayout_Move(panel, target, zone))
    {
        lua_pushnil(state);
        lua_pushliteral(state, "the panels are not in one workspace");
        return 2;
    }

    lua_pushboolean(state, true);
    return 1;
}

static int ECSI_BindingsLayoutClose(lua_State *state)
{
    lua_pushboolean(state, ECSLayout_Close(ECSI_BindingsCheckPanel(state, 1)));
    return 1;
}

static int ECSI_BindingsLayoutFocus(lua_State *state)
{
    ECSLayout_Focus(ECSI_BindingsCheckPanel(state, 1));
    return 0;
}

static int ECSI_BindingsLayoutGetFocus(lua_State *state)
{
    (void)state;
    ECSI_ServicesPushHandle(OPENECS_LUA_PANEL, ECSLayout_GetFocus());
    return 1;
}

static const luaL_Reg ECSI_BINDINGS_LAYOUT[] = {
    {"open", ECSI_BindingsLayoutOpen},
    {"move", ECSI_BindingsLayoutMove},
    {"close", ECSI_BindingsLayoutClose},
    {"focus", ECSI_BindingsLayoutFocus},
    {"get_focus", ECSI_BindingsLayoutGetFocus},
    {NULL, NULL},
};

static int ECSI_BindingsWorkspaceCount(lua_State *state)
{
    lua_pushinteger(state, (lua_Integer)ECSWorkspace_GetCount());
    return 1;
}

static int ECSI_BindingsWorkspaceGetCurrent(lua_State *state)
{
    lua_pushinteger(state, (lua_Integer)ECSWorkspace_GetCurrent() + 1);
    return 1;
}

static int ECSI_BindingsWorkspaceGetName(lua_State *state)
{
    lua_Integer index = luaL_checkinteger(state, 1);
    lua_pushstring(state, index >= 1 ? ECSWorkspace_GetName((usz)index - 1) : NULL);
    return 1;
}

static int ECSI_BindingsWorkspaceSwitch(lua_State *state)
{
    lua_Integer index = luaL_checkinteger(state, 1);

    if (index >= 1)
    {
        ECSWorkspace_Switch((usz)index - 1);
    }

    return 0;
}

static const luaL_Reg ECSI_BINDINGS_WORKSPACE[] = {
    {"count", ECSI_BindingsWorkspaceCount},
    {"get_current", ECSI_BindingsWorkspaceGetCurrent},
    {"get_name", ECSI_BindingsWorkspaceGetName},
    {"switch", ECSI_BindingsWorkspaceSwitch},
    {NULL, NULL},
};

#pragma endregion Layout

#pragma region Clipboard

static int ECSI_BindingsClipboardSetText(lua_State *state)
{
    lua_pushboolean(state, ECSClipboard_SetText(luaL_checkstring(state, 1)) == SHUResult_Ok);
    return 1;
}

static int ECSI_BindingsClipboardGetText(lua_State *state)
{
    lua_pushstring(state, ECSClipboard_GetText());
    return 1;
}

static int ECSI_BindingsClipboardSetData(lua_State *state)
{
    usz size = 0;
    const char *mimeType = luaL_checkstring(state, 1);
    const char *bytes = luaL_checklstring(state, 2, &size);
    lua_pushboolean(state, ECSClipboard_SetData(mimeType, (SHUSliceView){.data = bytes, .size = size}) == SHUResult_Ok);
    return 1;
}

static int ECSI_BindingsClipboardGetData(lua_State *state)
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

static const luaL_Reg ECSI_BINDINGS_CLIPBOARD[] = {
    {"set_text", ECSI_BindingsClipboardSetText},
    {"get_text", ECSI_BindingsClipboardGetText},
    {"set_data", ECSI_BindingsClipboardSetData},
    {"get_data", ECSI_BindingsClipboardGetData},
    {NULL, NULL},
};

#pragma endregion Clipboard

#pragma region Dialogs

/// @brief Names of the dialog types in Lua, in the order of ECSDialogType.
static const char *const ECSI_BINDINGS_DIALOG_TYPES[] = {"open_file", "save_file", "open_folder", NULL};

/// @brief A Lua function waiting for a file dialog's answer.
typedef struct ECSI_LuaDialog
{
    ECSPlugin plugin;
    int function; // registry reference
} ECSI_LuaDialog;

static void ECSI_BindingsDialogDone(void *data, const char *const *files, usz count)
{
    ECSI_LuaDialog *dialog = data;
    lua_State *state = ECSI_LuaGetState();

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

    if (ECSI_LuaCall(1, 0))
    {
        ECSI_PluginReportError(dialog->plugin, lua_tostring(state, -1));
        lua_pop(state, 1);
    }

    luaL_unref(state, LUA_REGISTRYINDEX, dialog->function);
    SDL_free(dialog);
}

/// @brief ecs.dialog.show({ type, filters = { { name, pattern } }, location, many }, function(files) end)
static int ECSI_BindingsDialogShow(lua_State *state)
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
        .type = (ECSDialogType)luaL_checkoption(state, -4, "open_file", ECSI_BINDINGS_DIALOG_TYPES),
        .filters = filters,
        .filterCount = arrlenu(filters),
        .location = lua_tostring(state, -3),
        .many = lua_toboolean(state, -2),
        .Done = ECSI_BindingsDialogDone,
    };

    ECSI_LuaDialog *dialog = SDL_malloc(sizeof(ECSI_LuaDialog));

    if (dialog == NULL)
    {
        arrfree(filters);
        return luaL_error(state, "out of memory");
    }

    lua_pushvalue(state, 2);
    dialog->function = luaL_ref(state, LUA_REGISTRYINDEX);
    dialog->plugin = ECSI_BindingsPlugin(state);
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
static int ECSI_BindingsDialogMessage(lua_State *state)
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

static const luaL_Reg ECSI_BINDINGS_DIALOG[] = {
    {"show", ECSI_BindingsDialogShow},
    {"message", ECSI_BindingsDialogMessage},
    {NULL, NULL},
};

#pragma endregion Dialogs

#pragma region Input

static int ECSI_BindingsInputBind(lua_State *state)
{
    const char *panelType = luaL_checkstring(state, 1);
    const char *setting = luaL_checkstring(state, 2);
    const char *function = luaL_checkstring(state, 3);
    SHUResult result = ECSKey_Bind(ECSI_BindingsPlugin(state), panelType, setting, function);

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

static const luaL_Reg ECSI_BINDINGS_INPUT[] = {
    {"bind", ECSI_BindingsInputBind},
    {NULL, NULL},
};

#pragma endregion Input

#pragma region Services

static int ECSI_BindingsServiceGet(lua_State *state)
{
    const char *name = luaL_checkstring(state, 1);
    const char *signature = luaL_optstring(state, 2, NULL);
    SHUResult result = ECSI_ServicesPushFunction(ECSI_BindingsPlugin(state), name, signature);

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
static int ECSI_BindingsServiceRegister(lua_State *state)
{
    const char *prefix = luaL_checkstring(state, 1);
    luaL_checktype(state, 2, LUA_TTABLE);
    ECSPlugin plugin = ECSI_BindingsPlugin(state);
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
        SHUResult registered = ECSI_ServicesRegisterLua(plugin, name, luaL_checkstring(state, -4), luaL_optstring(state, -3, ""));

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

static const luaL_Reg ECSI_BINDINGS_SERVICE[] = {
    {"register", ECSI_BindingsServiceRegister},
    {"get", ECSI_BindingsServiceGet},
    {NULL, NULL},
};

#pragma endregion Services

#pragma region Panels

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
    ECSI_PluginReportError(luaPanel->type->plugin, message);
    ECSI_PanelFault(luaPanel->panel, message);
    lua_pop(state, 1);
}

static void ECSI_BindingsPanelFree(lua_State *state, ECSI_LuaPanel *luaPanel)
{
    ECSI_ServicesForgetHandle(luaPanel->panel);
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

    ECSI_ServicesPushHandle(OPENECS_LUA_PANEL, panel);
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
            ECSI_PluginReportError(luaPanel->type->plugin, lua_tostring(state, -1));
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

static int ECSI_BindingsPanelSetUnsaved(lua_State *state)
{
    ECSPanel_SetUnsaved(ECSI_BindingsCheckPanel(state, 1), lua_toboolean(state, 2));
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
    {"set_unsaved", ECSI_BindingsPanelSetUnsaved},
    {"start_timer", ECSI_BindingsPanelStartTimer},
    {NULL, NULL},
};

static const luaL_Reg ECSI_BINDINGS_PANEL[] = {
    {"register_type", ECSI_BindingsPanelRegisterType},
    {"redraw", ECSI_BindingsPanelRedraw},
    {"get_title", ECSI_BindingsPanelGetTitle},
    {"set_title", ECSI_BindingsPanelSetTitle},
    {"set_unsaved", ECSI_BindingsPanelSetUnsaved},
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

    if (pixel == NULL)
    {
        return luaL_argerror(state, 2, "the position is outside the surface");
    }

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

    u32 *first = ECSI_BindingsPixel(surface, x + skip, y);

    if (count > 0 && first != NULL)
    {
        SDL_memcpy(first, bytes + skip * 4, (usz)count * 4);
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
    ECSI_BindingsAddTable(state, plugin, "service", ECSI_BINDINGS_SERVICE);
    ECSI_BindingsAddTable(state, plugin, "input", ECSI_BINDINGS_INPUT);
    ECSI_BindingsAddTable(state, plugin, "layout", ECSI_BINDINGS_LAYOUT);
    ECSI_BindingsAddTable(state, plugin, "workspace", ECSI_BINDINGS_WORKSPACE);
    ECSI_BindingsAddTable(state, plugin, "clipboard", ECSI_BINDINGS_CLIPBOARD);
    ECSI_BindingsAddTable(state, plugin, "dialog", ECSI_BINDINGS_DIALOG);

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

    ECSI_ServicesPushHandleMetatable(OPENECS_LUA_PANEL);
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
