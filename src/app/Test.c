#include "app/Test.h"
#include "base/Lua.h"
#include "interface/Keys.h"
#include "interface/Layout.h"
#include "interface/Panels.h"
#include "interface/Window.h"
#include "runtime/Services.h"

#include "lua/lauxlib.h"
#include "lua/lua.h"

#include "SDL3/SDL.h"
#include "stb/stbSDL3.h"

#ifdef DEBUG

#pragma region Source Only

/// @brief Preset of a test that names none.
#define OPENECS_TEST_PRESET "default"
/// @brief Pointer moves that test.drag makes between its press and its release.
#define OPENECS_TEST_DRAG_STEPS 4

static struct
{
    lua_State *thread;            // the coroutine of the test's run function, or NULL when no test runs
    int threadReference;          // registry reference that keeps the coroutine alive
    bool started;                 // false until the run function is first resumed
    bool failed;                  // true when the test failed
    const ECSIPresetInfo *info;  // what the preset said, for test.session
    SDL_Event *events;            // input events the test sent; one goes to the loop in each pass
    usz nextEvent;                // index of the next event to send
    u64 resumeTicks;              // when the test goes on after test.wait, in nanoseconds
    f32 pointerX;                 // the pointer's last position
    f32 pointerY;
    SDL_MouseButtonFlags buttons; // the buttons held
    char **files;                 // stb_ds array of the paths that the test's files field names
} TEST = {0};

/// @brief test.match: compares by contents and names the path of the first difference.
static const char OPENECS_TEST_MATCH[] =
    "local format = string.format\n"
    "local function show(value)\n"
    "  if type(value) == 'string' then return format('%q', value) end\n"
    "  if type(value) == 'table' then return 'a table' end\n"
    "  return tostring(value)\n"
    "end\n"
    "local function path(base, key)\n"
    "  if type(key) == 'string' and key:match('^[%a_][%w_]*$') then return base == '' and key or base .. '.' .. key end\n"
    "  return base .. '[' .. (type(key) == 'string' and format('%q', key) or tostring(key)) .. ']'\n"
    "end\n"
    "local function compare(actual, expected, at)\n"
    "  local name = at == '' and 'the value' or at\n"
    "  if type(expected) ~= 'table' then\n"
    "    if actual ~= expected then return format('%s: expected %s, got %s', name, show(expected), show(actual)) end\n"
    "  elseif type(actual) ~= 'table' then return format('%s: expected a table, got %s', name, show(actual))\n"
    "  elseif #actual ~= #expected then return format('%s: expected %d items, got %d', name, #expected, #actual)\n"
    "  else\n"
    "    for key, value in pairs(expected) do\n"
    "      local difference = compare(actual[key], value, path(at, key))\n"
    "      if difference then return difference end\n"
    "    end\n"
    "  end\n"
    "end\n"
    "return function(actual, expected, message)\n"
    "  local difference = compare(actual, expected, '')\n"
    "  if difference then error((message and message .. ': ' or '') .. difference, 2) end\n"
    "end\n";

/// @brief Adds an input event for the loop, in the test's OS window.
static void ECSITest_Send(SDL_Event event)
{
    SDL_Window *window = ECSIWindow_GetMain();
    SDL_WindowID id = window != NULL ? SDL_GetWindowID(window) : 0;

    switch (event.type)
    {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        event.key.windowID = id;
        break;
    case SDL_EVENT_MOUSE_MOTION:
        event.motion.windowID = id;
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        event.button.windowID = id;
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        event.wheel.windowID = id;
        break;
    default:
        break;
    }

    arrput(TEST.events, event);
}

/// @brief Adds a pointer move to a position.
static void ECSITest_SendMove(f32 x, f32 y)
{
    SDL_Event event = {.type = SDL_EVENT_MOUSE_MOTION};
    event.motion.x = x;
    event.motion.y = y;
    event.motion.xrel = x - TEST.pointerX;
    event.motion.yrel = y - TEST.pointerY;
    event.motion.state = TEST.buttons;
    ECSITest_Send(event);

    TEST.pointerX = x;
    TEST.pointerY = y;
}

/// @brief Adds a button press or release at a position.
static void ECSITest_SendButton(f32 x, f32 y, u8 button, bool down)
{
    if (down)
    {
        TEST.buttons |= SDL_BUTTON_MASK(button);
    }
    else
    {
        TEST.buttons &= ~SDL_BUTTON_MASK(button);
    }

    SDL_Event event = {.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP};
    event.button.button = button;
    event.button.down = down;
    event.button.clicks = 1;
    event.button.x = x;
    event.button.y = y;
    ECSITest_Send(event);

    TEST.pointerX = x;
    TEST.pointerY = y;
}

/// @brief Reads a position from two Lua arguments.
static void ECSITest_CheckPosition(lua_State *state, int index, f32 *retX, f32 *retY)
{
    *retX = (f32)luaL_checknumber(state, index);
    *retY = (f32)luaL_checknumber(state, index + 1);
}

/// @brief Reads a button from an optional Lua argument: 1 left, 2 middle, 3 right.
static u8 ECSITest_CheckButton(lua_State *state, int index)
{
    lua_Integer button = luaL_optinteger(state, index, SDL_BUTTON_LEFT);
    luaL_argcheck(state, button >= SDL_BUTTON_LEFT && button <= SDL_BUTTON_RIGHT, index, "1 (left), 2 (middle) or 3 (right) expected");
    return (u8)button;
}

/// @brief test.key(combination): presses and releases a key combination.
static int ECSITest_Key(lua_State *state)
{
    const char *text = luaL_checkstring(state, 1);
    u32 key = 0;
    u32 modifiers = 0;

    if (ECSIKeys_Parse(text, false, &key, &modifiers))
    {
        return luaL_error(state, "'%s' is not a key combination", text);
    }

    SDL_Keymod mod = SDL_KMOD_NONE;
    mod |= (modifiers & ECSModifier_Shift) ? SDL_KMOD_LSHIFT : 0;
    mod |= (modifiers & ECSModifier_Ctrl) ? SDL_KMOD_LCTRL : 0;
    mod |= (modifiers & ECSModifier_Alt) ? SDL_KMOD_LALT : 0;
    mod |= (modifiers & ECSModifier_Super) ? SDL_KMOD_LGUI : 0;

    SDL_Event event = {.type = SDL_EVENT_KEY_DOWN};
    event.key.key = key;
    event.key.scancode = SDL_GetScancodeFromKey(key, NULL);
    event.key.mod = mod;
    event.key.down = true;
    ECSITest_Send(event);

    event.type = SDL_EVENT_KEY_UP;
    event.key.down = false;
    ECSITest_Send(event);

    return lua_yield(state, 0);
}

/// @brief test.move(x, y): moves the pointer.
static int ECSITest_Move(lua_State *state)
{
    f32 x = 0.0f;
    f32 y = 0.0f;
    ECSITest_CheckPosition(state, 1, &x, &y);
    ECSITest_SendMove(x, y);
    return lua_yield(state, 0);
}

/// @brief test.press(x, y, button): presses a button.
static int ECSITest_Press(lua_State *state)
{
    f32 x = 0.0f;
    f32 y = 0.0f;
    ECSITest_CheckPosition(state, 1, &x, &y);
    ECSITest_SendButton(x, y, ECSITest_CheckButton(state, 3), true);
    return lua_yield(state, 0);
}

/// @brief test.release(x, y, button): releases a button.
static int ECSITest_Release(lua_State *state)
{
    f32 x = 0.0f;
    f32 y = 0.0f;
    ECSITest_CheckPosition(state, 1, &x, &y);
    ECSITest_SendButton(x, y, ECSITest_CheckButton(state, 3), false);
    return lua_yield(state, 0);
}

/// @brief test.click(x, y, button): presses and releases a button.
static int ECSITest_Click(lua_State *state)
{
    f32 x = 0.0f;
    f32 y = 0.0f;
    ECSITest_CheckPosition(state, 1, &x, &y);
    u8 button = ECSITest_CheckButton(state, 3);
    ECSITest_SendButton(x, y, button, true);
    ECSITest_SendButton(x, y, button, false);
    return lua_yield(state, 0);
}

/// @brief test.drag(x, y, toX, toY): presses the left button, moves in steps and releases it.
static int ECSITest_Drag(lua_State *state)
{
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 toX = 0.0f;
    f32 toY = 0.0f;
    ECSITest_CheckPosition(state, 1, &x, &y);
    ECSITest_CheckPosition(state, 3, &toX, &toY);

    ECSITest_SendMove(x, y);
    ECSITest_SendButton(x, y, SDL_BUTTON_LEFT, true);

    for (i32 i = 1; i <= OPENECS_TEST_DRAG_STEPS; i++)
    {
        f32 part = (f32)i / (f32)OPENECS_TEST_DRAG_STEPS;
        ECSITest_SendMove(x + (toX - x) * part, y + (toY - y) * part);
    }

    ECSITest_SendButton(toX, toY, SDL_BUTTON_LEFT, false);
    return lua_yield(state, 0);
}

/// @brief test.wheel(x, y, amount): turns the wheel; a positive amount is away from the user.
static int ECSITest_Wheel(lua_State *state)
{
    f32 x = 0.0f;
    f32 y = 0.0f;
    ECSITest_CheckPosition(state, 1, &x, &y);

    SDL_Event event = {.type = SDL_EVENT_MOUSE_WHEEL};
    event.wheel.y = (f32)luaL_checknumber(state, 3);
    event.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
    event.wheel.mouse_x = x;
    event.wheel.mouse_y = y;
    ECSITest_Send(event);

    return lua_yield(state, 0);
}

/// @brief test.call(name): runs a bound function with the focused panel.
static int ECSITest_Call(lua_State *state)
{
    const char *name = luaL_checkstring(state, 1);

    if (ECSIServices_CallBound(name, ECSILayout_GetFocus()))
    {
        return luaL_error(state, "'%s' cannot be called; see the log", name);
    }

    return lua_yield(state, 0);
}

/// @brief test.wait(seconds): lets the program run.
static int ECSITest_Wait(lua_State *state)
{
    lua_Number seconds = luaL_optnumber(state, 1, 0.0);
    luaL_argcheck(state, seconds >= 0.0, 1, "seconds must not be negative");

    TEST.resumeTicks = SDL_GetTicksNS() + (u64)(seconds * (lua_Number)SDL_NS_PER_SECOND);
    return lua_yield(state, 0);
}

/// @brief test.session(): the session that quitting would save now.
static int ECSITest_Session(lua_State *state)
{
    ECSValue *session = NULL;

    if (ECSValue_Create(&session) || ECSISession_Build(TEST.info, session))
    {
        ECSValue_Destroy(&session);
        return luaL_error(state, "the session cannot be built; see the log");
    }

    // values are pushed onto the main state, and the test runs in its own thread
    ECSILua_PushValue(session);
    lua_xmove(ECSILua_GetState(), state, 1);
    ECSValue_Destroy(&session);
    return 1;
}

/// @brief test.rect(id): the rectangle of a shown panel.
static int ECSITest_Rect(lua_State *state)
{
    lua_Integer id = luaL_checkinteger(state, 1);
    ECSPanel panel = id > 0 && id <= UINT32_MAX ? ECSLayout_FindPanel((u32)id) : NULL;

    if (panel == NULL)
    {
        return luaL_error(state, "no panel has the id %d", (int)id);
    }

    // a hidden panel keeps the rectangle it had when it was last shown
    if (!panel->visible)
    {
        return luaL_error(state, "the panel %d is not shown", (int)id);
    }

    lua_createtable(state, 0, 4);
    lua_pushnumber(state, (lua_Number)panel->x);
    lua_setfield(state, -2, "x");
    lua_pushnumber(state, (lua_Number)panel->y);
    lua_setfield(state, -2, "y");
    lua_pushnumber(state, (lua_Number)panel->width);
    lua_setfield(state, -2, "width");
    lua_pushnumber(state, (lua_Number)panel->height);
    lua_setfield(state, -2, "height");
    return 1;
}

/// @brief test.screenshot(path): draws a frame and saves it as a PNG file.
static int ECSITest_Screenshot(lua_State *state)
{
    const char *path = luaL_checkstring(state, 1);

    if (ECSIWindow_Screenshot(path))
    {
        return luaL_error(state, "cannot save a screenshot to '%s'", path);
    }

    return 0;
}

static const luaL_Reg OPENECS_TEST_FUNCTIONS[] = {
    {"key", ECSITest_Key},
    {"move", ECSITest_Move},
    {"press", ECSITest_Press},
    {"release", ECSITest_Release},
    {"click", ECSITest_Click},
    {"drag", ECSITest_Drag},
    {"wheel", ECSITest_Wheel},
    {"call", ECSITest_Call},
    {"wait", ECSITest_Wait},
    {"session", ECSITest_Session},
    {"rect", ECSITest_Rect},
    {"screenshot", ECSITest_Screenshot},
    {NULL, NULL},
};

/// @brief Pushes the test table. It also holds match, which is written in Lua.
static SHUResult ECSITest_PushTable(lua_State *state)
{
    luaL_newlib(state, OPENECS_TEST_FUNCTIONS);

    if (luaL_loadbufferx(state, OPENECS_TEST_MATCH, sizeof(OPENECS_TEST_MATCH) - 1, "=match", "t") != LUA_OK || ECSILua_Call(0, 1))
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "test.match does not load: %s", lua_tostring(state, -1));
        lua_pop(state, 2);
        return SHUResult_ErrInternal;
    }

    lua_setfield(state, -2, "match");
    return SHUResult_Ok;
}

/// @brief Ends the test and lets the garbage collector take its coroutine.
static void ECSITest_Finish(void)
{
    if (TEST.thread != NULL)
    {
        luaL_unref(ECSILua_GetState(), LUA_REGISTRYINDEX, TEST.threadReference);
        TEST.thread = NULL;
    }

    arrfree(TEST.events);
    TEST.nextEvent = 0;
}

#pragma endregion Source Only

SHUResult ECSITest_Load(const char *path, const ECSIPresetInfo *info, char **retPreset)
{
    SDL_assert(path != NULL);
    SDL_assert(info != NULL);
    SDL_assert(retPreset != NULL);
    SDL_assert(TEST.thread == NULL);

    lua_State *state = ECSILua_GetState();
    int top = lua_gettop(state);
    *retPreset = NULL;

    if (luaL_loadfilex(state, path, "t") != LUA_OK)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot read the test '%s': %s", path, lua_tostring(state, -1));
        lua_settop(state, top);
        return SHUResult_ErrFile;
    }

    // the test runs in its own environment, which reads other globals from the shared global table
    lua_newtable(state);
    lua_createtable(state, 0, 1);
    lua_pushglobaltable(state);
    lua_setfield(state, -2, "__index");
    lua_setmetatable(state, -2);
    lua_setupvalue(state, -2, 1);

    if (ECSILua_Call(0, 1))
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Error in the test '%s': %s", path, lua_tostring(state, -1));
        lua_settop(state, top);
        return SHUResult_ErrBadData;
    }

    if (!lua_istable(state, -1) || lua_getfield(state, -1, "run") != LUA_TFUNCTION)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "The test '%s' does not return a table with a run function.", path);
        lua_settop(state, top);
        return SHUResult_ErrBadData;
    }

    // a preset path is relative to the test file
    const char *preset = lua_getfield(state, -2, "preset") == LUA_TSTRING ? lua_tostring(state, -1) : OPENECS_TEST_PRESET;
    const char *slash = SDL_strrchr(path, '/');
    usz length = SDL_strlen(preset);
    bool file = SDL_strchr(preset, '/') != NULL || (length > 4 && SDL_strcmp(preset + length - 4, ".lua") == 0);
    int folderLength = slash != NULL && file && preset[0] != '/' ? (int)(slash - path + 1) : 0;

    if (SDL_asprintf(retPreset, "%.*s%s", folderLength, path, preset) < 0)
    {
        *retPreset = NULL;
        lua_settop(state, top);
        return SHUResult_ErrAllocation;
    }

    lua_pop(state, 1);

    // files are paths relative to the test file too, opened as if the command line named them
    lua_getfield(state, -2, "files");

    for (lua_Integer i = 1; lua_istable(state, -1) && lua_rawgeti(state, -1, i) == LUA_TSTRING; i++)
    {
        const char *name = lua_tostring(state, -1);
        char *copy = NULL;

        if (SDL_asprintf(&copy, "%.*s%s", slash != NULL && name[0] != '/' ? (int)(slash - path + 1) : 0, path, name) >= 0)
        {
            arrput(TEST.files, copy);
        }

        lua_pop(state, 1);
    }

    // the loop leaves the first value that is not a file on the stack, above the table
    lua_pop(state, lua_istable(state, -2) ? 2 : 1);

    // the run function and the test table wait on the coroutine's stack until its first resume
    TEST.thread = lua_newthread(state);
    TEST.threadReference = luaL_ref(state, LUA_REGISTRYINDEX);
    lua_xmove(state, TEST.thread, 1);

    if (ECSITest_PushTable(state))
    {
        ECSITest_Finish();
        SDL_free(*retPreset);
        *retPreset = NULL;
        lua_settop(state, top);
        return SHUResult_ErrInternal;
    }

    lua_xmove(state, TEST.thread, 1);
    lua_settop(state, top);

    TEST.info = info;
    TEST.started = false;
    TEST.failed = false;
    return SHUResult_Ok;
}

char **ECSITest_GetFiles(usz *retCount)
{
    SDL_assert(retCount != NULL);

    *retCount = arrlenu(TEST.files);
    return TEST.files;
}

void ECSITest_Terminate(void)
{
    ECSITest_Finish();

    for (usz i = 0; i < arrlenu(TEST.files); i++)
    {
        SDL_free(TEST.files[i]);
    }

    arrfree(TEST.files);
    SDL_zero(TEST);
}

bool ECSITest_IsRunning(void)
{
    return TEST.thread != NULL;
}

i32 ECSITest_GetWait(void)
{
    if (TEST.thread == NULL)
    {
        return -1;
    }

    u64 now = SDL_GetTicksNS();
    return now >= TEST.resumeTicks ? 0 : (i32)((TEST.resumeTicks - now + SDL_NS_PER_MS - 1) / SDL_NS_PER_MS);
}

bool ECSITest_Step(void)
{
    if (TEST.thread == NULL || SDL_GetTicksNS() < TEST.resumeTicks)
    {
        return true;
    }

    // each input event gets its own pass of the loop, so it is handled, its events delivered and the window drawn before the next
    if (TEST.nextEvent < arrlenu(TEST.events))
    {
        SDL_PushEvent(&TEST.events[TEST.nextEvent++]);
        return true;
    }

    arrfree(TEST.events);
    TEST.nextEvent = 0;

    lua_State *state = ECSILua_GetState();
    int results = 0;
    int status = lua_resume(TEST.thread, state, TEST.started ? 0 : 1, &results);
    TEST.started = true;

    if (status == LUA_YIELD)
    {
        lua_pop(TEST.thread, results);
        return true;
    }

    if (status == LUA_OK)
    {
        SDL_Log("The test passed.");
    }
    else
    {
        luaL_traceback(state, TEST.thread, lua_tostring(TEST.thread, -1), 0);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "The test failed: %s", lua_tostring(state, -1));
        lua_pop(state, 1);
        TEST.failed = true;
    }

    ECSITest_Finish();
    return false;
}

int ECSITest_GetStatus(void)
{
    return TEST.failed || TEST.thread != NULL ? 1 : 0;
}

#else

SHUResult ECSITest_Load(const char *path, const ECSIPresetInfo *info, char **retPreset)
{
    (void)path;
    (void)info;
    *retPreset = NULL;
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Only Debug builds run tests.");
    return SHUResult_ErrPrivileges;
}

char **ECSITest_GetFiles(usz *retCount)
{
    *retCount = 0;
    return NULL;
}

void ECSITest_Terminate(void)
{
}

bool ECSITest_IsRunning(void)
{
    return false;
}

i32 ECSITest_GetWait(void)
{
    return -1;
}

bool ECSITest_Step(void)
{
    return true;
}

int ECSITest_GetStatus(void)
{
    return 0;
}

#endif
