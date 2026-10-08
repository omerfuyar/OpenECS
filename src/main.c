#include "app/App.h"
#include "base/Log.h"

#include "SDL3/SDL.h"

#pragma region Source Only

/// @brief Preset used when the command line names none.
#define OPENECS_DEFAULT_PRESET "default"

static ECSI_Arguments ECSI_ReadArguments(int argc, char **argv)
{
    ECSI_Arguments arguments = {.preset = OPENECS_DEFAULT_PRESET};

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
        else
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Unknown argument '%s'. Usage: openecs [--preset NAME|FILE] [--session FILE] [--fresh]", argv[i]);
        }
    }

    return arguments;
}

#pragma endregion Source Only

int main(int argc, char **argv)
{
    ECSI_LogInitialize();
    ECSI_Arguments arguments = ECSI_ReadArguments(argc, argv);

    ECSI_AppStart(&arguments);
    ECSI_AppRun();
    ECSI_AppStop();

    return 0;
}
