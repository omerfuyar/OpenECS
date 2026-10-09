#include "app/App.h"
#include "app/Bindings.h"
#include "app/Session.h"
#include "app/Test.h"
#include "base/Log.h"
#include "base/Lua.h"
#include "base/Sanitizers.h"
#include "interface/Input.h"
#include "interface/Keys.h"
#include "interface/Layout.h"
#include "interface/Menus.h"
#include "interface/Panels.h"
#include "interface/Window.h"
#include "runtime/Events.h"
#include "runtime/Plugins.h"
#include "runtime/Services.h"
#include "runtime/Settings.h"

#include "SDL3/SDL.h"
#include "stb/stbSDL3.h"

#pragma region Source Only

/// @brief The core's settings file, relative to the executable.
#define OPENECS_CORE_SETTINGS_FILE "resources/settings.lua"

/// @brief The log file, in the state folder. Each start writes it anew.
#define OPENECS_LOG_FILE "openecs.log"
/// @brief Where the user's saved sessions go by default, in the data folder.
#define OPENECS_SESSIONS_FOLDER "sessions/"
/// @brief The user's presets, in the configuration folder.
#define OPENECS_PRESETS_FOLDER "presets/"

static struct
{
    ECSIPresetInfo preset; // identity and contents of the preset, or of the session that replaced it
    char *presetPath;
    char *sessionPath;   // NULL when OpenECS starts from the preset
    char *lastSession;   // where the session is saved on quit, or NULL if there is no state folder
    char *configFolder;  // NULL if there is none
    char *presetsFolder; // the user's presets, or NULL if there is no configuration folder
    char *stateFolder;   // NULL if there is none
    bool test;           // true when the program runs a test: no user files, no last session, nothing saved
} APP = {0};

/// @brief Finds an XDG base folder for OpenECS: $variable/openecs/, or ~/fallback/openecs/ if the variable is not set.
/// @return The folder, ending with a separator, or NULL if neither the variable nor HOME is set. Free it with SDL_free.
static char *ECSIApp_XdgFolder(const char *variable, const char *fallback)
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

/// @brief Removes everything a failed plugin registered, for the Plugins module.
static void ECSIApp_RemoveRegistrations(ECSPlugin plugin)
{
    ECSIPanels_RemovePlugin(plugin);
    ECSIServices_RemovePlugin(plugin);
    ECSISettings_RemovePlugin(plugin);
    ECSIEvents_StopTimersOfPlugin(plugin);
    ECSIEvents_RemovePlugin(plugin);
    ECSIKeys_RemovePlugin(plugin);
}

/// @brief Stops the program if a start-up step failed. The details are already in the log.
static void ECSIApp_CheckStart(SHUResult result, const char *step)
{
    if (!result)
    {
        return;
    }

    char *message = NULL;
    SDL_asprintf(&message, "Start-up failed while %s (%s). See the log for details.", step, SHUResult_String(result));
    SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "%s", message == NULL ? step : message);

    // a test runs without a display, so its failure goes to the log only
    if (!APP.test)
    {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "OpenECS", message == NULL ? step : message, NULL);
    }

    SDL_free(message);

    SDL_Quit();
    exit((int)result);
}

/// @brief Loads the plugins a preset names. Search order: the preset's directory, the user's plugins, the first-party plugins.
static void ECSIApp_LoadPlugins(const ECSIPresetInfo *preset)
{
    char *userData = APP.test ? NULL : SDL_GetPrefPath(NULL, "openecs");
    char *userPlugins = NULL;
    char *firstPartyPlugins = NULL;

    if (userData == NULL && !APP.test)
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

    if (SDL_asprintf(&userNeededBy, "the user's settings '%s'", ECSISettings_GetUserPath() != NULL ? ECSISettings_GetUserPath() : "") < 0)
    {
        userNeededBy = NULL;
    }

    // the preset's plugins first, then the extra plugins that the user's settings name
    SHUResult result = ECSIPlugins_Load(directories, directoryCount, ECSValue_GetTableField(preset->file, "depends"), neededBy != NULL ? neededBy : "the preset");
    SHUResult extraResult = ECSIPlugins_Load(directories, directoryCount, ECSISettings_GetPlugins(), userNeededBy != NULL ? userNeededBy : "the user's settings");

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

/// @brief Passes files to the function that the preset or session names in open, one call for each file.
static void ECSIApp_OpenFiles(char **files, usz count)
{
    const char *open = ECSValue_GetString(ECSValue_GetTableField(APP.preset.file, "open"), NULL);

    if (count > 0 && open == NULL)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "The preset names no function to open files with, so %zu files are not opened.", count);
        return;
    }

    for (usz i = 0; i < count; i++)
    {
        ECSIServices_CallOpen(open, files[i]);
    }
}

/// @brief Gives the shorter of two waits in milliseconds, where -1 means no wait.
static i32 ECSIApp_ShorterWait(i32 a, i32 b)
{
    return a < 0 || (b >= 0 && b < a) ? b : a;
}

#pragma endregion Source Only

void ECSIApp_Start(const ECSIArguments *arguments)
{
    SDL_assert(arguments != NULL);

    SDL_Log("OpenECS %s, plugin API %d.", OPENECS_VERSION, OPENECS_API_VERSION);

    // every path of the program's own files starts here
    ECSIApp_CheckStart(SDL_GetBasePath() == NULL ? SHUResult_ErrNotFound : SHUResult_Ok, "finding the program's folder");

    // read the preset first, because SDL needs the tool's identity before it starts
    ECSIApp_CheckStart(ECSILua_Initialize(), "starting Lua");
    ECSIApp_CheckStart(ECSIEvents_Initialize(), "preparing worker threads");
    ECSIApp_CheckStart(ECSIServices_Initialize(), "preparing services");

    // a test names its preset and starts from it alone, without the user's files and folders
    char *testPreset = NULL;
    APP.test = arguments->test != NULL;

    if (APP.test)
    {
        ECSIApp_CheckStart(ECSITest_Load(arguments->test, &APP.preset, &testPreset), "reading the test");
    }

    if (!APP.test)
    {
        APP.configFolder = ECSIApp_XdgFolder("XDG_CONFIG_HOME", ".config");
        APP.stateFolder = ECSIApp_XdgFolder("XDG_STATE_HOME", ".local/state");
    }

    if (APP.configFolder != NULL && SDL_asprintf(&APP.presetsFolder, "%s%s", APP.configFolder, OPENECS_PRESETS_FOLDER) < 0)
    {
        APP.presetsFolder = NULL;
    }

    ECSIApp_CheckStart(ECSISession_FindPreset(&APP.presetPath, APP.test ? testPreset : arguments->preset, APP.presetsFolder), "finding the preset");
    ECSIApp_CheckStart(ECSISession_ReadInfo(APP.presetPath, &APP.preset), "reading the preset");
    SDL_free(testPreset);

    // the tool's last session replaces the preset, unless the command line names a session or asks for a fresh start
    APP.lastSession = ECSISession_GetLastPath(APP.stateFolder, APP.preset.appId);

    // the lines logged so far went to standard error only
    char *logPath = NULL;

    if (APP.stateFolder != NULL && SDL_CreateDirectory(APP.stateFolder) && SDL_asprintf(&logPath, "%s%s", APP.stateFolder, OPENECS_LOG_FILE) >= 0)
    {
        ECSILog_OpenFile(logPath);
        SDL_free(logPath);
    }

    if (arguments->session != NULL && !APP.test)
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
        ECSISession_FreeInfo(&APP.preset);
        ECSIApp_CheckStart(ECSISession_ReadInfo(APP.sessionPath, &APP.preset), "reading the session");
        SDL_free(APP.lastSession);
        APP.lastSession = ECSISession_GetLastPath(APP.stateFolder, APP.preset.appId);
    }

    const char *sourcePath = APP.sessionPath != NULL ? APP.sessionPath : APP.presetPath;

    char *coreSettingsPath = NULL;
    ECSIApp_CheckStart(SDL_asprintf(&coreSettingsPath, "%s%s", SDL_GetBasePath(), OPENECS_CORE_SETTINGS_FILE) < 0 ? SHUResult_ErrAllocation : SHUResult_Ok, "finding the core's settings");
    ECSIApp_CheckStart(ECSISettings_Initialize(coreSettingsPath, ECSValue_GetTableField(APP.preset.file, "settings"), sourcePath, APP.preset.appId, APP.configFolder), "reading the settings");
    SDL_free(coreSettingsPath);

    SDL_SetAppMetadata(APP.preset.appName, NULL, APP.preset.appId);

    // a test needs no display; the environment variables still choose other drivers
    if (APP.test)
    {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
    }

    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL failed to start: %s", SDL_GetError());
        ECSIApp_CheckStart(SHUResult_ErrInternal, "starting SDL");
    }

    ECSIApp_CheckStart(ECSILayout_Initialize(), "declaring the layout settings");
    ECSIApp_CheckStart(ECSIWindow_Initialize(APP.preset.appName), "opening the window");
    ECSIApp_CheckStart(ECSIKeys_Initialize(), "declaring the key settings");
    ECSIApp_CheckStart(ECSIMenus_Initialize(), "registering the core's functions");
    ECSIApp_CheckStart(ECSIInput_Initialize(), "declaring the input settings");

    // a test has no data folder; its sessions field names the folder that stands for the saved sessions
    char *dataFolder = APP.test ? NULL : ECSIApp_XdgFolder("XDG_DATA_HOME", ".local/share");
    char *sessionsFolder = NULL;

    if (APP.test)
    {
        sessionsFolder = ECSITest_GetSessions() == NULL ? NULL : SDL_strdup(ECSITest_GetSessions());
    }
    else if (dataFolder != NULL && SDL_asprintf(&sessionsFolder, "%s%s", dataFolder, OPENECS_SESSIONS_FOLDER) < 0)
    {
        sessionsFolder = NULL;
    }

    ECSISessionFolders folders = {.presets = APP.presetsFolder, .sessions = sessionsFolder, .state = APP.stateFolder};
    ECSIApp_CheckStart(ECSISession_Initialize(&folders, !APP.test), "registering the session functions");
    SDL_free(dataFolder);
    SDL_free(sessionsFolder);

    ECSIBindings_Initialize();
    ECSIPluginHooks hooks = {.StartLua = ECSIBindings_StartPlugin, .RemoveRegistrations = ECSIApp_RemoveRegistrations};
    ECSIPlugins_SetHooks(&hooks);
    ECSIApp_LoadPlugins(&APP.preset);
    ECSIApp_CheckStart(ECSISession_Apply(sourcePath, &APP.preset), "building the layout");

    // misspelt names change nothing, so they are reported once everything is registered
    ECSISettings_ReportUndeclared();
    ECSIKeys_ReportUnknownFunctions();

    usz fileCount = arguments->fileCount;
    char **files = APP.test ? ECSITest_GetFiles(&fileCount) : arguments->files;
    ECSIApp_OpenFiles(files, fileCount);

    // drivers, plugins and system libraries are loaded now
    ECSISanitizers_KeepLibraries();
}

int ECSIApp_Run(void)
{
    // event-driven loop: it waits for input, the next timer, queued events, the next frame or the test's next step
    bool running = true;

    while (running)
    {
        SDL_Event event;
        i32 wait = ECSIApp_ShorterWait(ECSIEvents_GetWait(), ECSIWindow_GetFrameWait());
        wait = ECSIApp_ShorterWait(wait, ECSITest_GetWait());

        if (SDL_WaitEventTimeout(&event, wait))
        {
            do
            {
                running = ECSIInput_Handle(&event);
            } while (running && SDL_PollEvent(&event));
        }

        if (!running)
        {
            break;
        }

        ECSIEvents_RunTimers();
        ECSIEvents_Deliver();
        ECSISettings_DeliverChanges();
        ECSIPanels_DestroyClosed();

        // while a test runs, frames are not paced, so each step of the test sees a drawn window
        i32 frameWait = ECSIWindow_GetFrameWait();

        if (frameWait == 0 || (frameWait > 0 && ECSITest_IsRunning()))
        {
            ECSIWindow_Render(SDL_GetTicksNS());
        }

        // a session opened in this pass replaces the program once it stops
        running = ECSITest_Step() && ECSISession_GetNext(NULL) == NULL;
    }

    return ECSITest_GetStatus();
}

char *ECSIApp_Stop(const char **retOption)
{
    SDL_assert(retOption != NULL);

    ECSISanitizers_KeepLibraries();
    ECSITest_Terminate();

    if (APP.lastSession != NULL && ECSSession_Save(APP.lastSession) == SHUResult_Ok)
    {
        SDL_Log("Session saved to '%s'.", APP.lastSession);
    }

    // panels are destroyed before the renderer that made their textures and before their types, handles' objects before their plugins shut down, and plugins before they are unloaded
    ECSIInput_Terminate();
    ECSIMenus_Terminate();
    ECSIKeys_Terminate();
    ECSILayout_Terminate();
    ECSIWindow_Terminate();
    ECSIPanels_Terminate();
    ECSIServices_Terminate();

    // plugins shut down while timers and events still work, and their code is unloaded once no worker runs it
    ECSIPlugins_Shutdown();
    ECSIEvents_Terminate();
    ECSIPlugins_Unload();
    ECSISettings_Terminate();
    ECSIBindings_Terminate();
    char *next = ECSISession_GetNext(retOption) == NULL ? NULL : SDL_strdup(ECSISession_GetNext(NULL));
    ECSISession_Terminate();
    ECSISession_FreeInfo(&APP.preset);
    SDL_free(APP.presetPath);
    SDL_free(APP.sessionPath);
    SDL_free(APP.lastSession);
    SDL_free(APP.configFolder);
    SDL_free(APP.presetsFolder);
    SDL_free(APP.stateFolder);
    ECSILua_Terminate();

    // Debug builds check for leaks here, when OpenECS has freed its memory but SDL still holds its own
    ECSISanitizers_CheckLeaks();

    SDL_Quit();
    ECSILog_Terminate();
    return next;
}
