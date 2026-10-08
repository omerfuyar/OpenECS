#include "interface/Panels.h"

#include "base/Values.h"
#include "runtime/Events.h"
#include "runtime/Plugins.h"

#include "SDL3/SDL.h"
#include "stb/stbSDL3.h"

#pragma region Source Only

static struct
{
    struct
    {
        char *key; // the type's own copy of its name
        ECSI_PanelType *value;
    } *types;         // stb_ds hash map; panels point to the types, so each type is allocated on its own
    ECSPanel *closed; // stb_ds array
    u32 nextPanelId;
} PANELS = {0};

static void ECSI_PanelTypeFree(ECSI_PanelType *type)
{
    for (usz i = 0; i < arrlenu(type->menuEntries); i++)
    {
        SDL_free(type->menuEntries[i]);
    }

    arrfree(type->menuEntries);
    SDL_free(type->name);
    SDL_free(type->title);
    SDL_free(type);
}

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

static void ECSI_PanelDeliverEvent(void *target, const ECSPanelEvent *event)
{
    ECSPanel panel = target;

    if (!panel->closed && panel->fault == NULL && panel->type != NULL && panel->type->desc.Event != NULL)
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
        ECSI_PanelTypeFree(PANELS.types[i].value);
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

    if (panel->typeName == NULL || panel->title == NULL || (savedState != NULL && ECSValue_Create(&panel->savedState)))
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
            // the panel has no state to destroy, so it becomes a placeholder that keeps its saved state
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Panel type '%s' failed to create a panel; showing a placeholder.", typeName);
            ECSI_PanelFault(panel, "The panel type failed to create the panel.");
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
    ECSValue_Destroy(&target->savedState);
    SDL_free(target->fault);
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
    ECSI_PanelEmit("ecs.panel_closed", *panel);
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

void ECSI_PanelSetId(ECSPanel panel, u32 id)
{
    SDL_assert(panel != NULL);

    if (id != 0)
    {
        panel->id = id;
        PANELS.nextPanelId = SDL_max(PANELS.nextPanelId, id);
    }
}

SHUResult ECSI_PanelSave(ECSPanel panel, ECSValue *retPanel)
{
    SDL_assert(panel != NULL);
    SDL_assert(retPanel != NULL);

    ECSValue *field = NULL;
    ECSValue_SetTable(retPanel);
    SHU_ReturnResult(ECSValue_TableSetField(retPanel, "id", &field));
    ECSValue_SetInteger(field, panel->id);
    SHU_ReturnResult(ECSValue_TableSetField(retPanel, "type", &field));
    SHU_ReturnResult(ECSValue_SetString(field, panel->typeName));

    const ECSValue *state = panel->savedState;
    u32 version = panel->stateVersion;
    ECSValue *saved = NULL;

    if (panel->fault == NULL && panel->type != NULL && panel->type->desc.SaveState != NULL)
    {
        SHU_ReturnResult(ECSValue_Create(&saved));

        if (panel->type->desc.SaveState(panel->state, saved))
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Panel type '%s' failed to save a panel's state; its last saved state is kept.", panel->typeName);
        }
        else
        {
            // the panel keeps it as its last saved state, which a restart after a fault uses
            ECSValue_Destroy(&panel->savedState);
            panel->savedState = saved;
            panel->stateVersion = panel->type->desc.stateVersion;
            saved = NULL;
            state = panel->savedState;
            version = panel->stateVersion;
        }
    }

    SHUResult result = SHUResult_Ok;

    if (state != NULL)
    {
        result = ECSValue_TableSetField(retPanel, "state_version", &field);

        if (!result)
        {
            ECSValue_SetInteger(field, version);
            result = ECSValue_TableSetField(retPanel, "state", &field);
        }

        result = result ? result : ECSI_ValueCopy(field, state);
    }

    ECSValue_Destroy(&saved);
    return result;
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

    return panel->needsDraw || (panel->fault == NULL && panel->type != NULL && panel->type->desc.continuous);
}

void ECSI_PanelDraw(ECSPanel panel, SDL_Renderer *renderer, u64 nowTicks)
{
    SDL_assert(panel != NULL);
    SDL_assert(renderer != NULL);

    // placeholders and faulted panels have no pixels; the layout draws their text
    if (panel->type == NULL || panel->fault != NULL || panel->type->desc.Draw == NULL || panel->width < 1.0f || panel->height < 1.0f)
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
}

void ECSI_PanelShow(ECSPanel panel, SDL_Renderer *renderer)
{
    SDL_assert(panel != NULL);
    SDL_assert(renderer != NULL);

    if (panel->texture != NULL && panel->fault == NULL)
    {
        SDL_FRect rect = {panel->x, panel->y, panel->width, panel->height};
        SDL_RenderTexture(renderer, panel->texture, NULL, &rect);
    }
}

void ECSI_PanelSetVisible(ECSPanel panel, bool visible)
{
    SDL_assert(panel != NULL);

    if (visible != panel->visible)
    {
        panel->visible = visible;
        ECSPanelEvent event = {.type = visible ? ECSPanelEventType_Shown : ECSPanelEventType_Hidden, .size = {panel->width, panel->height}};
        ECSI_PanelPostEvent(panel, &event);
    }
    else if (visible && (panel->width != panel->toldWidth || panel->height != panel->toldHeight))
    {
        ECSPanelEvent event = {.type = ECSPanelEventType_Resized, .size = {panel->width, panel->height}};
        ECSI_PanelPostEvent(panel, &event);
    }

    if (visible)
    {
        panel->toldWidth = panel->width;
        panel->toldHeight = panel->height;
    }
}

void ECSI_PanelEmit(const char *name, ECSPanel panel)
{
    SDL_assert(name != NULL);
    SDL_assert(panel != NULL);

    ECSValue *value = NULL;
    ECSValue *field = NULL;

    if (ECSValue_Create(&value) || ECSValue_TableSetField(value, "panel", &field))
    {
        ECSValue_Destroy(&value);
        return;
    }

    ECSValue_SetInteger(field, panel->id);

    if (ECSValue_TableSetField(value, "type", &field) == SHUResult_Ok && ECSValue_SetString(field, panel->typeName) == SHUResult_Ok)
    {
        ECSI_EventsEmitCore(name, value);
    }

    ECSValue_Destroy(&value);
}

void ECSI_PanelPostEvent(ECSPanel panel, const ECSPanelEvent *event)
{
    SDL_assert(panel != NULL);
    SDL_assert(event != NULL);

    ECSI_EventsPost(ECSI_PanelDeliverEvent, panel, event);
}

void ECSI_PanelsRemovePlugin(ECSPlugin plugin)
{
    SDL_assert(plugin != NULL);

    // backwards, because shdel moves the last type into the hole
    for (usz i = shlenu(PANELS.types); i > 0; i--)
    {
        ECSI_PanelType *type = PANELS.types[i - 1].value;

        if (type->plugin == plugin)
        {
            (void)shdel(PANELS.types, type->name);
            ECSI_PanelTypeFree(type);
        }
    }
}

bool ECSI_PanelsConfirmClose(const ECSPanel *panels, usz count, bool quitting)
{
    SDL_assert(panels != NULL || count == 0);

    char *list = SDL_strdup("");
    usz unsaved = 0;

    for (usz i = 0; i < count && list != NULL; i++)
    {
        if (panels[i]->unsaved)
        {
            char *next = NULL;
            SDL_asprintf(&next, "%s\n- %s", list, panels[i]->title);
            SDL_free(list);
            list = next;
            unsaved++;
        }
    }

    if (unsaved == 0)
    {
        SDL_free(list);
        return true;
    }

    enum
    {
        ECSI_ANSWER_SAVE,
        ECSI_ANSWER_DISCARD,
        ECSI_ANSWER_CANCEL,
    };

    const SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, ECSI_ANSWER_SAVE, "Save"},
        {0, ECSI_ANSWER_DISCARD, "Discard"},
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, ECSI_ANSWER_CANCEL, "Cancel"},
    };

    char *message = NULL;
    SDL_asprintf(&message, "%s unsaved work:%s", unsaved == 1 ? "This panel has" : "These panels have", list == NULL ? "" : list);
    SDL_free(list);

    const SDL_MessageBoxData data = {
        .flags = SDL_MESSAGEBOX_WARNING,
        .title = "Unsaved work",
        .message = message == NULL ? "Some panels have unsaved work." : message,
        .numbuttons = SDL_arraysize(buttons),
        .buttons = buttons,
    };

    int answer = ECSI_ANSWER_CANCEL;

    // without a dialog the user cannot answer; quitting discards the work so it always finishes, closing panels keeps it
    if (!SDL_ShowMessageBox(&data, &answer))
    {
        answer = quitting ? ECSI_ANSWER_DISCARD : ECSI_ANSWER_CANCEL;
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cannot ask about unsaved work (%s); it is %s. %s", SDL_GetError(), quitting ? "discarded" : "kept", data.message);
    }

    SDL_free(message);

    if (answer != ECSI_ANSWER_SAVE)
    {
        return answer == ECSI_ANSWER_DISCARD;
    }

    // a failed save cancels the close
    for (usz i = 0; i < count; i++)
    {
        ECSPanel panel = panels[i];

        if (!panel->unsaved)
        {
            continue;
        }

        if (panel->type == NULL || panel->fault != NULL || panel->type->desc.Save == NULL || panel->type->desc.Save(panel->state))
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Panel '%s' could not save its work; closing is cancelled.", panel->title);
            return false;
        }

        panel->unsaved = false;
    }

    return true;
}

u32 ECSI_PanelsGetStateVersion(const char *typeName)
{
    SDL_assert(typeName != NULL);

    ECSI_PanelType *type = ECSI_PanelTypeFind(typeName);
    return type == NULL ? 0 : type->desc.stateVersion;
}

bool ECSI_PanelCanRestart(ECSPanel panel)
{
    SDL_assert(panel != NULL);

    return panel->fault != NULL || panel->type == NULL;
}

bool ECSI_PanelRestart(ECSPanel panel)
{
    SDL_assert(panel != NULL);

    ECSI_PanelType *type = ECSI_PanelTypeFind(panel->typeName);

    if (!ECSI_PanelCanRestart(panel) || type == NULL)
    {
        SDL_Log("'%s' cannot be restarted%s.", panel->title, type == NULL ? "; its type is still missing" : "");
        return false;
    }

    // a faulted panel's type is not called again, except Destroy, which frees the old state
    if (panel->type != NULL && panel->state != NULL)
    {
        panel->type->desc.Destroy(panel->state);
    }

    ECSI_EventsStopTimersOf(panel);
    SDL_free(panel->fault);
    panel->fault = NULL;
    panel->state = NULL;
    panel->type = type;
    panel->needsDraw = true;

    if (type->desc.Create(panel, panel->savedState, panel->stateVersion, &panel->state))
    {
        ECSI_PanelFault(panel, "The panel type failed to create the panel again.");
        panel->type = NULL;
        panel->state = NULL;
        return false;
    }

    SDL_Log("'%s' is restarted.", panel->title);
    return true;
}

const char *const *ECSI_PanelGetMenuEntries(ECSPanel panel, usz *retCount)
{
    SDL_assert(panel != NULL);
    SDL_assert(retCount != NULL);

    *retCount = panel->type == NULL ? 0 : arrlenu(panel->type->menuEntries);
    return *retCount == 0 ? NULL : (const char *const *)panel->type->menuEntries;
}

SHUResult ECSPanelType_AddMenuEntry(ECSPlugin plugin, const char *panelType, const char *function)
{
    SDL_assert(plugin != NULL);
    SDL_assert(panelType != NULL && function != NULL);

    ECSI_PanelType *type = ECSI_PanelTypeFind(panelType);

    if (type == NULL || type->plugin != plugin)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' adds a menu entry to '%s', which is not one of its panel types.", ECSI_PluginGetName(plugin), panelType);
        return SHUResult_ErrBadData;
    }

    char *copy = SDL_strdup(function);

    if (copy == NULL)
    {
        return SHUResult_ErrAllocation;
    }

    arrput(type->menuEntries, copy);
    return SHUResult_Ok;
}

void ECSI_PanelFault(ECSPanel panel, const char *message)
{
    SDL_assert(panel != NULL);
    SDL_assert(message != NULL);

    if (panel->fault == NULL)
    {
        panel->fault = SDL_strdup(message);
        panel->needsDraw = true;
    }
}

SHUResult ECSPanelType_Register(ECSPlugin plugin, const ECSPanelTypeDesc *desc)
{
    return ECSI_PanelTypeRegister(plugin, desc, NULL);
}

SHUResult ECSI_PanelTypeRegister(ECSPlugin plugin, const ECSPanelTypeDesc *desc, void *typeData)
{
    SDL_assert(plugin != NULL);
    SDL_assert(desc != NULL);

    const char *pluginName = ECSI_PluginGetName(plugin);

    if (desc->name == NULL || desc->Create == NULL || desc->Destroy == NULL)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' registered an incomplete panel type.", pluginName);
        return SHUResult_ErrBadData;
    }

    if (!ECSI_PluginOwnsName(plugin, desc->name))
    {
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

    *type = (ECSI_PanelType){.desc = *desc, .name = name, .title = title, .plugin = plugin, .typeData = typeData};
    type->desc.name = name;
    type->desc.title = title;
    shput(PANELS.types, name, type);

    return SHUResult_Ok;
}

SHUResult ECSPanel_StartTimer(ECSPanel panel, ECSTimer *retTimer, f64 seconds, bool repeat, ECSTimerFunction function, void *data)
{
    SDL_assert(panel != NULL);
    SDL_assert(panel->type != NULL); // placeholders run no code

    return ECSI_EventsStartTimer(panel->type->plugin, panel, retTimer, seconds, repeat, function, NULL, data);
}

void ECSPanel_SetUnsaved(ECSPanel panel, bool unsaved)
{
    SDL_assert(panel != NULL);

    // the tab shows the mark
    panel->needsDraw = panel->needsDraw || panel->unsaved != unsaved;
    panel->unsaved = unsaved;
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

u32 ECSPanel_GetId(ECSPanel panel)
{
    SDL_assert(panel != NULL);

    return panel->id;
}

const char *ECSPanel_GetType(ECSPanel panel)
{
    SDL_assert(panel != NULL);

    return panel->typeName;
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
