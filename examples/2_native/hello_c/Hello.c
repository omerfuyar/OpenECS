// The smallest native plugin: one panel type that fills itself with one colour. 1_hello does the same in Lua.

#include "OpenECS.h" // the only header a plugin includes; it declares everything the core gives plugins
                     // types such as u32, f64 and SHUResult come from shu.h, which it includes

#include <stdlib.h>

/// @brief A panel's state: what Create gives, and what the other functions get.
typedef struct Greeting
{
    ECSPanel panel;
} Greeting;

// gets the new panel, the state a session saved for it (NULL for a new panel) and that state's version; gives back the panel's state
// SHUResult is the interface's result: SHUResult_Ok, or an error
static SHUResult GreetingCreate(ECSPanel panel, const ECSValue *savedState, u32 version, void **retState)
{
    (void)savedState;
    (void)version;

    Greeting *greeting = calloc(1, sizeof(Greeting));

    if (greeting == NULL)
    {
        return SHUResult_ErrAllocation; // an error shows on the panel instead of its drawing
    }

    greeting->panel = panel;
    *retState = greeting;
    return SHUResult_Ok;
}

// runs when the panel closes; frees what Create made
static void GreetingDestroy(void *state)
{
    free(state);
}

// the surface is valid only during this call; pixels are ARGB, one row every pitch bytes
static void GreetingDraw(void *state, ECSSurface *surface, f64 seconds)
{
    (void)state;
    (void)seconds;

    for (i32 y = 0; y < surface->height; y++)
    {
        u32 *row = (u32 *)((u8 *)surface->pixels.data + (usz)y * (usz)surface->pitch);

        for (i32 x = 0; x < surface->width; x++)
        {
            row[x] = 0xFF5E81AC;
        }
    }
}

// the core calls it once, after it loads the library; register everything here, not later
SHUResult ECSPlugin_Init(ECSPlugin plugin)
{
    ECS_Log(plugin, ECSLogLevel_Info, "Hello from C!");

    const ECSPanelTypeDesc greeting = {
        .name = "hello_c.greeting",
        .title = "Greeting in C",
        .Create = GreetingCreate,
        .Destroy = GreetingDestroy,
        .Draw = GreetingDraw,
    };

    return ECSPanelType_Register(plugin, &greeting); // an error fails the plugin, and its panels show the error
}
