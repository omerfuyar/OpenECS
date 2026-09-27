#define SHU_IMPLEMENTATION
#define SHUC_ENABLE_INCREMENTAL
#define SHUC_MAX_COMMAND_BUFFER_SIZE (1024 * 16) // default 4096 truncates the object list of a module with many sources
#include "dependencies/shuild/shuild.h"

#define PrintUsage() SHU_LogInfo("\nUsage : ./shuild [D/R/RD/SR](Debug/Release/{RelWithDebInfo}/MinSizeRel) [S/D]({Static}/Dynamic)\n");

#define SDL_SetBaseFlags()             \
    SHU_CompilerSetFlags(BUILD_FLAGS); \
    SHU_CompilerAddFlags("-fvisibility=hidden -fno-strict-aliasing")

void Shuild_SetupConfiguration(int argc, char **argv);

const char *BUILD_TYPE = "RelWithDebInfo";
const char *BUILD_FLAGS = SHUM_FLAGS_OPTIMIZATION_MID SHUM_FLAGS_DEBUG " -DNDEBUG";
const char *LINK_TYPE = "Static";

char LIBRARY_DIRECTORY[SHUC_MAX_STRING_SIZE] = {0};
char EXECUTABLE_DIRECTORY[SHUC_MAX_STRING_SIZE] = {0};

int main(int argc, char **argv)
{
    SHU_CompilerTryConfigure("gcc");
    SHU_UtilAutomate(argc, argv);

    Shuild_SetupConfiguration(argc, argv);

#if SHUM_PLATFORM_IS_HOST(SHUM_PLATFORM_MACOS)
    // homebrew installs outside the default search paths, /opt/homebrew on apple silicon and /usr/local on intel
    Shuild_UtilAddSystemPrefix("/opt/homebrew");
    Shuild_UtilAddSystemPrefix("/usr/local");
#endif

    Shuild_SDL_net();
    Shuild_SDL_image();
    Shuild_SDL_mixer();

    SHU_CompilerSetFlags(BUILD_FLAGS);
    SHU_CompilerAddFlags(SHUM_FLAGS_WARNING_LOW SHUM_FLAGS_STANDARD_C23);

    SHU_ModuleBegin("OpenECS", NULL);
    SHU_ModuleAddIncludeDirectory("include/");
    SHU_ModuleAddSourceFile("src/");

    // SDL and SDL_ttf come from the package manager of the system, see README for the packages
    SHU_ModuleAddLibraryDirectory(LIBRARY_DIRECTORY);
    SHU_ModuleLinkLibrary("SDL3_image");
    SHU_ModuleLinkLibrary("SDL3_mixer");
    SHU_ModuleLinkLibrary("SDL3_net");
    SHU_ModuleLinkLibrary("SDL3_ttf");
    SHU_ModuleLinkLibrary("SDL3");

#if SHUM_PLATFORM_IS_HOST(SHUM_PLATFORM_WINDOWS)
    SHU_ModuleLinkLibrary("ws2_32");
    SHU_ModuleLinkLibrary("iphlpapi");
#else
    SHU_ModuleLinkLibrary("m");
#endif

    SHU_ModuleCompile(EXECUTABLE_DIRECTORY, SHUModuleType_Executable);
    return 0;
}
