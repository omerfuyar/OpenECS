#pragma once

// Keys: key combinations, the core prefix and the keys after it, and every key binding: the user's, the preset's and the plugins'.

#include "base/Values.h"

#pragma region Declarations

/// @brief A key combination and the function it runs.
typedef struct ECSIKeyBinding
{
    u32 key;
    u32 modifiers;
    char *text;     // the combination as written
    char *function; // name of the function
} ECSIKeyBinding;

/// @brief Reads a key combination written as text, such as "Ctrl+Shift+P". Key names are SDL's.
/// @param text The text.
/// @param report true to report a text that is not a key combination.
/// @param retKey The SDL key code.
/// @param retModifiers The ECSModifier bits.
/// @return SHUResult_Ok, SHUResult_ErrBadData if the text is not a key combination, or SHUResult_ErrAllocation.
SHUResult ECSIKeys_Parse(const char *text, bool report, u32 *retKey, u32 *retModifiers);

/// @brief Declares the settings ecs.prefix and ecs.prefixKeys, and reports the key texts of the user's files that are not key combinations.
/// @return SHUResult_Ok, SHUResult_ErrAllocation, or SHUResult_ErrBadData if a name of the core is taken.
SHUWUR SHUResult ECSIKeys_Initialize(void);

/// @brief Frees every binding.
void ECSIKeys_Terminate(void);

/// @brief Sets the preset's key bindings for the whole tool. Key texts that are not key combinations are reported.
/// @param keys A table of key texts and function names, or NULL. The core copies it.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSIKeys_SetTool(const ECSValue *keys);

/// @brief Adds the key bindings of the next workspace, in the order the layout adds workspaces.
/// @param keys A table of key texts and function names, or NULL for none. The core copies it.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSIKeys_AddWorkspace(const ECSValue *keys);

/// @brief Gets a workspace's key bindings, for saving the session.
/// @param index Position of the workspace, starting at 0.
/// @return The table, or NULL if the workspace has none.
const ECSValue *ECSIKeys_GetWorkspace(usz index);

/// @brief Reports the keys of the preset, the user's files and ecs.prefixKeys that run a function its owner does not have, though the owner runs (ECSIPlugins_OwnerRuns), so a misspelt name is noticed. Call it once the session is built.
void ECSIKeys_ReportUnknownFunctions(void);

/// @brief Removes the bindings a failed plugin made.
/// @param plugin The plugin.
void ECSIKeys_RemovePlugin(ECSPlugin plugin);

/// @brief Finds the function that a key press runs: the binding of the highest settings layer, and within it the most specific one.
/// @param key The SDL key code.
/// @param modifiers The ECSModifier bits.
/// @param focus The focused panel, or NULL.
/// @return The function's name, or NULL if no binding matches.
const char *ECSIKeys_Find(u32 key, u32 modifiers, ECSPanel focus);

/// @brief Checks whether a key press is the core prefix. The prefix is read when it is checked, so a changed prefix always wins. AltGr is never part of it.
/// @param key The SDL key code.
/// @param modifiers The ECSModifier bits.
/// @return true if it is the prefix.
bool ECSIKeys_IsPrefix(u32 key, u32 modifiers);

/// @brief Gets the keys after the prefix: the defaults, changed by the setting ecs.prefixKeys.
/// @return stb_ds array of the bindings, valid until the setting changes and the keys are read again.
const ECSIKeyBinding *ECSIKeys_GetPrefixKeys(void);

/// @brief Makes the text of the keys that run a function after the prefix, such as "Alt+W, X".
/// @param function Name of the function.
/// @return The text, or NULL if no key after the prefix runs the function. Free it with SDL_free.
char *ECSIKeys_PrefixTextOf(const char *function);

/// @brief Makes the text of the key that a plugin bound to a function for a panel type, such as "Ctrl+E".
/// @param panelType Name of the panel type.
/// @param function Name of the function.
/// @return The text, or NULL if no binding runs the function for the type. Free it with SDL_free.
char *ECSIKeys_BoundTextOf(const char *panelType, const char *function);

#pragma endregion Declarations
