#define SHU_IMPLEMENTATION
#define SHUC_ENABLE_INCREMENTAL
#define SHUC_NO_RUN_LOG
#include "dependencies/shuild/shuild.h"

#pragma region Setup

#define PrintUsage() SHU_LogInfo("\n\n\
Usage:\n\
./shuild [TYPE [LINK]]\n\n\
Arguments:\n\
TYPE\n\tD   Debug (Default)\n\tR   Release\n\tRD  RelWithDebInfo\n\tSR  MinSizeRel\n\
LINK\n\tS   Static (Default)\n\tD   Dynamic\n")

typedef enum BuildType
{
    BuildType_Debug,
    BuildType_Release,
    BuildType_RelWithDebInfo,
    BuildType_MinSizeRel,
} BuildType;

static const char *const _BUILD_TYPE_STRINGS[] = {"Debug", "Release", "RelWithDebInfo", "MinSizeRel"};
#define BuildType_String(buildType) _BUILD_TYPE_STRINGS[(buildType)]

static const char *const _LINK_TYPE_STRINGS[] = {"", "Static", "Dynamic"};
#define LinkType_String(linkType) _LINK_TYPE_STRINGS[(linkType)]

BuildType BUILD_TYPE = BuildType_Debug;
SHUModuleType LINK_TYPE = SHUModuleType_LibraryStatic;

SHUI_String BUILD_DIRECTORY = {0};
SHUI_String OUTPUT_DIRECTORY = {0};

#pragma endregion Setup

static void SetupConfiguration(int argc, char **argv);
static void SetBuildFlags(bool warnings);

static void Shuild_SDL(void);
static void Shuild_SDL_ttf(void);
static void Shuild_lua(void);
static void Shuild_clay(void);
static void Shuild_other(void);
static void Shuild_OpenECS(void);

int main(int argc, char **argv)
{
    SHU_CompilerTryConfigure("gcc");
    SHU_UtilAutomate(argc, argv);
    SetupConfiguration(argc, argv);

    Shuild_SDL();
    Shuild_lua();
    Shuild_SDL_ttf();
    Shuild_clay();
    Shuild_other();
    Shuild_OpenECS();

    return 0;
}

static void CopyFile(const char *file, const char *directory)
{
    SHU_UtilRun("cp -r %s %s", file, directory);
}

static void SetupConfiguration(int argc, char **argv)
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
            exit(1);
        }
    }

    if (argc >= 3)
    {
        if (!strcasecmp(argv[2], "S"))
        {
            LINK_TYPE = SHUModuleType_LibraryStatic;
        }
        else if (!strcasecmp(argv[2], "D"))
        {
            LINK_TYPE = SHUModuleType_LibraryDynamic;
        }
        else
        {
            SHU_LogError(0, "Unknown link type: '%s'", argv[2]);
            PrintUsage();
            exit(1);
        }
    }

    SHU_LogInfo("Build type: " SHUM_COLOR_BLUE("'%s'"), BuildType_String(BUILD_TYPE));
    SHU_LogInfo("Link type: " SHUM_COLOR_BLUE("'%s'"), LinkType_String(LINK_TYPE));

    SHUI_SFormat(&BUILD_DIRECTORY, ".shu/%s/%s/", LinkType_String(LINK_TYPE), BuildType_String(BUILD_TYPE));
    SHUI_SFormat(&OUTPUT_DIRECTORY, "build/%s/%s/", LinkType_String(LINK_TYPE), BuildType_String(BUILD_TYPE));

    SHU_CacheConfigure(BUILD_DIRECTORY.data);
}

static void SetBuildFlags(bool warnings)
{
    SHU_CompilerClearFlags();

    switch (BUILD_TYPE)
    {
    case BuildType_Debug:
        if (warnings)
        {
            SHU_CompilerAddFlags(SHUM_FLAGS_WARNING_MID);
        }
        SHU_CompilerAddFlags(SHUM_FLAGS_DEBUG SHUM_FLAGS_OPTIMIZATION_DEBUG);
        SHU_CompilerAddDefinitions("DEBUG", NULL);
        break;
    case BuildType_Release:
        SHU_CompilerAddFlags(SHUM_FLAGS_OPTIMIZATION_HIGH);
        SHU_CompilerAddDefinitions("NDEBUG", NULL, "SHU_NO_ASSERT", NULL);
        break;
    case BuildType_RelWithDebInfo:
        if (warnings)
        {
            SHU_CompilerAddFlags(SHUM_FLAGS_WARNING_LOW);
        }
        SHU_CompilerAddFlags(SHUM_FLAGS_DEBUG SHUM_FLAGS_OPTIMIZATION_MID);
        SHU_CompilerAddDefinitions("NDEBUG", NULL);
        break;
    case BuildType_MinSizeRel:
        SHU_CompilerAddFlags(SHUM_FLAGS_OPTIMIZATION_SIZE);
        SHU_CompilerAddDefinitions("NDEBUG", NULL);
        break;
    }
}

static void Shuild_SDL(void)
{
    SHU_LogInfo("Starting to build " SHUM_COLOR_MAGENTA("'SDL3'") "...");

    const char *root = SHU_UtilGetExecutablePath();
    const char *sharedOptStr = LINK_TYPE == SHUModuleType_LibraryDynamic ? "ON" : "OFF";
    const char *staticOptStr = LINK_TYPE == SHUModuleType_LibraryDynamic ? "OFF" : "ON";

    SHUI_String sourceDir;
    SHUI_String buildDir;
    SHUI_String outputPrefixDir;
    SHUI_SFormat(&sourceDir, "%sdependencies/SDL", root);
    SHUI_SFormat(&buildDir, "%s%sSDL3", root, BUILD_DIRECTORY.data);
    SHUI_SFormat(&outputPrefixDir, "%s%s", root, OUTPUT_DIRECTORY.data);

    SHU_UtilRun("cmake -S \"%s\" -B \"%s\" -G Ninja -DCMAKE_BUILD_TYPE=%s "
                "-DCMAKE_INSTALL_PREFIX=\"%s\" -DCMAKE_PREFIX_PATH=\"%s\" "
                "-DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_POSITION_INDEPENDENT_CODE=ON "
                "-DSDL_SHARED=%s -DSDL_STATIC=%s "                           // link type
                "-DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF " // options
                "--log-level=WARNING",                                       // logs
                sourceDir.data, buildDir.data, BuildType_String(BUILD_TYPE),
                outputPrefixDir.data, outputPrefixDir.data,
                sharedOptStr, staticOptStr);

    SHU_UtilRun(
        "cmake --build \"%s\" --parallel > "
#if SHUM_PLATFORM_IS_HOST(SHUM_PLATFORM_WINDOWS)
        "NUL"
#else
        "/dev/null"
#endif
        ,
        buildDir.data);

    SHU_UtilRun(
        "cmake --install \"%s\" > "
#if SHUM_PLATFORM_IS_HOST(SHUM_PLATFORM_WINDOWS)
        "NUL"
#else
        "/dev/null"
#endif
        ,
        buildDir.data);

    SHU_LogInfo("Done building " SHUM_COLOR_MAGENTA("'SDL3'"));
}

static void Shuild_SDL_ttf(void)
{
    SHU_LogInfo("Starting to build " SHUM_COLOR_MAGENTA("'SDL3_ttf'") "...");

    const char *root = SHU_UtilGetExecutablePath();
    const char *sharedOptStr = LINK_TYPE == SHUModuleType_LibraryDynamic ? "ON" : "OFF";
    const char *staticOptStr = LINK_TYPE == SHUModuleType_LibraryDynamic ? "OFF" : "ON";

    SHUI_String sourceDir;
    SHUI_String buildDir;
    SHUI_String outputPrefixDir;
    SHUI_SFormat(&sourceDir, "%sdependencies/SDL_ttf", root);
    SHUI_SFormat(&buildDir, "%s%sSDL3_ttf", root, BUILD_DIRECTORY.data);
    SHUI_SFormat(&outputPrefixDir, "%s%s", root, OUTPUT_DIRECTORY.data);

    SHU_UtilRun("cmake -S \"%s\" -B \"%s\" -G Ninja -DCMAKE_BUILD_TYPE=%s "
                "-DCMAKE_INSTALL_PREFIX=\"%s\" -DCMAKE_PREFIX_PATH=\"%s\" "
                "-DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_POSITION_INDEPENDENT_CODE=ON "
                "-DBUILD_SHARED_LIBS=%s "                   // link type
                "-DSDLTTF_VENDORED=ON -DSDLTTF_SAMPLES=OFF" // options
                "--log-level=WARNING",                      // logs
                sourceDir.data, buildDir.data, BuildType_String(BUILD_TYPE),
                outputPrefixDir.data, outputPrefixDir.data,
                sharedOptStr, staticOptStr);

    SHU_UtilRun(
        "cmake --build \"%s\" --parallel > "
#if SHUM_PLATFORM_IS_HOST(SHUM_PLATFORM_WINDOWS)
        "NUL"
#else
        "/dev/null"
#endif
        ,
        buildDir.data);

    SHU_UtilRun(
        "cmake --install \"%s\" > "
#if SHUM_PLATFORM_IS_HOST(SHUM_PLATFORM_WINDOWS)
        "NUL"
#else
        "/dev/null"
#endif
        ,
        buildDir.data);

    SHU_LogInfo("Done building " SHUM_COLOR_MAGENTA("'SDL3_ttf'"));
}

static void Shuild_lua(void)
{
    SHU_ModuleBegin("lua", "dependencies/lua");
    SetBuildFlags(false);

#if SHUM_PLATFORM_IS_HOST(SHUM_PLATFORM_LINUX)
    SHU_CompilerAddDefinitions("LUA_USE_LINUX", NULL);
#elif SHUM_PLATFORM_IS_HOST(SHUM_PLATFORM_MACOS)
    SHU_CompilerAddDefinitions("LUA_USE_MACOSX", NULL);
#endif

    SHU_CompilerAddDefinitions("MAKE_LIB", NULL);
    SHU_ModuleAddSourceFile("onelua.c");

    SHUI_String tempStr;
    SHUI_SFormat(&tempStr, "%sinclude/lua", OUTPUT_DIRECTORY.data);
    SHU_UtilCreateDirectory(tempStr.data);
    CopyFile("dependencies/lua/lua.h", tempStr.data);
    CopyFile("dependencies/lua/lauxlib.h", tempStr.data);
    CopyFile("dependencies/lua/lualib.h", tempStr.data);
    CopyFile("dependencies/lua/luaconf.h", tempStr.data);

    SHUI_SFormat(&tempStr, "%slib", OUTPUT_DIRECTORY.data);
    SHU_ModuleCompile(tempStr.data, LINK_TYPE);
}

static void Shuild_clay(void)
{
    SHU_ModuleBegin("clay", "dependencies/clay");
    SetBuildFlags(false);

    SHU_ModuleAddSourceFile("../other/clay.c");

    SHUI_String tempStr;

    SHUI_SFormat(&tempStr, "%sinclude/clay", OUTPUT_DIRECTORY.data);
    SHU_UtilCreateDirectory(tempStr.data);
    CopyFile("dependencies/clay/clay.h", tempStr.data);
    CopyFile("dependencies/other/claySDL3.h", tempStr.data);

    SHUI_SFormat(&tempStr, "%slib", OUTPUT_DIRECTORY.data);
    SHU_ModuleCompile(tempStr.data, LINK_TYPE);
}

static void Shuild_other(void)
{
    SHUI_String tempStr;
    SHUI_SFormat(&tempStr, "%sinclude/shu", OUTPUT_DIRECTORY.data);
    SHU_UtilCreateDirectory(tempStr.data);
    CopyFile("dependencies/shu/shu.h", tempStr.data);

    SHUI_SFormat(&tempStr, "%s/bin/resources", OUTPUT_DIRECTORY.data);
    SHU_UtilCreateDirectory(tempStr.data);
    CopyFile("resources", tempStr.data);
}

static void Shuild_OpenECS(void)
{
    SHU_ModuleBegin("OpenECS", NULL);
    SetBuildFlags(true);

    SHU_ModuleAddSourceFile("src/");
    SHU_ModuleAddIncludeDirectory("include/");

    SHUI_String tempStr;
    SHUI_SFormat(&tempStr, "%sinclude", OUTPUT_DIRECTORY.data);
    SHU_ModuleAddIncludeDirectory(tempStr.data);

    SHUI_SFormat(&tempStr, "%slib", OUTPUT_DIRECTORY.data);
    SHU_ModuleAddLibraryDirectory(tempStr.data);

    SHU_ModuleLinkLibrary("m");

    SHU_ModuleLinkLibrary("SDL3");
    SHU_ModuleLinkLibrary("SDL3_image");
    SHU_ModuleLinkLibrary("SDL3_mixer");
    SHU_ModuleLinkLibrary("SDL3_mixer");
    SHU_ModuleLinkLibrary("SDL3_net");
    SHU_ModuleLinkLibrary("SDL3_ttf");

    SHU_ModuleLinkLibrary("lua");

    SHU_ModuleLinkLibrary("clay");

    SHUI_SFormat(&tempStr, "%sbin", OUTPUT_DIRECTORY.data);
    SHU_ModuleCompile(tempStr.data, SHUModuleType_Executable);
}