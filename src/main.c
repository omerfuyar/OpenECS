#include "app/App.h"
#include "base/Log.h"

#include "SDL3/SDL.h"

#include <unistd.h>

#pragma region Source Only

/// @brief Preset used when the command line names none.
#define OPENECS_DEFAULT_PRESET "default"

/// @brief Reads the options, and moves the files to the start of argv, over options already read.
static ECSIArguments ECSIMain_ReadArguments(int argc, char **argv)
{
    ECSIArguments arguments = {.preset = OPENECS_DEFAULT_PRESET, .files = argv + 1};

    for (int i = 1; i < argc; i++)
    {
        if (SDL_strcmp(argv[i], "--preset") == 0 && i + 1 < argc)
        {
            arguments.preset = argv[++i];
        }
        else if (SDL_strcmp(argv[i], "--session") == 0 && i + 1 < argc)
        {
            arguments.session = argv[++i];
        }
        else if (SDL_strcmp(argv[i], "--fresh") == 0)
        {
            arguments.fresh = true;
        }
        else if (SDL_strcmp(argv[i], "--test") == 0 && i + 1 < argc)
        {
            arguments.test = argv[++i];
        }
        else if (SDL_strncmp(argv[i], "--", 2) != 0)
        {
            arguments.files[arguments.fileCount++] = argv[i];
        }
        else
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Unknown argument '%s'. Usage: openecs [--preset NAME|FILE] [--session FILE] [--fresh] [--test FILE] [FILE...]", argv[i]);
        }
    }

    return arguments;
}

#pragma endregion Source Only

int main(int argc, char **argv)
{
    ECSILog_Initialize();
    ECSIArguments arguments = ECSIMain_ReadArguments(argc, argv);

    ECSIApp_Start(&arguments);
    int status = ECSIApp_Run();
    char *next = ECSIApp_Stop();

    // a session opened while OpenECS ran starts a new OpenECS in place of this one; SDL can start a process but not replace one
    if (next != NULL)
    {
        execv("/proc/self/exe", (char *[]){argv[0], "--session", next, NULL});
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot start again from the session '%s'.", next);
        SDL_free(next);
        return 1;
    }

    return status;
}
