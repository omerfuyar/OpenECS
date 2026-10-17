#include "interface/Panels.h"

#include "base/Values.h"
#include "runtime/Events.h"
#include "runtime/Plugins.h"
#include "runtime/Services.h"
#include "runtime/Settings.h"

#include "SDL3/SDL.h"
#include "stb/stbSDL3.h"

#pragma region Source Only

static struct
{
    struct
    {
        char *key; // the type's own copy of its name
        ECSIPanelType *value;
    } *types;         // stb_ds hash map; panels point to the types, so each type is allocated on its own
    ECSPanel *closed; // stb_ds array
    u32 nextPanelID;
    ECSIAskFunction Ask; // asks about unsaved work; Input gives it
} PANELS = {0};

static void ECSIPanel_TypeFree(ECSIPanelType *type)
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

static ECSIPanelType *ECSIPanel_TypeFind(const char *name)
{
    return shget(PANELS.types, name);
}

/// @brief Gives a panel pixels of the size of its rectangle, reusing them when the size did not change.
/// @return false if the pixels cannot be allocated.
static bool ECSIPanel_ResizePixels(ECSPanel panel)
{
    int width = (int)SDL_max(1.0f, SDL_roundf(panel->width));
    int height = (int)SDL_max(1.0f, SDL_roundf(panel->height));

    if (panel->pixels != nullptr && panel->pixels->w == width && panel->pixels->h == height)
    {
        return true;
    }

    SDL_DestroySurface(panel->pixels);
    panel->pixels = SDL_CreateSurface(width, height, SDL_PIXELFORMAT_ARGB8888);

    if (panel->texture != nullptr)
    {
        SDL_DestroyTexture(panel->texture);
        panel->texture = nullptr;
    }

    return panel->pixels != nullptr;
}

/// @brief Copies a GPU panel's texture into the texture of a renderer without the device, once the panel's draw is sent: the GPU downloads it, and the renderer uploads it.
static void ECSIPanel_CopyGpuTexture(ECSPanel panel)
{
    SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(panel->gpuDevice);
    SDL_GPUCopyPass *pass = commands == nullptr ? nullptr : SDL_BeginGPUCopyPass(commands);

    if (pass == nullptr)
    {
        if (commands != nullptr)
        {
            SDL_CancelGPUCommandBuffer(commands);
        }

        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cannot copy a GPU panel: %s", SDL_GetError());
        return;
    }

    const SDL_GPUTextureRegion region = {.texture = panel->gpuTexture, .w = (Uint32)panel->gpuWidth, .h = (Uint32)panel->gpuHeight, .d = 1};
    const SDL_GPUTextureTransferInfo destination = {.transfer_buffer = panel->gpuCopy};
    SDL_DownloadFromGPUTexture(pass, &region, &destination);
    SDL_EndGPUCopyPass(pass);

    // the copy waits for the panel's commands, which were sent first, and the pixels are read once it is done
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);

    if (fence == nullptr || !SDL_WaitForGPUFences(panel->gpuDevice, true, &fence, 1))
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cannot copy a GPU panel: %s", SDL_GetError());
    }
    else
    {
        void *pixels = SDL_MapGPUTransferBuffer(panel->gpuDevice, panel->gpuCopy, false);

        if (pixels != nullptr)
        {
            SDL_UpdateTexture(panel->texture, nullptr, pixels, panel->gpuWidth * 4);
            SDL_UnmapGPUTransferBuffer(panel->gpuDevice, panel->gpuCopy);
        }
    }

    if (fence != nullptr)
    {
        SDL_ReleaseGPUFence(panel->gpuDevice, fence);
    }
}

/// @brief Releases a GPU panel's texture on the device, and the view of it that a renderer made.
static void ECSIPanel_ReleaseGpuTexture(ECSPanel panel)
{
    if (panel->texture != nullptr)
    {
        SDL_DestroyTexture(panel->texture);
        panel->texture = nullptr;
    }

    if (panel->gpuCopy != nullptr)
    {
        SDL_ReleaseGPUTransferBuffer(panel->gpuDevice, panel->gpuCopy);
        panel->gpuCopy = nullptr;
    }

    if (panel->gpuTexture != nullptr)
    {
        SDL_ReleaseGPUTexture(panel->gpuDevice, panel->gpuTexture);
        panel->gpuTexture = nullptr;
        panel->gpuDevice = nullptr;
    }
}

/// @brief Gives a GPU panel a texture of the size of its rectangle on OpenECS's GPU device, and a renderer's view of it.
/// A renderer without a GPU device, as when the GPU cannot present to its window, gets a texture of its own instead, which a copy of the panel's fills after each draw.
/// The texture is kept while its size and device stay, so it moves between OS windows; a new one is drawn before it shows.
/// @return false if there is no GPU device, or the texture cannot be made.
static bool ECSIPanel_ResizeGpuTexture(ECSPanel panel, SDL_Renderer *renderer)
{
    int width = (int)SDL_max(1.0f, SDL_roundf(panel->width));
    int height = (int)SDL_max(1.0f, SDL_roundf(panel->height));
    SDL_GPUDevice *shown = SDL_GetGPURendererDevice(renderer);
    SDL_GPUDevice *device = shown != nullptr ? shown : ECSGpu_GetDevice();
    bool copied = shown == nullptr;

    if (device == nullptr)
    {
        ECSIPanel_Fault(panel, "The panel draws with the GPU, but OpenECS has no GPU device here.");
        return false;
    }

    bool kept = panel->gpuTexture != nullptr && panel->gpuDevice == device && panel->gpuWidth == width && panel->gpuHeight == height && (panel->gpuCopy != nullptr) == copied;

    if (kept && panel->texture != nullptr && SDL_GetRendererFromTexture(panel->texture) == renderer)
    {
        return true;
    }

    if (!kept)
    {
        ECSIPanel_ReleaseGpuTexture(panel);

        // ARGB8888 is B8G8R8A8_UNORM on the GPU; the panel draws into it, and the renderers sample it
        const SDL_GPUTextureCreateInfo info = {
            .type = SDL_GPU_TEXTURETYPE_2D,
            .format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM,
            .usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER,
            .width = (Uint32)width,
            .height = (Uint32)height,
            .layer_count_or_depth = 1,
            .num_levels = 1,
        };

        panel->gpuTexture = SDL_CreateGPUTexture(device, &info);

        if (panel->gpuTexture == nullptr)
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cannot create a %dx%d GPU texture: %s", width, height, SDL_GetError());
            return false;
        }

        panel->gpuDevice = device;
        panel->gpuWidth = width;
        panel->gpuHeight = height;
        panel->needsDraw = true;

        if (copied)
        {
            const SDL_GPUTransferBufferCreateInfo copyInfo = {.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, .size = (Uint32)width * (Uint32)height * 4};
            panel->gpuCopy = SDL_CreateGPUTransferBuffer(device, &copyInfo);

            if (panel->gpuCopy == nullptr)
            {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cannot copy a %dx%d GPU texture: %s", width, height, SDL_GetError());
                return false;
            }
        }
    }
    else if (panel->texture != nullptr)
    {
        SDL_DestroyTexture(panel->texture);
        panel->texture = nullptr;
    }

    // a renderer without the device shows a texture of its own, which the next draw fills
    if (copied)
    {
        panel->texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, width, height);

        if (panel->texture == nullptr)
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cannot create a %dx%d texture: %s", width, height, SDL_GetError());
            return false;
        }

        SDL_SetTextureBlendMode(panel->texture, SDL_BLENDMODE_NONE);
        panel->needsDraw = true;
        return true;
    }

    // the renderer shows the texture without owning it
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, SDL_PIXELFORMAT_ARGB8888);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, SDL_TEXTUREACCESS_TARGET);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, width);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, height);
    SDL_SetPointerProperty(props, SDL_PROP_TEXTURE_CREATE_GPU_TEXTURE_POINTER, panel->gpuTexture);
    panel->texture = props == 0 ? nullptr : SDL_CreateTextureWithProperties(renderer, props);
    SDL_DestroyProperties(props);

    if (panel->texture == nullptr)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cannot show a %dx%d GPU texture: %s", width, height, SDL_GetError());
        return false;
    }

    // panels are opaque
    SDL_SetTextureBlendMode(panel->texture, SDL_BLENDMODE_NONE);
    return true;
}

static void ECSIPanel_DeliverEvent(void *target, const ECSPanelEvent *event)
{
    ECSPanel panel = target;

    if (!panel->closed && panel->fault == nullptr && panel->type != nullptr && panel->type->desc.Event != nullptr)
    {
        panel->type->desc.Event(panel->state, event);
    }
}

/// @brief Dropped data on its way to a panel; a Drop event's data points to it.
struct ECSIDropData
{
    ECSPanel panel;
    char *type;
    ECSValue *value; // a nil value if the drag gave none
};

/// @brief Delivers a Text event, then frees its copy of the text.
static void ECSIPanel_DeliverText(void *target, const ECSPanelEvent *event)
{
    ECSIPanel_DeliverEvent(*(ECSPanel *)target, event);
    SDL_free(target);
}

/// @brief Frees a panel's list of accepted types.
static void ECSIPanel_ClearAccepts(ECSPanel panel)
{
    for (usz i = 0; i < arrlenu(panel->accepts); i++)
    {
        SDL_free(panel->accepts[i]);
    }

    arrfree(panel->accepts);
}

/// @brief Delivers a Drop event, then frees its data.
static void ECSIPanel_DeliverDrop(void *target, const ECSPanelEvent *event)
{
    ECSDropData data = target;
    ECSIPanel_DeliverEvent(data->panel, event);

    SDL_free(data->type);
    ECSValue_Destroy(&data->value);
    SDL_free(data);
}

#pragma endregion Source Only

void ECSIPanels_Terminate(void)
{
    ECSIPanels_DestroyClosed();

    for (usz i = 0; i < shlenu(PANELS.types); i++)
    {
        ECSIPanel_TypeFree(PANELS.types[i].value);
    }

    shfree(PANELS.types);
    SDL_zero(PANELS);
}

SHUResult ECSIPanel_Create(ECSPanel *retPanel, const char *typeName, const ECSValue *savedState, u32 stateVersion)
{
    SDL_assert(retPanel != nullptr);
    SDL_assert(typeName != nullptr);

    ECSIPanelType *type = ECSIPanel_TypeFind(typeName);
    ECSPanel panel = SDL_calloc(1, sizeof(struct ECSIPanel));

    if (panel == nullptr)
    {
        return SHUResult_ErrAllocation;
    }

    panel->typeName = SDL_strdup(typeName);
    panel->title = SDL_strdup(type == nullptr ? typeName : type->title);

    if (panel->typeName == nullptr || panel->title == nullptr || (savedState != nullptr && ECSValue_Create(&panel->savedState)))
    {
        ECSIPanel_Destroy(&panel);
        return SHUResult_ErrAllocation;
    }

    if (savedState != nullptr)
    {
        SHU_ReturnResult(ECSIValue_Copy(panel->savedState, savedState), ECSIPanel_Destroy(&panel););
    }

    panel->id = ++PANELS.nextPanelID;
    panel->stateVersion = stateVersion;
    panel->needsDraw = true;

    if (type == nullptr)
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
            ECSIPanel_Fault(panel, "The panel type failed to create the panel.");
            panel->type = nullptr;
            panel->state = nullptr;
        }
    }

    *retPanel = panel;
    return SHUResult_Ok;
}

void ECSIPanel_Destroy(ECSPanel *panel)
{
    SDL_assert(panel != nullptr && *panel != nullptr);

    ECSPanel target = *panel;
    ECSIEvents_StopTimersOf(target);

    if (target->type != nullptr)
    {
        target->type->desc.Destroy(target->state);
    }

    ECSIPanel_ReleaseGpuTexture(target);
    SDL_DestroySurface(target->pixels);
    ECSIPanel_ClearAccepts(target);
    ECSValue_Destroy(&target->savedState);
    SDL_free(target->fault);
    SDL_free(target->typeName);
    SDL_free(target->title);
    SDL_free(target);

    *panel = nullptr;
}

void ECSIPanel_ReleaseTexture(ECSPanel panel)
{
    SDL_assert(panel != nullptr);

    if (panel->texture != nullptr)
    {
        SDL_DestroyTexture(panel->texture);
        panel->texture = nullptr;
    }
}

void ECSIPanel_Close(ECSPanel *panel)
{
    SDL_assert(panel != nullptr && *panel != nullptr);
    SDL_assert(!(*panel)->closed);

    ECSIEvents_StopTimersOf(*panel);
    ECSIPanel_Emit("ecs.panelClosed", *panel);

    // a closed panel is never shown again, and its OS window may close before it is destroyed, with the renderer its texture belongs to
    ECSIPanel_ReleaseGpuTexture(*panel);
    (*panel)->closed = true;
    arrput(PANELS.closed, *panel);
    *panel = nullptr;
}

void ECSIPanels_DestroyClosed(void)
{
    for (usz i = 0; i < arrlenu(PANELS.closed); i++)
    {
        ECSIPanel_Destroy(&PANELS.closed[i]);
    }

    arrfree(PANELS.closed);
}

void ECSIPanel_SetID(ECSPanel panel, u32 id)
{
    SDL_assert(panel != nullptr);

    if (id != 0)
    {
        panel->id = id;
        PANELS.nextPanelID = SDL_max(PANELS.nextPanelID, id);
    }
}

SHUResult ECSIPanel_Save(ECSPanel panel, ECSValue *retPanel)
{
    SDL_assert(panel != nullptr);
    SDL_assert(retPanel != nullptr);

    ECSValue *field = nullptr;
    ECSValue_SetTable(retPanel);
    SHU_ReturnResult(ECSValue_TableSetField(retPanel, "id", &field));
    ECSValue_SetInteger(field, panel->id);
    SHU_ReturnResult(ECSValue_TableSetField(retPanel, "type", &field));
    SHU_ReturnResult(ECSValue_SetString(field, panel->typeName));

    const ECSValue *state = panel->savedState;
    u32 version = panel->stateVersion;
    ECSValue *saved = nullptr;

    if (panel->fault == nullptr && panel->type != nullptr && panel->type->desc.SaveState != nullptr)
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
            saved = nullptr;
            state = panel->savedState;
            version = panel->stateVersion;
        }
    }

    SHUResult result = SHUResult_Ok;

    if (state != nullptr)
    {
        result = ECSValue_TableSetField(retPanel, "stateVersion", &field);

        if (!result)
        {
            ECSValue_SetInteger(field, version);
            result = ECSValue_TableSetField(retPanel, "state", &field);
        }

        result = result ? result : ECSIValue_Copy(field, state);
    }

    ECSValue_Destroy(&saved);
    return result;
}

void ECSIPanel_SetRect(ECSPanel panel, f32 x, f32 y, f32 width, f32 height)
{
    SDL_assert(panel != nullptr);

    if (SDL_roundf(panel->width) != SDL_roundf(width) || SDL_roundf(panel->height) != SDL_roundf(height))
    {
        panel->needsDraw = true;
    }

    panel->x = x;
    panel->y = y;
    panel->width = width;
    panel->height = height;
}

bool ECSIPanel_WantsFrame(ECSPanel panel)
{
    SDL_assert(panel != nullptr);

    return panel->needsDraw || (panel->fault == nullptr && panel->type != nullptr && panel->type->desc.continuous);
}

void ECSIPanel_Draw(ECSPanel panel, SDL_Renderer *renderer, u64 nowTicks)
{
    SDL_assert(panel != nullptr);
    SDL_assert(renderer != nullptr);

    // placeholders and faulted panels have no pixels; the layout draws their text
    if (panel->type == nullptr || panel->fault != nullptr || panel->type->desc.Draw == nullptr || panel->width < 1.0f || panel->height < 1.0f)
    {
        return;
    }

    f64 seconds = panel->lastDrawTicks == 0 ? 0.0 : (f64)(nowTicks - panel->lastDrawTicks) / 1e9;

    // a GPU panel draws into its texture on the device itself, with its own command buffer, which runs before the renderer's
    if (panel->type->desc.surface == ECSSurfaceType_Gpu)
    {
        if (!ECSIPanel_ResizeGpuTexture(panel, renderer) || !ECSIPanel_WantsFrame(panel))
        {
            return;
        }

        ECSSurface surface = {
            .type = ECSSurfaceType_Gpu,
            .width = panel->gpuWidth,
            .height = panel->gpuHeight,
            .scale = 1.0f,
            .texture = panel->gpuTexture,
        };

        panel->lastDrawTicks = nowTicks;
        panel->needsDraw = false;
        panel->type->desc.Draw(panel->state, &surface, seconds);
        ECSIServices_EndSurface(&surface);

        if (panel->gpuCopy != nullptr)
        {
            ECSIPanel_CopyGpuTexture(panel);
        }

        return;
    }

    // a texture belongs to one renderer, and the panel may have moved to another OS window
    if (panel->texture != nullptr && SDL_GetRendererFromTexture(panel->texture) != renderer)
    {
        SDL_DestroyTexture(panel->texture);
        panel->texture = nullptr;
    }

    if (ECSIPanel_WantsFrame(panel) || panel->texture == nullptr)
    {
        if (!ECSIPanel_ResizePixels(panel))
        {
            return;
        }

        SDL_Surface *pixels = panel->pixels;

        if (panel->texture == nullptr)
        {
            panel->texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, pixels->w, pixels->h);

            if (panel->texture == nullptr)
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

        panel->lastDrawTicks = nowTicks;
        panel->needsDraw = false;

        panel->type->desc.Draw(panel->state, &surface, seconds);

        // the services that drew into the surface finish, and its Lua handle goes
        ECSIServices_EndSurface(&surface);
        SDL_UpdateTexture(panel->texture, nullptr, pixels->pixels, pixels->pitch);
    }
}

void ECSIPanel_Show(ECSPanel panel, SDL_Renderer *renderer)
{
    SDL_assert(panel != nullptr);
    SDL_assert(renderer != nullptr);

    if (panel->texture != nullptr && panel->fault == nullptr)
    {
        SDL_FRect rect = {panel->x, panel->y, panel->width, panel->height};
        SDL_RenderTexture(renderer, panel->texture, nullptr, &rect);
    }
}

void ECSIPanel_SetVisible(ECSPanel panel, bool visible)
{
    SDL_assert(panel != nullptr);

    if (visible != panel->visible)
    {
        panel->visible = visible;
        ECSPanelEvent event = {.type = visible ? ECSPanelEventType_Shown : ECSPanelEventType_Hidden, .size = {panel->width, panel->height}};
        ECSIPanel_PostEvent(panel, &event);
    }
    else if (visible && (panel->width != panel->toldWidth || panel->height != panel->toldHeight))
    {
        ECSPanelEvent event = {.type = ECSPanelEventType_Resized, .size = {panel->width, panel->height}};
        ECSIPanel_PostEvent(panel, &event);
    }

    if (visible)
    {
        panel->toldWidth = panel->width;
        panel->toldHeight = panel->height;
    }
}

void ECSIPanel_Emit(const char *name, ECSPanel panel)
{
    SDL_assert(name != nullptr);
    SDL_assert(panel != nullptr);

    ECSValue *value = nullptr;
    ECSValue *field = nullptr;

    if (ECSValue_Create(&value) || ECSValue_TableSetField(value, "panel", &field))
    {
        ECSValue_Destroy(&value);
        return;
    }

    ECSValue_SetInteger(field, panel->id);

    if (ECSValue_TableSetField(value, "type", &field) == SHUResult_Ok && ECSValue_SetString(field, panel->typeName) == SHUResult_Ok)
    {
        ECSIEvents_EmitCore(name, value);
    }

    ECSValue_Destroy(&value);
}

void ECSIPanel_PostEvent(ECSPanel panel, const ECSPanelEvent *event)
{
    SDL_assert(panel != nullptr);
    SDL_assert(event != nullptr);

    ECSIEvents_Post(ECSIPanel_DeliverEvent, panel, event);
}

bool ECSIPanel_Accepts(ECSPanel panel, const char *type)
{
    SDL_assert(panel != nullptr);
    SDL_assert(type != nullptr);

    for (usz i = 0; !panel->closed && panel->fault == nullptr && i < arrlenu(panel->accepts); i++)
    {
        if (SDL_strcmp(panel->accepts[i], type) == 0)
        {
            return true;
        }
    }

    return false;
}

void ECSIPanel_PostDrop(ECSPanel panel, f32 x, f32 y, const char *type, const ECSValue *value, u32 modifiers)
{
    SDL_assert(panel != nullptr);
    SDL_assert(type != nullptr);

    ECSDropData data = SDL_calloc(1, sizeof(*data));
    SHUResult result = data == nullptr ? SHUResult_ErrAllocation : SHUResult_Ok;
    result = result ? result : ECSValue_Create(&data->value);
    result = result || value == nullptr ? result : ECSIValue_Copy(data->value, value);

    if (!result)
    {
        data->panel = panel;
        data->type = SDL_strdup(type);
        result = data->type == nullptr ? SHUResult_ErrAllocation : SHUResult_Ok;
    }

    if (result)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Data of type '%s' is not dropped on '%s': out of memory.", type, panel->title);

        if (data != nullptr)
        {
            SDL_free(data->type);
            ECSValue_Destroy(&data->value);
            SDL_free(data);
        }

        return;
    }

    ECSPanelEvent event = {.type = ECSPanelEventType_Drop, .modifiers = modifiers, .drop = {.x = x, .y = y, .data = data}};
    ECSIEvents_Post(ECSIPanel_DeliverDrop, data, &event);
}

void ECSIPanel_PostText(ECSPanel panel, const char *text, u32 modifiers)
{
    SDL_assert(panel != nullptr);
    SDL_assert(text != nullptr);

    // the panel and the text are kept in one block: the panel first, then the text
    usz length = SDL_strlen(text);
    ECSPanel *target = SDL_malloc(sizeof(ECSPanel) + length + 1);

    if (target == nullptr)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Typed text is lost: out of memory.");
        return;
    }

    char *copy = (char *)(target + 1);
    *target = panel;
    SDL_memcpy(copy, text, length + 1);

    ECSPanelEvent event = {.type = ECSPanelEventType_Text, .modifiers = modifiers, .text = {.text = copy}};
    ECSIEvents_Post(ECSIPanel_DeliverText, target, &event);
}

void ECSPanel_SetTextInput(ECSPanel panel, bool accept, f32 x, f32 y, f32 width, f32 height)
{
    SDL_assert(panel != nullptr);

    panel->textInput = accept;
    panel->textArea = (SDL_FRect){x, y, width, height};
}

SHUResult ECSPanel_AcceptDrops(ECSPanel panel, const char *const *types, usz count)
{
    SDL_assert(panel != nullptr);
    SDL_assert(types != nullptr || count == 0);

    ECSIPanel_ClearAccepts(panel);

    for (usz i = 0; i < count; i++)
    {
        SDL_assert(types[i] != nullptr);
        char *copy = SDL_strdup(types[i]);

        if (copy == nullptr)
        {
            ECSIPanel_ClearAccepts(panel);
            return SHUResult_ErrAllocation;
        }

        arrput(panel->accepts, copy);
    }

    return SHUResult_Ok;
}

const char *ECSDropData_GetType(ECSDropData data)
{
    SDL_assert(data != nullptr);
    return data->type;
}

const ECSValue *ECSDropData_GetValue(ECSDropData data)
{
    SDL_assert(data != nullptr);
    return data->value;
}

void ECSIPanels_RemovePlugin(ECSPlugin plugin)
{
    SDL_assert(plugin != nullptr);

    // backwards, because shdel moves the last type into the hole
    for (usz i = shlenu(PANELS.types); i > 0; i--)
    {
        ECSIPanelType *type = PANELS.types[i - 1].value;

        if (type->plugin == plugin)
        {
            (void)shdel(PANELS.types, type->name);
            ECSIPanel_TypeFree(type);
        }
    }
}

void ECSIPanels_SetAsk(ECSIAskFunction Ask)
{
    PANELS.Ask = Ask;
}

bool ECSIPanels_ConfirmClose(const ECSPanel *panels, usz count, bool quitting)
{
    SDL_assert(panels != nullptr || count == 0);

    char *list = SDL_strdup("");
    usz unsaved = 0;

    for (usz i = 0; i < count && list != nullptr; i++)
    {
        if (panels[i]->unsaved)
        {
            char *next = nullptr;
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
        OPENECS_ANSWER_SAVE,
        OPENECS_ANSWER_DISCARD,
        OPENECS_ANSWER_CANCEL,
    };

    const char *const buttons[] = {"Save", "Discard", "Cancel"};
    char *message = nullptr;
    SDL_asprintf(&message, "%s unsaved work:%s", unsaved == 1 ? "This panel has" : "These panels have", list == nullptr ? "" : list);
    SDL_free(list);

    const char *text = message == nullptr ? "Some panels have unsaved work." : message;
    i32 answer = PANELS.Ask == nullptr ? -1 : PANELS.Ask("Unsaved work", text, buttons, SDL_arraysize(buttons));

    // without a dialog the user cannot answer; quitting discards the work so it always finishes, closing panels keeps it
    if (answer < 0)
    {
        answer = quitting ? OPENECS_ANSWER_DISCARD : OPENECS_ANSWER_CANCEL;
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cannot ask about unsaved work; it is %s. %s", quitting ? "discarded" : "kept", text);
    }

    SDL_free(message);

    if (answer != OPENECS_ANSWER_SAVE)
    {
        return answer == OPENECS_ANSWER_DISCARD;
    }

    // a failed save cancels the close
    for (usz i = 0; i < count; i++)
    {
        ECSPanel panel = panels[i];

        if (!panel->unsaved)
        {
            continue;
        }

        if (panel->type == nullptr || panel->fault != nullptr || panel->type->desc.Save == nullptr || panel->type->desc.Save(panel->state))
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Panel '%s' could not save its work; closing is cancelled.", panel->title);
            return false;
        }

        panel->unsaved = false;
    }

    return true;
}

u32 ECSIPanels_GetStateVersion(const char *typeName)
{
    SDL_assert(typeName != nullptr);

    ECSIPanelType *type = ECSIPanel_TypeFind(typeName);
    return type == nullptr ? 0 : type->desc.stateVersion;
}

bool ECSIPanel_CanRestart(ECSPanel panel)
{
    SDL_assert(panel != nullptr);

    return panel->fault != nullptr || panel->type == nullptr;
}

bool ECSIPanel_Restart(ECSPanel panel)
{
    SDL_assert(panel != nullptr);

    ECSIPanelType *type = ECSIPanel_TypeFind(panel->typeName);

    if (!ECSIPanel_CanRestart(panel) || type == nullptr)
    {
        SDL_Log("'%s' cannot be restarted%s.", panel->title, type == nullptr ? "; its type is still missing" : "");
        return false;
    }

    // a faulted panel's type is not called again, except Destroy, which frees the old state
    if (panel->type != nullptr && panel->state != nullptr)
    {
        panel->type->desc.Destroy(panel->state);
    }

    // the new state says again which data it accepts
    ECSIEvents_StopTimersOf(panel);
    ECSIPanel_ClearAccepts(panel);
    panel->textInput = false;
    SDL_free(panel->fault);
    panel->fault = nullptr;
    panel->state = nullptr;
    panel->type = type;
    panel->needsDraw = true;

    if (type->desc.Create(panel, panel->savedState, panel->stateVersion, &panel->state))
    {
        ECSIPanel_Fault(panel, "The panel type failed to create the panel again.");
        panel->type = nullptr;
        panel->state = nullptr;
        return false;
    }

    SDL_Log("'%s' is restarted.", panel->title);
    return true;
}

const char *const *ECSIPanel_GetMenuEntries(ECSPanel panel, usz *retCount)
{
    SDL_assert(panel != nullptr);
    SDL_assert(retCount != nullptr);

    *retCount = panel->type == nullptr ? 0 : arrlenu(panel->type->menuEntries);
    return *retCount == 0 ? nullptr : (const char *const *)panel->type->menuEntries;
}

/// @brief Orders panel types by the name of their plugin, then by title.
static int SDLCALL ECSIPanel_CompareTypes(const void *a, const void *b)
{
    const ECSIPanelType *first = *(const ECSIPanelType *const *)a;
    const ECSIPanelType *second = *(const ECSIPanelType *const *)b;
    int plugins = SDL_strcmp(ECSIPlugin_GetName(first->plugin), ECSIPlugin_GetName(second->plugin));
    return plugins != 0 ? plugins : SDL_strcmp(first->title, second->title);
}

/// @brief Sets a field of a table to a string.
static SHUResult ECSIPanel_SetStringField(ECSValue *table, const char *name, const char *string)
{
    ECSValue *field = nullptr;
    SHU_ReturnResult(ECSValue_TableSetField(table, name, &field));
    return ECSValue_SetString(field, string);
}

SHUResult ECSPanelType_List(ECSValue *retList)
{
    SDL_assert(retList != nullptr);

    usz count = shlenu(PANELS.types);
    ECSIPanelType **types = count == 0 ? nullptr : SDL_malloc(count * sizeof(ECSIPanelType *));

    if (count > 0 && types == nullptr)
    {
        return SHUResult_ErrAllocation;
    }

    for (usz i = 0; i < count; i++)
    {
        types[i] = PANELS.types[i].value;
    }

    SDL_qsort(types, count, sizeof(ECSIPanelType *), ECSIPanel_CompareTypes);
    ECSValue_SetTable(retList);
    SHUResult result = SHUResult_Ok;

    for (usz i = 0; i < count && !result; i++)
    {
        ECSValue *entry = nullptr;
        result = ECSValue_ListAddItem(retList, &entry);

        if (!result)
        {
            ECSValue_SetTable(entry);
            result = ECSIPanel_SetStringField(entry, "name", types[i]->name);
        }

        result = result ? result : ECSIPanel_SetStringField(entry, "title", types[i]->title);
        result = result ? result : ECSIPanel_SetStringField(entry, "plugin", ECSIPlugin_GetName(types[i]->plugin));
        result = result ? result : ECSIPanel_SetStringField(entry, "description", ECSIPlugin_GetDescription(types[i]->plugin));
    }

    SDL_free(types);
    return result;
}

SHUResult ECSPanelType_AddMenuEntry(ECSPlugin plugin, const char *panelType, const char *function)
{
    SDL_assert(plugin != nullptr);
    SDL_assert(panelType != nullptr && function != nullptr);

    ECSIPanelType *type = ECSIPanel_TypeFind(panelType);

    if (type == nullptr || type->plugin != plugin)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' adds a menu entry to '%s', which is not one of its panel types.", ECSIPlugin_GetName(plugin), panelType);
        return SHUResult_ErrBadData;
    }

    char *copy = SDL_strdup(function);

    if (copy == nullptr)
    {
        return SHUResult_ErrAllocation;
    }

    arrput(type->menuEntries, copy);
    return SHUResult_Ok;
}

void ECSIPanel_Fault(ECSPanel panel, const char *message)
{
    SDL_assert(panel != nullptr);
    SDL_assert(message != nullptr);

    if (panel->fault == nullptr)
    {
        panel->fault = SDL_strdup(message);
        panel->needsDraw = true;
    }
}

SHUResult ECSPanelType_Register(ECSPlugin plugin, const ECSPanelTypeDesc *desc)
{
    return ECSIPanel_TypeRegister(plugin, desc, nullptr);
}

SHUResult ECSIPanel_TypeRegister(ECSPlugin plugin, const ECSPanelTypeDesc *desc, void *typeData)
{
    SDL_assert(plugin != nullptr);
    SDL_assert(desc != nullptr);

    const char *pluginName = ECSIPlugin_GetName(plugin);

    if (desc->name == nullptr || desc->Create == nullptr || desc->Destroy == nullptr)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Plugin '%s' registered an incomplete panel type.", pluginName);
        return SHUResult_ErrBadData;
    }

    // the description gives the local name, and the type is known by its full name
    char *fullName = ECSIPlugin_FullName(plugin, desc->name);

    if (fullName == nullptr)
    {
        return SHUResult_ErrBadData;
    }

    ECSPanelTypeDesc named = *desc;
    named.name = fullName;
    desc = &named;

    if (desc->surface != ECSSurfaceType_Pixels && desc->surface != ECSSurfaceType_Gpu)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Panel type '%s' asks for an unknown surface type.", desc->name);
        SDL_free(fullName);
        return SHUResult_ErrBadData;
    }

    if (ECSIPanel_TypeFind(desc->name) != nullptr)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Panel type '%s' is already registered.", desc->name);
        SDL_free(fullName);
        return SHUResult_ErrBadData;
    }

    ECSIPanelType *type = SDL_malloc(sizeof(ECSIPanelType));
    char *name = fullName;
    char *title = SDL_strdup(desc->title == nullptr ? desc->name : desc->title);

    if (type == nullptr || name == nullptr || title == nullptr)
    {
        SDL_free(type);
        SDL_free(name);
        SDL_free(title);
        return SHUResult_ErrAllocation;
    }

    *type = (ECSIPanelType){.desc = *desc, .name = name, .title = title, .plugin = plugin, .typeData = typeData};
    type->desc.name = name;
    type->desc.title = title;
    shput(PANELS.types, name, type);

    return SHUResult_Ok;
}

SHUResult ECSPanel_StartTimer(ECSPanel panel, ECSTimer *retTimer, f64 seconds, bool repeat, ECSTimerFunction function, void *data)
{
    SDL_assert(panel != nullptr);
    SDL_assert(panel->type != nullptr); // placeholders run no code

    return ECSIEvents_StartTimer(panel->type->plugin, panel, retTimer, seconds, repeat, function, nullptr, data);
}

bool ECSPanel_IsUnsaved(ECSPanel panel)
{
    SDL_assert(panel != nullptr);

    return panel->unsaved;
}

void ECSPanel_SetUnsaved(ECSPanel panel, bool unsaved)
{
    SDL_assert(panel != nullptr);

    // the tab shows the mark
    panel->needsDraw = panel->needsDraw || panel->unsaved != unsaved;
    panel->unsaved = unsaved;
}

void ECSPanel_Redraw(ECSPanel panel)
{
    SDL_assert(panel != nullptr);

    panel->needsDraw = true;
}

void ECSSurface_Fill(ECSSurface *surface, i32 x, i32 y, i32 width, i32 height, ECSColor color)
{
    SDL_assert(surface != nullptr);

    // surfaces hold premultiplied colours
    color = ECSIColor_Premultiply(color);

    if (surface->type != ECSSurfaceType_Pixels)
    {
        return;
    }

    i32 left = SDL_max(x, 0);
    i32 top = SDL_max(y, 0);
    i32 right = (i32)SDL_min((i64)x + width, (i64)surface->width);
    i32 bottom = (i32)SDL_min((i64)y + height, (i64)surface->height);

    for (i32 row = top; row < bottom; row++)
    {
        u32 *pixel = (u32 *)((u8 *)surface->pixels.data + (usz)row * (usz)surface->pitch);

        for (i32 column = left; column < right; column++)
        {
            pixel[column] = color;
        }
    }
}

const char *ECSPanel_GetTitle(ECSPanel panel)
{
    SDL_assert(panel != nullptr);

    return panel->title;
}

u32 ECSPanel_GetID(ECSPanel panel)
{
    SDL_assert(panel != nullptr);

    return panel->id;
}

const char *ECSPanel_GetType(ECSPanel panel)
{
    SDL_assert(panel != nullptr);

    return panel->typeName;
}

void ECSPanel_SetTitle(ECSPanel panel, const char *title)
{
    SDL_assert(panel != nullptr);
    SDL_assert(title != nullptr);

    char *copy = SDL_strdup(title);

    if (copy != nullptr)
    {
        SDL_free(panel->title);
        panel->title = copy;
    }
}
