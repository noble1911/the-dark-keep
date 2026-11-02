/**
 * @file keeper/keeper_roster.hpp
 *
 * Every monster the master can build: display data, unlock ranks, prices, and the "muster", the
 * part of the unlocked roster that is loaded for a level (a level only holds MaxLvlMTypes types).
 *
 * The muster is a pure function of the rank and the setup, so every client that agrees on those
 * (see keeper_campaign.hpp) loads the same types in the same order. Level type indexes go over the
 * network in CMD_SPAWNMONSTER, so this is required, not cosmetic.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace devilution::keeper {

/** Lair kinds in the roster table. The trap comes after them (see BuildKind). */
constexpr size_t NumLairKinds = 111;

/** One sprite set's colour variants, or a Hellfire line of monsters. Kinds of a family are its tiers. */
enum class KindFamily : uint8_t {
	Zombies,
	FallenSpears,
	FallenSwords,
	SkeletonAxes,
	SkeletonBows,
	SkeletonCaptains,
	Scavengers,
	Hidden,
	GoatMaces,
	GoatBows,
	Bats,
	AcidBeasts,
	Overlords,
	MagmaDemons,
	HornedDemons,
	StormDemons,
	Gargoyles,
	Slayers,
	Vipers,
	Knights,
	Succubi,
	Counselors,
	Hellboars,
	Scorpions,
	Orbs,
	Spiders,
	Felltwins,
	HorkSpawn,
	Lashworms,
	Torchants,
	Gravediggers,
	TombRats,
	Firebats,
	BoneDemons,
	Liches,
	CryptDemons,
	Biclops,
	FleshThings,
	Reapers,
};
constexpr size_t NumKindFamilies = static_cast<size_t>(KindFamily::Reapers) + 1;

/** Fills in the prices from the loaded monster data (called right after monstdat.tsv is loaded). */
void InitRoster();

/** The rank in force: the --keeper-rank test override if set, otherwise CampaignRank(). */
int RosterRank();

std::string_view KindFlavor(size_t kind);
KindFamily FamilyOfKind(size_t kind);
std::string_view FamilyName(KindFamily family);
int UnlockRankOf(size_t kind);
/** False for kinds this game's data can't provide (Hellfire monsters without Hellfire, the shareware game). */
bool IsKindOnRoster(size_t kind);
bool IsKindUnlocked(size_t kind);

/** Average hit points this kind spawns with (single player halves them, like the engine). */
int KindLife(size_t kind);
/** Lowest and highest damage of a melee blow. */
int KindMinDamage(size_t kind);
int KindMaxDamage(size_t kind);

/**
 * Picks what this level loads, from RosterRank() and CurrentSetup() only. Call once per level load,
 * before the kinds are added to the level's monster types. Later rank changes wait for the next level.
 */
void ChooseMuster();
/** Lair kinds loaded for this level, in table order. */
const std::vector<size_t> &MusterKinds();
/** True if the kind was loaded for this level and can be built. */
bool IsKindMustered(size_t kind);

/** Every kind the game's data provides, in unlock order: the Bestiary tab. */
const std::vector<size_t> &BestiaryKinds();

/** Names of the kinds unlocked by going from rank `fromRank` to `toRank`, comma separated (empty if none). */
std::string UnlocksBetween(int fromRank, int toRank);
/** Tells the master what a rank-up unlocked. Call on level load; only speaks when the rank rose. */
void AnnounceRankUnlocks();

} // namespace devilution::keeper
