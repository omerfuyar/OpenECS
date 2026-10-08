#include "interface/Keys.h"

#include "interface/Layout.h"
#include "runtime/Plugins.h"
#include "runtime/Settings.h"

#include "SDL3/SDL.h"
#include "stb/stbSDL3.h"

#pragma region Source Only

/// @brief Default of the setting ecs.prefix.
#define OPENECS_DEFAULT_PREFIX "Alt+W"

/// @brief The default keys after the core prefix, with the functions they run. The setting ecs.prefix_keys adds to them and changes them.
static const char *const ECSI_PREFIX_KEYS[][2] = {
    {"Left", "ecs.focus_left"},
    {"Right", "ecs.focus_right"},
    {"Up", "ecs.focus_up"},
    {"Down", "ecs.focus_down"},
    {"Shift+Left", "ecs.move_left"},
    {"Shift+Right", "ecs.move_right"},
    {"Shift+Up", "ecs.move_up"},
    {"Shift+Down", "ecs.move_down"},
    {"Tab", "ecs.next_tab"},
    {"M", "ecs.maximize"},
    {"X", "ecs.close"},
    {"Shift+X", "ecs.close_group"},
    {"L", "ecs.lock"},
    {"1", "ecs.workspace_1"},
    {"2", "ecs.workspace_2"},
    {"3", "ecs.workspace_3"},
    {"4", "ecs.workspace_4"},
    {"5", "ecs.workspace_5"},
    {"6", "ecs.workspace_6"},
    {"7", "ecs.workspace_7"},
    {"8", "ecs.workspace_8"},
    {"9", "ecs.workspace_9"},
    {"0", "ecs.workspace_10"},
    {"T", "ecs.reopen"},
    {"R", "ecs.restart"},
};

/// @brief A plugin's binding for its panel type. The key is the value of a key setting, so the user can change it.
typedef struct ECSI_PanelBinding
{
    ECSPlugin plugin;
    char *panelType;
    char *setting;
    char *function;
} ECSI_PanelBinding;

/// @brief How specific a binding is; a more specific binding wins within one settings layer.
typedef enum ECSI_BindingScope
{
    ECSI_BindingScope_Tool = 0,
    ECSI_BindingScope_Workspace,
    ECSI_BindingScope_PanelType,
} ECSI_BindingScope;

/// @brief The binding that a key press runs, found by ECSI_KeysFind.
typedef struct ECSI_BindingSearch
{
    u32 key;
    u32 modifiers;
    ECSI_SettingsLayer layer; // of the table being searched
    ECSI_BindingScope scope;  // of the table being searched
    const char *function;     // the best binding so far, or NULL
    ECSI_SettingsLayer bestLayer;
    ECSI_BindingScope bestScope;
} ECSI_BindingSearch;

static struct
{
    u32 prefixKey;
    u32 prefixModifiers;
    bool prefixDirty;                 // ecs.prefix changed and is read again at the next key press
    ECSI_KeyBinding *prefixKeys;      // stb_ds array of the keys after the prefix
    bool prefixKeysDirty;             // ecs.prefix_keys changed and is read again when they are used
    ECSI_PanelBinding *panelBindings; // stb_ds array of plugins' bindings for their panel types
    ECSValue *toolKeys;               // the preset's bindings for the whole tool, or NULL
    ECSValue **workspaceKeys;         // stb_ds array of the preset's bindings for each workspace; NULL for none
} KEYS = {0};

/// @brief Reads the core prefix from the setting ecs.prefix if the setting changed. A key text that cannot be read is reported, and the default is used.
static void ECSI_KeysReadPrefix(void)
{
    if (!KEYS.prefixDirty)
    {
        return;
    }

    KEYS.prefixDirty = false;

    if (ECSI_KeysParse(ECSValue_GetString(ECSSetting_Get("ecs.prefix"), OPENECS_DEFAULT_PREFIX), true, &KEYS.prefixKey, &KEYS.prefixModifiers))
    {
        SHUResult result = ECSI_KeysParse(OPENECS_DEFAULT_PREFIX, true, &KEYS.prefixKey, &KEYS.prefixModifiers);
        SDL_assert(result == SHUResult_Ok);
        (void)result;
    }
}

static void ECSI_KeysFreeBindings(ECSI_KeyBinding **bindings)
{
    for (usz i = 0; i < arrlenu(*bindings); i++)
    {
        SDL_free((*bindings)[i].text);
        SDL_free((*bindings)[i].function);
    }

    arrfree(*bindings);
}

/// @brief Adds a binding to a list, or changes the binding of the same combination. A function name of NULL removes it. A key text that cannot be read is reported and skipped.
static void ECSI_KeysPutBinding(ECSI_KeyBinding **bindings, const char *text, const char *function)
{
    u32 key = 0;
    u32 modifiers = 0;

    if (ECSI_KeysParse(text, true, &key, &modifiers))
    {
        return;
    }

    for (usz i = 0; i < arrlenu(*bindings); i++)
    {
        ECSI_KeyBinding *binding = &(*bindings)[i];

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

    ECSI_KeyBinding binding = {.key = key, .modifiers = modifiers, .text = SDL_strdup(text), .function = function == NULL ? NULL : SDL_strdup(function)};

    if (binding.text == NULL || binding.function == NULL)
    {
        SDL_free(binding.text);
        SDL_free(binding.function);
        return;
    }

    arrput(*bindings, binding);
}

/// @brief Adds the entries of the setting ecs.prefix_keys: key texts to function names, or false to remove a key.
static void ECSI_KeysAddPrefixKey(const char *name, const ECSValue *field, void *userData)
{
    (void)userData;
    ECSI_KeysPutBinding(&KEYS.prefixKeys, name, ECSValue_GetString(field, NULL));
}

/// @brief Reads the keys after the prefix: the defaults, then the setting ecs.prefix_keys, if the setting changed.
static void ECSI_KeysReadPrefixKeys(void)
{
    if (!KEYS.prefixKeysDirty)
    {
        return;
    }

    KEYS.prefixKeysDirty = false;
    ECSI_KeysFreeBindings(&KEYS.prefixKeys);

    for (usz i = 0; i < SDL_arraysize(ECSI_PREFIX_KEYS); i++)
    {
        ECSI_KeysPutBinding(&KEYS.prefixKeys, ECSI_PREFIX_KEYS[i][0], ECSI_PREFIX_KEYS[i][1]);
    }

    ECSI_ValueTableForEachField(ECSSetting_Get("ecs.prefix_keys"), ECSI_KeysAddPrefixKey, NULL);
}

/// @brief Keeps a binding that matches a key press, if it wins over the best one so far: a higher layer wins, then a more specific scope.
static void ECSI_KeysConsiderBinding(ECSI_BindingSearch *search, const char *text, const char *function)
{
    u32 key = 0;
    u32 modifiers = 0;

    if (function == NULL || ECSI_KeysParse(text, false, &key, &modifiers) || key != search->key || modifiers != search->modifiers)
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

static void ECSI_KeysConsiderField(const char *name, const ECSValue *field, void *userData)
{
    ECSI_KeysConsiderBinding(userData, name, ECSValue_GetString(field, NULL));
}

/// @brief Considers every binding of a table of key texts and function names.
static void ECSI_KeysConsiderTable(ECSI_BindingSearch *search, const ECSValue *keys, ECSI_SettingsLayer layer, ECSI_BindingScope scope)
{
    search->layer = layer;
    search->scope = scope;
    ECSI_ValueTableForEachField(keys, ECSI_KeysConsiderField, search);
}

static void ECSI_KeysCheckKey(const char *name, const ECSValue *field, void *userData)
{
    (void)field;
    (void)userData;
    u32 key = 0;
    u32 modifiers = 0;
    (void)ECSI_KeysParse(name, true, &key, &modifiers);
}

/// @brief Reports the key texts of a table of bindings that are not key combinations; they never match.
static void ECSI_KeysCheckKeys(const ECSValue *keys)
{
    ECSI_ValueTableForEachField(keys, ECSI_KeysCheckKey, NULL);
}

static void ECSI_KeysFreePanelBinding(ECSI_PanelBinding *binding)
{
    SDL_free(binding->panelType);
    SDL_free(binding->setting);
    SDL_free(binding->function);
}

/// @brief Marks a setting to be read again; given as the Changed function of the input settings.
static void ECSI_KeysSettingChanged(void *data)
{
    *(bool *)data = true;
}

#pragma endregion Source Only

SHUResult ECSI_KeysParse(const char *text, bool report, u32 *retKey, u32 *retModifiers)
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
        }
    }

    SDL_free(copy);

    if (*retKey == SDLK_UNKNOWN)
    {
        if (report)
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "'%s' is not a key combination.", text);
        }

        return SHUResult_ErrBadData;
    }

    return SHUResult_Ok;
}

SHUResult ECSI_KeysInitialize(void)
{
    ECSSettingDesc prefix = {
        .name = "ecs.prefix",
        .type = ECSSettingType_Key,
        .description = "The key combination before a core action",
        .defaultString = OPENECS_DEFAULT_PREFIX,
        .Changed = ECSI_KeysSettingChanged,
        .data = &KEYS.prefixDirty,
    };

    ECSSettingDesc prefixKeys = {
        .name = "ecs.prefix_keys",
        .type = ECSSettingType_Table,
        .description = "Keys after the prefix and the functions they run, added to the core's own; false removes a key",
        .Changed = ECSI_KeysSettingChanged,
        .data = &KEYS.prefixKeysDirty,
    };

    SHU_ReturnResult(ECSI_SettingsDeclareCore(&prefix));
    SHU_ReturnResult(ECSI_SettingsDeclareCore(&prefixKeys));

    KEYS.prefixDirty = true;
    KEYS.prefixKeysDirty = true;
    ECSI_KeysReadPrefix();
    ECSI_KeysCheckKeys(ECSI_SettingsGetKeys(ECSI_SettingsLayer_Window));
    ECSI_KeysCheckKeys(ECSI_SettingsGetKeys(ECSI_SettingsLayer_User));
    return SHUResult_Ok;
}

SHUResult ECSI_KeysSetTool(const ECSValue *keys)
{
    ECSValue_Destroy(&KEYS.toolKeys);
    ECSI_KeysCheckKeys(keys);
    SHU_ReturnResult(ECSValue_Create(&KEYS.toolKeys));
    return ECSI_ValueCopy(KEYS.toolKeys, keys);
}

SHUResult ECSI_KeysAddWorkspace(const ECSValue *keys)
{
    ECSValue *copy = NULL;

    if (keys != NULL)
    {
        ECSI_KeysCheckKeys(keys);
        SHU_ReturnResult(ECSValue_Create(&copy));
        SHU_ReturnResult(ECSI_ValueCopy(copy, keys), ECSValue_Destroy(&copy););
    }

    arrput(KEYS.workspaceKeys, copy);
    return SHUResult_Ok;
}

const ECSValue *ECSI_KeysGetWorkspace(usz index)
{
    return index < arrlenu(KEYS.workspaceKeys) ? KEYS.workspaceKeys[index] : NULL;
}

void ECSI_KeysRemovePlugin(ECSPlugin plugin)
{
    SDL_assert(plugin != NULL);

    for (usz i = arrlenu(KEYS.panelBindings); i > 0; i--)
    {
        if (KEYS.panelBindings[i - 1].plugin == plugin)
        {
            ECSI_KeysFreePanelBinding(&KEYS.panelBindings[i - 1]);
            arrdel(KEYS.panelBindings, i - 1);
        }
    }
}

void ECSI_KeysTerminate(void)
{
    ECSI_KeysFreeBindings(&KEYS.prefixKeys);

    for (usz i = 0; i < arrlenu(KEYS.panelBindings); i++)
    {
        ECSI_KeysFreePanelBinding(&KEYS.panelBindings[i]);
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

const char *ECSI_KeysFind(u32 key, u32 modifiers, ECSPanel focus)
{
    ECSI_BindingSearch search = {.key = key, .modifiers = modifiers};
    usz workspace = ECSI_LayoutGetCurrentWorkspace();

    ECSI_KeysConsiderTable(&search, ECSI_SettingsGetKeys(ECSI_SettingsLayer_User), ECSI_SettingsLayer_User, ECSI_BindingScope_Tool);
    ECSI_KeysConsiderTable(&search, ECSI_SettingsGetKeys(ECSI_SettingsLayer_Window), ECSI_SettingsLayer_Window, ECSI_BindingScope_Tool);
    ECSI_KeysConsiderTable(&search, KEYS.toolKeys, ECSI_SettingsLayer_Preset, ECSI_BindingScope_Tool);

    if (workspace < arrlenu(KEYS.workspaceKeys))
    {
        ECSI_KeysConsiderTable(&search, KEYS.workspaceKeys[workspace], ECSI_SettingsLayer_Preset, ECSI_BindingScope_Workspace);
    }

    // a plugin's binding counts in the layer that sets its key setting
    for (usz i = 0; focus != NULL && i < arrlenu(KEYS.panelBindings); i++)
    {
        ECSI_PanelBinding *binding = &KEYS.panelBindings[i];
        ECSPlugin owner = NULL;
        ECSSettingType type = ECSSettingType_Key;

        if (SDL_strcmp(binding->panelType, focus->typeName) == 0 && ECSI_SettingsDescribe(binding->setting, &owner, &type, &search.layer))
        {
            search.scope = ECSI_BindingScope_PanelType;
            ECSI_KeysConsiderBinding(&search, ECSValue_GetString(ECSSetting_Get(binding->setting), ""), binding->function);
        }
    }

    return search.function;
}

bool ECSI_KeysIsPrefix(u32 key, u32 modifiers)
{
    ECSI_KeysReadPrefix();
    return key == KEYS.prefixKey && (modifiers & ECSModifier_AltGr) == 0 && modifiers == KEYS.prefixModifiers;
}

const ECSI_KeyBinding *ECSI_KeysGetPrefixKeys(void)
{
    ECSI_KeysReadPrefixKeys();
    return KEYS.prefixKeys;
}

char *ECSI_KeysPrefixTextOf(const char *function)
{
    SDL_assert(function != NULL);

    const char *prefix = ECSValue_GetString(ECSSetting_Get("ecs.prefix"), OPENECS_DEFAULT_PREFIX);
    const ECSI_KeyBinding *keys = ECSI_KeysGetPrefixKeys();
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

char *ECSI_KeysBoundTextOf(const char *panelType, const char *function)
{
    SDL_assert(panelType != NULL);
    SDL_assert(function != NULL);

    for (usz i = 0; i < arrlenu(KEYS.panelBindings); i++)
    {
        const ECSI_PanelBinding *binding = &KEYS.panelBindings[i];

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
    ECSI_SettingsLayer layer = ECSI_SettingsLayer_Core;

    // a plugin binds keys only for its own panel types, with its own key settings
    if (!ECSI_PluginOwnsName(plugin, panelType))
    {
        return SHUResult_ErrBadData;
    }

    if (!ECSI_SettingsDescribe(setting, &owner, &type, &layer) || owner != plugin || type != ECSSettingType_Key)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' binds a key with '%s', which is not one of its key settings.", ECSI_PluginGetName(plugin), setting);
        return SHUResult_ErrBadData;
    }

    u32 key = 0;
    u32 modifiers = 0;
    ECSI_KeysReadPrefix();

    if (ECSI_KeysParse(ECSValue_GetString(ECSSetting_Get(setting), ""), true, &key, &modifiers) == SHUResult_Ok && key == KEYS.prefixKey && modifiers == KEYS.prefixModifiers)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "The key of '%s' is the core prefix; the binding is never triggered.", setting);
    }

    ECSI_PanelBinding binding = {.plugin = plugin, .panelType = SDL_strdup(panelType), .setting = SDL_strdup(setting), .function = SDL_strdup(function)};

    if (binding.panelType == NULL || binding.setting == NULL || binding.function == NULL)
    {
        ECSI_KeysFreePanelBinding(&binding);
        return SHUResult_ErrAllocation;
    }

    arrput(KEYS.panelBindings, binding);
    return SHUResult_Ok;
}
