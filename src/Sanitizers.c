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

/// @brief Leaks inside graphics drivers, which SDL reaches through EGL and OpenGL. They are not OpenECS's.
const char *__lsan_default_suppressions(void)
{
    return "leak:SDL_EGL_InitializeOffscreen\n"
           "leak:SDL_EGL_LoadLibrary\n"
           "leak:GL_RunCommandQueue\n";
}

#pragma endregion Source Only

#endif
