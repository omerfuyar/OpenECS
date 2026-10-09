// The ui standard plugin: draws rectangles, text, images and user-interface elements into the pixels surface of another plugin's panel (DESIGN 20.2).
// It is built like a third-party plugin, against the plugin interface; it calls SDL3 and SDL3_ttf itself, using the executable's copy.

#include "OpenECS.h"

#include "SDL3/SDL.h"
#include "SDL3_ttf/SDL_ttf.h"

#pragma region Source Only

/// @brief A name of the plugin: "ui." and a local name.
#define UI_NAME(localName) "ui." localName

/// @brief The colour of a mistake, so it shows: opaque magenta.
#define UI_BAD_COLOR 0xFFFF00FF

/// @brief The shortest a scrollbar's thumb gets, in layout units, so it can still be grabbed.
#define UI_THUMB_MIN 24.0f

/// @brief The inner space of buttons and fields, in layout units.
#define UI_PADDING 6.0f

/// @brief A font at one size, in pixels.
typedef struct UiFont
{
    i32 size;
    TTF_Font *font;
} UiFont;

/// @brief An image file, read once.
typedef struct UiImage
{
    char *path;
    SDL_Surface *surface; // NULL if the file cannot be read, so it is reported once
} UiImage;

static struct
{
    ECSPlugin plugin;
    char *fontPath;
    UiFont *fonts; // every size asked for so far, opened once
    usz fontCount;
    UiImage *images; // every image asked for so far
    usz imageCount;
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

/// @brief Fills a rectangle in pixels with an ARGB colour, blending by its alpha.
static void UiFillPixels(SDL_Surface *target, SDL_Rect rect, u32 argb)
{
    SDL_Rect bounds = {0, 0, target->w, target->h};
    u32 alpha = argb >> 24;

    if (alpha == 0xFF)
    {
        SDL_FillSurfaceRect(target, &rect, argb);
        return;
    }

    if (alpha == 0 || !SDL_GetRectIntersection(&rect, &bounds, &rect))
    {
        return;
    }

    // panels are opaque, so each channel blends and the alpha stays
    for (int row = rect.y; row < rect.y + rect.h; row++)
    {
        u32 *pixel = (u32 *)((u8 *)target->pixels + (usz)row * (usz)target->pitch) + rect.x;

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

/// @brief Turns a rectangle in layout units into pixels.
static SDL_Rect UiPixels(f32 scale, f32 x, f32 y, f32 width, f32 height)
{
    return (SDL_Rect){(int)SDL_roundf(x * scale), (int)SDL_roundf(y * scale), (int)SDL_roundf(width * scale), (int)SDL_roundf(height * scale)};
}

/// @brief Draws text with its line's top left at a position in layout units, inside the target's clip rectangle.
/// @return The text's width in layout units, or 0 if it was not drawn.
static f32 UiDrawText(SDL_Surface *target, f32 scale, const char *text, f32 x, f32 y, f32 size, i64 color)
{
    TTF_Font *font = scale <= 0.0f ? NULL : UiGetFont(size * scale);
    TTF_Text *drawn = font == NULL ? NULL : TTF_CreateText(UI.engine, font, text, 0);
    int width = 0;
    u32 argb = (u32)color;

    if (drawn == NULL)
    {
        return 0.0f;
    }

    TTF_SetTextColor(drawn, (Uint8)(argb >> 16), (Uint8)(argb >> 8), (Uint8)argb, (Uint8)(argb >> 24));
    TTF_DrawSurfaceText(drawn, (int)SDL_roundf(x * scale), (int)SDL_roundf(y * scale), target);
    TTF_GetTextSize(drawn, &width, NULL);
    TTF_DestroyText(drawn);
    return (f32)width / scale;
}

/// @brief Gives the size the elements' labels are drawn in: the core's ecs.fontSize, in layout units.
static f32 UiFontSize(void)
{
    return (f32)ECSValue_GetNumber(ECSSetting_Get("ecs.fontSize"), 14.0);
}

/// @brief Fills a rectangle, in layout units, with an ARGB colour, blending by its alpha.
static void UiFill(ECSSurface *surface, f32 x, f32 y, f32 width, f32 height, i64 color)
{
    SDL_Surface *target = UiWrap(surface);

    if (target != NULL)
    {
        UiFillPixels(target, UiPixels(surface->scale, x, y, width, height), (u32)color);
        SDL_DestroySurface(target);
    }
}

/// @brief Draws the edge of a rectangle, in layout units, with a thickness and a colour.
static void UiOutline(ECSSurface *surface, f32 x, f32 y, f32 width, f32 height, f32 thickness, i64 color)
{
    SDL_Surface *target = UiWrap(surface);

    if (target == NULL)
    {
        return;
    }

    SDL_Rect rect = UiPixels(surface->scale, x, y, width, height);
    int edge = SDL_max(1, (int)SDL_roundf(thickness * surface->scale));
    edge = SDL_min(edge, SDL_min(rect.w, rect.h) / 2 + 1);

    UiFillPixels(target, (SDL_Rect){rect.x, rect.y, rect.w, edge}, (u32)color);
    UiFillPixels(target, (SDL_Rect){rect.x, rect.y + rect.h - edge, rect.w, edge}, (u32)color);
    UiFillPixels(target, (SDL_Rect){rect.x, rect.y + edge, edge, rect.h - 2 * edge}, (u32)color);
    UiFillPixels(target, (SDL_Rect){rect.x + rect.w - edge, rect.y + edge, edge, rect.h - 2 * edge}, (u32)color);
    SDL_DestroySurface(target);
}

/// @brief Draws text with its line's top left at a position, in layout units.
/// @return The text's width in layout units, or 0 if it was not drawn.
static f32 UiText(ECSSurface *surface, const char *text, f32 x, f32 y, f32 size, i64 color)
{
    SDL_Surface *target = UiWrap(surface);
    f32 width = target == NULL ? 0.0f : UiDrawText(target, surface->scale, text, x, y, size, color);

    SDL_DestroySurface(target);
    return width;
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

/// @brief Gets an image file, reading it the first time; a relative path starts at the executable's folder.
/// @return The image in ARGB, or NULL if it cannot be read; the reason is logged the first time.
static SDL_Surface *UiGetImage(const char *path)
{
    for (usz i = 0; i < UI.imageCount; i++)
    {
        if (SDL_strcmp(UI.images[i].path, path) == 0)
        {
            return UI.images[i].surface;
        }
    }

    char *full = NULL;
    UiImage *images = SDL_realloc(UI.images, (UI.imageCount + 1) * sizeof(UiImage));

    if (images == NULL || SDL_asprintf(&full, "%s%s", path[0] == '/' ? "" : SDL_GetBasePath(), path) < 0)
    {
        UI.images = images == NULL ? UI.images : images;
        ECS_Log(UI.plugin, ECSLogLevel_Error, "Cannot read the image '%s': out of memory.", path);
        return NULL;
    }

    UI.images = images;
    SDL_Surface *loaded = SDL_LoadSurface(full);
    SDL_Surface *image = loaded == NULL ? NULL : SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_ARGB8888);

    if (image == NULL)
    {
        ECS_Log(UI.plugin, ECSLogLevel_Error, "Cannot read the image '%s': %s", full, SDL_GetError());
    }

    SDL_DestroySurface(loaded);
    SDL_free(full);
    UI.images[UI.imageCount++] = (UiImage){.path = SDL_strdup(path), .surface = image};

    if (UI.images[UI.imageCount - 1].path == NULL)
    {
        SDL_DestroySurface(image);
        UI.imageCount--;
        return NULL;
    }

    return image;
}

/// @brief Draws an image file, PNG, JPG or BMP, stretched to a rectangle in layout units.
/// @return true if it was drawn.
static bool UiImageDraw(ECSSurface *surface, const char *path, f32 x, f32 y, f32 width, f32 height)
{
    SDL_Surface *image = UiGetImage(path);
    SDL_Surface *target = image == NULL ? NULL : UiWrap(surface);

    if (target == NULL)
    {
        return false;
    }

    SDL_Rect rect = UiPixels(surface->scale, x, y, width, height);
    bool drawn = SDL_BlitSurfaceScaled(image, NULL, target, &rect, SDL_SCALEMODE_LINEAR);
    SDL_DestroySurface(target);
    return drawn;
}

/// @brief Gives an image file's size in pixels.
/// @return true if the file can be read.
static bool UiImageSize(const char *path, f32 *retWidth, f32 *retHeight)
{
    SDL_Surface *image = UiGetImage(path);

    *retWidth = image == NULL ? 0.0f : (f32)image->w;
    *retHeight = image == NULL ? 0.0f : (f32)image->h;
    return image != NULL;
}

/// @brief Draws a button: a box with a label in its middle. State 0 is normal, 1 is under the pointer and 2 is pressed.
static void UiButton(ECSSurface *surface, const char *label, f32 x, f32 y, f32 width, f32 height, i32 state)
{
    const char *const fills[] = {"tab", "tabShown", "accent"};
    f32 size = UiFontSize();
    f32 labelWidth = 0.0f;
    f32 line = 0.0f;
    TTF_Font *font = UiGetFont(size);

    if (font != NULL)
    {
        int pixels = 0;
        TTF_GetStringSize(font, label, 0, &pixels, NULL);
        labelWidth = (f32)pixels;
        line = (f32)TTF_GetFontHeight(font);
    }

    UiFill(surface, x, y, width, height, UiColor(fills[SDL_clamp(state, 0, 2)]));
    UiText(surface, label, x + (width - labelWidth) / 2.0f, y + (height - line) / 2.0f, size, UiColor("text"));
}

/// @brief Draws a check box: a square, filled when it is on.
static void UiCheck(ECSSurface *surface, f32 x, f32 y, f32 size, bool on)
{
    f32 inner = SDL_floorf(size / 4.0f);

    UiFill(surface, x, y, size, size, UiColor("tabRow"));
    UiOutline(surface, x, y, size, size, 1.0f, UiColor(on ? "accent" : "textDim"));

    if (on)
    {
        UiFill(surface, x + inner, y + inner, size - 2.0f * inner, size - 2.0f * inner, UiColor("accent"));
    }
}

/// @brief Draws a text field: a box with a line of text. A focused field has a cursor after its text and shows the text's end.
/// @return The cursor's x in layout units, such as for the input method's window.
static f32 UiField(ECSSurface *surface, const char *text, f32 x, f32 y, f32 width, f32 height, bool focused)
{
    SDL_Surface *target = UiWrap(surface);

    if (target == NULL)
    {
        return x;
    }

    f32 scale = surface->scale;
    f32 size = UiFontSize();
    f32 textWidth = 0.0f;
    f32 line = 0.0f;
    TTF_Font *font = UiGetFont(size);

    if (font != NULL)
    {
        int pixels = 0;
        TTF_GetStringSize(font, text, 0, &pixels, NULL);
        textWidth = (f32)pixels;
        line = (f32)TTF_GetFontHeight(font);
    }

    // the text moves left when it is longer than the box, so the cursor stays in view
    f32 inside = width - 2.0f * UI_PADDING - 2.0f;
    f32 textX = x + UI_PADDING - (focused ? SDL_max(0.0f, textWidth - inside) : 0.0f);
    f32 cursor = textX + textWidth;
    SDL_Rect clip = UiPixels(scale, x + 1.0f, y + 1.0f, width - 2.0f, height - 2.0f);

    UiFillPixels(target, UiPixels(scale, x, y, width, height), (u32)UiColor("tabRow"));
    SDL_SetSurfaceClipRect(target, &clip);
    UiDrawText(target, scale, text, textX, y + (height - line) / 2.0f, size, UiColor("text"));

    if (focused)
    {
        UiFillPixels(target, UiPixels(scale, cursor, y + (height - line) / 2.0f, 2.0f, line), (u32)UiColor("accent"));
    }

    SDL_DestroySurface(target);
    UiOutline(surface, x, y, width, height, 1.0f, UiColor(focused ? "accent" : "tabShown"));
    return cursor;
}

/// @brief Gives where a scrollbar's thumb is, from the top of its bar, and how long it is, in layout units.
/// @param length The bar's length.
/// @param total The length of all the content.
/// @param shown The length of the content in view.
/// @param offset How far the content is scrolled.
static void UiThumb(f32 length, f32 total, f32 shown, f32 offset, f32 *retStart, f32 *retLength)
{
    f32 thumb = total <= shown || total <= 0.0f ? length : SDL_max(SDL_min(UI_THUMB_MIN, length), length * shown / total);
    f32 travel = total - shown;

    *retLength = thumb;
    *retStart = travel <= 0.0f ? 0.0f : (length - thumb) * SDL_clamp(offset / travel, 0.0f, 1.0f);
}

/// @brief Draws a vertical scrollbar: a track and a thumb that shows which part of the content is in view. It draws nothing when all of it is in view.
static void UiScrollbar(ECSSurface *surface, f32 x, f32 y, f32 width, f32 height, f32 total, f32 shown, f32 offset)
{
    f32 start = 0.0f;
    f32 thumb = 0.0f;

    if (total <= shown)
    {
        return;
    }

    UiThumb(height, total, shown, offset, &start, &thumb);
    UiFill(surface, x, y, width, height, UiColor("tabRow"));
    UiFill(surface, x, y + start, width, thumb, UiColor("tabShown"));
}

/// @brief Frees the fonts and the text engine, and stops SDL_ttf.
static void UiFree(void)
{
    for (usz i = 0; i < UI.fontCount; i++)
    {
        TTF_CloseFont(UI.fonts[i].font);
    }

    for (usz i = 0; i < UI.imageCount; i++)
    {
        SDL_free(UI.images[i].path);
        SDL_DestroySurface(UI.images[i].surface);
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
    SDL_free(UI.images);
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
        {UI_NAME("fill"), (ECSFunction)UiFill, "void(handle<ecs.surface> surface, float x, float y, float width, float height, int64 color)", "Fill a rectangle with a colour"},
        {UI_NAME("text"), (ECSFunction)UiText, "float(handle<ecs.surface> surface, string text, float x, float y, float size, int64 color)", "Draw text and give its width"},
        {UI_NAME("measure"), (ECSFunction)UiMeasure, "void(string text, float size, out float width, out float lineHeight)", "Give the width and line height of a text"},
        {UI_NAME("color"), (ECSFunction)UiColor, "int64(string color)", "Read a colour, or a colour of the theme by name"},
        {UI_NAME("outline"), (ECSFunction)UiOutline, "void(handle<ecs.surface> surface, float x, float y, float width, float height, float thickness, int64 color)", "Draw the edge of a rectangle"},
        {UI_NAME("image"), (ECSFunction)UiImageDraw, "bool(handle<ecs.surface> surface, string path, float x, float y, float width, float height)", "Draw an image file stretched to a rectangle"},
        {UI_NAME("imageSize"), (ECSFunction)UiImageSize, "bool(string path, out float width, out float height)", "Give an image file's size in pixels"},
        {UI_NAME("button"), (ECSFunction)UiButton, "void(handle<ecs.surface> surface, string label, float x, float y, float width, float height, int state)", "Draw a button with a label"},
        {UI_NAME("check"), (ECSFunction)UiCheck, "void(handle<ecs.surface> surface, float x, float y, float size, bool on)", "Draw a check box"},
        {UI_NAME("field"), (ECSFunction)UiField, "float(handle<ecs.surface> surface, string text, float x, float y, float width, float height, bool focused)", "Draw a text field and give its cursor's x"},
        {UI_NAME("scrollbar"), (ECSFunction)UiScrollbar, "void(handle<ecs.surface> surface, float x, float y, float width, float height, float total, float shown, float offset)", "Draw a vertical scrollbar"},
        {UI_NAME("thumb"), (ECSFunction)UiThumb, "void(float length, float total, float shown, float offset, out float start, out float thumbLength)", "Give where a scrollbar's thumb is and how long"},
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
