// Panels for the tests, in C: boxes of one colour that count the clicks on them. The boxes plugin does the same in Lua.

#include "OpenECS.h"

#include <stdlib.h>

#pragma region Source Only

#define BOXES_COLOR 0xFFB48EAD
#define BOXES_MAX 64

/// @brief A box panel.
typedef struct Box
{
    ECSPanel panel;
    i64 clicks;
} Box;

/// @brief The type of a callback of cboxes.each: a box and its clicks.
typedef void (*BoxFunction)(ECSPanel panel, i32 clicks);

static struct
{
    Box *boxes[BOXES_MAX]; // every open box
    usz count;
} BOXES = {0};

static SHUResult BoxCreate(ECSPanel panel, const ECSValue *savedState, u32 version, void **retState)
{
    (void)version;

    if (BOXES.count == BOXES_MAX)
    {
        return SHUResult_ErrOverflow;
    }

    Box *box = calloc(1, sizeof(Box));

    if (box == NULL)
    {
        return SHUResult_ErrAllocation;
    }

    box->panel = panel;
    box->clicks = ECSValue_GetInteger(ECSValue_GetTableField(savedState, "clicks"), 0);
    BOXES.boxes[BOXES.count++] = box;
    *retState = box;
    return SHUResult_Ok;
}

static void BoxDestroy(void *state)
{
    for (usz i = 0; i < BOXES.count; i++)
    {
        if (BOXES.boxes[i] == state)
        {
            BOXES.boxes[i] = BOXES.boxes[--BOXES.count];
        }
    }

    free(state);
}

static void BoxDraw(void *state, ECSSurface *surface, f64 seconds)
{
    (void)state;
    (void)seconds;

    for (i32 y = 0; y < surface->height; y++)
    {
        u32 *row = (u32 *)((u8 *)surface->pixels.data + (usz)y * (usz)surface->pitch);

        for (i32 x = 0; x < surface->width; x++)
        {
            row[x] = BOXES_COLOR;
        }
    }
}

static void BoxEvent(void *state, const ECSPanelEvent *event)
{
    if (event->type == ECSPanelEventType_PointerDown)
    {
        ((Box *)state)->clicks++;
    }
}

static SHUResult BoxSaveState(void *state, ECSValue *retState)
{
    ECSValue *field = NULL;
    ECSValue_SetTable(retState);
    SHU_ReturnResult(ECSValue_TableSetField(retState, "clicks", &field));
    ECSValue_SetInteger(field, ((Box *)state)->clicks);
    return SHUResult_Ok;
}

/// @brief cboxes.each: calls a function with every box and its clicks.
static i32 BoxesEach(BoxFunction function)
{
    for (usz i = 0; function != NULL && i < BOXES.count; i++)
    {
        function(BOXES.boxes[i]->panel, (i32)BOXES.boxes[i]->clicks);
    }

    return (i32)BOXES.count;
}

#pragma endregion Source Only

SHUResult ECSPlugin_Init(ECSPlugin plugin)
{
    const ECSPanelTypeDesc box = {
        .name = "cboxes.box",
        .title = "C box",
        .stateVersion = 1,
        .Create = BoxCreate,
        .Destroy = BoxDestroy,
        .Draw = BoxDraw,
        .Event = BoxEvent,
        .SaveState = BoxSaveState,
    };

    SHU_ReturnResult(ECSPanelType_Register(plugin, &box));
    return ECSService_RegisterFunction(plugin, "cboxes.each", (ECSFunction)BoxesEach, "int(fn<void(handle<ecs.panel>, int)>)", "Calls a function with every box and its clicks");
}
