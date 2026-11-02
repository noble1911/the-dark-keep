/**
 * @file keeper/keeper_coop.hpp
 *
 * The co-op session around the siege: spending Renown between maps without leaving it, the wait for
 * the next map, and what happens when the host goes.
 *
 * Between maps the session stays up on the game-over screen and each master can open the Keeper screen
 * over it; nothing a master buys there touches the simulation. Host's choice of Next or Retry gives
 * everyone CoopMinGraceTicks, longer while anyone is in the Keeper, but never more than
 * CoopMaxGraceTicks: purchases are saved as they are made, so nothing is lost when the Keeper closes.
 * The host can start at once by choosing again. Until the host chooses, everyone else waits on the
 * game-over screen, and may leave whenever they like: the game never moves on by itself.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "engine/surface.hpp"

namespace devilution::keeper {

/** Everyone gets this long to see the next map coming, and to open the Keeper. */
constexpr int CoopMinGraceTicks = 5 * 20;
/** The most anyone in the Keeper can hold the next map up. */
constexpr int CoopMaxGraceTicks = 30 * 20;
/** How long the notice stays up when the host has gone, before going back to the menu. */
constexpr int CoopHostLeftNoticeTicks = 8 * 20;

void CoopOnLevelLoaded();
void CoopOnGameEnded();
/** Once a game tick, on every client. Sends this master's status when it changes. */
void CoopTick();

void CoopOpenKeeper();
/** A message from `playerId`, or this client's own. */
void CoopNoteInKeeper(size_t playerId, bool inKeeper);
bool CoopIsInKeeper(size_t playerId);
bool CoopAnyMasterInKeeper();
/** The host's word on how long until the next map (0: none coming). */
void CoopNoteStarting(int ticks);
int CoopStartingTicks();

/** What the game-over banner adds in co-op: what everyone is waiting for. */
std::string CoopGameOverLine();
/** Over the Keeper screen: the next map is coming. */
void DrawCoopKeeperBanner(const Surface &out);

/** The game's creator has gone (not for the next map): with TCP nobody can reach anybody any more, so the game ends. */
void CoopNoteHostLeft();
bool CoopHasHostLeft();
/** @return true when the notice has been up for long enough. */
bool CoopHostLeftTick();
void DrawCoopHostLeft(const Surface &out, int hudTop);

} // namespace devilution::keeper
