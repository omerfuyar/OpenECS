// Sketch in C: canvases to draw on with the mouse, and a clock. The sketch_lua plugin does the same in Lua, so the two can be compared.

#include "OpenECS.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma region Source Only

/// @brief Makes a name of this plugin, such as "sketch_c.canvas".
#define SKETCH_NAME(local) "sketch_c." local

/// @brief Colours a brush can have, in ARGB8888, and their names in the setting sketch_c.brushColor.
static const char *const SKETCH_COLOR_NAMES[] = {"white", "red", "green", "blue", "yellow", NULL};
static const u32 SKETCH_COLORS[] = {0xFFECEFF4, 0xFFBF616A, 0xFFA3BE8C, 0xFF5E81AC, 0xFFEBCB8B};
#define SKETCH_BACKGROUND 0xFF2E3440
#define SKETCH_CLOCK_HAND 0xFF88C0D0

/// @brief The clipboard type of strokes; both sketch plugins use it, so strokes copied in one paste into the other.
#define SKETCH_CLIPBOARD_TYPE "application/x-openecs-strokes"

/// @brief Version of the saved state of a canvas, and of the plugin's own state.
#define SKETCH_STATE_VERSION 1

/// @brief Smallest and largest brush.
#define SKETCH_MIN_SIZE 1
#define SKETCH_MAX_SIZE 64

/// @brief The type of sketch_c.stats, as callers look it up: int(out value).
typedef i32 (*SketchStatsFunction)(ECSValue *retStats);

/// @brief A brush that services hand out as a handle<sketch_c.brush>. Lua's handle and each canvas that uses it hold a reference.
typedef struct SketchBrush
{
    i32 size;
    u32 color;
    u32 references;
} SketchBrush;

/// @brief One stroke: its brush and the points the pointer passed, as x, y pairs in surface pixels.
typedef struct SketchStroke
{
    i32 size;
    u32 color;
    f32 *points;
    usz count; // numbers in points: two per point
    usz capacity;
} SketchStroke;

/// @brief A canvas panel.
typedef struct SketchCanvas
{
    ECSPanel panel;
    SketchStroke *strokes;
    usz strokeCount;
    usz strokeCapacity;
    bool drawing;       // the main button is held, and the last stroke grows
    bool unsaved;       // changed since the last save
    SketchBrush *brush; // a brush from sketch_c.useBrush, or NULL for the settings' brush
    i32 width;          // size of the last surface, for exporting
    i32 height;
    ECSTimer reminder;
} SketchCanvas;

/// @brief A clock panel: a hand that turns once a minute.
typedef struct SketchClock
{
    ECSPanel panel;
    f64 time;
    i64 shownSeconds; // the seconds its title shows
} SketchClock;

/// @brief A canvas being exported in the background: a copy of what it needs, so the canvas may close meanwhile.
typedef struct SketchExportJob
{
    char *path;
    u32 panelId;
    i32 width;
    i32 height;
    SketchStroke *strokes;
    usz strokeCount;
    bool written;
} SketchExportJob;

/// @brief A callback of sketch_c.eachCanvas: a canvas and its number of strokes.
typedef void (*SketchCanvasFunction)(ECSPanel panel, i32 strokes);

/// @brief The type of sketch_c.eachCanvas, as callers look it up: int(fn<void(handle<ecs.panel>, int)>).
typedef i32 (*SketchEachCanvasFunction)(SketchCanvasFunction function);

static struct
{
    ECSPlugin plugin;
    SketchCanvas **canvases; // every open canvas, to find a canvas from its panel
    usz canvasCount;
    usz canvasCapacity;
    i64 openCanvases; // counted from the core's events
    i64 total;        // strokes drawn in every session; the plugin's own state
    i64 saves;        // canvases saved in every session; the plugin's own state
    u8 *pixels;       // the buffer sketch_c.pixels returned last; valid until its next call
    ECSTimer statsTimer;
    SketchEachCanvasFunction eachCanvas; // sketch_c.eachCanvas, through its lookup
} SKETCH = {0};

#pragma region Strokes

static bool SketchGrow(void **items, usz *capacity, usz needed, usz itemSize)
{
    if (needed <= *capacity)
    {
        return true;
    }

    usz grown = *capacity == 0 ? 8 : *capacity * 2;
    grown = grown < needed ? needed : grown;
    void *moved = realloc(*items, grown * itemSize);

    if (moved == NULL)
    {
        return false;
    }

    *items = moved;
    *capacity = grown;
    return true;
}

static bool SketchStrokeAddPoint(SketchStroke *stroke, f32 x, f32 y)
{
    if (!SketchGrow((void **)&stroke->points, &stroke->capacity, stroke->count + 2, sizeof(f32)))
    {
        return false;
    }

    stroke->points[stroke->count++] = x;
    stroke->points[stroke->count++] = y;
    return true;
}

/// @brief Adds an empty stroke to a list of strokes.
static SketchStroke *SketchAddStroke(SketchStroke **strokes, usz *count, usz *capacity, i32 size, u32 color)
{
    if (!SketchGrow((void **)strokes, capacity, *count + 1, sizeof(SketchStroke)))
    {
        return NULL;
    }

    SketchStroke *stroke = &(*strokes)[(*count)++];
    *stroke = (SketchStroke){.size = size, .color = color};
    return stroke;
}

static void SketchFreeStrokes(SketchStroke *strokes, usz count)
{
    for (usz i = 0; i < count; i++)
    {
        free(strokes[i].points);
    }

    free(strokes);
}

/// @brief Paints a filled circle into pixels, clipped to them.
static void SketchDab(u32 *pixels, i32 width, i32 height, f32 cx, f32 cy, i32 size, u32 color)
{
    f32 radius = (f32)size / 2.0f;
    i32 left = (i32)floorf(cx - radius);
    i32 top = (i32)floorf(cy - radius);
    i32 right = (i32)ceilf(cx + radius);
    i32 bottom = (i32)ceilf(cy + radius);

    for (i32 y = top < 0 ? 0 : top; y <= bottom && y < height; y++)
    {
        for (i32 x = left < 0 ? 0 : left; x <= right && x < width; x++)
        {
            f32 dx = (f32)x + 0.5f - cx;
            f32 dy = (f32)y + 0.5f - cy;

            if (dx * dx + dy * dy <= radius * radius)
            {
                pixels[(usz)y * (usz)width + (usz)x] = color;
            }
        }
    }
}

/// @brief Paints strokes over a background into tightly packed pixels: dabs at every point, and between points every half brush.
static void SketchPaint(u32 *pixels, i32 width, i32 height, const SketchStroke *strokes, usz count)
{
    for (usz i = 0; i < (usz)width * (usz)height; i++)
    {
        pixels[i] = SKETCH_BACKGROUND;
    }

    for (usz s = 0; s < count; s++)
    {
        const SketchStroke *stroke = &strokes[s];
        f32 step = stroke->size > 2 ? (f32)stroke->size / 2.0f : 1.0f;

        for (usz p = 0; p + 1 < stroke->count; p += 2)
        {
            f32 x = stroke->points[p];
            f32 y = stroke->points[p + 1];
            SketchDab(pixels, width, height, x, y, stroke->size, stroke->color);

            if (p + 3 < stroke->count)
            {
                f32 dx = stroke->points[p + 2] - x;
                f32 dy = stroke->points[p + 3] - y;
                i32 steps = (i32)(sqrtf(dx * dx + dy * dy) / step);

                for (i32 k = 1; k < steps; k++)
                {
                    SketchDab(pixels, width, height, x + dx * (f32)k / (f32)steps, y + dy * (f32)k / (f32)steps, stroke->size, stroke->color);
                }
            }
        }
    }
}

/// @brief Writes strokes as text, one stroke per line: "size color x y x y ...". The clipboard uses it.
static char *SketchStrokesToText(const SketchStroke *strokes, usz count)
{
    usz length = 1;

    for (usz i = 0; i < count; i++)
    {
        length += 32 + strokes[i].count * 16;
    }

    char *text = malloc(length);

    if (text == NULL)
    {
        return NULL;
    }

    usz used = 0;
    text[0] = '\0';

    for (usz i = 0; i < count; i++)
    {
        used += (usz)snprintf(text + used, length - used, "%d %u", strokes[i].size, strokes[i].color);

        for (usz p = 0; p < strokes[i].count; p++)
        {
            used += (usz)snprintf(text + used, length - used, " %.1f", (double)strokes[i].points[p]);
        }

        used += (usz)snprintf(text + used, length - used, "\n");
    }

    return text;
}

/// @brief Reads strokes written by SketchStrokesToText and adds them to a canvas.
/// @return Number of strokes read.
static usz SketchStrokesFromText(SketchCanvas *canvas, SHUSliceView bytes)
{
    usz added = 0;
    const char *text = bytes.data;
    const char *end = text + bytes.size;

    while (text < end)
    {
        const char *lineEnd = memchr(text, '\n', (usz)(end - text));
        lineEnd = lineEnd == NULL ? end : lineEnd;

        char *cursor = NULL;
        long size = strtol(text, &cursor, 10);
        unsigned long color = cursor < lineEnd ? strtoul(cursor, &cursor, 10) : 0;

        if (cursor < lineEnd && size >= SKETCH_MIN_SIZE && size <= SKETCH_MAX_SIZE)
        {
            SketchStroke *stroke = SketchAddStroke(&canvas->strokes, &canvas->strokeCount, &canvas->strokeCapacity, (i32)size, (u32)color);

            while (stroke != NULL && cursor < lineEnd)
            {
                char *next = NULL;
                f32 x = strtof(cursor, &next);
                f32 y = next < lineEnd ? strtof(next, &next) : 0.0f;

                if (next == cursor || next > lineEnd || !SketchStrokeAddPoint(stroke, x, y))
                {
                    break;
                }

                cursor = next;
            }

            added += stroke != NULL ? 1 : 0;
        }

        text = lineEnd + 1;
    }

    return added;
}

#pragma endregion Strokes

#pragma region Canvas

static SketchCanvas *SketchFindCanvas(ECSPanel panel)
{
    for (usz i = 0; i < SKETCH.canvasCount; i++)
    {
        if (SKETCH.canvases[i]->panel == panel)
        {
            return SKETCH.canvases[i];
        }
    }

    ECS_Log(SKETCH.plugin, ECSLogLevel_Warning, "That panel is not a sketch_c canvas.");
    return NULL;
}

static i32 SketchSettingSize(void)
{
    i64 size = ECSValue_GetInteger(ECSSetting_Get(SKETCH_NAME("brushSize")), 6);
    return (i32)(size < SKETCH_MIN_SIZE ? SKETCH_MIN_SIZE : size > SKETCH_MAX_SIZE ? SKETCH_MAX_SIZE
                                                                                   : size);
}

static u32 SketchSettingColor(void)
{
    const char *name = ECSValue_GetString(ECSSetting_Get(SKETCH_NAME("brushColor")), "white");

    for (usz i = 0; SKETCH_COLOR_NAMES[i] != NULL; i++)
    {
        if (strcmp(SKETCH_COLOR_NAMES[i], name) == 0)
        {
            return SKETCH_COLORS[i];
        }
    }

    return SKETCH_COLORS[0];
}

/// @brief Shows the number of strokes in the canvas's title.
static void SketchCanvasTitle(SketchCanvas *canvas)
{
    char title[64];
    snprintf(title, sizeof(title), "C canvas (%zu)", canvas->strokeCount);
    ECSPanel_SetTitle(canvas->panel, title);
}

/// @brief Marks a canvas changed: unsaved, retitled and redrawn.
static void SketchCanvasChanged(SketchCanvas *canvas)
{
    canvas->unsaved = true;
    ECSPanel_SetUnsaved(canvas->panel, true);
    SketchCanvasTitle(canvas);
    ECSPanel_Redraw(canvas->panel);
}

static void SketchReleaseBrush(SketchBrush *brush)
{
    if (brush != NULL && --brush->references == 0)
    {
        free(brush);
    }
}

/// @brief Reminds the user of unsaved strokes; a panel timer, so it stops when the canvas closes.
static void SketchCanvasRemind(void *data)
{
    SketchCanvas *canvas = data;

    if (canvas->unsaved)
    {
        ECS_Log(SKETCH.plugin, ECSLogLevel_Info, "%s has unsaved strokes.", ECSPanel_GetTitle(canvas->panel));
    }
}

static SHUResult SketchCanvasCreate(ECSPanel panel, const ECSValue *savedState, u32 version, void **retState)
{
    SketchCanvas *canvas = calloc(1, sizeof(SketchCanvas));

    if (canvas == NULL || !SketchGrow((void **)&SKETCH.canvases, &SKETCH.canvasCapacity, SKETCH.canvasCount + 1, sizeof(SketchCanvas *)))
    {
        free(canvas);
        return SHUResult_ErrAllocation;
    }

    canvas->panel = panel;

    // strokes = { { size = 6, color = 0xFFECEFF4, points = { x, y, ... } }, ... }; older versions are not read
    const ECSValue *strokes = version == SKETCH_STATE_VERSION ? ECSValue_GetTableField(savedState, "strokes") : NULL;

    for (usz i = 0; i < ECSValue_GetListCount(strokes); i++)
    {
        const ECSValue *saved = ECSValue_GetListItem(strokes, i);
        const ECSValue *points = ECSValue_GetTableField(saved, "points");
        i64 size = ECSValue_GetInteger(ECSValue_GetTableField(saved, "size"), 6);
        SketchStroke *stroke = SketchAddStroke(&canvas->strokes, &canvas->strokeCount, &canvas->strokeCapacity, (i32)size, (u32)ECSValue_GetInteger(ECSValue_GetTableField(saved, "color"), SKETCH_COLORS[0]));

        for (usz p = 0; stroke != NULL && p + 1 < ECSValue_GetListCount(points); p += 2)
        {
            SketchStrokeAddPoint(stroke, (f32)ECSValue_GetNumber(ECSValue_GetListItem(points, p), 0.0), (f32)ECSValue_GetNumber(ECSValue_GetListItem(points, p + 1), 0.0));
        }
    }

    SKETCH.canvases[SKETCH.canvasCount++] = canvas;
    SketchCanvasTitle(canvas);

    // a canvas takes strokes dragged from a canvas, text in their format, and files of it
    const char *accepted[] = {SKETCH_CLIPBOARD_TYPE, "text", "file-list"};

    if (ECSPanel_AcceptDrops(panel, accepted, sizeof(accepted) / sizeof(*accepted)))
    {
        ECS_Log(SKETCH.plugin, ECSLogLevel_Warning, "A canvas cannot take dropped strokes.");
    }

    f64 seconds = ECSValue_GetNumber(ECSSetting_Get(SKETCH_NAME("reminderSeconds")), 30.0);

    if (ECSPanel_StartTimer(panel, &canvas->reminder, seconds > 1.0 ? seconds : 1.0, true, SketchCanvasRemind, canvas))
    {
        ECS_Log(SKETCH.plugin, ECSLogLevel_Warning, "Cannot start a canvas's reminder.");
    }

    *retState = canvas;
    return SHUResult_Ok;
}

static void SketchCanvasDestroy(void *state)
{
    SketchCanvas *canvas = state;

    for (usz i = 0; i < SKETCH.canvasCount; i++)
    {
        if (SKETCH.canvases[i] == canvas)
        {
            SKETCH.canvases[i] = SKETCH.canvases[--SKETCH.canvasCount];
            break;
        }
    }

    SketchReleaseBrush(canvas->brush);
    SketchFreeStrokes(canvas->strokes, canvas->strokeCount);
    free(canvas);
}

static void SketchCanvasDraw(void *state, ECSSurface *surface, f64 seconds)
{
    (void)seconds;
    SketchCanvas *canvas = state;
    canvas->width = surface->width;
    canvas->height = surface->height;

    // the surface's rows may be padded, so each row is copied on its own
    u32 *pixels = calloc((usz)surface->width * (usz)surface->height, sizeof(u32));

    if (pixels == NULL)
    {
        return;
    }

    SketchPaint(pixels, surface->width, surface->height, canvas->strokes, canvas->strokeCount);

    for (i32 y = 0; y < surface->height; y++)
    {
        memcpy((u8 *)surface->pixels.data + (usz)y * (usz)surface->pitch, pixels + (usz)y * (usz)surface->width, (usz)surface->width * sizeof(u32));
    }

    free(pixels);
}

/// @brief Tells subscribers that a canvas has a new stroke.
static void SketchEmitStroke(SketchCanvas *canvas)
{
    ECSValue *value = NULL;
    ECSValue *field = NULL;

    if (ECSValue_Create(&value))
    {
        return;
    }

    if (ECSValue_TableSetField(value, "panel", &field) == SHUResult_Ok)
    {
        ECSValue_SetInteger(field, ECSPanel_GetId(canvas->panel));
    }

    if (ECSValue_TableSetField(value, "strokes", &field) == SHUResult_Ok)
    {
        ECSValue_SetInteger(field, (i64)canvas->strokeCount);
    }

    if (ECSEvent_Emit(SKETCH.plugin, SKETCH_NAME("strokeAdded"), value))
    {
        ECS_Log(SKETCH.plugin, ECSLogLevel_Warning, "Cannot emit sketch_c.strokeAdded.");
    }

    ECSValue_Destroy(&value);
}

/// @brief Reads a whole file, ending it with a zero, because strtol reads the strokes.
/// @return The text, or NULL. Free it with free.
static char *SketchReadFile(const char *path, usz *retSize)
{
    FILE *file = fopen(path, "rb");
    char *text = NULL;
    long size = -1;

    if (file != NULL && fseek(file, 0, SEEK_END) == 0 && (size = ftell(file)) >= 0 && fseek(file, 0, SEEK_SET) == 0)
    {
        text = malloc((size_t)size + 1);
        size = text != NULL ? (long)fread(text, 1, (size_t)size, file) : -1;
    }

    if (file != NULL)
    {
        fclose(file);
    }

    if (size < 0)
    {
        free(text);
        return NULL;
    }

    text[size] = '\0';
    *retSize = (usz)size;
    return text;
}

/// @brief Starts dragging a canvas's strokes, in the text format of copy.
static void SketchDragStrokes(SketchCanvas *canvas)
{
    char *text = SketchStrokesToText(canvas->strokes, canvas->strokeCount);
    ECSValue *value = NULL;

    if (text == NULL || ECSValue_Create(&value) || ECSValue_SetString(value, text) || ECSPanel_StartDrag(canvas->panel, SKETCH_CLIPBOARD_TYPE, value))
    {
        ECS_Log(SKETCH.plugin, ECSLogLevel_Warning, "Cannot drag the strokes.");
    }

    ECSValue_Destroy(&value);
    free(text);
}

/// @brief Adds dropped strokes to a canvas: strokes or text in the format of copy, or the strokes of each dropped file.
static void SketchCanvasDrop(SketchCanvas *canvas, ECSDropData data)
{
    const ECSValue *value = ECSDropData_GetValue(data);
    usz added = 0;

    if (strcmp(ECSDropData_GetType(data), "file-list") == 0)
    {
        for (usz i = 0; i < ECSValue_GetListCount(value); i++)
        {
            usz size = 0;
            char *text = SketchReadFile(ECSValue_GetString(ECSValue_GetListItem(value, i), ""), &size);
            added += text != NULL ? SketchStrokesFromText(canvas, (SHUSliceView){.data = text, .size = size}) : 0;
            free(text);
        }
    }
    else
    {
        const char *text = ECSValue_GetString(value, "");
        added = SketchStrokesFromText(canvas, (SHUSliceView){.data = text, .size = strlen(text)});
    }

    ECS_Log(SKETCH.plugin, ECSLogLevel_Info, "Dropped %zu strokes.", added);

    if (added > 0)
    {
        SketchCanvasChanged(canvas);
    }
}

static void SketchCanvasEvent(void *state, const ECSPanelEvent *event)
{
    SketchCanvas *canvas = state;

    switch (event->type)
    {
    case ECSPanelEventType_PointerDown:
    {
        if (event->pointer.button != 1)
        {
            break;
        }

        // Shift and a press drag the canvas's strokes, in the text format of copy
        if (event->modifiers & ECSModifier_Shift)
        {
            SketchDragStrokes(canvas);
            break;
        }

        i32 size = canvas->brush != NULL ? canvas->brush->size : SketchSettingSize();
        u32 color = canvas->brush != NULL ? canvas->brush->color : SketchSettingColor();
        SketchStroke *stroke = SketchAddStroke(&canvas->strokes, &canvas->strokeCount, &canvas->strokeCapacity, size, color);
        canvas->drawing = stroke != NULL && SketchStrokeAddPoint(stroke, event->pointer.x, event->pointer.y);
        ECSPanel_Redraw(canvas->panel);
        break;
    }

    case ECSPanelEventType_PointerMove:
        if (canvas->drawing && SketchStrokeAddPoint(&canvas->strokes[canvas->strokeCount - 1], event->pointer.x, event->pointer.y))
        {
            ECSPanel_Redraw(canvas->panel);
        }
        break;

    case ECSPanelEventType_PointerUp:
        if (canvas->drawing)
        {
            canvas->drawing = false;
            SketchCanvasChanged(canvas);
            SketchEmitStroke(canvas);
        }
        break;

    case ECSPanelEventType_Wheel:
    {
        // the wheel changes the setting, so every canvas and the settings window see the new size
        ECSValue *size = NULL;

        if (ECSValue_Create(&size) == SHUResult_Ok)
        {
            i32 next = SketchSettingSize() + (event->wheel.amountY > 0.0f ? 1 : -1);
            ECSValue_SetInteger(size, next < SKETCH_MIN_SIZE ? SKETCH_MIN_SIZE : next > SKETCH_MAX_SIZE ? SKETCH_MAX_SIZE
                                                                                                        : next);

            if (ECSSetting_Set(SKETCH_NAME("brushSize"), size))
            {
                ECS_Log(SKETCH.plugin, ECSLogLevel_Warning, "Cannot change the brush size.");
            }

            ECSValue_Destroy(&size);
        }

        break;
    }

    case ECSPanelEventType_Focused:
    case ECSPanelEventType_Unfocused:
        ECS_Log(SKETCH.plugin, ECSLogLevel_Debug, "%s is %s.", ECSPanel_GetTitle(canvas->panel), event->type == ECSPanelEventType_Focused ? "focused" : "unfocused");
        break;

    case ECSPanelEventType_Shown:
    case ECSPanelEventType_Resized:
        ECS_Log(SKETCH.plugin, ECSLogLevel_Debug, "%s is %s at %.0fx%.0f.", ECSPanel_GetTitle(canvas->panel), event->type == ECSPanelEventType_Shown ? "shown" : "resized", (double)event->size.width, (double)event->size.height);
        break;

    case ECSPanelEventType_Hidden:
        ECS_Log(SKETCH.plugin, ECSLogLevel_Debug, "%s is hidden.", ECSPanel_GetTitle(canvas->panel));
        break;

    case ECSPanelEventType_Drop:
        SketchCanvasDrop(canvas, event->drop.data);
        break;

    default:
        break;
    }
}

static SHUResult SketchCanvasSaveState(void *state, ECSValue *retState)
{
    SketchCanvas *canvas = state;
    ECSValue *strokes = NULL;
    SHU_ReturnResult(ECSValue_TableSetField(retState, "strokes", &strokes));
    ECSValue_SetTable(strokes);

    for (usz i = 0; i < canvas->strokeCount; i++)
    {
        const SketchStroke *stroke = &canvas->strokes[i];
        ECSValue *saved = NULL;
        ECSValue *field = NULL;
        ECSValue *points = NULL;

        SHU_ReturnResult(ECSValue_ListAddItem(strokes, &saved));
        SHU_ReturnResult(ECSValue_TableSetField(saved, "size", &field));
        ECSValue_SetInteger(field, stroke->size);
        SHU_ReturnResult(ECSValue_TableSetField(saved, "color", &field));
        ECSValue_SetInteger(field, stroke->color);
        SHU_ReturnResult(ECSValue_TableSetField(saved, "points", &points));
        ECSValue_SetTable(points);

        for (usz p = 0; p < stroke->count; p++)
        {
            SHU_ReturnResult(ECSValue_ListAddItem(points, &field));
            ECSValue_SetNumber(field, stroke->points[p]);
        }
    }

    return SHUResult_Ok;
}

/// @brief Saves a canvas's unsaved work. Its strokes are already in its saved state, so saving only counts it.
static SHUResult SketchCanvasSave(void *state)
{
    SketchCanvas *canvas = state;
    canvas->unsaved = false;
    SKETCH.saves++;
    ECSPanel_SetUnsaved(canvas->panel, false);
    ECS_Log(SKETCH.plugin, ECSLogLevel_Info, "%s is saved.", ECSPanel_GetTitle(canvas->panel));
    return SHUResult_Ok;
}

#pragma endregion Canvas

#pragma region Clock

static SHUResult SketchClockCreate(ECSPanel panel, const ECSValue *savedState, u32 version, void **retState)
{
    (void)savedState;
    (void)version;

    SketchClock *clock = calloc(1, sizeof(SketchClock));

    if (clock == NULL)
    {
        return SHUResult_ErrAllocation;
    }

    clock->panel = panel;
    clock->shownSeconds = -1;
    *retState = clock;
    return SHUResult_Ok;
}

static void SketchClockDestroy(void *state)
{
    free(state);
}

/// @brief Draws a dark face with twelve marks and a hand that turns once a minute; a continuous panel, so it is drawn every frame.
static void SketchClockDraw(void *state, ECSSurface *surface, f64 seconds)
{
    SketchClock *clock = state;
    clock->time += seconds;

    for (i32 y = 0; y < surface->height; y++)
    {
        u32 *row = (u32 *)((u8 *)surface->pixels.data + (usz)y * (usz)surface->pitch);

        for (i32 x = 0; x < surface->width; x++)
        {
            row[x] = SKETCH_BACKGROUND;
        }
    }

    f32 cx = (f32)surface->width / 2.0f;
    f32 cy = (f32)surface->height / 2.0f;
    f32 radius = (cx < cy ? cx : cy) * 0.8f;
    u32 *pixels = surface->pixels.data;
    i32 stride = surface->pitch / 4;

    // the marks and the hand are dabs along lines from the centre
    for (i32 mark = 0; mark < 12; mark++)
    {
        f32 angle = (f32)mark * 3.14159265f / 6.0f;
        f32 x = cx + sinf(angle) * radius;
        f32 y = cy - cosf(angle) * radius;

        for (i32 dy = -2; dy <= 2; dy++)
        {
            for (i32 dx = -2; dx <= 2; dx++)
            {
                i32 px = (i32)x + dx;
                i32 py = (i32)y + dy;

                if (px >= 0 && py >= 0 && px < surface->width && py < surface->height)
                {
                    pixels[py * stride + px] = SKETCH_COLORS[0];
                }
            }
        }
    }

    f32 angle = (f32)(fmod(clock->time, 60.0) / 60.0 * 2.0 * 3.14159265);

    for (f32 t = 0.0f; t < radius; t += 1.0f)
    {
        i32 px = (i32)(cx + sinf(angle) * t);
        i32 py = (i32)(cy - cosf(angle) * t);

        if (px >= 0 && py >= 0 && px < surface->width && py < surface->height)
        {
            pixels[py * stride + px] = SKETCH_CLOCK_HAND;
        }
    }

    // the title changes once a second, not every frame
    i64 whole = (i64)clock->time;

    if (whole != clock->shownSeconds)
    {
        char title[32];
        snprintf(title, sizeof(title), "C clock %lld:%02lld", (long long)(whole / 60), (long long)(whole % 60));
        ECSPanel_SetTitle(clock->panel, title);
        clock->shownSeconds = whole;
    }
}

#pragma endregion Clock

#pragma region Export

static void SketchExportFree(SketchExportJob *job)
{
    SketchFreeStrokes(job->strokes, job->strokeCount);
    free(job->path);
    free(job);
}

/// @brief Writes pixels as a binary PPM file.
static bool SketchWritePpm(const char *path, const u32 *pixels, i32 width, i32 height)
{
    FILE *file = fopen(path, "wb");

    if (file == NULL)
    {
        return false;
    }

    fprintf(file, "P6\n%d %d\n255\n", width, height);

    for (usz i = 0; i < (usz)width * (usz)height; i++)
    {
        u8 rgb[3] = {(u8)(pixels[i] >> 16), (u8)(pixels[i] >> 8), (u8)pixels[i]};
        fwrite(rgb, 1, sizeof(rgb), file);
    }

    return fclose(file) == 0;
}

/// @brief Logs, on the main thread, that the worker painted the canvas and is writing it.
static void SketchExportProgress(void *data)
{
    char *message = data;
    ECS_Log(SKETCH.plugin, ECSLogLevel_Debug, "%s", message);
    free(message);
}

/// @brief Paints and writes the canvas on a worker thread. Only thread-safe functions are called here.
static void SketchExportWork(void *data)
{
    SketchExportJob *job = data;
    u32 *pixels = malloc((usz)job->width * (usz)job->height * sizeof(u32));

    if (pixels == NULL)
    {
        return;
    }

    SketchPaint(pixels, job->width, job->height, job->strokes, job->strokeCount);

    char *message = malloc(256);

    if (message != NULL)
    {
        snprintf(message, 256, "Canvas %u is painted; writing %s.", job->panelId, job->path);

        if (ECS_RunOnMainThread(SketchExportProgress, message))
        {
            free(message);
        }
    }

    job->written = SketchWritePpm(job->path, pixels, job->width, job->height);
    free(pixels);
}

/// @brief Reports the export on the main thread, and finds the canvas again by its id, because it may have closed.
static void SketchExportDone(void *data)
{
    SketchExportJob *job = data;
    ECSPanel panel = ECSLayout_FindPanel(job->panelId);
    ECS_Log(SKETCH.plugin, job->written ? ECSLogLevel_Info : ECSLogLevel_Warning, "%s %s %s.", panel == NULL ? "A closed canvas" : ECSPanel_GetTitle(panel), job->written ? "was exported to" : "could not be exported to", job->path);
    SketchExportFree(job);
}

/// @brief Starts the export of a canvas to the file the user chose.
static void SketchExportChosen(void *data, const char *const *files, usz count)
{
    u32 panelId = (u32)(uintptr_t)data;
    ECSPanel panel = ECSLayout_FindPanel(panelId);
    SketchCanvas *canvas = panel == NULL || count == 0 ? NULL : SketchFindCanvas(panel);

    if (files == NULL || canvas == NULL || canvas->width <= 0)
    {
        return;
    }

    // the worker gets copies, because the canvas may change or close meanwhile
    SketchExportJob *job = calloc(1, sizeof(SketchExportJob));
    usz capacity = 0;

    if (job == NULL || (job->path = strdup(files[0])) == NULL)
    {
        free(job);
        return;
    }

    job->panelId = panelId;
    job->width = canvas->width;
    job->height = canvas->height;

    for (usz i = 0; i < canvas->strokeCount; i++)
    {
        const SketchStroke *source = &canvas->strokes[i];
        SketchStroke *stroke = SketchAddStroke(&job->strokes, &job->strokeCount, &capacity, source->size, source->color);

        for (usz p = 0; stroke != NULL && p + 1 < source->count; p += 2)
        {
            SketchStrokeAddPoint(stroke, source->points[p], source->points[p + 1]);
        }
    }

    if (ECS_RunInBackground(SKETCH.plugin, SketchExportWork, SketchExportDone, job))
    {
        ECS_Log(SKETCH.plugin, ECSLogLevel_Warning, "Cannot start the export.");
        SketchExportFree(job);
    }
}

#pragma endregion Export

#pragma region Services

static i32 SketchStrokeCount(ECSPanel panel)
{
    SketchCanvas *canvas = SketchFindCanvas(panel);
    return canvas == NULL ? 0 : (i32)canvas->strokeCount;
}

/// @brief Clears a canvas. If it has unsaved strokes, the user is asked first.
static void SketchClear(ECSPanel panel)
{
    SketchCanvas *canvas = SketchFindCanvas(panel);

    if (canvas == NULL || canvas->strokeCount == 0)
    {
        return;
    }

    const char *const buttons[] = {"Clear", "Keep"};
    usz button = 0;

    // without a dialog, the canvas is cleared
    if (canvas->unsaved && ECSDialog_ShowMessage("Clear canvas", "This canvas has unsaved strokes. Clear them?", buttons, 2, &button) == SHUResult_Ok && button != 0)
    {
        return;
    }

    SketchFreeStrokes(canvas->strokes, canvas->strokeCount);
    canvas->strokes = NULL;
    canvas->strokeCount = 0;
    canvas->strokeCapacity = 0;
    SketchCanvasChanged(canvas);
}

static void SketchExport(ECSPanel panel)
{
    static const ECSDialogFilter filters[] = {{"PPM images", "ppm"}};

    ECSDialogDesc desc = {
        .type = ECSDialogType_SaveFile,
        .filters = filters,
        .filterCount = 1,
        .location = "canvas.ppm",
        .Done = SketchExportChosen,
        .data = (void *)(uintptr_t)ECSPanel_GetId(panel),
    };

    if (SketchFindCanvas(panel) != NULL && ECSDialog_Show(SKETCH.plugin, &desc))
    {
        ECS_Log(SKETCH.plugin, ECSLogLevel_Warning, "Cannot show the export dialog.");
    }
}

static void SketchCopy(ECSPanel panel)
{
    SketchCanvas *canvas = SketchFindCanvas(panel);
    char *text = canvas == NULL ? NULL : SketchStrokesToText(canvas->strokes, canvas->strokeCount);

    if (text != NULL && ECSClipboard_SetData(SKETCH_CLIPBOARD_TYPE, (SHUSliceView){.data = text, .size = strlen(text)}) == SHUResult_Ok)
    {
        ECS_Log(SKETCH.plugin, ECSLogLevel_Info, "Copied %zu strokes.", canvas->strokeCount);
    }

    free(text);
}

/// @brief Pastes strokes from the clipboard: strokes data if there is some, otherwise text in the same format.
static void SketchPaste(ECSPanel panel)
{
    SketchCanvas *canvas = SketchFindCanvas(panel);
    SHUSlice data = {0};

    if (canvas == NULL)
    {
        return;
    }

    usz added = 0;

    if (ECSClipboard_GetData(SKETCH_CLIPBOARD_TYPE, &data) == SHUResult_Ok)
    {
        added = SketchStrokesFromText(canvas, csv(data));
    }
    else
    {
        const char *text = ECSClipboard_GetText();
        added = text == NULL ? 0 : SketchStrokesFromText(canvas, (SHUSliceView){.data = text, .size = strlen(text)});
    }

    ECS_Log(SKETCH.plugin, ECSLogLevel_Info, "Pasted %zu strokes.", added);

    if (added > 0)
    {
        SketchCanvasChanged(canvas);
    }
}

/// @brief Opens a canvas with the strokes of a file in the text format of copy; the function a preset opens files with.
static void SketchOpen(const char *path)
{
    usz size = 0;
    char *text = SketchReadFile(path, &size);
    ECSPanel panel = NULL;

    if (text == NULL || ECSLayout_Open(SKETCH.plugin, &panel, SKETCH_NAME("canvas"), NULL, NULL, ECSZone_Default))
    {
        ECS_Log(SKETCH.plugin, ECSLogLevel_Warning, "Cannot open '%s'.", path);
        free(text);
        return;
    }

    SketchCanvas *canvas = SketchFindCanvas(panel);
    usz added = canvas != NULL ? SketchStrokesFromText(canvas, (SHUSliceView){.data = text, .size = size}) : 0;
    free(text);

    if (canvas == NULL)
    {
        return;
    }

    ECS_Log(SKETCH.plugin, ECSLogLevel_Info, "Opened %zu strokes from '%s'.", added, path);
    SketchCanvasTitle(canvas);
    ECSPanel_Redraw(panel);
}

static void SketchOpenBeside(ECSPanel panel)
{
    ECSPanel opened = NULL;

    if (ECSLayout_Open(SKETCH.plugin, &opened, SKETCH_NAME("canvas"), NULL, panel, ECSZone_Right))
    {
        ECS_Log(SKETCH.plugin, ECSLogLevel_Warning, "Cannot open a canvas.");
    }
}

/// @brief Moves every other canvas into the group of a panel, and focuses the panel.
static void SketchGather(ECSPanel panel)
{
    for (usz i = 0; i < SKETCH.canvasCount; i++)
    {
        if (SKETCH.canvases[i]->panel != panel && ECSLayout_Move(SKETCH.canvases[i]->panel, panel, ECSZone_Center))
        {
            ECS_Log(SKETCH.plugin, ECSLogLevel_Warning, "Cannot move %s.", ECSPanel_GetTitle(SKETCH.canvases[i]->panel));
        }
    }

    ECSLayout_Focus(panel);
}

/// @brief Closes every other canvas; each may ask about unsaved work.
static i32 SketchCloseOthers(ECSPanel panel)
{
    i32 closed = 0;

    // a closed canvas is destroyed after this call, so the list does not change while it is walked
    for (usz i = 0; i < SKETCH.canvasCount; i++)
    {
        closed += SKETCH.canvases[i]->panel != panel && ECSLayout_Close(SKETCH.canvases[i]->panel) ? 1 : 0;
    }

    return closed;
}

static void SketchNextWorkspace(void)
{
    usz next = ECSWorkspace_GetCurrent() % ECSWorkspace_GetCount() + 1;
    ECSWorkspace_Switch(next);
    ECS_Log(SKETCH.plugin, ECSLogLevel_Info, "Workspace %zu, '%s'.", next, ECSWorkspace_GetName(next));
}

/// @brief Calls a function with every open canvas and its number of strokes. The function is valid only during the call.
/// @return The number of open canvases.
static i32 SketchEachCanvas(SketchCanvasFunction function)
{
    for (usz i = 0; function != NULL && i < SKETCH.canvasCount; i++)
    {
        function(SKETCH.canvases[i]->panel, (i32)SKETCH.canvases[i]->strokeCount);
    }

    return (i32)SKETCH.canvasCount;
}

/// @brief Gives the number of strokes drawn in every session, and fills a table of numbers about the plugin.
static i32 SketchStats(ECSValue *retStats)
{
    ECSValue *field = NULL;
    ECSValue *settings = NULL;
    ECSPanel focus = ECSLayout_GetFocus();
    i64 own = 0;

    // the plugin's settings are counted from the list of every setting
    if (ECSValue_Create(&settings) == SHUResult_Ok && ECSSetting_List(settings) == SHUResult_Ok)
    {
        for (usz i = 0; i < ECSValue_GetListCount(settings); i++)
        {
            own += strncmp(ECSValue_GetString(ECSValue_GetListItem(settings, i), ""), "sketch_c.", 9) == 0 ? 1 : 0;
        }
    }

    ECSValue_Destroy(&settings);

    const struct
    {
        const char *name;
        i64 value;
    } numbers[] = {
        {"canvases", SKETCH.openCanvases},
        {"strokes", SKETCH.total},
        {"saves", SKETCH.saves},
        {"settings", own},
        {"focus", focus == NULL ? 0 : ECSPanel_GetId(focus)},
        {"workspace", (i64)ECSWorkspace_GetCurrent()},
    };

    for (usz i = 0; i < sizeof(numbers) / sizeof(*numbers); i++)
    {
        if (ECSValue_TableSetField(retStats, numbers[i].name, &field) == SHUResult_Ok)
        {
            ECSValue_SetInteger(field, numbers[i].value);
        }
    }

    return (i32)SKETCH.total;
}

/// @brief Gives a canvas's pixels as a PPM image. The buffer is valid until the next call.
static SHUSlice SketchPixels(ECSPanel panel)
{
    SketchCanvas *canvas = SketchFindCanvas(panel);

    if (canvas == NULL || canvas->width <= 0)
    {
        return (SHUSlice){0};
    }

    usz count = (usz)canvas->width * (usz)canvas->height;
    u32 *pixels = malloc(count * sizeof(u32));
    char header[32];
    usz headerLength = (usz)snprintf(header, sizeof(header), "P6\n%d %d\n255\n", canvas->width, canvas->height);
    u8 *buffer = realloc(SKETCH.pixels, headerLength + count * 3);

    if (pixels == NULL || buffer == NULL)
    {
        free(pixels);
        SKETCH.pixels = buffer;
        return (SHUSlice){0};
    }

    SKETCH.pixels = buffer;
    SketchPaint(pixels, canvas->width, canvas->height, canvas->strokes, canvas->strokeCount);
    memcpy(buffer, header, headerLength);

    for (usz i = 0; i < count; i++)
    {
        buffer[headerLength + i * 3] = (u8)(pixels[i] >> 16);
        buffer[headerLength + i * 3 + 1] = (u8)(pixels[i] >> 8);
        buffer[headerLength + i * 3 + 2] = (u8)pixels[i];
    }

    free(pixels);
    return (SHUSlice){.data = buffer, .size = headerLength + count * 3};
}

/// @brief Makes a brush, which a canvas can use instead of the settings' brush.
static SketchBrush *SketchNewBrush(i32 size, const char *color)
{
    SketchBrush *brush = calloc(1, sizeof(SketchBrush));

    if (brush == NULL)
    {
        return NULL;
    }

    brush->size = size < SKETCH_MIN_SIZE ? SKETCH_MIN_SIZE : size > SKETCH_MAX_SIZE ? SKETCH_MAX_SIZE
                                                                                    : size;
    brush->color = SKETCH_COLORS[0];
    brush->references = 1;

    for (usz i = 0; SKETCH_COLOR_NAMES[i] != NULL; i++)
    {
        brush->color = strcmp(SKETCH_COLOR_NAMES[i], color) == 0 ? SKETCH_COLORS[i] : brush->color;
    }

    return brush;
}

/// @brief Destroys a brush handle; a canvas that uses the brush keeps it.
static void SketchBrushDestroy(void *object)
{
    SketchReleaseBrush(object);
}

static void SketchUseBrush(SketchBrush *brush, ECSPanel panel)
{
    SketchCanvas *canvas = SketchFindCanvas(panel);

    if (canvas != NULL && brush != NULL)
    {
        brush->references++;
        SketchReleaseBrush(canvas->brush);
        canvas->brush = brush;
    }
}

#pragma endregion Services

#pragma region Events

/// @brief Redraws every canvas when a brush setting changes.
static void SketchBrushChanged(void *data)
{
    (void)data;

    for (usz i = 0; i < SKETCH.canvasCount; i++)
    {
        ECSPanel_Redraw(SKETCH.canvases[i]->panel);
    }

    ECS_Log(SKETCH.plugin, ECSLogLevel_Debug, "The brush is %d, %s.", SketchSettingSize(), ECSValue_GetString(ECSSetting_Get(SKETCH_NAME("brushColor")), "?"));
}

static void SketchOnEvent(void *data, const char *name, const ECSValue *value)
{
    (void)data;
    const char *type = ECSValue_GetString(ECSValue_GetTableField(value, "type"), "");

    if (strcmp(name, SKETCH_NAME("strokeAdded")) == 0)
    {
        SKETCH.total++;
    }
    else if (strcmp(name, "ecs.panelOpened") == 0 && strcmp(type, SKETCH_NAME("canvas")) == 0)
    {
        SKETCH.openCanvases++;
    }
    else if (strcmp(name, "ecs.panelClosed") == 0 && strcmp(type, SKETCH_NAME("canvas")) == 0)
    {
        SKETCH.openCanvases--;
    }
    else if (strcmp(name, "ecs.workspaceSwitched") == 0)
    {
        i64 number = ECSValue_GetInteger(ECSValue_GetTableField(value, "workspace"), 0);
        ECS_Log(SKETCH.plugin, ECSLogLevel_Debug, "Workspace %lld, '%s', is shown.", (long long)number, ECSWorkspace_GetName((usz)number));
    }
}

/// @brief Logs a canvas's number of strokes; a callback of sketch_c.eachCanvas.
static void SketchLogCanvas(ECSPanel panel, i32 strokes)
{
    ECS_Log(SKETCH.plugin, ECSLogLevel_Debug, "%s has %d strokes.", ECSPanel_GetTitle(panel), strokes);
}

/// @brief Logs the numbers of sketch_c.stats now and then, and each canvas through sketch_c.eachCanvas; a plugin timer.
static void SketchLogStats(void *data)
{
    (void)data;
    ECSValue *stats = NULL;

    if (ECSValue_Create(&stats) == SHUResult_Ok)
    {
        i32 total = SketchStats(stats);
        ECS_Log(SKETCH.plugin, ECSLogLevel_Debug, "%d strokes in all, %lld canvases open.", total, (long long)ECSValue_GetInteger(ECSValue_GetTableField(stats, "canvases"), 0));
    }

    ECSValue_Destroy(&stats);
    SKETCH.eachCanvas(SketchLogCanvas);
}

static SHUResult SketchSaveState(void *data, ECSValue *retState)
{
    (void)data;
    ECSValue *field = NULL;
    SHU_ReturnResult(ECSValue_TableSetField(retState, "total", &field));
    ECSValue_SetInteger(field, SKETCH.total);
    SHU_ReturnResult(ECSValue_TableSetField(retState, "saves", &field));
    ECSValue_SetInteger(field, SKETCH.saves);
    return SHUResult_Ok;
}

static SHUResult SketchRestoreState(void *data, const ECSValue *state, u32 version)
{
    (void)data;
    (void)version;
    SKETCH.total = ECSValue_GetInteger(ECSValue_GetTableField(state, "total"), 0);
    SKETCH.saves = ECSValue_GetInteger(ECSValue_GetTableField(state, "saves"), 0);
    ECS_Log(SKETCH.plugin, ECSLogLevel_Info, "Restored: %lld strokes and %lld saves so far.", (long long)SKETCH.total, (long long)SKETCH.saves);
    return SHUResult_Ok;
}

#pragma endregion Events

#pragma endregion Source Only

SHUResult ECSPlugin_Init(ECSPlugin plugin)
{
    SKETCH.plugin = plugin;

    const ECSSettingDesc settings[] = {
        {.name = SKETCH_NAME("brushSize"), .type = ECSSettingType_Integer, .description = "Size of the brush, in pixels; the wheel over a canvas changes it", .defaultInteger = 6, .Changed = SketchBrushChanged},
        {.name = SKETCH_NAME("brushColor"), .type = ECSSettingType_Choice, .description = "Colour of the brush", .defaultString = "white", .choices = SKETCH_COLOR_NAMES, .Changed = SketchBrushChanged},
        {.name = SKETCH_NAME("reminderSeconds"), .type = ECSSettingType_Number, .description = "How often a canvas with unsaved strokes says so", .defaultNumber = 30.0},
    };

    for (usz i = 0; i < sizeof(settings) / sizeof(*settings); i++)
    {
        SHU_ReturnResult(ECSSetting_Declare(plugin, &settings[i]));
    }

    const ECSPanelTypeDesc canvas = {
        .name = SKETCH_NAME("canvas"),
        .title = "C canvas",
        .stateVersion = SKETCH_STATE_VERSION,
        .minWidth = 64.0f,
        .minHeight = 64.0f,
        .Create = SketchCanvasCreate,
        .Destroy = SketchCanvasDestroy,
        .Draw = SketchCanvasDraw,
        .Event = SketchCanvasEvent,
        .SaveState = SketchCanvasSaveState,
        .Save = SketchCanvasSave,
    };

    const ECSPanelTypeDesc clock = {
        .name = SKETCH_NAME("clock"),
        .title = "C clock",
        .continuous = true,
        .Create = SketchClockCreate,
        .Destroy = SketchClockDestroy,
        .Draw = SketchClockDraw,
    };

    SHU_ReturnResult(ECSPanelType_Register(plugin, &canvas));
    SHU_ReturnResult(ECSPanelType_Register(plugin, &clock));
    SHU_ReturnResult(ECSHandle_RegisterType(plugin, SKETCH_NAME("brush"), SketchBrushDestroy));

    const struct
    {
        const char *name;
        ECSFunction function;
        const char *signature;
        const char *description;
    } services[] = {
        {SKETCH_NAME("strokeCount"), (ECSFunction)SketchStrokeCount, "int(handle<ecs.panel>)", "Counts a canvas's strokes"},
        {SKETCH_NAME("clear"), (ECSFunction)SketchClear, "void(handle<ecs.panel>)", "Clear the canvas"},
        {SKETCH_NAME("export"), (ECSFunction)SketchExport, "void(handle<ecs.panel>)", "Export the canvas as an image"},
        {SKETCH_NAME("copy"), (ECSFunction)SketchCopy, "void(handle<ecs.panel>)", "Copy the canvas's strokes"},
        {SKETCH_NAME("paste"), (ECSFunction)SketchPaste, "void(handle<ecs.panel>)", "Paste strokes"},
        {SKETCH_NAME("open"), (ECSFunction)SketchOpen, "void(string)", "Open a canvas with the strokes of a file"},
        {SKETCH_NAME("openBeside"), (ECSFunction)SketchOpenBeside, "void(handle<ecs.panel>)", "Open a canvas beside this one"},
        {SKETCH_NAME("gather"), (ECSFunction)SketchGather, "void(handle<ecs.panel>)", "Gather every canvas into this group"},
        {SKETCH_NAME("closeOthers"), (ECSFunction)SketchCloseOthers, "int(handle<ecs.panel>)", "Close the other canvases"},
        {SKETCH_NAME("nextWorkspace"), (ECSFunction)SketchNextWorkspace, "void()", "Switch to the next workspace"},
        {SKETCH_NAME("stats"), (ECSFunction)SketchStats, "int(out value)", "Counts strokes, canvases and saves"},
        {SKETCH_NAME("eachCanvas"), (ECSFunction)SketchEachCanvas, "int(fn<void(handle<ecs.panel>, int)>)", "Calls a function with every canvas and its number of strokes"},
        {SKETCH_NAME("pixels"), (ECSFunction)SketchPixels, "buffer(handle<ecs.panel>)", "Gives a canvas as a PPM image"},
        {SKETCH_NAME("brush"), (ECSFunction)SketchNewBrush, "handle<sketch_c.brush>(int, string)", "Makes a brush of a size and a colour"},
        {SKETCH_NAME("useBrush"), (ECSFunction)SketchUseBrush, "void(handle<sketch_c.brush>, handle<ecs.panel>)", "Makes a canvas draw with a brush"},
    };

    for (usz i = 0; i < sizeof(services) / sizeof(*services); i++)
    {
        SHU_ReturnResult(ECSService_RegisterFunction(plugin, services[i].name, services[i].function, services[i].signature, services[i].description));
    }

    // default keys that work while a canvas has the focus; the canvas's menu shows the same functions with their keys
    const char *const bindings[][2] = {
        {"Delete", SKETCH_NAME("clear")},
        {"Ctrl+E", SKETCH_NAME("export")},
        {"Ctrl+C", SKETCH_NAME("copy")},
        {"Ctrl+V", SKETCH_NAME("paste")},
        {"Ctrl+B", SKETCH_NAME("openBeside")},
        {"Ctrl+G", SKETCH_NAME("gather")},
    };

    for (usz i = 0; i < sizeof(bindings) / sizeof(*bindings); i++)
    {
        SHU_ReturnResult(ECSKey_Bind(plugin, SKETCH_NAME("canvas"), bindings[i][0], bindings[i][1]));
        SHU_ReturnResult(ECSPanelType_AddMenuEntry(plugin, SKETCH_NAME("canvas"), bindings[i][1]));
    }

    SHU_ReturnResult(ECSEvent_Declare(plugin, SKETCH_NAME("strokeAdded"), "A canvas has a new stroke: { panel = id, strokes = count }"));

    const char *const events[] = {SKETCH_NAME("strokeAdded"), "ecs.panelOpened", "ecs.panelClosed", "ecs.workspaceSwitched"};

    for (usz i = 0; i < sizeof(events) / sizeof(*events); i++)
    {
        ECSSubscription subscription = NULL;
        SHU_ReturnResult(ECSEvent_Subscribe(plugin, events[i], &subscription, SketchOnEvent, NULL));
    }

    const ECSPluginStateDesc state = {.version = SKETCH_STATE_VERSION, .Save = SketchSaveState, .Restore = SketchRestoreState};
    SHU_ReturnResult(ECSPlugin_RegisterState(plugin, &state));
    SHU_ReturnResult(ECSTimer_Start(plugin, &SKETCH.statsTimer, 60.0, true, SketchLogStats, NULL));

    // a service is called through its lookup like any other plugin would call it
    SketchStatsFunction stats = NULL;
    ECSValue *numbers = NULL;
    ECSValue *explanation = NULL;
    SHU_ReturnResult(ECSService_GetFunction(plugin, (ECSFunction *)&stats, SKETCH_NAME("stats"), "int(out value)"));
    SHU_ReturnResult(ECSService_GetFunction(plugin, (ECSFunction *)&SKETCH.eachCanvas, SKETCH_NAME("eachCanvas"), "int(fn<void(handle<ecs.panel>, int)>)"));
    SHU_ReturnResult(ECSValue_Create(&numbers));
    SHU_ReturnResult(ECSValue_Create(&explanation), ECSValue_Destroy(&numbers););

    if (ECSSetting_Explain(SKETCH_NAME("brushSize"), explanation) == SHUResult_Ok)
    {
        ECS_Log(plugin, ECSLogLevel_Debug, "brushSize comes from the %s layer.", ECSValue_GetString(ECSValue_GetTableField(explanation, "layer"), "?"));
    }

    i32 total = stats(numbers);
    ECS_Log(plugin, ECSLogLevel_Info, "Ready with %lld settings; %d strokes so far.", (long long)ECSValue_GetInteger(ECSValue_GetTableField(numbers, "settings"), 0), total);
    ECSValue_Destroy(&explanation);
    ECSValue_Destroy(&numbers);
    return SHUResult_Ok;
}

void ECSPlugin_Shutdown(ECSPlugin plugin)
{
    ECSTimer_Stop(&SKETCH.statsTimer);
    ECS_Log(plugin, ECSLogLevel_Info, "Goodbye after %lld strokes.", (long long)SKETCH.total);
    free(SKETCH.canvases);
    free(SKETCH.pixels);
    SKETCH.canvases = NULL;
    SKETCH.pixels = NULL;
}
