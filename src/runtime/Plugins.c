#include "runtime/Plugins.h"

#include "base/Lua.h"

#include "SDL3/SDL.h"
#include "stb/stbSDL3.h"

#pragma region Source Only

/// @brief Longest text that names what needs a plugin, in reports.
#define OPENECS_PLUGINS_REPORT_SIZE 1024

/// @brief A native plugin's ECSPlugin_Init.
typedef SHUResult (*ECSIPluginInitFunction)(ECSPlugin plugin);

/// @brief A native plugin's ECSPlugin_Shutdown.
typedef void (*ECSIPluginShutdownFunction)(ECSPlugin plugin);

typedef struct ECSIPlugin
{
    char *name;
    char *version;
    SDL_SharedObject *library;          // NULL if the plugin has no native code
    ECSIPluginShutdownFunction Shutdown;
    bool failed;                        // its ECSPlugin_Init failed; plugins that depend on it are skipped
    char **dependencies;                // stb_ds array of the names its manifest depends on
    ECSPluginStateDesc state;           // Save is NULL if the plugin saves no state of its own
    ECSTimerFunction stateRelease;
    ECSTaskFunction LuaShutdown; // the Lua code's shutdown, or NULL
    ECSTaskFunction luaShutdownRelease;
    void *luaShutdownData;
} ECSIPlugin;

/// @brief State while the plugins of a table are loaded.
typedef struct ECSIPluginLoader
{
    const char *dependent; // the plugin whose dependencies these are, or NULL for the plugins a preset or user names
    SHUResult result;
} ECSIPluginLoader;

/// @brief A plugin's manifest.
typedef struct ECSIManifest
{
    char *folder;   // the plugin's folder, ending with a separator
    ECSValue *file; // the whole manifest
} ECSIManifest;

static struct
{
    struct
    {
        char *key; // the plugin's own copy of its name
        ECSIPlugin *value;
    } *plugins; // stb_ds hash map in load order, because nothing is deleted from it; handles point to the plugins
    const char *const *directories;
    usz directoryCount;
    const char *neededBy; // what names the plugins being loaded, for reports
    const char **loading; // stb_ds array of the plugins being loaded, to find dependency cycles
    struct
    {
        char *key; // "plugin: message"
        u64 value; // how often it happened
    } *errors;     // stb_ds hash map with copied keys
    ECSIPluginHooks hooks;
} PLUGINS = {0};

static ECSIPlugin *ECSIPlugin_Find(const char *name)
{
    return shget(PLUGINS.plugins, name);
}

/// @brief Forgets a plugin's state description, and releases its data.
static void ECSIPlugin_ForgetState(ECSIPlugin *plugin)
{
    if (plugin->stateRelease != NULL)
    {
        plugin->stateRelease(plugin->state.data);
    }

    plugin->state = (ECSPluginStateDesc){0};
    plugin->stateRelease = NULL;

    if (plugin->luaShutdownRelease != NULL)
    {
        plugin->luaShutdownRelease(plugin->luaShutdownData);
    }

    plugin->LuaShutdown = NULL;
    plugin->luaShutdownRelease = NULL;
}

static void ECSIPlugin_Free(ECSIPlugin *plugin)
{
    for (usz i = 0; i < arrlenu(plugin->dependencies); i++)
    {
        SDL_free(plugin->dependencies[i]);
    }

    arrfree(plugin->dependencies);
    SDL_free(plugin->name);
    SDL_free(plugin->version);
}

/// @brief Copies the name of a dependency into a plugin.
static void ECSIPlugin_AddDependency(const char *name, const ECSValue *field, void *userData)
{
    (void)field;
    ECSIPlugin *plugin = userData;
    char *copy = SDL_strdup(name);

    if (copy != NULL)
    {
        arrput(plugin->dependencies, copy);
    }
}

static void ECSIManifest_Free(ECSIManifest *manifest)
{
    SDL_free(manifest->folder);
    ECSValue_Destroy(&manifest->file);
    SDL_zerop(manifest);
}

/// @brief Reads a version text, such as "1.2.0" or "1.2"; missing numbers are 0.
static void ECSIPlugin_ReadVersion(const char *text, u64 retParts[3])
{
    for (usz i = 0; i < 3; i++)
    {
        char *end = NULL;
        retParts[i] = SDL_strtoull(text, &end, 10);
        text = *end == '.' ? end + 1 : end;
    }
}

/// @brief Checks a version against a minimum version: the minimum or a later version with the same major number.
static bool ECSIPlugin_VersionMatches(const char *version, const char *minimum)
{
    u64 have[3];
    u64 need[3];
    ECSIPlugin_ReadVersion(version, have);
    ECSIPlugin_ReadVersion(minimum, need);

    return have[0] == need[0] && (have[1] > need[1] || (have[1] == need[1] && have[2] >= need[2]));
}

/// @brief Names what needs the plugin being looked for: the plugin whose dependencies are loading, or what names the plugins.
static void ECSIPlugin_NeededBy(SHUSlice buffer)
{
    if (arrlenu(PLUGINS.loading) > 0)
    {
        SDL_snprintf(buffer.data, buffer.size, "plugin '%s'", arrlast(PLUGINS.loading));
    }
    else
    {
        SDL_strlcpy(buffer.data, PLUGINS.neededBy, buffer.size);
    }
}

/// @brief Reports a plugin that no plugin directory holds: what needs it, and every folder that was looked in.
static void ECSIManifest_ReportMissing(const char *name)
{
    char neededBy[OPENECS_PLUGINS_REPORT_SIZE];
    ECSIPlugin_NeededBy(cs(neededBy, sizeof(neededBy)));
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' is not found. It is needed by %s.", name, neededBy);

    for (usz i = 0; i < PLUGINS.directoryCount; i++)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "  Looked for '%s%s/manifest.lua'.", PLUGINS.directories[i], name);
    }
}

/// @brief Finds a plugin's folder in the plugin directories and reads its manifest.
static SHUResult ECSIManifest_Find(const char *name, ECSIManifest *retManifest)
{
    SDL_zerop(retManifest);

    for (usz i = 0; i < PLUGINS.directoryCount; i++)
    {
        char *path = NULL;

        if (SDL_asprintf(&path, "%s%s/manifest.lua", PLUGINS.directories[i], name) < 0)
        {
            return SHUResult_ErrAllocation;
        }

        if (!SDL_GetPathInfo(path, NULL))
        {
            SDL_free(path);
            continue;
        }

        SHUResult result = ECSValue_Create(&retManifest->file);
        result = result ? result : ECSILua_ReadData(path, retManifest->file);
        SDL_free(path);
        SHU_ReturnResult(result, ECSIManifest_Free(retManifest););

        if (SDL_strcmp(ECSValue_GetString(ECSValue_GetTableField(retManifest->file, "name"), ""), name) != 0)
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "The manifest of plugin '%s' gives another name.", name);
            ECSIManifest_Free(retManifest);
            return SHUResult_ErrBadData;
        }

        if (SDL_asprintf(&retManifest->folder, "%s%s/", PLUGINS.directories[i], name) < 0)
        {
            retManifest->folder = NULL;
            ECSIManifest_Free(retManifest);
            return SHUResult_ErrAllocation;
        }

        return SHUResult_Ok;
    }

    ECSIManifest_ReportMissing(name);
    return SHUResult_ErrNotFound;
}

static SHUResult ECSIPlugin_LoadAll(const ECSValue *plugins, const char *dependent);

/// @brief Loads the plugins a manifest depends on, then the plugin itself, and runs its ECSPlugin_Init.
static SHUResult ECSIPlugin_Start(const char *name, const ECSIManifest *manifest)
{
    const ECSValue *file = manifest->file;
    i64 api = ECSValue_GetInteger(ECSValue_GetTableField(file, "api"), 0);
    const char *native = ECSValue_GetString(ECSValue_GetTableField(file, "native"), NULL);

    if (api != OPENECS_API_VERSION)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' was made for plugin API %" SDL_PRIs64 "; this is version %d.", name, api, OPENECS_API_VERSION);
        return SHUResult_ErrBadData;
    }

    arrput(PLUGINS.loading, name);
    SHUResult dependencies = ECSIPlugin_LoadAll(ECSValue_GetTableField(file, "depends"), name);
    (void)arrpop(PLUGINS.loading);
    SHU_ReturnResult(dependencies);

    ECSIPlugin plugin = {.name = SDL_strdup(name), .version = SDL_strdup(ECSValue_GetString(ECSValue_GetTableField(file, "version"), "0.0.0"))};
    ECSIPlugin *record = SDL_malloc(sizeof(ECSIPlugin));
    ECSIPluginInitFunction Init = NULL;

    ECSIValue_TableForEachField(ECSValue_GetTableField(file, "depends"), ECSIPlugin_AddDependency, &plugin);

    if (plugin.name == NULL || plugin.version == NULL || record == NULL)
    {
        SDL_free(record);
        ECSIPlugin_Free(&plugin);
        return SHUResult_ErrAllocation;
    }

    if (native != NULL)
    {
        char *path = NULL;

        if (SDL_asprintf(&path, "%s%s", manifest->folder, native) >= 0)
        {
            plugin.library = SDL_LoadObject(path);
            SDL_free(path);
        }

        if (plugin.library == NULL)
        {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot load the library of plugin '%s': %s", name, SDL_GetError());
            SDL_free(record);
            ECSIPlugin_Free(&plugin);
            return SHUResult_ErrFile;
        }

        Init = (ECSIPluginInitFunction)SDL_LoadFunction(plugin.library, "ECSPlugin_Init");
        plugin.Shutdown = (ECSIPluginShutdownFunction)SDL_LoadFunction(plugin.library, "ECSPlugin_Shutdown");

        if (Init == NULL)
        {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' does not export ECSPlugin_Init.", name);
            SDL_UnloadObject(plugin.library);
            SDL_free(record);
            ECSIPlugin_Free(&plugin);
            return SHUResult_ErrBadData;
        }
    }

    // the plugin is recorded before its Init runs, so the core knows it while it registers things
    *record = plugin;
    shput(PLUGINS.plugins, record->name, record);

    SHUResult result = Init == NULL ? SHUResult_Ok : Init(record);

    if (result)
    {
        // a plugin whose Init fails cleans up before it returns, so it gets no Shutdown
        record->Shutdown = NULL;
        record->failed = true;
        PLUGINS.hooks.RemoveRegistrations(record);
        ECSIPlugin_ForgetState(record);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' failed to start (%s).", name, SHUResult_String(result));
        return result;
    }

    // the native code starts first, then the Lua code
    const char *lua = ECSValue_GetString(ECSValue_GetTableField(file, "lua"), NULL);

    if (lua != NULL)
    {
        char *path = NULL;
        result = SDL_asprintf(&path, "%s%s", manifest->folder, lua) < 0 ? SHUResult_ErrAllocation : PLUGINS.hooks.StartLua(record, manifest->folder, path);
        SDL_free(path);

        if (result)
        {
            record->failed = true;
            PLUGINS.hooks.RemoveRegistrations(record);
            ECSIPlugin_ForgetState(record);
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' failed to start its Lua code (%s).", name, SHUResult_String(result));
            return result;
        }
    }

    SDL_Log("Plugin '%s' %s loaded from '%s'.", record->name, record->version, manifest->folder);
    return SHUResult_Ok;
}

/// @brief Loads a plugin after the plugins it depends on, if it is not loaded yet.
/// @param minimum The minimum version that the caller needs, or NULL.
static SHUResult ECSIPlugin_Load(const char *name, const char *minimum)
{
    ECSIPlugin *plugin = ECSIPlugin_Find(name);
    const char *version = NULL;

    if (plugin != NULL)
    {
        version = plugin->version;
    }
    else
    {
        for (usz i = 0; i < arrlenu(PLUGINS.loading); i++)
        {
            if (SDL_strcmp(PLUGINS.loading[i], name) == 0)
            {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' depends on itself through its dependencies.", name);
                return SHUResult_ErrBadData;
            }
        }
    }

    ECSIManifest manifest = {0};

    if (plugin == NULL)
    {
        SHU_ReturnResult(ECSIManifest_Find(name, &manifest));
        version = ECSValue_GetString(ECSValue_GetTableField(manifest.file, "version"), "0.0.0");
    }

    // the version is checked before the plugin's code runs
    if (minimum != NULL && !ECSIPlugin_VersionMatches(version, minimum))
    {
        char neededBy[OPENECS_PLUGINS_REPORT_SIZE];
        ECSIPlugin_NeededBy(cs(neededBy, sizeof(neededBy)));
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' has version %s, but %s needs version %s or a later one with the same major number.", name, version, neededBy, minimum);
        ECSIManifest_Free(&manifest);
        return SHUResult_ErrBadData;
    }

    if (plugin != NULL)
    {
        return plugin->failed ? SHUResult_ErrBadData : SHUResult_Ok;
    }

    SHUResult result = ECSIPlugin_Start(name, &manifest);
    ECSIManifest_Free(&manifest);
    return result;
}

static void ECSIPlugin_LoadField(const char *name, const ECSValue *field, void *userData)
{
    ECSIPluginLoader *loader = userData;

    // a dependency that fails skips the plugin that needs it, so the rest is not loaded for it
    if (loader->dependent != NULL && loader->result)
    {
        return;
    }

    SHUResult result = ECSIPlugin_Load(name, ECSValue_GetString(field, NULL));

    if (result)
    {
        loader->result = result;

        if (loader->dependent != NULL)
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' is skipped because '%s' failed.", loader->dependent, name);
        }
    }
}

/// @brief Loads the plugins that a table names: named fields with a minimum version, and list items with a name only.
/// @param dependent The plugin whose dependencies these are; it stops at the first failure. NULL loads every plugin it can.
static SHUResult ECSIPlugin_LoadAll(const ECSValue *plugins, const char *dependent)
{
    ECSIPluginLoader loader = {.dependent = dependent, .result = SHUResult_Ok};
    ECSIValue_TableForEachField(plugins, ECSIPlugin_LoadField, &loader);

    for (usz i = 0; i < ECSValue_GetListCount(plugins); i++)
    {
        const char *name = ECSValue_GetString(ECSValue_GetListItem(plugins, i), NULL);

        if (name != NULL)
        {
            ECSIPlugin_LoadField(name, NULL, &loader);
        }
    }

    return loader.result;
}

#pragma endregion Source Only

SHUResult ECSIPlugins_Load(const char *const *directories, usz directoryCount, const ECSValue *plugins, const char *neededBy)
{
    SDL_assert(directories != NULL);
    SDL_assert(neededBy != NULL);

    PLUGINS.directories = directories;
    PLUGINS.directoryCount = directoryCount;
    PLUGINS.neededBy = neededBy;
    SHUResult result = ECSIPlugin_LoadAll(plugins, NULL);
    PLUGINS.directories = NULL;
    PLUGINS.directoryCount = 0;
    PLUGINS.neededBy = NULL;
    return result;
}

void ECSIPlugins_Shutdown(void)
{
    for (usz i = shlenu(PLUGINS.plugins); i > 0; i--)
    {
        ECSIPlugin *plugin = PLUGINS.plugins[i - 1].value;

        // the Lua code started after the native code, so it shuts down first
        if (plugin->LuaShutdown != NULL)
        {
            plugin->LuaShutdown(plugin->luaShutdownData);
        }

        if (plugin->Shutdown != NULL)
        {
            plugin->Shutdown(plugin);
        }

        plugin->LuaShutdown = NULL;
        plugin->Shutdown = NULL;
    }
}

void ECSIPlugins_Unload(void)
{
    for (usz i = 0; i < shlenu(PLUGINS.errors); i++)
    {
        if (PLUGINS.errors[i].value > 1)
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "This error happened %" SDL_PRIu64 " times: %s", PLUGINS.errors[i].value, PLUGINS.errors[i].key);
        }
    }

    for (usz i = shlenu(PLUGINS.plugins); i > 0; i--)
    {
        ECSIPlugin *plugin = PLUGINS.plugins[i - 1].value;

        ECSIPlugin_ForgetState(plugin);

        if (plugin->library != NULL)
        {
            SDL_UnloadObject(plugin->library);
        }

        ECSIPlugin_Free(plugin);
        SDL_free(plugin);
    }

    shfree(PLUGINS.plugins);
    shfree(PLUGINS.errors);
    arrfree(PLUGINS.loading);
    SDL_zero(PLUGINS);
}

void ECSIPlugins_SetHooks(const ECSIPluginHooks *hooks)
{
    SDL_assert(hooks != NULL && hooks->StartLua != NULL && hooks->RemoveRegistrations != NULL);

    PLUGINS.hooks = *hooks;
}

const char *ECSIPlugin_GetVersion(ECSPlugin plugin)
{
    SDL_assert(plugin != NULL);

    return plugin->version;
}

const char *ECSIPlugin_GetName(ECSPlugin plugin)
{
    SDL_assert(plugin != NULL);

    return plugin->name;
}

SHUResult ECSIPlugin_RegisterState(ECSPlugin plugin, const ECSPluginStateDesc *desc, ECSTimerFunction release)
{
    SDL_assert(plugin != NULL);
    SDL_assert(desc != NULL);

    if (desc->Save == NULL || desc->Restore == NULL || plugin->state.Save != NULL)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' registers its state without Save and Restore, or a second time.", plugin->name);
        return SHUResult_ErrBadData;
    }

    plugin->state = *desc;
    plugin->stateRelease = release;
    return SHUResult_Ok;
}

void ECSIPlugin_SetLuaShutdown(ECSPlugin plugin, ECSTaskFunction function, ECSTaskFunction release, void *data)
{
    SDL_assert(plugin != NULL);

    if (plugin->luaShutdownRelease != NULL)
    {
        plugin->luaShutdownRelease(plugin->luaShutdownData);
    }

    plugin->LuaShutdown = function;
    plugin->luaShutdownRelease = release;
    plugin->luaShutdownData = data;
}

SHUResult ECSPlugin_RegisterState(ECSPlugin plugin, const ECSPluginStateDesc *desc)
{
    return ECSIPlugin_RegisterState(plugin, desc, NULL);
}

void ECSIPlugins_RestoreStates(const ECSValue *states)
{
    for (usz i = 0; i < shlenu(PLUGINS.plugins); i++)
    {
        ECSIPlugin *plugin = PLUGINS.plugins[i].value;
        const ECSValue *entry = ECSValue_GetTableField(states, plugin->name);

        if (plugin->state.Restore == NULL || entry == NULL)
        {
            continue;
        }

        i64 version = ECSValue_GetInteger(ECSValue_GetTableField(entry, "stateVersion"), 0);

        if (plugin->state.Restore(plugin->state.data, ECSValue_GetTableField(entry, "state"), version >= 0 && version <= SDL_MAX_UINT32 ? (u32)version : 0))
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' could not restore its state from the session.", plugin->name);
        }
    }
}

SHUResult ECSIPlugins_SaveStates(ECSValue *states)
{
    SDL_assert(states != NULL);

    ECSValue_SetTable(states);

    for (usz i = 0; i < shlenu(PLUGINS.plugins); i++)
    {
        ECSIPlugin *plugin = PLUGINS.plugins[i].value;
        ECSValue *saved = NULL;
        ECSValue *entry = NULL;
        ECSValue *field = NULL;

        if (plugin->state.Save == NULL)
        {
            continue;
        }

        SHU_ReturnResult(ECSValue_Create(&saved));

        // a failed save keeps the state the session already had
        if (plugin->state.Save(plugin->state.data, saved))
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' could not save its state.", plugin->name);
            ECSValue_Destroy(&saved);
            continue;
        }

        SHUResult result = ECSValue_TableSetField(states, plugin->name, &entry);

        if (!result)
        {
            ECSValue_SetTable(entry);
            result = ECSValue_TableSetField(entry, "state", &field);
        }

        result = result ? result : ECSIValue_Copy(field, saved);
        result = result ? result : ECSValue_TableSetField(entry, "stateVersion", &field);

        if (!result)
        {
            ECSValue_SetInteger(field, plugin->state.version);
        }

        ECSValue_Destroy(&saved);
        SHU_ReturnResult(result);
    }

    return SHUResult_Ok;
}

void ECSIPlugin_ReportError(ECSPlugin plugin, const char *message)
{
    SDL_assert(plugin != NULL);
    SDL_assert(message != NULL);

    char *key = NULL;

    if (SDL_asprintf(&key, "%s: %s", plugin->name, message) < 0)
    {
        return;
    }

    if (PLUGINS.errors == NULL)
    {
        sh_new_strdup(PLUGINS.errors);
    }

    u64 count = shget(PLUGINS.errors, key);
    shput(PLUGINS.errors, key, count + 1);
    SDL_free(key);

    if (count == 0)
    {
        ECS_Log(plugin, ECSLogLevel_Error, "%s", message);
    }
}

bool ECSIPlugin_DependsOn(ECSPlugin plugin, ECSPlugin other)
{
    SDL_assert(plugin != NULL);
    SDL_assert(other != NULL);

    for (usz i = 0; i < arrlenu(plugin->dependencies); i++)
    {
        if (SDL_strcmp(plugin->dependencies[i], other->name) == 0)
        {
            return true;
        }
    }

    return plugin == other;
}

bool ECSIPlugins_OwnerRuns(const char *name)
{
    SDL_assert(name != NULL);

    const char *dot = SDL_strchr(name, '.');

    if (dot == NULL)
    {
        return false;
    }

    usz length = (usz)(dot - name);

    if (length == 3 && SDL_strncmp(name, "ecs", 3) == 0)
    {
        return true;
    }

    // the plugins are keyed by whole names, so the owner's name is copied
    char *owner = SDL_strndup(name, length);
    ECSIPlugin *plugin = owner == NULL || PLUGINS.plugins == NULL ? NULL : ECSIPlugin_Find(owner);
    SDL_free(owner);
    return plugin != NULL && !plugin->failed;
}

bool ECSIPlugin_OwnsName(ECSPlugin plugin, const char *name)
{
    SDL_assert(plugin != NULL);
    SDL_assert(name != NULL);

    usz prefixLength = SDL_strlen(plugin->name);

    if (SDL_strncmp(name, plugin->name, prefixLength) != 0 || name[prefixLength] != '.' || name[prefixLength + 1] == '\0')
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "'%s' must start with '%s.', the name of the plugin that registers it.", name, plugin->name);
        return false;
    }

    return true;
}

void ECS_Log(ECSPlugin plugin, ECSLogLevel level, const char *format, ...)
{
    SDL_assert(plugin != NULL);
    SDL_assert(level <= ECSLogLevel_Error);
    SDL_assert(format != NULL);

    static const SDL_LogPriority PRIORITIES[] = {
        [ECSLogLevel_Debug] = SDL_LOG_PRIORITY_DEBUG,
        [ECSLogLevel_Info] = SDL_LOG_PRIORITY_INFO,
        [ECSLogLevel_Warning] = SDL_LOG_PRIORITY_WARN,
        [ECSLogLevel_Error] = SDL_LOG_PRIORITY_ERROR,
    };

    // a message the log does not show is not formatted
    if (PRIORITIES[level] < SDL_GetLogPriority(SDL_LOG_CATEGORY_APPLICATION))
    {
        return;
    }

    char *message = NULL;
    va_list arguments;
    va_start(arguments, format);
    int length = SDL_vasprintf(&message, format, arguments);
    va_end(arguments);

    if (length >= 0)
    {
        SDL_LogMessage(SDL_LOG_CATEGORY_APPLICATION, PRIORITIES[level], "[%s] %s", plugin->name, message);
        SDL_free(message);
    }
}
