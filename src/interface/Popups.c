#include "interface/Popups.h"

#include "runtime/Events.h"
#include "runtime/Services.h"

#include "SDL3/SDL.h"
#include "stb/stbSDL3.h"

#pragma region Source Only

static struct
{
    ECSPopup *open;   // stb_ds array, the oldest first
    ECSPopup *closed; // stb_ds array of the popups waiting to be freed
    ECSIPopupReleaseFunction Release;
} POPUPS = {0};

/// @brief Frees a popup and what the window shows it with.
static void ECSIPopup_Free(ECSPopup popup)
{
    if (POPUPS.Release != NULL)
    {
        POPUPS.Release(popup);
    }

    SDL_DestroySurface(popup->pixels);
    SDL_free(popup);
}

static void ECSIPopup_DeliverEvent(void *target, const ECSPanelEvent *event)
{
    ECSPopup popup = target;

    if (!popup->closed && popup->desc.Event != NULL)
    {
        popup->desc.Event(popup->desc.data, event);
    }
}

/// @brief Runs a closed popup's Closed function; then it can be freed.
static void ECSIPopup_DeliverClosed(void *target, const ECSPanelEvent *event)
{
    (void)event;
    ECSPopup popup = target;

    if (popup->desc.Closed != NULL)
    {
        popup->desc.Closed(popup->desc.data);
    }

    popup->released = true;
}

#pragma endregion Source Only

void ECSIPopups_SetRelease(ECSIPopupReleaseFunction function)
{
    POPUPS.Release = function;
}

void ECSIPopups_Terminate(void)
{
    ECSPanelEvent event = {0};

    // the Closed functions run now, while the panels and plugins live
    while (arrlenu(POPUPS.open) > 0)
    {
        ECSPopup popup = arrpop(POPUPS.open);
        popup->closed = true;
        arrput(POPUPS.closed, popup);
    }

    for (usz i = 0; i < arrlenu(POPUPS.closed); i++)
    {
        if (!POPUPS.closed[i]->released)
        {
            ECSIPopup_DeliverClosed(POPUPS.closed[i], &event);
        }

        ECSIPopup_Free(POPUPS.closed[i]);
    }

    arrfree(POPUPS.open);
    arrfree(POPUPS.closed);
    SDL_zero(POPUPS);
}

ECSPopup *ECSIPopups_GetOpen(usz *retCount)
{
    SDL_assert(retCount != NULL);

    *retCount = arrlenu(POPUPS.open);
    return POPUPS.open;
}

ECSPopup ECSIPopups_GetMenu(void)
{
    for (usz i = arrlenu(POPUPS.open); i > 0; i--)
    {
        if (POPUPS.open[i - 1]->desc.kind == ECSPopupKind_Menu)
        {
            return POPUPS.open[i - 1];
        }
    }

    return NULL;
}

void ECSIPopups_CloseAll(void)
{
    while (arrlenu(POPUPS.open) > 0)
    {
        ECSPopup_Close(arrlast(POPUPS.open));
    }
}

void ECSIPopups_CloseHidden(void)
{
    for (usz i = arrlenu(POPUPS.open); i > 0; i--)
    {
        ECSPanel panel = POPUPS.open[i - 1]->panel;

        if (panel->closed || panel->fault != NULL || !panel->visible)
        {
            ECSPopup_Close(POPUPS.open[i - 1]);
        }
    }
}

void ECSIPopups_DestroyClosed(void)
{
    for (usz i = 0; i < arrlenu(POPUPS.closed);)
    {
        if (POPUPS.closed[i]->released)
        {
            ECSIPopup_Free(POPUPS.closed[i]);
            arrdel(POPUPS.closed, i);
            continue;
        }

        i++;
    }
}

bool ECSIPopups_WantsFrame(void)
{
    for (usz i = 0; i < arrlenu(POPUPS.open); i++)
    {
        if (POPUPS.open[i]->needsDraw)
        {
            return true;
        }
    }

    return false;
}

bool ECSIPopup_Draw(ECSPopup popup)
{
    SDL_assert(popup != NULL);

    i32 width = (i32)SDL_max(1.0f, SDL_roundf(popup->desc.width));
    i32 height = (i32)SDL_max(1.0f, SDL_roundf(popup->desc.height));

    if (!popup->needsDraw && popup->pixels != NULL && popup->pixels->w == width && popup->pixels->h == height)
    {
        return false;
    }

    if (popup->pixels == NULL || popup->pixels->w != width || popup->pixels->h != height)
    {
        SDL_DestroySurface(popup->pixels);
        popup->pixels = SDL_CreateSurface(width, height, SDL_PIXELFORMAT_ARGB8888);

        if (popup->pixels == NULL)
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cannot make the %dx%d pixels of a popup: %s", width, height, SDL_GetError());
            return false;
        }
    }

    // a popup draws every pixel; what it leaves is transparent
    SDL_ClearSurface(popup->pixels, 0.0f, 0.0f, 0.0f, 0.0f);
    ECSSurface surface = {
        .type = ECSSurfaceType_Pixels,
        .width = width,
        .height = height,
        .scale = 1.0f,
        .pixels = cs(popup->pixels->pixels, (usz)popup->pixels->pitch * (usz)height),
        .pitch = popup->pixels->pitch,
    };

    popup->needsDraw = false;
    popup->desc.Draw(popup->desc.data, &surface);

    // the surface is valid only during Draw, so a Lua handle of it is too
    ECSIServices_ForgetHandle(&surface);
    return true;
}

void ECSIPopup_PostEvent(ECSPopup popup, const ECSPanelEvent *event)
{
    SDL_assert(popup != NULL);
    SDL_assert(event != NULL);

    ECSIEvents_Post(ECSIPopup_DeliverEvent, popup, event);
}

SHUResult ECSPopup_Open(ECSPanel panel, const ECSPopupDesc *desc, ECSPopup *retPopup)
{
    SDL_assert(panel != NULL);
    SDL_assert(desc != NULL);
    SDL_assert(retPopup != NULL);

    *retPopup = NULL;

    if (desc->Draw == NULL || desc->width <= 0.0f || desc->height <= 0.0f)
    {
        return SHUResult_ErrBadData;
    }

    if (panel->closed || !panel->visible)
    {
        return SHUResult_ErrNotFound;
    }

    ECSPopup popup = SDL_calloc(1, sizeof(struct ECSIPopup));

    if (popup == NULL)
    {
        return SHUResult_ErrAllocation;
    }

    popup->panel = panel;
    popup->desc = *desc;
    popup->needsDraw = true;
    arrput(POPUPS.open, popup);
    ECSILayout_RequestFrame();
    *retPopup = popup;
    return SHUResult_Ok;
}

void ECSPopup_Close(ECSPopup popup)
{
    SDL_assert(popup != NULL);

    if (popup->closed)
    {
        return;
    }

    for (usz i = 0; i < arrlenu(POPUPS.open); i++)
    {
        if (POPUPS.open[i] == popup)
        {
            arrdel(POPUPS.open, i);
            break;
        }
    }

    popup->closed = true;
    arrput(POPUPS.closed, popup);
    ECSILayout_RequestFrame();
    ECSPanelEvent event = {0};
    ECSIEvents_Post(ECSIPopup_DeliverClosed, popup, &event);
}

void ECSPopup_Redraw(ECSPopup popup)
{
    SDL_assert(popup != NULL);

    popup->needsDraw = true;
}

void ECSPopup_SetSize(ECSPopup popup, f32 width, f32 height)
{
    SDL_assert(popup != NULL);

    popup->desc.width = width > 0.0f ? width : popup->desc.width;
    popup->desc.height = height > 0.0f ? height : popup->desc.height;
    popup->needsDraw = true;
}
