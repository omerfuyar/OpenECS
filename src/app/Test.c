#include "app/Test.h"
#include "base/Lua.h"
#include "interface/Input.h"
#include "interface/Layout.h"
#include "interface/Panels.h"
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
    const ECSI_PresetInfo *info;  // what the preset said, for test.session
    SDL_Event *events;            // input events the test sent; one goes to the loop in each pass
    usz nextEvent;                // index of the next event to send
    u64 resumeTicks;              // when the test goes on after test.wait, in nanoseconds
    f32 pointerX;                 // the pointer's last position
    f32 pointerY;
    SDL_MouseButtonFlags buttons; // the buttons held
} TEST = {0};

/// @brief test.match: compares by contents and names the path of the first difference.
static const char ECSI_TEST_MATCH[] =
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
static void ECSI_TestSend(SDL_Event event)
{
    SDL_Window *window = ECSI_LayoutGetWindow();
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
static void ECSI_TestSendMove(f32 x, f32 y)
{
    SDL_Event event = {.type = SDL_EVENT_MOUSE_MOTION};
    event.motion.x = x;
    event.motion.y = y;
    event.motion.xrel = x - TEST.pointerX;
    event.motion.yrel = y - TEST.pointerY;
    event.motion.state = TEST.buttons;
    ECSI_TestSend(event);

    TEST.pointerX = x;
    TEST.pointerY = y;
}

/// @brief Adds a button press or release at a position.
static void ECSI_TestSendButton(f32 x, f32 y, u8 button, bool down)
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
    ECSI_TestSend(event);

    TEST.pointerX = x;
    TEST.pointerY = y;
}

/// @brief Reads a position from two Lua arguments.
static void ECSI_TestCheckPosition(lua_State *state, int index, f32 *retX, f32 *retY)
{
    *retX = (f32)luaL_checknumber(state, index);
    *retY = (f32)luaL_checknumber(state, index + 1);
}

/// @brief Reads a button from an optional Lua argument: 1 left, 2 middle, 3 right.
static u8 ECSI_TestCheckButton(lua_State *state, int index)
{
    lua_Integer button = luaL_optinteger(state, index, SDL_BUTTON_LEFT);
    luaL_argcheck(state, button >= SDL_BUTTON_LEFT && button <= SDL_BUTTON_RIGHT, index, "1 (left), 2 (middle) or 3 (right) expected");
    return (u8)button;
}

/// @brief test.key(combination): presses and releases a key combination.
static int ECSI_TestKey(lua_State *state)
{
    const char *text = luaL_checkstring(state, 1);
    u32 key = 0;
    u32 modifiers = 0;

    if (ECSI_InputParseKey(text, false, &key, &modifiers))
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
    ECSI_TestSend(event);

    event.type = SDL_EVENT_KEY_UP;
    event.key.down = false;
    ECSI_TestSend(event);

    return lua_yield(state, 0);
}

/// @brief test.move(x, y): moves the pointer.
static int ECSI_TestMove(lua_State *state)
{
    f32 x = 0.0f;
    f32 y = 0.0f;
    ECSI_TestCheckPosition(state, 1, &x, &y);
    ECSI_TestSendMove(x, y);
    return lua_yield(state, 0);
}

/// @brief test.press(x, y, button): presses a button.
static int ECSI_TestPress(lua_State *state)
{
    f32 x = 0.0f;
    f32 y = 0.0f;
    ECSI_TestCheckPosition(state, 1, &x, &y);
    ECSI_TestSendButton(x, y, ECSI_TestCheckButton(state, 3), true);
    return lua_yield(state, 0);
}

/// @brief test.release(x, y, button): releases a button.
static int ECSI_TestRelease(lua_State *state)
{
    f32 x = 0.0f;
    f32 y = 0.0f;
    ECSI_TestCheckPosition(state, 1, &x, &y);
    ECSI_TestSendButton(x, y, ECSI_TestCheckButton(state, 3), false);
    return lua_yield(state, 0);
}

/// @brief test.click(x, y, button): presses and releases a button.
static int ECSI_TestClick(lua_State *state)
{
    f32 x = 0.0f;
    f32 y = 0.0f;
    ECSI_TestCheckPosition(state, 1, &x, &y);
    u8 button = ECSI_TestCheckButton(state, 3);
    ECSI_TestSendButton(x, y, button, true);
    ECSI_TestSendButton(x, y, button, false);
    return lua_yield(state, 0);
}

/// @brief test.drag(x, y, toX, toY): presses the left button, moves in steps and releases it.
static int ECSI_TestDrag(lua_State *state)
{
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 toX = 0.0f;
    f32 toY = 0.0f;
    ECSI_TestCheckPosition(state, 1, &x, &y);
    ECSI_TestCheckPosition(state, 3, &toX, &toY);

    ECSI_TestSendMove(x, y);
    ECSI_TestSendButton(x, y, SDL_BUTTON_LEFT, true);

    for (i32 i = 1; i <= OPENECS_TEST_DRAG_STEPS; i++)
    {
        f32 part = (f32)i / (f32)OPENECS_TEST_DRAG_STEPS;
        ECSI_TestSendMove(x + (toX - x) * part, y + (toY - y) * part);
    }

    ECSI_TestSendButton(toX, toY, SDL_BUTTON_LEFT, false);
    return lua_yield(state, 0);
}

/// @brief test.wheel(x, y, amount): turns the wheel; a positive amount is away from the user.
static int ECSI_TestWheel(lua_State *state)
{
    f32 x = 0.0f;
    f32 y = 0.0f;
    ECSI_TestCheckPosition(state, 1, &x, &y);

    SDL_Event event = {.type = SDL_EVENT_MOUSE_WHEEL};
    event.wheel.y = (f32)luaL_checknumber(state, 3);
    event.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
    event.wheel.mouse_x = x;
    event.wheel.mouse_y = y;
    ECSI_TestSend(event);

    return lua_yield(state, 0);
}

/// @brief test.call(name): runs a bound function with the focused panel.
static int ECSI_TestCall(lua_State *state)
{
    const char *name = luaL_checkstring(state, 1);

    if (ECSI_ServicesCallBound(name, ECSI_LayoutGetFocus()))
    {
        return luaL_error(state, "'%s' cannot be called; see the log", name);
    }

    return lua_yield(state, 0);
}

/// @brief test.wait(seconds): lets the program run.
static int ECSI_TestWait(lua_State *state)
{
    lua_Number seconds = luaL_optnumber(state, 1, 0.0);
    luaL_argcheck(state, seconds >= 0.0, 1, "seconds must not be negative");

    TEST.resumeTicks = SDL_GetTicksNS() + (u64)(seconds * (lua_Number)SDL_NS_PER_SECOND);
    return lua_yield(state, 0);
}

/// @brief test.session(): the session that quitting would save now.
static int ECSI_TestSession(lua_State *state)
{
    ECSValue *session = NULL;

    if (ECSValue_Create(&session) || ECSI_SessionBuild(TEST.info, session))
    {
        ECSValue_Destroy(&session);
        return luaL_error(state, "the session cannot be built; see the log");
    }

    // values are pushed onto the main state, and the test runs in its own thread
    ECSI_LuaPushValue(session);
    lua_xmove(ECSI_LuaGetState(), state, 1);
    ECSValue_Destroy(&session);
    return 1;
}

/// @brief test.rect(id): the rectangle of a shown panel.
static int ECSI_TestRect(lua_State *state)
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
static int ECSI_TestScreenshot(lua_State *state)
{
    const char *path = luaL_checkstring(state, 1);

    if (ECSI_LayoutScreenshot(path))
    {
        return luaL_error(state, "cannot save a screenshot to '%s'", path);
    }

    return 0;
}

static const luaL_Reg ECSI_TEST_FUNCTIONS[] = {
    {"key", ECSI_TestKey},
    {"move", ECSI_TestMove},
    {"press", ECSI_TestPress},
    {"release", ECSI_TestRelease},
    {"click", ECSI_TestClick},
    {"drag", ECSI_TestDrag},
    {"wheel", ECSI_TestWheel},
    {"call", ECSI_TestCall},
    {"wait", ECSI_TestWait},
    {"session", ECSI_TestSession},
    {"rect", ECSI_TestRect},
    {"screenshot", ECSI_TestScreenshot},
    {NULL, NULL},
};

/// @brief Pushes the test table. It also holds match, which is written in Lua.
static SHUResult ECSI_TestPushTable(lua_State *state)
{
    luaL_newlib(state, ECSI_TEST_FUNCTIONS);

    if (luaL_loadbufferx(state, ECSI_TEST_MATCH, sizeof(ECSI_TEST_MATCH) - 1, "=match", "t") != LUA_OK || ECSI_LuaCall(0, 1))
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "test.match does not load: %s", lua_tostring(state, -1));
        lua_pop(state, 2);
        return SHUResult_ErrInternal;
    }

    lua_setfield(state, -2, "match");
    return SHUResult_Ok;
}

/// @brief Ends the test and lets the garbage collector take its coroutine.
static void ECSI_TestFinish(void)
{
    if (TEST.thread != NULL)
    {
        luaL_unref(ECSI_LuaGetState(), LUA_REGISTRYINDEX, TEST.threadReference);
        TEST.thread = NULL;
    }

    arrfree(TEST.events);
    TEST.nextEvent = 0;
}

#pragma endregion Source Only

SHUResult ECSI_TestLoad(const char *path, const ECSI_PresetInfo *info, char **retPreset)
{
    SDL_assert(path != NULL);
    SDL_assert(info != NULL);
    SDL_assert(retPreset != NULL);
    SDL_assert(TEST.thread == NULL);

    lua_State *state = ECSI_LuaGetState();
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

    if (ECSI_LuaCall(0, 1))
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

    // the run function and the test table wait on the coroutine's stack until its first resume
    TEST.thread = lua_newthread(state);
    TEST.threadReference = luaL_ref(state, LUA_REGISTRYINDEX);
    lua_xmove(state, TEST.thread, 1);

    if (ECSI_TestPushTable(state))
    {
        ECSI_TestFinish();
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

void ECSI_TestTerminate(void)
{
    ECSI_TestFinish();
    SDL_zero(TEST);
}

bool ECSI_TestIsRunning(void)
{
    return TEST.thread != NULL;
}

i32 ECSI_TestGetWait(void)
{
    if (TEST.thread == NULL)
    {
        return -1;
    }

    u64 now = SDL_GetTicksNS();
    return now >= TEST.resumeTicks ? 0 : (i32)((TEST.resumeTicks - now + SDL_NS_PER_MS - 1) / SDL_NS_PER_MS);
}

bool ECSI_TestStep(void)
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

    lua_State *state = ECSI_LuaGetState();
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

    ECSI_TestFinish();
    return false;
}

int ECSI_TestGetStatus(void)
{
    return TEST.failed || TEST.thread != NULL ? 1 : 0;
}

#else

SHUResult ECSI_TestLoad(const char *path, const ECSI_PresetInfo *info, char **retPreset)
{
    (void)path;
    (void)info;
    *retPreset = NULL;
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Only Debug builds run tests.");
    return SHUResult_ErrPrivileges;
}

void ECSI_TestTerminate(void)
{
}

bool ECSI_TestIsRunning(void)
{
    return false;
}

i32 ECSI_TestGetWait(void)
{
    return -1;
}

bool ECSI_TestStep(void)
{
    return true;
}

int ECSI_TestGetStatus(void)
{
    return 0;
}

#endif
