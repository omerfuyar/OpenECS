#pragma once

// Panels: panel types, panels, and the pixels each panel draws.

#include "OpenECS.h"

#include "SDL3/SDL_render.h"

#pragma region Declarations

/// @brief A registered panel type.
typedef struct ECSIPanelType
{
    ECSPanelTypeDesc desc; // its name and title point to the copies below
    char *name;
    char *title;
    ECSPlugin plugin;
    void *typeData;     // what the Bindings module keeps for a Lua panel type, or NULL
    char **menuEntries; // stb_ds array of the functions the type adds to its panels' menu
} ECSIPanelType;

/// @brief A panel: one instance of a panel type, placed in the layout.
struct ECSIPanel
{
    u32 id;
    ECSIPanelType *type; // NULL for a placeholder whose type is missing
    char *typeName;
    char *title;
    void *state;
    ECSValue *savedState; // the saved state the panel was created with; a placeholder keeps it to save it again
    u32 stateVersion;     // version of savedState
    bool needsDraw;
    bool unsaved; // the panel has unsaved work
    f32 x;        // rectangle in layout units, set by the layout
    f32 y;
    f32 width;
    f32 height;
    SDL_Surface *pixels;  // what the panel drew, kept between draws
    SDL_Texture *texture; // the pixels on the GPU, made by the renderer of the OS window that shows the panel
    u64 lastDrawTicks;
    u64 focusTicks; // when the panel last got focus, for placing new panels of its type; 0 if never
    bool closed;    // out of the layout, waiting for ECSIPanels_DestroyClosed; it gets no more events
    bool visible;   // what the panel was last told: shown or hidden
    f32 toldWidth;  // the size the panel was last told
    f32 toldHeight;
    char *fault; // the error of a callback, or NULL; a faulted panel shows it and its type is not called again, except Destroy
};

/// @brief Registers a panel type like ECSPanelType_Register, with data for the Bindings module.
/// @param plugin The plugin that provides the panel type.
/// @param desc Description of the panel type.
/// @param typeData Kept in the type, or NULL.
/// @return SHUResult_Ok, SHUResult_ErrBadData if the description is invalid, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSIPanel_TypeRegister(ECSPlugin plugin, const ECSPanelTypeDesc *desc, void *typeData);

/// @brief Asks the user about the unsaved work of panels that are about to close: Save, Discard or Cancel. One dialog lists them all.
/// @param panels The panels that close. Those without unsaved work are skipped.
/// @param count Number of panels.
/// @param quitting true when the program quits. If the dialog cannot be shown, quitting discards the work, so it always finishes; closing panels is cancelled.
/// @return true if the panels may close: none had unsaved work, the user discarded it, or every save worked.
bool ECSIPanels_ConfirmClose(const ECSPanel *panels, usz count, bool quitting);

/// @brief Recreates a faulted panel, or a placeholder whose type is registered now, from its last saved state. The panel keeps its place and id.
/// @param panel The panel.
/// @return true if the panel runs again; false if it was not faulted, its type is still missing, or creating it failed again.
bool ECSIPanel_Restart(ECSPanel panel);

/// @brief Checks whether a panel can be restarted: it is faulted, or a placeholder.
bool ECSIPanel_CanRestart(ECSPanel panel);

/// @brief Gets the functions that a panel's type adds to its menu.
/// @param panel The panel.
/// @param retCount Number of functions.
/// @return The function names, or NULL for none. Valid while the type is registered.
const char *const *ECSIPanel_GetMenuEntries(ECSPanel panel, usz *retCount);

/// @brief Removes every panel type a plugin registered. Call it before panels of those types exist.
/// @param plugin The plugin.
void ECSIPanels_RemovePlugin(ECSPlugin plugin);

/// @brief Gets the state version of a panel type, for a panel that code opens with a saved state.
/// @param typeName Name of the panel type.
/// @return The version, or 0 if the type is missing.
u32 ECSIPanels_GetStateVersion(const char *typeName);

/// @brief Makes a panel faulted: it shows the error instead of its pixels, and its type is not called again, except Destroy. A panel keeps its first fault.
/// @param panel The panel.
/// @param message The error. The core copies it.
void ECSIPanel_Fault(ECSPanel panel, const char *message);

/// @brief Destroys the closed panels, then frees every registered panel type. Call it after every other panel is destroyed.
void ECSIPanels_Terminate(void);

/// @brief Creates a panel. If its type is missing or fails to create the panel, the panel becomes a placeholder.
/// @param retPanel The new panel.
/// @param typeName Name of the panel's type.
/// @param savedState Saved state to create the panel from, or NULL for a new panel. The panel keeps a copy.
/// @param stateVersion Version of the saved state.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSIPanel_Create(ECSPanel *retPanel, const char *typeName, const ECSValue *savedState, u32 stateVersion);

/// @brief Destroys a panel at once and sets the handle to NULL. Use ECSIPanel_Close for a panel that may still have queued events.
/// @param panel Panel to destroy.
void ECSIPanel_Destroy(ECSPanel *panel);

/// @brief Closes a panel that left the layout, and sets the handle to NULL. Its timers stop and it gets no more events; ECSIPanels_DestroyClosed destroys it.
/// @param panel Panel to close.
void ECSIPanel_Close(ECSPanel *panel);

/// @brief Destroys the closed panels. Call it after the queued events are delivered, so no handler meets a destroyed panel.
void ECSIPanels_DestroyClosed(void);

/// @brief Gives a panel the id it had in a saved session. Later panels get higher ids.
/// @param panel The panel.
/// @param id The saved id. 0 keeps the panel's new id.
void ECSIPanel_SetId(ECSPanel panel, u32 id);

/// @brief Describes a panel for a session: its id, type, saved state and the state's version. A working panel saves its state through its type; a placeholder writes back the state it kept.
/// @param panel The panel.
/// @param retPanel The value to set to a table.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation. A type that fails to save its state is reported, and the last saved state is written.
SHUWUR SHUResult ECSIPanel_Save(ECSPanel panel, ECSValue *retPanel);

/// @brief Places a panel. The panel is drawn again if its size changes.
/// @param panel Panel to place.
/// @param x Left edge in layout units.
/// @param y Top edge in layout units.
/// @param width Width in layout units.
/// @param height Height in layout units.
void ECSIPanel_SetRect(ECSPanel panel, f32 x, f32 y, f32 width, f32 height);

/// @brief Checks whether a visible panel needs a new frame.
/// @param panel Panel to check.
/// @return true if it draws continuously or asked to be drawn.
bool ECSIPanel_WantsFrame(ECSPanel panel);

/// @brief Draws a panel's pixels into its texture if needed. The panel's Draw may change its title, so this runs before the interface is declared.
/// @param panel Panel to draw.
/// @param renderer Renderer of the OS window that shows the panel.
/// @param nowTicks Current time in nanoseconds.
void ECSIPanel_Draw(ECSPanel panel, SDL_Renderer *renderer, u64 nowTicks);

/// @brief Puts a panel's texture on screen at the panel's rectangle.
/// @param panel Panel to show.
/// @param renderer Renderer of the OS window that shows the panel.
void ECSIPanel_Show(ECSPanel panel, SDL_Renderer *renderer);

/// @brief Tells a panel whether it is visible, and its size. Queues Shown, Hidden or Resized when they changed since the last call.
/// @param panel The panel.
/// @param visible true if the panel is shown in the current workspace.
void ECSIPanel_SetVisible(ECSPanel panel, bool visible);

/// @brief Emits one of the core's named events about a panel, with the value { panel = id, type = name }.
/// @param name Name of the event, such as "ecs.panelOpened".
/// @param panel The panel.
void ECSIPanel_Emit(const char *name, ECSPanel panel);

/// @brief Queues an event for a panel's type.
/// @param panel Panel that receives the event.
/// @param event The event.
void ECSIPanel_PostEvent(ECSPanel panel, const ECSPanelEvent *event);

#pragma endregion Declarations
