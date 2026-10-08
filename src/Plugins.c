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

/// @brief What a manifest says, copied before the manifest is closed.
typedef struct ECSI_Manifest
{
    char *folder;  // the plugin's folder, ending with a separator
    char *version;
    char *native;  // file name of the native library, or NULL
    char **dependencies; // stb_ds array
    u32 api;
    bool hasLua;
    bool incomplete; // a text could not be copied
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
    SDL_free(manifest->version);
    SDL_free(manifest->native);

    for (usz i = 0; i < arrlenu(manifest->dependencies); i++)
    {
        SDL_free(manifest->dependencies[i]);
    }

    arrfree(manifest->dependencies);
    SDL_zerop(manifest);
}

static void ECSI_ManifestAddDependency(const char *key, const char *value, void *userData)
{
    (void)value;
    ECSI_Manifest *manifest = userData;
    char *dependency = SDL_strdup(key);

    if (dependency == NULL)
    {
        manifest->incomplete = true;
        return;
    }

    arrput(manifest->dependencies, dependency);
}

/// @brief Copies what the open manifest says. Its texts are valid only while it is open.
static void ECSI_ManifestRead(const char *directory, const char *name, ECSI_Manifest *retManifest)
{
    const char *native = ECSI_LuaDataGetText("native", NULL);

    if (SDL_asprintf(&retManifest->folder, "%s%s/", directory, name) < 0)
    {
        retManifest->incomplete = true;
    }

    retManifest->version = SDL_strdup(ECSI_LuaDataGetText("version", "0.0.0"));
    retManifest->native = native == NULL ? NULL : SDL_strdup(native);
    retManifest->api = (u32)ECSI_LuaDataGetNumber("api", 0);
    retManifest->hasLua = ECSI_LuaDataHas("lua");

    if (retManifest->version == NULL || (native != NULL && retManifest->native == NULL))
    {
        retManifest->incomplete = true;
    }

    if (ECSI_LuaDataEnterField("depends"))
    {
        ECSI_LuaDataForEachText(ECSI_ManifestAddDependency, retManifest);
        ECSI_LuaDataLeave();
    }
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

        SHUResult result = ECSI_LuaDataOpen(path);
        SDL_free(path);
        SHU_ReturnResult(result);

        bool nameMatches = SDL_strcmp(ECSI_LuaDataGetText("name", ""), name) == 0;
        ECSI_ManifestRead(PLUGINS.directories[i], name, retManifest);
        ECSI_LuaDataClose();

        if (!nameMatches)
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "The manifest of plugin '%s' gives another name.", name);
            ECSI_ManifestFree(retManifest);
            return SHUResult_ErrBadData;
        }

        if (retManifest->incomplete)
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "The manifest of plugin '%s' cannot be read whole.", name);
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
    if (manifest->api != OPENECS_API_VERSION)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' was made for plugin API %u; this is version %d.", name, manifest->api, OPENECS_API_VERSION);
        return SHUResult_ErrBadData;
    }

    arrput(PLUGINS.loading, name);

    for (usz i = 0; i < arrlenu(manifest->dependencies); i++)
    {
        SHU_ReturnResult(ECSI_PluginLoad(manifest->dependencies[i]),
                         (void)arrpop(PLUGINS.loading);
                         SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' is skipped because '%s' failed.", name, manifest->dependencies[i]););
    }

    (void)arrpop(PLUGINS.loading);

    if (manifest->hasLua)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' has Lua code, which is not supported yet; it is ignored.", name);
    }

    ECSI_Plugin plugin = {.name = SDL_strdup(name), .version = SDL_strdup(manifest->version)};
    ECSI_Plugin *record = SDL_malloc(sizeof(ECSI_Plugin));
    SHUResult (*Init)(ECSPlugin plugin) = NULL;

    if (plugin.name == NULL || plugin.version == NULL || record == NULL)
    {
        SDL_free(record);
        SDL_free(plugin.name);
        SDL_free(plugin.version);
        return SHUResult_ErrAllocation;
    }

    if (manifest->native != NULL)
    {
        char *path = NULL;

        if (SDL_asprintf(&path, "%s%s", manifest->folder, manifest->native) >= 0)
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
    SDL_assert(names != NULL);

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
