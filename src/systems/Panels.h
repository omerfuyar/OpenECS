#pragma once

// Panels: panel types, panels, and the pixels each panel draws.

#include "Global.h"
#include "tools/Renderer.h"

#pragma region Declarations

/// @brief A loaded plugin. Defined by the plugins module.
typedef struct ECSI_Plugin ECSI_Plugin;

/// @brief A registered panel type.
typedef struct ECSI_PanelType
{
    ECSPanelTypeDesc desc; // its name and title point to the buffers below
    char name[OPENECS_NAME_CAPACITY];
    char title[OPENECS_NAME_CAPACITY];
    ECSI_Plugin *plugin;
} ECSI_PanelType;

/// @brief A panel: one instance of a panel type, placed in the layout.
struct ECSI_Panel
{
    u32 id;
    ECSI_PanelType *type; // NULL for a placeholder whose type is missing
    char typeName[OPENECS_NAME_CAPACITY];
    char title[OPENECS_NAME_CAPACITY];
    void *state;
    bool needsDraw;
    f32 x; // rectangle in layout units, set by the layout
    f32 y;
    f32 width;
    f32 height;
    ECSSurface surface; // pixels, kept between draws
    ECSI_RenderTexture *texture;
    ECSI_RenderWindow *textureWindow; // window the texture belongs to
    u64 lastDrawTicks;
};

/// @brief Registers a panel type for a plugin.
/// @param plugin Plugin that provides the type.
/// @param pluginName Name of the plugin. The type's name must start with it and a dot.
/// @param desc Description of the type. Copied.
/// @return SHUResult_Ok, SHUResult_ErrBadData if the description is invalid, or SHUResult_ErrOverflow if there is no room.
SHUWUR SHUResult ECSI_PanelTypeRegister(ECSI_Plugin *plugin, const char *pluginName, const ECSPanelTypeDesc *desc);

/// @brief Finds a registered panel type.
/// @param name Name of the type.
/// @return The type, or NULL if no type has that name.
ECSI_PanelType *ECSI_PanelTypeFind(const char *name);

/// @brief Creates a panel. If its type is missing or fails to create the panel, the panel becomes a placeholder.
/// @param retPanel The new panel.
/// @param typeName Name of the panel's type.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSI_PanelCreate(ECSPanel *retPanel, const char *typeName);

/// @brief Destroys a panel and sets the handle to NULL.
/// @param panel Panel to destroy.
void ECSI_PanelDestroy(ECSPanel *panel);

/// @brief Places a panel. The panel is drawn again if its size changes.
/// @param panel Panel to place.
/// @param x Left edge in layout units.
/// @param y Top edge in layout units.
/// @param width Width in layout units.
/// @param height Height in layout units.
void ECSI_PanelSetRect(ECSPanel panel, f32 x, f32 y, f32 width, f32 height);

/// @brief Checks whether a visible panel needs a new frame.
/// @param panel Panel to check.
/// @return true if it draws continuously or asked to be drawn.
bool ECSI_PanelWantsFrame(ECSPanel panel);

/// @brief Draws a panel's pixels if needed, and puts them on screen at the panel's rectangle.
/// @param panel Panel to draw.
/// @param renderWindow Window to draw into.
/// @param nowTicks Current time in nanoseconds.
void ECSI_PanelRender(ECSPanel panel, ECSI_RenderWindow *renderWindow, u64 nowTicks);

/// @brief Sends an event to a panel's type.
/// @param panel Panel that receives the event.
/// @param event The event.
void ECSI_PanelSendEvent(ECSPanel panel, const ECSEvent *event);

/// @brief Asks for a panel to be drawn again.
/// @param panel Panel to draw.
void ECSI_PanelRedraw(ECSPanel panel);

/// @brief Gets a panel's title.
/// @param panel Panel to read.
/// @return The title.
const char *ECSI_PanelGetTitle(ECSPanel panel);

/// @brief Sets a panel's title.
/// @param panel Panel to change.
/// @param title New title. Copied.
void ECSI_PanelSetTitle(ECSPanel panel, const char *title);

#pragma endregion Declarations
