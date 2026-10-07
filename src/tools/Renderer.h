#pragma once

// Renderer: puts panel surfaces and the core's own interface on screen.

#include "Global.h"
#include "tools/Platform.h"

#include "clay/clay.h"

#pragma region Declarations

/// @brief Drawing state of one OS window.
typedef struct ECSI_RenderWindow ECSI_RenderWindow;

/// @brief A texture that shows a panel's pixels in one OS window.
typedef struct ECSI_RenderTexture ECSI_RenderTexture;

/// @brief Loads the font of the core's interface.
/// @param fontPath Path of a TrueType font file.
/// @return SHUResult_Ok, SHUResult_ErrInternal if SDL_ttf fails to start, or SHUResult_ErrFile if the font cannot be loaded.
SHUWUR SHUResult ECSI_RendererInitialize(const char *fontPath);

/// @brief Releases the font and the shared GPU device.
void ECSI_RendererTerminate(void);

/// @brief Measures text for Clay, with the core's font.
/// @param text Text to measure.
/// @param config Clay's text settings; only the font size is used.
/// @param userData Not used.
/// @return Size of the text in layout units.
Clay_Dimensions ECSI_RendererMeasureText(Clay_StringSlice text, Clay_TextElementConfig *config, void *userData);

/// @brief Creates the drawing state of an OS window. All OS windows share one GPU device.
/// @param retRenderWindow The new drawing state.
/// @param window OS window to draw into.
/// @return SHUResult_Ok, SHUResult_ErrInternal if no renderer can be created, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSI_RenderWindowCreate(ECSI_RenderWindow **retRenderWindow, ECSI_OsWindow *window);

/// @brief Destroys the drawing state of an OS window and sets the handle to NULL.
/// @param renderWindow Drawing state to destroy.
void ECSI_RenderWindowDestroy(ECSI_RenderWindow **renderWindow);

/// @brief Starts a frame by clearing the window with the background colour.
/// @param renderWindow Window to draw into.
void ECSI_RenderWindowBegin(ECSI_RenderWindow *renderWindow);

/// @brief Draws a texture into a rectangle.
/// @param renderWindow Window to draw into.
/// @param texture Texture to draw.
/// @param x Left edge in layout units.
/// @param y Top edge in layout units.
/// @param width Width in layout units.
/// @param height Height in layout units.
void ECSI_RenderWindowDrawTexture(ECSI_RenderWindow *renderWindow, ECSI_RenderTexture *texture, f32 x, f32 y, f32 width, f32 height);

/// @brief Draws Clay's render commands: the core's own interface.
/// @param renderWindow Window to draw into.
/// @param commands Commands from Clay_EndLayout.
void ECSI_RenderWindowDrawCommands(ECSI_RenderWindow *renderWindow, Clay_RenderCommandArray commands);

/// @brief Ends a frame and shows it.
/// @param renderWindow Window to show.
void ECSI_RenderWindowPresent(ECSI_RenderWindow *renderWindow);

/// @brief Creates a texture for a panel's pixels.
/// @param retTexture The new texture.
/// @param renderWindow Window the texture belongs to.
/// @param width Width in pixels.
/// @param height Height in pixels.
/// @return SHUResult_Ok, SHUResult_ErrInternal if SDL fails, or SHUResult_ErrAllocation.
SHUWUR SHUResult ECSI_RenderTextureCreate(ECSI_RenderTexture **retTexture, ECSI_RenderWindow *renderWindow, i32 width, i32 height);

/// @brief Destroys a texture and sets the handle to NULL.
/// @param texture Texture to destroy.
void ECSI_RenderTextureDestroy(ECSI_RenderTexture **texture);

/// @brief Uploads pixels to a texture.
/// @param texture Texture to change.
/// @param pixels ARGB8888 pixels, as large as the texture.
/// @param pitch Bytes per row of the pixels.
void ECSI_RenderTextureUpdate(ECSI_RenderTexture *texture, SHUSlice pixels, i32 pitch);

#pragma endregion Declarations
