// The ui standard plugin: draws rectangles and text into the pixels surface of another plugin's panel (DESIGN 20.2).
// It is built like a third-party plugin, against the plugin interface; it calls SDL3 and SDL3_ttf itself, using the executable's copy.

#include "OpenECS.h"

#include "SDL3/SDL.h"
#include "SDL3_ttf/SDL_ttf.h"

#pragma region Source Only

/// @brief A name of the plugin: "ui." and a local name.
#define UI_NAME(localName) "ui." localName

/// @brief The colour of a mistake, so it shows: opaque magenta.
#define UI_BAD_COLOR 0xFFFF00FF

/// @brief A font at one size, in pixels.
typedef struct UiFont
{
    i32 size;
    TTF_Font *font;
} UiFont;

static struct
{
    ECSPlugin plugin;
    char *fontPath;
    UiFont *fonts; // every size asked for so far, opened once
    usz fontCount;
    TTF_TextEngine *engine; // draws text into surfaces, and keeps the glyphs it drew
    bool ttfStarted;
} UI = {0};

/// @brief Gets the font at a size in pixels, opening it the first time.
/// @return The font, or NULL if it cannot be opened; the reason is logged.
static TTF_Font *UiGetFont(f32 pixels)
{
    i32 size = (i32)SDL_max(1.0f, SDL_roundf(pixels));

    for (usz i = 0; i < UI.fontCount; i++)
    {
        if (UI.fonts[i].size == size)
        {
            return UI.fonts[i].font;
        }
    }

    TTF_Font *font = TTF_OpenFont(UI.fontPath, (float)size);
    UiFont *fonts = font == NULL ? NULL : SDL_realloc(UI.fonts, (UI.fontCount + 1) * sizeof(UiFont));

    if (fonts == NULL)
    {
        ECS_Log(UI.plugin, ECSLogLevel_Error, "Cannot open the font '%s' at %d pixels: %s", UI.fontPath, size, SDL_GetError());
        TTF_CloseFont(font);
        return NULL;
    }

    UI.fonts = fonts;
    UI.fonts[UI.fontCount++] = (UiFont){.size = size, .font = font};
    return font;
}

/// @brief Wraps a panel's pixels in an SDL surface, for one call.
/// @return The surface, or NULL; free it with SDL_DestroySurface.
static SDL_Surface *UiWrap(ECSSurface *surface)
{
    if (surface->type != ECSSurfaceType_Pixels)
    {
        ECS_Log(UI.plugin, ECSLogLevel_Warning, "ui draws only into pixels surfaces.");
        return NULL;
    }

    return SDL_CreateSurfaceFrom(surface->width, surface->height, SDL_PIXELFORMAT_ARGB8888, surface->pixels.data, surface->pitch);
}

/// @brief Fills a rectangle, in layout units, with an ARGB colour, blending by its alpha.
static void UiFill(ECSSurface *surface, f32 x, f32 y, f32 width, f32 height, i64 color)
{
    SDL_Surface *target = UiWrap(surface);

    if (target == NULL)
    {
        return;
    }

    f32 scale = surface->scale;
    SDL_Rect bounds = {0, 0, surface->width, surface->height};
    SDL_Rect rect = {(int)SDL_roundf(x * scale), (int)SDL_roundf(y * scale), (int)SDL_roundf(width * scale), (int)SDL_roundf(height * scale)};
    u32 argb = (u32)color;
    u32 alpha = argb >> 24;

    if (alpha == 0xFF)
    {
        SDL_FillSurfaceRect(target, &rect, argb);
    }
    else if (alpha > 0 && SDL_GetRectIntersection(&rect, &bounds, &rect))
    {
        // panels are opaque, so each channel blends and the alpha stays
        for (int row = rect.y; row < rect.y + rect.h; row++)
        {
            u32 *pixel = (u32 *)((u8 *)surface->pixels.data + (usz)row * (usz)surface->pitch) + rect.x;

            for (int column = 0; column < rect.w; column++, pixel++)
            {
                u32 blended = *pixel & 0xFF000000;

                for (u32 shift = 0; shift < 24; shift += 8)
                {
                    u32 source = (argb >> shift) & 0xFF;
                    u32 destination = (*pixel >> shift) & 0xFF;
                    blended |= ((source * alpha + destination * (255 - alpha)) / 255) << shift;
                }

                *pixel = blended;
            }
        }
    }

    SDL_DestroySurface(target);
}

/// @brief Draws text with its line's top left at a position, in layout units.
/// @return The text's width in layout units, or 0 if it was not drawn.
static f32 UiText(ECSSurface *surface, const char *text, f32 x, f32 y, f32 size, i64 color)
{
    SDL_Surface *target = UiWrap(surface);
    TTF_Font *font = target == NULL ? NULL : UiGetFont(size * surface->scale);
    TTF_Text *drawn = font == NULL ? NULL : TTF_CreateText(UI.engine, font, text, 0);
    int width = 0;
    u32 argb = (u32)color;

    if (drawn != NULL)
    {
        TTF_SetTextColor(drawn, (Uint8)(argb >> 16), (Uint8)(argb >> 8), (Uint8)argb, (Uint8)(argb >> 24));
        TTF_DrawSurfaceText(drawn, (int)SDL_roundf(x * surface->scale), (int)SDL_roundf(y * surface->scale), target);
        TTF_GetTextSize(drawn, &width, NULL);
        TTF_DestroyText(drawn);
    }

    SDL_DestroySurface(target);
    return target == NULL || surface->scale <= 0.0f ? 0.0f : (f32)width / surface->scale;
}

/// @brief Gives the width and the line height of a text in a size, without drawing it, in layout units.
static void UiMeasure(const char *text, f32 size, f32 *retWidth, f32 *retHeight)
{
    TTF_Font *font = UiGetFont(size);
    int width = 0;

    if (font != NULL)
    {
        TTF_GetStringSize(font, text, 0, &width, NULL);
    }

    *retWidth = (f32)width;
    *retHeight = font == NULL ? 0.0f : (f32)TTF_GetFontHeight(font);
}

/// @brief Reads "#RRGGBB" or "#RRGGBBAA" as an ARGB colour.
/// @return true if the text is a colour.
static bool UiParseColor(const char *text, u32 *retColor)
{
    usz length = SDL_strlen(text);
    char *end = NULL;

    if (text[0] != '#' || (length != 7 && length != 9))
    {
        return false;
    }

    u32 value = (u32)SDL_strtoul(text + 1, &end, 16);

    if (*end != '\0')
    {
        return false;
    }

    *retColor = length == 7 ? 0xFF000000 | value : (value >> 8) | (value << 24);
    return true;
}

/// @brief Reads a colour: "#RRGGBB", "#RRGGBBAA", or the name of a colour of the core's theme, such as "text" for ecs.colorText.
/// @return The ARGB colour, or opaque magenta if it cannot be read.
static i64 UiColor(const char *name)
{
    u32 color = UI_BAD_COLOR;
    char *setting = NULL;

    if (UiParseColor(name, &color))
    {
        return color;
    }

    // a theme colour is the core's setting ecs.color and the name with a capital
    if (name[0] != '\0' && SDL_asprintf(&setting, "ecs.color%c%s", SDL_toupper((unsigned char)name[0]), name + 1) >= 0 &&
        UiParseColor(ECSValue_GetString(ECSSetting_Get(setting), ""), &color))
    {
        SDL_free(setting);
        return color;
    }

    ECS_Log(UI.plugin, ECSLogLevel_Warning, "'%s' is neither a colour nor a colour of the theme.", name);
    SDL_free(setting);
    return UI_BAD_COLOR;
}

/// @brief Frees the fonts and the text engine, and stops SDL_ttf.
static void UiFree(void)
{
    for (usz i = 0; i < UI.fontCount; i++)
    {
        TTF_CloseFont(UI.fonts[i].font);
    }

    if (UI.engine != NULL)
    {
        TTF_DestroySurfaceTextEngine(UI.engine);
    }

    // SDL_ttf counts its starts, so the core's own use goes on
    if (UI.ttfStarted)
    {
        TTF_Quit();
    }

    SDL_free(UI.fonts);
    SDL_free(UI.fontPath);
    SDL_zero(UI);
}

#pragma endregion Source Only

SHUResult ECSPlugin_Init(ECSPlugin plugin)
{
    UI.plugin = plugin;

    // the core's font; a relative path starts at the executable's folder
    const char *font = ECSValue_GetString(ECSSetting_Get("ecs.font"), "");

    if (SDL_asprintf(&UI.fontPath, "%s%s", font[0] == '/' ? "" : SDL_GetBasePath(), font) < 0)
    {
        UI.fontPath = NULL;
        UiFree();
        return SHUResult_ErrAllocation;
    }

    UI.ttfStarted = TTF_Init();
    UI.engine = UI.ttfStarted ? TTF_CreateSurfaceTextEngine() : NULL;

    if (UI.engine == NULL)
    {
        ECS_Log(plugin, ECSLogLevel_Error, "Cannot start SDL_ttf: %s", SDL_GetError());
        UiFree();
        return SHUResult_ErrInternal;
    }

    const struct
    {
        const char *name;
        ECSFunction function;
        const char *signature;
        const char *description;
    } functions[] = {
        {UI_NAME("fill"), (ECSFunction)UiFill, "void(handle<ecs.surface>, float, float, float, float, int64)", "Fill a rectangle with a colour"},
        {UI_NAME("text"), (ECSFunction)UiText, "float(handle<ecs.surface>, string, float, float, float, int64)", "Draw text and give its width"},
        {UI_NAME("measure"), (ECSFunction)UiMeasure, "void(string, float, out float, out float)", "Give the width and line height of a text"},
        {UI_NAME("color"), (ECSFunction)UiColor, "int64(string)", "Read a colour, or a colour of the theme by name"},
    };

    // a plugin whose Init fails gets no Shutdown, so it cleans up here
    for (usz i = 0; i < SDL_arraysize(functions); i++)
    {
        SHU_ReturnResult(ECSService_RegisterFunction(plugin, functions[i].name, functions[i].function, functions[i].signature, functions[i].description), UiFree(););
    }

    return SHUResult_Ok;
}

void ECSPlugin_Shutdown(ECSPlugin plugin)
{
    (void)plugin;
    UiFree();
}
