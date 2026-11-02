/**
 * @file keeper/keeper_raidtypes.hpp
 *
 * Kinds of raid, so that campaign levels differ in kind and not only in numbers. A raid's type comes from the
 * level's plan, the setup's seed and the raid's number alone, so every client works out the same one and nothing
 * needs sending. The types are data here; heroes and bots only have small hooks that read them.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "keeper/keeper_heroes.hpp"

namespace devilution {

struct Player;

namespace keeper {

/** Add new types at the end: test flags and the level plans refer to them. */
enum class RaidType : uint8_t {
	Mixed,
	Thieves,
	Coven,
	FireWarded,
	IronColumn,
	LightRaiders,
};
constexpr size_t NumRaidTypes = 6;
/** A party is dealt its classes from this pattern, over and over. The first heroes dealt are the full heroes. */
constexpr size_t RaidPatternLength = 6;

struct RaidTypeInfo {
	std::string_view name;
	/** What it means for the master, on the scout card. */
	std::string_view blurb;
	std::array<HeroClassKind, RaidPatternLength> pattern;
	/** Applied to the heroes' strength for the level and the raid. */
	int lifePercent;
	int damagePercent;
	int armorPercent;
	/** The monster's resistance flags for lesser heroes... */
	uint8_t resistances;
	/** ...and the same in percent for full heroes (the engine's cap applies). */
	int fireResist;
	int lightningResist;
	int magicResist;
	/** Of the class's chance to run when badly hurt. */
	int fearPercent;
	/** Of how far away the class notices bait. */
	int baitSightPercent;
	bool walksFast;
};

const RaidTypeInfo &RaidTypeInfoOf(RaidType type);
/** What raid `wave` of the game being played is. */
RaidType RaidTypeForWave(uint8_t wave);
/** The type of the current (or next, while preparing) raid. */
const RaidTypeInfo &CurrentRaidInfo();

/** The class of the `index`th hero of raid `wave`. */
HeroClassKind RaidHeroClass(uint8_t wave, size_t index);
int RaidFearPercent(int classPercent);
int RaidBaitSight(int classSight);

/** A lesser hero's body and resistances for this raid. */
void ApplyRaidTypeToBody(HeroBody &body);
/** After everything else that sets up a full hero: the engine works these out again whenever the gear changes. */
void ApplyRaidTypeToBot(Player &bot);
/** Frames of a step that a hero of this raid skips. */
int RaidWalkSkippedFrames();

} // namespace keeper

} // namespace devilution
