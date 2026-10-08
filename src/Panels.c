#include "Panels.h"

#include "Events.h"
#include "Plugins.h"
#include "Values.h"

#include "SDL3/SDL.h"
#include "stb/stbSDL3.h"

#pragma region Source Only

static struct
{
    struct
    {
        char *key; // the type's own copy of its name
        ECSI_PanelType *value;
    } *types; // stb_ds hash map; panels point to the types, so each type is allocated on its own
    ECSPanel *closed; // stb_ds array
    u32 nextPanelId;
} PANELS = {0};

static ECSI_PanelType *ECSI_PanelTypeFind(const char *name)
{
    return shget(PANELS.types, name);
}

/// @brief Gives a panel pixels of the size of its rectangle, reusing them when the size did not change.
/// @return false if the pixels cannot be allocated.
static bool ECSI_PanelResizePixels(ECSPanel panel)
{
    int width = (int)SDL_max(1.0f, SDL_roundf(panel->width));
    int height = (int)SDL_max(1.0f, SDL_roundf(panel->height));

    if (panel->pixels != NULL && panel->pixels->w == width && panel->pixels->h == height)
    {
        return true;
    }

    SDL_DestroySurface(panel->pixels);
    panel->pixels = SDL_CreateSurface(width, height, SDL_PIXELFORMAT_ARGB8888);

    if (panel->texture != NULL)
    {
        SDL_DestroyTexture(panel->texture);
        panel->texture = NULL;
    }

    return panel->pixels != NULL;
}

static void ECSI_PanelDeliverEvent(void *target, const ECSEvent *event)
{
    ECSPanel panel = target;

    if (!panel->closed && panel->type != NULL && panel->type->desc.Event != NULL)
    {
        panel->type->desc.Event(panel->state, event);
    }
}

#pragma endregion Source Only

void ECSI_PanelsTerminate(void)
{
    ECSI_PanelsDestroyClosed();

    for (usz i = 0; i < shlenu(PANELS.types); i++)
    {
        ECSI_PanelType *type = PANELS.types[i].value;
        SDL_free(type->name);
        SDL_free(type->title);
        SDL_free(type);
    }

    shfree(PANELS.types);
    SDL_zero(PANELS);
}

SHUResult ECSI_PanelCreate(ECSPanel *retPanel, const char *typeName, const ECSValue *savedState, u32 stateVersion)
{
    SDL_assert(retPanel != NULL);
    SDL_assert(typeName != NULL);

    ECSI_PanelType *type = ECSI_PanelTypeFind(typeName);
    ECSPanel panel = SDL_calloc(1, sizeof(struct ECSI_Panel));

    if (panel == NULL)
    {
        return SHUResult_ErrAllocation;
    }

    panel->typeName = SDL_strdup(typeName);
    panel->title = SDL_strdup(type == NULL ? typeName : type->title);

    if (panel->typeName == NULL || panel->title == NULL || (savedState != NULL && ECSI_ValueCreate(&panel->savedState)))
    {
        ECSI_PanelDestroy(&panel);
        return SHUResult_ErrAllocation;
    }

    if (savedState != NULL)
    {
        SHU_ReturnResult(ECSI_ValueCopy(panel->savedState, savedState), ECSI_PanelDestroy(&panel););
    }

    panel->id = ++PANELS.nextPanelId;
    panel->stateVersion = stateVersion;
    panel->needsDraw = true;

    if (type == NULL)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Panel type '%s' is missing; showing a placeholder.", typeName);
    }
    else
    {
        // the type may set the title while it creates the panel, so the type is set first
        panel->type = type;

        if (type->desc.Create(panel, panel->savedState, stateVersion, &panel->state))
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Panel type '%s' failed to create a panel; showing a placeholder.", typeName);
            panel->type = NULL;
            panel->state = NULL;
        }
    }

    *retPanel = panel;
    return SHUResult_Ok;
}

void ECSI_PanelDestroy(ECSPanel *panel)
{
    SDL_assert(panel != NULL && *panel != NULL);

    ECSPanel target = *panel;
    ECSI_EventsStopTimersOf(target);

    if (target->type != NULL)
    {
        target->type->desc.Destroy(target->state);
    }

    if (target->texture != NULL)
    {
        SDL_DestroyTexture(target->texture);
    }

    SDL_DestroySurface(target->pixels);
    ECSI_ValueDestroy(&target->savedState);
    SDL_free(target->typeName);
    SDL_free(target->title);
    SDL_free(target);

    *panel = NULL;
}

void ECSI_PanelClose(ECSPanel *panel)
{
    SDL_assert(panel != NULL && *panel != NULL);
    SDL_assert(!(*panel)->closed);

    ECSI_EventsStopTimersOf(*panel);
    (*panel)->closed = true;
    arrput(PANELS.closed, *panel);
    *panel = NULL;
}

void ECSI_PanelsDestroyClosed(void)
{
    for (usz i = 0; i < arrlenu(PANELS.closed); i++)
    {
        ECSI_PanelDestroy(&PANELS.closed[i]);
    }

    arrfree(PANELS.closed);
}

void ECSI_PanelSetRect(ECSPanel panel, f32 x, f32 y, f32 width, f32 height)
{
    SDL_assert(panel != NULL);

    if (SDL_roundf(panel->width) != SDL_roundf(width) || SDL_roundf(panel->height) != SDL_roundf(height))
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
    SDL_assert(panel != NULL);

    return panel->needsDraw || (panel->type != NULL && panel->type->desc.continuous);
}

void ECSI_PanelRender(ECSPanel panel, SDL_Renderer *renderer, u64 nowTicks)
{
    SDL_assert(panel != NULL);
    SDL_assert(renderer != NULL);

    // placeholders have no pixels; the layout draws their text
    if (panel->type == NULL || panel->type->desc.Draw == NULL || panel->width < 1.0f || panel->height < 1.0f)
    {
        return;
    }

    // a texture belongs to one renderer, and the panel may have moved to another OS window
    if (panel->texture != NULL && SDL_GetRendererFromTexture(panel->texture) != renderer)
    {
        SDL_DestroyTexture(panel->texture);
        panel->texture = NULL;
    }

    if (ECSI_PanelWantsFrame(panel) || panel->texture == NULL)
    {
        if (!ECSI_PanelResizePixels(panel))
        {
            return;
        }

        SDL_Surface *pixels = panel->pixels;

        if (panel->texture == NULL)
        {
            panel->texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, pixels->w, pixels->h);

            if (panel->texture == NULL)
            {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cannot create a %dx%d texture: %s", pixels->w, pixels->h, SDL_GetError());
                return;
            }

            // panels are opaque
            SDL_SetTextureBlendMode(panel->texture, SDL_BLENDMODE_NONE);
        }

        ECSSurface surface = {
            .type = ECSSurfaceType_Pixels,
            .width = pixels->w,
            .height = pixels->h,
            .scale = 1.0f,
            .pixels = cs(pixels->pixels, (usz)pixels->pitch * (usz)pixels->h),
            .pitch = pixels->pitch,
        };

        f64 seconds = panel->lastDrawTicks == 0 ? 0.0 : (f64)(nowTicks - panel->lastDrawTicks) / 1e9;
        panel->lastDrawTicks = nowTicks;
        panel->needsDraw = false;

        panel->type->desc.Draw(panel->state, &surface, seconds);
        SDL_UpdateTexture(panel->texture, NULL, pixels->pixels, pixels->pitch);
    }

    SDL_FRect rect = {panel->x, panel->y, panel->width, panel->height};
    SDL_RenderTexture(renderer, panel->texture, NULL, &rect);
}

void ECSI_PanelPostEvent(ECSPanel panel, const ECSEvent *event)
{
    SDL_assert(panel != NULL);
    SDL_assert(event != NULL);

    ECSI_EventsPost(ECSI_PanelDeliverEvent, panel, event);
}

SHUResult ECSPanelType_Register(ECSPlugin plugin, const ECSPanelTypeDesc *desc)
{
    SDL_assert(plugin != NULL);
    SDL_assert(desc != NULL);

    const char *pluginName = ECSI_PluginGetName(plugin);

    if (desc->name == NULL || desc->Create == NULL || desc->Destroy == NULL)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' registered an incomplete panel type.", pluginName);
        return SHUResult_ErrBadData;
    }

    usz prefixLength = SDL_strlen(pluginName);

    if (SDL_strncmp(desc->name, pluginName, prefixLength) != 0 || desc->name[prefixLength] != '.')
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Panel type '%s' must start with '%s.'.", desc->name, pluginName);
        return SHUResult_ErrBadData;
    }

    if (desc->surface != ECSSurfaceType_Pixels)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Panel type '%s' asks for a GPU surface, which is not implemented yet.", desc->name);
        return SHUResult_ErrBadData;
    }

    if (ECSI_PanelTypeFind(desc->name) != NULL)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Panel type '%s' is already registered.", desc->name);
        return SHUResult_ErrBadData;
    }

    ECSI_PanelType *type = SDL_malloc(sizeof(ECSI_PanelType));
    char *name = SDL_strdup(desc->name);
    char *title = SDL_strdup(desc->title == NULL ? desc->name : desc->title);

    if (type == NULL || name == NULL || title == NULL)
    {
        SDL_free(type);
        SDL_free(name);
        SDL_free(title);
        return SHUResult_ErrAllocation;
    }

    *type = (ECSI_PanelType){.desc = *desc, .name = name, .title = title, .plugin = plugin};
    type->desc.name = name;
    type->desc.title = title;
    shput(PANELS.types, name, type);

    return SHUResult_Ok;
}

SHUResult ECSPanel_StartTimer(ECSPanel panel, ECSTimer *retTimer, f64 seconds, bool repeat, ECSTimerFunction function, void *data)
{
    SDL_assert(panel != NULL);
    SDL_assert(panel->type != NULL); // placeholders run no code

    return ECSI_EventsStartTimer(panel->type->plugin, panel, retTimer, seconds, repeat, function, data);
}

void ECSPanel_Redraw(ECSPanel panel)
{
    SDL_assert(panel != NULL);

    panel->needsDraw = true;
}

const char *ECSPanel_GetTitle(ECSPanel panel)
{
    SDL_assert(panel != NULL);

    return panel->title;
}

void ECSPanel_SetTitle(ECSPanel panel, const char *title)
{
    SDL_assert(panel != NULL);
    SDL_assert(title != NULL);

    char *copy = SDL_strdup(title);

    if (copy != NULL)
    {
        SDL_free(panel->title);
        panel->title = copy;
    }
}
