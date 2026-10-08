#pragma once

// Values: generic values, for saved state, settings and calls between C and Lua.

#include "OpenECS.h"

#pragma region Declarations

/// @brief Function called for each named field of a table, by ECSI_ValueForEachField.
typedef void (*ECSI_ValueFieldFunction)(const char *name, const ECSValue *field, void *userData);

/// @brief Creates a nil value.
/// @param retValue The new value. Destroy it with ECSI_ValueDestroy.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSI_ValueCreate(ECSValue **retValue);

/// @brief Destroys a value, with everything it holds, and sets the handle to NULL.
/// @param value Value to destroy, or a handle to NULL.
void ECSI_ValueDestroy(ECSValue **value);

/// @brief Makes a value a deep copy of another.
/// @param value The value to set.
/// @param source The value to copy, or NULL for nil. Must not be inside the value.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation; the value holds a partial copy then.
SHUWUR SHUResult ECSI_ValueCopy(ECSValue *value, const ECSValue *source);

/// @brief Calls a function for every named field of a table, in the order they were added.
/// @param table The table, or NULL.
/// @param function Function to call.
/// @param userData Passed to the function.
void ECSI_ValueForEachField(const ECSValue *table, ECSI_ValueFieldFunction function, void *userData);

#pragma endregion Declarations
