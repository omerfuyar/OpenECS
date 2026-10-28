#include "app/Desktop.h"

#include "base/Paths.h"

#include "SDL3/SDL.h"
#include "stb/stbSDL3.h"

#pragma region Source Only

/// @brief The program's file, in its folder.
#define OPENECS_PROGRAM_FILE "OpenECS"

#if !defined(SDL_PLATFORM_WINDOWS)
/// @brief Finds the path of a tool's desktop entry: $XDG_DATA_HOME/applications/<app id>.desktop, or ~/.local/share/applications/ if the variable is not set.
/// @return The path, or nullptr if neither the variable nor HOME is set. Free it with SDL_free.
static char *ECSIDesktop_FindEntry(const char *appID)
{
    const char *base = SDL_getenv("XDG_DATA_HOME");
    const char *home = SDL_getenv("HOME");
    char *path = nullptr;

    if (base != nullptr && base[0] != '\0')
    {
        return SDL_asprintf(&path, "%s/applications/%s.desktop", base, appID) < 0 ? nullptr : path;
    }

    return home == nullptr || SDL_asprintf(&path, "%s/.local/share/applications/%s.desktop", home, appID) < 0 ? nullptr : path;
}

/// @brief Gives the full path of a path that the command line named, which is relative to the current folder.
/// @return The path, which the caller frees with SDL_free, or nullptr if there is no memory.
static char *ECSIDesktop_GetFullPath(const char *path)
{
    if (ECSPath_IsAbsolute(path))
    {
        return SDL_strdup(path);
    }

    char *folder = SDL_GetCurrentDirectory();
    char *full = nullptr;

    if (folder == nullptr || SDL_asprintf(&full, "%s%s", folder, path) < 0)
    {
        full = nullptr;
    }

    SDL_free(folder);
    return full;
}

/// @brief Adds text to an entry, with '\' doubled, as a desktop entry's values need.
static void ECSIDesktop_AddText(char **entry, const char *text)
{
    for (const char *character = text; *character != '\0'; character++)
    {
        if (*character == '\\')
        {
            arrput(*entry, '\\');
        }

        arrput(*entry, *character);
    }
}

/// @brief Adds a line to an entry: the key and its value, or nothing if the value is nullptr.
static void ECSIDesktop_AddLine(char **entry, const char *key, const char *value)
{
    if (value == nullptr)
    {
        return;
    }

    ECSIDesktop_AddText(entry, key);
    arrput(*entry, '=');
    ECSIDesktop_AddText(entry, value);
    arrput(*entry, '\n');
}

/// @brief Adds a line with a list of texts, each ending with ';', or nothing if the list is empty.
static void ECSIDesktop_AddList(char **entry, const char *key, const ECSValue *list)
{
    if (ECSValue_GetListCount(list) == 0)
    {
        return;
    }

    ECSIDesktop_AddText(entry, key);
    arrput(*entry, '=');

    for (usz i = 0; i < ECSValue_GetListCount(list); i++)
    {
        ECSIDesktop_AddText(entry, ECSValue_GetString(ECSValue_GetListItem(list, i), ""));
        arrput(*entry, ';');
    }

    arrput(*entry, '\n');
}

/// @brief Adds an argument of Exec in double quotes, with '"', '`', '$' and '\' escaped, and '%' doubled; the line's own escaping doubles '\' again.
static void ECSIDesktop_AddArgument(char **exec, const char *argument)
{
    arrput(*exec, '"');

    for (const char *character = argument; *character != '\0'; character++)
    {
        if (SDL_strchr("\"`$\\", *character) != nullptr)
        {
            arrput(*exec, '\\');
        }
        else if (*character == '%')
        {
            arrput(*exec, '%');
        }

        arrput(*exec, *character);
    }

    arrput(*exec, '"');
}

/// @brief Makes the Exec value: the program, the preset by its name or its full path, and the files to open.
/// @return The value, which the caller frees with arrfree, or nullptr if a path cannot be found.
static char *ECSIDesktop_MakeExec(const char *presetPath, const char *presetName)
{
    char *program = nullptr;
    char *preset = nullptr;
    char *exec = nullptr;

    if (SDL_asprintf(&program, "%s%s", SDL_GetBasePath(), OPENECS_PROGRAM_FILE) >= 0 && (presetName != nullptr || (preset = ECSIDesktop_GetFullPath(presetPath)) != nullptr))
    {
        ECSIDesktop_AddArgument(&exec, program);
        ECSIDesktop_AddText(&exec, " --preset ");
        ECSIDesktop_AddArgument(&exec, presetName != nullptr ? presetName : preset);
        ECSIDesktop_AddText(&exec, " %F");
        arrput(exec, '\0');
    }

    SDL_free(program);
    SDL_free(preset);
    return exec;
}

/// @brief Finds the full path of the tool's icon, which the preset names relative to its folder.
/// @return The path, which the caller frees with SDL_free, or nullptr if the preset names no icon or it is missing.
static char *ECSIDesktop_FindIcon(const ECSIPresetInfo *info, const char *presetPath)
{
    const char *icon = ECSValue_GetString(ECSValue_GetTableField(ECSValue_GetTableField(info->file, "app"), "icon"), nullptr);
    char *relative = nullptr;

    if (icon == nullptr || SDL_asprintf(&relative, "%.*s%s", ECSIPath_GetFolderLength(presetPath, icon), presetPath, icon) < 0)
    {
        return nullptr;
    }

    char *full = ECSIDesktop_GetFullPath(relative);
    SDL_free(relative);

    if (full != nullptr && !SDL_GetPathInfo(full, nullptr))
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "The icon '%s' of '%s' is missing; the desktop entry has none.", icon, presetPath);
        SDL_free(full);
        full = nullptr;
    }

    return full;
}
#endif

#pragma endregion Source Only

SHUResult ECSIDesktop_Install(const ECSIPresetInfo *info, const char *presetPath, const char *presetName)
{
    SDL_assert(info != nullptr && presetPath != nullptr);

#if defined(SDL_PLATFORM_WINDOWS)
    (void)presetName;
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "OpenECS cannot add '%s' to the system's launcher on Windows yet.", info->appName);
    return SHUResult_Err;
#else
    char *path = ECSIDesktop_FindEntry(info->appID);

    if (path == nullptr)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Neither XDG_DATA_HOME nor HOME is set, so there is no folder for desktop entries.");
        return SHUResult_ErrNotFound;
    }

    const ECSValue *app = ECSValue_GetTableField(info->file, "app");
    char *exec = ECSIDesktop_MakeExec(presetPath, presetName);
    char *icon = ECSIDesktop_FindIcon(info, presetPath);
    char *entry = nullptr;

    ECSIDesktop_AddText(&entry, "[Desktop Entry]\n");
    ECSIDesktop_AddLine(&entry, "Type", "Application");
    ECSIDesktop_AddLine(&entry, "Name", info->appName);
    ECSIDesktop_AddLine(&entry, "Comment", ECSValue_GetString(ECSValue_GetTableField(info->file, "hint"), nullptr));
    ECSIDesktop_AddLine(&entry, "Icon", icon);
    ECSIDesktop_AddLine(&entry, "Exec", exec);
    ECSIDesktop_AddLine(&entry, "StartupWMClass", info->appID);
    ECSIDesktop_AddList(&entry, "Categories", ECSValue_GetTableField(app, "categories"));
    ECSIDesktop_AddList(&entry, "MimeType", ECSValue_GetTableField(app, "mimeTypes"));

    // the folder may be missing on a new system
    const char *separator = ECSPath_FindLastSeparator(path);
    char *folder = SDL_strndup(path, (usz)(separator - path));
    SHUResult result = exec == nullptr || folder == nullptr ? SHUResult_ErrAllocation : SHUResult_Ok;

    if (result == SHUResult_Ok && (!SDL_CreateDirectory(folder) || !SDL_SaveFile(path, entry, arrlenu(entry))))
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot write the desktop entry '%s': %s", path, SDL_GetError());
        result = SHUResult_ErrFile;
    }

    if (result == SHUResult_Ok)
    {
        SDL_Log("Added '%s' to the system's launcher: '%s'.", info->appName, path);
    }

    SDL_free(folder);
    arrfree(entry);
    arrfree(exec);
    SDL_free(icon);
    SDL_free(path);
    return result;
#endif
}

SHUResult ECSIDesktop_Uninstall(const ECSIPresetInfo *info)
{
    SDL_assert(info != nullptr);

#if defined(SDL_PLATFORM_WINDOWS)
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "OpenECS cannot remove '%s' from the system's launcher on Windows yet.", info->appName);
    return SHUResult_Err;
#else
    char *path = ECSIDesktop_FindEntry(info->appID);

    if (path == nullptr)
    {
        return SHUResult_ErrNotFound;
    }

    SHUResult result = SHUResult_Ok;

    if (!SDL_GetPathInfo(path, nullptr))
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "'%s' has no desktop entry: '%s' is missing.", info->appName, path);
        result = SHUResult_ErrNotFound;
    }
    else if (!SDL_RemovePath(path))
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot remove the desktop entry '%s': %s", path, SDL_GetError());
        result = SHUResult_ErrFile;
    }
    else
    {
        SDL_Log("Removed '%s' from the system's launcher.", info->appName);
    }

    SDL_free(path);
    return result;
#endif
}
