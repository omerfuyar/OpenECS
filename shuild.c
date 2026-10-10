#define SHU_IMPLEMENTATION
#define SHUC_ENABLE_INCREMENTAL
#define SHUC_NO_RUN_LOG
#include "dependencies/shuild/shuild.h"

#include <dirent.h>

#pragma region Setup

typedef enum BuildType
{
    BuildType_Debug,
    BuildType_Release,
    BuildType_RelWithDebInfo,
    BuildType_MinSizeRel,
} BuildType;

static const char *const _BUILD_TYPE_STRINGS[] = {"Debug", "Release", "RelWithDebInfo", "MinSizeRel"};
#define BuildType_String(buildType) _BUILD_TYPE_STRINGS[(buildType)]

/// @brief Names of the build types for -b, in the order of BuildType.
static const char *const BUILD_TYPE_NAMES[] = {"debug", "release", "relwithdebinfo", "minsizerel"};

/// @brief What the flags choose.
static struct
{
    BuildType type;
    bool std;      // also build the standard plugins, from std/
    bool examples; // also build the examples, from examples/
    bool tests;    // also build the tests
} CONFIG = {BuildType_Debug, true, false, false};

static SHUI_String BUILD_DIRECTORY = {0};
static SHUI_String OUTPUT_DIRECTORY = {0};

#pragma endregion Setup

static void SetupConfiguration(int argc, char **argv);
static void SetBuildFlags(bool ownCode);
static bool IsBuilt(const char *library, bool shared);

static void Shuild_SDL(void);
static void Shuild_SDL_ttf(void);
static void Shuild_lua(void);
static void Shuild_clay(void);
static void Shuild_stb(void);
static void Shuild_libffi(void);
static void Shuild_OpenECS(void);
static void Shuild_Plugins(void);
static void Shuild_other(void);

int main(int argc, char **argv)
{
    SHU_CompilerTryConfigure("gcc");
    SHU_UtilAutomate(argc, argv);
    SetupConfiguration(argc, argv);

    // dependencies are built only once
    // SDL and SDL_ttf are always shared libraries, so the core and the plugins that link SDL share one copy
    if (!IsBuilt("SDL3", true))
    {
        Shuild_SDL();
    }

    if (!IsBuilt("SDL3_ttf", true))
    {
        Shuild_SDL_ttf();
    }

    if (!IsBuilt("lua", false))
    {
        Shuild_lua();
    }

    if (!IsBuilt("clay", false))
    {
        Shuild_clay();
    }

    if (!IsBuilt("ffi", false))
    {
        Shuild_libffi();
    }

    if (!IsBuilt("stb", false))
    {
        Shuild_stb();
    }

    Shuild_other();
    Shuild_OpenECS();
    Shuild_Plugins();

    return 0;
}

static void CopyFile(const char *file, const char *directory)
{
    SHU_UtilRun("cp -r %s %s", file, directory);
}

static void PrintUsage(void)
{
    printf("Usage: ./shuild.ignore [FLAG...]\n\n"
           "Flags:\n"
           "  -b, --build TYPE   Build type: debug (the default), with the static analyzer and the sanitizers;\n"
           "                     release; relwithdebinfo, a release with debug information; or minsizerel, a small release\n"
           "  -n, --no-std       Do not build the standard plugins of std/\n"
           "  -e, --examples     Also build the examples of examples/, into bin/examples/\n"
           "  -t, --tests        Also build the tests, into tests/ beside bin/, and std's into std/tests/ beside bin/\n"
           "  -h, --help         Show this help\n");
}

/// @brief Stops the build with what is wrong, the argument, and the usage.
static void Refuse(const char *what, const char *argument)
{
    SHU_LogError(0, "%s: " SHUM_COLOR_RED("'%s'"), what, argument);
    PrintUsage();
    exit(1);
}

static void SetupConfiguration(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
    {
        const char *flag = argv[i];

        if (!strcmp(flag, "-b") || !strcmp(flag, "--build"))
        {
            const char *name = i + 1 < argc ? argv[++i] : "";
            usz type = 0;

            while (type < sizeof(BUILD_TYPE_NAMES) / sizeof(*BUILD_TYPE_NAMES) && strcasecmp(name, BUILD_TYPE_NAMES[type]) != 0)
            {
                type++;
            }

            if (type == sizeof(BUILD_TYPE_NAMES) / sizeof(*BUILD_TYPE_NAMES))
            {
                Refuse("Unknown build type", name);
            }

            CONFIG.type = (BuildType)type;
        }
        else if (!strcmp(flag, "-n") || !strcmp(flag, "--no-std"))
        {
            CONFIG.std = false;
        }
        else if (!strcmp(flag, "-e") || !strcmp(flag, "--examples"))
        {
            CONFIG.examples = true;
        }
        else if (!strcmp(flag, "-t") || !strcmp(flag, "--tests"))
        {
            CONFIG.tests = true;
        }
        else if (!strcmp(flag, "-h") || !strcmp(flag, "--help"))
        {
            PrintUsage();
            exit(0);
        }
        else
        {
            Refuse(flag[0] == '-' ? "Unknown flag" : "Unknown argument", flag);
        }
    }

    SHU_LogInfo("Build type: " SHUM_COLOR_BLUE("'%s'") ", standard plugins: " SHUM_COLOR_BLUE("%s") ", examples: " SHUM_COLOR_BLUE("%s") ", tests: " SHUM_COLOR_BLUE("%s"),
                BuildType_String(CONFIG.type), CONFIG.std ? "yes" : "no", CONFIG.examples ? "yes" : "no", CONFIG.tests ? "yes" : "no");

    SHUI_SFormat(&BUILD_DIRECTORY, ".shu/%s/", BuildType_String(CONFIG.type));
    SHUI_SFormat(&OUTPUT_DIRECTORY, "build/%s/", BuildType_String(CONFIG.type));

    SHU_CacheConfigure(BUILD_DIRECTORY.data);
}

// our code gets warnings, and in Debug the static analyzer and the address and undefined-behaviour sanitizers; dependencies get none of them
// release builds drop every SDL_assert
static void SetBuildFlags(bool ownCode)
{
    SHU_CompilerClearFlags();
    SHU_CompilerAddFlags(SHUM_FLAGS_STANDARD_C23);

    switch (CONFIG.type)
    {
    case BuildType_Debug:
        if (ownCode)
        {
            SHU_CompilerAddFlags(SHUM_FLAGS_WARNING_MID " -fanalyzer -fsanitize=address,undefined -fno-omit-frame-pointer");
        }
        SHU_CompilerAddFlags(SHUM_FLAGS_DEBUG SHUM_FLAGS_OPTIMIZATION_DEBUG);
        SHU_CompilerAddDefinitions("DEBUG", NULL, "SDL_ASSERT_LEVEL", "2");
        break;
    case BuildType_Release:
        SHU_CompilerAddFlags(SHUM_FLAGS_OPTIMIZATION_HIGH);
        SHU_CompilerAddDefinitions("NDEBUG", NULL, "SDL_ASSERT_LEVEL", "0");
        break;
    case BuildType_RelWithDebInfo:
        if (ownCode)
        {
            SHU_CompilerAddFlags(SHUM_FLAGS_WARNING_LOW);
        }
        SHU_CompilerAddFlags(SHUM_FLAGS_DEBUG SHUM_FLAGS_OPTIMIZATION_MID);
        SHU_CompilerAddDefinitions("NDEBUG", NULL, "SDL_ASSERT_LEVEL", "0");
        break;
    case BuildType_MinSizeRel:
        SHU_CompilerAddFlags(SHUM_FLAGS_OPTIMIZATION_SIZE);
        SHU_CompilerAddDefinitions("NDEBUG", NULL, "SDL_ASSERT_LEVEL", "0");
        break;
    }
}

static bool IsBuilt(const char *library, bool shared)
{
    SHUI_String path;
    SHUI_SFormat(&path, "%slib/lib%s.%s", OUTPUT_DIRECTORY.data, library, shared ? "so" : "a");
    return SHU_UtilFileExists(path.data) == SHUFileType_Regular;
}

static void Shuild_SDL(void)
{
    SHU_LogInfo("Starting to build " SHUM_COLOR_MAGENTA("'SDL3'") "...");

    const char *root = SHU_UtilGetExecutablePath();

    SHUI_String sourceDir;
    SHUI_String buildDir;
    SHUI_String outputPrefixDir;
    SHUI_SFormat(&sourceDir, "%sdependencies/SDL/", root);
    SHUI_SFormat(&buildDir, "%s%sSDL3/", root, BUILD_DIRECTORY.data);
    SHUI_SFormat(&outputPrefixDir, "%s%s", root, OUTPUT_DIRECTORY.data);

    SHU_UtilRun(
        "cmake -S \"%s\" -B \"%s\" -G Ninja -DCMAKE_BUILD_TYPE=%s "
        "-DCMAKE_INSTALL_PREFIX=\"%s\" -DCMAKE_PREFIX_PATH=\"%s\" "
        "-DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_POSITION_INDEPENDENT_CODE=ON "
        "-DSDL_SHARED=ON -DSDL_STATIC=OFF "                          // link type
        "-DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF " // options
        "--log-level=WARNING",                                       // logs
        sourceDir.data, buildDir.data, BuildType_String(CONFIG.type),
        outputPrefixDir.data, outputPrefixDir.data);

    SHU_UtilRun(
        "cmake --build \"%s\" --parallel > %s",
        buildDir.data, SHUM_PLATFORM_IS_HOST(SHUM_PLATFORM_WINDOWS) ? "NUL" : "/dev/null");

    SHU_UtilRun(
        "cmake --install \"%s\" > %s",
        buildDir.data, SHUM_PLATFORM_IS_HOST(SHUM_PLATFORM_WINDOWS) ? "NUL" : "/dev/null");

    SHU_LogInfo("Done building " SHUM_COLOR_MAGENTA("'SDL3'") "\n");
}

static void Shuild_SDL_ttf(void)
{
    SHU_LogInfo("Starting to build " SHUM_COLOR_MAGENTA("'SDL3_ttf'") "...");

    const char *root = SHU_UtilGetExecutablePath();

    SHUI_String sourceDir;
    SHUI_String buildDir;
    SHUI_String outputPrefixDir;

    SHUI_SFormat(&sourceDir, "%sdependencies/SDL_ttf/", root);
    SHUI_SFormat(&buildDir, "%s%sSDL3_ttf/", root, BUILD_DIRECTORY.data);
    SHUI_SFormat(&outputPrefixDir, "%s%s/", root, OUTPUT_DIRECTORY.data);

    SHU_UtilRun(
        "cmake -S \"%s\" -B \"%s\" -G Ninja -DCMAKE_BUILD_TYPE=%s "
        "-DCMAKE_INSTALL_PREFIX=\"%s\" -DCMAKE_PREFIX_PATH=\"%s\" "
        "-DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_POSITION_INDEPENDENT_CODE=ON "
        "-DBUILD_SHARED_LIBS=ON "                    // link type
        "-DSDLTTF_VENDORED=ON -DSDLTTF_SAMPLES=OFF " // options
        "--log-level=WARNING",                       // logs
        sourceDir.data, buildDir.data, BuildType_String(CONFIG.type),
        outputPrefixDir.data, outputPrefixDir.data);

    SHU_UtilRun(
        "cmake --build \"%s\" --parallel > %s",
        buildDir.data, SHUM_PLATFORM_IS_HOST(SHUM_PLATFORM_WINDOWS) ? "NUL" : "/dev/null");

    SHU_UtilRun(
        "cmake --install \"%s\" > %s",
        buildDir.data, SHUM_PLATFORM_IS_HOST(SHUM_PLATFORM_WINDOWS) ? "NUL" : "/dev/null");

    SHU_LogInfo("Done building " SHUM_COLOR_MAGENTA("'SDL3_ttf'") "\n");
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
    SHUI_SFormat(&tempStr, "%sinclude/lua/", OUTPUT_DIRECTORY.data);
    SHU_UtilCreateDirectory(tempStr.data);
    CopyFile("dependencies/lua/lua.h", tempStr.data);
    CopyFile("dependencies/lua/lauxlib.h", tempStr.data);
    CopyFile("dependencies/lua/lualib.h", tempStr.data);
    CopyFile("dependencies/lua/luaconf.h", tempStr.data);

    SHUI_SFormat(&tempStr, "%slib/", OUTPUT_DIRECTORY.data);
    SHU_ModuleCompile(tempStr.data, SHUModuleType_LibraryStatic);
}

static void Shuild_clay(void)
{
    SHU_ModuleBegin("clay", "dependencies/clay");
    SetBuildFlags(false);

    SHU_ModuleAddSourceFile("../other/clay/clay.c");

    SHUI_String tempStr;
    SHUI_SFormat(&tempStr, "../../%sinclude/", OUTPUT_DIRECTORY.data);
    SHU_ModuleAddIncludeDirectory(tempStr.data);

    SHUI_SFormat(&tempStr, "%slib/", OUTPUT_DIRECTORY.data);
    SHU_ModuleCompile(tempStr.data, SHUModuleType_LibraryStatic);

    // clay.c includes the headers from the submodule, so they are copied for the core after it compiles
    SHUI_SFormat(&tempStr, "%sinclude/clay/", OUTPUT_DIRECTORY.data);
    SHU_UtilCreateDirectory(tempStr.data);
    CopyFile("dependencies/clay/clay.h", tempStr.data);
    CopyFile("dependencies/other/clay/claySDL3.h", tempStr.data);
}

static void Shuild_stb(void)
{
    SHU_ModuleBegin("stb", "dependencies/other/stb");
    SetBuildFlags(false);

    // stb.c includes the copied glue header, so the headers are copied first
    SHUI_String tempStr;
    SHUI_SFormat(&tempStr, "%sinclude/stb/", OUTPUT_DIRECTORY.data);
    SHU_UtilCreateDirectory(tempStr.data);
    CopyFile("dependencies/stb/stb_ds.h", tempStr.data);
    CopyFile("dependencies/other/stb/stbSDL3.h", tempStr.data);

    SHU_ModuleAddSourceFile("stb.c");

    SHUI_SFormat(&tempStr, "../../../%sinclude/", OUTPUT_DIRECTORY.data);
    SHU_ModuleAddIncludeDirectory(tempStr.data);

    SHUI_SFormat(&tempStr, "%slib/", OUTPUT_DIRECTORY.data);
    SHU_ModuleCompile(tempStr.data, SHUModuleType_LibraryStatic);
}

/// @brief Reads a whole file. Free it with free.
static char *ReadWholeFile(const char *path)
{
    FILE *file = fopen(path, "rb");
    char *text = NULL;
    long length = -1;

    if (file != NULL && fseek(file, 0, SEEK_END) == 0 && (length = ftell(file)) >= 0 && fseek(file, 0, SEEK_SET) == 0)
    {
        text = calloc((size_t)length + 1, 1);
    }

    if (text != NULL && fread(text, 1, (size_t)length, file) != (size_t)length)
    {
        free(text);
        text = NULL;
    }

    if (file != NULL)
    {
        fclose(file);
    }

    return text;
}

/// @brief Writes libffi's ffi.h from its template, include/ffi.h.in, with the values that libffi's configure script would give on this machine.
static void MakeFfiHeader(const char *target, const char *path)
{
    // the version is the second argument of AC_INIT, such as AC_INIT([libffi],[3.8.0],...)
    char *configure = ReadWholeFile("dependencies/libffi/configure.ac");
    const char *init = configure == NULL ? NULL : strstr(configure, "AC_INIT([libffi],[");
    char version[32] = "";
    int major = 0;
    int minor = 0;
    int micro = 0;

    if (init == NULL || sscanf(init, "AC_INIT([libffi],[%31[^]]", version) != 1 || sscanf(version, "%d.%d.%d", &major, &minor, &micro) < 2)
    {
        SHU_LogError(SHUResult_ErrBadData, "Cannot find libffi's version in its configure.ac.");
        exit(1);
    }

    free(configure);
    char number[16];
    snprintf(number, sizeof(number), "%d", major * 10000 + minor * 100 + micro);

    const char *const values[][2] = {
        {"@VERSION@", version},
        {"@FFI_VERSION_STRING@", version},
        {"@FFI_VERSION_NUMBER@", number},
        {"@TARGET@", target},
        {"@HAVE_LONG_DOUBLE@", "1"},
        {"@FFI_EXEC_TRAMPOLINE_TABLE@", "0"},
    };

    char *text = ReadWholeFile("dependencies/libffi/include/ffi.h.in");
    FILE *header = fopen(path, "wb");

    if (text == NULL || header == NULL)
    {
        SHU_LogError(SHUResult_ErrFile, "Cannot write '%s' from libffi's include/ffi.h.in.", path);
        exit(1);
    }

    for (const char *cursor = text; *cursor != '\0';)
    {
        size_t i = 0;

        while (i < sizeof(values) / sizeof(*values) && strncmp(cursor, values[i][0], strlen(values[i][0])) != 0)
        {
            i++;
        }

        if (i < sizeof(values) / sizeof(*values))
        {
            fputs(values[i][1], header);
            cursor += strlen(values[i][0]);
        }
        else
        {
            fputc(*cursor++, header);
        }
    }

    fclose(header);
    free(text);
}

// libffi is compiled without its configure script: fficonfig.h is a glue header, and ffi.h is made from its template
static void Shuild_libffi(void)
{
#if defined(__x86_64__)
    const char *target = "X86_64";
    const char *targetDirectory = "src/x86/";
    const char *const targetSources[] = {"src/x86/ffi64.c", "src/x86/unix64.S", "src/x86/ffiw64.c", "src/x86/win64.S"};
#elif defined(__aarch64__)
    const char *target = "AARCH64";
    const char *targetDirectory = "src/aarch64/";
    const char *const targetSources[] = {"src/aarch64/ffi.c", "src/aarch64/sysv.S"};
#else
    SHU_LogError(SHUResult_ErrBadData, "libffi is configured for x86_64 and aarch64 only.");
    exit(1);
#endif

    // the sources include ffi.h and ffitarget.h, so they are written to the build before libffi compiles
    SHUI_String headers;
    SHUI_String tempStr;
    SHUI_SFormat(&headers, "%sinclude/libffi/", OUTPUT_DIRECTORY.data);
    SHU_UtilCreateDirectory(headers.data);
    SHUI_SFormat(&tempStr, "%sffi.h", headers.data);
    MakeFfiHeader(target, tempStr.data);
    SHUI_SFormat(&tempStr, "dependencies/libffi/%sffitarget.h", targetDirectory);
    CopyFile(tempStr.data, headers.data);

    SHU_ModuleBegin("ffi", "dependencies/libffi");
    SetBuildFlags(false);
    SHU_CompilerAddFlags(" -w"); // libffi warns about its own deprecated Java interface
    SHU_CompilerAddDefinitions("HAVE_CONFIG_H", NULL);

    const char *const sources[] = {"src/prep_cif.c", "src/types.c", "src/raw_api.c", "src/java_raw_api.c", "src/closures.c", "src/tramp.c"};

    for (usz i = 0; i < sizeof(sources) / sizeof(*sources); i++)
    {
        SHU_ModuleAddSourceFile(sources[i]);
    }

    for (usz i = 0; i < sizeof(targetSources) / sizeof(*targetSources); i++)
    {
        SHU_ModuleAddSourceFile(targetSources[i]);
    }

    // libffi checks its arguments in Debug builds
    if (CONFIG.type == BuildType_Debug)
    {
        SHU_CompilerAddDefinitions("FFI_DEBUG", NULL);
        SHU_ModuleAddSourceFile("src/debug.c");
    }

    SHU_ModuleAddIncludeDirectory("../other/libffi/");
    SHU_ModuleAddIncludeDirectory("include/");
    SHU_ModuleAddIncludeDirectory("src/");
    SHU_ModuleAddIncludeDirectory(targetDirectory);
    SHUI_SFormat(&tempStr, "../../%sinclude/libffi/", OUTPUT_DIRECTORY.data);
    SHU_ModuleAddIncludeDirectory(tempStr.data);

    SHUI_SFormat(&tempStr, "%slib/", OUTPUT_DIRECTORY.data);
    SHU_ModuleCompile(tempStr.data, SHUModuleType_LibraryStatic);
}

static void Shuild_OpenECS(void)
{
    SHU_ModuleBegin("OpenECS", NULL);
    SetBuildFlags(true);

    // the executable exports only the plugin interface: the OPENECS_EXPORT functions, whose names start with ECS
    // it finds the shared libraries of SDL and SDL_ttf in its own folder
    SHU_CompilerAddFlags(" -fvisibility=hidden '-Wl,--export-dynamic-symbol=ECS*' '-Wl,-rpath,$ORIGIN'");

    // the sanitizers find their settings in src/base/Sanitizers.c by name
    if (CONFIG.type == BuildType_Debug)
    {
        SHU_CompilerAddFlags(" '-Wl,--export-dynamic-symbol=__*san_default_*'");
    }

    SHU_ModuleAddSourceFile("src/");
    SHU_ModuleAddIncludeDirectory("include/");

    SHUI_String tempStr;
    SHUI_SFormat(&tempStr, "%sinclude/", OUTPUT_DIRECTORY.data);
    SHU_ModuleAddIncludeDirectory(tempStr.data);

    SHUI_SFormat(&tempStr, "%slib/", OUTPUT_DIRECTORY.data);
    SHU_ModuleAddLibraryDirectory(tempStr.data);

    SHU_ModuleLinkLibrary("clay");
    SHU_ModuleLinkLibrary("SDL3_ttf");
    SHU_ModuleLinkLibrary("SDL3");
    SHU_ModuleLinkLibrary("lua");
    SHU_ModuleLinkLibrary("ffi");
    SHU_ModuleLinkLibrary("stb");
    SHU_ModuleLinkLibrary("m");

    SHUI_SFormat(&tempStr, "%sbin/", OUTPUT_DIRECTORY.data);
    SHU_ModuleCompile(tempStr.data, SHUModuleType_Executable);

    // the shared libraries go next to the executable under the names it asks for, as files rather than links
    SHU_UtilRun("cp -L %slib/libSDL3.so.0 %slib/libSDL3_ttf.so.0 %s", OUTPUT_DIRECTORY.data, OUTPUT_DIRECTORY.data, tempStr.data);

    SHUI_SFormat(&tempStr, "%sinclude/", OUTPUT_DIRECTORY.data);
    CopyFile("include/OpenECS.h", tempStr.data);
    CopyFile("include/ecs.lua", tempStr.data);
}

/// @brief Checks whether a file name ends with a suffix.
static bool EndsWith(const char *name, const char *suffix)
{
    size_t nameLength = strlen(name);
    size_t suffixLength = strlen(suffix);
    return nameLength >= suffixLength && strcmp(name + nameLength - suffixLength, suffix) == 0;
}

/// @brief Calls a function for each folder in a folder, in no order; names starting with a dot are left out.
static void ForEachFolder(const char *path, void (*function)(const char *path, const char *name))
{
    DIR *folder = opendir(path);
    struct dirent *entry = NULL;

    while (folder != NULL && (entry = readdir(folder)) != NULL)
    {
        if (entry->d_type == DT_DIR && entry->d_name[0] != '.')
        {
            function(path, entry->d_name);
        }
    }

    if (folder != NULL)
    {
        closedir(folder);
    }
}

/// @brief Compiles the C files of a plugin's folder, root, into the plugin's native library in output. Does nothing if the folder has no C file.
static void Shuild_NativePlugin(const char *name, const char *root, const char *output)
{
    bool native = false;
    DIR *folder = opendir(root);
    struct dirent *entry = NULL;

    while (folder != NULL && (entry = readdir(folder)) != NULL)
    {
        native = native || EndsWith(entry->d_name, ".c");
    }

    if (folder != NULL)
    {
        closedir(folder);
    }

    if (!native)
    {
        return;
    }

    // the include path is relative to the plugin's folder, so it climbs one level for each folder in root
    SHUI_String include = {0};

    for (const char *c = root; *c != '\0'; c++)
    {
        if (*c == '/')
        {
            SHUI_SAppendC(&include, "../");
        }
    }

    SHUI_SAppendC(&include, OUTPUT_DIRECTORY.data);
    SHUI_SAppendC(&include, "include/");

    SHU_ModuleBegin(name, root);
    SetBuildFlags(true);
    SHU_CompilerAddFlags(" -fvisibility=hidden");

    // plugins see only the copied plugin header and the dependencies' headers, never the core's headers
    SHU_ModuleAddSourceFile("./");
    SHU_ModuleAddIncludeDirectory(include.data);
    SHU_ModuleCompile(output, SHUModuleType_LibraryDynamic);
}

// a plugin folder of the tests is built in place, in the copy of its folder
static void Shuild_TestPlugin(const char *path, const char *name)
{
    SHUI_String root;
    SHUI_String output;
    SHUI_SFormat(&root, "%s%s/", path, name);
    SHUI_SFormat(&output, "%s%s", OUTPUT_DIRECTORY.data, root.data);
    Shuild_NativePlugin(name, root.data, output.data);
}

/// @brief Builds a repository checked out in a folder of OpenECS, std/ or examples/, with the shuild.c of its own (DESIGN 17.7).
/// Its shuild.c is compiled with shu and shuild from dependencies/ into shuild.ignore in its folder, because shuild finds every path from the folder of the program it runs in.
/// It runs with the build type, this build's output folder as seen from its folder, and --tests if asked.
static void Shuild_Repository(const char *folder, bool tests)
{
    SHUI_String source;
    SHUI_SFormat(&source, "%sshuild.c", folder);

    if (SHU_UtilFileExists(source.data) != SHUFileType_Regular)
    {
        SHU_LogWarning("%s is not checked out, so it is not built; clone OpenECS with --recursive", folder);
        return;
    }

    // the output folder is named from the repository's folder, which is one level deeper for each of its folders
    SHUI_String output = {0};

    for (const char *c = folder; *c != '\0'; c++)
    {
        if (*c == '/')
        {
            SHUI_SAppendC(&output, "../");
        }
    }

    SHUI_SAppendC(&output, OUTPUT_DIRECTORY.data);

    SHU_LogInfo("Starting to build " SHUM_COLOR_MAGENTA("'%s'") "...", folder);
    SHU_UtilRun("gcc -O2 -Idependencies %s -o %sshuild.ignore", source.data, folder);
    SHU_UtilRun("cd %s && ./shuild.ignore -b %s -o %s%s", folder, BUILD_TYPE_NAMES[CONFIG.type], output.data, tests ? " -t" : "");
    SHU_LogInfo("Done building " SHUM_COLOR_MAGENTA("'%s'") "\n", folder);
}

static void Shuild_Plugins(void)
{
    // examples and tests are built when the flags ask for them; a copy from an earlier build is removed otherwise
    SHU_UtilRun("rm -rf %sbin/examples %stests %sstd", OUTPUT_DIRECTORY.data, OUTPUT_DIRECTORY.data, OUTPUT_DIRECTORY.data);

    // the tests are copied beside bin/, so their presets and plugins are found next to the test files, and their native plugins are built there
    if (CONFIG.tests)
    {
        SHU_UtilRun("cp -r tests %s", OUTPUT_DIRECTORY.data);
        ForEachFolder("tests/plugins/", Shuild_TestPlugin);
    }

    // OpenECS-std builds the standard plugins into bin/plugins/ and their presets into bin/presets/; OpenECS-examples builds the examples into bin/examples/
    if (CONFIG.std)
    {
        Shuild_Repository("std/", CONFIG.tests);
    }

    if (CONFIG.examples)
    {
        Shuild_Repository("examples/", false);
    }
}

static void Shuild_other(void)
{
    SHUI_String tempStr;
    SHUI_SFormat(&tempStr, "%sinclude/shu/", OUTPUT_DIRECTORY.data);
    SHU_UtilCreateDirectory(tempStr.data);
    CopyFile("dependencies/shu/shu.h", tempStr.data);

    SHUI_SFormat(&tempStr, "%sbin/", OUTPUT_DIRECTORY.data);

    SHU_UtilCreateDirectory(tempStr.data);
    CopyFile("resources/", tempStr.data);

}