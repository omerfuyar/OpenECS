#include "runtime/Settings.h"

#include "base/Lua.h"

#include "SDL3/SDL.h"
#include "stb/stbSDL3.h"

#pragma region Source Only

/// @brief The user's hand-edited settings file, in the configuration folder.
#define OPENECS_SETTINGS_USER_FILE "settings.lua"
/// @brief The file that the settings window writes, in the configuration folder.
#define OPENECS_SETTINGS_WINDOW_FILE "settings-window.lua"

/// @brief What a value of each setting type must be, for messages.
static const char *const OPENECS_SETTING_TYPE_TEXTS[] = {
    [ECSSettingType_Bool] = "true or false",
    [ECSSettingType_Integer] = "a whole number",
    [ECSSettingType_Number] = "a number",
    [ECSSettingType_String] = "a text",
    [ECSSettingType_Choice] = "one of these texts:",
    [ECSSettingType_Key] = "a key combination text",
    [ECSSettingType_List] = "a list",
    [ECSSettingType_Table] = "a table",
};

/// @brief A declared setting.
typedef struct ECSISetting
{
    char *name;
    char *description;
    ECSSettingType type;
    ECSPlugin owner; // NULL for the core
    ECSValue *defaultValue;
    char **choices;        // stb_ds array; choice settings only
    const ECSValue *value; // the value in effect: the default, or a value of a layer
    ECSISettingsLayer layer;
    ECSSettingChangedFunction Changed;
    void *data;
} ECSISetting;

/// @brief Names of the layers, for explanations; the core and plugin layers hold defaults.
static const char *const OPENECS_SETTINGS_LAYER_NAMES[ECSISettingsLayer_Count] = {"default", "default", "preset", "window", "user"};

/// @brief Names of the setting types, for explanations, in the order of ECSSettingType.
static const char *const OPENECS_SETTING_TYPE_NAMES[] = {"bool", "integer", "number", "string", "choice", "key", "list", "table"};

/// @brief Copies every field of a table into another, while its fields are walked.
typedef struct ECSISettingsFileCopier
{
    ECSValue *target;
    SHUResult result;
} ECSISettingsFileCopier;

/// @brief Collects what a user file says, while its fields are walked.
typedef struct ECSISettingsFileReader
{
    ECSValue *layer;
    SHUResult result;
} ECSISettingsFileReader;

static struct
{
    struct
    {
        char *key; // the setting's own copy of its name
        ECSISetting *value;
    } *settings;                                // stb_ds hash map, in declaration order
    ECSValue *layers[ECSISettingsLayer_Count]; // the preset, window and user layers: tables of setting names and values
    char *paths[ECSISettingsLayer_Count];      // the file of each of those layers
    ECSValue *plugins;                          // list of extra plugin names
    ECSValue *keys[ECSISettingsLayer_Count];   // the window and user layers' key bindings: key texts and function names
    ECSValue *windowFile;                       // the whole settings window's file, which ECSSetting_Set changes and writes
    char *appId;                                // chooses the tool's own part of the user's files
    ECSISetting **changed;                     // stb_ds array of settings whose owners are not told yet
} SETTINGS = {0};

/// @brief Checks that a value has the type of a setting.
static bool ECSISetting_Check(const ECSISetting *setting, const ECSValue *value)
{
    ECSValueType type = ECSValue_GetType(value);

    switch (setting->type)
    {
    case ECSSettingType_Bool:
        return type == ECSValueType_Bool;
    case ECSSettingType_Integer:
        // a number without a fraction reads the same with any fallback
        return ECSValue_GetInteger(value, 0) == ECSValue_GetInteger(value, 1);
    case ECSSettingType_Number:
        return type == ECSValueType_Integer || type == ECSValueType_Number;
    case ECSSettingType_String:
    case ECSSettingType_Key:
        return type == ECSValueType_String;
    case ECSSettingType_Choice:
        for (usz i = 0; type == ECSValueType_String && i < arrlenu(setting->choices); i++)
        {
            if (SDL_strcmp(setting->choices[i], ECSValue_GetString(value, "")) == 0)
            {
                return true;
            }
        }

        return false;
    case ECSSettingType_List:
    case ECSSettingType_Table:
        return type == ECSValueType_Table;
    }

    return false;
}

/// @brief Writes a choice setting's choices for a message, after a space; nothing for other settings.
static void ECSISetting_WriteChoices(const ECSISetting *setting, SHUSlice text)
{
    SDL_strlcpy(text.data, "", text.size);

    for (usz i = 0; i < arrlenu(setting->choices); i++)
    {
        SDL_strlcat(text.data, i == 0 ? " " : ", ", text.size);
        SDL_strlcat(text.data, setting->choices[i], text.size);
    }
}

/// @brief Reports a value of the wrong type for a setting.
/// @param file The file that gave the value, or NULL if a plugin did.
static void ECSISetting_ReportType(const ECSISetting *setting, const char *file)
{
    char choices[256];
    ECSISetting_WriteChoices(setting, cs(choices, sizeof(choices)));

    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Setting '%s'%s%s%s must be %s%s; that value is ignored.", setting->name, file == NULL ? "" : " in '", file == NULL ? "" : file, file == NULL ? "" : "'", OPENECS_SETTING_TYPE_TEXTS[setting->type], choices);
}

/// @brief Finds the value in effect of a setting: the highest layer that sets it with the right type, or the default.
static void ECSISetting_Resolve(ECSISetting *setting)
{
    for (i32 layer = ECSISettingsLayer_User; layer >= ECSISettingsLayer_Preset; layer--)
    {
        const ECSValue *value = ECSValue_GetTableField(SETTINGS.layers[layer], setting->name);

        if (value == NULL)
        {
            continue;
        }

        if (ECSISetting_Check(setting, value))
        {
            setting->value = value;
            setting->layer = (ECSISettingsLayer)layer;
            return;
        }

        ECSISetting_ReportType(setting, SETTINGS.paths[layer]);
    }

    setting->value = setting->defaultValue;
    setting->layer = setting->owner == NULL ? ECSISettingsLayer_Core : ECSISettingsLayer_Plugin;
}

static void ECSISetting_Free(ECSISetting *setting)
{
    for (usz i = 0; i < arrlenu(setting->choices); i++)
    {
        SDL_free(setting->choices[i]);
    }

    arrfree(setting->choices);
    ECSValue_Destroy(&setting->defaultValue);
    SDL_free(setting->name);
    SDL_free(setting->description);
    SDL_free(setting);
}

/// @brief Sets a new setting's default from its description.
static SHUResult ECSISetting_SetDefault(ECSISetting *setting, const ECSSettingDesc *desc)
{
    SHU_ReturnResult(ECSValue_Create(&setting->defaultValue));

    switch (desc->type)
    {
    case ECSSettingType_Bool:
        ECSValue_SetBool(setting->defaultValue, desc->defaultBool);
        return SHUResult_Ok;
    case ECSSettingType_Integer:
        ECSValue_SetInteger(setting->defaultValue, desc->defaultInteger);
        return SHUResult_Ok;
    case ECSSettingType_Number:
        ECSValue_SetNumber(setting->defaultValue, desc->defaultNumber);
        return SHUResult_Ok;
    case ECSSettingType_String:
    case ECSSettingType_Key:
        return ECSValue_SetString(setting->defaultValue, desc->defaultString == NULL ? "" : desc->defaultString);
    case ECSSettingType_Choice:
        for (const char *const *choice = desc->choices; *choice != NULL; choice++)
        {
            char *copy = SDL_strdup(*choice);

            if (copy == NULL)
            {
                return SHUResult_ErrAllocation;
            }

            arrput(setting->choices, copy);
        }

        // the first choice is the default if the description names none
        SHU_ReturnResult(ECSValue_SetString(setting->defaultValue, desc->defaultString == NULL ? setting->choices[0] : desc->defaultString));

        if (!ECSISetting_Check(setting, setting->defaultValue))
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "The default of setting '%s' is not one of its choices.", desc->name);
            return SHUResult_ErrBadData;
        }

        return SHUResult_Ok;
    case ECSSettingType_List:
    case ECSSettingType_Table:
        ECSValue_SetTable(setting->defaultValue);
        return SHUResult_Ok;
    }

    return SHUResult_ErrBadData;
}

/// @brief Declares a setting for the core or a plugin; the caller has checked the name.
/// @param defaultValue The default as a value, or NULL to take it from the description.
static SHUResult ECSISettings_Declare(ECSPlugin owner, const ECSSettingDesc *desc, const ECSValue *defaultValue)
{
    if (desc->type > ECSSettingType_Table || (desc->type == ECSSettingType_Choice && (desc->choices == NULL || desc->choices[0] == NULL)))
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Setting '%s' has an invalid type or no choices.", desc->name);
        return SHUResult_ErrBadData;
    }

    if (shget(SETTINGS.settings, desc->name) != NULL)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Setting '%s' is already declared.", desc->name);
        return SHUResult_ErrBadData;
    }

    ECSISetting *setting = SDL_calloc(1, sizeof(ECSISetting));

    if (setting == NULL)
    {
        return SHUResult_ErrAllocation;
    }

    setting->name = SDL_strdup(desc->name);
    setting->description = SDL_strdup(desc->description == NULL ? "" : desc->description);
    setting->type = desc->type;
    setting->owner = owner;
    setting->Changed = desc->Changed;
    setting->data = desc->data;

    if (setting->name == NULL || setting->description == NULL)
    {
        ECSISetting_Free(setting);
        return SHUResult_ErrAllocation;
    }

    SHU_ReturnResult(ECSISetting_SetDefault(setting, desc), ECSISetting_Free(setting););

    if (defaultValue != NULL)
    {
        if (!ECSISetting_Check(setting, defaultValue))
        {
            char choices[256];
            ECSISetting_WriteChoices(setting, cs(choices, sizeof(choices)));
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "The default of setting '%s' must be %s%s.", desc->name, OPENECS_SETTING_TYPE_TEXTS[desc->type], choices);
            ECSISetting_Free(setting);
            return SHUResult_ErrBadData;
        }

        SHU_ReturnResult(ECSIValue_Copy(setting->defaultValue, defaultValue), ECSISetting_Free(setting););
    }

    shput(SETTINGS.settings, setting->name, setting);
    ECSISetting_Resolve(setting);
    return SHUResult_Ok;
}

/// @brief Copies a field of a user file into its layer if its name is a setting's: plugin or core name, a dot, local name.
static void ECSISettings_ReadField(const char *name, const ECSValue *field, void *userData)
{
    ECSISettingsFileReader *reader = userData;
    ECSValue *copy = NULL;

    if (reader->result || SDL_strchr(name, '.') == NULL)
    {
        return;
    }

    reader->result = ECSValue_TableSetField(reader->layer, name, &copy);
    reader->result = reader->result ? reader->result : ECSIValue_Copy(copy, field);
}

static void ECSISettings_CopyField(const char *name, const ECSValue *field, void *userData)
{
    ECSISettingsFileCopier *copier = userData;
    ECSValue *copy = NULL;

    if (!copier->result)
    {
        copier->result = ECSValue_TableSetField(copier->target, name, &copy);
        copier->result = copier->result ? copier->result : ECSIValue_Copy(copy, field);
    }
}

/// @brief Adds the plugins that a part of a user file names.
static SHUResult ECSISettings_ReadPlugins(const ECSValue *part)
{
    const ECSValue *plugins = ECSValue_GetTableField(part, "plugins");

    for (usz i = 0; i < ECSValue_GetListCount(plugins); i++)
    {
        const char *name = ECSValue_GetString(ECSValue_GetListItem(plugins, i), NULL);
        ECSValue *item = NULL;

        if (name != NULL)
        {
            SHU_ReturnResult(ECSValue_ListAddItem(SETTINGS.plugins, &item));
            SHU_ReturnResult(ECSValue_SetString(item, name));
        }
    }

    return SHUResult_Ok;
}

/// @brief Builds a user file's layer: the part for every tool, then the part for this tool, which wins.
static SHUResult ECSISettings_BuildLayer(ECSISettingsLayer layer, const ECSValue *file)
{
    ECSValue_Destroy(&SETTINGS.layers[layer]);
    SHU_ReturnResult(ECSValue_Create(&SETTINGS.layers[layer]));
    ECSValue_SetTable(SETTINGS.layers[layer]);

    const ECSValue *tool = ECSValue_GetTableField(ECSValue_GetTableField(file, "tools"), SETTINGS.appId);
    ECSISettingsFileReader reader = {.layer = SETTINGS.layers[layer], .result = SHUResult_Ok};
    ECSIValue_TableForEachField(file, ECSISettings_ReadField, &reader);
    ECSIValue_TableForEachField(tool, ECSISettings_ReadField, &reader);
    return reader.result;
}

/// @brief Copies the key bindings of a part of a user file into a layer's keys.
static SHUResult ECSISettings_ReadKeys(ECSISettingsLayer layer, const ECSValue *part)
{
    ECSISettingsFileCopier copier = {.target = SETTINGS.keys[layer], .result = SHUResult_Ok};
    ECSIValue_TableForEachField(ECSValue_GetTableField(part, "keys"), ECSISettings_CopyField, &copier);
    return copier.result;
}

/// @brief Reads a user file into its layer and adds the plugins it names. A missing file, or one that cannot be read, gives an empty table.
/// @param retFile The whole file, kept by the caller, or NULL to free it.
static SHUResult ECSISettings_ReadFile(ECSISettingsLayer layer, const char *folder, const char *fileName, ECSValue **retFile)
{
    if (SDL_asprintf(&SETTINGS.paths[layer], "%s%s", folder, fileName) < 0)
    {
        SETTINGS.paths[layer] = NULL;
        return SHUResult_ErrAllocation;
    }

    ECSValue *file = NULL;
    SHU_ReturnResult(ECSValue_Create(&file));
    SHUResult result = SDL_GetPathInfo(SETTINGS.paths[layer], NULL) ? ECSILua_ReadData(SETTINGS.paths[layer], file) : SHUResult_Ok;

    // a file that cannot be read is already reported, and skipped
    if (result == SHUResult_ErrFile || result == SHUResult_ErrBadData)
    {
        result = SHUResult_Ok;
    }

    if (ECSValue_GetType(file) != ECSValueType_Table)
    {
        ECSValue_SetTable(file);
    }

    result = result ? result : ECSISettings_BuildLayer(layer, file);
    result = result ? result : ECSValue_Create(&SETTINGS.keys[layer]);

    if (!result)
    {
        ECSValue_SetTable(SETTINGS.keys[layer]);
        result = ECSISettings_ReadKeys(layer, file);
        result = result ? result : ECSISettings_ReadKeys(layer, ECSValue_GetTableField(ECSValue_GetTableField(file, "tools"), SETTINGS.appId));
    }

    result = result ? result : ECSISettings_ReadPlugins(file);
    result = result ? result : ECSISettings_ReadPlugins(ECSValue_GetTableField(ECSValue_GetTableField(file, "tools"), SETTINGS.appId));

    if (retFile != NULL && !result)
    {
        *retFile = file;
    }
    else
    {
        ECSValue_Destroy(&file);
    }

    return result;
}

/// @brief Adds a field of an explanation, copied from a value.
static SHUResult ECSISettings_ExplainField(ECSValue *explanation, const char *name, const ECSValue *value)
{
    ECSValue *field = NULL;
    SHU_ReturnResult(ECSValue_TableSetField(explanation, name, &field));
    return ECSIValue_Copy(field, value);
}

/// @brief Adds a text field of an explanation.
static SHUResult ECSISettings_ExplainText(ECSValue *explanation, const char *name, const char *text)
{
    ECSValue *field = NULL;
    SHU_ReturnResult(ECSValue_TableSetField(explanation, name, &field));
    return ECSValue_SetString(field, text);
}

static ECSISetting *ECSISettings_Find(const char *name)
{
    // shget would allocate a map that is missing
    return SETTINGS.settings == NULL ? NULL : shget(SETTINGS.settings, name);
}

/// @brief Reports a field of a layer that is not a declared setting; the data is the layer's file.
static void ECSISettings_ReportField(const char *name, const ECSValue *field, void *userData)
{
    (void)field;

    if (ECSISettings_Find(name) == NULL && ECSIPlugins_OwnerRuns(name))
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "'%s' in '%s' is not a declared setting; it is kept.", name, (const char *)userData);
    }
}

#pragma endregion Source Only

SHUResult ECSISettings_Initialize(const char *corePath, const ECSValue *presetSettings, const char *presetPath, const char *appId, const char *configFolder)
{
    SDL_assert(corePath != NULL);
    SDL_assert(presetPath != NULL);
    SDL_assert(appId != NULL);

    // the core's settings file is required: the core's settings have no other values
    ECSValue *coreFile = NULL;
    SHU_ReturnResult(ECSValue_Create(&coreFile));
    SHU_ReturnResult(ECSValue_Create(&SETTINGS.layers[ECSISettingsLayer_Core]), ECSValue_Destroy(&coreFile););
    ECSValue_SetTable(SETTINGS.layers[ECSISettingsLayer_Core]);
    SETTINGS.paths[ECSISettingsLayer_Core] = SDL_strdup(corePath);

    SHU_ReturnResult(ECSValue_Create(&SETTINGS.plugins), ECSValue_Destroy(&coreFile););
    ECSValue_SetTable(SETTINGS.plugins);

    // like a user file, the core's file may name plugins that every tool loads
    ECSISettingsFileReader reader = {.layer = SETTINGS.layers[ECSISettingsLayer_Core], .result = SHUResult_Ok};
    reader.result = SETTINGS.paths[ECSISettingsLayer_Core] == NULL ? SHUResult_ErrAllocation : ECSILua_ReadData(corePath, coreFile);
    ECSIValue_TableForEachField(reader.result ? NULL : coreFile, ECSISettings_ReadField, &reader);
    reader.result = reader.result ? reader.result : ECSISettings_ReadPlugins(coreFile);
    ECSValue_Destroy(&coreFile);
    SHU_ReturnResult(reader.result);

    SHU_ReturnResult(ECSISettings_SetPreset(presetSettings, presetPath));
    SETTINGS.appId = SDL_strdup(appId);

    if (SETTINGS.appId == NULL)
    {
        return SHUResult_ErrAllocation;
    }

    if (configFolder == NULL)
    {
        return SHUResult_Ok;
    }

    SHU_ReturnResult(ECSISettings_ReadFile(ECSISettingsLayer_Window, configFolder, OPENECS_SETTINGS_WINDOW_FILE, &SETTINGS.windowFile));
    return ECSISettings_ReadFile(ECSISettingsLayer_User, configFolder, OPENECS_SETTINGS_USER_FILE, NULL);
}

SHUResult ECSISettings_SetPreset(const ECSValue *presetSettings, const char *presetPath)
{
    SDL_assert(presetPath != NULL);
    SDL_assert(shlenu(SETTINGS.settings) == 0);

    ECSValue_Destroy(&SETTINGS.layers[ECSISettingsLayer_Preset]);
    SDL_free(SETTINGS.paths[ECSISettingsLayer_Preset]);
    SETTINGS.paths[ECSISettingsLayer_Preset] = SDL_strdup(presetPath);
    SHU_ReturnResult(ECSValue_Create(&SETTINGS.layers[ECSISettingsLayer_Preset]));
    SHU_ReturnResult(ECSIValue_Copy(SETTINGS.layers[ECSISettingsLayer_Preset], presetSettings));
    return SETTINGS.paths[ECSISettingsLayer_Preset] == NULL ? SHUResult_ErrAllocation : SHUResult_Ok;
}

bool ECSISettings_PeekBool(const char *name, bool fallback)
{
    for (i32 layer = ECSISettingsLayer_Count - 1; layer >= 0; layer--)
    {
        const ECSValue *value = ECSValue_GetTableField(SETTINGS.layers[layer], name);

        if (ECSValue_GetType(value) == ECSValueType_Bool)
        {
            return ECSValue_GetBool(value, fallback);
        }
    }

    return fallback;
}

void ECSISettings_Terminate(void)
{
    for (usz i = 0; i < shlenu(SETTINGS.settings); i++)
    {
        ECSISetting_Free(SETTINGS.settings[i].value);
    }

    for (usz i = 0; i < ECSISettingsLayer_Count; i++)
    {
        ECSValue_Destroy(&SETTINGS.layers[i]);
        ECSValue_Destroy(&SETTINGS.keys[i]);
        SDL_free(SETTINGS.paths[i]);
    }

    shfree(SETTINGS.settings);
    arrfree(SETTINGS.changed);
    ECSValue_Destroy(&SETTINGS.plugins);
    ECSValue_Destroy(&SETTINGS.windowFile);
    SDL_free(SETTINGS.appId);
    SDL_zero(SETTINGS);
}

SHUResult ECSISettings_DeclareCore(const ECSSettingDesc *desc)
{
    SDL_assert(desc != NULL);
    SDL_assert(desc->name != NULL && SDL_strncmp(desc->name, "ecs.", 4) == 0);

    const ECSValue *value = ECSValue_GetTableField(SETTINGS.layers[ECSISettingsLayer_Core], desc->name);

    if (value == NULL)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "The core's settings file '%s' has no value for '%s'.", SETTINGS.paths[ECSISettingsLayer_Core], desc->name);
        return SHUResult_ErrBadData;
    }

    return ECSISettings_Declare(NULL, desc, value);
}

const ECSValue *ECSISettings_GetDefault(const char *name)
{
    SDL_assert(name != NULL);

    ECSISetting *setting = ECSISettings_Find(name);
    return setting == NULL ? NULL : setting->defaultValue;
}

void ECSISettings_RemovePlugin(ECSPlugin plugin)
{
    SDL_assert(plugin != NULL);

    // backwards, because shdel moves the last setting into the hole
    for (usz i = shlenu(SETTINGS.settings); i > 0; i--)
    {
        ECSISetting *setting = SETTINGS.settings[i - 1].value;

        if (setting->owner == plugin)
        {
            // a removed setting is not told about changes any more
            for (usz j = arrlenu(SETTINGS.changed); j > 0; j--)
            {
                if (SETTINGS.changed[j - 1] == setting)
                {
                    arrdel(SETTINGS.changed, j - 1);
                }
            }

            (void)shdel(SETTINGS.settings, setting->name);
            ECSISetting_Free(setting);
        }
    }
}

const ECSValue *ECSISettings_GetKeys(ECSISettingsLayer layer)
{
    SDL_assert(layer < ECSISettingsLayer_Count);

    return SETTINGS.keys[layer];
}

bool ECSISettings_Describe(const char *name, ECSPlugin *retOwner, ECSSettingType *retType, ECSISettingsLayer *retLayer)
{
    SDL_assert(name != NULL);
    SDL_assert(retOwner != NULL && retType != NULL && retLayer != NULL);

    ECSISetting *setting = ECSISettings_Find(name);

    if (setting == NULL)
    {
        return false;
    }

    *retOwner = setting->owner;
    *retType = setting->type;
    *retLayer = setting->layer;
    return true;
}

void ECSISettings_DeliverChanges(void)
{
    // an owner may set settings again; those are told in this call too
    for (usz i = 0; i < arrlenu(SETTINGS.changed); i++)
    {
        ECSISetting *setting = SETTINGS.changed[i];

        if (setting->Changed != NULL)
        {
            setting->Changed(setting->data);
        }
    }

    arrfree(SETTINGS.changed);
}

void ECSISettings_ReportUndeclared(void)
{
    for (usz i = 0; i < ECSISettingsLayer_Count; i++)
    {
        ECSIValue_TableForEachField(SETTINGS.layers[i], ECSISettings_ReportField, SETTINGS.paths[i]);
    }
}

const ECSValue *ECSISettings_GetPlugins(void)
{
    return SETTINGS.plugins;
}

SHUResult ECSSetting_Declare(ECSPlugin plugin, const ECSSettingDesc *desc)
{
    SDL_assert(plugin != NULL);
    SDL_assert(desc != NULL);

    return ECSISettings_DeclarePlugin(plugin, desc, NULL);
}

SHUResult ECSISettings_DeclarePlugin(ECSPlugin plugin, const ECSSettingDesc *desc, const ECSValue *defaultValue)
{
    SDL_assert(plugin != NULL);
    SDL_assert(desc != NULL);

    if (desc->name == NULL || !ECSIPlugin_OwnsName(plugin, desc->name))
    {
        return SHUResult_ErrBadData;
    }

    return ECSISettings_Declare(plugin, desc, defaultValue);
}

const ECSValue *ECSSetting_Get(const char *name)
{
    SDL_assert(name != NULL);

    ECSISetting *setting = ECSISettings_Find(name);
    return setting == NULL ? NULL : setting->value;
}

SHUResult ECSSetting_Set(const char *name, const ECSValue *value)
{
    SDL_assert(name != NULL);

    ECSISetting *setting = ECSISettings_Find(name);

    if (setting == NULL)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Setting '%s' is not declared.", name);
        return SHUResult_ErrNotFound;
    }

    if (!ECSISetting_Check(setting, value))
    {
        ECSISetting_ReportType(setting, NULL);
        return SHUResult_ErrBadData;
    }

    if (SETTINGS.windowFile == NULL)
    {
        SHU_ReturnResult(ECSValue_Create(&SETTINGS.windowFile));
        ECSValue_SetTable(SETTINGS.windowFile);
    }

    // the value in effect is kept, to tell the owner only about a real change
    ECSValue *old = NULL;
    SHU_ReturnResult(ECSValue_Create(&old));
    SHUResult result = ECSIValue_Copy(old, setting->value);

    // the tool's own part wins over the part for every tool, so a setting it already has is changed there
    ECSValue *tool = (ECSValue *)ECSValue_GetTableField(ECSValue_GetTableField(SETTINGS.windowFile, "tools"), SETTINGS.appId);
    ECSValue *part = ECSValue_GetTableField(tool, name) != NULL ? tool : SETTINGS.windowFile;
    ECSValue *field = NULL;
    result = result ? result : ECSValue_TableSetField(part, name, &field);
    result = result ? result : ECSIValue_Copy(field, value);
    result = result ? result : ECSISettings_BuildLayer(ECSISettingsLayer_Window, SETTINGS.windowFile);

    // the layer was rebuilt, so every value in effect is found again
    for (usz i = 0; i < shlenu(SETTINGS.settings); i++)
    {
        ECSISetting_Resolve(SETTINGS.settings[i].value);
    }

    if (!result && !ECSIValue_Equal(old, setting->value))
    {
        arrput(SETTINGS.changed, setting);
    }

    ECSValue_Destroy(&old);

    // without a configuration folder, the change lasts until the program exits
    if (!result && SETTINGS.paths[ECSISettingsLayer_Window] != NULL)
    {
        result = ECSILua_WriteData(SETTINGS.paths[ECSISettingsLayer_Window], SETTINGS.windowFile);
    }

    return result;
}

SHUResult ECSSetting_List(ECSValue *retList)
{
    SDL_assert(retList != NULL);

    ECSValue_SetTable(retList);

    for (usz i = 0; i < shlenu(SETTINGS.settings); i++)
    {
        ECSValue *item = NULL;
        SHU_ReturnResult(ECSValue_ListAddItem(retList, &item));
        SHU_ReturnResult(ECSValue_SetString(item, SETTINGS.settings[i].key));
    }

    return SHUResult_Ok;
}

SHUResult ECSSetting_Explain(const char *name, ECSValue *retExplanation)
{
    SDL_assert(name != NULL);
    SDL_assert(retExplanation != NULL);

    ECSISetting *setting = ECSISettings_Find(name);

    if (setting == NULL)
    {
        return SHUResult_ErrNotFound;
    }

    ECSValue_SetTable(retExplanation);
    SHU_ReturnResult(ECSISettings_ExplainText(retExplanation, "name", setting->name));
    SHU_ReturnResult(ECSISettings_ExplainText(retExplanation, "type", OPENECS_SETTING_TYPE_NAMES[setting->type]));
    SHU_ReturnResult(ECSISettings_ExplainText(retExplanation, "description", setting->description));
    SHU_ReturnResult(ECSISettings_ExplainText(retExplanation, "owner", setting->owner == NULL ? "ecs" : ECSIPlugin_GetName(setting->owner)));
    SHU_ReturnResult(ECSISettings_ExplainField(retExplanation, "value", setting->value));
    SHU_ReturnResult(ECSISettings_ExplainText(retExplanation, "layer", OPENECS_SETTINGS_LAYER_NAMES[setting->layer]));

    if (SETTINGS.paths[setting->layer] != NULL)
    {
        SHU_ReturnResult(ECSISettings_ExplainText(retExplanation, "file", SETTINGS.paths[setting->layer]));
    }

    ECSValue *choices = NULL;

    for (usz i = 0; i < arrlenu(setting->choices); i++)
    {
        ECSValue *item = NULL;
        SHU_ReturnResult(choices == NULL ? ECSValue_TableSetField(retExplanation, "choices", &choices) : SHUResult_Ok);
        SHU_ReturnResult(ECSValue_ListAddItem(choices, &item));
        SHU_ReturnResult(ECSValue_SetString(item, setting->choices[i]));
    }

    ECSValue *layers = NULL;
    SHU_ReturnResult(ECSValue_TableSetField(retExplanation, "layers", &layers));
    ECSValue_SetTable(layers);
    SHU_ReturnResult(ECSISettings_ExplainField(layers, "default", setting->defaultValue));

    for (i32 layer = ECSISettingsLayer_Preset; layer < ECSISettingsLayer_Count; layer++)
    {
        const ECSValue *value = ECSValue_GetTableField(SETTINGS.layers[layer], setting->name);

        if (value != NULL)
        {
            SHU_ReturnResult(ECSISettings_ExplainField(layers, OPENECS_SETTINGS_LAYER_NAMES[layer], value));
        }
    }

    return SHUResult_Ok;
}
