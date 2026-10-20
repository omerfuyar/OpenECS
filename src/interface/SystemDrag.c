#include "interface/SystemDrag.h"

#pragma region Source Only

// the drag uses Wayland's own drag and drop, which SDL does not offer, so it needs libwayland-client's headers when OpenECS is built; without them, it is left out
#if defined(SDL_PLATFORM_LINUX) && __has_include(<wayland-client.h>)
#define OPENECS_WAYLAND_DRAG 1
#endif

#ifdef OPENECS_WAYLAND_DRAG

// libwayland-client is loaded when it is first needed, as SDL loads it, so OpenECS still runs where it is missing.
// Its functions and interfaces are reached through pointers, which these names stand for in its headers.
#define wl_proxy_marshal_flags (*ECSIWayland_wl_proxy_marshal_flags)
#define wl_proxy_get_version (*ECSIWayland_wl_proxy_get_version)
#define wl_proxy_add_listener (*ECSIWayland_wl_proxy_add_listener)
#define wl_proxy_set_user_data (*ECSIWayland_wl_proxy_set_user_data)
#define wl_proxy_get_user_data (*ECSIWayland_wl_proxy_get_user_data)
#define wl_proxy_destroy (*ECSIWayland_wl_proxy_destroy)
#define wl_proxy_set_queue (*ECSIWayland_wl_proxy_set_queue)
#define wl_proxy_create_wrapper (*ECSIWayland_wl_proxy_create_wrapper)
#define wl_proxy_wrapper_destroy (*ECSIWayland_wl_proxy_wrapper_destroy)
#define wl_display_create_queue (*ECSIWayland_wl_display_create_queue)
#define wl_display_roundtrip_queue (*ECSIWayland_wl_display_roundtrip_queue)
#define wl_display_dispatch_queue_pending (*ECSIWayland_wl_display_dispatch_queue_pending)
#define wl_display_flush (*ECSIWayland_wl_display_flush)
#define wl_event_queue_destroy (*ECSIWayland_wl_event_queue_destroy)
#define wl_registry_interface (*ECSIWayland_wl_registry_interface)
#define wl_seat_interface (*ECSIWayland_wl_seat_interface)
#define wl_pointer_interface (*ECSIWayland_wl_pointer_interface)
#define wl_data_device_manager_interface (*ECSIWayland_wl_data_device_manager_interface)
#define wl_data_device_interface (*ECSIWayland_wl_data_device_interface)
#define wl_data_source_interface (*ECSIWayland_wl_data_source_interface)

#include <unistd.h>
#include <wayland-client.h>

// the pointers that the names above stand for; libwayland-client's headers declare the functions' pointers already
const struct wl_interface *ECSIWayland_wl_registry_interface = nullptr;
const struct wl_interface *ECSIWayland_wl_seat_interface = nullptr;
const struct wl_interface *ECSIWayland_wl_pointer_interface = nullptr;
const struct wl_interface *ECSIWayland_wl_data_device_manager_interface = nullptr;
const struct wl_interface *ECSIWayland_wl_data_device_interface = nullptr;
const struct wl_interface *ECSIWayland_wl_data_source_interface = nullptr;

/// @brief The type that a dragged panel offers. Only OpenECS's own windows take it.
#define OPENECS_DRAG_MIME "application/x-openecs-panel"

static const struct
{
    const char *name;
    void *address; // the pointer that gets the symbol
} ECSI_WAYLAND_SYMBOLS[] = {
    {"wl_proxy_marshal_flags", &ECSIWayland_wl_proxy_marshal_flags},
    {"wl_proxy_get_version", &ECSIWayland_wl_proxy_get_version},
    {"wl_proxy_add_listener", &ECSIWayland_wl_proxy_add_listener},
    {"wl_proxy_set_user_data", &ECSIWayland_wl_proxy_set_user_data},
    {"wl_proxy_get_user_data", &ECSIWayland_wl_proxy_get_user_data},
    {"wl_proxy_destroy", &ECSIWayland_wl_proxy_destroy},
    {"wl_proxy_set_queue", &ECSIWayland_wl_proxy_set_queue},
    {"wl_proxy_create_wrapper", &ECSIWayland_wl_proxy_create_wrapper},
    {"wl_proxy_wrapper_destroy", &ECSIWayland_wl_proxy_wrapper_destroy},
    {"wl_display_create_queue", &ECSIWayland_wl_display_create_queue},
    {"wl_display_roundtrip_queue", &ECSIWayland_wl_display_roundtrip_queue},
    {"wl_display_dispatch_queue_pending", &ECSIWayland_wl_display_dispatch_queue_pending},
    {"wl_display_flush", &ECSIWayland_wl_display_flush},
    {"wl_event_queue_destroy", &ECSIWayland_wl_event_queue_destroy},
    {"wl_registry_interface", &ECSIWayland_wl_registry_interface},
    {"wl_seat_interface", &ECSIWayland_wl_seat_interface},
    {"wl_pointer_interface", &ECSIWayland_wl_pointer_interface},
    {"wl_data_device_manager_interface", &ECSIWayland_wl_data_device_manager_interface},
    {"wl_data_device_interface", &ECSIWayland_wl_data_device_interface},
    {"wl_data_source_interface", &ECSIWayland_wl_data_source_interface},
};

static struct
{
    bool tried;  // the objects below were made, or could not be; they are made once
    bool ready;  // they were made
    SDL_SharedObject *library;
    struct wl_display *display; // SDL's connection
    struct wl_event_queue *queue; // the drag's own objects dispatch here, apart from SDL's
    struct wl_registry *registry;
    struct wl_seat *seat;
    struct wl_pointer *pointer; // tells the serial of the last press, which a drag needs
    struct wl_data_device_manager *manager;
    u32 managerVersion;
    struct wl_data_device *device;
    u32 pressSerial;

    // the drag, while it is on
    struct wl_data_source *source;
    bool carried; // the compositor took the drag: the pressed window lost the pointer
    struct wl_data_offer *offer; // what the window under the pointer is offered
    u32 enterSerial;
    SDL_Window *window; // the window under the pointer, or nullptr
    ECSISystemDragMoveFunction move;
    ECSISystemDragEndFunction end;
} DRAG = {0};

/// @brief Finds the OS window of a Wayland surface.
static SDL_Window *ECSISystemDrag_WindowOf(struct wl_surface *surface)
{
    int count = 0;
    SDL_Window **windows = SDL_GetWindows(&count);
    SDL_Window *found = nullptr;

    for (int i = 0; windows != nullptr && i < count && found == nullptr; i++)
    {
        void *own = SDL_GetPointerProperty(SDL_GetWindowProperties(windows[i]), SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
        found = own == surface ? windows[i] : nullptr;
    }

    SDL_free(windows);
    return found;
}

/// @brief Takes the offer of the drag, and says what it is taken as. SDL's own drop handling refuses it on every move, so it is taken again each time.
static void ECSISystemDrag_Accept(void)
{
    if (DRAG.offer == nullptr || DRAG.source == nullptr)
    {
        return;
    }

    wl_data_offer_accept(DRAG.offer, DRAG.enterSerial, DRAG.window != nullptr ? OPENECS_DRAG_MIME : nullptr);

    if (wl_data_offer_get_version(DRAG.offer) >= WL_DATA_OFFER_SET_ACTIONS_SINCE_VERSION)
    {
        u32 action = DRAG.window != nullptr ? WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE : WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE;
        wl_data_offer_set_actions(DRAG.offer, action, action);
    }
}

static void ECSISystemDrag_DropOffer(void)
{
    if (DRAG.offer != nullptr)
    {
        wl_data_offer_destroy(DRAG.offer);
        DRAG.offer = nullptr;
    }
}

/// @brief Ends the drag, calling its end function if asked, with whether the compositor carried it.
static void ECSISystemDrag_Finish(bool callEnd)
{
    if (DRAG.source == nullptr)
    {
        return;
    }

    bool carried = DRAG.carried;
    ECSISystemDragEndFunction end = DRAG.end;
    wl_data_source_destroy(DRAG.source);
    DRAG.source = nullptr;
    DRAG.carried = false;
    DRAG.window = nullptr;
    DRAG.move = nullptr;
    DRAG.end = nullptr;
    wl_display_flush(DRAG.display);

    if (callEnd && end != nullptr)
    {
        end(carried);
    }
}

#pragma region Listeners

static void ECSISystemDrag_OfferMime(void *data, struct wl_data_offer *offer, const char *mime)
{
    (void)data, (void)offer, (void)mime;
}

static void ECSISystemDrag_OfferSourceActions(void *data, struct wl_data_offer *offer, u32 actions)
{
    (void)data, (void)offer, (void)actions;
}

static void ECSISystemDrag_OfferAction(void *data, struct wl_data_offer *offer, u32 action)
{
    (void)data, (void)offer, (void)action;
}

static const struct wl_data_offer_listener ECSI_OFFER_LISTENER = {
    .offer = ECSISystemDrag_OfferMime,
    .source_actions = ECSISystemDrag_OfferSourceActions,
    .action = ECSISystemDrag_OfferAction,
};

static void ECSISystemDrag_DeviceOffer(void *data, struct wl_data_device *device, struct wl_data_offer *offer)
{
    (void)data, (void)device;
    wl_data_offer_add_listener(offer, &ECSI_OFFER_LISTENER, nullptr);
}

static void ECSISystemDrag_DeviceEnter(void *data, struct wl_data_device *device, u32 serial, struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y, struct wl_data_offer *offer)
{
    (void)data, (void)device;
    ECSISystemDrag_DropOffer();
    DRAG.offer = offer;
    DRAG.enterSerial = serial;

    // a drag from another program is left to SDL
    if (DRAG.source == nullptr)
    {
        return;
    }

    DRAG.window = surface != nullptr ? ECSISystemDrag_WindowOf(surface) : nullptr;
    ECSISystemDrag_Accept();
    DRAG.move(DRAG.window, (f32)wl_fixed_to_double(x), (f32)wl_fixed_to_double(y));
}

static void ECSISystemDrag_DeviceLeave(void *data, struct wl_data_device *device)
{
    (void)data, (void)device;
    ECSISystemDrag_DropOffer();

    if (DRAG.source != nullptr)
    {
        DRAG.window = nullptr;
        DRAG.move(nullptr, 0.0f, 0.0f);
    }
}

static void ECSISystemDrag_DeviceMotion(void *data, struct wl_data_device *device, u32 time, wl_fixed_t x, wl_fixed_t y)
{
    (void)data, (void)device, (void)time;

    if (DRAG.source != nullptr)
    {
        ECSISystemDrag_Accept();
        DRAG.move(DRAG.window, (f32)wl_fixed_to_double(x), (f32)wl_fixed_to_double(y));
    }
}

static void ECSISystemDrag_DeviceDrop(void *data, struct wl_data_device *device)
{
    (void)data, (void)device;

    // the panel lands where the pointer was last; nothing is read from the offer, as the panel never leaves the program
    ECSISystemDrag_DropOffer();
    ECSISystemDrag_Finish(true);
}

static void ECSISystemDrag_DeviceSelection(void *data, struct wl_data_device *device, struct wl_data_offer *offer)
{
    (void)data, (void)device;

    // the clipboard is SDL's
    if (offer != nullptr)
    {
        wl_data_offer_destroy(offer);
    }
}

static const struct wl_data_device_listener ECSI_DEVICE_LISTENER = {
    .data_offer = ECSISystemDrag_DeviceOffer,
    .enter = ECSISystemDrag_DeviceEnter,
    .leave = ECSISystemDrag_DeviceLeave,
    .motion = ECSISystemDrag_DeviceMotion,
    .drop = ECSISystemDrag_DeviceDrop,
    .selection = ECSISystemDrag_DeviceSelection,
};

static void ECSISystemDrag_SourceTarget(void *data, struct wl_data_source *source, const char *mime)
{
    (void)data, (void)source, (void)mime;
}

static void ECSISystemDrag_SourceSend(void *data, struct wl_data_source *source, const char *mime, int32_t fd)
{
    (void)data, (void)source, (void)mime;
    close(fd);
}

// released where no window took it, or ended by the compositor: the panel lands where the pointer was last, or pops out outside the windows
static void ECSISystemDrag_SourceCancelled(void *data, struct wl_data_source *source)
{
    (void)data, (void)source;
    ECSISystemDrag_Finish(true);
}

static void ECSISystemDrag_SourceDropPerformed(void *data, struct wl_data_source *source)
{
    (void)data, (void)source;
}

static void ECSISystemDrag_SourceFinished(void *data, struct wl_data_source *source)
{
    (void)data, (void)source;
    ECSISystemDrag_Finish(true);
}

static void ECSISystemDrag_SourceAction(void *data, struct wl_data_source *source, u32 action)
{
    (void)data, (void)source, (void)action;
}

static const struct wl_data_source_listener ECSI_SOURCE_LISTENER = {
    .target = ECSISystemDrag_SourceTarget,
    .send = ECSISystemDrag_SourceSend,
    .cancelled = ECSISystemDrag_SourceCancelled,
    .dnd_drop_performed = ECSISystemDrag_SourceDropPerformed,
    .dnd_finished = ECSISystemDrag_SourceFinished,
    .action = ECSISystemDrag_SourceAction,
};

static void ECSISystemDrag_PointerEnter(void *data, struct wl_pointer *pointer, u32 serial, struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y)
{
    (void)data, (void)pointer, (void)serial, (void)surface, (void)x, (void)y;
}

// the compositor takes the drag by taking the pointer from the pressed window
static void ECSISystemDrag_PointerLeave(void *data, struct wl_pointer *pointer, u32 serial, struct wl_surface *surface)
{
    (void)data, (void)pointer, (void)serial, (void)surface;
    DRAG.carried = DRAG.source != nullptr;
}

static void ECSISystemDrag_PointerMotion(void *data, struct wl_pointer *pointer, u32 time, wl_fixed_t x, wl_fixed_t y)
{
    (void)data, (void)pointer, (void)time, (void)x, (void)y;
}

static void ECSISystemDrag_PointerButton(void *data, struct wl_pointer *pointer, u32 serial, u32 time, u32 button, u32 state)
{
    (void)data, (void)pointer, (void)time, (void)button;

    if (state == WL_POINTER_BUTTON_STATE_PRESSED)
    {
        DRAG.pressSerial = serial;
    }

    // a release that the pressed window still gets means the compositor did not take the drag, as when the press is too old
    else if (DRAG.source != nullptr && !DRAG.carried)
    {
        ECSISystemDrag_Finish(true);
    }
}

static void ECSISystemDrag_PointerAxis(void *data, struct wl_pointer *pointer, u32 time, u32 axis, wl_fixed_t value)
{
    (void)data, (void)pointer, (void)time, (void)axis, (void)value;
}

// the seat is bound at version 1, so the pointer gets only these events
static const struct wl_pointer_listener ECSI_POINTER_LISTENER = {
    .enter = ECSISystemDrag_PointerEnter,
    .leave = ECSISystemDrag_PointerLeave,
    .motion = ECSISystemDrag_PointerMotion,
    .button = ECSISystemDrag_PointerButton,
    .axis = ECSISystemDrag_PointerAxis,
};

static void ECSISystemDrag_SeatCapabilities(void *data, struct wl_seat *seat, u32 capabilities)
{
    (void)data;

    if ((capabilities & WL_SEAT_CAPABILITY_POINTER) != 0 && DRAG.pointer == nullptr)
    {
        DRAG.pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(DRAG.pointer, &ECSI_POINTER_LISTENER, nullptr);
    }
}

static void ECSISystemDrag_SeatName(void *data, struct wl_seat *seat, const char *name)
{
    (void)data, (void)seat, (void)name;
}

static const struct wl_seat_listener ECSI_SEAT_LISTENER = {
    .capabilities = ECSISystemDrag_SeatCapabilities,
    .name = ECSISystemDrag_SeatName,
};

static void ECSISystemDrag_Global(void *data, struct wl_registry *registry, u32 name, const char *interface, u32 version)
{
    (void)data;

    // the first seat, as SDL takes; its pointer is the one that presses
    if (SDL_strcmp(interface, "wl_seat") == 0 && DRAG.seat == nullptr)
    {
        DRAG.seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
        wl_seat_add_listener(DRAG.seat, &ECSI_SEAT_LISTENER, nullptr);
    }
    else if (SDL_strcmp(interface, "wl_data_device_manager") == 0 && DRAG.manager == nullptr)
    {
        DRAG.managerVersion = SDL_min(version, 3u);
        DRAG.manager = wl_registry_bind(registry, name, &wl_data_device_manager_interface, DRAG.managerVersion);
    }
}

static void ECSISystemDrag_GlobalRemove(void *data, struct wl_registry *registry, u32 name)
{
    (void)data, (void)registry, (void)name;
}

static const struct wl_registry_listener ECSI_REGISTRY_LISTENER = {
    .global = ECSISystemDrag_Global,
    .global_remove = ECSISystemDrag_GlobalRemove,
};

#pragma endregion Listeners

/// @brief Loads libwayland-client and makes the drag's own objects on SDL's connection, once.
static bool ECSISystemDrag_Make(void)
{
    if (DRAG.tried)
    {
        return DRAG.ready;
    }

    DRAG.tried = true;
    DRAG.display = SDL_GetPointerProperty(SDL_GetGlobalProperties(), SDL_PROP_GLOBAL_VIDEO_WAYLAND_WL_DISPLAY_POINTER, nullptr);
    DRAG.library = DRAG.display == nullptr ? nullptr : SDL_LoadObject("libwayland-client.so.0");

    if (DRAG.library == nullptr)
    {
        return false;
    }

    for (usz i = 0; i < SDL_arraysize(ECSI_WAYLAND_SYMBOLS); i++)
    {
        // a function's and an interface's addresses alike come as a function pointer, which is copied into the pointer that stands for it
        SDL_FunctionPointer symbol = SDL_LoadFunction(DRAG.library, ECSI_WAYLAND_SYMBOLS[i].name);
        SDL_memcpy(ECSI_WAYLAND_SYMBOLS[i].address, &symbol, sizeof(symbol));

        if (symbol == nullptr)
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Panels are dragged between windows without a drop mark: libwayland-client has no %s.", ECSI_WAYLAND_SYMBOLS[i].name);
            return false;
        }
    }

    // the objects dispatch in their own queue, so SDL's dispatching never calls into them, and the main loop handles them in ECSISystemDrag_Pump
    DRAG.queue = wl_display_create_queue(DRAG.display);
    struct wl_display *wrapper = DRAG.queue == nullptr ? nullptr : wl_proxy_create_wrapper(DRAG.display);

    if (wrapper == nullptr)
    {
        return false;
    }

    wl_proxy_set_queue((struct wl_proxy *)wrapper, DRAG.queue);
    DRAG.registry = wl_display_get_registry(wrapper);
    wl_proxy_wrapper_destroy(wrapper);
    wl_registry_add_listener(DRAG.registry, &ECSI_REGISTRY_LISTENER, nullptr);

    // the round trip finds the seat and the manager; the seat's pointer comes when the seat has one, which may be later, as when a mouse is plugged in
    if (wl_display_roundtrip_queue(DRAG.display, DRAG.queue) < 0 || DRAG.seat == nullptr || DRAG.manager == nullptr)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Panels are dragged between windows without a drop mark: the compositor has no seat or no drag and drop.");
        return false;
    }

    DRAG.device = wl_data_device_manager_get_data_device(DRAG.manager, DRAG.seat);
    wl_data_device_add_listener(DRAG.device, &ECSI_DEVICE_LISTENER, nullptr);
    DRAG.ready = wl_display_roundtrip_queue(DRAG.display, DRAG.queue) >= 0;
    return DRAG.ready;
}

#endif

#pragma endregion Source Only

#pragma region Definitions

void ECSISystemDrag_Prepare(void)
{
#ifdef OPENECS_WAYLAND_DRAG
    const char *driver = SDL_GetCurrentVideoDriver();

    if (driver != nullptr && SDL_strcmp(driver, "wayland") == 0)
    {
        ECSISystemDrag_Make();
    }
#endif
}

bool ECSISystemDrag_Start(SDL_Window *from, ECSISystemDragMoveFunction move, ECSISystemDragEndFunction end)
{
#ifdef OPENECS_WAYLAND_DRAG
    const char *driver = SDL_GetCurrentVideoDriver();
    void *surface = SDL_GetPointerProperty(SDL_GetWindowProperties(from), SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);

    if (DRAG.source != nullptr || driver == nullptr || SDL_strcmp(driver, "wayland") != 0 || surface == nullptr || !ECSISystemDrag_Make())
    {
        return false;
    }

    // the press that the drag continues must be known
    wl_display_dispatch_queue_pending(DRAG.display, DRAG.queue);

    if (DRAG.pressSerial == 0)
    {
        return false;
    }

    DRAG.source = wl_data_device_manager_create_data_source(DRAG.manager);
    wl_data_source_add_listener(DRAG.source, &ECSI_SOURCE_LISTENER, nullptr);
    wl_data_source_offer(DRAG.source, OPENECS_DRAG_MIME);

    if (DRAG.managerVersion >= WL_DATA_SOURCE_SET_ACTIONS_SINCE_VERSION)
    {
        wl_data_source_set_actions(DRAG.source, WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE);
    }

    DRAG.move = move;
    DRAG.end = end;
    DRAG.window = from;
    wl_data_device_start_drag(DRAG.device, DRAG.source, surface, nullptr, DRAG.pressSerial);
    wl_display_flush(DRAG.display);
    return true;
#else
    (void)from, (void)move, (void)end;
    return false;
#endif
}

bool ECSISystemDrag_IsOn(void)
{
#ifdef OPENECS_WAYLAND_DRAG
    return DRAG.source != nullptr;
#else
    return false;
#endif
}

void ECSISystemDrag_Pump(void)
{
#ifdef OPENECS_WAYLAND_DRAG
    if (DRAG.ready)
    {
        wl_display_dispatch_queue_pending(DRAG.display, DRAG.queue);
        wl_display_flush(DRAG.display);
    }
#endif
}

void ECSISystemDrag_Cancel(void)
{
#ifdef OPENECS_WAYLAND_DRAG
    ECSISystemDrag_Finish(false);
#endif
}

void ECSISystemDrag_Terminate(void)
{
#ifdef OPENECS_WAYLAND_DRAG
    if (DRAG.ready || DRAG.queue != nullptr)
    {
        ECSISystemDrag_Finish(false);
        ECSISystemDrag_DropOffer();

        if (DRAG.device != nullptr)
        {
            wl_data_device_destroy(DRAG.device);
        }

        if (DRAG.pointer != nullptr)
        {
            wl_pointer_destroy(DRAG.pointer);
        }

        if (DRAG.seat != nullptr)
        {
            wl_seat_destroy(DRAG.seat);
        }

        if (DRAG.manager != nullptr)
        {
            wl_data_device_manager_destroy(DRAG.manager);
        }

        if (DRAG.registry != nullptr)
        {
            wl_registry_destroy(DRAG.registry);
        }

        wl_display_flush(DRAG.display);
        wl_event_queue_destroy(DRAG.queue);
    }

    if (DRAG.library != nullptr)
    {
        SDL_UnloadObject(DRAG.library);
    }

    DRAG = (__typeof__(DRAG)){0};
#endif
}

#pragma endregion Definitions
