// Example panels for trying out the layout: a colour that changes on click, an animated gradient, a checkerboard and a colour that blinks on a timer.

#include "OpenECS.h"

#include <math.h>

#pragma region Source Only

/// @brief Colours of the color panel, in ARGB8888.
static const u32 DEMO_COLORS[] = {0xFF2E3440, 0xFF5E81AC, 0xFFA3BE8C, 0xFFB48EAD, 0xFFD08770, 0xFFEBCB8B};

/// @brief State of one demo panel.
typedef struct DemoPanel
{
    ECSPanel panel;
    usz color;
    f64 time;
    f32 pointerX;
    f32 pointerY;
    i32 width;
    i32 height;
    ECSTimer timer;
    u32 ticks;
} DemoPanel;

static u32 *DemoRow(ECSSurface *surface, i32 y)
{
    return (u32 *)((u8 *)surface->pixels.data + (usz)y * (usz)surface->pitch);
}

static SHUResult DemoCreate(ECSPanel panel, const ECSValue *savedState, u32 version, void **retState)
{
    (void)version;

    DemoPanel *demo = calloc(1, sizeof(DemoPanel));

    if (demo == NULL)
    {
        return SHUResult_ErrAllocation;
    }

    // a missing or wrong colour in the saved state falls back to the first one
    i64 color = ECSValue_GetInteger(ECSValue_GetField(savedState, "color"), 0);
    demo->color = color >= 0 && (usz)color < sizeof(DEMO_COLORS) / sizeof(*DEMO_COLORS) ? (usz)color : 0;
    demo->panel = panel;
    demo->pointerX = -1.0f;
    demo->pointerY = -1.0f;
    *retState = demo;
    return SHUResult_Ok;
}

static void DemoDestroy(void *state)
{
    free(state);
}

#pragma region Color

static void DemoColorDraw(void *state, ECSSurface *surface, f64 seconds)
{
    (void)seconds;
    DemoPanel *demo = state;
    u32 color = DEMO_COLORS[demo->color];

    for (i32 y = 0; y < surface->height; y++)
    {
        u32 *row = DemoRow(surface, y);

        for (i32 x = 0; x < surface->width; x++)
        {
            bool cross = (i32)demo->pointerX == x || (i32)demo->pointerY == y;
            row[x] = cross ? 0xFFFFFFFF : color;
        }
    }
}

static void DemoColorEvent(void *state, const ECSEvent *event)
{
    DemoPanel *demo = state;

    if (event->type == ECSEventType_PointerDown)
    {
        demo->color = (demo->color + 1) % (sizeof(DEMO_COLORS) / sizeof(*DEMO_COLORS));
    }
    else if (event->type == ECSEventType_PointerMove)
    {
        demo->pointerX = event->x;
        demo->pointerY = event->y;
    }
    else
    {
        return;
    }

    ECSPanel_Redraw(demo->panel);
}

#pragma endregion Color

#pragma region Gradient

static void DemoGradientDraw(void *state, ECSSurface *surface, f64 seconds)
{
    DemoPanel *demo = state;
    demo->time += seconds;

    f64 shift = demo->time * 60.0;

    for (i32 y = 0; y < surface->height; y++)
    {
        u32 *row = DemoRow(surface, y);
        u32 green = (u32)(y * 255 / (surface->height > 1 ? surface->height - 1 : 1));

        for (i32 x = 0; x < surface->width; x++)
        {
            u32 red = (u32)fmod(x + shift, 256.0);
            u32 blue = (u32)(128.0 + 127.0 * sin(demo->time + x * 0.01));
            row[x] = 0xFF000000 | (red << 16) | (green << 8) | blue;
        }
    }
}

#pragma endregion Gradient

#pragma region Checker

static void DemoCheckerDraw(void *state, ECSSurface *surface, f64 seconds)
{
    (void)seconds;
    DemoPanel *demo = state;

    if (demo->width != surface->width || demo->height != surface->height)
    {
        demo->width = surface->width;
        demo->height = surface->height;

        char title[64];
        snprintf(title, sizeof(title), "Checker %dx%d", surface->width, surface->height);
        ECSPanel_SetTitle(demo->panel, title);
    }

    for (i32 y = 0; y < surface->height; y++)
    {
        u32 *row = DemoRow(surface, y);

        for (i32 x = 0; x < surface->width; x++)
        {
            row[x] = ((x / 16) + (y / 16)) % 2 == 0 ? 0xFF3A3F4B : 0xFF262A33;
        }
    }
}

#pragma endregion Checker

#pragma region Blink

static void DemoBlinkTick(void *data)
{
    DemoPanel *demo = data;
    demo->ticks++;
    demo->color = (demo->color + 1) % (sizeof(DEMO_COLORS) / sizeof(*DEMO_COLORS));

    char title[64];
    snprintf(title, sizeof(title), "Blink %u", demo->ticks);
    ECSPanel_SetTitle(demo->panel, title);
    ECSPanel_Redraw(demo->panel);
}

static SHUResult DemoBlinkCreate(ECSPanel panel, const ECSValue *savedState, u32 version, void **retState)
{
    SHU_ReturnResult(DemoCreate(panel, savedState, version, retState));

    // the timer belongs to the panel, so it stops when the panel closes
    DemoPanel *demo = *retState;
    f64 seconds = ECSValue_GetNumber(ECSSetting_Get("demo.blink_seconds"), 0.5);
    SHU_ReturnResult(ECSPanel_StartTimer(panel, &demo->timer, seconds > 0.0 ? seconds : 0.5, true, DemoBlinkTick, demo), DemoDestroy(demo););
    return SHUResult_Ok;
}

static void DemoBlinkDraw(void *state, ECSSurface *surface, f64 seconds)
{
    (void)seconds;
    DemoPanel *demo = state;

    for (i32 y = 0; y < surface->height; y++)
    {
        u32 *row = DemoRow(surface, y);

        for (i32 x = 0; x < surface->width; x++)
        {
            row[x] = DEMO_COLORS[demo->color];
        }
    }
}

#pragma endregion Blink

#pragma endregion Source Only

SHUResult ECSPlugin_Init(ECSPlugin plugin)
{
    ECSPanelTypeDesc color = {
        .name = "demo.color",
        .title = "Color",
        .Create = DemoCreate,
        .Destroy = DemoDestroy,
        .Draw = DemoColorDraw,
        .Event = DemoColorEvent,
    };

    ECSPanelTypeDesc gradient = {
        .name = "demo.gradient",
        .title = "Gradient",
        .continuous = true,
        .Create = DemoCreate,
        .Destroy = DemoDestroy,
        .Draw = DemoGradientDraw,
    };

    ECSPanelTypeDesc checker = {
        .name = "demo.checker",
        .title = "Checker",
        .Create = DemoCreate,
        .Destroy = DemoDestroy,
        .Draw = DemoCheckerDraw,
    };

    ECSPanelTypeDesc blink = {
        .name = "demo.blink",
        .title = "Blink",
        .Create = DemoBlinkCreate,
        .Destroy = DemoDestroy,
        .Draw = DemoBlinkDraw,
    };

    ECSSettingDesc blinkSeconds = {
        .name = "demo.blink_seconds",
        .type = ECSSettingType_Number,
        .description = "Seconds between the colours of a blink panel",
        .defaultNumber = 0.5,
    };

    SHU_ReturnResult(ECSSetting_Declare(plugin, &blinkSeconds));
    SHU_ReturnResult(ECSPanelType_Register(plugin, &color));
    SHU_ReturnResult(ECSPanelType_Register(plugin, &gradient));
    SHU_ReturnResult(ECSPanelType_Register(plugin, &checker));
    SHU_ReturnResult(ECSPanelType_Register(plugin, &blink));

    ECS_Log(plugin, ECSLogLevel_Info, "Registered 4 panel types.");
    return SHUResult_Ok;
}
