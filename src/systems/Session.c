#include "systems/Session.h"

#include "tools/Lua.h"
#include "tools/Platform.h"
#include "systems/Layout.h"
#include "systems/Panels.h"

#pragma region Source Only

static void ECSI_SessionAddPlugin(const char *key, const char *value, void *userData)
{
    (void)value;
    ECSI_PresetInfo *info = userData;

    if (info->pluginCount < OPENECS_MAX_PLUGINS)
    {
        ECSI_TextCopy(cs(info->plugins[info->pluginCount], OPENECS_NAME_CAPACITY), key);
        info->pluginCount++;
    }
}

/// @brief Builds the layout node described by the current table: a split if it has "split", a group otherwise.
static SHUResult ECSI_SessionReadNode(ECSI_Node **retNode)
{
    if (ECSI_LuaDataHas("split"))
    {
        bool vertical = ECSI_TextEqualsIgnoreCase(ECSI_LuaDataGetText("split", "horizontal"), "vertical");
        SHU_ReturnResult(ECSI_LayoutSplitCreate(retNode, vertical));

        for (usz i = 1; i <= ECSI_LuaDataCount(); i++)
        {
            if (!ECSI_LuaDataEnterIndex(i))
            {
                continue;
            }

            ECSI_Node *child = NULL;
            SHUResult result = ECSI_SessionReadNode(&child);

            if (!result)
            {
                result = ECSI_LayoutSplitAdd(*retNode, child, (f32)ECSI_LuaDataGetNumber("size", 0.0), (f32)ECSI_LuaDataGetNumber("share", 1.0));

                if (result)
                {
                    ECSI_LayoutNodeDestroy(&child);
                }
            }

            ECSI_LuaDataLeave();
            SHU_ReturnResult(result, ECSI_LayoutNodeDestroy(retNode););
        }

        return SHUResult_Ok;
    }

    SHU_ReturnResult(ECSI_LayoutGroupCreate(retNode));

    if (!ECSI_LuaDataEnterField("panels"))
    {
        return SHUResult_Ok;
    }

    for (usz i = 1; i <= ECSI_LuaDataCount(); i++)
    {
        if (!ECSI_LuaDataEnterIndex(i))
        {
            continue;
        }

        ECSPanel panel = NULL;
        SHUResult result = ECSI_PanelCreate(&panel, ECSI_LuaDataGetText("type", ""));

        if (!result)
        {
            result = ECSI_LayoutGroupAdd(*retNode, panel);

            if (result)
            {
                ECSI_PanelDestroy(&panel);
            }
        }

        ECSI_LuaDataLeave();
        SHU_ReturnResult(result, ECSI_LuaDataLeave(); ECSI_LayoutNodeDestroy(retNode););
    }

    ECSI_LuaDataLeave();
    return SHUResult_Ok;
}

/// @brief Builds the workspace described by the current table.
static SHUResult ECSI_SessionReadWorkspace(void)
{
    ECSI_Node *tree = NULL;

    if (ECSI_LuaDataEnterField("windows"))
    {
        if (ECSI_LuaDataCount() > 1)
        {
            SHU_LogWarning("Pop-out windows are not implemented yet; only the first window is used.");
        }

        SHUResult result = SHUResult_Ok;

        if (ECSI_LuaDataEnterIndex(1))
        {
            result = ECSI_SessionReadNode(&tree);
            ECSI_LuaDataLeave();
        }

        ECSI_LuaDataLeave();
        SHU_ReturnResult(result);
    }

    SHU_ReturnResult(ECSI_LayoutWorkspaceAdd(ECSI_LuaDataGetText("name", "workspace"), tree),
                     if (tree != NULL) { ECSI_LayoutNodeDestroy(&tree); });
    return SHUResult_Ok;
}

#pragma endregion Source Only

void ECSI_SessionFindPreset(SHUSlice retPath, const char *nameOrPath)
{
    SHU_AssertNullPointer(retPath.data);
    SHU_AssertNullPointer(nameOrPath);

    if (strchr(nameOrPath, '/') != NULL)
    {
        ECSI_TextCopy(retPath, nameOrPath);
    }
    else
    {
        snprintf(retPath.data, retPath.size, "%spresets/%s.lua", ECSI_PlatformGetBaseDirectory(), nameOrPath);
    }
}

SHUResult ECSI_SessionReadInfo(const char *path, ECSI_PresetInfo *retInfo)
{
    SHU_AssertNullPointer(path);
    SHU_AssertNullPointer(retInfo);

    *retInfo = (ECSI_PresetInfo){0};
    SHU_ReturnResult(ECSI_LuaDataOpen(path));

    ECSI_TextCopy(cs(retInfo->name, sizeof(retInfo->name)), ECSI_LuaDataGetText("name", "preset"));
    ECSI_TextCopy(cs(retInfo->appName, sizeof(retInfo->appName)), "OpenECS");

    if (snprintf(retInfo->appId, sizeof(retInfo->appId), "openecs.%s", retInfo->name) >= (int)sizeof(retInfo->appId))
    {
        SHU_LogWarning("Preset name '%s' is too long for an app id; the id is cut.", retInfo->name);
    }

    if (ECSI_LuaDataEnterField("app"))
    {
        ECSI_TextCopy(cs(retInfo->appName, sizeof(retInfo->appName)), ECSI_LuaDataGetText("name", retInfo->appName));
        ECSI_TextCopy(cs(retInfo->appId, sizeof(retInfo->appId)), ECSI_LuaDataGetText("id", retInfo->appId));
        ECSI_LuaDataLeave();
    }

    const char *pluginsDirectory = ECSI_LuaDataGetText("plugins_dir", "");

    if (pluginsDirectory[0] != '\0')
    {
        // relative to the preset's folder
        const char *slash = strrchr(path, '/');
        int folderLength = slash == NULL ? 0 : (int)(slash - path + 1);
        snprintf(retInfo->pluginsDirectory, sizeof(retInfo->pluginsDirectory), "%.*s%s/", folderLength, path, pluginsDirectory);
    }

    if (ECSI_LuaDataEnterField("depends"))
    {
        ECSI_LuaDataForEachText(ECSI_SessionAddPlugin, retInfo);
        ECSI_LuaDataLeave();
    }

    ECSI_LuaDataClose();
    return SHUResult_Ok;
}

SHUResult ECSI_SessionApply(const char *path)
{
    SHU_AssertNullPointer(path);

    SHU_ReturnResult(ECSI_LuaDataOpen(path));

    if (!ECSI_LuaDataEnterField("workspaces"))
    {
        SHU_LogWarning("'%s' has no workspaces.", path);
        ECSI_LuaDataClose();
        return SHUResult_ErrBadData;
    }

    SHUResult result = SHUResult_Ok;

    for (usz i = 1; i <= ECSI_LuaDataCount() && !result; i++)
    {
        if (ECSI_LuaDataEnterIndex(i))
        {
            result = ECSI_SessionReadWorkspace();
            ECSI_LuaDataLeave();
        }
    }

    ECSI_LuaDataClose();
    return result;
}
