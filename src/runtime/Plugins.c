#include "runtime/Plugins.h"

#include "base/Lua.h"

#include "SDL3/SDL.h"
#include "stb/stbSDL3.h"

#pragma region Source Only

/// @brief Longest text that names what needs a plugin, in reports.
#define OPENECS_PLUGINS_REPORT_SIZE 1024

/// @brief The file a native plugin's code is in, in its folder: its name with the platform's prefix and extension, such as "libcanvas.so".
#if defined(SDL_PLATFORM_WINDOWS)
#define OPENECS_LIBRARY_FORMAT "%s%s.dll"
#elif defined(SDL_PLATFORM_APPLE)
#define OPENECS_LIBRARY_FORMAT "%slib%s.dylib"
#else
#define OPENECS_LIBRARY_FORMAT "%slib%s.so"
#endif

/// @brief The file a Lua plugin's code starts in, in its folder.
#define OPENECS_LUA_ENTRY "init.lua"

/// @brief What a manifest's fields must be.
static const ECSIFieldRule OPENECS_MANIFEST_RULES[] = {
    {"name", ECSIFieldType_String, true},
    {"version", ECSIFieldType_String, true},
    {"api", ECSIFieldType_Integer, true},
    {"type", ECSIFieldType_String, true},
    {"description", ECSIFieldType_String, false},
    {"depends", ECSIFieldType_Table, false},
};

/// @brief A native plugin's ECSPlugin_Init.
typedef SHUResult (*ECSIPluginInitFunction)(ECSPlugin plugin);

typedef struct ECSIPlugin
{
    char *name;
    char *version;
    char *description;         // from the manifest, or empty
    char *folder;              // ends with a separator
    SDL_SharedObject *library; // nullptr if the plugin has no native code
    bool failed;               // its code failed to start; plugins that depend on it are skipped
    char **dependencies;       // stb_ds array of the names its manifest depends on
    bool registered;           // it registered desc
    ECSPluginDesc desc;        // its own state and its Destroy, from ECSPlugin_Register
    ECSTaskFunction release;   // releases desc's data when the plugin goes away, or nullptr
} ECSIPlugin;

/// @brief State while the plugins of a table are loaded.
typedef struct ECSIPluginLoader
{
    const char *dependent; // the plugin whose dependencies these are, or nullptr for the plugins a preset or user names
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
    const char *const *folders;
    usz folderCount;
    const char *neededBy; // what names the plugins being loaded, for reports
    const char **loading; // stb_ds array of the plugins being loaded, to find dependency cycles
    bool invalid;         // a manifest is not valid or plugins depend on each other in a cycle, so the start fails
    struct
    {
        char *key; // "plugin: message"
        u64 value; // how often it happened
    } *errors;     // stb_ds hash map with copied keys
    ECSIPluginHooks hooks;
} PLUGINS = {0};

static ECSIPlugin *ECSIPlugin_Find(const char *name)
{
    // shget would allocate a map that is missing
    return PLUGINS.plugins == nullptr ? nullptr : shget(PLUGINS.plugins, name);
}

/// @brief Forgets a plugin's description, and releases its data.
static void ECSIPlugin_Forget(ECSIPlugin *plugin)
{
    if (plugin->release != nullptr)
    {
        plugin->release(plugin->desc.data);
    }

    plugin->desc = (ECSPluginDesc){0};
    plugin->registered = false;
    plugin->release = nullptr;
}

static void ECSIPlugin_Free(ECSIPlugin *plugin)
{
    for (usz i = 0; i < arrlenu(plugin->dependencies); i++)
    {
        SDL_free(plugin->dependencies[i]);
    }

    arrfree(plugin->dependencies);
    SDL_free(plugin->name);
    SDL_free(plugin->description);
    SDL_free(plugin->version);
    SDL_free(plugin->folder);
}

/// @brief Gives a plugin folder as a full path, so it stays right if the working folder changes.
/// @return A copy to free with SDL_free, or nullptr if out of memory.
static char *ECSIPlugin_GetAbsoluteFolder(const char *folder)
{
    if (ECSPath_IsAbsolute(folder))
    {
        return SDL_strdup(folder);
    }

    char *current = SDL_GetCurrentDirectory();
    char *full = nullptr;

    if (current == nullptr || SDL_asprintf(&full, "%s%s", current, folder) < 0)
    {
        full = nullptr;
    }

    SDL_free(current);
    return full;
}

/// @brief Copies the name of a dependency into a plugin.
static void ECSIPlugin_AddDependency(const char *name, const ECSValue *field, void *userData)
{
    (void)field;
    ECSIPlugin *plugin = userData;
    char *copy = SDL_strdup(name);

    if (copy != nullptr)
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
        char *end = nullptr;
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
static void ECSIPlugin_DescribeNeeder(SHUSlice retText)
{
    if (arrlenu(PLUGINS.loading) > 0)
    {
        SDL_snprintf(retText.data, retText.size, "plugin '%s'", arrlast(PLUGINS.loading));
    }
    else
    {
        SDL_strlcpy(retText.data, PLUGINS.neededBy, retText.size);
    }
}

/// @brief Reports a plugin that no plugins folder holds: what needs it, and every folder that was looked in.
static void ECSIManifest_ReportMissing(const char *name)
{
    char neededBy[OPENECS_PLUGINS_REPORT_SIZE];
    ECSIPlugin_DescribeNeeder(cs(neededBy, sizeof(neededBy)));
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' is not found. It is needed by %s.", name, neededBy);

    for (usz i = 0; i < PLUGINS.folderCount; i++)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "  Looked for '%s%s/manifest.lua'.", PLUGINS.folders[i], name);
    }
}

/// @brief Reports a manifest that is not valid.
static OPENECS_PRINTF(2, 3) void ECSIManifest_Report(const char *path, const char *format, ...)
{
    char *problem = nullptr;
    va_list arguments;
    va_start(arguments, format);
    int length = SDL_vasprintf(&problem, format, arguments);
    va_end(arguments);
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "The manifest '%s' is not valid: %s.", path, length >= 0 ? problem : format);
    SDL_free(problem);
}

/// @brief The first dependency of a manifest whose version is not a version text, while the dependencies are walked.
typedef struct ECSIDependencyCheck
{
    const char *badName; // nullptr while every version is valid
} ECSIDependencyCheck;

/// @brief Checks a dependency of a manifest: a plugin's name with the lowest version it accepts.
static void ECSIManifest_CheckDependency(const char *name, const ECSValue *field, void *userData)
{
    ECSIDependencyCheck *check = userData;

    if (check->badName == nullptr && !ECSIValue_IsVersion(ECSValue_GetString(field, nullptr)))
    {
        check->badName = name;
    }
}

/// @brief Checks a manifest's structure (DESIGN 9.2), and reports what is wrong.
/// @param name The name of the plugin's folder.
/// @return true if it is valid.
static bool ECSIManifest_Check(const char *name, const char *path, const ECSValue *file)
{
    char *problem = nullptr;

    if (!ECSIValue_CheckFields(file, OPENECS_MANIFEST_RULES, SDL_arraysize(OPENECS_MANIFEST_RULES), &problem))
    {
        ECSIManifest_Report(path, "%s", problem != nullptr ? problem : "out of memory");
        SDL_free(problem);
        return false;
    }

    const char *type = ECSValue_GetString(ECSValue_GetTableField(file, "type"), "");
    const ECSValue *depends = ECSValue_GetTableField(file, "depends");
    ECSIDependencyCheck check = {0};
    ECSIValue_TableForEachField(depends, ECSIManifest_CheckDependency, &check);

    if (SDL_strcmp(ECSValue_GetString(ECSValue_GetTableField(file, "name"), ""), name) != 0)
    {
        ECSIManifest_Report(path, "'name' must be the name of its folder, '%s'", name);
    }
    else if (!ECSIValue_IsVersion(ECSValue_GetString(ECSValue_GetTableField(file, "version"), nullptr)))
    {
        ECSIManifest_Report(path, "'version' must be a version text, such as \"1.2.0\"");
    }
    else if (SDL_strcmp(type, "native") != 0 && SDL_strcmp(type, "lua") != 0)
    {
        ECSIManifest_Report(path, "'type' must be \"native\" or \"lua\"");
    }
    else if (ECSValue_GetListCount(depends) > 0)
    {
        ECSIManifest_Report(path, "'depends' names each plugin with a version, such as { ui = \"0.1\" }");
    }
    else if (check.badName != nullptr)
    {
        ECSIManifest_Report(path, "the version of '%s' in 'depends' must be a version text, such as \"0.1\"", check.badName);
    }
    else
    {
        return true;
    }

    return false;
}

/// @brief Finds a plugin's folder in the plugin folders and reads its manifest.
static SHUResult ECSIManifest_Find(const char *name, ECSIManifest *retManifest)
{
    SDL_zerop(retManifest);

    for (usz i = 0; i < PLUGINS.folderCount; i++)
    {
        char *path = nullptr;

        if (SDL_asprintf(&path, "%s%s/manifest.lua", PLUGINS.folders[i], name) < 0)
        {
            return SHUResult_ErrAllocation;
        }

        if (!SDL_GetPathInfo(path, nullptr))
        {
            SDL_free(path);
            continue;
        }

        SHUResult result = ECSValue_Create(&retManifest->file);
        result = result ? result : ECSILua_ReadData(path, retManifest->file);

        if (!result && !ECSIManifest_Check(name, path, retManifest->file))
        {
            result = SHUResult_ErrBadData;
        }

        if (result == SHUResult_ErrBadData)
        {
            PLUGINS.invalid = true;
        }

        SDL_free(path);
        SHU_ReturnResult(result, ECSIManifest_Free(retManifest););

        if (SDL_asprintf(&retManifest->folder, "%s%s/", PLUGINS.folders[i], name) < 0)
        {
            retManifest->folder = nullptr;
            ECSIManifest_Free(retManifest);
            return SHUResult_ErrAllocation;
        }

        return SHUResult_Ok;
    }

    ECSIManifest_ReportMissing(name);
    return SHUResult_ErrNotFound;
}

static SHUResult ECSIPlugins_LoadAll(const ECSValue *plugins, const char *dependent);

/// @brief Loads a native plugin's library and finds its ECSPlugin_Init.
static SHUResult ECSIPlugin_LoadLibrary(const char *name, const char *folder, SDL_SharedObject **retLibrary, ECSIPluginInitFunction *retInit)
{
    char *path = nullptr;
    *retLibrary = SDL_asprintf(&path, OPENECS_LIBRARY_FORMAT, folder, name) < 0 ? nullptr : SDL_LoadObject(path);
    SDL_free(path);

    if (*retLibrary == nullptr)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot load the library of plugin '%s': %s", name, SDL_GetError());
        return SHUResult_ErrFile;
    }

    *retInit = (ECSIPluginInitFunction)SDL_LoadFunction(*retLibrary, "ECSPlugin_Init");

    if (*retInit == nullptr)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' does not export ECSPlugin_Init.", name);
        SDL_UnloadObject(*retLibrary);
        *retLibrary = nullptr;
        return SHUResult_ErrBadData;
    }

    return SHUResult_Ok;
}

/// @brief Marks a plugin whose code failed to start, removes what it registered and forgets its description; its Destroy does not run, as it cleans up before it fails.
/// @param what What failed, for the report.
/// @return The result it failed with.
static SHUResult ECSIPlugin_Fail(ECSIPlugin *plugin, const char *what, SHUResult result)
{
    plugin->failed = true;
    PLUGINS.hooks.RemoveRegistrations(plugin);
    ECSIPlugin_Forget(plugin);
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' %s (%s).", plugin->name, what, SHUResult_String(result));
    return result;
}

/// @brief Loads the plugins a manifest depends on, then the plugin itself, and runs its ECSPlugin_Init or its Lua code.
static SHUResult ECSIPlugin_Start(const char *name, const ECSIManifest *manifest)
{
    const ECSValue *file = manifest->file;
    i64 api = ECSValue_GetInteger(ECSValue_GetTableField(file, "api"), 0);
    bool native = SDL_strcmp(ECSValue_GetString(ECSValue_GetTableField(file, "type"), ""), "native") == 0;

    if (api != OPENECS_API_VERSION)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' was made for plugin API %" SDL_PRIs64 "; this is version %d.", name, api, OPENECS_API_VERSION);
        return SHUResult_ErrBadData;
    }

    arrput(PLUGINS.loading, name);
    SHUResult dependencies = ECSIPlugins_LoadAll(ECSValue_GetTableField(file, "depends"), name);
    (void)arrpop(PLUGINS.loading);
    SHU_ReturnResult(dependencies);

    // a native plugin's library loads first, so a plugin whose code is missing is not recorded
    SDL_SharedObject *library = nullptr;
    ECSIPluginInitFunction Init = nullptr;

    if (native)
    {
        SHU_ReturnResult(ECSIPlugin_LoadLibrary(name, manifest->folder, &library, &Init));
    }

    ECSIPlugin *plugin = SDL_calloc(1, sizeof(ECSIPlugin));

    if (plugin != nullptr)
    {
        plugin->name = SDL_strdup(name);
        plugin->version = SDL_strdup(ECSValue_GetString(ECSValue_GetTableField(file, "version"), "0.0.0"));
        plugin->description = SDL_strdup(ECSValue_GetString(ECSValue_GetTableField(file, "description"), ""));
        plugin->folder = ECSIPlugin_GetAbsoluteFolder(manifest->folder);
        plugin->library = library;
        ECSIValue_TableForEachField(ECSValue_GetTableField(file, "depends"), ECSIPlugin_AddDependency, plugin);
    }

    if (plugin == nullptr || plugin->name == nullptr || plugin->version == nullptr || plugin->description == nullptr || plugin->folder == nullptr)
    {
        if (plugin != nullptr)
        {
            ECSIPlugin_Free(plugin);
            SDL_free(plugin);
        }

        if (library != nullptr)
        {
            SDL_UnloadObject(library);
        }

        return SHUResult_ErrAllocation;
    }

    // the plugin is recorded before its code runs, so the core knows it while it registers things
    shput(PLUGINS.plugins, plugin->name, plugin);

    if (native)
    {
        SHUResult result = Init(plugin);
        SHU_ReturnResult(result, ECSIPlugin_Fail(plugin, "failed to start", result););
    }
    else
    {
        char *path = nullptr;
        SHUResult result = SDL_asprintf(&path, "%s" OPENECS_LUA_ENTRY, manifest->folder) < 0 ? SHUResult_ErrAllocation : PLUGINS.hooks.StartLua(plugin, manifest->folder, path);
        SDL_free(path);
        SHU_ReturnResult(result, ECSIPlugin_Fail(plugin, "failed to start its Lua code", result););
    }

    SDL_Log("Plugin '%s' %s loaded from '%s'.", plugin->name, plugin->version, manifest->folder);
    return SHUResult_Ok;
}

/// @brief Checks a plugin's version against the minimum that what needs it asks for, and reports a version that does not match.
/// @param minimum The minimum version, or nullptr for any.
static bool ECSIPlugin_CheckVersion(const char *name, const char *version, const char *minimum)
{
    if (minimum == nullptr || ECSIPlugin_VersionMatches(version, minimum))
    {
        return true;
    }

    char neededBy[OPENECS_PLUGINS_REPORT_SIZE];
    ECSIPlugin_DescribeNeeder(cs(neededBy, sizeof(neededBy)));
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' has version %s, but %s needs version %s or a later one with the same major number.", name, version, neededBy, minimum);
    return false;
}

/// @brief Loads a plugin after the plugins it depends on, if it is not loaded yet.
/// @param minimum The minimum version that the caller needs, or nullptr.
static SHUResult ECSIPlugin_Load(const char *name, const char *minimum)
{
    ECSIPlugin *plugin = ECSIPlugin_Find(name);

    if (plugin != nullptr)
    {
        return ECSIPlugin_CheckVersion(name, plugin->version, minimum) && !plugin->failed ? SHUResult_Ok : SHUResult_ErrBadData;
    }

    for (usz i = 0; i < arrlenu(PLUGINS.loading); i++)
    {
        if (SDL_strcmp(PLUGINS.loading[i], name) == 0)
        {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' depends on itself through its dependencies.", name);
            PLUGINS.invalid = true;
            return SHUResult_ErrBadData;
        }
    }

    // the version is checked before the plugin's code runs
    ECSIManifest manifest = {0};
    SHU_ReturnResult(ECSIManifest_Find(name, &manifest));
    const char *version = ECSValue_GetString(ECSValue_GetTableField(manifest.file, "version"), "0.0.0");
    SHUResult result = ECSIPlugin_CheckVersion(name, version, minimum) ? ECSIPlugin_Start(name, &manifest) : SHUResult_ErrBadData;
    ECSIManifest_Free(&manifest);
    return result;
}

static void ECSIPlugin_LoadField(const char *name, const ECSValue *field, void *userData)
{
    ECSIPluginLoader *loader = userData;

    // a dependency that fails skips the plugin that needs it, so the rest is not loaded for it
    if (loader->dependent != nullptr && loader->result)
    {
        return;
    }

    SHUResult result = ECSIPlugin_Load(name, ECSValue_GetString(field, nullptr));

    if (result)
    {
        loader->result = result;

        if (loader->dependent != nullptr)
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' is skipped because '%s' failed.", loader->dependent, name);
        }
    }
}

/// @brief Loads the plugins that a table names: named fields with a minimum version, and list items with a name only.
/// @param dependent The plugin whose dependencies these are; it stops at the first failure. nullptr loads every plugin it can.
static SHUResult ECSIPlugins_LoadAll(const ECSValue *plugins, const char *dependent)
{
    ECSIPluginLoader loader = {.dependent = dependent, .result = SHUResult_Ok};
    ECSIValue_TableForEachField(plugins, ECSIPlugin_LoadField, &loader);

    for (usz i = 0; i < ECSValue_GetListCount(plugins); i++)
    {
        const char *name = ECSValue_GetString(ECSValue_GetListItem(plugins, i), nullptr);

        if (name != nullptr)
        {
            ECSIPlugin_LoadField(name, nullptr, &loader);
        }
    }

    return loader.result;
}

#pragma endregion Source Only

SHUResult ECSIPlugins_Load(const char *const *folders, usz folderCount, const ECSValue *plugins, const char *neededBy)
{
    SDL_assert(folders != nullptr);
    SDL_assert(neededBy != nullptr);

    PLUGINS.folders = folders;
    PLUGINS.folderCount = folderCount;
    PLUGINS.neededBy = neededBy;
    SHUResult result = ECSIPlugins_LoadAll(plugins, nullptr);
    PLUGINS.folders = nullptr;
    PLUGINS.folderCount = 0;
    PLUGINS.neededBy = nullptr;

    if (PLUGINS.invalid)
    {
        return SHUResult_ErrBadData;
    }

    return result ? SHUResult_Err : SHUResult_Ok;
}

void ECSIPlugins_Destroy(void)
{
    for (usz i = shlenu(PLUGINS.plugins); i > 0; i--)
    {
        ECSIPlugin *plugin = PLUGINS.plugins[i - 1].value;

        if (plugin->desc.Destroy != nullptr)
        {
            plugin->desc.Destroy(plugin->desc.data);
        }

        plugin->desc.Destroy = nullptr;
    }
}

void ECSIPlugins_ReportRepeatedErrors(void)
{
    for (usz i = 0; i < shlenu(PLUGINS.errors); i++)
    {
        if (PLUGINS.errors[i].value > 1)
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "This error happened %" SDL_PRIu64 " times: %s", PLUGINS.errors[i].value, PLUGINS.errors[i].key);
        }
    }
}

void ECSIPlugins_Unload(void)
{
    for (usz i = shlenu(PLUGINS.plugins); i > 0; i--)
    {
        ECSIPlugin *plugin = PLUGINS.plugins[i - 1].value;

        ECSIPlugin_Forget(plugin);

        if (plugin->library != nullptr)
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
    SDL_assert(hooks != nullptr && hooks->StartLua != nullptr && hooks->RemoveRegistrations != nullptr);

    PLUGINS.hooks = *hooks;
}

const char *ECSIPlugin_GetVersion(ECSPlugin plugin)
{
    SDL_assert(plugin != nullptr);

    return plugin->version;
}

const char *ECSIPlugin_GetName(ECSPlugin plugin)
{
    SDL_assert(plugin != nullptr);

    return plugin->name;
}

const char *ECSIPlugin_GetDescription(ECSPlugin plugin)
{
    SDL_assert(plugin != nullptr);

    return plugin->description;
}

SHUResult ECSIPlugin_Register(ECSPlugin plugin, const ECSPluginDesc *desc, ECSTaskFunction release)
{
    SDL_assert(plugin != nullptr);
    SDL_assert(desc != nullptr);

    if ((desc->SaveState == nullptr) != (desc->RestoreState == nullptr) || plugin->registered)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' registers itself with only one of SaveState and RestoreState, or a second time.", plugin->name);
        return SHUResult_ErrBadData;
    }

    plugin->desc = *desc;
    plugin->registered = true;
    plugin->release = release;
    return SHUResult_Ok;
}

const char *ECSPlugin_GetFolder(ECSPlugin plugin)
{
    SDL_assert(plugin != nullptr);

    return plugin->folder;
}

SHUResult ECSPlugin_Register(ECSPlugin plugin, const ECSPluginDesc *desc)
{
    return ECSIPlugin_Register(plugin, desc, nullptr);
}

void ECSIPlugins_RestoreStates(const ECSValue *states)
{
    for (usz i = 0; i < shlenu(PLUGINS.plugins); i++)
    {
        ECSIPlugin *plugin = PLUGINS.plugins[i].value;
        const ECSValue *entry = ECSValue_GetTableField(states, plugin->name);

        if (plugin->desc.RestoreState == nullptr || entry == nullptr)
        {
            continue;
        }

        i64 version = ECSValue_GetInteger(ECSValue_GetTableField(entry, "stateVersion"), 0);

        if (plugin->desc.RestoreState(plugin->desc.data, ECSValue_GetTableField(entry, "state"), version >= 0 && version <= SDL_MAX_UINT32 ? (u32)version : 0))
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' could not restore its state from the session.", plugin->name);
        }
    }
}

SHUResult ECSIPlugins_SaveStates(ECSValue *states)
{
    SDL_assert(states != nullptr);

    // the session's entries stay, so a plugin whose save fails keeps the state the session had
    if (ECSValue_GetType(states) != ECSValueType_Table)
    {
        ECSValue_SetTable(states);
    }

    for (usz i = 0; i < shlenu(PLUGINS.plugins); i++)
    {
        ECSIPlugin *plugin = PLUGINS.plugins[i].value;
        ECSValue *saved = nullptr;
        ECSValue *entry = nullptr;
        ECSValue *field = nullptr;

        if (plugin->desc.SaveState == nullptr)
        {
            continue;
        }

        SHU_ReturnResult(ECSValue_Create(&saved));

        if (plugin->desc.SaveState(plugin->desc.data, saved))
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' could not save its state.", plugin->name);
            ECSValue_Destroy(&saved);
            continue;
        }

        SHUResult result = ECSValue_SetTableField(states, plugin->name, &entry);

        if (!result)
        {
            ECSValue_SetTable(entry);
            result = ECSValue_SetTableField(entry, "state", &field);
        }

        result = result ? result : ECSIValue_Copy(field, saved);
        result = result ? result : ECSValue_SetTableField(entry, "stateVersion", &field);

        if (!result)
        {
            ECSValue_SetInteger(field, plugin->desc.stateVersion);
        }

        ECSValue_Destroy(&saved);
        SHU_ReturnResult(result);
    }

    return SHUResult_Ok;
}

void ECSIPlugin_ReportError(ECSPlugin plugin, const char *message)
{
    SDL_assert(plugin != nullptr);
    SDL_assert(message != nullptr);

    char *key = nullptr;

    if (SDL_asprintf(&key, "%s: %s", plugin->name, message) < 0)
    {
        return;
    }

    if (PLUGINS.errors == nullptr)
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
    SDL_assert(plugin != nullptr);
    SDL_assert(other != nullptr);

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
    SDL_assert(name != nullptr);

    const char *dot = SDL_strchr(name, '.');

    if (dot == nullptr)
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
    ECSIPlugin *plugin = owner == nullptr ? nullptr : ECSIPlugin_Find(owner);
    SDL_free(owner);
    return plugin != nullptr && !plugin->failed;
}

char *ECSIPlugin_MakeFullName(ECSPlugin plugin, const char *localName)
{
    SDL_assert(plugin != nullptr);

    usz prefixLength = SDL_strlen(plugin->name);

    if (localName == nullptr || localName[0] == '\0')
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' registers something without a name.", plugin->name);
        return nullptr;
    }

    // the core adds the plugin's name, so a name that has it already is a mistake
    if (SDL_strncmp(localName, plugin->name, prefixLength) == 0 && localName[prefixLength] == '.')
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "'%s' is given without '%s.', which the core adds, so it is '%s.%s'.", localName, plugin->name, plugin->name, localName);
        return nullptr;
    }

    char *name = nullptr;
    return SDL_asprintf(&name, "%s.%s", plugin->name, localName) < 0 ? nullptr : name;
}

bool ECSIPlugin_OwnsName(ECSPlugin plugin, const char *name)
{
    SDL_assert(plugin != nullptr);
    SDL_assert(name != nullptr);

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
    SDL_assert(plugin != nullptr);
    SDL_assert(level <= ECSLogLevel_Error);
    SDL_assert(format != nullptr);

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

    char *message = nullptr;
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

ECSPlugin ECSIPlugins_Get(const char *name)
{
    SDL_assert(name != nullptr);

    ECSIPlugin *plugin = ECSIPlugin_Find(name);
    return plugin == nullptr || plugin->failed ? nullptr : plugin;
}

SHUResult ECSIPlugins_ForEach(ECSIPluginsVisitFunction Visit, void *data)
{
    SDL_assert(Visit != nullptr);

    SHUResult result = SHUResult_Ok;

    for (usz i = 0; i < shlenu(PLUGINS.plugins); i++)
    {
        if (!PLUGINS.plugins[i].value->failed)
        {
            SHUResult visited = Visit(PLUGINS.plugins[i].value, data);
            result = result ? result : visited;
        }
    }

    return result;
}
