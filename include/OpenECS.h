#pragma once

// The plugin interface of OpenECS. This is the only header a plugin includes.

#include "shu/shu.h"

#pragma region Macros

/// @brief Version of OpenECS (DESIGN 19.1). Between releases it is the next release with "-dev".
#define OPENECS_VERSION "0.2.0-dev"

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
typedef struct ECSIPlugin *ECSPlugin;

/// @brief Handle of a panel.
typedef struct ECSIPanel *ECSPanel;

/// @brief Handle of a timer.
typedef struct ECSITimer *ECSTimer;

/// @brief A plugin's subscription to a named event.
typedef struct ECSISubscription *ECSSubscription;

/// @brief A function of a service, of any signature. Cast it to its real type before calling it.
typedef void (*ECSFunction)(void);

/// @brief A function that runs later, on the main thread or on a worker thread.
/// @param data The data given with the function.
typedef void (*ECSTaskFunction)(void *data);

/// @brief Function that a timer calls.
/// @param data The data given when the timer started.
typedef void (*ECSTimerFunction)(void *data);

/// @brief A generic value: nil, a boolean, an integer, a number, a string, or a table that holds a list and named fields. Saved state, settings and services use values.
typedef struct ECSIValue ECSValue;

/// @brief Function called with a named event that a plugin subscribed to.
/// @param data The data given to ECSEvent_Subscribe.
/// @param name Name of the event.
/// @param value What the event carries; nil if it carries nothing. Valid only during the call.
typedef void (*ECSEventFunction)(void *data, const char *name, const ECSValue *value);

/// @brief Destroys the object of a handle when Lua no longer uses the handle.
/// @param object The object.
typedef void (*ECSHandleDestroyFunction)(void *object);

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

/// @brief Where a panel goes, next to a target panel.
typedef enum ECSZone
{
    ECSZone_Default = 0, // where new panels go by default; for a move, the same as ECSZone_Center
    ECSZone_Center,      // the target's group
    ECSZone_Left,        // a split beside the target's group
    ECSZone_Right,
    ECSZone_Top,
    ECSZone_Bottom,
} ECSZone;

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
typedef enum ECSPanelEventType
{
    ECSPanelEventType_PointerDown = 0,
    ECSPanelEventType_PointerUp,
    ECSPanelEventType_PointerMove,
    ECSPanelEventType_Wheel,
    ECSPanelEventType_KeyDown,
    ECSPanelEventType_KeyUp,
    ECSPanelEventType_Focused,
    ECSPanelEventType_Unfocused,
    ECSPanelEventType_Shown,   // the panel became visible
    ECSPanelEventType_Hidden,  // the panel is no longer visible: another tab, workspace or maximized group is shown
    ECSPanelEventType_Resized, // the panel's size changed while it is visible
} ECSPanelEventType;

/// @brief An event sent to a panel. Its type chooses the member of the union that is set.
typedef struct ECSPanelEvent
{
    ECSPanelEventType type;
    u32 modifiers; // ECSModifier bits held when the event happened

    union
    {
        // PointerDown, PointerUp and PointerMove
        struct
        {
            f32 x;      // position in surface pixels
            f32 y;      // position in surface pixels
            i32 button; // PointerDown and PointerUp: 1 left, 2 middle, 3 right
        } pointer;

        // Wheel
        struct
        {
            f32 x;       // pointer position in surface pixels
            f32 y;       // pointer position in surface pixels
            f32 amountX; // scrolled amount; positive is to the right
            f32 amountY; // scrolled amount; positive is away from the user
        } wheel;

        // KeyDown and KeyUp
        struct
        {
            u32 code; // SDL key code
        } key;

        // Shown and Resized
        struct
        {
            f32 width;  // in layout units
            f32 height; // in layout units
        } size;
    };
} ECSPanelEvent;

/// @brief Creates a panel's state. Required.
/// @param panel The new panel.
/// @param savedState The state the session saved, or NULL for a new panel.
/// @param version The version the state was saved with.
/// @param retState The panel's state, which the other functions of the type get.
/// @return SHUResult_Ok, or an error; the panel then shows the error.
typedef SHUResult (*ECSPanelCreateFunction)(ECSPanel panel, const ECSValue *savedState, u32 version, void **retState);

/// @brief Destroys a panel's state. Required.
/// @param state The panel's state.
typedef void (*ECSPanelDestroyFunction)(void *state);

/// @brief Draws a panel into its surface.
/// @param state The panel's state.
/// @param surface The surface, valid only during the call.
/// @param seconds Time since the panel was last drawn.
typedef void (*ECSPanelDrawFunction)(void *state, ECSSurface *surface, f64 seconds);

/// @brief Tells a panel about an event.
/// @param state The panel's state.
/// @param event The event, valid only during the call.
typedef void (*ECSPanelEventFunction)(void *state, const ECSPanelEvent *event);

/// @brief Saves a panel's state into the session.
/// @param state The panel's state.
/// @param retState The value to fill; it starts as nil.
/// @return SHUResult_Ok, or an error.
typedef SHUResult (*ECSPanelSaveStateFunction)(void *state, ECSValue *retState);

/// @brief Saves a panel's unsaved work.
/// @param state The panel's state.
/// @return SHUResult_Ok, or an error, which cancels closing the panel.
typedef SHUResult (*ECSPanelSaveFunction)(void *state);

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
    ECSPanelCreateFunction Create;
    ECSPanelDestroyFunction Destroy;

    // optional, NULL if unused
    ECSPanelDrawFunction Draw;
    ECSPanelEventFunction Event;
    ECSPanelSaveStateFunction SaveState;
    ECSPanelSaveFunction Save;
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

/// @brief Tells a setting's owner that the value in effect changed. It runs after the queued events, outside other callbacks.
/// @param data The data given with the setting.
typedef void (*ECSSettingChangedFunction)(void *data);

/// @brief Describes a setting. Passed to ECSSetting_Declare.
typedef struct ECSSettingDesc
{
    const char *name; // "canvas.grid": plugin name + local name
    ECSSettingType type;
    const char *description; // one line, for the settings window
    // the default, in the field of the setting's type; a list or table setting starts empty
    bool defaultBool;
    i64 defaultInteger;
    f64 defaultNumber;
    const char *defaultString;  // string, choice and key settings
    const char *const *choices; // choice settings: the allowed strings, ending with NULL

    // optional, NULL if unused
    ECSSettingChangedFunction Changed;
    void *data; // passed to Changed
} ECSSettingDesc;

/// @brief Saves a plugin's own state when the session is saved.
/// @param data The data given with the state.
/// @param retState The value to fill; it starts as nil.
/// @return SHUResult_Ok, or an error.
typedef SHUResult (*ECSPluginStateSaveFunction)(void *data, ECSValue *retState);

/// @brief Restores a plugin's own state when a session that holds it is applied, before panels are created.
/// @param data The data given with the state.
/// @param state The saved state.
/// @param version The version the state was saved with.
/// @return SHUResult_Ok, or an error.
typedef SHUResult (*ECSPluginStateRestoreFunction)(void *data, const ECSValue *state, u32 version);

/// @brief Describes how a plugin saves its own state into the session, apart from its panels' state. Passed to ECSPlugin_RegisterState.
typedef struct ECSPluginStateDesc
{
    u32 version; // version of the state the plugin saves now; Restore gets the version the state was saved with

    ECSPluginStateSaveFunction Save;
    ECSPluginStateRestoreFunction Restore;
    void *data; // passed to the functions
} ECSPluginStateDesc;

/// @brief Type of a file dialog.
typedef enum ECSDialogType
{
    ECSDialogType_OpenFile = 0,
    ECSDialogType_SaveFile,
    ECSDialogType_OpenFolder,
} ECSDialogType;

/// @brief A filter of a file dialog.
typedef struct ECSDialogFilter
{
    const char *name;    // shown to the user, such as "Images"
    const char *pattern; // extensions without dots, separated by semicolons, such as "png;jpg"; "*" for every file
} ECSDialogFilter;

/// @brief Gets the user's choice in a file dialog, on the main thread.
/// @param data The data given with the dialog.
/// @param files The chosen paths, valid during the call, or NULL if the user cancelled or the dialog failed.
/// @param count Number of paths.
typedef void (*ECSDialogDoneFunction)(void *data, const char *const *files, usz count);

/// @brief Describes a file dialog. Passed to ECSDialog_Show.
typedef struct ECSDialogDesc
{
    ECSDialogType type;
    const ECSDialogFilter *filters; // file dialogs: the filters to choose from, or NULL
    usz filterCount;
    const char *location; // the folder or file to start in, or NULL
    bool many;            // open dialogs: the user may choose more than one

    ECSDialogDoneFunction Done;
    void *data; // passed to Done
} ECSDialogDesc;

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
/// @lua none: a Lua plugin's code runs when it is loaded
OPENECS_EXPORT SHUResult ECSPlugin_Init(ECSPlugin plugin);

/// @brief A native plugin may define this function. The core calls it once, before the program exits.
/// @param plugin The plugin itself.
/// @lua ecs.plugin.onShutdown
OPENECS_EXPORT void ECSPlugin_Shutdown(ECSPlugin plugin);

#pragma endregion Plugin Functions

#pragma region Core Functions

/// @brief Registers how a plugin saves and restores its own state in sessions. Call it from ECSPlugin_Init. Main thread only.
/// @param plugin The plugin.
/// @param desc Description of the state. The core copies it.
/// @return SHUResult_Ok, or SHUResult_ErrBadData if the description lacks Save or Restore, or the plugin registered its state already.
/// @lua ecs.plugin.registerState
OPENECS_EXPORT SHUWUR SHUResult ECSPlugin_RegisterState(ECSPlugin plugin, const ECSPluginStateDesc *desc);

/// @brief Writes a message to the log, with the plugin's name. Thread-safe.
/// @param plugin The plugin that writes the message.
/// @param level Level of the message.
/// @param format printf-style format.
/// @param ... Format arguments.
/// @lua ecs.log.debug, ecs.log.info, ecs.log.warn, ecs.log.error
OPENECS_EXPORT OPENECS_PRINTF(3, 4) void ECS_Log(ECSPlugin plugin, ECSLogLevel level, const char *format, ...);

/// @brief Runs a function on a worker thread, then another function on the main thread. Thread-safe.
/// @param plugin The plugin that asks.
/// @param work Runs on a worker thread from a small pool. It must not call core functions that are for the main thread only.
/// @param done Runs on the main thread after work returns, or NULL. Work that has not started when the program exits does not run, and neither does its done.
/// @param data Passed to both functions.
/// @return SHUResult_Ok, SHUResult_ErrAllocation, or SHUResult_ErrInternal if no worker thread can start.
/// @lua none: Lua code never runs on worker threads; Lua plugins use services that work in the background
OPENECS_EXPORT SHUWUR SHUResult ECS_RunInBackground(ECSPlugin plugin, ECSTaskFunction work, ECSTaskFunction done, void *data);

/// @brief Runs a function on the main thread, between passes of the main loop. Thread-safe.
/// @param function The function.
/// @param data Passed to the function.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation.
/// @lua none: Lua code runs only on the main thread
OPENECS_EXPORT SHUWUR SHUResult ECS_RunOnMainThread(ECSTaskFunction function, void *data);

/// @brief Registers a panel type. The core copies the description and its texts.
/// @param plugin The plugin that provides the panel type.
/// @param desc Description of the panel type. Its name must start with the plugin's name and a dot.
/// @return SHUResult_Ok, SHUResult_ErrBadData if the description is invalid, or SHUResult_ErrAllocation.
/// @lua ecs.panel.registerType
OPENECS_EXPORT SHUWUR SHUResult ECSPanelType_Register(ECSPlugin plugin, const ECSPanelTypeDesc *desc);

/// @brief Adds a function to the menu of a panel type's panels, after the core's entries. The entry shows the function's description, and runs it on the panel. Main thread only.
/// @param plugin The plugin that registered the panel type.
/// @param panelType Name of the panel type.
/// @param function Name of a service function whose signature is void(handle<ecs.panel>) or void().
/// @return SHUResult_Ok, SHUResult_ErrBadData if the panel type is not the plugin's, or SHUResult_ErrAllocation.
/// @lua ecs.panel.addMenuEntry
OPENECS_EXPORT SHUWUR SHUResult ECSPanelType_AddMenuEntry(ECSPlugin plugin, const char *panelType, const char *function);

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
OPENECS_EXPORT usz ECSValue_GetListCount(const ECSValue *table);

/// @brief Gets a list item of a table.
/// @param table The table, or NULL.
/// @param index Position of the item, starting at 0.
/// @return The item, or NULL if the value is not a table or has no such item.
OPENECS_EXPORT const ECSValue *ECSValue_GetListItem(const ECSValue *table, usz index);

/// @brief Gets a named field of a table.
/// @param table The table, or NULL.
/// @param name Name of the field.
/// @return The field, or NULL if the value is not a table or has no such field.
OPENECS_EXPORT const ECSValue *ECSValue_GetTableField(const ECSValue *table, const char *name);

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
OPENECS_EXPORT SHUWUR SHUResult ECSValue_ListAddItem(ECSValue *table, ECSValue **retItem);

/// @brief Gets a named field of a table to set it, and adds it as nil if it is missing. A value that is not a table becomes an empty table first.
/// @param table The table.
/// @param name Name of the field. The core copies it.
/// @param retField The field, to be set. Valid as long as the table is not set to something else.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation.
OPENECS_EXPORT SHUWUR SHUResult ECSValue_TableSetField(ECSValue *table, const char *name, ECSValue **retField);

/// @brief Declares a setting. Its value in effect comes from the highest settings layer that sets it with the right type; otherwise it is the default. Main thread only.
/// @param plugin The plugin that owns the setting.
/// @param desc Description of the setting. Its name must start with the plugin's name and a dot. The core copies it.
/// @return SHUResult_Ok, SHUResult_ErrBadData if the description is invalid or the name is taken, or SHUResult_ErrAllocation.
/// @lua ecs.settings.declare
OPENECS_EXPORT SHUWUR SHUResult ECSSetting_Declare(ECSPlugin plugin, const ECSSettingDesc *desc);

/// @brief Gets the value in effect of a declared setting. Main thread only.
/// @param name Name of the setting, such as "canvas.grid" or "ecs.focus".
/// @return The value, or NULL if no setting has the name. Valid until the setting changes.
/// @lua ecs.settings.get
OPENECS_EXPORT const ECSValue *ECSSetting_Get(const char *name);

/// @brief Registers a function of a plugin's service, so other plugins and Lua can call it. Main thread only.
/// @param plugin The plugin that provides the function.
/// @param name Name of the function. It must start with the plugin's name and a dot, such as "audio.play".
/// @param function The function, cast to ECSFunction. Its real type must match the signature; the core cannot check that.
/// @param signature The function's signature, such as "int(string, out float)". Types: void (result only), bool, int, int64, float, double, string, buffer, value, handle<name>, and out before a type.
/// @param description One line that says what the function does.
/// @return SHUResult_Ok, SHUResult_ErrBadData if the name or signature is invalid or the name is taken, or SHUResult_ErrAllocation.
/// @lua ecs.service.register
OPENECS_EXPORT SHUWUR SHUResult ECSService_RegisterFunction(ECSPlugin plugin, const char *name, ECSFunction function, const char *signature, const char *description);

/// @brief Looks up a function of a service. Lua functions come as C function pointers too. Main thread only.
/// @param plugin The plugin that asks. It may look up its own functions and those of the plugins its manifest depends on.
/// @param retFunction The function, to be cast to its real type. Valid until the program exits.
/// @param name Name of the function, such as "audio.play".
/// @param signature The signature the caller expects. It must match the registered one.
/// @return SHUResult_Ok, SHUResult_ErrNotFound if no function has the name, SHUResult_ErrPrivileges if the plugin does not depend on the provider, or SHUResult_ErrBadData if the signature is invalid or does not match.
/// @lua ecs.service.get
OPENECS_EXPORT SHUWUR SHUResult ECSService_GetFunction(ECSPlugin plugin, ECSFunction *retFunction, const char *name, const char *signature);

/// @brief Sets a setting in the settings window's layer, and writes that layer's file. A higher layer may still override it; ECSSetting_Explain tells. Main thread only.
/// @param name Name of the setting.
/// @param value The new value. It must have the setting's type. The core copies it.
/// @return SHUResult_Ok, SHUResult_ErrNotFound if no setting has the name, SHUResult_ErrBadData if the value has the wrong type, SHUResult_ErrFile if the file cannot be written, or SHUResult_ErrAllocation.
/// @lua ecs.settings.set
OPENECS_EXPORT SHUWUR SHUResult ECSSetting_Set(const char *name, const ECSValue *value);

/// @brief Lists every declared setting. Main thread only.
/// @param retList The value to set to a list of setting names, in the order they were declared.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation.
/// @lua ecs.settings.list
OPENECS_EXPORT SHUWUR SHUResult ECSSetting_List(ECSValue *retList);

/// @brief Explains a setting: its value in effect, the layer it comes from, and what each layer says. Main thread only.
/// @param name Name of the setting.
/// @param retExplanation The value to set to a table: name, type, description, owner, value, layer, file (of that layer; missing for defaults), choices (choice settings), and layers, which holds the value of each layer that sets it: default, preset, window and user.
/// @return SHUResult_Ok, SHUResult_ErrNotFound if no setting has the name, or SHUResult_ErrAllocation.
/// @lua ecs.settings.explain
OPENECS_EXPORT SHUWUR SHUResult ECSSetting_Explain(const char *name, ECSValue *retExplanation);

/// @brief Registers a type of handles, for services that pass objects as handle<name>. In Lua, a handle is a userdata that names its type; the same object always gets the same Lua handle. Main thread only.
/// @param plugin The plugin that provides the objects.
/// @param name Name of the type. It must start with the plugin's name and a dot, such as "audio.sound".
/// @param Destroy Called with the object when Lua no longer uses its handle, or NULL. A provider that keeps using the object counts references.
/// @return SHUResult_Ok, SHUResult_ErrBadData if the name is invalid or taken, or SHUResult_ErrAllocation.
/// @lua ecs.handle.registerType
OPENECS_EXPORT SHUWUR SHUResult ECSHandle_RegisterType(ECSPlugin plugin, const char *name, ECSHandleDestroyFunction Destroy);

/// @brief Binds a key to a function for one of the plugin's panel types: the key works while a panel of that type has focus. Main thread only.
/// @param plugin The plugin. It owns the panel type and the setting.
/// @param panelType Name of the panel type, such as "canvas.view".
/// @param setting Name of the plugin's key setting that holds the key combination, so the user can change it.
/// @param function Name of the function the key runs. It takes no arguments, or the focused panel: void() or void(handle<ecs.panel>).
/// @return SHUResult_Ok, SHUResult_ErrBadData if the panel type or the setting is not the plugin's, or SHUResult_ErrAllocation.
/// @lua ecs.input.bind
OPENECS_EXPORT SHUWUR SHUResult ECSKey_Bind(ECSPlugin plugin, const char *panelType, const char *setting, const char *function);

/// @brief Opens a panel in the current workspace and focuses it. Any plugin may open any panel type. Main thread only.
/// @param plugin The plugin that opens the panel.
/// @param retPanel The new panel. A missing type gives a placeholder.
/// @param type Name of the panel type.
/// @param state Saved state to create the panel from, in the type's current version, or NULL for a new panel. The core copies it.
/// @param target A panel of the current workspace to open next to, or NULL. With NULL, the panel joins the group of the most recently focused panel of its type, or else the focused group.
/// @param zone Where next to the target: its group or a side of it.
/// @return SHUResult_Ok, SHUResult_ErrNotFound if the target is not in the current workspace, or SHUResult_ErrAllocation.
/// @lua ecs.layout.open
OPENECS_EXPORT SHUWUR SHUResult ECSLayout_Open(ECSPlugin plugin, ECSPanel *retPanel, const char *type, const ECSValue *state, ECSPanel target, ECSZone zone);

/// @brief Moves a panel into a target's group, or beside it, also from another workspace. Main thread only.
/// @param panel Panel to move.
/// @param target The target panel.
/// @param zone Where next to the target.
/// @return SHUResult_Ok, or SHUResult_ErrNotFound if a panel is not in the layout.
/// @lua ecs.layout.move
OPENECS_EXPORT SHUWUR SHUResult ECSLayout_Move(ECSPanel panel, ECSPanel target, ECSZone zone);

/// @brief Closes a panel. If it has unsaved work, the user is asked first and may cancel. Main thread only.
/// @param panel Panel to close, or NULL for the user's close, which keys run: the focused panel, unless its group is locked. Its handle is invalid after the panel closes.
/// @return true if the panel closed.
/// @lua ecs.layout.close
OPENECS_EXPORT bool ECSLayout_Close(ECSPanel panel);

/// @brief Focuses a panel. If it is in another workspace, that workspace is shown; if it is behind another tab, its tab is shown. Main thread only.
/// @param panel Panel to focus.
/// @lua ecs.layout.focus
OPENECS_EXPORT void ECSLayout_Focus(ECSPanel panel);

/// @brief Gets the focused panel of the current workspace. Main thread only.
/// @return The panel, or NULL if the workspace has none.
/// @lua ecs.layout.getFocus
OPENECS_EXPORT ECSPanel ECSLayout_GetFocus(void);

/// @brief Finds a panel in any workspace by its id. Main thread only.
/// @param id The panel's id, as ECSPanel_GetId gives it and the core's events carry it.
/// @return The panel, or NULL if no open panel has the id.
/// @lua ecs.layout.find
OPENECS_EXPORT ECSPanel ECSLayout_FindPanel(u32 id);

/// @brief Counts the workspaces. Main thread only.
/// @return Number of workspaces.
/// @lua ecs.workspace.count
OPENECS_EXPORT usz ECSWorkspace_GetCount(void);

/// @brief Gets the current workspace. Main thread only.
/// @return Its number; workspaces are numbered from 1.
/// @lua ecs.workspace.getCurrent
OPENECS_EXPORT usz ECSWorkspace_GetCurrent(void);

/// @brief Gets a workspace's name. Main thread only.
/// @param number Number of the workspace, starting at 1.
/// @return The name, or NULL if there is no such workspace. Valid until the workspace goes away.
/// @lua ecs.workspace.getName
OPENECS_EXPORT const char *ECSWorkspace_GetName(usz number);

/// @brief Switches to a workspace. Main thread only.
/// @param number Number of the workspace, starting at 1: ECSWorkspace_Switch(10) switches to workspace 10. Ignored if there is no such workspace.
/// @lua ecs.workspace.switch
OPENECS_EXPORT void ECSWorkspace_Switch(usz number);

/// @brief Writes the session to a file: the plugins' state, the workspaces and the panels with their saved state. Quitting still saves the tool's last session. Main thread only.
/// @param path Path of the file, or NULL to ask the user with a save dialog that starts in the folder of saved sessions. Missing folders are created.
/// @return SHUResult_Ok when the file is written or the dialog is shown, SHUResult_ErrFile if the file cannot be written, or SHUResult_ErrAllocation.
/// @lua ecs.session.save
OPENECS_EXPORT SHUWUR SHUResult ECSSession_Save(const char *path);

/// @brief Opens a session in place of the current one: it asks about unsaved work, and once the current pass of the main loop ends, OpenECS saves the tool's last session, stops and starts again from the session. Main thread only.
/// @param path Path of the session file, or NULL to ask the user with an open dialog that starts in the folder of saved sessions.
/// @return SHUResult_Ok if OpenECS restarts into the session or the dialog is shown, SHUResult_ErrFile or SHUResult_ErrBadData if the file is not a session, SHUResult_Err if the user keeps the unsaved work, SHUResult_ErrPrivileges during a test, which cannot restart, or SHUResult_ErrAllocation.
/// @lua ecs.session.open
OPENECS_EXPORT SHUWUR SHUResult ECSSession_Open(const char *path);

/// @brief Puts text on the clipboard. Main thread only.
/// @param text The text. The core copies it.
/// @return SHUResult_Ok, or SHUResult_ErrInternal if the system refuses it.
/// @lua ecs.clipboard.setText
OPENECS_EXPORT SHUWUR SHUResult ECSClipboard_SetText(const char *text);

/// @brief Gets the text on the clipboard. Main thread only.
/// @return The text; empty if there is none. Valid until the next call of a clipboard function.
/// @lua ecs.clipboard.getText
OPENECS_EXPORT const char *ECSClipboard_GetText(void);

/// @brief Puts typed data on the clipboard, such as an image as "image/png". Main thread only.
/// @param mimeType The data's type.
/// @param data The data. The core copies it.
/// @return SHUResult_Ok, SHUResult_ErrAllocation, or SHUResult_ErrInternal if the system refuses it.
/// @lua ecs.clipboard.setData
OPENECS_EXPORT SHUWUR SHUResult ECSClipboard_SetData(const char *mimeType, SHUSliceView data);

/// @brief Gets typed data from the clipboard. Main thread only.
/// @param mimeType The type wanted.
/// @param retData The data; empty if the clipboard has none of that type. Valid until the next call of a clipboard function.
/// @return SHUResult_Ok, or SHUResult_ErrNotFound if the clipboard has no data of that type.
/// @lua ecs.clipboard.getData
OPENECS_EXPORT SHUWUR SHUResult ECSClipboard_GetData(const char *mimeType, SHUSlice *retData);

/// @brief Shows a file dialog. It does not wait: the description's Done function gets the answer later. Main thread only.
/// @param plugin The plugin that asks.
/// @param desc Description of the dialog. The core copies it.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation.
/// @lua ecs.dialog.show
OPENECS_EXPORT SHUWUR SHUResult ECSDialog_Show(ECSPlugin plugin, const ECSDialogDesc *desc);

/// @brief Shows a message dialog and waits for the user to press a button. Main thread only.
/// @param title Title of the dialog.
/// @param message The message.
/// @param buttons Texts of the buttons, from left to right. The first is the default for Enter, and the last for Escape.
/// @param buttonCount Number of buttons, at least 1.
/// @param retButton Position of the button the user pressed, starting at 0.
/// @return SHUResult_Ok, SHUResult_ErrNotFound if the user closed the dialog without a button, or SHUResult_ErrInternal if it cannot be shown.
/// @lua ecs.dialog.message
OPENECS_EXPORT SHUWUR SHUResult ECSDialog_ShowMessage(const char *title, const char *message, const char *const *buttons, usz buttonCount, usz *retButton);

/// @brief Starts a timer that calls a function on the main thread, once or repeatedly. Main thread only.
/// @param plugin The plugin that owns the timer.
/// @param retTimer The new timer. A one-shot timer's handle is invalid after its function returns.
/// @param seconds Time until the first call, and between repeated calls. Must be positive for a repeating timer; 0 for a one-shot timer means the next pass of the main loop.
/// @param repeat true to call the function until the timer is stopped.
/// @param function Function to call.
/// @param data Passed to the function.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation.
/// @brief Declares a named event that a plugin emits. Main thread only.
/// @param plugin The plugin that emits the event.
/// @param name Name of the event: the plugin's name, a dot and a local name, such as "canvas.selectionChanged".
/// @param description One line about the event.
/// @return SHUResult_Ok, SHUResult_ErrBadData if the name is taken or does not belong to the plugin, or SHUResult_ErrAllocation.
/// @lua ecs.event.declare
OPENECS_EXPORT SHUWUR SHUResult ECSEvent_Declare(ECSPlugin plugin, const char *name, const char *description);

/// @brief Emits a named event that the plugin declared. Subscribers get it after the current callback returns. Main thread only.
/// @param plugin The plugin that declared the event.
/// @param name Name of the event.
/// @param value What the event carries, or NULL. The core copies it.
/// @return SHUResult_Ok, SHUResult_ErrNotFound if the plugin declared no such event, or SHUResult_ErrAllocation.
/// @lua ecs.event.emit
OPENECS_EXPORT SHUWUR SHUResult ECSEvent_Emit(ECSPlugin plugin, const char *name, const ECSValue *value);

/// @brief Subscribes to a named event. The event must be declared by the core, by the plugin itself, or by a plugin its manifest depends on. Main thread only.
/// @param plugin The plugin that subscribes.
/// @param name Name of the event.
/// @param retSubscription The new subscription.
/// @param function Function called with each emission.
/// @param data Passed to the function.
/// @return SHUResult_Ok, SHUResult_ErrNotFound if no such event is declared, SHUResult_ErrBadData if the plugin does not depend on the event's plugin, or SHUResult_ErrAllocation.
/// @lua ecs.event.subscribe
OPENECS_EXPORT SHUWUR SHUResult ECSEvent_Subscribe(ECSPlugin plugin, const char *name, ECSSubscription *retSubscription, ECSEventFunction function, void *data);

/// @brief Ends a subscription and sets the handle to NULL. Main thread only.
/// @param subscription The subscription, or a handle to NULL.
/// @lua subscription:cancel
OPENECS_EXPORT void ECSEvent_Unsubscribe(ECSSubscription *subscription);

/// @lua ecs.timer.start
OPENECS_EXPORT SHUWUR SHUResult ECSTimer_Start(ECSPlugin plugin, ECSTimer *retTimer, f64 seconds, bool repeat, ECSTimerFunction function, void *data);

/// @brief Stops a timer and sets the handle to NULL. A timer may stop itself from its own function. Main thread only.
/// @param timer Timer to stop.
/// @lua timer:stop
OPENECS_EXPORT void ECSTimer_Stop(ECSTimer *timer);

/// @brief Starts a timer that belongs to a panel, like ECSTimer_Start. The timer stops when the panel closes; its handle is invalid then. Main thread only.
/// @param panel The panel. Its type's plugin owns the timer.
/// @param retTimer The new timer. A one-shot timer's handle is invalid after its function returns.
/// @param seconds Time until the first call, and between repeated calls. Must be positive for a repeating timer.
/// @param repeat true to call the function until the timer is stopped.
/// @param function Function to call.
/// @param data Passed to the function.
/// @return SHUResult_Ok, or SHUResult_ErrAllocation.
/// @lua ecs.panel.startTimer, panel:startTimer
OPENECS_EXPORT SHUWUR SHUResult ECSPanel_StartTimer(ECSPanel panel, ECSTimer *retTimer, f64 seconds, bool repeat, ECSTimerFunction function, void *data);

/// @brief Marks whether a panel has unsaved work. Before such a panel closes, the core asks the user to save it, discard it or cancel. Main thread only.
/// @param panel Panel to mark.
/// @param unsaved true if the panel has unsaved work.
/// @lua ecs.panel.setUnsaved, panel:setUnsaved
OPENECS_EXPORT void ECSPanel_SetUnsaved(ECSPanel panel, bool unsaved);

/// @brief Asks the core to draw the panel again.
/// @param panel Panel to draw.
/// @lua ecs.panel.redraw, panel:redraw
OPENECS_EXPORT void ECSPanel_Redraw(ECSPanel panel);

/// @brief Gets the panel's title, shown in its tab.
/// @param panel Panel to read.
/// @return The title. Valid until the title changes.
/// @lua ecs.panel.getTitle, panel:getTitle
OPENECS_EXPORT const char *ECSPanel_GetTitle(ECSPanel panel);

/// @brief Sets the panel's title, shown in its tab. The core copies the text.
/// @param panel Panel to change.
/// @param title New title.
/// @lua ecs.panel.setTitle, panel:setTitle
OPENECS_EXPORT void ECSPanel_SetTitle(ECSPanel panel, const char *title);

/// @brief Gets a panel's id, which is unique and stable within a session. Main thread only.
/// @param panel The panel.
/// @return The id.
/// @lua ecs.panel.getId, panel:getId
OPENECS_EXPORT u32 ECSPanel_GetId(ECSPanel panel);

/// @brief Gets the name of a panel's type, such as "canvas.view". Main thread only.
/// @param panel The panel.
/// @return The name. Valid while the panel exists.
/// @lua ecs.panel.getType, panel:getType
OPENECS_EXPORT const char *ECSPanel_GetType(ECSPanel panel);

#pragma endregion Core Functions
