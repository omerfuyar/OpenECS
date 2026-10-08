#include "Session.h"

#include "Layout.h"
#include "Lua.h"
#include "Panels.h"

#include "SDL3/SDL.h"
#include "stb/stbSDL3.h"

#pragma region Source Only

static void ECSI_SessionAddPlugin(const char *key, const char *value, void *userData)
{
    (void)value;
    ECSI_PresetInfo *info = userData;
    char *plugin = SDL_strdup(key);

    if (plugin != NULL)
    {
        arrput(info->plugins, plugin);
    }
}

/// @brief Builds the layout node described by the current table: a split if it has "split", a group otherwise.
static SHUResult ECSI_SessionReadNode(ECSI_Node **retNode)
{
    if (ECSI_LuaDataHas("split"))
    {
        bool vertical = SDL_strcasecmp(ECSI_LuaDataGetText("split", "horizontal"), "vertical") == 0;
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
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Pop-out windows are not implemented yet; only the first window is used.");
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
    SHU_ReturnResult(ECSI_LuaDataOpen(path));

    retInfo->name = SDL_strdup(ECSI_LuaDataGetText("name", "preset"));
    const char *appName = "OpenECS";
    const char *appId = NULL;

    if (ECSI_LuaDataEnterField("app"))
    {
        appName = ECSI_LuaDataGetText("name", appName);
        appId = ECSI_LuaDataGetText("id", NULL);
        ECSI_LuaDataLeave();
    }

    retInfo->appName = SDL_strdup(appName);

    if (appId != NULL)
    {
        retInfo->appId = SDL_strdup(appId);
    }
    else if (retInfo->name != NULL)
    {
        SDL_asprintf(&retInfo->appId, "openecs.%s", retInfo->name);
    }

    const char *pluginsDirectory = ECSI_LuaDataGetText("plugins_dir", NULL);

    if (pluginsDirectory != NULL)
    {
        // relative to the preset's folder
        const char *slash = SDL_strrchr(path, '/');
        int folderLength = slash == NULL ? 0 : (int)(slash - path + 1);
        SDL_asprintf(&retInfo->pluginsDirectory, "%.*s%s/", folderLength, path, pluginsDirectory);
    }

    if (ECSI_LuaDataEnterField("depends"))
    {
        ECSI_LuaDataForEachText(ECSI_SessionAddPlugin, retInfo);
        ECSI_LuaDataLeave();
    }

    ECSI_LuaDataClose();

    if (retInfo->name == NULL || retInfo->appName == NULL || retInfo->appId == NULL || (pluginsDirectory != NULL && retInfo->pluginsDirectory == NULL))
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

    for (usz i = 0; i < arrlenu(info->plugins); i++)
    {
        SDL_free(info->plugins[i]);
    }

    arrfree(info->plugins);
    SDL_zerop(info);
}

SHUResult ECSI_SessionApply(const char *path)
{
    SDL_assert(path != NULL);

    SHU_ReturnResult(ECSI_LuaDataOpen(path));

    if (!ECSI_LuaDataEnterField("workspaces"))
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "'%s' has no workspaces.", path);
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
