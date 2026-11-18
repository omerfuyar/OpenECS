#pragma once

// Profile: where the main thread's time goes, by step of the main loop, by plugin and by panel, while the setting ecs.profile is on (DESIGN 3.5).

#include "OpenECS.h"

#pragma region Types

/// @brief A step of the main loop's pass (DESIGN 3.1). Each moment of a pass belongs to one step.
typedef enum ECSIProfileStep : u8
{
    ECSIProfileStep_Wait,      // waiting for input, a timer or a frame
    ECSIProfileStep_Input,     // handling input
    ECSIProfileStep_Timers,    // running timers
    ECSIProfileStep_Events,    // delivering events, and the rest of the pass before the frame
    ECSIProfileStep_Layout,    // updating the layout and the popups
    ECSIProfileStep_Panels,    // panels drawing
    ECSIProfileStep_Upload,    // copying panels' pixels to their textures
    ECSIProfileStep_Interface, // the core's own interface
    ECSIProfileStep_Present,   // presenting the OS windows
    ECSIProfileStep_Count,
} ECSIProfileStep;

/// @brief A plugin's code while it runs on the main thread, from ECSIProfile_Enter to ECSIProfile_Leave.
typedef struct ECSIProfileScope
{
    ECSPlugin plugin; // nullptr for the core's own functions
    const char *name; // the plugin's name, valid while its code runs
    u64 start;        // performance counter at entry
    u64 inner;        // counter ticks spent in scopes inside it
    bool active;      // measured: the profile was on, on the main thread
    struct ECSIProfileScope *outer;
} ECSIProfileScope;

#pragma endregion Types

#pragma region Declarations

/// @brief Turns measuring on or off. Off, the other functions do nothing, and the measures are dropped. Main thread only.
/// @param measure Whether to measure.
void ECSIProfile_SetOn(bool measure);

/// @brief Checks whether measuring is on.
/// @return true while ecs.profile is on.
bool ECSIProfile_IsOn(void);

/// @brief Moves the pass into a step, until the next call. Main thread only.
/// @param step The step that the pass enters.
/// @return The step it left, to return to with another call.
ECSIProfileStep ECSIProfile_SetStep(ECSIProfileStep step);

/// @brief Marks the start of a plugin's code, so its time is the plugin's. Other threads are not measured.
/// @param scope The scope, which lives until ECSIProfile_Leave.
/// @param plugin The plugin whose code runs, or nullptr for a function of the core.
/// @param name The plugin's name, or "ecs" for the core; valid until ECSIProfile_Leave.
void ECSIProfile_Enter(ECSIProfileScope *scope, ECSPlugin plugin, const char *name);

/// @brief Marks the end of the code that ECSIProfile_Enter started. Its time, less the time of scopes inside it, is its plugin's.
/// @param scope The scope.
void ECSIProfile_Leave(ECSIProfileScope *scope);

/// @brief Drops a plugin's measures, as it goes away and its handle may be used again. Main thread only.
/// @param plugin The plugin.
void ECSIProfile_ForgetPlugin(ECSPlugin plugin);

/// @brief Marks the start of a panel's Draw. Main thread only.
/// @return The performance counter, for ECSIProfile_EndDraw; 0 while measuring is off.
u64 ECSIProfile_BeginDraw(void);

/// @brief Adds a panel's Draw, and what the plugins did inside it, to the panel's time. Main thread only.
/// @param start What ECSIProfile_BeginDraw gave.
/// @param id The panel's id.
/// @param type The panel's type, kept the first time the panel is seen.
void ECSIProfile_EndDraw(u64 start, u32 id, const char *type);

/// @brief Ends a pass of the main loop: records its steps, and once a second sums the plugins' and panels' times. Main thread only.
void ECSIProfile_EndPass(void);

/// @brief Frees the measures.
void ECSIProfile_Terminate(void);

#pragma endregion Declarations
