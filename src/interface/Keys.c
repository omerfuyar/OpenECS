#include "interface/Keys.h"

#include "interface/Layout.h"
#include "runtime/Plugins.h"
#include "runtime/Services.h"
#include "runtime/Settings.h"

#include "SDL3/SDL.h"
#include "stb/stbSDL3.h"

#pragma region Source Only

/// @brief A plugin's binding for its panel type. The key is the value of a key setting, so the user can change it.
typedef struct ECSIPanelBinding
{
    ECSPlugin plugin;
    char *panelType;
    char *setting;
    char *function;
} ECSIPanelBinding;

/// @brief How specific a binding is; a more specific binding wins within one settings layer.
typedef enum ECSIBindingScope
{
    ECSIBindingScope_Tool = 0,
    ECSIBindingScope_Workspace,
    ECSIBindingScope_PanelType,
} ECSIBindingScope;

/// @brief The binding that a key press runs, found by ECSIKeys_Find.
typedef struct ECSIBindingSearch
{
    u32 key;
    u32 modifiers;
    ECSISettingsLayer layer; // of the table being searched
    ECSIBindingScope scope;  // of the table being searched
    const char *function;     // the best binding so far, or NULL
    ECSISettingsLayer bestLayer;
    ECSIBindingScope bestScope;
} ECSIBindingSearch;

static struct
{
    u32 prefixKey;
    u32 prefixModifiers;
    bool prefixDirty;                 // ecs.prefix changed and is read again at the next key press
    ECSIKeyBinding *prefixKeys;      // stb_ds array of the keys after the prefix
    bool prefixKeysDirty;             // ecs.prefixKeys changed and is read again when they are used
    ECSIPanelBinding *panelBindings; // stb_ds array of plugins' bindings for their panel types
    ECSValue *toolKeys;               // the preset's bindings for the whole tool, or NULL
    ECSValue **workspaceKeys;         // stb_ds array of the preset's bindings for each workspace; NULL for none
} KEYS = {0};

/// @brief Reads the core prefix from the setting ecs.prefix if the setting changed. A key text that cannot be read is reported, and the default is used.
static void ECSIKeys_ReadPrefix(void)
{
    if (!KEYS.prefixDirty)
    {
        return;
    }

    KEYS.prefixDirty = false;

    // the default is checked when the setting is declared
    if (ECSIKeys_Parse(ECSValue_GetString(ECSSetting_Get("ecs.prefix"), ""), true, &KEYS.prefixKey, &KEYS.prefixModifiers))
    {
        SHUResult result = ECSIKeys_Parse(ECSValue_GetString(ECSISettings_GetDefault("ecs.prefix"), ""), true, &KEYS.prefixKey, &KEYS.prefixModifiers);
        SDL_assert(result == SHUResult_Ok);
        (void)result;
    }
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

/// @brief Adds a binding to a list, or changes the binding of the same combination. A function name of NULL removes it. A key text that cannot be read is reported and skipped.
static void ECSIKeys_PutBinding(ECSIKeyBinding **bindings, const char *text, const char *function)
{
    u32 key = 0;
    u32 modifiers = 0;

    if (ECSIKeys_Parse(text, true, &key, &modifiers))
    {
        return;
    }

    for (usz i = 0; i < arrlenu(*bindings); i++)
    {
        ECSIKeyBinding *binding = &(*bindings)[i];

        if (binding->key != key || binding->modifiers != modifiers)
        {
            continue;
        }

        char *copy = function == NULL ? NULL : SDL_strdup(function);
        SDL_free(binding->function);
        SDL_free(binding->text);
        binding->function = copy;
        binding->text = SDL_strdup(text);

        if (copy == NULL || binding->text == NULL)
        {
            SDL_free(binding->function);
            SDL_free(binding->text);
            arrdel(*bindings, i);
        }

        return;
    }

    ECSIKeyBinding binding = {.key = key, .modifiers = modifiers, .text = SDL_strdup(text), .function = function == NULL ? NULL : SDL_strdup(function)};

    if (binding.text == NULL || binding.function == NULL)
    {
        SDL_free(binding.text);
        SDL_free(binding.function);
        return;
    }

    arrput(*bindings, binding);
}

/// @brief Adds the entries of the setting ecs.prefixKeys: key texts to function names, or false to remove a key.
static void ECSIKeys_AddPrefixKey(const char *name, const ECSValue *field, void *userData)
{
    (void)userData;
    ECSIKeys_PutBinding(&KEYS.prefixKeys, name, ECSValue_GetString(field, NULL));
}

/// @brief Reads the keys after the prefix: the defaults, then the setting ecs.prefixKeys, if the setting changed.
static void ECSIKeys_ReadPrefixKeys(void)
{
    if (!KEYS.prefixKeysDirty)
    {
        return;
    }

    KEYS.prefixKeysDirty = false;
    ECSIKeys_FreeBindings(&KEYS.prefixKeys);

    // the value in effect adds to the core's default, so a layer above it changes keys without repeating the others
    ECSIValue_TableForEachField(ECSISettings_GetDefault("ecs.prefixKeys"), ECSIKeys_AddPrefixKey, NULL);
    ECSIValue_TableForEachField(ECSSetting_Get("ecs.prefixKeys"), ECSIKeys_AddPrefixKey, NULL);
}

/// @brief Keeps a binding that matches a key press, if it wins over the best one so far: a higher layer wins, then a more specific scope.
static void ECSIKeys_ConsiderBinding(ECSIBindingSearch *search, const char *text, const char *function)
{
    u32 key = 0;
    u32 modifiers = 0;

    if (function == NULL || ECSIKeys_Parse(text, false, &key, &modifiers) || key != search->key || modifiers != search->modifiers)
    {
        return;
    }

    if (search->function == NULL || search->layer > search->bestLayer || (search->layer == search->bestLayer && search->scope > search->bestScope))
    {
        search->function = function;
        search->bestLayer = search->layer;
        search->bestScope = search->scope;
    }
}

static void ECSIKeys_ConsiderField(const char *name, const ECSValue *field, void *userData)
{
    ECSIKeys_ConsiderBinding(userData, name, ECSValue_GetString(field, NULL));
}

/// @brief Considers every binding of a table of key texts and function names.
static void ECSIKeys_ConsiderTable(ECSIBindingSearch *search, const ECSValue *keys, ECSISettingsLayer layer, ECSIBindingScope scope)
{
    search->layer = layer;
    search->scope = scope;
    ECSIValue_TableForEachField(keys, ECSIKeys_ConsiderField, search);
}

static void ECSIKeys_CheckKey(const char *name, const ECSValue *field, void *userData)
{
    (void)field;
    (void)userData;
    u32 key = 0;
    u32 modifiers = 0;
    (void)ECSIKeys_Parse(name, true, &key, &modifiers);
}

/// @brief Reports a key that runs a function its owner does not have, though the owner runs.
static void ECSIKeys_ReportFunction(const char *key, const char *function, const char *place)
{
    if (ECSIServices_GetDescription(function) == NULL && ECSIPlugins_OwnerRuns(function))
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "The key '%s' in %s runs '%s', which does not exist.", key, place, function);
    }
}

/// @brief Reports a binding of a table that runs a function that does not exist; the data names where the table comes from.
static void ECSIKeys_ReportField(const char *name, const ECSValue *field, void *userData)
{
    const char *function = ECSValue_GetString(field, NULL);

    if (function != NULL)
    {
        ECSIKeys_ReportFunction(name, function, userData);
    }
}

/// @brief Reports the key texts of a table of bindings that are not key combinations; they never match.
static void ECSIKeys_CheckKeys(const ECSValue *keys)
{
    ECSIValue_TableForEachField(keys, ECSIKeys_CheckKey, NULL);
}

static void ECSIKeys_FreePanelBinding(ECSIPanelBinding *binding)
{
    SDL_free(binding->panelType);
    SDL_free(binding->setting);
    SDL_free(binding->function);
}

/// @brief Marks a setting to be read again; given as the Changed function of the input settings.
static void ECSIKeys_SettingChanged(void *data)
{
    *(bool *)data = true;
}

#pragma endregion Source Only

SHUResult ECSIKeys_Parse(const char *text, bool report, u32 *retKey, u32 *retModifiers)
{
    SDL_assert(text != NULL);
    SDL_assert(retKey != NULL);
    SDL_assert(retModifiers != NULL);

    char *copy = SDL_strdup(text);

    if (copy == NULL)
    {
        return SHUResult_ErrAllocation;
    }

    *retKey = SDLK_UNKNOWN;
    *retModifiers = ECSModifier_None;

    // the parts before the key are modifiers, so a word that is neither makes the text invalid
    usz keyParts = 0;
    char *save = NULL;

    for (char *part = SDL_strtok_r(copy, "+", &save); part != NULL; part = SDL_strtok_r(NULL, "+", &save))
    {
        if (SDL_strcasecmp(part, "Ctrl") == 0)
        {
            *retModifiers |= ECSModifier_Ctrl;
        }
        else if (SDL_strcasecmp(part, "Shift") == 0)
        {
            *retModifiers |= ECSModifier_Shift;
        }
        else if (SDL_strcasecmp(part, "Alt") == 0)
        {
            *retModifiers |= ECSModifier_Alt;
        }
        else if (SDL_strcasecmp(part, "Super") == 0)
        {
            *retModifiers |= ECSModifier_Super;
        }
        else
        {
            *retKey = SDL_GetKeyFromName(part);
            keyParts++;
        }
    }

    SDL_free(copy);

    if (*retKey == SDLK_UNKNOWN || keyParts != 1)
    {
        if (report)
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "'%s' is not a key combination.", text);
        }

        return SHUResult_ErrBadData;
    }

    return SHUResult_Ok;
}

SHUResult ECSIKeys_Initialize(void)
{
    ECSSettingDesc prefix = {
        .name = "ecs.prefix",
        .type = ECSSettingType_Key,
        .description = "The key combination before a core action",
        .Changed = ECSIKeys_SettingChanged,
        .data = &KEYS.prefixDirty,
    };

    ECSSettingDesc prefixKeys = {
        .name = "ecs.prefixKeys",
        .type = ECSSettingType_Table,
        .description = "Keys after the prefix and the functions they run, added to the core's own; false removes a key",
        .Changed = ECSIKeys_SettingChanged,
        .data = &KEYS.prefixKeysDirty,
    };

    SHU_ReturnResult(ECSISettings_DeclareCore(&prefix));
    SHU_ReturnResult(ECSISettings_DeclareCore(&prefixKeys));

    // a prefix that cannot be read falls back to the default, so the default must be a key combination
    u32 key = 0;
    u32 modifiers = 0;
    SHU_ReturnResult(ECSIKeys_Parse(ECSValue_GetString(ECSISettings_GetDefault("ecs.prefix"), ""), true, &key, &modifiers));

    KEYS.prefixDirty = true;
    KEYS.prefixKeysDirty = true;
    ECSIKeys_ReadPrefix();
    ECSIKeys_CheckKeys(ECSISettings_GetKeys(ECSISettingsLayer_Window));
    ECSIKeys_CheckKeys(ECSISettings_GetKeys(ECSISettingsLayer_User));
    return SHUResult_Ok;
}

SHUResult ECSIKeys_SetTool(const ECSValue *keys)
{
    ECSValue_Destroy(&KEYS.toolKeys);
    ECSIKeys_CheckKeys(keys);
    SHU_ReturnResult(ECSValue_Create(&KEYS.toolKeys));
    return ECSIValue_Copy(KEYS.toolKeys, keys);
}

SHUResult ECSIKeys_AddWorkspace(const ECSValue *keys)
{
    ECSValue *copy = NULL;

    if (keys != NULL)
    {
        ECSIKeys_CheckKeys(keys);
        SHU_ReturnResult(ECSValue_Create(&copy));
        SHU_ReturnResult(ECSIValue_Copy(copy, keys), ECSValue_Destroy(&copy););
    }

    arrput(KEYS.workspaceKeys, copy);
    return SHUResult_Ok;
}

const ECSValue *ECSIKeys_GetWorkspace(usz index)
{
    return index < arrlenu(KEYS.workspaceKeys) ? KEYS.workspaceKeys[index] : NULL;
}

void ECSIKeys_ReportUnknownFunctions(void)
{
    const ECSValue *tables[] = {KEYS.toolKeys, ECSISettings_GetKeys(ECSISettingsLayer_Window), ECSISettings_GetKeys(ECSISettingsLayer_User)};
    const char *places[] = {"the preset", "the settings window's file", "the user's settings"};

    for (usz i = 0; i < SDL_arraysize(tables); i++)
    {
        ECSIValue_TableForEachField(tables[i], ECSIKeys_ReportField, (void *)places[i]);
    }

    for (usz i = 0; i < arrlenu(KEYS.workspaceKeys); i++)
    {
        ECSIValue_TableForEachField(KEYS.workspaceKeys[i], ECSIKeys_ReportField, "a workspace of the preset");
    }

    const ECSIKeyBinding *prefixKeys = ECSIKeys_GetPrefixKeys();

    for (usz i = 0; i < arrlenu(prefixKeys); i++)
    {
        ECSIKeys_ReportFunction(prefixKeys[i].text, prefixKeys[i].function, "ecs.prefixKeys");
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
    ECSIBindingSearch search = {.key = key, .modifiers = modifiers};
    usz workspace = ECSILayout_GetCurrentWorkspace();

    ECSIKeys_ConsiderTable(&search, ECSISettings_GetKeys(ECSISettingsLayer_User), ECSISettingsLayer_User, ECSIBindingScope_Tool);
    ECSIKeys_ConsiderTable(&search, ECSISettings_GetKeys(ECSISettingsLayer_Window), ECSISettingsLayer_Window, ECSIBindingScope_Tool);
    ECSIKeys_ConsiderTable(&search, KEYS.toolKeys, ECSISettingsLayer_Preset, ECSIBindingScope_Tool);

    if (workspace < arrlenu(KEYS.workspaceKeys))
    {
        ECSIKeys_ConsiderTable(&search, KEYS.workspaceKeys[workspace], ECSISettingsLayer_Preset, ECSIBindingScope_Workspace);
    }

    // a plugin's binding counts in the layer that sets its key setting
    for (usz i = 0; focus != NULL && i < arrlenu(KEYS.panelBindings); i++)
    {
        ECSIPanelBinding *binding = &KEYS.panelBindings[i];
        ECSPlugin owner = NULL;
        ECSSettingType type = ECSSettingType_Key;

        if (SDL_strcmp(binding->panelType, focus->typeName) == 0 && ECSISettings_Describe(binding->setting, &owner, &type, &search.layer))
        {
            search.scope = ECSIBindingScope_PanelType;
            ECSIKeys_ConsiderBinding(&search, ECSValue_GetString(ECSSetting_Get(binding->setting), ""), binding->function);
        }
    }

    return search.function;
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

    for (usz i = 0; i < arrlenu(KEYS.panelBindings); i++)
    {
        const ECSIPanelBinding *binding = &KEYS.panelBindings[i];

        if (SDL_strcmp(binding->panelType, panelType) == 0 && SDL_strcmp(binding->function, function) == 0)
        {
            const char *key = ECSValue_GetString(ECSSetting_Get(binding->setting), NULL);
            return key == NULL ? NULL : SDL_strdup(key);
        }
    }

    return NULL;
}

SHUResult ECSKey_Bind(ECSPlugin plugin, const char *panelType, const char *setting, const char *function)
{
    SDL_assert(plugin != NULL);
    SDL_assert(panelType != NULL && setting != NULL && function != NULL);

    ECSPlugin owner = NULL;
    ECSSettingType type = ECSSettingType_Bool;
    ECSISettingsLayer layer = ECSISettingsLayer_Core;

    // a plugin binds keys only for its own panel types, with its own key settings
    if (!ECSIPlugin_OwnsName(plugin, panelType))
    {
        return SHUResult_ErrBadData;
    }

    if (!ECSISettings_Describe(setting, &owner, &type, &layer) || owner != plugin || type != ECSSettingType_Key)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' binds a key with '%s', which is not one of its key settings.", ECSIPlugin_GetName(plugin), setting);
        return SHUResult_ErrBadData;
    }

    u32 key = 0;
    u32 modifiers = 0;
    ECSIKeys_ReadPrefix();

    if (ECSIKeys_Parse(ECSValue_GetString(ECSSetting_Get(setting), ""), true, &key, &modifiers) == SHUResult_Ok && key == KEYS.prefixKey && modifiers == KEYS.prefixModifiers)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "The key of '%s' is the core prefix; the binding is never triggered.", setting);
    }

    ECSIPanelBinding binding = {.plugin = plugin, .panelType = SDL_strdup(panelType), .setting = SDL_strdup(setting), .function = SDL_strdup(function)};

    if (binding.panelType == NULL || binding.setting == NULL || binding.function == NULL)
    {
        ECSIKeys_FreePanelBinding(&binding);
        return SHUResult_ErrAllocation;
    }

    arrput(KEYS.panelBindings, binding);
    return SHUResult_Ok;
}
