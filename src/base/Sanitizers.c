// for dl_iterate_phdr and RTLD_NODELETE
#define _GNU_SOURCE

#include "base/Sanitizers.h"

#include "OpenECS.h"

#ifdef DEBUG

#include <dlfcn.h>
#include <link.h>
#include <sanitizer/lsan_interface.h>

#pragma region Source Only

// the sanitizers look these functions up by name, so the executable exports them
OPENECS_EXPORT const char *__asan_default_options(void);
OPENECS_EXPORT const char *__lsan_default_suppressions(void);

const char *__asan_default_options(void)
{
    return "detect_leaks=1:strict_string_checks=1:check_initialization_order=1";
}

/// @brief Leaks inside the system libraries that SDL loads: graphics drivers, display servers, input methods and D-Bus. They are not OpenECS's. The libraries are kept loaded, so their names can be matched.
const char *__lsan_default_suppressions(void)
{
    return "leak:libdbus-1.so\n"
           "leak:libX11.so\n"
           "leak:libXi.so\n"
           "leak:libxkbcommon.so\n"
           "leak:libwayland-client.so\n"
           "leak:libdecor\n"
           "leak:libEGL\n"
           "leak:libGLX\n"
           "leak:libGL.so\n"
           "leak:libGLdispatch.so\n"
           "leak:libgallium\n"
           "leak:libLLVM\n"
           "leak:_dri.so\n"
           "leak:libdrm\n"
           "leak:libvulkan\n" // the loader and drivers such as libvulkan_radeon.so
           "leak:libnvidia\n"
           "leak:libibus\n"
           "leak:libfcitx\n"
           "leak:libfontconfig.so\n"
           // drivers that SDL unloads before the check at exit; also matched by the SDL function that called them
           "leak:SDL_EGL_InitializeOffscreen\n"
           "leak:SDL_EGL_LoadLibrary\n"
           "leak:GL_RunCommandQueue\n";
}

/// @brief Marks a loaded library so it is never unloaded. dlopen with RTLD_NOLOAD finds it without loading anything.
static int ECSI_SanitizersKeep(struct dl_phdr_info *info, size_t size, void *data)
{
    (void)size;
    (void)data;

    if (info->dlpi_name != NULL && info->dlpi_name[0] != '\0')
    {
        (void)dlopen(info->dlpi_name, RTLD_LAZY | RTLD_NOLOAD | RTLD_NODELETE);
    }

    return 0;
}

#pragma endregion Source Only

void ECSI_SanitizersKeepLibraries(void)
{
    dl_iterate_phdr(ECSI_SanitizersKeep, NULL);
}

void ECSI_SanitizersCheckLeaks(void)
{
    __lsan_do_leak_check();
}

#else

void ECSI_SanitizersKeepLibraries(void)
{
}

void ECSI_SanitizersCheckLeaks(void)
{
}

#endif
