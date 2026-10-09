#include "interface/Keys.h"

#include "interface/Layout.h"
#include "runtime/Plugins.h"
#include "runtime/Services.h"
#include "runtime/Settings.h"

#include "SDL3/SDL.h"
#include "stb/stbSDL3.h"

#pragma region Source Only

/// @brief The field of a keys table that holds the keys after the core prefix.
#define OPENECS_KEYS_PREFIX "prefix"

/// @brief A default key that a plugin bound for one of its panel types.
typedef struct ECSIPanelBinding
{
    ECSPlugin plugin;
    char *panelType;
    char *key;
    char *function;
} ECSIPanelBinding;

/// @brief The layers of keys tables, lowest first (DESIGN 7.8).
typedef enum ECSIKeysLayer
{
    ECSIKeysLayer_Default = 0,
    ECSIKeysLayer_Preset,
    ECSIKeysLayer_User,
} ECSIKeysLayer;

/// @brief How specific a binding is; within one layer, a more specific binding wins.
typedef enum ECSIBindingScope
{
    ECSIBindingScope_Tool = 0,
    ECSIBindingScope_Workspace,
    ECSIBindingScope_PanelType,
    ECSIBindingScope_WorkspacePanelType,
} ECSIBindingScope;

/// @brief The binding that a key press runs, found by ECSIKeys_FindFor.
typedef struct ECSIBindingSearch
{
    u32 key;
    u32 modifiers;
    ECSIKeysLayer layer;     // of the table being searched
    ECSIBindingScope scope;  // of the table being searched
    bool found;              // a binding matched; its function may be NULL, when false removes the key
    const char *function;    // the best binding so far
    ECSIKeysLayer bestLayer;
    ECSIBindingScope bestScope;
} ECSIBindingSearch;

static struct
{
    u32 prefixKey;
    u32 prefixModifiers;
    bool prefixDirty;                // ecs.prefix changed and is read again at the next key press
    ECSIKeyBinding *prefixKeys;      // stb_ds array of the keys after the prefix, merged from every layer
    bool prefixKeysDirty;            // the keys tables changed, so the keys after the prefix are merged again
    usz prefixWorkspace;             // the workspace whose keys the keys after the prefix include
    ECSIPanelBinding *panelBindings; // stb_ds array of plugins' default keys for their panel types
    ECSValue *toolKeys;              // the preset's keys table, or NULL
    ECSValue **workspaceKeys;        // stb_ds array of each workspace's keys table; NULL for none
} KEYS = {0};

/// @brief Reads the core prefix from the setting ecs.prefix if the setting changed. Its value is always a key combination, because key settings take nothing else.
static void ECSIKeys_ReadPrefix(void)
{
    if (!KEYS.prefixDirty)
    {
        return;
    }

    KEYS.prefixDirty = false;
    SHUResult result = ECSISettings_ParseKey(ECSValue_GetString(ECSSetting_Get("ecs.prefix"), ""), true, &KEYS.prefixKey, &KEYS.prefixModifiers);
    SDL_assert(result == SHUResult_Ok);
    (void)result;
}

static void ECSIKeys_FreeBindings(ECSIKeyBinding **bindings)
{
    for (usz i = 0; i < arrlenu(*bindings); i++)
    {
        SDL_free((*bindings)[i].text);
        SDL_free((*bindings)[i].function);
    }

    arrfree(*bindings);
}

/// @brief Adds a binding to a list, or changes the binding of the same combination. A function name of NULL removes it. A key text that cannot be read is skipped; it is reported when its table is checked.
static void ECSIKeys_PutBinding(ECSIKeyBinding **bindings, const char *text, const char *function)
{
    u32 key = 0;
    u32 modifiers = 0;

    if (ECSISettings_ParseKey(text, false, &key, &modifiers))
    {
        return;
    }

    for (usz i = 0; i < arrlenu(*bindings); i++)
    {
        ECSIKeyBinding *binding = &(*bindings)[i];

        if (binding->key == key && binding->modifiers == modifiers)
        {
            SDL_free(binding->text);
            SDL_free(binding->function);
            arrdel(*bindings, i);
            break;
        }
    }

    ECSIKeyBinding binding = {.key = key, .modifiers = modifiers, .text = SDL_strdup(text), .function = function == NULL ? NULL : SDL_strdup(function)};

    if (function == NULL || binding.text == NULL || binding.function == NULL)
    {
        SDL_free(binding.text);
        SDL_free(binding.function);
        return;
    }

    arrput(*bindings, binding);
}

/// @brief Adds a key of a prefix table: a key text and a function name, or false to remove the key.
static void ECSIKeys_AddPrefixKey(const char *name, const ECSValue *field, void *userData)
{
    (void)userData;
    ECSIKeys_PutBinding(&KEYS.prefixKeys, name, ECSValue_GetString(field, NULL));
}

/// @brief Merges the keys after the prefix from every layer if the keys tables or the current workspace changed. A higher layer adds to the lower ones.
static void ECSIKeys_ReadPrefixKeys(void)
{
    usz workspace = ECSILayout_GetCurrentWorkspace();

    if (!KEYS.prefixKeysDirty && workspace == KEYS.prefixWorkspace)
    {
        return;
    }

    KEYS.prefixKeysDirty = false;
    KEYS.prefixWorkspace = workspace;
    ECSIKeys_FreeBindings(&KEYS.prefixKeys);

    const ECSValue *tables[] = {
        ECSISettings_GetKeys(ECSISettingsLayer_Core),
        KEYS.toolKeys,
        workspace < arrlenu(KEYS.workspaceKeys) ? KEYS.workspaceKeys[workspace] : NULL,
        ECSISettings_GetKeys(ECSISettingsLayer_User),
    };

    for (usz i = 0; i < SDL_arraysize(tables); i++)
    {
        ECSIValue_TableForEachField(ECSValue_GetTableField(tables[i], OPENECS_KEYS_PREFIX), ECSIKeys_AddPrefixKey, NULL);
    }
}

/// @brief Keeps a binding that matches a key press, if it wins over the best one so far: a higher layer wins, then a more specific scope.
/// @param function The function's name, or NULL for a binding that removes the key.
static void ECSIKeys_ConsiderBinding(ECSIBindingSearch *search, const char *text, const char *function)
{
    u32 key = 0;
    u32 modifiers = 0;

    if (ECSISettings_ParseKey(text, false, &key, &modifiers) || key != search->key || modifiers != search->modifiers)
    {
        return;
    }

    if (!search->found || search->layer > search->bestLayer || (search->layer == search->bestLayer && search->scope > search->bestScope))
    {
        search->found = true;
        search->function = function;
        search->bestLayer = search->layer;
        search->bestScope = search->scope;
    }
}

/// @brief Considers a field of a keys table: the name of a function, or false, which removes the key.
static void ECSIKeys_ConsiderField(const char *name, const ECSValue *field, void *userData)
{
    if (ECSValue_GetType(field) == ECSValueType_String)
    {
        ECSIKeys_ConsiderBinding(userData, name, ECSValue_GetString(field, NULL));
    }
    else if (ECSValue_GetType(field) == ECSValueType_Bool && !ECSValue_GetBool(field, true))
    {
        ECSIKeys_ConsiderBinding(userData, name, NULL);
    }
}

/// @brief Considers the bindings of a keys table: the ones for the whole table's scope, and the ones for a panel type, which are more specific.
static void ECSIKeys_ConsiderTable(ECSIBindingSearch *search, const ECSValue *keys, ECSIKeysLayer layer, bool workspace, const char *panelType)
{
    search->layer = layer;
    search->scope = workspace ? ECSIBindingScope_Workspace : ECSIBindingScope_Tool;
    ECSIValue_TableForEachField(keys, ECSIKeys_ConsiderField, search);

    if (panelType != NULL)
    {
        search->scope = workspace ? ECSIBindingScope_WorkspacePanelType : ECSIBindingScope_PanelType;
        ECSIValue_TableForEachField(ECSValue_GetTableField(keys, panelType), ECSIKeys_ConsiderField, search);
    }
}

/// @brief Finds the function that a key press runs while a panel of a type has focus.
/// @param panelType The focused panel's type, or NULL.
/// @return The function's name, or NULL if no binding matches or the winning binding removes the key.
static const char *ECSIKeys_FindFor(u32 key, u32 modifiers, const char *panelType)
{
    ECSIBindingSearch search = {.key = key, .modifiers = modifiers};
    usz workspace = ECSILayout_GetCurrentWorkspace();

    ECSIKeys_ConsiderTable(&search, ECSISettings_GetKeys(ECSISettingsLayer_User), ECSIKeysLayer_User, false, panelType);

    if (workspace < arrlenu(KEYS.workspaceKeys))
    {
        ECSIKeys_ConsiderTable(&search, KEYS.workspaceKeys[workspace], ECSIKeysLayer_Preset, true, panelType);
    }

    ECSIKeys_ConsiderTable(&search, KEYS.toolKeys, ECSIKeysLayer_Preset, false, panelType);
    ECSIKeys_ConsiderTable(&search, ECSISettings_GetKeys(ECSISettingsLayer_Core), ECSIKeysLayer_Default, false, panelType);

    for (usz i = 0; panelType != NULL && i < arrlenu(KEYS.panelBindings); i++)
    {
        const ECSIPanelBinding *binding = &KEYS.panelBindings[i];

        if (SDL_strcmp(binding->panelType, panelType) == 0)
        {
            search.layer = ECSIKeysLayer_Default;
            search.scope = ECSIBindingScope_PanelType;
            ECSIKeys_ConsiderBinding(&search, binding->key, binding->function);
        }
    }

    return search.function;
}

/// @brief Reports a binding whose key is not a key combination, or whose value is neither a function's name nor false; the data is the place the table comes from.
static void ECSIKeys_CheckInnerField(const char *name, const ECSValue *field, void *userData)
{
    u32 key = 0;
    u32 modifiers = 0;
    ECSValueType type = ECSValue_GetType(field);

    if (type != ECSValueType_String && type != ECSValueType_Bool)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "The key '%s' in %s must run a function, by name, or be false.", name, (const char *)userData);
        return;
    }

    if (ECSISettings_ParseKey(name, false, &key, &modifiers))
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "'%s' in the keys of %s is not a key combination.", name, (const char *)userData);
    }
}

/// @brief Reports a field of a keys table that never works: a bad binding, or a table named neither prefix nor like a panel type.
static void ECSIKeys_CheckField(const char *name, const ECSValue *field, void *userData)
{
    if (ECSValue_GetType(field) != ECSValueType_Table)
    {
        ECSIKeys_CheckInnerField(name, field, userData);
    }
    else if (SDL_strcmp(name, OPENECS_KEYS_PREFIX) != 0 && SDL_strchr(name, '.') == NULL)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "'%s' in the keys of %s is neither prefix nor a panel type.", name, (const char *)userData);
    }
    else
    {
        ECSIValue_TableForEachField(field, ECSIKeys_CheckInnerField, userData);
    }
}

/// @brief Reports what a keys table holds that never works.
static void ECSIKeys_CheckKeys(const ECSValue *keys, const char *place)
{
    ECSIValue_TableForEachField(keys, ECSIKeys_CheckField, (void *)place);
}

/// @brief Reports a binding that runs a function its owner does not have, though the owner runs; recurses into tables of keys.
static void ECSIKeys_ReportField(const char *name, const ECSValue *field, void *userData)
{
    const char *function = ECSValue_GetString(field, NULL);

    if (ECSValue_GetType(field) == ECSValueType_Table)
    {
        ECSIValue_TableForEachField(field, ECSIKeys_ReportField, userData);
    }
    else if (function != NULL && ECSIServices_GetDescription(function) == NULL && ECSIPlugins_OwnerRuns(function))
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "The key '%s' in %s runs '%s', which does not exist.", name, (const char *)userData, function);
    }
}

static void ECSIKeys_FreePanelBinding(ECSIPanelBinding *binding)
{
    SDL_free(binding->panelType);
    SDL_free(binding->key);
    SDL_free(binding->function);
}

/// @brief Marks ecs.prefix to be read again; the setting's Changed function.
static void ECSIKeys_PrefixChanged(void *data)
{
    (void)data;
    KEYS.prefixDirty = true;
}

/// @brief Looks for a key that runs a function in a keys table, and that wins for a panel type, while the fields are walked.
typedef struct ECSIKeysTextSearch
{
    const char *panelType;
    const char *function;
    const char *text; // the key found, or NULL
} ECSIKeysTextSearch;

static void ECSIKeys_TextField(const char *name, const ECSValue *field, void *userData)
{
    ECSIKeysTextSearch *search = userData;
    u32 key = 0;
    u32 modifiers = 0;

    if (search->text != NULL || SDL_strcmp(ECSValue_GetString(field, ""), search->function) != 0 || ECSISettings_ParseKey(name, false, &key, &modifiers))
    {
        return;
    }

    const char *found = ECSIKeys_FindFor(key, modifiers, search->panelType);

    if (found != NULL && SDL_strcmp(found, search->function) == 0)
    {
        search->text = name;
    }
}

/// @brief Finds the text of a key of a keys table that runs a function while a panel of a type has focus: one for the type first, then one for the table's whole scope.
static const char *ECSIKeys_TextIn(const ECSValue *keys, const char *panelType, const char *function)
{
    ECSIKeysTextSearch search = {.panelType = panelType, .function = function};
    ECSIValue_TableForEachField(ECSValue_GetTableField(keys, panelType), ECSIKeys_TextField, &search);
    ECSIValue_TableForEachField(keys, ECSIKeys_TextField, &search);
    return search.text;
}

#pragma endregion Source Only

SHUResult ECSIKeys_Initialize(void)
{
    ECSSettingDesc prefix = {
        .name = "ecs.prefix",
        .type = ECSSettingType_Key,
        .description = "The key combination before a core action",
        .Changed = ECSIKeys_PrefixChanged,
    };

    SHU_ReturnResult(ECSISettings_DeclareCore(&prefix));

    KEYS.prefixDirty = true;
    KEYS.prefixKeysDirty = true;
    ECSIKeys_ReadPrefix();
    ECSIKeys_CheckKeys(ECSISettings_GetKeys(ECSISettingsLayer_Core), "the core's settings file");
    ECSIKeys_CheckKeys(ECSISettings_GetKeys(ECSISettingsLayer_User), "the user's settings");
    return SHUResult_Ok;
}

SHUResult ECSIKeys_SetTool(const ECSValue *keys)
{
    ECSValue_Destroy(&KEYS.toolKeys);
    ECSIKeys_CheckKeys(keys, "the preset");
    KEYS.prefixKeysDirty = true;
    SHU_ReturnResult(ECSValue_Create(&KEYS.toolKeys));
    return ECSIValue_Copy(KEYS.toolKeys, keys);
}

SHUResult ECSIKeys_AddWorkspace(const ECSValue *keys)
{
    ECSValue *copy = NULL;

    if (keys != NULL)
    {
        ECSIKeys_CheckKeys(keys, "a workspace");
        SHU_ReturnResult(ECSValue_Create(&copy));
        SHU_ReturnResult(ECSIValue_Copy(copy, keys), ECSValue_Destroy(&copy););
    }

    KEYS.prefixKeysDirty = true;
    arrput(KEYS.workspaceKeys, copy);
    return SHUResult_Ok;
}

const ECSValue *ECSIKeys_GetWorkspace(usz index)
{
    return index < arrlenu(KEYS.workspaceKeys) ? KEYS.workspaceKeys[index] : NULL;
}

void ECSIKeys_ReportUnknownFunctions(void)
{
    ECSIValue_TableForEachField(ECSISettings_GetKeys(ECSISettingsLayer_Core), ECSIKeys_ReportField, "the core's settings file");
    ECSIValue_TableForEachField(KEYS.toolKeys, ECSIKeys_ReportField, "the preset");
    ECSIValue_TableForEachField(ECSISettings_GetKeys(ECSISettingsLayer_User), ECSIKeys_ReportField, "the user's settings");

    for (usz i = 0; i < arrlenu(KEYS.workspaceKeys); i++)
    {
        ECSIValue_TableForEachField(KEYS.workspaceKeys[i], ECSIKeys_ReportField, "a workspace of the preset");
    }
}

void ECSIKeys_RemovePlugin(ECSPlugin plugin)
{
    SDL_assert(plugin != NULL);

    for (usz i = arrlenu(KEYS.panelBindings); i > 0; i--)
    {
        if (KEYS.panelBindings[i - 1].plugin == plugin)
        {
            ECSIKeys_FreePanelBinding(&KEYS.panelBindings[i - 1]);
            arrdel(KEYS.panelBindings, i - 1);
        }
    }
}

void ECSIKeys_Terminate(void)
{
    ECSIKeys_FreeBindings(&KEYS.prefixKeys);

    for (usz i = 0; i < arrlenu(KEYS.panelBindings); i++)
    {
        ECSIKeys_FreePanelBinding(&KEYS.panelBindings[i]);
    }

    for (usz i = 0; i < arrlenu(KEYS.workspaceKeys); i++)
    {
        ECSValue_Destroy(&KEYS.workspaceKeys[i]);
    }

    arrfree(KEYS.panelBindings);
    arrfree(KEYS.workspaceKeys);
    ECSValue_Destroy(&KEYS.toolKeys);
    SDL_zero(KEYS);
}

const char *ECSIKeys_Find(u32 key, u32 modifiers, ECSPanel focus)
{
    return ECSIKeys_FindFor(key, modifiers, focus == NULL ? NULL : focus->typeName);
}

bool ECSIKeys_IsPrefix(u32 key, u32 modifiers)
{
    ECSIKeys_ReadPrefix();
    return key == KEYS.prefixKey && (modifiers & ECSModifier_AltGr) == 0 && modifiers == KEYS.prefixModifiers;
}

const ECSIKeyBinding *ECSIKeys_GetPrefixKeys(void)
{
    ECSIKeys_ReadPrefixKeys();
    return KEYS.prefixKeys;
}

char *ECSIKeys_PrefixTextOf(const char *function)
{
    SDL_assert(function != NULL);

    const char *prefix = ECSValue_GetString(ECSSetting_Get("ecs.prefix"), "");
    const ECSIKeyBinding *keys = ECSIKeys_GetPrefixKeys();
    char *text = NULL;

    for (usz i = 0; text == NULL && i < arrlenu(keys); i++)
    {
        if (SDL_strcmp(keys[i].function, function) == 0 && SDL_asprintf(&text, "%s, %s", prefix, keys[i].text) < 0)
        {
            text = NULL;
        }
    }

    return text;
}

char *ECSIKeys_BoundTextOf(const char *panelType, const char *function)
{
    SDL_assert(panelType != NULL);
    SDL_assert(function != NULL);

    usz workspace = ECSILayout_GetCurrentWorkspace();
    const ECSValue *tables[] = {
        ECSISettings_GetKeys(ECSISettingsLayer_User),
        workspace < arrlenu(KEYS.workspaceKeys) ? KEYS.workspaceKeys[workspace] : NULL,
        KEYS.toolKeys,
        ECSISettings_GetKeys(ECSISettingsLayer_Core),
    };

    for (usz i = 0; i < SDL_arraysize(tables); i++)
    {
        const char *text = ECSIKeys_TextIn(tables[i], panelType, function);

        if (text != NULL)
        {
            return SDL_strdup(text);
        }
    }

    for (usz i = 0; i < arrlenu(KEYS.panelBindings); i++)
    {
        const ECSIPanelBinding *binding = &KEYS.panelBindings[i];
        u32 key = 0;
        u32 modifiers = 0;

        if (SDL_strcmp(binding->panelType, panelType) == 0 && SDL_strcmp(binding->function, function) == 0 && !ECSISettings_ParseKey(binding->key, false, &key, &modifiers))
        {
            const char *found = ECSIKeys_FindFor(key, modifiers, panelType);

            if (found != NULL && SDL_strcmp(found, function) == 0)
            {
                return SDL_strdup(binding->key);
            }
        }
    }

    return NULL;
}

SHUResult ECSKey_Bind(ECSPlugin plugin, const char *panelType, const char *key, const char *function)
{
    SDL_assert(plugin != NULL);
    SDL_assert(panelType != NULL && key != NULL && function != NULL);

    u32 code = 0;
    u32 modifiers = 0;

    // a plugin binds keys only for its own panel types
    if (!ECSIPlugin_OwnsName(plugin, panelType))
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' binds a key for '%s', which is not one of its panel types.", ECSIPlugin_GetName(plugin), panelType);
        return SHUResult_ErrBadData;
    }

    if (ECSISettings_ParseKey(key, true, &code, &modifiers))
    {
        return SHUResult_ErrBadData;
    }

    ECSIKeys_ReadPrefix();

    if (code == KEYS.prefixKey && modifiers == KEYS.prefixModifiers)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' binds '%s', the core prefix; the binding is never triggered.", ECSIPlugin_GetName(plugin), key);
    }

    ECSIPanelBinding binding = {.plugin = plugin, .panelType = SDL_strdup(panelType), .key = SDL_strdup(key), .function = SDL_strdup(function)};

    if (binding.panelType == NULL || binding.key == NULL || binding.function == NULL)
    {
        ECSIKeys_FreePanelBinding(&binding);
        return SHUResult_ErrAllocation;
    }

    arrput(KEYS.panelBindings, binding);
    return SHUResult_Ok;
}
