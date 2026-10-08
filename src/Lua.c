#include "Lua.h"

#include "lua/lua.h"
#include "lua/lauxlib.h"
#include "lua/lualib.h"

#include "SDL3/SDL.h"

#pragma region Source Only

static struct
{
    lua_State *state;
    int dataBase; // stack top before the open data file
    bool dataOpen;
} LUA = {0};

/// @brief Deepest nesting of tables that converts to a value; deeper tables are most likely cycles.
#define OPENECS_MAX_VALUE_DEPTH 64

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

    for (usz i = 0; i < SDL_arraysize(ECSI_LUA_DATA_FUNCTIONS); i++)
    {
        lua_getglobal(state, ECSI_LUA_DATA_FUNCTIONS[i]);
        lua_setfield(state, -2, ECSI_LUA_DATA_FUNCTIONS[i]);
    }

    for (usz i = 0; i < SDL_arraysize(ECSI_LUA_DATA_LIBRARIES); i++)
    {
        lua_getglobal(state, ECSI_LUA_DATA_LIBRARIES[i]);
        lua_setfield(state, -2, ECSI_LUA_DATA_LIBRARIES[i]);
    }
}

/// @brief Copies the Lua value at an index of the stack into a value.
static SHUResult ECSI_LuaToValue(int index, ECSValue *value, u32 depth)
{
    lua_State *state = LUA.state;
    index = lua_absindex(state, index);

    switch (lua_type(state, index))
    {
    case LUA_TNIL:
        ECSValue_SetNil(value);
        return SHUResult_Ok;
    case LUA_TBOOLEAN:
        ECSValue_SetBool(value, lua_toboolean(state, index));
        return SHUResult_Ok;
    case LUA_TNUMBER:
        if (lua_isinteger(state, index))
        {
            ECSValue_SetInteger(value, (i64)lua_tointeger(state, index));
        }
        else
        {
            ECSValue_SetNumber(value, (f64)lua_tonumber(state, index));
        }

        return SHUResult_Ok;
    case LUA_TSTRING:
        return ECSValue_SetString(value, lua_tostring(state, index));
    case LUA_TTABLE:
        break;
    default:
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "A %s is not data; it is skipped.", luaL_typename(state, index));
        ECSValue_SetNil(value);
        return SHUResult_Ok;
    }

    if (depth == OPENECS_MAX_VALUE_DEPTH)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Tables are nested more than %d deep, or hold themselves.", OPENECS_MAX_VALUE_DEPTH);
        return SHUResult_ErrBadData;
    }

    ECSValue_SetTable(value);
    usz count = (usz)lua_rawlen(state, index);

    for (usz i = 1; i <= count; i++)
    {
        ECSValue *item = NULL;
        SHU_ReturnResult(ECSValue_AddItem(value, &item));

        lua_rawgeti(state, index, (lua_Integer)i);
        SHUResult result = ECSI_LuaToValue(-1, item, depth + 1);
        lua_pop(state, 1);
        SHU_ReturnResult(result);
    }

    lua_pushnil(state);

    while (lua_next(state, index) != 0)
    {
        SHUResult result = SHUResult_Ok;

        // lua_tostring would change a number key in place and break lua_next, so check the types first
        if (lua_type(state, -2) == LUA_TSTRING)
        {
            ECSValue *field = NULL;
            result = ECSValue_SetField(value, lua_tostring(state, -2), &field);
            result = result ? result : ECSI_LuaToValue(-1, field, depth + 1);
        }
        else if (!lua_isinteger(state, -2) || lua_tointeger(state, -2) < 1 || (usz)lua_tointeger(state, -2) > count)
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "A table key that is not a text or a list position is skipped.");
        }

        lua_pop(state, 1);
        SHU_ReturnResult(result, lua_pop(state, 1););
    }

    return SHUResult_Ok;
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

    SDL_zero(LUA);
}

SHUResult ECSI_LuaDataOpen(const char *path)
{
    SDL_assert(LUA.state != NULL);
    SDL_assert(path != NULL);
    SDL_assert(!LUA.dataOpen); // only one data file is open at a time

    lua_State *state = LUA.state;
    LUA.dataBase = lua_gettop(state);

    if (luaL_loadfilex(state, path, "t") != LUA_OK)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cannot read '%s': %s", path, lua_tostring(state, -1));
        lua_settop(state, LUA.dataBase);
        return SHUResult_ErrFile;
    }

    // the first upvalue of a loaded chunk is its environment
    ECSI_LuaPushDataEnvironment();
    lua_setupvalue(state, -2, 1);

    if (lua_pcall(state, 0, 1, 0) != LUA_OK)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Error in '%s': %s", path, lua_tostring(state, -1));
        lua_settop(state, LUA.dataBase);
        return SHUResult_ErrBadData;
    }

    if (!lua_istable(state, -1))
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "'%s' does not return a table.", path);
        lua_settop(state, LUA.dataBase);
        return SHUResult_ErrBadData;
    }

    LUA.dataOpen = true;
    return SHUResult_Ok;
}

void ECSI_LuaDataClose(void)
{
    SDL_assert(LUA.dataOpen);

    lua_settop(LUA.state, LUA.dataBase);
    LUA.dataOpen = false;
}

bool ECSI_LuaDataEnterField(const char *key)
{
    SDL_assert(LUA.dataOpen);

    if (lua_getfield(LUA.state, -1, key) == LUA_TTABLE)
    {
        return true;
    }

    lua_pop(LUA.state, 1);
    return false;
}

bool ECSI_LuaDataEnterIndex(usz index)
{
    SDL_assert(LUA.dataOpen);

    if (lua_geti(LUA.state, -1, (lua_Integer)index) == LUA_TTABLE)
    {
        return true;
    }

    lua_pop(LUA.state, 1);
    return false;
}

void ECSI_LuaDataLeave(void)
{
    SDL_assert(LUA.dataOpen && lua_gettop(LUA.state) > LUA.dataBase + 1);

    lua_pop(LUA.state, 1);
}

usz ECSI_LuaDataCount(void)
{
    SDL_assert(LUA.dataOpen);

    return (usz)lua_rawlen(LUA.state, -1);
}

bool ECSI_LuaDataHas(const char *key)
{
    SDL_assert(LUA.dataOpen);

    bool has = lua_getfield(LUA.state, -1, key) != LUA_TNIL;
    lua_pop(LUA.state, 1);
    return has;
}

const char *ECSI_LuaDataGetText(const char *key, const char *fallback)
{
    SDL_assert(LUA.dataOpen);

    // the table keeps the text alive after it is popped
    const char *text = lua_getfield(LUA.state, -1, key) == LUA_TSTRING ? lua_tostring(LUA.state, -1) : fallback;
    lua_pop(LUA.state, 1);
    return text;
}

f64 ECSI_LuaDataGetNumber(const char *key, f64 fallback)
{
    SDL_assert(LUA.dataOpen);

    f64 number = lua_getfield(LUA.state, -1, key) == LUA_TNUMBER ? (f64)lua_tonumber(LUA.state, -1) : fallback;
    lua_pop(LUA.state, 1);
    return number;
}

SHUResult ECSI_LuaDataGetValue(const char *key, ECSValue *value)
{
    SDL_assert(LUA.dataOpen);
    SDL_assert(value != NULL);

    if (key == NULL)
    {
        return ECSI_LuaToValue(-1, value, 0);
    }

    lua_getfield(LUA.state, -1, key);
    SHUResult result = ECSI_LuaToValue(-1, value, 0);
    lua_pop(LUA.state, 1);
    return result;
}

void ECSI_LuaDataForEachText(ECSI_LuaTextFunction function, void *userData)
{
    SDL_assert(LUA.dataOpen);
    SDL_assert(function != NULL);

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
