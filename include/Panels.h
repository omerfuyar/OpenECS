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
    void *typeData; // what the Bindings module keeps for a Lua panel type, or NULL
} ECSI_PanelType;

/// @brief A panel: one instance of a panel type, placed in the layout.
struct ECSI_Panel
{
    u32 id;
    ECSI_PanelType *type; // NULL for a placeholder whose type is missing
    char *typeName;
    char *title;
    void *state;
    ECSValue *savedState; // the saved state the panel was created with; a placeholder keeps it to save it again
    u32 stateVersion;     // version of savedState
    bool needsDraw;
    bool unsaved; // the panel has unsaved work
    f32 x; // rectangle in layout units, set by the layout
    f32 y;
    f32 width;
    f32 height;
    SDL_Surface *pixels;  // what the panel drew, kept between draws
    SDL_Texture *texture; // the pixels on the GPU, made by the renderer of the OS window that shows the panel
    u64 lastDrawTicks;
    bool closed; // out of the layout, waiting for ECSI_PanelsDestroyClosed; it gets no more events
    char *fault; // the error of a callback, or NULL; a faulted panel shows it and its type is not called again, except Destroy
};

/// @brief Registers a panel type like ECSPanelType_Register, with data for the Bindings module.
/// @param plugin The plugin that provides the panel type.
/// @param desc Description of the panel type.
/// @param typeData Kept in the type, or NULL.
/// @return SHUResult_Ok, SHUResult_ErrBadData if the description is invalid, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSI_PanelTypeRegister(ECSPlugin plugin, const ECSPanelTypeDesc *desc, void *typeData);

/// @brief Asks the user about the unsaved work of panels that are about to close: Save, Discard or Cancel. One dialog lists them all.
/// @param panels The panels that close. Those without unsaved work are skipped.
/// @param count Number of panels.
/// @return true if the panels may close: none had unsaved work, the user discarded it, or every save worked.
bool ECSI_PanelsConfirmClose(const ECSPanel *panels, usz count);

/// @brief Removes every panel type a plugin registered. Call it before panels of those types exist.
/// @param plugin The plugin.
void ECSI_PanelsRemovePlugin(ECSPlugin plugin);

/// @brief Makes a panel faulted: it shows the error instead of its pixels, and its type is not called again, except Destroy. A panel keeps its first fault.
/// @param panel The panel.
/// @param message The error. The core copies it.
void ECSI_PanelFault(ECSPanel panel, const char *message);

/// @brief Destroys the closed panels, then frees every registered panel type. Call it after every other panel is destroyed.
void ECSI_PanelsTerminate(void);

/// @brief Creates a panel. If its type is missing or fails to create the panel, the panel becomes a placeholder.
/// @param retPanel The new panel.
/// @param typeName Name of the panel's type.
/// @param savedState Saved state to create the panel from, or NULL for a new panel. The panel keeps a copy.
/// @param stateVersion Version of the saved state.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSI_PanelCreate(ECSPanel *retPanel, const char *typeName, const ECSValue *savedState, u32 stateVersion);

/// @brief Destroys a panel at once and sets the handle to NULL. Use ECSI_PanelClose for a panel that may still have queued events.
/// @param panel Panel to destroy.
void ECSI_PanelDestroy(ECSPanel *panel);

/// @brief Closes a panel that left the layout, and sets the handle to NULL. Its timers stop and it gets no more events; ECSI_PanelsDestroyClosed destroys it.
/// @param panel Panel to close.
void ECSI_PanelClose(ECSPanel *panel);

/// @brief Destroys the closed panels. Call it after the queued events are delivered, so no handler meets a destroyed panel.
void ECSI_PanelsDestroyClosed(void);

/// @brief Gives a panel the id it had in a saved session. Later panels get higher ids.
/// @param panel The panel.
/// @param id The saved id. 0 keeps the panel's new id.
void ECSI_PanelSetId(ECSPanel panel, u32 id);

/// @brief Describes a panel for a session: its id, type, saved state and the state's version. A working panel saves its state through its type; a placeholder writes back the state it kept.
/// @param panel The panel.
/// @param retPanel The value to set to a table.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation. A type that fails to save its state is reported, and the last saved state is written.
SHUWUR SHUResult ECSI_PanelSave(ECSPanel panel, ECSValue *retPanel);

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
