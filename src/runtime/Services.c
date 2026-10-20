#include "runtime/Services.h"

#include "base/Lua.h"
#include "runtime/Settings.h"

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
    ECSIParameterType_Color,
    ECSIParameterType_String,
    ECSIParameterType_Buffer,
    ECSIParameterType_Value,
    ECSIParameterType_Handle,
    ECSIParameterType_Fn,
    ECSIParameterType_Count,
} ECSIParameterType;

/// @brief Names of the parameter types in signatures.
static const char *const OPENECS_PARAMETER_TYPE_NAMES[ECSIParameterType_Count] = {"void", "bool", "int", "int64", "float", "double", "color", "string", "buffer", "value", "handle", "fn"};

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
    ECSPlugin plugin; // nullptr for the core
    ECSHandleDestroyFunction Destroy;
    char **methods;                     // stb_ds array: the names of its methods in Lua
    const struct ECSIFunction **targets; // stb_ds array: the function each method calls, in the same order
} ECSIHandleType;

/// @brief A Lua handle: the userdata that stands for an object.
typedef struct ECSIHandle
{
    void *object; // nullptr when the object is gone
    ECSIHandleType *type;
} ECSIHandle;

/// @brief An object that Lua has handles of, with every one of them that is not collected yet.
/// A handle the garbage collector is about to finalize has left the weak table already, so the object can get a new handle meanwhile; the object is destroyed when its last handle is collected (10.6).
typedef struct ECSIHandleObject
{
    ECSIHandleType *type;
    ECSIHandle **handles; // stb_ds array; a userdata's memory does not move
} ECSIHandleObject;

/// @brief The object of a handle whose type a Lua plugin provides: the Lua value it stands for.
typedef struct ECSILuaObject
{
    int value; // registry reference
} ECSILuaObject;

/// @brief A parameter or result of a signature.
typedef struct ECSIParameter
{
    ECSIParameterType type;
    char *name;                        // the name the signature gives it, or nullptr; only for documentation
    bool out;                          // an output parameter: a pointer in C, an extra result in Lua
    bool keep;                         // a callback that stays valid after the call, until the function that took it releases it
    ECSIHandleType *handle;           // handles: their type
    struct ECSISignature *callback;   // callbacks: their signature, owned by the parameter
    char *luaType;                     // values: the Lua type that value<...> names for editors, or nullptr; only for documentation, as names are
    char *doc;                         // what the parameter or result is, from the function's description, or nullptr; only for documentation
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
    struct
    {
        ECSPlugin key;
        int value;
    } *callers;           // stb_ds hash map: for each plugin, the registry reference of the Lua function that calls it for that plugin, made when the plugin first asks for it
    ECSPlugin giver;      // a callback: the plugin that gave the Lua function it calls, or nullptr
    bool inert;           // a callback whose giver was removed: C may still call it, and nothing runs
    int lua;              // registry reference of the Lua function, or LUA_NOREF for a C function
    int anchors;          // registry reference of a table that keeps the strings and buffers a Lua function last gave C
    ECSValue *result;     // the value a Lua function last returned to C, or nullptr
    ffi_closure *closure; // the code that C calls for a Lua function, or nullptr
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
    ECSColor color;
    const char *string;
    SHUSlice buffer;
    void *pointer; // values and outputs
} ECSISlot;

static_assert(sizeof(usz) == sizeof(u64), "a buffer's size is described to libffi as 64 bits");

/// @brief The libffi description of SHUSlice, which buffers are.
static ffi_type *OPENECS_SLICE_ELEMENTS[] = {&ffi_type_pointer, &ffi_type_uint64, nullptr};
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
    struct
    {
        void *key; // the object
        ECSIHandleObject *value;
    } *objects; // stb_ds hash map of the objects that Lua has handles of
    struct
    {
        void *key; // the code that C calls
        ECSIFunction *value;
    } *kept; // stb_ds hash map of the closures made for Lua functions that C keeps, until ECSService_ReleaseCallback
    struct
    {
        ECSSurface *surface;
        ECSSurfaceDrawnFunction Drawn;
        void *data;
    } *drawn; // stb_ds array of the functions that run once the Draw of their surface returns, in the order they were given
} SERVICES = {0};

static ECSIHandleType *ECSIServices_FindHandleType(const char *name)
{
    // shget would allocate a map that is missing
    return SERVICES.handleTypes == nullptr ? nullptr : shget(SERVICES.handleTypes, name);
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
    case ECSIParameterType_Color:
        return &ffi_type_uint32;
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
    parameter->name = nullptr;
    SDL_free(parameter->luaType);
    parameter->luaType = nullptr;
    SDL_free(parameter->doc);
    parameter->doc = nullptr;

    if (parameter->callback != nullptr)
    {
        ECSISignature_Free(parameter->callback);
        SDL_free(parameter->callback);
        parameter->callback = nullptr;
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

    if (**text != '<' || end == nullptr || end == start)
    {
        return false;
    }

    char *name = SDL_strndup(start, (usz)(end - start));
    retParameter->handle = name == nullptr ? nullptr : ECSIServices_FindHandleType(name);

    if (name != nullptr && retParameter->handle == nullptr)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Handle type '%s' is not registered.", name);
    }

    SDL_free(name);
    *text = end + 1;
    ECSISignature_SkipSpaces(text);
    return retParameter->handle != nullptr;
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

    char *inner = *end == '>' ? SDL_strndup(start, (usz)(end - start)) : nullptr;
    retParameter->callback = inner == nullptr ? nullptr : SDL_calloc(1, sizeof(ECSISignature));
    bool valid = retParameter->callback != nullptr && ECSISignature_Parse(inner, retParameter->callback) == SHUResult_Ok;

    if (!valid)
    {
        SDL_free(retParameter->callback);
        retParameter->callback = nullptr;
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

/// @brief Reads what a value<...> names: the Lua type of the value, such as ui.Props, for the definitions only.
static bool ECSISignature_ReadLuaType(const char **text, ECSIParameter *retParameter)
{
    if (**text != '<')
    {
        return true;
    }

    const char *start = *text + 1;
    const char *end = SDL_strchr(start, '>');

    if (end == nullptr || end == start)
    {
        return false;
    }

    retParameter->luaType = SDL_strndup(start, (usz)(end - start));
    *text = end + 1;
    ECSISignature_SkipSpaces(text);
    return retParameter->luaType != nullptr;
}

/// @brief Reads a parameter or result of a signature: a type name, after "out" for an output parameter.
static bool ECSISignature_ReadParameter(const char **text, ECSIParameter *retParameter)
{
    const char *word = nullptr;
    usz length = ECSISignature_ReadWord(text, &word);
    retParameter->out = length == 3 && SDL_strncmp(word, "out", 3) == 0;
    retParameter->keep = length == 4 && SDL_strncmp(word, "keep", 4) == 0;

    if (retParameter->out || retParameter->keep)
    {
        length = ECSISignature_ReadWord(text, &word);
    }

    for (i32 type = 0; type < ECSIParameterType_Count; type++)
    {
        const char *name = OPENECS_PARAMETER_TYPE_NAMES[type];

        if (length == SDL_strlen(name) && SDL_strncmp(word, name, length) == 0)
        {
            retParameter->type = (ECSIParameterType)type;

            // only a callback is kept
            if (retParameter->keep && type != ECSIParameterType_Fn)
            {
                return false;
            }

            switch (type)
            {
            case ECSIParameterType_Handle:
                return ECSISignature_ReadHandle(text, retParameter);
            case ECSIParameterType_Fn:
                return ECSISignature_ReadCallback(text, retParameter);
            case ECSIParameterType_Value:
                return ECSISignature_ReadLuaType(text, retParameter);
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
    char *next = nullptr;

    // a handle names its type, and a callback its signature, between angle brackets
    const char *inner = parameter.type == ECSIParameterType_Handle ? parameter.handle->name : parameter.type == ECSIParameterType_Fn ? parameter.callback->text
                                                                                                                                       : nullptr;

    if (SDL_asprintf(&next, "%s%s%s%s%s%s%s", *text == nullptr ? "" : *text, separator, parameter.out ? "out " : parameter.keep ? "keep " : "", OPENECS_PARAMETER_TYPE_NAMES[parameter.type], inner == nullptr ? "" : "<", inner == nullptr ? "" : inner, inner == nullptr ? "" : ">") < 0)
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
            const char *word = nullptr;
            usz length = ECSISignature_ReadWord(&cursor, &word);
            parameter.name = SDL_strndup(word, length);
            valid = parameter.name != nullptr;
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

    // the closing parenthesis was written before a "void"; that word is cut off again, and without parameters the parenthesis opens too, after the result with its type
    if (!result)
    {
        retSignature->text[SDL_strlen(retSignature->text) - SDL_strlen("void")] = '\0';
    }

    if (!result && count == 0)
    {
        char *written = retSignature->text;
        retSignature->text = nullptr;
        result = SDL_asprintf(&retSignature->text, "%.*s()", (int)(SDL_strlen(written) - 1), written) < 0 ? SHUResult_ErrAllocation : SHUResult_Ok;
        SDL_free(written);
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
    for (usz i = 0; i < hmlenu(function->callers); i++)
    {
        luaL_unref(ECSILua_GetState(), LUA_REGISTRYINDEX, function->callers[i].value);
    }

    hmfree(function->callers);

    if (function->closure != nullptr)
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
    ECSIFunction *function = SERVICES.functions == nullptr ? nullptr : shget(SERVICES.functions, name);

    if (function == nullptr)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' looks up '%s', which is not registered.", ECSIPlugin_GetName(plugin), name);
        return SHUResult_ErrNotFound;
    }

    // every plugin may use the core's functions
    if (function->plugin != nullptr && !ECSIPlugin_DependsOn(plugin, function->plugin))
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' looks up '%s', but its manifest does not depend on '%s'.", ECSIPlugin_GetName(plugin), name, ECSIPlugin_GetName(function->plugin));
        return SHUResult_ErrPrivileges;
    }

    if (signature != nullptr)
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

static SHUResult ECSIServices_MakeCallback(lua_State *state, int index, const ECSIFunction *service, ECSPlugin giver, const ECSISignature *signature, ECSIFunction **retCallback);

static void ECSIServices_FreeCallback(ECSIFunction **callback);

#pragma region Lua Calls C

/// @brief Pushes a colour for Lua, as "#RRGGBBAA".
static void ECSIServices_PushColor(lua_State *state, ECSColor color)
{
    char text[OPENECS_COLOR_TEXT_SIZE];
    ECSIColor_Format(color, text);
    lua_pushstring(state, text);
}

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
    case ECSIParameterType_Color:
        luaL_argcheck(state, ECSColor_Parse(luaL_checkstring(state, index), &slot->color), index, "is not a colour: \"#RRGGBB\", \"#RRGGBBAA\" or a colour of the theme, such as \"text\"");
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

        slot->pointer = nullptr;
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
    case ECSIParameterType_Color:
        ECSIServices_PushColor(state, result ? (ECSColor)slot->integral : slot->color);
        break;
    case ECSIParameterType_String:
        lua_pushstring(state, slot->string);
        break;
    case ECSIParameterType_Buffer:
        lua_pushlstring(state, slot->buffer.data == nullptr ? "" : slot->buffer.data, slot->buffer.data == nullptr ? 0 : slot->buffer.size);
        break;
    case ECSIParameterType_Value:
        ECSILua_PushValue(slot->pointer);
        break;
    default:
        lua_pushnil(state);
        break;
    }
}

/// @brief Calls a C function from Lua through libffi. The function is the first upvalue: a registered function, or a callback that C gave Lua. The second, for a registered function, is the plugin that calls it. Output parameters become extra results.
static int ECSIServices_CallC(lua_State *state)
{
    const ECSIFunction *function = lua_touserdata(state, lua_upvalueindex(1));
    ECSPlugin caller = lua_touserdata(state, lua_upvalueindex(2));
    const ECSISignature *signature = &function->signature;
    usz count = arrlenu(signature->parameters);

    if (function->pointer == nullptr)
    {
        return luaL_error(state, "a callback given to '%s' is valid only during the call that gave it", function->name);
    }

    // a missing argument is nil; the stack holds one for each, so the storage below never stands where an argument is read
    int inputs = 0;

    for (usz i = 0; i < count; i++)
    {
        inputs += signature->parameters[i].out ? 0 : 1;
    }

    if (lua_gettop(state) < inputs)
    {
        lua_settop(state, inputs);
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
        callbacks[i] = nullptr;

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

        ECSValue *value = nullptr;
        result = result ? result : ECSValue_Create(&value);
        result = result || parameter.out ? result : ECSILua_GetValue(indices[i], value);
        slots[i].pointer = value;

        if (parameter.out)
        {
            outputs[i].pointer = value;
        }
    }

    // a Lua function becomes a closure that C calls; it is valid until this call returns, or until C releases it if C keeps it
    for (usz i = 0; !result && i < count; i++)
    {
        if (signature->parameters[i].type == ECSIParameterType_Fn && !lua_isnoneornil(state, indices[i]))
        {
            result = ECSIServices_MakeCallback(state, indices[i], function, caller, signature->parameters[i].callback, &callbacks[i]);

            // C gets the closure's code; POSIX lets a data pointer hold code
            if (!result)
            {
                SDL_memcpy(&slots[i].pointer, &callbacks[i]->pointer, sizeof(slots[i].pointer));
            }

            // a kept closure is the map's from now on, so C may release it during the call
            if (!result && signature->parameters[i].keep)
            {
                hmput(SERVICES.kept, slots[i].pointer, callbacks[i]);
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

        // a kept closure that the call got is C's now, released or not; when the call did not run, nothing can release it
        if (callbacks[i] != nullptr && signature->parameters[i].keep)
        {
            if (result)
            {
                (void)hmdel(SERVICES.kept, slots[i].pointer);
                ECSIServices_FreeCallback(&callbacks[i]);
            }
        }
        else if (callbacks[i] != nullptr)
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

/// @brief Makes every handle of an object invalid and forgets the object; the handles' userdata stay until they are collected.
/// @return The object's type, or nullptr if Lua had no handle of it.
static ECSIHandleType *ECSIServices_DropObject(void *object)
{
    ECSIHandleObject *record = SERVICES.objects == nullptr ? nullptr : hmget(SERVICES.objects, object);

    if (record == nullptr)
    {
        return nullptr;
    }

    for (usz i = 0; i < arrlenu(record->handles); i++)
    {
        record->handles[i]->object = nullptr;
    }

    ECSIHandleType *type = record->type;
    (void)hmdel(SERVICES.objects, object);
    arrfree(record->handles);
    SDL_free(record);

    // the weak table may still map the object to a handle that is not collected yet
    lua_State *state = ECSILua_GetState();
    lua_rawgeti(state, LUA_REGISTRYINDEX, SERVICES.handles);
    lua_pushnil(state);
    lua_rawsetp(state, -2, object);
    lua_pop(state, 1);
    return type;
}

/// @brief Lets go of a Lua handle that the garbage collector frees; the object is destroyed with its last handle.
static int ECSIServices_HandleCollect(lua_State *state)
{
    ECSIHandle *handle = lua_touserdata(state, 1);
    void *object = handle->object;
    ECSIHandleObject *record = object == nullptr || SERVICES.objects == nullptr ? nullptr : hmget(SERVICES.objects, object);
    handle->object = nullptr;

    if (record == nullptr)
    {
        return 0;
    }

    for (usz i = 0; i < arrlenu(record->handles); i++)
    {
        if (record->handles[i] == handle)
        {
            arrdelswap(record->handles, i);
            break;
        }
    }

    // another handle of the object, made while this one waited for its finalizer, still stands for it
    if (arrlenu(record->handles) > 0)
    {
        return 0;
    }

    ECSIHandleType *type = ECSIServices_DropObject(object);

    if (type != nullptr && type->Destroy != nullptr)
    {
        type->Destroy(object);
    }

    return 0;
}

static int ECSIServices_HandleText(lua_State *state)
{
    ECSIHandle *handle = lua_touserdata(state, 1);
    lua_pushfstring(state, "handle<%s>%s", handle->type->name, handle->object == nullptr ? " (gone)" : "");
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
    if (ECSIServices_FindHandleType(name) != nullptr)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Handle type '%s' is already registered.", name);
        return SHUResult_ErrBadData;
    }

    ECSIHandleType *type = SDL_calloc(1, sizeof(ECSIHandleType));

    if (type == nullptr || (type->name = SDL_strdup(name)) == nullptr || SDL_asprintf(&type->metatable, "%s%s", OPENECS_HANDLE_METATABLE, name) < 0)
    {
        if (type != nullptr)
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

/// @brief Makes every Lua handle of a type invalid; with destroy, their objects are destroyed first, each once. nullptr is every type.
static void ECSIServices_ForgetHandles(const ECSIHandleType *type, bool destroy)
{
    // a destructor may forget other objects, so the objects are listed first
    void **objects = nullptr;

    for (usz i = 0; i < hmlenu(SERVICES.objects); i++)
    {
        if (type == nullptr || SERVICES.objects[i].value->type == type)
        {
            arrput(objects, SERVICES.objects[i].key);
        }
    }

    for (usz i = 0; i < arrlenu(objects); i++)
    {
        ECSIHandleType *objectType = ECSIServices_DropObject(objects[i]);

        if (destroy && objectType != nullptr && objectType->Destroy != nullptr)
        {
            objectType->Destroy(objects[i]);
        }
    }

    arrfree(objects);
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
    case ECSIParameterType_Color:
        ECSIServices_PushColor(state, *(const ECSColor *)argument);
        break;
    case ECSIParameterType_String:
        lua_pushstring(state, *(const char *const *)argument);
        break;
    case ECSIParameterType_Buffer:
    {
        const SHUSlice *buffer = argument;
        lua_pushlstring(state, buffer->data == nullptr ? "" : buffer->data, buffer->data == nullptr ? 0 : buffer->size);
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
        *(void **)target = handle == nullptr ? nullptr : handle->object;
        lua_pushvalue(state, index);
        lua_rawseti(state, anchors, (lua_Integer)lua_rawlen(state, anchors) + 1);
        return handle != nullptr || lua_isnil(state, index);
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
    case ECSIParameterType_Color:
    {
        ECSColor color = 0;
        bool valid = lua_type(state, index) == LUA_TSTRING && ECSColor_Parse(lua_tostring(state, index), &color);

        if (result)
        {
            *(ffi_arg *)target = (ffi_arg)color;
        }
        else
        {
            *(ECSColor *)target = color;
        }

        return valid;
    }
    case ECSIParameterType_String:
    case ECSIParameterType_Buffer:
    {
        bool text = lua_type(state, index) == LUA_TSTRING;
        usz length = 0;
        const char *bytes = text ? lua_tolstring(state, index, &length) : nullptr;

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

        return value != nullptr && ECSILua_GetValue(index, value) == SHUResult_Ok;
    }
    default:
        return true;
    }
}

/// @brief Reports that a Lua function gave a value of the wrong type.
static void ECSIServices_ReportType(lua_State *state, const ECSIFunction *function, int index)
{
    char *message = nullptr;

    if (SDL_asprintf(&message, "'%s' gave a %s where its signature %s needs another type.", function->name, luaL_typename(state, index), function->signature.text) >= 0)
    {
        ECSIPlugin_ReportError(function->plugin, message);
        SDL_free(message);
    }
}

/// @brief Pushes a callback that C passed to a Lua function: a Lua function that calls it, or nil for nullptr. Unless it is kept, it is added to the table of callbacks at an index of the stack, which makes it invalid when the call returns.
static void ECSIServices_PushCallback(lua_State *state, const ECSIFunction *service, ECSIParameter parameter, const void *argument, int callbacks)
{
    ECSFunction pointer = nullptr;
    SDL_memcpy(&pointer, argument, sizeof(pointer));

    if (pointer == nullptr)
    {
        lua_pushnil(state);
        return;
    }

    // the record borrows the service's name and the callback's signature, which live as long as the service; one that is not kept is never used after the call
    ECSIFunction *callback = lua_newuserdatauv(state, sizeof(ECSIFunction), 0);
    *callback = (ECSIFunction){.name = service->name, .plugin = service->plugin, .signature = *parameter.callback, .pointer = pointer, .lua = LUA_NOREF, .anchors = LUA_NOREF};

    if (!parameter.keep)
    {
        lua_pushvalue(state, -1);
        lua_rawseti(state, callbacks, (lua_Integer)lua_rawlen(state, callbacks) + 1);
    }

    lua_pushcclosure(state, ECSIServices_CallC, 1);
}

/// @brief Makes the callbacks that C passed to a Lua function invalid, now that the call returned.
static void ECSIServices_EndCallbacks(lua_State *state, int callbacks)
{
    for (lua_Integer i = 1; i <= (lua_Integer)lua_rawlen(state, callbacks); i++)
    {
        lua_rawgeti(state, callbacks, i);
        ((ECSIFunction *)lua_touserdata(state, -1))->pointer = nullptr;
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

    // the plugin that gave the Lua function was removed, so its code never runs again
    if (function->inert)
    {
        return;
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
/// @param giver The plugin whose Lua code gives the function, or nullptr.
/// @param signature The callback's signature. The record borrows it.
static SHUResult ECSIServices_MakeCallback(lua_State *state, int index, const ECSIFunction *service, ECSPlugin giver, const ECSISignature *signature, ECSIFunction **retCallback)
{
    ECSIFunction *callback = SDL_calloc(1, sizeof(ECSIFunction));

    if (callback == nullptr || SDL_asprintf(&callback->name, "a callback given to '%s'", service->name) < 0)
    {
        SDL_free(callback);
        return SHUResult_ErrAllocation;
    }

    callback->plugin = service->plugin;
    callback->giver = giver;
    callback->signature = *signature;

    void *code = nullptr;
    callback->closure = ffi_closure_alloc(sizeof(ffi_closure), &code);

    if (callback->closure == nullptr || ffi_prep_closure_loc(callback->closure, &callback->signature.cif, ECSIServices_CallLua, callback, code) != FFI_OK)
    {
        if (callback->closure != nullptr)
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
    *callback = nullptr;
}

#pragma endregion C Calls Lua

/// @brief Reads the lines of a function's description after its first: "@param name text" says what a parameter is, and "@return text" what the
/// function gives. The first line stays the description, which menus show. A line that names no parameter of the signature is reported and skipped.
static SHUResult ECSIServices_ReadDocs(ECSIFunction *function)
{
    char *line = SDL_strchr(function->description, '\n');

    if (line == nullptr)
    {
        return SHUResult_Ok;
    }

    *line++ = '\0';

    while (line != nullptr)
    {
        char *next = SDL_strchr(line, '\n');

        if (next != nullptr)
        {
            *next++ = '\0';
        }

        ECSIParameter *target = nullptr;
        const char *text = nullptr;

        if (SDL_strncmp(line, "@return ", 8) == 0)
        {
            target = &function->signature.result;
            text = line + 8;
        }
        else if (SDL_strncmp(line, "@param ", 7) == 0)
        {
            const char *name = line + 7;
            const char *space = SDL_strchr(name, ' ');
            usz length = space == nullptr ? SDL_strlen(name) : (usz)(space - name);
            text = name[length] == ' ' ? name + length + 1 : name + length;

            for (usz i = 0; i < arrlenu(function->signature.parameters) && target == nullptr; i++)
            {
                const char *given = function->signature.parameters[i].name;
                target = given != nullptr && SDL_strlen(given) == length && SDL_strncmp(given, name, length) == 0 ? &function->signature.parameters[i] : nullptr;
            }
        }

        if (target == nullptr || target->doc != nullptr || text[0] == '\0')
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "The description of '%s' has a line that is not \"@param name text\" of a parameter it names once, or \"@return text\" once: '%s'", function->name, line);
        }
        else
        {
            target->doc = SDL_strdup(text);

            if (target->doc == nullptr)
            {
                return SHUResult_ErrAllocation;
            }
        }

        line = next;
    }

    return SHUResult_Ok;
}

/// @brief Checks a function's name and makes its record, without its code.
static SHUResult ECSIServices_Create(ECSPlugin plugin, const char *name, const char *signature, const char *description, ECSIFunction **retFunction)
{
    // the core's own names start with "ecs."
    if (plugin == nullptr ? SDL_strncmp(name, "ecs.", 4) != 0 : !ECSIPlugin_OwnsName(plugin, name))
    {
        return SHUResult_ErrBadData;
    }

    if (SERVICES.functions != nullptr && shget(SERVICES.functions, name) != nullptr)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Function '%s' is already registered.", name);
        return SHUResult_ErrBadData;
    }

    ECSIFunction *function = SDL_calloc(1, sizeof(ECSIFunction));

    if (function == nullptr)
    {
        return SHUResult_ErrAllocation;
    }

    function->name = SDL_strdup(name);
    function->description = SDL_strdup(description == nullptr ? "" : description);
    function->plugin = plugin;
    function->lua = LUA_NOREF;
    function->anchors = LUA_NOREF;

    SHUResult result = function->name == nullptr || function->description == nullptr ? SHUResult_ErrAllocation : ECSISignature_Parse(signature, &function->signature);

    if (!result && (result = ECSIServices_ReadDocs(function)))
    {
        ECSISignature_Free(&function->signature);
    }

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

static bool ECSIServices_ReadHandle(int index, const char **retType, void **retObject);

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
    ECSILua_SetHandleFunctions(ECSIServices_ReadHandle, ECSIServices_PushHandle);

    // the core's handle types: panels, popups, and the surface a panel or popup draws into, valid during Draw
    SHU_ReturnResult(ECSIServices_RegisterHandleType(nullptr, "ecs.panel", nullptr));
    SHU_ReturnResult(ECSIServices_RegisterHandleType(nullptr, "ecs.popup", nullptr));
    return ECSIServices_RegisterHandleType(nullptr, "ecs.surface", nullptr);
}

/// @brief Reads a Lua handle for a value: its type's name and its object.
static bool ECSIServices_ReadHandle(int index, const char **retType, void **retObject)
{
    lua_State *state = ECSILua_GetState();

    // every handle's metatable is named after its type, with a prefix of its own
    if (!lua_getmetatable(state, index))
    {
        return false;
    }

    lua_getfield(state, -1, "__name");
    const char *name = lua_tostring(state, -1);
    bool handle = name != nullptr && SDL_strncmp(name, OPENECS_HANDLE_METATABLE, SDL_strlen(OPENECS_HANDLE_METATABLE)) == 0;
    lua_pop(state, 2);

    ECSIHandle *found = handle ? lua_touserdata(state, index) : nullptr;

    if (found == nullptr || found->object == nullptr)
    {
        return false;
    }

    *retType = found->type->name;
    *retObject = found->object;
    return true;
}

void ECSIServices_PushHandle(const char *type, void *object)
{
    SDL_assert(type != nullptr);

    lua_State *state = ECSILua_GetState();

    if (object == nullptr)
    {
        lua_pushnil(state);
        return;
    }

    // the type is gone when its plugin was removed; a handle cannot be made without it
    ECSIHandleType *handleType = ECSIServices_FindHandleType(type);

    if (handleType == nullptr)
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

    // an object at the address of a gone object of another type is another object
    ECSIHandleObject *record = SERVICES.objects == nullptr ? nullptr : hmget(SERVICES.objects, object);

    if (record != nullptr && record->type != handleType)
    {
        ECSIServices_DropObject(object);
        record = nullptr;
    }

    if (record == nullptr)
    {
        record = SDL_calloc(1, sizeof(ECSIHandleObject));

        if (record == nullptr)
        {
            lua_pop(state, 1);
            lua_pushnil(state);
            return;
        }

        record->type = handleType;
        hmput(SERVICES.objects, object, record);
    }

    ECSIHandle *handle = lua_newuserdatauv(state, sizeof(ECSIHandle), 0);
    *handle = (ECSIHandle){.object = object, .type = handleType};
    arrput(record->handles, handle);
    luaL_setmetatable(state, handleType->metatable);
    lua_pushvalue(state, -1);
    lua_rawsetp(state, -3, object);
    lua_remove(state, -2);
}

void *ECSIServices_CheckHandle(int index, const char *type)
{
    SDL_assert(type != nullptr);

    lua_State *state = ECSILua_GetState();
    ECSIHandleType *handleType = ECSIServices_FindHandleType(type);

    if (handleType == nullptr)
    {
        luaL_error(state, "handle type '%s' is gone; its plugin was removed", type);
        return nullptr;
    }

    ECSIHandle *handle = luaL_checkudata(state, index, handleType->metatable);

    if (handle->object == nullptr)
    {
        luaL_argerror(state, index, "the object of this handle is gone");
    }

    return handle->object;
}

void ECSIServices_ForgetHandle(void *object)
{
    SDL_assert(object != nullptr);

    ECSIServices_DropObject(object);
}

void ECSIServices_EndSurface(ECSSurface *surface)
{
    SDL_assert(surface != nullptr);

    // the functions run in order, and one may give another, which runs too
    for (usz i = 0; i < arrlenu(SERVICES.drawn);)
    {
        if (SERVICES.drawn[i].surface != surface)
        {
            i++;
            continue;
        }

        ECSSurfaceDrawnFunction Drawn = SERVICES.drawn[i].Drawn;
        void *data = SERVICES.drawn[i].data;
        arrdel(SERVICES.drawn, i);
        Drawn(surface, data);
    }

    // the surface is valid only during Draw, so a Lua handle of it is too
    ECSIServices_ForgetHandle(surface);
}

SHUResult ECSSurface_OnDrawn(ECSSurface *surface, ECSSurfaceDrawnFunction Drawn, void *data)
{
    SDL_assert(surface != nullptr);
    SDL_assert(Drawn != nullptr);

    usz count = arrlenu(SERVICES.drawn);
    arrput(SERVICES.drawn, ((typeof(*SERVICES.drawn)){.surface = surface, .Drawn = Drawn, .data = data}));
    return arrlenu(SERVICES.drawn) > count ? SHUResult_Ok : SHUResult_ErrAllocation;
}

void ECSIServices_PushHandleMetatable(const char *type)
{
    ECSIHandleType *handleType = ECSIServices_FindHandleType(type);

    if (handleType == nullptr)
    {
        lua_pushnil(ECSILua_GetState());
        return;
    }

    luaL_getmetatable(ECSILua_GetState(), handleType->metatable);
}

void ECSIServices_DestroyHandles(void)
{
    // handles that wait for their finalizer are no longer in the weak table, so a full collection runs those finalizers now, while the types and their plugins' code exist
    lua_gc(ECSILua_GetState(), LUA_GCCOLLECT);

    // objects are destroyed while their plugins' code is still loaded
    ECSIServices_ForgetHandles(nullptr, true);
}

void ECSIServices_Terminate(void)
{
    // handles that plugins made while they shut down are only forgotten, as their plugins' state is gone
    ECSIServices_ForgetHandles(nullptr, false);

    for (usz i = 0; i < shlenu(SERVICES.handleTypes); i++)
    {
        SDL_free(SERVICES.handleTypes[i].value->name);
        SDL_free(SERVICES.handleTypes[i].value->metatable);

        for (usz j = 0; j < arrlenu(SERVICES.handleTypes[i].value->methods); j++)
        {
            SDL_free(SERVICES.handleTypes[i].value->methods[j]);
        }

        arrfree(SERVICES.handleTypes[i].value->methods);
        arrfree(SERVICES.handleTypes[i].value->targets);
        SDL_free(SERVICES.handleTypes[i].value);
    }

    shfree(SERVICES.handleTypes);
    hmfree(SERVICES.objects);

    for (usz i = 0; i < shlenu(SERVICES.functions); i++)
    {
        ECSIFunction_Free(SERVICES.functions[i].value);
    }

    shfree(SERVICES.functions);

    // what C still keeps is freed with the Lua state's references
    for (usz i = 0; i < hmlenu(SERVICES.kept); i++)
    {
        ECSIServices_FreeCallback(&SERVICES.kept[i].value);
    }

    hmfree(SERVICES.kept);
    arrfree(SERVICES.drawn);
    ECSILua_SetHandleFunctions(nullptr, nullptr);
    SDL_zero(SERVICES);
}

void ECSIServices_RemovePlugin(ECSPlugin plugin)
{
    SDL_assert(plugin != nullptr);

    // a failed plugin's objects may not be valid, so their handles are only forgotten
    for (usz i = shlenu(SERVICES.handleTypes); i > 0; i--)
    {
        ECSIHandleType *type = SERVICES.handleTypes[i - 1].value;

        if (type->plugin == plugin)
        {
            // the core made the objects of a Lua plugin's types, so it can still destroy them
            ECSIServices_ForgetHandles(type, type->Destroy == ECSIServices_DestroyLuaObject);
            type->Destroy = nullptr;

            // the methods call the plugin's functions, which go below, so the type loses them
            if (arrlenu(type->methods) > 0)
            {
                lua_State *state = ECSILua_GetState();
                luaL_getmetatable(state, type->metatable);
                lua_pushnil(state);
                lua_setfield(state, -2, "__index");
                lua_pop(state, 1);
            }

            for (usz j = 0; j < arrlenu(type->methods); j++)
            {
                SDL_free(type->methods[j]);
            }

            arrfree(type->methods);
            arrfree(type->targets);
        }
    }

    // the callbacks that a failed plugin kept are never called again, so they are freed; those it gave another plugin stay, since that plugin may still call them, but they run nothing
    for (usz i = hmlenu(SERVICES.kept); i > 0; i--)
    {
        ECSIFunction *callback = SERVICES.kept[i - 1].value;

        if (callback->plugin == plugin)
        {
            (void)hmdel(SERVICES.kept, SERVICES.kept[i - 1].key);
            ECSIServices_FreeCallback(&callback);
        }
        else if (callback->giver == plugin && !callback->inert)
        {
            callback->inert = true;
            luaL_unref(ECSILua_GetState(), LUA_REGISTRYINDEX, callback->lua);
            callback->lua = LUA_NOREF;
        }
    }

    // the functions that called others for the plugin go with it
    for (usz i = 0; i < shlenu(SERVICES.functions); i++)
    {
        ECSIFunction *function = SERVICES.functions[i].value;

        if (hmgeti(function->callers, plugin) >= 0)
        {
            luaL_unref(ECSILua_GetState(), LUA_REGISTRYINDEX, hmget(function->callers, plugin));
            (void)hmdel(function->callers, plugin);
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
    SDL_assert(plugin != nullptr);
    SDL_assert(name != nullptr);

    ECSIFunction *function = nullptr;
    SHU_ReturnResult(ECSIServices_Find(plugin, name, signature, &function));

    lua_State *state = ECSILua_GetState();

    // a Lua function is called as it is
    if (function->lua != LUA_NOREF)
    {
        lua_rawgeti(state, LUA_REGISTRYINDEX, function->lua);
        return SHUResult_Ok;
    }

    // one Lua function per C function and plugin, made when the plugin first asks for it, so a callback knows which plugin gave it
    if (hmgeti(function->callers, plugin) < 0)
    {
        lua_pushlightuserdata(state, function);
        lua_pushlightuserdata(state, plugin);
        lua_pushcclosure(state, ECSIServices_CallC, 2);
        hmput(function->callers, plugin, luaL_ref(state, LUA_REGISTRYINDEX));
    }

    lua_rawgeti(state, LUA_REGISTRYINDEX, hmget(function->callers, plugin));
    return SHUResult_Ok;
}

SHUResult ECSService_RegisterFunction(ECSPlugin plugin, const char *name, ECSFunction function, const char *signature, const char *description)
{
    SDL_assert(plugin != nullptr);
    SDL_assert(name != nullptr);
    SDL_assert(function != nullptr);
    SDL_assert(signature != nullptr);

    // the plugin gives the local name, and the function is known by its full name
    char *fullName = ECSIPlugin_FullName(plugin, name);
    ECSIFunction *registered = nullptr;
    SHUResult result = fullName == nullptr ? SHUResult_ErrBadData : ECSIServices_Create(plugin, fullName, signature, description, &registered);
    SDL_free(fullName);
    SHU_ReturnResult(result);
    registered->pointer = function;
    shput(SERVICES.functions, registered->name, registered);
    return SHUResult_Ok;
}

SHUResult ECSIServices_RegisterLua(ECSPlugin plugin, const char *name, const char *signature, const char *description)
{
    SDL_assert(plugin != nullptr);
    SDL_assert(name != nullptr);
    SDL_assert(signature != nullptr);

    ECSIFunction *function = nullptr;
    SHU_ReturnResult(ECSIServices_Create(plugin, name, signature, description, &function));

    void *code = nullptr;
    function->closure = ffi_closure_alloc(sizeof(ffi_closure), &code);

    if (function->closure == nullptr || ffi_prep_closure_loc(function->closure, &function->signature.cif, ECSIServices_CallLua, function, code) != FFI_OK)
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
    SDL_assert(name != nullptr);
    SDL_assert(function != nullptr);
    SDL_assert(signature != nullptr);

    ECSIFunction *registered = nullptr;
    SHU_ReturnResult(ECSIServices_Create(nullptr, name, signature, description, &registered));
    registered->pointer = function;
    shput(SERVICES.functions, registered->name, registered);
    return SHUResult_Ok;
}

SHUResult ECSIServices_CallBound(const char *name, ECSPanel focus)
{
    SDL_assert(name != nullptr);

    ECSIFunction *function = SERVICES.functions == nullptr ? nullptr : shget(SERVICES.functions, name);

    if (function == nullptr)
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
    SDL_assert(name != nullptr);
    SDL_assert(path != nullptr);

    ECSIFunction *function = SERVICES.functions == nullptr ? nullptr : shget(SERVICES.functions, name);

    if (function == nullptr)
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
    SDL_assert(function != nullptr);

    for (usz i = 0; i < shlenu(SERVICES.functions); i++)
    {
        if (SERVICES.functions[i].value->plugin == nullptr)
        {
            function(SERVICES.functions[i].key, data);
        }
    }
}

const char *ECSIServices_GetDescription(const char *name)
{
    SDL_assert(name != nullptr);

    ECSIFunction *function = SERVICES.functions == nullptr ? nullptr : shget(SERVICES.functions, name);
    return function == nullptr ? nullptr : function->description;
}

SHUResult ECSIServices_AddMethod(ECSPlugin plugin, const char *type, const char *method, const char *function)
{
    SDL_assert(plugin != nullptr);
    SDL_assert(type != nullptr);
    SDL_assert(method != nullptr);
    SDL_assert(function != nullptr);

    char *typeName = ECSIPlugin_FullName(plugin, type);
    char *functionName = ECSIPlugin_FullName(plugin, function);
    ECSIHandleType *handleType = typeName == nullptr ? nullptr : ECSIServices_FindHandleType(typeName);
    ECSIFunction *target = functionName == nullptr ? nullptr : shget(SERVICES.functions, functionName);
    SHUResult result = SHUResult_Ok;

    // the method is a function of the same plugin whose first parameter is a handle of the type, which the handle itself gives
    if (handleType == nullptr || handleType->plugin != plugin || target == nullptr || arrlenu(target->signature.parameters) == 0 ||
        target->signature.parameters[0].type != ECSIParameterType_Handle || target->signature.parameters[0].handle != handleType)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "'%s' cannot be a method of '%s': it needs to be a function of the plugin whose first parameter is a handle<%s>.", functionName != nullptr ? functionName : function, typeName != nullptr ? typeName : type, typeName != nullptr ? typeName : type);
        result = SHUResult_ErrBadData;
    }

    for (usz i = 0; !result && i < arrlenu(handleType->methods); i++)
    {
        if (SDL_strcmp(handleType->methods[i], method) == 0)
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Handle type '%s' already has a method '%s'.", typeName, method);
            result = SHUResult_ErrBadData;
        }
    }

    char *copy = result ? nullptr : SDL_strdup(method);
    result = result || copy != nullptr ? result : SHUResult_ErrAllocation;

    // the metatable's __index holds the methods, each the function as the plugin itself calls it
    lua_State *state = ECSILua_GetState();
    int top = lua_gettop(state);

    if (!result)
    {
        luaL_getmetatable(state, handleType->metatable);

        if (lua_getfield(state, -1, "__index") != LUA_TTABLE)
        {
            lua_pop(state, 1);
            lua_newtable(state);
            lua_pushvalue(state, -1);
            lua_setfield(state, -3, "__index");
        }

        result = ECSIServices_PushFunction(plugin, functionName, nullptr);
    }

    if (!result)
    {
        lua_setfield(state, -2, method);
        arrput(handleType->methods, copy);
        arrput(handleType->targets, target);
        copy = nullptr;
    }

    lua_settop(state, top);
    SDL_free(copy);
    SDL_free(typeName);
    SDL_free(functionName);
    return result;
}

SHUResult ECSIServices_RegisterLuaHandleType(ECSPlugin plugin, const char *name)
{
    SDL_assert(plugin != nullptr);
    SDL_assert(name != nullptr);

    char *fullName = ECSIPlugin_FullName(plugin, name);
    SHUResult result = fullName == nullptr ? SHUResult_ErrBadData : ECSIServices_RegisterHandleType(plugin, fullName, ECSIServices_DestroyLuaObject);
    SDL_free(fullName);
    return result;
}

/// @brief Finds a handle type that a Lua plugin provides, and raises a Lua error if it is not the plugin's.
static ECSIHandleType *ECSIServices_CheckLuaHandleType(lua_State *state, ECSPlugin plugin, const char *name)
{
    ECSIHandleType *type = ECSIServices_FindHandleType(name);

    if (type == nullptr || type->plugin != plugin || type->Destroy != ECSIServices_DestroyLuaObject)
    {
        luaL_error(state, "handle type '%s' is not one that this plugin registered in Lua", name);
    }

    return type;
}

void ECSIServices_PushLuaHandle(ECSPlugin plugin, const char *name, int index)
{
    SDL_assert(plugin != nullptr);
    SDL_assert(name != nullptr);

    lua_State *state = ECSILua_GetState();
    index = lua_absindex(state, index);
    ECSIServices_CheckLuaHandleType(state, plugin, name);

    ECSILuaObject *object = SDL_malloc(sizeof(ECSILuaObject));

    if (object == nullptr)
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
    SDL_assert(plugin != nullptr);
    SDL_assert(name != nullptr);

    lua_State *state = ECSILua_GetState();
    ECSIServices_CheckLuaHandleType(state, plugin, name);
    ECSILuaObject *object = ECSIServices_CheckHandle(index, name);

    // the check raises a Lua error instead of giving nullptr
    if (object == nullptr)
    {
        lua_pushnil(state);
        return;
    }

    lua_rawgeti(state, LUA_REGISTRYINDEX, object->value);
}

SHUResult ECSHandle_RegisterType(ECSPlugin plugin, const char *name, ECSHandleDestroyFunction Destroy)
{
    SDL_assert(plugin != nullptr);
    SDL_assert(name != nullptr);

    char *fullName = ECSIPlugin_FullName(plugin, name);
    SHUResult result = fullName == nullptr ? SHUResult_ErrBadData : ECSIServices_RegisterHandleType(plugin, fullName, Destroy);
    SDL_free(fullName);
    return result;
}

SHUResult ECSHandle_AddMethod(ECSPlugin plugin, const char *type, const char *method, const char *function)
{
    SDL_assert(plugin != nullptr);
    SDL_assert(type != nullptr);
    SDL_assert(method != nullptr);
    SDL_assert(function != nullptr);

    return ECSIServices_AddMethod(plugin, type, method, function);
}

void ECSHandle_Forget(void *object)
{
    SDL_assert(object != nullptr);

    ECSIServices_ForgetHandle(object);
}

SHUResult ECSService_GetFunction(ECSPlugin plugin, ECSFunction *retFunction, const char *name, const char *signature)
{
    SDL_assert(plugin != nullptr);
    SDL_assert(retFunction != nullptr);
    SDL_assert(name != nullptr);
    SDL_assert(signature != nullptr);

    ECSIFunction *function = nullptr;
    SHU_ReturnResult(ECSIServices_Find(plugin, name, signature, &function));
    *retFunction = function->pointer;
    return SHUResult_Ok;
}

void ECSService_ReleaseCallback(ECSFunction callback)
{
    void *code = nullptr;
    SDL_memcpy(&code, &callback, sizeof(code));

    // a C function's pointer, or one that was released, is not in the map
    ECSIFunction *kept = callback == nullptr || SERVICES.kept == nullptr ? nullptr : hmget(SERVICES.kept, code);

    if (kept != nullptr)
    {
        (void)hmdel(SERVICES.kept, code);
        ECSIServices_FreeCallback(&kept);
    }
}

SHUResult ECSIServices_PushPlugin(ECSPlugin plugin, ECSPlugin provider)
{
    SDL_assert(plugin != nullptr);
    SDL_assert(provider != nullptr);

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
            SHU_ReturnResult(ECSIServices_PushFunction(plugin, function->name, nullptr), lua_pop(state, 1););
            lua_setfield(state, -2, SDL_strchr(function->name, '.') + 1);
        }
    }

    return SHUResult_Ok;
}

#pragma region Definitions

/// @brief Text that grows as parts are added; a failed addition leaves it nullptr, and later additions do nothing.
typedef struct ECSIDefinitionText
{
    char *data;
    bool failed;
} ECSIDefinitionText;

static void ECSIDefinitionText_Add(ECSIDefinitionText *text, SDL_PRINTF_FORMAT_STRING const char *format, ...) SDL_PRINTF_VARARG_FUNC(2);

static void ECSIDefinitionText_Add(ECSIDefinitionText *text, const char *format, ...)
{
    char *part = nullptr;
    char *joined = nullptr;
    va_list arguments;
    va_start(arguments, format);
    bool ok = !text->failed && SDL_vasprintf(&part, format, arguments) >= 0;
    va_end(arguments);

    ok = ok && SDL_asprintf(&joined, "%s%s", text->data == nullptr ? "" : text->data, part) >= 0;
    SDL_free(part);
    SDL_free(text->data);
    text->data = ok ? joined : nullptr;
    text->failed = !ok;
}

/// @brief Names a parameter that its signature leaves unnamed: arg1, arg2 and so on.
static const char *ECSIDefinition_Name(ECSIParameter parameter, usz index, char *buffer, usz size)
{
    if (parameter.name != nullptr)
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
    static const char *const types[ECSIParameterType_Count] = {"nil", "boolean", "integer", "integer", "number", "number", "string", "string", "string", "any", "userdata", "function"};

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
    else if (parameter.type == ECSIParameterType_Handle && parameter.handle != nullptr)
    {
        // a plugin's handle type is a class of its definitions, with its methods
        ECSIDefinitionText_Add(text, "%s", parameter.handle->name);
    }
    else if (parameter.type == ECSIParameterType_Value && parameter.luaType != nullptr)
    {
        ECSIDefinitionText_Add(text, "%s", parameter.luaType);
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
    static const char *const types[ECSIParameterType_Count] = {"void", "bool", "i32", "i64", "f32", "f64", "ECSColor", "const char *", "SHUSlice ", "const ECSValue *", "void *", ""};
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

/// @brief Adds a file of a plugin's folder to the end of a definition, if the plugin has it.
/// @param comment How a line comment starts in the definition's language.
static void ECSIDefinition_AddFile(ECSIDefinitionText *text, ECSPlugin provider, const char *name, const char *comment)
{
    char *path = nullptr;
    usz size = 0;
    char *contents = SDL_asprintf(&path, "%s%s", ECSPlugin_GetFolder(provider), name) < 0 ? nullptr : SDL_LoadFile(path, &size);

    if (contents != nullptr)
    {
        ECSIDefinitionText_Add(text, "\n%s the plugin's own, from its %s\n%s%s", comment, name, contents, size > 0 && contents[size - 1] == '\n' ? "" : "\n");
    }

    SDL_free(contents);
    SDL_free(path);
}

/// @brief Writes a text file.
static SHUResult ECSIDefinition_Save(const char *folder, const char *name, const char *extension, const ECSIDefinitionText *text)
{
    char *path = nullptr;

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

/// @brief Adds a function to a Lua definition: its description, parameters, result and outputs, then the function, as a field of a table or a method.
/// @param owner The table, such as "Web" or "WebSocket".
/// @param name The function's name in the table.
/// @param method Whether it is a method, whose first parameter is the handle it is called on.
static void ECSIDefinition_AddLuaFunctionOf(ECSIDefinitionText *lua, const ECSIFunction *function, const char *owner, const char *name, bool method)
{
    const ECSISignature *signature = &function->signature;
    usz first = method ? 1 : 0;
    char buffer[32];
    ECSIDefinitionText_Add(lua, "\n---%s\n", function->description);

    for (usz j = first; j < arrlenu(signature->parameters); j++)
    {
        if (!signature->parameters[j].out)
        {
            ECSIDefinitionText_Add(lua, "---@param %s ", ECSIDefinition_Name(signature->parameters[j], j, buffer, sizeof(buffer)));
            ECSIDefinition_AddLuaType(lua, signature->parameters[j]);
            ECSIDefinitionText_Add(lua, "%s%s\n", signature->parameters[j].doc == nullptr ? "" : " ", signature->parameters[j].doc == nullptr ? "" : signature->parameters[j].doc);
        }
    }

    for (usz j = 0; j <= arrlenu(signature->parameters); j++)
    {
        ECSIParameter result = j == 0 ? signature->result : signature->parameters[j - 1];

        if ((j == 0 && result.type != ECSIParameterType_Void) || (j > 0 && result.out))
        {
            // LuaLS reads a description after a name, or after # without one
            ECSIDefinitionText_Add(lua, "---@return ");
            ECSIDefinition_AddLuaType(lua, result);
            ECSIDefinitionText_Add(lua, "%s%s", result.name == nullptr ? "" : " ", result.name == nullptr ? "" : result.name);
            ECSIDefinitionText_Add(lua, "%s%s\n", result.doc == nullptr ? "" : result.name == nullptr ? " # " : " ", result.doc == nullptr ? "" : result.doc);
        }
    }

    // a local name with a dot is not a Lua name, so it is written as a key
    if (method)
    {
        ECSIDefinitionText_Add(lua, "function %s:%s(", owner, name);
    }
    else if (SDL_strchr(name, '.') == nullptr)
    {
        ECSIDefinitionText_Add(lua, "function %s.%s(", owner, name);
    }
    else
    {
        ECSIDefinitionText_Add(lua, "%s[\"%s\"] = function(", owner, name);
    }

    for (usz j = first, written = 0; j < arrlenu(signature->parameters); j++)
    {
        if (!signature->parameters[j].out)
        {
            ECSIDefinitionText_Add(lua, "%s%s", written++ == 0 ? "" : ", ", ECSIDefinition_Name(signature->parameters[j], j, buffer, sizeof(buffer)));
        }
    }

    ECSIDefinitionText_Add(lua, ") end\n");
}

SHUResult ECSIServices_WriteDefinitions(ECSPlugin provider, const char *folder)
{
    SDL_assert(provider != nullptr);
    SDL_assert(folder != nullptr);

    const char *plugin = ECSIPlugin_GetName(provider);
    char pascal[128];
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

        ECSIDefinition_AddLuaFunctionOf(&lua, function, pascal, local, false);

        // C: a member that points to the function
        char declarator[300];
        ECSIDefinition_Member(local, member, sizeof(member));
        SDL_snprintf(declarator, sizeof(declarator), "(*%s)", member);
        ECSIDefinitionText_Add(&c, "    /// @brief %s\n", function->description);

        for (usz j = 0; j < arrlenu(signature->parameters); j++)
        {
            if (signature->parameters[j].doc != nullptr)
            {
                ECSIDefinitionText_Add(&c, "    /// @param %s %s\n", signature->parameters[j].name, signature->parameters[j].doc);
            }
        }

        if (signature->result.doc != nullptr)
        {
            ECSIDefinitionText_Add(&c, "    /// @return %s\n", signature->result.doc);
        }

        ECSIDefinitionText_Add(&c, "    ");
        ECSIDefinition_AddCDeclaration(&c, signature->result, declarator);
        ECSIDefinition_AddCParameters(&c, signature);
        ECSIDefinitionText_Add(&c, ";\n");
    }

    // each of the plugin's handle types is a class, with its methods
    for (usz i = 0; i < shlenu(SERVICES.handleTypes); i++)
    {
        const ECSIHandleType *type = SERVICES.handleTypes[i].value;

        if (type->plugin != provider)
        {
            continue;
        }

        char className[256];
        ECSIDefinition_Pascal(type->name, className, sizeof(className));
        ECSIDefinitionText_Add(&lua, "\n---A handle of the type %s.\n---@class %s\nlocal %s = {}\n", type->name, type->name, className);

        for (usz j = 0; j < arrlenu(type->methods); j++)
        {
            ECSIDefinition_AddLuaFunctionOf(&lua, type->targets[j], className, type->methods[j], true);
        }
    }

    // the classes that the plugin's value<...> types name, from its types.lua, which holds only annotations
    ECSIDefinition_AddFile(&lua, provider, "types.lua", "--");
    ECSIDefinitionText_Add(&lua, "\nreturn %s\n", pascal);
    ECSIDefinitionText_Add(&c, "} %sFunctions;\n\n/// @brief Looks up every function of the plugin %s. Call it from ECSPlugin_Init; the plugin's manifest must depend on %s.\n/// @return SHUResult_Ok, or the error of the first function that cannot be looked up.\nstatic inline SHUResult %sFunctions_Get(ECSPlugin plugin, %sFunctions *retFunctions)\n{\n    ECSFunction function = nullptr;\n", pascal, plugin, plugin, pascal, pascal);

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

    // what the plugin gives C plugins beside its functions, such as macros, from its header.h
    ECSIDefinition_AddFile(&c, provider, "header.h", "//");

    SHUResult result = ECSIDefinition_Save(folder, plugin, ".lua", &lua);
    result = result ? result : ECSIDefinition_Save(folder, plugin, ".h", &c);
    SDL_free(lua.data);
    SDL_free(c.data);
    return result;
}
