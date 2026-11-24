#include "interface/Keys.h"

#include "base/Log.h"
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
    char *panelType; // the panel type it works for, or "prefix" for a key after the prefix
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

/// @brief A keys table in effect, and its layer.
typedef struct ECSIKeysTable
{
    const ECSValue *keys; // nullptr for none
    ECSIKeysLayer layer;
    bool workspace; // the current workspace's table, whose keys are more specific than the ones for the whole tool
} ECSIKeysTable;

/// @brief Number of keys tables in effect: the core's settings file, the preset, the current workspace and the user's settings.
#define OPENECS_KEYS_TABLE_COUNT 4

/// @brief How specific a binding is; within one layer, a more specific binding wins.
typedef enum ECSIBindingScope
{
    ECSIBindingScope_Tool = 0,
    ECSIBindingScope_Workspace,
    ECSIBindingScope_PanelType,
    ECSIBindingScope_WorkspacePanelType,
} ECSIBindingScope;

/// @brief A binding of a keys table in effect, or a plugin's for its panel type, with its key read once.
typedef struct ECSIParsedBinding
{
    u32 key;
    u32 modifiers;
    ECSIKeysLayer layer;
    ECSIBindingScope scope;
    const char *panelType; // the panel type it works for, or nullptr for its table's whole scope; the tables and the plugins' bindings own it
    const char *function;  // nullptr when false removes the key; owned as panelType is
} ECSIParsedBinding;

/// @brief A keys table being read into the parsed bindings: its layer, its scope and the panel type of its inner table.
typedef struct ECSIBindingRead
{
    ECSIKeysLayer layer;
    ECSIBindingScope scope;
    const char *panelType;
} ECSIBindingRead;

static struct
{
    u32 prefixKey;
    u32 prefixModifiers;
    bool prefixDirty;                // ecs.prefix changed and is read again at the next key press
    ECSIKeyBinding *prefixKeys;      // stb_ds array of the keys after the prefix, merged from every layer
    ECSIParsedBinding *bindings;     // stb_ds array of the bindings of the keys tables in effect and of the plugins, parsed from their texts
    bool tablesDirty;                // the keys tables or the plugins' bindings changed, so the keys after the prefix and the bindings are read again
    usz tablesWorkspace;             // the workspace whose keys they include
    ECSIPanelBinding *panelBindings; // stb_ds array of plugins' default keys for their panel types, and after the prefix with the scope "prefix"
    ECSValue *presetKeys;            // the preset's keys table, or nullptr
    ECSValue **workspaceKeys;        // stb_ds array of each workspace's keys table; nullptr for none
} KEYS = {0};

/// @brief Reads the core prefix from the setting ecs.prefix if the setting changed. Its value is always a key combination, because key settings take nothing else.
static void ECSIKeys_ReadPrefix(void)
{
    if (!KEYS.prefixDirty)
    {
        return;
    }

    KEYS.prefixDirty = false;
    SHUResult result = ECSISettings_ParseKey(ECSValue_GetString(ECSSetting_Get("ecs.prefix"), ""), &KEYS.prefixKey, &KEYS.prefixModifiers);
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

/// @brief Adds a binding to a list, or changes the binding of the same combination. A function name of nullptr removes it. A key text that cannot be read is skipped; it is reported when its table is checked.
static void ECSIKeys_PutBinding(ECSIKeyBinding **bindings, const char *text, const char *function)
{
    u32 key = 0;
    u32 modifiers = 0;

    if (ECSISettings_ParseKey(text, &key, &modifiers))
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

    ECSIKeyBinding binding = {.key = key, .modifiers = modifiers, .text = SDL_strdup(text), .function = function == nullptr ? nullptr : SDL_strdup(function)};

    if (function == nullptr || binding.text == nullptr || binding.function == nullptr)
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
    ECSIKeys_PutBinding(&KEYS.prefixKeys, name, ECSValue_GetString(field, nullptr));
}

/// @brief Gets the keys tables in effect, from the lowest layer to the highest (DESIGN 7.8). Within the preset's layer, the current workspace's table comes after the preset's own.
static void ECSIKeys_GetTables(ECSIKeysTable retTables[OPENECS_KEYS_TABLE_COUNT])
{
    usz workspace = ECSILayout_GetCurrentIndex();
    retTables[0] = (ECSIKeysTable){ECSISettings_GetKeys(ECSISettingsLayer_Core), ECSIKeysLayer_Default, false};
    retTables[1] = (ECSIKeysTable){KEYS.presetKeys, ECSIKeysLayer_Preset, false};
    retTables[2] = (ECSIKeysTable){workspace < arrlenu(KEYS.workspaceKeys) ? KEYS.workspaceKeys[workspace] : nullptr, ECSIKeysLayer_Preset, true};
    retTables[3] = (ECSIKeysTable){ECSISettings_GetKeys(ECSISettingsLayer_User), ECSIKeysLayer_User, false};
}

/// @brief Adds a field of a keys table to the parsed bindings: a key with a function's name or false, or the inner table of a panel type.
static void ECSIKeys_ReadField(const char *name, const ECSValue *field, void *userData)
{
    const ECSIBindingRead *read = userData;
    ECSValueType type = ECSValue_GetType(field);

    if (type == ECSValueType_Table)
    {
        // a panel type's keys; the keys after the prefix are read apart
        if (read->panelType == nullptr && SDL_strcmp(name, OPENECS_KEYS_PREFIX) != 0)
        {
            ECSIBindingRead inner = {read->layer, read->scope == ECSIBindingScope_Workspace ? ECSIBindingScope_WorkspacePanelType : ECSIBindingScope_PanelType, name};
            ECSIValue_TableForEachField(field, ECSIKeys_ReadField, &inner);
        }

        return;
    }

    ECSIParsedBinding binding = {.layer = read->layer, .scope = read->scope, .panelType = read->panelType};

    if ((type != ECSValueType_String && (type != ECSValueType_Bool || ECSValue_GetBool(field, true))) || ECSISettings_ParseKey(name, &binding.key, &binding.modifiers))
    {
        return;
    }

    binding.function = type == ECSValueType_String ? ECSValue_GetString(field, nullptr) : nullptr;
    arrput(KEYS.bindings, binding);
}

/// @brief Reads the keys after the prefix and the bindings again if the keys tables, the plugins' bindings or the current workspace changed.
/// The keys after the prefix merge every layer, a higher layer adding to the lower ones.
static void ECSIKeys_ReadTables(void)
{
    usz workspace = ECSILayout_GetCurrentIndex();

    if (!KEYS.tablesDirty && workspace == KEYS.tablesWorkspace)
    {
        return;
    }

    KEYS.tablesDirty = false;
    KEYS.tablesWorkspace = workspace;
    ECSIKeys_FreeBindings(&KEYS.prefixKeys);
    arrfree(KEYS.bindings);
    ECSIKeysTable tables[OPENECS_KEYS_TABLE_COUNT];
    ECSIKeys_GetTables(tables);

    // the plugins' keys after the prefix are defaults below the core's settings file, so the core's keys win
    for (usz i = 0; i < arrlenu(KEYS.panelBindings); i++)
    {
        if (SDL_strcmp(KEYS.panelBindings[i].panelType, OPENECS_KEYS_PREFIX) == 0)
        {
            ECSIKeys_PutBinding(&KEYS.prefixKeys, KEYS.panelBindings[i].key, KEYS.panelBindings[i].function);
        }
    }

    for (usz i = 0; i < OPENECS_KEYS_TABLE_COUNT; i++)
    {
        ECSIValue_TableForEachField(ECSValue_GetTableField(tables[i].keys, OPENECS_KEYS_PREFIX), ECSIKeys_AddPrefixKey, nullptr);

        ECSIBindingRead read = {tables[i].layer, tables[i].workspace ? ECSIBindingScope_Workspace : ECSIBindingScope_Tool, nullptr};
        ECSIValue_TableForEachField(tables[i].keys, ECSIKeys_ReadField, &read);
    }

    // the plugins' keys for their panel types are defaults too
    for (usz i = 0; i < arrlenu(KEYS.panelBindings); i++)
    {
        const ECSIPanelBinding *panelBinding = &KEYS.panelBindings[i];
        ECSIParsedBinding binding = {.layer = ECSIKeysLayer_Default, .scope = ECSIBindingScope_PanelType, .panelType = panelBinding->panelType, .function = panelBinding->function};

        if (SDL_strcmp(panelBinding->panelType, OPENECS_KEYS_PREFIX) != 0 && !ECSISettings_ParseKey(panelBinding->key, &binding.key, &binding.modifiers))
        {
            arrput(KEYS.bindings, binding);
        }
    }
}

/// @brief Finds the function that a key press runs while a panel of a type has focus.
/// @param panelType The focused panel's type, or nullptr.
/// @return The function's name, or nullptr if no binding matches or the winning binding removes the key.
static const char *ECSIKeys_FindFor(u32 key, u32 modifiers, const char *panelType)
{
    ECSIKeys_ReadTables();
    const ECSIParsedBinding *best = nullptr;

    // a higher layer wins, then a more specific scope
    for (usz i = 0; i < arrlenu(KEYS.bindings); i++)
    {
        const ECSIParsedBinding *binding = &KEYS.bindings[i];
        bool applies = binding->panelType == nullptr || (panelType != nullptr && SDL_strcmp(binding->panelType, panelType) == 0);

        if (binding->key == key && binding->modifiers == modifiers && applies &&
            (best == nullptr || binding->layer > best->layer || (binding->layer == best->layer && binding->scope > best->scope)))
        {
            best = binding;
        }
    }

    return best == nullptr ? nullptr : best->function;
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

    if (ECSISettings_ParseKey(name, &key, &modifiers))
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
    else if (SDL_strcmp(name, OPENECS_KEYS_PREFIX) != 0 && SDL_strchr(name, '.') == nullptr)
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
    const char *function = ECSValue_GetString(field, nullptr);

    if (ECSValue_GetType(field) == ECSValueType_Table)
    {
        ECSIValue_TableForEachField(field, ECSIKeys_ReportField, userData);
    }
    else if (function != nullptr && ECSIServices_GetDescription(function) == nullptr && ECSIPlugins_OwnerRuns(function))
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
    const char *text; // the key found, or nullptr
} ECSIKeysTextSearch;

static void ECSIKeys_TextField(const char *name, const ECSValue *field, void *userData)
{
    ECSIKeysTextSearch *search = userData;
    u32 key = 0;
    u32 modifiers = 0;

    if (search->text != nullptr || SDL_strcmp(ECSValue_GetString(field, ""), search->function) != 0 || ECSISettings_ParseKey(name, &key, &modifiers))
    {
        return;
    }

    const char *found = ECSIKeys_FindFor(key, modifiers, search->panelType);

    if (found != nullptr && SDL_strcmp(found, search->function) == 0)
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

/// @brief A binding of a keys table, as ECSKey_List gives it.
typedef struct ECSIListedBinding
{
    const char *scope;    // nullptr for the whole tool, "prefix", or a panel type
    const char *text;     // the key combination as written
    const char *function; // nullptr for a key that false removes
    u32 key;
    u32 modifiers;
    ECSIKeysLayer layer;
    bool workspace;
    u32 rank; // a binding of a higher rank wins in its scope; of the same rank, the first one listed
} ECSIListedBinding;

/// @brief The bindings being listed, and the table and scope that the next ones come from.
typedef struct ECSIBindingList
{
    ECSIListedBinding *bindings; // stb_ds array
    const ECSIKeysTable *table;
    u32 rank;
    const char *scope;
} ECSIBindingList;

static void ECSIKeys_ListBinding(ECSIBindingList *list, const char *text, const char *function)
{
    ECSIListedBinding binding = {.scope = list->scope, .text = text, .function = function, .rank = list->rank};
    binding.layer = list->table != nullptr ? list->table->layer : ECSIKeysLayer_Default;
    binding.workspace = list->table != nullptr && list->table->workspace;

    // a key text that cannot be read is reported when its table is checked, and is not listed
    if (!ECSISettings_ParseKey(text, &binding.key, &binding.modifiers))
    {
        arrput(list->bindings, binding);
    }
}

/// @brief Lists a field of a keys table: a binding, a removed key, or a table of the keys of a scope, whose bindings are listed in turn.
static void ECSIKeys_ListField(const char *name, const ECSValue *field, void *userData)
{
    ECSIBindingList *list = userData;

    if (ECSValue_GetType(field) == ECSValueType_String)
    {
        ECSIKeys_ListBinding(list, name, ECSValue_GetString(field, nullptr));
    }
    else if (ECSValue_GetType(field) == ECSValueType_Bool && !ECSValue_GetBool(field, true))
    {
        ECSIKeys_ListBinding(list, name, nullptr);
    }
    else if (ECSValue_GetType(field) == ECSValueType_Table && list->scope == nullptr)
    {
        list->scope = name;
        ECSIValue_TableForEachField(field, ECSIKeys_ListField, list);
        list->scope = nullptr;
    }
}

/// @brief Checks whether a binding runs: no other one of its scope binds its key at a higher rank, or before it at the same rank.
static bool ECSIKeys_IsActive(const ECSIListedBinding *bindings, usz index)
{
    const ECSIListedBinding *binding = &bindings[index];

    for (usz i = 0; i < arrlenu(bindings); i++)
    {
        const ECSIListedBinding *other = &bindings[i];
        bool sameScope = (other->scope == nullptr && binding->scope == nullptr) ||
                         (other->scope != nullptr && binding->scope != nullptr && SDL_strcmp(other->scope, binding->scope) == 0);

        if (i != index && sameScope && other->key == binding->key && other->modifiers == binding->modifiers &&
            (other->rank > binding->rank || (other->rank == binding->rank && i < index)))
        {
            return false;
        }
    }

    return true;
}

static SHUResult ECSIKeys_FillBinding(ECSValue *item, const ECSIListedBinding *binding, bool active)
{
    static const char *const LAYERS[] = {"default", "preset", "user"};
    ECSValue *field = nullptr;
    ECSValue_SetTable(item);
    SHU_ReturnResult(ECSValue_SetTableField(item, "key", &field));
    SHU_ReturnResult(ECSValue_SetString(field, binding->text));
    SHU_ReturnResult(ECSValue_SetTableField(item, "function", &field));

    if (binding->function != nullptr)
    {
        SHU_ReturnResult(ECSValue_SetString(field, binding->function));
    }
    else
    {
        ECSValue_SetBool(field, false);
    }

    if (binding->scope != nullptr)
    {
        SHU_ReturnResult(ECSValue_SetTableField(item, "scope", &field));
        SHU_ReturnResult(ECSValue_SetString(field, binding->scope));
    }

    SHU_ReturnResult(ECSValue_SetTableField(item, "layer", &field));
    SHU_ReturnResult(ECSValue_SetString(field, LAYERS[binding->layer]));
    SHU_ReturnResult(ECSValue_SetTableField(item, "workspace", &field));
    ECSValue_SetBool(field, binding->workspace);
    SHU_ReturnResult(ECSValue_SetTableField(item, "active", &field));
    ECSValue_SetBool(field, active);
    return SHUResult_Ok;
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
    KEYS.tablesDirty = true;
    ECSIKeys_ReadPrefix();
    ECSIKeys_CheckKeys(ECSISettings_GetKeys(ECSISettingsLayer_Core), "the core's settings file");
    ECSIKeys_CheckKeys(ECSISettings_GetKeys(ECSISettingsLayer_User), "the user's settings");
    return SHUResult_Ok;
}

SHUResult ECSIKeys_SetPreset(const ECSValue *keys)
{
    ECSValue_Destroy(&KEYS.presetKeys);
    ECSIKeys_CheckKeys(keys, "the preset");
    KEYS.tablesDirty = true;
    SHU_ReturnResult(ECSValue_Create(&KEYS.presetKeys));
    return ECSIValue_Copy(KEYS.presetKeys, keys);
}

SHUResult ECSIKeys_AddWorkspace(const ECSValue *keys)
{
    ECSValue *copy = nullptr;

    if (keys != nullptr)
    {
        ECSIKeys_CheckKeys(keys, "a workspace");
        SHU_ReturnResult(ECSValue_Create(&copy));
        SHU_ReturnResult(ECSIValue_Copy(copy, keys), ECSValue_Destroy(&copy););
    }

    KEYS.tablesDirty = true;
    arrput(KEYS.workspaceKeys, copy);
    return SHUResult_Ok;
}

void ECSIKeys_ClearWorkspaces(void)
{
    for (usz i = 0; i < arrlenu(KEYS.workspaceKeys); i++)
    {
        ECSValue_Destroy(&KEYS.workspaceKeys[i]);
    }

    arrfree(KEYS.workspaceKeys);
    KEYS.tablesDirty = true;
}

const ECSValue *ECSIKeys_GetWorkspace(usz index)
{
    return index < arrlenu(KEYS.workspaceKeys) ? KEYS.workspaceKeys[index] : nullptr;
}

void ECSIKeys_ReportUnknownFunctions(void)
{
    ECSIValue_TableForEachField(ECSISettings_GetKeys(ECSISettingsLayer_Core), ECSIKeys_ReportField, "the core's settings file");
    ECSIValue_TableForEachField(KEYS.presetKeys, ECSIKeys_ReportField, "the preset");
    ECSIValue_TableForEachField(ECSISettings_GetKeys(ECSISettingsLayer_User), ECSIKeys_ReportField, "the user's settings");

    for (usz i = 0; i < arrlenu(KEYS.workspaceKeys); i++)
    {
        ECSIValue_TableForEachField(KEYS.workspaceKeys[i], ECSIKeys_ReportField, "a workspace of the preset");
    }
}

void ECSIKeys_RemovePlugin(ECSPlugin plugin)
{
    SDL_assert(plugin != nullptr);

    for (usz i = arrlenu(KEYS.panelBindings); i > 0; i--)
    {
        if (KEYS.panelBindings[i - 1].plugin == plugin)
        {
            ECSIKeys_FreePanelBinding(&KEYS.panelBindings[i - 1]);
            arrdel(KEYS.panelBindings, i - 1);
            KEYS.tablesDirty = true;
        }
    }
}

void ECSIKeys_Terminate(void)
{
    ECSIKeys_FreeBindings(&KEYS.prefixKeys);
    arrfree(KEYS.bindings);

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
    ECSValue_Destroy(&KEYS.presetKeys);
    SDL_zero(KEYS);
}

const char *ECSIKeys_Find(u32 key, u32 modifiers, ECSPanel focus)
{
    return ECSIKeys_FindFor(key, modifiers, focus == nullptr ? nullptr : focus->typeName);
}

bool ECSIKeys_IsPrefix(u32 key, u32 modifiers)
{
    ECSIKeys_ReadPrefix();
    return key == KEYS.prefixKey && modifiers == KEYS.prefixModifiers;
}

const ECSIKeyBinding *ECSIKeys_GetPrefixKeys(void)
{
    ECSIKeys_ReadTables();
    return KEYS.prefixKeys;
}

char *ECSIKeys_PrefixTextOf(const char *function)
{
    SDL_assert(function != nullptr);

    const char *prefix = ECSValue_GetString(ECSSetting_Get("ecs.prefix"), "");
    const ECSIKeyBinding *keys = ECSIKeys_GetPrefixKeys();
    char *text = nullptr;

    for (usz i = 0; text == nullptr && i < arrlenu(keys); i++)
    {
        if (SDL_strcmp(keys[i].function, function) == 0 && SDL_asprintf(&text, "%s, %s", prefix, keys[i].text) < 0)
        {
            text = nullptr;
        }
    }

    return text;
}

char *ECSIKeys_BoundTextOf(const char *panelType, const char *function)
{
    SDL_assert(panelType != nullptr);
    SDL_assert(function != nullptr);

    // the highest table first
    ECSIKeysTable tables[OPENECS_KEYS_TABLE_COUNT];
    ECSIKeys_GetTables(tables);

    for (usz i = OPENECS_KEYS_TABLE_COUNT; i > 0; i--)
    {
        const char *text = ECSIKeys_TextIn(tables[i - 1].keys, panelType, function);

        if (text != nullptr)
        {
            return SDL_strdup(text);
        }
    }

    for (usz i = 0; i < arrlenu(KEYS.panelBindings); i++)
    {
        const ECSIPanelBinding *binding = &KEYS.panelBindings[i];
        u32 key = 0;
        u32 modifiers = 0;

        if (SDL_strcmp(binding->panelType, panelType) == 0 && SDL_strcmp(binding->function, function) == 0 && !ECSISettings_ParseKey(binding->key, &key, &modifiers))
        {
            const char *found = ECSIKeys_FindFor(key, modifiers, panelType);

            if (found != nullptr && SDL_strcmp(found, function) == 0)
            {
                return SDL_strdup(binding->key);
            }
        }
    }

    return nullptr;
}

SHUResult ECSKey_Bind(ECSPlugin plugin, const char *scope, const char *key, const char *function)
{
    ECSILog_Check(plugin != nullptr, SHUResult_ErrAssertion);
    ECSILog_Check(scope != nullptr && key != nullptr && function != nullptr, SHUResult_ErrAssertion);

    u32 code = 0;
    u32 modifiers = 0;
    bool prefix = SDL_strcmp(scope, OPENECS_KEYS_PREFIX) == 0;

    // a plugin binds keys only for its own panel types, and keys after the prefix only for its own functions
    if (!prefix && !ECSIPlugin_OwnsName(plugin, scope))
    {
        (void)ECS_Check(nullptr, false, "Plugin '%s' binds a key for '%s', which is not one of its panel types.", ECSIPlugin_GetName(plugin), scope);
        return SHUResult_ErrAssertion;
    }

    if (prefix && !ECSIPlugin_OwnsName(plugin, function))
    {
        (void)ECS_Check(nullptr, false, "Plugin '%s' binds a key after the prefix to '%s', which is not one of its functions.", ECSIPlugin_GetName(plugin), function);
        return SHUResult_ErrAssertion;
    }

    if (ECSISettings_ParseKey(key, &code, &modifiers))
    {
        (void)ECS_Check(nullptr, false, "'%s' is not a key combination.", key);
        return SHUResult_ErrAssertion;
    }

    ECSIKeys_ReadPrefix();

    if (!prefix && code == KEYS.prefixKey && modifiers == KEYS.prefixModifiers)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' binds '%s', the core prefix; the binding is never triggered.", ECSIPlugin_GetName(plugin), key);
    }

    ECSIPanelBinding binding = {.plugin = plugin, .panelType = SDL_strdup(scope), .key = SDL_strdup(key), .function = SDL_strdup(function)};

    if (binding.panelType == nullptr || binding.key == nullptr || binding.function == nullptr)
    {
        ECSIKeys_FreePanelBinding(&binding);
        return SHUResult_ErrAllocation;
    }

    arrput(KEYS.panelBindings, binding);
    KEYS.tablesDirty = true;
    return SHUResult_Ok;
}

SHUResult ECSKey_List(ECSValue *retList)
{
    ECSILog_Check(retList != nullptr, SHUResult_ErrAssertion);
    ECSILog_Check(SDL_IsMainThread(), SHUResult_ErrAssertion);

    ECSIKeysTable tables[OPENECS_KEYS_TABLE_COUNT];
    ECSIKeys_GetTables(tables);
    ECSIBindingList list = {0};

    // the tables from the lowest layer to the highest, and the plugins' keys after the core's file, in the defaults' layer, so the core's keys win
    for (usz i = 0; i < OPENECS_KEYS_TABLE_COUNT; i++)
    {
        list.table = &tables[i];
        list.rank = (u32)i;
        ECSIValue_TableForEachField(tables[i].keys, ECSIKeys_ListField, &list);

        for (usz j = 0; i == 0 && j < arrlenu(KEYS.panelBindings); j++)
        {
            list.table = nullptr;
            list.scope = KEYS.panelBindings[j].panelType;
            ECSIKeys_ListBinding(&list, KEYS.panelBindings[j].key, KEYS.panelBindings[j].function);
            list.scope = nullptr;
        }
    }

    ECSValue_SetTable(retList);
    SHUResult result = SHUResult_Ok;

    for (usz i = 0; !result && i < arrlenu(list.bindings); i++)
    {
        ECSValue *item = nullptr;
        result = ECSValue_AddListItem(retList, &item);
        result = result ? result : ECSIKeys_FillBinding(item, &list.bindings[i], ECSIKeys_IsActive(list.bindings, i));
    }

    arrfree(list.bindings);
    return result;
}

SHUResult ECSKey_Set(const char *scope, const char *key, const ECSValue *function)
{
    ECSILog_Check(key != nullptr, SHUResult_ErrAssertion);
    ECSILog_Check(SDL_IsMainThread(), SHUResult_ErrAssertion);

    u32 code = 0;
    u32 modifiers = 0;
    ECSValueType type = ECSValue_GetType(function);

    if (!ECS_Check(nullptr, !ECSISettings_ParseKey(key, &code, &modifiers), "'%s' is not a key combination, such as \"Ctrl+S\".", key) ||
        !ECS_Check(nullptr, scope == nullptr || SDL_strcmp(scope, OPENECS_KEYS_PREFIX) == 0 || SDL_strchr(scope, '.') != nullptr,
                   "A key's scope is nil for the whole tool, \"prefix\", or a panel type, such as \"canvas.view\"; not '%s'.", scope != nullptr ? scope : "") ||
        !ECS_Check(nullptr, type == ECSValueType_String || type == ECSValueType_Nil || (type == ECSValueType_Bool && !ECSValue_GetBool(function, true)),
                   "A key is set to a function's name, false to remove it, or nil to take the user's binding away."))
    {
        return SHUResult_ErrAssertion;
    }

    // the user's keys are read again even when the file cannot be written, so the bindings, which point into them, are made again whatever the result
    SHUResult result = ECSISettings_SetKey(scope, key, function);
    KEYS.tablesDirty = true;
    return result;
}

SHUResult ECSKey_ListFunctions(ECSValue *retList)
{
    ECSILog_Check(retList != nullptr, SHUResult_ErrAssertion);
    ECSILog_Check(SDL_IsMainThread(), SHUResult_ErrAssertion);
    return ECSIServices_ListBindable(retList);
}
