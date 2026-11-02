/**
 * @file keeper/keeper_menu.hpp
 *
 * The master's menu screens: campaign level select, custom map list and the host's game choice.
 * Each runs its own loop and, when the player picks something, calls SetCurrentSetup().
 */
#pragma once

namespace devilution::keeper {

/** The character screen: XP, stats, Renown, and everything unlocked or still to unlock. */
void UiKeeperScreen();
/** @return true if a level was chosen. */
bool UiCampaignLevelSelect();
/** @return true if a map was chosen. */
bool UiCustomMapSelect();
/** Multiplayer host: campaign level or custom map. @return true if a game was chosen. */
bool UiHostGameSelect();

} // namespace devilution::keeper
