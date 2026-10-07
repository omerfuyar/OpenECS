#include "Input.h"
#include "Layout.h"
#include "Lua.h"
#include "Panels.h"
#include "Plugins.h"
#include "Session.h"

#include "SDL3/SDL.h"

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
        if (SDL_strcmp(argv[i], "--preset") == 0 && i + 1 < argc)
        {
            arguments.preset = argv[++i];
        }
        else
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Unknown argument '%s'. Usage: openecs [--preset NAME|FILE]", argv[i]);
        }
    }

    return arguments;
}

/// @brief Stops the program if a start-up step failed. The details are already in the log.
static void ECSI_CheckStart(SHUResult result, const char *step)
{
    if (!result)
    {
        return;
    }

    char *message = NULL;
    SDL_asprintf(&message, "Start-up failed while %s (%s). See the log for details.", step, SHUResult_String(result));
    SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "%s", message == NULL ? step : message);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "OpenECS", message == NULL ? step : message, NULL);
    SDL_free(message);

    SDL_Quit();
    exit((int)result);
}

/// @brief Loads the plugins a preset names. Search order: the preset's directory, the user's plugins, the first-party plugins.
static void ECSI_LoadPlugins(const ECSI_PresetInfo *preset)
{
    char *userData = SDL_GetPrefPath(NULL, "openecs");
    char *userPlugins = NULL;
    char *firstPartyPlugins = NULL;

    if (userData == NULL)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "The user's plugin directory is not available: %s", SDL_GetError());
    }
    else
    {
        SDL_asprintf(&userPlugins, "%splugins/", userData);
    }

    SDL_asprintf(&firstPartyPlugins, "%splugins/", SDL_GetBasePath());

    const char *directories[3];
    usz directoryCount = 0;
    const char *candidates[] = {preset->pluginsDirectory, userPlugins, firstPartyPlugins};

    for (usz i = 0; i < SDL_arraysize(candidates); i++)
    {
        if (candidates[i] != NULL)
        {
            directories[directoryCount++] = candidates[i];
        }
    }

    const char *plugins[OPENECS_MAX_PLUGINS];

    for (usz i = 0; i < preset->pluginCount; i++)
    {
        plugins[i] = preset->plugins[i];
    }

    if (ECSI_PluginsLoad(directories, directoryCount, plugins, preset->pluginCount))
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Some plugins failed to load; their panels are shown as placeholders.");
    }

    SDL_free(userData);
    SDL_free(userPlugins);
    SDL_free(firstPartyPlugins);
}

int main(int argc, char **argv)
{
    ECSI_Arguments arguments = ECSI_ReadArguments(argc, argv);

    // every path of the program's own files starts here
    ECSI_CheckStart(SDL_GetBasePath() == NULL ? SHUResult_ErrNotFound : SHUResult_Ok, "finding the program's folder");

    // read the preset first, because SDL needs the tool's identity before it starts
    ECSI_CheckStart(ECSI_LuaInitialize(), "starting Lua");

    char *presetPath = NULL;
    ECSI_PresetInfo preset;
    ECSI_CheckStart(ECSI_SessionFindPreset(&presetPath, arguments.preset), "finding the preset");
    ECSI_CheckStart(ECSI_SessionReadInfo(presetPath, &preset), "reading the preset");

    SDL_SetAppMetadata(preset.appName, NULL, preset.appId);

    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL failed to start: %s", SDL_GetError());
        ECSI_CheckStart(SHUResult_ErrInternal, "starting SDL");
    }

    char *fontPath = NULL;
    ECSI_CheckStart(SDL_asprintf(&fontPath, "%s%s", SDL_GetBasePath(), OPENECS_FONT_FILE) < 0 ? SHUResult_ErrAllocation : SHUResult_Ok, "finding the font");
    ECSI_CheckStart(ECSI_LayoutInitialize(preset.appName, fontPath), "opening the window");
    ECSI_CheckStart(ECSI_InputInitialize(OPENECS_DEFAULT_PREFIX), "reading the core keys");
    SDL_free(fontPath);

    ECSI_LoadPlugins(&preset);
    ECSI_CheckStart(ECSI_SessionApply(presetPath), "building the layout");

    // event-driven loop: it waits for events, unless a frame is needed
    bool running = true;

    while (running)
    {
        SDL_Event event;

        if (SDL_WaitEventTimeout(&event, ECSI_LayoutWantsFrame() ? 0 : -1))
        {
            do
            {
                running = ECSI_InputHandle(&event);
            } while (running && SDL_PollEvent(&event));
        }

        if (running && ECSI_LayoutWantsFrame())
        {
            ECSI_LayoutRender(SDL_GetTicksNS());
        }
    }

    // panels are destroyed before their types, and their types before their plugins are unloaded
    ECSI_LayoutTerminate();
    ECSI_PanelsTerminate();
    ECSI_PluginsUnload();
    ECSI_SessionFreeInfo(&preset);
    SDL_free(presetPath);
    SDL_Quit();
    ECSI_LuaTerminate();

    return 0;
}
