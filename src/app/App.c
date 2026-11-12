#include "app/App.h"
#include "app/Bindings.h"
#include "app/Desktop.h"
#include "app/Session.h"
#include "app/Test.h"
#include "app/Workers.h"
#include "base/Log.h"
#include "base/Lua.h"
#include "base/Sanitizers.h"
#include "base/Timers.h"
#include "interface/Clipboard.h"
#include "interface/Dialogs.h"
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
#include "runtime/Tasks.h"

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
    char *sessionPath;       // nullptr when OpenECS starts from the preset
    char *lastSession;       // where the session is saved on quit, or nullptr if there is no state folder
    char *configFolder;      // nullptr if there is none
    char *presetsFolder;     // the user's presets, or nullptr if there is no configuration folder
    char *stateFolder;       // nullptr if there is none
    char *dataFolder;        // the user's plugins and saved sessions are in it; nullptr if there is none, as in a test
    bool test;               // true when the program runs a test: no user files, no last session, nothing saved
    bool definitions;        // true when the program only writes definition files: no display, nothing opened or saved
    char *userPlugins;       // the user's plugin folder, or nullptr
    char *firstPartyPlugins; // the first-party plugins shipped with OpenECS
    const char *pluginFolders[3];
    usz pluginFolderCount;
    struct
    {
        char *name;
        bool enable;
        char *misuse;  // for a plugin disabled because it misused a function, what the misuse said; nullptr when it was asked for
    } *pluginRequests; // stb_ds array of plugins to enable or disable when the current pass of the main loop ends
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

/// @brief Removes everything a failed plugin registered: its panel types, functions, settings, timers, events and keys.
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

/// @brief Names the file that asks for the preset's plugins, for reports: the tool's last session, a session or the preset.
/// @return The name, which the caller frees, or nullptr when it cannot be allocated.
static char *ECSIApp_NamePluginsSource(bool lastSession)
{
    const char *source = "the preset";

    if (lastSession)
    {
        source = "the tool's last session";
    }
    else if (APP.sessionPath != nullptr)
    {
        source = "the session";
    }

    char *name = nullptr;

    if (SDL_asprintf(&name, "%s '%s'", source, APP.sessionPath != nullptr ? APP.sessionPath : APP.presetPath) < 0)
    {
        return nullptr;
    }

    return name;
}

/// @brief Loads every plugin in some folders of plugins, for writing the definitions.
static SHUResult ECSIApp_LoadEveryPlugin(const char *const *folders, usz folderCount)
{
    ECSValue *every = nullptr;
    SHUResult result = ECSValue_Create(&every);
    result = result ? result : ECSIApp_FindPlugins(folders, folderCount, every);
    result = result ? result : ECSIPlugins_Load(folders, folderCount, every, "writing the definitions");
    ECSValue_Destroy(&every);
    return result;
}

/// @brief Loads the plugins a preset names. Search order: the preset's plugins folder, the user's plugins, the first-party plugins.
static void ECSIApp_LoadPlugins(const ECSIPresetInfo *preset)
{
    if (APP.dataFolder != nullptr && SDL_asprintf(&APP.userPlugins, "%splugins/", APP.dataFolder) < 0)
    {
        APP.userPlugins = nullptr;
    }

    if (SDL_asprintf(&APP.firstPartyPlugins, "%splugins/", SDL_GetBasePath()) < 0)
    {
        APP.firstPartyPlugins = nullptr;
    }

    // the folders are kept, as plugins are enabled later too
    const char *const *folders = APP.pluginFolders;
    const char *candidates[] = {preset->pluginsFolder, APP.userPlugins, APP.firstPartyPlugins};

    for (usz i = 0; i < SDL_arraysize(candidates); i++)
    {
        if (candidates[i] != nullptr)
        {
            APP.pluginFolders[APP.pluginFolderCount++] = candidates[i];
        }
    }

    usz folderCount = APP.pluginFolderCount;

    // the plugins that the session disabled start disabled, and those that it enabled load after the preset's
    const ECSValue *disabled = ECSValue_GetTableField(preset->file, "disabled");
    const char **disabledNames = nullptr;

    for (usz i = 0; i < ECSValue_GetListCount(disabled); i++)
    {
        arrput(disabledNames, ECSValue_GetString(ECSValue_GetListItem(disabled, i), ""));
    }

    ECSIPlugins_SetDisabled(disabledNames, arrlenu(disabledNames));

    // reports name the file that asks for a plugin
    bool lastSession = APP.sessionPath != nullptr && APP.lastSession != nullptr && SDL_strcmp(APP.sessionPath, APP.lastSession) == 0;
    char *neededBy = ECSIApp_NamePluginsSource(lastSession);

    // the preset's plugins first, then the plugins that the settings files name for every tool; a test loads only what its preset names, and writing definitions loads every plugin it finds
    SHUResult result = ECSIPlugins_Load(folders, folderCount, ECSValue_GetTableField(preset->file, "depends"), neededBy != nullptr ? neededBy : "the preset");
    SHUResult enabledResult = ECSIPlugins_Load(folders, folderCount, ECSValue_GetTableField(preset->file, "enabled"), "the plugins the session enabled");
    result = result ? result : enabledResult;
    SHUResult extraResult = APP.test ? SHUResult_Ok : ECSIPlugins_Load(folders, folderCount, ECSISettings_GetPlugins(), "the settings files");
    ECSIPlugins_SetDisabled(nullptr, 0);
    arrfree(disabledNames);

    if (APP.definitions)
    {
        SHUResult everyResult = ECSIApp_LoadEveryPlugin(folders, folderCount);
        extraResult = extraResult ? extraResult : everyResult;
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
static i32 ECSIApp_ShorterWait(i32 first, i32 second)
{
    return first < 0 || (second >= 0 && second < first) ? second : first;
}

/// @brief Starts from a session in place of the preset: the session's identity wins, so it may name another tool.
static void ECSIApp_UseSession(const char *path)
{
    APP.sessionPath = SDL_strdup(path);
    ECSISession_FreeInfo(&APP.preset);
    ECSIApp_ExitOnFailure(APP.sessionPath == nullptr ? SHUResult_ErrAllocation : ECSISession_ReadInfo(APP.sessionPath, &APP.preset), "reading the session", path);
}

/// @brief Tells whether the tool's last session can be read. One that cannot is reported and moved aside, so the tool starts from its preset now and at the next start.
static bool ECSIApp_CheckLastSession(void)
{
    ECSIPresetInfo info = {0};
    SHUResult result = ECSISession_ReadInfo(APP.lastSession, &info);
    ECSISession_FreeInfo(&info);

    if (result)
    {
        char *aside = nullptr;
        bool moved = SDL_asprintf(&aside, "%s.unreadable", APP.lastSession) >= 0 && SDL_RenamePath(APP.lastSession, aside);
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "The last session '%s' cannot be read (%s), so the tool starts from its preset.%s", APP.lastSession, SHUResult_String(result),
                    moved ? " The file is kept beside it, with '.unreadable' after its name." : "");
        SDL_free(aside);
    }

    return !result;
}

/// @brief Finds the user's folders, opens the log file in the state folder and finds the folder of the user's presets.
/// A test has no folders of the user; its presets field names the folder that stands for the user's presets.
static void ECSIApp_FindUserFolders(void)
{
    if (APP.test)
    {
        APP.presetsFolder = ECSITest_GetPresetsFolder() == nullptr ? nullptr : SDL_strdup(ECSITest_GetPresetsFolder());
        return;
    }

    APP.configFolder = ECSIApp_FindXdgFolder("XDG_CONFIG_HOME", ".config");
    APP.stateFolder = ECSIApp_FindXdgFolder("XDG_STATE_HOME", ".local/state");
    APP.dataFolder = ECSIApp_FindXdgFolder("XDG_DATA_HOME", ".local/share");

    // the lines logged so far went to standard error only; from here, a failed start can name the log file
    char *logPath = nullptr;

    if (APP.stateFolder != nullptr && SDL_CreateDirectory(APP.stateFolder) && SDL_asprintf(&logPath, "%s%s", APP.stateFolder, OPENECS_LOG_FILE) >= 0)
    {
        ECSILog_OpenFile(logPath);
        SDL_free(logPath);
    }

    if (APP.configFolder != nullptr && SDL_asprintf(&APP.presetsFolder, "%s%s", APP.configFolder, OPENECS_PRESETS_FOLDER) < 0)
    {
        APP.presetsFolder = nullptr;
    }
}

/// @brief Finds the folder of the saved sessions. A test's sessions field names the folder that stands for it.
/// @return The folder, which the caller frees, or nullptr when there is none.
static char *ECSIApp_FindSessionsFolder(void)
{
    if (APP.test)
    {
        return ECSITest_GetSessionsFolder() == nullptr ? nullptr : SDL_strdup(ECSITest_GetSessionsFolder());
    }

    char *folder = nullptr;

    if (APP.dataFolder == nullptr || SDL_asprintf(&folder, "%s%s", APP.dataFolder, OPENECS_SESSIONS_FOLDER) < 0)
    {
        return nullptr;
    }

    return folder;
}

/// @brief Starts SDL's video. A test, and writing definitions, need no display and make no sound; the environment variables still choose other drivers.
static void ECSIApp_StartSDL(void)
{
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

/// @brief Asks on the main thread for a plugin that misused a function to be disabled. data is the request, which this takes.
static void ECSIApp_RequestMisusedPlugin(void *data)
{
    typeof(*APP.pluginRequests) *request = data;
    arrput(APP.pluginRequests, *request);
    SDL_free(request);
}

/// @brief Deals with a plugin's callback that misused a function (DESIGN 14.2): its panel faults, or its plugin is disabled when the pass ends.
static void ECSIApp_Misused(const ECSICallback *callback)
{
    if (callback->type == ECSICallbackType_Panel)
    {
        ECSIPanel_Fault(callback->panel, callback->misuse);
        ECSILayout_RequestFrame();
        return;
    }

    typeof(*APP.pluginRequests) *request = SDL_malloc(sizeof(*request));
    char *name = SDL_strdup(ECSIPlugin_GetName(callback->plugin));
    char *misuse = SDL_strdup(callback->misuse);

    if (request == nullptr || name == nullptr || misuse == nullptr)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' misused a function, and cannot be disabled: out of memory.", ECSIPlugin_GetName(callback->plugin));
        SDL_free(request);
        SDL_free(name);
        SDL_free(misuse);
        return;
    }

    *request = (typeof(*request)){.name = name, .enable = false, .misuse = misuse};

    // a worker thread's misuse is sent to the main thread, which changes plugins
    if (SDL_IsMainThread())
    {
        ECSIApp_RequestMisusedPlugin(request);
    }
    else if (!SDL_RunOnMainThread(ECSIApp_RequestMisusedPlugin, request, false))
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' misused a function, and cannot be disabled: %s", name, SDL_GetError());
        SDL_free(request);
        SDL_free(name);
        SDL_free(misuse);
    }
}

/// @brief Notes a misuse as it happens: a test fails on it.
static void ECSIApp_Misuse(const char *message)
{
    (void)message;
    ECSITest_NoteMisuse();
}

/// @brief Notes a misuse of a core function, which the Lua module found in an argument from Lua.
static void ECSIApp_NoteLuaMisuse(const char *message)
{
    ECSIPlugin_NoteMisuse(nullptr, message);
}

void ECSIApp_Start(const ECSIArguments *arguments)
{
    SDL_assert(arguments != nullptr);

    SDL_Log("OpenECS %s, plugin API %d.", OPENECS_VERSION, OPENECS_API_VERSION);

    // every path of the program's own files starts here
    ECSIApp_ExitOnFailure(SDL_GetBasePath() == nullptr ? SHUResult_ErrNotFound : SHUResult_Ok, "finding the program's folder", nullptr);

    // read the preset first, because SDL needs the tool's identity before it starts
    ECSIApp_ExitOnFailure(ECSILua_Initialize(), "starting Lua", nullptr);
    ECSIApp_ExitOnFailure(ECSIEvents_Initialize(), "declaring the core's events", nullptr);
    ECSIApp_ExitOnFailure(ECSITasks_Initialize(), "preparing worker threads", nullptr);
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

    ECSIApp_FindUserFolders();

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

    if (APP.sessionPath == nullptr && !arguments->fresh && ECSISettings_PeekBool(OPENECS_KEEP_SESSION, false) && APP.lastSession != nullptr && SDL_GetPathInfo(APP.lastSession, nullptr) && ECSIApp_CheckLastSession())
    {
        ECSIApp_UseSession(APP.lastSession);
        ECSIApp_ExitOnFailure(ECSISettings_SetPreset(ECSValue_GetTableField(APP.preset.file, "settings"), APP.sessionPath), "reading the session's settings", APP.sessionPath);
    }

    ECSSettingDesc keepSession = {.name = OPENECS_KEEP_SESSION, .type = ECSSettingType_Bool, .description = "Save the tool's session when it quits, and start from it the next time"};
    ECSIApp_ExitOnFailure(ECSISettings_DeclareCore(&keepSession), "declaring the session settings", nullptr);
    ECSIApp_ExitOnFailure(ECSIServices_DeclareSettings(), "declaring the services' settings", nullptr);
    const char *sourcePath = APP.sessionPath != nullptr ? APP.sessionPath : APP.presetPath;

    SDL_SetAppMetadata(APP.preset.appName, nullptr, APP.preset.appID);
    ECSIDesktop_SetAppID(APP.preset.appID);

    ECSIApp_StartSDL();

    // the system's theme is known once SDL has started
    ECSISettings_ApplyTheme();
    ECSIApp_ExitOnFailure(ECSILayout_Initialize(), "declaring the layout settings", nullptr);
    ECSIApp_ExitOnFailure(ECSIWindow_Initialize(APP.preset.appName), "opening the window", nullptr);
    ECSIWindow_SetHint(ECSValue_GetString(ECSValue_GetTableField(APP.preset.file, "hint"), nullptr));
    ECSIApp_ExitOnFailure(ECSIKeys_Initialize(), "reading the keys", nullptr);
    ECSIApp_ExitOnFailure(ECSIMenus_Initialize(), "registering the core's functions", nullptr);
    ECSIApp_ExitOnFailure(ECSIInput_Initialize(), "declaring the input settings", nullptr);
    ECSIDialogs_Initialize();

    char *sessionsFolder = ECSIApp_FindSessionsFolder();

    // the layout is reset from the preset, or from the session the command line named
    const char *origin = arguments->session != nullptr && !APP.test ? arguments->session : APP.presetPath;
    ECSISessionFolders folders = {.presets = APP.presetsFolder, .sessions = sessionsFolder, .state = APP.stateFolder, .origin = origin};
    ECSIApp_ExitOnFailure(ECSISession_Initialize(&folders, !APP.test), "registering the session functions", nullptr);
    SDL_free(sessionsFolder);

    ECSIBindings_Initialize();
    ECSIWorkers_Initialize();
    ECSIPluginHooks hooks = {.StartLua = ECSIBindings_StartPlugin, .RemoveRegistrations = ECSIApp_RemoveRegistrations, .Misused = ECSIApp_Misused, .Misuse = ECSIApp_Misuse};
    ECSIPlugins_SetHooks(&hooks);
    ECSILua_SetMisuseFunction(ECSIApp_NoteLuaMisuse);

    ECSIApp_LoadPlugins(&APP.preset);
    ECSIApp_ExitOnFailure(ECSISession_Apply(sourcePath, &APP.preset), "building the layout", nullptr);

    // misspelt names change nothing, so they are reported once everything is registered
    ECSISettings_ReportUndeclared();
    ECSIKeys_ReportUnknownFunctions();

    usz fileCount = APP.definitions ? 0 : arguments->fileCount;
    char **files = APP.test ? ECSITest_GetFiles(&fileCount) : arguments->files;
    ECSIApp_OpenFiles(files, fileCount);
}

int ECSIApp_Install(const ECSIArguments *arguments)
{
    SDL_assert(arguments != nullptr);

    SHUResult result = SDL_GetBasePath() == nullptr ? SHUResult_ErrNotFound : ECSILua_Initialize();
    ECSIApp_FindUserFolders();
    result = result ? result : ECSISession_FindPreset(&APP.presetPath, arguments->preset, APP.presetsFolder);
    result = result ? result : ECSISession_ReadInfo(APP.presetPath, &APP.preset);

    // a preset named by its name keeps working when OpenECS moves its presets
    const char *name = ECSISession_IsPresetPath(arguments->preset) ? nullptr : arguments->preset;
    result = result ? result : (arguments->install ? ECSIDesktop_Install(&APP.preset, APP.presetPath, name) : ECSIDesktop_Uninstall(&APP.preset));

    if (result)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Could not %s the preset '%s' (%s).", arguments->install ? "install" : "uninstall", arguments->preset, SHUResult_String(result));
    }

    ECSISession_FreeInfo(&APP.preset);
    SDL_free(APP.presetPath);
    SDL_free(APP.configFolder);
    SDL_free(APP.presetsFolder);
    SDL_free(APP.stateFolder);
    SDL_free(APP.dataFolder);
    ECSILua_Terminate();
    ECSISanitizers_CheckLeaks();
    SDL_Quit();
    ECSILog_CloseFile();
    return result ? 1 : 0;
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

    // the plugin interface first, so the folder holds everything a plugin's author includes
    SHUResult result = ECSIServices_WriteInterface(path);
    SHUResult written = ECSIPlugins_ForEach(ECSIApp_WritePluginDefinitions, path);
    result = result ? result : written;

    SDL_Log("The plugin interface and the definition files written to '%s'.", path);
    SDL_free(path);
    return result;
}

/// @brief Tells whether a panel's type belongs to one of some plugins.
static bool ECSIApp_PanelOfPlugins(ECSPanel panel, const ECSPlugin *plugins)
{
    for (usz i = 0; panel->type != nullptr && i < arrlenu(plugins); i++)
    {
        if (panel->type->plugin == plugins[i])
        {
            return true;
        }
    }

    return false;
}

/// @brief Sets whether the session names a plugin in its enabled list: a plugin the user enables that the preset does not load.
static void ECSIApp_NoteEnabled(const char *name, bool enabled)
{
    ECSValue *file = APP.preset.file;
    ECSValue *list = nullptr;
    const ECSValue *old = ECSValue_GetTableField(file, "enabled");
    char **kept = nullptr;

    for (usz i = 0; i < ECSValue_GetListCount(old); i++)
    {
        const char *item = ECSValue_GetString(ECSValue_GetListItem(old, i), "");

        if (SDL_strcmp(item, name) != 0)
        {
            arrput(kept, SDL_strdup(item));
        }
    }

    if (enabled && ECSValue_GetTableField(ECSValue_GetTableField(file, "depends"), name) == nullptr)
    {
        arrput(kept, SDL_strdup(name));
    }

    // the list is written again without the name, and with it if it is enabled
    if (!ECSValue_SetTableField(file, "enabled", &list))
    {
        ECSValue_SetTable(list);

        for (usz i = 0; i < arrlenu(kept); i++)
        {
            ECSValue *item = nullptr;

            if (kept[i] != nullptr && (ECSValue_AddListItem(list, &item) || ECSValue_SetString(item, kept[i])))
            {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "The session cannot name the plugin '%s': out of memory.", kept[i]);
            }
        }
    }

    for (usz i = 0; i < arrlenu(kept); i++)
    {
        SDL_free(kept[i]);
    }

    arrfree(kept);
}

/// @brief Disables a plugin that no enabled plugin depends on, after asking about the unsaved work of its panels, which become placeholders.
/// A plugin that misused a function is disabled without asking, with the plugins that depend on it (DESIGN 9.10).
/// @param misuse What the misuse said, or nullptr when the disable was asked for.
static void ECSIApp_DisablePlugin(const char *name, const char *misuse)
{
    ECSPlugin target = ECSIPlugins_Get(name);

    if (target == nullptr)
    {
        // a plugin that misused a function twice in a pass is disabled once
        if (misuse == nullptr)
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' is not enabled, so it is not disabled.", name);
        }

        return;
    }

    ECSPlugin *plugins = ECSIPlugins_GetDependents(target);

    // the last loaded goes first, so the first one depends on the plugin when any does
    if (misuse == nullptr && arrlenu(plugins) > 1)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' is not disabled, as the enabled plugin '%s' depends on it. Disable that one first.", name,
                    ECSIPlugin_GetName(plugins[0]));
        arrfree(plugins);
        return;
    }

    ECSPanel *panels = ECSILayout_GetPanels();
    ECSPanel *affected = nullptr;

    for (usz i = 0; i < arrlenu(panels); i++)
    {
        if (ECSIApp_PanelOfPlugins(panels[i], plugins))
        {
            arrput(affected, panels[i]);
        }
    }

    // Cancel keeps every plugin enabled; a misuse is not asked about, and each panel keeps its state in its placeholder
    if (misuse != nullptr)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' is disabled, with the plugins that depend on it, because it misused a function.", name);
    }

    if (misuse != nullptr || ECSIPanels_ConfirmClose(affected, arrlenu(affected), false))
    {
        // popups may belong to the plugins' panels, so every popup closes, and their Closed functions run while the plugins' code is loaded
        ECSIPopups_CloseAll();
        ECSIEvents_Deliver();
        ECSIPopups_DestroyClosed();

        for (usz i = 0; i < arrlenu(plugins); i++)
        {
            for (usz j = 0; j < arrlenu(affected); j++)
            {
                if (affected[j]->type != nullptr && affected[j]->type->plugin == plugins[i])
                {
                    ECSIPanel_Unload(affected[j]);
                }
            }

            ECSIWorkers_StopPlugin(plugins[i]);
            ECSIPlugin_StopWork(plugins[i]);
            ECSITasks_ForgetPlugin(plugins[i]);
            ECSIServices_DestroyHandlesOfPlugin(plugins[i]);
            ECSIApp_NoteEnabled(ECSIPlugin_GetName(plugins[i]), false);
            ECSIPlugins_Disable(plugins[i], misuse);
        }

        ECSIEvents_EmitCore("ecs.pluginsChanged", nullptr);
        ECSILayout_RequestFrame();
    }

    arrfree(affected);
    arrfree(panels);
    arrfree(plugins);
}

/// @brief Enables a plugin and the plugins it depends on, and creates again the placeholders of their panels.
static void ECSIApp_EnablePlugin(const char *name)
{
    if (ECSIPlugins_Get(name) != nullptr)
    {
        return;
    }

    if (ECSIPlugins_Enable(APP.pluginFolders, APP.pluginFolderCount, name))
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' could not be enabled.", name);
        return;
    }

    ECSIApp_NoteEnabled(name, true);
    ECSPanel *panels = ECSILayout_GetPanels();

    // the placeholders of the panel types that now run come back, from the state they kept; panels that failed stay as they are
    for (usz i = 0; i < arrlenu(panels); i++)
    {
        if (panels[i]->type == nullptr && panels[i]->fault == nullptr && ECSIPlugins_OwnerRuns(panels[i]->typeName))
        {
            (void)ECSIPanel_Restart(panels[i]);
        }
    }

    arrfree(panels);
    ECSIEvents_EmitCore("ecs.pluginsChanged", nullptr);
    ECSILayout_RequestFrame();
}

/// @brief Makes the changes to plugins that were asked for during the pass, now that no plugin's code runs.
static void ECSIApp_ChangePlugins(void)
{
    // a change runs plugins' code, whose misuse asks for more; those wait for the next pass
    typeof(APP.pluginRequests) requests = APP.pluginRequests;
    APP.pluginRequests = nullptr;

    for (usz i = 0; i < arrlenu(requests); i++)
    {
        if (requests[i].enable)
        {
            ECSIApp_EnablePlugin(requests[i].name);
        }
        else
        {
            ECSIApp_DisablePlugin(requests[i].name, requests[i].misuse);
        }

        SDL_free(requests[i].name);
        SDL_free(requests[i].misuse);
    }

    arrfree(requests);
}

/// @brief Asks for a plugin to be enabled or disabled when the current pass of the main loop ends.
static SHUResult ECSIApp_RequestPlugin(const char *name, bool enable)
{
    SDL_assert(name != nullptr);

    char *copy = SDL_strdup(name);

    if (copy == nullptr)
    {
        return SHUResult_ErrAllocation;
    }

    arrput(APP.pluginRequests, ((typeof(*APP.pluginRequests)){.name = copy, .enable = enable}));
    return SHUResult_Ok;
}

SHUResult ECSPlugins_Enable(const char *name)
{
    return ECSIApp_RequestPlugin(name, true);
}

SHUResult ECSPlugins_Disable(const char *name)
{
    return ECSIApp_RequestPlugin(name, false);
}

SHUResult ECSPlugins_List(ECSValue *retList)
{
    ECSILog_Check(retList != nullptr, SHUResult_ErrAssertion);
    return ECSIPlugins_List(APP.pluginFolders, APP.pluginFolderCount, retList);
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

        ECSITimers_Run();
        ECSIEvents_Deliver();
        ECSISettings_DeliverChanges();

        // popups of closed or hidden panels close, and their Closed functions run before the panels are destroyed
        ECSIPopups_CloseHidden();
        ECSIEvents_Deliver();
        ECSIPanels_DestroyClosed();
        ECSIPopups_DestroyClosed();
        ECSIApp_ChangePlugins();

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
    ECSIDialogs_Terminate();
    ECSIClipboard_Terminate();
    ECSIMenus_Terminate();
    ECSIKeys_Terminate();
    ECSIPopups_Terminate();
    ECSILayout_Terminate();
    ECSIWindow_Terminate();
    ECSIPanels_Terminate();
    ECSIServices_DestroyHandles();

    // no work runs in the background on what the plugins free: running work finishes first, and waiting work is dropped
    ECSITasks_Stop();

    // plugins shut down while timers, events and the functions of other plugins still work, and their code is unloaded later; the GPU device goes after them, since they may hold its objects
    ECSIPlugins_Destroy();
    ECSIWindow_DestroyDevice();
    ECSIWorkers_Terminate();
    ECSIServices_Terminate();
    ECSITimers_Terminate();
    ECSITasks_Terminate();
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
    SDL_free(APP.userPlugins);
    SDL_free(APP.firstPartyPlugins);

    for (usz i = 0; i < arrlenu(APP.pluginRequests); i++)
    {
        SDL_free(APP.pluginRequests[i].name);
    }

    arrfree(APP.pluginRequests);
    ECSILua_Terminate();

    // Debug builds check for leaks here, when OpenECS has freed its memory but SDL still holds its own
    ECSISanitizers_CheckLeaks();

    SDL_Quit();
    ECSILog_CloseFile();
    return restart;
}
