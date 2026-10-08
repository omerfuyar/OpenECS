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

/// @brief Handle of a timer.
typedef struct ECSI_Timer *ECSTimer;

/// @brief A function of a service, of any signature. Cast it to its real type before calling it.
typedef void (*ECSFunction)(void);

/// @brief Function that a timer calls.
/// @param data The data given when the timer started.
typedef void (*ECSTimerFunction)(void *data);

/// @brief A generic value: nil, a boolean, an integer, a number, a string, or a table that holds a list and named fields. Saved state, settings and services use values.
typedef struct ECSI_Value ECSValue;

/// @brief Type of a value.
typedef enum ECSValueType
{
    ECSValueType_Nil = 0,
    ECSValueType_Bool,
    ECSValueType_Integer,
    ECSValueType_Number,
    ECSValueType_String,
    ECSValueType_Table,
} ECSValueType;

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

/// @brief Type of a setting's value.
typedef enum ECSSettingType
{
    ECSSettingType_Bool = 0,
    ECSSettingType_Integer,
    ECSSettingType_Number,
    ECSSettingType_String,
    ECSSettingType_Choice, // one string of a list
    ECSSettingType_Key,    // a key combination, such as "Ctrl+Shift+P"
    ECSSettingType_List,   // a table with list items only
    ECSSettingType_Table,
} ECSSettingType;

/// @brief Describes a setting. Passed to ECSSetting_Declare.
typedef struct ECSSettingDesc
{
    const char *name;        // "canvas.grid": plugin name + local name
    ECSSettingType type;
    const char *description; // one line, for the settings window
    // the default, in the field of the setting's type; a list or table setting starts empty
    bool defaultBool;
    i64 defaultInteger;
    f64 defaultNumber;
    const char *defaultString; // string, choice and key settings
    const char *const *choices; // choice settings: the allowed strings, ending with NULL
} ECSSettingDesc;

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
/// @return SHUResult_Ok, SHUResult_ErrBadData if the description is invalid, or SHUResult_ErrAllocation.
OPENECS_EXPORT SHUWUR SHUResult ECSPanelType_Register(ECSPlugin plugin, const ECSPanelTypeDesc *desc);

/// @brief Creates a nil value, for a plugin that passes a value to the core or to a service. Main thread only.
/// @param retValue The new value. Destroy it with ECSValue_Destroy.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation.
OPENECS_EXPORT SHUWUR SHUResult ECSValue_Create(ECSValue **retValue);

/// @brief Destroys a value that ECSValue_Create made, with everything it holds, and sets the handle to NULL. Main thread only.
/// @param value Value to destroy, or a handle to NULL.
OPENECS_EXPORT void ECSValue_Destroy(ECSValue **value);

/// @brief Gets the type of a value.
/// @param value The value, or NULL.
/// @return Its type; ECSValueType_Nil for NULL.
OPENECS_EXPORT ECSValueType ECSValue_GetType(const ECSValue *value);

/// @brief Reads a boolean.
/// @param value The value, or NULL.
/// @param fallback Returned if the value is not a boolean.
/// @return The boolean.
OPENECS_EXPORT bool ECSValue_GetBool(const ECSValue *value, bool fallback);

/// @brief Reads an integer. A number with no fraction counts as an integer.
/// @param value The value, or NULL.
/// @param fallback Returned if the value is not an integer.
/// @return The integer.
OPENECS_EXPORT i64 ECSValue_GetInteger(const ECSValue *value, i64 fallback);

/// @brief Reads a number. An integer counts as a number.
/// @param value The value, or NULL.
/// @param fallback Returned if the value is not a number.
/// @return The number.
OPENECS_EXPORT f64 ECSValue_GetNumber(const ECSValue *value, f64 fallback);

/// @brief Reads a string.
/// @param value The value, or NULL.
/// @param fallback Returned if the value is not a string.
/// @return The string. Valid as long as the value does not change.
OPENECS_EXPORT const char *ECSValue_GetString(const ECSValue *value, const char *fallback);

/// @brief Counts the list items of a table.
/// @param table The table, or NULL.
/// @return Number of items; 0 if the value is not a table.
OPENECS_EXPORT usz ECSValue_GetCount(const ECSValue *table);

/// @brief Gets a list item of a table.
/// @param table The table, or NULL.
/// @param index Position of the item, starting at 0.
/// @return The item, or NULL if the value is not a table or has no such item.
OPENECS_EXPORT const ECSValue *ECSValue_GetItem(const ECSValue *table, usz index);

/// @brief Gets a named field of a table.
/// @param table The table, or NULL.
/// @param name Name of the field.
/// @return The field, or NULL if the value is not a table or has no such field.
OPENECS_EXPORT const ECSValue *ECSValue_GetField(const ECSValue *table, const char *name);

/// @brief Makes a value nil.
/// @param value The value.
OPENECS_EXPORT void ECSValue_SetNil(ECSValue *value);

/// @brief Makes a value a boolean.
/// @param value The value.
/// @param boolean The boolean.
OPENECS_EXPORT void ECSValue_SetBool(ECSValue *value, bool boolean);

/// @brief Makes a value an integer.
/// @param value The value.
/// @param integer The integer.
OPENECS_EXPORT void ECSValue_SetInteger(ECSValue *value, i64 integer);

/// @brief Makes a value a number.
/// @param value The value.
/// @param number The number.
OPENECS_EXPORT void ECSValue_SetNumber(ECSValue *value, f64 number);

/// @brief Makes a value a string. The core copies the text.
/// @param value The value.
/// @param string The string.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation; the value is unchanged then.
OPENECS_EXPORT SHUWUR SHUResult ECSValue_SetString(ECSValue *value, const char *string);

/// @brief Makes a value an empty table.
/// @param value The value.
OPENECS_EXPORT void ECSValue_SetTable(ECSValue *value);

/// @brief Adds a nil item to the end of a table's list. A value that is not a table becomes an empty table first.
/// @param table The table.
/// @param retItem The new item, to be set. Valid as long as the table is not set to something else.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation.
OPENECS_EXPORT SHUWUR SHUResult ECSValue_AddItem(ECSValue *table, ECSValue **retItem);

/// @brief Gets a named field of a table to set it, and adds it as nil if it is missing. A value that is not a table becomes an empty table first.
/// @param table The table.
/// @param name Name of the field. The core copies it.
/// @param retField The field, to be set. Valid as long as the table is not set to something else.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation.
OPENECS_EXPORT SHUWUR SHUResult ECSValue_SetField(ECSValue *table, const char *name, ECSValue **retField);

/// @brief Declares a setting. Its value in effect comes from the highest settings layer that sets it with the right type; otherwise it is the default. Main thread only.
/// @param plugin The plugin that owns the setting.
/// @param desc Description of the setting. Its name must start with the plugin's name and a dot. The core copies it.
/// @return SHUResult_Ok, SHUResult_ErrBadData if the description is invalid or the name is taken, or SHUResult_ErrAllocation.
OPENECS_EXPORT SHUWUR SHUResult ECSSetting_Declare(ECSPlugin plugin, const ECSSettingDesc *desc);

/// @brief Gets the value in effect of a declared setting. Main thread only.
/// @param name Name of the setting, such as "canvas.grid" or "ecs.focus".
/// @return The value, or NULL if no setting has the name. Valid until the setting changes.
OPENECS_EXPORT const ECSValue *ECSSetting_Get(const char *name);

/// @brief Registers a function of a plugin's service, so other plugins and Lua can call it. Main thread only.
/// @param plugin The plugin that provides the function.
/// @param name Name of the function. It must start with the plugin's name and a dot, such as "audio.play".
/// @param function The function, cast to ECSFunction. Its real type must match the signature; the core cannot check that.
/// @param signature The function's signature, such as "int(string, float)". Types: void (result only), bool, int, int64, float, double, string.
/// @param description One line that says what the function does.
/// @return SHUResult_Ok, SHUResult_ErrBadData if the name or signature is invalid or the name is taken, or SHUResult_ErrAllocation.
OPENECS_EXPORT SHUWUR SHUResult ECSService_RegisterFunction(ECSPlugin plugin, const char *name, ECSFunction function, const char *signature, const char *description);

/// @brief Looks up a function of a service. Lua functions come as C function pointers too. Main thread only.
/// @param plugin The plugin that asks. It may look up its own functions and those of the plugins its manifest depends on.
/// @param retFunction The function, to be cast to its real type. Valid until the program exits.
/// @param name Name of the function, such as "audio.play".
/// @param signature The signature the caller expects. It must match the registered one.
/// @return SHUResult_Ok, SHUResult_ErrNotFound if no function has the name, SHUResult_ErrPrivileges if the plugin does not depend on the provider, or SHUResult_ErrBadData if the signature is invalid or does not match.
OPENECS_EXPORT SHUWUR SHUResult ECSService_GetFunction(ECSPlugin plugin, ECSFunction *retFunction, const char *name, const char *signature);

/// @brief Starts a timer that calls a function on the main thread, once or repeatedly. Main thread only.
/// @param plugin The plugin that owns the timer.
/// @param retTimer The new timer. A one-shot timer's handle is invalid after its function returns.
/// @param seconds Time until the first call, and between repeated calls. Must be positive for a repeating timer; 0 for a one-shot timer means the next pass of the main loop.
/// @param repeat true to call the function until the timer is stopped.
/// @param function Function to call.
/// @param data Passed to the function.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation.
OPENECS_EXPORT SHUWUR SHUResult ECSTimer_Start(ECSPlugin plugin, ECSTimer *retTimer, f64 seconds, bool repeat, ECSTimerFunction function, void *data);

/// @brief Stops a timer and sets the handle to NULL. A timer may stop itself from its own function. Main thread only.
/// @param timer Timer to stop.
OPENECS_EXPORT void ECSTimer_Stop(ECSTimer *timer);

/// @brief Starts a timer that belongs to a panel, like ECSTimer_Start. The timer stops when the panel closes; its handle is invalid then. Main thread only.
/// @param panel The panel. Its type's plugin owns the timer.
/// @param retTimer The new timer. A one-shot timer's handle is invalid after its function returns.
/// @param seconds Time until the first call, and between repeated calls. Must be positive for a repeating timer.
/// @param repeat true to call the function until the timer is stopped.
/// @param function Function to call.
/// @param data Passed to the function.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation.
OPENECS_EXPORT SHUWUR SHUResult ECSPanel_StartTimer(ECSPanel panel, ECSTimer *retTimer, f64 seconds, bool repeat, ECSTimerFunction function, void *data);

/// @brief Marks whether a panel has unsaved work. Before such a panel closes, the core asks the user to save it, discard it or cancel. Main thread only.
/// @param panel Panel to mark.
/// @param unsaved true if the panel has unsaved work.
OPENECS_EXPORT void ECSPanel_SetUnsaved(ECSPanel panel, bool unsaved);

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
