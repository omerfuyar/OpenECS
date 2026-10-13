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

/// @brief What the fields of a preset or a session must be; a session is a preset that OpenECS wrote, with the state it was saved in.
static const ECSIFieldRule OPENECS_PRESET_RULES[] = {
    {"format", ECSIFieldType_Integer, true},
    {"name", ECSIFieldType_String, true},
    {"version", ECSIFieldType_String, true},
    {"app", ECSIFieldType_Table, true},
    {"depends", ECSIFieldType_Table, true},
    {"pluginsDir", ECSIFieldType_String, false},
    {"open", ECSIFieldType_String, false},
    {"settings", ECSIFieldType_Table, false},
    {"keys", ECSIFieldType_Table, false},
    {"listed", ECSIFieldType_Bool, false},
    {"workspaces", ECSIFieldType_Table, true},
    {"currentWorkspace", ECSIFieldType_Integer, false},
    {"pluginState", ECSIFieldType_Table, false},
};

/// @brief What the fields of a preset's app table must be.
static const ECSIFieldRule OPENECS_APP_RULES[] = {
    {"id", ECSIFieldType_String, true},
    {"name", ECSIFieldType_String, true},
    {"icon", ECSIFieldType_String, false},
};

/// @brief What the fields of a workspace must be.
static const ECSIFieldRule OPENECS_WORKSPACE_RULES[] = {
    {"name", ECSIFieldType_String, true},
    {"windows", ECSIFieldType_Table, true},
    {"keys", ECSIFieldType_Table, false},
    {"focus", ECSIFieldType_Integer, false},
};

/// @brief What the fields of a layout node must be: a split has "split" and its children as list items, a group has "panels".
static const ECSIFieldRule OPENECS_NODE_RULES[] = {
    {"split", ECSIFieldType_String, false},
    {"panels", ECSIFieldType_Table, false},
    {"size", ECSIFieldType_Number, false},
    {"share", ECSIFieldType_Number, false},
    {"shown", ECSIFieldType_Integer, false},
    {"locked", ECSIFieldType_Bool, false},
    {"maximized", ECSIFieldType_Bool, false},
    {"width", ECSIFieldType_Number, false},
    {"height", ECSIFieldType_Number, false},
};

/// @brief What the fields of a panel must be.
static const ECSIFieldRule OPENECS_PANEL_RULES[] = {
    {"type", ECSIFieldType_String, true},
    {"state", ECSIFieldType_Any, false},
    {"stateVersion", ECSIFieldType_Integer, false},
    {"id", ECSIFieldType_Integer, false},
};

static struct
{
    const ECSIPresetInfo *info; // the preset or session in use, which saved sessions are written from
    char *presets;              // the user's presets, or nullptr
    char *folder;               // the saved sessions, where the dialogs that ask for session files start, or nullptr
    char *state;                // the state folder, or nullptr
    char *origin;               // the preset, or the session the command line named, that ecs.layout.reset builds the layout from
    bool canOpen;               // false during a test
    char *next;                 // the session or preset that OpenECS starts again from once it stops, or nullptr
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
    bool valid; // while the file is checked: no problem is found yet
} ECSISessionReader;

/// @brief Reports a problem at the part of the file being read.
static OPENECS_PRINTF(2, 3) void ECSISession_Report(const ECSISessionReader *reader, const char *format, ...)
{
    char *message = nullptr;
    va_list arguments;
    va_start(arguments, format);
    int length = SDL_vasprintf(&message, format, arguments);
    va_end(arguments);

    if (length >= 0)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "'%s', at %s: %s.", reader->file, reader->path[0] != '\0' ? reader->path : "the top", message);
        SDL_free(message);
    }
}

/// @brief Adds a field name or a list position to the path, and returns the old length to restore it.
static usz ECSISession_Enter(ECSISessionReader *reader, const char *field, usz index)
{
    usz length = SDL_strlen(reader->path);
    usz room = sizeof(reader->path) - length;

    if (field != nullptr)
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

/// @brief Checks a table's named fields against rules, and reports the first problem at the part being read.
static bool ECSISession_CheckFields(ECSISessionReader *reader, const ECSValue *table, const ECSIFieldRule *rules, usz count)
{
    char *problem = nullptr;

    if (ECSIValue_CheckFields(table, rules, count, &problem))
    {
        return true;
    }

    ECSISession_Report(reader, "%s", problem != nullptr ? problem : "out of memory");
    SDL_free(problem);
    return false;
}

/// @brief Checks the number fields of a node that must not be negative.
static bool ECSISession_CheckSizes(ECSISessionReader *reader, const ECSValue *node)
{
    const char *const names[] = {"size", "share", "width", "height"};

    for (usz i = 0; i < SDL_arraysize(names); i++)
    {
        if (ECSValue_GetNumber(ECSValue_GetTableField(node, names[i]), 0.0) < 0.0)
        {
            ECSISession_Report(reader, "'%s' must not be negative", names[i]);
            return false;
        }
    }

    return true;
}

/// @brief Checks a layout node: a split with "split" and at least one part as list items, or a group with a list of panels.
static bool ECSISession_CheckNode(ECSISessionReader *reader, const ECSValue *node)
{
    if (!ECSISession_CheckFields(reader, node, OPENECS_NODE_RULES, SDL_arraysize(OPENECS_NODE_RULES)) || !ECSISession_CheckSizes(reader, node))
    {
        return false;
    }

    const char *split = ECSValue_GetString(ECSValue_GetTableField(node, "split"), nullptr);
    const ECSValue *panels = ECSValue_GetTableField(node, "panels");
    usz parts = ECSValue_GetListCount(node);
    bool valid = true;

    if (split != nullptr)
    {
        if (SDL_strcmp(split, "horizontal") != 0 && SDL_strcmp(split, "vertical") != 0)
        {
            ECSISession_Report(reader, "'split' must be \"horizontal\" or \"vertical\"");
            return false;
        }

        if (panels != nullptr || parts == 0)
        {
            ECSISession_Report(reader, "a split has its parts as list items, at least one, and no 'panels'");
            return false;
        }

        for (usz i = 0; valid && i < parts; i++)
        {
            usz length = ECSISession_Enter(reader, nullptr, i);
            valid = ECSISession_CheckNode(reader, ECSValue_GetListItem(node, i));
            ECSISession_Leave(reader, length);
        }

        return valid;
    }

    if (parts > 0)
    {
        ECSISession_Report(reader, "a group has no list items; a node with parts needs 'split'");
        return false;
    }

    usz panelsLength = ECSISession_Enter(reader, "panels", 0);
    valid = panels == nullptr || ECSISession_CheckFields(reader, panels, nullptr, 0);

    for (usz i = 0; valid && i < ECSValue_GetListCount(panels); i++)
    {
        usz length = ECSISession_Enter(reader, nullptr, i);
        valid = ECSISession_CheckFields(reader, ECSValue_GetListItem(panels, i), OPENECS_PANEL_RULES, SDL_arraysize(OPENECS_PANEL_RULES));
        ECSISession_Leave(reader, length);
    }

    ECSISession_Leave(reader, panelsLength);
    return valid;
}

/// @brief Checks a list of tables with no named fields, at least one long, and each of its items with a function.
static bool ECSISession_CheckList(ECSISessionReader *reader, const char *field, const ECSValue *list, bool (*CheckItem)(ECSISessionReader *, const ECSValue *))
{
    usz listLength = ECSISession_Enter(reader, field, 0);
    bool valid = ECSISession_CheckFields(reader, list, nullptr, 0);

    if (valid && ECSValue_GetListCount(list) == 0)
    {
        ECSISession_Report(reader, "it needs at least one item");
        valid = false;
    }

    for (usz i = 0; valid && i < ECSValue_GetListCount(list); i++)
    {
        usz length = ECSISession_Enter(reader, nullptr, i);
        valid = CheckItem(reader, ECSValue_GetListItem(list, i));
        ECSISession_Leave(reader, length);
    }

    ECSISession_Leave(reader, listLength);
    return valid;
}

/// @brief Checks a workspace and the layout tree of each of its windows.
static bool ECSISession_CheckWorkspace(ECSISessionReader *reader, const ECSValue *workspace)
{
    return ECSISession_CheckFields(reader, workspace, OPENECS_WORKSPACE_RULES, SDL_arraysize(OPENECS_WORKSPACE_RULES)) &&
           ECSISession_CheckList(reader, "windows", ECSValue_GetTableField(workspace, "windows"), ECSISession_CheckNode);
}

/// @brief Checks a dependency of a preset: a plugin's name with the lowest version it accepts.
static void ECSISession_CheckDependency(const char *name, const ECSValue *field, void *userData)
{
    ECSISessionReader *reader = userData;

    if (reader->valid && !ECSIValue_IsVersion(ECSValue_GetString(field, nullptr)))
    {
        ECSISession_Report(reader, "the version of '%s' must be a version text, such as \"0.1\"", name);
        reader->valid = false;
    }
}

/// @brief Checks the structure of a preset or a session (DESIGN 13.2), and reports the first problem with the part of the file it is in.
static bool ECSISession_Check(const char *path, const ECSValue *file)
{
    ECSISessionReader reader = {.file = path, .valid = true};

    if (!ECSISession_CheckFields(&reader, file, OPENECS_PRESET_RULES, SDL_arraysize(OPENECS_PRESET_RULES)))
    {
        return false;
    }

    i64 format = ECSValue_GetInteger(ECSValue_GetTableField(file, "format"), 0);

    if (format < 1 || format > OPENECS_SESSION_FORMAT)
    {
        ECSISession_Report(&reader, "'format' is %" SDL_PRIs64 ", but this OpenECS reads format %d", format, OPENECS_SESSION_FORMAT);
        return false;
    }

    if (!ECSIValue_IsVersion(ECSValue_GetString(ECSValue_GetTableField(file, "version"), nullptr)))
    {
        ECSISession_Report(&reader, "'version' must be a version text, such as \"1.0.0\"");
        return false;
    }

    usz length = ECSISession_Enter(&reader, "app", 0);
    reader.valid = ECSISession_CheckFields(&reader, ECSValue_GetTableField(file, "app"), OPENECS_APP_RULES, SDL_arraysize(OPENECS_APP_RULES));
    ECSISession_Leave(&reader, length);

    // depends names each plugin with a version
    const ECSValue *depends = ECSValue_GetTableField(file, "depends");
    length = ECSISession_Enter(&reader, "depends", 0);

    if (reader.valid && ECSValue_GetListCount(depends) > 0)
    {
        ECSISession_Report(&reader, "it names each plugin with a version, such as { ui = \"0.1\" }");
        reader.valid = false;
    }

    ECSIValue_TableForEachField(depends, ECSISession_CheckDependency, &reader);
    ECSISession_Leave(&reader, length);
    return reader.valid && ECSISession_CheckList(&reader, "workspaces", ECSValue_GetTableField(file, "workspaces"), ECSISession_CheckWorkspace);
}

/// @brief Reads a number field that the check made sure is not negative.
static f32 ECSISession_GetSize(const ECSValue *node, const char *name, f32 fallback)
{
    return (f32)ECSValue_GetNumber(ECSValue_GetTableField(node, name), fallback);
}

/// @brief Creates the panel that a table describes and adds it to a group.
static SHUResult ECSISession_ReadPanel(ECSISessionReader *reader, const ECSValue *saved, ECSINode *group)
{
    const char *type = ECSValue_GetString(ECSValue_GetTableField(saved, "type"), "");

    const ECSValue *state = ECSValue_GetTableField(saved, "state");
    i64 version = ECSValue_GetInteger(ECSValue_GetTableField(saved, "stateVersion"), 0);
    i64 id = ECSValue_GetInteger(ECSValue_GetTableField(saved, "id"), 0);
    ECSPanel panel = nullptr;

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

    if (split != nullptr)
    {
        SHU_ReturnResult(ECSILayout_SplitCreate(retNode, SDL_strcmp(ECSValue_GetString(split, ""), "vertical") == 0));

        for (usz i = 0; i < ECSValue_GetListCount(saved); i++)
        {
            const ECSValue *savedChild = ECSValue_GetListItem(saved, i);
            usz length = ECSISession_Enter(reader, nullptr, i);
            ECSINode *child = nullptr;
            SHUResult result = ECSISession_ReadNode(reader, savedChild, &child);

            if (child != nullptr)
            {
                ECSILayout_SplitAdd(*retNode, child, ECSISession_GetSize(savedChild, "size", 0.0f), ECSISession_GetSize(savedChild, "share", 1.0f));
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
        usz length = ECSISession_Enter(reader, nullptr, i);
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
    SHUResult result = roots == nullptr ? SHUResult_ErrAllocation : SHUResult_Ok;

    reader->focusId = ECSValue_GetInteger(ECSValue_GetTableField(saved, "focus"), 0);
    reader->focus = nullptr;

    for (usz i = 0; !result && i < ECSValue_GetListCount(windows); i++)
    {
        const ECSValue *tree = ECSValue_GetListItem(windows, i);
        usz length = ECSISession_Enter(reader, "windows", 0);
        ECSISession_Enter(reader, nullptr, i);
        reader->maximized = nullptr;
        result = ECSISession_ReadNode(reader, tree, &roots[i].tree);
        roots[i].maximized = reader->maximized;

        // a pop-out window opens with the size it had
        if (i > 0)
        {
            roots[i].width = ECSISession_GetSize(tree, "width", OPENECS_SESSION_POP_OUT_SIZE);
            roots[i].height = ECSISession_GetSize(tree, "height", OPENECS_SESSION_POP_OUT_SIZE);
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
        for (usz i = 0; roots != nullptr && i < count; i++)
        {
            if (roots[i].tree != nullptr)
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

/// @brief Builds the workspaces of a checked preset or session.
static SHUResult ECSISession_ReadWorkspaces(const char *path, const ECSValue *file)
{
    ECSISessionReader reader = {.file = path, .path = "workspaces"};
    const ECSValue *workspaces = ECSValue_GetTableField(file, "workspaces");

    for (usz i = 0; i < ECSValue_GetListCount(workspaces); i++)
    {
        usz length = ECSISession_Enter(&reader, nullptr, i);
        SHU_ReturnResult(ECSISession_ReadWorkspace(&reader, ECSValue_GetListItem(workspaces, i)));
        ECSISession_Leave(&reader, length);
    }

    return SHUResult_Ok;
}

/// @brief Gets the position of the workspace a preset or session shows first, starting at 0.
static usz ECSISession_CurrentWorkspace(const ECSValue *file)
{
    return (usz)SDL_max(1, ECSValue_GetInteger(ECSValue_GetTableField(file, "currentWorkspace"), 1)) - 1;
}

/// @brief Builds every workspace again from the file the tool started from, once the user has been asked about unsaved work. The plugins keep running, with their state.
static void ECSISession_ResetBound(void)
{
    ECSPanel *panels = ECSILayout_GetPanels();
    bool confirmed = ECSIPanels_ConfirmClose(panels, arrlenu(panels), false);
    arrfree(panels);

    // the file is read again, so a preset changed since the start is used
    ECSIPresetInfo info = {0};

    if (!confirmed || ECSISession_ReadInfo(SESSION.origin, &info))
    {
        ECSISession_FreeInfo(&info);
        return;
    }

    ECSILayout_Clear();
    ECSIKeys_ClearWorkspaces();

    if (ECSISession_ReadWorkspaces(SESSION.origin, info.file))
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "The layout cannot be built again from '%s'.", SESSION.origin);
    }

    ECSILayout_ShowFirst(ECSISession_CurrentWorkspace(info.file));
    ECSISession_FreeInfo(&info);
    SDL_Log("The layout is built again from '%s'.", SESSION.origin);
}

/// @brief Saves the session to the file the user chose, adding ".lua" if the name has no such ending; a cancelled dialog gives no file.
static void ECSISession_SaveChosen(void *data, const char *const *files, usz count)
{
    (void)data;
    usz length = count > 0 ? SDL_strlen(files[0]) : 0;
    char *path = nullptr;

    if (count == 0 || SDL_asprintf(&path, "%s%s", files[0], length >= 4 && SDL_strcmp(files[0] + length - 4, ".lua") == 0 ? "" : ".lua") < 0)
    {
        return;
    }

    if (ECSSession_Save(path) == SHUResult_Ok)
    {
        SDL_Log("Session saved to '%s'.", path);
    }
    else
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot save the session to '%s'.", path);
    }

    SDL_free(path);
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

/// @brief Shows a dialog of saved sessions and presets, which starts in the sessions folder. A save dialog suggests a file named after the preset, so a new file is made by pressing Save.
static SHUResult ECSISession_ShowDialog(ECSDialogType type, ECSDialogDoneFunction Done)
{
    if (SESSION.folder != nullptr && !SDL_CreateDirectory(SESSION.folder))
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cannot make the sessions folder '%s': %s", SESSION.folder, SDL_GetError());
    }

    char *location = nullptr;

    if (type == ECSDialogType_SaveFile && SESSION.folder != nullptr && SDL_asprintf(&location, "%s%s.lua", SESSION.folder, SESSION.info->name != nullptr ? SESSION.info->name : "session") < 0)
    {
        location = nullptr;
    }

    ECSDialogFilter filter = {"OpenECS sessions and presets", "lua"};
    ECSDialogDesc desc = {.type = type, .filters = &filter, .filterCount = 1, .location = location != nullptr ? location : SESSION.folder, .Done = Done};
    SHUResult result = ECSIInput_ShowDialog(nullptr, &desc);
    SDL_free(location);
    return result;
}

/// @brief What keys and menus run as ecs.session.save: saving without a path, which asks for the file.
static void ECSISession_SaveBound(void)
{
    if (ECSSession_Save(nullptr))
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cannot ask where to save the session.");
    }
}

/// @brief What keys and menus run as ecs.session.open: opening without a path, which asks for the file. ECSSession_Open reports why it refuses.
static void ECSISession_OpenBound(void)
{
    if (ECSSession_Open(nullptr) == SHUResult_ErrAllocation)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cannot ask which session to open.");
    }
}

/// @brief Adds the .lua files of a folder to a list, unless the list has a file of the same name. A folder that does not exist adds nothing.
static SHUResult ECSISession_FindFiles(ECSISessionFile **files, const char *folder)
{
    if (folder == nullptr)
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

        for (usz j = 0; file.name != nullptr && j < arrlenu(*files); j++)
        {
            listed = listed || SDL_strcmp((*files)[j].name, file.name) == 0;
        }

        if (file.name == nullptr || (!listed && SDL_asprintf(&file.path, "%s%s", folder, names[i]) < 0))
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
    ECSValue *field = nullptr;
    SHU_ReturnResult(ECSValue_TableSetField(entry, name, &field));
    return ECSValue_SetString(field, text);
}

/// @brief Sets a field of a list entry to the time a file was written, in seconds since 1970. A missing file sets nothing.
static SHUResult ECSISession_SetTime(ECSValue *entry, const char *name, const char *path)
{
    SDL_PathInfo info = {0};

    if (path == nullptr || !SDL_GetPathInfo(path, &info))
    {
        return SHUResult_Ok;
    }

    ECSValue *field = nullptr;
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
            ECSValue *entry = nullptr;
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
    ECSISession_FreeInfo(&info);
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

    if (next == nullptr)
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
    SDL_assert(retPath != nullptr);
    SDL_assert(nameOrPath != nullptr);

    // a name has no folder and no extension
    usz length = SDL_strlen(nameOrPath);

    if (SDL_strchr(nameOrPath, '/') != nullptr || (length > 4 && SDL_strcmp(nameOrPath + length - 4, ".lua") == 0))
    {
        *retPath = SDL_strdup(nameOrPath);
        return *retPath == nullptr ? SHUResult_ErrAllocation : SHUResult_Ok;
    }

    // the user's preset wins over a first-party preset of the same name
    if (userFolder != nullptr)
    {
        if (SDL_asprintf(retPath, "%s%s.lua", userFolder, nameOrPath) < 0)
        {
            return SHUResult_ErrAllocation;
        }

        if (SDL_GetPathInfo(*retPath, nullptr))
        {
            return SHUResult_Ok;
        }

        SDL_free(*retPath);
    }

    return SDL_asprintf(retPath, "%spresets/%s.lua", SDL_GetBasePath(), nameOrPath) < 0 ? SHUResult_ErrAllocation : SHUResult_Ok;
}

char *ECSISession_GetLastPath(const char *stateFolder, const char *appId)
{
    SDL_assert(appId != nullptr);

    char *path = nullptr;

    if (stateFolder != nullptr && SDL_asprintf(&path, "%s%s/%s", stateFolder, appId, OPENECS_LAST_SESSION_FILE) < 0)
    {
        path = nullptr;
    }

    return path;
}

SHUResult ECSISession_ReadInfo(const char *path, ECSIPresetInfo *retInfo)
{
    SDL_assert(path != nullptr);
    SDL_assert(retInfo != nullptr);

    SDL_zerop(retInfo);
    SHU_ReturnResult(ECSValue_Create(&retInfo->file));
    SHU_ReturnResult(ECSILua_ReadData(path, retInfo->file));

    // a file that is not a valid preset or session never starts a tool
    if (!ECSISession_Check(path, retInfo->file))
    {
        return SHUResult_ErrBadData;
    }

    const ECSValue *file = retInfo->file;
    const ECSValue *app = ECSValue_GetTableField(file, "app");
    const char *appId = ECSValue_GetString(ECSValue_GetTableField(app, "id"), nullptr);
    const char *pluginsDirectory = ECSValue_GetString(ECSValue_GetTableField(file, "pluginsDir"), nullptr);

    retInfo->name = SDL_strdup(ECSValue_GetString(ECSValue_GetTableField(file, "name"), ""));
    retInfo->appName = SDL_strdup(ECSValue_GetString(ECSValue_GetTableField(app, "name"), ""));
    retInfo->appId = SDL_strdup(appId);

    if (pluginsDirectory != nullptr)
    {
        // relative to the preset's folder, unless it is absolute
        const char *slash = SDL_strrchr(path, '/');
        int folderLength = slash == nullptr || pluginsDirectory[0] == '/' ? 0 : (int)(slash - path + 1);

        if (SDL_asprintf(&retInfo->pluginsDirectory, "%.*s%s/", folderLength, path, pluginsDirectory) < 0)
        {
            return SHUResult_ErrAllocation;
        }
    }

    if (retInfo->name == nullptr || retInfo->appName == nullptr || retInfo->appId == nullptr)
    {
        return SHUResult_ErrAllocation;
    }

    return SHUResult_Ok;
}

void ECSISession_FreeInfo(ECSIPresetInfo *info)
{
    SDL_assert(info != nullptr);

    SDL_free(info->name);
    SDL_free(info->appId);
    SDL_free(info->appName);
    SDL_free(info->pluginsDirectory);
    ECSValue_Destroy(&info->file);
    SDL_zerop(info);
}

SHUResult ECSISession_Apply(const char *path, const ECSIPresetInfo *info)
{
    SDL_assert(path != nullptr);
    SDL_assert(info != nullptr);

    SESSION.info = info;
    SHU_ReturnResult(ECSIKeys_SetTool(ECSValue_GetTableField(info->file, "keys")));

    // plugins get their state before panels are created, so panels find their data
    ECSIPlugins_RestoreStates(ECSValue_GetTableField(info->file, "pluginState"));
    SHU_ReturnResult(ECSISession_ReadWorkspaces(path, info->file));
    ECSILayout_WorkspaceSwitch(ECSISession_CurrentWorkspace(info->file));
    return SHUResult_Ok;
}

SHUResult ECSISession_Build(const ECSIPresetInfo *info, ECSValue *retSession)
{
    SDL_assert(info != nullptr);
    SDL_assert(retSession != nullptr);

    // a copy of the file the session came from, so fields the core does not use are kept
    ECSValue *field = nullptr;
    usz current = 0;
    SHUResult result = ECSIValue_Copy(retSession, info->file);
    result = result ? result : ECSValue_TableSetField(retSession, "format", &field);

    if (!result)
    {
        ECSValue_SetInteger(field, OPENECS_SESSION_FORMAT);
    }

    // the plugin directory is written resolved, because the session lives in another folder
    if (!result && info->pluginsDirectory != nullptr)
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
        ECSValue *copy = nullptr;

        if (keys != nullptr)
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
    SDL_assert(folders != nullptr);

    SESSION.presets = folders->presets == nullptr ? nullptr : SDL_strdup(folders->presets);
    SESSION.folder = folders->sessions == nullptr ? nullptr : SDL_strdup(folders->sessions);
    SESSION.state = folders->state == nullptr ? nullptr : SDL_strdup(folders->state);
    SESSION.origin = SDL_strdup(folders->origin);
    SESSION.canOpen = canOpen;

    if ((folders->presets != nullptr && SESSION.presets == nullptr) || (folders->sessions != nullptr && SESSION.folder == nullptr) || (folders->state != nullptr && SESSION.state == nullptr) || SESSION.origin == nullptr)
    {
        return SHUResult_ErrAllocation;
    }

    // keys run the same names as the Lua functions, which ask for the file when they get no path
    SHU_ReturnResult(ECSIServices_RegisterCore("ecs.session.save", (ECSFunction)ECSISession_SaveBound, "void()", "Save the session"));
    SHU_ReturnResult(ECSIServices_RegisterCore("ecs.session.open", (ECSFunction)ECSISession_OpenBound, "void()", "Open a session"));
    return ECSIServices_RegisterCore("ecs.layout.reset", (ECSFunction)ECSISession_ResetBound, "void()", "Reset the layout");
}

const char *ECSISession_GetNext(const char **retOption)
{
    if (retOption != nullptr)
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
    SDL_free(SESSION.origin);
    SDL_free(SESSION.next);
    SDL_zero(SESSION);
}

SHUResult ECSSession_Open(const char *path)
{
    if (!ECSISession_CanOpen())
    {
        return SHUResult_ErrPrivileges;
    }

    if (path == nullptr)
    {
        return ECSISession_ShowDialog(ECSDialogType_OpenFile, ECSISession_OpenChosen);
    }

    return ECSISession_OpenFile(path, false);
}

SHUResult ECSSession_OpenPreset(const char *nameOrPath)
{
    SDL_assert(nameOrPath != nullptr);

    if (!ECSISession_CanOpen())
    {
        return SHUResult_ErrPrivileges;
    }

    char *path = nullptr;
    SHU_ReturnResult(ECSISession_FindPreset(&path, nameOrPath, SESSION.presets));
    SHUResult result = ECSISession_OpenFile(path, true);
    SDL_free(path);
    return result;
}

SHUResult ECSSession_ListPresets(ECSValue *retList)
{
    SDL_assert(retList != nullptr);

    // the user's presets first, so they win over first-party presets of the same name
    ECSISessionFile *files = nullptr;
    char *firstParty = nullptr;
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
    SDL_assert(retList != nullptr);

    ECSISessionFile *files = nullptr;
    SHUResult result = ECSISession_FindFiles(&files, SESSION.folder);
    result = result ? result : ECSISession_FillList(retList, files, false);

    ECSISession_FreeFiles(&files);
    return result;
}

SHUResult ECSSession_Save(const char *path)
{
    SDL_assert(SESSION.info != nullptr);

    if (path == nullptr)
    {
        return ECSISession_ShowDialog(ECSDialogType_SaveFile, ECSISession_SaveChosen);
    }

    ECSValue *session = nullptr;
    SHU_ReturnResult(ECSValue_Create(&session));
    SHUResult result = ECSISession_Build(SESSION.info, session);
    result = result ? result : ECSILua_WriteData(path, session);

    ECSValue_Destroy(&session);
    return result;
}
