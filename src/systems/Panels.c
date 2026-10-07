#include "systems/Panels.h"

#include <math.h>

#pragma region Source Only

static struct
{
    ECSI_PanelType types[OPENECS_MAX_PANEL_TYPES];
    usz typeCount;
    u32 nextPanelId;
} PANELS = {0};

/// @brief Gives a panel pixels of the size of its rectangle, reusing them when the size did not change.
/// @return false if the pixels cannot be allocated.
static bool ECSI_PanelResizePixels(ECSPanel panel)
{
    i32 width = (i32)fmaxf(1.0f, roundf(panel->width));
    i32 height = (i32)fmaxf(1.0f, roundf(panel->height));

    if (panel->surface.pixels.data != NULL && panel->surface.width == width && panel->surface.height == height)
    {
        return true;
    }

    free(panel->surface.pixels.data);
    ECSI_RenderTextureDestroy(&panel->texture);

    usz size = (usz)width * (usz)height * 4;
    void *pixels = calloc(1, size);

    if (pixels == NULL)
    {
        panel->surface = (ECSSurface){0};
        return false;
    }

    panel->surface = (ECSSurface){
        .kind = ECSSurfaceKind_Pixels,
        .width = width,
        .height = height,
        .scale = 1.0f,
        .pixels = cs(pixels, size),
        .pitch = width * 4,
    };

    return true;
}

#pragma endregion Source Only

SHUResult ECSI_PanelTypeRegister(ECSI_Plugin *plugin, const char *pluginName, const ECSPanelTypeDesc *desc)
{
    SHU_AssertNullPointer(pluginName);
    SHU_AssertNullPointer(desc);

    if (desc->structSize < sizeof(ECSPanelTypeDesc) || desc->name == NULL || desc->Create == NULL || desc->Destroy == NULL)
    {
        SHU_LogWarning("Plugin '%s' registered an incomplete panel type.", pluginName);
        return SHUResult_ErrBadData;
    }

    usz prefixLength = strlen(pluginName);

    if (strncmp(desc->name, pluginName, prefixLength) != 0 || desc->name[prefixLength] != '.')
    {
        SHU_LogWarning("Panel type '%s' must start with '%s.'.", desc->name, pluginName);
        return SHUResult_ErrBadData;
    }

    if (desc->surface != ECSSurfaceKind_Pixels)
    {
        SHU_LogWarning("Panel type '%s' asks for a GPU surface, which is not implemented yet.", desc->name);
        return SHUResult_ErrBadData;
    }

    if (ECSI_PanelTypeFind(desc->name) != NULL)
    {
        SHU_LogWarning("Panel type '%s' is already registered.", desc->name);
        return SHUResult_ErrBadData;
    }

    if (PANELS.typeCount == OPENECS_MAX_PANEL_TYPES)
    {
        return SHUResult_ErrOverflow;
    }

    ECSI_PanelType *type = &PANELS.types[PANELS.typeCount++];
    type->desc = *desc;
    type->plugin = plugin;

    ECSI_TextCopy(cs(type->name, sizeof(type->name)), desc->name);
    ECSI_TextCopy(cs(type->title, sizeof(type->title)), desc->title == NULL ? desc->name : desc->title);
    type->desc.name = type->name;
    type->desc.title = type->title;

    return SHUResult_Ok;
}

ECSI_PanelType *ECSI_PanelTypeFind(const char *name)
{
    SHU_AssertNullPointer(name);

    for (usz i = 0; i < PANELS.typeCount; i++)
    {
        if (strcmp(PANELS.types[i].name, name) == 0)
        {
            return &PANELS.types[i];
        }
    }

    return NULL;
}

SHUResult ECSI_PanelCreate(ECSPanel *retPanel, const char *typeName)
{
    SHU_AssertNullPointer(retPanel);
    SHU_AssertNullPointer(typeName);

    ECSPanel panel = calloc(1, sizeof(struct ECSI_Panel));

    if (panel == NULL)
    {
        return SHUResult_ErrAllocation;
    }

    panel->id = ++PANELS.nextPanelId;
    panel->needsDraw = true;
    ECSI_TextCopy(cs(panel->typeName, sizeof(panel->typeName)), typeName);

    ECSI_PanelType *type = ECSI_PanelTypeFind(typeName);

    if (type == NULL)
    {
        SHU_LogWarning("Panel type '%s' is missing; showing a placeholder.", typeName);
        ECSI_TextCopy(cs(panel->title, sizeof(panel->title)), typeName);
    }
    else
    {
        ECSI_TextCopy(cs(panel->title, sizeof(panel->title)), type->title);

        // the type may set the title while it creates the panel, so the type is set first
        panel->type = type;

        if (type->desc.Create(panel, NULL, 0, &panel->state))
        {
            SHU_LogWarning("Panel type '%s' failed to create a panel; showing a placeholder.", typeName);
            panel->type = NULL;
            panel->state = NULL;
        }
    }

    *retPanel = panel;
    return SHUResult_Ok;
}

void ECSI_PanelDestroy(ECSPanel *panel)
{
    SHU_AssertNullPointer(panel);
    SHU_AssertNullPointer(*panel);

    ECSPanel target = *panel;

    if (target->type != NULL)
    {
        target->type->desc.Destroy(target->state);
    }

    ECSI_RenderTextureDestroy(&target->texture);
    free(target->surface.pixels.data);
    free(target);

    *panel = NULL;
}

void ECSI_PanelSetRect(ECSPanel panel, f32 x, f32 y, f32 width, f32 height)
{
    SHU_AssertNullPointer(panel);

    if (roundf(panel->width) != roundf(width) || roundf(panel->height) != roundf(height))
    {
        panel->needsDraw = true;
    }

    panel->x = x;
    panel->y = y;
    panel->width = width;
    panel->height = height;
}

bool ECSI_PanelWantsFrame(ECSPanel panel)
{
    SHU_AssertNullPointer(panel);

    return panel->needsDraw || (panel->type != NULL && panel->type->desc.continuous);
}

void ECSI_PanelRender(ECSPanel panel, ECSI_RenderWindow *renderWindow, u64 nowTicks)
{
    SHU_AssertNullPointer(panel);
    SHU_AssertNullPointer(renderWindow);

    // placeholders have no pixels; the layout draws their text
    if (panel->type == NULL || panel->type->desc.Draw == NULL || panel->width < 1.0f || panel->height < 1.0f)
    {
        return;
    }

    if (panel->textureWindow != renderWindow)
    {
        ECSI_RenderTextureDestroy(&panel->texture);
        panel->textureWindow = renderWindow;
    }

    if (ECSI_PanelWantsFrame(panel) || panel->texture == NULL)
    {
        if (!ECSI_PanelResizePixels(panel))
        {
            return;
        }

        if (panel->texture == NULL && ECSI_RenderTextureCreate(&panel->texture, renderWindow, panel->surface.width, panel->surface.height))
        {
            return;
        }

        f64 seconds = panel->lastDrawTicks == 0 ? 0.0 : (f64)(nowTicks - panel->lastDrawTicks) / 1e9;
        panel->lastDrawTicks = nowTicks;
        panel->needsDraw = false;

        panel->type->desc.Draw(panel->state, &panel->surface, seconds);
        ECSI_RenderTextureUpdate(panel->texture, panel->surface.pixels, panel->surface.pitch);
    }

    ECSI_RenderWindowDrawTexture(renderWindow, panel->texture, panel->x, panel->y, panel->width, panel->height);
}

void ECSI_PanelSendEvent(ECSPanel panel, const ECSEvent *event)
{
    SHU_AssertNullPointer(panel);
    SHU_AssertNullPointer(event);

    if (panel->type != NULL && panel->type->desc.Event != NULL)
    {
        panel->type->desc.Event(panel->state, event);
    }
}

void ECSI_PanelRedraw(ECSPanel panel)
{
    SHU_AssertNullPointer(panel);

    panel->needsDraw = true;
}

const char *ECSI_PanelGetTitle(ECSPanel panel)
{
    SHU_AssertNullPointer(panel);

    return panel->title;
}

void ECSI_PanelSetTitle(ECSPanel panel, const char *title)
{
    SHU_AssertNullPointer(panel);

    ECSI_TextCopy(cs(panel->title, sizeof(panel->title)), title);
}
