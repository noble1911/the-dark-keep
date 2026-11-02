/**
 * @file keeper/keeper_flow.hpp
 *
 * How a game paces: game speed and the single-player choice of no timer on preparation.
 * Controller code can call these directly; each is also a bound key (KeeperAction).
 */
#pragma once

#include <array>
#include <cstddef>

namespace devilution::keeper {

// ---- game speed --------------------------------------------------------------------------

/** The speeds the toggle goes through, as multiples of the game's own tick rate. */
constexpr std::array<int, 3> GameSpeeds { 1, 2, 3 };

/** Index into GameSpeeds. */
size_t GameSpeedLevel();
int GameSpeedMultiplier();
/** Asks for the next speed. In co-op it goes by way of the host, and everyone switches together. */
void CycleGameSpeed();
/** Sets the tick length, on this client alone (keeper_game.cpp decides when). */
void ApplyGameSpeedLevel(size_t level);
/** A keep has loaded: single player carries on at its chosen speed, co-op starts at the normal one. */
void OnKeepLoadedSpeed();
/** Back to the game's own tick rate, for the menus. The choice is remembered for the next keep. */
void OnGameEndedSpeed();

// ---- no timer on preparation (single player) -----------------------------------------------

/** There is only someone else to wait for in co-op, so only single player can stop the clock. */
bool CanHoldPrepTimer();
/** The clock isn't running in preparation and respite: the raid comes when the master opens the gates. */
bool IsPrepTimerHeld();
void TogglePrepTimerHold();

} // namespace devilution::keeper
