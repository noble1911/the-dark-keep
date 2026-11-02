/**
 * @file keeper/keeper_flow.hpp
 *
 * How a game flows: game speed, the single-player choice of no timer on preparation, and pings.
 * Controller code can call these directly; each is also a bound key (KeeperAction).
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "engine/point.hpp"

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
/** Back to the game's own tick rate, for the menus. Single player's speed is remembered for the next keep; the held timer isn't. */
void OnGameEndedSpeed();

// ---- no timer on preparation (single player) -----------------------------------------------

/** There is only someone else to wait for in co-op, so only single player can stop the clock. */
bool CanHoldPrepTimer();
/** The clock isn't running in preparation and respite: the raid comes when the master opens the gates. */
bool IsPrepTimerHeld();
void TogglePrepTimerHold();

// ---- pings ---------------------------------------------------------------------------------

/**
 * Marks `tile` for every master: they see and hear it for a few seconds. One press and no typing, so
 * any button can do it: this is the call for a controller. @return false if this master pinged a moment ago.
 */
bool RequestPing(Point tile);
/** A master's ping arrived (every client, the sender included). Too many from one master are dropped. */
void ApplyPing(size_t playerId, Point tile);

struct Ping {
	Point tile;
	uint8_t player = 0;
	/** SDL_GetTicks when it fades. It is a mark on the screen and not part of the game, so it keeps real time at any speed. */
	uint32_t until = 0;
};
constexpr size_t MaxPings = 8;
/** Those with until > now are live. */
const std::array<Ping, MaxPings> &Pings();
/** How long a ping lasts, in milliseconds. */
constexpr uint32_t PingMilliseconds = 4000;
void ClearPings();

} // namespace devilution::keeper
