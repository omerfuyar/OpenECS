#include "runtime/Services.h"

#include "base/Log.h"
#include "base/Lua.h"
#include "base/Profile.h"
#include "runtime/Plugins.h"
#include "runtime/Settings.h"

#include "lua/compat-5.3.h"

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

/// @brief The most parameters whose call storage a call from Lua keeps on the C stack; a function with more gets a userdata.
#define OPENECS_CALL_PARAMETERS 16

/// @brief The methods whose names make them Lua's operators too: a struct type's add is also its +. OPENECS_HANDLE_HELPERS names them as well.
static const char *const OPENECS_STRUCT_OPERATORS[] = {"add", "sub", "mul", "div", "mod", "pow", "unm", "concat", "eq", "lt", "le", "len", "tostring"};

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
    bool kept;                           // its plugin went; it takes the type back when it registers it again
    int ctype;                           // registry reference of its FFI type, made when the first handle or wrapper needs it, or LUA_NOREF
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

/// @brief A registered struct type: plain data that crosses between C and Lua by value, a C struct in C and cdata of its own FFI type in Lua (10.11).
typedef struct ECSIStructType
{
    char *name;
    char *metatable;         // name of its table of methods in the registry, which its FFI type's metatable reads
    int ctype;               // registry reference of its FFI type, made when Lua first needs it; LUA_NOREF before
    ECSPlugin plugin;        // nullptr for the core
    ECSIStructField *fields; // stb_ds array, in the order of the C struct
    usz size;
    usz alignment;
    char **methods;                      // stb_ds array: the names of its methods in Lua
    const struct ECSIFunction **targets; // stb_ds array: the function each method calls, in the same order
    bool kept;                           // its plugin went; it takes the type back when it registers it again
} ECSIStructType;

/// @brief A Lua handle: the memory of the FFI cdata that stands for an object, a struct of its type's own FFI type (DESIGN 10.6).
typedef struct ECSIHandle
{
    void *object; // nullptr when the object is gone
    ECSIHandleType *type;
} ECSIHandle;

static_assert(offsetof(ECSIHandle, object) == 0, "a value reads a Lua handle's object at the start of its memory");

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
    ECSIHandleType *handle;         // handles: their type, which stays while the core runs, as handle types are never freed
    ECSIStructType *structType;     // structs: their type
    struct ECSISignature *callback; // callbacks: their signature, owned by the parameter
    struct ECSIFunction *spare;     // a callback that is not kept: the closure a finished call left, which the next call takes, so a call makes none
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
    bool fails; // its last parameter is "out string error": the function can fail, and gives nothing else then (DESIGN 10.8)
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
    } *callers; // stb_ds hash map: for each plugin, the registry reference of the Lua function that calls it for that plugin, made when the plugin first asks for it
    struct
    {
        ECSPlugin key;
        struct ECSICaller *value;
    } *stubs;             // stb_ds hash map, for a Lua function that keeps callbacks: for each plugin whose C code asked for it, the code that calls it for that plugin
    ECSPlugin giver;      // a callback: the plugin that gave it, or nullptr if it is not known
    u32 generation;       // a callback that C gave to Lua: its giver's generation then; once the giver's differs, its code is gone and nothing runs
    bool inert;           // a callback whose giver was removed: C may still call it, and nothing runs
    int lua;              // registry reference of the Lua function, or LUA_NOREF for a C function
    int anchors;          // registry reference of a table that keeps the strings and buffers a Lua function last gave C
    ECSValue *result;     // the value a Lua function last returned to C, or nullptr
    ffi_closure *closure; // the code that C calls for a Lua function, or nullptr
} ECSIFunction;

/// @brief The code that a plugin's C code calls for a Lua function that keeps callbacks, so the callbacks know which plugin gave them.
typedef struct ECSICaller
{
    ECSIFunction *function;
    ECSPlugin plugin;
    ffi_closure *closure;
    ECSFunction pointer;
} ECSICaller;

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
    } *drawn;                // stb_ds array of the functions that run once the Draw of their surface returns, in the order they were given
    SDL_AtomicInt logErrors; // the setting ecs.logServiceErrors, which threads read
    int ffiHelpers;          // registry reference of the helpers of Lua's wrappers of C functions, made with the first wrapper
    int handleHelpers;       // registry reference of the Lua functions that make handles' FFI types and tell a handle's type
    struct
    {
        ECSPlugin key;
        bool value;
    } *luaCallers; // stb_ds hash map of the plugins whose C code looked up a function that calls Lua, so Lua calls theirs through the C API
    struct
    {
        ECSIHandle *key;
        bool value;
    } *liveHandles; // stb_ds hash map of every handle's cdata that Lua has not collected, so C tells a handle from other cdata without Lua
    struct
    {
        const char *name;
        ECSIHandleType *type;
    } coreTypes[3]; // the core's handle types that ECSIServices_CheckHandle met, by the address of their names, as they live until exit
} SERVICES = {0};

/// @brief File-level state of each thread.
static thread_local struct
{
    char *error; // the message of the last ECS_Fail on this thread, or nullptr
} SERVICES_THREAD = {0};

/// @brief The setting that logs services' failures (DESIGN 10.8).
#define OPENECS_LOG_SERVICE_ERRORS "ecs.logServiceErrors"

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
static void ECSIServices_FreeSpare(struct ECSIFunction **spare);

static void ECSIParameter_Free(ECSIParameter *parameter)
{
    SDL_free(parameter->name);
    parameter->name = nullptr;
    SDL_free(parameter->luaType);
    parameter->luaType = nullptr;
    SDL_free(parameter->documentation);
    parameter->documentation = nullptr;

    if (parameter->spare != nullptr)
    {
        ECSIServices_FreeSpare(&parameter->spare);
    }

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
        (void)ECS_Check(nullptr, false, "'%s' is not a signature, such as \"int(string, out float)\".", text);
        ECSISignature_Free(retSignature);
        return SHUResult_ErrAssertion;
    }

    // a function that can fail says why in its last parameter, and only there
    usz count = arrlenu(retSignature->parameters);

    for (usz i = 0; i < count; i++)
    {
        const ECSIParameter *parameter = &retSignature->parameters[i];
        bool error = parameter->out && parameter->type == ECSIParameterType_String && parameter->name != nullptr && SDL_strcmp(parameter->name, "error") == 0;

        if (error && i + 1 != count)
        {
            (void)ECS_Check(nullptr, false, "In '%s', out string error is not the last parameter.", text);
            ECSISignature_Free(retSignature);
            return SHUResult_ErrAssertion;
        }

        retSignature->fails = error;
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

    for (usz i = 0; i < hmlenu(function->stubs); i++)
    {
        ffi_closure_free(function->stubs[i].value->closure);
        SDL_free(function->stubs[i].value);
    }

    hmfree(function->stubs);

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
static void ECSIServices_ParkCallback(ECSIFunction *callback);
static void ECSIServices_PushHandleOf(lua_State *state, ECSIHandleType *handleType, void *object);
static void *ECSIServices_CheckHandleOf(lua_State *state, int index, ECSIHandleType *handleType);

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

static const char *ECSIFfi_GetCType(ECSIParameterType type);

/// @brief Pushes the FFI type of a struct type, which it makes the first time, after the types of the structs in its fields (DESIGN 10.11). Its
/// metatable gives the methods and operators.
static void ECSIStructs_PushCType(lua_State *state, ECSIStructType *type)
{
    if (type->ctype != LUA_NOREF)
    {
        lua_rawgeti(state, LUA_REGISTRYINDEX, type->ctype);
        return;
    }

    // a struct in a field is $, which the FFI fills with that struct's type; a list struct's list is ecsList
    ECSIText declaration = {0};
    ECSIText_Add(&declaration, "struct {");

    for (usz i = 0; i < arrlenu(type->fields); i++)
    {
        const ECSIStructField *field = &type->fields[i];
        const char *cType = field->type == ECSIParameterType_Struct ? "$" : ECSIFfi_GetCType(field->type);
        ECSIText_Add(&declaration, " %s %s", cType, field->name != nullptr ? field->name : "ecsList");

        if (field->count > 0)
        {
            ECSIText_Add(&declaration, "[%zu]", field->count);
        }

        ECSIText_Add(&declaration, ";");
    }

    ECSIText_Add(&declaration, " }");
    SDL_assert(!declaration.failed);

    lua_rawgeti(state, LUA_REGISTRYINDEX, SERVICES.handleHelpers);
    lua_getfield(state, -1, "newStruct");
    luaL_getmetatable(state, type->metatable);
    lua_pushstring(state, declaration.data);
    lua_pushinteger(state, (lua_Integer)type->size);
    lua_pushstring(state, type->name);
    bool list = type->fields[0].name == nullptr;
    list ? lua_pushinteger(state, (lua_Integer)type->fields[0].count) : lua_pushnil(state);
    lua_createtable(state, (int)arrlenu(type->fields), 0);

    for (usz i = 0; !list && i < arrlenu(type->fields); i++)
    {
        lua_pushstring(state, type->fields[i].name);
        lua_rawseti(state, -2, (int)i + 1);
    }

    lua_pushlightuserdata(state, type);
    int nested = 0;

    for (usz i = 0; i < arrlenu(type->fields); i++)
    {
        if (type->fields[i].type == ECSIParameterType_Struct)
        {
            ECSIStructs_PushCType(state, (ECSIStructType *)type->fields[i].structType);
            nested++;
        }
    }

    lua_call(state, 7 + nested, 1);
    SDL_free(declaration.data);
    lua_remove(state, -2);
    lua_pushvalue(state, -1);
    type->ctype = luaL_ref(state, LUA_REGISTRYINDEX);
}

/// @brief Gives the type of the struct at an index of the stack, and its memory, or nullptr for another value.
static ECSIStructType *ECSIStructs_GetType(lua_State *state, int index, void **retMemory)
{
    *retMemory = nullptr;

    if (lua_type(state, index) != OPENECS_LUA_TCDATA)
    {
        return nullptr;
    }

    index = lua_absindex(state, index);
    lua_rawgeti(state, LUA_REGISTRYINDEX, SERVICES.handleHelpers);
    lua_getfield(state, -1, "structOf");
    lua_pushvalue(state, index);
    lua_call(state, 1, 2);
    ECSIStructType *type = lua_touserdata(state, -2);

    // the memory comes as a pointer's cdata, as a struct inside another is a reference
    if (type != nullptr)
    {
        *retMemory = *(void *const *)lua_topointer(state, -1);
    }

    lua_pop(state, 3);
    return type;
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
        return ECSILua_ToInt64(state, index, retMemory);
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

/// @brief Reads a struct of the type, or a table, at an index of the stack, into a struct's memory: a table's fields by their names, and a list's values
/// by their positions. Any table with the fields will do.
/// @param retField The name of the first field that is missing or of another type, or nullptr if the value is not a table.
/// @return false if the value is neither, or a field is missing or of another type.
static bool ECSIStructs_Read(lua_State *state, int index, const ECSIStructType *type, void *retMemory, const char **retField)
{
    *retField = nullptr;
    void *bytes = nullptr;

    // a struct of the type is copied whole
    if (ECSIStructs_GetType(state, index, &bytes) == type)
    {
        SDL_memcpy(retMemory, bytes, type->size);
        return true;
    }

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
                lua_rawgeti(state, -1, (int)j + 1);
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

static void ECSIStructs_PushFields(lua_State *state, const ECSIStructType *type, const void *memory);

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
        ECSIStructs_PushFields(state, field->structType, memory);
        break;
    }
}

/// @brief Pushes a struct's memory as a new table of its fields, which values and saved state hold (DESIGN 10.11).
static void ECSIStructs_PushFields(lua_State *state, const ECSIStructType *type, const void *memory)
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
            lua_rawseti(state, -2, (int)j + 1);
        }

        if (field->name != nullptr)
        {
            lua_setfield(state, -2, field->name);
        }
    }
}

/// @brief Pushes a struct's memory as a new struct for Lua: cdata of its type's FFI type, with its methods and operators.
static void ECSIStructs_Push(lua_State *state, ECSIStructType *type, const void *memory)
{
    ECSIStructs_PushCType(state, type);
    lua_call(state, 0, 1);

    // a struct's bytes are where lua_topointer points
    SDL_memcpy((void *)lua_topointer(state, -1), memory, type->size);
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

/// @brief Gives a struct type back to its plugin, which registers it again after it was disabled. The fields must be the same, as other plugins' functions and tables may use them.
static SHUResult ECSIStructs_TakeBack(ECSIStructType *type, const char *fields)
{
    ECSIStructType read = {0};
    bool same = ECSIStructs_ReadFields(fields, &read) && read.size == type->size && arrlenu(read.fields) == arrlenu(type->fields);

    for (usz i = 0; same && i < arrlenu(read.fields); i++)
    {
        const ECSIStructField *now = &read.fields[i];
        const ECSIStructField *before = &type->fields[i];
        same = now->type == before->type && now->structType == before->structType && now->count == before->count && now->offset == before->offset &&
               (now->name == nullptr ? before->name == nullptr : before->name != nullptr && SDL_strcmp(now->name, before->name) == 0);
    }

    ECSIStructs_FreeFields(&read.fields);

    if (!same)
    {
        (void)ECS_Check(nullptr, false, "Struct type '%s' must keep the fields it had before its plugin was disabled.", type->name);
        return SHUResult_ErrAssertion;
    }

    type->kept = false;
    return SHUResult_Ok;
}

/// @brief Registers a struct type by its full name.
static SHUResult ECSIStructs_Register(ECSPlugin plugin, const char *name, const char *fields)
{
    ECSIStructType *kept = ECSIServices_FindStructType(name);

    if (kept != nullptr && kept->kept && kept->plugin == plugin)
    {
        return ECSIStructs_TakeBack(kept, fields);
    }

    // handles and structs share the names of types, so a parameter names one of them
    if (kept != nullptr || ECSIServices_FindHandleType(name) != nullptr)
    {
        (void)ECS_Check(nullptr, false, "Type '%s' is already registered.", name);
        return SHUResult_ErrAssertion;
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
        (void)ECS_Check(nullptr, false, "'%s' are not the fields of a struct, such as \"float x, float y\" or \"float[16]\".", fields);
        ECSIStructs_FreeType(type);
        return SHUResult_ErrAssertion;
    }

    // its FFI type's metatable reads the methods from this table, so they can come and go
    lua_State *state = ECSILua_GetState();
    lua_newtable(state);
    lua_setfield(state, LUA_REGISTRYINDEX, type->metatable);
    type->ctype = LUA_NOREF;

    shput(SERVICES.structTypes, type->name, type);
    return SHUResult_Ok;
}

/// @brief Takes a struct type's methods away, when the plugin whose functions they call goes.
static void ECSIStructs_DropMethods(ECSIStructType *type)
{
    lua_State *state = ECSILua_GetState();
    luaL_getmetatable(state, type->metatable);

    for (usz i = 0; i < arrlenu(type->methods); i++)
    {
        lua_pushnil(state);
        lua_setfield(state, -2, type->methods[i]);
        SDL_free(type->methods[i]);
    }

    lua_pop(state, 1);

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
        retSlot->pointer = ECSIServices_CheckHandleOf(state, index, parameter.handle);
        break;
    case ECSIParameterType_Bool:
        retSlot->boolean = (u8)(lua_toboolean(state, index) != 0);
        break;
    case ECSIParameterType_Int:
    {
        lua_Integer integer = ECSILua_CheckInteger(state, index);
        ECSILua_ArgCheck(state, integer >= SDL_MIN_SINT32 && integer <= SDL_MAX_SINT32, index, "does not fit in an int");
        retSlot->integer = (i32)integer;
        break;
    }
    case ECSIParameterType_Int64:
        retSlot->integer64 = (i64)ECSILua_CheckInteger(state, index);
        break;
    case ECSIParameterType_Float:
        retSlot->single = (f32)ECSILua_CheckNumber(state, index);
        break;
    case ECSIParameterType_Double:
        retSlot->number = (f64)ECSILua_CheckNumber(state, index);
        break;
    case ECSIParameterType_Color:
        ECSILua_ArgCheck(state, ECSColor_Parse(ECSILua_CheckString(state, index), &retSlot->color), index, "is not a colour: \"#RRGGBB\", \"#RRGGBBAA\" or a colour of the theme, such as \"text\"");
        break;
    case ECSIParameterType_String:
        // valid during the call, because the argument stays on the stack
        retSlot->string = ECSILua_CheckString(state, index);
        break;
    case ECSIParameterType_Callback:
        // the closure is made later, because it allocates
        if (!lua_isnoneornil(state, index))
        {
            ECSILua_CheckType(state, index, LUA_TFUNCTION);
        }

        retSlot->pointer = nullptr;
        break;
    case ECSIParameterType_Struct:
    {
        // the struct's memory is a userdata, so it lives on the stack during the call; C gets a pointer to it
        const char *field = nullptr;
        void *memory = lua_newuserdata(state, parameter.structType->size);
        SDL_memset(memory, 0, parameter.structType->size);

        if (!ECSIStructs_Read(state, index, parameter.structType, memory, &field))
        {
            const char *problem = field == nullptr ? lua_pushfstring(state, "is not a %s, a table of its fields", parameter.structType->name)
                                                   : lua_pushfstring(state, "is not a %s: its field '%s' is missing or of another type", parameter.structType->name, field);
            ECSILua_ArgError(state, index, problem);
        }

        retSlot->pointer = memory;
        break;
    }
    case ECSIParameterType_Buffer:
    {
        // a string is copied, because the function may write into a buffer and Lua strings must not change; other buffers give their own bytes
        usz length = 0;
        bool text = false;
        const char *bytes = ECSILua_CheckBytes(state, index, &length, &text);

        if (text)
        {
            void *copy = lua_newuserdata(state, length + 1);
            SDL_memcpy(copy, bytes, length);
            bytes = copy;
        }

        retSlot->buffer = cs((void *)bytes, length);
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
            void *memory = lua_newuserdata(state, signature->parameters[i].structType->size);
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
static SHUResult ECSIServices_MakeCallbacks(lua_State *state, ECSIFunction *function, ECSPlugin caller, ECSICallParameter *parameters)
{
    ECSISignature *signature = &function->signature;

    for (usz i = 0; i < arrlenu(signature->parameters); i++)
    {
        ECSICallParameter *call = &parameters[i];

        if (signature->parameters[i].type != ECSIParameterType_Callback || lua_isnoneornil(state, call->index))
        {
            continue;
        }

        // a callback that is not kept takes the closure the last call left, and only its Lua function changes
        ECSIParameter *parameter = &signature->parameters[i];

        if (!parameter->keep && parameter->spare != nullptr)
        {
            call->callback = parameter->spare;
            parameter->spare = nullptr;
            call->callback->giver = caller;
            lua_pushvalue(state, call->index);
            call->callback->lua = luaL_ref(state, LUA_REGISTRYINDEX);
            lua_newtable(state);
            call->callback->anchors = luaL_ref(state, LUA_REGISTRYINDEX);
        }
        else
        {
            SHU_ReturnResult(ECSIServices_MakeCallback(state, call->index, function, caller, parameter->callback, &call->callback));
        }

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
    usz count = arrlenu(signature->parameters);

    // a function that failed gives nil for each result, and why last, as Lua's io.open does
    bool failed = signature->fails && *(const char *const *)&parameters[count - 1].output != nullptr;

    if (signature->result.type != ECSIParameterType_Void)
    {
        ECSIServices_NarrowResult(signature->result, returned);

        if (failed)
        {
            lua_pushnil(state);
        }
        else
        {
            ECSIServices_PushArgument(state, signature->result, returned);
        }

        pushed++;
    }

    for (usz i = 0; i < count; i++)
    {
        if (!signature->parameters[i].out)
        {
            continue;
        }

        if (failed && i + 1 < count)
        {
            lua_pushnil(state);
        }
        else
        {
            ECSIServices_PushArgument(state, signature->parameters[i], &parameters[i].output);
        }

        pushed++;
    }

    return pushed;
}

/// @brief Frees the values and closures of a call. A kept closure that the call got is C's once the call ran, released or not; when the call did not run, nothing can release it.
static void ECSIServices_FreeCallParameters(ECSISignature *signature, ECSICallParameter *parameters, bool called)
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
            ECSIServices_FreeCallback(&call->callback);
            continue;
        }

        // the closure waits for the next call; its Lua function and anchors go now, so the garbage collector can take them
        ECSIFunction **spare = &signature->parameters[i].spare;
        ECSIServices_ParkCallback(call->callback);

        if (*spare == nullptr)
        {
            *spare = call->callback;
            call->callback = nullptr;
        }
        else
        {
            ECSIServices_FreeSpare(&call->callback);
        }
    }
}

/// @brief Calls a C function from Lua through libffi. The function is the first upvalue: a registered function, or a callback that C gave Lua. The second, for a registered function, is the plugin that calls it. Output parameters become extra results.
static int ECSIServices_CallC(lua_State *state)
{
    ECSIFunction *function = lua_touserdata(state, lua_upvalueindex(1));
    ECSPlugin caller = lua_touserdata(state, lua_upvalueindex(2));
    ECSISignature *signature = &function->signature;
    usz count = arrlenu(signature->parameters);

    if (function->pointer == nullptr)
    {
        ECSILua_Misuse(state, "a callback given to '%s' is valid only during the call that gave it", function->name);
    }

    // a missing argument is nil; the stack holds one for each, so the storage below never stands where an argument is read
    int inputs = (int)(count - signature->outCount);

    if (lua_gettop(state) < inputs)
    {
        lua_settop(state, inputs);
    }

    u64 misuses = ECSIPlugin_GetMisuseCount();

    // the storage is on the C stack, which an argument's error leaves as well; a function of many parameters gets a userdata, which the garbage
    // collector frees then; libffi takes the arguments as an array of pointers
    ECSICallParameter local[OPENECS_CALL_PARAMETERS];
    void *localArguments[OPENECS_CALL_PARAMETERS];
    ECSICallParameter *parameters = local;
    void **arguments = localArguments;

    if (count > OPENECS_CALL_PARAMETERS)
    {
        parameters = lua_newuserdata(state, count * (sizeof(ECSICallParameter) + sizeof(void *)));
        arguments = (void **)(parameters + count);
    }

    ECSIServices_CheckArguments(state, signature, parameters, arguments);

    // values allocate, so they are made after every check that can raise an error
    SHUResult result = ECSIServices_MakeValues(state, signature, parameters);
    result = result ? result : ECSIServices_MakeCallbacks(state, function, caller, parameters);
    int pushed = 0;

    if (!result)
    {
        ECSISlot returned = {0};

        // a callback whose giver was disabled since has lost its code: nothing runs, and Lua gets zeros
        if (function->giver == nullptr || ECSIPlugin_GetGeneration(function->giver) == function->generation)
        {
            // the called function's time is its plugin's, or the core's, not the Lua code's that called it
            ECSIProfileScope profile;
            ECSIProfile_Enter(&profile, function->plugin, function->plugin != nullptr ? ECSIPlugin_GetName(function->plugin) : nullptr);
            lua_State *outer = ECSILua_SetCaller(state);
            ffi_call((ffi_cif *)&signature->cif, FFI_FN(function->pointer), &returned, arguments);
            (void)ECSILua_SetCaller(outer);
            ECSIProfile_Leave(&profile);
        }

        pushed = ECSIServices_PushResults(state, signature, parameters, &returned);
    }

    ECSIServices_FreeCallParameters(signature, parameters, !result);

    if (result)
    {
        ECSIServices_Raise(state, result, "cannot convert a value for '%s'", function->name);
    }

    // a misuse that the C function checked stops the Lua code at its line, as Lua's own checks do (DESIGN 1.4)
    if (ECSIPlugin_GetMisuseCount() != misuses)
    {
        return luaL_error(state, "%s", ECSIPlugin_GetLastMisuse());
    }

    return pushed;
}

#pragma endregion Lua Calls C

#pragma region Handles

/// @brief The Lua functions of handles: new(metatable, type) makes a handle type's FFI type, a struct laid out as ECSIHandle whose metatable is the
/// type's, and typeOf(value) gives the type of a handle, as a light userdata, or nil for another value.
static const char OPENECS_HANDLE_HELPERS[] =
    "local ffi = require('ffi')\n"
    "local types = {}\n"
    "local function new(metatable, handleType)\n"
    "  local ctype = ffi.metatype(ffi.typeof('struct { void *const ecsObject; void *const ecsType; }'), metatable)\n"
    "  types[tonumber(ctype)] = handleType\n"
    "  return ctype\n"
    "end\n"
    "local function typeOf(value)\n"
    "  if type(value) ~= 'cdata' then return nil end\n"
    "  return types[tonumber(ffi.typeof(value))]\n"
    "end\n"
    // a struct type's operators call its methods, which come and go with its plugin, as its metatable cannot change
    "local format, istype, structs = string.format, ffi.istype, {}\n"
    "local OPERATORS = { add = '__add', sub = '__sub', mul = '__mul', div = '__div', mod = '__mod', pow = '__pow', unm = '__unm', concat = '__concat',\n"
    "  lt = '__lt', le = '__le', len = '__len', tostring = '__tostring' }\n"
    "local function newStruct(methods, declaration, size, name, count, fields, structType, ...)\n"
    "  local ctype = ffi.typeof(declaration, ...)\n"
    "  assert(ffi.sizeof(ctype) == size, 'a struct type has the layout of the core')\n"
    "  local metatable = { __index = methods }\n"
    // a list struct is its list, whose positions start at 1 as Lua's lists do
    "  if count ~= nil then\n"
    "    metatable.__index = function(self, key)\n"
    "      if type(key) ~= 'number' then return methods[key] end\n"
    "      if key >= 1 and key <= count then return self.ecsList[key - 1] end\n"
    "    end\n"
    "    metatable.__newindex = function(self, key, value)\n"
    "      if type(key) ~= 'number' or key % 1 ~= 0 or key < 1 or key > count then error(format('a %s has the positions 1 to %d', name, count), 2) end\n"
    "      self.ecsList[key - 1] = value\n"
    "    end\n"
    "  end\n"
    // pairs gives the fields in their order, or a list's positions and values, as it does for a table of them; ipairs gives a list's
    "  metatable.__pairs = function(self)\n"
    "    local i = 0\n"
    "    return function()\n"
    "      i = i + 1\n"
    "      if count ~= nil then\n"
    "        if i <= count then return i, self.ecsList[i - 1] end\n"
    "      elseif fields[i] ~= nil then\n"
    "        return fields[i], self[fields[i]]\n"
    "      end\n"
    "    end\n"
    "  end\n"
    "  if count ~= nil then metatable.__ipairs = metatable.__pairs end\n"
    "  for method, event in pairs(OPERATORS) do\n"
    "    metatable[event] = function(a, b)\n"
    "      local operator = methods[method]\n"
    "      if operator ~= nil then return operator(a, b) end\n"
    "      if method == 'tostring' then return name end\n"
    "      if method == 'len' and count ~= nil then return count end\n"
    "      error(format('a %s has no method %s', name, method), 2)\n"
    "    end\n"
    "  end\n"
    // == also compares a struct with other values, such as nil, which are never equal to it
    "  metatable.__eq = function(a, b)\n"
    "    local operator = methods.eq\n"
    "    if operator == nil or not istype(ctype, a) or not istype(ctype, b) then return rawequal(a, b) end\n"
    "    return operator(a, b)\n"
    "  end\n"
    "  ctype = ffi.metatype(ctype, metatable)\n"
    // a struct inside another is a reference, of a type of its own
    "  structs[tonumber(ctype)], structs[tonumber(ffi.typeof('$ &', ctype))] = structType, structType\n"
    "  return ctype\n"
    "end\n"
    "local voidPointer = ffi.typeof('void *')\n"
    "local function structOf(value)\n"
    "  if type(value) ~= 'cdata' then return nil end\n"
    "  local structType = structs[tonumber(ffi.typeof(value))]\n"
    "  if structType == nil then return nil end\n"
    "  return structType, ffi.cast(voidPointer, value)\n"
    "end\n"
    "return { new = new, typeOf = typeOf, newStruct = newStruct, structOf = structOf }\n";

static_assert(sizeof(ECSIHandle) == 2 * sizeof(void *), "a handle's FFI type is two pointers");

/// @brief Pushes the FFI type of a handle type, which it makes the first time. Its metatable cannot change after that, but its __index table can, so
/// methods are added to that table.
static void ECSIServices_PushHandleCType(lua_State *state, ECSIHandleType *type)
{
    if (type->ctype == LUA_NOREF)
    {
        lua_rawgeti(state, LUA_REGISTRYINDEX, SERVICES.handleHelpers);
        lua_getfield(state, -1, "new");
        luaL_getmetatable(state, type->metatable);
        lua_pushlightuserdata(state, type);
        lua_call(state, 2, 1);
        type->ctype = luaL_ref(state, LUA_REGISTRYINDEX);
        lua_pop(state, 1);
    }

    lua_rawgeti(state, LUA_REGISTRYINDEX, type->ctype);
}

/// @brief Gives the handle at an index of the stack, or nullptr for another value.
static ECSIHandle *ECSIServices_ToHandle(lua_State *state, int index)
{
    if (lua_type(state, index) != OPENECS_LUA_TCDATA)
    {
        return nullptr;
    }

    // a handle's bytes are where lua_topointer points, and each handle that Lua holds is in the map
    ECSIHandle *handle = (ECSIHandle *)lua_topointer(state, index);
    return hmgeti(SERVICES.liveHandles, handle) >= 0 ? handle : nullptr;
}

/// @brief Makes every handle of an object invalid and forgets the object; the handles' userdata stay until they are collected.
/// @return The object's type, or nullptr if Lua had no handle of it.
static ECSIHandleType *ECSIServices_DropObject(void *object)
{
    // an emitted event that names the object is delivered later
    ECSIEvents_ForgetObject(object);

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

    // the weak table may still map the object to the invalid handle until it is collected, which ECSIServices_PushHandleOf passes over; Lua is
    // not used here, as a C function that Lua calls through the FFI may free its object (DESIGN 10.4)
    return type;
}

/// @brief Calls the Destroy of an object's type, if it has a type and the type has one. A misuse in it is only reported.
static void ECSIServices_DestroyObject(ECSIHandleType *type, void *object)
{
    if (type == nullptr || type->Destroy == nullptr)
    {
        return;
    }

    ECSICallback callback;
    ECSIPlugin_Enter(&callback, type->plugin, nullptr, ECSICallbackType_Quiet);
    type->Destroy(object);
    (void)ECSIPlugin_Leave(&callback);
}

/// @brief Lets go of a Lua handle that the garbage collector frees; the object is destroyed with its last handle.
static int ECSIServices_HandleCollect(lua_State *state)
{
    ECSIHandle *handle = (ECSIHandle *)lua_topointer(state, 1);
    (void)hmdel(SERVICES.liveHandles, handle);
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

    ECSIServices_DestroyObject(type, object);
    return 0;
}

static int ECSIServices_HandleToString(lua_State *state)
{
    const ECSIHandle *handle = lua_topointer(state, 1);
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
        ECSILua_Misuse(state, "handle type '%s' is not one that this plugin registered in Lua", name);
    }
}

static SHUResult ECSIServices_RegisterHandleType(ECSPlugin plugin, const char *name, ECSHandleDestroyFunction Destroy)
{
    ECSIHandleType *kept = ECSIServices_FindHandleType(name);

    // a plugin that was disabled takes its type back, with the metatable that Lua's invalid handles of it still have
    if (kept != nullptr && kept->kept && kept->plugin == plugin)
    {
        kept->Destroy = Destroy;
        kept->kept = false;
        return SHUResult_Ok;
    }

    if (kept != nullptr || ECSIServices_FindStructType(name) != nullptr)
    {
        (void)ECS_Check(nullptr, false, "Type '%s' is already registered.", name);
        return SHUResult_ErrAssertion;
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
    type->ctype = LUA_NOREF;

    // the metatable is the FFI type's, which cannot change once the type is made, so the methods go into its __index table
    lua_State *state = ECSILua_GetState();
    luaL_newmetatable(state, type->metatable);
    lua_pushcfunction(state, ECSIServices_HandleCollect);
    lua_setfield(state, -2, "__gc");
    lua_pushcfunction(state, ECSIServices_HandleToString);
    lua_setfield(state, -2, "__tostring");
    lua_newtable(state);
    lua_setfield(state, -2, "__index");
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

        ECSIServices_DestroyObject(objectType, objects[i]);
    }

    arrfree(objects);
}

/// @brief Takes the methods out of a handle type's __index table, as they call its plugin's functions, which go. The table stays, as the type's FFI
/// type keeps its metatable.
static void ECSIServices_DropHandleMethods(const ECSIHandleType *type)
{
    lua_State *state = ECSILua_GetState();
    luaL_getmetatable(state, type->metatable);
    lua_getfield(state, -1, "__index");

    for (usz i = 0; i < arrlenu(type->methods); i++)
    {
        lua_pushnil(state);
        lua_setfield(state, -2, type->methods[i]);
    }

    lua_pop(state, 2);
}

#pragma endregion Handles

#pragma region C Calls Lua

/// @brief Pushes an argument that C passed to a Lua function.
static void ECSIServices_PushArgument(lua_State *state, ECSIParameter parameter, const void *argument)
{
    switch (parameter.type)
    {
    case ECSIParameterType_Handle:
        ECSIServices_PushHandleOf(state, parameter.handle, *(void *const *)argument);
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

/// @brief Writes a string or a buffer that a Lua function gave, at an index of the stack, where C reads it (DESIGN 10.7).
/// The anchor keeps the value alive; a string as a buffer gets a copy, because C may write into it.
/// @param anchors Index of the table that keeps strings and buffers alive until the function returns again.
/// @return false if the Lua value is not a string, or not a buffer.
static bool ECSIServices_WriteBytes(lua_State *state, int index, int anchors, bool buffer, void *retTarget)
{
    bool text = false;
    usz length = 0;
    const char *bytes = buffer ? ECSILua_ToBytes(state, index, &length, &text) : nullptr;

    if (!buffer)
    {
        text = lua_type(state, index) == LUA_TSTRING;
        bytes = text ? lua_tolstring(state, index, &length) : nullptr;
    }

    if (buffer && text)
    {
        void *copy = lua_newuserdata(state, length + 1);
        SDL_memcpy(copy, bytes, length);
        bytes = copy;
    }
    else
    {
        lua_pushvalue(state, index);
    }

    lua_rawseti(state, anchors, (int)lua_rawlen(state, anchors) + 1);

    if (buffer)
    {
        *(SHUSlice *)retTarget = cs((void *)bytes, length);
    }
    else
    {
        *(const char **)retTarget = bytes;
    }

    return bytes != nullptr;
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
        ECSIHandle *handle = ECSIServices_ToHandle(state, index);
        handle = handle != nullptr && handle->type == parameter.handle ? handle : nullptr;
        *(void **)retTarget = handle == nullptr ? nullptr : handle->object;
        lua_pushvalue(state, index);
        lua_rawseti(state, anchors, (int)lua_rawlen(state, anchors) + 1);
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
        return ECSILua_ToInt64(state, index, retTarget);
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
/// @param giver The plugin whose C code gave the callback, or nullptr if it is not known.
static void ECSIServices_PushCallback(lua_State *state, const ECSIFunction *service, ECSPlugin giver, ECSIParameter parameter, const void *argument, int callbacks)
{
    ECSFunction pointer = nullptr;
    SDL_memcpy(&pointer, argument, sizeof(pointer));

    if (pointer == nullptr)
    {
        lua_pushnil(state);
        return;
    }

    // the record borrows the service's name and the callback's signature, which live as long as the service; one that is not kept is never used after the call
    ECSIFunction *callback = lua_newuserdata(state, sizeof(ECSIFunction));
    *callback = (ECSIFunction){.name = service->name, .plugin = service->plugin, .signature = *parameter.callback, .pointer = pointer, .lua = LUA_NOREF, .anchors = LUA_NOREF};
    callback->giver = giver;
    callback->generation = giver == nullptr ? 0 : ECSIPlugin_GetGeneration(giver);

    if (!parameter.keep)
    {
        lua_pushvalue(state, -1);
        lua_rawseti(state, callbacks, (int)lua_rawlen(state, callbacks) + 1);
    }

    lua_pushcclosure(state, ECSIServices_CallC, 1);
}

/// @brief Makes the callbacks that C passed to a Lua function invalid, now that the call returned.
static void ECSIServices_EndCallbacks(lua_State *state, int callbacks)
{
    for (int i = 1; i <= (int)lua_rawlen(state, callbacks); i++)
    {
        lua_rawgeti(state, callbacks, i);
        ((ECSIFunction *)lua_touserdata(state, -1))->pointer = nullptr;
        lua_pop(state, 1);
    }
}

/// @brief Gives a Lua function a new, empty result value for C, in place of the one its last call gave. A call does it when its Lua code has returned,
/// so a call of the same function inside it does not free the value that the outer caller gets.
static void ECSIServices_RenewResult(ECSIFunction *function, void *result)
{
    if (function->signature.result.type == ECSIParameterType_Value)
    {
        ECSValue_Destroy(&function->result);

        if (!ECSValue_Create(&function->result))
        {
            *(const ECSValue **)result = function->result;
        }
    }
}

/// @brief Calls a Lua function for C, with the arguments and the result as libffi gives them. Output parameters are read from the extra results.
/// @param caller The plugin whose C code calls, which gives the callbacks it passes, or nullptr if it is not known.
static void ECSIServices_RunLua(ECSIFunction *function, ECSPlugin caller, void *result, void **arguments)
{
    SDL_assert(SDL_IsMainThread());
    const ECSISignature *signature = &function->signature;

    // a failed call gives C a zero result
    SDL_memset(result, 0, SDL_max(sizeof(ffi_arg), ECSIServices_GetFfiType(signature->result)->size));

    // a C function that Lua called through the FFI may be compiled code, which Lua cannot run inside (DESIGN 10.4)
    if (ECSILua_InFfiCall())
    {
        const ECSIFunction *running = *ECSILua_GetFfiCall();
        char *message = nullptr;
        bool written = SDL_asprintf(&message, "'%s' called the Lua function '%s' while Lua called it; a C function that Lua calls calls Lua only through its callbacks",
                                    running->name, function->name) >= 0;
        ECSIPlugin_NoteMisuse(running->plugin, written ? message : "a C function that Lua calls called Lua");
        SDL_free(message);
        return;
    }

    lua_State *state = ECSILua_GetState();
    int top = lua_gettop(state);
    usz count = arrlenu(signature->parameters);
    int resultCount = (signature->result.type != ECSIParameterType_Void ? 1 : 0) + (int)signature->outCount;

    // the plugin that gave the Lua function was removed, so its code never runs again
    if (function->inert)
    {
        ECSIServices_RenewResult(function, result);
        return;
    }

    lua_newtable(state);
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
            ECSIServices_PushCallback(state, function, caller, parameter, arguments[i], callbacks);
            pushed++;
        }
        else if (!parameter.out)
        {
            ECSIServices_PushArgument(state, parameter, arguments[i]);
            pushed++;
        }
    }

    // the Lua function's time is its plugin's, not the C code's that called it
    ECSIProfileScope profile;
    ECSIProfile_Enter(&profile, function->plugin, function->plugin != nullptr ? ECSIPlugin_GetName(function->plugin) : nullptr);
    bool failed = ECSILua_Call(state, pushed, resultCount) != SHUResult_Ok;
    ECSIProfile_Leave(&profile);
    ECSIServices_EndCallbacks(state, callbacks);

    // the new anchor table replaces the last call's only now, so what C got from the last call stayed valid until the Lua code returned,
    // also when that code called this function again
    lua_pushvalue(state, anchors);
    lua_rawseti(state, LUA_REGISTRYINDEX, function->anchors);
    ECSIServices_RenewResult(function, result);

    if (failed)
    {
        ECSIPlugin_ReportError(function->plugin, lua_tostring(state, -1));
        lua_settop(state, top);
        return;
    }

    int index = callbacks + 1;

    // a Lua function that failed gives nil for its results, and why last: C gets zeros for them
    // one that worked gives nil for why, its last output, which C gets as nullptr (DESIGN 10.8)
    int last = callbacks + resultCount;
    bool failedCall = signature->fails && lua_type(state, last) == LUA_TSTRING;

    if (signature->result.type != ECSIParameterType_Void && !ECSIServices_WriteOutput(state, function, index++, anchors, signature->result, result, true) &&
        !(failedCall && lua_isnil(state, index - 1)))
    {
        ECSIServices_ReportType(state, function, index - 1);
    }

    for (usz i = 0; i < count; i++)
    {
        if (signature->parameters[i].out && !ECSIServices_WriteOutput(state, function, index++, anchors, signature->parameters[i], *(void **)arguments[i], false) &&
            !((failedCall || (signature->fails && index - 1 == last)) && lua_isnil(state, index - 1)))
        {
            ECSIServices_ReportType(state, function, index - 1);
        }
    }

    lua_settop(state, top);
}

/// @brief The handler of a Lua function's closure: C calls it through the closure's code.
static void ECSIServices_CallLua(ffi_cif *cif, void *result, void **arguments, void *data)
{
    (void)cif;
    ECSIServices_RunLua(data, nullptr, result, arguments);
}

/// @brief The handler of the closure that one plugin's C code calls for a Lua function.
static void ECSIServices_CallLuaFor(ffi_cif *cif, void *result, void **arguments, void *data)
{
    (void)cif;
    ECSICaller *caller = data;
    ECSIServices_RunLua(caller->function, caller->plugin, result, arguments);
}

/// @brief Gives the code that a plugin's C code calls for a function. A Lua function that keeps callbacks gets code of its own for each plugin, made when
/// the plugin first asks, so a callback it keeps runs nothing once the plugin that gave it is disabled; every other function has one pointer.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation.
static bool ECSIFfi_CallsLua(ECSPlugin plugin);

/// @brief Forgets the Lua functions made for a plugin's C functions, so a later lookup gets them through the C API, now that the plugin calls Lua.
static void ECSIFfi_ForgetWrappers(ECSPlugin plugin)
{
    lua_State *state = ECSILua_GetState();

    for (usz i = 0; i < shlenu(SERVICES.functions); i++)
    {
        ECSIFunction *function = SERVICES.functions[i].value;

        for (usz j = 0; function->plugin == plugin && j < hmlenu(function->callers); j++)
        {
            luaL_unref(state, LUA_REGISTRYINDEX, function->callers[j].value);
        }

        if (function->plugin == plugin)
        {
            hmfree(function->callers);
        }
    }
}

static SHUResult ECSIFunction_GetPointerFor(ECSIFunction *function, ECSPlugin plugin, ECSFunction *retPointer)
{
    bool keeps = false;

    for (usz i = 0; function->lua != LUA_NOREF && i < arrlenu(function->signature.parameters); i++)
    {
        keeps = keeps || function->signature.parameters[i].keep;
    }

    if (keeps && hmgeti(function->stubs, plugin) < 0)
    {
        void *code = nullptr;
        ECSICaller *caller = SDL_calloc(1, sizeof(ECSICaller));

        if (caller == nullptr || (caller->closure = ffi_closure_alloc(sizeof(ffi_closure), &code)) == nullptr ||
            ffi_prep_closure_loc(caller->closure, &function->signature.cif, ECSIServices_CallLuaFor, caller, code) != FFI_OK)
        {
            if (caller != nullptr && caller->closure != nullptr)
            {
                ffi_closure_free(caller->closure);
            }

            SDL_free(caller);
            return SHUResult_ErrAllocation;
        }

        caller->function = function;
        caller->plugin = plugin;
        SDL_memcpy(&caller->pointer, &code, sizeof(code));
        hmput(function->stubs, plugin, caller);
    }

    // a plugin that can call Lua is not called through the FFI, as Lua cannot run inside compiled code (DESIGN 10.4)
    if (plugin != nullptr && (function->lua != LUA_NOREF || ECSIFfi_CallsLua(function->plugin)) && !hmget(SERVICES.luaCallers, plugin))
    {
        hmput(SERVICES.luaCallers, plugin, true);
        ECSIFfi_ForgetWrappers(plugin);
    }

    *retPointer = keeps ? hmget(function->stubs, plugin)->pointer : function->pointer;
    return SHUResult_Ok;
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

/// @brief Lets go of a callback's Lua function and anchors, and of what it last returned, so its closure waits for another call without keeping them.
static void ECSIServices_ParkCallback(ECSIFunction *callback)
{
    lua_State *state = ECSILua_GetState();
    luaL_unref(state, LUA_REGISTRYINDEX, callback->lua);
    luaL_unref(state, LUA_REGISTRYINDEX, callback->anchors);
    callback->lua = LUA_NOREF;
    callback->anchors = LUA_NOREF;
    ECSValue_Destroy(&callback->result);
}

/// @brief Frees a parked callback, which holds nothing of Lua's, so it needs no Lua state.
static void ECSIServices_FreeSpare(ECSIFunction **spare)
{
    ffi_closure_free((*spare)->closure);
    SDL_free((*spare)->name);
    SDL_free(*spare);
    *spare = nullptr;
}

#pragma endregion C Calls Lua

#pragma region Lua Calls C Through the FFI

/// @brief The helpers of Lua's wrappers of C functions (DESIGN 10.4). The chunk gets the FFI, a table of the C functions that the wrappers call and of
/// Lua C functions for the rare cases, and the registry; it gives the table of helpers that each wrapper gets.
static const char OPENECS_FFI_HELPERS[] =
    "local ffi, C = ...\n"
    "ffi.cdef('struct ECSIFfiSlice { void *data; size_t size; };')\n"
    "local H = {\n"
    "  ffi = ffi,\n"
    "  slice = ffi.typeof('struct ECSIFfiSlice'),\n"

    "  bytes = ffi.typeof('uint8_t[?]'),\n"
    "  current = ffi.cast('void **', C.current),\n"
    "  misuses = ffi.cast('uint64_t (*)(void)', C.misuses),\n"
    "  profiling = ffi.cast('bool (*)(void)', C.profiling),\n"
    "  enter = ffi.cast('void (*)(void *, void *, const char *)', C.enter),\n"
    "  leave = ffi.cast('void (*)(void *)', C.leave),\n"
    "  parseColor = ffi.cast('bool (*)(const char *, uint32_t *)', C.parseColor),\n"
    "}\n"
    "for name, fn in pairs(C.lua) do H[name] = fn end\n"
    "local format, floor = string.format, math.floor\n"
    "function H.text(color) return format('#%06X%02X', color % 16777216, floor(color / 16777216)) end\n"
    "H.bytesOf = C.bytesOf\n"
    "function H.copy(text)\n"
    "  local length = #text\n"
    "  local bytes = H.bytes(length + 1)\n"
    "  ffi.copy(bytes, text, length)\n"
    "  return bytes, length\n"
    "end\n"
    "return H\n";

/// @brief Gives a pointer that Lua passes: a light userdata, or a pointer cdata, whose bytes are where lua_topointer points.
static void *ECSIFfi_ToPointer(lua_State *state, int index)
{
    if (lua_type(state, index) == OPENECS_LUA_TCDATA)
    {
        return *(void *const *)lua_topointer(state, index);
    }

    return lua_touserdata(state, index);
}

/// @brief H.argError(argument, message): a bad argument of the wrapper that calls it.
static int ECSIFfi_ArgError(lua_State *state)
{
    ECSILua_ArgErrorAt(state, 1, (int)lua_tointeger(state, 1), lua_tostring(state, 2));
}

/// @brief H.typeError(argument, typeName, value): an argument of the wrapper that calls it has another type.
static int ECSIFfi_TypeError(lua_State *state)
{
    ECSILua_TypeErrorAt(state, 1, (int)lua_tointeger(state, 1), 3, lua_tostring(state, 2));
}

/// @brief H.number(argument, value): a number, or a text that Lua reads as one.
static int ECSIFfi_Number(lua_State *state)
{
    int isNumber = 0;
    lua_Number number = lua_tonumberx(state, 2, &isNumber);

    if (!isNumber)
    {
        ECSILua_TypeErrorAt(state, 1, (int)lua_tointeger(state, 1), 2, lua_typename(state, LUA_TNUMBER));
    }

    lua_pushnumber(state, number);
    return 1;
}

/// @brief H.int(argument, value) and H.int64(argument, value): a whole number that fits, or for an int64 an int64_t or uint64_t cdata, which goes to C as it is.
static int ECSIFfi_Integer(lua_State *state, bool wide)
{
    int argument = (int)lua_tointeger(state, 1);
    i64 integer = 0;
    int isInteger = 0;

    if (wide)
    {
        isInteger = ECSILua_ToInt64(state, 2, &integer);
    }
    else
    {
        integer = (i64)lua_tointegerx(state, 2, &isInteger);
    }

    if (!isInteger && lua_isnumber(state, 2))
    {
        ECSILua_ArgErrorAt(state, 1, argument, "number has no integer representation");
    }

    if (!isInteger)
    {
        ECSILua_TypeErrorAt(state, 1, argument, 2, lua_typename(state, LUA_TNUMBER));
    }

    if (!wide && (integer < SDL_MIN_SINT32 || integer > SDL_MAX_SINT32))
    {
        ECSILua_ArgErrorAt(state, 1, argument, "does not fit in an int");
    }

    if (lua_type(state, 2) == OPENECS_LUA_TCDATA)
    {
        lua_pushvalue(state, 2);
    }
    else
    {
        lua_pushnumber(state, (lua_Number)integer);
    }

    return 1;
}

static int ECSIFfi_Int(lua_State *state)
{
    return ECSIFfi_Integer(state, false);
}

static int ECSIFfi_Int64(lua_State *state)
{
    return ECSIFfi_Integer(state, true);
}

/// @brief H.string(argument, value): a text, or a number as Lua writes it.
static int ECSIFfi_String(lua_State *state)
{
    if (lua_tostring(state, 2) == nullptr)
    {
        ECSILua_TypeErrorAt(state, 1, (int)lua_tointeger(state, 1), 2, lua_typename(state, LUA_TSTRING));
    }

    lua_pushvalue(state, 2);
    return 1;
}

/// @brief H.color(argument, value): a colour that the wrapper could not read at once, or the error that it is none.
static int ECSIFfi_Color(lua_State *state)
{
    int argument = (int)lua_tointeger(state, 1);
    const char *text = lua_tostring(state, 2);

    if (text == nullptr)
    {
        ECSILua_TypeErrorAt(state, 1, argument, 2, lua_typename(state, LUA_TSTRING));
    }

    ECSColor color = 0;

    if (!ECSColor_Parse(text, &color))
    {
        ECSILua_ArgErrorAt(state, 1, argument, "is not a colour: \"#RRGGBB\", \"#RRGGBBAA\" or a colour of the theme, such as \"text\"");
    }

    lua_pushnumber(state, (lua_Number)color);
    return 1;
}

/// @brief H.value(value): a new ECSValue of a Lua value, as a light userdata, or nil if it cannot be made.
static int ECSIFfi_Value(lua_State *state)
{
    ECSValue *value = nullptr;

    if (ECSValue_Create(&value))
    {
        return 0;
    }

    if (ECSILua_GetValue(state, 1, value))
    {
        ECSValue_Destroy(&value);
        return 0;
    }

    lua_pushlightuserdata(state, value);
    return 1;
}

/// @brief H.newValue(): a new empty ECSValue for an output, or nil if it cannot be made.
static int ECSIFfi_NewValue(lua_State *state)
{
    ECSValue *value = nullptr;

    if (ECSValue_Create(&value))
    {
        return 0;
    }

    lua_pushlightuserdata(state, value);
    return 1;
}

/// @brief H.pushValue(value, destroy): the Lua value of an ECSValue, which it destroys when destroy is true.
static int ECSIFfi_PushValue(lua_State *state)
{
    ECSValue *value = ECSIFfi_ToPointer(state, 1);
    ECSILua_PushValue(state, value);

    if (lua_toboolean(state, 2))
    {
        ECSValue_Destroy(&value);
    }

    return 1;
}

/// @brief H.free(...): destroys the ECSValues that H.value and H.newValue made; nil is skipped.
static int ECSIFfi_Free(lua_State *state)
{
    for (int i = 1; i <= lua_gettop(state); i++)
    {
        ECSValue *value = ECSIFfi_ToPointer(state, i);
        ECSValue_Destroy(&value);
    }

    return 0;
}

/// @brief H.struct(argument, value, type): the memory of a struct read from a table of its fields, or from a struct inside another, as a userdata,
/// which the FFI passes as a pointer.
static int ECSIFfi_Struct(lua_State *state)
{
    int argument = (int)lua_tointeger(state, 1);
    const ECSIStructType *type = lua_touserdata(state, 3);
    void *memory = lua_newuserdata(state, type->size);
    SDL_memset(memory, 0, type->size);
    const char *field = nullptr;

    if (!ECSIStructs_Read(state, 2, type, memory, &field))
    {
        const char *problem = field == nullptr ? lua_pushfstring(state, "is not a %s, a table of its fields", type->name)
                                               : lua_pushfstring(state, "is not a %s: its field '%s' is missing or of another type", type->name, field);
        ECSILua_ArgErrorAt(state, 1, argument, problem);
    }

    return 1;
}

/// @brief H.pushHandle(type, object): the handle of an object that C gave, or nil.
static int ECSIFfi_PushHandle(lua_State *state)
{
    ECSIServices_PushHandleOf(state, lua_touserdata(state, 1), ECSIFfi_ToPointer(state, 2));
    return 1;
}

/// @brief H.raise(): the misuse that the C function noted, raised at the line that called the wrapper, as a misuse in a C function is (DESIGN 1.4).
static int ECSIFfi_Raise(lua_State *state)
{
    ECSIPlugin_ReportDeferredMisuse(state);
    luaL_where(state, 2);
    lua_pushstring(state, ECSIPlugin_GetLastMisuse());
    lua_concat(state, 2);
    return lua_error(state);
}

/// @brief H.cannot(name): a value that cannot be made for a call.
static int ECSIFfi_Cannot(lua_State *state)
{
    ECSILua_MisuseAt(state, 2, "cannot convert a value for '%s'", lua_tostring(state, 1));
}

/// @brief Sets a field of the table on the top of the stack to a C function, as a light userdata; POSIX lets a data pointer hold code.
static void ECSIFfi_SetCode(lua_State *state, const char *name, void (*code)(void))
{
    void *pointer = nullptr;
    SDL_memcpy(&pointer, &code, sizeof(pointer));
    lua_pushlightuserdata(state, pointer);
    lua_setfield(state, -2, name);
}

/// @brief Pushes the helpers of the wrappers, which are made with the first wrapper.
static SHUResult ECSIFfi_PushHelpers(lua_State *state)
{
    if (SERVICES.ffiHelpers != LUA_NOREF)
    {
        lua_rawgeti(state, LUA_REGISTRYINDEX, SERVICES.ffiHelpers);
        return SHUResult_Ok;
    }

    static const luaL_Reg HELPERS[] = {
        {"argError", ECSIFfi_ArgError},
        {"typeError", ECSIFfi_TypeError},
        {"number", ECSIFfi_Number},
        {"int", ECSIFfi_Int},
        {"int64", ECSIFfi_Int64},
        {"string", ECSIFfi_String},
        {"color", ECSIFfi_Color},
        {"value", ECSIFfi_Value},
        {"newValue", ECSIFfi_NewValue},
        {"pushValue", ECSIFfi_PushValue},
        {"free", ECSIFfi_Free},
        {"struct", ECSIFfi_Struct},
        {"pushHandle", ECSIFfi_PushHandle},
        {"raise", ECSIFfi_Raise},
        {"cannot", ECSIFfi_Cannot},
        {nullptr, nullptr},
    };

    if (luaL_loadbuffer(state, OPENECS_FFI_HELPERS, sizeof(OPENECS_FFI_HELPERS) - 1, "=ecs.ffi") != LUA_OK)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "The FFI's helpers do not load: %s", lua_tostring(state, -1));
        lua_pop(state, 1);
        return SHUResult_Err;
    }

    lua_getglobal(state, "require");
    lua_pushliteral(state, "ffi");
    lua_call(state, 1, 1);

    lua_newtable(state);
    lua_pushlightuserdata(state, (void *)ECSILua_GetFfiCall());
    lua_setfield(state, -2, "current");
    ECSIFfi_SetCode(state, "misuses", (void (*)(void))ECSIPlugin_GetMisuseCount);
    ECSIFfi_SetCode(state, "profiling", (void (*)(void))ECSIProfile_IsOn);
    ECSIFfi_SetCode(state, "enter", (void (*)(void))ECSIProfile_Enter);
    ECSIFfi_SetCode(state, "leave", (void (*)(void))ECSIProfile_Leave);
    ECSIFfi_SetCode(state, "parseColor", (void (*)(void))ECSColor_Parse);
    ECSILua_PushBytesOf(state);
    lua_setfield(state, -2, "bytesOf");
    lua_newtable(state);
    luaL_setfuncs(state, HELPERS, 0);
    lua_setfield(state, -2, "lua");

    if (ECSILua_Call(state, 2, 1))
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "The FFI's helpers do not start: %s", lua_tostring(state, -1));
        lua_pop(state, 1);
        return SHUResult_Err;
    }

    lua_pushvalue(state, -1);
    SERVICES.ffiHelpers = luaL_ref(state, LUA_REGISTRYINDEX);
    return SHUResult_Ok;
}

/// @brief Tells whether a plugin's C code may call Lua outside its functions' callbacks: it keeps callbacks, or calls Lua functions that it looked up.
static bool ECSIFfi_CallsLua(ECSPlugin plugin)
{
    if (plugin == nullptr || hmget(SERVICES.luaCallers, plugin))
    {
        return true;
    }

    for (usz i = 0; i < shlenu(SERVICES.functions); i++)
    {
        const ECSIFunction *function = SERVICES.functions[i].value;

        for (usz j = 0; function->plugin == plugin && j < arrlenu(function->signature.parameters); j++)
        {
            if (function->signature.parameters[j].keep)
            {
                return true;
            }
        }
    }

    return false;
}

/// @brief Tells whether Lua calls a function through a wrapper of the FFI: a plugin's C function whose code calls no Lua. The JIT compiles such a
/// call with its caller, and Lua cannot run inside compiled code; the others, and the core's own functions, which may run Lua such as a panel's
/// Create, are called through the C API.
static bool ECSIFfi_Wraps(const ECSIFunction *function)
{
    if (function->pointer == nullptr || function->lua != LUA_NOREF || ECSIFfi_CallsLua(function->plugin))
    {
        return false;
    }

    for (usz i = 0; i < arrlenu(function->signature.parameters); i++)
    {
        if (function->signature.parameters[i].type == ECSIParameterType_Callback)
        {
            return false;
        }
    }

    return true;
}

/// @brief The C type through which the FFI passes a parameter or a result of a type.
static const char *ECSIFfi_GetCType(ECSIParameterType type)
{
    switch (type)
    {
    case ECSIParameterType_Bool:
        return "bool";
    case ECSIParameterType_Int:
        return "int32_t";
    case ECSIParameterType_Int64:
        return "int64_t";
    case ECSIParameterType_Float:
        return "float";
    case ECSIParameterType_Double:
        return "double";
    case ECSIParameterType_Color:
        return "uint32_t";
    case ECSIParameterType_String:
        return "const char *";
    case ECSIParameterType_Buffer:
        return "struct ECSIFfiSlice";
    case ECSIParameterType_Void:
        return "void";
    default:
        // handles, values and structs: a pointer
        return "void *";
    }
}

/// @brief Tells whether an output parameter is a pointer to storage that the wrapper keeps, as opposed to a value or a struct that each call makes.
static bool ECSIFfi_IsKeptOutput(ECSIParameter parameter)
{
    return parameter.out && parameter.type != ECSIParameterType_Struct && parameter.type != ECSIParameterType_Value;
}

/// @brief Adds the C type of a function pointer of a signature, such as "void (*)(void *, float)".
static void ECSIFfi_AddFunctionType(ECSIText *text, const ECSISignature *signature)
{
    ECSIText_Add(text, "%s (*)(", ECSIFfi_GetCType(signature->result.type));

    for (usz i = 0; i < arrlenu(signature->parameters); i++)
    {
        ECSIParameter parameter = signature->parameters[i];
        ECSIText_Add(text, "%s%s%s", i > 0 ? ", " : "", ECSIFfi_GetCType(parameter.type), ECSIFfi_IsKeptOutput(parameter) ? " *" : "");
    }

    ECSIText_Add(text, ")");
}

/// @brief Adds the Lua code that checks the argument of a parameter, a, into what C gets, p, both numbered by the parameter.
static void ECSIFfi_AddCheck(ECSIText *text, ECSIParameter parameter, usz i, int argument)
{
    switch (parameter.type)
    {
    case ECSIParameterType_Bool:
        ECSIText_Add(text, "  local p%zu = not not a%zu\n", i, i);
        break;
    case ECSIParameterType_Int:
        ECSIText_Add(text, "  local p%zu = a%zu\n  if type(p%zu) ~= 'number' or p%zu %% 1 ~= 0 or p%zu < -2147483648 or p%zu > 2147483647 then p%zu = H.int(%d, p%zu) end\n", i, i, i,
                     i, i, i, i, argument, i);
        break;
    case ECSIParameterType_Int64:
        ECSIText_Add(text, "  local p%zu = a%zu\n  if type(p%zu) ~= 'number' or p%zu %% 1 ~= 0 then p%zu = H.int64(%d, p%zu) end\n", i, i, i, i, i, argument, i);
        break;
    case ECSIParameterType_Float:
    case ECSIParameterType_Double:
        ECSIText_Add(text, "  local p%zu = a%zu\n  if type(p%zu) ~= 'number' then p%zu = H.number(%d, p%zu) end\n", i, i, i, i, argument, i);
        break;
    case ECSIParameterType_String:
        ECSIText_Add(text, "  local p%zu = a%zu\n  if type(p%zu) ~= 'string' then p%zu = H.string(%d, p%zu) end\n", i, i, i, i, argument, i);
        break;
    case ECSIParameterType_Color:
        ECSIText_Add(text, "  local p%zu\n  if type(a%zu) == 'string' and parseColor(a%zu, color) then p%zu = color[0] else p%zu = H.color(%d, a%zu) end\n", i, i, i, i, i, argument, i);
        break;
    case ECSIParameterType_Buffer:
        // a string is copied, because the function may write into a buffer and Lua strings must not change; other buffers give their own bytes
        // (DESIGN 10.7); the bytes stay a local during the call
        ECSIText_Add(text, "  local bytes%zu, length%zu\n  if type(a%zu) == 'string' then bytes%zu, length%zu = copy(a%zu) else bytes%zu, length%zu = H.bytesOf(a%zu) end\n", i, i, i,
                     i, i, i, i, i, i);
        ECSIText_Add(text, "  if bytes%zu == nil then H.typeError(%d, 'string, string.buffer or cdata', a%zu) end\n", i, argument, i);
        ECSIText_Add(text, "  local p%zu = s%zu\n  p%zu.data, p%zu.size = bytes%zu, length%zu\n", i, i, i, i, i, i);
        break;
    case ECSIParameterType_Handle:
        ECSIText_Add(text, "  if not istype(C%zu, a%zu) then H.typeError(%d, M%zu, a%zu) end\n", i, i, argument, i, i);
        ECSIText_Add(text, "  local p%zu = a%zu.ecsObject\n  if p%zu == nil then H.argError(%d, 'the object of this handle is gone') end\n", i, i, i, argument);
        break;
    case ECSIParameterType_Struct:
        // a struct is passed as it is, and a table of its fields is read
        ECSIText_Add(text, "  local p%zu = a%zu\n  if not istype(C%zu, p%zu) then p%zu = H.struct(%d, a%zu, T%zu) end\n", i, i, i, i, i, argument, i, i);
        break;
    default:
        break;
    }
}

/// @brief Adds the code that converts what C gave, an expression, into a result of Lua, r and its number; type names the local of its handle's or
/// struct's type.
static void ECSIFfi_AddResult(ECSIText *text, ECSIParameter parameter, usz number, const char *from, const char *type)
{
    switch (parameter.type)
    {
    case ECSIParameterType_Int64:
        ECSIText_Add(text, "  local r%zu = tonumber(%s)\n", number, from);
        break;
    case ECSIParameterType_Color:
        ECSIText_Add(text, "  local r%zu = text(%s)\n", number, from);
        break;
    case ECSIParameterType_String:
        ECSIText_Add(text, "  local r%zu = %s\n  if r%zu == nil then r%zu = nil else r%zu = ffi.string(r%zu) end\n", number, from, number, number, number, number);
        break;
    case ECSIParameterType_Buffer:
        ECSIText_Add(text, "  local r%zu = %s\n  if r%zu.data == nil then r%zu = '' else r%zu = ffi.string(r%zu.data, r%zu.size) end\n", number, from, number, number, number, number,
                     number);
        break;
    case ECSIParameterType_Handle:
        ECSIText_Add(text, "  local r%zu = H.pushHandle(%s, %s)\n", number, type, from);
        break;
    case ECSIParameterType_Value:
        ECSIText_Add(text, "  local r%zu = H.pushValue(%s, %s)\n", number, from, parameter.out ? "true" : "false");
        break;
    case ECSIParameterType_Struct:
        // the struct that C filled
        ECSIText_Add(text, "  local r%zu = %s\n", number, from);
        break;
    default:
        // booleans and numbers are Lua's as they are
        ECSIText_Add(text, "  local r%zu = %s\n", number, from);
        break;
    }
}

/// @brief Adds the arguments of H.free: the values that a call made, of the parameters before one.
static void ECSIFfi_AddValues(ECSIText *text, const ECSISignature *signature, usz before)
{
    for (usz i = 0; i < before; i++)
    {
        if (signature->parameters[i].type == ECSIParameterType_Value)
        {
            ECSIText_Add(text, ", p%zu", i);
        }
    }
}

/// @brief Adds the locals that a wrapper keeps between its calls: the function, its helpers, and storage for the outputs.
static void ECSIFfi_AddHeader(ECSIText *text, const ECSISignature *signature)
{
    ECSIText_Add(text, "local H, F = ...\nlocal ffi, cast, istype, copy, text, parseColor = H.ffi, H.ffi.cast, H.ffi.istype, H.copy, H.text, H.parseColor\n");
    ECSIText_Add(text, "local current, misuses, profiling, enter, leave = H.current, H.misuses, H.profiling, H.enter, H.leave\n");
    ECSIText_Add(text, "local call = cast(F.ctype, F.pointer)\nlocal FUNCTION, PLUGIN, NAME = cast('void *', F.called), cast('void *', F.plugin), cast('const char *', F.name)\n");
    ECSIText_Add(text, "local scope, color = H.bytes(F.scope), ffi.new('uint32_t[1]')\nlocal M, T, C = F.metatables, F.types, F.ctypes\nlocal TR = T[0]\n");

    for (usz i = 0; i < arrlenu(signature->parameters); i++)
    {
        ECSIParameter parameter = signature->parameters[i];

        if (ECSIFfi_IsKeptOutput(parameter))
        {
            ECSIText_Add(text, "local o%zu = ffi.new('%s[1]')\n", i, ECSIFfi_GetCType(parameter.type));
        }
        else if (parameter.type == ECSIParameterType_Buffer)
        {
            ECSIText_Add(text, "local s%zu = H.slice()\n", i);
        }

        if (parameter.type == ECSIParameterType_Handle || parameter.type == ECSIParameterType_Struct)
        {
            ECSIText_Add(text, "local M%zu, T%zu, C%zu = M[%zu], T[%zu], C[%zu]\n", i, i, i, i + 1, i + 1, i + 1);
        }
    }
}

/// @brief Adds what a wrapper does before the call: it checks the arguments, then makes the values, which allocate, and clears the outputs.
static void ECSIFfi_AddArguments(ECSIText *text, const ECSISignature *signature)
{
    int argument = 0;

    for (usz i = 0; i < arrlenu(signature->parameters); i++)
    {
        if (!signature->parameters[i].out)
        {
            ECSIFfi_AddCheck(text, signature->parameters[i], i, ++argument);
        }
    }

    for (usz i = 0; i < arrlenu(signature->parameters); i++)
    {
        ECSIParameter parameter = signature->parameters[i];

        if (parameter.type == ECSIParameterType_Value)
        {
            ECSIText_Add(text, parameter.out ? "  local p%zu = H.newValue()\n" : "  local p%zu = H.value(a%zu)\n", i, i);
            ECSIText_Add(text, "  if p%zu == nil then H.free(nil", i);
            ECSIFfi_AddValues(text, signature, i);
            ECSIText_Add(text, ") H.cannot(F.full) end\n");
        }
        else if (parameter.out && parameter.type == ECSIParameterType_Struct)
        {
            ECSIText_Add(text, "  local p%zu = C%zu()\n", i, i);
        }
        else if (ECSIFfi_IsKeptOutput(parameter))
        {
            const char *zero = parameter.type == ECSIParameterType_Bool                                                   ? "false"
                               : parameter.type == ECSIParameterType_String || parameter.type == ECSIParameterType_Handle ? "nil"
                               : parameter.type == ECSIParameterType_Buffer                                               ? "H.slice()"
                                                                                                                          : "0";
            ECSIText_Add(text, "  o%zu[0] = %s\n  local p%zu = o%zu\n", i, zero, i, i);
        }
    }
}

/// @brief Adds the call and what a wrapper does after it: it raises a misuse that the function noted, and gives nil and why when it failed.
static void ECSIFfi_AddCall(ECSIText *text, const ECSISignature *signature)
{
    usz count = arrlenu(signature->parameters);

    // the function is the core's while it runs: Lua must not run, and its time is its plugin's, not the caller's (DESIGN 3.5)
    ECSIText_Add(text, "  local before, profiled = misuses(), profiling()\n  if profiled then enter(scope, PLUGIN, NAME) end\n");
    ECSIText_Add(text, "  local outer = current[0]\n  current[0] = FUNCTION\n  %scall(", signature->result.type != ECSIParameterType_Void ? "local r = " : "");

    for (usz i = 0; i < count; i++)
    {
        ECSIText_Add(text, "%sp%zu", i > 0 ? ", " : "", i);
    }

    ECSIText_Add(text, ")\n  current[0] = outer\n  if profiled then leave(scope) end\n  if misuses() ~= before then H.free(nil");
    ECSIFfi_AddValues(text, signature, count);
    ECSIText_Add(text, ") H.raise() end\n");

    // a function that failed gives nil for each result, and why last, as Lua's io.open does
    if (signature->fails)
    {
        ECSIText_Add(text, "  if o%zu[0] ~= nil then\n    H.free(nil", count - 1);
        ECSIFfi_AddValues(text, signature, count);
        ECSIText_Add(text, ")\n    return ");
        usz results = signature->outCount - 1 + (signature->result.type != ECSIParameterType_Void ? 1 : 0);

        for (usz i = 0; i < results; i++)
        {
            ECSIText_Add(text, "nil, ");
        }

        ECSIText_Add(text, "ffi.string(o%zu[0])\n  end\n", count - 1);
    }
}

/// @brief Adds the results of a wrapper, frees the values of its call, and returns the result, then the outputs.
static void ECSIFfi_AddReturn(ECSIText *text, const ECSISignature *signature)
{
    usz count = arrlenu(signature->parameters);
    bool result = signature->result.type != ECSIParameterType_Void;

    if (result)
    {
        ECSIFfi_AddResult(text, signature->result, 0, "r", "TR");
    }

    for (usz i = 0; i < count; i++)
    {
        ECSIParameter parameter = signature->parameters[i];

        if (parameter.out)
        {
            char from[32];
            char type[32];
            (void)SDL_snprintf(from, sizeof(from), ECSIFfi_IsKeptOutput(parameter) ? "o%zu[0]" : "p%zu", i);
            (void)SDL_snprintf(type, sizeof(type), "T%zu", i);
            ECSIFfi_AddResult(text, parameter, i + 1, from, type);
        }
    }

    // the values that the call made are freed; a call without them calls nothing more, so the JIT compiles it whole
    bool values = false;

    for (usz i = 0; i < count; i++)
    {
        if (signature->parameters[i].type == ECSIParameterType_Value && !signature->parameters[i].out)
        {
            ECSIText_Add(text, values ? ", p%zu" : "  H.free(p%zu", i);
            values = true;
        }
    }

    ECSIText_Add(text, values ? ")\n  return " : "  return ");
    bool first = true;

    if (result)
    {
        ECSIText_Add(text, "r0");
        first = false;
    }

    for (usz i = 0; i < count; i++)
    {
        if (signature->parameters[i].out)
        {
            ECSIText_Add(text, "%sr%zu", first ? "" : ", ", i + 1);
            first = false;
        }
    }

    ECSIText_Add(text, "\nend\n");
}

/// @brief Writes the Lua source of a function's wrapper.
static ECSIText ECSIFfi_Write(const ECSIFunction *function)
{
    const ECSISignature *signature = &function->signature;
    ECSIText text = {0};
    ECSIFfi_AddHeader(&text, signature);
    ECSIText_Add(&text, "return function(");
    bool first = true;

    for (usz i = 0; i < arrlenu(signature->parameters); i++)
    {
        if (!signature->parameters[i].out)
        {
            ECSIText_Add(&text, "%sa%zu", first ? "" : ", ", i);
            first = false;
        }
    }

    ECSIText_Add(&text, ")\n");
    ECSIFfi_AddArguments(&text, signature);
    ECSIFfi_AddCall(&text, signature);
    ECSIFfi_AddReturn(&text, signature);
    return text;
}

/// @brief Sets the fields of a wrapper's constants about its handles and structs, by the parameter's number from 1 and the result's at 0: metatables,
/// the names of the handles' types, types, the handle or struct types themselves, and ctypes, the handles' FFI types.
static void ECSIFfi_SetTypes(lua_State *state, const ECSISignature *signature)
{
    lua_newtable(state);
    lua_newtable(state);
    lua_newtable(state);

    for (usz i = 0; i <= arrlenu(signature->parameters); i++)
    {
        ECSIParameter parameter = i == 0 ? signature->result : signature->parameters[i - 1];

        if (parameter.type == ECSIParameterType_Handle)
        {
            lua_pushstring(state, parameter.handle->metatable);
            lua_rawseti(state, -4, (int)i);
            lua_pushlightuserdata(state, parameter.handle);
            lua_rawseti(state, -3, (int)i);
            ECSIServices_PushHandleCType(state, parameter.handle);
            lua_rawseti(state, -2, (int)i);
        }
        else if (parameter.type == ECSIParameterType_Struct)
        {
            lua_pushlightuserdata(state, parameter.structType);
            lua_rawseti(state, -3, (int)i);
            ECSIStructs_PushCType(state, parameter.structType);
            lua_rawseti(state, -2, (int)i);
        }
    }

    lua_setfield(state, -4, "ctypes");
    lua_setfield(state, -3, "types");
    lua_setfield(state, -2, "metatables");
}

/// @brief Pushes the constants of a function's wrapper: its C type, its pointer, its plugin and the types of its handles and structs.
static SHUResult ECSIFfi_PushConstants(lua_State *state, const ECSIFunction *function)
{
    ECSIText type = {0};
    ECSIFfi_AddFunctionType(&type, &function->signature);

    if (type.failed)
    {
        return SHUResult_ErrAllocation;
    }

    void *pointer = nullptr;
    SDL_memcpy(&pointer, &function->pointer, sizeof(pointer));

    lua_newtable(state);
    lua_pushstring(state, type.data);
    SDL_free(type.data);
    lua_setfield(state, -2, "ctype");
    lua_pushlightuserdata(state, pointer);
    lua_setfield(state, -2, "pointer");
    lua_pushlightuserdata(state, (void *)function);
    lua_setfield(state, -2, "called");
    lua_pushlightuserdata(state, function->plugin);
    lua_setfield(state, -2, "plugin");
    lua_pushlightuserdata(state, (void *)ECSIPlugin_GetName(function->plugin));
    lua_setfield(state, -2, "name");
    lua_pushstring(state, function->name);
    lua_setfield(state, -2, "full");
    lua_pushinteger(state, (lua_Integer)sizeof(ECSIProfileScope));
    lua_setfield(state, -2, "scope");
    ECSIFfi_SetTypes(state, &function->signature);
    return SHUResult_Ok;
}

/// @brief Pushes a new wrapper of a C function, which calls it through the FFI (DESIGN 10.4).
static SHUResult ECSIFfi_PushWrapper(lua_State *state, const ECSIFunction *function)
{
    ECSIText source = ECSIFfi_Write(function);
    char *chunk = nullptr;

    if (source.failed || SDL_asprintf(&chunk, "=%s", function->name) < 0)
    {
        SDL_free(source.data);
        return SHUResult_ErrAllocation;
    }

    int loaded = luaL_loadbuffer(state, source.data, SDL_strlen(source.data), chunk);
    SDL_free(chunk);
    SDL_free(source.data);

    if (loaded != LUA_OK)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "The wrapper of '%s' does not load: %s", function->name, lua_tostring(state, -1));
        lua_pop(state, 1);
        return SHUResult_Err;
    }

    SHU_ReturnResult(ECSIFfi_PushHelpers(state), lua_pop(state, 1););
    SHU_ReturnResult(ECSIFfi_PushConstants(state, function), lua_pop(state, 2););

    if (ECSILua_Call(state, 2, 1))
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "The wrapper of '%s' does not start: %s", function->name, lua_tostring(state, -1));
        lua_pop(state, 1);
        return SHUResult_Err;
    }

    return SHUResult_Ok;
}

#pragma endregion Lua Calls C Through the FFI

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
        (void)ECS_Check(nullptr, false, "Function '%s' is not named after the plugin that registers it.", name);
        return SHUResult_ErrAssertion;
    }

    if (ECSIServices_FindFunction(name) != nullptr)
    {
        (void)ECS_Check(nullptr, false, "Function '%s' is already registered.", name);
        return SHUResult_ErrAssertion;
    }

    const ECSIFunction *clash = plugin == nullptr ? nullptr : ECSIServices_FindNamespaceClash(plugin, name);

    if (clash != nullptr)
    {
        (void)ECS_Check(nullptr, false, "Function '%s' cannot be registered: '%s' is a function, and a name's part before a dot is a namespace.", name, clash->name);
        return SHUResult_ErrAssertion;
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

/// @brief Pushes the table of the fields of the struct at an index of the core's state's stack, for values; false for another value.
static bool ECSIServices_PushStructFields(int index)
{
    lua_State *state = ECSILua_GetState();
    void *memory = nullptr;
    const ECSIStructType *type = ECSIStructs_GetType(state, index, &memory);

    if (type != nullptr)
    {
        ECSIStructs_PushFields(state, type, memory);
    }

    return type != nullptr;
}

/// @brief Reads a Lua handle for a value: its type's name and its object.
static bool ECSIServices_ReadHandle(int index, const char **retType, void **retObject)
{
    lua_State *state = ECSILua_GetState();

    ECSIHandle *found = ECSIServices_ToHandle(state, index);

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

            // a C function takes its buffers' bytes from more than strings (DESIGN 10.7)
            if (parameter.type == ECSIParameterType_Buffer && function->lua == LUA_NOREF)
            {
                ECSIText_Add(lua, "string|string.buffer|ffi.cdata*");
            }
            else
            {
                ECSIDefinition_AddLuaType(lua, parameter);
            }

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
    ECSIText_Add(lua, "\n---A struct of the type %s: plain data, cdata of its own FFI type; a function also takes a table of its fields.\n---@class %s", type->name, type->name);

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

            if (SDL_strcmp(type->methods[i], OPENECS_STRUCT_OPERATORS[j]) != 0 || SDL_strcmp(type->methods[i], "tostring") == 0 || SDL_strcmp(type->methods[i], "eq") == 0)
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
    SERVICES.ffiHelpers = LUA_NOREF;

    if (luaL_loadbuffer(state, OPENECS_HANDLE_HELPERS, sizeof(OPENECS_HANDLE_HELPERS) - 1, "=ecs.handles") != LUA_OK || ECSILua_Call(state, 0, 1))
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "The handles' Lua functions do not start: %s", lua_tostring(state, -1));
        lua_pop(state, 1);
        return SHUResult_ErrInternal;
    }

    SERVICES.handleHelpers = luaL_ref(state, LUA_REGISTRYINDEX);
    ECSILua_SetValueFunctions(ECSIServices_ReadHandle, ECSIServices_PushHandle, ECSIServices_PushStructFields);

    // the core's handle types: panels, popups, and the surface a panel or popup draws into, valid during Draw
    SHU_ReturnResult(ECSIServices_RegisterHandleType(nullptr, "ecs.panel", nullptr));
    SHU_ReturnResult(ECSIServices_RegisterHandleType(nullptr, "ecs.popup", nullptr));
    return ECSIServices_RegisterHandleType(nullptr, "ecs.surface", nullptr);
}

void ECSIServices_Raise(lua_State *state, SHUResult result, const char *format, ...)
{
    SDL_assert(state != nullptr);
    SDL_assert(result != SHUResult_Ok);

    if (result == SHUResult_ErrAllocation)
    {
        luaL_error(state, "out of memory");
    }

    if (result == SHUResult_ErrAssertion)
    {
        luaL_error(state, "%s", ECSIPlugin_GetLastMisuse());
    }

    va_list arguments;
    va_start(arguments, format);
    const char *message = lua_pushvfstring(state, format, arguments);
    va_end(arguments);
    ECSILua_Misuse(state, "%s (%s)", message, SHUResult_String(result));
}

/// @brief Follows the setting ecs.logServiceErrors, for every thread.
static void ECSIServices_LogErrorsChanged(void *data)
{
    (void)data;
    SDL_SetAtomicInt(&SERVICES.logErrors, ECSValue_GetBool(ECSSetting_Get(OPENECS_LOG_SERVICE_ERRORS), true));
}

SHUResult ECSIServices_DeclareSettings(void)
{
    ECSSettingDesc desc = {
        .name = OPENECS_LOG_SERVICE_ERRORS,
        .type = ECSSettingType_Bool,
        .description = "Log the failures that services give their callers, such as a file that cannot be read",
        .Changed = ECSIServices_LogErrorsChanged,
    };
    SHU_ReturnResult(ECSISettings_DeclareCore(&desc));
    ECSIServices_LogErrorsChanged(nullptr);
    return SHUResult_Ok;
}

void ECS_Fail(ECSPlugin plugin, const char **retError, const char *format, ...)
{
    if (!ECS_Check(nullptr, plugin != nullptr && retError != nullptr && format != nullptr, "ECS_Fail needs a plugin, an error to write and a format"))
    {
        return;
    }

    va_list arguments;
    va_start(arguments, format);
    char *message = nullptr;
    bool formatted = SDL_vasprintf(&message, format, arguments) >= 0;
    va_end(arguments);

    // the arguments may name the last message, such as an inner call's error, so it is freed only now
    SDL_free(SERVICES_THREAD.error);
    SERVICES_THREAD.error = formatted ? message : nullptr;
    *retError = SERVICES_THREAD.error != nullptr ? SERVICES_THREAD.error : "out of memory";

    if (SDL_GetAtomicInt(&SERVICES.logErrors) != 0)
    {
        ECS_Log(plugin, ECSLogLevel_Warning, "%s", *retError);
    }
}

bool ECS_Check(ECSPlugin plugin, bool condition, const char *format, ...)
{
    if (condition)
    {
        return true;
    }

    char *message = nullptr;

    if (format != nullptr)
    {
        va_list arguments;
        va_start(arguments, format);
        message = SDL_vasprintf(&message, format, arguments) < 0 ? nullptr : message;
        va_end(arguments);
    }

    ECSIPlugin_NoteMisuse(plugin, message != nullptr ? message : "a check failed");
    SDL_free(message);
    return false;
}

void ECSIServices_PushHandle(const char *type, void *object)
{
    SDL_assert(type != nullptr);
    ECSIServices_PushHandleOf(ECSILua_GetState(), ECSIServices_FindHandleType(type), object);
}

/// @brief Pushes a handle of an object of a type, or nil for no object or no type.
static void ECSIServices_PushHandleOf(lua_State *state, ECSIHandleType *handleType, void *object)
{
    if (object == nullptr)
    {
        lua_pushnil(state);
        return;
    }

    // the type is gone when its plugin was removed, and its objects when its plugin was disabled; a handle cannot be made then
    if (handleType == nullptr || handleType->kept)
    {
        lua_pushnil(state);
        return;
    }

    lua_rawgeti(state, LUA_REGISTRYINDEX, SERVICES.handles);

    const ECSIHandle *cached = lua_rawgetp(state, -1, object) == OPENECS_LUA_TCDATA ? lua_topointer(state, -1) : nullptr;

    if (cached != nullptr && cached->type == handleType && cached->object == object)
    {
        lua_remove(state, -2);
        return;
    }

    lua_pop(state, 1);

    // the cdata is made before the object's record is found: making it may run the garbage collector, whose finalizer of the object's last
    // other handle frees the record. A cdata's memory does not move, so the record keeps its address
    ECSIServices_PushHandleCType(state, handleType);
    lua_call(state, 0, 1);
    ECSIHandle *handle = (ECSIHandle *)lua_topointer(state, -1);
    *handle = (ECSIHandle){0};
    hmput(SERVICES.liveHandles, handle, true);

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
            lua_pop(state, 2);
            lua_pushnil(state);
            return;
        }

        record->type = handleType;
        hmput(SERVICES.objects, object, record);
    }

    *handle = (ECSIHandle){.object = object, .type = handleType};
    arrput(record->handles, handle);
    lua_pushvalue(state, -1);
    lua_rawsetp(state, -3, object);
    lua_remove(state, -2);
}

void *ECSIServices_CheckHandle(int index, const char *type)
{
    SDL_assert(type != nullptr);

    lua_State *state = ECSILua_GetState();
    ECSIHandleType *handleType = nullptr;

    for (usz i = 0; i < SDL_arraysize(SERVICES.coreTypes) && handleType == nullptr; i++)
    {
        handleType = SERVICES.coreTypes[i].name == type ? SERVICES.coreTypes[i].type : nullptr;
    }

    if (handleType == nullptr)
    {
        handleType = ECSIServices_FindHandleType(type);

        // the core's types live until exit, so they are kept by the address of their names
        for (usz i = 0; i < SDL_arraysize(SERVICES.coreTypes) && handleType != nullptr && handleType->plugin == nullptr; i++)
        {
            if (SERVICES.coreTypes[i].name == nullptr)
            {
                SERVICES.coreTypes[i].name = type;
                SERVICES.coreTypes[i].type = handleType;
                break;
            }
        }
    }

    if (handleType == nullptr)
    {
        ECSILua_Misuse(state, "handle type '%s' is gone; its plugin was removed", type);
        return nullptr;
    }

    return ECSIServices_CheckHandleOf(state, index, handleType);
}

/// @brief Checks that a Lua argument is a handle of a type whose object is there, and gives the object; raises a Lua error otherwise.
static void *ECSIServices_CheckHandleOf(lua_State *state, int index, ECSIHandleType *handleType)
{
    ECSIHandle *handle = ECSIServices_ToHandle(state, index);

    if (handle == nullptr || handle->type != handleType)
    {
        ECSILua_TypeError(state, index, handleType->metatable);
    }

    if (handle->object == nullptr)
    {
        ECSILua_ArgError(state, index, "the object of this handle is gone");
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
    ECSILog_Check(surface != nullptr, SHUResult_ErrAssertion);
    ECSILog_Check(Drawn != nullptr, SHUResult_ErrAssertion);

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

void ECSIServices_EndThread(void)
{
    SDL_free(SERVICES_THREAD.error);
    SERVICES_THREAD.error = nullptr;
}

void ECSIServices_DestroyHandles(void)
{
    // handles that wait for their finalizer are no longer in the weak table, so a full collection runs those finalizers now, while the types and their plugins' code exist
    lua_gc(ECSILua_GetState(), LUA_GCCOLLECT, 0);

    // objects are destroyed while their plugins' code is still loaded
    ECSIServices_DropHandles(nullptr);
}

void ECSIServices_DestroyHandlesOfPlugin(ECSPlugin plugin)
{
    SDL_assert(plugin != nullptr);

    // as at exit: finalizers that wait run first, while the plugin's code is loaded
    lua_gc(ECSILua_GetState(), LUA_GCCOLLECT, 0);

    for (usz i = 0; i < shlenu(SERVICES.handleTypes); i++)
    {
        if (SERVICES.handleTypes[i].value->plugin == plugin)
        {
            ECSIServices_DropHandles(SERVICES.handleTypes[i].value);
        }
    }
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
    hmfree(SERVICES.liveHandles);

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
    hmfree(SERVICES.luaCallers);
    arrfree(SERVICES.drawn);
    ECSILua_SetValueFunctions(nullptr, nullptr, nullptr);
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
            SERVICES.structTypes[i].value->kept = true;
        }
    }
}

void ECSIServices_RemovePlugin(ECSPlugin plugin)
{
    SDL_assert(plugin != nullptr);

    (void)hmdel(SERVICES.luaCallers, plugin);

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

            ECSIServices_DropHandleMethods(type);

            for (usz j = 0; j < arrlenu(type->methods); j++)
            {
                SDL_free(type->methods[j]);
            }

            arrfree(type->methods);
            arrfree(type->targets);
            type->kept = true;
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

        if (hmgeti(function->stubs, plugin) >= 0)
        {
            ECSICaller *caller = hmget(function->stubs, plugin);
            (void)hmdel(function->stubs, plugin);
            ffi_closure_free(caller->closure);
            SDL_free(caller);
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

    // one Lua function per C function and plugin, made when the plugin first asks for it, so a callback knows which plugin gave it; a function
    // without callbacks is a wrapper that calls it through the FFI, which the JIT compiles with its caller (DESIGN 10.4)
    if (hmgeti(function->callers, plugin) < 0)
    {
        if (ECSIFfi_Wraps(function))
        {
            SHU_ReturnResult(ECSIFfi_PushWrapper(state, function));
        }
        else
        {
            lua_pushlightuserdata(state, function);
            lua_pushlightuserdata(state, plugin);
            lua_pushcclosure(state, ECSIServices_CallC, 2);
        }

        hmput(function->callers, plugin, luaL_ref(state, LUA_REGISTRYINDEX));
    }

    lua_rawgeti(state, LUA_REGISTRYINDEX, hmget(function->callers, plugin));
    return SHUResult_Ok;
}

SHUResult ECSService_RegisterFunction(ECSPlugin plugin, const char *name, ECSFunction function, const char *signature, const char *description)
{
    ECSILog_Check(SDL_IsMainThread(), SHUResult_ErrAssertion);
    ECSILog_Check(plugin != nullptr, SHUResult_ErrAssertion);
    ECSILog_Check(name != nullptr, SHUResult_ErrAssertion);
    ECSILog_Check(function != nullptr, SHUResult_ErrAssertion);
    ECSILog_Check(signature != nullptr, SHUResult_ErrAssertion);

    // the plugin gives the local name, and the function is known by its full name
    char *fullName = nullptr;
    ECSIFunction *registered = nullptr;
    SHUResult result = ECSIPlugin_MakeFullName(plugin, name, &fullName);
    result = result ? result : ECSIServices_CreateFunction(plugin, fullName, signature, description, &registered);
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

    bool plain = SDL_strcmp(function->signature.text, "void()") == 0;

    if (plain || SDL_strcmp(function->signature.text, "void(handle<ecs.panel>)") == 0)
    {
        // a misuse in the function is its plugin's, as in any other callback
        ECSICallback callback;
        ECSIPlugin_Enter(&callback, function->plugin, nullptr, ECSICallbackType_Plugin);

        // a C function and a Lua function's closure are called the same way
        if (plain)
        {
            function->pointer();
        }
        else
        {
            ((ECSIPanelFunction)function->pointer)(focus);
        }

        (void)ECSIPlugin_Leave(&callback);
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

    ECSICallback callback;
    ECSIPlugin_Enter(&callback, function->plugin, nullptr, ECSICallbackType_Plugin);
    ((ECSIOpenFunction)function->pointer)(path);
    (void)ECSIPlugin_Leave(&callback);
    return SHUResult_Ok;
}

SHUResult ECSIServices_ListBindable(ECSValue *retList)
{
    SDL_assert(retList != nullptr);

    ECSValue_SetTable(retList);

    for (usz i = 0; i < shlenu(SERVICES.functions); i++)
    {
        const ECSIFunction *function = SERVICES.functions[i].value;
        bool panel = SDL_strcmp(function->signature.text, "void(handle<ecs.panel>)") == 0;

        if (!panel && SDL_strcmp(function->signature.text, "void()") != 0)
        {
            continue;
        }

        ECSValue *item = nullptr;
        ECSValue *field = nullptr;
        SHU_ReturnResult(ECSValue_AddListItem(retList, &item));
        ECSValue_SetTable(item);
        SHU_ReturnResult(ECSValue_SetTableField(item, "name", &field));
        SHU_ReturnResult(ECSValue_SetString(field, function->name));
        SHU_ReturnResult(ECSValue_SetTableField(item, "description", &field));
        SHU_ReturnResult(ECSValue_SetString(field, function->description != nullptr ? function->description : ""));
        SHU_ReturnResult(ECSValue_SetTableField(item, "owner", &field));
        SHU_ReturnResult(ECSValue_SetString(field, function->plugin != nullptr ? ECSIPlugin_GetName(function->plugin) : "ecs"));
        SHU_ReturnResult(ECSValue_SetTableField(item, "panel", &field));
        ECSValue_SetBool(field, panel);
    }

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
    ECSILog_Check(SDL_IsMainThread(), SHUResult_ErrAssertion);
    ECSILog_Check(plugin != nullptr, SHUResult_ErrAssertion);
    ECSILog_Check(type != nullptr, SHUResult_ErrAssertion);
    ECSILog_Check(method != nullptr, SHUResult_ErrAssertion);
    ECSILog_Check(function != nullptr, SHUResult_ErrAssertion);

    char *typeName = nullptr;
    char *functionName = nullptr;
    (void)ECSIPlugin_MakeFullName(plugin, type, &typeName);
    (void)ECSIPlugin_MakeFullName(plugin, function, &functionName);
    ECSIHandleType *handleType = typeName == nullptr ? nullptr : ECSIServices_FindHandleType(typeName);
    ECSIFunction *target = functionName == nullptr ? nullptr : ECSIServices_FindFunction(functionName);
    SHUResult result = SHUResult_Ok;

    // the method is a function of the same plugin whose first parameter is a handle of the type, which the handle itself gives
    if (handleType == nullptr || handleType->plugin != plugin || target == nullptr || arrlenu(target->signature.parameters) == 0 ||
        target->signature.parameters[0].type != ECSIParameterType_Handle || target->signature.parameters[0].handle != handleType)
    {
        (void)ECS_Check(nullptr, false, "'%s' cannot be a method of '%s': it needs to be a function of the plugin whose first parameter is a handle<%s>.", functionName != nullptr ? functionName : function, typeName != nullptr ? typeName : type, typeName != nullptr ? typeName : type);
        result = SHUResult_ErrAssertion;
    }

    for (usz i = 0; !result && i < arrlenu(handleType->methods); i++)
    {
        if (SDL_strcmp(handleType->methods[i], method) == 0)
        {
            (void)ECS_Check(nullptr, false, "Handle type '%s' already has a method '%s'.", typeName, method);
            result = SHUResult_ErrAssertion;
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
    ECSILog_Check(plugin != nullptr, SHUResult_ErrAssertion);
    ECSILog_Check(name != nullptr, SHUResult_ErrAssertion);
    ECSILog_Check(fields != nullptr, SHUResult_ErrAssertion);

    char *fullName = nullptr;
    SHUResult result = ECSIPlugin_MakeFullName(plugin, name, &fullName);
    result = result ? result : ECSIStructs_Register(plugin, fullName, fields);
    SDL_free(fullName);
    return result;
}

SHUResult ECSIServices_RegisterCoreStruct(const char *name, const char *fields)
{
    SDL_assert(SDL_strncmp(name, "ecs.", 4) == 0);
    SDL_assert(fields != nullptr);

    return ECSIStructs_Register(nullptr, name, fields);
}

/// @brief Sets the function on top of the stack as a method of a struct type, which is also an operator if its name makes one; pops it.
static void ECSIStructs_SetMethod(lua_State *state, const ECSIStructType *type, const char *method)
{
    luaL_getmetatable(state, type->metatable);
    lua_insert(state, -2);
    lua_setfield(state, -2, method);
    lua_pop(state, 1);
}

SHUResult ECSStruct_AddMethod(ECSPlugin plugin, const char *type, const char *method, const char *function)
{
    ECSILog_Check(plugin != nullptr, SHUResult_ErrAssertion);
    ECSILog_Check(type != nullptr, SHUResult_ErrAssertion);
    ECSILog_Check(method != nullptr, SHUResult_ErrAssertion);
    ECSILog_Check(function != nullptr, SHUResult_ErrAssertion);

    char *typeName = nullptr;
    char *functionName = nullptr;
    (void)ECSIPlugin_MakeFullName(plugin, type, &typeName);
    (void)ECSIPlugin_MakeFullName(plugin, function, &functionName);
    ECSIStructType *structType = typeName == nullptr ? nullptr : ECSIServices_FindStructType(typeName);
    ECSIFunction *target = functionName == nullptr ? nullptr : ECSIServices_FindFunction(functionName);
    SHUResult result = SHUResult_Ok;

    // the method is a function of the same plugin whose first parameter is a struct of the type, which the struct itself gives
    if (structType == nullptr || structType->plugin != plugin || target == nullptr || arrlenu(target->signature.parameters) == 0 ||
        target->signature.parameters[0].type != ECSIParameterType_Struct || target->signature.parameters[0].out || target->signature.parameters[0].structType != structType)
    {
        (void)ECS_Check(nullptr, false, "'%s' cannot be a method of '%s': it needs to be a function of the plugin whose first parameter is a struct<%s>.", functionName != nullptr ? functionName : function, typeName != nullptr ? typeName : type, typeName != nullptr ? typeName : type);
        result = SHUResult_ErrAssertion;
    }

    for (usz i = 0; !result && i < arrlenu(structType->methods); i++)
    {
        if (SDL_strcmp(structType->methods[i], method) == 0)
        {
            (void)ECS_Check(nullptr, false, "Struct type '%s' already has a method '%s'.", typeName, method);
            result = SHUResult_ErrAssertion;
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
    ECSIStructType *structType = ECSIServices_FindStructType(type);

    if (structType == nullptr)
    {
        ECSILua_Misuse(state, "struct type '%s' is not registered", type);
        return;
    }

    // the fields are checked and copied, so the new struct holds only its own fields, with its type's methods
    const char *field = nullptr;
    void *memory = lua_newuserdata(state, structType->size);
    SDL_memset(memory, 0, structType->size);

    if (!ECSIStructs_Read(state, index, structType, memory, &field))
    {
        ECSILua_Misuse(state, field == nullptr ? "a %s is made from a table of its fields" : "a %s needs its field '%s', of its type", type, field);
        return;
    }

    ECSIStructs_Push(state, structType, memory);
    lua_remove(state, -2);
}

SHUResult ECSIServices_RegisterLuaHandleType(ECSPlugin plugin, const char *name)
{
    SDL_assert(plugin != nullptr);
    SDL_assert(name != nullptr);

    char *fullName = nullptr;
    SHUResult result = ECSIPlugin_MakeFullName(plugin, name, &fullName);
    result = result ? result : ECSIServices_RegisterHandleType(plugin, fullName, ECSIServices_DestroyLuaObject);
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
    ECSILog_Check(SDL_IsMainThread(), SHUResult_ErrAssertion);
    ECSILog_Check(plugin != nullptr, SHUResult_ErrAssertion);
    ECSILog_Check(name != nullptr, SHUResult_ErrAssertion);

    char *fullName = nullptr;
    SHUResult result = ECSIPlugin_MakeFullName(plugin, name, &fullName);
    result = result ? result : ECSIServices_RegisterHandleType(plugin, fullName, Destroy);
    SDL_free(fullName);
    return result;
}

void ECSHandle_Forget(void *object)
{
    ECSILog_Check(SDL_IsMainThread());
    ECSILog_Check(object != nullptr);

    ECSIServices_DropObject(object);
}

void ECSHandle_Free(ECSPlugin plugin, const char *type, void *object)
{
    ECSILog_Check(SDL_IsMainThread());
    ECSILog_Check(plugin != nullptr);
    ECSILog_Check(type != nullptr);
    ECSILog_Check(object != nullptr);

    // Lua may have no handle of the object, so its type is found by name
    char *fullName = nullptr;
    (void)ECSIPlugin_MakeFullName(plugin, type, &fullName);
    ECSIHandleType *handleType = fullName == nullptr ? nullptr : ECSIServices_FindHandleType(fullName);
    SDL_free(fullName);
    ECSILog_Check(handleType != nullptr && handleType->plugin == plugin);

    ECSIServices_DropObject(object);

    ECSIServices_DestroyObject(handleType, object);
}

SHUResult ECSService_GetFunction(ECSPlugin plugin, ECSFunction *retFunction, const char *name, const char *signature)
{
    ECSILog_Check(SDL_IsMainThread(), SHUResult_ErrAssertion);
    ECSILog_Check(plugin != nullptr, SHUResult_ErrAssertion);
    ECSILog_Check(retFunction != nullptr, SHUResult_ErrAssertion);
    ECSILog_Check(name != nullptr, SHUResult_ErrAssertion);
    ECSILog_Check(signature != nullptr, SHUResult_ErrAssertion);

    ECSIFunction *function = nullptr;
    SHU_ReturnResult(ECSIServices_Find(plugin, name, signature, &function));
    return ECSIFunction_GetPointerFor(function, plugin, retFunction);
}

void ECSService_ReleaseCallback(ECSFunction callback)
{
    ECSILog_Check(SDL_IsMainThread());
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
