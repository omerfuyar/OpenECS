#include "base/Log.h"

#include "SDL3/SDL.h"

#pragma region Source Only

static struct
{
    SDL_IOStream *file; // NULL until the state folder is known
} LOG = {0};

/// @brief Names of the log levels, as log lines show them.
static const char *const ECSI_LOG_LEVELS[SDL_LOG_PRIORITY_COUNT] = {
    [SDL_LOG_PRIORITY_TRACE] = "trace",
    [SDL_LOG_PRIORITY_VERBOSE] = "verbose",
    [SDL_LOG_PRIORITY_DEBUG] = "debug",
    [SDL_LOG_PRIORITY_INFO] = "info",
    [SDL_LOG_PRIORITY_WARN] = "warning",
    [SDL_LOG_PRIORITY_ERROR] = "error",
    [SDL_LOG_PRIORITY_CRITICAL] = "critical",
};

/// @brief Writes a log line with the time, the level, the plugin and the message, to standard error and the log file. SDL calls it under its log lock, so any thread may log.
static void ECSI_LogOutput(void *userData, int category, SDL_LogPriority priority, const char *message)
{
    (void)userData;
    (void)category;

    SDL_Time now = 0;
    SDL_DateTime time = {0};

    if (SDL_GetCurrentTime(&now))
    {
        SDL_TimeToDateTime(now, &time, true);
    }

    // messages from ECS_Log start with the plugin's name in brackets; the core's own messages get "ecs"
    const char *level = priority > SDL_LOG_PRIORITY_INVALID && priority < SDL_LOG_PRIORITY_COUNT ? ECSI_LOG_LEVELS[priority] : "?";
    char *line = NULL;
    int length = SDL_asprintf(&line, "%02d:%02d:%02d.%03d %-8s %s%s\n", time.hour, time.minute, time.second, time.nanosecond / 1000000, level, message[0] == '[' ? "" : "[ecs] ", message);

    if (length < 0)
    {
        return;
    }

    fputs(line, stderr);

    if (LOG.file != NULL)
    {
        SDL_WriteIO(LOG.file, line, (usz)length);
        SDL_FlushIO(LOG.file);
    }

    SDL_free(line);
}

#pragma endregion Source Only

void ECSI_LogInitialize(void)
{
    SDL_SetLogOutputFunction(ECSI_LogOutput, NULL);

#ifdef DEBUG
    SDL_SetLogPriority(SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_DEBUG);
#endif
}

void ECSI_LogOpenFile(const char *path)
{
    SDL_assert(path != NULL);

    ECSI_LogTerminate();
    LOG.file = SDL_IOFromFile(path, "w");

    if (LOG.file == NULL)
    {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cannot write the log file '%s': %s", path, SDL_GetError());
    }
}

void ECSI_LogTerminate(void)
{
    if (LOG.file != NULL)
    {
        SDL_CloseIO(LOG.file);
        LOG.file = NULL;
    }
}
