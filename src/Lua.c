#include "Lua.h"

#include "lua/lua.h"
#include "lua/lauxlib.h"
#include "lua/lualib.h"

#include "SDL3/SDL.h"
#include "stb/stbSDL3.h"

#pragma region Source Only

static struct
{
    lua_State *state;
    int writer; // registry reference of the writer function
} LUA = {0};

/// @brief The writer of data files. It takes a value as ECSI_LuaPushOrderedValue pushes it, and returns Lua source that returns the value.
static const char ECSI_LUA_WRITER[] =
    "local format, concat = string.format, table.concat\n"
    "local keywords = {}\n"
    "for word in ('and break do else elseif end false for function global goto if in local nil not or repeat return then true until while'):gmatch('%a+') do keywords[word] = true end\n"
    "local function number(n)\n"
    "  if math.type(n) == 'integer' then return format('%d', n) end\n"
    "  if n ~= n or n == math.huge or n == -math.huge then return format('%q', n) end\n"
    "  local text = format('%.15g', n)\n"
    "  if tonumber(text) ~= n then text = format('%.17g', n) end\n"
    "  if not text:find('[%.eEn]') then text = text .. '.0' end\n"
    "  return text\n"
    "end\n"
    "local function key(name)\n"
    "  if name:match('^[%a_][%w_]*$') and not keywords[name] then return name end\n"
    "  return '[' .. format('%q', name) .. ']'\n"
    "end\n"
    "local function write(value, indent, out)\n"
    "  local kind = type(value)\n"
    "  if kind == 'table' then\n"
    "    if value.items.n == 0 and #value.fields == 0 then out[#out + 1] = '{}' return end\n"
    "    local inner = indent .. '  '\n"
    "    out[#out + 1] = '{\\n'\n"
    "    for i = 1, #value.fields, 2 do out[#out + 1] = inner .. key(value.fields[i]) .. ' = ' write(value.fields[i + 1], inner, out) out[#out + 1] = ',\\n' end\n"
    "    for i = 1, value.items.n do out[#out + 1] = inner write(value.items[i], inner, out) out[#out + 1] = ',\\n' end\n"
    "    out[#out + 1] = indent .. '}'\n"
    "  elseif kind == 'string' then out[#out + 1] = format('%q', value)\n"
    "  elseif kind == 'number' then out[#out + 1] = number(value)\n"
    "  else out[#out + 1] = tostring(value) end\n"
    "end\n"
    "return function(value)\n"
    "  local out = { '-- written by OpenECS\\nreturn ' }\n"
    "  write(value, '', out)\n"
    "  out[#out + 1] = '\\n'\n"
    "  return concat(out)\n"
    "end\n";

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

static int ECSI_LuaCompareTexts(const void *first, const void *second)
{
    return SDL_strcmp(*(const char *const *)first, *(const char *const *)second);
}

/// @brief Copies the Lua value at an index of the stack into a value. Named fields are added in the order of their names, so the same table always gives the same value.
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

    // the names stay alive in the table while it is on the stack
    const char **names = NULL;
    lua_pushnil(state);

    while (lua_next(state, index) != 0)
    {
        // lua_tostring would change a number key in place and break lua_next, so check the types first
        if (lua_type(state, -2) == LUA_TSTRING)
        {
            arrput(names, lua_tostring(state, -2));
        }
        else if (!lua_isinteger(state, -2) || lua_tointeger(state, -2) < 1 || (usz)lua_tointeger(state, -2) > count)
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "A table key that is not a text or a list position is skipped.");
        }

        lua_pop(state, 1);
    }

    SDL_qsort(names, arrlenu(names), sizeof(*names), ECSI_LuaCompareTexts);
    SHUResult result = SHUResult_Ok;

    for (usz i = 0; i < arrlenu(names) && !result; i++)
    {
        ECSValue *field = NULL;
        result = ECSValue_SetField(value, names[i], &field);

        if (!result)
        {
            lua_pushstring(state, names[i]);
            lua_rawget(state, index);
            result = ECSI_LuaToValue(-1, field, depth + 1);
            lua_pop(state, 1);
        }
    }

    arrfree(names);
    return result;
}

/// @brief Pushes a value for the writer, keeping the order of named fields: a table becomes { items = { n = count, ... }, fields = { name, value, ... } }.
static void ECSI_LuaPushOrderedField(const char *name, const ECSValue *field, void *userData);

static void ECSI_LuaPushOrderedValue(const ECSValue *value)
{
    lua_State *state = LUA.state;
    luaL_checkstack(state, 4, "a value is nested too deeply");

    switch (ECSValue_GetType(value))
    {
    case ECSValueType_Nil:
        lua_pushnil(state);
        return;
    case ECSValueType_Bool:
        lua_pushboolean(state, ECSValue_GetBool(value, false));
        return;
    case ECSValueType_Integer:
        lua_pushinteger(state, (lua_Integer)ECSValue_GetInteger(value, 0));
        return;
    case ECSValueType_Number:
        lua_pushnumber(state, (lua_Number)ECSValue_GetNumber(value, 0.0));
        return;
    case ECSValueType_String:
        lua_pushstring(state, ECSValue_GetString(value, ""));
        return;
    case ECSValueType_Table:
        break;
    }

    usz count = ECSValue_GetCount(value);
    lua_createtable(state, 0, 2);
    lua_createtable(state, (int)SDL_min(count, (usz)SDL_MAX_SINT32), 1);

    for (usz i = 0; i < count; i++)
    {
        ECSI_LuaPushOrderedValue(ECSValue_GetItem(value, i));
        lua_rawseti(state, -2, (lua_Integer)i + 1);
    }

    lua_pushinteger(state, (lua_Integer)count);
    lua_setfield(state, -2, "n");
    lua_setfield(state, -2, "items");

    lua_newtable(state);
    ECSI_ValueForEachField(value, ECSI_LuaPushOrderedField, NULL);
    lua_setfield(state, -2, "fields");
}

static void ECSI_LuaPushOrderedField(const char *name, const ECSValue *field, void *userData)
{
    (void)userData;
    lua_State *state = LUA.state;
    lua_Integer length = (lua_Integer)lua_rawlen(state, -1);

    lua_pushstring(state, name);
    lua_rawseti(state, -2, length + 1);
    ECSI_LuaPushOrderedValue(field);
    lua_rawseti(state, -2, length + 2);
}

/// @brief Adds a stack trace to the message of an error raised in a protected call.
static int ECSI_LuaTraceback(lua_State *state)
{
    luaL_traceback(state, state, luaL_tolstring(state, 1, NULL), 1);
    return 1;
}

/// @brief Runs the writer on the value given as a light userdata, inside a protected call, so pushing the value cannot fail outside one.
static int ECSI_LuaWriteProtected(lua_State *state)
{
    const ECSValue *value = lua_touserdata(state, 1);

    lua_rawgeti(state, LUA_REGISTRYINDEX, LUA.writer);
    ECSI_LuaPushOrderedValue(value);
    lua_call(state, 1, 1);
    return 1;
}

#pragma endregion Source Only

SHUResult ECSI_LuaInitialize(void)
{
    LUA.state = luaL_newstate();

    if (LUA.state == NULL)
    {
        return SHUResult_ErrAllocation;
    }

    // every library is open for plugins; data files see only a few, through their own environment
    luaL_openlibs(LUA.state);

    if (luaL_loadbufferx(LUA.state, ECSI_LUA_WRITER, sizeof(ECSI_LUA_WRITER) - 1, "=writer", "t") != LUA_OK || lua_pcall(LUA.state, 0, 1, 0) != LUA_OK)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "The data file writer does not load: %s", lua_tostring(LUA.state, -1));
        return SHUResult_ErrInternal;
    }

    LUA.writer = luaL_ref(LUA.state, LUA_REGISTRYINDEX);
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

lua_State *ECSI_LuaGetState(void)
{
    SDL_assert(LUA.state != NULL);

    return LUA.state;
}

SHUResult ECSI_LuaGetValue(int index, ECSValue *value)
{
    SDL_assert(LUA.state != NULL);
    SDL_assert(value != NULL);

    return ECSI_LuaToValue(index, value, 0);
}

static void ECSI_LuaPushField(const char *name, const ECSValue *field, void *userData)
{
    (void)userData;
    ECSI_LuaPushValue(field);
    lua_setfield(LUA.state, -2, name);
}

void ECSI_LuaPushValue(const ECSValue *value)
{
    SDL_assert(LUA.state != NULL);

    lua_State *state = LUA.state;
    luaL_checkstack(state, 3, "a value is nested too deeply");

    if (ECSValue_GetType(value) != ECSValueType_Table)
    {
        switch (ECSValue_GetType(value))
        {
        case ECSValueType_Bool:
            lua_pushboolean(state, ECSValue_GetBool(value, false));
            return;
        case ECSValueType_Integer:
            lua_pushinteger(state, (lua_Integer)ECSValue_GetInteger(value, 0));
            return;
        case ECSValueType_Number:
            lua_pushnumber(state, (lua_Number)ECSValue_GetNumber(value, 0.0));
            return;
        case ECSValueType_String:
            lua_pushstring(state, ECSValue_GetString(value, ""));
            return;
        default:
            lua_pushnil(state);
            return;
        }
    }

    usz count = ECSValue_GetCount(value);
    lua_createtable(state, (int)SDL_min(count, (usz)SDL_MAX_SINT32), 0);

    for (usz i = 0; i < count; i++)
    {
        ECSI_LuaPushValue(ECSValue_GetItem(value, i));
        lua_rawseti(state, -2, (lua_Integer)i + 1);
    }

    ECSI_ValueForEachField(value, ECSI_LuaPushField, NULL);
}

SHUResult ECSI_LuaCall(int argumentCount, int resultCount)
{
    SDL_assert(LUA.state != NULL);

    lua_State *state = LUA.state;
    int base = lua_gettop(state) - argumentCount;

    lua_pushcfunction(state, ECSI_LuaTraceback);
    lua_insert(state, base);
    int status = lua_pcall(state, argumentCount, resultCount, base);
    lua_remove(state, base);
    return status == LUA_OK ? SHUResult_Ok : SHUResult_Err;
}

SHUResult ECSI_LuaReadData(const char *path, ECSValue *retValue)
{
    SDL_assert(LUA.state != NULL);
    SDL_assert(path != NULL);
    SDL_assert(retValue != NULL);

    lua_State *state = LUA.state;
    int top = lua_gettop(state);

    if (luaL_loadfilex(state, path, "t") != LUA_OK)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cannot read '%s': %s", path, lua_tostring(state, -1));
        lua_settop(state, top);
        return SHUResult_ErrFile;
    }

    // the first upvalue of a loaded chunk is its environment
    ECSI_LuaPushDataEnvironment();
    lua_setupvalue(state, -2, 1);

    if (lua_pcall(state, 0, 1, 0) != LUA_OK)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Error in '%s': %s", path, lua_tostring(state, -1));
        lua_settop(state, top);
        return SHUResult_ErrBadData;
    }

    if (!lua_istable(state, -1))
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "'%s' does not return a table.", path);
        lua_settop(state, top);
        return SHUResult_ErrBadData;
    }

    SHUResult result = ECSI_LuaToValue(-1, retValue, 0);
    lua_settop(state, top);
    return result;
}

SHUResult ECSI_LuaWriteData(const char *path, const ECSValue *value)
{
    SDL_assert(LUA.state != NULL);
    SDL_assert(path != NULL);

    lua_State *state = LUA.state;
    int top = lua_gettop(state);

    lua_pushcfunction(state, ECSI_LuaWriteProtected);
    lua_pushlightuserdata(state, (void *)value);

    if (lua_pcall(state, 1, 1, 0) != LUA_OK)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cannot write '%s': %s", path, lua_tostring(state, -1));
        lua_settop(state, top);
        return SHUResult_ErrAllocation;
    }

    usz length = 0;
    const char *text = lua_tolstring(state, -1, &length);

    // the folder of the file
    char *folder = SDL_strdup(path);
    char *temporary = NULL;

    if (folder == NULL || SDL_asprintf(&temporary, "%s.tmp", path) < 0)
    {
        SDL_free(folder);
        lua_settop(state, top);
        return SHUResult_ErrAllocation;
    }

    char *slash = SDL_strrchr(folder, '/');

    if (slash != NULL)
    {
        slash[1] = '\0';
    }

    bool written = (slash == NULL || SDL_CreateDirectory(folder)) && SDL_SaveFile(temporary, text, length) && SDL_RenamePath(temporary, path);

    if (!written)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cannot write '%s': %s", path, SDL_GetError());
        SDL_RemovePath(temporary);
    }

    SDL_free(folder);
    SDL_free(temporary);
    lua_settop(state, top);
    return written ? SHUResult_Ok : SHUResult_ErrFile;
}
