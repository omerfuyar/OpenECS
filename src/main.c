#include "app/App.h"
#include "base/Log.h"

#include "SDL3/SDL.h"

#include <stdio.h>

#if defined(SDL_PLATFORM_WINDOWS)
#include <stdlib.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#pragma region Source Only

/// @brief Preset used when the command line names none.
#define OPENECS_DEFAULT_PRESET "launcher"

/// @brief What --help prints.
#define OPENECS_USAGE                                                                                 \
    "Usage: OpenECS [OPTION...] [FILE...]\n"                                                          \
    "\n"                                                                                              \
    "Starts the launcher, which lists the presets of the presets folders and the saved sessions,\n"   \
    "or the tool of a preset or a session. The preset's open function opens each FILE.\n"             \
    "\n"                                                                                              \
    "Options:\n"                                                                                      \
    "  -p, --preset NAME|FILE  Start from a preset: a name from the presets folders, or a file\n"     \
    "  -s, --session FILE      Open a saved session\n"                                                \
    "  -f, --fresh             Start from the preset, not from the tool's last session\n"             \
    "  -t, --test FILE         Run a test; Debug builds only\n"                                       \
    "  -d, --definitions DIR   Write the definition files of every plugin found into DIR, and exit\n" \
    "  -v, --version           Print the version and exit\n"                                          \
    "  -h, --help              Print this help and exit\n"

/// @brief One option of the command line.
typedef struct ECSIOption
{
    const char *shortName;
    const char *longName;
    bool takesValue;
} ECSIOption;

/// @brief The options, in the order of ECSIOptionIndex.
static const ECSIOption OPENECS_OPTIONS[] = {
    {"-p", "--preset", true},
    {"-s", "--session", true},
    {"-f", "--fresh", false},
    {"-t", "--test", true},
    {"-d", "--definitions", true},
    {"-v", "--version", false},
    {"-h", "--help", false},
};

typedef enum ECSIOptionIndex
{
    ECSIOptionIndex_Preset,
    ECSIOptionIndex_Session,
    ECSIOptionIndex_Fresh,
    ECSIOptionIndex_Test,
    ECSIOptionIndex_Definitions,
    ECSIOptionIndex_Version,
    ECSIOptionIndex_Help,
} ECSIOptionIndex;

/// @brief Reads the options, and moves the files to the start of argv, over options already read.
/// @return false if an option is unknown or lacks its value; the reason and the help are printed.
static bool ECSIMain_ReadArguments(int argc, char **argv, ECSIArguments *retArguments)
{
    *retArguments = (ECSIArguments){.preset = OPENECS_DEFAULT_PRESET, .files = argv + 1};

    for (int i = 1; i < argc; i++)
    {
        // anything that does not start with a dash is a file
        if (argv[i][0] != '-')
        {
            retArguments->files[retArguments->fileCount++] = argv[i];
            continue;
        }

        usz option = SDL_arraysize(OPENECS_OPTIONS);

        for (usz j = 0; j < SDL_arraysize(OPENECS_OPTIONS); j++)
        {
            if (SDL_strcmp(argv[i], OPENECS_OPTIONS[j].shortName) == 0 || SDL_strcmp(argv[i], OPENECS_OPTIONS[j].longName) == 0)
            {
                option = j;
            }
        }

        if (option == SDL_arraysize(OPENECS_OPTIONS))
        {
            fprintf(stderr, "OpenECS: unknown option '%s'.\n\n%s", argv[i], OPENECS_USAGE);
            return false;
        }

        const char *value = nullptr;

        if (OPENECS_OPTIONS[option].takesValue)
        {
            if (i + 1 >= argc)
            {
                fprintf(stderr, "OpenECS: the option '%s' needs a value.\n\n%s", argv[i], OPENECS_USAGE);
                return false;
            }

            value = argv[++i];
        }

        switch ((ECSIOptionIndex)option)
        {
        case ECSIOptionIndex_Preset:
            retArguments->preset = value;
            break;
        case ECSIOptionIndex_Session:
            retArguments->session = value;
            break;
        case ECSIOptionIndex_Fresh:
            retArguments->fresh = true;
            break;
        case ECSIOptionIndex_Test:
            retArguments->test = value;
            break;
        case ECSIOptionIndex_Definitions:
            retArguments->definitions = value;
            break;
        case ECSIOptionIndex_Version:
            retArguments->version = true;
            break;
        case ECSIOptionIndex_Help:
            retArguments->help = true;
            break;
        }
    }

    return true;
}

#if defined(SDL_PLATFORM_WINDOWS)
/// @brief A Windows build other than Debug has no console of its own (DESIGN 14.4). Started from a console, it writes to that console, unless its output goes somewhere already, such as a pipe.
static void ECSIMain_AttachConsole(void)
{
    if (GetStdHandle(STD_ERROR_HANDLE) == nullptr && AttachConsole(ATTACH_PARENT_PROCESS))
    {
        (void)freopen("CONOUT$", "w", stdout);
        (void)freopen("CONOUT$", "w", stderr);
    }
}
#endif

#pragma endregion Source Only

int main(int argc, char **argv)
{
#if defined(SDL_PLATFORM_WINDOWS)
    ECSIMain_AttachConsole();
#endif
    ECSILog_Initialize();
    ECSIArguments arguments;

    if (!ECSIMain_ReadArguments(argc, argv, &arguments))
    {
        return 2;
    }

    if (arguments.version)
    {
        printf("OpenECS %s, plugin API %d\n", OPENECS_VERSION, OPENECS_API_VERSION);
        return 0;
    }

    if (arguments.help)
    {
        printf("%s", OPENECS_USAGE);
        return 0;
    }

    ECSIApp_Start(&arguments);
    int status = arguments.definitions != nullptr ? (ECSIApp_WriteDefinitions(arguments.definitions) ? 1 : 0) : ECSIApp_Run();
    ECSIRestart restart = ECSIApp_Stop();

    // a session or preset opened while OpenECS ran starts a new OpenECS in place of this one; SDL can start a process but not replace one
    if (restart.path != nullptr)
    {
#if defined(SDL_PLATFORM_WINDOWS)
        // Windows cannot replace a process either, so the new one starts and this one ends; SDL quotes its arguments for the command line
        char *program = nullptr;
        SDL_Process *process = _get_pgmptr(&program) == 0 ? SDL_CreateProcess((const char *[]){program, restart.option, restart.path, nullptr}, false) : nullptr;

        if (process != nullptr)
        {
            SDL_DestroyProcess(process);
            SDL_free(restart.path);
            return status;
        }
#else
        execv("/proc/self/exe", (char *[]){argv[0], (char *)restart.option, restart.path, nullptr});
#endif
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot start again from '%s'.", restart.path);
        SDL_free(restart.path);
        return 1;
    }

    return status;
}
