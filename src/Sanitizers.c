// Sanitizers: settings of the address and leak sanitizers that the Debug build uses.

#ifdef DEBUG

#include "OpenECS.h"

#pragma region Source Only

// the sanitizers look these functions up by name, so the executable exports them
OPENECS_EXPORT const char *__asan_default_options(void);
OPENECS_EXPORT const char *__lsan_default_suppressions(void);

const char *__asan_default_options(void)
{
    return "detect_leaks=1:strict_string_checks=1:check_initialization_order=1";
}

/// @brief Leaks inside the system libraries that SDL loads: graphics drivers, display servers, input methods and D-Bus. They are not OpenECS's. App checks for leaks while these libraries are still loaded, so their names can be matched.
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
           "leak:libvulkan\n"
           "leak:libnvidia\n"
           "leak:libibus\n"
           "leak:libfcitx\n"
           "leak:libfontconfig.so\n"
           // drivers that SDL unloads before the check at exit; also matched by the SDL function that called them
           "leak:SDL_EGL_InitializeOffscreen\n"
           "leak:SDL_EGL_LoadLibrary\n"
           "leak:GL_RunCommandQueue\n";
}

#pragma endregion Source Only

#endif
