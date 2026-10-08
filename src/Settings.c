#include "Settings.h"

#include "Lua.h"

#include "SDL3/SDL.h"
#include "stb/stbSDL3.h"

#pragma region Source Only

/// @brief The user's hand-edited settings file, in the configuration folder.
#define OPENECS_SETTINGS_USER_FILE "settings.lua"
/// @brief The file that the settings window writes, in the configuration folder.
#define OPENECS_SETTINGS_WINDOW_FILE "settings-window.lua"

/// @brief What a value of each setting type must be, for messages.
static const char *const ECSI_SETTING_TYPE_TEXTS[] = {
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
typedef struct ECSI_Setting
{
    char *name;
    char *description;
    ECSSettingType type;
    ECSPlugin owner; // NULL for the core
    ECSValue *defaultValue;
    char **choices;        // stb_ds array; choice settings only
    const ECSValue *value; // the value in effect: the default, or a value of a layer
    ECSI_SettingsLayer layer;
} ECSI_Setting;

/// @brief Collects what a user file says, while its fields are walked.
typedef struct ECSI_SettingsFileReader
{
    ECSValue *layer;
    SHUResult result;
} ECSI_SettingsFileReader;

static struct
{
    struct
    {
        char *key; // the setting's own copy of its name
        ECSI_Setting *value;
    } *settings;                                // stb_ds hash map, in declaration order
    ECSValue *layers[ECSI_SettingsLayer_Count]; // the preset, window and user layers: tables of setting names and values
    char *paths[ECSI_SettingsLayer_Count];      // the file of each of those layers
    ECSValue *plugins;                          // list of extra plugin names
} SETTINGS = {0};

/// @brief Checks that a value has the type of a setting.
static bool ECSI_SettingCheck(const ECSI_Setting *setting, const ECSValue *value)
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

/// @brief Finds the value in effect of a setting: the highest layer that sets it with the right type, or the default.
static void ECSI_SettingResolve(ECSI_Setting *setting)
{
    for (i32 layer = ECSI_SettingsLayer_User; layer >= ECSI_SettingsLayer_Preset; layer--)
    {
        const ECSValue *value = ECSValue_GetField(SETTINGS.layers[layer], setting->name);

        if (value == NULL)
        {
            continue;
        }

        if (ECSI_SettingCheck(setting, value))
        {
            setting->value = value;
            setting->layer = (ECSI_SettingsLayer)layer;
            return;
        }

        char choices[256] = "";

        for (usz i = 0; i < arrlenu(setting->choices); i++)
        {
            SDL_strlcat(choices, i == 0 ? " " : ", ", sizeof(choices));
            SDL_strlcat(choices, setting->choices[i], sizeof(choices));
        }

        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Setting '%s' in '%s' must be %s%s; that value is ignored.", setting->name, SETTINGS.paths[layer], ECSI_SETTING_TYPE_TEXTS[setting->type], choices);
    }

    setting->value = setting->defaultValue;
    setting->layer = setting->owner == NULL ? ECSI_SettingsLayer_Core : ECSI_SettingsLayer_Plugin;
}

static void ECSI_SettingFree(ECSI_Setting *setting)
{
    for (usz i = 0; i < arrlenu(setting->choices); i++)
    {
        SDL_free(setting->choices[i]);
    }

    arrfree(setting->choices);
    ECSI_ValueDestroy(&setting->defaultValue);
    SDL_free(setting->name);
    SDL_free(setting->description);
    SDL_free(setting);
}

/// @brief Sets a new setting's default from its description.
static SHUResult ECSI_SettingSetDefault(ECSI_Setting *setting, const ECSSettingDesc *desc)
{
    SHU_ReturnResult(ECSI_ValueCreate(&setting->defaultValue));

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

        if (!ECSI_SettingCheck(setting, setting->defaultValue))
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
static SHUResult ECSI_SettingsDeclare(ECSPlugin owner, const ECSSettingDesc *desc, const ECSValue *defaultValue)
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

    ECSI_Setting *setting = SDL_calloc(1, sizeof(ECSI_Setting));

    if (setting == NULL)
    {
        return SHUResult_ErrAllocation;
    }

    setting->name = SDL_strdup(desc->name);
    setting->description = SDL_strdup(desc->description == NULL ? "" : desc->description);
    setting->type = desc->type;
    setting->owner = owner;

    if (setting->name == NULL || setting->description == NULL)
    {
        ECSI_SettingFree(setting);
        return SHUResult_ErrAllocation;
    }

    SHU_ReturnResult(ECSI_SettingSetDefault(setting, desc), ECSI_SettingFree(setting););

    if (defaultValue != NULL)
    {
        if (!ECSI_SettingCheck(setting, defaultValue))
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "The default of setting '%s' must be %s.", desc->name, ECSI_SETTING_TYPE_TEXTS[desc->type]);
            ECSI_SettingFree(setting);
            return SHUResult_ErrBadData;
        }

        SHU_ReturnResult(ECSI_ValueCopy(setting->defaultValue, defaultValue), ECSI_SettingFree(setting););
    }

    shput(SETTINGS.settings, setting->name, setting);
    ECSI_SettingResolve(setting);
    return SHUResult_Ok;
}

/// @brief Copies a field of a user file into its layer if its name is a setting's: plugin or core name, a dot, local name.
static void ECSI_SettingsReadField(const char *name, const ECSValue *field, void *userData)
{
    ECSI_SettingsFileReader *reader = userData;
    ECSValue *copy = NULL;

    if (reader->result || SDL_strchr(name, '.') == NULL)
    {
        return;
    }

    reader->result = ECSValue_SetField(reader->layer, name, &copy);
    reader->result = reader->result ? reader->result : ECSI_ValueCopy(copy, field);
}

/// @brief Adds the plugins that a part of a user file names.
static SHUResult ECSI_SettingsReadPlugins(const ECSValue *part)
{
    const ECSValue *plugins = ECSValue_GetField(part, "plugins");

    for (usz i = 0; i < ECSValue_GetCount(plugins); i++)
    {
        const char *name = ECSValue_GetString(ECSValue_GetItem(plugins, i), NULL);
        ECSValue *item = NULL;

        if (name != NULL)
        {
            SHU_ReturnResult(ECSValue_AddItem(SETTINGS.plugins, &item));
            SHU_ReturnResult(ECSValue_SetString(item, name));
        }
    }

    return SHUResult_Ok;
}

/// @brief Reads a user file into its layer: the part for every tool, then the part for this tool, which wins.
static SHUResult ECSI_SettingsReadFile(ECSI_SettingsLayer layer, const char *folder, const char *fileName, const char *appId)
{
    if (SDL_asprintf(&SETTINGS.paths[layer], "%s%s", folder, fileName) < 0)
    {
        SETTINGS.paths[layer] = NULL;
        return SHUResult_ErrAllocation;
    }

    SHU_ReturnResult(ECSI_ValueCreate(&SETTINGS.layers[layer]));
    ECSValue_SetTable(SETTINGS.layers[layer]);

    if (!SDL_GetPathInfo(SETTINGS.paths[layer], NULL))
    {
        return SHUResult_Ok;
    }

    ECSValue *file = NULL;
    SHU_ReturnResult(ECSI_ValueCreate(&file));
    SHUResult result = ECSI_LuaReadData(SETTINGS.paths[layer], file);

    // a file that cannot be read is already reported, and skipped
    if (result == SHUResult_ErrFile || result == SHUResult_ErrBadData)
    {
        ECSI_ValueDestroy(&file);
        return SHUResult_Ok;
    }

    const ECSValue *tool = ECSValue_GetField(ECSValue_GetField(file, "tools"), appId);
    ECSI_SettingsFileReader reader = {.layer = SETTINGS.layers[layer], .result = result};
    ECSI_ValueForEachField(file, ECSI_SettingsReadField, &reader);
    ECSI_ValueForEachField(tool, ECSI_SettingsReadField, &reader);

    result = reader.result ? reader.result : ECSI_SettingsReadPlugins(file);
    result = result ? result : ECSI_SettingsReadPlugins(tool);
    ECSI_ValueDestroy(&file);
    return result;
}

#pragma endregion Source Only

SHUResult ECSI_SettingsInitialize(const ECSValue *presetSettings, const char *presetPath, const char *appId, const char *configFolder)
{
    SDL_assert(presetPath != NULL);
    SDL_assert(appId != NULL);

    SHU_ReturnResult(ECSI_ValueCreate(&SETTINGS.layers[ECSI_SettingsLayer_Preset]));
    SHU_ReturnResult(ECSI_ValueCopy(SETTINGS.layers[ECSI_SettingsLayer_Preset], presetSettings));
    SETTINGS.paths[ECSI_SettingsLayer_Preset] = SDL_strdup(presetPath);

    if (SETTINGS.paths[ECSI_SettingsLayer_Preset] == NULL)
    {
        return SHUResult_ErrAllocation;
    }

    SHU_ReturnResult(ECSI_ValueCreate(&SETTINGS.plugins));
    ECSValue_SetTable(SETTINGS.plugins);

    if (configFolder == NULL)
    {
        return SHUResult_Ok;
    }

    SHU_ReturnResult(ECSI_SettingsReadFile(ECSI_SettingsLayer_Window, configFolder, OPENECS_SETTINGS_WINDOW_FILE, appId));
    return ECSI_SettingsReadFile(ECSI_SettingsLayer_User, configFolder, OPENECS_SETTINGS_USER_FILE, appId);
}

void ECSI_SettingsTerminate(void)
{
    for (usz i = 0; i < shlenu(SETTINGS.settings); i++)
    {
        ECSI_SettingFree(SETTINGS.settings[i].value);
    }

    for (usz i = 0; i < ECSI_SettingsLayer_Count; i++)
    {
        ECSI_ValueDestroy(&SETTINGS.layers[i]);
        SDL_free(SETTINGS.paths[i]);
    }

    shfree(SETTINGS.settings);
    ECSI_ValueDestroy(&SETTINGS.plugins);
    SDL_zero(SETTINGS);
}

SHUResult ECSI_SettingsDeclareCore(const ECSSettingDesc *desc)
{
    SDL_assert(desc != NULL);
    SDL_assert(desc->name != NULL && SDL_strncmp(desc->name, "ecs.", 4) == 0);

    return ECSI_SettingsDeclare(NULL, desc, NULL);
}

void ECSI_SettingsRemovePlugin(ECSPlugin plugin)
{
    SDL_assert(plugin != NULL);

    // backwards, because shdel moves the last setting into the hole
    for (usz i = shlenu(SETTINGS.settings); i > 0; i--)
    {
        ECSI_Setting *setting = SETTINGS.settings[i - 1].value;

        if (setting->owner == plugin)
        {
            (void)shdel(SETTINGS.settings, setting->name);
            ECSI_SettingFree(setting);
        }
    }
}

const ECSValue *ECSI_SettingsGetPlugins(void)
{
    return SETTINGS.plugins;
}

SHUResult ECSSetting_Declare(ECSPlugin plugin, const ECSSettingDesc *desc)
{
    SDL_assert(plugin != NULL);
    SDL_assert(desc != NULL);

    return ECSI_SettingsDeclarePlugin(plugin, desc, NULL);
}

SHUResult ECSI_SettingsDeclarePlugin(ECSPlugin plugin, const ECSSettingDesc *desc, const ECSValue *defaultValue)
{
    SDL_assert(plugin != NULL);
    SDL_assert(desc != NULL);

    if (desc->name == NULL || !ECSI_PluginOwnsName(plugin, desc->name))
    {
        return SHUResult_ErrBadData;
    }

    return ECSI_SettingsDeclare(plugin, desc, defaultValue);
}

const ECSValue *ECSSetting_Get(const char *name)
{
    SDL_assert(name != NULL);

    // shget would allocate a map that is missing
    ECSI_Setting *setting = SETTINGS.settings == NULL ? NULL : shget(SETTINGS.settings, name);
    return setting == NULL ? NULL : setting->value;
}
