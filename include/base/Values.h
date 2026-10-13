#pragma once

// Values: generic values, for saved state, settings and calls between C and Lua.

#include "OpenECS.h"

#pragma region Declarations

/// @brief Type that a field of a checked table must have.
typedef enum ECSIFieldType
{
    ECSIFieldType_Any = 0,
    ECSIFieldType_Bool,
    ECSIFieldType_Integer, // a number with no fraction
    ECSIFieldType_Number,
    ECSIFieldType_String,
    ECSIFieldType_Table,
} ECSIFieldType;

/// @brief What a named field of a checked table must be.
typedef struct ECSIFieldRule
{
    const char *name;
    ECSIFieldType type;
    bool required;
} ECSIFieldRule;

/// @brief Function called for each named field of a table, by ECSIValue_TableForEachField.
typedef void (*ECSIValueFieldFunction)(const char *name, const ECSValue *field, void *userData);

/// @brief Makes a value a deep copy of another.
/// @param value The value to set.
/// @param source The value to copy, or NULL for nil. Must not be inside the value.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation; the value holds a partial copy then.
SHUWUR SHUResult ECSIValue_Copy(ECSValue *value, const ECSValue *source);

/// @brief Checks whether two values are the same: the same type and contents, with fields in any order.
/// @param first A value, or NULL for nil.
/// @param second A value, or NULL for nil.
/// @return true if they are the same.
bool ECSIValue_Equal(const ECSValue *first, const ECSValue *second);

/// @brief Calls a function for every named field of a table, in the order they were added.
/// @param table The table, or NULL.
/// @param function Function to call.
/// @param userData Passed to the function.
void ECSIValue_TableForEachField(const ECSValue *table, ECSIValueFieldFunction function, void *userData);

/// @brief Checks the named fields of a table against rules: each field a rule names has the rule's type, a required one is there, and no other named field is. List items are not checked.
/// @param table The table, or NULL, which is not a table.
/// @param rules The rules.
/// @param count Number of rules.
/// @param retProblem Gets the first problem, such as "'name' must be a text", to free with SDL_free; NULL when there is none.
/// @return true if the value is a table that follows the rules.
bool ECSIValue_CheckFields(const ECSValue *table, const ECSIFieldRule *rules, usz count, char **retProblem);

/// @brief Checks a version text: one to three numbers separated by dots, such as "1.2.0" or "0.1".
/// @param text The text, or NULL.
/// @return true if it is a version.
bool ECSIValue_IsVersion(const char *text);

#pragma endregion Declarations
