#include "runtime/Services.h"

#include "base/Lua.h"

#include "lua/lauxlib.h"
#include "lua/lua.h"

#include "libffi/ffi.h"

#include "SDL3/SDL.h"
#include "stb/stbSDL3.h"

#pragma region Source Only

/// @brief Type of a parameter or result in a signature.
typedef enum ECSIParameterType
{
    ECSIParameterType_Void = 0,
    ECSIParameterType_Bool,
    ECSIParameterType_Int,
    ECSIParameterType_Int64,
    ECSIParameterType_Float,
    ECSIParameterType_Double,
    ECSIParameterType_String,
    ECSIParameterType_Buffer,
    ECSIParameterType_Value,
    ECSIParameterType_Handle,
    ECSIParameterType_Fn,
    ECSIParameterType_Count,
} ECSIParameterType;

/// @brief Names of the parameter types in signatures.
static const char *const OPENECS_PARAMETER_TYPE_NAMES[ECSIParameterType_Count] = {"void", "bool", "int", "int64", "float", "double", "string", "buffer", "value", "handle", "fn"};

/// @brief Prefix of the names of handle types' metatables.
#define OPENECS_HANDLE_METATABLE "ecs.handle:"

/// @brief A bound function that takes the focused panel: void(handle<ecs.panel>).
typedef void (*ECSIPanelFunction)(ECSPanel panel);

/// @brief A preset's open function, which takes a file's path: void(string).
typedef void (*ECSIOpenFunction)(const char *path);

/// @brief A registered type of handles.
typedef struct ECSIHandleType
{
    char *name;
    char *metatable;  // name of its metatable in the registry
    ECSPlugin plugin; // NULL for the core
    ECSHandleDestroyFunction Destroy;
} ECSIHandleType;

/// @brief A Lua handle: the userdata that stands for an object.
typedef struct ECSIHandle
{
    void *object; // NULL when the object is gone
    ECSIHandleType *type;
} ECSIHandle;

/// @brief The object of a handle whose type a Lua plugin provides: the Lua value it stands for.
typedef struct ECSILuaObject
{
    int value; // registry reference
} ECSILuaObject;

/// @brief A parameter or result of a signature.
typedef struct ECSIParameter
{
    ECSIParameterType type;
    char *name;                        // the name the signature gives it, or NULL; only for documentation
    bool out;                          // an output parameter: a pointer in C, an extra result in Lua
    ECSIHandleType *handle;           // handles: their type
    struct ECSISignature *callback;   // callbacks: their signature, owned by the parameter
} ECSIParameter;

/// @brief A parsed signature with its libffi call description.
typedef struct ECSISignature
{
    ECSIParameter result;
    ECSIParameter *parameters; // stb_ds array
    ffi_type **types;           // stb_ds array of the parameters' libffi types
    usz outCount;
    ffi_cif cif;
    char *text; // the signature written the same way every time, such as "int(string, out float)"
} ECSISignature;

/// @brief A registered function.
typedef struct ECSIFunction
{
    char *name;
    char *description;
    ECSPlugin plugin;
    ECSISignature signature;
    ECSFunction pointer;
    int caller;           // registry reference of the Lua function that calls it, made when Lua first asks for it
    int lua;              // registry reference of the Lua function, or LUA_NOREF for a C function
    int anchors;          // registry reference of a table that keeps the strings and buffers a Lua function last gave C
    ECSValue *result;     // the value a Lua function last returned to C, or NULL
    ffi_closure *closure; // the code that C calls for a Lua function, or NULL
} ECSIFunction;

/// @brief Storage for one argument, output or result of a call through libffi.
typedef union ECSISlot
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
} ECSISlot;

static_assert(sizeof(usz) == sizeof(u64), "a buffer's size is described to libffi as 64 bits");

/// @brief The libffi description of SHUSlice, which buffers are.
static ffi_type *OPENECS_SLICE_ELEMENTS[] = {&ffi_type_pointer, &ffi_type_uint64, NULL};
static ffi_type OPENECS_SLICE_TYPE = {0, 0, FFI_TYPE_STRUCT, OPENECS_SLICE_ELEMENTS};

static struct
{
    struct
    {
        char *key; // the function's own copy of its name
        ECSIFunction *value;
    } *functions; // stb_ds hash map; each function is allocated on its own, because Lua callers point to it
    struct
    {
        char *key; // the type's own copy of its name
        ECSIHandleType *value;
    } *handleTypes; // stb_ds hash map; handles point to their type
    int handles;    // registry reference of the weak table that maps each object to its Lua handle
} SERVICES = {0};

static ECSIHandleType *ECSIServices_FindHandleType(const char *name)
{
    // shget would allocate a map that is missing
    return SERVICES.handleTypes == NULL ? NULL : shget(SERVICES.handleTypes, name);
}

static ffi_type *ECSIServices_FfiType(ECSIParameter parameter)
{
    if (parameter.out)
    {
        return &ffi_type_pointer;
    }

    switch (parameter.type)
    {
    case ECSIParameterType_Void:
        return &ffi_type_void;
    case ECSIParameterType_Bool:
        return &ffi_type_uint8;
    case ECSIParameterType_Int:
        return &ffi_type_sint32;
    case ECSIParameterType_Int64:
        return &ffi_type_sint64;
    case ECSIParameterType_Float:
        return &ffi_type_float;
    case ECSIParameterType_Double:
        return &ffi_type_double;
    case ECSIParameterType_Buffer:
        return &OPENECS_SLICE_TYPE;
    default:
        return &ffi_type_pointer;
    }
}

static SHUResult ECSISignature_Parse(const char *text, ECSISignature *retSignature);

static void ECSISignature_Free(ECSISignature *signature);

/// @brief Frees what a parameter owns: its name and a callback's signature.
static void ECSIParameter_Free(ECSIParameter *parameter)
{
    SDL_free(parameter->name);
    parameter->name = NULL;

    if (parameter->callback != NULL)
    {
        ECSISignature_Free(parameter->callback);
        SDL_free(parameter->callback);
        parameter->callback = NULL;
    }
}

static void ECSISignature_Free(ECSISignature *signature)
{
    ECSIParameter_Free(&signature->result);

    for (usz i = 0; i < arrlenu(signature->parameters); i++)
    {
        ECSIParameter_Free(&signature->parameters[i]);
    }

    arrfree(signature->parameters);
    arrfree(signature->types);
    SDL_free(signature->text);
    SDL_zerop(signature);
}

static void ECSISignature_SkipSpaces(const char **text)
{
    while (SDL_isspace((unsigned char)**text))
    {
        (*text)++;
    }
}

/// @brief Reads one word of a signature and moves past it and the spaces after it.
/// @return The word's length; 0 if there is none.
static usz ECSISignature_ReadWord(const char **text, const char **retWord)
{
    *retWord = *text;

    while (SDL_isalnum((unsigned char)**text) || **text == '_')
    {
        (*text)++;
    }

    usz length = (usz)(*text - *retWord);
    ECSISignature_SkipSpaces(text);
    return length;
}

/// @brief Reads the type of a handle, "<name>", after the word "handle". The type must be registered.
static bool ECSISignature_ReadHandle(const char **text, ECSIParameter *retParameter)
{
    const char *start = *text + 1;
    const char *end = SDL_strchr(start, '>');

    if (**text != '<' || end == NULL || end == start)
    {
        return false;
    }

    char *name = SDL_strndup(start, (usz)(end - start));
    retParameter->handle = name == NULL ? NULL : ECSIServices_FindHandleType(name);

    if (name != NULL && retParameter->handle == NULL)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Handle type '%s' is not registered.", name);
    }

    SDL_free(name);
    *text = end + 1;
    ECSISignature_SkipSpaces(text);
    return retParameter->handle != NULL;
}

/// @brief Reads the signature of a callback, "<signature>", after the word "fn". A callback's signature has no callbacks.
static bool ECSISignature_ReadCallback(const char **text, ECSIParameter *retParameter)
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
    retParameter->callback = inner == NULL ? NULL : SDL_calloc(1, sizeof(ECSISignature));
    bool valid = retParameter->callback != NULL && ECSISignature_Parse(inner, retParameter->callback) == SHUResult_Ok;

    if (!valid)
    {
        SDL_free(retParameter->callback);
        retParameter->callback = NULL;
    }

    for (usz i = 0; valid && i < arrlenu(retParameter->callback->parameters); i++)
    {
        valid = retParameter->callback->parameters[i].type != ECSIParameterType_Fn;
    }

    SDL_free(inner);
    *text = *end == '>' ? end + 1 : end;
    ECSISignature_SkipSpaces(text);
    return valid;
}

/// @brief Reads a parameter or result of a signature: a type name, after "out" for an output parameter.
static bool ECSISignature_ReadParameter(const char **text, ECSIParameter *retParameter)
{
    const char *word = NULL;
    usz length = ECSISignature_ReadWord(text, &word);
    retParameter->out = length == 3 && SDL_strncmp(word, "out", 3) == 0;

    if (retParameter->out)
    {
        length = ECSISignature_ReadWord(text, &word);
    }

    for (i32 type = 0; type < ECSIParameterType_Count; type++)
    {
        const char *name = OPENECS_PARAMETER_TYPE_NAMES[type];

        if (length == SDL_strlen(name) && SDL_strncmp(word, name, length) == 0)
        {
            retParameter->type = (ECSIParameterType)type;

            switch (type)
            {
            case ECSIParameterType_Handle:
                return ECSISignature_ReadHandle(text, retParameter);
            case ECSIParameterType_Fn:
                return ECSISignature_ReadCallback(text, retParameter);
            default:
                return true;
            }
        }
    }

    return false;
}

/// @brief Writes a parameter as the signature text has it.
static SHUResult ECSISignature_Append(char **text, const char *separator, ECSIParameter parameter)
{
    char *next = NULL;

    // a handle names its type, and a callback its signature, between angle brackets
    const char *inner = parameter.type == ECSIParameterType_Handle ? parameter.handle->name : parameter.type == ECSIParameterType_Fn ? parameter.callback->text
                                                                                                                                       : NULL;

    if (SDL_asprintf(&next, "%s%s%s%s%s%s%s", *text == NULL ? "" : *text, separator, parameter.out ? "out " : "", OPENECS_PARAMETER_TYPE_NAMES[parameter.type], inner == NULL ? "" : "<", inner == NULL ? "" : inner, inner == NULL ? "" : ">") < 0)
    {
        return SHUResult_ErrAllocation;
    }

    SDL_free(*text);
    *text = next;
    return SHUResult_Ok;
}

/// @brief Parses a signature such as "int(string path, out float)", and prepares its libffi call description. Parameter names are kept apart from the signature's text, so they never change how signatures compare.
static SHUResult ECSISignature_Parse(const char *text, ECSISignature *retSignature)
{
    SDL_zerop(retSignature);
    const char *cursor = text;
    ECSISignature_SkipSpaces(&cursor);

    // a callback is a parameter only; it is never an output or a result
    bool valid = ECSISignature_ReadParameter(&cursor, &retSignature->result) && !retSignature->result.out && retSignature->result.type != ECSIParameterType_Fn && *cursor++ == '(';
    ECSISignature_SkipSpaces(&cursor);

    while (valid && *cursor != ')')
    {
        ECSIParameter parameter = {0};
        valid = ECSISignature_ReadParameter(&cursor, &parameter) && parameter.type != ECSIParameterType_Void && !(parameter.out && parameter.type == ECSIParameterType_Fn);

        // a parameter may have a name after its type, such as "float x"
        if (valid && (SDL_isalpha((unsigned char)*cursor) || *cursor == '_'))
        {
            const char *word = NULL;
            usz length = ECSISignature_ReadWord(&cursor, &word);
            parameter.name = SDL_strndup(word, length);
            valid = parameter.name != NULL;
        }

        valid = valid && (*cursor == ',' || *cursor == ')');
        cursor += valid && *cursor == ',' ? 1 : 0;
        ECSISignature_SkipSpaces(&cursor);

        if (!valid)
        {
            ECSIParameter_Free(&parameter);
        }
        else
        {
            arrput(retSignature->parameters, parameter);
            arrput(retSignature->types, ECSIServices_FfiType(parameter));
            retSignature->outCount += parameter.out ? 1 : 0;
        }
    }

    valid = valid && *cursor++ == ')';
    ECSISignature_SkipSpaces(&cursor);

    if (!valid || *cursor != '\0')
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "'%s' is not a signature, such as \"int(string, out float)\".", text);
        ECSISignature_Free(retSignature);
        return SHUResult_ErrBadData;
    }

    // the text is written again, so signatures compare the same however they are spaced
    usz count = arrlenu(retSignature->parameters);
    SHUResult result = ECSISignature_Append(&retSignature->text, "", retSignature->result);

    for (usz i = 0; !result && i <= count; i++)
    {
        result = i == count ? ECSISignature_Append(&retSignature->text, ")", (ECSIParameter){0}) : ECSISignature_Append(&retSignature->text, i == 0 ? "(" : ", ", retSignature->parameters[i]);
    }

    // the closing parenthesis was written before a "void"; that word is cut off again
    if (!result && count == 0)
    {
        SDL_free(retSignature->text);
        retSignature->text = NULL;
        result = SDL_asprintf(&retSignature->text, "%s()", OPENECS_PARAMETER_TYPE_NAMES[retSignature->result.type]) < 0 ? SHUResult_ErrAllocation : SHUResult_Ok;
    }
    else if (!result)
    {
        retSignature->text[SDL_strlen(retSignature->text) - SDL_strlen("void")] = '\0';
    }

    SHU_ReturnResult(result, ECSISignature_Free(retSignature););

    if (ffi_prep_cif(&retSignature->cif, FFI_DEFAULT_ABI, (unsigned int)count, ECSIServices_FfiType(retSignature->result), retSignature->types) != FFI_OK)
    {
        ECSISignature_Free(retSignature);
        return SHUResult_ErrInternal;
    }

    return SHUResult_Ok;
}

static void ECSIFunction_Free(ECSIFunction *function)
{
    if (function->closure != NULL)
    {
        ffi_closure_free(function->closure);
    }

    ECSValue_Destroy(&function->result);
    ECSISignature_Free(&function->signature);
    SDL_free(function->name);
    SDL_free(function->description);
    SDL_free(function);
}

/// @brief Finds a function that a plugin may use, and checks the signature the plugin expects.
static SHUResult ECSIServices_Find(ECSPlugin plugin, const char *name, const char *signature, ECSIFunction **retFunction)
{
    // shget would allocate a map that is missing
    ECSIFunction *function = SERVICES.functions == NULL ? NULL : shget(SERVICES.functions, name);

    if (function == NULL)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' looks up '%s', which is not registered.", ECSIPlugin_GetName(plugin), name);
        return SHUResult_ErrNotFound;
    }

    // every plugin may use the core's functions
    if (function->plugin != NULL && !ECSIPlugin_DependsOn(plugin, function->plugin))
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' looks up '%s', but its manifest does not depend on '%s'.", ECSIPlugin_GetName(plugin), name, ECSIPlugin_GetName(function->plugin));
        return SHUResult_ErrPrivileges;
    }

    if (signature != NULL)
    {
        ECSISignature expected;
        SHU_ReturnResult(ECSISignature_Parse(signature, &expected));
        bool matches = SDL_strcmp(expected.text, function->signature.text) == 0;
        ECSISignature_Free(&expected);

        if (!matches)
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' expects '%s' to be %s, but it is %s.", ECSIPlugin_GetName(plugin), name, signature, function->signature.text);
            return SHUResult_ErrBadData;
        }
    }

    *retFunction = function;
    return SHUResult_Ok;
}

static SHUResult ECSIServices_MakeCallback(lua_State *state, int index, const ECSIFunction *service, const ECSISignature *signature, ECSIFunction **retCallback);

static void ECSIServices_FreeCallback(ECSIFunction **callback);

#pragma region Lua Calls C

/// @brief Reads a Lua argument into a call slot. Raises a Lua error if it has the wrong type; values are read later, because they allocate.
static void ECSIServices_CheckArgument(lua_State *state, int index, ECSIParameter parameter, ECSISlot *slot)
{
    switch (parameter.type)
    {
    case ECSIParameterType_Handle:
        slot->pointer = ECSIServices_CheckHandle(index, parameter.handle->name);
        break;
    case ECSIParameterType_Bool:
        slot->boolean = (u8)(lua_toboolean(state, index) != 0);
        break;
    case ECSIParameterType_Int:
    {
        lua_Integer integer = luaL_checkinteger(state, index);
        luaL_argcheck(state, integer >= SDL_MIN_SINT32 && integer <= SDL_MAX_SINT32, index, "does not fit in an int");
        slot->integer = (i32)integer;
        break;
    }
    case ECSIParameterType_Int64:
        slot->integer64 = (i64)luaL_checkinteger(state, index);
        break;
    case ECSIParameterType_Float:
        slot->single = (f32)luaL_checknumber(state, index);
        break;
    case ECSIParameterType_Double:
        slot->number = (f64)luaL_checknumber(state, index);
        break;
    case ECSIParameterType_String:
        // valid during the call, because the argument stays on the stack
        slot->string = luaL_checkstring(state, index);
        break;
    case ECSIParameterType_Fn:
        // the closure is made later, because it allocates
        if (!lua_isnoneornil(state, index))
        {
            luaL_checktype(state, index, LUA_TFUNCTION);
        }

        slot->pointer = NULL;
        break;
    case ECSIParameterType_Buffer:
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
static void ECSIServices_PushOutput(lua_State *state, ECSIParameter parameter, const ECSISlot *slot, bool result)
{
    switch (parameter.type)
    {
    case ECSIParameterType_Handle:
        ECSIServices_PushHandle(parameter.handle->name, slot->pointer);
        break;
    case ECSIParameterType_Bool:
        lua_pushboolean(state, (result ? (u8)slot->integral : slot->boolean) != 0);
        break;
    case ECSIParameterType_Int:
        lua_pushinteger(state, (lua_Integer)(result ? (i32)slot->integral : slot->integer));
        break;
    case ECSIParameterType_Int64:
        lua_pushinteger(state, (lua_Integer)slot->integer64);
        break;
    case ECSIParameterType_Float:
        lua_pushnumber(state, (lua_Number)slot->single);
        break;
    case ECSIParameterType_Double:
        lua_pushnumber(state, (lua_Number)slot->number);
        break;
    case ECSIParameterType_String:
        lua_pushstring(state, slot->string);
        break;
    case ECSIParameterType_Buffer:
        lua_pushlstring(state, slot->buffer.data == NULL ? "" : slot->buffer.data, slot->buffer.data == NULL ? 0 : slot->buffer.size);
        break;
    case ECSIParameterType_Value:
        ECSILua_PushValue(slot->pointer);
        break;
    default:
        lua_pushnil(state);
        break;
    }
}

/// @brief Calls a C function from Lua through libffi. The function is the upvalue: a registered function, or a callback that C gave Lua. Output parameters become extra results.
static int ECSIServices_CallC(lua_State *state)
{
    const ECSIFunction *function = lua_touserdata(state, lua_upvalueindex(1));
    const ECSISignature *signature = &function->signature;
    usz count = arrlenu(signature->parameters);

    if (function->pointer == NULL)
    {
        return luaL_error(state, "a callback given to '%s' is valid only during the call that gave it", function->name);
    }

    // the storage is a userdata, so it is freed even when an argument raises an error
    ECSISlot *slots = lua_newuserdatauv(state, count * (2 * sizeof(ECSISlot) + 2 * sizeof(void *) + sizeof(int)) + 1, 0);
    ECSISlot *outputs = slots + count;
    void **arguments = (void **)(outputs + count);
    ECSIFunction **callbacks = (ECSIFunction **)(arguments + count); // closures made for Lua functions
    int *indices = (int *)(callbacks + count);                           // where value and callback arguments are on the stack
    int index = 1;

    for (usz i = 0; i < count; i++)
    {
        ECSIParameter parameter = signature->parameters[i];
        arguments[i] = &slots[i];
        outputs[i] = (ECSISlot){0};
        callbacks[i] = NULL;

        if (parameter.out)
        {
            slots[i].pointer = &outputs[i];
        }
        else
        {
            indices[i] = index;
            ECSIServices_CheckArgument(state, index++, parameter, &slots[i]);
        }
    }

    // values allocate, so they are made after every check that can raise an error
    SHUResult result = SHUResult_Ok;

    for (usz i = 0; i < count; i++)
    {
        ECSIParameter parameter = signature->parameters[i];

        if (parameter.type != ECSIParameterType_Value)
        {
            continue;
        }

        ECSValue *value = NULL;
        result = result ? result : ECSValue_Create(&value);
        result = result || parameter.out ? result : ECSILua_GetValue(indices[i], value);
        slots[i].pointer = value;

        if (parameter.out)
        {
            outputs[i].pointer = value;
        }
    }

    // a Lua function becomes a closure that C calls; it is valid until this call returns
    for (usz i = 0; !result && i < count; i++)
    {
        if (signature->parameters[i].type == ECSIParameterType_Fn && !lua_isnoneornil(state, indices[i]))
        {
            result = ECSIServices_MakeCallback(state, indices[i], function, signature->parameters[i].callback, &callbacks[i]);

            // C gets the closure's code; POSIX lets a data pointer hold code
            if (!result)
            {
                SDL_memcpy(&slots[i].pointer, &callbacks[i]->pointer, sizeof(slots[i].pointer));
            }
        }
    }

    ECSISlot returned = {0};

    if (!result)
    {
        ffi_call((ffi_cif *)&signature->cif, FFI_FN(function->pointer), &returned, arguments);
    }

    int pushed = 0;

    if (!result && signature->result.type != ECSIParameterType_Void)
    {
        ECSIServices_PushOutput(state, signature->result, &returned, true);
        pushed++;
    }

    for (usz i = 0; !result && i < count; i++)
    {
        if (signature->parameters[i].out)
        {
            ECSIServices_PushOutput(state, signature->parameters[i], &outputs[i], false);
            pushed++;
        }
    }

    for (usz i = 0; i < count; i++)
    {
        if (signature->parameters[i].type == ECSIParameterType_Value)
        {
            ECSValue_Destroy((ECSValue **)&slots[i].pointer);
        }

        if (callbacks[i] != NULL)
        {
            ECSIServices_FreeCallback(&callbacks[i]);
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
static int ECSIServices_HandleCollect(lua_State *state)
{
    ECSIHandle *handle = lua_touserdata(state, 1);

    if (handle->object != NULL && handle->type->Destroy != NULL)
    {
        handle->type->Destroy(handle->object);
    }

    handle->object = NULL;
    return 0;
}

static int ECSIServices_HandleText(lua_State *state)
{
    ECSIHandle *handle = lua_touserdata(state, 1);
    lua_pushfstring(state, "handle<%s>%s", handle->type->name, handle->object == NULL ? " (gone)" : "");
    return 1;
}

/// @brief Destroys the object of a handle whose type a Lua plugin provides; the Lua value it kept may then be collected.
static void ECSIServices_DestroyLuaObject(void *object)
{
    ECSILuaObject *luaObject = object;
    luaL_unref(ECSILua_GetState(), LUA_REGISTRYINDEX, luaObject->value);
    SDL_free(luaObject);
}

static SHUResult ECSIServices_RegisterHandleType(ECSPlugin plugin, const char *name, ECSHandleDestroyFunction Destroy)
{
    if (ECSIServices_FindHandleType(name) != NULL)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Handle type '%s' is already registered.", name);
        return SHUResult_ErrBadData;
    }

    ECSIHandleType *type = SDL_calloc(1, sizeof(ECSIHandleType));

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

    lua_State *state = ECSILua_GetState();
    luaL_newmetatable(state, type->metatable);
    lua_pushcfunction(state, ECSIServices_HandleCollect);
    lua_setfield(state, -2, "__gc");
    lua_pushcfunction(state, ECSIServices_HandleText);
    lua_setfield(state, -2, "__tostring");
    lua_pop(state, 1);

    shput(SERVICES.handleTypes, type->name, type);
    return SHUResult_Ok;
}

/// @brief Makes every Lua handle of a type invalid; with destroy, their objects are destroyed first. NULL is every type.
static void ECSIServices_ForgetHandles(const ECSIHandleType *type, bool destroy)
{
    lua_State *state = ECSILua_GetState();
    lua_rawgeti(state, LUA_REGISTRYINDEX, SERVICES.handles);
    lua_pushnil(state);

    while (lua_next(state, -2) != 0)
    {
        ECSIHandle *handle = lua_touserdata(state, -1);

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
static void ECSIServices_PushArgument(lua_State *state, ECSIParameter parameter, const void *argument)
{
    switch (parameter.type)
    {
    case ECSIParameterType_Handle:
        ECSIServices_PushHandle(parameter.handle->name, *(void *const *)argument);
        break;
    case ECSIParameterType_Bool:
        lua_pushboolean(state, *(const u8 *)argument != 0);
        break;
    case ECSIParameterType_Int:
        lua_pushinteger(state, (lua_Integer) * (const i32 *)argument);
        break;
    case ECSIParameterType_Int64:
        lua_pushinteger(state, (lua_Integer) * (const i64 *)argument);
        break;
    case ECSIParameterType_Float:
        lua_pushnumber(state, (lua_Number) * (const f32 *)argument);
        break;
    case ECSIParameterType_Double:
        lua_pushnumber(state, (lua_Number) * (const f64 *)argument);
        break;
    case ECSIParameterType_String:
        lua_pushstring(state, *(const char *const *)argument);
        break;
    case ECSIParameterType_Buffer:
    {
        const SHUSlice *buffer = argument;
        lua_pushlstring(state, buffer->data == NULL ? "" : buffer->data, buffer->data == NULL ? 0 : buffer->size);
        break;
    }
    case ECSIParameterType_Value:
        ECSILua_PushValue(*(const ECSValue *const *)argument);
        break;
    default:
        lua_pushnil(state);
        break;
    }
}

/// @brief Writes a value that a Lua function gave, at an index of the stack, where C reads it: a result or an output.
/// @param anchors Index of the table that keeps strings and buffers alive until the function returns again.
/// @return false if the Lua value has the wrong type.
static bool ECSIServices_WriteOutput(lua_State *state, ECSIFunction *function, int index, int anchors, ECSIParameter parameter, void *target, bool result)
{
    int isNumber = 0;
    ECSIParameterType type = parameter.type;

    switch (type)
    {
    case ECSIParameterType_Handle:
    {
        // the handle stays alive in the anchors while C may use its object
        ECSIHandle *handle = luaL_testudata(state, index, parameter.handle->metatable);
        *(void **)target = handle == NULL ? NULL : handle->object;
        lua_pushvalue(state, index);
        lua_rawseti(state, anchors, (lua_Integer)lua_rawlen(state, anchors) + 1);
        return handle != NULL || lua_isnil(state, index);
    }
    case ECSIParameterType_Bool:
        if (result)
        {
            *(ffi_arg *)target = (ffi_arg)lua_toboolean(state, index);
        }
        else
        {
            *(u8 *)target = (u8)(lua_toboolean(state, index) != 0);
        }

        return true;
    case ECSIParameterType_Int:
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
    case ECSIParameterType_Int64:
        *(i64 *)target = (i64)lua_tointegerx(state, index, &isNumber);
        return isNumber;
    case ECSIParameterType_Float:
        *(f32 *)target = (f32)lua_tonumberx(state, index, &isNumber);
        return isNumber;
    case ECSIParameterType_Double:
        *(f64 *)target = (f64)lua_tonumberx(state, index, &isNumber);
        return isNumber;
    case ECSIParameterType_String:
    case ECSIParameterType_Buffer:
    {
        bool text = lua_type(state, index) == LUA_TSTRING;
        usz length = 0;
        const char *bytes = text ? lua_tolstring(state, index, &length) : NULL;

        // the anchor keeps the string alive; a buffer gets a copy, because C may write into it
        if (type == ECSIParameterType_Buffer && text)
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

        if (type == ECSIParameterType_String)
        {
            *(const char **)target = bytes;
        }
        else
        {
            *(SHUSlice *)target = cs((void *)bytes, length);
        }

        return text;
    }
    case ECSIParameterType_Value:
    {
        // a result value is kept until the function returns again; an output fills the caller's value
        ECSValue *value = result ? function->result : target;

        if (result)
        {
            *(const ECSValue **)target = value;
        }

        return value != NULL && ECSILua_GetValue(index, value) == SHUResult_Ok;
    }
    default:
        return true;
    }
}

/// @brief Reports that a Lua function gave a value of the wrong type.
static void ECSIServices_ReportType(lua_State *state, const ECSIFunction *function, int index)
{
    char *message = NULL;

    if (SDL_asprintf(&message, "'%s' gave a %s where its signature %s needs another type.", function->name, luaL_typename(state, index), function->signature.text) >= 0)
    {
        ECSIPlugin_ReportError(function->plugin, message);
        SDL_free(message);
    }
}

/// @brief Pushes a callback that C passed to a Lua function: a Lua function that calls it, or nil for NULL. It is added to the table of callbacks at an index of the stack, which makes it invalid when the call returns.
static void ECSIServices_PushCallback(lua_State *state, const ECSIFunction *service, ECSIParameter parameter, const void *argument, int callbacks)
{
    ECSFunction pointer = NULL;
    SDL_memcpy(&pointer, argument, sizeof(pointer));

    if (pointer == NULL)
    {
        lua_pushnil(state);
        return;
    }

    // the record borrows the service's name and the callback's signature, and is never used after the call
    ECSIFunction *callback = lua_newuserdatauv(state, sizeof(ECSIFunction), 0);
    *callback = (ECSIFunction){.name = service->name, .plugin = service->plugin, .signature = *parameter.callback, .pointer = pointer, .caller = LUA_NOREF, .lua = LUA_NOREF, .anchors = LUA_NOREF};
    lua_pushvalue(state, -1);
    lua_rawseti(state, callbacks, (lua_Integer)lua_rawlen(state, callbacks) + 1);
    lua_pushcclosure(state, ECSIServices_CallC, 1);
}

/// @brief Makes the callbacks that C passed to a Lua function invalid, now that the call returned.
static void ECSIServices_EndCallbacks(lua_State *state, int callbacks)
{
    for (lua_Integer i = 1; i <= (lua_Integer)lua_rawlen(state, callbacks); i++)
    {
        lua_rawgeti(state, callbacks, i);
        ((ECSIFunction *)lua_touserdata(state, -1))->pointer = NULL;
        lua_pop(state, 1);
    }
}

/// @brief The handler of a Lua function's closure: C calls it through the closure's code. Output parameters are read from the extra results.
static void ECSIServices_CallLua(ffi_cif *cif, void *result, void **arguments, void *data)
{
    (void)cif;
    ECSIFunction *function = data;
    const ECSISignature *signature = &function->signature;
    lua_State *state = ECSILua_GetState();
    int top = lua_gettop(state);
    usz count = arrlenu(signature->parameters);
    int resultCount = (signature->result.type != ECSIParameterType_Void ? 1 : 0) + (int)signature->outCount;

    // a failed call gives C a zero result
    SDL_memset(result, 0, SDL_max(sizeof(ffi_arg), ECSIServices_FfiType(signature->result)->size));

    if (signature->result.type == ECSIParameterType_Value)
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
        ECSIParameter parameter = signature->parameters[i];

        if (parameter.type == ECSIParameterType_Fn)
        {
            ECSIServices_PushCallback(state, function, parameter, arguments[i], callbacks);
            pushed++;
        }
        else if (!parameter.out)
        {
            ECSIServices_PushArgument(state, parameter, arguments[i]);
            pushed++;
        }
    }

    bool failed = ECSILua_Call(pushed, resultCount) != SHUResult_Ok;
    ECSIServices_EndCallbacks(state, callbacks);

    if (failed)
    {
        ECSIPlugin_ReportError(function->plugin, lua_tostring(state, -1));
        lua_settop(state, top);
        return;
    }

    int index = callbacks + 1;

    if (signature->result.type != ECSIParameterType_Void && !ECSIServices_WriteOutput(state, function, index++, anchors, signature->result, result, true))
    {
        ECSIServices_ReportType(state, function, index - 1);
    }

    for (usz i = 0; i < count; i++)
    {
        if (signature->parameters[i].out && !ECSIServices_WriteOutput(state, function, index++, anchors, signature->parameters[i], *(void **)arguments[i], false))
        {
            ECSIServices_ReportType(state, function, index - 1);
        }
    }

    lua_settop(state, top);
}

/// @brief Makes a closure that C calls, which calls the Lua function at an index of the stack. Free it with ECSIServices_FreeCallback.
/// @param service The function that the callback is given to, for reports.
/// @param signature The callback's signature. The record borrows it.
static SHUResult ECSIServices_MakeCallback(lua_State *state, int index, const ECSIFunction *service, const ECSISignature *signature, ECSIFunction **retCallback)
{
    ECSIFunction *callback = SDL_calloc(1, sizeof(ECSIFunction));

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

    if (callback->closure == NULL || ffi_prep_closure_loc(callback->closure, &callback->signature.cif, ECSIServices_CallLua, callback, code) != FFI_OK)
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

/// @brief Frees a closure that ECSIServices_MakeCallback made, and lets the garbage collector take its Lua function.
static void ECSIServices_FreeCallback(ECSIFunction **callback)
{
    lua_State *state = ECSILua_GetState();
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
static SHUResult ECSIServices_Create(ECSPlugin plugin, const char *name, const char *signature, const char *description, ECSIFunction **retFunction)
{
    // the core's own names start with "ecs."
    if (plugin == NULL ? SDL_strncmp(name, "ecs.", 4) != 0 : !ECSIPlugin_OwnsName(plugin, name))
    {
        return SHUResult_ErrBadData;
    }

    if (SERVICES.functions != NULL && shget(SERVICES.functions, name) != NULL)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Function '%s' is already registered.", name);
        return SHUResult_ErrBadData;
    }

    ECSIFunction *function = SDL_calloc(1, sizeof(ECSIFunction));

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

    SHUResult result = function->name == NULL || function->description == NULL ? SHUResult_ErrAllocation : ECSISignature_Parse(signature, &function->signature);

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

SHUResult ECSIServices_Initialize(void)
{
    lua_State *state = ECSILua_GetState();

    // the handles are weak values, so the table never keeps a handle alive
    lua_newtable(state);
    lua_newtable(state);
    lua_pushliteral(state, "v");
    lua_setfield(state, -2, "__mode");
    lua_setmetatable(state, -2);
    SERVICES.handles = luaL_ref(state, LUA_REGISTRYINDEX);

    // the core's handle types: panels, popups, and the surface a panel or popup draws into, valid during Draw
    SHU_ReturnResult(ECSIServices_RegisterHandleType(NULL, "ecs.panel", NULL));
    SHU_ReturnResult(ECSIServices_RegisterHandleType(NULL, "ecs.popup", NULL));
    return ECSIServices_RegisterHandleType(NULL, "ecs.surface", NULL);
}

void ECSIServices_PushHandle(const char *type, void *object)
{
    SDL_assert(type != NULL);

    lua_State *state = ECSILua_GetState();

    if (object == NULL)
    {
        lua_pushnil(state);
        return;
    }

    // the type is gone when its plugin was removed; a handle cannot be made without it
    ECSIHandleType *handleType = ECSIServices_FindHandleType(type);

    if (handleType == NULL)
    {
        lua_pushnil(state);
        return;
    }

    lua_rawgeti(state, LUA_REGISTRYINDEX, SERVICES.handles);

    if (lua_rawgetp(state, -1, object) == LUA_TUSERDATA && ((ECSIHandle *)lua_touserdata(state, -1))->type == handleType)
    {
        lua_remove(state, -2);
        return;
    }

    lua_pop(state, 1);
    ECSIHandle *handle = lua_newuserdatauv(state, sizeof(ECSIHandle), 0);
    *handle = (ECSIHandle){.object = object, .type = handleType};
    luaL_setmetatable(state, handleType->metatable);
    lua_pushvalue(state, -1);
    lua_rawsetp(state, -3, object);
    lua_remove(state, -2);
}

void *ECSIServices_CheckHandle(int index, const char *type)
{
    SDL_assert(type != NULL);

    lua_State *state = ECSILua_GetState();
    ECSIHandleType *handleType = ECSIServices_FindHandleType(type);

    if (handleType == NULL)
    {
        luaL_error(state, "handle type '%s' is gone; its plugin was removed", type);
        return NULL;
    }

    ECSIHandle *handle = luaL_checkudata(state, index, handleType->metatable);

    if (handle->object == NULL)
    {
        luaL_argerror(state, index, "the object of this handle is gone");
    }

    return handle->object;
}

void ECSIServices_ForgetHandle(void *object)
{
    SDL_assert(object != NULL);

    lua_State *state = ECSILua_GetState();
    lua_rawgeti(state, LUA_REGISTRYINDEX, SERVICES.handles);

    if (lua_rawgetp(state, -1, object) == LUA_TUSERDATA)
    {
        ((ECSIHandle *)lua_touserdata(state, -1))->object = NULL;
    }

    lua_pop(state, 1);
    lua_pushnil(state);
    lua_rawsetp(state, -2, object);
    lua_pop(state, 1);
}

void ECSIServices_PushHandleMetatable(const char *type)
{
    ECSIHandleType *handleType = ECSIServices_FindHandleType(type);

    if (handleType == NULL)
    {
        lua_pushnil(ECSILua_GetState());
        return;
    }

    luaL_getmetatable(ECSILua_GetState(), handleType->metatable);
}

void ECSIServices_Terminate(void)
{
    // handles that wait for their finalizer are no longer in the weak table, so a full collection runs those finalizers now, while the types and their plugins' code exist
    lua_gc(ECSILua_GetState(), LUA_GCCOLLECT);

    // objects are destroyed while their plugins' code is still loaded
    ECSIServices_ForgetHandles(NULL, true);

    for (usz i = 0; i < shlenu(SERVICES.handleTypes); i++)
    {
        SDL_free(SERVICES.handleTypes[i].value->name);
        SDL_free(SERVICES.handleTypes[i].value->metatable);
        SDL_free(SERVICES.handleTypes[i].value);
    }

    shfree(SERVICES.handleTypes);

    for (usz i = 0; i < shlenu(SERVICES.functions); i++)
    {
        ECSIFunction_Free(SERVICES.functions[i].value);
    }

    shfree(SERVICES.functions);
    SDL_zero(SERVICES);
}

void ECSIServices_RemovePlugin(ECSPlugin plugin)
{
    SDL_assert(plugin != NULL);

    // a failed plugin's objects may not be valid, so their handles are only forgotten
    for (usz i = shlenu(SERVICES.handleTypes); i > 0; i--)
    {
        ECSIHandleType *type = SERVICES.handleTypes[i - 1].value;

        if (type->plugin == plugin)
        {
            // the core made the objects of a Lua plugin's types, so it can still destroy them
            ECSIServices_ForgetHandles(type, type->Destroy == ECSIServices_DestroyLuaObject);
            type->Destroy = NULL;
        }
    }

    // backwards, because shdel moves the last function into the hole
    for (usz i = shlenu(SERVICES.functions); i > 0; i--)
    {
        ECSIFunction *function = SERVICES.functions[i - 1].value;

        if (function->plugin == plugin)
        {
            (void)shdel(SERVICES.functions, function->name);
            ECSIFunction_Free(function);
        }
    }
}

SHUResult ECSIServices_PushFunction(ECSPlugin plugin, const char *name, const char *signature)
{
    SDL_assert(plugin != NULL);
    SDL_assert(name != NULL);

    ECSIFunction *function = NULL;
    SHU_ReturnResult(ECSIServices_Find(plugin, name, signature, &function));

    lua_State *state = ECSILua_GetState();

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
        lua_pushcclosure(state, ECSIServices_CallC, 1);
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

    ECSIFunction *registered = NULL;
    SHU_ReturnResult(ECSIServices_Create(plugin, name, signature, description, &registered));
    registered->pointer = function;
    shput(SERVICES.functions, registered->name, registered);
    return SHUResult_Ok;
}

SHUResult ECSIServices_RegisterLua(ECSPlugin plugin, const char *name, const char *signature, const char *description)
{
    SDL_assert(plugin != NULL);
    SDL_assert(name != NULL);
    SDL_assert(signature != NULL);

    ECSIFunction *function = NULL;
    SHU_ReturnResult(ECSIServices_Create(plugin, name, signature, description, &function));

    void *code = NULL;
    function->closure = ffi_closure_alloc(sizeof(ffi_closure), &code);

    if (function->closure == NULL || ffi_prep_closure_loc(function->closure, &function->signature.cif, ECSIServices_CallLua, function, code) != FFI_OK)
    {
        ECSIFunction_Free(function);
        return SHUResult_ErrAllocation;
    }

    // C calls the closure's code, which calls the Lua function; POSIX lets a data pointer hold code, but ISO C has no cast for it
    SDL_memcpy(&function->pointer, &code, sizeof(code));
    lua_State *state = ECSILua_GetState();
    function->lua = luaL_ref(state, LUA_REGISTRYINDEX);
    lua_newtable(state);
    function->anchors = luaL_ref(state, LUA_REGISTRYINDEX);
    shput(SERVICES.functions, function->name, function);
    return SHUResult_Ok;
}

SHUResult ECSIServices_RegisterCore(const char *name, ECSFunction function, const char *signature, const char *description)
{
    SDL_assert(name != NULL);
    SDL_assert(function != NULL);
    SDL_assert(signature != NULL);

    ECSIFunction *registered = NULL;
    SHU_ReturnResult(ECSIServices_Create(NULL, name, signature, description, &registered));
    registered->pointer = function;
    shput(SERVICES.functions, registered->name, registered);
    return SHUResult_Ok;
}

SHUResult ECSIServices_CallBound(const char *name, ECSPanel focus)
{
    SDL_assert(name != NULL);

    ECSIFunction *function = SERVICES.functions == NULL ? NULL : shget(SERVICES.functions, name);

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
        ((ECSIPanelFunction)function->pointer)(focus);
        return SHUResult_Ok;
    }

    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "A key is bound to '%s', which is %s; a bound function is void() or void(handle<ecs.panel>).", name, function->signature.text);
    return SHUResult_ErrBadData;
}

SHUResult ECSIServices_CallOpen(const char *name, const char *path)
{
    SDL_assert(name != NULL);
    SDL_assert(path != NULL);

    ECSIFunction *function = SERVICES.functions == NULL ? NULL : shget(SERVICES.functions, name);

    if (function == NULL)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "The preset opens files with '%s', which is not registered.", name);
        return SHUResult_ErrNotFound;
    }

    if (SDL_strcmp(function->signature.text, "void(string)") != 0)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "The preset opens files with '%s', which is %s; it must be void(string).", name, function->signature.text);
        return SHUResult_ErrBadData;
    }

    ((ECSIOpenFunction)function->pointer)(path);
    return SHUResult_Ok;
}

void ECSIServices_ForEachCore(ECSIServicesCoreFunction function, void *data)
{
    SDL_assert(function != NULL);

    for (usz i = 0; i < shlenu(SERVICES.functions); i++)
    {
        if (SERVICES.functions[i].value->plugin == NULL)
        {
            function(SERVICES.functions[i].key, data);
        }
    }
}

const char *ECSIServices_GetDescription(const char *name)
{
    SDL_assert(name != NULL);

    ECSIFunction *function = SERVICES.functions == NULL ? NULL : shget(SERVICES.functions, name);
    return function == NULL ? NULL : function->description;
}

SHUResult ECSIServices_RegisterLuaHandleType(ECSPlugin plugin, const char *name)
{
    SDL_assert(plugin != NULL);
    SDL_assert(name != NULL);

    return ECSIPlugin_OwnsName(plugin, name) ? ECSIServices_RegisterHandleType(plugin, name, ECSIServices_DestroyLuaObject) : SHUResult_ErrBadData;
}

/// @brief Finds a handle type that a Lua plugin provides, and raises a Lua error if it is not the plugin's.
static ECSIHandleType *ECSIServices_CheckLuaHandleType(lua_State *state, ECSPlugin plugin, const char *name)
{
    ECSIHandleType *type = ECSIServices_FindHandleType(name);

    if (type == NULL || type->plugin != plugin || type->Destroy != ECSIServices_DestroyLuaObject)
    {
        luaL_error(state, "handle type '%s' is not one that this plugin registered in Lua", name);
    }

    return type;
}

void ECSIServices_PushLuaHandle(ECSPlugin plugin, const char *name, int index)
{
    SDL_assert(plugin != NULL);
    SDL_assert(name != NULL);

    lua_State *state = ECSILua_GetState();
    index = lua_absindex(state, index);
    ECSIServices_CheckLuaHandleType(state, plugin, name);

    ECSILuaObject *object = SDL_malloc(sizeof(ECSILuaObject));

    if (object == NULL)
    {
        luaL_error(state, "out of memory");
        return;
    }

    lua_pushvalue(state, index);
    object->value = luaL_ref(state, LUA_REGISTRYINDEX);
    ECSIServices_PushHandle(name, object);
}

void ECSIServices_PushLuaHandleValue(ECSPlugin plugin, const char *name, int index)
{
    SDL_assert(plugin != NULL);
    SDL_assert(name != NULL);

    lua_State *state = ECSILua_GetState();
    ECSIServices_CheckLuaHandleType(state, plugin, name);
    ECSILuaObject *object = ECSIServices_CheckHandle(index, name);

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

    if (!ECSIPlugin_OwnsName(plugin, name))
    {
        return SHUResult_ErrBadData;
    }

    return ECSIServices_RegisterHandleType(plugin, name, Destroy);
}

SHUResult ECSService_GetFunction(ECSPlugin plugin, ECSFunction *retFunction, const char *name, const char *signature)
{
    SDL_assert(plugin != NULL);
    SDL_assert(retFunction != NULL);
    SDL_assert(name != NULL);
    SDL_assert(signature != NULL);

    ECSIFunction *function = NULL;
    SHU_ReturnResult(ECSIServices_Find(plugin, name, signature, &function));
    *retFunction = function->pointer;
    return SHUResult_Ok;
}

SHUResult ECSIServices_PushPlugin(ECSPlugin plugin, ECSPlugin provider)
{
    SDL_assert(plugin != NULL);
    SDL_assert(provider != NULL);

    if (!ECSIPlugin_DependsOn(plugin, provider))
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' requires '%s', but its manifest does not depend on it.", ECSIPlugin_GetName(plugin), ECSIPlugin_GetName(provider));
        return SHUResult_ErrPrivileges;
    }

    lua_State *state = ECSILua_GetState();
    lua_newtable(state);

    for (usz i = 0; i < shlenu(SERVICES.functions); i++)
    {
        ECSIFunction *function = SERVICES.functions[i].value;

        if (function->plugin == provider)
        {
            SHU_ReturnResult(ECSIServices_PushFunction(plugin, function->name, NULL), lua_pop(state, 1););
            lua_setfield(state, -2, SDL_strchr(function->name, '.') + 1);
        }
    }

    return SHUResult_Ok;
}

#pragma region Definitions

/// @brief Text that grows as parts are added; a failed addition leaves it NULL, and later additions do nothing.
typedef struct ECSIDefinitionText
{
    char *data;
    bool failed;
} ECSIDefinitionText;

static void ECSIDefinitionText_Add(ECSIDefinitionText *text, SDL_PRINTF_FORMAT_STRING const char *format, ...) SDL_PRINTF_VARARG_FUNC(2);

static void ECSIDefinitionText_Add(ECSIDefinitionText *text, const char *format, ...)
{
    char *part = NULL;
    char *joined = NULL;
    va_list arguments;
    va_start(arguments, format);
    bool ok = !text->failed && SDL_vasprintf(&part, format, arguments) >= 0;
    va_end(arguments);

    ok = ok && SDL_asprintf(&joined, "%s%s", text->data == NULL ? "" : text->data, part) >= 0;
    SDL_free(part);
    SDL_free(text->data);
    text->data = ok ? joined : NULL;
    text->failed = !ok;
}

/// @brief Names a parameter that its signature leaves unnamed: arg1, arg2 and so on.
static const char *ECSIDefinition_Name(ECSIParameter parameter, usz index, char *buffer, usz size)
{
    if (parameter.name != NULL)
    {
        return parameter.name;
    }

    SDL_snprintf(buffer, size, "arg%zu", index + 1);
    return buffer;
}

static void ECSIDefinition_AddLuaType(ECSIDefinitionText *text, ECSIParameter parameter);

/// @brief Adds a callback's type for LuaLS, such as "fun(path: string): integer".
static void ECSIDefinition_AddLuaFunction(ECSIDefinitionText *text, const ECSISignature *signature)
{
    char buffer[32];
    ECSIDefinitionText_Add(text, "fun(");

    for (usz i = 0, written = 0; i < arrlenu(signature->parameters); i++)
    {
        if (!signature->parameters[i].out)
        {
            ECSIDefinitionText_Add(text, "%s%s: ", written++ == 0 ? "" : ", ", ECSIDefinition_Name(signature->parameters[i], i, buffer, sizeof(buffer)));
            ECSIDefinition_AddLuaType(text, signature->parameters[i]);
        }
    }

    ECSIDefinitionText_Add(text, ")");

    // the result comes first, then the outputs
    for (usz i = 0, written = 0; i <= arrlenu(signature->parameters); i++)
    {
        ECSIParameter result = i == 0 ? signature->result : signature->parameters[i - 1];

        if ((i == 0 && result.type != ECSIParameterType_Void) || (i > 0 && result.out))
        {
            ECSIDefinitionText_Add(text, "%s", written++ == 0 ? ": " : ", ");
            ECSIDefinition_AddLuaType(text, result);
        }
    }
}

/// @brief Adds a parameter's type for LuaLS.
static void ECSIDefinition_AddLuaType(ECSIDefinitionText *text, ECSIParameter parameter)
{
    static const char *const types[ECSIParameterType_Count] = {"nil", "boolean", "integer", "integer", "number", "number", "string", "string", "any", "userdata", "function"};

    if (parameter.type == ECSIParameterType_Fn)
    {
        ECSIDefinitionText_Add(text, "(");
        ECSIDefinition_AddLuaFunction(text, parameter.callback);
        ECSIDefinitionText_Add(text, ")?");
    }
    else if (parameter.type == ECSIParameterType_Handle && SDL_strcmp(parameter.handle->name, "ecs.panel") == 0)
    {
        ECSIDefinitionText_Add(text, "ecs.Panel");
    }
    else if (parameter.type == ECSIParameterType_Handle && SDL_strcmp(parameter.handle->name, "ecs.popup") == 0)
    {
        ECSIDefinitionText_Add(text, "ecs.Popup");
    }
    else if (parameter.type == ECSIParameterType_Handle && SDL_strcmp(parameter.handle->name, "ecs.surface") == 0)
    {
        ECSIDefinitionText_Add(text, "ecs.Surface");
    }
    else
    {
        ECSIDefinitionText_Add(text, "%s", types[parameter.type]);
    }
}

/// @brief Adds a parameter's C declaration, such as "f32 x", "const char **retName" or "void (*callback)(i32 arg1)".
static void ECSIDefinition_AddCDeclaration(ECSIDefinitionText *text, ECSIParameter parameter, const char *name);

/// @brief Adds a callback's parameter list in C, such as "(i32 arg1, f32 arg2)".
static void ECSIDefinition_AddCParameters(ECSIDefinitionText *text, const ECSISignature *signature)
{
    char buffer[32];
    ECSIDefinitionText_Add(text, "(");

    for (usz i = 0; i < arrlenu(signature->parameters); i++)
    {
        ECSIDefinitionText_Add(text, "%s", i == 0 ? "" : ", ");
        ECSIDefinition_AddCDeclaration(text, signature->parameters[i], ECSIDefinition_Name(signature->parameters[i], i, buffer, sizeof(buffer)));
    }

    ECSIDefinitionText_Add(text, "%s)", arrlenu(signature->parameters) == 0 ? "void" : "");
}

static void ECSIDefinition_AddCDeclaration(ECSIDefinitionText *text, ECSIParameter parameter, const char *name)
{
    static const char *const types[ECSIParameterType_Count] = {"void", "bool", "i32", "i64", "f32", "f64", "const char *", "SHUSlice ", "const ECSValue *", "void *", ""};
    const char *type = types[parameter.type];

    if (parameter.type == ECSIParameterType_Fn)
    {
        char declarator[64];
        SDL_snprintf(declarator, sizeof(declarator), "(*%s)", name);
        ECSIDefinition_AddCDeclaration(text, parameter.callback->result, declarator);
        ECSIDefinition_AddCParameters(text, parameter.callback);
        return;
    }

    if (parameter.type == ECSIParameterType_Handle)
    {
        type = SDL_strcmp(parameter.handle->name, "ecs.panel") == 0 ? "ECSPanel " : SDL_strcmp(parameter.handle->name, "ecs.popup") == 0 ? "ECSPopup " : SDL_strcmp(parameter.handle->name, "ecs.surface") == 0 ? "ECSSurface *" : "void *";
    }
    else if (parameter.type == ECSIParameterType_Value && parameter.out)
    {
        // an out value is a value that the caller gives and the function fills
        ECSIDefinitionText_Add(text, "ECSValue *%s", name);
        return;
    }

    // a type that ends with a pointer needs no space before the name
    usz length = SDL_strlen(type);
    bool pointer = length > 0 && type[length - 1] == '*';
    bool spaced = length > 0 && type[length - 1] == ' ';
    ECSIDefinitionText_Add(text, "%s%s%s%s", type, pointer || spaced ? "" : " ", parameter.out ? "*" : "", name);
}

/// @brief Writes a text file.
static SHUResult ECSIDefinition_Save(const char *folder, const char *name, const char *extension, const ECSIDefinitionText *text)
{
    char *path = NULL;

    if (text->failed || SDL_asprintf(&path, "%s%s%s", folder, name, extension) < 0)
    {
        return SHUResult_ErrAllocation;
    }

    bool saved = SDL_SaveFile(path, text->data, SDL_strlen(text->data));

    if (!saved)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot write '%s': %s", path, SDL_GetError());
    }

    SDL_free(path);
    return saved ? SHUResult_Ok : SHUResult_ErrFile;
}

/// @brief Turns a plugin's name into the start of C names: "sketch_c" becomes "SketchC".
static void ECSIDefinition_Pascal(const char *name, char *buffer, usz size)
{
    usz length = 0;
    bool upper = true;

    for (const char *c = name; *c != '\0' && length + 1 < size; c++)
    {
        if (!SDL_isalnum((unsigned char)*c))
        {
            upper = true;
            continue;
        }

        buffer[length++] = upper ? (char)SDL_toupper((unsigned char)*c) : *c;
        upper = false;
    }

    buffer[length] = '\0';
}

/// @brief Turns a function's local name into a C member name: dots become underscores.
static void ECSIDefinition_Member(const char *local, char *buffer, usz size)
{
    SDL_strlcpy(buffer, local, size);

    for (char *c = buffer; *c != '\0'; c++)
    {
        *c = *c == '.' ? '_' : *c;
    }
}

#pragma endregion Definitions

SHUResult ECSIServices_WriteDefinitions(ECSPlugin provider, const char *folder)
{
    SDL_assert(provider != NULL);
    SDL_assert(folder != NULL);

    const char *plugin = ECSIPlugin_GetName(provider);
    char pascal[128];
    char buffer[32];
    char member[256];
    ECSIDefinitionText lua = {0};
    ECSIDefinitionText c = {0};
    ECSIDefinition_Pascal(plugin, pascal, sizeof(pascal));

    ECSIDefinitionText_Add(&lua, "---@meta %s\n-- The functions of the plugin %s, for editors. OpenECS --definitions wrote this file.\n-- A Lua plugin whose manifest depends on %s gets them with require(\"%s\").\n\n---@class %s\nlocal %s = {}\n", plugin, plugin, plugin, plugin, plugin, pascal);
    ECSIDefinitionText_Add(&c, "// The functions of the plugin %s, for plugins in C. OpenECS --definitions wrote this file.\n#pragma once\n\n#include \"OpenECS.h\"\n\n/// @brief The functions of the plugin %s. %sFunctions_Get fills them.\ntypedef struct %sFunctions\n{\n", plugin, plugin, pascal, pascal);

    for (usz i = 0; i < shlenu(SERVICES.functions); i++)
    {
        const ECSIFunction *function = SERVICES.functions[i].value;
        const ECSISignature *signature = &function->signature;
        const char *local = SDL_strchr(function->name, '.') + 1;

        if (function->plugin != provider)
        {
            continue;
        }

        // Lua: the description, the parameters, the result and the outputs, then the function
        ECSIDefinitionText_Add(&lua, "\n---%s\n", function->description);

        for (usz j = 0; j < arrlenu(signature->parameters); j++)
        {
            if (!signature->parameters[j].out)
            {
                ECSIDefinitionText_Add(&lua, "---@param %s ", ECSIDefinition_Name(signature->parameters[j], j, buffer, sizeof(buffer)));
                ECSIDefinition_AddLuaType(&lua, signature->parameters[j]);
                ECSIDefinitionText_Add(&lua, "\n");
            }
        }

        for (usz j = 0; j <= arrlenu(signature->parameters); j++)
        {
            ECSIParameter result = j == 0 ? signature->result : signature->parameters[j - 1];

            if ((j == 0 && result.type != ECSIParameterType_Void) || (j > 0 && result.out))
            {
                ECSIDefinitionText_Add(&lua, "---@return ");
                ECSIDefinition_AddLuaType(&lua, result);
                ECSIDefinitionText_Add(&lua, "%s%s\n", result.name == NULL ? "" : " ", result.name == NULL ? "" : result.name);
            }
        }

        // a local name with a dot is not a Lua name, so it is written as a key
        if (SDL_strchr(local, '.') == NULL)
        {
            ECSIDefinitionText_Add(&lua, "function %s.%s(", pascal, local);
        }
        else
        {
            ECSIDefinitionText_Add(&lua, "%s[\"%s\"] = function(", pascal, local);
        }

        for (usz j = 0, written = 0; j < arrlenu(signature->parameters); j++)
        {
            if (!signature->parameters[j].out)
            {
                ECSIDefinitionText_Add(&lua, "%s%s", written++ == 0 ? "" : ", ", ECSIDefinition_Name(signature->parameters[j], j, buffer, sizeof(buffer)));
            }
        }

        ECSIDefinitionText_Add(&lua, ") end\n");

        // C: a member that points to the function
        char declarator[300];
        ECSIDefinition_Member(local, member, sizeof(member));
        SDL_snprintf(declarator, sizeof(declarator), "(*%s)", member);
        ECSIDefinitionText_Add(&c, "    /// @brief %s\n    ", function->description);
        ECSIDefinition_AddCDeclaration(&c, signature->result, declarator);
        ECSIDefinition_AddCParameters(&c, signature);
        ECSIDefinitionText_Add(&c, ";\n");
    }

    ECSIDefinitionText_Add(&lua, "\nreturn %s\n", pascal);
    ECSIDefinitionText_Add(&c, "} %sFunctions;\n\n/// @brief Looks up every function of the plugin %s. Call it from ECSPlugin_Init; the plugin's manifest must depend on %s.\n/// @return SHUResult_Ok, or the error of the first function that cannot be looked up.\nstatic inline SHUResult %sFunctions_Get(ECSPlugin plugin, %sFunctions *retFunctions)\n{\n    ECSFunction function = NULL;\n", pascal, plugin, plugin, pascal, pascal);

    for (usz i = 0; i < shlenu(SERVICES.functions); i++)
    {
        const ECSIFunction *function = SERVICES.functions[i].value;

        if (function->plugin == provider)
        {
            ECSIDefinition_Member(SDL_strchr(function->name, '.') + 1, member, sizeof(member));
            ECSIDefinitionText_Add(&c, "\n    SHU_ReturnResult(ECSService_GetFunction(plugin, &function, \"%s\", \"%s\"));\n    retFunctions->%s = (typeof(retFunctions->%s))function;\n", function->name, function->signature.text, member, member);
        }
    }

    ECSIDefinitionText_Add(&c, "\n    return SHUResult_Ok;\n}\n");

    SHUResult result = ECSIDefinition_Save(folder, plugin, ".lua", &lua);
    result = result ? result : ECSIDefinition_Save(folder, plugin, ".h", &c);
    SDL_free(lua.data);
    SDL_free(c.data);
    return result;
}
