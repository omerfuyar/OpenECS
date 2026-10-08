#pragma once

// Values: generic values, for saved state, settings and calls between C and Lua.

#include "OpenECS.h"

#pragma region Declarations

/// @brief Function called for each named field of a table, by ECSI_ValueTableForEachField.
typedef void (*ECSI_ValueFieldFunction)(const char *name, const ECSValue *field, void *userData);

/// @brief Makes a value a deep copy of another.
/// @param value The value to set.
/// @param source The value to copy, or NULL for nil. Must not be inside the value.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation; the value holds a partial copy then.
SHUWUR SHUResult ECSI_ValueCopy(ECSValue *value, const ECSValue *source);

/// @brief Checks whether two values are the same: the same type and contents, with fields in any order.
/// @param first A value, or NULL for nil.
/// @param second A value, or NULL for nil.
/// @return true if they are the same.
bool ECSI_ValueEqual(const ECSValue *first, const ECSValue *second);

/// @brief Calls a function for every named field of a table, in the order they were added.
/// @param table The table, or NULL.
/// @param function Function to call.
/// @param userData Passed to the function.
void ECSI_ValueTableForEachField(const ECSValue *table, ECSI_ValueFieldFunction function, void *userData);

#pragma endregion Declarations
