// this file is a glue header of the glue between clay and SDL3
// copied to build/.../include together with clay.h
// implementation is in clay.c

#ifndef CLAY_HEADER
#include "clay.h"
#endif
#include "SDL3/SDL.h"
#include "SDL3_ttf/SDL_ttf.h"

typedef struct
{
    SDL_Renderer *renderer;
    TTF_TextEngine *textEngine;
    TTF_Font **fonts;
} Clay_SDL3RendererData;

void SDL_Clay_RenderFillRoundedRect(Clay_SDL3RendererData *rendererData, const SDL_FRect rect, const float cornerRadius, const Clay_Color _color);

void SDL_Clay_RenderArc(Clay_SDL3RendererData *rendererData, const SDL_FPoint center, const float radius, const float startAngle, const float endAngle, const float thickness, const Clay_Color color);

void SDL_Clay_RenderClayCommands(Clay_SDL3RendererData *rendererData, Clay_RenderCommandArray *rcommands);
