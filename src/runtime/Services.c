#include "runtime/Services.h"

#include "base/Lua.h"

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
    ECSI_ParameterType_Handle,
    ECSI_ParameterType_Fn,
    ECSI_ParameterType_Count,
} ECSI_ParameterType;

/// @brief Names of the parameter types in signatures.
static const char *const ECSI_PARAMETER_TYPE_NAMES[ECSI_ParameterType_Count] = {"void", "bool", "int", "int64", "float", "double", "string", "buffer", "value", "handle", "fn"};

/// @brief Prefix of the names of handle types' metatables.
#define OPENECS_HANDLE_METATABLE "ecs.handle:"

/// @brief A bound function that takes the focused panel: void(handle<ecs.panel>).
typedef void (*ECSI_PanelFunction)(ECSPanel panel);

/// @brief A registered type of handles.
typedef struct ECSI_HandleType
{
    char *name;
    char *metatable;  // name of its metatable in the registry
    ECSPlugin plugin; // NULL for the core
    ECSHandleDestroyFunction Destroy;
} ECSI_HandleType;

/// @brief A Lua handle: the userdata that stands for an object.
typedef struct ECSI_Handle
{
    void *object; // NULL when the object is gone
    ECSI_HandleType *type;
} ECSI_Handle;

/// @brief The object of a handle whose type a Lua plugin provides: the Lua value it stands for.
typedef struct ECSI_LuaObject
{
    int value; // registry reference
} ECSI_LuaObject;

/// @brief A parameter or result of a signature.
typedef struct ECSI_Parameter
{
    ECSI_ParameterType type;
    bool out;                          // an output parameter: a pointer in C, an extra result in Lua
    ECSI_HandleType *handle;           // handles: their type
    struct ECSI_Signature *callback;   // callbacks: their signature, owned by the parameter
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
    struct
    {
        char *key; // the type's own copy of its name
        ECSI_HandleType *value;
    } *handleTypes; // stb_ds hash map; handles point to their type
    int handles;    // registry reference of the weak table that maps each object to its Lua handle
} SERVICES = {0};

static ECSI_HandleType *ECSI_ServicesFindHandleType(const char *name)
{
    // shget would allocate a map that is missing
    return SERVICES.handleTypes == NULL ? NULL : shget(SERVICES.handleTypes, name);
}

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

static SHUResult ECSI_SignatureParse(const char *text, ECSI_Signature *retSignature);

static void ECSI_SignatureFree(ECSI_Signature *signature);

/// @brief Frees what a parameter owns: a callback's signature.
static void ECSI_ParameterFree(ECSI_Parameter *parameter)
{
    if (parameter->callback != NULL)
    {
        ECSI_SignatureFree(parameter->callback);
        SDL_free(parameter->callback);
        parameter->callback = NULL;
    }
}

static void ECSI_SignatureFree(ECSI_Signature *signature)
{
    ECSI_ParameterFree(&signature->result);

    for (usz i = 0; i < arrlenu(signature->parameters); i++)
    {
        ECSI_ParameterFree(&signature->parameters[i]);
    }

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

/// @brief Reads the type of a handle, "<name>", after the word "handle". The type must be registered.
static bool ECSI_SignatureReadHandle(const char **text, ECSI_Parameter *retParameter)
{
    const char *start = *text + 1;
    const char *end = SDL_strchr(start, '>');

    if (**text != '<' || end == NULL || end == start)
    {
        return false;
    }

    char *name = SDL_strndup(start, (usz)(end - start));
    retParameter->handle = name == NULL ? NULL : ECSI_ServicesFindHandleType(name);

    if (name != NULL && retParameter->handle == NULL)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Handle type '%s' is not registered.", name);
    }

    SDL_free(name);
    *text = end + 1;
    ECSI_SignatureSkipSpaces(text);
    return retParameter->handle != NULL;
}

/// @brief Reads the signature of a callback, "<signature>", after the word "fn". A callback's signature has no callbacks.
static bool ECSI_SignatureReadCallback(const char **text, ECSI_Parameter *retParameter)
{
    if (**text != '<')
    {
        return false;
    }

    // the signature ends at the '>' that matches the '<', because handles inside it have their own
    const char *start = *text + 1;
    const char *end = start;

    for (i32 depth = 1; *end != '\0' && (*end != '>' || --depth > 0); end++)
    {
        depth += *end == '<' ? 1 : 0;
    }

    char *inner = *end == '>' ? SDL_strndup(start, (usz)(end - start)) : NULL;
    retParameter->callback = inner == NULL ? NULL : SDL_calloc(1, sizeof(ECSI_Signature));
    bool valid = retParameter->callback != NULL && ECSI_SignatureParse(inner, retParameter->callback) == SHUResult_Ok;

    if (!valid)
    {
        SDL_free(retParameter->callback);
        retParameter->callback = NULL;
    }

    for (usz i = 0; valid && i < arrlenu(retParameter->callback->parameters); i++)
    {
        valid = retParameter->callback->parameters[i].type != ECSI_ParameterType_Fn;
    }

    SDL_free(inner);
    *text = *end == '>' ? end + 1 : end;
    ECSI_SignatureSkipSpaces(text);
    return valid;
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

            switch (type)
            {
            case ECSI_ParameterType_Handle:
                return ECSI_SignatureReadHandle(text, retParameter);
            case ECSI_ParameterType_Fn:
                return ECSI_SignatureReadCallback(text, retParameter);
            default:
                return true;
            }
        }
    }

    return false;
}

/// @brief Writes a parameter as the signature text has it.
static SHUResult ECSI_SignatureAppend(char **text, const char *separator, ECSI_Parameter parameter)
{
    char *next = NULL;

    // a handle names its type, and a callback its signature, between angle brackets
    const char *inner = parameter.type == ECSI_ParameterType_Handle ? parameter.handle->name : parameter.type == ECSI_ParameterType_Fn ? parameter.callback->text
                                                                                                                                       : NULL;

    if (SDL_asprintf(&next, "%s%s%s%s%s%s%s", *text == NULL ? "" : *text, separator, parameter.out ? "out " : "", ECSI_PARAMETER_TYPE_NAMES[parameter.type], inner == NULL ? "" : "<", inner == NULL ? "" : inner, inner == NULL ? "" : ">") < 0)
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

    // a callback is a parameter only; it is never an output or a result
    bool valid = ECSI_SignatureReadParameter(&cursor, &retSignature->result) && !retSignature->result.out && retSignature->result.type != ECSI_ParameterType_Fn && *cursor++ == '(';
    ECSI_SignatureSkipSpaces(&cursor);

    while (valid && *cursor != ')')
    {
        ECSI_Parameter parameter = {0};
        valid = ECSI_SignatureReadParameter(&cursor, &parameter) && parameter.type != ECSI_ParameterType_Void && !(parameter.out && parameter.type == ECSI_ParameterType_Fn) && (*cursor == ',' || *cursor == ')');
        cursor += valid && *cursor == ',' ? 1 : 0;
        ECSI_SignatureSkipSpaces(&cursor);

        if (!valid)
        {
            ECSI_ParameterFree(&parameter);
        }
        else
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

    // every plugin may use the core's functions
    if (function->plugin != NULL && !ECSI_PluginDependsOn(plugin, function->plugin))
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

static SHUResult ECSI_ServicesMakeCallback(lua_State *state, int index, const ECSI_Function *service, const ECSI_Signature *signature, ECSI_Function **retCallback);

static void ECSI_ServicesFreeCallback(ECSI_Function **callback);

#pragma region Lua Calls C

/// @brief Reads a Lua argument into a call slot. Raises a Lua error if it has the wrong type; values are read later, because they allocate.
static void ECSI_ServicesCheckArgument(lua_State *state, int index, ECSI_Parameter parameter, ECSI_Slot *slot)
{
    switch (parameter.type)
    {
    case ECSI_ParameterType_Handle:
        slot->pointer = ECSI_ServicesCheckHandle(index, parameter.handle->name);
        break;
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
    case ECSI_ParameterType_Fn:
        // the closure is made later, because it allocates
        if (!lua_isnoneornil(state, index))
        {
            luaL_checktype(state, index, LUA_TFUNCTION);
        }

        slot->pointer = NULL;
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
static void ECSI_ServicesPushOutput(lua_State *state, ECSI_Parameter parameter, const ECSI_Slot *slot, bool result)
{
    switch (parameter.type)
    {
    case ECSI_ParameterType_Handle:
        ECSI_ServicesPushHandle(parameter.handle->name, slot->pointer);
        break;
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

/// @brief Calls a C function from Lua through libffi. The function is the upvalue: a registered function, or a callback that C gave Lua. Output parameters become extra results.
static int ECSI_ServicesCallC(lua_State *state)
{
    const ECSI_Function *function = lua_touserdata(state, lua_upvalueindex(1));
    const ECSI_Signature *signature = &function->signature;
    usz count = arrlenu(signature->parameters);

    if (function->pointer == NULL)
    {
        return luaL_error(state, "a callback given to '%s' is valid only during the call that gave it", function->name);
    }

    // the storage is a userdata, so it is freed even when an argument raises an error
    ECSI_Slot *slots = lua_newuserdatauv(state, count * (2 * sizeof(ECSI_Slot) + 2 * sizeof(void *) + sizeof(int)) + 1, 0);
    ECSI_Slot *outputs = slots + count;
    void **arguments = (void **)(outputs + count);
    ECSI_Function **callbacks = (ECSI_Function **)(arguments + count); // closures made for Lua functions
    int *indices = (int *)(callbacks + count);                           // where value and callback arguments are on the stack
    int index = 1;

    for (usz i = 0; i < count; i++)
    {
        ECSI_Parameter parameter = signature->parameters[i];
        arguments[i] = &slots[i];
        outputs[i] = (ECSI_Slot){0};
        callbacks[i] = NULL;

        if (parameter.out)
        {
            slots[i].pointer = &outputs[i];
        }
        else
        {
            indices[i] = index;
            ECSI_ServicesCheckArgument(state, index++, parameter, &slots[i]);
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

    // a Lua function becomes a closure that C calls; it is valid until this call returns
    for (usz i = 0; !result && i < count; i++)
    {
        if (signature->parameters[i].type == ECSI_ParameterType_Fn && !lua_isnoneornil(state, indices[i]))
        {
            result = ECSI_ServicesMakeCallback(state, indices[i], function, signature->parameters[i].callback, &callbacks[i]);

            // C gets the closure's code; POSIX lets a data pointer hold code
            if (!result)
            {
                SDL_memcpy(&slots[i].pointer, &callbacks[i]->pointer, sizeof(slots[i].pointer));
            }
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
        ECSI_ServicesPushOutput(state, signature->result, &returned, true);
        pushed++;
    }

    for (usz i = 0; !result && i < count; i++)
    {
        if (signature->parameters[i].out)
        {
            ECSI_ServicesPushOutput(state, signature->parameters[i], &outputs[i], false);
            pushed++;
        }
    }

    for (usz i = 0; i < count; i++)
    {
        if (signature->parameters[i].type == ECSI_ParameterType_Value)
        {
            ECSValue_Destroy((ECSValue **)&slots[i].pointer);
        }

        if (callbacks[i] != NULL)
        {
            ECSI_ServicesFreeCallback(&callbacks[i]);
        }
    }

    if (result)
    {
        return luaL_error(state, "cannot convert a value for '%s' (%s)", function->name, SHUResult_String(result));
    }

    return pushed;
}

#pragma endregion Lua Calls C

#pragma region Handles

/// @brief Destroys the object of a Lua handle that the garbage collector frees.
static int ECSI_ServicesHandleCollect(lua_State *state)
{
    ECSI_Handle *handle = lua_touserdata(state, 1);

    if (handle->object != NULL && handle->type->Destroy != NULL)
    {
        handle->type->Destroy(handle->object);
    }

    handle->object = NULL;
    return 0;
}

static int ECSI_ServicesHandleText(lua_State *state)
{
    ECSI_Handle *handle = lua_touserdata(state, 1);
    lua_pushfstring(state, "handle<%s>%s", handle->type->name, handle->object == NULL ? " (gone)" : "");
    return 1;
}

/// @brief Destroys the object of a handle whose type a Lua plugin provides; the Lua value it kept may then be collected.
static void ECSI_ServicesDestroyLuaObject(void *object)
{
    ECSI_LuaObject *luaObject = object;
    luaL_unref(ECSI_LuaGetState(), LUA_REGISTRYINDEX, luaObject->value);
    SDL_free(luaObject);
}

static SHUResult ECSI_ServicesRegisterHandleType(ECSPlugin plugin, const char *name, ECSHandleDestroyFunction Destroy)
{
    if (ECSI_ServicesFindHandleType(name) != NULL)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Handle type '%s' is already registered.", name);
        return SHUResult_ErrBadData;
    }

    ECSI_HandleType *type = SDL_calloc(1, sizeof(ECSI_HandleType));

    if (type == NULL || (type->name = SDL_strdup(name)) == NULL || SDL_asprintf(&type->metatable, "%s%s", OPENECS_HANDLE_METATABLE, name) < 0)
    {
        if (type != NULL)
        {
            SDL_free(type->name);
        }

        SDL_free(type);
        return SHUResult_ErrAllocation;
    }

    type->plugin = plugin;
    type->Destroy = Destroy;

    lua_State *state = ECSI_LuaGetState();
    luaL_newmetatable(state, type->metatable);
    lua_pushcfunction(state, ECSI_ServicesHandleCollect);
    lua_setfield(state, -2, "__gc");
    lua_pushcfunction(state, ECSI_ServicesHandleText);
    lua_setfield(state, -2, "__tostring");
    lua_pop(state, 1);

    shput(SERVICES.handleTypes, type->name, type);
    return SHUResult_Ok;
}

/// @brief Makes every Lua handle of a type invalid; with destroy, their objects are destroyed first. NULL is every type.
static void ECSI_ServicesForgetHandles(const ECSI_HandleType *type, bool destroy)
{
    lua_State *state = ECSI_LuaGetState();
    lua_rawgeti(state, LUA_REGISTRYINDEX, SERVICES.handles);
    lua_pushnil(state);

    while (lua_next(state, -2) != 0)
    {
        ECSI_Handle *handle = lua_touserdata(state, -1);

        if (type == NULL || handle->type == type)
        {
            if (destroy && handle->object != NULL && handle->type->Destroy != NULL)
            {
                handle->type->Destroy(handle->object);
            }

            handle->object = NULL;
        }

        lua_pop(state, 1);
    }

    lua_pop(state, 1);
}

#pragma endregion Handles

#pragma region C Calls Lua

/// @brief Pushes an argument that C passed to a Lua function.
static void ECSI_ServicesPushArgument(lua_State *state, ECSI_Parameter parameter, const void *argument)
{
    switch (parameter.type)
    {
    case ECSI_ParameterType_Handle:
        ECSI_ServicesPushHandle(parameter.handle->name, *(void *const *)argument);
        break;
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
static bool ECSI_ServicesWriteOutput(lua_State *state, ECSI_Function *function, int index, int anchors, ECSI_Parameter parameter, void *target, bool result)
{
    int isNumber = 0;
    ECSI_ParameterType type = parameter.type;

    switch (type)
    {
    case ECSI_ParameterType_Handle:
    {
        // the handle stays alive in the anchors while C may use its object
        ECSI_Handle *handle = luaL_testudata(state, index, parameter.handle->metatable);
        *(void **)target = handle == NULL ? NULL : handle->object;
        lua_pushvalue(state, index);
        lua_rawseti(state, anchors, (lua_Integer)lua_rawlen(state, anchors) + 1);
        return handle != NULL || lua_isnil(state, index);
    }
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

/// @brief Pushes a callback that C passed to a Lua function: a Lua function that calls it, or nil for NULL. It is added to the table of callbacks at an index of the stack, which makes it invalid when the call returns.
static void ECSI_ServicesPushCallback(lua_State *state, const ECSI_Function *service, ECSI_Parameter parameter, const void *argument, int callbacks)
{
    ECSFunction pointer = NULL;
    SDL_memcpy(&pointer, argument, sizeof(pointer));

    if (pointer == NULL)
    {
        lua_pushnil(state);
        return;
    }

    // the record borrows the service's name and the callback's signature, and is never used after the call
    ECSI_Function *callback = lua_newuserdatauv(state, sizeof(ECSI_Function), 0);
    *callback = (ECSI_Function){.name = service->name, .plugin = service->plugin, .signature = *parameter.callback, .pointer = pointer, .caller = LUA_NOREF, .lua = LUA_NOREF, .anchors = LUA_NOREF};
    lua_pushvalue(state, -1);
    lua_rawseti(state, callbacks, (lua_Integer)lua_rawlen(state, callbacks) + 1);
    lua_pushcclosure(state, ECSI_ServicesCallC, 1);
}

/// @brief Makes the callbacks that C passed to a Lua function invalid, now that the call returned.
static void ECSI_ServicesEndCallbacks(lua_State *state, int callbacks)
{
    for (lua_Integer i = 1; i <= (lua_Integer)lua_rawlen(state, callbacks); i++)
    {
        lua_rawgeti(state, callbacks, i);
        ((ECSI_Function *)lua_touserdata(state, -1))->pointer = NULL;
        lua_pop(state, 1);
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
    lua_newtable(state);
    int callbacks = lua_gettop(state);

    lua_rawgeti(state, LUA_REGISTRYINDEX, function->lua);
    int pushed = 0;

    for (usz i = 0; i < count; i++)
    {
        ECSI_Parameter parameter = signature->parameters[i];

        if (parameter.type == ECSI_ParameterType_Fn)
        {
            ECSI_ServicesPushCallback(state, function, parameter, arguments[i], callbacks);
            pushed++;
        }
        else if (!parameter.out)
        {
            ECSI_ServicesPushArgument(state, parameter, arguments[i]);
            pushed++;
        }
    }

    bool failed = ECSI_LuaCall(pushed, resultCount) != SHUResult_Ok;
    ECSI_ServicesEndCallbacks(state, callbacks);

    if (failed)
    {
        ECSI_PluginReportError(function->plugin, lua_tostring(state, -1));
        lua_settop(state, top);
        return;
    }

    int index = callbacks + 1;

    if (signature->result.type != ECSI_ParameterType_Void && !ECSI_ServicesWriteOutput(state, function, index++, anchors, signature->result, result, true))
    {
        ECSI_ServicesReportType(state, function, index - 1);
    }

    for (usz i = 0; i < count; i++)
    {
        if (signature->parameters[i].out && !ECSI_ServicesWriteOutput(state, function, index++, anchors, signature->parameters[i], *(void **)arguments[i], false))
        {
            ECSI_ServicesReportType(state, function, index - 1);
        }
    }

    lua_settop(state, top);
}

/// @brief Makes a closure that C calls, which calls the Lua function at an index of the stack. Free it with ECSI_ServicesFreeCallback.
/// @param service The function that the callback is given to, for reports.
/// @param signature The callback's signature. The record borrows it.
static SHUResult ECSI_ServicesMakeCallback(lua_State *state, int index, const ECSI_Function *service, const ECSI_Signature *signature, ECSI_Function **retCallback)
{
    ECSI_Function *callback = SDL_calloc(1, sizeof(ECSI_Function));

    if (callback == NULL || SDL_asprintf(&callback->name, "a callback given to '%s'", service->name) < 0)
    {
        SDL_free(callback);
        return SHUResult_ErrAllocation;
    }

    callback->plugin = service->plugin;
    callback->signature = *signature;
    callback->caller = LUA_NOREF;

    void *code = NULL;
    callback->closure = ffi_closure_alloc(sizeof(ffi_closure), &code);

    if (callback->closure == NULL || ffi_prep_closure_loc(callback->closure, &callback->signature.cif, ECSI_ServicesCallLua, callback, code) != FFI_OK)
    {
        if (callback->closure != NULL)
        {
            ffi_closure_free(callback->closure);
        }

        SDL_free(callback->name);
        SDL_free(callback);
        return SHUResult_ErrAllocation;
    }

    SDL_memcpy(&callback->pointer, &code, sizeof(code));
    lua_pushvalue(state, index);
    callback->lua = luaL_ref(state, LUA_REGISTRYINDEX);
    lua_newtable(state);
    callback->anchors = luaL_ref(state, LUA_REGISTRYINDEX);
    *retCallback = callback;
    return SHUResult_Ok;
}

/// @brief Frees a closure that ECSI_ServicesMakeCallback made, and lets the garbage collector take its Lua function.
static void ECSI_ServicesFreeCallback(ECSI_Function **callback)
{
    lua_State *state = ECSI_LuaGetState();
    luaL_unref(state, LUA_REGISTRYINDEX, (*callback)->lua);
    luaL_unref(state, LUA_REGISTRYINDEX, (*callback)->anchors);
    ffi_closure_free((*callback)->closure);
    ECSValue_Destroy(&(*callback)->result);
    SDL_free((*callback)->name);
    SDL_free(*callback);
    *callback = NULL;
}

#pragma endregion C Calls Lua

/// @brief Checks a function's name and makes its record, without its code.
static SHUResult ECSI_ServicesCreate(ECSPlugin plugin, const char *name, const char *signature, const char *description, ECSI_Function **retFunction)
{
    // the core's own names start with "ecs."
    if (plugin == NULL ? SDL_strncmp(name, "ecs.", 4) != 0 : !ECSI_PluginOwnsName(plugin, name))
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

SHUResult ECSI_ServicesInitialize(void)
{
    lua_State *state = ECSI_LuaGetState();

    // the handles are weak values, so the table never keeps a handle alive
    lua_newtable(state);
    lua_newtable(state);
    lua_pushliteral(state, "v");
    lua_setfield(state, -2, "__mode");
    lua_setmetatable(state, -2);
    SERVICES.handles = luaL_ref(state, LUA_REGISTRYINDEX);

    return ECSI_ServicesRegisterHandleType(NULL, "ecs.panel", NULL);
}

void ECSI_ServicesPushHandle(const char *type, void *object)
{
    SDL_assert(type != NULL);

    lua_State *state = ECSI_LuaGetState();

    if (object == NULL)
    {
        lua_pushnil(state);
        return;
    }

    // the type is gone when its plugin was removed; a handle cannot be made without it
    ECSI_HandleType *handleType = ECSI_ServicesFindHandleType(type);

    if (handleType == NULL)
    {
        lua_pushnil(state);
        return;
    }

    lua_rawgeti(state, LUA_REGISTRYINDEX, SERVICES.handles);

    if (lua_rawgetp(state, -1, object) == LUA_TUSERDATA && ((ECSI_Handle *)lua_touserdata(state, -1))->type == handleType)
    {
        lua_remove(state, -2);
        return;
    }

    lua_pop(state, 1);
    ECSI_Handle *handle = lua_newuserdatauv(state, sizeof(ECSI_Handle), 0);
    *handle = (ECSI_Handle){.object = object, .type = handleType};
    luaL_setmetatable(state, handleType->metatable);
    lua_pushvalue(state, -1);
    lua_rawsetp(state, -3, object);
    lua_remove(state, -2);
}

void *ECSI_ServicesCheckHandle(int index, const char *type)
{
    SDL_assert(type != NULL);

    lua_State *state = ECSI_LuaGetState();
    ECSI_HandleType *handleType = ECSI_ServicesFindHandleType(type);

    if (handleType == NULL)
    {
        luaL_error(state, "handle type '%s' is gone; its plugin was removed", type);
        return NULL;
    }

    ECSI_Handle *handle = luaL_checkudata(state, index, handleType->metatable);

    if (handle->object == NULL)
    {
        luaL_argerror(state, index, "the object of this handle is gone");
    }

    return handle->object;
}

void ECSI_ServicesForgetHandle(void *object)
{
    SDL_assert(object != NULL);

    lua_State *state = ECSI_LuaGetState();
    lua_rawgeti(state, LUA_REGISTRYINDEX, SERVICES.handles);

    if (lua_rawgetp(state, -1, object) == LUA_TUSERDATA)
    {
        ((ECSI_Handle *)lua_touserdata(state, -1))->object = NULL;
    }

    lua_pop(state, 1);
    lua_pushnil(state);
    lua_rawsetp(state, -2, object);
    lua_pop(state, 1);
}

void ECSI_ServicesPushHandleMetatable(const char *type)
{
    ECSI_HandleType *handleType = ECSI_ServicesFindHandleType(type);

    if (handleType == NULL)
    {
        lua_pushnil(ECSI_LuaGetState());
        return;
    }

    luaL_getmetatable(ECSI_LuaGetState(), handleType->metatable);
}

void ECSI_ServicesTerminate(void)
{
    // handles that wait for their finalizer are no longer in the weak table, so a full collection runs those finalizers now, while the types and their plugins' code exist
    lua_gc(ECSI_LuaGetState(), LUA_GCCOLLECT);

    // objects are destroyed while their plugins' code is still loaded
    ECSI_ServicesForgetHandles(NULL, true);

    for (usz i = 0; i < shlenu(SERVICES.handleTypes); i++)
    {
        SDL_free(SERVICES.handleTypes[i].value->name);
        SDL_free(SERVICES.handleTypes[i].value->metatable);
        SDL_free(SERVICES.handleTypes[i].value);
    }

    shfree(SERVICES.handleTypes);

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

    // a failed plugin's objects may not be valid, so their handles are only forgotten
    for (usz i = shlenu(SERVICES.handleTypes); i > 0; i--)
    {
        ECSI_HandleType *type = SERVICES.handleTypes[i - 1].value;

        if (type->plugin == plugin)
        {
            // the core made the objects of a Lua plugin's types, so it can still destroy them
            ECSI_ServicesForgetHandles(type, type->Destroy == ECSI_ServicesDestroyLuaObject);
            type->Destroy = NULL;
        }
    }

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

SHUResult ECSI_ServicesRegisterCore(const char *name, ECSFunction function, const char *signature, const char *description)
{
    SDL_assert(name != NULL);
    SDL_assert(function != NULL);
    SDL_assert(signature != NULL);

    ECSI_Function *registered = NULL;
    SHU_ReturnResult(ECSI_ServicesCreate(NULL, name, signature, description, &registered));
    registered->pointer = function;
    shput(SERVICES.functions, registered->name, registered);
    return SHUResult_Ok;
}

SHUResult ECSI_ServicesCallBound(const char *name, ECSPanel focus)
{
    SDL_assert(name != NULL);

    ECSI_Function *function = SERVICES.functions == NULL ? NULL : shget(SERVICES.functions, name);

    if (function == NULL)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "A key is bound to '%s', which is not registered.", name);
        return SHUResult_ErrNotFound;
    }

    // a C function and a Lua function's closure are called the same way
    if (SDL_strcmp(function->signature.text, "void()") == 0)
    {
        function->pointer();
        return SHUResult_Ok;
    }

    if (SDL_strcmp(function->signature.text, "void(handle<ecs.panel>)") == 0)
    {
        ((ECSI_PanelFunction)function->pointer)(focus);
        return SHUResult_Ok;
    }

    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "A key is bound to '%s', which is %s; a bound function is void() or void(handle<ecs.panel>).", name, function->signature.text);
    return SHUResult_ErrBadData;
}

const char *ECSI_ServicesGetDescription(const char *name)
{
    SDL_assert(name != NULL);

    ECSI_Function *function = SERVICES.functions == NULL ? NULL : shget(SERVICES.functions, name);
    return function == NULL ? NULL : function->description;
}

SHUResult ECSI_ServicesRegisterLuaHandleType(ECSPlugin plugin, const char *name)
{
    SDL_assert(plugin != NULL);
    SDL_assert(name != NULL);

    return ECSI_PluginOwnsName(plugin, name) ? ECSI_ServicesRegisterHandleType(plugin, name, ECSI_ServicesDestroyLuaObject) : SHUResult_ErrBadData;
}

/// @brief Finds a handle type that a Lua plugin provides, and raises a Lua error if it is not the plugin's.
static ECSI_HandleType *ECSI_ServicesCheckLuaHandleType(lua_State *state, ECSPlugin plugin, const char *name)
{
    ECSI_HandleType *type = ECSI_ServicesFindHandleType(name);

    if (type == NULL || type->plugin != plugin || type->Destroy != ECSI_ServicesDestroyLuaObject)
    {
        luaL_error(state, "handle type '%s' is not one that this plugin registered in Lua", name);
    }

    return type;
}

void ECSI_ServicesPushLuaHandle(ECSPlugin plugin, const char *name, int index)
{
    SDL_assert(plugin != NULL);
    SDL_assert(name != NULL);

    lua_State *state = ECSI_LuaGetState();
    index = lua_absindex(state, index);
    ECSI_ServicesCheckLuaHandleType(state, plugin, name);

    ECSI_LuaObject *object = SDL_malloc(sizeof(ECSI_LuaObject));

    if (object == NULL)
    {
        luaL_error(state, "out of memory");
        return;
    }

    lua_pushvalue(state, index);
    object->value = luaL_ref(state, LUA_REGISTRYINDEX);
    ECSI_ServicesPushHandle(name, object);
}

void ECSI_ServicesPushLuaHandleValue(ECSPlugin plugin, const char *name, int index)
{
    SDL_assert(plugin != NULL);
    SDL_assert(name != NULL);

    lua_State *state = ECSI_LuaGetState();
    ECSI_ServicesCheckLuaHandleType(state, plugin, name);
    ECSI_LuaObject *object = ECSI_ServicesCheckHandle(index, name);

    // the check raises a Lua error instead of giving NULL
    if (object == NULL)
    {
        lua_pushnil(state);
        return;
    }

    lua_rawgeti(state, LUA_REGISTRYINDEX, object->value);
}

SHUResult ECSHandle_RegisterType(ECSPlugin plugin, const char *name, ECSHandleDestroyFunction Destroy)
{
    SDL_assert(plugin != NULL);
    SDL_assert(name != NULL);

    if (!ECSI_PluginOwnsName(plugin, name))
    {
        return SHUResult_ErrBadData;
    }

    return ECSI_ServicesRegisterHandleType(plugin, name, Destroy);
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
