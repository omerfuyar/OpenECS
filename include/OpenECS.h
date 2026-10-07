#pragma once

// The plugin interface of OpenECS. This is the only header a plugin includes.

#include "shu/shu.h"

#pragma region Macros

/// @brief Version of this plugin interface. The core refuses a plugin whose manifest names another version.
#define OPENECS_API_VERSION 1

/// @brief Marks a function that crosses the boundary between the core and a plugin: a core function that plugins call, or a plugin function that the core calls.
#define OPENECS_EXPORT __attribute__((visibility("default")))

/// @brief Lets the compiler check the arguments of a printf-style function.
/// @param formatIndex Position of the format parameter, counting from 1.
/// @param argumentsIndex Position of the first argument that the format uses.
#define OPENECS_PRINTF(formatIndex, argumentsIndex) __attribute__((format(printf, formatIndex, argumentsIndex)))

#pragma endregion Macros

#pragma region Types

/// @brief Handle of a loaded plugin. Functions that act for a plugin take it first.
typedef struct ECSI_Plugin *ECSPlugin;

/// @brief Handle of a panel.
typedef struct ECSI_Panel *ECSPanel;

/// @brief Saved state of a panel. Saving state is not implemented yet, so it is always NULL.
typedef struct ECSI_Value ECSValue;

/// @brief Type of picture a panel draws into.
typedef enum ECSSurfaceType
{
    ECSSurfaceType_Pixels = 0,
    ECSSurfaceType_Gpu,
} ECSSurfaceType;

/// @brief The picture a panel draws into. Valid only during the Draw call.
typedef struct ECSSurface
{
    ECSSurfaceType type;
    i32 width;       // physical pixels
    i32 height;      // physical pixels
    f32 scale;       // physical pixels per layout unit
    SHUSlice pixels; // pixels type: ARGB8888 in native byte order, premultiplied alpha
    i32 pitch;       // pixels type: bytes per row
} ECSSurface;

/// @brief Modifier keys held during an event, as bits.
typedef enum ECSModifier
{
    ECSModifier_None = 0,
    ECSModifier_Shift = 1 << 0,
    ECSModifier_Ctrl = 1 << 1,
    ECSModifier_Alt = 1 << 2,
    ECSModifier_Super = 1 << 3,
    ECSModifier_AltGr = 1 << 4,
} ECSModifier;

/// @brief Type of an event.
typedef enum ECSEventType
{
    ECSEventType_PointerDown = 0,
    ECSEventType_PointerUp,
    ECSEventType_PointerMove,
    ECSEventType_Wheel,
    ECSEventType_KeyDown,
    ECSEventType_KeyUp,
    ECSEventType_Focused,
    ECSEventType_Unfocused,
} ECSEventType;

/// @brief An event sent to a panel. Only the fields that belong to its type are set.
typedef struct ECSEvent
{
    ECSEventType type;
    f32 x;         // pointer events: position in surface pixels
    f32 y;         // pointer events: position in surface pixels
    f32 wheelX;    // wheel events
    f32 wheelY;    // wheel events
    i32 button;    // pointer buttons: 1 left, 2 middle, 3 right
    u32 key;       // key events: SDL key code
    u32 modifiers; // ECSModifier bits
} ECSEvent;

/// @brief Describes a panel type. Passed to ECSPanelType_Register.
typedef struct ECSPanelTypeDesc
{
    const char *name;       // "canvas.view": plugin name + local name
    const char *title;      // default title for tabs and menus
    u32 stateVersion;       // version of the saved state
    ECSSurfaceType surface; // only ECSSurfaceType_Pixels is implemented
    bool continuous;        // draw every frame while visible
    f32 minWidth;           // in layout units, 0 for none
    f32 minHeight;          // in layout units, 0 for none

    // required
    SHUResult (*Create)(ECSPanel panel, const ECSValue *savedState, u32 version, void **retState);
    void (*Destroy)(void *state);

    // optional, NULL if unused
    void (*Draw)(void *state, ECSSurface *surface, f64 seconds);
    void (*Event)(void *state, const ECSEvent *event);
    SHUResult (*SaveState)(void *state, ECSValue *retState);
    SHUResult (*Save)(void *state); // saves unsaved work
} ECSPanelTypeDesc;

/// @brief Level of a log message.
typedef enum ECSLogLevel
{
    ECSLogLevel_Debug = 0,
    ECSLogLevel_Info,
    ECSLogLevel_Warning,
    ECSLogLevel_Error,
} ECSLogLevel;

#pragma endregion Types

#pragma region Plugin Functions

/// @brief Every native plugin defines this function. The core calls it once, after it loads the plugin's library. The plugin registers everything it provides here.
/// @param plugin The plugin itself. Keep it for the functions that act for the plugin.
/// @return SHUResult_Ok, or an error to mark the plugin failed.
OPENECS_EXPORT SHUResult ECSPlugin_Init(ECSPlugin plugin);

/// @brief A native plugin may define this function. The core calls it once, before the program exits.
/// @param plugin The plugin itself.
OPENECS_EXPORT void ECSPlugin_Shutdown(ECSPlugin plugin);

#pragma endregion Plugin Functions

#pragma region Core Functions

/// @brief Writes a message to the log, with the plugin's name. Thread-safe.
/// @param plugin The plugin that writes the message.
/// @param level Level of the message.
/// @param format printf-style format.
/// @param ... Format arguments.
OPENECS_EXPORT OPENECS_PRINTF(3, 4) void ECS_Log(ECSPlugin plugin, ECSLogLevel level, const char *format, ...);

/// @brief Registers a panel type. The core copies the description and its texts.
/// @param plugin The plugin that provides the panel type.
/// @param desc Description of the panel type. Its name must start with the plugin's name and a dot.
/// @return SHUResult_Ok, SHUResult_ErrBadData if the description is invalid, SHUResult_ErrOverflow if there is no room for more panel types, or SHUResult_ErrAllocation.
OPENECS_EXPORT SHUWUR SHUResult ECSPanelType_Register(ECSPlugin plugin, const ECSPanelTypeDesc *desc);

/// @brief Asks the core to draw the panel again.
/// @param panel Panel to draw.
OPENECS_EXPORT void ECSPanel_Redraw(ECSPanel panel);

/// @brief Gets the panel's title, shown in its tab.
/// @param panel Panel to read.
/// @return The title. Valid until the title changes.
OPENECS_EXPORT const char *ECSPanel_GetTitle(ECSPanel panel);

/// @brief Sets the panel's title, shown in its tab. The core copies the text.
/// @param panel Panel to change.
/// @param title New title.
OPENECS_EXPORT void ECSPanel_SetTitle(ECSPanel panel, const char *title);

#pragma endregion Core Functions
