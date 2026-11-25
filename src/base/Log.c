#include "base/Log.h"

#include "SDL3/SDL.h"
#include "stb/stbSDL3.h"

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#pragma region Source Only

/// @brief Longest error kept for ECSILog_GetLastError.
#define OPENECS_LOG_ERROR_SIZE 1024

/// @brief Most lines that wait for the main loop; more are counted and left out, so a flood of lines cannot grow the memory.
#define OPENECS_LOG_QUEUE_SIZE 512

static struct
{
    SDL_IOStream *file;                     // nullptr until the state folder is known
    char *path;                             // of the log file, or nullptr
    char lastError[OPENECS_LOG_ERROR_SIZE]; // the last error or critical message, empty if none
    bool colors;                            // true when standard error is a terminal that shows colours

    // lines that wait for the main loop, which emits them as ecs.logged (DESIGN 14.4)
    SDL_Mutex *lock;
    ECSILogLine *queue;      // stb_ds array, guarded by lock
    usz dropped;             // lines left out since the last take, guarded by lock
    SDL_AtomicInt waiting;   // whether lines wait, read without the lock
    SDL_ThreadID mainThread; // lines of the main thread are not queued while it is quiet
    bool quiet;              // main thread only: a handler of ecs.logged runs
    SDL_AtomicU32 wakeEvent; // an event that does nothing, which wakes the main loop for a line of another thread
} LOG = {0};

/// @brief Names of the log levels, as log lines show them.
static const char *const OPENECS_LOG_LEVELS[SDL_LOG_PRIORITY_COUNT] = {
    [SDL_LOG_PRIORITY_TRACE] = "trace",
    [SDL_LOG_PRIORITY_VERBOSE] = "verbose",
    [SDL_LOG_PRIORITY_DEBUG] = "debug",
    [SDL_LOG_PRIORITY_INFO] = "info",
    [SDL_LOG_PRIORITY_WARN] = "warning",
    [SDL_LOG_PRIORITY_ERROR] = "error",
    [SDL_LOG_PRIORITY_CRITICAL] = "critical",
};

/// @brief Names of the log levels as ecs.log has them, which ecs.logged gives.
static const char *const OPENECS_LOG_EVENT_LEVELS[SDL_LOG_PRIORITY_COUNT] = {
    [SDL_LOG_PRIORITY_TRACE] = "debug",
    [SDL_LOG_PRIORITY_VERBOSE] = "debug",
    [SDL_LOG_PRIORITY_DEBUG] = "debug",
    [SDL_LOG_PRIORITY_INFO] = "info",
    [SDL_LOG_PRIORITY_WARN] = "warn",
    [SDL_LOG_PRIORITY_ERROR] = "error",
    [SDL_LOG_PRIORITY_CRITICAL] = "error",
};

/// @brief Queues a line for ecs.logged, and wakes the main loop when another thread logs it.
static void ECSILog_Queue(const char *stamp, SDL_LogPriority priority, const char *message)
{
    bool main = SDL_GetCurrentThreadID() == LOG.mainThread;

    if (LOG.lock == nullptr || (main && LOG.quiet))
    {
        return;
    }

    // messages from ECS_Log start with the plugin's name in brackets
    const char *plugin = "ecs";
    usz pluginLength = 3;
    const char *close = message[0] == '[' ? SDL_strstr(message, "] ") : nullptr;

    if (close != nullptr)
    {
        plugin = message + 1;
        pluginLength = (usz)(close - plugin);
        message = close + 2;
    }

    SDL_LockMutex(LOG.lock);
    bool wasEmpty = arrlenu(LOG.queue) == 0;

    if (arrlenu(LOG.queue) >= OPENECS_LOG_QUEUE_SIZE)
    {
        LOG.dropped++;
    }
    else
    {
        ECSILogLine line = {.level = OPENECS_LOG_EVENT_LEVELS[priority], .plugin = SDL_strndup(plugin, pluginLength), .message = SDL_strdup(message)};
        SDL_strlcpy(line.time, stamp, sizeof(line.time));

        if (line.plugin != nullptr && line.message != nullptr)
        {
            arrput(LOG.queue, line);
        }
        else
        {
            SDL_free(line.plugin);
            SDL_free(line.message);
            LOG.dropped++;
        }
    }

    SDL_SetAtomicInt(&LOG.waiting, 1);
    SDL_UnlockMutex(LOG.lock);

    if (!main && wasEmpty)
    {
        u32 type = SDL_GetAtomicU32(&LOG.wakeEvent);
        SDL_Event event = {.type = type};
        (void)SDL_PushEvent(&event);
    }
}

/// @brief Colours of the log levels on a terminal, as ANSI escape codes: dim for the detailed levels, then green, yellow, red and bold red.
static const char *const OPENECS_LOG_COLORS[SDL_LOG_PRIORITY_COUNT] = {
    [SDL_LOG_PRIORITY_TRACE] = "\x1b[2m",
    [SDL_LOG_PRIORITY_VERBOSE] = "\x1b[2m",
    [SDL_LOG_PRIORITY_DEBUG] = "\x1b[2m",
    [SDL_LOG_PRIORITY_INFO] = "\x1b[32m",
    [SDL_LOG_PRIORITY_WARN] = "\x1b[33m",
    [SDL_LOG_PRIORITY_ERROR] = "\x1b[31m",
    [SDL_LOG_PRIORITY_CRITICAL] = "\x1b[1;31m",
};

/// @brief Writes a log line with the time, the level, the plugin and the message, to standard error and the log file. SDL calls it under its log lock, so any thread may log.
static void ECSILog_Output(void *userData, int category, SDL_LogPriority priority, const char *message)
{
    (void)userData;
    (void)category;

    SDL_Time now = 0;
    SDL_DateTime time = {0};

    if (SDL_GetCurrentTime(&now))
    {
        SDL_TimeToDateTime(now, &time, true);
    }

    char stamp[16];
    SDL_snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d.%03d", time.hour, time.minute, time.second, time.nanosecond / 1000000);

    // messages from ECS_Log start with the plugin's name in brackets; the core's own messages get "ecs"
    bool known = priority > SDL_LOG_PRIORITY_INVALID && priority < SDL_LOG_PRIORITY_COUNT;
    const char *level = known ? OPENECS_LOG_LEVELS[priority] : "?";
    const char *source = message[0] == '[' ? "" : "[ecs] ";
    char *line = nullptr;
    int length = SDL_asprintf(&line, "%s %-8s %s%s\n", stamp, level, source, message);

    if (length < 0)
    {
        return;
    }

    // the terminal shows the level in its colour; the file keeps plain text
    if (LOG.colors && known)
    {
        fprintf(stderr, "%s %s%-8s\x1b[0m %s%s\n", stamp, OPENECS_LOG_COLORS[priority], level, source, message);
    }
    else
    {
        fputs(line, stderr);
    }

    // the dialog of a failed start shows the last error, since it is the reason
    if (priority >= SDL_LOG_PRIORITY_ERROR)
    {
        SDL_strlcpy(LOG.lastError, message, sizeof(LOG.lastError));
    }

    if (LOG.file != nullptr)
    {
        SDL_WriteIO(LOG.file, line, (usz)length);
        SDL_FlushIO(LOG.file);
    }

    SDL_free(line);

    if (known)
    {
        ECSILog_Queue(stamp, priority, message);
    }
}

#pragma endregion Source Only

bool ECSILog_HasColors(FILE *stream)
{
    if (SDL_getenv("NO_COLOR") != nullptr)
    {
        return false;
    }

#ifdef _WIN32
    HANDLE console = GetStdHandle(stream == stdout ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE);
    DWORD mode = 0;
    return _isatty(_fileno(stream)) && GetConsoleMode(console, &mode) && SetConsoleMode(console, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#else
    return isatty(fileno(stream));
#endif
}

void ECSILog_Initialize(void)
{
    LOG.colors = ECSILog_HasColors(stderr);
    LOG.mainThread = SDL_GetCurrentThreadID();
    LOG.lock = SDL_CreateMutex();
    SDL_SetAtomicU32(&LOG.wakeEvent, SDL_RegisterEvents(1));
    SDL_SetLogOutputFunction(ECSILog_Output, nullptr);

#ifdef DEBUG
    SDL_SetLogPriority(SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_DEBUG);
#endif
}

void ECSILog_OpenFile(const char *path)
{
    SDL_assert(path != nullptr);

    ECSILog_CloseFile();
    LOG.file = SDL_IOFromFile(path, "w");
    LOG.path = LOG.file == nullptr ? nullptr : SDL_strdup(path);

    if (LOG.file == nullptr)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cannot write the log file '%s': %s", path, SDL_GetError());
    }
}

void ECSILog_CloseFile(void)
{
    if (LOG.file != nullptr)
    {
        SDL_CloseIO(LOG.file);
        LOG.file = nullptr;
    }

    SDL_free(LOG.path);
    LOG.path = nullptr;
}

const char *ECSILog_GetPath(void)
{
    return LOG.path;
}

const char *ECSILog_GetLastError(void)
{
    return LOG.lastError[0] == '\0' ? nullptr : LOG.lastError;
}

bool ECSILog_IsWaiting(void)
{
    return SDL_GetAtomicInt(&LOG.waiting) != 0;
}

ECSILogLine *ECSILog_Take(usz *retDropped)
{
    *retDropped = 0;

    if (LOG.lock == nullptr)
    {
        return nullptr;
    }

    SDL_LockMutex(LOG.lock);
    ECSILogLine *lines = LOG.queue;
    *retDropped = LOG.dropped;
    LOG.queue = nullptr;
    LOG.dropped = 0;
    SDL_SetAtomicInt(&LOG.waiting, 0);
    SDL_UnlockMutex(LOG.lock);
    return lines;
}

void ECSILog_FreeLines(ECSILogLine *lines)
{
    for (usz i = 0; i < arrlenu(lines); i++)
    {
        SDL_free(lines[i].plugin);
        SDL_free(lines[i].message);
    }

    arrfree(lines);
}

void ECSILog_SetQuiet(bool quiet)
{
    LOG.quiet = quiet;
}

void ECSILog_Terminate(void)
{
    usz dropped = 0;
    ECSILog_FreeLines(ECSILog_Take(&dropped));
    SDL_Mutex *lock = LOG.lock;
    LOG.lock = nullptr;
    SDL_DestroyMutex(lock);
}
