#pragma once

// Menus: the core's bindable functions, such as ecs.close and ecs.lock, and the panel and group menus that offer them.

#include "interface/Panels.h"

#include "SDL3/SDL_events.h"

#pragma region Declarations

/// @brief Registers the core's bindable functions.
/// @return SHUResult_Ok, SHUResult_ErrAllocation, or SHUResult_ErrBadData if a name of the core is taken.
SHUWUR SHUResult ECSIMenus_Initialize(void);

/// @brief Closes the menus and frees their entries.
void ECSIMenus_Terminate(void);

/// @brief Checks whether a core function can act on a panel now, so menus and the list of prefix keys offer it.
/// @param function Name of the function.
/// @param panel The focused panel, or NULL.
/// @return true if it can.
bool ECSIMenus_Offers(const char *function, ECSPanel panel);

/// @brief Gets the label of a function: what it does to a panel now, or its description.
/// @param function Name of the function.
/// @param panel The focused panel, or NULL.
/// @return The label, valid while the function is registered.
const char *ECSIMenus_LabelOf(const char *function, ECSPanel panel);

/// @brief Gets a function's position among the core's functions, so lists of them keep one order.
/// @param function Name of the function.
/// @return Its position, starting at 0, or the number of the core's functions if it is not one of them.
usz ECSIMenus_GetOrder(const char *function);

/// @brief Opens a panel's menu, or the menu of the panel's group, at a point. The panel gets the focus.
/// @param panel The panel, or the group's shown panel.
/// @param x Horizontal position in layout units.
/// @param y Vertical position in layout units.
/// @param group true for the group's menu.
void ECSIMenus_Open(ECSPanel panel, f32 x, f32 y, bool group);

/// @brief Gives an event to the open menus. Pointer and key events go to the menus only; keys go to the deepest one.
/// @param event The event.
/// @return true if the menus used the event.
bool ECSIMenus_Handle(const SDL_Event *event);

#pragma endregion Declarations
