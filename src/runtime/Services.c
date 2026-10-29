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
    ECSIParameterType_Callback,
    ECSIParameterType_Struct,
    ECSIParameterType_Count,
} ECSIParameterType;

/// @brief Names of the parameter types in signatures.
static const char *const OPENECS_PARAMETER_TYPE_NAMES[ECSIParameterType_Count] = {"void", "bool", "int", "int64", "float", "double", "color", "string", "buffer", "value", "handle", "fn", "struct"};

/// @brief Prefix of the names of handle types' metatables.
#define OPENECS_HANDLE_METATABLE "ecs.handle:"

/// @brief Prefix of the names of struct types' metatables.
#define OPENECS_STRUCT_METATABLE "ecs.struct:"

/// @brief The methods whose names make them Lua's operators too, with the metatable fields of those operators: a struct type's add is also its +.
static const char *const OPENECS_STRUCT_OPERATORS[][2] = {
    {"add", "__add"},
    {"sub", "__sub"},
    {"mul", "__mul"},
    {"div", "__div"},
    {"unm", "__unm"},
    {"eq", "__eq"},
    {"lt", "__lt"},
    {"le", "__le"},
    {"len", "__len"},
    {"tostring", "__tostring"},
};

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
    char **methods;                      // stb_ds array: the names of its methods in Lua
    const struct ECSIFunction **targets; // stb_ds array: the function each method calls, in the same order
} ECSIHandleType;

/// @brief A field of a struct type: one value, or a list of a fixed length.
typedef struct ECSIStructField
{
    char *name;                              // nullptr for the list that a list struct is
    ECSIParameterType type;                  // bool, int, int64, float, double, color or struct
    const struct ECSIStructType *structType; // structs: the field's type
    usz count;                               // 0 for one value; the length of a list
    usz offset;                              // in the C struct
} ECSIStructField;

/// @brief A registered struct type: plain data that crosses between C and Lua by value, a C struct in C and a table in Lua (10.11).
typedef struct ECSIStructType
{
    char *name;
    char *metatable;         // name of its metatable in the registry
    ECSPlugin plugin;        // nullptr for the core
    ECSIStructField *fields; // stb_ds array, in the order of the C struct
    usz size;
    usz alignment;
    char **methods;                      // stb_ds array: the names of its methods in Lua
    const struct ECSIFunction **targets; // stb_ds array: the function each method calls, in the same order
} ECSIStructType;

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
    char *name;                     // the name the signature gives it, or nullptr; only for documentation
    bool out;                       // an output parameter: a pointer in C, an extra result in Lua
    bool keep;                      // a callback that stays valid after the call, until the function that took it releases it
    ECSIHandleType *handle;         // handles: their type
    ECSIStructType *structType;     // structs: their type
    struct ECSISignature *callback; // callbacks: their signature, owned by the parameter
    char *luaType;                  // values: the Lua type that value<...> names for editors, or nullptr; only for documentation, as names are
    char *documentation;            // what the parameter or result is, from the function's description, or nullptr; only for documentation
} ECSIParameter;

/// @brief A parsed signature with its libffi call description.
typedef struct ECSISignature
{
    ECSIParameter result;
    ECSIParameter *parameters; // stb_ds array
    ffi_type **types;          // stb_ds array of the parameters' libffi types
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

/// @brief What a call from Lua through libffi keeps for one parameter.
typedef struct ECSICallParameter
{
    ECSISlot slot;                 // the argument; for an output, a pointer to output
    ECSISlot output;               // an output's storage
    struct ECSIFunction *callback; // the closure made for a Lua function argument, or nullptr
    int index;                     // where the argument is on the Lua stack; outputs have none
} ECSICallParameter;

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
    struct
    {
        char *key; // the type's own copy of its name
        ECSIStructType *value;
    } *structTypes; // stb_ds hash map; parameters and fields point to their type
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

/// @brief Text that grows as parts are added; a failed addition leaves it nullptr, and later additions do nothing.
typedef struct ECSIText
{
    char *data;
    bool failed;
} ECSIText;

static void ECSIText_Add(ECSIText *text, SDL_PRINTF_FORMAT_STRING const char *format, ...) SDL_PRINTF_VARARG_FUNC(2);

static void ECSIText_Add(ECSIText *text, const char *format, ...)
{
    char *part = nullptr;
    char *joined = nullptr;
    va_list arguments;
    va_start(arguments, format);
    bool succeeded = !text->failed && SDL_vasprintf(&part, format, arguments) >= 0;
    va_end(arguments);

    succeeded = succeeded && SDL_asprintf(&joined, "%s%s", text->data == nullptr ? "" : text->data, part) >= 0;
    SDL_free(part);
    SDL_free(text->data);
    text->data = succeeded ? joined : nullptr;
    text->failed = !succeeded;
}

static ECSIHandleType *ECSIServices_FindHandleType(const char *name)
{
    // shget would allocate a map that is missing
    return SERVICES.handleTypes == nullptr ? nullptr : shget(SERVICES.handleTypes, name);
}

static ECSIStructType *ECSIServices_FindStructType(const char *name)
{
    // shget would allocate a map that is missing
    return SERVICES.structTypes == nullptr ? nullptr : shget(SERVICES.structTypes, name);
}

static struct ECSIFunction *ECSIServices_FindFunction(const char *name)
{
    // shget would allocate a map that is missing
    return SERVICES.functions == nullptr ? nullptr : shget(SERVICES.functions, name);
}

static ffi_type *ECSIServices_GetFfiType(ECSIParameter parameter)
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
    SDL_free(parameter->documentation);
    parameter->documentation = nullptr;

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

static void ECSISignature_SkipSpaces(const char **cursor)
{
    while (SDL_isspace((unsigned char)**cursor))
    {
        (*cursor)++;
    }
}

/// @brief Reads one word of a signature and moves past it and the spaces after it.
/// @return The word's length; 0 if there is none.
static usz ECSISignature_ReadWord(const char **cursor, const char **retWord)
{
    *retWord = *cursor;

    while (SDL_isalnum((unsigned char)**cursor) || **cursor == '_')
    {
        (*cursor)++;
    }

    usz length = (usz)(*cursor - *retWord);
    ECSISignature_SkipSpaces(cursor);
    return length;
}

/// @brief Reads the type of a handle, "<name>", after the word "handle". The type must be registered.
static bool ECSISignature_ReadHandle(const char **cursor, ECSIParameter *retParameter)
{
    const char *start = *cursor + 1;
    const char *end = SDL_strchr(start, '>');

    if (**cursor != '<' || end == nullptr || end == start)
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
    *cursor = end + 1;
    ECSISignature_SkipSpaces(cursor);
    return retParameter->handle != nullptr;
}

/// @brief Reads the type of a struct, "<name>", after the word "struct". The type must be registered.
static bool ECSISignature_ReadStruct(const char **cursor, ECSIParameter *retParameter)
{
    const char *start = *cursor + 1;
    const char *end = SDL_strchr(start, '>');

    if (**cursor != '<' || end == nullptr || end == start)
    {
        return false;
    }

    char *name = SDL_strndup(start, (usz)(end - start));
    retParameter->structType = name == nullptr ? nullptr : ECSIServices_FindStructType(name);

    if (name != nullptr && retParameter->structType == nullptr)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Struct type '%s' is not registered.", name);
    }

    SDL_free(name);
    *cursor = end + 1;
    ECSISignature_SkipSpaces(cursor);
    return retParameter->structType != nullptr;
}

/// @brief Reads the signature of a callback, "<signature>", after the word "fn". A callback's signature has no callbacks.
static bool ECSISignature_ReadCallback(const char **cursor, ECSIParameter *retParameter)
{
    if (**cursor != '<')
    {
        return false;
    }

    // the signature ends at the '>' that matches the '<', because handles inside it have their own
    const char *start = *cursor + 1;
    const char *end = start;
    i32 depth = 1;

    while (*end != '\0')
    {
        if (*end == '<')
        {
            depth++;
        }
        else if (*end == '>')
        {
            depth--;
        }

        if (depth == 0)
        {
            break;
        }

        end++;
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
        valid = retParameter->callback->parameters[i].type != ECSIParameterType_Callback;
    }

    SDL_free(inner);
    *cursor = *end == '>' ? end + 1 : end;
    ECSISignature_SkipSpaces(cursor);
    return valid;
}

/// @brief Reads what a value<...> names: the Lua type of the value, such as ui.Props, for the definitions only.
static bool ECSISignature_ReadLuaType(const char **cursor, ECSIParameter *retParameter)
{
    if (**cursor != '<')
    {
        return true;
    }

    const char *start = *cursor + 1;
    const char *end = SDL_strchr(start, '>');

    if (end == nullptr || end == start)
    {
        return false;
    }

    retParameter->luaType = SDL_strndup(start, (usz)(end - start));
    *cursor = end + 1;
    ECSISignature_SkipSpaces(cursor);
    return retParameter->luaType != nullptr;
}

/// @brief Reads a parameter or result of a signature: a type name, after "out" for an output parameter.
static bool ECSISignature_ReadParameter(const char **cursor, ECSIParameter *retParameter)
{
    const char *word = nullptr;
    usz length = ECSISignature_ReadWord(cursor, &word);
    retParameter->out = length == 3 && SDL_strncmp(word, "out", 3) == 0;
    retParameter->keep = length == 4 && SDL_strncmp(word, "keep", 4) == 0;

    if (retParameter->out || retParameter->keep)
    {
        length = ECSISignature_ReadWord(cursor, &word);
    }

    for (i32 type = 0; type < ECSIParameterType_Count; type++)
    {
        const char *name = OPENECS_PARAMETER_TYPE_NAMES[type];

        if (length == SDL_strlen(name) && SDL_strncmp(word, name, length) == 0)
        {
            retParameter->type = (ECSIParameterType)type;

            // only a callback is kept
            if (retParameter->keep && type != ECSIParameterType_Callback)
            {
                return false;
            }

            switch (type)
            {
            case ECSIParameterType_Handle:
                return ECSISignature_ReadHandle(cursor, retParameter);
            case ECSIParameterType_Struct:
                return ECSISignature_ReadStruct(cursor, retParameter);
            case ECSIParameterType_Callback:
                return ECSISignature_ReadCallback(cursor, retParameter);
            case ECSIParameterType_Value:
                return ECSISignature_ReadLuaType(cursor, retParameter);
            default:
                return true;
            }
        }
    }

    return false;
}

/// @brief Adds a parameter as the signature text has it, such as "out float" or "handle<ecs.panel>".
static void ECSISignature_AddParameter(ECSIText *text, ECSIParameter parameter)
{
    if (parameter.out)
    {
        ECSIText_Add(text, "out ");
    }
    else if (parameter.keep)
    {
        ECSIText_Add(text, "keep ");
    }

    ECSIText_Add(text, "%s", OPENECS_PARAMETER_TYPE_NAMES[parameter.type]);

    // a handle names its type, and a callback its signature, between angle brackets
    if (parameter.type == ECSIParameterType_Handle)
    {
        ECSIText_Add(text, "<%s>", parameter.handle->name);
    }
    else if (parameter.type == ECSIParameterType_Struct)
    {
        ECSIText_Add(text, "<%s>", parameter.structType->name);
    }
    else if (parameter.type == ECSIParameterType_Callback)
    {
        ECSIText_Add(text, "<%s>", parameter.callback->text);
    }
}

/// @brief Reads a parameter of a signature's list, with its name if it has one and the comma after it, and adds it to the signature.
/// @return false if the parameter is not valid; then nothing is added.
static bool ECSISignature_ReadListedParameter(const char **cursor, ECSISignature *signature)
{
    ECSIParameter parameter = {0};
    bool valid = ECSISignature_ReadParameter(cursor, &parameter) && parameter.type != ECSIParameterType_Void && !(parameter.out && parameter.type == ECSIParameterType_Callback);

    // a parameter may have a name after its type, such as "float x"
    if (valid && (SDL_isalpha((unsigned char)**cursor) || **cursor == '_'))
    {
        const char *word = nullptr;
        usz length = ECSISignature_ReadWord(cursor, &word);
        parameter.name = SDL_strndup(word, length);
        valid = parameter.name != nullptr;
    }

    valid = valid && (**cursor == ',' || **cursor == ')');
    *cursor += valid && **cursor == ',' ? 1 : 0;
    ECSISignature_SkipSpaces(cursor);

    if (!valid)
    {
        ECSIParameter_Free(&parameter);
        return false;
    }

    arrput(signature->parameters, parameter);
    arrput(signature->types, ECSIServices_GetFfiType(parameter));
    signature->outCount += parameter.out ? 1 : 0;
    return true;
}

/// @brief Writes a signature's text again, so signatures compare the same however they are spaced: the result, then the parameters in parentheses.
static ECSIText ECSISignature_Write(const ECSISignature *signature)
{
    ECSIText written = {0};
    ECSISignature_AddParameter(&written, signature->result);
    ECSIText_Add(&written, "(");

    for (usz i = 0; i < arrlenu(signature->parameters); i++)
    {
        ECSIText_Add(&written, "%s", i == 0 ? "" : ", ");
        ECSISignature_AddParameter(&written, signature->parameters[i]);
    }

    ECSIText_Add(&written, ")");
    return written;
}

/// @brief Parses a signature such as "int(string path, out float)", and prepares its libffi call description. Parameter names are kept apart from the signature's text, so they never change how signatures compare.
static SHUResult ECSISignature_Parse(const char *text, ECSISignature *retSignature)
{
    SDL_zerop(retSignature);
    const char *cursor = text;
    ECSISignature_SkipSpaces(&cursor);

    // a callback is a parameter only; it is never an output or a result; a struct comes back as an output, as C passes structs by pointer
    bool valid = ECSISignature_ReadParameter(&cursor, &retSignature->result) && !retSignature->result.out && retSignature->result.type != ECSIParameterType_Callback &&
                 retSignature->result.type != ECSIParameterType_Struct && *cursor++ == '(';
    ECSISignature_SkipSpaces(&cursor);

    while (valid && *cursor != ')')
    {
        valid = ECSISignature_ReadListedParameter(&cursor, retSignature);
    }

    valid = valid && *cursor++ == ')';
    ECSISignature_SkipSpaces(&cursor);

    if (!valid || *cursor != '\0')
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "'%s' is not a signature, such as \"int(string, out float)\".", text);
        ECSISignature_Free(retSignature);
        return SHUResult_ErrBadData;
    }

    ECSIText written = ECSISignature_Write(retSignature);
    retSignature->text = written.data;

    if (written.failed)
    {
        ECSISignature_Free(retSignature);
        return SHUResult_ErrAllocation;
    }

    if (ffi_prep_cif(&retSignature->cif, FFI_DEFAULT_ABI, (unsigned int)arrlenu(retSignature->parameters), ECSIServices_GetFfiType(retSignature->result), retSignature->types) != FFI_OK)
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
    ECSIFunction *function = ECSIServices_FindFunction(name);

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

static void ECSIServices_PushArgument(lua_State *state, ECSIParameter parameter, const void *argument);

#pragma region Structs

/// @brief The size of one value of a field, which is also its alignment, as C lays out these types.
static usz ECSIStructs_GetFieldSize(const ECSIStructField *field)
{
    switch (field->type)
    {
    case ECSIParameterType_Bool:
        return sizeof(bool);
    case ECSIParameterType_Int:
        return sizeof(i32);
    case ECSIParameterType_Int64:
        return sizeof(i64);
    case ECSIParameterType_Float:
        return sizeof(f32);
    case ECSIParameterType_Double:
        return sizeof(f64);
    case ECSIParameterType_Color:
        return sizeof(ECSColor);
    default:
        return field->structType->size;
    }
}

static usz ECSIStructs_GetFieldAlignment(const ECSIStructField *field)
{
    return field->type == ECSIParameterType_Struct ? field->structType->alignment : ECSIStructs_GetFieldSize(field);
}

/// @brief Reads one field of a struct's fields, such as "float x", "struct<cglm.vec3> position" or "float m[16]"; a list struct's only field has no name, as "float[16]".
static bool ECSIStructs_ReadField(const char **cursor, ECSIStructField *retField)
{
    ECSIParameter parameter = {0};

    if (!ECSISignature_ReadParameter(cursor, &parameter) || parameter.out || parameter.keep)
    {
        ECSIParameter_Free(&parameter);
        return false;
    }

    retField->type = parameter.type;
    retField->structType = parameter.structType;
    ECSIParameter_Free(&parameter);

    if (retField->type < ECSIParameterType_Bool || (retField->type > ECSIParameterType_Color && retField->type != ECSIParameterType_Struct))
    {
        return false;
    }

    if (SDL_isalpha((unsigned char)**cursor) || **cursor == '_')
    {
        const char *word = nullptr;
        usz length = ECSISignature_ReadWord(cursor, &word);
        retField->name = SDL_strndup(word, length);

        if (retField->name == nullptr)
        {
            return false;
        }
    }

    // a list of a fixed length, such as [16]
    if (**cursor == '[')
    {
        char *end = nullptr;
        long count = SDL_strtol(*cursor + 1, &end, 10);

        if (count <= 0 || *end != ']')
        {
            return false;
        }

        retField->count = (usz)count;
        *cursor = end + 1;
        ECSISignature_SkipSpaces(cursor);
    }

    return retField->name != nullptr || retField->count > 0;
}

static void ECSIStructs_FreeFields(ECSIStructField **fields)
{
    for (usz i = 0; i < arrlenu(*fields); i++)
    {
        SDL_free((*fields)[i].name);
    }

    arrfree(*fields);
}

/// @brief Reads a struct's fields, separated by commas, and lays them out as C does: each at a multiple of its alignment, and the size a multiple of the largest.
static bool ECSIStructs_ReadFields(const char *text, ECSIStructType *type)
{
    const char *cursor = text;
    ECSISignature_SkipSpaces(&cursor);
    type->alignment = 1;

    while (*cursor != '\0')
    {
        ECSIStructField field = {0};
        bool valid = ECSIStructs_ReadField(&cursor, &field) && (*cursor == ',' || *cursor == '\0');

        // a field without a name is the list that the struct is, so it is the only one
        valid = valid && (field.name != nullptr || (arrlenu(type->fields) == 0 && *cursor == '\0'));

        if (!valid)
        {
            SDL_free(field.name);
            return false;
        }

        usz size = ECSIStructs_GetFieldSize(&field);
        usz alignment = ECSIStructs_GetFieldAlignment(&field);
        field.offset = (type->size + alignment - 1) / alignment * alignment;
        type->size = field.offset + size * SDL_max(field.count, 1);
        type->alignment = SDL_max(type->alignment, alignment);
        arrput(type->fields, field);

        cursor += *cursor == ',' ? 1 : 0;
        ECSISignature_SkipSpaces(&cursor);
    }

    type->size = (type->size + type->alignment - 1) / type->alignment * type->alignment;
    return arrlenu(type->fields) > 0;
}

static bool ECSIStructs_Read(lua_State *state, int index, const ECSIStructType *type, void *retMemory, const char **retField);

/// @brief Reads one Lua value of a field's type into memory.
static bool ECSIStructs_ReadValue(lua_State *state, int index, const ECSIStructField *field, void *retMemory)
{
    int isNumber = 0;
    const char *nested = nullptr;

    switch (field->type)
    {
    case ECSIParameterType_Bool:
        *(bool *)retMemory = lua_toboolean(state, index) != 0;
        return lua_isboolean(state, index);
    case ECSIParameterType_Int:
    {
        lua_Integer integer = lua_tointegerx(state, index, &isNumber);
        *(i32 *)retMemory = (i32)integer;
        return isNumber && integer >= SDL_MIN_SINT32 && integer <= SDL_MAX_SINT32;
    }
    case ECSIParameterType_Int64:
        *(i64 *)retMemory = (i64)lua_tointegerx(state, index, &isNumber);
        return isNumber;
    case ECSIParameterType_Float:
        *(f32 *)retMemory = (f32)lua_tonumberx(state, index, &isNumber);
        return isNumber;
    case ECSIParameterType_Double:
        *(f64 *)retMemory = (f64)lua_tonumberx(state, index, &isNumber);
        return isNumber;
    case ECSIParameterType_Color:
        return lua_type(state, index) == LUA_TSTRING && ECSColor_Parse(lua_tostring(state, index), retMemory);
    default:
        return ECSIStructs_Read(state, index, field->structType, retMemory, &nested);
    }
}

/// @brief Reads a table, at an index of the stack, into a struct's memory: each field by its name, and a list's values by their positions. Any table with the fields will do.
/// @param retField The name of the first field that is missing or of another type, or nullptr if the value is not a table.
/// @return false if the value is not a table, or a field is missing or of another type.
static bool ECSIStructs_Read(lua_State *state, int index, const ECSIStructType *type, void *retMemory, const char **retField)
{
    *retField = nullptr;

    if (!lua_istable(state, index))
    {
        return false;
    }

    index = lua_absindex(state, index);

    for (usz i = 0; i < arrlenu(type->fields); i++)
    {
        const ECSIStructField *field = &type->fields[i];
        u8 *memory = (u8 *)retMemory + field->offset;
        bool valid = true;

        // a list struct's list is the table itself
        if (field->name != nullptr)
        {
            lua_getfield(state, index, field->name);
        }
        else
        {
            lua_pushvalue(state, index);
        }

        if (field->count == 0)
        {
            valid = ECSIStructs_ReadValue(state, -1, field, memory);
        }
        else
        {
            valid = lua_istable(state, -1);

            for (usz j = 0; valid && j < field->count; j++)
            {
                lua_rawgeti(state, -1, (lua_Integer)j + 1);
                valid = ECSIStructs_ReadValue(state, -1, field, memory + j * ECSIStructs_GetFieldSize(field));
                lua_pop(state, 1);
            }
        }

        lua_pop(state, 1);

        if (!valid)
        {
            *retField = field->name != nullptr ? field->name : "list";
            return false;
        }
    }

    return true;
}

static void ECSIStructs_Push(lua_State *state, const ECSIStructType *type, const void *memory);

static void ECSIServices_PushColor(lua_State *state, ECSColor color);

/// @brief Pushes one value of a field's type from memory.
static void ECSIStructs_PushValue(lua_State *state, const ECSIStructField *field, const void *memory)
{
    switch (field->type)
    {
    case ECSIParameterType_Bool:
        lua_pushboolean(state, *(const bool *)memory);
        break;
    case ECSIParameterType_Int:
        lua_pushinteger(state, (lua_Integer) * (const i32 *)memory);
        break;
    case ECSIParameterType_Int64:
        lua_pushinteger(state, (lua_Integer) * (const i64 *)memory);
        break;
    case ECSIParameterType_Float:
        lua_pushnumber(state, (lua_Number) * (const f32 *)memory);
        break;
    case ECSIParameterType_Double:
        lua_pushnumber(state, (lua_Number) * (const f64 *)memory);
        break;
    case ECSIParameterType_Color:
        ECSIServices_PushColor(state, *(const ECSColor *)memory);
        break;
    default:
        ECSIStructs_Push(state, field->structType, memory);
        break;
    }
}

/// @brief Pushes a struct's memory as a new table of its fields, with the type's metatable, which gives its methods and operators.
static void ECSIStructs_Push(lua_State *state, const ECSIStructType *type, const void *memory)
{
    bool list = type->fields[0].name == nullptr;
    lua_createtable(state, list ? (int)type->fields[0].count : 0, list ? 0 : (int)arrlenu(type->fields));

    for (usz i = 0; i < arrlenu(type->fields); i++)
    {
        const ECSIStructField *field = &type->fields[i];
        const u8 *start = (const u8 *)memory + field->offset;

        if (field->count == 0)
        {
            ECSIStructs_PushValue(state, field, start);
            lua_setfield(state, -2, field->name);
            continue;
        }

        // a list struct's values go into the struct's table, a list field's into a table of its own
        if (field->name != nullptr)
        {
            lua_createtable(state, (int)field->count, 0);
        }

        for (usz j = 0; j < field->count; j++)
        {
            ECSIStructs_PushValue(state, field, start + j * ECSIStructs_GetFieldSize(field));
            lua_rawseti(state, -2, (lua_Integer)j + 1);
        }

        if (field->name != nullptr)
        {
            lua_setfield(state, -2, field->name);
        }
    }

    luaL_setmetatable(state, type->metatable);
}

static void ECSIStructs_FreeType(ECSIStructType *type)
{
    ECSIStructs_FreeFields(&type->fields);

    for (usz i = 0; i < arrlenu(type->methods); i++)
    {
        SDL_free(type->methods[i]);
    }

    arrfree(type->methods);
    arrfree(type->targets);
    SDL_free(type->name);
    SDL_free(type->metatable);
    SDL_free(type);
}

/// @brief Registers a struct type by its full name.
static SHUResult ECSIStructs_Register(ECSPlugin plugin, const char *name, const char *fields)
{
    // handles and structs share the names of types, so a parameter names one of them
    if (ECSIServices_FindStructType(name) != nullptr || ECSIServices_FindHandleType(name) != nullptr)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Type '%s' is already registered.", name);
        return SHUResult_ErrBadData;
    }

    ECSIStructType *type = SDL_calloc(1, sizeof(ECSIStructType));

    if (type == nullptr || (type->name = SDL_strdup(name)) == nullptr || SDL_asprintf(&type->metatable, "%s%s", OPENECS_STRUCT_METATABLE, name) < 0)
    {
        if (type != nullptr)
        {
            SDL_free(type->name);
        }

        SDL_free(type);
        return SHUResult_ErrAllocation;
    }

    type->plugin = plugin;

    if (!ECSIStructs_ReadFields(fields, type))
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "'%s' are not the fields of a struct, such as \"float x, float y\" or \"float[16]\".", fields);
        ECSIStructs_FreeType(type);
        return SHUResult_ErrBadData;
    }

    // the methods go into __index once there are some
    lua_State *state = ECSILua_GetState();
    luaL_newmetatable(state, type->metatable);
    lua_pop(state, 1);

    shput(SERVICES.structTypes, type->name, type);
    return SHUResult_Ok;
}

/// @brief Takes a struct type's methods away, when the plugin whose functions they call goes.
static void ECSIStructs_DropMethods(ECSIStructType *type)
{
    lua_State *state = ECSILua_GetState();
    luaL_getmetatable(state, type->metatable);
    lua_pushnil(state);
    lua_setfield(state, -2, "__index");

    for (usz i = 0; i < SDL_arraysize(OPENECS_STRUCT_OPERATORS); i++)
    {
        lua_pushnil(state);
        lua_setfield(state, -2, OPENECS_STRUCT_OPERATORS[i][1]);
    }

    lua_pop(state, 1);

    for (usz i = 0; i < arrlenu(type->methods); i++)
    {
        SDL_free(type->methods[i]);
    }

    arrfree(type->methods);
    arrfree(type->targets);
}

#pragma endregion Structs

#pragma region Lua Calls C

/// @brief Pushes a colour for Lua, as "#RRGGBBAA".
static void ECSIServices_PushColor(lua_State *state, ECSColor color)
{
    char text[OPENECS_COLOR_TEXT_SIZE];
    ECSIColor_Format(color, text);
    lua_pushstring(state, text);
}

/// @brief Reads a Lua argument into a call slot. Raises a Lua error if it has the wrong type; values are read later, because they allocate.
static void ECSIServices_CheckArgument(lua_State *state, int index, ECSIParameter parameter, ECSISlot *retSlot)
{
    switch (parameter.type)
    {
    case ECSIParameterType_Handle:
        retSlot->pointer = ECSIServices_CheckHandle(index, parameter.handle->name);
        break;
    case ECSIParameterType_Bool:
        retSlot->boolean = (u8)(lua_toboolean(state, index) != 0);
        break;
    case ECSIParameterType_Int:
    {
        lua_Integer integer = luaL_checkinteger(state, index);
        luaL_argcheck(state, integer >= SDL_MIN_SINT32 && integer <= SDL_MAX_SINT32, index, "does not fit in an int");
        retSlot->integer = (i32)integer;
        break;
    }
    case ECSIParameterType_Int64:
        retSlot->integer64 = (i64)luaL_checkinteger(state, index);
        break;
    case ECSIParameterType_Float:
        retSlot->single = (f32)luaL_checknumber(state, index);
        break;
    case ECSIParameterType_Double:
        retSlot->number = (f64)luaL_checknumber(state, index);
        break;
    case ECSIParameterType_Color:
        luaL_argcheck(state, ECSColor_Parse(luaL_checkstring(state, index), &retSlot->color), index, "is not a colour: \"#RRGGBB\", \"#RRGGBBAA\" or a colour of the theme, such as \"text\"");
        break;
    case ECSIParameterType_String:
        // valid during the call, because the argument stays on the stack
        retSlot->string = luaL_checkstring(state, index);
        break;
    case ECSIParameterType_Callback:
        // the closure is made later, because it allocates
        if (!lua_isnoneornil(state, index))
        {
            luaL_checktype(state, index, LUA_TFUNCTION);
        }

        retSlot->pointer = nullptr;
        break;
    case ECSIParameterType_Struct:
    {
        // the struct's memory is a userdata, so it lives on the stack during the call; C gets a pointer to it
        const char *field = nullptr;
        void *memory = lua_newuserdatauv(state, parameter.structType->size, 0);
        SDL_memset(memory, 0, parameter.structType->size);

        if (!ECSIStructs_Read(state, index, parameter.structType, memory, &field))
        {
            const char *problem = field == nullptr ? lua_pushfstring(state, "is not a %s, a table of its fields", parameter.structType->name)
                                                   : lua_pushfstring(state, "is not a %s: its field '%s' is missing or of another type", parameter.structType->name, field);
            luaL_argerror(state, index, problem);
        }

        retSlot->pointer = memory;
        break;
    }
    case ECSIParameterType_Buffer:
    {
        // a copy, because the function may write into a buffer and Lua strings must not change
        usz length = 0;
        const char *bytes = luaL_checklstring(state, index, &length);
        void *copy = lua_newuserdatauv(state, length + 1, 0);
        SDL_memcpy(copy, bytes, length);
        retSlot->buffer = cs(copy, length);
        break;
    }
    default:
        break;
    }
}

/// @brief Narrows a C function's result for ECSIServices_PushArgument: libffi gives an integral result smaller than a register as a whole register.
static void ECSIServices_NarrowResult(ECSIParameter result, ECSISlot *slot)
{
    switch (result.type)
    {
    case ECSIParameterType_Bool:
        slot->boolean = (u8)slot->integral;
        break;
    case ECSIParameterType_Int:
        slot->integer = (i32)slot->integral;
        break;
    case ECSIParameterType_Color:
        slot->color = (ECSColor)slot->integral;
        break;
    default:
        break;
    }
}

/// @brief Checks the arguments of a call from Lua and puts them in the call's parameters; an output parameter points to where C writes it. Raises a Lua error for a wrong argument.
static void ECSIServices_CheckArguments(lua_State *state, const ECSISignature *signature, ECSICallParameter *parameters, void **arguments)
{
    int index = 1;

    for (usz i = 0; i < arrlenu(signature->parameters); i++)
    {
        ECSICallParameter *call = &parameters[i];
        *call = (ECSICallParameter){0};
        arguments[i] = &call->slot;

        if (signature->parameters[i].out && signature->parameters[i].type == ECSIParameterType_Struct)
        {
            // an output struct is memory of its size, which C fills through the pointer it gets
            void *memory = lua_newuserdatauv(state, signature->parameters[i].structType->size, 0);
            SDL_memset(memory, 0, signature->parameters[i].structType->size);
            call->slot.pointer = memory;
            call->output.pointer = memory;
        }
        else if (signature->parameters[i].out)
        {
            call->slot.pointer = &call->output;
        }
        else
        {
            call->index = index++;
            ECSIServices_CheckArgument(state, call->index, signature->parameters[i], &call->slot);
        }
    }
}

/// @brief Makes the values that a call passes: the Lua arguments as values, and empty values for the outputs.
static SHUResult ECSIServices_MakeValues(lua_State *state, const ECSISignature *signature, ECSICallParameter *parameters)
{
    SHUResult result = SHUResult_Ok;

    for (usz i = 0; i < arrlenu(signature->parameters); i++)
    {
        ECSIParameter parameter = signature->parameters[i];

        if (parameter.type != ECSIParameterType_Value)
        {
            continue;
        }

        ECSValue *value = nullptr;
        result = result ? result : ECSValue_Create(&value);
        result = result || parameter.out ? result : ECSILua_GetValue(state, parameters[i].index, value);
        parameters[i].slot.pointer = value;

        if (parameter.out)
        {
            parameters[i].output.pointer = value;
        }
    }

    return result;
}

/// @brief Makes the Lua functions that a call passes into closures that C calls. A closure is valid until the call returns, or until C releases it if C keeps it.
static SHUResult ECSIServices_MakeCallbacks(lua_State *state, const ECSIFunction *function, ECSPlugin caller, ECSICallParameter *parameters)
{
    const ECSISignature *signature = &function->signature;

    for (usz i = 0; i < arrlenu(signature->parameters); i++)
    {
        ECSICallParameter *call = &parameters[i];

        if (signature->parameters[i].type != ECSIParameterType_Callback || lua_isnoneornil(state, call->index))
        {
            continue;
        }

        SHU_ReturnResult(ECSIServices_MakeCallback(state, call->index, function, caller, signature->parameters[i].callback, &call->callback));

        // C gets the closure's code; POSIX lets a data pointer hold code
        SDL_memcpy(&call->slot.pointer, &call->callback->pointer, sizeof(call->slot.pointer));

        // a kept closure is the map's from now on, so C may release it during the call
        if (signature->parameters[i].keep)
        {
            hmput(SERVICES.kept, call->slot.pointer, call->callback);
        }
    }

    return SHUResult_Ok;
}

/// @brief Pushes what a call gives back to Lua: its result, then its output parameters.
/// @return The number of values pushed.
static int ECSIServices_PushResults(lua_State *state, const ECSISignature *signature, const ECSICallParameter *parameters, ECSISlot *returned)
{
    int pushed = 0;

    if (signature->result.type != ECSIParameterType_Void)
    {
        ECSIServices_NarrowResult(signature->result, returned);
        ECSIServices_PushArgument(state, signature->result, returned);
        pushed++;
    }

    for (usz i = 0; i < arrlenu(signature->parameters); i++)
    {
        if (signature->parameters[i].out)
        {
            ECSIServices_PushArgument(state, signature->parameters[i], &parameters[i].output);
            pushed++;
        }
    }

    return pushed;
}

/// @brief Frees the values and closures of a call. A kept closure that the call got is C's once the call ran, released or not; when the call did not run, nothing can release it.
static void ECSIServices_FreeCallParameters(const ECSISignature *signature, ECSICallParameter *parameters, bool called)
{
    for (usz i = 0; i < arrlenu(signature->parameters); i++)
    {
        ECSICallParameter *call = &parameters[i];
        bool keep = signature->parameters[i].keep;

        if (signature->parameters[i].type == ECSIParameterType_Value)
        {
            ECSValue_Destroy((ECSValue **)&call->slot.pointer);
        }

        if (call->callback == nullptr || (keep && called))
        {
            continue;
        }

        if (keep)
        {
            (void)hmdel(SERVICES.kept, call->slot.pointer);
        }

        ECSIServices_FreeCallback(&call->callback);
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
    int inputs = (int)(count - signature->outCount);

    if (lua_gettop(state) < inputs)
    {
        lua_settop(state, inputs);
    }

    // the storage is a userdata, so it is freed even when an argument raises an error; libffi takes the arguments as an array of pointers
    ECSICallParameter *parameters = lua_newuserdatauv(state, count * (sizeof(ECSICallParameter) + sizeof(void *)) + 1, 0);
    void **arguments = (void **)(parameters + count);
    ECSIServices_CheckArguments(state, signature, parameters, arguments);

    // values allocate, so they are made after every check that can raise an error
    SHUResult result = ECSIServices_MakeValues(state, signature, parameters);
    result = result ? result : ECSIServices_MakeCallbacks(state, function, caller, parameters);
    int pushed = 0;

    if (!result)
    {
        ECSISlot returned = {0};
        ffi_call((ffi_cif *)&signature->cif, FFI_FN(function->pointer), &returned, arguments);
        pushed = ECSIServices_PushResults(state, signature, parameters, &returned);
    }

    ECSIServices_FreeCallParameters(signature, parameters, !result);

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

static int ECSIServices_HandleToString(lua_State *state)
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

/// @brief Raises a Lua error unless a handle type is one that a Lua plugin registered.
static void ECSIServices_CheckLuaHandleType(lua_State *state, ECSPlugin plugin, const char *name)
{
    ECSIHandleType *type = ECSIServices_FindHandleType(name);

    if (type == nullptr || type->plugin != plugin || type->Destroy != ECSIServices_DestroyLuaObject)
    {
        luaL_error(state, "handle type '%s' is not one that this plugin registered in Lua", name);
    }
}

static SHUResult ECSIServices_RegisterHandleType(ECSPlugin plugin, const char *name, ECSHandleDestroyFunction Destroy)
{
    if (ECSIServices_FindHandleType(name) != nullptr || ECSIServices_FindStructType(name) != nullptr)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Type '%s' is already registered.", name);
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
    lua_pushcfunction(state, ECSIServices_HandleToString);
    lua_setfield(state, -2, "__tostring");
    lua_pop(state, 1);

    shput(SERVICES.handleTypes, type->name, type);
    return SHUResult_Ok;
}

/// @brief Makes every Lua handle of a type invalid, and destroys each object once with its type's Destroy, if it has one. nullptr is every type.
static void ECSIServices_DropHandles(const ECSIHandleType *type)
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

        if (objectType != nullptr && objectType->Destroy != nullptr)
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
        ECSILua_PushValue(state, *(const ECSValue *const *)argument);
        break;
    case ECSIParameterType_Struct:
        // C passes a struct by its pointer; an output's storage holds the pointer too
        ECSIStructs_Push(state, parameter.structType, *(const void *const *)argument);
        break;
    default:
        lua_pushnil(state);
        break;
    }
}

/// @brief Writes a string that a Lua function gave, at an index of the stack, where C reads it: as a string, or as a buffer.
/// The anchor keeps the string alive; a buffer gets a copy, because C may write into it.
/// @param anchors Index of the table that keeps strings and buffers alive until the function returns again.
/// @return false if the Lua value is not a string.
static bool ECSIServices_WriteBytes(lua_State *state, int index, int anchors, bool buffer, void *retTarget)
{
    bool text = lua_type(state, index) == LUA_TSTRING;
    usz length = 0;
    const char *bytes = text ? lua_tolstring(state, index, &length) : nullptr;

    if (buffer && text)
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

    if (buffer)
    {
        *(SHUSlice *)retTarget = cs((void *)bytes, length);
    }
    else
    {
        *(const char **)retTarget = bytes;
    }

    return text;
}

/// @brief Writes a value that a Lua function gave, at an index of the stack, where C reads it: its return value or an output.
/// @param anchors Index of the table that keeps strings and buffers alive until the function returns again.
/// @return false if the Lua value has the wrong type.
static bool ECSIServices_WriteOutput(lua_State *state, ECSIFunction *function, int index, int anchors, ECSIParameter parameter, void *retTarget, bool isReturnValue)
{
    int isNumber = 0;
    ECSIParameterType type = parameter.type;

    switch (type)
    {
    case ECSIParameterType_Handle:
    {
        // the handle stays alive in the anchors while C may use its object
        ECSIHandle *handle = luaL_testudata(state, index, parameter.handle->metatable);
        *(void **)retTarget = handle == nullptr ? nullptr : handle->object;
        lua_pushvalue(state, index);
        lua_rawseti(state, anchors, (lua_Integer)lua_rawlen(state, anchors) + 1);
        return handle != nullptr || lua_isnil(state, index);
    }
    case ECSIParameterType_Bool:
        if (isReturnValue)
        {
            *(ffi_arg *)retTarget = (ffi_arg)lua_toboolean(state, index);
        }
        else
        {
            *(u8 *)retTarget = (u8)(lua_toboolean(state, index) != 0);
        }

        return true;
    case ECSIParameterType_Int:
    {
        lua_Integer integer = lua_tointegerx(state, index, &isNumber);

        if (isReturnValue)
        {
            *(ffi_sarg *)retTarget = (ffi_sarg)(i32)integer;
        }
        else
        {
            *(i32 *)retTarget = (i32)integer;
        }

        return isNumber && integer >= SDL_MIN_SINT32 && integer <= SDL_MAX_SINT32;
    }
    case ECSIParameterType_Int64:
        *(i64 *)retTarget = (i64)lua_tointegerx(state, index, &isNumber);
        return isNumber;
    case ECSIParameterType_Float:
        *(f32 *)retTarget = (f32)lua_tonumberx(state, index, &isNumber);
        return isNumber;
    case ECSIParameterType_Double:
        *(f64 *)retTarget = (f64)lua_tonumberx(state, index, &isNumber);
        return isNumber;
    case ECSIParameterType_Color:
    {
        ECSColor color = 0;
        bool valid = lua_type(state, index) == LUA_TSTRING && ECSColor_Parse(lua_tostring(state, index), &color);

        if (isReturnValue)
        {
            *(ffi_arg *)retTarget = (ffi_arg)color;
        }
        else
        {
            *(ECSColor *)retTarget = color;
        }

        return valid;
    }
    case ECSIParameterType_String:
    case ECSIParameterType_Buffer:
        return ECSIServices_WriteBytes(state, index, anchors, type == ECSIParameterType_Buffer, retTarget);
    case ECSIParameterType_Value:
    {
        // a result value is kept until the function returns again; an output fills the caller's value
        ECSValue *value = isReturnValue ? function->result : retTarget;

        if (isReturnValue)
        {
            *(const ECSValue **)retTarget = value;
        }

        return value != nullptr && ECSILua_GetValue(state, index, value) == SHUResult_Ok;
    }
    case ECSIParameterType_Struct:
    {
        // an output struct is the caller's memory, which the table's fields fill
        const char *field = nullptr;
        return ECSIStructs_Read(state, index, parameter.structType, retTarget, &field);
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
    SDL_memset(result, 0, SDL_max(sizeof(ffi_arg), ECSIServices_GetFfiType(signature->result)->size));

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

        if (parameter.type == ECSIParameterType_Callback)
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

    bool failed = ECSILua_Call(state, pushed, resultCount) != SHUResult_Ok;
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

/// @brief Makes the code that C calls for a Lua function, which calls the Lua function at an index of the stack, and keeps the Lua function and a table of anchors in the registry.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation; the function's closure, if it got one, is freed with the function then.
static SHUResult ECSIFunction_MakeClosure(lua_State *state, int index, ECSIFunction *function)
{
    void *code = nullptr;
    function->closure = ffi_closure_alloc(sizeof(ffi_closure), &code);

    if (function->closure == nullptr || ffi_prep_closure_loc(function->closure, &function->signature.cif, ECSIServices_CallLua, function, code) != FFI_OK)
    {
        return SHUResult_ErrAllocation;
    }

    // C calls the closure's code; POSIX lets a data pointer hold code, but ISO C has no cast for it
    SDL_memcpy(&function->pointer, &code, sizeof(code));
    lua_pushvalue(state, index);
    function->lua = luaL_ref(state, LUA_REGISTRYINDEX);
    lua_newtable(state);
    function->anchors = luaL_ref(state, LUA_REGISTRYINDEX);
    return SHUResult_Ok;
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

    if (ECSIFunction_MakeClosure(state, index, callback))
    {
        if (callback->closure != nullptr)
        {
            ffi_closure_free(callback->closure);
        }

        SDL_free(callback->name);
        SDL_free(callback);
        return SHUResult_ErrAllocation;
    }

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

/// @brief Finds what a line of a function's description documents: the result for "@return text", or the parameter it names for "@param name text".
/// @param retText Where the line's text after the tag and name starts.
/// @return The result or the parameter, or nullptr when the line is neither, or names no parameter of the signature.
static ECSIParameter *ECSIServices_FindDocumented(ECSISignature *signature, const char *line, const char **retText)
{
    if (SDL_strncmp(line, "@return ", 8) == 0)
    {
        *retText = line + 8;
        return &signature->result;
    }

    if (SDL_strncmp(line, "@param ", 7) != 0)
    {
        return nullptr;
    }

    const char *name = line + 7;
    const char *space = SDL_strchr(name, ' ');
    usz length = space == nullptr ? SDL_strlen(name) : (usz)(space - name);
    *retText = name[length] == ' ' ? name + length + 1 : name + length;

    for (usz i = 0; i < arrlenu(signature->parameters); i++)
    {
        const char *given = signature->parameters[i].name;

        if (given != nullptr && SDL_strlen(given) == length && SDL_strncmp(given, name, length) == 0)
        {
            return &signature->parameters[i];
        }
    }

    return nullptr;
}

/// @brief Reads the lines of a function's description after its first: "@param name text" says what a parameter is, and "@return text" what the
/// function gives. The first line stays the description, which menus show. A line that names no parameter of the signature is reported and skipped.
static SHUResult ECSIServices_ReadDocumentation(ECSIFunction *function)
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

        const char *text = nullptr;
        ECSIParameter *target = ECSIServices_FindDocumented(&function->signature, line, &text);

        if (target == nullptr || target->documentation != nullptr || text[0] == '\0')
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "The description of '%s' has a line that is not \"@param name text\" of a parameter it names once, or \"@return text\" once: '%s'", function->name, line);
        }
        else
        {
            target->documentation = SDL_strdup(text);

            if (target->documentation == nullptr)
            {
                return SHUResult_ErrAllocation;
            }
        }

        line = next;
    }

    return SHUResult_Ok;
}

/// @brief Tells whether a name is a namespace of another: the other starts with it and a dot, as parse.json is of parse.json.read.
static bool ECSIServices_IsNamespaceOf(const char *name, const char *other)
{
    usz length = SDL_strlen(name);
    return SDL_strncmp(name, other, length) == 0 && other[length] == '.';
}

/// @brief Finds a function of a plugin whose name is a namespace of a name, or has the name as its namespace; Lua's table of the plugin cannot hold both.
static const ECSIFunction *ECSIServices_FindNamespaceClash(ECSPlugin plugin, const char *name)
{
    for (usz i = 0; i < shlenu(SERVICES.functions); i++)
    {
        const ECSIFunction *function = SERVICES.functions[i].value;

        if (function->plugin == plugin && (ECSIServices_IsNamespaceOf(function->name, name) || ECSIServices_IsNamespaceOf(name, function->name)))
        {
            return function;
        }
    }

    return nullptr;
}

/// @brief Checks a function's name and makes its record, without its code.
static SHUResult ECSIServices_CreateFunction(ECSPlugin plugin, const char *name, const char *signature, const char *description, ECSIFunction **retFunction)
{
    // the core's own names start with "ecs."
    if (plugin == nullptr ? SDL_strncmp(name, "ecs.", 4) != 0 : !ECSIPlugin_OwnsName(plugin, name))
    {
        return SHUResult_ErrBadData;
    }

    if (ECSIServices_FindFunction(name) != nullptr)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Function '%s' is already registered.", name);
        return SHUResult_ErrBadData;
    }

    const ECSIFunction *clash = plugin == nullptr ? nullptr : ECSIServices_FindNamespaceClash(plugin, name);

    if (clash != nullptr)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Function '%s' cannot be registered: '%s' is a function, and a name's part before a dot is a namespace.", name, clash->name);
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

    if (!result && (result = ECSIServices_ReadDocumentation(function)))
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

#pragma region Definitions

/// @brief The core's handle types, with their names in Lua's definitions and in C.
static const struct
{
    const char *name;
    const char *luaClass;
    const char *cType; // ends with a space or a '*', so the name follows it
} OPENECS_CORE_HANDLES[] = {
    {"ecs.panel", "ecs.Panel", "ECSPanel "},
    {"ecs.popup", "ecs.Popup", "ECSPopup "},
    {"ecs.surface", "ecs.Surface", "ECSSurface *"},
};

/// @brief Finds one of the core's handle types.
/// @return Its position in OPENECS_CORE_HANDLES, or -1 for another type.
static i32 ECSIDefinition_FindCoreHandle(const ECSIHandleType *type)
{
    for (usz i = 0; i < SDL_arraysize(OPENECS_CORE_HANDLES); i++)
    {
        if (SDL_strcmp(type->name, OPENECS_CORE_HANDLES[i].name) == 0)
        {
            return (i32)i;
        }
    }

    return -1;
}

/// @brief Names a parameter in Lua: the name its signature gives it, or arg1, arg2 and so on.
/// @param retName Room for a name that the signature does not give.
static const char *ECSIDefinition_GetLuaName(ECSIParameter parameter, usz index, SHUSlice retName)
{
    if (parameter.name != nullptr)
    {
        return parameter.name;
    }

    SDL_snprintf(retName.data, retName.size, "arg%zu", index + 1);
    return retName.data;
}

/// @brief Names a parameter in C: as in Lua, but an output is "ret" and its name with a capital first letter, such as retWidth.
/// @param retName Room for the name.
static const char *ECSIDefinition_GetCName(ECSIParameter parameter, usz index, SHUSlice retName)
{
    char luaName[64];
    const char *name = ECSIDefinition_GetLuaName(parameter, index, cs(luaName, sizeof(luaName)));

    if (!parameter.out)
    {
        return name;
    }

    SDL_snprintf(retName.data, retName.size, "ret%c%s", SDL_toupper((unsigned char)name[0]), name + 1);
    return retName.data;
}

static void ECSIDefinition_AddLuaType(ECSIText *text, ECSIParameter parameter);

/// @brief Writes a name in PascalCase, such as CglmVec3 for cglm.vec3, for C's names of plugins and types.
static void ECSIDefinition_ToPascalCase(const char *name, SHUSlice retName);

/// @brief Adds the result and the outputs of a Lua function: the result first, then each output.
/// @param separator What comes before the first of them.
static void ECSIDefinition_AddLuaResults(ECSIText *text, const ECSISignature *signature, const char *separator)
{
    if (signature->result.type != ECSIParameterType_Void)
    {
        ECSIText_Add(text, "%s", separator);
        ECSIDefinition_AddLuaType(text, signature->result);
        separator = ", ";
    }

    for (usz i = 0; i < arrlenu(signature->parameters); i++)
    {
        if (signature->parameters[i].out)
        {
            ECSIText_Add(text, "%s", separator);
            ECSIDefinition_AddLuaType(text, signature->parameters[i]);
            separator = ", ";
        }
    }
}

/// @brief Adds a callback's type for LuaLS, such as "fun(path: string): integer".
static void ECSIDefinition_AddLuaFunction(ECSIText *text, const ECSISignature *signature)
{
    char name[32];
    const char *separator = "";
    ECSIText_Add(text, "fun(");

    for (usz i = 0; i < arrlenu(signature->parameters); i++)
    {
        if (!signature->parameters[i].out)
        {
            ECSIText_Add(text, "%s%s: ", separator, ECSIDefinition_GetLuaName(signature->parameters[i], i, cs(name, sizeof(name))));
            ECSIDefinition_AddLuaType(text, signature->parameters[i]);
            separator = ", ";
        }
    }

    ECSIText_Add(text, ")");
    ECSIDefinition_AddLuaResults(text, signature, ": ");
}

/// @brief Adds a parameter's type for LuaLS.
static void ECSIDefinition_AddLuaType(ECSIText *text, ECSIParameter parameter)
{
    static const char *const types[ECSIParameterType_Count] = {"nil", "boolean", "integer", "integer", "number", "number", "ecs.Color", "string", "string", "any", "userdata", "function", "table"};

    if (parameter.type == ECSIParameterType_Callback)
    {
        ECSIText_Add(text, "(");
        ECSIDefinition_AddLuaFunction(text, parameter.callback);
        ECSIText_Add(text, ")?");
    }
    else if (parameter.type == ECSIParameterType_Handle)
    {
        // a plugin's handle type is a class of its definitions, with its methods
        i32 core = ECSIDefinition_FindCoreHandle(parameter.handle);
        ECSIText_Add(text, "%s", core >= 0 ? OPENECS_CORE_HANDLES[core].luaClass : parameter.handle->name);
    }
    else if (parameter.type == ECSIParameterType_Value && parameter.luaType != nullptr)
    {
        ECSIText_Add(text, "%s", parameter.luaType);
    }
    else if (parameter.type == ECSIParameterType_Struct)
    {
        // a struct type is a class of its plugin's definitions, with its fields and methods
        ECSIText_Add(text, "%s", parameter.structType->name);
    }
    else
    {
        ECSIText_Add(text, "%s", types[parameter.type]);
    }
}

/// @brief Adds a parameter's C declaration, such as "f32 x", "const char **retName" or "void (*callback)(i32 arg1)".
static void ECSIDefinition_AddCDeclaration(ECSIText *text, ECSIParameter parameter, const char *name);

/// @brief Adds a callback's parameter list in C, such as "(i32 arg1, f32 *retWidth)".
static void ECSIDefinition_AddCParameters(ECSIText *text, const ECSISignature *signature)
{
    char name[64];
    ECSIText_Add(text, "(");

    for (usz i = 0; i < arrlenu(signature->parameters); i++)
    {
        ECSIText_Add(text, "%s", i == 0 ? "" : ", ");
        ECSIDefinition_AddCDeclaration(text, signature->parameters[i], ECSIDefinition_GetCName(signature->parameters[i], i, cs(name, sizeof(name))));
    }

    ECSIText_Add(text, "%s)", arrlenu(signature->parameters) == 0 ? "void" : "");
}

static void ECSIDefinition_AddCDeclaration(ECSIText *text, ECSIParameter parameter, const char *name)
{
    static const char *const types[ECSIParameterType_Count] = {"void", "bool", "i32", "i64", "f32", "f64", "ECSColor", "const char *", "SHUSlice ", "const ECSValue *", "void *", "", ""};
    const char *type = types[parameter.type];

    if (parameter.type == ECSIParameterType_Callback)
    {
        char declarator[80];
        SDL_snprintf(declarator, sizeof(declarator), "(*%s)", name);
        ECSIDefinition_AddCDeclaration(text, parameter.callback->result, declarator);
        ECSIDefinition_AddCParameters(text, parameter.callback);
        return;
    }

    if (parameter.type == ECSIParameterType_Handle)
    {
        i32 core = ECSIDefinition_FindCoreHandle(parameter.handle);
        type = core >= 0 ? OPENECS_CORE_HANDLES[core].cType : "void *";
    }
    else if (parameter.type == ECSIParameterType_Value && parameter.out)
    {
        // an out value is a value that the caller gives and the function fills
        ECSIText_Add(text, "ECSValue *%s", name);
        return;
    }
    else if (parameter.type == ECSIParameterType_Struct)
    {
        // a struct passes by its pointer: one the function reads, or for an output one it fills
        char structName[256];
        ECSIDefinition_ToPascalCase(parameter.structType->name, cs(structName, sizeof(structName)));
        ECSIText_Add(text, "%s%s *%s", parameter.out ? "" : "const ", structName, name);
        return;
    }

    // a type that ends with a pointer or a space needs no space before the name
    usz length = SDL_strlen(type);
    bool joined = length > 0 && (type[length - 1] == '*' || type[length - 1] == ' ');
    ECSIText_Add(text, "%s%s%s%s", type, joined ? "" : " ", parameter.out ? "*" : "", name);
}

/// @brief Adds a file of a plugin's folder to the end of a definition, if the plugin has it.
/// @param comment How a line comment starts in the definition's language.
static void ECSIDefinition_AddFile(ECSIText *text, ECSPlugin provider, const char *name, const char *comment)
{
    char *path = nullptr;
    usz size = 0;
    char *contents = SDL_asprintf(&path, "%s%s", ECSPlugin_GetFolder(provider), name) < 0 ? nullptr : SDL_LoadFile(path, &size);

    if (contents != nullptr)
    {
        ECSIText_Add(text, "\n%s the plugin's own, from its %s\n%s%s", comment, name, contents, size > 0 && contents[size - 1] == '\n' ? "" : "\n");
    }

    SDL_free(contents);
    SDL_free(path);
}

/// @brief Writes a definition file, and frees its text.
static SHUResult ECSIDefinition_Save(const char *folder, const char *name, const char *extension, ECSIText *text)
{
    char *path = nullptr;

    if (text->failed || SDL_asprintf(&path, "%s%s%s", folder, name, extension) < 0)
    {
        SDL_free(text->data);
        return SHUResult_ErrAllocation;
    }

    bool saved = SDL_SaveFile(path, text->data, SDL_strlen(text->data));

    if (!saved)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot write '%s': %s", path, SDL_GetError());
    }

    SDL_free(path);
    SDL_free(text->data);
    return saved ? SHUResult_Ok : SHUResult_ErrFile;
}

/// @brief Turns a plugin's name into the start of C names: "sketch_c" becomes "SketchC".
static void ECSIDefinition_ToPascalCase(const char *name, SHUSlice retName)
{
    char *written = retName.data;
    usz length = 0;
    bool upper = true;

    for (const char *character = name; *character != '\0' && length + 1 < retName.size; character++)
    {
        if (!SDL_isalnum((unsigned char)*character))
        {
            upper = true;
            continue;
        }

        written[length++] = upper ? (char)SDL_toupper((unsigned char)*character) : *character;
        upper = false;
    }

    written[length] = '\0';
}

/// @brief Turns a function's local name into a C member name: dots become underscores.
static void ECSIDefinition_ToMemberName(const char *local, SHUSlice retName)
{
    SDL_strlcpy(retName.data, local, retName.size);

    for (char *character = retName.data; *character != '\0'; character++)
    {
        if (*character == '.')
        {
            *character = '_';
        }
    }
}

/// @brief Adds a ---@return line for a result or an output; LuaLS reads a description after a name, or after # without one.
static void ECSIDefinition_AddLuaReturn(ECSIText *lua, ECSIParameter result)
{
    ECSIText_Add(lua, "---@return ");
    ECSIDefinition_AddLuaType(lua, result);

    if (result.name != nullptr)
    {
        ECSIText_Add(lua, " %s", result.name);
    }

    if (result.documentation != nullptr)
    {
        ECSIText_Add(lua, "%s%s", result.name == nullptr ? " # " : " ", result.documentation);
    }

    ECSIText_Add(lua, "\n");
}

/// @brief Adds a function to a Lua definition: its description, parameters, result and outputs, then the function, as a field of a table or a method.
/// @param owner The table, such as "Web" or "WebSocket".
/// @param name The function's name in the table.
/// @param method Whether it is a method, whose first parameter is the handle it is called on.
static void ECSIDefinition_AddLuaFunctionOf(ECSIText *lua, const ECSIFunction *function, const char *owner, const char *name, bool method)
{
    const ECSISignature *signature = &function->signature;
    usz first = method ? 1 : 0;
    char parameterName[32];
    ECSIText_Add(lua, "\n---%s\n", function->description);

    for (usz i = first; i < arrlenu(signature->parameters); i++)
    {
        ECSIParameter parameter = signature->parameters[i];

        if (!parameter.out)
        {
            ECSIText_Add(lua, "---@param %s ", ECSIDefinition_GetLuaName(parameter, i, cs(parameterName, sizeof(parameterName))));
            ECSIDefinition_AddLuaType(lua, parameter);
            ECSIText_Add(lua, "%s%s\n", parameter.documentation == nullptr ? "" : " ", parameter.documentation == nullptr ? "" : parameter.documentation);
        }
    }

    // the result first, then the outputs
    if (signature->result.type != ECSIParameterType_Void)
    {
        ECSIDefinition_AddLuaReturn(lua, signature->result);
    }

    for (usz i = 0; i < arrlenu(signature->parameters); i++)
    {
        if (signature->parameters[i].out)
        {
            ECSIDefinition_AddLuaReturn(lua, signature->parameters[i]);
        }
    }

    // a local name with dots is in the namespaces that its parts name, such as json.read
    ECSIText_Add(lua, method ? "function %s:%s(" : "function %s.%s(", owner, name);

    const char *separator = "";

    for (usz i = first; i < arrlenu(signature->parameters); i++)
    {
        if (!signature->parameters[i].out)
        {
            ECSIText_Add(lua, "%s%s", separator, ECSIDefinition_GetLuaName(signature->parameters[i], i, cs(parameterName, sizeof(parameterName))));
            separator = ", ";
        }
    }

    ECSIText_Add(lua, ") end\n");
}

/// @brief Declares the namespaces of a function's name that the function before it, in sorted order, did not declare: each a class, such as parse.json, and a table of the plugin's table.
static void ECSIDefinition_AddLuaNamespaces(ECSIText *lua, const char *owner, const char *name, const char *previous)
{
    const char *local = SDL_strchr(name, '.') + 1;

    for (const char *dot = SDL_strchr(local, '.'); dot != nullptr; dot = SDL_strchr(dot + 1, '.'))
    {
        usz length = (usz)(dot - name);

        // the previous function has this namespace too, so it is declared already
        if (SDL_strncmp(name, previous, length) == 0 && previous[length] == '.')
        {
            continue;
        }

        const char *plugin = SDL_strchr(name, '.');
        ECSIText_Add(lua, "\n---The functions of %.*s.\n---@class %.*s\n%s%.*s = {}\n", (int)length, name, (int)length, name, owner, (int)(dot - plugin), plugin);
    }
}

/// @brief Adds the Lua type of a struct field's one value.
static void ECSIDefinition_AddLuaFieldType(ECSIText *lua, const ECSIStructField *field)
{
    ECSIParameter parameter = {.type = field->type, .structType = (ECSIStructType *)field->structType};
    ECSIDefinition_AddLuaType(lua, parameter);
}

/// @brief Adds a struct type's class: its fields, a list's values, the operators its methods make, and its methods.
static void ECSIDefinition_AddLuaStruct(ECSIText *lua, const ECSIStructType *type)
{
    char className[256];
    ECSIDefinition_ToPascalCase(type->name, cs(className, sizeof(className)));
    bool list = type->fields[0].name == nullptr;
    ECSIText_Add(lua, "\n---A struct of the type %s: plain data, which a table of its fields stands for.\n---@class %s", type->name, type->name);

    if (list)
    {
        ECSIText_Add(lua, ": { [integer]: ");
        ECSIDefinition_AddLuaFieldType(lua, &type->fields[0]);
        ECSIText_Add(lua, " }");
    }

    ECSIText_Add(lua, "\n");

    for (usz i = 0; !list && i < arrlenu(type->fields); i++)
    {
        ECSIText_Add(lua, "---@field %s ", type->fields[i].name);
        ECSIDefinition_AddLuaFieldType(lua, &type->fields[i]);
        ECSIText_Add(lua, "%s\n", type->fields[i].count > 0 ? "[]" : "");
    }

    // an operator's result is its function's first output, or its result, as Lua gets it
    for (usz i = 0; i < arrlenu(type->methods); i++)
    {
        for (usz j = 0; j < SDL_arraysize(OPENECS_STRUCT_OPERATORS); j++)
        {
            const ECSISignature *signature = &type->targets[i]->signature;

            if (SDL_strcmp(type->methods[i], OPENECS_STRUCT_OPERATORS[j][0]) != 0 || SDL_strcmp(type->methods[i], "tostring") == 0 || SDL_strcmp(type->methods[i], "eq") == 0)
            {
                continue;
            }

            ECSIText_Add(lua, "---@operator %s", type->methods[i]);

            if (arrlenu(signature->parameters) > 1 && !signature->parameters[1].out)
            {
                ECSIText_Add(lua, "(");
                ECSIDefinition_AddLuaType(lua, signature->parameters[1]);
                ECSIText_Add(lua, ")");
            }

            ECSIText_Add(lua, ": ");
            ECSIDefinition_AddLuaResults(lua, signature, "");
            ECSIText_Add(lua, "\n");
        }
    }

    ECSIText_Add(lua, "local %s = {}\n", className);

    for (usz i = 0; i < arrlenu(type->methods); i++)
    {
        ECSIDefinition_AddLuaFunctionOf(lua, type->targets[i], className, type->methods[i], true);
    }
}

/// @brief Writes the Lua definitions of a plugin's functions and handle types, for editors.
static int ECSIDefinition_CompareFunctions(const void *first, const void *second)
{
    return SDL_strcmp((*(const ECSIFunction *const *)first)->name, (*(const ECSIFunction *const *)second)->name);
}

/// @brief Gives a plugin's functions sorted by name, so its definition files are the same each time they are written.
/// @return A stb_ds array; free it with arrfree.
static const ECSIFunction **ECSIDefinition_GetFunctions(ECSPlugin provider)
{
    const ECSIFunction **functions = nullptr;

    for (usz i = 0; i < shlenu(SERVICES.functions); i++)
    {
        if (SERVICES.functions[i].value->plugin == provider)
        {
            arrput(functions, SERVICES.functions[i].value);
        }
    }

    SDL_qsort(functions, arrlenu(functions), sizeof(const ECSIFunction *), ECSIDefinition_CompareFunctions);
    return functions;
}

static SHUResult ECSIDefinition_WriteLua(ECSPlugin provider, const char *folder)
{
    const char *plugin = ECSIPlugin_GetName(provider);
    char pascal[128];
    ECSIDefinition_ToPascalCase(plugin, cs(pascal, sizeof(pascal)));

    ECSIText lua = {0};
    ECSIText_Add(&lua, "---@meta %s\n-- The functions of the plugin %s, for editors. OpenECS --definitions wrote this file.\n-- A Lua plugin whose manifest depends on %s gets them with require(\"%s\").\n\n---@class %s\nlocal %s = {}\n", plugin, plugin, plugin, plugin, plugin, pascal);

    const ECSIFunction **functions = ECSIDefinition_GetFunctions(provider);

    for (usz i = 0; i < arrlenu(functions); i++)
    {
        const char *local = SDL_strchr(functions[i]->name, '.') + 1;
        ECSIDefinition_AddLuaNamespaces(&lua, pascal, functions[i]->name, i > 0 ? functions[i - 1]->name : "");
        ECSIDefinition_AddLuaFunctionOf(&lua, functions[i], pascal, local, false);
    }

    arrfree(functions);

    // each of the plugin's struct types is a class of its fields, with its operators and methods
    for (usz i = 0; i < shlenu(SERVICES.structTypes); i++)
    {
        const ECSIStructType *type = SERVICES.structTypes[i].value;

        if (type->plugin == provider)
        {
            ECSIDefinition_AddLuaStruct(&lua, type);
        }
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
        ECSIDefinition_ToPascalCase(type->name, cs(className, sizeof(className)));
        ECSIText_Add(&lua, "\n---A handle of the type %s.\n---@class %s\nlocal %s = {}\n", type->name, type->name, className);

        for (usz j = 0; j < arrlenu(type->methods); j++)
        {
            ECSIDefinition_AddLuaFunctionOf(&lua, type->targets[j], className, type->methods[j], true);
        }
    }

    // the classes that the plugin's value<...> types name, from its types.lua, which holds only annotations
    ECSIDefinition_AddFile(&lua, provider, "types.lua", "--");
    ECSIText_Add(&lua, "\nreturn %s\n", pascal);
    return ECSIDefinition_Save(folder, plugin, ".lua", &lua);
}

/// @brief Writes the C header of a plugin's functions: a struct of function pointers, and the function that fills it.
/// @brief Adds a struct type's C declaration, such as "typedef struct CglmVec3 { f32 x; f32 y; f32 z; } CglmVec3;", with a list's length after its field.
static void ECSIDefinition_AddCStruct(ECSIText *header, const ECSIStructType *type)
{
    char className[256];
    ECSIDefinition_ToPascalCase(type->name, cs(className, sizeof(className)));
    ECSIText_Add(header, "\n/// @brief A struct of the type %s: plain data, which the plugin's functions take and give by pointer.\ntypedef struct %s\n{\n", type->name, className);

    for (usz i = 0; i < arrlenu(type->fields); i++)
    {
        const ECSIStructField *field = &type->fields[i];
        char fieldName[80];
        SDL_snprintf(fieldName, sizeof(fieldName), "%s", field->name != nullptr ? field->name : "values");

        if (field->count > 0)
        {
            SDL_snprintf(fieldName + SDL_strlen(fieldName), sizeof(fieldName) - SDL_strlen(fieldName), "[%zu]", field->count);
        }

        if (field->type == ECSIParameterType_Struct)
        {
            char nestedName[256];
            ECSIDefinition_ToPascalCase(field->structType->name, cs(nestedName, sizeof(nestedName)));
            ECSIText_Add(header, "    %s %s;\n", nestedName, fieldName);
        }
        else
        {
            ECSIText_Add(header, "    ");
            ECSIDefinition_AddCDeclaration(header, (ECSIParameter){.type = field->type}, fieldName);
            ECSIText_Add(header, ";\n");
        }
    }

    ECSIText_Add(header, "} %s;\n", className);
}

static SHUResult ECSIDefinition_WriteC(ECSPlugin provider, const char *folder)
{
    const char *plugin = ECSIPlugin_GetName(provider);
    char pascal[128];
    char member[256];
    ECSIDefinition_ToPascalCase(plugin, cs(pascal, sizeof(pascal)));

    ECSIText header = {0};
    ECSIText_Add(&header, "// The functions of the plugin %s, for plugins in C. OpenECS --definitions wrote this file.\n#pragma once\n\n#include \"OpenECS.h\"\n", plugin);

    // each of the plugin's struct types, laid out as the core lays it out
    for (usz i = 0; i < shlenu(SERVICES.structTypes); i++)
    {
        if (SERVICES.structTypes[i].value->plugin == provider)
        {
            ECSIDefinition_AddCStruct(&header, SERVICES.structTypes[i].value);
        }
    }

    ECSIText_Add(&header, "\n/// @brief The functions of the plugin %s. %sFunctions_Get fills them.\ntypedef struct %sFunctions\n{\n", plugin, pascal, pascal);

    const ECSIFunction **functions = ECSIDefinition_GetFunctions(provider);

    for (usz i = 0; i < arrlenu(functions); i++)
    {
        const ECSIFunction *function = functions[i];
        const ECSISignature *signature = &function->signature;

        // a member that points to the function
        ECSIText_Add(&header, "    /// @brief %s\n", function->description);

        for (usz j = 0; j < arrlenu(signature->parameters); j++)
        {
            char name[64];

            if (signature->parameters[j].documentation != nullptr)
            {
                ECSIText_Add(&header, "    /// @param %s %s\n", ECSIDefinition_GetCName(signature->parameters[j], j, cs(name, sizeof(name))), signature->parameters[j].documentation);
            }
        }

        if (signature->result.documentation != nullptr)
        {
            ECSIText_Add(&header, "    /// @return %s\n", signature->result.documentation);
        }

        char declarator[300];
        ECSIDefinition_ToMemberName(SDL_strchr(function->name, '.') + 1, cs(member, sizeof(member)));
        SDL_snprintf(declarator, sizeof(declarator), "(*%s)", member);
        ECSIText_Add(&header, "    ");
        ECSIDefinition_AddCDeclaration(&header, signature->result, declarator);
        ECSIDefinition_AddCParameters(&header, signature);
        ECSIText_Add(&header, ";\n");
    }

    ECSIText_Add(&header, "} %sFunctions;\n\n/// @brief Looks up every function of the plugin %s. Call it from ECSPlugin_Init; the plugin's manifest must depend on %s.\n/// @return SHUResult_Ok, or the error of the first function that cannot be looked up.\nstatic inline SHUResult %sFunctions_Get(ECSPlugin plugin, %sFunctions *retFunctions)\n{\n    ECSFunction function = nullptr;\n", pascal, plugin, plugin, pascal, pascal);

    for (usz i = 0; i < arrlenu(functions); i++)
    {
        ECSIDefinition_ToMemberName(SDL_strchr(functions[i]->name, '.') + 1, cs(member, sizeof(member)));
        ECSIText_Add(&header, "\n    SHU_ReturnResult(ECSService_GetFunction(plugin, &function, \"%s\", \"%s\"));\n    retFunctions->%s = (typeof(retFunctions->%s))function;\n", functions[i]->name, functions[i]->signature.text, member, member);
    }

    arrfree(functions);

    ECSIText_Add(&header, "\n    return SHUResult_Ok;\n}\n");

    // what the plugin gives C plugins beside its functions, such as macros, from its header.h
    ECSIDefinition_AddFile(&header, provider, "header.h", "//");
    return ECSIDefinition_Save(folder, plugin, ".h", &header);
}

#pragma endregion Definitions

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
    ECSILua_SetHandleFunctions(ECSIServices_ReadHandle, ECSIServices_PushHandle);

    // the core's handle types: panels, popups, and the surface a panel or popup draws into, valid during Draw
    SHU_ReturnResult(ECSIServices_RegisterHandleType(nullptr, "ecs.panel", nullptr));
    SHU_ReturnResult(ECSIServices_RegisterHandleType(nullptr, "ecs.popup", nullptr));
    return ECSIServices_RegisterHandleType(nullptr, "ecs.surface", nullptr);
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
    ECSHandle_Forget(surface);
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
    ECSIServices_DropHandles(nullptr);
}

void ECSIServices_Terminate(void)
{
    // handles that plugins made while they shut down are only forgotten, as their plugins' state is gone
    for (usz i = 0; i < shlenu(SERVICES.handleTypes); i++)
    {
        SERVICES.handleTypes[i].value->Destroy = nullptr;
    }

    ECSIServices_DropHandles(nullptr);

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

    for (usz i = 0; i < shlenu(SERVICES.structTypes); i++)
    {
        ECSIStructs_FreeType(SERVICES.structTypes[i].value);
    }

    shfree(SERVICES.structTypes);

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

/// @brief Drops the methods of a plugin's struct types. The types stay, as tables and other types may still have their fields, but the methods call the plugin's functions, which go with it.
static void ECSIStructs_DropPluginMethods(ECSPlugin plugin)
{
    for (usz i = 0; i < shlenu(SERVICES.structTypes); i++)
    {
        if (SERVICES.structTypes[i].value->plugin == plugin)
        {
            ECSIStructs_DropMethods(SERVICES.structTypes[i].value);
        }
    }
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
            if (type->Destroy != ECSIServices_DestroyLuaObject)
            {
                type->Destroy = nullptr;
            }

            ECSIServices_DropHandles(type);
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

    ECSIStructs_DropPluginMethods(plugin);

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
    char *fullName = ECSIPlugin_MakeFullName(plugin, name);
    ECSIFunction *registered = nullptr;
    SHUResult result = fullName == nullptr ? SHUResult_ErrBadData : ECSIServices_CreateFunction(plugin, fullName, signature, description, &registered);
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
    SHU_ReturnResult(ECSIServices_CreateFunction(plugin, name, signature, description, &function));

    // the Lua function on top of the stack is taken
    lua_State *state = ECSILua_GetState();

    if (ECSIFunction_MakeClosure(state, -1, function))
    {
        ECSIFunction_Free(function);
        return SHUResult_ErrAllocation;
    }

    lua_pop(state, 1);
    shput(SERVICES.functions, function->name, function);
    return SHUResult_Ok;
}

SHUResult ECSIServices_RegisterCore(const char *name, ECSFunction function, const char *signature, const char *description)
{
    SDL_assert(name != nullptr);
    SDL_assert(function != nullptr);
    SDL_assert(signature != nullptr);

    ECSIFunction *registered = nullptr;
    SHU_ReturnResult(ECSIServices_CreateFunction(nullptr, name, signature, description, &registered));
    registered->pointer = function;
    shput(SERVICES.functions, registered->name, registered);
    return SHUResult_Ok;
}

SHUResult ECSIServices_CallBound(const char *name, ECSPanel focus)
{
    SDL_assert(name != nullptr);

    ECSIFunction *function = ECSIServices_FindFunction(name);

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

    ECSIFunction *function = ECSIServices_FindFunction(name);

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

    ECSIFunction *function = ECSIServices_FindFunction(name);
    return function == nullptr ? nullptr : function->description;
}

SHUResult ECSHandle_AddMethod(ECSPlugin plugin, const char *type, const char *method, const char *function)
{
    SDL_assert(plugin != nullptr);
    SDL_assert(type != nullptr);
    SDL_assert(method != nullptr);
    SDL_assert(function != nullptr);

    char *typeName = ECSIPlugin_MakeFullName(plugin, type);
    char *functionName = ECSIPlugin_MakeFullName(plugin, function);
    ECSIHandleType *handleType = typeName == nullptr ? nullptr : ECSIServices_FindHandleType(typeName);
    ECSIFunction *target = functionName == nullptr ? nullptr : ECSIServices_FindFunction(functionName);
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

SHUResult ECSStruct_Register(ECSPlugin plugin, const char *name, const char *fields)
{
    SDL_assert(plugin != nullptr);
    SDL_assert(name != nullptr);
    SDL_assert(fields != nullptr);

    char *fullName = ECSIPlugin_MakeFullName(plugin, name);
    SHUResult result = fullName == nullptr ? SHUResult_ErrBadData : ECSIStructs_Register(plugin, fullName, fields);
    SDL_free(fullName);
    return result;
}

SHUResult ECSIServices_RegisterCoreStruct(const char *name, const char *fields)
{
    SDL_assert(SDL_strncmp(name, "ecs.", 4) == 0);
    SDL_assert(fields != nullptr);

    return ECSIStructs_Register(nullptr, name, fields);
}

/// @brief Sets the function on top of the stack as a method, and as an operator if the method's name makes one, of a struct type's metatable; pops it.
static void ECSIStructs_SetMethod(lua_State *state, const ECSIStructType *type, const char *method)
{
    luaL_getmetatable(state, type->metatable);

    if (lua_getfield(state, -1, "__index") != LUA_TTABLE)
    {
        lua_pop(state, 1);
        lua_newtable(state);
        lua_pushvalue(state, -1);
        lua_setfield(state, -3, "__index");
    }

    lua_pushvalue(state, -3);
    lua_setfield(state, -2, method);
    lua_pop(state, 1);

    for (usz i = 0; i < SDL_arraysize(OPENECS_STRUCT_OPERATORS); i++)
    {
        if (SDL_strcmp(method, OPENECS_STRUCT_OPERATORS[i][0]) == 0)
        {
            lua_pushvalue(state, -2);
            lua_setfield(state, -2, OPENECS_STRUCT_OPERATORS[i][1]);
        }
    }

    lua_pop(state, 2);
}

SHUResult ECSStruct_AddMethod(ECSPlugin plugin, const char *type, const char *method, const char *function)
{
    SDL_assert(plugin != nullptr);
    SDL_assert(type != nullptr);
    SDL_assert(method != nullptr);
    SDL_assert(function != nullptr);

    char *typeName = ECSIPlugin_MakeFullName(plugin, type);
    char *functionName = ECSIPlugin_MakeFullName(plugin, function);
    ECSIStructType *structType = typeName == nullptr ? nullptr : ECSIServices_FindStructType(typeName);
    ECSIFunction *target = functionName == nullptr ? nullptr : ECSIServices_FindFunction(functionName);
    SHUResult result = SHUResult_Ok;

    // the method is a function of the same plugin whose first parameter is a struct of the type, which the struct itself gives
    if (structType == nullptr || structType->plugin != plugin || target == nullptr || arrlenu(target->signature.parameters) == 0 ||
        target->signature.parameters[0].type != ECSIParameterType_Struct || target->signature.parameters[0].out || target->signature.parameters[0].structType != structType)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "'%s' cannot be a method of '%s': it needs to be a function of the plugin whose first parameter is a struct<%s>.", functionName != nullptr ? functionName : function, typeName != nullptr ? typeName : type, typeName != nullptr ? typeName : type);
        result = SHUResult_ErrBadData;
    }

    for (usz i = 0; !result && i < arrlenu(structType->methods); i++)
    {
        if (SDL_strcmp(structType->methods[i], method) == 0)
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Struct type '%s' already has a method '%s'.", typeName, method);
            result = SHUResult_ErrBadData;
        }
    }

    char *copy = result ? nullptr : SDL_strdup(method);
    result = result || copy != nullptr ? result : SHUResult_ErrAllocation;
    result = result ? result : ECSIServices_PushFunction(plugin, functionName, nullptr);

    if (!result)
    {
        ECSIStructs_SetMethod(ECSILua_GetState(), structType, method);
        arrput(structType->methods, copy);
        arrput(structType->targets, target);
        copy = nullptr;
    }

    SDL_free(copy);
    SDL_free(typeName);
    SDL_free(functionName);
    return result;
}

void ECSIServices_PushStruct(int index, const char *type)
{
    lua_State *state = ECSILua_GetState();
    const ECSIStructType *structType = ECSIServices_FindStructType(type);

    if (structType == nullptr)
    {
        luaL_error(state, "struct type '%s' is not registered", type);
        return;
    }

    // the fields are checked and copied, so the new struct holds only its own fields, with its type's methods
    const char *field = nullptr;
    void *memory = lua_newuserdatauv(state, structType->size, 0);
    SDL_memset(memory, 0, structType->size);

    if (!ECSIStructs_Read(state, index, structType, memory, &field))
    {
        luaL_error(state, field == nullptr ? "a %s is made from a table of its fields" : "a %s needs its field '%s', of its type", type, field);
        return;
    }

    ECSIStructs_Push(state, structType, memory);
    lua_remove(state, -2);
}

SHUResult ECSIServices_RegisterLuaHandleType(ECSPlugin plugin, const char *name)
{
    SDL_assert(plugin != nullptr);
    SDL_assert(name != nullptr);

    char *fullName = ECSIPlugin_MakeFullName(plugin, name);
    SHUResult result = fullName == nullptr ? SHUResult_ErrBadData : ECSIServices_RegisterHandleType(plugin, fullName, ECSIServices_DestroyLuaObject);
    SDL_free(fullName);
    return result;
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
    lua_rawgeti(state, LUA_REGISTRYINDEX, object->value);
}

SHUResult ECSHandle_RegisterType(ECSPlugin plugin, const char *name, ECSHandleDestroyFunction Destroy)
{
    SDL_assert(plugin != nullptr);
    SDL_assert(name != nullptr);

    char *fullName = ECSIPlugin_MakeFullName(plugin, name);
    SHUResult result = fullName == nullptr ? SHUResult_ErrBadData : ECSIServices_RegisterHandleType(plugin, fullName, Destroy);
    SDL_free(fullName);
    return result;
}

void ECSHandle_Forget(void *object)
{
    SDL_assert(object != nullptr);

    ECSIServices_DropObject(object);
}

void ECSHandle_Free(ECSPlugin plugin, const char *type, void *object)
{
    SDL_assert(plugin != nullptr);
    SDL_assert(type != nullptr);
    SDL_assert(object != nullptr);

    // Lua may have no handle of the object, so its type is found by name
    char *fullName = ECSIPlugin_MakeFullName(plugin, type);
    ECSIHandleType *handleType = fullName == nullptr ? nullptr : ECSIServices_FindHandleType(fullName);
    SDL_free(fullName);
    SDL_assert(handleType != nullptr && handleType->plugin == plugin);

    ECSIServices_DropObject(object);

    if (handleType != nullptr && handleType->Destroy != nullptr)
    {
        handleType->Destroy(object);
    }
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

/// @brief Sets the function on top of the stack into a table by its local name, whose parts before dots are nested tables, such as json.read; pops the function.
static void ECSIServices_SetByLocalName(lua_State *state, int table, const char *local)
{
    int function = lua_gettop(state);
    lua_pushvalue(state, table);

    for (const char *dot = SDL_strchr(local, '.'); dot != nullptr; dot = SDL_strchr(local, '.'))
    {
        // the namespace's table, made the first time one of its functions comes
        lua_pushlstring(state, local, (usz)(dot - local));

        if (lua_rawget(state, -2) != LUA_TTABLE)
        {
            lua_pop(state, 1);
            lua_newtable(state);
            lua_pushlstring(state, local, (usz)(dot - local));
            lua_pushvalue(state, -2);
            lua_rawset(state, -4);
        }

        lua_remove(state, -2);
        local = dot + 1;
    }

    lua_pushvalue(state, function);
    lua_setfield(state, -2, local);
    lua_pop(state, 2);
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
    int table = lua_gettop(state);

    for (usz i = 0; i < shlenu(SERVICES.functions); i++)
    {
        ECSIFunction *function = SERVICES.functions[i].value;

        if (function->plugin == provider)
        {
            SHU_ReturnResult(ECSIServices_PushFunction(plugin, function->name, nullptr), lua_pop(state, 1););
            ECSIServices_SetByLocalName(state, table, SDL_strchr(function->name, '.') + 1);
        }
    }

    return SHUResult_Ok;
}

SHUResult ECSIServices_WriteInterface(const char *folder)
{
    SDL_assert(folder != nullptr);

    for (usz i = 0; i < ECSI_INTERFACE_FILE_COUNT; i++)
    {
        const ECSIInterfaceFile *file = &ECSI_INTERFACE_FILES[i];
        char *path = nullptr;

        if (SDL_asprintf(&path, "%s%s", folder, file->name) < 0)
        {
            return SHUResult_ErrAllocation;
        }

        bool saved = SDL_SaveFile(path, file->data, file->size);

        if (!saved)
        {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot write '%s': %s", path, SDL_GetError());
        }

        SDL_free(path);
        SHU_ReturnResult(saved ? SHUResult_Ok : SHUResult_ErrFile);
    }

    return SHUResult_Ok;
}

SHUResult ECSIServices_WriteDefinitions(ECSPlugin provider, const char *folder)
{
    SDL_assert(provider != nullptr);
    SDL_assert(folder != nullptr);

    SHUResult result = ECSIDefinition_WriteLua(provider, folder);
    SHUResult written = ECSIDefinition_WriteC(provider, folder);
    return result ? result : written;
}
