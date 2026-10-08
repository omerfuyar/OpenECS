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
    ECSI_ParameterType_Count,
} ECSI_ParameterType;

/// @brief Names of the parameter types in signatures.
static const char *const ECSI_PARAMETER_TYPE_NAMES[ECSI_ParameterType_Count] = {"void", "bool", "int", "int64", "float", "double", "string"};

/// @brief A parsed signature with its libffi call description.
typedef struct ECSI_Signature
{
    ECSI_ParameterType result;
    ECSI_ParameterType *parameters; // stb_ds array
    ffi_type **types;               // stb_ds array of the parameters' libffi types
    ffi_cif cif;
    char *text; // the signature written the same way every time, such as "int(string, float)"
} ECSI_Signature;

/// @brief A registered function.
typedef struct ECSI_Function
{
    char *name;
    char *description;
    ECSPlugin plugin;
    ECSI_Signature signature;
    ECSFunction pointer;
    int caller; // registry reference of the Lua function that calls it, made when Lua first asks for it
} ECSI_Function;

/// @brief Storage for one argument or result of a call through libffi.
typedef union ECSI_Slot
{
    ffi_arg integral; // results of integral types smaller than a register come back as one
    u8 boolean;
    i32 integer;
    i64 integer64;
    f32 single;
    f64 number;
    const char *string;
} ECSI_Slot;

static struct
{
    struct
    {
        char *key; // the function's own copy of its name
        ECSI_Function *value;
    } *functions; // stb_ds hash map; each function is allocated on its own, because Lua callers point to it
} SERVICES = {0};

static ffi_type *ECSI_ServicesFfiType(ECSI_ParameterType type)
{
    switch (type)
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

/// @brief Reads one type name of a signature and moves past it and the spaces after it.
static bool ECSI_SignatureReadType(const char **text, ECSI_ParameterType *retType)
{
    const char *start = *text;
    const char *end = start;

    while (SDL_isalnum((unsigned char)*end))
    {
        end++;
    }

    for (i32 type = 0; type < ECSI_ParameterType_Count; type++)
    {
        const char *name = ECSI_PARAMETER_TYPE_NAMES[type];

        if ((usz)(end - start) == SDL_strlen(name) && SDL_strncmp(start, name, (usz)(end - start)) == 0)
        {
            *retType = (ECSI_ParameterType)type;
            *text = end;

            while (SDL_isspace((unsigned char)**text))
            {
                (*text)++;
            }

            return true;
        }
    }

    return false;
}

/// @brief Parses a signature such as "int(string, float)", and prepares its libffi call description.
static SHUResult ECSI_SignatureParse(const char *text, ECSI_Signature *retSignature)
{
    SDL_zerop(retSignature);
    const char *cursor = text;

    while (SDL_isspace((unsigned char)*cursor))
    {
        cursor++;
    }

    bool valid = ECSI_SignatureReadType(&cursor, &retSignature->result) && *cursor++ == '(';

    while (SDL_isspace((unsigned char)*cursor))
    {
        cursor++;
    }

    while (valid && *cursor != ')')
    {
        ECSI_ParameterType type = ECSI_ParameterType_Void;
        valid = ECSI_SignatureReadType(&cursor, &type) && type != ECSI_ParameterType_Void && (*cursor == ',' || *cursor == ')');
        cursor += valid && *cursor == ',' ? 1 : 0;

        while (SDL_isspace((unsigned char)*cursor))
        {
            cursor++;
        }

        if (valid)
        {
            arrput(retSignature->parameters, type);
            arrput(retSignature->types, ECSI_ServicesFfiType(type));
        }
    }

    valid = valid && *cursor++ == ')';

    while (valid && SDL_isspace((unsigned char)*cursor))
    {
        cursor++;
    }

    if (!valid || *cursor != '\0')
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "'%s' is not a signature, such as \"int(string, float)\".", text);
        ECSI_SignatureFree(retSignature);
        return SHUResult_ErrBadData;
    }

    // the text is written again, so signatures compare the same however they are spaced
    usz count = arrlenu(retSignature->parameters);
    char *written = NULL;
    SDL_asprintf(&written, "%s(", ECSI_PARAMETER_TYPE_NAMES[retSignature->result]);

    for (usz i = 0; written != NULL && i < count; i++)
    {
        char *next = NULL;
        SDL_asprintf(&next, "%s%s%s", written, i == 0 ? "" : ", ", ECSI_PARAMETER_TYPE_NAMES[retSignature->parameters[i]]);
        SDL_free(written);
        written = next;
    }

    if (written == NULL || SDL_asprintf(&retSignature->text, "%s)", written) < 0)
    {
        retSignature->text = NULL;
        SDL_free(written);
        ECSI_SignatureFree(retSignature);
        return SHUResult_ErrAllocation;
    }

    SDL_free(written);

    if (ffi_prep_cif(&retSignature->cif, FFI_DEFAULT_ABI, (unsigned int)count, ECSI_ServicesFfiType(retSignature->result), retSignature->types) != FFI_OK)
    {
        ECSI_SignatureFree(retSignature);
        return SHUResult_ErrInternal;
    }

    return SHUResult_Ok;
}

static void ECSI_FunctionFree(ECSI_Function *function)
{
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

/// @brief Reads a Lua argument into a call slot. Raises a Lua error if it has the wrong type.
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
    default:
        break;
    }
}

/// @brief Pushes the result of a call through libffi.
static int ECSI_ServicesPushResult(lua_State *state, ECSI_ParameterType type, const ECSI_Slot *result)
{
    switch (type)
    {
    case ECSI_ParameterType_Void:
        return 0;
    case ECSI_ParameterType_Bool:
        lua_pushboolean(state, (u8)result->integral != 0);
        return 1;
    case ECSI_ParameterType_Int:
        lua_pushinteger(state, (lua_Integer)(i32)result->integral);
        return 1;
    case ECSI_ParameterType_Int64:
        lua_pushinteger(state, (lua_Integer)result->integer64);
        return 1;
    case ECSI_ParameterType_Float:
        lua_pushnumber(state, (lua_Number)result->single);
        return 1;
    case ECSI_ParameterType_Double:
        lua_pushnumber(state, (lua_Number)result->number);
        return 1;
    case ECSI_ParameterType_String:
        // Lua keeps a copy
        lua_pushstring(state, result->string);
        return 1;
    default:
        return 0;
    }
}

/// @brief Calls a C function from Lua through libffi. The function is the upvalue.
static int ECSI_ServicesCallC(lua_State *state)
{
    const ECSI_Function *function = lua_touserdata(state, lua_upvalueindex(1));
    const ECSI_Signature *signature = &function->signature;
    usz count = arrlenu(signature->parameters);

    // the storage is a userdata, so it is freed even when an argument raises an error
    ECSI_Slot *slots = lua_newuserdatauv(state, count * (sizeof(ECSI_Slot) + sizeof(void *)) + 1, 0);
    void **arguments = (void **)(slots + count);

    for (usz i = 0; i < count; i++)
    {
        ECSI_ServicesCheckArgument(state, (int)i + 1, signature->parameters[i], &slots[i]);
        arguments[i] = &slots[i];
    }

    ECSI_Slot result = {0};
    ffi_call((ffi_cif *)&signature->cif, FFI_FN(function->pointer), &result, arguments);
    return ECSI_ServicesPushResult(state, signature->result, &result);
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

SHUResult ECSI_ServicesPushFunction(ECSPlugin plugin, const char *name, const char *signature)
{
    SDL_assert(plugin != NULL);
    SDL_assert(name != NULL);

    ECSI_Function *function = NULL;
    SHU_ReturnResult(ECSI_ServicesFind(plugin, name, signature, &function));

    lua_State *state = ECSI_LuaGetState();

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

    if (!ECSI_PluginOwnsName(plugin, name))
    {
        return SHUResult_ErrBadData;
    }

    if (SERVICES.functions != NULL && shget(SERVICES.functions, name) != NULL)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Function '%s' is already registered.", name);
        return SHUResult_ErrBadData;
    }

    ECSI_Function *registered = SDL_calloc(1, sizeof(ECSI_Function));

    if (registered == NULL)
    {
        return SHUResult_ErrAllocation;
    }

    registered->name = SDL_strdup(name);
    registered->description = SDL_strdup(description == NULL ? "" : description);
    registered->plugin = plugin;
    registered->pointer = function;
    registered->caller = LUA_NOREF;

    SHUResult result = registered->name == NULL || registered->description == NULL ? SHUResult_ErrAllocation : ECSI_SignatureParse(signature, &registered->signature);

    if (result)
    {
        SDL_free(registered->name);
        SDL_free(registered->description);
        SDL_free(registered);
        return result;
    }

    shput(SERVICES.functions, registered->name, registered);
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
