#pragma once

// Input: focus, pointer routing, the core prefix, key bindings and the clipboard.

#include "base/Values.h"

#include "SDL3/SDL_events.h"

#pragma region Declarations

/// @brief Declares the input settings, ecs.prefix, ecs.prefix_keys and ecs.focus, registers the core's bindable functions, and prepares input handling.
/// @return SHUResult_Ok, SHUResult_ErrAllocation, or SHUResult_ErrBadData if a name of the core is taken.
SHUWUR SHUResult ECSI_InputInitialize(void);

/// @brief Reads a key combination written as text, such as "Ctrl+Shift+P". Key names are SDL's.
/// @param text The text.
/// @param report true to report a text that is not a key combination.
/// @param retKey The SDL key code.
/// @param retModifiers The ECSModifier bits.
/// @return SHUResult_Ok, SHUResult_ErrBadData if the text is not a key combination, or SHUResult_ErrAllocation.
SHUResult ECSI_InputParseKey(const char *text, bool report, u32 *retKey, u32 *retModifiers);

/// @brief Sets the preset's key bindings for the whole tool. Key texts that are not key combinations are reported.
/// @param keys A table of key texts and function names, or NULL. The core copies it.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSI_InputSetToolKeys(const ECSValue *keys);

/// @brief Adds the key bindings of the next workspace, in the order the layout adds workspaces.
/// @param keys A table of key texts and function names, or NULL for none. The core copies it.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSI_InputAddWorkspaceKeys(const ECSValue *keys);

/// @brief Gets a workspace's key bindings, for saving the session.
/// @param index Position of the workspace, starting at 0.
/// @return The table, or NULL.
const ECSValue *ECSI_InputGetWorkspaceKeys(usz index);

/// @brief Removes every key binding a plugin made.
/// @param plugin The plugin.
void ECSI_InputRemovePlugin(ECSPlugin plugin);

/// @brief Frees what input handling keeps.
void ECSI_InputTerminate(void);

/// @brief Handles one SDL event.
/// @param event The event.
/// @return false if the program should quit.
bool ECSI_InputHandle(const SDL_Event *event);

#pragma endregion Declarations
