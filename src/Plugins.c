#include "Plugins.h"

#include "Lua.h"

#include "SDL3/SDL.h"
#include "stb/stbSDL3.h"

#pragma region Source Only

typedef struct ECSI_Plugin
{
    char *name;
    char *version;
    SDL_SharedObject *library; // NULL if the plugin has no native code
    void (*Shutdown)(ECSPlugin plugin);
    bool failed; // its ECSPlugin_Init failed; plugins that depend on it are skipped
    char **dependencies; // stb_ds array of the names its manifest depends on
} ECSI_Plugin;

/// @brief State while the plugins of a table are loaded.
typedef struct ECSI_PluginLoader
{
    const char *dependent; // the plugin whose dependencies these are, or NULL for the plugins a preset or user names
    SHUResult result;
} ECSI_PluginLoader;

/// @brief A plugin's manifest.
typedef struct ECSI_Manifest
{
    char *folder;   // the plugin's folder, ending with a separator
    ECSValue *file; // the whole manifest
} ECSI_Manifest;

static struct
{
    struct
    {
        char *key; // the plugin's own copy of its name
        ECSI_Plugin *value;
    } *plugins; // stb_ds hash map in load order, because nothing is deleted from it; handles point to the plugins
    const char *const *directories;
    usz directoryCount;
    const char **loading; // stb_ds array of the plugins being loaded, to find dependency cycles
    struct
    {
        char *key; // "plugin: message"
        u64 value; // how often it happened
    } *errors; // stb_ds hash map with copied keys
    ECSI_PluginHooks hooks;
} PLUGINS = {0};

static ECSI_Plugin *ECSI_PluginFind(const char *name)
{
    return shget(PLUGINS.plugins, name);
}

static void ECSI_PluginFree(ECSI_Plugin *plugin)
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
static void ECSI_PluginAddDependency(const char *name, const ECSValue *field, void *userData)
{
    (void)field;
    ECSI_Plugin *plugin = userData;
    char *copy = SDL_strdup(name);

    if (copy != NULL)
    {
        arrput(plugin->dependencies, copy);
    }
}

static void ECSI_ManifestFree(ECSI_Manifest *manifest)
{
    SDL_free(manifest->folder);
    ECSValue_Destroy(&manifest->file);
    SDL_zerop(manifest);
}

/// @brief Reads a version text, such as "1.2.0" or "1.2"; missing numbers are 0.
static void ECSI_PluginReadVersion(const char *text, u64 retParts[3])
{
    for (usz i = 0; i < 3; i++)
    {
        char *end = NULL;
        retParts[i] = SDL_strtoull(text, &end, 10);
        text = *end == '.' ? end + 1 : end;
    }
}

/// @brief Checks a version against a minimum version: the minimum or a later version with the same major number.
static bool ECSI_PluginVersionMatches(const char *version, const char *minimum)
{
    u64 have[3];
    u64 need[3];
    ECSI_PluginReadVersion(version, have);
    ECSI_PluginReadVersion(minimum, need);

    return have[0] == need[0] && (have[1] > need[1] || (have[1] == need[1] && have[2] >= need[2]));
}

/// @brief Finds a plugin's folder in the plugin directories and reads its manifest.
static SHUResult ECSI_ManifestFind(const char *name, ECSI_Manifest *retManifest)
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
        result = result ? result : ECSI_LuaReadData(path, retManifest->file);
        SDL_free(path);
        SHU_ReturnResult(result, ECSI_ManifestFree(retManifest););

        if (SDL_strcmp(ECSValue_GetString(ECSValue_GetField(retManifest->file, "name"), ""), name) != 0)
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "The manifest of plugin '%s' gives another name.", name);
            ECSI_ManifestFree(retManifest);
            return SHUResult_ErrBadData;
        }

        if (SDL_asprintf(&retManifest->folder, "%s%s/", PLUGINS.directories[i], name) < 0)
        {
            retManifest->folder = NULL;
            ECSI_ManifestFree(retManifest);
            return SHUResult_ErrAllocation;
        }

        return SHUResult_Ok;
    }

    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' is not found in any plugin directory.", name);
    return SHUResult_ErrNotFound;
}

static SHUResult ECSI_PluginLoadAll(const ECSValue *plugins, const char *dependent);

/// @brief Loads the plugins a manifest depends on, then the plugin itself, and runs its ECSPlugin_Init.
static SHUResult ECSI_PluginStart(const char *name, const ECSI_Manifest *manifest)
{
    const ECSValue *file = manifest->file;
    i64 api = ECSValue_GetInteger(ECSValue_GetField(file, "api"), 0);
    const char *native = ECSValue_GetString(ECSValue_GetField(file, "native"), NULL);

    if (api != OPENECS_API_VERSION)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' was made for plugin API %" SDL_PRIs64 "; this is version %d.", name, api, OPENECS_API_VERSION);
        return SHUResult_ErrBadData;
    }

    arrput(PLUGINS.loading, name);
    SHUResult dependencies = ECSI_PluginLoadAll(ECSValue_GetField(file, "depends"), name);
    (void)arrpop(PLUGINS.loading);
    SHU_ReturnResult(dependencies);


    ECSI_Plugin plugin = {.name = SDL_strdup(name), .version = SDL_strdup(ECSValue_GetString(ECSValue_GetField(file, "version"), "0.0.0"))};
    ECSI_Plugin *record = SDL_malloc(sizeof(ECSI_Plugin));
    SHUResult (*Init)(ECSPlugin plugin) = NULL;

    ECSI_ValueForEachField(ECSValue_GetField(file, "depends"), ECSI_PluginAddDependency, &plugin);

    if (plugin.name == NULL || plugin.version == NULL || record == NULL)
    {
        SDL_free(record);
        ECSI_PluginFree(&plugin);
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
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cannot load the library of plugin '%s': %s", name, SDL_GetError());
            SDL_free(record);
            ECSI_PluginFree(&plugin);
            return SHUResult_ErrFile;
        }

        Init = (SHUResult (*)(ECSPlugin))SDL_LoadFunction(plugin.library, "ECSPlugin_Init");
        plugin.Shutdown = (void (*)(ECSPlugin))SDL_LoadFunction(plugin.library, "ECSPlugin_Shutdown");

        if (Init == NULL)
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' does not export ECSPlugin_Init.", name);
            SDL_UnloadObject(plugin.library);
            SDL_free(record);
            ECSI_PluginFree(&plugin);
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
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' failed to start (%s).", name, SHUResult_String(result));
        return result;
    }

    // the native code starts first, then the Lua code
    const char *lua = ECSValue_GetString(ECSValue_GetField(file, "lua"), NULL);

    if (lua != NULL)
    {
        char *path = NULL;
        result = SDL_asprintf(&path, "%s%s", manifest->folder, lua) < 0 ? SHUResult_ErrAllocation : PLUGINS.hooks.StartLua(record, path);
        SDL_free(path);

        if (result)
        {
            record->failed = true;
            PLUGINS.hooks.RemoveRegistrations(record);
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' failed to start its Lua code (%s).", name, SHUResult_String(result));
            return result;
        }
    }

    SDL_Log("Plugin '%s' %s loaded.", record->name, record->version);
    return SHUResult_Ok;
}

/// @brief Loads a plugin after the plugins it depends on, if it is not loaded yet.
/// @param minimum The minimum version that the caller needs, or NULL.
static SHUResult ECSI_PluginLoad(const char *name, const char *minimum)
{
    ECSI_Plugin *plugin = ECSI_PluginFind(name);
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

    ECSI_Manifest manifest = {0};

    if (plugin == NULL)
    {
        SHU_ReturnResult(ECSI_ManifestFind(name, &manifest));
        version = ECSValue_GetString(ECSValue_GetField(manifest.file, "version"), "0.0.0");
    }

    // the version is checked before the plugin's code runs
    if (minimum != NULL && !ECSI_PluginVersionMatches(version, minimum))
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' has version %s, but version %s or a later one with the same major number is needed.", name, version, minimum);
        ECSI_ManifestFree(&manifest);
        return SHUResult_ErrBadData;
    }

    if (plugin != NULL)
    {
        return plugin->failed ? SHUResult_ErrBadData : SHUResult_Ok;
    }

    SHUResult result = ECSI_PluginStart(name, &manifest);
    ECSI_ManifestFree(&manifest);
    return result;
}

static void ECSI_PluginLoadField(const char *name, const ECSValue *field, void *userData)
{
    ECSI_PluginLoader *loader = userData;

    // a dependency that fails skips the plugin that needs it, so the rest is not loaded for it
    if (loader->dependent != NULL && loader->result)
    {
        return;
    }

    SHUResult result = ECSI_PluginLoad(name, ECSValue_GetString(field, NULL));

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
static SHUResult ECSI_PluginLoadAll(const ECSValue *plugins, const char *dependent)
{
    ECSI_PluginLoader loader = {.dependent = dependent, .result = SHUResult_Ok};
    ECSI_ValueForEachField(plugins, ECSI_PluginLoadField, &loader);

    for (usz i = 0; i < ECSValue_GetCount(plugins); i++)
    {
        const char *name = ECSValue_GetString(ECSValue_GetItem(plugins, i), NULL);

        if (name != NULL)
        {
            ECSI_PluginLoadField(name, NULL, &loader);
        }
    }

    return loader.result;
}

#pragma endregion Source Only

SHUResult ECSI_PluginsLoad(const char *const *directories, usz directoryCount, const ECSValue *plugins)
{
    SDL_assert(directories != NULL);

    PLUGINS.directories = directories;
    PLUGINS.directoryCount = directoryCount;
    SHUResult result = ECSI_PluginLoadAll(plugins, NULL);
    PLUGINS.directories = NULL;
    PLUGINS.directoryCount = 0;
    return result;
}

void ECSI_PluginsUnload(void)
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
        ECSI_Plugin *plugin = PLUGINS.plugins[i - 1].value;

        if (plugin->Shutdown != NULL)
        {
            plugin->Shutdown(plugin);
        }

        if (plugin->library != NULL)
        {
            SDL_UnloadObject(plugin->library);
        }

        ECSI_PluginFree(plugin);
        SDL_free(plugin);
    }

    shfree(PLUGINS.plugins);
    shfree(PLUGINS.errors);
    arrfree(PLUGINS.loading);
    SDL_zero(PLUGINS);
}

void ECSI_PluginsSetHooks(const ECSI_PluginHooks *hooks)
{
    SDL_assert(hooks != NULL && hooks->StartLua != NULL && hooks->RemoveRegistrations != NULL);

    PLUGINS.hooks = *hooks;
}

const char *ECSI_PluginGetVersion(ECSPlugin plugin)
{
    SDL_assert(plugin != NULL);

    return plugin->version;
}

const char *ECSI_PluginGetName(ECSPlugin plugin)
{
    SDL_assert(plugin != NULL);

    return plugin->name;
}

void ECSI_PluginReportError(ECSPlugin plugin, const char *message)
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

bool ECSI_PluginDependsOn(ECSPlugin plugin, ECSPlugin other)
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

bool ECSI_PluginOwnsName(ECSPlugin plugin, const char *name)
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
