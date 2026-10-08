// this file is a glue header that sets stb_ds to use SDL's allocator and assertions
// copied to build/.../include/stb/ together with stb_ds.h
// include it instead of stb_ds.h; implementation is in stb.c

#include "SDL3/SDL_assert.h"
#include "SDL3/SDL_stdinc.h"

#define STBDS_REALLOC(context, pointer, size) SDL_realloc(pointer, size)
#define STBDS_FREE(context, pointer) SDL_free(pointer)
#define STBDS_ASSERT(condition) SDL_assert(condition)

#include "stb_ds.h"
