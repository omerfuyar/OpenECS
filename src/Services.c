#include "Services.h"

#include "Lua.h"

#include "lua/lauxlib.h"
#include "lua/lua.h"

#include "libffi/ffi.h"

#include "SDL3/SDL.h"
#include "stb/stbSDL3.h"

#pragma region Source Only

/// @brief Type of a parameter or result in a signature.
typedef enum ECSI_ParameterType
{
    ECSI_ParameterType_Void = 0,
    ECSI_ParameterType_Bool,
    ECSI_ParameterType_Int,
    ECSI_ParameterType_Int64,
    ECSI_ParameterType_Float,
    ECSI_ParameterType_Double,
    ECSI_ParameterType_String,
    ECSI_ParameterType_Buffer,
    ECSI_ParameterType_Value,
    ECSI_ParameterType_Count,
} ECSI_ParameterType;

/// @brief Names of the parameter types in signatures.
static const char *const ECSI_PARAMETER_TYPE_NAMES[ECSI_ParameterType_Count] = {"void", "bool", "int", "int64", "float", "double", "string", "buffer", "value"};

/// @brief A parameter or result of a signature.
typedef struct ECSI_Parameter
{
    ECSI_ParameterType type;
    bool out; // an output parameter: a pointer in C, an extra result in Lua
} ECSI_Parameter;

/// @brief A parsed signature with its libffi call description.
typedef struct ECSI_Signature
{
    ECSI_Parameter result;
    ECSI_Parameter *parameters; // stb_ds array
    ffi_type **types;           // stb_ds array of the parameters' libffi types
    usz outCount;
    ffi_cif cif;
    char *text; // the signature written the same way every time, such as "int(string, out float)"
} ECSI_Signature;

/// @brief A registered function.
typedef struct ECSI_Function
{
    char *name;
    char *description;
    ECSPlugin plugin;
    ECSI_Signature signature;
    ECSFunction pointer;
    int caller;           // registry reference of the Lua function that calls it, made when Lua first asks for it
    int lua;              // registry reference of the Lua function, or LUA_NOREF for a C function
    int anchors;          // registry reference of a table that keeps the strings and buffers a Lua function last gave C
    ECSValue *result;     // the value a Lua function last returned to C, or NULL
    ffi_closure *closure; // the code that C calls for a Lua function, or NULL
} ECSI_Function;

/// @brief Storage for one argument, output or result of a call through libffi.
typedef union ECSI_Slot
{
    ffi_arg integral; // results of integral types smaller than a register come back as one
    u8 boolean;
    i32 integer;
    i64 integer64;
    f32 single;
    f64 number;
    const char *string;
    SHUSlice buffer;
    void *pointer; // values and outputs
} ECSI_Slot;

static_assert(sizeof(usz) == sizeof(u64), "a buffer's size is described to libffi as 64 bits");

/// @brief The libffi description of SHUSlice, which buffers are.
static ffi_type *ECSI_SLICE_ELEMENTS[] = {&ffi_type_pointer, &ffi_type_uint64, NULL};
static ffi_type ECSI_SLICE_TYPE = {0, 0, FFI_TYPE_STRUCT, ECSI_SLICE_ELEMENTS};

static struct
{
    struct
    {
        char *key; // the function's own copy of its name
        ECSI_Function *value;
    } *functions; // stb_ds hash map; each function is allocated on its own, because Lua callers point to it
} SERVICES = {0};

static ffi_type *ECSI_ServicesFfiType(ECSI_Parameter parameter)
{
    if (parameter.out)
    {
        return &ffi_type_pointer;
    }

    switch (parameter.type)
    {
    case ECSI_ParameterType_Void:
        return &ffi_type_void;
    case ECSI_ParameterType_Bool:
        return &ffi_type_uint8;
    case ECSI_ParameterType_Int:
        return &ffi_type_sint32;
    case ECSI_ParameterType_Int64:
        return &ffi_type_sint64;
    case ECSI_ParameterType_Float:
        return &ffi_type_float;
    case ECSI_ParameterType_Double:
        return &ffi_type_double;
    case ECSI_ParameterType_Buffer:
        return &ECSI_SLICE_TYPE;
    default:
        return &ffi_type_pointer;
    }
}

static void ECSI_SignatureFree(ECSI_Signature *signature)
{
    arrfree(signature->parameters);
    arrfree(signature->types);
    SDL_free(signature->text);
    SDL_zerop(signature);
}

static void ECSI_SignatureSkipSpaces(const char **text)
{
    while (SDL_isspace((unsigned char)**text))
    {
        (*text)++;
    }
}

/// @brief Reads one word of a signature and moves past it and the spaces after it.
/// @return The word's length; 0 if there is none.
static usz ECSI_SignatureReadWord(const char **text, const char **retWord)
{
    *retWord = *text;

    while (SDL_isalnum((unsigned char)**text))
    {
        (*text)++;
    }

    usz length = (usz)(*text - *retWord);
    ECSI_SignatureSkipSpaces(text);
    return length;
}

/// @brief Reads a parameter or result of a signature: a type name, after "out" for an output parameter.
static bool ECSI_SignatureReadParameter(const char **text, ECSI_Parameter *retParameter)
{
    const char *word = NULL;
    usz length = ECSI_SignatureReadWord(text, &word);
    retParameter->out = length == 3 && SDL_strncmp(word, "out", 3) == 0;

    if (retParameter->out)
    {
        length = ECSI_SignatureReadWord(text, &word);
    }

    for (i32 type = 0; type < ECSI_ParameterType_Count; type++)
    {
        const char *name = ECSI_PARAMETER_TYPE_NAMES[type];

        if (length == SDL_strlen(name) && SDL_strncmp(word, name, length) == 0)
        {
            retParameter->type = (ECSI_ParameterType)type;
            return true;
        }
    }

    return false;
}

/// @brief Writes a parameter as the signature text has it.
static SHUResult ECSI_SignatureAppend(char **text, const char *separator, ECSI_Parameter parameter)
{
    char *next = NULL;

    if (SDL_asprintf(&next, "%s%s%s%s", *text == NULL ? "" : *text, separator, parameter.out ? "out " : "", ECSI_PARAMETER_TYPE_NAMES[parameter.type]) < 0)
    {
        return SHUResult_ErrAllocation;
    }

    SDL_free(*text);
    *text = next;
    return SHUResult_Ok;
}

/// @brief Parses a signature such as "int(string, out float)", and prepares its libffi call description.
static SHUResult ECSI_SignatureParse(const char *text, ECSI_Signature *retSignature)
{
    SDL_zerop(retSignature);
    const char *cursor = text;
    ECSI_SignatureSkipSpaces(&cursor);

    bool valid = ECSI_SignatureReadParameter(&cursor, &retSignature->result) && !retSignature->result.out && *cursor++ == '(';
    ECSI_SignatureSkipSpaces(&cursor);

    while (valid && *cursor != ')')
    {
        ECSI_Parameter parameter = {0};
        valid = ECSI_SignatureReadParameter(&cursor, &parameter) && parameter.type != ECSI_ParameterType_Void && (*cursor == ',' || *cursor == ')');
        cursor += valid && *cursor == ',' ? 1 : 0;
        ECSI_SignatureSkipSpaces(&cursor);

        if (valid)
        {
            arrput(retSignature->parameters, parameter);
            arrput(retSignature->types, ECSI_ServicesFfiType(parameter));
            retSignature->outCount += parameter.out ? 1 : 0;
        }
    }

    valid = valid && *cursor++ == ')';
    ECSI_SignatureSkipSpaces(&cursor);

    if (!valid || *cursor != '\0')
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "'%s' is not a signature, such as \"int(string, out float)\".", text);
        ECSI_SignatureFree(retSignature);
        return SHUResult_ErrBadData;
    }

    // the text is written again, so signatures compare the same however they are spaced
    usz count = arrlenu(retSignature->parameters);
    SHUResult result = ECSI_SignatureAppend(&retSignature->text, "", retSignature->result);

    for (usz i = 0; !result && i <= count; i++)
    {
        result = i == count ? ECSI_SignatureAppend(&retSignature->text, ")", (ECSI_Parameter){0}) : ECSI_SignatureAppend(&retSignature->text, i == 0 ? "(" : ", ", retSignature->parameters[i]);
    }

    // the closing parenthesis was written before a "void"; that word is cut off again
    if (!result && count == 0)
    {
        SDL_free(retSignature->text);
        retSignature->text = NULL;
        result = SDL_asprintf(&retSignature->text, "%s()", ECSI_PARAMETER_TYPE_NAMES[retSignature->result.type]) < 0 ? SHUResult_ErrAllocation : SHUResult_Ok;
    }
    else if (!result)
    {
        retSignature->text[SDL_strlen(retSignature->text) - SDL_strlen("void")] = '\0';
    }

    SHU_ReturnResult(result, ECSI_SignatureFree(retSignature););

    if (ffi_prep_cif(&retSignature->cif, FFI_DEFAULT_ABI, (unsigned int)count, ECSI_ServicesFfiType(retSignature->result), retSignature->types) != FFI_OK)
    {
        ECSI_SignatureFree(retSignature);
        return SHUResult_ErrInternal;
    }

    return SHUResult_Ok;
}

static void ECSI_FunctionFree(ECSI_Function *function)
{
    if (function->closure != NULL)
    {
        ffi_closure_free(function->closure);
    }

    ECSValue_Destroy(&function->result);
    ECSI_SignatureFree(&function->signature);
    SDL_free(function->name);
    SDL_free(function->description);
    SDL_free(function);
}

/// @brief Finds a function that a plugin may use, and checks the signature the plugin expects.
static SHUResult ECSI_ServicesFind(ECSPlugin plugin, const char *name, const char *signature, ECSI_Function **retFunction)
{
    // shget would allocate a map that is missing
    ECSI_Function *function = SERVICES.functions == NULL ? NULL : shget(SERVICES.functions, name);

    if (function == NULL)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' looks up '%s', which is not registered.", ECSI_PluginGetName(plugin), name);
        return SHUResult_ErrNotFound;
    }

    if (!ECSI_PluginDependsOn(plugin, function->plugin))
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' looks up '%s', but its manifest does not depend on '%s'.", ECSI_PluginGetName(plugin), name, ECSI_PluginGetName(function->plugin));
        return SHUResult_ErrPrivileges;
    }

    if (signature != NULL)
    {
        ECSI_Signature expected;
        SHU_ReturnResult(ECSI_SignatureParse(signature, &expected));
        bool matches = SDL_strcmp(expected.text, function->signature.text) == 0;
        ECSI_SignatureFree(&expected);

        if (!matches)
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' expects '%s' to be %s, but it is %s.", ECSI_PluginGetName(plugin), name, signature, function->signature.text);
            return SHUResult_ErrBadData;
        }
    }

    *retFunction = function;
    return SHUResult_Ok;
}

#pragma region Lua Calls C

/// @brief Reads a Lua argument into a call slot. Raises a Lua error if it has the wrong type; values are read later, because they allocate.
static void ECSI_ServicesCheckArgument(lua_State *state, int index, ECSI_ParameterType type, ECSI_Slot *slot)
{
    switch (type)
    {
    case ECSI_ParameterType_Bool:
        slot->boolean = (u8)(lua_toboolean(state, index) != 0);
        break;
    case ECSI_ParameterType_Int:
    {
        lua_Integer integer = luaL_checkinteger(state, index);
        luaL_argcheck(state, integer >= SDL_MIN_SINT32 && integer <= SDL_MAX_SINT32, index, "does not fit in an int");
        slot->integer = (i32)integer;
        break;
    }
    case ECSI_ParameterType_Int64:
        slot->integer64 = (i64)luaL_checkinteger(state, index);
        break;
    case ECSI_ParameterType_Float:
        slot->single = (f32)luaL_checknumber(state, index);
        break;
    case ECSI_ParameterType_Double:
        slot->number = (f64)luaL_checknumber(state, index);
        break;
    case ECSI_ParameterType_String:
        // valid during the call, because the argument stays on the stack
        slot->string = luaL_checkstring(state, index);
        break;
    case ECSI_ParameterType_Buffer:
    {
        // a copy, because the function may write into a buffer and Lua strings must not change
        usz length = 0;
        const char *bytes = luaL_checklstring(state, index, &length);
        void *copy = lua_newuserdatauv(state, length + 1, 0);
        SDL_memcpy(copy, bytes, length);
        slot->buffer = cs(copy, length);
        break;
    }
    default:
        break;
    }
}

/// @brief Pushes a result or output of a C function for Lua. Lua keeps copies of strings, buffers and values.
static void ECSI_ServicesPushOutput(lua_State *state, ECSI_ParameterType type, const ECSI_Slot *slot, bool result)
{
    switch (type)
    {
    case ECSI_ParameterType_Bool:
        lua_pushboolean(state, (result ? (u8)slot->integral : slot->boolean) != 0);
        break;
    case ECSI_ParameterType_Int:
        lua_pushinteger(state, (lua_Integer)(result ? (i32)slot->integral : slot->integer));
        break;
    case ECSI_ParameterType_Int64:
        lua_pushinteger(state, (lua_Integer)slot->integer64);
        break;
    case ECSI_ParameterType_Float:
        lua_pushnumber(state, (lua_Number)slot->single);
        break;
    case ECSI_ParameterType_Double:
        lua_pushnumber(state, (lua_Number)slot->number);
        break;
    case ECSI_ParameterType_String:
        lua_pushstring(state, slot->string);
        break;
    case ECSI_ParameterType_Buffer:
        lua_pushlstring(state, slot->buffer.data == NULL ? "" : slot->buffer.data, slot->buffer.data == NULL ? 0 : slot->buffer.size);
        break;
    case ECSI_ParameterType_Value:
        ECSI_LuaPushValue(slot->pointer);
        break;
    default:
        lua_pushnil(state);
        break;
    }
}

/// @brief Calls a C function from Lua through libffi. The function is the upvalue. Output parameters become extra results.
static int ECSI_ServicesCallC(lua_State *state)
{
    const ECSI_Function *function = lua_touserdata(state, lua_upvalueindex(1));
    const ECSI_Signature *signature = &function->signature;
    usz count = arrlenu(signature->parameters);

    // the storage is a userdata, so it is freed even when an argument raises an error
    ECSI_Slot *slots = lua_newuserdatauv(state, count * (2 * sizeof(ECSI_Slot) + sizeof(void *) + sizeof(int)) + 1, 0);
    ECSI_Slot *outputs = slots + count;
    void **arguments = (void **)(outputs + count);
    int *indices = (int *)(arguments + count); // where value arguments are on the stack
    int index = 1;

    for (usz i = 0; i < count; i++)
    {
        ECSI_Parameter parameter = signature->parameters[i];
        arguments[i] = &slots[i];
        outputs[i] = (ECSI_Slot){0};

        if (parameter.out)
        {
            slots[i].pointer = &outputs[i];
        }
        else
        {
            indices[i] = index;
            ECSI_ServicesCheckArgument(state, index++, parameter.type, &slots[i]);
        }
    }

    // values allocate, so they are made after every check that can raise an error
    SHUResult result = SHUResult_Ok;

    for (usz i = 0; i < count; i++)
    {
        ECSI_Parameter parameter = signature->parameters[i];

        if (parameter.type != ECSI_ParameterType_Value)
        {
            continue;
        }

        ECSValue *value = NULL;
        result = result ? result : ECSValue_Create(&value);
        result = result || parameter.out ? result : ECSI_LuaGetValue(indices[i], value);
        slots[i].pointer = value;

        if (parameter.out)
        {
            outputs[i].pointer = value;
        }
    }

    ECSI_Slot returned = {0};

    if (!result)
    {
        ffi_call((ffi_cif *)&signature->cif, FFI_FN(function->pointer), &returned, arguments);
    }

    int pushed = 0;

    if (!result && signature->result.type != ECSI_ParameterType_Void)
    {
        ECSI_ServicesPushOutput(state, signature->result.type, &returned, true);
        pushed++;
    }

    for (usz i = 0; !result && i < count; i++)
    {
        if (signature->parameters[i].out)
        {
            ECSI_ServicesPushOutput(state, signature->parameters[i].type, &outputs[i], false);
            pushed++;
        }
    }

    for (usz i = 0; i < count; i++)
    {
        if (signature->parameters[i].type == ECSI_ParameterType_Value)
        {
            ECSValue_Destroy((ECSValue **)&slots[i].pointer);
        }
    }

    if (result)
    {
        return luaL_error(state, "cannot convert a value for '%s' (%s)", function->name, SHUResult_String(result));
    }

    return pushed;
}

#pragma endregion Lua Calls C

#pragma region C Calls Lua

/// @brief Pushes an argument that C passed to a Lua function.
static void ECSI_ServicesPushArgument(lua_State *state, ECSI_ParameterType type, const void *argument)
{
    switch (type)
    {
    case ECSI_ParameterType_Bool:
        lua_pushboolean(state, *(const u8 *)argument != 0);
        break;
    case ECSI_ParameterType_Int:
        lua_pushinteger(state, (lua_Integer) * (const i32 *)argument);
        break;
    case ECSI_ParameterType_Int64:
        lua_pushinteger(state, (lua_Integer) * (const i64 *)argument);
        break;
    case ECSI_ParameterType_Float:
        lua_pushnumber(state, (lua_Number) * (const f32 *)argument);
        break;
    case ECSI_ParameterType_Double:
        lua_pushnumber(state, (lua_Number) * (const f64 *)argument);
        break;
    case ECSI_ParameterType_String:
        lua_pushstring(state, *(const char *const *)argument);
        break;
    case ECSI_ParameterType_Buffer:
    {
        const SHUSlice *buffer = argument;
        lua_pushlstring(state, buffer->data == NULL ? "" : buffer->data, buffer->data == NULL ? 0 : buffer->size);
        break;
    }
    case ECSI_ParameterType_Value:
        ECSI_LuaPushValue(*(const ECSValue *const *)argument);
        break;
    default:
        lua_pushnil(state);
        break;
    }
}

/// @brief Writes a value that a Lua function gave, at an index of the stack, where C reads it: a result or an output.
/// @param anchors Index of the table that keeps strings and buffers alive until the function returns again.
/// @return false if the Lua value has the wrong type.
static bool ECSI_ServicesWriteOutput(lua_State *state, ECSI_Function *function, int index, int anchors, ECSI_ParameterType type, void *target, bool result)
{
    int isNumber = 0;

    switch (type)
    {
    case ECSI_ParameterType_Bool:
        if (result)
        {
            *(ffi_arg *)target = (ffi_arg)lua_toboolean(state, index);
        }
        else
        {
            *(u8 *)target = (u8)(lua_toboolean(state, index) != 0);
        }

        return true;
    case ECSI_ParameterType_Int:
    {
        lua_Integer integer = lua_tointegerx(state, index, &isNumber);

        if (result)
        {
            *(ffi_sarg *)target = (ffi_sarg)(i32)integer;
        }
        else
        {
            *(i32 *)target = (i32)integer;
        }

        return isNumber && integer >= SDL_MIN_SINT32 && integer <= SDL_MAX_SINT32;
    }
    case ECSI_ParameterType_Int64:
        *(i64 *)target = (i64)lua_tointegerx(state, index, &isNumber);
        return isNumber;
    case ECSI_ParameterType_Float:
        *(f32 *)target = (f32)lua_tonumberx(state, index, &isNumber);
        return isNumber;
    case ECSI_ParameterType_Double:
        *(f64 *)target = (f64)lua_tonumberx(state, index, &isNumber);
        return isNumber;
    case ECSI_ParameterType_String:
    case ECSI_ParameterType_Buffer:
    {
        bool text = lua_type(state, index) == LUA_TSTRING;
        usz length = 0;
        const char *bytes = text ? lua_tolstring(state, index, &length) : NULL;

        // the anchor keeps the string alive; a buffer gets a copy, because C may write into it
        if (type == ECSI_ParameterType_Buffer && text)
        {
            void *copy = lua_newuserdatauv(state, length + 1, 0);
            SDL_memcpy(copy, bytes, length);
            bytes = copy;
        }
        else
        {
            lua_pushvalue(state, index);
        }

        lua_rawseti(state, anchors, (lua_Integer)lua_rawlen(state, anchors) + 1);

        if (type == ECSI_ParameterType_String)
        {
            *(const char **)target = bytes;
        }
        else
        {
            *(SHUSlice *)target = cs((void *)bytes, length);
        }

        return text;
    }
    case ECSI_ParameterType_Value:
    {
        // a result value is kept until the function returns again; an output fills the caller's value
        ECSValue *value = result ? function->result : target;

        if (result)
        {
            *(const ECSValue **)target = value;
        }

        return value != NULL && ECSI_LuaGetValue(index, value) == SHUResult_Ok;
    }
    default:
        return true;
    }
}

/// @brief Reports that a Lua function gave a value of the wrong type.
static void ECSI_ServicesReportType(lua_State *state, const ECSI_Function *function, int index)
{
    char *message = NULL;

    if (SDL_asprintf(&message, "'%s' gave a %s where its signature %s needs another type.", function->name, luaL_typename(state, index), function->signature.text) >= 0)
    {
        ECSI_PluginReportError(function->plugin, message);
        SDL_free(message);
    }
}

/// @brief The handler of a Lua function's closure: C calls it through the closure's code. Output parameters are read from the extra results.
static void ECSI_ServicesCallLua(ffi_cif *cif, void *result, void **arguments, void *data)
{
    (void)cif;
    ECSI_Function *function = data;
    const ECSI_Signature *signature = &function->signature;
    lua_State *state = ECSI_LuaGetState();
    int top = lua_gettop(state);
    usz count = arrlenu(signature->parameters);
    int resultCount = (signature->result.type != ECSI_ParameterType_Void ? 1 : 0) + (int)signature->outCount;

    // a failed call gives C a zero result
    SDL_memset(result, 0, SDL_max(sizeof(ffi_arg), ECSI_ServicesFfiType(signature->result)->size));

    if (signature->result.type == ECSI_ParameterType_Value)
    {
        ECSValue_Destroy(&function->result);

        if (ECSValue_Create(&function->result))
        {
            return;
        }

        *(const ECSValue **)result = function->result;
    }

    // a new anchor table replaces the last call's, so what C got from the last call stays valid until now
    lua_newtable(state);
    lua_pushvalue(state, -1);
    lua_rawseti(state, LUA_REGISTRYINDEX, function->anchors);
    int anchors = lua_gettop(state);

    lua_rawgeti(state, LUA_REGISTRYINDEX, function->lua);
    int pushed = 0;

    for (usz i = 0; i < count; i++)
    {
        if (!signature->parameters[i].out)
        {
            ECSI_ServicesPushArgument(state, signature->parameters[i].type, arguments[i]);
            pushed++;
        }
    }

    if (ECSI_LuaCall(pushed, resultCount))
    {
        ECSI_PluginReportError(function->plugin, lua_tostring(state, -1));
        lua_settop(state, top);
        return;
    }

    int index = anchors + 1;

    if (signature->result.type != ECSI_ParameterType_Void && !ECSI_ServicesWriteOutput(state, function, index++, anchors, signature->result.type, result, true))
    {
        ECSI_ServicesReportType(state, function, index - 1);
    }

    for (usz i = 0; i < count; i++)
    {
        if (signature->parameters[i].out && !ECSI_ServicesWriteOutput(state, function, index++, anchors, signature->parameters[i].type, *(void **)arguments[i], false))
        {
            ECSI_ServicesReportType(state, function, index - 1);
        }
    }

    lua_settop(state, top);
}

#pragma endregion C Calls Lua

/// @brief Checks a function's name and makes its record, without its code.
static SHUResult ECSI_ServicesCreate(ECSPlugin plugin, const char *name, const char *signature, const char *description, ECSI_Function **retFunction)
{
    if (!ECSI_PluginOwnsName(plugin, name))
    {
        return SHUResult_ErrBadData;
    }

    if (SERVICES.functions != NULL && shget(SERVICES.functions, name) != NULL)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Function '%s' is already registered.", name);
        return SHUResult_ErrBadData;
    }

    ECSI_Function *function = SDL_calloc(1, sizeof(ECSI_Function));

    if (function == NULL)
    {
        return SHUResult_ErrAllocation;
    }

    function->name = SDL_strdup(name);
    function->description = SDL_strdup(description == NULL ? "" : description);
    function->plugin = plugin;
    function->caller = LUA_NOREF;
    function->lua = LUA_NOREF;
    function->anchors = LUA_NOREF;

    SHUResult result = function->name == NULL || function->description == NULL ? SHUResult_ErrAllocation : ECSI_SignatureParse(signature, &function->signature);

    if (result)
    {
        SDL_free(function->name);
        SDL_free(function->description);
        SDL_free(function);
        return result;
    }

    *retFunction = function;
    return SHUResult_Ok;
}

#pragma endregion Source Only

void ECSI_ServicesTerminate(void)
{
    for (usz i = 0; i < shlenu(SERVICES.functions); i++)
    {
        ECSI_FunctionFree(SERVICES.functions[i].value);
    }

    shfree(SERVICES.functions);
    SDL_zero(SERVICES);
}

void ECSI_ServicesRemovePlugin(ECSPlugin plugin)
{
    SDL_assert(plugin != NULL);

    // backwards, because shdel moves the last function into the hole
    for (usz i = shlenu(SERVICES.functions); i > 0; i--)
    {
        ECSI_Function *function = SERVICES.functions[i - 1].value;

        if (function->plugin == plugin)
        {
            (void)shdel(SERVICES.functions, function->name);
            ECSI_FunctionFree(function);
        }
    }
}

SHUResult ECSI_ServicesPushFunction(ECSPlugin plugin, const char *name, const char *signature)
{
    SDL_assert(plugin != NULL);
    SDL_assert(name != NULL);

    ECSI_Function *function = NULL;
    SHU_ReturnResult(ECSI_ServicesFind(plugin, name, signature, &function));

    lua_State *state = ECSI_LuaGetState();

    // a Lua function is called as it is
    if (function->lua != LUA_NOREF)
    {
        lua_rawgeti(state, LUA_REGISTRYINDEX, function->lua);
        return SHUResult_Ok;
    }

    // one Lua function per C function, made when Lua first asks for it
    if (function->caller == LUA_NOREF)
    {
        lua_pushlightuserdata(state, function);
        lua_pushcclosure(state, ECSI_ServicesCallC, 1);
        function->caller = luaL_ref(state, LUA_REGISTRYINDEX);
    }

    lua_rawgeti(state, LUA_REGISTRYINDEX, function->caller);
    return SHUResult_Ok;
}

SHUResult ECSService_RegisterFunction(ECSPlugin plugin, const char *name, ECSFunction function, const char *signature, const char *description)
{
    SDL_assert(plugin != NULL);
    SDL_assert(name != NULL);
    SDL_assert(function != NULL);
    SDL_assert(signature != NULL);

    ECSI_Function *registered = NULL;
    SHU_ReturnResult(ECSI_ServicesCreate(plugin, name, signature, description, &registered));
    registered->pointer = function;
    shput(SERVICES.functions, registered->name, registered);
    return SHUResult_Ok;
}

SHUResult ECSI_ServicesRegisterLua(ECSPlugin plugin, const char *name, const char *signature, const char *description)
{
    SDL_assert(plugin != NULL);
    SDL_assert(name != NULL);
    SDL_assert(signature != NULL);

    ECSI_Function *function = NULL;
    SHU_ReturnResult(ECSI_ServicesCreate(plugin, name, signature, description, &function));

    void *code = NULL;
    function->closure = ffi_closure_alloc(sizeof(ffi_closure), &code);

    if (function->closure == NULL || ffi_prep_closure_loc(function->closure, &function->signature.cif, ECSI_ServicesCallLua, function, code) != FFI_OK)
    {
        ECSI_FunctionFree(function);
        return SHUResult_ErrAllocation;
    }

    // C calls the closure's code, which calls the Lua function; POSIX lets a data pointer hold code, but ISO C has no cast for it
    SDL_memcpy(&function->pointer, &code, sizeof(code));
    lua_State *state = ECSI_LuaGetState();
    function->lua = luaL_ref(state, LUA_REGISTRYINDEX);
    lua_newtable(state);
    function->anchors = luaL_ref(state, LUA_REGISTRYINDEX);
    shput(SERVICES.functions, function->name, function);
    return SHUResult_Ok;
}

SHUResult ECSService_GetFunction(ECSPlugin plugin, ECSFunction *retFunction, const char *name, const char *signature)
{
    SDL_assert(plugin != NULL);
    SDL_assert(retFunction != NULL);
    SDL_assert(name != NULL);
    SDL_assert(signature != NULL);

    ECSI_Function *function = NULL;
    SHU_ReturnResult(ECSI_ServicesFind(plugin, name, signature, &function));
    *retFunction = function->pointer;
    return SHUResult_Ok;
}
