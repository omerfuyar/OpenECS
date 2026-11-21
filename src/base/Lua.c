#include "base/Lua.h"

#include "lua/compat-5.3.h"

#include "SDL3/SDL.h"
#include "stb/stbSDL3.h"

#pragma region Source Only

static struct
{
    lua_State *state;
    int writer;                           // registry reference of the writer function
    int isInt64;                          // registry reference of the function that tells LuaJIT's 64-bit integer cdata
    ECSILuaReadHandleFunction ReadHandle; // set by the Services module
    ECSILuaPushHandleFunction PushHandle;
    ECSILuaMisuseFunction Misuse;
    void *ffiCall; // the function that a Lua wrapper calls through the FFI, or nullptr
} LUA = {0};

/// @brief File-level state of each thread.
static thread_local struct
{
    lua_State *caller; // the Lua state whose code calls C on this thread, or nullptr
} LUA_THREAD = {0};

/// @brief The writer of data files. It takes a value as ECSILua_PushOrderedValue pushes it, and returns Lua source that returns the value.
static const char OPENECS_LUA_WRITER[] =
    "local format, concat = string.format, table.concat\n"
    "local keywords = {}\n"
    "for word in ('and break const continue do else elseif end false for function global goto if in local nil not or repeat return then true until while'):gmatch('%a+') do keywords[word] = true end\n"
    "local function number(n)\n"
    "  if n % 1 == 0 and n > -2^63 and n < 2^63 then return format('%d', n) end\n"
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

/// @brief A function that tells whether a value is a 64-bit integer cdata of LuaJIT's FFI, which only the FFI tells apart from other cdata.
static const char OPENECS_LUA_IS_INT64[] =
    "local ffi = require('ffi')\n"
    "local signed, unsigned = ffi.typeof('int64_t'), ffi.typeof('uint64_t')\n"
    "return function(value) return ffi.istype(signed, value) or ffi.istype(unsigned, value) end\n";

/// @brief Deepest nesting of tables that converts to a value; deeper tables are most likely cycles.
#define OPENECS_MAX_VALUE_DEPTH 64

/// @brief Names of the globals that data files may use: Lua's basic functions, and a few of its libraries.
static const char *const OPENECS_LUA_DATA_GLOBALS[] = {
    "assert", "error", "ipairs", "next", "pairs", "pcall", "rawequal", "rawget", "rawlen", "rawset", "select", "tonumber",
    "tostring", "type", "xpcall", "getmetatable", "setmetatable", "string", "table", "math", "utf8"};

/// @brief Pushes the environment in which data files run: basic functions and a few libraries, without access to the core.
static void ECSILua_PushDataEnvironment(void)
{
    lua_State *state = LUA.state;
    lua_newtable(state);

    for (usz i = 0; i < SDL_arraysize(OPENECS_LUA_DATA_GLOBALS); i++)
    {
        lua_getglobal(state, OPENECS_LUA_DATA_GLOBALS[i]);
        lua_setfield(state, -2, OPENECS_LUA_DATA_GLOBALS[i]);
    }
}

static int ECSILua_CompareTexts(const void *first, const void *second)
{
    return SDL_strcmp(*(const char *const *)first, *(const char *const *)second);
}

/// @brief Copies the Lua value at an index of the stack into a value. Named fields are added in the order of their names, so the same table always gives the same value.
/// @param handles Whether handles stay handles: in the core's state only, and not in a value that goes to another thread, where a handle is skipped.
static SHUResult ECSILua_ToValue(lua_State *state, int index, ECSValue *retValue, bool handles, u32 depth)
{
    index = lua_absindex(state, index);

    switch (lua_type(state, index))
    {
    // a missing argument is nil, so optional values may be left out
    case LUA_TNONE:
    case LUA_TNIL:
        ECSValue_SetNil(retValue);
        return SHUResult_Ok;
    case LUA_TBOOLEAN:
        ECSValue_SetBool(retValue, lua_toboolean(state, index));
        return SHUResult_Ok;
    case LUA_TNUMBER:
        if (lua_isinteger(state, index))
        {
            ECSValue_SetInteger(retValue, (i64)lua_tointeger(state, index));
        }
        else
        {
            ECSValue_SetNumber(retValue, (f64)lua_tonumber(state, index));
        }

        return SHUResult_Ok;
    case LUA_TSTRING:
    {
        // a string with a zero byte is bytes, which a C string cannot hold
        usz length = 0;
        const char *text = lua_tolstring(state, index, &length);
        return SDL_strlen(text) == length ? ECSValue_SetString(retValue, text) : ECSValue_SetBuffer(retValue, (SHUSliceView){.data = text, .size = length});
    }
    case LUA_TUSERDATA:
    case OPENECS_LUA_TCDATA:
    {
        const char *type = nullptr;
        void *object = nullptr;

        // the value keeps the handle itself, so the handle it gives back is gone once its object is, and the object lives while the value does
        if (handles && LUA.ReadHandle != nullptr && LUA.ReadHandle(index, &type, &object))
        {
            void *const *handle = lua_topointer(state, index);
            lua_pushvalue(state, index);
            return ECSIValue_SetLuaHandle(retValue, type, object, luaL_ref(state, LUA_REGISTRYINDEX), handle);
        }

        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, handles ? "A userdata or cdata that is not a handle, or whose object is gone, is not data; it is skipped." : "A handle, userdata or cdata is not data that can go to another thread; it is skipped.");
        ECSValue_SetNil(retValue);
        return SHUResult_Ok;
    }
    case LUA_TTABLE:
        break;
    default:
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "A %s is not data; it is skipped.", luaL_typename(state, index));
        ECSValue_SetNil(retValue);
        return SHUResult_Ok;
    }

    if (depth == OPENECS_MAX_VALUE_DEPTH)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Tables are nested more than %d deep, or hold themselves.", OPENECS_MAX_VALUE_DEPTH);
        return SHUResult_ErrBadData;
    }

    ECSValue_SetTable(retValue);
    usz count = (usz)lua_rawlen(state, index);

    for (usz i = 1; i <= count; i++)
    {
        ECSValue *item = nullptr;
        SHU_ReturnResult(ECSValue_AddListItem(retValue, &item));

        lua_rawgeti(state, index, (int)i);
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
        result = ECSValue_SetTableField(retValue, names[i], &field);

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

/// @brief Pushes a value that is neither a table nor a handle.
/// @return false for a table or a handle, which the caller pushes; nothing is pushed then.
static bool ECSILua_PushScalar(lua_State *state, const ECSValue *value)
{
    switch (ECSValue_GetType(value))
    {
    case ECSValueType_Nil:
        lua_pushnil(state);
        return true;
    case ECSValueType_Bool:
        lua_pushboolean(state, ECSValue_GetBool(value, false));
        return true;
    case ECSValueType_Integer:
        lua_pushinteger(state, (lua_Integer)ECSValue_GetInteger(value, 0));
        return true;
    case ECSValueType_Number:
        lua_pushnumber(state, (lua_Number)ECSValue_GetNumber(value, 0.0));
        return true;
    case ECSValueType_String:
        lua_pushstring(state, ECSValue_GetString(value, ""));
        return true;
    case ECSValueType_Buffer:
    {
        SHUSlice bytes = ECSValue_GetBuffer(value, (SHUSlice){0});
        lua_pushlstring(state, bytes.data, bytes.size);
        return true;
    }
    case ECSValueType_Handle:
    case ECSValueType_Table:
        return false;
    }

    return false;
}

/// @brief Pushes a value for the writer, keeping the order of named fields: a table becomes { items = { n = count, ... }, fields = { name, value, ... } }.
static void ECSILua_PushOrderedField(const char *name, const ECSValue *field, void *userData);

static void ECSILua_PushOrderedValue(const ECSValue *value)
{
    lua_State *state = LUA.state;
    luaL_checkstack(state, 4, "a value is nested too deeply");

    if (ECSILua_PushScalar(state, value))
    {
        return;
    }

    // a file holds only data, so a handle is left out
    if (ECSValue_GetType(value) == ECSValueType_Handle)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "A handle is not data that a file can hold; it is left out.");
        lua_pushnil(state);
        return;
    }

    usz count = ECSValue_GetListCount(value);
    lua_createtable(state, 0, 2);
    lua_createtable(state, (int)SDL_min(count, (usz)SDL_MAX_SINT32), 1);

    for (usz i = 0; i < count; i++)
    {
        ECSILua_PushOrderedValue(ECSValue_GetListItem(value, i));
        lua_rawseti(state, -2, (int)i + 1);
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
    int length = (int)lua_rawlen(state, -1);

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

// lua-compat-5.3's utf8 library, which lutf8lib.c names so beside LuaJIT's own libraries
int luaopen_compat53_utf8(lua_State *state);

/// @brief Opens every library for a state: LuaJIT's, with bit, ffi and jit, and utf8 from lua-compat-5.3. require finds LuaJIT's jit modules, such as jit.p,
/// in the program's folder, where the build puts them.
static void ECSILua_OpenLibraries(lua_State *state)
{
    luaL_openlibs(state);
    luaL_requiref(state, "utf8", luaopen_compat53_utf8, 1);
    lua_pop(state, 1);

    lua_getglobal(state, "package");
    lua_pushfstring(state, "%s?.lua;", SDL_GetBasePath());
    lua_getfield(state, -2, "path");
    lua_concat(state, 2);
    lua_setfield(state, -2, "path");
    lua_pop(state, 1);
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

    if (ECSILua_PushScalar(state, value))
    {
        return;
    }

    if (ECSValue_GetType(value) == ECSValueType_Handle)
    {
        const char *type = ECSIValue_GetHandleType(value);

        if (state != LUA.state)
        {
            lua_pushnil(state);
        }
        else if (ECSIValue_GetLuaHandle(value) != 0)
        {
            lua_rawgeti(state, LUA_REGISTRYINDEX, ECSIValue_GetLuaHandle(value));
        }
        else if (LUA.PushHandle != nullptr)
        {
            LUA.PushHandle(type, ECSValue_GetHandle(value, type));
        }
        else
        {
            lua_pushnil(state);
        }

        return;
    }

    usz count = ECSValue_GetListCount(value);
    lua_createtable(state, (int)SDL_min(count, (usz)SDL_MAX_SINT32), 0);

    for (usz i = 0; i < count; i++)
    {
        ECSILua_Push(state, ECSValue_GetListItem(value, i));
        lua_rawseti(state, -2, (int)i + 1);
    }

    ECSIValue_TableForEachField(value, ECSILua_PushField, state);
}

#pragma endregion Source Only

SHUResult ECSILua_Initialize(void)
{
    LUA.state = lua_newstate(ECSILua_Allocate, nullptr);

    if (LUA.state == nullptr)
    {
        return SHUResult_ErrAllocation;
    }

    lua_atpanic(LUA.state, ECSILua_Panic);

    // every library is open for plugins; data files see only a few, through their own environment
    ECSILua_OpenLibraries(LUA.state);

    if (luaL_loadbufferx(LUA.state, OPENECS_LUA_WRITER, sizeof(OPENECS_LUA_WRITER) - 1, "=writer", "t") != LUA_OK || lua_pcall(LUA.state, 0, 1, 0) != LUA_OK)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "The data file writer does not load: %s", lua_tostring(LUA.state, -1));
        return SHUResult_ErrInternal;
    }

    LUA.writer = luaL_ref(LUA.state, LUA_REGISTRYINDEX);

    if (luaL_loadbufferx(LUA.state, OPENECS_LUA_IS_INT64, sizeof(OPENECS_LUA_IS_INT64) - 1, "=int64", "t") != LUA_OK || lua_pcall(LUA.state, 0, 1, 0) != LUA_OK)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "The test of 64-bit integers does not load: %s", lua_tostring(LUA.state, -1));
        return SHUResult_ErrInternal;
    }

    LUA.isInt64 = luaL_ref(LUA.state, LUA_REGISTRYINDEX);
    ECSIValue_SetLuaFunctions(ECSILua_KeepHandle, ECSILua_DropHandle);
    return SHUResult_Ok;
}

void ECSILua_Terminate(void)
{
    // a value that outlives the state lets go of no handle
    ECSIValue_SetLuaFunctions(nullptr, nullptr);

    if (LUA.state != nullptr)
    {
        lua_close(LUA.state);
    }

    SDL_zero(LUA);
}

lua_State *ECSILua_GetState(void)
{
    SDL_assert(LUA.state != nullptr);
    SDL_assert(LUA.ffiCall == nullptr || !SDL_IsMainThread());

    return LUA.state;
}

void ECSILua_SetHandleFunctions(ECSILuaReadHandleFunction Read, ECSILuaPushHandleFunction Push)
{
    LUA.ReadHandle = Read;
    LUA.PushHandle = Push;
}

SHUResult ECSILua_GetValue(lua_State *state, int index, ECSValue *retValue)
{
    SDL_assert(state != nullptr);
    SDL_assert(retValue != nullptr);

    return ECSILua_ToValue(state, index, retValue, state == LUA.state, 0);
}

SHUResult ECSILua_GetMessage(lua_State *state, int index, ECSValue *retValue)
{
    SDL_assert(state != nullptr);
    SDL_assert(retValue != nullptr);

    return ECSILua_ToValue(state, index, retValue, false, 0);
}

void ECSILua_PushValue(lua_State *state, const ECSValue *value)
{
    SDL_assert(state != nullptr);

    ECSILua_Push(state, value);
}

void ECSILua_CheckFunctionField(lua_State *state, int index, const char *name)
{
    SDL_assert(state != nullptr);
    SDL_assert(name != nullptr);

    int type = lua_getfield(state, index, name);
    if (type != LUA_TFUNCTION && type != LUA_TNIL)
    {
        luaL_typerror(state, index, lua_pushfstring(state, "%s as a function or nil", name));
    }

    lua_pop(state, 1);
}

int ECSILua_ReferenceFunctionField(lua_State *state, int index, const char *name)
{
    SDL_assert(state != nullptr);
    SDL_assert(name != nullptr);

    if (lua_getfield(state, index, name) == LUA_TFUNCTION)
    {
        return luaL_ref(state, LUA_REGISTRYINDEX);
    }

    lua_pop(state, 1);
    return LUA_NOREF;
}

lua_State *ECSILua_NewState(void)
{
    lua_State *state = lua_newstate(ECSILua_Allocate, nullptr);

    if (state != nullptr)
    {
        lua_atpanic(state, ECSILua_Panic);
        ECSILua_OpenLibraries(state);
    }

    return state;
}

void ECSILua_SetMisuseFunction(ECSILuaMisuseFunction Misuse)
{
    LUA.Misuse = Misuse;
}

/// @brief Notes a misuse whose message is on the top of the stack, and raises it with the line at a level of the stack before it.
[[noreturn]] static void ECSILua_RaiseMisuse(lua_State *state, int level)
{
    if (LUA.Misuse != nullptr)
    {
        lua_State *outer = ECSILua_SetCaller(state);
        LUA.Misuse(lua_tostring(state, -1));
        (void)ECSILua_SetCaller(outer);
    }

    // as luaL_error, with the Lua caller's line before the message
    luaL_where(state, level);
    lua_insert(state, -2);
    lua_concat(state, 2);
    lua_error(state);
    unreachable();
}

void ECSILua_Misuse(lua_State *state, const char *format, ...)
{
    SDL_assert(state != nullptr);
    SDL_assert(format != nullptr);

    va_list arguments;
    va_start(arguments, format);
    (void)lua_pushvfstring(state, format, arguments);
    va_end(arguments);
    ECSILua_RaiseMisuse(state, 1);
}

void ECSILua_MisuseAt(lua_State *state, int level, const char *format, ...)
{
    SDL_assert(state != nullptr);
    SDL_assert(format != nullptr);

    va_list arguments;
    va_start(arguments, format);
    (void)lua_pushvfstring(state, format, arguments);
    va_end(arguments);
    ECSILua_RaiseMisuse(state, level);
}

void ECSILua_ArgError(lua_State *state, int argument, const char *message)
{
    ECSILua_ArgErrorAt(state, 0, argument, message);
}

void ECSILua_ArgErrorAt(lua_State *state, int level, int argument, const char *message)
{
    // as luaL_argerror: a method's self is not counted
    lua_Debug info;

    if (!lua_getstack(state, level, &info))
    {
        ECSILua_MisuseAt(state, level + 1, "bad argument #%d (%s)", argument, message);
    }

    lua_getinfo(state, "n", &info);

    if (info.namewhat != nullptr && SDL_strcmp(info.namewhat, "method") == 0 && --argument == 0)
    {
        ECSILua_MisuseAt(state, level + 1, "calling '%s' on bad self (%s)", info.name != nullptr ? info.name : "?", message);
    }

    ECSILua_MisuseAt(state, level + 1, "bad argument #%d to '%s' (%s)", argument, info.name != nullptr ? info.name : "?", message);
}

void ECSILua_TypeError(lua_State *state, int argument, const char *typeName)
{
    ECSILua_TypeErrorAt(state, 0, argument, argument, typeName);
}

void ECSILua_TypeErrorAt(lua_State *state, int level, int argument, int index, const char *typeName)
{
    const char *given = luaL_getmetafield(state, index, "__name") == LUA_TSTRING ? lua_tostring(state, -1)
                        : lua_type(state, index) == LUA_TLIGHTUSERDATA           ? "light userdata"
                                                                                 : luaL_typename(state, index);
    ECSILua_ArgErrorAt(state, level, argument, lua_pushfstring(state, "%s expected, got %s", typeName, given));
}

/// @brief The function that ECSILua_PushBytesOf pushes. A cdata gives its bytes if it is an array or a struct, which its type's name tells once for each
/// type; a pointer's or a number's bytes are not its data.
static const char OPENECS_LUA_BYTES_OF[] =
    "local ffi, buffer = require('ffi'), require('string.buffer')\n"
    "local typeof, sizeof, cast, find = ffi.typeof, ffi.sizeof, ffi.cast, string.find\n"
    "local bytes, bufferMetatable, whole = ffi.typeof('uint8_t *'), getmetatable(buffer.new()), {}\n"
    "return function(value)\n"
    "  local kind = type(value)\n"
    "  if kind == 'cdata' then\n"
    "    local ctype = typeof(value)\n"
    "    local id = tonumber(ctype)\n"
    "    local isWhole = whole[id]\n"
    "    if isWhole == nil then\n"
    "      local name = tostring(ctype)\n"
    "      isWhole = find(name, '%]>$') ~= nil or (find(name, '^ctype<struct ') ~= nil or find(name, '^ctype<union ') ~= nil) and find(name, '[*&]>$') == nil\n"
    "      whole[id] = isWhole\n"
    "    end\n"
    "    if isWhole then return cast(bytes, value), sizeof(value) end\n"
    "  elseif kind == 'userdata' and getmetatable(value) == bufferMetatable then\n"
    "    return value:ref()\n"
    "  end\n"
    "end\n";

/// @brief The registry's key of the function that ECSILua_PushBytesOf pushes, made once for each state.
static const char OPENECS_LUA_BYTES_OF_KEY = 0;

void ECSILua_PushBytesOf(lua_State *state)
{
    lua_rawgetp(state, LUA_REGISTRYINDEX, &OPENECS_LUA_BYTES_OF_KEY);

    if (lua_isnil(state, -1))
    {
        lua_pop(state, 1);
        int loaded = luaL_loadbufferx(state, OPENECS_LUA_BYTES_OF, sizeof(OPENECS_LUA_BYTES_OF) - 1, "=bytes", "t");
        SDL_assert(loaded == LUA_OK);
        lua_call(state, 0, 1);
        lua_pushvalue(state, -1);
        lua_rawsetp(state, LUA_REGISTRYINDEX, &OPENECS_LUA_BYTES_OF_KEY);
    }
}

const char *ECSILua_ToBytes(lua_State *state, int index, usz *retLength, bool *retText)
{
    size_t length = 0;
    bool text = lua_type(state, index) == LUA_TSTRING;

    if (retText != nullptr)
    {
        *retText = text;
    }

    if (text)
    {
        const char *string = lua_tolstring(state, index, &length);
        *retLength = length;
        return string;
    }

    index = lua_absindex(state, index);
    ECSILua_PushBytesOf(state);
    lua_pushvalue(state, index);
    lua_call(state, 1, 2);

    // the pointer's cdata holds the pointer; the bytes are the value's, which stays on the stack
    const char *bytes = lua_isnil(state, -2) ? nullptr : *(const char *const *)lua_topointer(state, -2);
    *retLength = bytes == nullptr ? 0 : (usz)lua_tointeger(state, -1);
    lua_pop(state, 2);
    return bytes;
}

const char *ECSILua_CheckBytes(lua_State *state, int argument, usz *retLength, bool *retText)
{
    const char *bytes = ECSILua_ToBytes(state, argument, retLength, retText);

    if (bytes == nullptr)
    {
        ECSILua_TypeError(state, argument, "string, string.buffer or cdata");
    }

    return bytes;
}

const char *ECSILua_CheckLString(lua_State *state, int argument, usz *retLength)
{
    size_t length = 0;
    const char *text = lua_tolstring(state, argument, &length);

    if (text == nullptr)
    {
        ECSILua_TypeError(state, argument, lua_typename(state, LUA_TSTRING));
    }

    if (retLength != nullptr)
    {
        *retLength = length;
    }

    return text;
}

const char *ECSILua_CheckString(lua_State *state, int argument)
{
    return ECSILua_CheckLString(state, argument, nullptr);
}

const char *ECSILua_OptString(lua_State *state, int argument, const char *fallback)
{
    return lua_isnoneornil(state, argument) ? fallback : ECSILua_CheckString(state, argument);
}

i64 ECSILua_CheckInteger(lua_State *state, int argument)
{
    int isInteger = 0;
    lua_Integer integer = lua_tointegerx(state, argument, &isInteger);

    if (!isInteger)
    {
        if (lua_isnumber(state, argument))
        {
            ECSILua_ArgError(state, argument, "number has no integer representation");
        }

        ECSILua_TypeError(state, argument, lua_typename(state, LUA_TNUMBER));
    }

    return (i64)integer;
}

bool ECSILua_ToInt64(lua_State *state, int index, i64 *retValue)
{
    SDL_assert(state != nullptr);
    SDL_assert(retValue != nullptr);

    if (lua_type(state, index) != OPENECS_LUA_TCDATA || state != LUA.state)
    {
        int isInteger = 0;
        *retValue = (i64)lua_tointegerx(state, index, &isInteger);
        return isInteger;
    }

    // a cdata's bytes are where lua_topointer points
    index = lua_absindex(state, index);
    lua_rawgeti(state, LUA_REGISTRYINDEX, LUA.isInt64);
    lua_pushvalue(state, index);
    lua_call(state, 1, 1);
    bool isInteger = lua_toboolean(state, -1);
    lua_pop(state, 1);
    *retValue = isInteger ? *(const i64 *)lua_topointer(state, index) : 0;
    return isInteger;
}

i64 ECSILua_OptInteger(lua_State *state, int argument, i64 fallback)
{
    return lua_isnoneornil(state, argument) ? fallback : ECSILua_CheckInteger(state, argument);
}

f64 ECSILua_CheckNumber(lua_State *state, int argument)
{
    int isNumber = 0;
    lua_Number number = lua_tonumberx(state, argument, &isNumber);

    if (!isNumber)
    {
        ECSILua_TypeError(state, argument, lua_typename(state, LUA_TNUMBER));
    }

    return (f64)number;
}

f64 ECSILua_OptNumber(lua_State *state, int argument, f64 fallback)
{
    return lua_isnoneornil(state, argument) ? fallback : ECSILua_CheckNumber(state, argument);
}

void ECSILua_CheckType(lua_State *state, int argument, int type)
{
    if (lua_type(state, argument) != type)
    {
        ECSILua_TypeError(state, argument, lua_typename(state, type));
    }
}

void ECSILua_CheckAny(lua_State *state, int argument)
{
    if (lua_type(state, argument) == LUA_TNONE)
    {
        ECSILua_ArgError(state, argument, "value expected");
    }
}

void *ECSILua_CheckUdata(lua_State *state, int argument, const char *typeName)
{
    void *data = luaL_testudata(state, argument, typeName);

    if (data == nullptr)
    {
        ECSILua_TypeError(state, argument, typeName);
    }

    return data;
}

int ECSILua_CheckOption(lua_State *state, int argument, const char *fallback, const char *const options[])
{
    const char *name = fallback != nullptr ? ECSILua_OptString(state, argument, fallback) : ECSILua_CheckString(state, argument);

    for (int i = 0; options[i] != nullptr; i++)
    {
        if (SDL_strcmp(options[i], name) == 0)
        {
            return i;
        }
    }

    ECSILua_ArgError(state, argument, lua_pushfstring(state, "invalid option '%s'", name));
}

void **ECSILua_GetFfiCall(void)
{
    return &LUA.ffiCall;
}

bool ECSILua_InFfiCall(void)
{
    return LUA.ffiCall != nullptr && SDL_IsMainThread();
}

lua_State *ECSILua_SetCaller(lua_State *state)
{
    lua_State *previous = LUA_THREAD.caller;
    LUA_THREAD.caller = state;
    return previous;
}

lua_State *ECSILua_GetCaller(void)
{
    return LUA_THREAD.caller;
}

SHUResult ECSILua_Call(lua_State *state, int argumentCount, int resultCount)
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

    ECSILua_PushDataEnvironment();
    lua_setfenv(state, -2);

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

    SHUResult result = ECSILua_ToValue(state, -1, retValue, false, 0);
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

    const char *slash = ECSPath_FindLastSeparator(path);

    if (slash != nullptr)
    {
        folder[slash - path + 1] = '\0';
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
