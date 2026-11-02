/**
 * @file keeper/keeper_flow.cpp
 *
 * See keeper_flow.hpp.
 */
#include "keeper/keeper_flow.hpp"

#include <algorithm>

#ifdef USE_SDL3
#include <SDL3/SDL_timer.h>
#else
#include <SDL.h>
#endif

#include <fmt/format.h>

#include "diablo.h"
#include "effects.h"
#include "keeper/keeper_game.hpp"
#include "levels/gendung.h"
#include "multi.h"
#include "player.h"
#include "plrmsg.h"

namespace devilution::keeper {

namespace {

/** A tick shorter than this can't be drawn at all on a 60Hz screen, whatever the game's own rate and the toggle ask for. */
constexpr int MinTickMilliseconds = 16;

/** A master can't send pings closer together than this. */
constexpr uint32_t PingSendGapMilliseconds = 1000;
/** Nor is anyone made to show them closer than this, whatever arrives (a little under, for network jitter). */
constexpr uint32_t PingReceiveGapMilliseconds = 500;

std::array<Ping, MaxPings> pings;
std::array<uint32_t, MAX_PLRS> lastPingAt {};
uint32_t lastPingSentAt = 0;

size_t speedLevel = 0;
bool prepTimerHeld = false;

int BaseTickMilliseconds()
{
	return 1000 / std::max<int>(sgGameInitInfo.nTickRate, 1);
}

} // namespace

size_t GameSpeedLevel()
{
	return speedLevel;
}

int GameSpeedMultiplier()
{
	return GameSpeeds[speedLevel];
}

void CycleGameSpeed()
{
	RequestGameSpeed((speedLevel + 1) % GameSpeeds.size());
}

void ApplyGameSpeedLevel(size_t level)
{
	speedLevel = std::min(level, GameSpeeds.size() - 1);
	gnTickDelay = static_cast<uint16_t>(std::max(BaseTickMilliseconds() / GameSpeeds[speedLevel], MinTickMilliseconds));
}

void OnKeepLoadedSpeed()
{
	if (gbIsMultiplayer)
		speedLevel = 0;
	// Left alone at the normal speed: a test's own tick length (the devctl's "speed") stands until asked for.
	if (speedLevel != 0)
		ApplyGameSpeedLevel(speedLevel);
}

void OnGameEndedSpeed()
{
	if (speedLevel != 0)
		gnTickDelay = static_cast<uint16_t>(BaseTickMilliseconds());
}

bool CanHoldPrepTimer()
{
	return !gbIsMultiplayer;
}

bool IsPrepTimerHeld()
{
	return prepTimerHeld && CanHoldPrepTimer();
}

void TogglePrepTimerHold()
{
	if (CanHoldPrepTimer())
		prepTimerHeld = !prepTimerHeld;
}

bool RequestPing(Point tile)
{
	const uint32_t now = SDL_GetTicks();
	if (lastPingSentAt != 0 && now - lastPingSentAt < PingSendGapMilliseconds)
		return false;
	lastPingSentAt = now;
	SendPing(tile);
	return true;
}

void ApplyPing(size_t playerId, Point tile)
{
	if (playerId >= MAX_PLRS || !InDungeonBounds(tile))
		return;
	const uint32_t now = SDL_GetTicks();
	if (lastPingAt[playerId] != 0 && now - lastPingAt[playerId] < PingReceiveGapMilliseconds)
		return;
	lastPingAt[playerId] = now;
	// The oldest makes way.
	Ping &slot = *std::min_element(pings.begin(), pings.end(), [](const Ping &a, const Ping &b) { return a.until < b.until; });
	slot = { tile, static_cast<uint8_t>(playerId), now + PingMilliseconds };
	PlaySFX(SfxID::QuestDone);
	if (playerId != MyPlayerId)
		EventPlrMsg(fmt::format("{} marks a spot on the map.", Players[playerId]._pName), UiFlags::ColorGold);
}

const std::array<Ping, MaxPings> &Pings()
{
	return pings;
}

void ClearPings()
{
	pings = {};
	lastPingAt = {};
}

} // namespace devilution::keeper
