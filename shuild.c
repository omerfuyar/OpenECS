#define SHU_IMPLEMENTATION
#define SHUC_ENABLE_INCREMENTAL
#include "dependencies/shuild/shuild.h"

#pragma region Setup

typedef enum BuildType
{
    BuildType_Debug,
    BuildType_Release,
    BuildType_RelWithDebInfo,
    BuildType_MinSizeRel,
} BuildType;

static const char *const BUILD_TYPE_STRINGS[] = {"Debug", "Release", "RelWithDebInfo", "MinSizeRel"};
#define BuildType_String(buildType) BUILD_TYPE_STRINGS[(buildType)]

/// @brief What the flags choose.
static struct
{
    BuildType type;
    bool std;      // also build the standard plugins, from std/
    bool examples; // also build the examples, from examples/
    bool test;     // run the examples' tests after the build
    bool help;     // print the help, and build nothing
    const char *compiler;
    const char *badArgument; // the first argument that cannot be read, and why
    const char *badArgumentReason;
    bool windows; // the compiler compiles for Windows
    char *buildDir;
    char *cmakeDir; // the CMake builds, a folder for each compiler, as CMake keeps the compiler it configured with, such as .shu/Debug/cmake/gcc/
    char *outputDir;
} CONFIG = {.type = BuildType_Debug, .std = true, .compiler = SHUM_HOST_WINDOWS ? "clang" : "gcc"};

#pragma endregion Setup

static void ReadArguments(int argc, char **argv);
static void PrintUsage(void);
static void Configure(void);
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
static void Shuild_Definitions(void);
static void RunTests(void);

int main(int argc, char **argv)
{
    ReadArguments(argc, argv);
    SHU_CompilerConfigureTry(CONFIG.compiler);

    // a changed shuild.c is compiled again and run with the same arguments, so a bad argument is reported after it, by the new program
    SHU_UtilAutomate(argc, argv);

    if (CONFIG.badArgument != NULL)
    {
        PrintUsage();
        SHU_UtilFail("%s: " SHUM_LOG_VALUE("%s"), CONFIG.badArgumentReason, CONFIG.badArgument);
    }

    if (CONFIG.help)
    {
        PrintUsage();
        return 0;
    }

    Configure();

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

    if (CONFIG.test)
    {
        RunTests();
    }

    return 0;
}

/// @brief Deletes a file or folder if it exists.
static void DeleteIfExists(const char *path)
{
    if (SHU_UtilFileExists(path) != SHUFileType_Invalid)
    {
        SHU_UtilDeleteFile(path);
    }
}

static void PrintUsage(void)
{
    printf("Usage: ./shuild.ignore [FLAG...]\n\n"
           "Flags:\n"
           "  -b, --build TYPE   Build type: debug (the default), with the static analyzer and the sanitizers;\n"
           "                     release; relwithdebinfo, a release with debug information; or minsizerel, a small release\n"
           "  -n, --no-std       Do not build the standard plugins of std/\n"
           "  -e, --examples     Also build the examples of examples/, into bin/examples/\n"
           "  -t, --test         Build in Debug with the examples, then run every example's test. Prints pass or FAIL for each,\n"
           "                     and fails when a test fails or its output has a sanitizer's report\n"
           "  -c, --compiler CC  The compiler: gcc (the default), clang, or on Windows LLVM-MinGW's clang (the default there).\n"
           "                     A cross compiler, such as x86_64-w64-mingw32-gcc, builds for its system into build/TYPE-SYSTEM/\n"
           "  -h, --help         Show this help\n");
}

/// @brief Notes the first argument that cannot be read. main reports it, after shuild.c is compiled again if it changed.
static void NoteBadArgument(const char *reason, const char *argument)
{
    if (CONFIG.badArgument == NULL)
    {
        CONFIG.badArgumentReason = reason;
        CONFIG.badArgument = argument;
    }
}

/// @brief Reads every flag into CONFIG.
static void ReadArguments(int argc, char **argv)
{
    bool buildTypeNamed = false;

    for (int i = 1; i < argc; i++)
    {
        const char *flag = argv[i];
        bool takesValue = !strcmp(flag, "-b") || !strcmp(flag, "--build") || !strcmp(flag, "-c") || !strcmp(flag, "--compiler");

        if (takesValue && i + 1 == argc)
        {
            NoteBadArgument("A value is missing after the flag", flag);
            return;
        }

        if (!strcmp(flag, "-b") || !strcmp(flag, "--build"))
        {
            const char *name = argv[++i];
            usz type = 0;

            while (type < sizeof(BUILD_TYPE_STRINGS) / sizeof(*BUILD_TYPE_STRINGS) && strcasecmp(name, BuildType_String(type)) != 0)
            {
                type++;
            }

            if (type == sizeof(BUILD_TYPE_STRINGS) / sizeof(*BUILD_TYPE_STRINGS))
            {
                NoteBadArgument("Unknown build type", name);
                return;
            }

            CONFIG.type = (BuildType)type;
            buildTypeNamed = true;
        }
        else if (!strcmp(flag, "-c") || !strcmp(flag, "--compiler"))
        {
            CONFIG.compiler = argv[++i];
        }
        else if (!strcmp(flag, "-n") || !strcmp(flag, "--no-std"))
        {
            CONFIG.std = false;
        }
        else if (!strcmp(flag, "-e") || !strcmp(flag, "--examples"))
        {
            CONFIG.examples = true;
        }
        else if (!strcmp(flag, "-t") || !strcmp(flag, "--test"))
        {
            CONFIG.test = true;
        }
        else if (!strcmp(flag, "-h") || !strcmp(flag, "--help"))
        {
            CONFIG.help = true;
        }
        else
        {
            NoteBadArgument(flag[0] == '-' ? "Unknown flag" : "Unknown argument", flag);
            return;
        }
    }

    // only Debug builds run tests, and the tests are the examples' (DESIGN 17.5)
    if (CONFIG.test)
    {
        if (buildTypeNamed && CONFIG.type != BuildType_Debug)
        {
            NoteBadArgument("The tests run with a Debug build, not", BuildType_String(CONFIG.type));
            return;
        }

        CONFIG.examples = true;
    }
}

/// @brief Names the build's folders from the configured compiler, and configures shuild's cache.
static void Configure(void)
{
    // a build for another system goes to folders of its own, such as build/Debug-Windows/
    SHUPlatform target = SHU_CompilerGetTarget();
    const char *system = SHUM_PLATFORM_IS_HOST(target) ? "" : SHU_UtilFormat("-%s", SHUPlatform_GetString(target));
    CONFIG.windows = target == SHUPlatform_Windows;
    CONFIG.buildDir = SHU_UtilFormat(".shu/%s%s/", BuildType_String(CONFIG.type), system);
    CONFIG.outputDir = SHU_UtilFormat("build/%s%s/", BuildType_String(CONFIG.type), system);

    // a program built for another system cannot run here
    if (CONFIG.test && !SHUM_PLATFORM_IS_HOST(target))
    {
        SHU_UtilFail("the tests cannot run here, as OpenECS is built for " SHUM_LOG_VALUE("%s") ".", SHUPlatform_GetString(target));
    }

    SHU_CacheConfigure(CONFIG.buildDir);

    // a compiler named by its path, such as /usr/bin/clang, has a folder of its name
    const char *compilerName = CONFIG.compiler + strlen(CONFIG.compiler);

    while (compilerName > CONFIG.compiler && compilerName[-1] != '/' && compilerName[-1] != '\\')
    {
        compilerName--;
    }

    CONFIG.cmakeDir = SHU_UtilFormat("%scmake/%s/", CONFIG.buildDir, compilerName);

    SHU_UtilLog("building " SHUM_LOG_VALUE("%s") " with " SHUM_LOG_VALUE("%s") " for " SHUM_LOG_VALUE("%s") "; standard plugins: " SHUM_LOG_VALUE("%s") ", examples: " SHUM_LOG_VALUE("%s"),
                BuildType_String(CONFIG.type), CONFIG.compiler, SHUPlatform_GetString(target), CONFIG.std ? "yes" : "no", CONFIG.examples ? "yes" : "no");
}

// our code gets warnings, and in Debug gcc's static analyzer and the address and undefined-behaviour sanitizers, which MinGW does not have;
// dependencies get none of them, and compile without warnings, as their warnings are not ours to fix
// release builds drop every SDL_assert
static void SetBuildFlags(bool ownCode)
{
    SHU_CompilerClearFlags();
    SHU_CompilerAddFlags(SHUM_FLAGS_STANDARD_C23);

    if (!ownCode)
    {
        SHU_CompilerAddFlags("-w");
    }

    switch (CONFIG.type)
    {
    case BuildType_Debug:
        if (ownCode)
        {
            SHU_CompilerAddFlags(SHUM_FLAGS_WARNING_MID);

            if (SHU_CompilerGetFamily() == SHUCompiler_GCC)
            {
                SHU_CompilerAddFlags("-fanalyzer");
            }

            if (!CONFIG.windows)
            {
                SHU_CompilerAddFlags("-fsanitize=address,undefined", "-fno-omit-frame-pointer");
            }
        }
        SHU_CompilerAddFlags(SHUM_FLAGS_DEBUG, SHUM_FLAGS_OPTIMIZATION_DEBUG);
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
        SHU_CompilerAddFlags(SHUM_FLAGS_DEBUG, SHUM_FLAGS_OPTIMIZATION_MID);
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
    // lib/ has the file a library is linked by: on Windows, a DLL's import library, as the DLL is in bin/
    char *file = SHU_UtilLibraryFile(library, shared ? SHUModuleType_LibraryDynamic : SHUModuleType_LibraryStatic);
    return SHU_UtilFileExists(SHU_UtilFormat("%slib/%s", CONFIG.outputDir, file)) == SHUFileType_Regular;
}

/// @brief Builds a CMake project of dependencies/ and installs it into the build's folder. The output of each step goes to a log in the cache folder,
/// such as .shu/Debug/cmake/gcc/SDL3-build.log, and the end of the log shows when the step fails.
static void Shuild_CMake(const char *name, const char *source, const char *options)
{
    SHU_UtilLog(SHUM_LOG_NAME("%s") " (CMake): building", name);

    const char *root = SHU_UtilGetScriptDirectory();

    char *sourceDir = SHU_UtilFormat("%sdependencies/%s/", root, source);
    char *buildDir = SHU_UtilFormat("%s%s%s/", root, CONFIG.cmakeDir, name);
    char *outputPrefixDir = SHU_UtilFormat("%s%s", root, CONFIG.outputDir);

    // CMake compiles with the same compiler, and a cross compiler builds for its system
    char *compilers = SHU_UtilFormat("-DCMAKE_C_COMPILER=\"%s\" -DCMAKE_CXX_COMPILER=\"%s\"", CONFIG.compiler, SHU_CompilerGetTool("g++", "clang++"));

    if (!SHUM_PLATFORM_IS_HOST(SHU_CompilerGetTarget()))
    {
        compilers = SHU_UtilFormat("%s -DCMAKE_SYSTEM_NAME=%s -DCMAKE_RC_COMPILER=\"%s\"", compilers, SHUPlatform_GetString(SHU_CompilerGetTarget()), SHU_CompilerGetTool("windres", "windres"));
    }

    char *log = SHU_UtilFormat("%s%s-configure.log", CONFIG.cmakeDir, name);
    SHU_UtilRunLogged(log,
                      "cmake -S \"%s\" -B \"%s\" -G Ninja -DCMAKE_BUILD_TYPE=%s %s "
                      "-DCMAKE_INSTALL_PREFIX=\"%s\" -DCMAKE_PREFIX_PATH=\"%s\" "
                      "-DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_POSITION_INDEPENDENT_CODE=ON %s",
                      sourceDir, buildDir, BuildType_String(CONFIG.type), compilers,
                      outputPrefixDir, outputPrefixDir, options);

    log = SHU_UtilFormat("%s%s-build.log", CONFIG.cmakeDir, name);
    SHU_UtilRunLogged(log, "cmake --build \"%s\" --parallel", buildDir);

    log = SHU_UtilFormat("%s%s-install.log", CONFIG.cmakeDir, name);
    SHU_UtilRunLogged(log, "cmake --install \"%s\"", buildDir);

    SHU_UtilLog(SHUM_LOG_NAME("%s") " (CMake): built", name);
}

static void Shuild_SDL(void)
{
    Shuild_CMake("SDL3", "SDL",
                 "-DSDL_SHARED=ON -DSDL_STATIC=OFF "                           // link type
                 "-DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF"); // options
}

static void Shuild_SDL_ttf(void)
{
    Shuild_CMake("SDL3_ttf", "SDL_ttf",
                 "-DBUILD_SHARED_LIBS=ON "                     // link type
                 "-DSDLTTF_VENDORED=ON -DSDLTTF_SAMPLES=OFF"); // options
}

static void Shuild_lua(void)
{
    char *headerOutputDir = SHU_UtilFormat("%sinclude/lua/", CONFIG.outputDir);
    SHU_UtilCopyFile("dependencies/lua/lua.h", headerOutputDir);
    SHU_UtilCopyFile("dependencies/lua/lauxlib.h", headerOutputDir);
    SHU_UtilCopyFile("dependencies/lua/lualib.h", headerOutputDir);
    SHU_UtilCopyFile("dependencies/lua/luaconf.h", headerOutputDir);

    char *binOutputDir = SHU_UtilFormat("%slib/", CONFIG.outputDir);

    SHU_MODULE("lua", "dependencies/lua", SHUModuleType_LibraryStatic, binOutputDir)
    {
        SetBuildFlags(false);

        // Lua finds Windows by itself
        if (SHU_CompilerGetTarget() == SHUPlatform_Linux)
        {
            SHU_CompilerAddDefinitions("LUA_USE_LINUX", NULL);
        }
        else if (SHU_CompilerGetTarget() == SHUPlatform_MacOS)
        {
            SHU_CompilerAddDefinitions("LUA_USE_MACOSX", NULL);
        }

        SHU_CompilerAddDefinitions("MAKE_LIB", NULL);
        SHU_ModuleAddSourceFiles("onelua.c");
    }
}

static void Shuild_clay(void)
{
    char *clayIncludeDir = SHU_UtilFormat("../../%sinclude/", CONFIG.outputDir);
    char *binOutputDir = SHU_UtilFormat("%slib/", CONFIG.outputDir);

    SHU_MODULE("clay", "dependencies/clay", SHUModuleType_LibraryStatic, binOutputDir)
    {
        SetBuildFlags(false);
        SHU_ModuleAddSourceFiles("../other/clay/clay.c");
        SHU_ModuleAddIncludeDirectories(clayIncludeDir);
    }

    // clay.c includes the headers from the submodule, so they are copied for the core after it compiles
    char *headerOutputDir = SHU_UtilFormat("%sinclude/clay/", CONFIG.outputDir);
    SHU_UtilCopyFile("dependencies/clay/clay.h", headerOutputDir);
    SHU_UtilCopyFile("dependencies/other/clay/claySDL3.h", headerOutputDir);
}

static void Shuild_stb(void)
{
    // stb.c includes the copied glue header, so the headers are copied first
    char *headerOutputDir = SHU_UtilFormat("%sinclude/stb/", CONFIG.outputDir);
    SHU_UtilCopyFile("dependencies/stb/stb_ds.h", headerOutputDir);
    SHU_UtilCopyFile("dependencies/other/stb/stbSDL3.h", headerOutputDir);

    char *includeDir = SHU_UtilFormat("../../../%sinclude/", CONFIG.outputDir);
    char *binOutputDir = SHU_UtilFormat("%slib/", CONFIG.outputDir);

    SHU_MODULE("stb", "dependencies/other/stb", SHUModuleType_LibraryStatic, binOutputDir)
    {
        SetBuildFlags(false);
        SHU_ModuleAddSourceFiles("stb.c");
        SHU_ModuleAddIncludeDirectories(includeDir);
    }
}

/// @brief Writes libffi's ffi.h from its template, include/ffi.h.in, with the values that libffi's configure script would give on this machine.
static void MakeFfiHeader(const char *target, const char *path)
{
    // the version is the second argument of AC_INIT, such as AC_INIT([libffi],[3.8.0],...)
    SHUSlice configure = SHU_UtilReadFile("dependencies/libffi/configure.ac");
    const char *init = strstr(configure.data, "AC_INIT([libffi],[");
    char version[32] = "";
    int major = 0;
    int minor = 0;
    int micro = 0;

    if (init == NULL || sscanf(init, "AC_INIT([libffi],[%31[^]]", version) != 1 || sscanf(version, "%d.%d.%d", &major, &minor, &micro) < 2)
    {
        SHU_UtilFail("libffi's version is not in its " SHUM_LOG_PATH("configure.ac") ".");
    }

    free(configure.data);
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

    // each name is at least 8 characters, and each value at most 31, so the header is at most 4 times as long as its template
    SHUSlice text = SHU_UtilReadFile("dependencies/libffi/include/ffi.h.in");
    char *header = malloc(text.size * 4 + 1);
    usz length = 0;
    if (header == NULL)
    {
        SHU_UtilFail(SHUM_LOG_PATH("%s") " cannot be written from " SHUM_LOG_PATH("dependencies/libffi/include/ffi.h.in") ".", path);
    }

    for (const char *cursor = text.data; *cursor != '\0';)
    {
        size_t i = 0;

        while (i < sizeof(values) / sizeof(*values) && strncmp(cursor, values[i][0], strlen(values[i][0])) != 0)
        {
            i++;
        }

        if (i < sizeof(values) / sizeof(*values))
        {
            memcpy(header + length, values[i][1], strlen(values[i][1]));
            length += strlen(values[i][1]);
            cursor += strlen(values[i][0]);
        }
        else
        {
            header[length++] = *cursor++;
        }
    }

    SHU_UtilWriteFile(path, csv(cs(header, length)));
    free(header);
    free(text.data);
}

// libffi is compiled without its configure script: fficonfig.h is a glue header, and ffi.h is made from its template
static void Shuild_libffi(void)
{
    // Windows is built for on x86_64, and Linux on the host's processor
    const char *target = "X86_WIN64";
    const char *targetDirectory = "src/x86/";
    const char *const windowsSources[] = {"src/x86/ffiw64.c", "src/x86/win64.S"};
    const char *const *targetSources = windowsSources;
    usz targetSourceCount = sizeof(windowsSources) / sizeof(*windowsSources);

#if defined(__x86_64__)
    const char *const linuxSources[] = {"src/x86/ffi64.c", "src/x86/unix64.S", "src/x86/ffiw64.c", "src/x86/win64.S"};
    const char *linuxTarget = "X86_64";
#elif defined(__aarch64__)
    const char *const linuxSources[] = {"src/aarch64/ffi.c", "src/aarch64/sysv.S"};
    const char *linuxTarget = "AARCH64";
    targetDirectory = CONFIG.windows ? targetDirectory : "src/aarch64/";
#else
    const char *const linuxSources[] = {""};
    const char *linuxTarget = NULL;
#endif

    if (!CONFIG.windows)
    {
        if (linuxTarget == NULL)
        {
            SHU_UtilFail(SHUM_LOG_NAME("libffi") " is configured for x86_64 and aarch64 only.");
        }

        target = linuxTarget;
        targetSources = linuxSources;
        targetSourceCount = sizeof(linuxSources) / sizeof(*linuxSources);
    }

    // the sources include ffi.h and ffitarget.h, so they are written to the build before libffi compiles; the copy makes the folder
    char *headers = SHU_UtilFormat("%sinclude/libffi/", CONFIG.outputDir);
    char *targetHeader = SHU_UtilFormat("dependencies/libffi/%sffitarget.h", targetDirectory);
    SHU_UtilCopyFile(targetHeader, headers);
    char *ffiHeader = SHU_UtilFormat("%sffi.h", headers);
    MakeFfiHeader(target, ffiHeader);

    char *headersFromRoot = SHU_UtilFormat("../../%sinclude/libffi/", CONFIG.outputDir);
    char *binOutputDir = SHU_UtilFormat("%slib/", CONFIG.outputDir);

    SHU_MODULE("ffi", "dependencies/libffi", SHUModuleType_LibraryStatic, binOutputDir)
    {
        SetBuildFlags(false);
        SHU_CompilerAddDefinitions("HAVE_CONFIG_H", NULL);

        SHU_ModuleAddSourceFiles("src/prep_cif.c", "src/types.c", "src/raw_api.c", "src/java_raw_api.c", "src/closures.c", "src/tramp.c");

        for (usz i = 0; i < targetSourceCount; i++)
        {
            SHU_ModuleAddSourceFiles(targetSources[i]);
        }

        // libffi checks its arguments in Debug builds
        if (CONFIG.type == BuildType_Debug)
        {
            SHU_CompilerAddDefinitions("FFI_DEBUG", NULL);
            SHU_ModuleAddSourceFiles("src/debug.c");
        }

        SHU_ModuleAddIncludeDirectories("../other/libffi/", "include/", "src/", targetDirectory, headersFromRoot);
    }
}

static void Shuild_OpenECS(void)
{
    char *includeDir = SHU_UtilFormat("%sinclude/", CONFIG.outputDir);
    char *ffiIncludeDir = SHU_UtilFormat("%sinclude/libffi/", CONFIG.outputDir);
    char *libraryDir = SHU_UtilFormat("%slib/", CONFIG.outputDir);
    char *binOutputDir = SHU_UtilFormat("%sbin/", CONFIG.outputDir);

    SHU_MODULE("OpenECS", NULL, SHUModuleType_Executable, binOutputDir)
    {
        SetBuildFlags(true);

        // OpenECS.h exports the plugin interface from the executable, where plugins import it from (DESIGN 17.6)
        SHU_CompilerAddDefinitions("OPENECS_CORE", NULL);

        if (CONFIG.windows)
        {
            // a plugin's DLL is linked with the executable's import library, as Windows finds a DLL's functions through it
            SHU_CompilerAddLinkerFlags(SHU_UtilFormat("'-Wl,--out-implib,%s%slib/libOpenECS.a'", SHU_UtilGetScriptDirectory(), CONFIG.outputDir));

            // only Debug builds open a console window; the others log to their log file, and to a console they are started from (DESIGN 14.4)
            if (CONFIG.type != BuildType_Debug)
            {
                SHU_CompilerAddLinkerFlags("-Wl,--subsystem,windows");
            }
        }
        else
        {
            // the executable exports only the plugin interface: the OPENECS_EXPORT functions, whose names start with ECS
            // it finds the shared libraries of SDL and SDL_ttf in its own folder
            SHU_CompilerAddFlags("-fvisibility=hidden");
            SHU_CompilerAddLinkerFlags("'-Wl,--export-dynamic-symbol=ECS*'", "'-Wl,-rpath,$ORIGIN'");

            // the sanitizers find their settings in src/base/Sanitizers.c by name, and its dlclose replaces the system's
            if (CONFIG.type == BuildType_Debug)
            {
                SHU_CompilerAddLinkerFlags("'-Wl,--export-dynamic-symbol=__*san_default_*'", "'-Wl,--export-dynamic-symbol=dlclose'");
            }
        }

        SHU_ModuleAddSourceFiles("src/");

        // ffi.h includes ffitarget.h beside it as <ffitarget.h>, which must not be a system copy of another libffi
        SHU_ModuleAddIncludeDirectories("include/", includeDir, ffiIncludeDir);

        SHU_ModuleAddLibraryDirectories(libraryDir);
        SHU_ModuleLinkLibraries("clay", "SDL3_ttf", "SDL3", "lua", "ffi", "stb");

        if (!CONFIG.windows)
        {
            SHU_ModuleLinkLibraries("m");
        }
    }

    // the shared libraries go next to the executable under the names it asks for, as files rather than links; CMake installs Windows' DLLs there already
    if (!CONFIG.windows)
    {
        char *library = SHU_UtilFormat("%slib/libSDL3.so.0", CONFIG.outputDir);
        SHU_UtilCopyFile(library, binOutputDir);
        library = SHU_UtilFormat("%slib/libSDL3_ttf.so.0", CONFIG.outputDir);
        SHU_UtilCopyFile(library, binOutputDir);
    }

    SHU_UtilCopyFile("include/OpenECS.h", includeDir);
    SHU_UtilCopyFile("include/ecs.lua", includeDir);
}

/// @brief Builds a repository checked out in a folder of OpenECS, std/ or examples/, with the shuild.c of its own (DESIGN 17.7).
/// Its shuild.c is compiled with shu and shuild from dependencies/ into shuild.ignore in its folder, because shuild finds every path from the folder of the program it runs in.
/// It runs with the build type and this build's output folder as seen from its folder.
static void Shuild_Repository(const char *folder)
{
    char *source = SHU_UtilFormat("%sshuild.c", folder);

    // the name of the folder, without its last slash
    int nameLength = (int)strlen(folder) - 1;

    if (SHU_UtilFileExists(source) != SHUFileType_Regular)
    {
        SHU_UtilFail(SHUM_LOG_NAME("%.*s") " is not checked out; clone OpenECS with " SHUM_LOG_COMMAND("--recursive") ".", nameLength, folder);
    }

    // the output folder is named from the repository's folder, which is one level deeper for each of its folders
    const char *output = "";

    for (const char *c = folder; *c != '\0'; c++)
    {
        if (*c == '/')
        {
            output = SHU_UtilFormat("%s../", output);
        }
    }

    output = SHU_UtilFormat("%s%s", output, CONFIG.outputDir);

    // shuild compiles it with shu and shuild from dependencies/, and runs it in its folder
    char *program = SHU_UtilFormat("%sshuild.ignore", folder);
    char *flags = SHU_UtilFormat("-I%sdependencies", SHU_UtilGetScriptDirectory());
    char *arguments = SHU_UtilFormat("-b %s -o %s -c %s", BuildType_String(CONFIG.type), output, CONFIG.compiler);

    SHU_UtilLog(SHUM_LOG_NAME("%.*s") " (build script): building", nameLength, folder);
    SHU_UtilBuildScript(source, program, flags, arguments);
    SHU_UtilLog(SHUM_LOG_NAME("%.*s") " (build script): built", nameLength, folder);
}

static void Shuild_Plugins(void)
{
    // the examples are built when the flags ask for them; a copy from an earlier build is removed otherwise
    if (!CONFIG.examples)
    {
        char *examples = SHU_UtilFormat("%sbin/examples/", CONFIG.outputDir);
        DeleteIfExists(examples);
    }

    // OpenECS-std builds the standard plugins into bin/plugins/ and their presets into bin/presets/; OpenECS-examples builds the examples into bin/examples/
    if (CONFIG.std)
    {
        Shuild_Repository("std/");
    }

    // the definitions come before the examples, so an example's C plugin includes the headers of the standard plugins, such as ui.h
    Shuild_Definitions();

    if (CONFIG.examples)
    {
        Shuild_Repository("examples/");
    }
}

static void Shuild_other(void)
{
    char *shuHeaderDir = SHU_UtilFormat("%sinclude/shu/", CONFIG.outputDir);
    SHU_UtilCopyFile("dependencies/shu/shu.h", shuHeaderDir);

    char *resourcesDir = SHU_UtilFormat("%sbin/resources/", CONFIG.outputDir);
    SHU_UtilCopyFile("resources/", resourcesDir);
}

/// @brief The built executable's path, to run with the shell. cmd.exe runs a program by a path with backslashes.
static char *ProgramPath(void)
{
    char *program = SHU_UtilFormat("%sbin/OpenECS", CONFIG.outputDir);

    for (char *c = program; SHUM_HOST_WINDOWS && *c != '\0'; c++)
    {
        *c = *c == '/' ? '\\' : *c;
    }

    return program;
}

/// @brief Writes a .luarc.json file that points LuaLS at OpenECS's ecs.lua and the definition files, so editors complete what Lua code uses.
/// @param ignore Files LuaLS leaves out of the workspace, in .gitignore's grammar, or "": the plugins' types.lua, whose classes the definition files hold already.
static void WriteLuaConfig(const char *folder, const char *include, const char *definitions, const char *ignore)
{
    char *path = SHU_UtilFormat("%s.luarc.json", folder);
    char *text = SHU_UtilFormat("{\n  \"workspace.library\": [\"%s\", \"%s\"]%s%s%s\n}\n", include, definitions, ignore[0] == '\0' ? "" : ",\n  \"workspace.ignoreDir\": [\"", ignore, ignore[0] == '\0' ? "" : "\"]");
    SHU_UtilWriteFile(path, csv(cs(text, strlen(text))));
}

/// @brief Deletes a file of the build's definitions/ that the new definitions, in the folder of data, do not have, as its plugin is gone.
static void DeleteOldDefinition(const char *path, SHUFileType type, void *data)
{
    (void)type;
    const char *name = path + strlen(path);

    while (name > path && name[-1] != '/' && name[-1] != SHUM_PATH_SEPARATOR)
    {
        name--;
    }

    if (SHU_UtilFileExists(SHU_UtilFormat("%s%s", (const char *)data, name)) == SHUFileType_Invalid)
    {
        SHU_UtilDeleteFile(path);
    }
}

// editors complete the functions of other plugins from their definition files, which the built program writes for every plugin it finds;
// the .luarc.json files, which git ignores, point LuaLS at them from OpenECS, std/ and examples/
static void Shuild_Definitions(void)
{
    char *definitions = SHU_UtilFormat("%sdefinitions/", CONFIG.outputDir);

    // a program built for another system cannot run here to write them
    if (!SHUM_PLATFORM_IS_HOST(SHU_CompilerGetTarget()))
    {
        SHU_UtilWarn("the definition files are not written, as OpenECS is built for another system.");
        return;
    }

    // a program that fails to write them does not stop the build
    char *newDefinitions = SHU_UtilFormat("%sdefinitions/", CONFIG.buildDir);
    DeleteIfExists(newDefinitions);
    SHU_UtilRunTry("%s --definitions %s > %sdefinitions.log 2>&1", ProgramPath(), newDefinitions, CONFIG.outputDir);

    if (SHU_UtilFileExists(newDefinitions) != SHUFileType_Directory)
    {
        SHU_UtilWarn("the definition files are not written; " SHUM_LOG_PATH("%sdefinitions.log") " says why.", CONFIG.outputDir);
        return;
    }

    // they are written fresh into the cache, then copied: a file that has not changed keeps its time, so the C plugins that include it do not compile again
    if (SHU_UtilFileExists(definitions) == SHUFileType_Directory)
    {
        SHU_UtilListDirectory(definitions, false, DeleteOldDefinition, newDefinitions);
    }

    SHU_UtilCopyFile(newDefinitions, definitions);

    char *up = SHU_UtilFormat("../%s", definitions);
    WriteLuaConfig("", "include", definitions, "std/plugins/*/types.lua");
    WriteLuaConfig("std/", "../include", up, "plugins/*/types.lua");
    WriteLuaConfig("examples/", "../include", up, "");
}

/// @brief How many tests ran, and how many failed.
typedef struct TestCount
{
    int run;
    int failed;
} TestCount;

/// @brief Runs a file of an example, if it is a test: a Lua file whose name starts with test (DESIGN 17.5). Prints pass or FAIL, and the output of a test that fails.
/// A test fails when the program fails, or when its output has a sanitizer's report.
static void RunTest(const char *path, SHUFileType type, void *data)
{
    const char *name = path + strlen(path);

    while (name > path && name[-1] != '/' && name[-1] != SHUM_PATH_SEPARATOR)
    {
        name--;
    }

    if (type != SHUFileType_Regular || !SHU_UtilMatch(name, "test*.lua"))
    {
        return;
    }

    TestCount *count = data;
    char *log = SHU_UtilFormat("%stest.log", CONFIG.outputDir);
    bool passed = SHU_UtilRunTry("%s --test %s > %s 2>&1", ProgramPath(), path, log) == 0;

    SHUSlice output = SHU_UtilReadFile(log);
    passed = passed && strstr(output.data, "Sanitizer") == NULL && strstr(output.data, "runtime error:") == NULL;

    count->run++;

    if (passed)
    {
        printf("pass %s\n", path);
    }
    else
    {
        count->failed++;
        printf("FAIL %s\n%s\n", path, (char *)output.data);
    }

    fflush(stdout);
    free(output.data);
}

/// @brief Runs the test of each example of the folder.
static void RunExampleTests(const char *path, SHUFileType type, void *data)
{
    if (type == SHUFileType_Directory)
    {
        SHU_UtilListDirectory(path, false, RunTest, data);
    }
}

// the examples are the tests: each test of bin/examples/ runs with the Debug build, on SDL's offscreen driver unless SDL_VIDEO_DRIVER names another (DESIGN 17.5)
static void RunTests(void)
{
    TestCount count = {0};
    char *examples = SHU_UtilFormat("%sbin/examples/", CONFIG.outputDir);
    SHU_UtilListDirectory(examples, false, RunExampleTests, &count);

    if (count.run == 0)
    {
        SHU_UtilFail("no test is found in " SHUM_LOG_PATH("%s") ".", examples);
    }

    if (count.failed > 0)
    {
        SHU_UtilFail("%d of %d tests failed.", count.failed, count.run);
    }

    SHU_UtilLog("all %d tests passed.", count.run);
}
