#include "tools/Renderer.h"

#include "clay/claySDL3.h"

#pragma region Source Only

/// @brief Size of the core's font, in layout units.
#define OPENECS_FONT_SIZE 14

/// @brief Background colour; it shows through the gaps between panels.
#define OPENECS_COLOR_BACKGROUND 0x18, 0x19, 0x1C, 0xFF

struct ECSI_RenderWindow
{
    SDL_Renderer *renderer;
    Clay_SDL3RendererData clay;
};

struct ECSI_RenderTexture
{
    SDL_Texture *texture;
};

static struct
{
    SDL_GPUDevice *device; // shared by every OS window; NULL until the first window, or if the GPU renderer is not available
    TTF_Font *fonts[1];
} RENDERER = {0};

/// @brief Creates a 2D renderer on the shared GPU device. Falls back to SDL's default renderer if the GPU renderer is not available.
static SDL_Renderer *ECSI_RendererCreate(ECSI_OsWindow *window)
{
    SDL_Renderer *renderer = SDL_CreateGPURenderer(RENDERER.device, window);

    if (renderer != NULL)
    {
        if (RENDERER.device == NULL)
        {
            RENDERER.device = SDL_GetGPURendererDevice(renderer);
        }

        return renderer;
    }

    SHU_LogWarning("GPU renderer not available (%s); using SDL's default renderer.", SDL_GetError());
    return SDL_CreateRenderer(window, NULL);
}

#pragma endregion Source Only

SHUResult ECSI_RendererInitialize(const char *fontPath)
{
    SHU_AssertNullPointer(fontPath);

    if (!TTF_Init())
    {
        SHU_LogWarning("SDL_ttf failed to start: %s", SDL_GetError());
        return SHUResult_ErrInternal;
    }

    RENDERER.fonts[0] = TTF_OpenFont(fontPath, OPENECS_FONT_SIZE);

    if (RENDERER.fonts[0] == NULL)
    {
        SHU_LogWarning("Cannot load font '%s': %s", fontPath, SDL_GetError());
        return SHUResult_ErrFile;
    }

    return SHUResult_Ok;
}

void ECSI_RendererTerminate(void)
{
    if (RENDERER.fonts[0] != NULL)
    {
        TTF_CloseFont(RENDERER.fonts[0]);
    }

    TTF_Quit();
    RENDERER = (typeof(RENDERER)){0};
}

Clay_Dimensions ECSI_RendererMeasureText(Clay_StringSlice text, Clay_TextElementConfig *config, void *userData)
{
    (void)userData;

    TTF_Font *font = RENDERER.fonts[config->fontId];
    int width = 0;
    int height = 0;

    TTF_SetFontSize(font, config->fontSize);
    TTF_GetStringSize(font, text.chars, (size_t)text.length, &width, &height);

    return (Clay_Dimensions){(f32)width, (f32)height};
}

SHUResult ECSI_RenderWindowCreate(ECSI_RenderWindow **retRenderWindow, ECSI_OsWindow *window)
{
    SHU_AssertNullPointer(retRenderWindow);
    SHU_AssertNullPointer(window);

    ECSI_RenderWindow *renderWindow = calloc(1, sizeof(ECSI_RenderWindow));

    if (renderWindow == NULL)
    {
        return SHUResult_ErrAllocation;
    }

    renderWindow->renderer = ECSI_RendererCreate(window);

    if (renderWindow->renderer == NULL)
    {
        SHU_LogWarning("Cannot create a renderer: %s", SDL_GetError());
        free(renderWindow);
        return SHUResult_ErrInternal;
    }

    SDL_SetRenderVSync(renderWindow->renderer, 1);

    renderWindow->clay.renderer = renderWindow->renderer;
    renderWindow->clay.textEngine = TTF_CreateRendererTextEngine(renderWindow->renderer);
    renderWindow->clay.fonts = RENDERER.fonts;

    *retRenderWindow = renderWindow;
    return SHUResult_Ok;
}

void ECSI_RenderWindowDestroy(ECSI_RenderWindow **renderWindow)
{
    SHU_AssertNullPointer(renderWindow);
    SHU_AssertNullPointer(*renderWindow);

    TTF_DestroyRendererTextEngine((*renderWindow)->clay.textEngine);
    SDL_DestroyRenderer((*renderWindow)->renderer);

    free(*renderWindow);
    *renderWindow = NULL;
}

void ECSI_RenderWindowBegin(ECSI_RenderWindow *renderWindow)
{
    SHU_AssertNullPointer(renderWindow);

    SDL_SetRenderDrawColor(renderWindow->renderer, OPENECS_COLOR_BACKGROUND);
    SDL_RenderClear(renderWindow->renderer);
}

void ECSI_RenderWindowDrawTexture(ECSI_RenderWindow *renderWindow, ECSI_RenderTexture *texture, f32 x, f32 y, f32 width, f32 height)
{
    SHU_AssertNullPointer(renderWindow);
    SHU_AssertNullPointer(texture);

    SDL_FRect rect = {x, y, width, height};
    SDL_RenderTexture(renderWindow->renderer, texture->texture, NULL, &rect);
}

void ECSI_RenderWindowDrawCommands(ECSI_RenderWindow *renderWindow, Clay_RenderCommandArray commands)
{
    SHU_AssertNullPointer(renderWindow);

    SDL_Clay_RenderClayCommands(&renderWindow->clay, &commands);
}

void ECSI_RenderWindowPresent(ECSI_RenderWindow *renderWindow)
{
    SHU_AssertNullPointer(renderWindow);

    SDL_RenderPresent(renderWindow->renderer);
}

SHUResult ECSI_RenderTextureCreate(ECSI_RenderTexture **retTexture, ECSI_RenderWindow *renderWindow, i32 width, i32 height)
{
    SHU_AssertNullPointer(retTexture);
    SHU_AssertNullPointer(renderWindow);

    ECSI_RenderTexture *texture = calloc(1, sizeof(ECSI_RenderTexture));

    if (texture == NULL)
    {
        return SHUResult_ErrAllocation;
    }

    texture->texture = SDL_CreateTexture(renderWindow->renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, width, height);

    if (texture->texture == NULL)
    {
        SHU_LogWarning("Cannot create a %dx%d texture: %s", width, height, SDL_GetError());
        free(texture);
        return SHUResult_ErrInternal;
    }

    // panels are opaque
    SDL_SetTextureBlendMode(texture->texture, SDL_BLENDMODE_NONE);

    *retTexture = texture;
    return SHUResult_Ok;
}

void ECSI_RenderTextureDestroy(ECSI_RenderTexture **texture)
{
    SHU_AssertNullPointer(texture);

    if (*texture != NULL)
    {
        SDL_DestroyTexture((*texture)->texture);
        free(*texture);
        *texture = NULL;
    }
}

void ECSI_RenderTextureUpdate(ECSI_RenderTexture *texture, SHUSlice pixels, i32 pitch)
{
    SHU_AssertNullPointer(texture);
    SHU_AssertNullPointer(pixels.data);

    SDL_UpdateTexture(texture->texture, NULL, pixels.data, pitch);
}
