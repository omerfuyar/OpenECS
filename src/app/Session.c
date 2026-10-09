#include "app/Session.h"

#include "base/Lua.h"
#include "base/Values.h"
#include "interface/Input.h"
#include "interface/Keys.h"
#include "interface/Layout.h"
#include "interface/Panels.h"
#include "runtime/Services.h"

#include "SDL3/SDL.h"
#include "stb/stbSDL3.h"

#pragma region Source Only

/// @brief Version of the preset and session format that this core writes.
#define OPENECS_SESSION_FORMAT 1

/// @brief Longest path to a part of a file that a problem report names.
#define OPENECS_SESSION_PATH_SIZE 256

/// @brief Size of a pop-out window whose session gives none, in layout units.
#define OPENECS_SESSION_POP_OUT_SIZE 400.0f

/// @brief A tool's last session, in its folder in the state folder.
#define OPENECS_LAST_SESSION_FILE "session.lua"

static struct
{
    const ECSIPresetInfo *info; // the preset or session in use, which saved sessions are written from
    char *presets;              // the user's presets, or NULL
    char *folder;               // the saved sessions, where the dialogs that ask for session files start, or NULL
    char *state;                // the state folder, or NULL
    bool canOpen;               // false during a test
    char *next;                 // the session or preset that OpenECS starts again from once it stops, or NULL
    bool nextIsPreset;          // true when next is a preset
} SESSION = {0};

/// @brief A file of a list of presets or sessions, before it is read.
typedef struct ECSISessionFile
{
    char *name; // without .lua
    char *path;
} ECSISessionFile;

/// @brief State while the workspaces of a file are built.
typedef struct ECSISessionReader
{
    const char *file;
    char path[OPENECS_SESSION_PATH_SIZE]; // the part being read, such as "workspaces[1].windows[1]"
    i64 focusId;                          // of the workspace being read
    ECSPanel focus;
    ECSINode *maximized;
} ECSISessionReader;

/// @brief Reports a problem at the part of the file being read.
static OPENECS_PRINTF(2, 3) void ECSISession_Report(const ECSISessionReader *reader, const char *format, ...)
{
    char *message = NULL;
    va_list arguments;
    va_start(arguments, format);
    int length = SDL_vasprintf(&message, format, arguments);
    va_end(arguments);

    if (length >= 0)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "'%s', at %s: %s", reader->file, reader->path, message);
        SDL_free(message);
    }
}

/// @brief Adds a field name or a list position to the path, and returns the old length to restore it.
static usz ECSISession_Enter(ECSISessionReader *reader, const char *field, usz index)
{
    usz length = SDL_strlen(reader->path);
    usz room = sizeof(reader->path) - length;

    if (field != NULL)
    {
        SDL_snprintf(reader->path + length, room, length == 0 ? "%s" : ".%s", field);
    }
    else
    {
        SDL_snprintf(reader->path + length, room, "[%zu]", index + 1);
    }

    return length;
}

static void ECSISession_Leave(ECSISessionReader *reader, usz length)
{
    reader->path[length] = '\0';
}

/// @brief Reads a number field that must not be negative.
static f32 ECSISession_GetSize(ECSISessionReader *reader, const ECSValue *node, const char *name, f32 fallback)
{
    const ECSValue *field = ECSValue_GetTableField(node, name);
    f64 size = ECSValue_GetNumber(field, -1.0);

    if (field == NULL)
    {
        return fallback;
    }

    if (size < 0.0)
    {
        ECSISession_Report(reader, "'%s' must be a number of at least 0; it is ignored.", name);
        return fallback;
    }

    return (f32)size;
}

/// @brief Creates the panel that a table describes and adds it to a group. A bad panel is reported and skipped.
static SHUResult ECSISession_ReadPanel(ECSISessionReader *reader, const ECSValue *saved, ECSINode *group)
{
    const char *type = ECSValue_GetString(ECSValue_GetTableField(saved, "type"), NULL);

    if (type == NULL)
    {
        ECSISession_Report(reader, "a panel must be a table with a 'type' text; it is skipped.");
        return SHUResult_Ok;
    }

    const ECSValue *state = ECSValue_GetTableField(saved, "state");
    i64 version = ECSValue_GetInteger(ECSValue_GetTableField(saved, "stateVersion"), 0);
    i64 id = ECSValue_GetInteger(ECSValue_GetTableField(saved, "id"), 0);
    ECSPanel panel = NULL;

    SHU_ReturnResult(ECSIPanel_Create(&panel, type, state, version >= 0 && version <= SDL_MAX_UINT32 ? (u32)version : 0));
    ECSIPanel_SetId(panel, id > 0 && id <= SDL_MAX_UINT32 ? (u32)id : 0);
    ECSILayout_GroupAdd(group, panel);

    if (id != 0 && id == reader->focusId)
    {
        reader->focus = panel;
    }

    return SHUResult_Ok;
}

/// @brief Builds the layout node that a table describes: a split if it has "split", a group otherwise.
static SHUResult ECSISession_ReadNode(ECSISessionReader *reader, const ECSValue *saved, ECSINode **retNode)
{
    const ECSValue *split = ECSValue_GetTableField(saved, "split");

    if (split != NULL)
    {
        const char *direction = ECSValue_GetString(split, "");
        bool vertical = SDL_strcmp(direction, "vertical") == 0;

        if (!vertical && SDL_strcmp(direction, "horizontal") != 0)
        {
            ECSISession_Report(reader, "'split' must be \"horizontal\" or \"vertical\"; \"horizontal\" is used.");
        }

        SHU_ReturnResult(ECSILayout_SplitCreate(retNode, vertical));

        for (usz i = 0; i < ECSValue_GetListCount(saved); i++)
        {
            const ECSValue *savedChild = ECSValue_GetListItem(saved, i);
            usz length = ECSISession_Enter(reader, NULL, i);
            ECSINode *child = NULL;
            SHUResult result = SHUResult_Ok;

            if (ECSValue_GetType(savedChild) != ECSValueType_Table)
            {
                ECSISession_Report(reader, "a part of a split must be a table; it is skipped.");
            }
            else
            {
                result = ECSISession_ReadNode(reader, savedChild, &child);
            }

            if (child != NULL)
            {
                ECSILayout_SplitAdd(*retNode, child, ECSISession_GetSize(reader, savedChild, "size", 0.0f), ECSISession_GetSize(reader, savedChild, "share", 1.0f));
            }

            ECSISession_Leave(reader, length);
            SHU_ReturnResult(result, ECSILayout_NodeDestroy(retNode););
        }

        return SHUResult_Ok;
    }

    SHU_ReturnResult(ECSILayout_GroupCreate(retNode));

    if (ECSValue_GetBool(ECSValue_GetTableField(saved, "maximized"), false))
    {
        reader->maximized = *retNode;
    }

    ECSILayout_GroupSetLocked(*retNode, ECSValue_GetBool(ECSValue_GetTableField(saved, "locked"), false));

    const ECSValue *panels = ECSValue_GetTableField(saved, "panels");
    usz panelsLength = ECSISession_Enter(reader, "panels", 0);

    for (usz i = 0; i < ECSValue_GetListCount(panels); i++)
    {
        usz length = ECSISession_Enter(reader, NULL, i);
        SHUResult result = ECSISession_ReadPanel(reader, ECSValue_GetListItem(panels, i), *retNode);
        ECSISession_Leave(reader, length);
        SHU_ReturnResult(result, ECSILayout_NodeDestroy(retNode););
    }

    ECSISession_Leave(reader, panelsLength);
    ECSILayout_GroupShow(*retNode, (usz)SDL_max(1, ECSValue_GetInteger(ECSValue_GetTableField(saved, "shown"), 1)) - 1);
    return SHUResult_Ok;
}

/// @brief Builds the workspace that a table describes: the main window's tree, then a tree for each pop-out window.
static SHUResult ECSISession_ReadWorkspace(ECSISessionReader *reader, const ECSValue *saved)
{
    const ECSValue *windows = ECSValue_GetTableField(saved, "windows");
    usz count = SDL_max(1, ECSValue_GetListCount(windows));
    ECSIRootDesc *roots = SDL_calloc(count, sizeof(ECSIRootDesc));
    SHUResult result = roots == NULL ? SHUResult_ErrAllocation : SHUResult_Ok;

    reader->focusId = ECSValue_GetInteger(ECSValue_GetTableField(saved, "focus"), 0);
    reader->focus = NULL;

    for (usz i = 0; !result && i < ECSValue_GetListCount(windows); i++)
    {
        const ECSValue *tree = ECSValue_GetListItem(windows, i);
        usz length = ECSISession_Enter(reader, "windows", 0);
        ECSISession_Enter(reader, NULL, i);
        reader->maximized = NULL;

        if (ECSValue_GetType(tree) != ECSValueType_Table)
        {
            ECSISession_Report(reader, "a window must be a table; it is left empty.");
        }
        else
        {
            result = ECSISession_ReadNode(reader, tree, &roots[i].tree);
            roots[i].maximized = reader->maximized;

            // a pop-out window opens with the size it had
            if (i > 0)
            {
                roots[i].width = ECSISession_GetSize(reader, tree, "width", OPENECS_SESSION_POP_OUT_SIZE);
                roots[i].height = ECSISession_GetSize(reader, tree, "height", OPENECS_SESSION_POP_OUT_SIZE);
            }
        }

        ECSISession_Leave(reader, length);
    }

    // the workspace takes the trees, also when it fails
    if (!result)
    {
        result = ECSILayout_WorkspaceAdd(ECSValue_GetString(ECSValue_GetTableField(saved, "name"), "workspace"), roots, count, reader->focus);
    }
    else
    {
        for (usz i = 0; roots != NULL && i < count; i++)
        {
            if (roots[i].tree != NULL)
            {
                ECSILayout_NodeDestroy(&roots[i].tree);
            }
        }
    }

    SDL_free(roots);
    SHU_ReturnResult(result);

    // the keys go with the workspace just added, so their positions match
    return ECSIKeys_AddWorkspace(ECSValue_GetTableField(saved, "keys"));
}

/// @brief Saves the session to the file the user chose; a cancelled dialog gives no file.
static void ECSISession_SaveChosen(void *data, const char *const *files, usz count)
{
    (void)data;

    if (count > 0 && ECSSession_Save(files[0]) == SHUResult_Ok)
    {
        SDL_Log("Session saved to '%s'.", files[0]);
    }
    else if (count > 0)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cannot save the session to '%s'.", files[0]);
    }
}

/// @brief Opens the session the user chose; a cancelled dialog gives no file. ECSSession_Open reports the other reasons a file is not opened.
static void ECSISession_OpenChosen(void *data, const char *const *files, usz count)
{
    (void)data;

    if (count > 0 && ECSSession_Open(files[0]) == SHUResult_ErrAllocation)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cannot open the session '%s': out of memory.", files[0]);
    }
}

/// @brief Shows a dialog of saved sessions, which starts in the sessions folder.
static SHUResult ECSISession_ShowDialog(ECSDialogType type, ECSDialogDoneFunction Done)
{
    if (SESSION.folder != NULL && !SDL_CreateDirectory(SESSION.folder))
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cannot make the sessions folder '%s': %s", SESSION.folder, SDL_GetError());
    }

    ECSDialogFilter filter = {"OpenECS sessions", "lua"};
    ECSDialogDesc desc = {.type = type, .filters = &filter, .filterCount = 1, .location = SESSION.folder, .Done = Done};
    return ECSIInput_ShowDialog(NULL, &desc);
}

/// @brief What keys and menus run as ecs.session.save: saving without a path, which asks for the file.
static void ECSISession_SaveBound(void)
{
    if (ECSSession_Save(NULL))
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cannot ask where to save the session.");
    }
}

/// @brief What keys and menus run as ecs.session.open: opening without a path, which asks for the file. ECSSession_Open reports why it refuses.
static void ECSISession_OpenBound(void)
{
    if (ECSSession_Open(NULL) == SHUResult_ErrAllocation)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cannot ask which session to open.");
    }
}

/// @brief Adds the .lua files of a folder to a list, unless the list has a file of the same name. A folder that does not exist adds nothing.
static SHUResult ECSISession_FindFiles(ECSISessionFile **files, const char *folder)
{
    if (folder == NULL)
    {
        return SHUResult_Ok;
    }

    int count = 0;
    char **names = SDL_GlobDirectory(folder, "*.lua", 0, &count);
    SHUResult result = SHUResult_Ok;

    for (int i = 0; !result && i < count; i++)
    {
        ECSISessionFile file = {.name = SDL_strndup(names[i], SDL_strlen(names[i]) - 4)};
        bool listed = false;

        for (usz j = 0; file.name != NULL && j < arrlenu(*files); j++)
        {
            listed = listed || SDL_strcmp((*files)[j].name, file.name) == 0;
        }

        if (file.name == NULL || (!listed && SDL_asprintf(&file.path, "%s%s", folder, names[i]) < 0))
        {
            result = SHUResult_ErrAllocation;
        }

        if (result || listed)
        {
            SDL_free(file.name);
            continue;
        }

        arrput(*files, file);
    }

    SDL_free(names);
    return result;
}

static int ECSISession_CompareFiles(const void *a, const void *b)
{
    return SDL_strcmp(((const ECSISessionFile *)a)->name, ((const ECSISessionFile *)b)->name);
}

/// @brief Sets a field of a list entry to a text.
static SHUResult ECSISession_SetText(ECSValue *entry, const char *name, const char *text)
{
    ECSValue *field = NULL;
    SHU_ReturnResult(ECSValue_TableSetField(entry, name, &field));
    return ECSValue_SetString(field, text);
}

/// @brief Sets a field of a list entry to the time a file was written, in seconds since 1970. A missing file sets nothing.
static SHUResult ECSISession_SetTime(ECSValue *entry, const char *name, const char *path)
{
    SDL_PathInfo info = {0};

    if (path == NULL || !SDL_GetPathInfo(path, &info))
    {
        return SHUResult_Ok;
    }

    ECSValue *field = NULL;
    SHU_ReturnResult(ECSValue_TableSetField(entry, name, &field));
    ECSValue_SetInteger(field, info.modify_time / (SDL_Time)SDL_NS_PER_SECOND);
    return SHUResult_Ok;
}

/// @brief Reads a list's files, sorted by name, and adds an entry for each to the list. A file that cannot be read is reported and left out.
static SHUResult ECSISession_FillList(ECSValue *retList, ECSISessionFile *files, bool presets)
{
    SDL_qsort(files, arrlenu(files), sizeof(*files), ECSISession_CompareFiles);
    ECSValue_SetTable(retList);
    SHUResult result = SHUResult_Ok;

    for (usz i = 0; !result && i < arrlenu(files); i++)
    {
        ECSIPresetInfo info = {0};
        result = ECSISession_ReadInfo(files[i].path, &info);

        // the Lua module reported why the file cannot be read
        if (result && result != SHUResult_ErrAllocation)
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "'%s' is left out of the list of %s.", files[i].path, presets ? "presets" : "sessions");
            ECSISession_FreeInfo(&info);
            result = SHUResult_Ok;
            continue;
        }

        if (!result && (!presets || ECSValue_GetBool(ECSValue_GetTableField(info.file, "listed"), true)))
        {
            ECSValue *entry = NULL;
            result = ECSValue_ListAddItem(retList, &entry);

            if (!result)
            {
                ECSValue_SetTable(entry);
            }

            result = result ? result : ECSISession_SetText(entry, "name", files[i].name);
            result = result ? result : ECSISession_SetText(entry, "path", files[i].path);
            result = result ? result : ECSISession_SetText(entry, "appId", info.appId);
            result = result ? result : ECSISession_SetText(entry, "appName", info.appName);

            if (presets)
            {
                char *lastSession = ECSISession_GetLastPath(SESSION.state, info.appId);
                result = result ? result : ECSISession_SetTime(entry, "lastUsed", lastSession);
                SDL_free(lastSession);
            }
            else
            {
                result = result ? result : ECSISession_SetTime(entry, "saved", files[i].path);
            }
        }

        ECSISession_FreeInfo(&info);
    }

    return result;
}

static void ECSISession_FreeFiles(ECSISessionFile **files)
{
    for (usz i = 0; i < arrlenu(*files); i++)
    {
        SDL_free((*files)[i].name);
        SDL_free((*files)[i].path);
    }

    arrfree(*files);
}

/// @brief Opens a session or a preset in place of the current one, once the user has been asked about unsaved work.
static SHUResult ECSISession_OpenFile(const char *path, bool preset)
{
    // the file is checked first, so a file that is not a session never stops OpenECS
    ECSIPresetInfo info = {0};
    SHUResult result = ECSISession_ReadInfo(path, &info);
    bool hasWorkspaces = ECSValue_GetListCount(ECSValue_GetTableField(info.file, "workspaces")) > 0;
    ECSISession_FreeInfo(&info);

    if (!result && !hasWorkspaces)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "'%s' is not a %s: it has no list of workspaces.", path, preset ? "preset" : "session");
        result = SHUResult_ErrBadData;
    }

    SHU_ReturnResult(result);

    // like closing panels, opening a session keeps the unsaved work when the user cannot be asked
    ECSPanel *panels = ECSILayout_GetPanels();
    bool confirmed = ECSIPanels_ConfirmClose(panels, arrlenu(panels), false);
    arrfree(panels);

    if (!confirmed)
    {
        return SHUResult_Err;
    }

    char *next = SDL_strdup(path);

    if (next == NULL)
    {
        return SHUResult_ErrAllocation;
    }

    SDL_free(SESSION.next);
    SESSION.next = next;
    SESSION.nextIsPreset = preset;
    SDL_Log("OpenECS starts again from the %s '%s'.", preset ? "preset" : "session", path);
    return SHUResult_Ok;
}

/// @brief Refuses to open a session or a preset during a test, which cannot restart.
static bool ECSISession_CanOpen(void)
{
    if (!SESSION.canOpen)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "A test starts from its preset alone, so it cannot open a session or a preset.");
    }

    return SESSION.canOpen;
}

#pragma endregion Source Only

SHUResult ECSISession_FindPreset(char **retPath, const char *nameOrPath, const char *userFolder)
{
    SDL_assert(retPath != NULL);
    SDL_assert(nameOrPath != NULL);

    // a name has no folder and no extension
    usz length = SDL_strlen(nameOrPath);

    if (SDL_strchr(nameOrPath, '/') != NULL || (length > 4 && SDL_strcmp(nameOrPath + length - 4, ".lua") == 0))
    {
        *retPath = SDL_strdup(nameOrPath);
        return *retPath == NULL ? SHUResult_ErrAllocation : SHUResult_Ok;
    }

    // the user's preset wins over a first-party preset of the same name
    if (userFolder != NULL)
    {
        if (SDL_asprintf(retPath, "%s%s.lua", userFolder, nameOrPath) < 0)
        {
            return SHUResult_ErrAllocation;
        }

        if (SDL_GetPathInfo(*retPath, NULL))
        {
            return SHUResult_Ok;
        }

        SDL_free(*retPath);
    }

    return SDL_asprintf(retPath, "%spresets/%s.lua", SDL_GetBasePath(), nameOrPath) < 0 ? SHUResult_ErrAllocation : SHUResult_Ok;
}

char *ECSISession_GetLastPath(const char *stateFolder, const char *appId)
{
    SDL_assert(appId != NULL);

    char *path = NULL;

    if (stateFolder != NULL && SDL_asprintf(&path, "%s%s/%s", stateFolder, appId, OPENECS_LAST_SESSION_FILE) < 0)
    {
        path = NULL;
    }

    return path;
}

SHUResult ECSISession_ReadInfo(const char *path, ECSIPresetInfo *retInfo)
{
    SDL_assert(path != NULL);
    SDL_assert(retInfo != NULL);

    SDL_zerop(retInfo);
    SHU_ReturnResult(ECSValue_Create(&retInfo->file));
    SHU_ReturnResult(ECSILua_ReadData(path, retInfo->file));

    const ECSValue *file = retInfo->file;
    const ECSValue *app = ECSValue_GetTableField(file, "app");
    const char *appId = ECSValue_GetString(ECSValue_GetTableField(app, "id"), NULL);
    const char *pluginsDirectory = ECSValue_GetString(ECSValue_GetTableField(file, "pluginsDir"), NULL);

    retInfo->name = SDL_strdup(ECSValue_GetString(ECSValue_GetTableField(file, "name"), "preset"));
    retInfo->appName = SDL_strdup(ECSValue_GetString(ECSValue_GetTableField(app, "name"), "OpenECS"));

    if (appId != NULL)
    {
        retInfo->appId = SDL_strdup(appId);
    }
    else if (retInfo->name != NULL && SDL_asprintf(&retInfo->appId, "openecs.%s", retInfo->name) < 0)
    {
        retInfo->appId = NULL;
    }

    if (pluginsDirectory != NULL)
    {
        // relative to the preset's folder, unless it is absolute
        const char *slash = SDL_strrchr(path, '/');
        int folderLength = slash == NULL || pluginsDirectory[0] == '/' ? 0 : (int)(slash - path + 1);

        if (SDL_asprintf(&retInfo->pluginsDirectory, "%.*s%s/", folderLength, path, pluginsDirectory) < 0)
        {
            return SHUResult_ErrAllocation;
        }
    }

    if (retInfo->name == NULL || retInfo->appName == NULL || retInfo->appId == NULL)
    {
        return SHUResult_ErrAllocation;
    }

    return SHUResult_Ok;
}

void ECSISession_FreeInfo(ECSIPresetInfo *info)
{
    SDL_assert(info != NULL);

    SDL_free(info->name);
    SDL_free(info->appId);
    SDL_free(info->appName);
    SDL_free(info->pluginsDirectory);
    ECSValue_Destroy(&info->file);
    SDL_zerop(info);
}

SHUResult ECSISession_Apply(const char *path, const ECSIPresetInfo *info)
{
    SDL_assert(path != NULL);
    SDL_assert(info != NULL);

    ECSISessionReader reader = {.file = path, .path = "workspaces"};
    SESSION.info = info;
    SHU_ReturnResult(ECSIKeys_SetTool(ECSValue_GetTableField(info->file, "keys")));

    // plugins get their state before panels are created, so panels find their data
    ECSIPlugins_RestoreStates(ECSValue_GetTableField(info->file, "pluginState"));
    const ECSValue *workspaces = ECSValue_GetTableField(info->file, "workspaces");

    if (ECSValue_GetListCount(workspaces) == 0)
    {
        ECSISession_Report(&reader, "there must be a list of at least one workspace.");
        return SHUResult_ErrBadData;
    }

    for (usz i = 0; i < ECSValue_GetListCount(workspaces); i++)
    {
        const ECSValue *workspace = ECSValue_GetListItem(workspaces, i);
        usz length = ECSISession_Enter(&reader, NULL, i);

        if (ECSValue_GetType(workspace) != ECSValueType_Table)
        {
            ECSISession_Report(&reader, "a workspace must be a table; it is skipped.");
        }
        else
        {
            SHU_ReturnResult(ECSISession_ReadWorkspace(&reader, workspace));
        }

        ECSISession_Leave(&reader, length);
    }

    ECSILayout_WorkspaceSwitch((usz)SDL_max(1, ECSValue_GetInteger(ECSValue_GetTableField(info->file, "currentWorkspace"), 1)) - 1);
    return SHUResult_Ok;
}

SHUResult ECSISession_Build(const ECSIPresetInfo *info, ECSValue *retSession)
{
    SDL_assert(info != NULL);
    SDL_assert(retSession != NULL);

    // a copy of the file the session came from, so fields the core does not use are kept
    ECSValue *field = NULL;
    usz current = 0;
    SHUResult result = ECSIValue_Copy(retSession, info->file);
    result = result ? result : ECSValue_TableSetField(retSession, "format", &field);

    if (!result)
    {
        ECSValue_SetInteger(field, OPENECS_SESSION_FORMAT);
    }

    // the plugin directory is written resolved, because the session lives in another folder
    if (!result && info->pluginsDirectory != NULL)
    {
        result = ECSValue_TableSetField(retSession, "pluginsDir", &field);
        result = result ? result : ECSValue_SetString(field, info->pluginsDirectory);
    }

    result = result ? result : ECSValue_TableSetField(retSession, "pluginState", &field);
    result = result ? result : ECSIPlugins_SaveStates(field);
    result = result ? result : ECSValue_TableSetField(retSession, "workspaces", &field);
    result = result ? result : ECSILayout_Save(field, &current);

    // the layout knows nothing of keys, so each workspace's keys are added to it
    for (usz i = 0; !result && i < ECSValue_GetListCount(field); i++)
    {
        const ECSValue *keys = ECSIKeys_GetWorkspace(i);
        ECSValue *copy = NULL;

        if (keys != NULL)
        {
            result = ECSValue_TableSetField((ECSValue *)ECSValue_GetListItem(field, i), "keys", &copy);
            result = result ? result : ECSIValue_Copy(copy, keys);
        }
    }

    result = result ? result : ECSValue_TableSetField(retSession, "currentWorkspace", &field);

    if (!result)
    {
        ECSValue_SetInteger(field, (i64)current + 1);
    }

    return result;
}

SHUResult ECSISession_Initialize(const ECSISessionFolders *folders, bool canOpen)
{
    SDL_assert(folders != NULL);

    SESSION.presets = folders->presets == NULL ? NULL : SDL_strdup(folders->presets);
    SESSION.folder = folders->sessions == NULL ? NULL : SDL_strdup(folders->sessions);
    SESSION.state = folders->state == NULL ? NULL : SDL_strdup(folders->state);
    SESSION.canOpen = canOpen;

    if ((folders->presets != NULL && SESSION.presets == NULL) || (folders->sessions != NULL && SESSION.folder == NULL) || (folders->state != NULL && SESSION.state == NULL))
    {
        return SHUResult_ErrAllocation;
    }

    // keys run the same names as the Lua functions, which ask for the file when they get no path
    SHU_ReturnResult(ECSIServices_RegisterCore("ecs.session.save", (ECSFunction)ECSISession_SaveBound, "void()", "Save the session to a file"));
    return ECSIServices_RegisterCore("ecs.session.open", (ECSFunction)ECSISession_OpenBound, "void()", "Open a saved session");
}

const char *ECSISession_GetNext(const char **retOption)
{
    if (retOption != NULL)
    {
        *retOption = SESSION.nextIsPreset ? "--preset" : "--session";
    }

    return SESSION.next;
}

void ECSISession_Terminate(void)
{
    SDL_free(SESSION.presets);
    SDL_free(SESSION.folder);
    SDL_free(SESSION.state);
    SDL_free(SESSION.next);
    SDL_zero(SESSION);
}

SHUResult ECSSession_Open(const char *path)
{
    if (!ECSISession_CanOpen())
    {
        return SHUResult_ErrPrivileges;
    }

    if (path == NULL)
    {
        return ECSISession_ShowDialog(ECSDialogType_OpenFile, ECSISession_OpenChosen);
    }

    return ECSISession_OpenFile(path, false);
}

SHUResult ECSSession_OpenPreset(const char *nameOrPath)
{
    SDL_assert(nameOrPath != NULL);

    if (!ECSISession_CanOpen())
    {
        return SHUResult_ErrPrivileges;
    }

    char *path = NULL;
    SHU_ReturnResult(ECSISession_FindPreset(&path, nameOrPath, SESSION.presets));
    SHUResult result = ECSISession_OpenFile(path, true);
    SDL_free(path);
    return result;
}

SHUResult ECSSession_ListPresets(ECSValue *retList)
{
    SDL_assert(retList != NULL);

    // the user's presets first, so they win over first-party presets of the same name
    ECSISessionFile *files = NULL;
    char *firstParty = NULL;
    SHUResult result = SDL_asprintf(&firstParty, "%spresets/", SDL_GetBasePath()) < 0 ? SHUResult_ErrAllocation : SHUResult_Ok;
    result = result ? result : ECSISession_FindFiles(&files, SESSION.presets);
    result = result ? result : ECSISession_FindFiles(&files, firstParty);
    result = result ? result : ECSISession_FillList(retList, files, true);

    ECSISession_FreeFiles(&files);
    SDL_free(firstParty);
    return result;
}

SHUResult ECSSession_ListSessions(ECSValue *retList)
{
    SDL_assert(retList != NULL);

    ECSISessionFile *files = NULL;
    SHUResult result = ECSISession_FindFiles(&files, SESSION.folder);
    result = result ? result : ECSISession_FillList(retList, files, false);

    ECSISession_FreeFiles(&files);
    return result;
}

SHUResult ECSSession_Save(const char *path)
{
    SDL_assert(SESSION.info != NULL);

    if (path == NULL)
    {
        return ECSISession_ShowDialog(ECSDialogType_SaveFile, ECSISession_SaveChosen);
    }

    ECSValue *session = NULL;
    SHU_ReturnResult(ECSValue_Create(&session));
    SHUResult result = ECSISession_Build(SESSION.info, session);
    result = result ? result : ECSILua_WriteData(path, session);

    ECSValue_Destroy(&session);
    return result;
}
