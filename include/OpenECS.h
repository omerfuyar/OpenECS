#pragma once

// The plugin interface of OpenECS. This is the only header a plugin includes.

#include "shu/shu.h"

#include <stdarg.h>

#pragma region Declarations

/// @brief Version of this plugin interface. The core refuses a plugin built for another version.
#define OPENECS_API_VERSION 1

/// @brief Capacity of one log message, including the closing zero byte.
#define OPENECS_LOG_CAPACITY 512

/// @brief Handle of a panel.
typedef struct ECSI_Panel *ECSPanel;

/// @brief Handle of the calling plugin. It is also the table of core functions that the plugin calls through.
typedef const struct ECSContextTable *ECSContext;

/// @brief Saved state of a panel. Saving state is not implemented yet, so it is always NULL.
typedef struct ECSI_Value ECSValue;

/// @brief Kind of picture a panel draws into.
typedef enum ECSSurfaceKind
{
    ECSSurfaceKind_Pixels = 0,
    ECSSurfaceKind_Gpu,
} ECSSurfaceKind;

/// @brief The picture a panel draws into. Valid only during the Draw call.
typedef struct ECSSurface
{
    ECSSurfaceKind kind;
    i32 width;       // physical pixels
    i32 height;      // physical pixels
    f32 scale;       // physical pixels per layout unit
    SHUSlice pixels; // pixels kind: ARGB8888 in native byte order, premultiplied alpha
    i32 pitch;       // pixels kind: bytes per row
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

/// @brief Kind of an event.
typedef enum ECSEventKind
{
    ECSEventKind_PointerDown = 0,
    ECSEventKind_PointerUp,
    ECSEventKind_PointerMove,
    ECSEventKind_Wheel,
    ECSEventKind_KeyDown,
    ECSEventKind_KeyUp,
    ECSEventKind_Focused,
    ECSEventKind_Unfocused,
} ECSEventKind;

/// @brief An event sent to a panel. Only the fields that belong to its kind are set.
typedef struct ECSEvent
{
    u32 structSize;
    ECSEventKind kind;
    f32 x;         // pointer events: position in surface pixels
    f32 y;
    f32 wheelX;    // wheel events
    f32 wheelY;
    i32 button;    // pointer buttons: 1 left, 2 middle, 3 right
    u32 key;       // key events: SDL key code
    u32 modifiers; // ECSModifier bits
} ECSEvent;

/// @brief Describes a panel type. Passed to ECSPanelType_Register.
typedef struct ECSPanelTypeDesc
{
    u32 structSize;         // sizeof this struct
    const char *name;       // "canvas.view": plugin name + local name
    const char *title;      // default title for tabs and menus
    u32 stateVersion;       // version of the saved state
    ECSSurfaceKind surface; // only ECSSurfaceKind_Pixels is implemented
    bool continuous;        // draw every frame while visible
    f32 minWidth;           // in layout units, 0 for none
    f32 minHeight;

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
    ECSLogLevel_Info = 0,
    ECSLogLevel_Warning,
    ECSLogLevel_Error,
} ECSLogLevel;

/// @brief The core functions that a native plugin calls through. Call the wrapper functions below instead.
struct ECSContextTable
{
    u32 structSize;
    void (*Log)(ECSContext context, ECSLogLevel level, const char *message);
    SHUResult (*PanelTypeRegister)(ECSContext context, const ECSPanelTypeDesc *desc);
    void (*PanelRedraw)(ECSPanel panel);
    const char *(*PanelGetTitle)(ECSPanel panel);
    void (*PanelSetTitle)(ECSPanel panel, const char *title);
};

/// @brief Describes a native plugin. Returned by ECSPlugin_Main.
typedef struct ECSPluginInfo
{
    u32 structSize;
    u32 apiVersion;                        // OPENECS_API_VERSION when the plugin was built
    SHUResult (*Init)(ECSContext context); // registers everything
    void (*Shutdown)(ECSContext context);
} ECSPluginInfo;

/// @brief The only name a native plugin exports. Defined by ECSPlugin_Define.
/// @return Description of the plugin.
const ECSPluginInfo *ECSPlugin_Main(void);

/// @brief Context of this plugin. Set by ECSPlugin_Define before Init runs.
extern ECSContext ECSI_context;

/// @brief Defines ECSPlugin_Main for a native plugin. Use it once, in one source file of the plugin.
/// @param init Function that registers everything the plugin provides. Returns SHUResult_Ok on success.
/// @param shutdown Function called before the program exits.
#define ECSPlugin_Define(init, shutdown)                                                                           \
    ECSContext ECSI_context = NULL;                                                                               \
                                                                                                                   \
    static SHUResult ECSI_PluginInit(ECSContext context)                                                          \
    {                                                                                                              \
        ECSI_context = context;                                                                                   \
        return init(context);                                                                                      \
    }                                                                                                              \
                                                                                                                   \
    __attribute__((visibility("default"))) const ECSPluginInfo *ECSPlugin_Main(void)                              \
    {                                                                                                              \
        static const ECSPluginInfo info = {sizeof(ECSPluginInfo), OPENECS_API_VERSION, ECSI_PluginInit, shutdown}; \
        return &info;                                                                                              \
    }

#pragma endregion Declarations

#pragma region Functions

/// @brief Formats a message and sends it to the core's log. Use the ECS_Log functions instead.
/// @param context Context of the plugin.
/// @param level Level of the message.
/// @param format printf-style format.
/// @param arguments Format arguments.
static inline void ECSI_LogFormat(ECSContext context, ECSLogLevel level, const char *format, va_list arguments)
{
    char message[OPENECS_LOG_CAPACITY];
    vsnprintf(message, sizeof(message), format, arguments);
    context->Log(context, level, message);
}

/// @brief Writes an informational message to the log, with the plugin's name.
/// @param context Context of the plugin.
/// @param format printf-style format.
/// @param ... Format arguments.
__attribute__((format(printf, 2, 3))) static inline void ECS_LogInfo(ECSContext context, const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    ECSI_LogFormat(context, ECSLogLevel_Info, format, arguments);
    va_end(arguments);
}

/// @brief Writes a warning to the log, with the plugin's name.
/// @param context Context of the plugin.
/// @param format printf-style format.
/// @param ... Format arguments.
__attribute__((format(printf, 2, 3))) static inline void ECS_LogWarning(ECSContext context, const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    ECSI_LogFormat(context, ECSLogLevel_Warning, format, arguments);
    va_end(arguments);
}

/// @brief Writes an error to the log, with the plugin's name. Does not stop the program.
/// @param context Context of the plugin.
/// @param format printf-style format.
/// @param ... Format arguments.
__attribute__((format(printf, 2, 3))) static inline void ECS_LogError(ECSContext context, const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    ECSI_LogFormat(context, ECSLogLevel_Error, format, arguments);
    va_end(arguments);
}

/// @brief Registers a panel type. The core copies the description.
/// @param context Context of the plugin.
/// @param desc Description of the panel type. Its name must start with the plugin's name and a dot.
/// @return SHUResult_Ok, SHUResult_ErrBadData if the description is invalid, or SHUResult_ErrOverflow if there is no room for more panel types.
static inline SHUResult ECSPanelType_Register(ECSContext context, const ECSPanelTypeDesc *desc)
{
    return context->PanelTypeRegister(context, desc);
}

/// @brief Asks the core to draw the panel again.
/// @param panel Panel to draw.
static inline void ECSPanel_Redraw(ECSPanel panel)
{
    ECSI_context->PanelRedraw(panel);
}

/// @brief Gets the panel's title, shown in its tab.
/// @param panel Panel to read.
/// @return The title. Valid until the title changes.
static inline const char *ECSPanel_GetTitle(ECSPanel panel)
{
    return ECSI_context->PanelGetTitle(panel);
}

/// @brief Sets the panel's title, shown in its tab. The core copies the text.
/// @param panel Panel to change.
/// @param title New title.
static inline void ECSPanel_SetTitle(ECSPanel panel, const char *title)
{
    ECSI_context->PanelSetTitle(panel, title);
}

#pragma endregion Functions
