#include "app/App.h"
#include "base/Log.h"

#include "SDL3/SDL.h"

#include <stdio.h>
#include <unistd.h>

#pragma region Source Only

/// @brief Preset used when the command line names none.
#define OPENECS_DEFAULT_PRESET "launcher"

/// @brief What --help prints.
#define OPENECS_USAGE                                                                                    \
    "Usage: OpenECS [OPTION...] [FILE...]\n"                                                            \
    "\n"                                                                                                \
    "Starts the tool a preset describes, or the launcher without one. The preset's open function\n"     \
    "opens each FILE.\n"                                                                                \
    "\n"                                                                                                \
    "Options:\n"                                                                                        \
    "  -p, --preset NAME|FILE  Start from a preset: a name from the presets folders, or a file\n"       \
    "  -s, --session FILE      Open a saved session\n"                                                  \
    "  -f, --fresh             Start from the preset, not from the tool's last session\n"               \
    "  -t, --test FILE         Run a test; Debug builds only\n"                                         \
    "  -v, --version           Print the version and exit\n"                                            \
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
    {"-v", "--version", false},
    {"-h", "--help", false},
};

typedef enum ECSIOptionIndex
{
    ECSIOption_Preset,
    ECSIOption_Session,
    ECSIOption_Fresh,
    ECSIOption_Test,
    ECSIOption_Version,
    ECSIOption_Help,
} ECSIOptionIndex;

/// @brief Reads the options, and moves the files to the start of argv, over options already read.
/// @return false if an option is unknown or lacks its value; the reason is printed.
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
            fprintf(stderr, "OpenECS: unknown option '%s'; OpenECS --help lists the options.\n", argv[i]);
            return false;
        }

        const char *value = NULL;

        if (OPENECS_OPTIONS[option].takesValue)
        {
            if (i + 1 >= argc)
            {
                fprintf(stderr, "OpenECS: the option '%s' needs a value; OpenECS --help lists the options.\n", argv[i]);
                return false;
            }

            value = argv[++i];
        }

        switch ((ECSIOptionIndex)option)
        {
        case ECSIOption_Preset:
            retArguments->preset = value;
            break;
        case ECSIOption_Session:
            retArguments->session = value;
            break;
        case ECSIOption_Fresh:
            retArguments->fresh = true;
            break;
        case ECSIOption_Test:
            retArguments->test = value;
            break;
        case ECSIOption_Version:
            retArguments->version = true;
            break;
        case ECSIOption_Help:
            retArguments->help = true;
            break;
        }
    }

    return true;
}

#pragma endregion Source Only

int main(int argc, char **argv)
{
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
    int status = ECSIApp_Run();
    const char *option = NULL;
    char *next = ECSIApp_Stop(&option);

    // a session or preset opened while OpenECS ran starts a new OpenECS in place of this one; SDL can start a process but not replace one
    if (next != NULL)
    {
        execv("/proc/self/exe", (char *[]){argv[0], (char *)option, next, NULL});
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot start again from '%s'.", next);
        SDL_free(next);
        return 1;
    }

    return status;
}
