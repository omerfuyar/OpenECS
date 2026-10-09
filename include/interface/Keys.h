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

/// @brief Declares the setting ecs.prefix, and reports what the keys tables of the core's and the user's settings files hold that never works.
/// @return SHUResult_Ok, SHUResult_ErrAllocation, or SHUResult_ErrBadData if a name of the core is taken.
SHUWUR SHUResult ECSIKeys_Initialize(void);

/// @brief Frees every binding.
void ECSIKeys_Terminate(void);

/// @brief Sets the preset's keys table (DESIGN 7.8). What it holds that never works is reported.
/// @param keys The keys table, or NULL. The core copies it.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSIKeys_SetTool(const ECSValue *keys);

/// @brief Adds the keys table of the next workspace, in the order the layout adds workspaces.
/// @param keys The keys table, or NULL for none. The core copies it.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSIKeys_AddWorkspace(const ECSValue *keys);

/// @brief Gets a workspace's keys table, for saving the session.
/// @param index Position of the workspace, starting at 0.
/// @return The table, or NULL if the workspace has none.
const ECSValue *ECSIKeys_GetWorkspace(usz index);

/// @brief Reports the keys of every keys table that run a function its owner does not have, though the owner runs (ECSIPlugins_OwnerRuns), so a misspelt name is noticed. Call it once the session is built.
void ECSIKeys_ReportUnknownFunctions(void);

/// @brief Removes the bindings a failed plugin made.
/// @param plugin The plugin.
void ECSIKeys_RemovePlugin(ECSPlugin plugin);

/// @brief Finds the function that a key press runs: the binding of the highest layer, and within it the most specific one (DESIGN 7.8).
/// @param key The SDL key code.
/// @param modifiers The ECSModifier bits.
/// @param focus The focused panel, or NULL.
/// @return The function's name, or NULL if no binding matches or the winning binding removes the key.
const char *ECSIKeys_Find(u32 key, u32 modifiers, ECSPanel focus);

/// @brief Checks whether a key press is the core prefix. The prefix is read when it is checked, so a changed prefix always wins. AltGr is never part of it.
/// @param key The SDL key code.
/// @param modifiers The ECSModifier bits.
/// @return true if it is the prefix.
bool ECSIKeys_IsPrefix(u32 key, u32 modifiers);

/// @brief Gets the keys after the prefix: the prefix tables of every layer, merged, with the current workspace's.
/// @return stb_ds array of the bindings, valid until the current workspace changes.
const ECSIKeyBinding *ECSIKeys_GetPrefixKeys(void);

/// @brief Makes the text of the keys that run a function after the prefix, such as "Alt+W, X".
/// @param function Name of the function.
/// @return The text, or NULL if no key after the prefix runs the function. Free it with SDL_free.
char *ECSIKeys_PrefixTextOf(const char *function);

/// @brief Makes the text of a key that runs a function while a panel of a type has focus, such as "Ctrl+E".
/// @param panelType Name of the panel type.
/// @param function Name of the function.
/// @return The text, or NULL if no binding runs the function for the type. Free it with SDL_free.
char *ECSIKeys_BoundTextOf(const char *panelType, const char *function);

#pragma endregion Declarations
