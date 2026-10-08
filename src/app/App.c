#include "app/App.h"
#include "app/Bindings.h"
#include "app/Session.h"
#include "base/Log.h"
#include "base/Lua.h"
#include "base/Sanitizers.h"
#include "interface/Input.h"
#include "interface/Layout.h"
#include "interface/Panels.h"
#include "runtime/Events.h"
#include "runtime/Plugins.h"
#include "runtime/Services.h"
#include "runtime/Settings.h"

#include "SDL3/SDL.h"
#include "stb/stbSDL3.h"

#pragma region Source Only

/// @brief Font of the core's interface, relative to the executable.
#define OPENECS_FONT_FILE "resources/Roboto-Regular.ttf"

/// @brief A tool's last session, in its folder in the state folder.
#define OPENECS_LAST_SESSION_FILE "session.lua"
/// @brief The log file, in the state folder. Each start writes it anew.
#define OPENECS_LOG_FILE "openecs.log"

static struct
{
    ECSI_PresetInfo preset; // identity and contents of the preset, or of the session that replaced it
    char *presetPath;
    char *sessionPath;  // NULL when OpenECS starts from the preset
    char *lastSession;  // where the session is saved on quit, or NULL if there is no state folder
    char *configFolder; // NULL if there is none
    char *stateFolder;  // NULL if there is none
} APP = {0};

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
    ECSI_EventsRemovePlugin(plugin);
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

    // reports name the file that asks for a plugin
    bool lastSession = APP.sessionPath != NULL && APP.lastSession != NULL && SDL_strcmp(APP.sessionPath, APP.lastSession) == 0;
    char *neededBy = NULL;
    char *userNeededBy = NULL;

    if (SDL_asprintf(&neededBy, "%s '%s'", lastSession ? "the tool's last session" : APP.sessionPath != NULL ? "the session"
                                                                                                             : "the preset",
                     APP.sessionPath != NULL ? APP.sessionPath : APP.presetPath) < 0)
    {
        neededBy = NULL;
    }

    if (SDL_asprintf(&userNeededBy, "the user's settings '%s'", ECSI_SettingsGetUserPath() != NULL ? ECSI_SettingsGetUserPath() : "") < 0)
    {
        userNeededBy = NULL;
    }

    // the preset's plugins first, then the extra plugins that the user's settings name
    SHUResult result = ECSI_PluginsLoad(directories, directoryCount, ECSValue_GetTableField(preset->file, "depends"), neededBy != NULL ? neededBy : "the preset");
    SHUResult extraResult = ECSI_PluginsLoad(directories, directoryCount, ECSI_SettingsGetPlugins(), userNeededBy != NULL ? userNeededBy : "the user's settings");

    if (result || extraResult)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Some plugins failed to load; their panels are shown as placeholders.");
    }

    // a last session keeps the plugins it was saved with, even when the preset has changed since
    if (result && lastSession)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "The last session keeps the plugins it was saved with. Start with --fresh to use the preset '%s' instead.", APP.presetPath);
    }

    SDL_free(neededBy);
    SDL_free(userNeededBy);

    SDL_free(userData);
    SDL_free(userPlugins);
    SDL_free(firstPartyPlugins);
}

#pragma endregion Source Only

void ECSI_AppStart(const ECSI_Arguments *arguments)
{
    SDL_assert(arguments != NULL);

    // every path of the program's own files starts here
    ECSI_CheckStart(SDL_GetBasePath() == NULL ? SHUResult_ErrNotFound : SHUResult_Ok, "finding the program's folder");

    // read the preset first, because SDL needs the tool's identity before it starts
    ECSI_CheckStart(ECSI_LuaInitialize(), "starting Lua");
    ECSI_CheckStart(ECSI_EventsInitialize(), "preparing worker threads");
    ECSI_CheckStart(ECSI_ServicesInitialize(), "preparing services");

    ECSI_CheckStart(ECSI_SessionFindPreset(&APP.presetPath, arguments->preset), "finding the preset");
    ECSI_CheckStart(ECSI_SessionReadInfo(APP.presetPath, &APP.preset), "reading the preset");

    // the tool's last session replaces the preset, unless the command line names a session or asks for a fresh start
    APP.configFolder = ECSI_XdgFolder("XDG_CONFIG_HOME", ".config");
    APP.stateFolder = ECSI_XdgFolder("XDG_STATE_HOME", ".local/state");
    APP.lastSession = ECSI_LastSessionPath(APP.stateFolder, APP.preset.appId);

    // the lines logged so far went to standard error only
    char *logPath = NULL;

    if (APP.stateFolder != NULL && SDL_CreateDirectory(APP.stateFolder) && SDL_asprintf(&logPath, "%s%s", APP.stateFolder, OPENECS_LOG_FILE) >= 0)
    {
        ECSI_LogOpenFile(logPath);
        SDL_free(logPath);
    }

    if (arguments->session != NULL)
    {
        APP.sessionPath = SDL_strdup(arguments->session);
    }
    else if (!arguments->fresh && APP.lastSession != NULL && SDL_GetPathInfo(APP.lastSession, NULL))
    {
        APP.sessionPath = SDL_strdup(APP.lastSession);
    }

    if (APP.sessionPath != NULL)
    {
        // the session's identity wins; it may name another tool, whose last session it then replaces
        ECSI_SessionFreeInfo(&APP.preset);
        ECSI_CheckStart(ECSI_SessionReadInfo(APP.sessionPath, &APP.preset), "reading the session");
        SDL_free(APP.lastSession);
        APP.lastSession = ECSI_LastSessionPath(APP.stateFolder, APP.preset.appId);
    }

    const char *sourcePath = APP.sessionPath != NULL ? APP.sessionPath : APP.presetPath;

    ECSI_CheckStart(ECSI_SettingsInitialize(ECSValue_GetTableField(APP.preset.file, "settings"), sourcePath, APP.preset.appId, APP.configFolder), "reading the settings");

    SDL_SetAppMetadata(APP.preset.appName, NULL, APP.preset.appId);

    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL failed to start: %s", SDL_GetError());
        ECSI_CheckStart(SHUResult_ErrInternal, "starting SDL");
    }

    char *fontPath = NULL;
    ECSI_CheckStart(SDL_asprintf(&fontPath, "%s%s", SDL_GetBasePath(), OPENECS_FONT_FILE) < 0 ? SHUResult_ErrAllocation : SHUResult_Ok, "finding the font");
    ECSI_CheckStart(ECSI_LayoutInitialize(APP.preset.appName, fontPath), "opening the window");
    ECSI_CheckStart(ECSI_InputInitialize(), "declaring the input settings");
    SDL_free(fontPath);

    ECSI_BindingsInitialize();
    ECSI_PluginHooks hooks = {.StartLua = ECSI_BindingsStartPlugin, .RemoveRegistrations = ECSI_RemoveRegistrations};
    ECSI_PluginsSetHooks(&hooks);
    ECSI_LoadPlugins(&APP.preset);
    ECSI_CheckStart(ECSI_SessionApply(sourcePath, &APP.preset), "building the layout");

    // drivers, plugins and system libraries are loaded now
    ECSI_SanitizersKeepLibraries();
}

void ECSI_AppRun(void)
{
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
}

void ECSI_AppStop(void)
{
    ECSI_SanitizersKeepLibraries();

    if (APP.lastSession != NULL && ECSI_SessionSave(APP.lastSession, &APP.preset) == SHUResult_Ok)
    {
        SDL_Log("Session saved to '%s'.", APP.lastSession);
    }

    // panels are destroyed before their types, handles' objects before their plugins shut down, and plugins before they are unloaded
    ECSI_InputTerminate();
    ECSI_LayoutTerminate();
    ECSI_PanelsTerminate();
    ECSI_ServicesTerminate();

    // plugins shut down while timers and events still work, and their code is unloaded once no worker runs it
    ECSI_PluginsShutdown();
    ECSI_EventsTerminate();
    ECSI_PluginsUnload();
    ECSI_SettingsTerminate();
    ECSI_BindingsTerminate();
    ECSI_SessionFreeInfo(&APP.preset);
    SDL_free(APP.presetPath);
    SDL_free(APP.sessionPath);
    SDL_free(APP.lastSession);
    SDL_free(APP.configFolder);
    SDL_free(APP.stateFolder);
    ECSI_LuaTerminate();

    // Debug builds check for leaks here, when OpenECS has freed its memory but SDL still holds its own
    ECSI_SanitizersCheckLeaks();

    SDL_Quit();
    ECSI_LogTerminate();
}
