#pragma once

// Menus: the core's bindable functions, such as ecs.close and ecs.lock, and the panel and group menus that offer them.

#include "interface/Panels.h"

#include "SDL3/SDL_events.h"

#pragma region Declarations

/// @brief Registers the core's bindable functions.
/// @return SHUResult_Ok, SHUResult_ErrAllocation, or SHUResult_ErrBadData if a name of the core is taken.
SHUWUR SHUResult ECSI_MenusInitialize(void);

/// @brief Closes the menus and frees their entries.
void ECSI_MenusTerminate(void);

/// @brief Checks whether a core function can act on a panel now, so menus and the list of prefix keys offer it.
/// @param function Name of the function.
/// @param panel The focused panel, or NULL.
/// @return true if it can.
bool ECSI_MenusOffers(const char *function, ECSPanel panel);

/// @brief Gets the label of a function: what it does to a panel now, or its description.
/// @param function Name of the function.
/// @param panel The focused panel, or NULL.
/// @return The label, valid while the function is registered.
const char *ECSI_MenusLabelOf(const char *function, ECSPanel panel);

/// @brief Opens a panel's menu, or the menu of the panel's group, at a point. The panel gets the focus.
/// @param panel The panel, or the group's shown panel.
/// @param x Horizontal position in layout units.
/// @param y Vertical position in layout units.
/// @param group true for the group's menu.
void ECSI_MenusOpen(ECSPanel panel, f32 x, f32 y, bool group);

/// @brief Gives an event to the open menus. Pointer and key events go to the menus only; keys go to the deepest one.
/// @param event The event.
/// @return true if the menus used the event.
bool ECSI_MenusHandle(const SDL_Event *event);

#pragma endregion Declarations
