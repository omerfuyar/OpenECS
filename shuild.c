#define SHU_IMPLEMENTATION
#define SHUC_ENABLE_INCREMENTAL
#include "dependencies/shuild/shuild.h"

#ifdef _WIN32
#else
#define FREETYPE2_DIR "/usr/include/freetype2"
#define HARFBUZZ_DIR "/usr/include/harfbuzz"
#endif

#define SDL_SetBaseFlags(verMajor, verMinor, verMicro)                      \
    SHU_CompilerSetFlags("-O3 -std=gnu99 -fvisibility=hidden -DBUILD_SDL"); \
    SHU_CompilerAddDefinitions("BUILD_SDL", NULL,                           \
                               "SDL_BUILD_MAJOR_VERSION", #verMajor,        \
                               "SDL_BUILD_MINOR_VERSION", #verMinor,        \
                               "SDL_BUILD_MICRO_VERSION", #verMicro)

// todo add configs for debug/release, static/dynamic builds etc.

void Shuild_SDL_net(void);
void Shuild_SDL_image(void);
void Shuild_SDL_mixer(void);
void Shuild_SDL_ttf(void);
void Shuild_SDL(void);

int main(int argc, char **argv)
{
    SHU_CompilerTryConfigure("gcc");
    SHU_UtilAutomate(argc, argv);

    Shuild_SDL_net();
    Shuild_SDL_image();
    Shuild_SDL_mixer();
    Shuild_SDL_ttf();

    SHU_CompilerSetFlags(SHUM_FLAGS_DEBUG SHUM_FLAGS_WARNING_LOW SHUM_FLAGS_STANDARD_C23);
    SHU_ModuleBegin("OpenECS", NULL);
    SHU_ModuleAddIncludeDirectory("include/");
    SHU_ModuleAddIncludeDirectory("dependencies/SDL/include/");
    SHU_ModuleAddIncludeDirectory("dependencies/SDL_net/include/");
    SHU_ModuleAddIncludeDirectory("dependencies/SDL_image/include/");
    SHU_ModuleAddIncludeDirectory("dependencies/SDL_mixer/include/");
    SHU_ModuleAddIncludeDirectory("dependencies/SDL_ttf/include/");
    SHU_ModuleAddSourceFile("src/");

    SHU_ModuleAddLibraryDirectory("build/lib/");
    SHU_ModuleLinkLibrary("SDL_net");
    SHU_ModuleLinkLibrary("SDL_image");
    SHU_ModuleLinkLibrary("SDL_mixer");
    SHU_ModuleLinkLibrary("SDL_ttf");
    SHU_ModuleLinkLibrary("SDL");
    SHU_ModuleLinkLibrary("freetype");
    SHU_ModuleLinkLibrary("harfbuzz");
    SHU_ModuleLinkLibrary("m");

    SHU_ModuleCompile("build/bin/", SHUModuleType_Executable);
    return 0;
}

void Shuild_SDL_net(void)
{
    SDL_SetBaseFlags(3, 3, 0);
    SHU_CompilerAddDefinitions("HAVE_GETIFADDRS", NULL);

    SHU_ModuleBegin("SDL_net", "dependencies/SDL_net/");
    SHU_ModuleAddIncludeDirectory("include/");
    SHU_ModuleAddIncludeDirectory("../SDL/include/");
    SHU_ModuleAddSourceFile("src/SDL_net.c");
    SHU_ModuleCompile("build/lib/", SHUModuleType_LibraryStatic);
}

void Shuild_SDL_image(void)
{
    SDL_SetBaseFlags(3, 5, 0);
    SHU_CompilerAddDefinitions("USE_STBIMAGE", NULL,
                               "LOAD_PNG", NULL, "LOAD_JPG", NULL,
                               "LOAD_BMP", NULL, "LOAD_GIF", NULL,
                               "LOAD_ANI", NULL, "LOAD_LBM", NULL,
                               "LOAD_PCX", NULL, "LOAD_PNM", NULL,
                               "LOAD_QOI", NULL, "LOAD_SVG", NULL,
                               "LOAD_TGA", NULL, "LOAD_XCF", NULL,
                               "LOAD_XPM", NULL, "LOAD_XV", NULL);

    SHU_ModuleBegin("SDL3_image", "dependencies/SDL_image/");
    SHU_ModuleAddIncludeDirectory("include/");
    SHU_ModuleAddIncludeDirectory("../SDL/include/");
    SHU_ModuleAddSourceFile("src/");
    SHU_ModuleCompile("build/lib/", SHUModuleType_LibraryStatic);
}

void Shuild_SDL_mixer(void)
{
    SDL_SetBaseFlags(3, 3, 0);
    SHU_CompilerAddDefinitions("DECODER_WAV", NULL, "DECODER_AIFF", NULL,
                               "DECODER_VOC", NULL, "DECODER_AU", NULL,
                               "DECODER_FLAC_DRFLAC", NULL,
                               "DECODER_MP3_DRMP3", NULL,
                               "DECODER_OGGVORBIS_STB", NULL,
                               "DECODER_MIDI_TIMIDITY", NULL);

    SHU_ModuleBegin("SDL_mixer", "dependencies/SDL_mixer/");
    SHU_ModuleAddIncludeDirectory("src/");
    SHU_ModuleAddIncludeDirectory("include/");
    SHU_ModuleAddIncludeDirectory("../SDL/include/");
    SHU_ModuleAddSourceFile("src/");
    SHU_ModuleCompile("build/lib/", SHUModuleType_LibraryStatic);
}

void Shuild_SDL_ttf(void)
{
    SDL_SetBaseFlags(3, 3, 0);
    SHU_CompilerAddDefinitions("TTF_USE_HARFBUZZ", "1");
    // system include paths can't go through AddIncludeDirectory
    SHU_CompilerAddFlags("-I" FREETYPE2_DIR " -I" HARFBUZZ_DIR);

    SHU_ModuleBegin("SDL_ttf", "dependencies/SDL_ttf/");
    SHU_ModuleAddIncludeDirectory("include/");
    SHU_ModuleAddIncludeDirectory("../SDL/include/");
    SHU_ModuleAddSourceFile("src/");
    SHU_ModuleCompile("build/lib/", SHUModuleType_LibraryStatic);
}

void Shuild_SDL(void)
{
    SDL_SetBaseFlags(3, 4, 16);

    SHU_ModuleBegin("SDL", "dependencies/SDL/");
    SHU_ModuleAddIncludeDirectory("include/");
    SHU_ModuleAddSourceFile("src/");
    SHU_ModuleCompile("build/lib/", SHUModuleType_LibraryStatic);
}