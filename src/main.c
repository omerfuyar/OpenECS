#include "Bindings.h"
#include "Events.h"
#include "Input.h"
#include "Layout.h"
#include "Lua.h"
#include "Panels.h"
#include "Plugins.h"
#include "Services.h"
#include "Session.h"
#include "Settings.h"

#include "SDL3/SDL.h"
#include "stb/stbSDL3.h"

/// @brief Preset used when the command line names none.
#define OPENECS_DEFAULT_PRESET "default"

/// @brief Font of the core's interface, relative to the executable.
#define OPENECS_FONT_FILE "resources/Roboto-Regular.ttf"

/// @brief A tool's last session, in its folder in the state folder.
#define OPENECS_LAST_SESSION_FILE "session.lua"
/// @brief The log file, in the state folder. Each start writes it anew.
#define OPENECS_LOG_FILE "openecs.log"

static struct
{
    SDL_IOStream *file; // NULL until the state folder is known
} LOG = {0};

/// @brief Names of the log levels, as log lines show them.
static const char *const ECSI_LOG_LEVELS[SDL_LOG_PRIORITY_COUNT] = {
    [SDL_LOG_PRIORITY_TRACE] = "trace",
    [SDL_LOG_PRIORITY_VERBOSE] = "verbose",
    [SDL_LOG_PRIORITY_DEBUG] = "debug",
    [SDL_LOG_PRIORITY_INFO] = "info",
    [SDL_LOG_PRIORITY_WARN] = "warning",
    [SDL_LOG_PRIORITY_ERROR] = "error",
    [SDL_LOG_PRIORITY_CRITICAL] = "critical",
};

/// @brief Writes a log line with the time, the level, the plugin and the message, to standard error and the log file. SDL calls it under its log lock, so any thread may log.
static void ECSI_LogOutput(void *userData, int category, SDL_LogPriority priority, const char *message)
{
    (void)userData;
    (void)category;

    SDL_Time now = 0;
    SDL_DateTime time = {0};

    if (SDL_GetCurrentTime(&now))
    {
        SDL_TimeToDateTime(now, &time, true);
    }

    // messages from ECS_Log start with the plugin's name in brackets; the core's own messages get "ecs"
    const char *level = priority > SDL_LOG_PRIORITY_INVALID && priority < SDL_LOG_PRIORITY_COUNT ? ECSI_LOG_LEVELS[priority] : "?";
    char *line = NULL;
    int length = SDL_asprintf(&line, "%02d:%02d:%02d.%03d %-8s %s%s\n", time.hour, time.minute, time.second, time.nanosecond / 1000000, level, message[0] == '[' ? "" : "[ecs] ", message);

    if (length < 0)
    {
        return;
    }

    fputs(line, stderr);

    if (LOG.file != NULL)
    {
        SDL_WriteIO(LOG.file, line, (usz)length);
        SDL_FlushIO(LOG.file);
    }

    SDL_free(line);
}

/// @brief What the command line asks for.
typedef struct ECSI_Arguments
{
    const char *preset;
    const char *session; // NULL if the command line names none
    bool fresh;
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
        else if (SDL_strcmp(argv[i], "--session") == 0 && i + 1 < argc)
        {
            arguments.session = argv[++i];
        }
        else if (SDL_strcmp(argv[i], "--fresh") == 0)
        {
            arguments.fresh = true;
        }
        else
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Unknown argument '%s'. Usage: openecs [--preset NAME|FILE] [--session FILE] [--fresh]", argv[i]);
        }
    }

    return arguments;
}

/// @brief Finds an XDG base folder for OpenECS: $variable/openecs/, or ~/fallback/openecs/ if the variable is not set.
/// @return The folder, ending with a separator, or NULL if neither the variable nor HOME is set. Free it with SDL_free.
static char *ECSI_XdgFolder(const char *variable, const char *fallback)
{
    const char *base = SDL_getenv(variable);
    const char *home = SDL_getenv("HOME");
    char *folder = NULL;

    if (base != NULL && base[0] != '\0')
    {
        SDL_asprintf(&folder, "%s/openecs/", base);
    }
    else if (home != NULL)
    {
        SDL_asprintf(&folder, "%s/%s/openecs/", home, fallback);
    }
    else
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Neither %s nor HOME is set; OpenECS does not use that folder.", variable);
    }

    return folder;
}

/// @brief Finds a tool's last session file in the state folder.
/// @return The path, or NULL if there is no state folder. Free it with SDL_free.
static char *ECSI_LastSessionPath(const char *stateFolder, const char *appId)
{
    char *path = NULL;

    if (stateFolder != NULL && SDL_asprintf(&path, "%s%s/%s", stateFolder, appId, OPENECS_LAST_SESSION_FILE) < 0)
    {
        path = NULL;
    }

    return path;
}

/// @brief Removes everything a failed plugin registered, for the Plugins module.
static void ECSI_RemoveRegistrations(ECSPlugin plugin)
{
    ECSI_PanelsRemovePlugin(plugin);
    ECSI_ServicesRemovePlugin(plugin);
    ECSI_SettingsRemovePlugin(plugin);
    ECSI_EventsStopTimersOfPlugin(plugin);
    ECSI_InputRemovePlugin(plugin);
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

    // the preset's plugins first, then the extra plugins that the user's settings name
    SHUResult result = ECSI_PluginsLoad(directories, directoryCount, ECSValue_GetField(preset->file, "depends"));
    SHUResult extraResult = ECSI_PluginsLoad(directories, directoryCount, ECSI_SettingsGetPlugins());

    if (result || extraResult)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Some plugins failed to load; their panels are shown as placeholders.");
    }

    SDL_free(userData);
    SDL_free(userPlugins);
    SDL_free(firstPartyPlugins);
}

int main(int argc, char **argv)
{
    SDL_SetLogOutputFunction(ECSI_LogOutput, NULL);
    ECSI_Arguments arguments = ECSI_ReadArguments(argc, argv);

    // every path of the program's own files starts here
    ECSI_CheckStart(SDL_GetBasePath() == NULL ? SHUResult_ErrNotFound : SHUResult_Ok, "finding the program's folder");

    // read the preset first, because SDL needs the tool's identity before it starts
    ECSI_CheckStart(ECSI_LuaInitialize(), "starting Lua");
    ECSI_CheckStart(ECSI_EventsInitialize(), "preparing worker threads");
    ECSI_CheckStart(ECSI_ServicesInitialize(), "preparing services");

    char *presetPath = NULL;
    ECSI_PresetInfo preset;
    ECSI_CheckStart(ECSI_SessionFindPreset(&presetPath, arguments.preset), "finding the preset");
    ECSI_CheckStart(ECSI_SessionReadInfo(presetPath, &preset), "reading the preset");

    // the tool's last session replaces the preset, unless the command line names a session or asks for a fresh start
    char *configFolder = ECSI_XdgFolder("XDG_CONFIG_HOME", ".config");
    char *stateFolder = ECSI_XdgFolder("XDG_STATE_HOME", ".local/state");
    char *lastSession = ECSI_LastSessionPath(stateFolder, preset.appId);
    char *logPath = NULL;

    // the lines logged so far went to standard error only
    if (stateFolder != NULL && SDL_CreateDirectory(stateFolder) && SDL_asprintf(&logPath, "%s%s", stateFolder, OPENECS_LOG_FILE) >= 0)
    {
        LOG.file = SDL_IOFromFile(logPath, "w");
        SDL_free(logPath);
    }
    char *sessionPath = NULL;

    if (arguments.session != NULL)
    {
        sessionPath = SDL_strdup(arguments.session);
    }
    else if (!arguments.fresh && lastSession != NULL && SDL_GetPathInfo(lastSession, NULL))
    {
        sessionPath = SDL_strdup(lastSession);
    }

    if (sessionPath != NULL)
    {
        // the session's identity wins; it may name another tool, whose last session it then replaces
        ECSI_SessionFreeInfo(&preset);
        ECSI_CheckStart(ECSI_SessionReadInfo(sessionPath, &preset), "reading the session");
        SDL_free(lastSession);
        lastSession = ECSI_LastSessionPath(stateFolder, preset.appId);
    }

    const char *sourcePath = sessionPath != NULL ? sessionPath : presetPath;

    ECSI_CheckStart(ECSI_SettingsInitialize(ECSValue_GetField(preset.file, "settings"), sourcePath, preset.appId, configFolder), "reading the settings");

    SDL_SetAppMetadata(preset.appName, NULL, preset.appId);

    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL failed to start: %s", SDL_GetError());
        ECSI_CheckStart(SHUResult_ErrInternal, "starting SDL");
    }

    char *fontPath = NULL;
    ECSI_CheckStart(SDL_asprintf(&fontPath, "%s%s", SDL_GetBasePath(), OPENECS_FONT_FILE) < 0 ? SHUResult_ErrAllocation : SHUResult_Ok, "finding the font");
    ECSI_CheckStart(ECSI_LayoutInitialize(preset.appName, fontPath), "opening the window");
    ECSI_CheckStart(ECSI_InputInitialize(), "declaring the input settings");
    SDL_free(fontPath);

    ECSI_BindingsInitialize();
    ECSI_PluginHooks hooks = {.StartLua = ECSI_BindingsStartPlugin, .RemoveRegistrations = ECSI_RemoveRegistrations};
    ECSI_PluginsSetHooks(&hooks);
    ECSI_LoadPlugins(&preset);
    ECSI_CheckStart(ECSI_SessionApply(sourcePath, &preset), "building the layout");

    // event-driven loop: it waits for input, the next timer, queued events or the next frame
    bool running = true;

    while (running)
    {
        SDL_Event event;

        i32 wait = ECSI_EventsGetWait();
        i32 frameWait = ECSI_LayoutGetFrameWait();

        if (frameWait >= 0 && (wait < 0 || frameWait < wait))
        {
            wait = frameWait;
        }

        if (SDL_WaitEventTimeout(&event, wait))
        {
            do
            {
                running = ECSI_InputHandle(&event);
            } while (running && SDL_PollEvent(&event));
        }

        if (!running)
        {
            break;
        }

        ECSI_EventsRunTimers();
        ECSI_EventsDeliver();
        ECSI_SettingsDeliverChanges();
        ECSI_PanelsDestroyClosed();

        if (ECSI_LayoutGetFrameWait() == 0)
        {
            ECSI_LayoutRender(SDL_GetTicksNS());
        }
    }

    if (lastSession != NULL && ECSI_SessionSave(lastSession, &preset) == SHUResult_Ok)
    {
        SDL_Log("Session saved to '%s'.", lastSession);
    }

    // panels are destroyed before their types, and their types before their plugins are unloaded
    ECSI_InputTerminate();
    ECSI_LayoutTerminate();
    ECSI_PanelsTerminate();
    ECSI_EventsTerminate();
    ECSI_ServicesTerminate();
    ECSI_PluginsUnload();
    ECSI_SettingsTerminate();
    ECSI_BindingsTerminate();
    ECSI_SessionFreeInfo(&preset);
    SDL_free(presetPath);
    SDL_free(sessionPath);
    SDL_free(lastSession);
    SDL_free(configFolder);
    SDL_free(stateFolder);
    SDL_Quit();

    if (LOG.file != NULL)
    {
        SDL_CloseIO(LOG.file);
        LOG.file = NULL;
    }
    ECSI_LuaTerminate();

    return 0;
}
