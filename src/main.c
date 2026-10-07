#include "Global.h"
#include "tools/Lua.h"
#include "tools/Platform.h"
#include "tools/Renderer.h"
#include "systems/Input.h"
#include "systems/Layout.h"
#include "systems/Plugins.h"
#include "systems/Session.h"

/// @brief Preset used when the command line names none.
#define OPENECS_DEFAULT_PRESET "default"

/// @brief Default core prefix: the default of the setting ecs.prefix.
#define OPENECS_DEFAULT_PREFIX "Alt+W"

/// @brief Font of the core's interface, relative to the executable.
#define OPENECS_FONT_FILE "resources/Roboto-Regular.ttf"

/// @brief What the command line asks for.
typedef struct ECSI_Arguments
{
    const char *preset;
} ECSI_Arguments;

static ECSI_Arguments ECSI_ReadArguments(int argc, char **argv)
{
    ECSI_Arguments arguments = {.preset = OPENECS_DEFAULT_PRESET};

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--preset") == 0 && i + 1 < argc)
        {
            arguments.preset = argv[++i];
        }
        else
        {
            SHU_LogWarning("Unknown argument '%s'. Usage: openecs [--preset NAME|FILE]", argv[i]);
        }
    }

    return arguments;
}

/// @brief Stops the program if a start-up step failed.
static void ECSI_CheckStart(SHUResult result, const char *step)
{
    if (result)
    {
        SHU_LogError(result, "Start-up failed: %s (%s).", step, SHUResult_String(result));
    }
}

int main(int argc, char **argv)
{
    ECSI_Arguments arguments = ECSI_ReadArguments(argc, argv);

    // read the preset first, because SDL needs the tool's identity before it starts
    ECSI_CheckStart(ECSI_LuaInitialize(), "starting Lua");

    char presetPath[OPENECS_PATH_CAPACITY];
    ECSI_SessionFindPreset(cs(presetPath, sizeof(presetPath)), arguments.preset);

    ECSI_PresetInfo preset;
    ECSI_CheckStart(ECSI_SessionReadInfo(presetPath, &preset), "reading the preset");
    ECSI_CheckStart(ECSI_PlatformInitialize(preset.appName, preset.appId), "starting SDL");

    char fontPath[OPENECS_PATH_CAPACITY];
    snprintf(fontPath, sizeof(fontPath), "%s%s", ECSI_PlatformGetBaseDirectory(), OPENECS_FONT_FILE);
    ECSI_CheckStart(ECSI_RendererInitialize(fontPath), "loading the font");
    ECSI_CheckStart(ECSI_LayoutInitialize(preset.appName), "opening the window");
    ECSI_CheckStart(ECSI_InputInitialize(OPENECS_DEFAULT_PREFIX), "reading the core keys");

    // plugin search order: the preset's directory, the user's plugins, the first-party plugins
    char userPlugins[OPENECS_PATH_CAPACITY];
    char firstPartyPlugins[OPENECS_PATH_CAPACITY];
    ECSI_PathUserData(cs(userPlugins, sizeof(userPlugins)), "openecs/plugins/");
    snprintf(firstPartyPlugins, sizeof(firstPartyPlugins), "%splugins/", ECSI_PlatformGetBaseDirectory());

    const char *directories[3];
    usz directoryCount = 0;

    if (preset.pluginsDirectory[0] != '\0')
    {
        directories[directoryCount++] = preset.pluginsDirectory;
    }

    directories[directoryCount++] = userPlugins;
    directories[directoryCount++] = firstPartyPlugins;

    const char *plugins[OPENECS_MAX_PLUGINS];

    for (usz i = 0; i < preset.pluginCount; i++)
    {
        plugins[i] = preset.plugins[i];
    }

    if (ECSI_PluginsLoad(directories, directoryCount, plugins, preset.pluginCount))
    {
        SHU_LogWarning("Some plugins failed to load; their panels are shown as placeholders.");
    }

    ECSI_CheckStart(ECSI_SessionApply(presetPath), "building the layout");

    // event-driven loop: it waits for events, unless a frame is needed
    bool running = true;

    while (running)
    {
        ECSI_PlatformEvent event;
        i32 timeoutMs = ECSI_LayoutWantsFrame() ? 0 : -1;

        while (running && ECSI_PlatformEventWait(&event, timeoutMs))
        {
            running = ECSI_InputHandle(&event);
            timeoutMs = 0;
        }

        if (running && ECSI_LayoutWantsFrame())
        {
            ECSI_LayoutRender(ECSI_PlatformGetTicks());
        }
    }

    // panels are destroyed before their plugins are unloaded
    ECSI_LayoutTerminate();
    ECSI_PluginsUnload();
    ECSI_RendererTerminate();
    ECSI_PlatformTerminate();
    ECSI_LuaTerminate();

    return 0;
}
