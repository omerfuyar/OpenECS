#pragma once

// Popups: popups, the pixels each popup draws, and its events (DESIGN 4.6).

#include "interface/Layout.h"

#pragma region Declarations

/// @brief A popup of a panel.
struct ECSIPopup
{
    ECSPanel panel;
    ECSPopupDesc desc;
    SDL_Surface *pixels; // what it drew last, or NULL before it is first drawn
    bool needsDraw;
    bool closed;   // out of the list of open popups, waiting for its Closed function and ECSIPopups_DestroyClosed; it gets no more events
    bool released; // its Closed function has run, so ECSIPopups_DestroyClosed frees it
    void *window;  // what the window shows it with, or NULL
};

/// @brief Function that frees what the window shows a popup with, before the popup is freed.
typedef void (*ECSIPopupReleaseFunction)(ECSPopup popup);

/// @brief Sets the function that frees what the window shows a popup with.
/// @param function The function, or NULL.
void ECSIPopups_SetRelease(ECSIPopupReleaseFunction function);

/// @brief Closes every popup, runs the Closed functions that have not run, and frees the popups. Call it before the panels and the window are terminated.
void ECSIPopups_Terminate(void);

/// @brief Gets the open popups, the oldest first.
/// @param retCount Gets the number of popups.
/// @return The popups, valid until a popup opens or closes.
ECSPopup *ECSIPopups_GetOpen(usz *retCount);

/// @brief Gets the newest open menu, which takes the key presses.
/// @return The menu, or NULL if no menu is open.
ECSPopup ECSIPopups_GetMenu(void);

/// @brief Closes every open popup, as a press outside them does.
void ECSIPopups_CloseAll(void);

/// @brief Closes the popups whose panel closed, failed or is hidden. Call it before closed panels are destroyed, then deliver the events, so their Closed functions run while the panels live.
void ECSIPopups_CloseHidden(void);

/// @brief Frees the closed popups whose Closed function has run.
void ECSIPopups_DestroyClosed(void);

/// @brief Checks whether an open popup needs to be drawn.
bool ECSIPopups_WantsFrame(void);

/// @brief Draws a popup into its pixels if it needs it.
/// @param popup The popup.
/// @return true if its pixels changed.
bool ECSIPopup_Draw(ECSPopup popup);

/// @brief Queues an event for a popup. It is delivered after the current callback returns, unless the popup closes first.
/// @param popup The popup.
/// @param event The event, with positions in the popup's surface pixels. The queue keeps a copy.
void ECSIPopup_PostEvent(ECSPopup popup, const ECSPanelEvent *event);

#pragma endregion Declarations
