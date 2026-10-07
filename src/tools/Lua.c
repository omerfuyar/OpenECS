#include "tools/Lua.h"

#include "lua/lua.h"
#include "lua/lauxlib.h"
#include "lua/lualib.h"

#pragma region Source Only

static struct
{
    lua_State *state;
    int dataBase; // stack top before the open data file
    bool dataOpen;
} LUA = {0};

/// @brief Names of Lua's basic functions that data files may use.
static const char *const ECSI_LUA_DATA_FUNCTIONS[] = {
    "assert", "error", "ipairs", "next", "pairs", "pcall", "rawequal", "rawget", "rawlen",
    "rawset", "select", "tonumber", "tostring", "type", "xpcall", "getmetatable", "setmetatable"};

/// @brief Names of Lua's libraries that data files may use.
static const char *const ECSI_LUA_DATA_LIBRARIES[] = {"string", "table", "math", "utf8"};

/// @brief Pushes the environment in which data files run: basic functions and a few libraries, without access to the core.
static void ECSI_LuaPushDataEnvironment(void)
{
    lua_State *state = LUA.state;
    lua_newtable(state);

    for (usz i = 0; i < sizeof(ECSI_LUA_DATA_FUNCTIONS) / sizeof(*ECSI_LUA_DATA_FUNCTIONS); i++)
    {
        lua_getglobal(state, ECSI_LUA_DATA_FUNCTIONS[i]);
        lua_setfield(state, -2, ECSI_LUA_DATA_FUNCTIONS[i]);
    }

    for (usz i = 0; i < sizeof(ECSI_LUA_DATA_LIBRARIES) / sizeof(*ECSI_LUA_DATA_LIBRARIES); i++)
    {
        lua_getglobal(state, ECSI_LUA_DATA_LIBRARIES[i]);
        lua_setfield(state, -2, ECSI_LUA_DATA_LIBRARIES[i]);
    }
}

#pragma endregion Source Only

SHUResult ECSI_LuaInitialize(void)
{
    LUA.state = luaL_newstate();

    if (LUA.state == NULL)
    {
        return SHUResult_ErrAllocation;
    }

    luaL_openselectedlibs(LUA.state, LUA_GLIBK | LUA_STRLIBK | LUA_TABLIBK | LUA_MATHLIBK | LUA_UTF8LIBK, 0);
    return SHUResult_Ok;
}

void ECSI_LuaTerminate(void)
{
    if (LUA.state != NULL)
    {
        lua_close(LUA.state);
    }

    LUA = (typeof(LUA)){0};
}

SHUResult ECSI_LuaDataOpen(const char *path)
{
    SHU_AssertNullPointer(LUA.state);
    SHU_AssertNullPointer(path);
    SHU_Assert(!LUA.dataOpen, "A data file is already open.");

    lua_State *state = LUA.state;
    LUA.dataBase = lua_gettop(state);

    if (luaL_loadfilex(state, path, "t") != LUA_OK)
    {
        SHU_LogWarning("Cannot read '%s': %s", path, lua_tostring(state, -1));
        lua_settop(state, LUA.dataBase);
        return SHUResult_ErrFile;
    }

    // the first upvalue of a loaded chunk is its environment
    ECSI_LuaPushDataEnvironment();
    lua_setupvalue(state, -2, 1);

    if (lua_pcall(state, 0, 1, 0) != LUA_OK)
    {
        SHU_LogWarning("Error in '%s': %s", path, lua_tostring(state, -1));
        lua_settop(state, LUA.dataBase);
        return SHUResult_ErrBadData;
    }

    if (!lua_istable(state, -1))
    {
        SHU_LogWarning("'%s' does not return a table.", path);
        lua_settop(state, LUA.dataBase);
        return SHUResult_ErrBadData;
    }

    LUA.dataOpen = true;
    return SHUResult_Ok;
}

void ECSI_LuaDataClose(void)
{
    SHU_Assert(LUA.dataOpen, "No data file is open.");

    lua_settop(LUA.state, LUA.dataBase);
    LUA.dataOpen = false;
}

bool ECSI_LuaDataEnterField(const char *key)
{
    SHU_Assert(LUA.dataOpen, "No data file is open.");

    if (lua_getfield(LUA.state, -1, key) == LUA_TTABLE)
    {
        return true;
    }

    lua_pop(LUA.state, 1);
    return false;
}

bool ECSI_LuaDataEnterIndex(usz index)
{
    SHU_Assert(LUA.dataOpen, "No data file is open.");

    if (lua_geti(LUA.state, -1, (lua_Integer)index) == LUA_TTABLE)
    {
        return true;
    }

    lua_pop(LUA.state, 1);
    return false;
}

void ECSI_LuaDataLeave(void)
{
    SHU_Assert(LUA.dataOpen && lua_gettop(LUA.state) > LUA.dataBase + 1, "No table to leave.");

    lua_pop(LUA.state, 1);
}

usz ECSI_LuaDataCount(void)
{
    SHU_Assert(LUA.dataOpen, "No data file is open.");

    return (usz)lua_rawlen(LUA.state, -1);
}

bool ECSI_LuaDataHas(const char *key)
{
    SHU_Assert(LUA.dataOpen, "No data file is open.");

    bool has = lua_getfield(LUA.state, -1, key) != LUA_TNIL;
    lua_pop(LUA.state, 1);
    return has;
}

const char *ECSI_LuaDataGetText(const char *key, const char *fallback)
{
    SHU_Assert(LUA.dataOpen, "No data file is open.");

    // the table keeps the text alive after it is popped
    const char *text = lua_getfield(LUA.state, -1, key) == LUA_TSTRING ? lua_tostring(LUA.state, -1) : fallback;
    lua_pop(LUA.state, 1);
    return text;
}

f64 ECSI_LuaDataGetNumber(const char *key, f64 fallback)
{
    SHU_Assert(LUA.dataOpen, "No data file is open.");

    f64 number = lua_getfield(LUA.state, -1, key) == LUA_TNUMBER ? (f64)lua_tonumber(LUA.state, -1) : fallback;
    lua_pop(LUA.state, 1);
    return number;
}

void ECSI_LuaDataForEachText(ECSI_LuaTextFunction function, void *userData)
{
    SHU_Assert(LUA.dataOpen, "No data file is open.");
    SHU_AssertNullPointer(function);

    lua_State *state = LUA.state;
    lua_pushnil(state);

    while (lua_next(state, -2) != 0)
    {
        // lua_tostring would change a number key in place and break lua_next, so check the types first
        if (lua_type(state, -2) == LUA_TSTRING && lua_type(state, -1) == LUA_TSTRING)
        {
            function(lua_tostring(state, -2), lua_tostring(state, -1), userData);
        }

        lua_pop(state, 1);
    }
}
