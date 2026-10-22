#include "app/App.h"
#include "app/Bindings.h"
#include "app/Session.h"
#include "app/Test.h"
#include "app/Workers.h"
#include "base/Log.h"
#include "base/Lua.h"
#include "base/Sanitizers.h"
#include "interface/Input.h"
#include "interface/Keys.h"
#include "interface/Layout.h"
#include "interface/Menus.h"
#include "interface/Panels.h"
#include "interface/Popups.h"
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
#define OPENECS_LOG_FILE "OpenECS.log"

/// @brief The setting that keeps the tool's session between runs.
#define OPENECS_KEEP_SESSION "ecs.keepSession"

/// @brief Where the user's saved sessions go by default, in the data folder.
#define OPENECS_SESSIONS_FOLDER "sessions/"
/// @brief The user's presets, in the configuration folder.
#define OPENECS_PRESETS_FOLDER "presets/"

static struct
{
    ECSIPresetInfo preset; // identity and contents of the preset, or of the session that replaced it
    char *presetPath;
    char *sessionPath;   // nullptr when OpenECS starts from the preset
    char *lastSession;   // where the session is saved on quit, or nullptr if there is no state folder
    char *configFolder;  // nullptr if there is none
    char *presetsFolder; // the user's presets, or nullptr if there is no configuration folder
    char *stateFolder;   // nullptr if there is none
    char *dataFolder;    // the user's plugins and saved sessions are in it; nullptr if there is none, as in a test
    bool test;           // true when the program runs a test: no user files, no last session, nothing saved
    bool definitions;    // true when the program only writes definition files: no display, nothing opened or saved
} APP = {0};

/// @brief Finds an XDG base folder for OpenECS: $variable/OpenECS/, or ~/fallback/OpenECS/ if the variable is not set.
/// On Windows, every folder is SDL's pref path, %APPDATA%\OpenECS\ (DESIGN 16).
/// @return The folder, ending with a separator, or nullptr if neither the variable nor HOME is set. Free it with SDL_free.
static char *ECSIApp_FindXdgFolder(const char *variable, const char *fallback)
{
#if defined(SDL_PLATFORM_WINDOWS)
    (void)fallback;
    char *prefPath = SDL_GetPrefPath(nullptr, "OpenECS");

    if (prefPath == nullptr)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "OpenECS has no folder for %s: %s", variable, SDL_GetError());
    }

    return prefPath;
#else
    const char *base = SDL_getenv(variable);
    const char *home = SDL_getenv("HOME");
    char *folder = nullptr;

    if (base != nullptr && base[0] != '\0')
    {
        SDL_asprintf(&folder, "%s/OpenECS/", base);
    }
    else if (home != nullptr)
    {
        SDL_asprintf(&folder, "%s/%s/OpenECS/", home, fallback);
    }
    else
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Neither %s nor HOME is set; OpenECS does not use that folder.", variable);
    }

    return folder;
#endif
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

/// @brief Stops the program if a start-up step failed. The dialog names the step, its file, the last error and where the log is.
/// @param file The file the step reads, or nullptr.
static void ECSIApp_ExitOnFailure(SHUResult result, const char *step, const char *file)
{
    if (!result)
    {
        return;
    }

    const char *error = ECSILog_GetLastError();
    const char *log = ECSILog_GetPath();
    char *message = nullptr;
    SDL_asprintf(&message, "Start-up failed while %s%s%s%s (%s).\n\n%s%s%s%s", step, file != nullptr ? " '" : "", file != nullptr ? file : "", file != nullptr ? "'" : "",
                 SHUResult_String(result), error != nullptr ? error : "", error != nullptr ? "\n\n" : "", log != nullptr ? "The log is in " : "The log went to standard error.", log != nullptr ? log : "");
    SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "%s", message == nullptr ? step : message);

    // a test runs without a display, so its failure goes to the log only
    if (!APP.test)
    {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "OpenECS", message == nullptr ? step : message, nullptr);
    }

    SDL_free(message);

    SDL_Quit();
    exit((int)result);
}

/// @brief Lists every plugin in some folders of plugins: each folder in them that holds a manifest.
static SHUResult ECSIApp_FindPlugins(const char *const *folders, usz folderCount, ECSValue *retList)
{
    ECSValue_SetTable(retList);

    for (usz i = 0; i < folderCount; i++)
    {
        int count = 0;
        char **manifests = SDL_GlobDirectory(folders[i], "*/manifest.lua", 0, &count);

        for (int j = 0; j < count; j++)
        {
            // the folder's name is the plugin's: "ui/manifest.lua" is the plugin ui
            const char *slash = manifests[j];

            while (*slash != '\0' && !ECSPath_IsSeparator(*slash))
            {
                slash++;
            }

            slash = *slash == '\0' ? nullptr : slash;
            char *name = slash == nullptr ? nullptr : SDL_strndup(manifests[j], (usz)(slash - manifests[j]));
            ECSValue *item = nullptr;
            SHUResult result = name == nullptr ? SHUResult_ErrAllocation : ECSValue_AddListItem(retList, &item);
            result = result ? result : ECSValue_SetString(item, name);
            SDL_free(name);
            SHU_ReturnResult(result, SDL_free(manifests););
        }

        SDL_free(manifests);
    }

    return SHUResult_Ok;
}

/// @brief Loads the plugins a preset names. Search order: the preset's plugins folder, the user's plugins, the first-party plugins.
static void ECSIApp_LoadPlugins(const ECSIPresetInfo *preset)
{
    char *userPlugins = nullptr;
    char *firstPartyPlugins = nullptr;

    if (APP.dataFolder != nullptr && SDL_asprintf(&userPlugins, "%splugins/", APP.dataFolder) < 0)
    {
        userPlugins = nullptr;
    }

    if (SDL_asprintf(&firstPartyPlugins, "%splugins/", SDL_GetBasePath()) < 0)
    {
        firstPartyPlugins = nullptr;
    }

    const char *folders[3];
    usz folderCount = 0;
    const char *candidates[] = {preset->pluginsFolder, userPlugins, firstPartyPlugins};

    for (usz i = 0; i < SDL_arraysize(candidates); i++)
    {
        if (candidates[i] != nullptr)
        {
            folders[folderCount++] = candidates[i];
        }
    }

    // reports name the file that asks for a plugin
    bool lastSession = APP.sessionPath != nullptr && APP.lastSession != nullptr && SDL_strcmp(APP.sessionPath, APP.lastSession) == 0;
    char *neededBy = nullptr;

    if (SDL_asprintf(&neededBy, "%s '%s'", lastSession ? "the tool's last session" : APP.sessionPath != nullptr ? "the session"
                                                                                                                : "the preset",
                     APP.sessionPath != nullptr ? APP.sessionPath : APP.presetPath) < 0)
    {
        neededBy = nullptr;
    }

    // the preset's plugins first, then the plugins that the settings files name for every tool; a test loads only what its preset names, and writing definitions loads every plugin it finds
    SHUResult result = ECSIPlugins_Load(folders, folderCount, ECSValue_GetTableField(preset->file, "depends"), neededBy != nullptr ? neededBy : "the preset");
    SHUResult extraResult = APP.test ? SHUResult_Ok : ECSIPlugins_Load(folders, folderCount, ECSISettings_GetPlugins(), "the settings files");

    if (APP.definitions)
    {
        ECSValue *every = nullptr;
        SHUResult found = ECSValue_Create(&every);
        found = found ? found : ECSIApp_FindPlugins(folders, folderCount, every);
        SHUResult everyResult = found ? found : ECSIPlugins_Load(folders, folderCount, every, "writing the definitions");
        extraResult = extraResult ? extraResult : everyResult;
        ECSValue_Destroy(&every);
    }

    if (result == SHUResult_Err || extraResult == SHUResult_Err)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Some plugins failed to load; their panels are shown as placeholders.");
    }

    // a last session keeps the plugins it was saved with, even when the preset has changed since
    if (result && lastSession)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "The last session keeps the plugins it was saved with. Start with --fresh to use the preset '%s' instead.", APP.presetPath);
    }

    SDL_free(neededBy);
    SDL_free(userPlugins);
    SDL_free(firstPartyPlugins);

    // a manifest that is not valid, or a cycle of dependencies, is a mistake in the tool's files, so it stops the start; a missing or failed plugin only loses its panels
    ECSIApp_ExitOnFailure(result == SHUResult_ErrBadData || extraResult == SHUResult_ErrBadData ? SHUResult_ErrBadData : SHUResult_Ok, "loading the plugins", nullptr);
}

/// @brief Passes files to the function that the preset or session names in open, one call for each file.
static void ECSIApp_OpenFiles(char **files, usz count)
{
    const char *open = ECSValue_GetString(ECSValue_GetTableField(APP.preset.file, "open"), nullptr);

    if (count > 0 && open == nullptr)
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

/// @brief Starts from a session in place of the preset: the session's identity wins, so it may name another tool.
static void ECSIApp_UseSession(const char *path)
{
    APP.sessionPath = SDL_strdup(path);
    ECSISession_FreeInfo(&APP.preset);
    ECSIApp_ExitOnFailure(APP.sessionPath == nullptr ? SHUResult_ErrAllocation : ECSISession_ReadInfo(APP.sessionPath, &APP.preset), "reading the session", path);
}

/// @brief Writes the definition files of a plugin into the folder that the data names.
static SHUResult ECSIApp_WritePluginDefinitions(ECSPlugin plugin, void *data)
{
    return ECSIServices_WriteDefinitions(plugin, data);
}

#pragma endregion Source Only

void ECSApp_Quit(void)
{
    // the main loop asks about unsaved work, as when the system or the main window's close button asks to quit
    SDL_Event event = {.type = SDL_EVENT_QUIT};
    SDL_PushEvent(&event);
}

void ECSIApp_Start(const ECSIArguments *arguments)
{
    SDL_assert(arguments != nullptr);

    SDL_Log("OpenECS %s, plugin API %d.", OPENECS_VERSION, OPENECS_API_VERSION);

    // every path of the program's own files starts here
    ECSIApp_ExitOnFailure(SDL_GetBasePath() == nullptr ? SHUResult_ErrNotFound : SHUResult_Ok, "finding the program's folder", nullptr);

    // read the preset first, because SDL needs the tool's identity before it starts
    ECSIApp_ExitOnFailure(ECSILua_Initialize(), "starting Lua", nullptr);
    ECSIApp_ExitOnFailure(ECSIEvents_Initialize(), "preparing worker threads", nullptr);
    ECSIApp_ExitOnFailure(ECSIServices_Initialize(), "preparing services", nullptr);
    ECSIApp_ExitOnFailure(ECSIServices_RegisterCore("ecs.app.quit", (ECSFunction)ECSApp_Quit, "void()", "Quit"), "registering the core's functions", nullptr);

    // a test names its preset and starts from it alone, without the user's files and folders
    char *testPreset = nullptr;
    APP.test = arguments->test != nullptr;
    APP.definitions = arguments->definitions != nullptr;

    if (APP.test)
    {
        ECSIApp_ExitOnFailure(ECSITest_Load(arguments->test, &testPreset), "reading the test", arguments->test);
    }

    // a test has no folders of the user
    if (!APP.test)
    {
        APP.configFolder = ECSIApp_FindXdgFolder("XDG_CONFIG_HOME", ".config");
        APP.stateFolder = ECSIApp_FindXdgFolder("XDG_STATE_HOME", ".local/state");
        APP.dataFolder = ECSIApp_FindXdgFolder("XDG_DATA_HOME", ".local/share");
    }

    // the lines logged so far went to standard error only; from here, a failed start can name the log file
    char *logPath = nullptr;

    if (APP.stateFolder != nullptr && SDL_CreateDirectory(APP.stateFolder) && SDL_asprintf(&logPath, "%s%s", APP.stateFolder, OPENECS_LOG_FILE) >= 0)
    {
        ECSILog_OpenFile(logPath);
        SDL_free(logPath);
    }

    // a test's presets field names the folder that stands for the user's presets
    if (APP.test)
    {
        APP.presetsFolder = ECSITest_GetPresetsFolder() == nullptr ? nullptr : SDL_strdup(ECSITest_GetPresetsFolder());
    }
    else if (APP.configFolder != nullptr && SDL_asprintf(&APP.presetsFolder, "%s%s", APP.configFolder, OPENECS_PRESETS_FOLDER) < 0)
    {
        APP.presetsFolder = nullptr;
    }

    ECSIApp_ExitOnFailure(ECSISession_FindPreset(&APP.presetPath, APP.test ? testPreset : arguments->preset, APP.presetsFolder), "finding the preset", APP.test ? testPreset : arguments->preset);
    ECSIApp_ExitOnFailure(ECSISession_ReadInfo(APP.presetPath, &APP.preset), "reading the preset", APP.presetPath);
    SDL_free(testPreset);

    // a session on the command line replaces the preset; its identity wins, so it may name another tool
    if (arguments->session != nullptr && !APP.test)
    {
        ECSIApp_UseSession(arguments->session);
    }

    char *coreSettingsPath = nullptr;
    ECSIApp_ExitOnFailure(SDL_asprintf(&coreSettingsPath, "%s%s", SDL_GetBasePath(), OPENECS_CORE_SETTINGS_FILE) < 0 ? SHUResult_ErrAllocation : SHUResult_Ok, "finding the core's settings", nullptr);
    ECSIApp_ExitOnFailure(ECSISettings_Initialize(coreSettingsPath, ECSValue_GetTableField(APP.preset.file, "settings"), APP.sessionPath != nullptr ? APP.sessionPath : APP.presetPath, APP.preset.appID, APP.configFolder), "reading the settings", nullptr);
    SDL_free(coreSettingsPath);

    // with ecs.keepSession, the tool's last session replaces the preset, unless the command line asks for a fresh start
    APP.lastSession = ECSISession_GetLastPath(APP.stateFolder, APP.preset.appID);

    if (APP.sessionPath == nullptr && !arguments->fresh && ECSISettings_PeekBool(OPENECS_KEEP_SESSION, false) && APP.lastSession != nullptr && SDL_GetPathInfo(APP.lastSession, nullptr))
    {
        ECSIApp_UseSession(APP.lastSession);
        ECSIApp_ExitOnFailure(ECSISettings_SetPreset(ECSValue_GetTableField(APP.preset.file, "settings"), APP.sessionPath), "reading the session's settings", APP.sessionPath);
    }

    ECSSettingDesc keepSession = {.name = OPENECS_KEEP_SESSION, .type = ECSSettingType_Bool, .description = "Save the tool's session when it quits, and start from it the next time"};
    ECSIApp_ExitOnFailure(ECSISettings_DeclareCore(&keepSession), "declaring the session settings", nullptr);
    const char *sourcePath = APP.sessionPath != nullptr ? APP.sessionPath : APP.presetPath;

    SDL_SetAppMetadata(APP.preset.appName, nullptr, APP.preset.appID);

    // a test, and writing definitions, need no display and make no sound; the environment variables still choose other drivers
    if (APP.test || APP.definitions)
    {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
        SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    }

    // the core quits when the main window closes, after asking about unsaved work; SDL's own quit for the last window would come with the close and answer that question
    SDL_SetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE, "0");

    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL failed to start: %s", SDL_GetError());
        ECSIApp_ExitOnFailure(SHUResult_ErrInternal, "starting SDL", nullptr);
    }

    // the system's theme is known once SDL has started
    ECSISettings_ApplyTheme();
    ECSIApp_ExitOnFailure(ECSILayout_Initialize(), "declaring the layout settings", nullptr);
    ECSIApp_ExitOnFailure(ECSIWindow_Initialize(APP.preset.appName), "opening the window", nullptr);
    ECSIWindow_SetHint(ECSValue_GetString(ECSValue_GetTableField(APP.preset.file, "hint"), nullptr));
    ECSIApp_ExitOnFailure(ECSIKeys_Initialize(), "reading the keys", nullptr);
    ECSIApp_ExitOnFailure(ECSIMenus_Initialize(), "registering the core's functions", nullptr);
    ECSIApp_ExitOnFailure(ECSIInput_Initialize(), "declaring the input settings", nullptr);

    // a test's sessions field names the folder that stands for the saved sessions
    char *sessionsFolder = nullptr;

    if (APP.test)
    {
        sessionsFolder = ECSITest_GetSessionsFolder() == nullptr ? nullptr : SDL_strdup(ECSITest_GetSessionsFolder());
    }
    else if (APP.dataFolder != nullptr && SDL_asprintf(&sessionsFolder, "%s%s", APP.dataFolder, OPENECS_SESSIONS_FOLDER) < 0)
    {
        sessionsFolder = nullptr;
    }

    // the layout is reset from the preset, or from the session the command line named
    const char *origin = arguments->session != nullptr && !APP.test ? arguments->session : APP.presetPath;
    ECSISessionFolders folders = {.presets = APP.presetsFolder, .sessions = sessionsFolder, .state = APP.stateFolder, .origin = origin};
    ECSIApp_ExitOnFailure(ECSISession_Initialize(&folders, !APP.test), "registering the session functions", nullptr);
    SDL_free(sessionsFolder);

    ECSIBindings_Initialize();
    ECSIWorkers_Initialize();
    ECSIPluginHooks hooks = {.StartLua = ECSIBindings_StartPlugin, .RemoveRegistrations = ECSIApp_RemoveRegistrations};
    ECSIPlugins_SetHooks(&hooks);

    ECSIApp_LoadPlugins(&APP.preset);
    ECSIApp_ExitOnFailure(ECSISession_Apply(sourcePath, &APP.preset), "building the layout", nullptr);

    // misspelt names change nothing, so they are reported once everything is registered
    ECSISettings_ReportUndeclared();
    ECSIKeys_ReportUnknownFunctions();

    usz fileCount = APP.definitions ? 0 : arguments->fileCount;
    char **files = APP.test ? ECSITest_GetFiles(&fileCount) : arguments->files;
    ECSIApp_OpenFiles(files, fileCount);
}

SHUResult ECSIApp_WriteDefinitions(const char *folder)
{
    SDL_assert(folder != nullptr);

    char *path = nullptr;
    usz length = SDL_strlen(folder);

    if (SDL_asprintf(&path, "%s%s", folder, length > 0 && ECSPath_IsSeparator(folder[length - 1]) ? "" : "/") < 0)
    {
        return SHUResult_ErrAllocation;
    }

    if (!SDL_CreateDirectory(path))
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot make the folder '%s': %s", path, SDL_GetError());
        SDL_free(path);
        return SHUResult_ErrFile;
    }

    SHUResult result = ECSIPlugins_ForEach(ECSIApp_WritePluginDefinitions, path);

    SDL_Log("Definition files written to '%s'.", path);
    SDL_free(path);
    return result;
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

        // popups of closed or hidden panels close, and their Closed functions run before the panels are destroyed
        ECSIPopups_CloseHidden();
        ECSIEvents_Deliver();
        ECSIPanels_DestroyClosed();
        ECSIPopups_DestroyClosed();

        // closing the last panel quits, as nothing is left to show
        if (ECSILayout_WasEmptied())
        {
            SDL_Log("The last panel is closed, so OpenECS quits.");
            break;
        }

        ECSIInput_UpdateTextInput();

        // while a test runs, frames are not paced, so each step of the test sees a drawn window
        i32 frameWait = ECSIWindow_GetFrameWait();

        if (frameWait == 0 || (frameWait > 0 && ECSITest_IsRunning()))
        {
            ECSIWindow_Render(SDL_GetTicksNS());
        }

        // a session opened in this pass replaces the program once it stops
        running = ECSITest_Step() && ECSISession_GetNext() == nullptr;
    }

    return ECSITest_GetStatus();
}

ECSIRestart ECSIApp_Stop(void)
{
    ECSITest_Terminate();

    // the setting in effect when the tool quits decides; a tool whose panels were all closed starts from its preset the next time
    bool keep = APP.lastSession != nullptr && !APP.definitions && ECSValue_GetBool(ECSSetting_Get(OPENECS_KEEP_SESSION), false);

    if (keep && ECSILayout_WasEmptied())
    {
        SDL_RemovePath(APP.lastSession);
    }
    else if (keep && ECSSession_Save(APP.lastSession) == SHUResult_Ok)
    {
        SDL_Log("Session saved to '%s'.", APP.lastSession);
    }

    // popups close before their panels; panels are destroyed before the renderer that made their textures and before their types, handles' objects before their plugins shut down, and plugins before they are unloaded
    ECSIInput_Terminate();
    ECSIMenus_Terminate();
    ECSIKeys_Terminate();
    ECSIPopups_Terminate();
    ECSILayout_Terminate();
    ECSIWindow_Terminate();
    ECSIPanels_Terminate();
    ECSIServices_DestroyHandles();

    // plugins shut down while timers, events and the functions of other plugins still work, and their code is unloaded once no worker runs it; the GPU device goes after them, since they may hold its objects
    ECSIPlugins_Destroy();
    ECSIWindow_DestroyDevice();
    ECSIWorkers_Terminate();
    ECSIServices_Terminate();
    ECSIEvents_Terminate();
    ECSIPlugins_ReportRepeatedErrors();
    ECSIPlugins_Unload();
    ECSISettings_Terminate();
    ECSIBindings_Terminate();
    ECSIRestart restart = {.option = ECSISession_GetNextOption()};
    restart.path = ECSISession_GetNext() == nullptr ? nullptr : SDL_strdup(ECSISession_GetNext());
    ECSISession_Terminate();
    ECSISession_FreeInfo(&APP.preset);
    SDL_free(APP.presetPath);
    SDL_free(APP.sessionPath);
    SDL_free(APP.lastSession);
    SDL_free(APP.configFolder);
    SDL_free(APP.presetsFolder);
    SDL_free(APP.stateFolder);
    SDL_free(APP.dataFolder);
    ECSILua_Terminate();

    // Debug builds check for leaks here, when OpenECS has freed its memory but SDL still holds its own
    ECSISanitizers_CheckLeaks();

    SDL_Quit();
    ECSILog_CloseFile();
    return restart;
}
