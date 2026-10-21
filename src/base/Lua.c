#include "base/Lua.h"

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
    ECSILuaReadHandleFunction ReadHandle; // set by the Services module
    ECSILuaPushHandleFunction PushHandle;
} LUA = {0};

/// @brief The writer of data files. It takes a value as ECSILua_PushOrderedValue pushes it, and returns Lua source that returns the value.
static const char OPENECS_LUA_WRITER[] =
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
static const char *const OPENECS_LUA_DATA_FUNCTIONS[] = {
    "assert", "error", "ipairs", "next", "pairs", "pcall", "rawequal", "rawget", "rawlen",
    "rawset", "select", "tonumber", "tostring", "type", "xpcall", "getmetatable", "setmetatable"};

/// @brief Names of Lua's libraries that data files may use.
static const char *const OPENECS_LUA_DATA_LIBRARIES[] = {"string", "table", "math", "utf8"};

/// @brief Pushes the environment in which data files run: basic functions and a few libraries, without access to the core.
static void ECSILua_PushDataEnvironment(void)
{
    lua_State *state = LUA.state;
    lua_newtable(state);

    for (usz i = 0; i < SDL_arraysize(OPENECS_LUA_DATA_FUNCTIONS); i++)
    {
        lua_getglobal(state, OPENECS_LUA_DATA_FUNCTIONS[i]);
        lua_setfield(state, -2, OPENECS_LUA_DATA_FUNCTIONS[i]);
    }

    for (usz i = 0; i < SDL_arraysize(OPENECS_LUA_DATA_LIBRARIES); i++)
    {
        lua_getglobal(state, OPENECS_LUA_DATA_LIBRARIES[i]);
        lua_setfield(state, -2, OPENECS_LUA_DATA_LIBRARIES[i]);
    }
}

static int ECSILua_CompareTexts(const void *first, const void *second)
{
    return SDL_strcmp(*(const char *const *)first, *(const char *const *)second);
}

/// @brief Copies the Lua value at an index of the stack into a value. Named fields are added in the order of their names, so the same table always gives the same value.
/// @param handles Whether a handle stays a handle; without it, a handle is skipped, so the value can go to another thread.
static SHUResult ECSILua_ToValue(lua_State *state, int index, ECSValue *value, bool handles, u32 depth)
{
    index = lua_absindex(state, index);

    switch (lua_type(state, index))
    {
    // a missing argument is nil, so optional values may be left out
    case LUA_TNONE:
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
    {
        // a string with a zero byte is bytes, which a C string cannot hold
        usz length = 0;
        const char *text = lua_tolstring(state, index, &length);
        return SDL_strlen(text) == length ? ECSValue_SetString(value, text) : ECSValue_SetBuffer(value, (SHUSliceView){.data = text, .size = length});
    }
    case LUA_TUSERDATA:
    {
        const char *type = nullptr;
        void *object = nullptr;

        // the value keeps the handle itself, so the handle it gives back is gone once its object is, and the object lives while the value does
        if (handles && LUA.ReadHandle != nullptr && LUA.ReadHandle(index, &type, &object))
        {
            lua_pushvalue(state, index);
            return ECSIValue_SetLuaHandle(value, type, object, luaL_ref(state, LUA_REGISTRYINDEX));
        }

        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, handles ? "A userdata that is not a handle, or whose object is gone, is not data; it is skipped." : "A userdata is not data that can go to another thread; it is skipped.");
        ECSValue_SetNil(value);
        return SHUResult_Ok;
    }
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
        ECSValue *item = nullptr;
        SHU_ReturnResult(ECSValue_AddListItem(value, &item));

        lua_rawgeti(state, index, (lua_Integer)i);
        SHUResult result = ECSILua_ToValue(state, -1, item, handles, depth + 1);
        lua_pop(state, 1);
        SHU_ReturnResult(result);
    }

    // the names stay alive in the table while it is on the stack
    const char **names = nullptr;
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

    SDL_qsort(names, arrlenu(names), sizeof(*names), ECSILua_CompareTexts);
    SHUResult result = SHUResult_Ok;

    for (usz i = 0; i < arrlenu(names) && !result; i++)
    {
        ECSValue *field = nullptr;
        result = ECSValue_SetTableField(value, names[i], &field);

        if (!result)
        {
            lua_pushstring(state, names[i]);
            lua_rawget(state, index);
            result = ECSILua_ToValue(state, -1, field, handles, depth + 1);
            lua_pop(state, 1);
        }
    }

    arrfree(names);
    return result;
}

/// @brief Pushes a value for the writer, keeping the order of named fields: a table becomes { items = { n = count, ... }, fields = { name, value, ... } }.
static void ECSILua_PushOrderedField(const char *name, const ECSValue *field, void *userData);

static void ECSILua_PushOrderedValue(const ECSValue *value)
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
    case ECSValueType_Buffer:
    {
        SHUSlice bytes = ECSValue_GetBuffer(value, (SHUSlice){0});
        lua_pushlstring(state, bytes.data, bytes.size);
        return;
    }
    case ECSValueType_Handle:
        // a file holds only data, so a handle is left out
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "A handle is not data that a file can hold; it is left out.");
        lua_pushnil(state);
        return;
    case ECSValueType_Table:
        break;
    }

    usz count = ECSValue_GetListCount(value);
    lua_createtable(state, 0, 2);
    lua_createtable(state, (int)SDL_min(count, (usz)SDL_MAX_SINT32), 1);

    for (usz i = 0; i < count; i++)
    {
        ECSILua_PushOrderedValue(ECSValue_GetListItem(value, i));
        lua_rawseti(state, -2, (lua_Integer)i + 1);
    }

    lua_pushinteger(state, (lua_Integer)count);
    lua_setfield(state, -2, "n");
    lua_setfield(state, -2, "items");

    lua_newtable(state);
    ECSIValue_TableForEachField(value, ECSILua_PushOrderedField, nullptr);
    lua_setfield(state, -2, "fields");
}

static void ECSILua_PushOrderedField(const char *name, const ECSValue *field, void *userData)
{
    (void)userData;
    lua_State *state = LUA.state;
    lua_Integer length = (lua_Integer)lua_rawlen(state, -1);

    // a field that is a handle is left out with its name, so the names and values after it keep their places
    if (ECSValue_GetType(field) == ECSValueType_Handle)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "A handle is not data that a file can hold; the field '%s' is left out.", name);
        return;
    }

    lua_pushstring(state, name);
    lua_rawseti(state, -2, length + 1);
    ECSILua_PushOrderedValue(field);
    lua_rawseti(state, -2, length + 2);
}

/// @brief Adds a stack trace to the message of an error raised in a protected call.
static int ECSILua_Traceback(lua_State *state)
{
    luaL_traceback(state, state, luaL_tolstring(state, 1, nullptr), 1);
    return 1;
}

/// @brief Runs the writer on the value given as a light userdata, inside a protected call, so pushing the value cannot fail outside one.
static int ECSILua_WriteProtected(lua_State *state)
{
    const ECSValue *value = lua_touserdata(state, 1);

    lua_rawgeti(state, LUA_REGISTRYINDEX, LUA.writer);
    ECSILua_PushOrderedValue(value);
    lua_call(state, 1, 1);
    return 1;
}

/// @brief Lua's allocator: SDL's, like the rest of the core.
static void *ECSILua_Allocate(void *userData, void *block, size_t oldSize, size_t newSize)
{
    (void)userData;
    (void)oldSize;

    if (newSize == 0)
    {
        SDL_free(block);
        return nullptr;
    }

    return SDL_realloc(block, newSize);
}

/// @brief Logs an error raised outside a protected call. Lua aborts the program after it.
static int ECSILua_Panic(lua_State *state)
{
    const char *message = lua_tostring(state, -1);
    SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "Lua error outside a protected call: %s", message == nullptr ? "?" : message);
    return 0;
}

/// @brief Logs Lua's warnings. A warning may come in pieces, which are joined first; control messages such as "@on" are ignored.
static void ECSILua_Warn(void *userData, const char *message, int toContinue)
{
    (void)userData;
    static char *pending = nullptr;

    if (pending == nullptr && !toContinue && message[0] == '@')
    {
        return;
    }

    char *joined = nullptr;

    if (SDL_asprintf(&joined, "%s%s", pending == nullptr ? "" : pending, message) < 0)
    {
        joined = nullptr;
    }

    SDL_free(pending);
    pending = joined;

    if (!toContinue && pending != nullptr)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Lua: %s", pending);
        SDL_free(pending);
        pending = nullptr;
    }
}

/// @brief Takes another reference to a Lua handle that a value keeps.
static int ECSILua_KeepHandle(int reference)
{
    lua_rawgeti(LUA.state, LUA_REGISTRYINDEX, reference);
    return luaL_ref(LUA.state, LUA_REGISTRYINDEX);
}

static void ECSILua_DropHandle(int reference)
{
    luaL_unref(LUA.state, LUA_REGISTRYINDEX, reference);
}

/// @brief Gives the object of a Lua handle that a value keeps, or nullptr once it is gone.
static void *ECSILua_HandleObject(int reference)
{
    const char *type = nullptr;
    void *object = nullptr;
    lua_rawgeti(LUA.state, LUA_REGISTRYINDEX, reference);
    bool found = LUA.ReadHandle != nullptr && LUA.ReadHandle(lua_gettop(LUA.state), &type, &object);
    lua_pop(LUA.state, 1);
    return found ? object : nullptr;
}

#pragma endregion Source Only

SHUResult ECSILua_Initialize(void)
{
    LUA.state = lua_newstate(ECSILua_Allocate, nullptr, luaL_makeseed(nullptr));

    if (LUA.state == nullptr)
    {
        return SHUResult_ErrAllocation;
    }

    lua_atpanic(LUA.state, ECSILua_Panic);
    lua_setwarnf(LUA.state, ECSILua_Warn, nullptr);

    // every library is open for plugins; data files see only a few, through their own environment
    luaL_openlibs(LUA.state);

    if (luaL_loadbufferx(LUA.state, OPENECS_LUA_WRITER, sizeof(OPENECS_LUA_WRITER) - 1, "=writer", "t") != LUA_OK || lua_pcall(LUA.state, 0, 1, 0) != LUA_OK)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "The data file writer does not load: %s", lua_tostring(LUA.state, -1));
        return SHUResult_ErrInternal;
    }

    LUA.writer = luaL_ref(LUA.state, LUA_REGISTRYINDEX);
    ECSIValue_SetLuaFunctions(ECSILua_KeepHandle, ECSILua_DropHandle, ECSILua_HandleObject);
    return SHUResult_Ok;
}

void ECSILua_Terminate(void)
{
    // a value that outlives the state lets go of no handle
    ECSIValue_SetLuaFunctions(nullptr, nullptr, nullptr);

    if (LUA.state != nullptr)
    {
        lua_close(LUA.state);
    }

    SDL_zero(LUA);
}

lua_State *ECSILua_GetState(void)
{
    SDL_assert(LUA.state != nullptr);

    return LUA.state;
}

void ECSILua_SetHandleFunctions(ECSILuaReadHandleFunction Read, ECSILuaPushHandleFunction Push)
{
    LUA.ReadHandle = Read;
    LUA.PushHandle = Push;
}

SHUResult ECSILua_GetValue(int index, ECSValue *value)
{
    SDL_assert(LUA.state != nullptr);
    SDL_assert(value != nullptr);

    return ECSILua_ToValue(LUA.state, index, value, true, 0);
}

static void ECSILua_Push(lua_State *state, const ECSValue *value);

static void ECSILua_PushField(const char *name, const ECSValue *field, void *userData)
{
    lua_State *state = userData;
    ECSILua_Push(state, field);
    lua_setfield(state, -2, name);
}

/// @brief Pushes a value onto a Lua stack. A handle becomes nil on a state other than the core's, which owns handles.
static void ECSILua_Push(lua_State *state, const ECSValue *value)
{
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
        case ECSValueType_Buffer:
        {
            SHUSlice bytes = ECSValue_GetBuffer(value, (SHUSlice){0});
            lua_pushlstring(state, bytes.data, bytes.size);
            return;
        }
        case ECSValueType_Handle:
            if (state != LUA.state)
            {
                lua_pushnil(state);
                return;
            }

            if (ECSIValue_GetLuaHandle(value) != 0)
            {
                lua_rawgeti(state, LUA_REGISTRYINDEX, ECSIValue_GetLuaHandle(value));
                return;
            }

            if (LUA.PushHandle != nullptr)
            {
                const char *type = ECSIValue_GetHandleType(value);
                LUA.PushHandle(type, ECSValue_GetHandle(value, type));
                return;
            }

            lua_pushnil(state);
            return;
        default:
            lua_pushnil(state);
            return;
        }
    }

    usz count = ECSValue_GetListCount(value);
    lua_createtable(state, (int)SDL_min(count, (usz)SDL_MAX_SINT32), 0);

    for (usz i = 0; i < count; i++)
    {
        ECSILua_Push(state, ECSValue_GetListItem(value, i));
        lua_rawseti(state, -2, (lua_Integer)i + 1);
    }

    ECSIValue_TableForEachField(value, ECSILua_PushField, state);
}

void ECSILua_PushValue(const ECSValue *value)
{
    SDL_assert(LUA.state != nullptr);

    ECSILua_Push(LUA.state, value);
}

lua_State *ECSILua_NewState(void)
{
    lua_State *state = lua_newstate(ECSILua_Allocate, nullptr, luaL_makeseed(nullptr));

    if (state != nullptr)
    {
        lua_atpanic(state, ECSILua_Panic);
        luaL_openlibs(state);
    }

    return state;
}

SHUResult ECSILua_GetData(lua_State *state, int index, ECSValue *value)
{
    SDL_assert(state != nullptr);
    SDL_assert(value != nullptr);

    return ECSILua_ToValue(state, index, value, false, 0);
}

void ECSILua_PushData(lua_State *state, const ECSValue *value)
{
    SDL_assert(state != nullptr);

    ECSILua_Push(state, value);
}

SHUResult ECSILua_Call(int argumentCount, int resultCount)
{
    SDL_assert(LUA.state != nullptr);

    return ECSILua_CallIn(LUA.state, argumentCount, resultCount);
}

SHUResult ECSILua_CallIn(lua_State *state, int argumentCount, int resultCount)
{
    SDL_assert(state != nullptr);

    int base = lua_gettop(state) - argumentCount;

    lua_pushcfunction(state, ECSILua_Traceback);
    lua_insert(state, base);
    int status = lua_pcall(state, argumentCount, resultCount, base);
    lua_remove(state, base);
    return status == LUA_OK ? SHUResult_Ok : SHUResult_Err;
}

SHUResult ECSILua_ReadData(const char *path, ECSValue *retValue)
{
    SDL_assert(LUA.state != nullptr);
    SDL_assert(path != nullptr);
    SDL_assert(retValue != nullptr);

    lua_State *state = LUA.state;
    int top = lua_gettop(state);

    if (luaL_loadfilex(state, path, "t") != LUA_OK)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot read '%s': %s", path, lua_tostring(state, -1));
        lua_settop(state, top);
        return SHUResult_ErrFile;
    }

    // the first upvalue of a loaded chunk is its environment
    ECSILua_PushDataEnvironment();
    lua_setupvalue(state, -2, 1);

    if (lua_pcall(state, 0, 1, 0) != LUA_OK)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Error in '%s': %s", path, lua_tostring(state, -1));
        lua_settop(state, top);
        return SHUResult_ErrBadData;
    }

    if (!lua_istable(state, -1))
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "'%s' does not return a table.", path);
        lua_settop(state, top);
        return SHUResult_ErrBadData;
    }

    SHUResult result = ECSILua_ToValue(state, -1, retValue, true, 0);
    lua_settop(state, top);
    return result;
}

SHUResult ECSILua_WriteData(const char *path, const ECSValue *value)
{
    SDL_assert(LUA.state != nullptr);
    SDL_assert(path != nullptr);

    lua_State *state = LUA.state;
    int top = lua_gettop(state);

    lua_pushcfunction(state, ECSILua_WriteProtected);
    lua_pushlightuserdata(state, (void *)value);

    if (lua_pcall(state, 1, 1, 0) != LUA_OK)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot write '%s': %s", path, lua_tostring(state, -1));
        lua_settop(state, top);
        return SHUResult_ErrAllocation;
    }

    usz length = 0;
    const char *text = lua_tolstring(state, -1, &length);

    // the folder of the file
    char *folder = SDL_strdup(path);
    char *temporary = nullptr;

    if (folder == nullptr || SDL_asprintf(&temporary, "%s.tmp", path) < 0)
    {
        SDL_free(folder);
        lua_settop(state, top);
        return SHUResult_ErrAllocation;
    }

    char *slash = ECSPath_FindLastSeparator(folder);

    if (slash != nullptr)
    {
        slash[1] = '\0';
    }

    bool written = (slash == nullptr || SDL_CreateDirectory(folder)) && SDL_SaveFile(temporary, text, length) && SDL_RenamePath(temporary, path);

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
