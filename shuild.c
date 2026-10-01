#define SHU_IMPLEMENTATION
#define SHUC_ENABLE_INCREMENTAL
// #define SHUC_NO_RUN_LOG
#define SHUC_MAX_COMMAND_BUFFER_SIZE (1024 * 16) // default 4096 truncates the object list of a module with many sources
#include "dependencies/shuild/shuild.h"

#pragma region Platform And Setup

#define PrintUsage() SHU_LogInfo("\nUsage:\n\
    ./shuild\n\
    [D/R/RD/SR]({Debug}/Release/RelWithDebInfo/MinSizeRel)\n\
    [S/D]({Static}/Dynamic)")

typedef enum BuildType
{
    BuildType_Debug,
    BuildType_Release,
    BuildType_RelWithDebInfo,
    BuildType_MinSizeRel,
} BuildType;

static const char *const _BUILD_TYPE_STRINGS[] = {"Debug", "Release", "RelWithDebInfo", "MinSizeRel"};
#define BuildType_String(buildType) _BUILD_TYPE_STRINGS[(buildType)]

typedef enum LinkType
{
    LinkType_Static = 1,
    LinkType_Dynamic = 2
} LinkType;

static const char *const _LINK_TYPE_STRINGS[] = {"", "Static", "Dynamic"};
#define LinkType_String(linkType) _LINK_TYPE_STRINGS[(linkType)]

BuildType BUILD_TYPE = BuildType_Debug;
LinkType LINK_TYPE = LinkType_Static;

SHUI_String BUILD_DIRECTORY = {0};
SHUI_String OUTPUT_DIRECTORY = {0};

void Shuild_SetupConfiguration(int argc, char **argv)
{
    if (argc >= 2)
    {
        if (!strcasecmp(argv[1], "D"))
        {
            BUILD_TYPE = BuildType_Debug;
        }
        else if (!strcasecmp(argv[1], "R"))
        {
            BUILD_TYPE = BuildType_Release;
        }
        else if (!strcasecmp(argv[1], "RD"))
        {
            BUILD_TYPE = BuildType_RelWithDebInfo;
        }
        else if (!strcasecmp(argv[1], "SR"))
        {
            BUILD_TYPE = BuildType_MinSizeRel;
        }
        else
        {
            SHU_LogError(0, "Unknown build type: " SHUM_COLOR_RED("%s"), argv[1]);
            PrintUsage();
        }
    }

    if (argc >= 3)
    {
        if (!strcasecmp(argv[2], "S"))
        {
            LINK_TYPE = LinkType_Static;
        }
        else if (!strcasecmp(argv[2], "D"))
        {
            LINK_TYPE = LinkType_Dynamic;
        }
        else
        {
            SHU_LogError(0, "Unknown link type: '%s'", argv[2]);
            PrintUsage();
        }
    }

    SHU_LogInfo("Build type: " SHUM_COLOR_BLUE("'%s'"), BuildType_String(BUILD_TYPE));
    SHU_LogInfo("Link type: " SHUM_COLOR_BLUE("'%s'"), LinkType_String(LINK_TYPE));

    SHUI_SFormat(&BUILD_DIRECTORY, ".shu/%s/%s/", LinkType_String(LINK_TYPE), BuildType_String(BUILD_TYPE));
    SHUI_SFormat(&OUTPUT_DIRECTORY, "build/%s/%s/", LinkType_String(LINK_TYPE), BuildType_String(BUILD_TYPE));
}

#pragma endregion Platform And Setup

#pragma region SDL Dependencies

typedef struct SDLLibrary
{
    const char *name;    // Library name, also what you link with (-l<name>)
    const char *source;  // Source directory (submodule), relative to the shuild executable
    const char *options; // Extra cmake options
} SDLLibrary;

static const SDLLibrary SDL_LIBRARIES[] = {
    {"SDL3", "dependencies/SDL", "-DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF"},
    {"SDL3_image", "dependencies/SDL_image", "-DSDLIMAGE_VENDORED=ON -DSDLIMAGE_SAMPLES=OFF -DSDLIMAGE_AVIF=OFF -DSDLIMAGE_TIF=OFF -DSDLIMAGE_WEBP=OFF -DSDLIMAGE_JXL=OFF"},
    {"SDL3_mixer", "dependencies/SDL_mixer", "-DSDLMIXER_VENDORED=ON -DSDLMIXER_SAMPLES=OFF"},
    {"SDL3_net", "dependencies/SDL_net", ""},
    {"SDL3_ttf", "dependencies/SDL_ttf", "-DSDLTTF_VENDORED=ON -DSDLTTF_SAMPLES=OFF"},
};

static void Shuild_SDLs()
{
    const char *root = SHU_UtilGetExecutablePath();
    const char *sharedOptStr = LINK_TYPE == LinkType_Dynamic ? "ON" : "OFF";
    const char *staticOptStr = LINK_TYPE == LinkType_Dynamic ? "OFF" : "ON";

    for (usz i = 0; i < sizeof(SDL_LIBRARIES) / sizeof(SDL_LIBRARIES[0]); i++)
    {
        const SDLLibrary *library = &SDL_LIBRARIES[i];

        SHUI_String sourceDir = {0};
        SHUI_String buildDir = {0};
        SHUI_SFormat(&sourceDir, "%s%s", root, library->source);
        SHUI_SFormat(&buildDir, "%s%s%s", root, BUILD_DIRECTORY.data, library->name);

        SHUI_String linkOptions = {0};
        if (i == 0)
        {
            SHUI_SFormat(&linkOptions, "-DSDL_SHARED=%s -DSDL_STATIC=%s", sharedOptStr, staticOptStr);
        }
        else
        {
            SHUI_SFormat(&linkOptions, "-DBUILD_SHARED_LIBS=%s", sharedOptStr);
        }

        SHU_LogInfo("Building " SHUM_COLOR_MAGENTA("'%s'") " ...", library->name);

        SHU_UtilRun("cmake -S \"%s\" -B \"%s\" -G Ninja -DCMAKE_BUILD_TYPE=%s "
                    "-DCMAKE_INSTALL_LIBDIR=%s%s  -DCMAKE_POSITION_INDEPENDENT_CODE=ON %s %s",
                    sourceDir.data, buildDir.data, BuildType_String(BUILD_TYPE),
                    root, OUTPUT_DIRECTORY.data,
                    linkOptions.data, library->options);

        SHU_UtilRun("cmake --build \"%s\" --parallel", buildDir.data);
        SHU_UtilRun("cmake --install \"%s\"", buildDir.data);
    }
}

#pragma endregion SDL Dependencies

int main(int argc, char **argv)
{
    SHU_CompilerTryConfigure("gcc");
    SHU_UtilAutomate(argc, argv);
    Shuild_SetupConfiguration(argc, argv);

    switch (BUILD_TYPE)
    {
    case BuildType_Debug:
        SHU_CompilerAddFlags(SHUM_FLAGS_DEBUG SHUM_FLAGS_WARNING_HIGH);
        break;
    case BuildType_Release:
        SHU_CompilerAddFlags(SHUM_FLAGS_OPTIMIZATION_HIGH);
        SHU_CompilerAddDefinitions("NDEBUG", NULL, "SHU_NO_ASSERT", NULL);
        break;
    case BuildType_RelWithDebInfo:
        SHU_CompilerAddFlags(SHUM_FLAGS_DEBUG SHUM_FLAGS_OPTIMIZATION_DEBUG SHUM_FLAGS_WARNING_LOW);
        SHU_CompilerAddDefinitions("NDEBUG", NULL);
        break;
    case BuildType_MinSizeRel:
        SHU_CompilerAddFlags(SHUM_FLAGS_OPTIMIZATION_SIZE);
        SHU_CompilerAddDefinitions("NDEBUG", NULL);
        break;
    }

    Shuild_SDLs();

    SHU_ModuleBegin("OpenECS", NULL);
    SHU_ModuleAddIncludeDirectory("include/");
    // SHU_ModuleAddIncludeDirectory("build/include");

    SHU_ModuleAddSourceFile("src/");

    SHU_ModuleCompile(BUILD_DIRECTORY.data, SHUModuleType_Executable);
    // SHU_ModuleAddLibraryDirectory("build/")

    // SHU_ModuleCompile(BUILD_DIRECTORY.data, LINK_TYPE);

    /* An executable that uses OpenECS (editor, game...) links SDL like this :

    SHU_ModuleBegin("editor", NULL);
    SHU_ModuleAddIncludeDirectory("include/");
    Shuild_SDLAddIncludes();
    SHU_ModuleAddSourceFile("editor/");
    SHU_ModuleAddLibraryDirectory(BUILD_DIRECTORY);
    SHU_ModuleLinkLibrary("OpenECS");
    Shuild_SDLLink();
    SHU_ModuleCompile(BUILD_DIRECTORY, SHUModuleType_Executable);
    */

    return 0;
}