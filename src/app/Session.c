#include "app/Session.h"

#include "interface/Input.h"
#include "interface/Layout.h"
#include "base/Lua.h"
#include "interface/Panels.h"
#include "base/Values.h"

#include "SDL3/SDL.h"
#include "stb/stbSDL3.h"

#pragma region Source Only

/// @brief Version of the preset and session format that this core writes.
#define OPENECS_SESSION_FORMAT 1

/// @brief Longest path to a part of a file that a problem report names.
#define OPENECS_SESSION_PATH_SIZE 256

/// @brief State while the workspaces of a file are built.
typedef struct ECSI_SessionReader
{
    const char *file;
    char path[OPENECS_SESSION_PATH_SIZE]; // the part being read, such as "workspaces[1].windows[1]"
    i64 focusId;                          // of the workspace being read
    ECSPanel focus;
    ECSI_Node *maximized;
} ECSI_SessionReader;

/// @brief Reports a problem at the part of the file being read.
static OPENECS_PRINTF(2, 3) void ECSI_SessionReport(const ECSI_SessionReader *reader, const char *format, ...)
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
static usz ECSI_SessionEnter(ECSI_SessionReader *reader, const char *field, usz index)
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

static void ECSI_SessionLeave(ECSI_SessionReader *reader, usz length)
{
    reader->path[length] = '\0';
}

/// @brief Reads a number field that must not be negative.
static f32 ECSI_SessionGetSize(ECSI_SessionReader *reader, const ECSValue *node, const char *name, f32 fallback)
{
    const ECSValue *field = ECSValue_GetField(node, name);
    f64 size = ECSValue_GetNumber(field, -1.0);

    if (field == NULL)
    {
        return fallback;
    }

    if (size < 0.0)
    {
        ECSI_SessionReport(reader, "'%s' must be a number of at least 0; it is ignored.", name);
        return fallback;
    }

    return (f32)size;
}

/// @brief Creates the panel that a table describes and adds it to a group. A bad panel is reported and skipped.
static SHUResult ECSI_SessionReadPanel(ECSI_SessionReader *reader, const ECSValue *saved, ECSI_Node *group)
{
    const char *type = ECSValue_GetString(ECSValue_GetField(saved, "type"), NULL);

    if (type == NULL)
    {
        ECSI_SessionReport(reader, "a panel must be a table with a 'type' text; it is skipped.");
        return SHUResult_Ok;
    }

    const ECSValue *state = ECSValue_GetField(saved, "state");
    i64 version = ECSValue_GetInteger(ECSValue_GetField(saved, "state_version"), 0);
    i64 id = ECSValue_GetInteger(ECSValue_GetField(saved, "id"), 0);
    ECSPanel panel = NULL;

    SHU_ReturnResult(ECSI_PanelCreate(&panel, type, state, version >= 0 && version <= SDL_MAX_UINT32 ? (u32)version : 0));
    ECSI_PanelSetId(panel, id > 0 && id <= SDL_MAX_UINT32 ? (u32)id : 0);
    ECSI_LayoutGroupAdd(group, panel);

    if (id != 0 && id == reader->focusId)
    {
        reader->focus = panel;
    }

    return SHUResult_Ok;
}

/// @brief Builds the layout node that a table describes: a split if it has "split", a group otherwise.
static SHUResult ECSI_SessionReadNode(ECSI_SessionReader *reader, const ECSValue *saved, ECSI_Node **retNode)
{
    const ECSValue *split = ECSValue_GetField(saved, "split");

    if (split != NULL)
    {
        const char *direction = ECSValue_GetString(split, "");
        bool vertical = SDL_strcmp(direction, "vertical") == 0;

        if (!vertical && SDL_strcmp(direction, "horizontal") != 0)
        {
            ECSI_SessionReport(reader, "'split' must be \"horizontal\" or \"vertical\"; \"horizontal\" is used.");
        }

        SHU_ReturnResult(ECSI_LayoutSplitCreate(retNode, vertical));

        for (usz i = 0; i < ECSValue_GetCount(saved); i++)
        {
            const ECSValue *savedChild = ECSValue_GetItem(saved, i);
            usz length = ECSI_SessionEnter(reader, NULL, i);
            ECSI_Node *child = NULL;
            SHUResult result = SHUResult_Ok;

            if (ECSValue_GetType(savedChild) != ECSValueType_Table)
            {
                ECSI_SessionReport(reader, "a part of a split must be a table; it is skipped.");
            }
            else
            {
                result = ECSI_SessionReadNode(reader, savedChild, &child);
            }

            if (child != NULL)
            {
                ECSI_LayoutSplitAdd(*retNode, child, ECSI_SessionGetSize(reader, savedChild, "size", 0.0f), ECSI_SessionGetSize(reader, savedChild, "share", 1.0f));
            }

            ECSI_SessionLeave(reader, length);
            SHU_ReturnResult(result, ECSI_LayoutNodeDestroy(retNode););
        }

        return SHUResult_Ok;
    }

    SHU_ReturnResult(ECSI_LayoutGroupCreate(retNode));

    if (ECSValue_GetBool(ECSValue_GetField(saved, "maximized"), false))
    {
        reader->maximized = *retNode;
    }

    ECSI_LayoutGroupSetLocked(*retNode, ECSValue_GetBool(ECSValue_GetField(saved, "locked"), false));

    const ECSValue *panels = ECSValue_GetField(saved, "panels");
    usz panelsLength = ECSI_SessionEnter(reader, "panels", 0);

    for (usz i = 0; i < ECSValue_GetCount(panels); i++)
    {
        usz length = ECSI_SessionEnter(reader, NULL, i);
        SHUResult result = ECSI_SessionReadPanel(reader, ECSValue_GetItem(panels, i), *retNode);
        ECSI_SessionLeave(reader, length);
        SHU_ReturnResult(result, ECSI_LayoutNodeDestroy(retNode););
    }

    ECSI_SessionLeave(reader, panelsLength);
    ECSI_LayoutGroupShow(*retNode, (usz)SDL_max(1, ECSValue_GetInteger(ECSValue_GetField(saved, "shown"), 1)) - 1);
    return SHUResult_Ok;
}

/// @brief Builds the workspace that a table describes.
static SHUResult ECSI_SessionReadWorkspace(ECSI_SessionReader *reader, const ECSValue *saved)
{
    const ECSValue *windows = ECSValue_GetField(saved, "windows");
    const ECSValue *tree = ECSValue_GetItem(windows, 0);
    ECSI_Node *root = NULL;

    reader->focusId = ECSValue_GetInteger(ECSValue_GetField(saved, "focus"), 0);
    reader->focus = NULL;
    reader->maximized = NULL;

    if (ECSValue_GetCount(windows) > 1)
    {
        ECSI_SessionReport(reader, "pop-out windows are not implemented yet; only the first window is used.");
    }

    if (tree != NULL)
    {
        usz length = ECSI_SessionEnter(reader, "windows", 0);
        ECSI_SessionEnter(reader, NULL, 0);

        if (ECSValue_GetType(tree) != ECSValueType_Table)
        {
            ECSI_SessionReport(reader, "a window must be a table; the workspace is left empty.");
        }
        else
        {
            SHU_ReturnResult(ECSI_SessionReadNode(reader, tree, &root));
        }

        ECSI_SessionLeave(reader, length);
    }

    SHU_ReturnResult(ECSI_LayoutWorkspaceAdd(ECSValue_GetString(ECSValue_GetField(saved, "name"), "workspace"), root, reader->focus, reader->maximized),
                     if (root != NULL) { ECSI_LayoutNodeDestroy(&root); });

    // the keys go with the workspace just added, so their positions match
    return ECSI_InputAddWorkspaceKeys(ECSValue_GetField(saved, "keys"));
}

#pragma endregion Source Only

SHUResult ECSI_SessionFindPreset(char **retPath, const char *nameOrPath)
{
    SDL_assert(retPath != NULL);
    SDL_assert(nameOrPath != NULL);

    if (SDL_strchr(nameOrPath, '/') != NULL)
    {
        *retPath = SDL_strdup(nameOrPath);
        return *retPath == NULL ? SHUResult_ErrAllocation : SHUResult_Ok;
    }

    return SDL_asprintf(retPath, "%spresets/%s.lua", SDL_GetBasePath(), nameOrPath) < 0 ? SHUResult_ErrAllocation : SHUResult_Ok;
}

SHUResult ECSI_SessionReadInfo(const char *path, ECSI_PresetInfo *retInfo)
{
    SDL_assert(path != NULL);
    SDL_assert(retInfo != NULL);

    SDL_zerop(retInfo);
    SHU_ReturnResult(ECSValue_Create(&retInfo->file));
    SHU_ReturnResult(ECSI_LuaReadData(path, retInfo->file));

    const ECSValue *file = retInfo->file;
    const ECSValue *app = ECSValue_GetField(file, "app");
    const char *appId = ECSValue_GetString(ECSValue_GetField(app, "id"), NULL);
    const char *pluginsDirectory = ECSValue_GetString(ECSValue_GetField(file, "plugins_dir"), NULL);

    retInfo->name = SDL_strdup(ECSValue_GetString(ECSValue_GetField(file, "name"), "preset"));
    retInfo->appName = SDL_strdup(ECSValue_GetString(ECSValue_GetField(app, "name"), "OpenECS"));

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

void ECSI_SessionFreeInfo(ECSI_PresetInfo *info)
{
    SDL_assert(info != NULL);

    SDL_free(info->name);
    SDL_free(info->appId);
    SDL_free(info->appName);
    SDL_free(info->pluginsDirectory);
    ECSValue_Destroy(&info->file);
    SDL_zerop(info);
}

SHUResult ECSI_SessionApply(const char *path, const ECSI_PresetInfo *info)
{
    SDL_assert(path != NULL);
    SDL_assert(info != NULL);

    ECSI_SessionReader reader = {.file = path, .path = "workspaces"};
    SHU_ReturnResult(ECSI_InputSetToolKeys(ECSValue_GetField(info->file, "keys")));
    const ECSValue *workspaces = ECSValue_GetField(info->file, "workspaces");

    if (ECSValue_GetCount(workspaces) == 0)
    {
        ECSI_SessionReport(&reader, "there must be a list of at least one workspace.");
        return SHUResult_ErrBadData;
    }

    for (usz i = 0; i < ECSValue_GetCount(workspaces); i++)
    {
        const ECSValue *workspace = ECSValue_GetItem(workspaces, i);
        usz length = ECSI_SessionEnter(&reader, NULL, i);

        if (ECSValue_GetType(workspace) != ECSValueType_Table)
        {
            ECSI_SessionReport(&reader, "a workspace must be a table; it is skipped.");
        }
        else
        {
            SHU_ReturnResult(ECSI_SessionReadWorkspace(&reader, workspace));
        }

        ECSI_SessionLeave(&reader, length);
    }

    ECSI_LayoutWorkspaceSwitch((usz)SDL_max(1, ECSValue_GetInteger(ECSValue_GetField(info->file, "current_workspace"), 1)) - 1);
    return SHUResult_Ok;
}

SHUResult ECSI_SessionSave(const char *path, const ECSI_PresetInfo *info)
{
    SDL_assert(path != NULL);
    SDL_assert(info != NULL);

    // a copy of the file the session came from, so fields the core does not use are kept
    ECSValue *session = NULL;
    ECSValue *field = NULL;
    usz current = 0;
    SHU_ReturnResult(ECSValue_Create(&session));
    SHUResult result = ECSI_ValueCopy(session, info->file);
    result = result ? result : ECSValue_SetField(session, "format", &field);

    if (!result)
    {
        ECSValue_SetInteger(field, OPENECS_SESSION_FORMAT);
    }

    // the plugin directory is written resolved, because the session lives in another folder
    if (!result && info->pluginsDirectory != NULL)
    {
        result = ECSValue_SetField(session, "plugins_dir", &field);
        result = result ? result : ECSValue_SetString(field, info->pluginsDirectory);
    }

    result = result ? result : ECSValue_SetField(session, "workspaces", &field);
    result = result ? result : ECSI_LayoutSave(field, &current);

    // the layout knows nothing of keys, so each workspace's keys are added to it
    for (usz i = 0; !result && i < ECSValue_GetCount(field); i++)
    {
        const ECSValue *keys = ECSI_InputGetWorkspaceKeys(i);
        ECSValue *copy = NULL;

        if (keys != NULL)
        {
            result = ECSValue_SetField((ECSValue *)ECSValue_GetItem(field, i), "keys", &copy);
            result = result ? result : ECSI_ValueCopy(copy, keys);
        }
    }

    result = result ? result : ECSValue_SetField(session, "current_workspace", &field);

    if (!result)
    {
        ECSValue_SetInteger(field, (i64)current + 1);
        result = ECSI_LuaWriteData(path, session);
    }

    ECSValue_Destroy(&session);
    return result;
}
