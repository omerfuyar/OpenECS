#define SHU_IMPLEMENTATION
#define SHUC_ENABLE_INCREMENTAL
#define SHUC_COMPILE_DATABASE // compile_commands.json, which clangd and check.sh's clang-tidy read
#include "dependencies/shuild/shuild.h"

#pragma region Setup

typedef enum BuildType
{
    BuildType_Debug,
    BuildType_Release,
    BuildType_RelWithDebInfo,
    BuildType_MinSizeRel,
} BuildType;

// the build types as the flag -b takes them, case aside, and as the build folders are named
static const SHUFlagChoice BUILD_TYPES[] = {
    {"Debug", "the default, with the static analyzer and the sanitizers"},
    {"Release", "an optimized build"},
    {"RelWithDebInfo", "a release with debug information"},
    {"MinSizeRel", "a small release"},
    {NULL, NULL},
};
#define BuildType_String(buildType) BUILD_TYPES[(buildType)].name

/// @brief What the build does and where it goes, in groups, as shuild keeps its own state.
static struct
{
    // what the command line chooses
    struct
    {
        int type; // a BuildType
        const char *compiler;
        bool noStd;        // do not build the standard plugins, from std/
        bool examples;     // also build the examples, from examples/
        SHUFlagList tests; // run the tests of the examples it names, or of every example, after the build
        bool help;         // print the help, and build nothing
    } FLAGS;

    // the build's folders, from the build type and the system the compiler compiles for
    struct
    {
        char *build;        // shuild's cache, such as .shu/Debug/
        char *dependencies; // the built dependencies, which only the build uses: their headers in include/ and their libraries in lib/, such as .shu/Debug/dependencies/
        char *cmake;        // the CMake builds, a folder for each compiler, as CMake keeps the compiler it configured with, such as .shu/Debug/cmake/gcc/
        char *output;       // what people use: the program in bin/, the plugin interface in include/, and on Windows OpenECS's import library in lib/
    } DIRS;

    bool windows; // the compiler compiles for Windows

    // the examples' tests that ran, and those that failed
    struct
    {
        int run;
        int failed;
    } TESTS;
} CONFIG = {.FLAGS = {.type = BuildType_Debug, .compiler = SHUM_HOST_WINDOWS || SHUM_HOST_MACOS ? "clang" : "gcc"}};

// the flags, which shuild reads and prints the help of
static const SHUFlag FLAGS[] = {
    SHU_FLAG_CHOICE("-b", "--build", "TYPE", &CONFIG.FLAGS.type, BUILD_TYPES, "Build type:"),
    SHU_FLAG_SWITCH("-n", "--no-std", &CONFIG.FLAGS.noStd, "Do not build the standard plugins of std/"),
    SHU_FLAG_SWITCH("-e", "--examples", &CONFIG.FLAGS.examples, "Also build the examples of examples/, into bin/examples/"),
    SHU_FLAG_LIST("-t", "--test", "EXAMPLE", &CONFIG.FLAGS.tests,
                  "Build in Debug with the examples, then run the tests of the examples named, such as c12_sketch,\n"
                  "or of every example. Prints pass or FAIL for each, and fails when a test fails or its output has a sanitizer's report"),
    SHU_FLAG_STRING("-c", "--compiler", "CC", &CONFIG.FLAGS.compiler,
                    "The compiler: gcc (default for linux), clang, or on Windows LLVM-MinGW's clang (default for windows).\n"
                    "A cross compiler, such as x86_64-w64-mingw32-gcc, builds for its system into build/TYPE-SYSTEM/"),
    SHU_FLAG_SWITCH("-h", "--help", &CONFIG.FLAGS.help, "Show this help"),
};

#pragma endregion Setup

static void CheckArguments(void);
static void Configure(void);
static void SetBuildFlags(bool ownCode);
static bool IsBuilt(const char *library, bool shared);

static void Shuild_SDL(void);
static void Shuild_SDL_ttf(void);
static void Shuild_luajit(void);
static void CopyLuaJitModules(void);
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
    SHU_FlagsRead(FLAGS, argc, argv);
    SHU_CompilerConfigureTry(CONFIG.FLAGS.compiler);

    // a changed shuild.c is compiled again and run with the same arguments, so a bad argument is reported after it, by the new program
    SHU_UtilAutomate(argc, argv);

    if (CONFIG.FLAGS.help)
    {
        SHU_FlagsPrintHelp(FLAGS);
        return 0;
    }

    SHU_FlagsCheck(FLAGS);
    CheckArguments();

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

    if (!IsBuilt("luajit", false))
    {
        Shuild_luajit();
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

    if (CONFIG.FLAGS.tests.given)
    {
        RunTests();
    }

    return 0;
}

/// @brief Checks what the flags cannot check alone: the examples that -t names, and its build type.
static void CheckArguments(void)
{
    for (int i = 0; i < CONFIG.FLAGS.tests.count; i++)
    {
        if (SHU_UtilFileExists(SHU_UtilFormat("examples/%s/", CONFIG.FLAGS.tests.values[i])) != SHUFileType_Directory)
        {
            SHU_UtilFail("No example is named: " SHUM_LOG_VALUE("%s"), CONFIG.FLAGS.tests.values[i]);
        }
    }

    // only Debug builds run tests, and the tests are the examples' (DESIGN 17.5)
    if (CONFIG.FLAGS.tests.given)
    {
        if (CONFIG.FLAGS.type != BuildType_Debug)
        {
            SHU_UtilFail("The tests run with a Debug build, not " SHUM_LOG_VALUE("%s"), BuildType_String(CONFIG.FLAGS.type));
        }

        CONFIG.FLAGS.examples = true;
    }
}

/// @brief Names the build's folders from the configured compiler, and configures shuild's cache.
static void Configure(void)
{
    // a build for another system goes to folders of its own, such as build/Debug-Windows/
    SHUPlatform target = SHU_CompilerGetTarget();
    const char *system = SHUM_PLATFORM_IS_HOST(target) ? "" : SHU_UtilFormat("-%s", SHUPlatform_GetString(target));
    CONFIG.windows = target == SHUPlatform_Windows;
    CONFIG.DIRS.build = SHU_UtilFormat(".shu/%s%s/", BuildType_String(CONFIG.FLAGS.type), system);
    CONFIG.DIRS.output = SHU_UtilFormat("build/%s%s/", BuildType_String(CONFIG.FLAGS.type), system);
    CONFIG.DIRS.dependencies = SHU_UtilFormat("%sdependencies/", CONFIG.DIRS.build);

    // a program built for another system cannot run here
    if (CONFIG.FLAGS.tests.given && !SHUM_PLATFORM_IS_HOST(target))
    {
        SHU_UtilFail("the tests cannot run here, as OpenECS is built for " SHUM_LOG_VALUE("%s") ".", SHUPlatform_GetString(target));
    }

    SHU_CacheConfigure(CONFIG.DIRS.build);

    // a compiler named by its path, such as /usr/bin/clang, has a folder of its name
    const char *compilerName = CONFIG.FLAGS.compiler + strlen(CONFIG.FLAGS.compiler);

    while (compilerName > CONFIG.FLAGS.compiler && compilerName[-1] != '/' && compilerName[-1] != '\\')
    {
        compilerName--;
    }

    CONFIG.DIRS.cmake = SHU_UtilFormat("%scmake/%s/", CONFIG.DIRS.build, compilerName);

    SHU_UtilLog("building " SHUM_LOG_VALUE("%s") " with " SHUM_LOG_VALUE("%s") " for " SHUM_LOG_VALUE("%s") "; standard plugins: " SHUM_LOG_VALUE("%s") ", examples: " SHUM_LOG_VALUE("%s"),
                BuildType_String(CONFIG.FLAGS.type), CONFIG.FLAGS.compiler, SHUPlatform_GetString(target), !CONFIG.FLAGS.noStd ? "yes" : "no", CONFIG.FLAGS.examples ? "yes" : "no");
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

    switch ((BuildType)CONFIG.FLAGS.type)
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
    return SHU_UtilFileExists(SHU_UtilFormat("%slib/%s", CONFIG.DIRS.dependencies, file)) == SHUFileType_Regular;
}

/// @brief Builds a CMake project of dependencies/ and installs it into the dependencies' folder. The output of each step goes to a log in the cache folder,
/// such as .shu/Debug/cmake/gcc/SDL3-build.log, and the end of the log shows when the step fails.
static void Shuild_CMake(const char *name, const char *source, const char *options)
{
    SHU_UtilLog(SHUM_LOG_NAME("%s") " (" SHUM_COLOR_BLUE("CMake") "): building", name);

    const char *root = SHU_UtilGetScriptDirectory();

    char *sourceDir = SHU_UtilFormat("%sdependencies/%s/", root, source);
    char *buildDir = SHU_UtilFormat("%s%s%s/", root, CONFIG.DIRS.cmake, name);
    char *outputPrefixDir = SHU_UtilFormat("%s%s", root, CONFIG.DIRS.dependencies);

    // CMake compiles with the same compiler, and a cross compiler builds for its system
    char *compilers = SHU_UtilFormat("-DCMAKE_C_COMPILER=\"%s\" -DCMAKE_CXX_COMPILER=\"%s\"", CONFIG.FLAGS.compiler, SHU_CompilerGetTool("g++", "clang++"));

    if (!SHUM_PLATFORM_IS_HOST(SHU_CompilerGetTarget()))
    {
        compilers = SHU_UtilFormat("%s -DCMAKE_SYSTEM_NAME=%s -DCMAKE_RC_COMPILER=\"%s\"", compilers, SHUPlatform_GetString(SHU_CompilerGetTarget()), SHU_CompilerGetTool("windres", "windres"));
    }

    char *log = SHU_UtilFormat("%s%s-configure.log", CONFIG.DIRS.cmake, name);
    SHU_UtilRunLogged(log,
                      "cmake -S \"%s\" -B \"%s\" -G Ninja -DCMAKE_BUILD_TYPE=%s %s "
                      "-DCMAKE_INSTALL_PREFIX=\"%s\" -DCMAKE_PREFIX_PATH=\"%s\" "
                      "-DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_POSITION_INDEPENDENT_CODE=ON %s",
                      sourceDir, buildDir, BuildType_String(CONFIG.FLAGS.type), compilers,
                      outputPrefixDir, outputPrefixDir, options);

    log = SHU_UtilFormat("%s%s-build.log", CONFIG.DIRS.cmake, name);
    SHU_UtilRunLogged(log, "cmake --build \"%s\" --parallel", buildDir);

    log = SHU_UtilFormat("%s%s-install.log", CONFIG.DIRS.cmake, name);
    SHU_UtilRunLogged(log, "cmake --install \"%s\"", buildDir);

    SHU_UtilLog(SHUM_LOG_NAME("%s") " (" SHUM_COLOR_BLUE("CMake") "): built", name);
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

/// @brief Tells whether the target's lj_arch.h defines a macro with a value, as LuaJIT's Makefile finds it in TARGET_TESTARCH.
static bool LuaJitArchHas(const char *defines, const char *name, const char *value)
{
    // the line ends with \r\n where the compiler writes Windows' line ends
    const char *line = SHU_UtilFormat("#define %s %s", name, value);
    usz length = strlen(line);

    for (const char *found = strstr(defines, line); found != NULL; found = strstr(found + 1, line))
    {
        if (found[length] == '\n' || found[length] == '\r')
        {
            return true;
        }
    }

    return false;
}

/// @brief Writes LuaJIT's generated files into its folder of the build, as LuaJIT's src/Makefile does, and gives the name of its architecture,
/// such as x64. The host tools minilua and buildvm are built with the host's compiler, which is the configured one unless it builds for another system.
static const char *MakeLuaJitFiles(const char *generated)
{
    const char *root = SHU_UtilGetScriptDirectory();
    char *source = SHU_UtilFormat("%sdependencies/luajit/src/", root);
    char *out = SHU_UtilFormat("%s%s", root, generated);
    char *log = SHU_UtilFormat("%sluajit.log", generated);
    bool cross = !SHUM_PLATFORM_IS_HOST(SHU_CompilerGetTarget());
    const char *hostCompiler = cross ? "cc" : CONFIG.FLAGS.compiler;
    const char *exe = SHUM_HOST_WINDOWS ? ".exe" : "";
    // cmd.exe drops the first and last quotes of a command that starts with one; call keeps them
    const char *run = SHUM_HOST_WINDOWS ? "call " : "";
    SHU_UtilCreateDirectory(SHU_UtilFormat("%sjit/", generated));

    // the target's architecture, from the macros that its compiler defines in lj_arch.h
    char *archFile = SHU_UtilFormat("%sarch.txt", generated);
    SHU_UtilRunLogged(log, "%s\"%s\" -E -dM -x c \"%slj_arch.h\" -o \"%s%s\"", run, CONFIG.FLAGS.compiler, source, root, archFile);
    SHUSlice archText = SHU_UtilReadFile(archFile);
    const char *defines = archText.data;
    const char *arch = LuaJitArchHas(defines, "LJ_TARGET_X64", "1") ? "x64" : LuaJitArchHas(defines, "LJ_TARGET_ARM64", "1") ? "arm64"
                                                                                                                             : NULL;

    if (arch == NULL)
    {
        SHU_UtilFail(SHUM_LOG_NAME("LuaJIT") " is built for x64 and arm64 only.");
    }

    // DASM_AFLAGS of the Makefile
    const char *const flags[][3] = {
        {"LJ_LE", "1", "-D ENDIAN_LE"},
        {"LJ_ARCH_BITS", "64", "-D P64"},
        {"LJ_HASJIT", "1", "-D JIT"},
        {"LJ_HASFFI", "1", "-D FFI"},
        {"LJ_DUALNUM", "1", "-D DUALNUM"},
        {"LJ_ARCH_HASFPU", "1", "-D FPU"},
        {"LJ_NO_UNWIND", "1", "-D NO_UNWIND"},
        {"LJ_ABI_PAUTH", "1", "-D PAUTH"},
        {"LJ_ABI_BRANCH_TRACK", "1", "-D BRANCH_TRACK"},
        {"LJ_ABI_SHADOW_STACK", "1", "-D SHADOW_STACK"},
    };
    char *dasmFlags = LuaJitArchHas(defines, "LJ_LE", "1") ? "" : "-D ENDIAN_BE";

    for (usz i = 0; i < sizeof(flags) / sizeof(*flags); i++)
    {
        dasmFlags = LuaJitArchHas(defines, flags[i][0], flags[i][1]) ? SHU_UtilFormat("%s %s", dasmFlags, flags[i][2]) : dasmFlags;
    }

    const char *version = strstr(defines, "#define LJ_ARCH_VERSION ");
    char *archVersion = version != NULL ? SHU_UtilFormat("%ld", strtol(version + strlen("#define LJ_ARCH_VERSION "), NULL, 10)) : "";
    dasmFlags = SHU_UtilFormat("%s%s -D VER=%s%s", dasmFlags, LuaJitArchHas(defines, "LJ_ABI_SOFTFP", "1") ? "" : " -D HFABI", archVersion, CONFIG.windows ? " -D WIN" : "");
    free(archText.data);

    // the host tools build for the target: its architecture, its system when that is not the host's, and Lua 5.2's functions, as buildvm
    // writes the tables of the library functions
    char *hostFlags = SHU_UtilFormat("-O1 -DLUAJIT_ENABLE_LUA52COMPAT -DLUAJIT_TARGET=LUAJIT_ARCH_%s -I\"%s\" -I\"%s\"", arch, out, source);

    if (cross)
    {
        hostFlags = SHU_UtilFormat("%s %s", hostFlags, CONFIG.windows ? "-malign-double -DLUAJIT_OS=LUAJIT_OS_WINDOWS" : "-DLUAJIT_OS=LUAJIT_OS_LINUX");
    }

    char *minilua = SHU_UtilFormat("%sminilua%s", out, exe);
    SHU_UtilRunLogged(log, "%s\"%s\" %s -o \"%s\" \"%shost/minilua.c\"%s", run, hostCompiler, hostFlags, minilua, source, SHUM_HOST_WINDOWS ? "" : " -lm");
    SHU_UtilRunLogged(log, "%s\"%s\" \"%sdependencies/luajit/dynasm/dynasm.lua\" %s -o \"%sbuildvm_arch.h\" \"%svm_%s.dasc\"", run, minilua, root, dasmFlags, out, source, arch);

    // luajit.h gets the release number: the time of the submodule's commit, or .relver in a source archive
    char *relver = SHU_UtilFormat("%sluajit_relver.txt", out);

    if (SHU_UtilRunTry("git -C \"%sdependencies/luajit\" show -s --format=%%ct > \"%s\"", root, relver) != 0)
    {
        SHU_UtilCopyFile("dependencies/luajit/.relver", generated);
        relver = SHU_UtilFormat("%s.relver", out);
    }

    SHU_UtilRunLogged(log, "%s\"%s\" \"%shost/genversion.lua\" \"%sluajit_rolling.h\" \"%s\" \"%sluajit.h\"", run, minilua, source, source, relver, out);

    // buildvm makes the VM's assembly and the tables of the library functions
    char *buildvm = SHU_UtilFormat("%sbuildvm%s", out, exe);
    SHU_UtilRunLogged(log, "%s\"%s\" %s -o \"%s\" \"%shost/buildvm.c\" \"%shost/buildvm_asm.c\" \"%shost/buildvm_peobj.c\" \"%shost/buildvm_lib.c\" \"%shost/buildvm_fold.c\"", run,
                      hostCompiler, hostFlags, buildvm, source, source, source, source, source);

    const char *const libraries[] = {"lib_base", "lib_math", "lib_bit", "lib_string", "lib_table", "lib_io", "lib_os", "lib_package", "lib_debug", "lib_jit", "lib_ffi", "lib_buffer"};
    char *libraryFiles = "";

    for (usz i = 0; i < sizeof(libraries) / sizeof(*libraries); i++)
    {
        libraryFiles = SHU_UtilFormat("%s \"%s%s.c\"", libraryFiles, source, libraries[i]);
    }

    SHU_UtilRunLogged(log, "%s\"%s\" -m %s -o \"%s%s\"", run, buildvm, CONFIG.windows ? "peobj" : "elfasm", out, CONFIG.windows ? "lj_vm.o" : "lj_vm.S");
    const char *const tables[] = {"bcdef", "ffdef", "libdef", "recdef"};

    for (usz i = 0; i < sizeof(tables) / sizeof(*tables); i++)
    {
        SHU_UtilRunLogged(log, "%s\"%s\" -m %s -o \"%slj_%s.h\"%s", run, buildvm, tables[i], out, tables[i], libraryFiles);
    }

    SHU_UtilRunLogged(log, "%s\"%s\" -m folddef -o \"%slj_folddef.h\" \"%slj_opt_fold.c\"", run, buildvm, out, source);
    SHU_UtilRunLogged(log, "%s\"%s\" -m vmdef -o \"%sjit/vmdef.lua\"%s", run, buildvm, out, libraryFiles);
    return arch;
}

// LuaJIT is built as its src/Makefile builds it, with every generated file in the build's folder, never in dependencies/
static void Shuild_luajit(void)
{
    char *generated = SHU_UtilFormat("%sgenerated/luajit/", CONFIG.DIRS.build);
    MakeLuaJitFiles(generated);

    // the core includes Lua's headers from lua/, and lua-compat-5.3's beside them, as its header for the 5.3 forms includes them (DESIGN 17.4)
    char *headerOutputDir = SHU_UtilFormat("%sinclude/lua/", CONFIG.DIRS.dependencies);
    const char *const headers[] = {"lua.h", "lauxlib.h", "lualib.h", "luaconf.h", "lua.hpp"};

    for (usz i = 0; i < sizeof(headers) / sizeof(*headers); i++)
    {
        SHU_UtilCopyFile(SHU_UtilFormat("dependencies/luajit/src/%s", headers[i]), headerOutputDir);
    }

    SHU_UtilCopyFile(SHU_UtilFormat("%sluajit.h", generated), headerOutputDir);
    SHU_UtilCopyFile("dependencies/lua-compat-5.3/c-api/compat-5.3.h", headerOutputDir);

    char *generatedFromRoot = SHU_UtilFormat("../../../%s", generated);
    char *binOutputDir = SHU_UtilFormat("%slib/", CONFIG.DIRS.dependencies);

    SHU_Module("luajit", "dependencies/luajit/src", SHUModuleType_LibraryStatic, binOutputDir)
    {
        SetBuildFlags(false);
        // LuaJIT's C is older than C23, so it compiles as its Makefile compiles it, with the compiler's own standard
        SHU_CompilerAddFlags("-std=gnu11", "-U_FORTIFY_SOURCE");
        SHU_CompilerAddDefinitions("LUAJIT_ENABLE_LUA52COMPAT", NULL, "_FILE_OFFSET_BITS", "64", "_LARGEFILE_SOURCE", NULL);

        // errors unwind with the system's unwinder, through the C frames between a protected call and the error, which gcc and clang give
        // unwind tables on x64; Windows always unwinds that way
        if (!CONFIG.windows)
        {
            SHU_CompilerAddFlags("-funwind-tables", "-fno-stack-protector");
            SHU_CompilerAddDefinitions("LUAJIT_UNWIND_EXTERNAL", NULL);
        }

        // Debug builds check the C API's use and LuaJIT's own invariants
        if (CONFIG.FLAGS.type == BuildType_Debug)
        {
            SHU_CompilerAddDefinitions("LUA_USE_APICHECK", NULL, "LUA_USE_ASSERT", NULL);
        }

        SHU_ModuleAddSourceFiles("ljamalg.c");

        // lua-compat-5.3's functions for the C API's 5.3 forms, and its utf8 library, under names of their own beside Lua's
        SHU_CompilerAddDefinitions("COMPAT53_PREFIX", "compat53");
        SHU_ModuleAddSourceFiles("../../lua-compat-5.3/c-api/compat-5.3.c", "../../lua-compat-5.3/lutf8lib.c");

        // on Windows, buildvm writes the VM as an object file with its unwind data, which goes into the library below
        if (!CONFIG.windows)
        {
            SHU_ModuleAddSourceFiles(SHU_UtilFormat("%slj_vm.S", generatedFromRoot));
        }

        SHU_ModuleAddIncludeDirectories(generatedFromRoot, "./", "../../lua-compat-5.3/");
    }

    if (CONFIG.windows)
    {
        SHU_UtilRun("%s\"%s\" rs \"%s%s\" \"%slj_vm.o\"", SHUM_HOST_WINDOWS ? "call " : "", SHU_CompilerGetTool("ar", "llvm-ar"), binOutputDir, SHU_UtilLibraryFile("luajit", SHUModuleType_LibraryStatic), generated);
    }
}

// the Lua files of LuaJIT's jit module go next to the program as jit/, so require("jit.p"), jit.v and jit.dump work for whoever studies traces
static void CopyLuaJitModules(void)
{
    char *jitDir = SHU_UtilFormat("%sbin/jit/", CONFIG.DIRS.output);
    SHU_UtilCopyFile("dependencies/luajit/src/jit/", jitDir);
    SHU_UtilCopyFile(SHU_UtilFormat("%sgenerated/luajit/jit/vmdef.lua", CONFIG.DIRS.build), jitDir);
}

static void Shuild_clay(void)
{
    char *clayIncludeDir = SHU_UtilFormat("../../%sinclude/", CONFIG.DIRS.dependencies);
    char *binOutputDir = SHU_UtilFormat("%slib/", CONFIG.DIRS.dependencies);

    SHU_Module("clay", "dependencies/clay", SHUModuleType_LibraryStatic, binOutputDir)
    {
        SetBuildFlags(false);
        SHU_ModuleAddSourceFiles("../other/clay/clay.c");
        SHU_ModuleAddIncludeDirectories(clayIncludeDir);
    }

    // clay.c includes the headers from the submodule, so they are copied for the core after it compiles
    char *headerOutputDir = SHU_UtilFormat("%sinclude/clay/", CONFIG.DIRS.dependencies);
    SHU_UtilCopyFile("dependencies/clay/clay.h", headerOutputDir);
    SHU_UtilCopyFile("dependencies/other/clay/claySDL3.h", headerOutputDir);
}

static void Shuild_stb(void)
{
    // stb.c includes the copied glue header, so the headers are copied first
    char *headerOutputDir = SHU_UtilFormat("%sinclude/stb/", CONFIG.DIRS.dependencies);
    SHU_UtilCopyFile("dependencies/stb/stb_ds.h", headerOutputDir);
    SHU_UtilCopyFile("dependencies/other/stb/stbSDL3.h", headerOutputDir);

    char *includeDir = SHU_UtilFormat("../../../%sinclude/", CONFIG.DIRS.dependencies);
    char *binOutputDir = SHU_UtilFormat("%slib/", CONFIG.DIRS.dependencies);

    SHU_Module("stb", "dependencies/other/stb", SHUModuleType_LibraryStatic, binOutputDir)
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
    char *headers = SHU_UtilFormat("%sinclude/libffi/", CONFIG.DIRS.dependencies);
    char *targetHeader = SHU_UtilFormat("dependencies/libffi/%sffitarget.h", targetDirectory);
    SHU_UtilCopyFile(targetHeader, headers);
    char *ffiHeader = SHU_UtilFormat("%sffi.h", headers);
    MakeFfiHeader(target, ffiHeader);

    char *headersFromRoot = SHU_UtilFormat("../../%sinclude/libffi/", CONFIG.DIRS.dependencies);
    char *binOutputDir = SHU_UtilFormat("%slib/", CONFIG.DIRS.dependencies);

    SHU_Module("ffi", "dependencies/libffi", SHUModuleType_LibraryStatic, binOutputDir)
    {
        SetBuildFlags(false);
        SHU_CompilerAddDefinitions("HAVE_CONFIG_H", NULL);

        SHU_ModuleAddSourceFiles("src/prep_cif.c", "src/types.c", "src/raw_api.c", "src/java_raw_api.c", "src/closures.c", "src/tramp.c");

        for (usz i = 0; i < targetSourceCount; i++)
        {
            SHU_ModuleAddSourceFiles(targetSources[i]);
        }

        // libffi checks its arguments in Debug builds
        if (CONFIG.FLAGS.type == BuildType_Debug)
        {
            SHU_CompilerAddDefinitions("FFI_DEBUG", NULL);
            SHU_ModuleAddSourceFiles("src/debug.c");
        }

        SHU_ModuleAddIncludeDirectories("../other/libffi/", "include/", "src/", targetDirectory, headersFromRoot);
    }
}

/// @brief Writes the source file that puts the plugin interface into the executable: the bytes of OpenECS.h, shu.h and ecs.lua, which
/// OpenECS --definitions writes for plugin authors, so they always match the program (DESIGN 10.10). A file whose bytes did not change keeps its time,
/// so it compiles again only after a change.
/// @return The source file's path.
static char *WriteInterfaceSource(void)
{
    const char *const files[][2] = {{"OpenECS.h", "include/OpenECS.h"}, {"shu.h", "dependencies/shu/shu.h"}, {"ecs.lua", "include/ecs.lua"}};
    usz count = sizeof(files) / sizeof(*files);
    SHUSlice contents[sizeof(files) / sizeof(*files)];
    usz size = 1024;

    for (usz i = 0; i < count; i++)
    {
        contents[i] = SHU_UtilReadFile(files[i][1]);
        // "0xNN," and a space or a line break with its indent
        size += contents[i].size * 6 + contents[i].size / 16 * 5 + 256;
    }

    // each byte is written as 0xNN, with 16 on a line
    char *source = malloc(size);
    usz length = 0;
    if (source == NULL)
    {
        SHU_UtilFail("the plugin interface's source file cannot be written.");
    }

    length += (usz)snprintf(source + length, size - length, "// OpenECS's build writes this file: the files of the plugin interface, which OpenECS --definitions writes (DESIGN 10.10)\n\n#include \"runtime/Services.h\"\n");

    for (usz i = 0; i < count; i++)
    {
        length += (usz)snprintf(source + length, size - length, "\nstatic const u8 FILE_%zu[] = {", i);

        for (usz j = 0; j < contents[i].size; j++)
        {
            length += (usz)snprintf(source + length, size - length, "%s0x%02x,", j % 16 == 0 ? "\n    " : " ", ((const unsigned char *)contents[i].data)[j]);
        }

        length += (usz)snprintf(source + length, size - length, "\n};\n");
        free(contents[i].data);
    }

    length += (usz)snprintf(source + length, size - length, "\nconst ECSIInterfaceFile ECSI_INTERFACE_FILES[] = {\n");

    for (usz i = 0; i < count; i++)
    {
        length += (usz)snprintf(source + length, size - length, "    {\"%s\", FILE_%zu, sizeof(FILE_%zu)},\n", files[i][0], i, i);
    }

    length += (usz)snprintf(source + length, size - length, "};\n\nconst usz ECSI_INTERFACE_FILE_COUNT = %zu;\n", count);

    char *folder = SHU_UtilFormat("%sinterface/", CONFIG.DIRS.build);
    char *path = SHU_UtilFormat("%sInterface.c", folder);
    SHU_UtilCreateDirectory(folder);
    SHU_UtilWriteFile(path, csv(cs(source, length)));
    free(source);
    return path;
}

static void Shuild_OpenECS(void)
{
    char *dependenciesIncludeDir = SHU_UtilFormat("%s%sinclude/", SHU_UtilGetScriptDirectory(), CONFIG.DIRS.dependencies);
    char *libraryDir = SHU_UtilFormat("%slib/", CONFIG.DIRS.dependencies);
    char *binOutputDir = SHU_UtilFormat("%sbin/", CONFIG.DIRS.output);
    char *interfaceSource = WriteInterfaceSource();

    // a plugin's DLL on Windows links the executable's import library, in the build's lib/
    if (CONFIG.windows)
    {
        SHU_UtilCreateDirectory(SHU_UtilFormat("%slib/", CONFIG.DIRS.output));
    }

    SHU_Module("OpenECS", NULL, SHUModuleType_Executable, binOutputDir)
    {
        SetBuildFlags(true);

        // OpenECS.h exports the plugin interface from the executable, where plugins import it from (DESIGN 17.6)
        SHU_CompilerAddDefinitions("OPENECS_CORE", NULL);

        if (CONFIG.windows)
        {
            // a plugin's DLL is linked with the executable's import library, as Windows finds a DLL's functions through it
            SHU_CompilerAddLinkerFlags(SHU_UtilFormat("'-Wl,--out-implib,%s%slib/libOpenECS.a'", SHU_UtilGetScriptDirectory(), CONFIG.DIRS.output));

            // only Debug builds open a console window; the others log to their log file, and to a console they are started from (DESIGN 14.4)
            if (CONFIG.FLAGS.type != BuildType_Debug)
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
            if (CONFIG.FLAGS.type == BuildType_Debug)
            {
                SHU_CompilerAddLinkerFlags("'-Wl,--export-dynamic-symbol=__*san_default_*'", "'-Wl,--export-dynamic-symbol=dlclose'");
            }
        }

        SHU_ModuleAddSourceFiles("src/", interfaceSource);
        SHU_ModuleAddIncludeDirectories("include/");

        // the dependencies' headers are system headers, so their warnings and macros stay out of our checks: shu.h, which OpenECS.h includes, and
        // the built dependencies'; ffi.h includes ffitarget.h beside it as <ffitarget.h>, which must not be a system copy of another libffi
        SHU_CompilerAddFlags(SHU_UtilFormat("-isystem %sdependencies/shu", SHU_UtilGetScriptDirectory()), SHU_UtilFormat("-isystem %s", dependenciesIncludeDir),
                             SHU_UtilFormat("-isystem %slibffi", dependenciesIncludeDir), SHU_UtilFormat("-isystem %slua", dependenciesIncludeDir));

        // lua-compat-5.3's header gives the C API's 5.3 forms, whose functions LuaJIT's library holds under its prefix
        SHU_CompilerAddDefinitions("COMPAT53_PREFIX", "compat53");

        SHU_ModuleAddLibraryDirectories(libraryDir);
        SHU_ModuleLinkLibraries("clay", "SDL3_ttf", "SDL3", "luajit", "ffi", "stb");

        if (!CONFIG.windows)
        {
            SHU_ModuleLinkLibraries("m");
        }
        else
        {
            // the Start menu's shortcuts and "Open with" of --install, and the app id of the taskbar (DESIGN 13.9)
            SHU_ModuleLinkLibraries("ole32", "uuid", "shell32", "advapi32");
        }
    }

    // the shared libraries go next to the executable under the names it asks for, as files rather than links
    const char *libraries[] = {"lib/libSDL3.so.0", "lib/libSDL3_ttf.so.0"};
    const char *windowsLibraries[] = {"bin/SDL3.dll", "bin/SDL3_ttf.dll"};

    for (usz i = 0; i < 2; i++)
    {
        SHU_UtilCopyFile(SHU_UtilFormat("%s%s", CONFIG.DIRS.dependencies, CONFIG.windows ? windowsLibraries[i] : libraries[i]), binOutputDir);
    }

    // the plugin interface goes to include/, where the standard plugins and the examples are built against it; OpenECS --definitions writes the same
    // files with the plugins' definitions, and a build for another system cannot run it
    char *includeDir = SHU_UtilFormat("%sinclude/", CONFIG.DIRS.output);
    SHU_UtilCopyFile("include/OpenECS.h", includeDir);
    SHU_UtilCopyFile("include/ecs.lua", includeDir);
    SHU_UtilCopyFile("dependencies/shu/shu.h", includeDir);
}

/// @brief A path of this folder as seen from a folder in it, such as ../build/Debug/ from std/: one level up for each of its folders.
static char *PathFromFolder(const char *folder, const char *path)
{
    const char *up = "";

    for (const char *c = folder; *c != '\0'; c++)
    {
        if (*c == '/')
        {
            up = SHU_UtilFormat("%s../", up);
        }
    }

    return SHU_UtilFormat("%s%s", up, path);
}

/// @brief Builds a repository checked out in a folder of OpenECS, std/ or examples/, with the shuild.c of its own (DESIGN 17.7).
/// Its shuild.c is compiled with shu and shuild from dependencies/ into shuild.ignore in its folder, because shuild finds every path from the folder of the program it runs in.
/// It runs with the build type and this build's output folder as seen from its folder, and the arguments given.
static void Shuild_Repository(const char *folder, const char *moreArguments)
{
    char *source = SHU_UtilFormat("%sshuild.c", folder);

    // the name of the folder, without its last slash
    int nameLength = (int)strlen(folder) - 1;

    if (SHU_UtilFileExists(source) != SHUFileType_Regular)
    {
        SHU_UtilFail(SHUM_LOG_NAME("%.*s") " is not checked out; clone OpenECS with " SHUM_LOG_COMMAND("--recursive") ".", nameLength, folder);
    }

    // shuild compiles it with shu and shuild from dependencies/, and runs it in its folder
    char *program = SHU_UtilFormat("%sshuild.ignore", folder);
    char *flags = SHU_UtilFormat("-I%sdependencies", SHU_UtilGetScriptDirectory());
    char *arguments = SHU_UtilFormat("-b %s -o %s -c %s %s", BuildType_String(CONFIG.FLAGS.type), PathFromFolder(folder, CONFIG.DIRS.output), CONFIG.FLAGS.compiler, moreArguments);

    // shuild logs its build, with a line before it that sets it apart from the build here
    SHU_Shuild(source, program, flags, arguments);
}

static void Shuild_Plugins(void)
{
    // the examples are built when the flags ask for them; a copy from an earlier build is removed otherwise
    if (!CONFIG.FLAGS.examples)
    {
        char *examples = SHU_UtilFormat("%sbin/examples/", CONFIG.DIRS.output);
        SHU_UtilDeleteFile(examples);
    }

    // OpenECS-std builds the standard plugins into bin/plugins/ and their presets into bin/presets/, with the built dependencies that they reuse;
    // OpenECS-examples builds the examples into bin/examples/, with the plugin interface alone, as any plugin's author does
    if (!CONFIG.FLAGS.noStd)
    {
        Shuild_Repository("std/", SHU_UtilFormat("-d %s", PathFromFolder("std/", CONFIG.DIRS.dependencies)));
    }

    // the definitions come before the examples, so an example's C plugin includes the headers of the standard plugins, such as ui.h
    Shuild_Definitions();

    if (CONFIG.FLAGS.examples)
    {
        Shuild_Repository("examples/", "");
    }
}

static void Shuild_other(void)
{
    char *resourcesDir = SHU_UtilFormat("%sbin/resources/", CONFIG.DIRS.output);
    SHU_UtilCopyFile("resources/", resourcesDir);
    CopyLuaJitModules();
}

/// @brief Writes a .luarc.json file that points LuaLS at the build's include/, with ecs.lua and the definition files, so editors complete what Lua code uses,
/// and reads it as LuaJIT with its extensions, which plugins run on (DESIGN 9.6).
/// @param ignore Files LuaLS leaves out of the workspace, in .gitignore's grammar: the plugins' types.lua, whose classes the definition files hold already,
/// and OpenECS's own ecs.lua, as the build's copy is the one in the library.
static void WriteLuaConfig(const char *folder, const char *include, const char *ignore)
{
    char *path = SHU_UtilFormat("%s.luarc.json", folder);
    char *text = SHU_UtilFormat("{\n  \"runtime.version\": \"LuaJIT\",\n  \"runtime.enableLuaJITExtensions\": true,\n  \"workspace.library\": [\"%s\"]%s%s%s\n}\n", include, ignore[0] == '\0' ? "" : ",\n  \"workspace.ignoreDir\": [", ignore, ignore[0] == '\0' ? "" : "]");
    SHU_UtilWriteFile(path, csv(cs(text, strlen(text))));
}

/// @brief Deletes a file of the build's include/ that the new definitions, in the folder of data, do not have, as its plugin is gone.
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

// the built program writes the plugin interface and the definition files of every plugin it finds, which go to include/ (DESIGN 10.10);
// the .luarc.json files, which git ignores, point LuaLS at them from OpenECS, std/ and examples/
static void Shuild_Definitions(void)
{
    char *definitions = SHU_UtilFormat("%sinclude/", CONFIG.DIRS.output);

    // a program built for another system cannot run here to write them
    if (!SHUM_PLATFORM_IS_HOST(SHU_CompilerGetTarget()))
    {
        SHU_UtilWarn("the definition files are not written, as OpenECS is built for another system.");
        return;
    }

    // a program that fails to write them does not stop the build
    char *newDefinitions = SHU_UtilFormat("%sdefinitions/", CONFIG.DIRS.build);
    SHU_UtilDeleteFile(newDefinitions);
    SHU_UtilRunTry("\"%s\" --definitions %s > %sdefinitions.log 2>&1", SHU_UtilGetPath(SHU_UtilFormat("%sbin/OpenECS", CONFIG.DIRS.output)), newDefinitions, CONFIG.DIRS.output);

    if (SHU_UtilFileExists(newDefinitions) != SHUFileType_Directory)
    {
        SHU_UtilWarn("the definition files are not written; " SHUM_LOG_PATH("%sdefinitions.log") " says why.", CONFIG.DIRS.output);
        return;
    }

    // they are written fresh into the cache, then copied: a file that has not changed keeps its time, so the C plugins that include it do not compile again
    if (SHU_UtilFileExists(definitions) == SHUFileType_Directory)
    {
        SHU_UtilListDirectory(definitions, false, DeleteOldDefinition, newDefinitions);
    }

    SHU_UtilCopyFile(newDefinitions, definitions);

    char *up = SHU_UtilFormat("../%s", definitions);
    WriteLuaConfig("", definitions, "\"std/plugins/*/types.lua\", \"include/ecs.lua\"");
    WriteLuaConfig("std/", up, "\"plugins/*/types.lua\"");
    WriteLuaConfig("examples/", up, "");
}

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

    // the test gets its path with forward slashes on every system, as a test finds its own folder by them
    char *test = SHU_UtilFormat("%s", path);

    for (char *c = test; *c != '\0'; c++)
    {
        *c = *c == '\\' ? '/' : *c;
    }

    (void)data;
    char *log = SHU_UtilFormat("%stest.log", CONFIG.DIRS.output);
    bool passed = SHU_UtilRunTry("\"%s\" --test %s > %s 2>&1", SHU_UtilGetPath(SHU_UtilFormat("%sbin/OpenECS", CONFIG.DIRS.output)), test, log) == 0;

    SHUSlice output = SHU_UtilReadFile(log);
    passed = passed && strstr(output.data, "Sanitizer") == NULL && strstr(output.data, "runtime error:") == NULL;

    CONFIG.TESTS.run++;

    if (passed)
    {
        printf("pass %s\n", test);
    }
    else
    {
        CONFIG.TESTS.failed++;
        printf("FAIL %s\n%s\n", test, (char *)output.data);
    }

    fflush(stdout);
    free(output.data);
}

/// @brief Tells whether the command line asks for an example's tests: it names the example, or names none.
static bool IsTested(const char *path)
{
    // the entry is the folder's path, with a separator at its end
    int end = (int)strlen(path) - 1;
    int start = end;

    while (start > 0 && path[start - 1] != '/' && path[start - 1] != SHUM_PATH_SEPARATOR)
    {
        start--;
    }

    bool named = CONFIG.FLAGS.tests.count == 0;

    for (int i = 0; i < CONFIG.FLAGS.tests.count && !named; i++)
    {
        named = (int)strlen(CONFIG.FLAGS.tests.values[i]) == end - start && strncmp(path + start, CONFIG.FLAGS.tests.values[i], (usz)(end - start)) == 0;
    }

    return named;
}

/// @brief Runs the test of each example of the folder that the command line asks for.
static void RunExampleTests(const char *path, SHUFileType type, void *data)
{
    if (type == SHUFileType_Directory && IsTested(path))
    {
        SHU_UtilListDirectory(path, false, RunTest, data);
    }
}

// the examples are the tests: each test of bin/examples/, or of the examples the command line names, runs with the Debug build, on SDL's offscreen driver
// unless SDL_VIDEO_DRIVER names another (DESIGN 17.5)
static void RunTests(void)
{
    char *examples = SHU_UtilFormat("%sbin/examples/", CONFIG.DIRS.output);
    SHU_UtilListDirectory(examples, false, RunExampleTests, NULL);

    if (CONFIG.TESTS.run == 0)
    {
        SHU_UtilFail("no test is found in " SHUM_LOG_PATH("%s") ".", examples);
    }

    if (CONFIG.TESTS.failed > 0)
    {
        SHU_UtilFail("%d of %d tests failed.", CONFIG.TESTS.failed, CONFIG.TESTS.run);
    }

    SHU_UtilLog("all %d tests passed.", CONFIG.TESTS.run);
}
