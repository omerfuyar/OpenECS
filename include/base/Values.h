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
/// @param retCopy The value to set to the copy.
/// @param source The value to copy, or nullptr for nil. Must not be inside retCopy.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation; retCopy holds a partial copy then.
SHUWUR SHUResult ECSIValue_Copy(ECSValue *retCopy, const ECSValue *source);

/// @brief Checks whether two values are the same: the same type and contents, with fields in any order.
/// @param first A value, or nullptr for nil.
/// @param second A value, or nullptr for nil.
/// @return true if they are the same.
bool ECSIValue_Equal(const ECSValue *first, const ECSValue *second);

/// @brief Calls a function for every named field of a table, in the order they were added.
/// @param table The table, or nullptr.
/// @param function Function to call.
/// @param userData Passed to the function.
void ECSIValue_TableForEachField(const ECSValue *table, ECSIValueFieldFunction function, void *userData);

/// @brief Checks the named fields of a table against rules: each field a rule names has the rule's type, a required one is there, and no other named field is. List items are not checked.
/// @param table The table, or nullptr, which is not a table.
/// @param rules The rules.
/// @param count Number of rules.
/// @param retProblem Gets the first problem, such as "'name' must be a text", to free with SDL_free; nullptr when there is none.
/// @return true if the value is a table that follows the rules.
bool ECSIValue_CheckFields(const ECSValue *table, const ECSIFieldRule *rules, usz count, char **retProblem);

/// @brief Gets the name of a handle's type.
/// @param value The value, or nullptr.
/// @return The type's full name, or nullptr if the value is not a handle. Valid as long as the value does not change.
const char *ECSIValue_GetHandleType(const ECSValue *value);

/// @brief Takes another reference to a Lua handle that a value keeps: a registry reference. The Lua module gives it.
typedef int (*ECSIValueKeepFunction)(int reference);

/// @brief Lets go of a Lua handle that a value kept.
typedef void (*ECSIValueDropFunction)(int reference);

/// @brief Gives the object of a Lua handle that a value keeps, or nullptr once the object is gone.
typedef void *(*ECSIValueObjectFunction)(int reference);

/// @brief Sets the functions for the Lua handles that values keep; the Lua module gives them as it starts, and nullptr functions as it ends.
void ECSIValue_SetLuaFunctions(ECSIValueKeepFunction Keep, ECSIValueDropFunction Drop, ECSIValueObjectFunction Object);

/// @brief Makes a value a handle read from Lua, which keeps the Lua handle itself, so it gives Lua that same handle back, and gives C the object only while it lives.
/// @param reference A registry reference to the Lua handle; the value owns it.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation; the reference is dropped then.
SHUWUR SHUResult ECSIValue_SetLuaHandle(ECSValue *value, const char *type, void *object, int reference);

/// @brief Gives the registry reference of the Lua handle that a value keeps, or 0 for a handle that C set, and for any other value.
int ECSIValue_GetLuaHandle(const ECSValue *value);

/// @brief Checks whether a value is a whole number: an integer, or a number without a fraction that fits in 64 bits.
/// @param value The value, or nullptr.
/// @return true if it is a whole number.
bool ECSIValue_IsInteger(const ECSValue *value);

/// @brief Checks a version text: one to three numbers separated by dots, such as "1.2.0" or "0.1".
/// @param text The text, or nullptr.
/// @return true if it is a version.
bool ECSIValue_IsVersion(const char *text);

#pragma endregion Declarations
