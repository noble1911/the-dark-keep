/**
 * @file keeper/keeper_roster.cpp
 *
 * The roster table, the price formulas and the muster. See keeper_roster.hpp.
 */
#include "keeper/keeper_roster.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <iterator>
#include <optional>
#include <string>
#include <utility>

#include <fmt/format.h>

#include "engine/load_cl2.hpp"
#include "engine/load_file.hpp"
#include "engine/render/clx_render.hpp"
#include "game_mode.hpp"
#include "headless_mode.hpp"
#include "keeper/keeper.hpp"
#include "keeper/keeper_campaign.hpp"
#include "keeper/keeper_game.hpp"
#include "keeper/keeper_profile.hpp"
#include "monster.h"
#include "multi.h"
#include "plrmsg.h"
#include "tables/monstdat.h"
#include "utils/str_cat.hpp"

namespace devilution::keeper {

namespace {

// ---- balance -------------------------------------------------------------------
// The per-kind overrides are in the table below; everything else is decided here.

/** Monster level -> rank: level 1 is rank 1, level 30 is rank 20. */
constexpr int UnlockRankPerLevelNumerator = NumCampaignLevels - 1;
constexpr int UnlockRankPerLevelDenominator = 29;

/** Gold = base + power * multiplier, rounded to the step. Power is sqrt(toughness * punch). */
constexpr int KindCostBase = 25;
constexpr int KindCostPerPower = 9;
constexpr int KindCostRoundTo = 5;
/** A family's tier always costs at least this much more than the tier below it. */
constexpr int TierMinCostStep = 10;
/**
 * Pop = 1 + power / divisor, at most the maximum (which fits the first raid's Pop cap, BaseSupply). Above 1 Pop, power
 * per Pop sets what a full keep is worth: this divisor is what makes higher tiers beat a swarm of low ones.
 */
constexpr int KindPopPowerDivisor = 20;
constexpr int KindMaxPop = 14;
/** Worth more than their stats say, in percent of power. */
constexpr int RangedPowerPercent = 125;
constexpr int ChargerPowerPercent = 110;
constexpr int AmbusherPowerPercent = 110;

static_assert(NumLairKinds + 8 <= MaxLvlMTypes, "Every kind must fit in a level's type table, with the Golem, heroes and Keep Lord");
/**
 * Most sprite memory loaded kinds may take in monstdat's `image` units (the engine's own random levels
 * stop at 4000; the whole roster is about 117000), on the Switch and in any co-op game: a joiner may be
 * on a Switch, and it has to load whatever the host builds. Single player on a computer has no limit.
 */
constexpr int LimitedImageBudget = 24000;

// ---- the table -----------------------------------------------------------------

struct RosterEntry {
	_monster_id type;
	std::string_view name;
	KindFamily family;
	std::string_view flavor;
	/** Overrides for the formulas; 0 = use the formula. */
	int8_t unlockRank = 0;
	int16_t cost = 0;
	uint8_t pop = 0;
};

using enum KindFamily;

/**
 * Lair kinds, family by family, tiers ascending. Names are short enough for a card; monstdat's own
 * repeat across families ("Fallen One", "Skeleton", "Flesh Clan").
 *
 * Not here, because monstdat marks them as never spawning (availability Never: unused or unfinished
 * data, quest bosses): Wyrms, Red Death line, Incinerators (FireMan has no AI function), The Shredded
 * line, Invisible Lord, Satyr Lord, Devil Kin Brute, Skeleton King, Butcher, Diablo, Malignus, Golem,
 * Hork Demon, Hell Bug, Na-Krul.
 */
constexpr RosterEntry Roster[] = {
	{ MT_NZOMBIE, "Zombie", Zombies, "Slow, cheap shamblers that get in the heroes' way.", 1, 40, 1 },
	{ MT_BZOMBIE, "Ghoul", Zombies, "A hungrier zombie with a harder swing." },
	{ MT_GZOMBIE, "Carcass", Zombies, "Rotting, patient and hard to put down." },
	{ MT_YZOMBIE, "Black Death", Zombies, "The worst of the shamblers, with a crushing blow." },

	{ MT_RFALLSP, "Fallen", FallenSpears, "Cowardly little demons. Cheap enough to place in swarms.", 1, 35, 1 },
	{ MT_DFALLSP, "Carver", FallenSpears, "A bolder spear. Still runs when its friends die." },
	{ MT_YFALLSP, "Devil Kin", FallenSpears, "Rallies nearby Fallen with a shrill howl." },
	{ MT_BFALLSP, "Dark One", FallenSpears, "The spear pack's veterans. They heal as they howl." },

	{ MT_RFALLSD, "Fallen Blade", FallenSwords, "A Fallen with a sword and slightly more nerve.", 2 },
	{ MT_DFALLSD, "Carver Blade", FallenSwords, "Quick cuts, then a quick retreat." },
	{ MT_YFALLSD, "Kin Blade", FallenSwords, "Hits like a much larger demon. Scares like a small one." },
	{ MT_BFALLSD, "Dark Blade", FallenSwords, "The finest swords the Fallen ever stole." },

	{ MT_WSKELAX, "Bones", SkeletonAxes, "Skeleton axemen that hold the line.", 1, 30, 1 },
	{ MT_TSKELAX, "Corpse Axe", SkeletonAxes, "A sturdier skeleton with a heavier axe." },
	{ MT_RSKELAX, "Burning Axe", SkeletonAxes, "Burning dead: faster on its feet than it looks." },
	{ MT_XSKELAX, "Horror Axe", SkeletonAxes, "Black bones and a practised chop." },

	{ MT_WSKELBW, "Archer", SkeletonBows, "Skeleton archers that shoot from range.", 1, 60, 1 },
	{ MT_TSKELBW, "Corpse Bow", SkeletonBows, "Backs away from heroes and keeps shooting." },
	{ MT_RSKELBW, "Burning Bow", SkeletonBows, "A burning archer with a steady hand." },
	{ MT_XSKELBW, "Horror Bow", SkeletonBows, "The deadliest skeleton archer in any crypt." },

	{ MT_WSKELSD, "Captain", SkeletonCaptains, "A skeleton officer with a real sword." },
	{ MT_TSKELSD, "Corpse Capt", SkeletonCaptains, "Tougher than its axemen and knows it." },
	{ MT_RSKELSD, "Burning Capt", SkeletonCaptains, "Leads from the front, on fire." },
	{ MT_XSKELSD, "Horror Capt", SkeletonCaptains, "Hunts heroes around corners." },

	{ MT_NSCAV, "Scavenger", Scavengers, "Eats the fallen to heal itself when hurt." },
	{ MT_BSCAV, "Plague Eater", Scavengers, "Feeds on corpses and comes back for more." },
	{ MT_WSCAV, "Shadow Beast", Scavengers, "A big scavenger. Never leaves a corpse behind." },
	{ MT_YSCAV, "Bone Gasher", Scavengers, "Crunches bones, then whoever left them." },

	{ MT_SNEAK, "Hidden", Hidden, "Invisible until a hero is close. Heroes can't target it hidden." },
	{ MT_STALKER, "Stalker", Hidden, "An invisible ambusher with heavy claws.", 6, 100, 1 },
	{ MT_UNSEEN, "Unseen", Hidden, "Strikes from nowhere, then circles away.", 8, 140, 2 },
	{ MT_ILLWEAV, "Weaver", Hidden, "An illusion weaver: can't be hit while it retreats." },

	{ MT_NGOATMC, "Flesh Clan", GoatMaces, "Goat warriors that circle their prey." },
	{ MT_BGOATMC, "Stone Clan", GoatMaces, "Harder hide, harder mace." },
	{ MT_RGOATMC, "Fire Clan", GoatMaces, "Fierce fighters that lash out when wounded." },
	{ MT_GGOATMC, "Night Clan", GoatMaces, "The clan's elite. Hurting one makes it dangerous." },

	{ MT_NGOATBW, "Flesh Bow", GoatBows, "Goat archers that keep their distance." },
	{ MT_BGOATBW, "Stone Bow", GoatBows, "Steady shots from a tough hide." },
	{ MT_RGOATBW, "Fire Bow", GoatBows, "Backs away and never stops firing." },
	{ MT_GGOATBW, "Night Bow", GoatBows, "The clan's sharpest eyes." },

	{ MT_FIEND, "Fiend", Bats, "A fast, flapping nuisance: bites, then darts away." },
	{ MT_BLINK, "Blink", Bats, "Hit it hard and it blinks to its attacker's side." },
	{ MT_GLOOM, "Gloom", Bats, "Dives at heroes from a distance." },
	{ MT_FAMILIAR, "Familiar", Bats, "Its bite calls down a crackle of lightning." },

	{ MT_NACID, "Acid Beast", AcidBeasts, "Spits acid from range and leaves a burning pool when it dies." },
	{ MT_RACID, "Spitter", AcidBeasts, "Stronger venom, longer puddles." },
	{ MT_BACID, "Pit Beast", AcidBeasts, "A heavy brute that spits as hard as it bites." },
	{ MT_XACID, "Lava Maw", AcidBeasts, "Its death pool keeps burning long after it's gone." },

	{ MT_FAT, "Brute", Overlords, "A hulking brute that hits very hard.", 1, 220, 3 },
	{ MT_MUDMAN, "Mud Man", Overlords, "All bulk. It takes a party to bring one down." },
	{ MT_TOAD, "Toad Demon", Overlords, "A bloated horror with a savage second attack." },
	{ MT_FLAYED, "Flayed One", Overlords, "The largest of the brutes and the angriest." },

	{ MT_NMAGMA, "Magma", MagmaDemons, "Hurls balls of fire, and swings twice up close." },
	{ MT_YMAGMA, "Blood Stone", MagmaDemons, "Throws fire down long corridors." },
	{ MT_BMAGMA, "Hell Stone", MagmaDemons, "Immune to its own kind of fire. Heroes aren't." },
	{ MT_WMAGMA, "Lava Lord", MagmaDemons, "A burning lord of the deep caves." },

	{ MT_HORNED, "Horned", HornedDemons, "Charges from afar and hits like a battering ram." },
	{ MT_MUDRUN, "Mud Runner", HornedDemons, "A faster, meaner charge." },
	{ MT_FROSTC, "Frost Horn", HornedDemons, "Crosses a room in a heartbeat." },
	{ MT_OBLORD, "Obsidian", HornedDemons, "The hardest charge in the keep." },

	{ MT_STORM, "Red Storm", StormDemons, "Throws lightning that arcs along its path." },
	{ MT_RSTORM, "Storm Rider", StormDemons, "Quick to strike, near or far." },
	{ MT_STORML, "Storm Lord", StormDemons, "Lightning from afar, two blows up close." },
	{ MT_MAEL, "Maelstrom", StormDemons, "The storm made flesh." },

	{ MT_WINGED, "Winged", Gargoyles, "Retreats when hurt to turn to stone and heal." },
	{ MT_GARGOYLE, "Gargoyle", Gargoyles, "A patient stone hunter. Heals when left alone." },
	{ MT_BLOODCLW, "Blood Claw", Gargoyles, "Heavy claws, and it mends between fights." },
	{ MT_DEATHW, "Death Wing", Gargoyles, "Kills fast, heals faster." },

	{ MT_MEGA, "Slayer", Slayers, "A towering demon that breathes a wave of fire.", 14, 350, 3 },
	{ MT_GUARD, "Guardian", Slayers, "Fire close up, a great blade closer.", 15, 420, 3 },
	{ MT_VTEXLRD, "Vortex Lord", Slayers, "Its fire reaches further than heroes expect.", 16, 500, 4 },
	{ MT_BALROG, "Balrog", Slayers, "Very few heroes have seen one twice.", 18, 650, 4 },

	{ MT_NSNAKE, "Cave Viper", Vipers, "Fast and winding, with a lunge at close range." },
	{ MT_RSNAKE, "Fire Drake", Vipers, "A heavier lunge, and harder to pin down." },
	{ MT_BSNAKE, "Gold Viper", Vipers, "Its strikes rarely miss." },
	{ MT_GSNAKE, "Azure Drake", Vipers, "The great serpent of the deep halls." },

	{ MT_NBLACK, "Black Knight", Knights, "An armoured killer that hunts around corners." },
	{ MT_RTBLACK, "Doom Guard", Knights, "A knight sworn to a fallen lord." },
	{ MT_BTBLACK, "Steel Lord", Knights, "Armoured beyond most heroes' reach." },
	{ MT_RBLACK, "Blood Knight", Knights, "The most feared blade in the Burning Hells." },

	{ MT_SUCCUBUS, "Succubus", Succubi, "Fires blood stars and retreats from melee." },
	{ MT_SNOWWICH, "Snow Witch", Succubi, "Cold stars from a cold heart." },
	{ MT_HLSPWN, "Hell Spawn", Succubi, "Burns through a hero's armour from across a room." },
	{ MT_SOLBRNR, "Soul Burner", Succubi, "The deadliest caster of her kind." },

	{ MT_COUNSLR, "Counselor", Counselors, "Throws firebolts and fades away when threatened." },
	{ MT_MAGISTR, "Magistrate", Counselors, "Three bolts of lightning at a time." },
	{ MT_CABALIST, "Cabalist", Counselors, "Chain lightning from an untouchable sorcerer." },
	{ MT_ADVOCATE, "Advocate", Counselors, "Fireballs, and a flash of power for those too close." },

	{ MT_HELLBOAR, "Hellboar", Hellboars, "A hellish beast whose blows knock heroes back." },

	{ MT_STINGER, "Stinger", Scorpions, "Tiny, quick and full of venom." },
	{ MT_VENMTAIL, "Venomtail", Scorpions, "A bigger sting with far more poison in it." },

	{ MT_PSYCHORB, "Psychorb", Orbs, "A floating eye that fires bolts of mind." },
	{ MT_NECRMORB, "Necromorb", Orbs, "A fragile eye with a terrible stare." },

	{ MT_ARACHNON, "Arachnon", Spiders, "A spider that stalks heroes through the halls." },
	{ MT_SPIDLORD, "Spider Lord", Spiders, "Spits acid and leaves a burning pool when it dies." },

	{ MT_FELLTWIN, "Felltwin", Felltwins, "A shadowy duelist that seeks out intruders." },

	{ MT_HORKSPWN, "Hork Spawn", HorkSpawn, "A squirming spawn of the hive. Quick to die, quick to bite." },

	{ MT_LASHWORM, "Lashworm", Lashworms, "A grasping worm with a long, cruel reach." },

	{ MT_TORCHANT, "Torchant", Torchants, "A burning ant that spits fireballs." },

	{ MT_GRAVEDIG, "Gravedigger", Gravediggers, "Eats the dead to mend itself." },

	{ MT_TOMBRAT, "Tomb Rat", TombRats, "A fierce rat that seldom misses." },

	{ MT_FIREBAT, "Firebat", Firebats, "A flying menace that spits firebolts." },
	{ MT_HELLBAT, "Hellbat", Firebats, "Its fireballs burst among the party." },

	{ MT_SKLWING, "Skullwing", BoneDemons, "A winged skeleton with a vicious strike." },
	{ MT_BONEDEMN, "Bone Demon", BoneDemons, "Throws blue flame and dares heroes to come closer." },

	{ MT_LICH, "Lich", Liches, "A dead sorcerer who keeps his distance and his spells." },
	{ MT_ARCHLICH, "Arch Lich", Liches, "Its golden flares melt armour." },

	{ MT_CRYPTDMN, "Crypt Demon", CryptDemons, "A heavy guardian of the crypts." },

	{ MT_BICLOPS, "Biclops", Biclops, "Two heads, one club, and it knocks heroes flying." },

	{ MT_FLESTHNG, "Flesh Thing", FleshThings, "A mound of flesh that is very hard to kill." },

	{ MT_REAPER, "Reaper", Reapers, "The end of the road for any party." },
};
static_assert(std::size(Roster) == NumLairKinds, "NumLairKinds must match the roster table");
static_assert(NumLairKinds < 0xFF, "Kinds travel over the network as a byte");

constexpr BuildInfo MakeBuildInfo(size_t kind)
{
	// Prices are filled in by InitRoster once the monster data is loaded.
	return { Roster[kind].name, 0, Roster[kind].type, 1, 1 };
}

template <size_t... Kinds>
constexpr std::array<BuildInfo, NumBuildKinds> MakeBuildKinds(std::index_sequence<Kinds...> /*unused*/)
{
	return { MakeBuildInfo(Kinds)...,
		BuildInfo { TrapKinds[0].name, TrapKinds[0].cost, MT_INVALID, 0, 0 },
		BuildInfo { TrapKinds[1].name, TrapKinds[1].cost, MT_INVALID, 0, 0 },
		BuildInfo { TrapKinds[2].name, TrapKinds[2].cost, MT_INVALID, 0, 0 },
		BuildInfo { TrapKinds[3].name, TrapKinds[3].cost, MT_INVALID, 0, 0 } };
}

constexpr std::array<std::string_view, NumKindFamilies> FamilyNames {
	"Zombies", "Fallen spears", "Fallen swords", "Skeleton axemen", "Skeleton archers", "Skeleton captains", "Scavengers",
	"The Hidden", "Goat warriors", "Goat archers", "Bats", "Acid beasts", "Overlords", "Magma demons", "Horned demons",
	"Storm demons", "Gargoyles", "Slayers", "Vipers", "Knights", "Succubi", "Counselors", "Hellboars", "Scorpions",
	"Orbs", "Spiders", "Felltwins", "Hork spawn", "Lashworms", "Torchants", "Gravediggers", "Tomb rats", "Firebats",
	"Bone demons", "Liches", "Crypt demons", "Biclopes", "Flesh things", "Reapers"
};

std::vector<size_t> muster;
std::array<bool, NumLairKinds> mustered {};
/** The stand sprites last asked for: a kind's full graphics only load when one spawns. */
std::optional<OwnedClxSpriteListOrSheet> portrait;
size_t portraitKind = 0;
std::vector<size_t> bestiary;

int IntegerSquareRoot(int value)
{
	int root = 0;
	while ((root + 1) * (root + 1) <= value)
		root++;
	return root;
}

int PowerPercentFor(MonsterAIID ai)
{
	switch (ai) {
	case MonsterAIID::SkeletonRanged:
	case MonsterAIID::GoatRanged:
	case MonsterAIID::Succubus:
	case MonsterAIID::Acid:
	case MonsterAIID::Magma:
	case MonsterAIID::Storm:
	case MonsterAIID::Mega:
	case MonsterAIID::Counselor:
	case MonsterAIID::FireBat:
	case MonsterAIID::Torchant:
	case MonsterAIID::Lich:
	case MonsterAIID::ArchLich:
	case MonsterAIID::Psychorb:
	case MonsterAIID::Necromorb:
	case MonsterAIID::BoneDemon:
		return RangedPowerPercent;
	case MonsterAIID::Rhino:
	case MonsterAIID::Snake:
		return ChargerPowerPercent;
	case MonsterAIID::Sneak:
		return AmbusherPowerPercent;
	default:
		return 100;
	}
}

/**
 * A kind's worth, from its stats. Toughness is life made bigger by armour (hero missiles miss by
 * armour); punch is damage times the chance to hit (melee between monsters rolls only that).
 */
int KindPower(const MonsterData &data)
{
	const int life = (data.hitPointsMinimum + data.hitPointsMaximum) / 2;
	const int toughness = life * (100 + data.armorClass) / 100;
	const int damage = (data.minDamage + data.maxDamage) / 2;
	const int punch = damage * std::min<int>(data.toHit, 100) / 100;
	return IntegerSquareRoot(std::max(toughness * punch, 1)) * PowerPercentFor(data.ai) / 100;
}

int FormulaCost(const MonsterData &data)
{
	const int cost = KindCostBase + KindPower(data) * KindCostPerPower;
	return (cost + KindCostRoundTo / 2) / KindCostRoundTo * KindCostRoundTo;
}

uint8_t FormulaPop(const MonsterData &data)
{
	return static_cast<uint8_t>(std::min(1 + KindPower(data) / KindPopPowerDivisor, KindMaxPop));
}

int FormulaUnlockRank(const MonsterData &data)
{
	const int rank = 1 + (data.level * UnlockRankPerLevelNumerator - 1) / UnlockRankPerLevelDenominator;
	return std::clamp(rank, 1, NumCampaignLevels);
}

} // namespace

std::array<BuildInfo, NumBuildKinds> BuildKinds = MakeBuildKinds(std::make_index_sequence<NumLairKinds> {});

void InitRoster()
{
	for (size_t kind = 0; kind < NumLairKinds; kind++) {
		const RosterEntry &entry = Roster[kind];
		if (static_cast<size_t>(entry.type) >= MonstersData.size())
			continue;
		const MonsterData &data = MonstersData[entry.type];
		BuildInfo &info = BuildKinds[kind];
		info.cost = entry.cost != 0 ? entry.cost : FormulaCost(data);
		info.supply = entry.pop != 0 ? entry.pop : FormulaPop(data);
		// The starters' hand-set prices sit above the formula: a family's next tier must not undercut them.
		if (kind > 0 && Roster[kind - 1].family == entry.family) {
			if (entry.cost == 0)
				info.cost = std::max(info.cost, BuildKinds[kind - 1].cost + TierMinCostStep);
			if (entry.pop == 0)
				info.supply = std::max(info.supply, BuildKinds[kind - 1].supply);
		}
	}

	bestiary.clear();
	for (size_t kind = 0; kind < NumLairKinds; kind++) {
		if (IsKindOnRoster(kind))
			bestiary.push_back(kind);
	}
	std::stable_sort(bestiary.begin(), bestiary.end(), [](size_t a, size_t b) { return UnlockRankOf(a) < UnlockRankOf(b); });
}

std::string_view KindFlavor(size_t kind)
{
	return kind < NumLairKinds ? Roster[kind].flavor : TrapKinds[kind - NumLairKinds].flavor;
}

KindFamily FamilyOfKind(size_t kind)
{
	return Roster[std::min(kind, NumLairKinds - 1)].family;
}

std::string_view FamilyName(KindFamily family)
{
	return FamilyNames[static_cast<size_t>(family)];
}

DungeonGroup GroupOfFamily(KindFamily family)
{
	// Dungeon levels 1-4 are the Cathedral, then four each of the Catacombs, Caves and Hell; Hellfire adds the Hive (17-20) and Crypt (21-24).
	int lowest = 99;
	for (const RosterEntry &entry : Roster) {
		if (entry.family == family && static_cast<size_t>(entry.type) < MonstersData.size() && !MonstersData[entry.type].name.empty())
			lowest = std::min<int>(lowest, MonstersData[entry.type].minDunLvl);
	}
	if (lowest <= 4)
		return DungeonGroup::Cathedral;
	if (lowest <= 8)
		return DungeonGroup::Catacombs;
	if (lowest <= 12)
		return DungeonGroup::Caves;
	if (lowest <= 16)
		return DungeonGroup::Hell;
	return lowest <= 20 ? DungeonGroup::Hive : DungeonGroup::Crypt;
}

std::string_view GroupName(DungeonGroup group)
{
	constexpr std::array<std::string_view, NumDungeonGroups> names { "Cathedral", "Catacombs", "Caves", "Hell", "Hive", "Crypt" };
	return names[static_cast<size_t>(group)];
}

int UnlockRankOf(size_t kind)
{
	if (kind >= NumLairKinds)
		return 1;
	const RosterEntry &entry = Roster[kind];
	if (entry.unlockRank != 0)
		return entry.unlockRank;
	return static_cast<size_t>(entry.type) < MonstersData.size() ? FormulaUnlockRank(MonstersData[entry.type]) : NumCampaignLevels;
}

bool IsKindOnRoster(size_t kind)
{
	if (kind >= NumLairKinds)
		return true;
	const _monster_id type = Roster[kind].type;
	if (static_cast<size_t>(type) >= MonstersData.size())
		return false;
	const MonsterData &data = MonstersData[type];
	// Hellfire's slots stay empty without Hellfire's data; the shareware game has the Always monsters only.
	if (data.name.empty() || data.availability == MonsterAvailability::Never)
		return false;
	return !gbIsSpawn || data.availability != MonsterAvailability::Retail;
}

bool IsKindUnlocked(size_t kind)
{
	return IsKindOnRoster(kind) && IsKindAvailable(kind);
}

int KindLife(size_t kind)
{
	if (kind >= NumLairKinds)
		return 0;
	const MonsterData &data = MonstersData[Roster[kind].type];
	const int life = (data.hitPointsMinimum + data.hitPointsMaximum) / 2;
	return gbIsMultiplayer ? life : std::max(life / 2, 1);
}

int KindMinDamage(size_t kind)
{
	return kind < NumLairKinds ? MonstersData[Roster[kind].type].minDamage : 0;
}

int KindMaxDamage(size_t kind)
{
	return kind < NumLairKinds ? MonstersData[Roster[kind].type].maxDamage : 0;
}

void ChooseMuster()
{
	muster.clear();
	mustered.fill(false);
	for (size_t kind = 0; kind < NumLairKinds; kind++) {
		if (IsKindUnlocked(kind)) {
			muster.push_back(kind);
			mustered[kind] = true;
		}
	}
}

bool HasRoomToLoadKind(size_t kind)
{
#ifndef __SWITCH__
	if (!gbIsMultiplayer)
		return true;
#endif
	if (kind >= NumLairKinds)
		return true;
	int image = 0;
	for (size_t i = 0; i < LevelMonsterTypeCount; i++) {
		const CMonster &monsterType = LevelMonsterTypes[i];
		if (monsterType.animData == nullptr)
			continue;
		if (monsterType.type == Roster[kind].type)
			return true;
		image += MonstersData[monsterType.type].image;
	}
	return image + MonstersData[Roster[kind].type].image <= LimitedImageBudget;
}

OptionalClxSpriteList KindPortrait(size_t kind)
{
	if (kind >= NumLairKinds || HeadlessMode)
		return std::nullopt;
	if (!portrait || portraitKind != kind) {
		const MonsterData &data = MonstersData[Roster[kind].type];
		const std::string path = StrCat("monsters\\", data.spritePath(), "n");
		portrait.emplace(LoadCl2ListOrSheet(path.c_str(), PointerOrValue<uint16_t> { data.width }));
		portraitKind = kind;
		if (!data.trnFile.empty()) {
			const std::string trnPath = StrCat("monsters\\", data.trnFile, ".trn");
			std::array<uint8_t, 256> translation;
			LoadFileInMem(trnPath.c_str(), translation);
			std::replace(translation.begin(), translation.end(), static_cast<uint8_t>(255), static_cast<uint8_t>(0));
			const ClxSpriteListOrSheet sprites { *portrait };
			if (sprites.isSheet())
				ClxApplyTrans(ClxSpriteSheet { sprites.sheet() }, translation.data());
			else
				ClxApplyTrans(ClxSpriteList { sprites.list() }, translation.data());
		}
	}
	const ClxSpriteListOrSheet sprites { *portrait };
	return sprites.isSheet() ? sprites.sheet()[0] : sprites.list();
}

KindImage MakeKindImage(size_t kind, int maxWidth, int maxHeight)
{
	const OptionalClxSpriteList sprites = KindPortrait(kind);
	if (!sprites || sprites->numSprites() == 0) {
		KindImage image;
		image.built = true;
		return image;
	}
	return MakeSpriteImage((*sprites)[0], maxWidth, maxHeight);
}

KindImage MakeSpriteImage(ClxSprite sprite, int maxWidth, int maxHeight)
{
	KindImage image;
	image.built = true;
	const int width = sprite.width();
	const int height = sprite.height();
	OwnedSurface canvas(width, height);
	for (int y = 0; y < height; y++)
		std::memset(canvas.at(0, y), 0, width);
	ClxDraw(canvas, { 0, height }, sprite);

	int left = width;
	int right = -1;
	int top = height;
	int bottom = -1;
	for (int y = 0; y < height; y++) {
		const uint8_t *row = canvas.at(0, y);
		for (int x = 0; x < width; x++) {
			if (row[x] == 0)
				continue;
			left = std::min(left, x);
			right = std::max(right, x);
			top = std::min(top, y);
			bottom = std::max(bottom, y);
		}
	}
	if (right < 0)
		return image;
	const int bodyWidth = right - left + 1;
	const int bodyHeight = bottom - top + 1;
	// Whole-number maths: a fraction `scaleTop / scaleBottom`, never enlarging.
	int scaleTop = 1;
	int scaleBottom = 1;
	if (bodyWidth * maxHeight > bodyHeight * maxWidth) {
		if (bodyWidth > maxWidth) {
			scaleTop = maxWidth;
			scaleBottom = bodyWidth;
		}
	} else if (bodyHeight > maxHeight) {
		scaleTop = maxHeight;
		scaleBottom = bodyHeight;
	}
	image.width = std::max(bodyWidth * scaleTop / scaleBottom, 1);
	image.height = std::max(bodyHeight * scaleTop / scaleBottom, 1);
	image.pixels.resize(static_cast<size_t>(image.width) * image.height);
	for (int y = 0; y < image.height; y++) {
		const uint8_t *row = canvas.at(left, top + y * scaleBottom / scaleTop);
		for (int x = 0; x < image.width; x++)
			image.pixels[static_cast<size_t>(y) * image.width + x] = row[x * scaleBottom / scaleTop];
	}
	return image;
}

void DrawKindImage(const Surface &area, const KindImage &image)
{
	const int offsetX = (area.w() - image.width) / 2;
	const int offsetY = area.h() - image.height;
	if (offsetX < 0 || offsetY < 0)
		return;
	for (int y = 0; y < image.height; y++) {
		uint8_t *out = area.at(offsetX, offsetY + y);
		const uint8_t *row = &image.pixels[static_cast<size_t>(y) * image.width];
		for (int x = 0; x < image.width; x++) {
			if (row[x] != 0)
				out[x] = row[x];
		}
	}
}

const std::vector<size_t> &MusterKinds()
{
	return muster;
}

bool IsKindMustered(size_t kind)
{
	return kind < NumLairKinds && mustered[kind];
}

const std::vector<size_t> &BestiaryKinds()
{
	return bestiary;
}

std::optional<size_t> PreviousTier(size_t kind)
{
	if (kind == 0 || kind >= NumLairKinds || Roster[kind - 1].family != Roster[kind].family)
		return std::nullopt;
	return kind - 1;
}

} // namespace devilution::keeper
