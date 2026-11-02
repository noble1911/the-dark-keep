/**
 * @file keeper/keeper_flow.cpp
 *
 * See keeper_flow.hpp.
 */
#include "keeper/keeper_flow.hpp"

#include <algorithm>

#include "diablo.h"
#include "keeper/keeper_game.hpp"
#include "multi.h"

namespace devilution::keeper {

namespace {

/** A tick shorter than this can't be drawn at all on a 60Hz screen, whatever the game's own rate and the toggle ask for. */
constexpr int MinTickMilliseconds = 16;

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

} // namespace devilution::keeper
