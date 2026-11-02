/**
 * @file keeper/keeper_roster.hpp
 *
 * Every monster the master can build: display data, unlock ranks, prices, and the "muster", the
 * unlocked kinds listed in a level's monster types. Their graphics only load when one first spawns.
 *
 * The muster is a pure function of the unlocks, so every client that agrees on them (see keeper_profile.hpp)
 * lists the same types in the same order. Level type indexes go over the network in CMD_SPAWNMONSTER,
 * so this is required, not cosmetic.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/clx_sprite.hpp"
#include "engine/surface.hpp"

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
 * Lists what this level offers, from the game's unlocks only. Call once per level load, before the kinds are
 * added to the level's monster types. Later rank changes wait for the next level.
 */
void ChooseMuster();
/** Lair kinds this level offers, in table order. */
const std::vector<size_t> &MusterKinds();
/** True if the kind can be built on this level. */
bool IsKindMustered(size_t kind);
/** Memory limit on small devices: false if loading this kind too would go over it. */
bool HasRoomToLoadKind(size_t kind);
/** A monster trimmed and shrunk to fit a box, kept so its sprites needn't stay in memory. */
struct KindImage {
	bool built = false;
	int width = 0;
	int height = 0;
	std::vector<uint8_t> pixels;
};
/** Trims away the empty space of a monster's frame and shrinks what is left, keeping its proportions. */
KindImage MakeKindImage(size_t kind, int maxWidth, int maxHeight);
/** Draws it at the bottom middle of `area`, whose size should be what it was made for. */
void DrawKindImage(const Surface &area, const KindImage &image);
/** The kind's standing sprites facing south. Only the last kind asked for stays loaded. */
OptionalClxSpriteList KindPortrait(size_t kind);

/** Every kind the game's data provides, in unlock order: the Bestiary tab. */
const std::vector<size_t> &BestiaryKinds();

/** The tier below this one in its family, which has to be unlocked first. */
std::optional<size_t> PreviousTier(size_t kind);

} // namespace devilution::keeper
