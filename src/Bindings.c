#include "Bindings.h"

#include "Lua.h"

#include "lua/lauxlib.h"
#include "lua/lua.h"

#include "SDL3/SDL.h"

#pragma region Source Only

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

    lua_newtable(state);
    lua_pushstring(state, ECSI_PluginGetName(plugin));
    lua_setfield(state, -2, "name");
    lua_pushstring(state, ECSI_PluginGetVersion(plugin));
    lua_setfield(state, -2, "version");
    lua_setfield(state, -2, "plugin");
}

#pragma endregion Source Only

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
