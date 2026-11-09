#include "app/Desktop.h"

#include "base/Paths.h"

#include "SDL3/SDL.h"
#include "stb/stbSDL3.h"

#if defined(SDL_PLATFORM_WINDOWS)
#define COBJMACROS
#include <shlobj.h>
#include <shobjidl.h>
#include <windows.h>
#endif

#pragma region Source Only

/// @brief The program's file, in its folder.
#if defined(SDL_PLATFORM_WINDOWS)
#define OPENECS_PROGRAM_FILE "OpenECS.exe"
#else
#define OPENECS_PROGRAM_FILE "OpenECS"
#endif

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
#else
/// @brief A text in UTF-16, as Windows' functions take it.
/// @return The text, which the caller frees with SDL_free, or nullptr if there is no memory.
static wchar_t *ECSIDesktop_Wide(const char *text)
{
    return (wchar_t *)SDL_iconv_string("UTF-16LE", "UTF-8", text, SDL_strlen(text) + 1);
}

/// @brief A known folder of the user's, such as the Start menu's programs.
/// @return The path in UTF-8, which the caller frees with SDL_free, or nullptr if there is none.
static char *ECSIDesktop_GetKnownFolder(REFKNOWNFOLDERID id)
{
    wchar_t *wide = nullptr;
    char *path = nullptr;

    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &wide)))
    {
        path = SDL_iconv_string("UTF-8", "UTF-16LE", (const char *)wide, (SDL_wcslen(wide) + 1) * sizeof(wchar_t));
    }

    CoTaskMemFree(wide);
    return path;
}

/// @brief The tool's shortcut in the Start menu: the programs folder, then the app's name, without the characters that a file name cannot hold.
/// @return The path, which the caller frees with SDL_free, or nullptr if there is no folder or no memory.
static char *ECSIDesktop_FindShortcut(const char *appName)
{
    char *folder = ECSIDesktop_GetKnownFolder(&FOLDERID_Programs);
    char *path = nullptr;

    if (folder == nullptr || SDL_asprintf(&path, "%s\\%s.lnk", folder, appName) < 0)
    {
        path = nullptr;
    }

    // the folder's own separators come before the name
    for (char *character = path == nullptr ? nullptr : path + SDL_strlen(folder) + 1; character != nullptr && *character != '\0'; character++)
    {
        if (SDL_strchr("<>:\"/\\|?*", *character) != nullptr)
        {
            *character = '_';
        }
    }

    SDL_free(folder);
    return path;
}

/// @brief The icon file that Windows shows for the tool: %LOCALAPPDATA%\OpenECS\icons\<app id>.ico.
/// @return The path, which the caller frees with SDL_free, or nullptr if there is no folder or no memory.
static char *ECSIDesktop_FindIconFile(const char *appID)
{
    char *folder = ECSIDesktop_GetKnownFolder(&FOLDERID_LocalAppData);
    char *path = nullptr;

    if (folder == nullptr || SDL_asprintf(&path, "%s\\OpenECS\\icons\\%s.ico", folder, appID) < 0)
    {
        path = nullptr;
    }

    SDL_free(folder);
    return path;
}

/// @brief Writes the preset's PNG icon as an icon file, which holds the PNG as it is, as Windows' shortcuts take no PNG.
/// @return The icon file's path, which the caller frees with SDL_free, or nullptr if the preset has no PNG icon or it cannot be written.
static char *ECSIDesktop_WriteIcon(const ECSIPresetInfo *info, const char *presetPath)
{
    char *png = ECSIDesktop_FindIcon(info, presetPath);
    usz size = 0;
    u8 *data = png == nullptr ? nullptr : SDL_LoadFile(png, &size);
    char *path = data == nullptr ? nullptr : ECSIDesktop_FindIconFile(info->appID);
    static const u8 SIGNATURE[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

    // a PNG starts with its signature, then its header with its width and height, big-endian
    if (path != nullptr && (size < 24 || SDL_memcmp(data, SIGNATURE, sizeof(SIGNATURE)) != 0))
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "The icon of '%s' is not a PNG; Windows shows the tool without it.", presetPath);
        SDL_free(path);
        path = nullptr;
    }

    if (path != nullptr)
    {
        u32 width = ((u32)data[16] << 24) | ((u32)data[17] << 16) | ((u32)data[18] << 8) | data[19];
        u32 height = ((u32)data[20] << 24) | ((u32)data[21] << 16) | ((u32)data[22] << 8) | data[23];

        // the icon file's header and its one entry, little-endian; a size of 256 or more is written as 0
        u8 header[22] = {0, 0, 1, 0, 1, 0, (u8)(width >= 256 ? 0 : width), (u8)(height >= 256 ? 0 : height), 0, 0, 1, 0, 32, 0};
        SDL_memcpy(header + 14, &(u32){(u32)size}, 4);
        SDL_memcpy(header + 18, &(u32){sizeof(header)}, 4);

        const char *separator = ECSPath_FindLastSeparator(path);
        char *folder = SDL_strndup(path, (usz)(separator - path));
        SDL_IOStream *file = folder != nullptr && SDL_CreateDirectory(folder) ? SDL_IOFromFile(path, "wb") : nullptr;
        bool written = file != nullptr && SDL_WriteIO(file, header, sizeof(header)) == sizeof(header) && SDL_WriteIO(file, data, size) == size;

        if (file != nullptr && !SDL_CloseIO(file))
        {
            written = false;
        }

        if (!written)
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cannot write the icon '%s': %s", path, SDL_GetError());
            SDL_free(path);
            path = nullptr;
        }

        SDL_free(folder);
    }

    SDL_free(data);
    SDL_free(png);
    return path;
}

/// @brief The command that opens a file with the tool: the program, the preset by its name or its full path, and the file.
/// @return The command, which the caller frees with SDL_free, or nullptr if a path cannot be found.
static char *ECSIDesktop_MakeCommand(const char *presetPath, const char *presetName)
{
    char *preset = presetName != nullptr ? SDL_strdup(presetName) : ECSIDesktop_GetFullPath(presetPath);
    char *command = nullptr;

    if (preset == nullptr || SDL_asprintf(&command, "\"%s%s\" --preset \"%s\" \"%%1\"", SDL_GetBasePath(), OPENECS_PROGRAM_FILE, preset) < 0)
    {
        command = nullptr;
    }

    SDL_free(preset);
    return command;
}

// the property that groups a shortcut with the windows of the process that has the same AppUserModelID, as SDL's notifications use the app id
static const PROPERTYKEY OPENECS_DESKTOP_APP_ID = {{0x9F4C2855, 0x9F79, 0x4B39, {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}}, 5};

/// @brief Sets a shortcut's AppUserModelID, so the taskbar groups the tool's windows under it.
static HRESULT ECSIDesktop_SetShortcutAppID(IShellLinkW *link, const char *appID)
{
    IPropertyStore *store = nullptr;
    HRESULT result = IShellLinkW_QueryInterface(link, &IID_IPropertyStore, (void **)&store);
    wchar_t *id = SUCCEEDED(result) ? ECSIDesktop_Wide(appID) : nullptr;

    if (id != nullptr)
    {
        PROPVARIANT value = {.vt = VT_LPWSTR};
        value.pwszVal = id;
        result = IPropertyStore_SetValue(store, &OPENECS_DESKTOP_APP_ID, &value);
        result = SUCCEEDED(result) ? IPropertyStore_Commit(store) : result;
    }

    if (store != nullptr)
    {
        IPropertyStore_Release(store);
    }

    SDL_free(id);
    return result;
}

/// @brief Writes a shortcut that runs the tool, with its icon, its hint and its app id.
static HRESULT ECSIDesktop_WriteShortcut(const char *path, const char *presetPath, const char *presetName, const char *icon, const ECSIPresetInfo *info)
{
    char *program = nullptr;
    char *preset = presetName != nullptr ? SDL_strdup(presetName) : ECSIDesktop_GetFullPath(presetPath);
    char *arguments = nullptr;

    if (preset == nullptr || SDL_asprintf(&program, "%s%s", SDL_GetBasePath(), OPENECS_PROGRAM_FILE) < 0 || SDL_asprintf(&arguments, "--preset \"%s\"", preset) < 0)
    {
        SDL_free(preset);
        SDL_free(program);
        return E_OUTOFMEMORY;
    }

    const char *hint = ECSValue_GetString(ECSValue_GetTableField(info->file, "hint"), "");
    wchar_t *texts[] = {ECSIDesktop_Wide(program), ECSIDesktop_Wide(arguments), ECSIDesktop_Wide(hint), ECSIDesktop_Wide(SDL_GetBasePath()), ECSIDesktop_Wide(path), icon == nullptr ? nullptr : ECSIDesktop_Wide(icon)};
    IShellLinkW *link = nullptr;
    IPersistFile *file = nullptr;
    HRESULT result = texts[0] == nullptr || texts[1] == nullptr || texts[2] == nullptr || texts[3] == nullptr || texts[4] == nullptr ? E_OUTOFMEMORY : S_OK;

    result = SUCCEEDED(result) ? CoCreateInstance(&CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link) : result;
    result = SUCCEEDED(result) ? IShellLinkW_SetPath(link, texts[0]) : result;
    result = SUCCEEDED(result) ? IShellLinkW_SetArguments(link, texts[1]) : result;
    result = SUCCEEDED(result) ? IShellLinkW_SetDescription(link, texts[2]) : result;
    result = SUCCEEDED(result) ? IShellLinkW_SetWorkingDirectory(link, texts[3]) : result;
    result = SUCCEEDED(result) && texts[5] != nullptr ? IShellLinkW_SetIconLocation(link, texts[5], 0) : result;
    result = SUCCEEDED(result) ? ECSIDesktop_SetShortcutAppID(link, info->appID) : result;
    result = SUCCEEDED(result) ? IShellLinkW_QueryInterface(link, &IID_IPersistFile, (void **)&file) : result;
    result = SUCCEEDED(result) ? IPersistFile_Save(file, texts[4], TRUE) : result;

    if (file != nullptr)
    {
        IPersistFile_Release(file);
    }

    if (link != nullptr)
    {
        IShellLinkW_Release(link);
    }

    for (usz i = 0; i < SDL_arraysize(texts); i++)
    {
        SDL_free(texts[i]);
    }

    SDL_free(arguments);
    SDL_free(program);
    SDL_free(preset);
    return result;
}

/// @brief Sets a text value of a key of the user's classes, the key's default value for a nullptr name; true if it is set.
static bool ECSIDesktop_SetClassValue(const char *key, const char *name, const char *value)
{
    char *path = nullptr;
    wchar_t *texts[3] = {nullptr, name == nullptr ? nullptr : ECSIDesktop_Wide(name), ECSIDesktop_Wide(value)};

    if (SDL_asprintf(&path, "Software\\Classes\\%s", key) >= 0)
    {
        texts[0] = ECSIDesktop_Wide(path);
    }

    bool set = texts[0] != nullptr && texts[2] != nullptr && (name == nullptr || texts[1] != nullptr) &&
               RegSetKeyValueW(HKEY_CURRENT_USER, texts[0], texts[1], REG_SZ, texts[2], (DWORD)((SDL_wcslen(texts[2]) + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;

    for (usz i = 0; i < SDL_arraysize(texts); i++)
    {
        SDL_free(texts[i]);
    }

    SDL_free(path);
    return set;
}

/// @brief Calls a function with each file extension of a key of classes whose "Content Type" is one of the MIME types, such as .jpg and .jpeg for image/jpeg.
static void ECSIDesktop_ForEachExtension(HKEY classes, const ECSValue *mimeTypes, void (*function)(const wchar_t *extension, void *data), void *data)
{
    wchar_t name[256];
    wchar_t type[256];

    for (DWORD index = 0;; index++)
    {
        DWORD length = SDL_arraysize(name);
        LSTATUS status = RegEnumKeyExW(classes, index, name, &length, nullptr, nullptr, nullptr, nullptr);

        if (status == ERROR_NO_MORE_ITEMS)
        {
            break;
        }

        DWORD size = sizeof(type);

        if (status != ERROR_SUCCESS || name[0] != L'.' || RegGetValueW(classes, name, L"Content Type", RRF_RT_REG_SZ, nullptr, type, &size) != ERROR_SUCCESS)
        {
            continue;
        }

        char *text = SDL_iconv_string("UTF-8", "UTF-16LE", (const char *)type, (SDL_wcslen(type) + 1) * sizeof(wchar_t));

        for (usz i = 0; text != nullptr && i < ECSValue_GetListCount(mimeTypes); i++)
        {
            if (SDL_strcasecmp(text, ECSValue_GetString(ECSValue_GetListItem(mimeTypes, i), "")) == 0)
            {
                function(name, data);
                break;
            }
        }

        SDL_free(text);
    }
}

/// @brief Adds the tool's class to "Open with" of a file extension.
static void ECSIDesktop_AddOpenWith(const wchar_t *extension, void *data)
{
    wchar_t key[300];
    SDL_swprintf(key, SDL_arraysize(key), L"Software\\Classes\\%ls\\OpenWithProgids", extension);
    RegSetKeyValueW(HKEY_CURRENT_USER, key, data, REG_NONE, nullptr, 0);
}

/// @brief Removes the tool's class from "Open with" of a file extension.
static void ECSIDesktop_RemoveOpenWith(const wchar_t *extension, void *data)
{
    wchar_t key[300];
    SDL_swprintf(key, SDL_arraysize(key), L"Software\\Classes\\%ls\\OpenWithProgids", extension);
    RegDeleteKeyValueW(HKEY_CURRENT_USER, key, data);
}

/// @brief Registers the tool's class, which opens a file with the tool, and adds it to "Open with" of the extensions of its MIME types.
/// @return true if the class is registered.
static bool ECSIDesktop_RegisterClass(const ECSIPresetInfo *info, const char *presetPath, const char *presetName, const char *icon)
{
    const ECSValue *mimeTypes = ECSValue_GetTableField(ECSValue_GetTableField(info->file, "app"), "mimeTypes");

    if (ECSValue_GetListCount(mimeTypes) == 0)
    {
        return true;
    }

    char *progID = nullptr;
    char *key = nullptr;
    char *command = ECSIDesktop_MakeCommand(presetPath, presetName);
    bool registered = command != nullptr && SDL_asprintf(&progID, "OpenECS.%s", info->appID) >= 0 && ECSIDesktop_SetClassValue(progID, nullptr, info->appName);
    registered = registered && SDL_asprintf(&key, "%s\\shell\\open\\command", progID) >= 0 && ECSIDesktop_SetClassValue(key, nullptr, command);
    SDL_free(key);
    key = nullptr;

    if (registered && icon != nullptr && SDL_asprintf(&key, "%s\\DefaultIcon", progID) >= 0)
    {
        ECSIDesktop_SetClassValue(key, nullptr, icon);
    }

    wchar_t *wideID = registered ? ECSIDesktop_Wide(progID) : nullptr;

    if (wideID != nullptr)
    {
        ECSIDesktop_ForEachExtension(HKEY_CLASSES_ROOT, mimeTypes, ECSIDesktop_AddOpenWith, wideID);
    }

    SDL_free(wideID);
    SDL_free(key);
    SDL_free(progID);
    SDL_free(command);
    return registered;
}

/// @brief Removes the tool's class and its places in "Open with".
static void ECSIDesktop_UnregisterClass(const ECSIPresetInfo *info)
{
    char *progID = nullptr;
    char *key = nullptr;

    if (SDL_asprintf(&progID, "OpenECS.%s", info->appID) < 0 || SDL_asprintf(&key, "Software\\Classes\\%s", progID) < 0)
    {
        SDL_free(progID);
        return;
    }

    wchar_t *wideID = ECSIDesktop_Wide(progID);
    wchar_t *wideKey = ECSIDesktop_Wide(key);
    const ECSValue *mimeTypes = ECSValue_GetTableField(ECSValue_GetTableField(info->file, "app"), "mimeTypes");

    if (wideID != nullptr)
    {
        ECSIDesktop_ForEachExtension(HKEY_CLASSES_ROOT, mimeTypes, ECSIDesktop_RemoveOpenWith, wideID);
    }

    if (wideKey != nullptr)
    {
        RegDeleteTreeW(HKEY_CURRENT_USER, wideKey);
    }

    SDL_free(wideKey);
    SDL_free(wideID);
    SDL_free(key);
    SDL_free(progID);
}

/// @brief Starts COM on this thread for the shell's objects; true if it must be ended with CoUninitialize.
static bool ECSIDesktop_StartCom(void)
{
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    return result == S_OK || result == S_FALSE;
}
#endif

#pragma endregion Source Only

SHUResult ECSIDesktop_Install(const ECSIPresetInfo *info, const char *presetPath, const char *presetName)
{
    SDL_assert(info != nullptr && presetPath != nullptr);

#if defined(SDL_PLATFORM_WINDOWS)
    char *path = ECSIDesktop_FindShortcut(info->appName);

    if (path == nullptr)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "There is no Start menu folder for '%s'.", info->appName);
        return SHUResult_ErrNotFound;
    }

    bool com = ECSIDesktop_StartCom();
    char *icon = ECSIDesktop_WriteIcon(info, presetPath);
    HRESULT written = ECSIDesktop_WriteShortcut(path, presetPath, presetName, icon, info);
    SHUResult result = SHUResult_Ok;

    if (FAILED(written))
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot write the shortcut '%s': error 0x%08lX.", path, (unsigned long)written);
        result = SHUResult_ErrFile;
    }
    else if (!ECSIDesktop_RegisterClass(info, presetPath, presetName, icon))
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot add '%s' to \"Open with\" in the registry.", info->appName);
        result = SHUResult_ErrFile;
    }
    else
    {
        SDL_Log("Added '%s' to the Start menu: '%s'.", info->appName, path);
    }

    // Explorer reads the file types' programs again
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);

    if (com)
    {
        CoUninitialize();
    }

    SDL_free(icon);
    SDL_free(path);
    return result;
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
    char *path = ECSIDesktop_FindShortcut(info->appName);
    char *icon = ECSIDesktop_FindIconFile(info->appID);
    SHUResult result = SHUResult_Ok;

    if (path == nullptr || !SDL_GetPathInfo(path, nullptr))
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "'%s' has no shortcut in the Start menu.", info->appName);
        result = SHUResult_ErrNotFound;
    }
    else if (!SDL_RemovePath(path))
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot remove the shortcut '%s': %s", path, SDL_GetError());
        result = SHUResult_ErrFile;
    }

    // the class and the icon go too, even without a shortcut
    ECSIDesktop_UnregisterClass(info);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);

    if (icon != nullptr && SDL_GetPathInfo(icon, nullptr))
    {
        SDL_RemovePath(icon);
    }

    if (result == SHUResult_Ok)
    {
        SDL_Log("Removed '%s' from the Start menu.", info->appName);
    }

    SDL_free(icon);
    SDL_free(path);
    return result;
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

void ECSIDesktop_SetAppID(const char *appID)
{
    SDL_assert(appID != nullptr);

#if defined(SDL_PLATFORM_WINDOWS)
    wchar_t *id = ECSIDesktop_Wide(appID);

    if (id == nullptr || FAILED(SetCurrentProcessExplicitAppUserModelID(id)))
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "The taskbar cannot group the windows of '%s' under its shortcut.", appID);
    }

    SDL_free(id);
#else
    (void)appID;
#endif
}
