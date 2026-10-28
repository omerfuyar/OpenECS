#include "base/Values.h"

#include "SDL3/SDL.h"
#include "stb/stbSDL3.h"

#pragma region Source Only

struct ECSIValue
{
    ECSValueType type;

    // type chooses the member that is set
    union
    {
        bool boolean;
        i64 integer;
        f64 number;
        char *string;

        // buffer
        struct
        {
            u8 *bytes;
            usz size;
        };

        // handle
        struct
        {
            char *handleType;
            void *object;
            int reference; // a registry reference to the Lua handle it was read from, or 0 for one that C set
        };

        // table
        struct
        {
            ECSValue **items; // stb_ds array; callers keep pointers to items, so each is allocated on its own
            struct
            {
                char *key;
                ECSValue *value;
            } *fields; // stb_ds hash map with copied keys, in the order fields were added
        };
    };
};

/// @brief The functions for the Lua handles that values keep, from the Lua module.
static struct
{
    ECSIValueKeepFunction Keep;
    ECSIValueDropFunction Drop;
    ECSIValueObjectFunction Object;
} VALUES = {0};

/// @brief Frees what a value holds and makes it nil.
static void ECSIValue_Clear(ECSValue *value)
{
    if (value->type == ECSValueType_String)
    {
        SDL_free(value->string);
    }
    else if (value->type == ECSValueType_Buffer)
    {
        SDL_free(value->bytes);
    }
    else if (value->type == ECSValueType_Handle)
    {
        SDL_free(value->handleType);

        if (value->reference != 0 && VALUES.Drop != nullptr)
        {
            VALUES.Drop(value->reference);
        }
    }
    else if (value->type == ECSValueType_Table)
    {
        for (usz i = 0; i < arrlenu(value->items); i++)
        {
            ECSValue_Destroy(&value->items[i]);
        }

        for (usz i = 0; i < shlenu(value->fields); i++)
        {
            ECSValue_Destroy(&value->fields[i].value);
        }

        arrfree(value->items);
        shfree(value->fields);
    }

    SDL_zerop(value);
}

static void ECSIValue_MakeTable(ECSValue *value)
{
    if (value->type != ECSValueType_Table)
    {
        ECSIValue_Clear(value);
        value->type = ECSValueType_Table;
    }
}

#pragma endregion Source Only

SHUResult ECSValue_Create(ECSValue **retValue)
{
    SDL_assert(retValue != nullptr);

    *retValue = SDL_calloc(1, sizeof(ECSValue));
    return *retValue == nullptr ? SHUResult_ErrAllocation : SHUResult_Ok;
}

void ECSValue_Destroy(ECSValue **value)
{
    SDL_assert(value != nullptr);

    if (*value != nullptr)
    {
        ECSIValue_Clear(*value);
        SDL_free(*value);
        *value = nullptr;
    }
}

SHUResult ECSIValue_Copy(ECSValue *retCopy, const ECSValue *source)
{
    SDL_assert(retCopy != nullptr);
    SDL_assert(retCopy != source);

    switch (ECSValue_GetType(source))
    {
    case ECSValueType_Nil:
        ECSValue_SetNil(retCopy);
        return SHUResult_Ok;
    case ECSValueType_Bool:
        ECSValue_SetBool(retCopy, source->boolean);
        return SHUResult_Ok;
    case ECSValueType_Integer:
        ECSValue_SetInteger(retCopy, source->integer);
        return SHUResult_Ok;
    case ECSValueType_Number:
        ECSValue_SetNumber(retCopy, source->number);
        return SHUResult_Ok;
    case ECSValueType_String:
        return ECSValue_SetString(retCopy, source->string);
    case ECSValueType_Buffer:
        return ECSValue_SetBuffer(retCopy, (SHUSliceView){.data = source->bytes, .size = source->size});
    case ECSValueType_Handle:
        return source->reference != 0 && VALUES.Keep != nullptr ? ECSIValue_SetLuaHandle(retCopy, source->handleType, source->object, VALUES.Keep(source->reference))
                                                                : ECSValue_SetHandle(retCopy, source->handleType, source->object);
    case ECSValueType_Table:
        break;
    }

    ECSValue_SetTable(retCopy);

    for (usz i = 0; i < arrlenu(source->items); i++)
    {
        ECSValue *item = nullptr;
        SHU_ReturnResult(ECSValue_AddListItem(retCopy, &item));
        SHU_ReturnResult(ECSIValue_Copy(item, source->items[i]));
    }

    for (usz i = 0; i < shlenu(source->fields); i++)
    {
        ECSValue *field = nullptr;
        SHU_ReturnResult(ECSValue_SetTableField(retCopy, source->fields[i].key, &field));
        SHU_ReturnResult(ECSIValue_Copy(field, source->fields[i].value));
    }

    return SHUResult_Ok;
}

bool ECSIValue_Equal(const ECSValue *first, const ECSValue *second)
{
    ECSValueType type = ECSValue_GetType(first);

    if (type != ECSValue_GetType(second))
    {
        return false;
    }

    switch (type)
    {
    case ECSValueType_Nil:
        return true;
    case ECSValueType_Bool:
        return first->boolean == second->boolean;
    case ECSValueType_Integer:
        return first->integer == second->integer;
    case ECSValueType_Number:
        return first->number == second->number;
    case ECSValueType_String:
        return SDL_strcmp(first->string, second->string) == 0;
    case ECSValueType_Buffer:
        return first->size == second->size && (first->size == 0 || SDL_memcmp(first->bytes, second->bytes, first->size) == 0);
    case ECSValueType_Handle:
        return ECSValue_GetHandle(first, first->handleType) == ECSValue_GetHandle(second, first->handleType) && SDL_strcmp(first->handleType, second->handleType) == 0;
    case ECSValueType_Table:
        break;
    }

    if (arrlenu(first->items) != arrlenu(second->items) || shlenu(first->fields) != shlenu(second->fields))
    {
        return false;
    }

    for (usz i = 0; i < arrlenu(first->items); i++)
    {
        if (!ECSIValue_Equal(first->items[i], second->items[i]))
        {
            return false;
        }
    }

    for (usz i = 0; i < shlenu(first->fields); i++)
    {
        if (!ECSIValue_Equal(first->fields[i].value, ECSValue_GetTableField(second, first->fields[i].key)))
        {
            return false;
        }
    }

    return true;
}

void ECSIValue_TableForEachField(const ECSValue *table, ECSIValueFieldFunction function, void *userData)
{
    SDL_assert(function != nullptr);

    if (ECSValue_GetType(table) != ECSValueType_Table)
    {
        return;
    }

    for (usz i = 0; i < shlenu(table->fields); i++)
    {
        function(table->fields[i].key, table->fields[i].value, userData);
    }
}

bool ECSIValue_CheckFields(const ECSValue *table, const ECSIFieldRule *rules, usz count, char **retProblem)
{
    SDL_assert(rules != nullptr || count == 0);
    SDL_assert(retProblem != nullptr);

    static const char *const TYPE_NAMES[] = {"any value", "true or false", "a whole number", "a number", "a text", "a table"};
    *retProblem = nullptr;

    if (ECSValue_GetType(table) != ECSValueType_Table)
    {
        *retProblem = SDL_strdup("it must be a table");
        return false;
    }

    for (usz i = 0; i < shlenu(table->fields); i++)
    {
        const char *name = table->fields[i].key;
        const ECSValue *field = table->fields[i].value;
        const ECSIFieldRule *rule = nullptr;

        for (usz j = 0; j < count && rule == nullptr; j++)
        {
            rule = SDL_strcmp(rules[j].name, name) == 0 ? &rules[j] : nullptr;
        }

        if (rule == nullptr)
        {
            SDL_asprintf(retProblem, "'%s' is not a field it can have", name);
            return false;
        }

        ECSValueType type = ECSValue_GetType(field);
        bool matches = true;

        switch (rule->type)
        {
        case ECSIFieldType_Any:
            break;
        case ECSIFieldType_Bool:
            matches = type == ECSValueType_Bool;
            break;
        case ECSIFieldType_Integer:
            matches = ECSIValue_IsInteger(field);
            break;
        case ECSIFieldType_Number:
            matches = type == ECSValueType_Integer || type == ECSValueType_Number;
            break;
        case ECSIFieldType_String:
            matches = type == ECSValueType_String;
            break;
        case ECSIFieldType_Table:
            matches = type == ECSValueType_Table;
            break;
        }

        if (!matches)
        {
            SDL_asprintf(retProblem, "'%s' must be %s", name, TYPE_NAMES[rule->type]);
            return false;
        }
    }

    for (usz i = 0; i < count; i++)
    {
        if (rules[i].required && ECSValue_GetTableField(table, rules[i].name) == nullptr)
        {
            SDL_asprintf(retProblem, "it needs the field '%s'", rules[i].name);
            return false;
        }
    }

    return true;
}

bool ECSIValue_IsVersion(const char *text)
{
    usz numbers = 0;

    while (text != nullptr && numbers < 3 && SDL_isdigit((unsigned char)*text))
    {
        while (SDL_isdigit((unsigned char)*text))
        {
            text++;
        }

        numbers++;

        if (*text != '.')
        {
            break;
        }

        text++;
    }

    return numbers > 0 && text != nullptr && *text == '\0';
}

bool ECSIValue_IsInteger(const ECSValue *value)
{
    switch (ECSValue_GetType(value))
    {
    case ECSValueType_Integer:
        return true;
    case ECSValueType_Number:
        // the range check keeps the conversion to an integer defined
        return value->number >= -0x1p63 && value->number < 0x1p63 && value->number == SDL_floor(value->number);
    default:
        return false;
    }
}

ECSValueType ECSValue_GetType(const ECSValue *value)
{
    return value == nullptr ? ECSValueType_Nil : value->type;
}

bool ECSValue_GetBool(const ECSValue *value, bool fallback)
{
    return ECSValue_GetType(value) == ECSValueType_Bool ? value->boolean : fallback;
}

i64 ECSValue_GetInteger(const ECSValue *value, i64 fallback)
{
    switch (ECSValue_GetType(value))
    {
    case ECSValueType_Integer:
        return value->integer;
    case ECSValueType_Number:
        return ECSIValue_IsInteger(value) ? (i64)value->number : fallback;
    default:
        return fallback;
    }
}

f64 ECSValue_GetNumber(const ECSValue *value, f64 fallback)
{
    switch (ECSValue_GetType(value))
    {
    case ECSValueType_Integer:
        return (f64)value->integer;
    case ECSValueType_Number:
        return value->number;
    default:
        return fallback;
    }
}

const char *ECSValue_GetString(const ECSValue *value, const char *fallback)
{
    return ECSValue_GetType(value) == ECSValueType_String ? value->string : fallback;
}

SHUSlice ECSValue_GetBuffer(const ECSValue *value, SHUSlice fallback)
{
    switch (ECSValue_GetType(value))
    {
    case ECSValueType_Buffer:
        return (SHUSlice){.data = value->bytes, .size = value->size};
    case ECSValueType_String:
        return (SHUSlice){.data = value->string, .size = SDL_strlen(value->string)};
    default:
        return fallback;
    }
}

const char *ECSIValue_GetHandleType(const ECSValue *value)
{
    return ECSValue_GetType(value) == ECSValueType_Handle ? value->handleType : nullptr;
}

void *ECSValue_GetHandle(const ECSValue *value, const char *type)
{
    SDL_assert(type != nullptr);

    if (ECSValue_GetType(value) != ECSValueType_Handle || SDL_strcmp(value->handleType, type) != 0)
    {
        return nullptr;
    }

    // a handle read from Lua gives its object only while the object lives
    return value->reference != 0 && VALUES.Object != nullptr ? VALUES.Object(value->reference) : value->object;
}

int ECSIValue_GetLuaHandle(const ECSValue *value)
{
    return ECSValue_GetType(value) == ECSValueType_Handle ? value->reference : 0;
}

void ECSIValue_SetLuaFunctions(ECSIValueKeepFunction Keep, ECSIValueDropFunction Drop, ECSIValueObjectFunction Object)
{
    VALUES.Keep = Keep;
    VALUES.Drop = Drop;
    VALUES.Object = Object;
}

usz ECSValue_GetListCount(const ECSValue *table)
{
    return ECSValue_GetType(table) == ECSValueType_Table ? arrlenu(table->items) : 0;
}

const ECSValue *ECSValue_GetListItem(const ECSValue *table, usz index)
{
    return index < ECSValue_GetListCount(table) ? table->items[index] : nullptr;
}

const ECSValue *ECSValue_GetTableField(const ECSValue *table, const char *name)
{
    SDL_assert(name != nullptr);

    // shget would allocate a map that is missing
    if (ECSValue_GetType(table) != ECSValueType_Table || table->fields == nullptr)
    {
        return nullptr;
    }

    // the cast is safe: shget only reads a map that exists, but its macro takes a modifiable one
    return shget(((ECSValue *)table)->fields, name);
}

usz ECSValue_GetTableFieldCount(const ECSValue *table)
{
    return ECSValue_GetType(table) == ECSValueType_Table ? shlenu(table->fields) : 0;
}

const ECSValue *ECSValue_GetTableFieldAt(const ECSValue *table, usz index, const char **retName)
{
    SDL_assert(retName != nullptr);

    if (index >= ECSValue_GetTableFieldCount(table))
    {
        *retName = nullptr;
        return nullptr;
    }

    *retName = table->fields[index].key;
    return table->fields[index].value;
}

void ECSValue_SetNil(ECSValue *value)
{
    SDL_assert(value != nullptr);

    ECSIValue_Clear(value);
}

void ECSValue_SetBool(ECSValue *value, bool boolean)
{
    SDL_assert(value != nullptr);

    ECSIValue_Clear(value);
    value->type = ECSValueType_Bool;
    value->boolean = boolean;
}

void ECSValue_SetInteger(ECSValue *value, i64 integer)
{
    SDL_assert(value != nullptr);

    ECSIValue_Clear(value);
    value->type = ECSValueType_Integer;
    value->integer = integer;
}

void ECSValue_SetNumber(ECSValue *value, f64 number)
{
    SDL_assert(value != nullptr);

    ECSIValue_Clear(value);
    value->type = ECSValueType_Number;
    value->number = number;
}

SHUResult ECSValue_SetString(ECSValue *value, const char *string)
{
    SDL_assert(value != nullptr);
    SDL_assert(string != nullptr);

    char *copy = SDL_strdup(string);

    if (copy == nullptr)
    {
        return SHUResult_ErrAllocation;
    }

    ECSIValue_Clear(value);
    value->type = ECSValueType_String;
    value->string = copy;
    return SHUResult_Ok;
}

SHUResult ECSValue_SetBuffer(ECSValue *value, SHUSliceView buffer)
{
    SDL_assert(value != nullptr);
    SDL_assert(buffer.data != nullptr || buffer.size == 0);

    // a copy of one byte at least, so an empty buffer has bytes to point to
    u8 *bytes = SDL_malloc(SDL_max(buffer.size, 1));

    if (bytes == nullptr)
    {
        return SHUResult_ErrAllocation;
    }

    if (buffer.size > 0)
    {
        SDL_memcpy(bytes, buffer.data, buffer.size);
    }

    ECSIValue_Clear(value);
    value->type = ECSValueType_Buffer;
    value->bytes = bytes;
    value->size = buffer.size;
    return SHUResult_Ok;
}

SHUResult ECSValue_SetHandle(ECSValue *value, const char *type, void *object)
{
    return ECSIValue_SetLuaHandle(value, type, object, 0);
}

SHUResult ECSIValue_SetLuaHandle(ECSValue *value, const char *type, void *object, int reference)
{
    SDL_assert(value != nullptr);
    SDL_assert(type != nullptr);
    SDL_assert(object != nullptr);

    char *copy = SDL_strdup(type);

    if (copy == nullptr)
    {
        if (reference != 0 && VALUES.Drop != nullptr)
        {
            VALUES.Drop(reference);
        }

        return SHUResult_ErrAllocation;
    }

    ECSIValue_Clear(value);
    value->type = ECSValueType_Handle;
    value->handleType = copy;
    value->object = object;
    value->reference = reference;
    return SHUResult_Ok;
}

void ECSValue_SetTable(ECSValue *value)
{
    SDL_assert(value != nullptr);

    ECSIValue_Clear(value);
    value->type = ECSValueType_Table;
}

SHUResult ECSValue_AddListItem(ECSValue *table, ECSValue **retItem)
{
    SDL_assert(table != nullptr);
    SDL_assert(retItem != nullptr);

    ECSIValue_MakeTable(table);
    SHU_ReturnResult(ECSValue_Create(retItem));
    arrput(table->items, *retItem);
    return SHUResult_Ok;
}

SHUResult ECSValue_SetTableField(ECSValue *table, const char *name, ECSValue **retField)
{
    SDL_assert(table != nullptr);
    SDL_assert(name != nullptr);
    SDL_assert(retField != nullptr);

    ECSIValue_MakeTable(table);

    // keys are copied; the mode is set before shget, which would allocate a map without it
    if (table->fields == nullptr)
    {
        sh_new_strdup(table->fields);
    }

    ECSValue *field = shget(table->fields, name);

    if (field != nullptr)
    {
        *retField = field;
        return SHUResult_Ok;
    }

    SHU_ReturnResult(ECSValue_Create(&field));
    shput(table->fields, name, field);
    *retField = field;
    return SHUResult_Ok;
}
