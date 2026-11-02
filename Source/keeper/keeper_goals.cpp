/**
 * @file keeper/keeper_goals.cpp
 *
 * Level goals and medals. See keeper_goals.hpp.
 */
#include "keeper/keeper_goals.hpp"

#include "keeper/keeper_campaign.hpp"
#include "keeper/keeper_game.hpp"
#include "keeper/keeper_heroes.hpp"
#include "keeper/keeper_profile.hpp"
#include "monster.h"

namespace devilution::keeper {

namespace {

constexpr std::array<LevelGoalInfo, NumLevelGoals> Goals { {
	{ "No escapes", "No hero gets out of the keep alive, other than in terror." },
	{ "Lord unharmed", "The Keep Lord takes no wound in any raid." },
	{ "Vault untouched", "No hero takes a coin from the vault." },
} };

uint8_t failedGoals = 0;

} // namespace

const LevelGoalInfo &GoalInfo(size_t goal)
{
	return Goals[goal];
}

void FailGoal(LevelGoal goal)
{
	if (IsHost())
		failedGoals |= static_cast<uint8_t>(1U << static_cast<size_t>(goal));
}

void WatchLordForGoal()
{
	const Monster *boss = FindBoss();
	if (State.bossDown || (boss != nullptr && boss->hitPoints < boss->maxHitPoints))
		FailGoal(LevelGoal::LordUnharmed);
}

uint8_t FailedGoals()
{
	return failedGoals;
}

void SetFailedGoals(uint8_t failed)
{
	failedGoals = failed;
}

bool GoalsApply()
{
	return CurrentSetup().mode == GameMode::Campaign;
}

void AwardLevelGoals()
{
	if (!GoalsApply())
		return;
	const auto all = static_cast<uint8_t>((1U << NumLevelGoals) - 1);
	ProfileAwardMedals(CurrentSetup().campaignLevel, static_cast<uint8_t>(all & ~failedGoals));
}

} // namespace devilution::keeper
