#define SHU_IMPLEMENTATION
#define SHUC_ENABLE_INCREMENTAL
#define SHUC_MAX_COMMAND_BUFFER_SIZE (1024 * 16) // default 4096 truncates the object list of a module with many sources
#include "dependencies/shuild/shuild.h"

// todo cross compilation somehow

#pragma region Platform And Setup

#define PrintUsage() SHU_LogInfo("\nUsage : \
    ./shuild\n\
    [D/R/RD/SR]({Debug}/Release/RelWithDebInfo/MinSizeRel)\n\
    [S/D]({Static}/Dynamic)\n\
    [XL/XW/XM/AL/AW/AM](X64Linux/X64Windows/X64MacOS/ARM64Windows/ARM64Linux/ARM64MacOS){Default is the host platform}\n\
    [<Compiler Command>]{Default is gcc}\n")

typedef enum BuildType
{
    BuildType_Debug,
    BuildType_Release,
    BuildType_RelWithDebInfo,
    BuildType_MinSizeRel,
} BuildType;

typedef enum BuildTarget
{
    BuildTarget_X64_Windows,
    BuildTarget_X64_Linux,
    BuildTarget_X64_MacOS,
    BuildTarget_ARM64_Windows,
    BuildTarget_ARM64_Linux,
    BuildTarget_ARM64_MacOS,
} BuildTarget;

BuildType BUILD_TYPE = BuildType_Debug;
const char *BUILD_TYPE_STR = "Debug";

SHUModuleType LINK_TYPE = SHUModuleType_LibraryStatic;
const char *LINK_TYPE_STR = "Static";

#ifdef __x86_64__
#ifdef _WIN32
BuildTarget BUILD_TARGET = BuildTarget_X64_Windows;
const char *BUILD_TARGET_STR = "X64Windows";
#elif __linux__
BuildTarget BUILD_TARGET = BuildTarget_X64_Linux;
const char *BUILD_TARGET_STR = "X64Linux";
#else
#error Unknown platform to build on.
#endif
#elif __aarch64__
#ifdef _WIN32
BuildTarget BUILD_TARGET = BuildTarget_ARM64_Windows;
const char *BUILD_TARGET_STR = "ARM64Windows";
#elif __linux__
BuildTarget BUILD_TARGET = BuildTarget_ARM64_Linux;
const char *BUILD_TARGET_STR = "ARM64Linux";
#else
#error Unknown platform to build on.
#endif
#elif __APPLE__
#ifdef __arm64__
BuildTarget BUILD_TARGET = BuildTarget_ARM64_MacOS;
const char *BUILD_TARGET_STR = "ARM64MacOS";
#else
BuildTarget BUILD_TARGET = BuildTarget_X64_MacOS;
const char *BUILD_TARGET_STR = "X64MacOS";
#endif
#else
#error Unknown architecture to build on.
#endif

SHUI_String BUILD_DIRECTORY_BASE = {0};

void Shuild_SetupConfiguration(int argc, char **argv)
{
    if (argc < 2)
    {
        goto setup;
    }

    if (!strcasecmp(argv[1], "D"))
    {
        BUILD_TYPE = BuildType_Debug;
        BUILD_TYPE_STR = "Debug";
    }
    else if (!strcasecmp(argv[1], "R"))
    {
        BUILD_TYPE = BuildType_Release;
        BUILD_TYPE_STR = "Release";
    }
    else if (!strcasecmp(argv[1], "RD"))
    {
        BUILD_TYPE = BuildType_RelWithDebInfo;
        BUILD_TYPE_STR = "RelWithDebInfo";
    }
    else if (!strcasecmp(argv[1], "SR"))
    {
        BUILD_TYPE = BuildType_MinSizeRel;
        BUILD_TYPE_STR = "MinSizeRel";
    }

    if (argc < 3)
    {
        goto setup;
    }

    if (!strcasecmp(argv[2], "S"))
    {
        LINK_TYPE = SHUModuleType_LibraryStatic;
        LINK_TYPE_STR = "Static";
    }
    else if (!strcasecmp(argv[2], "D"))
    {
        LINK_TYPE = SHUModuleType_LibraryDynamic;
        LINK_TYPE_STR = "Dynamic";
    }

    if (argc < 4)
    {
        goto setup;
    }

    if (!strcasecmp(argv[3], "XL"))
    {
        BUILD_TARGET = BuildTarget_X64_Linux;
        BUILD_TARGET_STR = "X64Linux";
    }
    else if (!strcasecmp(argv[3], "XW"))
    {
        BUILD_TARGET = BuildTarget_X64_Windows;
        BUILD_TARGET_STR = "X64Windows";
    }
    else if (!strcasecmp(argv[3], "XM"))
    {
        BUILD_TARGET = BuildTarget_X64_MacOS;
        BUILD_TARGET_STR = "X64MacOS";
    }
    else if (!strcasecmp(argv[3], "AL"))
    {
        BUILD_TARGET = BuildTarget_ARM64_Linux;
        BUILD_TARGET_STR = "ARM64Linux";
    }
    else if (!strcasecmp(argv[3], "AW"))
    {
        BUILD_TARGET = BuildTarget_ARM64_Windows;
        BUILD_TARGET_STR = "ARM64Windows";
    }
    else if (!strcasecmp(argv[3], "AM"))
    {
        BUILD_TARGET = BuildTarget_ARM64_MacOS;
        BUILD_TARGET_STR = "ARM64MacOS";
    }

setup:
    SHUI_SFormat(&BUILD_DIRECTORY_BASE, "%sbuild/%s/%s/%s/", SHU_UtilGetExecutablePath(), BUILD_TARGET_STR, LINK_TYPE_STR, BUILD_TYPE_STR);
}

#pragma endregion Platform And Setup

int main(int argc, char **argv)
{
    SHU_CompilerTryConfigure("gcc");
    SHU_UtilAutomate(argc, argv);

    Shuild_SetupConfiguration(argc, argv);

    SHU_ModuleBegin("OpenECS", NULL);
    SHU_ModuleAddIncludeDirectory("include/");
    SHU_ModuleAddSourceFile("src/");

    return 0;
}
