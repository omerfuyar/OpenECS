#include "base/Log.h"

#include "SDL3/SDL.h"

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#pragma region Source Only

/// @brief Longest error kept for ECSILog_GetLastError.
#define OPENECS_LOG_ERROR_SIZE 1024

static struct
{
    SDL_IOStream *file;                     // nullptr until the state folder is known
    char *path;                             // of the log file, or nullptr
    char lastError[OPENECS_LOG_ERROR_SIZE]; // the last error or critical message, empty if none
    bool colors;                            // true when standard error is a terminal that shows colours
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

/// @brief Checks whether standard error shows colours: a terminal, unless NO_COLOR is set (no-color.org). On Windows it turns the console's colours on.
static bool ECSILog_TerminalHasColors(void)
{
    if (SDL_getenv("NO_COLOR") != nullptr)
    {
        return false;
    }

#ifdef _WIN32
    HANDLE console = GetStdHandle(STD_ERROR_HANDLE);
    DWORD mode = 0;
    return _isatty(_fileno(stderr)) && GetConsoleMode(console, &mode) && SetConsoleMode(console, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#else
    return isatty(STDERR_FILENO);
#endif
}

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
}

#pragma endregion Source Only

void ECSILog_Initialize(void)
{
    LOG.colors = ECSILog_TerminalHasColors();
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
