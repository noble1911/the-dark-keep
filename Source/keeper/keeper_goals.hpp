/**
 * @file keeper/keeper_goals.hpp
 *
 * Optional goals for a campaign level: clear it in style for a medal and a little XP, once per medal. Each client's
 * own tallies can differ in co-op (see keeper_raid.cpp), so only the host watches for a goal being lost. It sends
 * the result with its state, and every client awards its own master from that.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace devilution::keeper {

/** Bits of a level's medals. Add new goals at the end: saved profiles hold these. */
enum class LevelGoal : uint8_t {
	NoEscapes,
	LordUnharmed,
	VaultUntouched,
};
constexpr size_t NumLevelGoals = 3;

struct LevelGoalInfo {
	std::string_view name;
	std::string_view description;
};
const LevelGoalInfo &GoalInfo(size_t goal);
/** A new keep: nothing failed and nothing paid yet. */
void ForgetLevelGoals();
/** The medals for this keep have been handed out to this master. */
bool WereGoalsAwarded();

/** Host: a goal can no longer be met in this game. */
void FailGoal(LevelGoal goal);
/** Host, every tick of a raid: the Keep Lord has been hurt or killed. */
void WatchLordForGoal();

/** A bit for each goal lost so far in this game: the host's word. */
uint8_t FailedGoals();
void SetFailedGoals(uint8_t failed);
bool GoalsApply();

/** Every client, as the keep stands: awards the medals the host's result allows. */
void AwardLevelGoals();

} // namespace devilution::keeper
