#include "interface/Clipboard.h"

#include "SDL3/SDL.h"

#pragma region Source Only

/// @brief Typed data that the core offers on the clipboard.
typedef struct ECSIClipboardData
{
    char *mimeType;
    usz size;
    u8 bytes[]; // the data
} ECSIClipboardData;

static struct
{
    void *held; // what a clipboard getter returned last, freed by the next call
} CLIPBOARD = {0};

static const void *ECSIClipboard_Provide(void *userData, const char *mimeType, size_t *retSize)
{
    ECSIClipboardData *data = userData;

    if (SDL_strcmp(mimeType, data->mimeType) != 0)
    {
        *retSize = 0;
        return nullptr;
    }

    *retSize = data->size;
    return data->bytes;
}

static void ECSIClipboard_Release(void *userData)
{
    ECSIClipboardData *data = userData;
    SDL_free(data->mimeType);
    SDL_free(data);
}

/// @brief Frees what a clipboard getter returned last.
static void ECSIClipboard_Forget(void)
{
    SDL_free(CLIPBOARD.held);
    CLIPBOARD.held = nullptr;
}

#pragma endregion Source Only

void ECSIClipboard_Terminate(void)
{
    ECSIClipboard_Forget();
}

SHUResult ECSClipboard_SetText(const char *text)
{
    SDL_assert(text != nullptr);

    ECSIClipboard_Forget();

    if (!SDL_SetClipboardText(text))
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cannot set the clipboard: %s", SDL_GetError());
        return SHUResult_ErrInternal;
    }

    return SHUResult_Ok;
}

const char *ECSClipboard_GetText(void)
{
    ECSIClipboard_Forget();
    CLIPBOARD.held = SDL_GetClipboardText();
    return CLIPBOARD.held == nullptr ? "" : CLIPBOARD.held;
}

SHUResult ECSClipboard_SetData(const char *mimeType, SHUSliceView data)
{
    SDL_assert(mimeType != nullptr);
    SDL_assert(data.data != nullptr || data.size == 0);

    ECSIClipboard_Forget();
    ECSIClipboardData *copy = SDL_malloc(sizeof(ECSIClipboardData) + data.size);
    char *type = SDL_strdup(mimeType);

    if (copy == nullptr || type == nullptr)
    {
        SDL_free(copy);
        SDL_free(type);
        return SHUResult_ErrAllocation;
    }

    copy->mimeType = type;
    copy->size = data.size;
    SDL_memcpy(copy->bytes, data.data, data.size);

    // SDL asks for the data when another program pastes it, and releases it when the clipboard changes
    const char *types[] = {copy->mimeType};

    if (!SDL_SetClipboardData(ECSIClipboard_Provide, ECSIClipboard_Release, copy, types, 1))
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cannot set the clipboard: %s", SDL_GetError());
        ECSIClipboard_Release(copy);
        return SHUResult_ErrInternal;
    }

    return SHUResult_Ok;
}

SHUResult ECSClipboard_GetData(const char *mimeType, SHUSlice *retData)
{
    SDL_assert(mimeType != nullptr);
    SDL_assert(retData != nullptr);

    ECSIClipboard_Forget();
    usz size = 0;
    CLIPBOARD.held = SDL_HasClipboardData(mimeType) ? SDL_GetClipboardData(mimeType, &size) : nullptr;
    *retData = cs(CLIPBOARD.held, CLIPBOARD.held == nullptr ? 0 : size);
    return CLIPBOARD.held == nullptr ? SHUResult_ErrNotFound : SHUResult_Ok;
}
