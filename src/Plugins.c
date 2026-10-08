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
} ECSI_Plugin;

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
} PLUGINS = {0};

static ECSI_Plugin *ECSI_PluginFind(const char *name)
{
    return shget(PLUGINS.plugins, name);
}

static void ECSI_ManifestFree(ECSI_Manifest *manifest)
{
    SDL_free(manifest->folder);
    ECSI_ValueDestroy(&manifest->file);
    SDL_zerop(manifest);
}

static void ECSI_ManifestAddDependency(const char *name, const ECSValue *field, void *userData)
{
    (void)field;
    const char ***dependencies = userData;
    arrput(*dependencies, name);
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

        SHUResult result = ECSI_ValueCreate(&retManifest->file);
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

static SHUResult ECSI_PluginLoad(const char *name);

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

    // the names stay alive in the manifest
    const char **dependencies = NULL;
    ECSI_ValueForEachField(ECSValue_GetField(file, "depends"), ECSI_ManifestAddDependency, &dependencies);
    arrput(PLUGINS.loading, name);

    for (usz i = 0; i < arrlenu(dependencies); i++)
    {
        SHU_ReturnResult(ECSI_PluginLoad(dependencies[i]),
                         (void)arrpop(PLUGINS.loading);
                         SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' is skipped because '%s' failed.", name, dependencies[i]);
                         arrfree(dependencies););
    }

    (void)arrpop(PLUGINS.loading);
    arrfree(dependencies);

    if (ECSValue_GetField(file, "lua") != NULL)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' has Lua code, which is not supported yet; it is ignored.", name);
    }

    ECSI_Plugin plugin = {.name = SDL_strdup(name), .version = SDL_strdup(ECSValue_GetString(ECSValue_GetField(file, "version"), "0.0.0"))};
    ECSI_Plugin *record = SDL_malloc(sizeof(ECSI_Plugin));
    SHUResult (*Init)(ECSPlugin plugin) = NULL;

    if (plugin.name == NULL || plugin.version == NULL || record == NULL)
    {
        SDL_free(record);
        SDL_free(plugin.name);
        SDL_free(plugin.version);
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
            SDL_free(plugin.name);
            SDL_free(plugin.version);
            return SHUResult_ErrFile;
        }

        Init = (SHUResult (*)(ECSPlugin))SDL_LoadFunction(plugin.library, "ECSPlugin_Init");
        plugin.Shutdown = (void (*)(ECSPlugin))SDL_LoadFunction(plugin.library, "ECSPlugin_Shutdown");

        if (Init == NULL)
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' does not export ECSPlugin_Init.", name);
            SDL_UnloadObject(plugin.library);
            SDL_free(record);
            SDL_free(plugin.name);
            SDL_free(plugin.version);
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
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' failed to start (%s).", name, SHUResult_String(result));
        return result;
    }

    SDL_Log("Plugin '%s' %s loaded.", record->name, record->version);
    return SHUResult_Ok;
}

/// @brief Loads a plugin after the plugins it depends on.
static SHUResult ECSI_PluginLoad(const char *name)
{
    if (ECSI_PluginFind(name) != NULL)
    {
        return SHUResult_Ok;
    }

    for (usz i = 0; i < arrlenu(PLUGINS.loading); i++)
    {
        if (SDL_strcmp(PLUGINS.loading[i], name) == 0)
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' depends on itself through its dependencies.", name);
            return SHUResult_ErrBadData;
        }
    }

    ECSI_Manifest manifest;
    SHU_ReturnResult(ECSI_ManifestFind(name, &manifest));

    SHUResult result = ECSI_PluginStart(name, &manifest);
    ECSI_ManifestFree(&manifest);
    return result;
}

#pragma endregion Source Only

SHUResult ECSI_PluginsLoad(const char *const *directories, usz directoryCount, const char *const *names, usz nameCount)
{
    SDL_assert(directories != NULL);
    SDL_assert(names != NULL || nameCount == 0);

    PLUGINS.directories = directories;
    PLUGINS.directoryCount = directoryCount;

    SHUResult result = SHUResult_Ok;

    for (usz i = 0; i < nameCount; i++)
    {
        SHUResult loaded = ECSI_PluginLoad(names[i]);

        if (loaded)
        {
            result = loaded;
        }
    }

    PLUGINS.directories = NULL;
    PLUGINS.directoryCount = 0;
    return result;
}

void ECSI_PluginsUnload(void)
{
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

        SDL_free(plugin->name);
        SDL_free(plugin->version);
        SDL_free(plugin);
    }

    shfree(PLUGINS.plugins);
    arrfree(PLUGINS.loading);
    SDL_zero(PLUGINS);
}

const char *ECSI_PluginGetName(ECSPlugin plugin)
{
    SDL_assert(plugin != NULL);

    return plugin->name;
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
