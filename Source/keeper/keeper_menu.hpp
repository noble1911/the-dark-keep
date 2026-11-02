/**
 * @file keeper/keeper_menu.hpp
 *
 * The master's menu screens: campaign level select, custom map list and the host's game choice.
 * Each runs its own loop and, when the player picks something, calls SetCurrentSetup().
 */
#pragma once

#include "engine/point.hpp"
#include "engine/surface.hpp"

namespace devilution::keeper {

/** The character screen: XP, stats, Renown, and everything unlocked or still to unlock. */
void UiKeeperScreen();
/**
 * The same screen over a running co-op game, between maps. Unlike UiKeeperScreen it has no loop of its
 * own: the game's turns have to keep flowing, so the game draws it and passes it the input.
 */
void OpenKeeperOverlay();
void CloseKeeperOverlay();
/** The game is over: its images use its palette. They stay between openings otherwise, as decoding them is slow. */
void FreeKeeperOverlayImages();
bool IsKeeperOverlayOpen();
void DrawKeeperOverlay(const Surface &out, Point mouse);
void KeeperOverlayClick();
void KeeperOverlayWheel(int deltaY);
/** @return true if a level was chosen. */
bool UiCampaignLevelSelect();
/** @return true if a map was chosen. */
bool UiCustomMapSelect();
/** Multiplayer host: campaign level or custom map. @return true if a game was chosen. */
bool UiHostGameSelect();

} // namespace devilution::keeper
