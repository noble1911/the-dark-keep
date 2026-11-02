/**
 * @file keeper/keeper_raidtypes.cpp
 *
 * The raid types and which levels get which. See keeper_raidtypes.hpp.
 */
#include "keeper/keeper_raidtypes.hpp"

#include <algorithm>

#include "keeper/keeper.hpp"
#include "keeper/keeper_campaign.hpp"
#include "keeper/keeper_game.hpp"
#include "player.h"
#include "tables/monstdat.h"

namespace devilution::keeper {

namespace {

constexpr HeroClassKind W = HeroClassKind::Warrior;
constexpr HeroClassKind R = HeroClassKind::Rogue;
constexpr HeroClassKind S = HeroClassKind::Sorcerer;

// clang-format off
/** Balance is tuned here. Columns as in RaidTypeInfo: life, damage and armour %, resistances, fear and bait %, fast. */
constexpr std::array<RaidTypeInfo, NumRaidTypes> RaidTypes { {
	{ "Mixed Party", "A bit of everything: no easy answer, and no great weakness.",
		{ W, R, S, W, R, S }, 100, 100, 100, 0, 0, 0, 0, 100, 100, false },
	{ "Thieves' Band", "Rogues spot traps and go for the gold. Greedy: bait works. Guard the vault.",
		{ R, R, W, R, S, R }, 90, 90, 90, 0, 0, 0, 0, 100, 130, false },
	{ "Coven", "Sorcerers hit hard from afar and fall up close. They ignore bait, and locks hold them.",
		{ S, S, W, S, R, S }, 90, 110, 100, 0, 0, 0, 0, 100, 100, false },
	{ "Fire-Warded Host", "Warded against fire: Fire Traps, fire walls and fire spells do little.",
		{ W, R, S, W, R, S }, 100, 100, 100, RESIST_FIRE, 75, 0, 0, 100, 100, false },
	{ "Iron Column", "Heavy armour: traps and arrows rarely land. Few run. Spells and big monsters do the work.",
		{ W, W, S, W, R, W }, 110, 90, 170, 0, 0, 0, 0, 60, 70, false },
	{ "Light Raiders", "Fast and frail. They reach the vault early and scare easily: hold them near the entrance.",
		{ R, W, S, R, W, S }, 70, 100, 60, 0, 0, 0, 0, 140, 100, true },
} };

/** The first type in a level's plan is its signature, and the one its champions come as. */
constexpr size_t MaxPlanTypes = 4;
struct LevelPlan {
	std::array<RaidType, MaxPlanTypes> types;
	size_t count;
};
using T = RaidType;
/** One line per campaign level, in the order of CampaignLevels. Early levels bring in one new idea at a time. */
constexpr std::array<LevelPlan, NumCampaignLevels> LevelPlans { {
	{ { T::Mixed },                                           1 }, // Old Cellars
	{ { T::Mixed, T::Thieves },                               2 }, // Rat Warrens
	{ { T::Mixed, T::Coven },                                 2 }, // Sunken Chapel
	{ { T::Mixed, T::IronColumn },                            2 }, // Bone Pits
	{ { T::Thieves, T::Mixed },                               2 }, // Black Vaults
	{ { T::LightRaiders, T::Mixed },                          2 }, // Weeping Halls
	{ { T::FireWarded, T::Mixed, T::Coven },                  3 }, // Ashen Crypts
	{ { T::IronColumn, T::Thieves, T::Mixed },                3 }, // Iron Gaol
	{ { T::LightRaiders, T::Coven, T::Mixed },                3 }, // Maggot Deeps
	{ { T::Coven, T::IronColumn, T::Mixed },                  3 }, // Hollow Court
	{ { T::Thieves, T::LightRaiders, T::FireWarded },         3 }, // Blood Galleries
	{ { T::Coven, T::FireWarded, T::Mixed },                  3 }, // Shrouded Nave
	{ { T::IronColumn, T::Thieves, T::LightRaiders },         3 }, // Gallows Reach
	{ { T::FireWarded, T::Coven, T::IronColumn },             3 }, // Cinder Keep
	{ { T::LightRaiders, T::IronColumn, T::Thieves },         3 }, // Howling Stair
	{ { T::IronColumn, T::Coven, T::FireWarded },             3 }, // Tomb of Kings
	{ { T::Coven, T::Thieves, T::LightRaiders },              3 }, // Silent Abbey
	{ { T::IronColumn, T::FireWarded, T::LightRaiders },      3 }, // Chains of Night
	{ { T::FireWarded, T::Coven, T::IronColumn, T::Thieves }, 4 }, // Burning Sepulcre
	{ { T::Mixed, T::IronColumn, T::Coven, T::FireWarded },   4 }, // Throne of Ash
} };
/** Custom maps have no place in the curve, so they can bring anything. */
constexpr LevelPlan CustomPlan { { T::Mixed, T::Thieves, T::Coven, T::IronColumn }, 4 };
// clang-format on

/** Levels up to this one open with a plain raid, so the first thing a new player meets is the simplest. */
constexpr int PlainFirstRaidLevels = 4;
/** How many frames of a step fast heroes skip (of 8 for monster heroes), matching how a run looks for a full hero. */
constexpr int FastWalkSkippedFrames = 2;

uint32_t MixHash(uint32_t seed, uint32_t wave)
{
	uint32_t hash = seed ^ (wave * 2654435761U);
	hash ^= hash >> 15;
	hash *= 2246822519U;
	hash ^= hash >> 13;
	hash *= 3266489917U;
	return hash ^ (hash >> 16);
}

const LevelPlan &CurrentPlan()
{
	return CurrentSetup().mode == GameMode::Campaign ? LevelPlans[CurrentLevelNumber() - 1] : CustomPlan;
}

int Percent(int value, int percent)
{
	return value * percent / 100;
}

} // namespace

const RaidTypeInfo &RaidTypeInfoOf(RaidType type)
{
	return RaidTypes[static_cast<size_t>(type)];
}

RaidType RaidTypeForWave(uint8_t wave)
{
	if (TestRaidType >= 0 && static_cast<size_t>(TestRaidType) < NumRaidTypes)
		return static_cast<RaidType>(TestRaidType);
	const LevelPlan &plan = CurrentPlan();
	if (wave >= NumWaves())
		return plan.types[0];
	// Never the same kind twice running when the plan has another, worked out from raid 1 so it stays deterministic.
	const uint32_t seed = CurrentSetup().seed;
	const bool plainFirst = CurrentSetup().mode == GameMode::Campaign && CurrentLevelNumber() <= PlainFirstRaidLevels;
	RaidType type = RaidType::Mixed;
	for (uint32_t raid = 1; raid <= wave; raid++) {
		if (raid == 1 && plainFirst)
			continue; // it stays Mixed, and the next raid has to differ from that
		// The raid before the champions also keeps clear of their type, where the plan has a third to use.
		const bool beforeChampions = raid + 1 == static_cast<uint32_t>(NumWaves()) && plan.count > 2;
		size_t next = MixHash(seed, raid) % plan.count;
		while (plan.count > 1 && ((raid > 1 && plan.types[next] == type) || (beforeChampions && next == 0)))
			next = (next + 1) % plan.count;
		type = plan.types[next];
	}
	return type;
}

const RaidTypeInfo &CurrentRaidInfo()
{
	return RaidTypeInfoOf(RaidTypeForWave(State.wave));
}

HeroClassKind RaidHeroClass(uint8_t wave, size_t index)
{
	return RaidTypeInfoOf(RaidTypeForWave(wave)).pattern[index % RaidPatternLength];
}

int RaidFearPercent(int classPercent)
{
	return std::min(Percent(classPercent, CurrentRaidInfo().fearPercent), 100);
}

int RaidBaitSight(int classSight)
{
	return Percent(classSight, CurrentRaidInfo().baitSightPercent);
}

void ApplyRaidTypeToBody(HeroBody &body)
{
	const RaidTypeInfo &type = CurrentRaidInfo();
	body.life = std::max(Percent(body.life, type.lifePercent), 1);
	body.minDamage = std::min(Percent(body.minDamage, type.damagePercent), 255);
	body.maxDamage = std::min(Percent(body.maxDamage, type.damagePercent), 255);
	body.armor = std::min(Percent(body.armor, type.armorPercent), 255);
}

void ApplyRaidTypeToBot(Player &bot)
{
	const RaidTypeInfo &type = CurrentRaidInfo();
	bot._pMaxHPBase = Percent(bot._pMaxHPBase, type.lifePercent);
	bot._pHPBase = bot._pMaxHPBase;
	bot._pMaxHP = Percent(bot._pMaxHP, type.lifePercent);
	SetPlayerHitPoints(bot, bot._pMaxHP);
	bot._pIMinDam = Percent(bot._pIMinDam, type.damagePercent);
	bot._pIMaxDam = Percent(bot._pIMaxDam, type.damagePercent);
	bot._pDamageMod = Percent(bot._pDamageMod, type.damagePercent);
	bot._pMagic = Percent(bot._pMagic, type.damagePercent);
	bot._pIBonusAC += Percent(bot.GetArmor(), type.armorPercent - 100);
	bot._pFireResist = std::clamp<int>(bot._pFireResist + type.fireResist, 0, MaxResistance);
	bot._pLghtResist = std::clamp<int>(bot._pLghtResist + type.lightningResist, 0, MaxResistance);
	bot._pMagResist = std::clamp<int>(bot._pMagResist + type.magicResist, 0, MaxResistance);
}

int RaidWalkSkippedFrames()
{
	return CurrentRaidInfo().walksFast ? FastWalkSkippedFrames : 0;
}

} // namespace devilution::keeper
