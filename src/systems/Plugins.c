#include "systems/Plugins.h"

#include "tools/Lua.h"
#include "tools/Platform.h"
#include "systems/Panels.h"

#pragma region Source Only

/// @brief Most dependencies of one plugin.
#define OPENECS_MAX_DEPENDENCIES 32

struct ECSI_Plugin
{
    struct ECSContextTable context; // first, so the plugin's context is also the plugin
    char name[OPENECS_NAME_CAPACITY];
    char version[OPENECS_NAME_CAPACITY];
    void *library;
    const ECSPluginInfo *info;
};

/// @brief What a manifest says, copied before the manifest is closed.
typedef struct ECSI_Manifest
{
    char version[OPENECS_NAME_CAPACITY];
    char native[OPENECS_NAME_CAPACITY];
    char dependencies[OPENECS_MAX_DEPENDENCIES][OPENECS_NAME_CAPACITY];
    usz dependencyCount;
    u32 api;
    bool hasLua;
} ECSI_Manifest;

static struct
{
    ECSI_Plugin plugins[OPENECS_MAX_PLUGINS];
    usz count;
    const char *const *directories;
    usz directoryCount;
    const char *loading[OPENECS_MAX_PLUGINS]; // plugins being loaded, to find dependency cycles
    usz loadingCount;
} PLUGINS = {0};

#pragma region Context

static void ECSI_PluginLog(ECSContext context, ECSLogLevel level, const char *message)
{
    const ECSI_Plugin *plugin = (const ECSI_Plugin *)context;

    switch (level)
    {
    case ECSLogLevel_Info:
        SHU_LogInfo("[%s] %s", plugin->name, message);
        break;
    case ECSLogLevel_Warning:
        SHU_LogWarning("[%s] %s", plugin->name, message);
        break;
    default:
        SHU_LogError(SHUResult_Ok, "[%s] %s", plugin->name, message);
        break;
    }
}

static SHUResult ECSI_PluginPanelTypeRegister(ECSContext context, const ECSPanelTypeDesc *desc)
{
    ECSI_Plugin *plugin = (ECSI_Plugin *)context;
    return ECSI_PanelTypeRegister(plugin, plugin->name, desc);
}

/// @brief The core functions every plugin calls through. Each plugin gets a copy as its context.
static const struct ECSContextTable ECSI_CONTEXT_TABLE = {
    .structSize = sizeof(struct ECSContextTable),
    .Log = ECSI_PluginLog,
    .PanelTypeRegister = ECSI_PluginPanelTypeRegister,
    .PanelRedraw = ECSI_PanelRedraw,
    .PanelGetTitle = ECSI_PanelGetTitle,
    .PanelSetTitle = ECSI_PanelSetTitle,
};

#pragma endregion Context

static ECSI_Plugin *ECSI_PluginFind(const char *name)
{
    for (usz i = 0; i < PLUGINS.count; i++)
    {
        if (strcmp(PLUGINS.plugins[i].name, name) == 0)
        {
            return &PLUGINS.plugins[i];
        }
    }

    return NULL;
}

static void ECSI_PluginAddDependency(const char *key, const char *value, void *userData)
{
    (void)value;
    ECSI_Manifest *manifest = userData;

    if (manifest->dependencyCount < OPENECS_MAX_DEPENDENCIES)
    {
        ECSI_TextCopy(cs(manifest->dependencies[manifest->dependencyCount], OPENECS_NAME_CAPACITY), key);
        manifest->dependencyCount++;
    }
}

/// @brief Finds a plugin's folder in the plugin directories and reads its manifest.
static SHUResult ECSI_PluginReadManifest(const char *name, SHUSlice retFolder, ECSI_Manifest *retManifest)
{
    char path[OPENECS_PATH_CAPACITY];

    for (usz i = 0; i < PLUGINS.directoryCount; i++)
    {
        snprintf(path, sizeof(path), "%s%s/manifest.lua", PLUGINS.directories[i], name);
        FILE *file = fopen(path, "r");

        if (file == NULL)
        {
            continue;
        }

        fclose(file);
        snprintf(retFolder.data, retFolder.size, "%s%s/", PLUGINS.directories[i], name);
        SHU_ReturnResult(ECSI_LuaDataOpen(path));

        *retManifest = (ECSI_Manifest){0};
        ECSI_TextCopy(cs(retManifest->version, sizeof(retManifest->version)), ECSI_LuaDataGetText("version", "0.0.0"));
        ECSI_TextCopy(cs(retManifest->native, sizeof(retManifest->native)), ECSI_LuaDataGetText("native", ""));
        retManifest->api = (u32)ECSI_LuaDataGetNumber("api", 0);
        retManifest->hasLua = ECSI_LuaDataHas("lua");

        bool nameMatches = strcmp(ECSI_LuaDataGetText("name", ""), name) == 0;

        if (ECSI_LuaDataEnterField("depends"))
        {
            ECSI_LuaDataForEachText(ECSI_PluginAddDependency, retManifest);
            ECSI_LuaDataLeave();
        }

        ECSI_LuaDataClose();

        if (!nameMatches)
        {
            SHU_LogWarning("The manifest of plugin '%s' gives another name.", name);
            return SHUResult_ErrBadData;
        }

        return SHUResult_Ok;
    }

    SHU_LogWarning("Plugin '%s' is not found in any plugin directory.", name);
    return SHUResult_ErrNotFound;
}

/// @brief Loads a plugin after the plugins it depends on.
static SHUResult ECSI_PluginLoad(const char *name)
{
    if (ECSI_PluginFind(name) != NULL)
    {
        return SHUResult_Ok;
    }

    for (usz i = 0; i < PLUGINS.loadingCount; i++)
    {
        if (strcmp(PLUGINS.loading[i], name) == 0)
        {
            SHU_LogWarning("Plugin '%s' depends on itself through its dependencies.", name);
            return SHUResult_ErrBadData;
        }
    }

    if (PLUGINS.count == OPENECS_MAX_PLUGINS || PLUGINS.loadingCount == OPENECS_MAX_PLUGINS)
    {
        return SHUResult_ErrOverflow;
    }

    char folder[OPENECS_PATH_CAPACITY];
    ECSI_Manifest manifest;
    SHU_ReturnResult(ECSI_PluginReadManifest(name, cs(folder, sizeof(folder)), &manifest));

    if (manifest.api != OPENECS_API_VERSION)
    {
        SHU_LogWarning("Plugin '%s' was made for plugin API %u; this is version %d.", name, manifest.api, OPENECS_API_VERSION);
        return SHUResult_ErrBadData;
    }

    PLUGINS.loading[PLUGINS.loadingCount++] = name;

    for (usz i = 0; i < manifest.dependencyCount; i++)
    {
        SHU_ReturnResult(ECSI_PluginLoad(manifest.dependencies[i]),
                         PLUGINS.loadingCount--;
                         SHU_LogWarning("Plugin '%s' is skipped because '%s' failed.", name, manifest.dependencies[i]););
    }

    PLUGINS.loadingCount--;

    if (manifest.hasLua)
    {
        SHU_LogWarning("Plugin '%s' has Lua code, which is not supported yet; it is ignored.", name);
    }

    if (manifest.native[0] == '\0')
    {
        return SHUResult_Ok;
    }

    char path[OPENECS_PATH_CAPACITY];

    if (snprintf(path, sizeof(path), "%s%s", folder, manifest.native) >= (int)sizeof(path))
    {
        SHU_LogWarning("The library path of plugin '%s' is too long.", name);
        return SHUResult_ErrOverflow;
    }

    void *library = NULL;
    SHU_ReturnResult(ECSI_PlatformLibraryOpen(&library, path));

    typedef const ECSPluginInfo *(*ECSI_PluginMain)(void);
    ECSI_PluginMain main = (ECSI_PluginMain)ECSI_PlatformLibraryGetFunction(library, "ECSPlugin_Main");
    const ECSPluginInfo *info = main == NULL ? NULL : main();

    if (info == NULL || info->structSize < sizeof(ECSPluginInfo) || info->apiVersion != OPENECS_API_VERSION || info->Init == NULL)
    {
        SHU_LogWarning("Plugin '%s' does not export a valid ECSPlugin_Main.", name);
        ECSI_PlatformLibraryClose(&library);
        return SHUResult_ErrBadData;
    }

    ECSI_Plugin *plugin = &PLUGINS.plugins[PLUGINS.count++];
    plugin->context = ECSI_CONTEXT_TABLE;
    plugin->library = library;
    plugin->info = info;
    ECSI_TextCopy(cs(plugin->name, sizeof(plugin->name)), name);
    ECSI_TextCopy(cs(plugin->version, sizeof(plugin->version)), manifest.version);

    SHUResult result = info->Init(&plugin->context);

    if (result)
    {
        SHU_LogWarning("Plugin '%s' failed to start (%s).", name, SHUResult_String(result));
        return result;
    }

    SHU_LogInfo("Plugin '%s' %s loaded.", plugin->name, plugin->version);
    return SHUResult_Ok;
}

#pragma endregion Source Only

SHUResult ECSI_PluginsLoad(const char *const *directories, usz directoryCount, const char *const *names, usz nameCount)
{
    SHU_AssertNullPointer(directories);
    SHU_AssertNullPointer(names);

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
    for (usz i = PLUGINS.count; i > 0; i--)
    {
        ECSI_Plugin *plugin = &PLUGINS.plugins[i - 1];

        if (plugin->info->Shutdown != NULL)
        {
            plugin->info->Shutdown(&plugin->context);
        }

        ECSI_PlatformLibraryClose(&plugin->library);
    }

    PLUGINS = (typeof(PLUGINS)){0};
}
