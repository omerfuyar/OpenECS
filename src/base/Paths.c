#include "base/Paths.h"

#include "SDL3/SDL.h"

bool ECSPath_IsSeparator(char character)
{
#if defined(_WIN32)
    return character == '/' || character == '\\';
#else
    return character == '/';
#endif
}

bool ECSPath_IsAbsolute(const char *path)
{
    SDL_assert(path != nullptr);

#if defined(_WIN32)
    bool letter = (path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z');

    if (letter && path[1] == ':' && ECSPath_IsSeparator(path[2]))
    {
        return true;
    }
#endif

    return ECSPath_IsSeparator(path[0]);
}

const char *ECSPath_FindLastSeparator(const char *path)
{
    SDL_assert(path != nullptr);

    const char *last = nullptr;

    for (const char *character = path; *character != '\0'; character++)
    {
        if (ECSPath_IsSeparator(*character))
        {
            last = character;
        }
    }

    return last;
}

SHUResult ECSPath_GetFull(char **retFullPath, const char *path)
{
    SDL_assert(retFullPath != nullptr);
    SDL_assert(path != nullptr);

    if (SDL_asprintf(retFullPath, "%s%s", ECSPath_IsAbsolute(path) ? "" : SDL_GetBasePath(), path) < 0)
    {
        *retFullPath = nullptr;
        return SHUResult_ErrAllocation;
    }

    return SHUResult_Ok;
}

int ECSIPath_GetFolderLength(const char *file, const char *path)
{
    SDL_assert(file != nullptr);
    SDL_assert(path != nullptr);

    const char *slash = ECSPath_FindLastSeparator(file);
    return slash == nullptr || ECSPath_IsAbsolute(path) ? 0 : (int)(slash - file + 1);
}
