#pragma once

// Panels: panel types, panels, and the pixels each panel draws.

#include "OpenECS.h"

#include "SDL3/SDL_render.h"

#pragma region Declarations

/// @brief A registered panel type.
typedef struct ECSI_PanelType
{
    ECSPanelTypeDesc desc; // its name and title point to the copies below
    char *name;
    char *title;
    ECSPlugin plugin;
} ECSI_PanelType;

/// @brief A panel: one instance of a panel type, placed in the layout.
struct ECSI_Panel
{
    u32 id;
    ECSI_PanelType *type; // NULL for a placeholder whose type is missing
    char *typeName;
    char *title;
    void *state;
    bool needsDraw;
    f32 x; // rectangle in layout units, set by the layout
    f32 y;
    f32 width;
    f32 height;
    SDL_Surface *pixels;  // what the panel drew, kept between draws
    SDL_Texture *texture; // the pixels on the GPU, made by the renderer of the OS window that shows the panel
    u64 lastDrawTicks;
    bool closed; // out of the layout, waiting for ECSI_PanelsDestroyClosed; it gets no more events
};

/// @brief Destroys the closed panels, then frees every registered panel type. Call it after every other panel is destroyed.
void ECSI_PanelsTerminate(void);

/// @brief Creates a panel. If its type is missing or fails to create the panel, the panel becomes a placeholder.
/// @param retPanel The new panel.
/// @param typeName Name of the panel's type.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSI_PanelCreate(ECSPanel *retPanel, const char *typeName);

/// @brief Destroys a panel at once and sets the handle to NULL. Use ECSI_PanelClose for a panel that may still have queued events.
/// @param panel Panel to destroy.
void ECSI_PanelDestroy(ECSPanel *panel);

/// @brief Closes a panel that left the layout, and sets the handle to NULL. Its timers stop and it gets no more events; ECSI_PanelsDestroyClosed destroys it.
/// @param panel Panel to close.
void ECSI_PanelClose(ECSPanel *panel);

/// @brief Destroys the closed panels. Call it after the queued events are delivered, so no handler meets a destroyed panel.
void ECSI_PanelsDestroyClosed(void);

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
/// @param renderer Renderer of the OS window that shows the panel.
/// @param nowTicks Current time in nanoseconds.
void ECSI_PanelRender(ECSPanel panel, SDL_Renderer *renderer, u64 nowTicks);

/// @brief Queues an event for a panel's type.
/// @param panel Panel that receives the event.
/// @param event The event.
void ECSI_PanelPostEvent(ECSPanel panel, const ECSEvent *event);

#pragma endregion Declarations
