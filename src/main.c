#define SDL_MAIN_USE_CALLBACKS 1
#include "SDL3/SDL.h"
#include "SDL3/SDL_main.h"
#include "SDL3_image/SDL_image.h"
#include "SDL3_mixer/SDL_mixer.h"
#include "SDL3_net/SDL_net.h"
#include "SDL3_ttf/SDL_ttf.h"
#include "shu/shu.h"
#include "lua/lua.h"

#define checkSDL(fun) SHU_Assert((fun), "SDL Error: '%s'", SDL_GetError())

static SDL_Window *window = NULL;
static SDL_Renderer *renderer = NULL;
static Uint64 lastFrameTime;
static double dt;

SDL_AppResult SDL_AppInit(void **appstate, int argc, char **argv)
{
    checkSDL(SDL_SetAppMetadata("Open Editor Config System", "0.0", "com.omerfuyar.open-ecs"));
    checkSDL(SDL_Init(SDL_INIT_VIDEO));
    checkSDL(SDL_CreateWindowAndRenderer("OpenECS", 640, 480, SDL_WINDOW_RESIZABLE, &window, &renderer));
    checkSDL(SDL_SetRenderLogicalPresentation(renderer, 640, 480, SDL_LOGICAL_PRESENTATION_LETTERBOX));

    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event)
{
    if (event->type == SDL_EVENT_QUIT)
    {
        return SDL_APP_SUCCESS;
    }

    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void *appstate)
{
    {
        Uint64 thisFrameTime = SDL_GetTicks();
        dt = (double)(thisFrameTime - lastFrameTime) / 1000.0;
        lastFrameTime = thisFrameTime;
    }

    SDL_SetRenderDrawColorFloat(renderer, 0, 0, 0, SDL_ALPHA_OPAQUE_FLOAT); /* new color, full alpha. */
    SDL_RenderClear(renderer);

    SDL_RenderPresent(renderer);

    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result)
{
}