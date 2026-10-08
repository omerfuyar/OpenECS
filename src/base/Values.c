#include "base/Values.h"

#include "SDL3/SDL.h"
#include "stb/stbSDL3.h"

#pragma region Source Only

struct ECSI_Value
{
    ECSValueType type;

    union
    {
        bool boolean;
        i64 integer;
        f64 number;
        char *string;
    };

    // table
    ECSValue **items; // stb_ds array; callers keep pointers to items, so each is allocated on its own
    struct
    {
        char *key;
        ECSValue *value;
    } *fields; // stb_ds hash map with copied keys, in the order fields were added
};

/// @brief Frees what a value holds and makes it nil.
static void ECSI_ValueClear(ECSValue *value)
{
    if (value->type == ECSValueType_String)
    {
        SDL_free(value->string);
    }

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
    SDL_zerop(value);
}

static void ECSI_ValueMakeTable(ECSValue *value)
{
    if (value->type != ECSValueType_Table)
    {
        ECSI_ValueClear(value);
        value->type = ECSValueType_Table;
    }
}

#pragma endregion Source Only

SHUResult ECSValue_Create(ECSValue **retValue)
{
    SDL_assert(retValue != NULL);

    *retValue = SDL_calloc(1, sizeof(ECSValue));
    return *retValue == NULL ? SHUResult_ErrAllocation : SHUResult_Ok;
}

void ECSValue_Destroy(ECSValue **value)
{
    SDL_assert(value != NULL);

    if (*value != NULL)
    {
        ECSI_ValueClear(*value);
        SDL_free(*value);
        *value = NULL;
    }
}

SHUResult ECSI_ValueCopy(ECSValue *value, const ECSValue *source)
{
    SDL_assert(value != NULL);
    SDL_assert(value != source);

    switch (ECSValue_GetType(source))
    {
    case ECSValueType_Nil:
        ECSValue_SetNil(value);
        return SHUResult_Ok;
    case ECSValueType_Bool:
        ECSValue_SetBool(value, source->boolean);
        return SHUResult_Ok;
    case ECSValueType_Integer:
        ECSValue_SetInteger(value, source->integer);
        return SHUResult_Ok;
    case ECSValueType_Number:
        ECSValue_SetNumber(value, source->number);
        return SHUResult_Ok;
    case ECSValueType_String:
        return ECSValue_SetString(value, source->string);
    case ECSValueType_Table:
        break;
    }

    ECSValue_SetTable(value);

    for (usz i = 0; i < arrlenu(source->items); i++)
    {
        ECSValue *item = NULL;
        SHU_ReturnResult(ECSValue_AddItem(value, &item));
        SHU_ReturnResult(ECSI_ValueCopy(item, source->items[i]));
    }

    for (usz i = 0; i < shlenu(source->fields); i++)
    {
        ECSValue *field = NULL;
        SHU_ReturnResult(ECSValue_SetField(value, source->fields[i].key, &field));
        SHU_ReturnResult(ECSI_ValueCopy(field, source->fields[i].value));
    }

    return SHUResult_Ok;
}

bool ECSI_ValueEqual(const ECSValue *first, const ECSValue *second)
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
    case ECSValueType_Table:
        break;
    }

    if (arrlenu(first->items) != arrlenu(second->items) || shlenu(first->fields) != shlenu(second->fields))
    {
        return false;
    }

    for (usz i = 0; i < arrlenu(first->items); i++)
    {
        if (!ECSI_ValueEqual(first->items[i], second->items[i]))
        {
            return false;
        }
    }

    for (usz i = 0; i < shlenu(first->fields); i++)
    {
        if (!ECSI_ValueEqual(first->fields[i].value, ECSValue_GetField(second, first->fields[i].key)))
        {
            return false;
        }
    }

    return true;
}

void ECSI_ValueForEachField(const ECSValue *table, ECSI_ValueFieldFunction function, void *userData)
{
    SDL_assert(function != NULL);

    if (ECSValue_GetType(table) != ECSValueType_Table)
    {
        return;
    }

    for (usz i = 0; i < shlenu(table->fields); i++)
    {
        function(table->fields[i].key, table->fields[i].value, userData);
    }
}

ECSValueType ECSValue_GetType(const ECSValue *value)
{
    return value == NULL ? ECSValueType_Nil : value->type;
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
        // the range check keeps the conversion defined
        if (value->number >= -0x1p63 && value->number < 0x1p63 && value->number == SDL_floor(value->number))
        {
            return (i64)value->number;
        }

        return fallback;
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

usz ECSValue_GetCount(const ECSValue *table)
{
    return ECSValue_GetType(table) == ECSValueType_Table ? arrlenu(table->items) : 0;
}

const ECSValue *ECSValue_GetItem(const ECSValue *table, usz index)
{
    return index < ECSValue_GetCount(table) ? table->items[index] : NULL;
}

const ECSValue *ECSValue_GetField(const ECSValue *table, const char *name)
{
    SDL_assert(name != NULL);

    // shget would allocate a map that is missing
    if (ECSValue_GetType(table) != ECSValueType_Table || table->fields == NULL)
    {
        return NULL;
    }

    // the cast is safe: shget only reads a map that exists, but its macro takes a modifiable one
    return shget(((ECSValue *)table)->fields, name);
}

void ECSValue_SetNil(ECSValue *value)
{
    SDL_assert(value != NULL);

    ECSI_ValueClear(value);
}

void ECSValue_SetBool(ECSValue *value, bool boolean)
{
    SDL_assert(value != NULL);

    ECSI_ValueClear(value);
    value->type = ECSValueType_Bool;
    value->boolean = boolean;
}

void ECSValue_SetInteger(ECSValue *value, i64 integer)
{
    SDL_assert(value != NULL);

    ECSI_ValueClear(value);
    value->type = ECSValueType_Integer;
    value->integer = integer;
}

void ECSValue_SetNumber(ECSValue *value, f64 number)
{
    SDL_assert(value != NULL);

    ECSI_ValueClear(value);
    value->type = ECSValueType_Number;
    value->number = number;
}

SHUResult ECSValue_SetString(ECSValue *value, const char *string)
{
    SDL_assert(value != NULL);
    SDL_assert(string != NULL);

    char *copy = SDL_strdup(string);

    if (copy == NULL)
    {
        return SHUResult_ErrAllocation;
    }

    ECSI_ValueClear(value);
    value->type = ECSValueType_String;
    value->string = copy;
    return SHUResult_Ok;
}

void ECSValue_SetTable(ECSValue *value)
{
    SDL_assert(value != NULL);

    ECSI_ValueClear(value);
    value->type = ECSValueType_Table;
}

SHUResult ECSValue_AddItem(ECSValue *table, ECSValue **retItem)
{
    SDL_assert(table != NULL);
    SDL_assert(retItem != NULL);

    ECSI_ValueMakeTable(table);
    SHU_ReturnResult(ECSValue_Create(retItem));
    arrput(table->items, *retItem);
    return SHUResult_Ok;
}

SHUResult ECSValue_SetField(ECSValue *table, const char *name, ECSValue **retField)
{
    SDL_assert(table != NULL);
    SDL_assert(name != NULL);
    SDL_assert(retField != NULL);

    ECSI_ValueMakeTable(table);

    // keys are copied; the mode is set before shget, which would allocate a map without it
    if (table->fields == NULL)
    {
        sh_new_strdup(table->fields);
    }

    ECSValue *field = shget(table->fields, name);

    if (field != NULL)
    {
        *retField = field;
        return SHUResult_Ok;
    }

    SHU_ReturnResult(ECSValue_Create(&field));
    shput(table->fields, name, field);
    *retField = field;
    return SHUResult_Ok;
}
