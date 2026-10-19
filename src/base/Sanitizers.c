#include "base/Sanitizers.h"

#include "OpenECS.h"

// MinGW has no sanitizers, so Windows builds keep the empty functions
#if defined(DEBUG) && defined(__linux__)

#include <dlfcn.h>
#include <sanitizer/lsan_interface.h>

#pragma region Source Only

// the sanitizers look these functions up by name, and the libraries call dlclose, so the executable exports them
OPENECS_EXPORT const char *__asan_default_options(void);
OPENECS_EXPORT const char *__lsan_default_suppressions(void);
OPENECS_EXPORT int dlclose(void *handle);

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
           "leak:libVkLayer\n" // Vulkan layers, such as Mesa's device select
           "leak:libnvidia\n"
           "leak:libibus\n"
           "leak:libfcitx\n"
           "leak:libfontconfig.so\n"
           "leak:libasound.so\n" // audio systems, which plugins such as audio start
           "leak:libpulse\n"
           "leak:libpipewire\n"
           // drivers that SDL unloads before the check at exit; also matched by the SDL function that called them
           "leak:SDL_EGL_InitializeOffscreen\n"
           "leak:SDL_EGL_LoadLibrary\n"
           "leak:GL_RunCommandQueue\n";
}

/// @brief Replaces the system's dlclose, as the executable's symbols come before the libraries': no library is unloaded until the program exits,
/// so a leak inside one is matched by its name. Graphics drivers are otherwise unloaded when SDL or Vulkan's loader is done with them, and their leaks would show an unknown module.
int dlclose(void *handle)
{
    (void)handle;
    return 0;
}

#pragma endregion Source Only

void ECSISanitizers_CheckLeaks(void)
{
    __lsan_do_leak_check();
}

#else

void ECSISanitizers_CheckLeaks(void)
{
}

#endif
