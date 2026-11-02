/**
 * @file keeper/keeper_heroes.cpp
 *
 * AI heroes for The Dark Keep. See keeper_heroes.hpp.
 */
#include "keeper/keeper_heroes.hpp"

#include <algorithm>
#include <array>
#include <functional>
#include <optional>
#include <queue>
#include <vector>

#include <fmt/format.h>

#include "engine/direction.hpp"
#include "engine/load_cl2.hpp"
#include "engine/path.h"
#include "engine/random.hpp"
#include "keeper/keeper.hpp"
#include "keeper/keeper_bots.hpp"
#include "keeper/keeper_game.hpp"
#include "keeper/keeper_raid.hpp"
#include "keeper/keeper_upgrades.hpp"
#include "levels/gendung.h"
#include "levels/tile_properties.hpp"
#include "missiles.h"
#include "monster.h"
#include "objects.h"
#include "player.h"
#include "plrmsg.h"
#include "tables/playerdat.hpp"
#include "utils/str_cat.hpp"

namespace devilution {

// Exported from monster.cpp for the keeper's hero AI.
void KeeperStartMeleeAttack(Monster &monster);
void KeeperStartRangedAttack(Monster &monster, MissileID missileType, int dam);
bool KeeperStartOwnRangedAttack(Monster &monster);
void KeeperAiDelay(Monster &monster, int len);

namespace keeper {

namespace {

struct HeroClassInfo {
	std::string_view name;
	/** Player class sprite folder + 3-letter prefix (class, armour, weapon). */
	std::string_view classPath;
	std::string_view prefix;
	std::string_view deathPrefix;
	std::string_view attackAnim;
	uint16_t attackWidth;
	int8_t attackActionFrame;
	std::string_view soundSuffix;
	int carryCap;
};

constexpr std::array<HeroClassInfo, NumHeroClasses> HeroClasses { {
	{ "Warrior", "warrior", "wmd", "wmn", "at", 128, 9, "fallen\\fallen", 120 },
	{ "Rogue", "rogue", "rlb", "rln", "at", 128, 11, "fallen\\fallen", 200 },
	{ "Sorcerer", "sorceror", "slt", "sln", "fm", 128, 14, "fallen\\fallen", 100 },
} };

/**
 * A monster hero's stats at 100% strength. How strong a level's heroes are belongs to CampaignLevels: long
 * levels already reach higher raid numbers, so growth per raid is kept gentle or the two multiply up.
 */
struct HeroStatLine {
	int lifeBase;
	int lifePerRaid;
	int minDamageBase;
	int maxDamageBase;
	int armorBase;
	int armorPerRaid;
};
constexpr std::array<HeroStatLine, NumHeroClasses> HeroStats { {
	{ 33, 7, 3, 7, 11, 2 }, // Warrior
	{ 22, 5, 2, 5, 5, 2 },  // Rogue
	{ 17, 4, 3, 6, 3, 1 },  // Sorcerer
} };
constexpr int HeroMinDamagePerTenRaids = 6;
constexpr int HeroMaxDamagePerTenRaids = 12;
/** Gold for killing a monster hero. Heroes who run and get away pay nothing, which is a good third of them. */
constexpr int HeroBountyBase = 30;
constexpr int HeroBountyPerWave = 8;
/**
 * The Keep Lord at 100% strength. Parties gain damage faster than strength, as they grow, so his life also
 * grows per campaign level: a few heroes leaking past the army must not end the keep at once.
 */
constexpr int LordLifeBase = 600;
constexpr int LordLifePerLevel = 75;
constexpr int LordArmorBase = 40;

std::array<_monster_id, NumHeroClasses> HeroMonsterIds {};
bool heroDataRegistered = false;
/** The Keep Lord: a Black Knight under its own name (shares the knight's sprites). */
_monster_id BossMonsterId = MT_INVALID;
constexpr _monster_id BossBaseType = MT_NBLACK;

/** The heroes' way out once the Lord is down. Simulated on every client, like the hero AI. */
struct PortalState {
	/** Monster id of a hero monster channelling the portal (-1 = none). */
	int caster = -1;
	/** Player id of a bot channelling the portal (-1 = none). */
	int botCaster = -1;
	int channel = 0;
	/** A bot finished channelling and is casting the real Town Portal spell. */
	bool awaitingBotCast = false;
	int castTimeout = 0;
	bool open = false;
	Point position;
};
PortalState Portal;
constexpr int PortalChannelTicks = 8 * 20;

/** Owned sprite sheets per class: Stand, Walk, Attack, Hit, Death. Loaded once, reused across games. */
std::array<std::array<std::optional<OwnedClxSpriteListOrSheet>, 5>, NumHeroClasses> HeroSprites;

constexpr int HeroSight = 6;
constexpr int GuardRadius = 8;
/** A keep monster goes after a hero it can see this far from its post, then gives up and walks back. */
constexpr int KeepMonsterChase = 12;
constexpr int SpawnIntervalTicks = 15;

/** Trap and bait masks hold one bit per slot. */
static_assert(MaxTraps <= 16);
static_assert(MaxBaits <= 8);

// Hero AI tuning. Route penalties are in "steps a hero would rather walk instead".
/** Per danger tile: crossing a trap's 3 tile zone is worth a detour of about 3x this. */
constexpr uint16_t TrapRoutePenalty = 20;
/** Tiles from a trap that set it off (matches UpdateTraps). */
constexpr int TrapDangerRadius = 1;
/** How far away a hero with a clear view notices a trap going off. */
constexpr int TrapWitnessRange = 8;
constexpr int RogueDisarmPercent = 50;
constexpr int RogueSpotRange = 3;
constexpr int WarriorBashBlows = 3;
constexpr int OtherBashBlows = 1;
/** How far each class notices fool's gold. Sorcerers aren't fooled by it at all. */
constexpr int WarriorBaitSight = 8;
constexpr int RogueBaitSight = 10;
constexpr int SorcererBaitSight = 0;
constexpr int FearHealthDivisor = 4;
constexpr int FearChancePercent = 60;
/** Most steps a hero spends falling back, so it can't run from a fight for good. */
constexpr int FallBackMaxSteps = 12;
/** Waiting for the party: a hero this many tiles ahead of the rearguard holds, in pauses of this length. */
constexpr int RegroupLeadTiles = 10;
constexpr int RegroupWaitTicks = 10;

struct HeroState {
	bool active = false;
	HeroClassKind cls = HeroClassKind::Warrior;
	int carried = 0;
	bool champion = false;
	bool fleeing = false;
	bool fearRolled = false;
	uint8_t baitsVisited = 0;
	/** Traps a Rogue has rolled to disarm. */
	uint16_t trapsSeen = 0;
	/** Traps this hero saw go off or spotted: its route avoids them. */
	uint16_t trapsKnown = 0;
	bool fellBack = false;
	int fallBackSteps = 0;
	int regroupBudget = RegroupBudgetTicks;
};

std::array<HeroState, MaxMonsters> Heroes;
std::array<Point, MaxMonsters> Homes;
std::array<bool, MaxMonsters> Roaming {};

std::vector<HeroClassKind> pendingParty;
int spawnCooldown = 0;

DistanceField ToVault;
DistanceField ToEntrance;
DistanceField ToPortal;

std::optional<HeroClassKind> HeroClassOf(_monster_id type)
{
	for (size_t i = 0; i < NumHeroClasses; i++) {
		if (heroDataRegistered && HeroMonsterIds[i] == type)
			return static_cast<HeroClassKind>(i);
	}
	return std::nullopt;
}

} // namespace

bool IsPassable(Point tile)
{
	return InDungeonBounds(tile) && IsTileWalkable(tile, /*ignoreDoors=*/true);
}

bool CanStepDiagonal(Point from, Displacement step)
{
	if (step.deltaX == 0 || step.deltaY == 0)
		return true;
	return !IsTileSolid(from + Displacement { step.deltaX, 0 }) && !IsTileSolid(from + Displacement { 0, step.deltaY });
}

void BuildFieldAvoiding(DistanceField &field, Point target, tl::function_ref<bool(Point)> isShut)
{
	for (auto &column : field)
		column.fill(Unreachable);
	if (!InDungeonBounds(target))
		return;
	std::queue<Point> open;
	field[target.x][target.y] = 0;
	open.push(target);
	while (!open.empty()) {
		const Point tile = open.front();
		open.pop();
		const uint16_t next = field[tile.x][tile.y] + 1;
		for (const Direction dir : { Direction::South, Direction::SouthWest, Direction::West, Direction::NorthWest, Direction::North, Direction::NorthEast, Direction::East, Direction::SouthEast }) {
			const Displacement step(dir);
			const Point neighbour = tile + step;
			if (!IsPassable(neighbour) || field[neighbour.x][neighbour.y] != Unreachable || !CanStepDiagonal(tile, step) || isShut(neighbour))
				continue;
			field[neighbour.x][neighbour.y] = next;
			open.push(neighbour);
		}
	}
}

void BuildField(DistanceField &field, Point target)
{
	BuildFieldAvoiding(field, target, [](Point) { return false; });
}

bool IsOpenFloor(Point tile)
{
	return InDungeonBounds(tile) && IsTileWalkable(tile) && !IsObjectAtPosition(tile);
}

namespace {

uint16_t FieldAt(const DistanceField &field, Point tile)
{
	if (!InDungeonBounds(tile))
		return Unreachable;
	return field[tile.x][tile.y];
}

/**
 * A route field: like the plain fields, but each tile also costs its penalty, so the lowest neighbour
 * is always the cheapest way on. It depends only on the goal, the known traps and the locks, so every
 * client computes the same one, and heroes with the same knowledge share it.
 */
struct RouteEntry {
	bool valid = false;
	HeroGoal goal = HeroGoal::Vault;
	uint16_t traps = 0;
	/** Where those traps stand: a slot can be sold and reused. */
	uint32_t trapSites = 0;
	uint32_t lastUsed = 0;
	DistanceField field;
};
/** Each hero uses one route at a time, so this holds a whole party whatever each of them knows. */
constexpr size_t RouteCacheSize = 12;
std::array<RouteEntry, RouteCacheSize> Routes;
uint32_t routeClock = 0;

void InvalidateRoutes()
{
	for (RouteEntry &entry : Routes)
		entry.valid = false;
}

uint32_t TrapSitesOf(uint16_t traps)
{
	uint32_t sites = 0;
	for (size_t i = 0; i < MaxTraps; i++) {
		if ((traps & (1U << i)) != 0)
			sites = sites * 31 + static_cast<uint32_t>(State.traps[i].position.x * 256 + State.traps[i].position.y + 1);
	}
	return sites;
}

Point GoalTile(HeroGoal goal)
{
	switch (goal) {
	case HeroGoal::Vault:
		return State.vault;
	case HeroGoal::Entrance:
		return State.entrance;
	case HeroGoal::Portal:
		break;
	}
	return Portal.open ? Portal.position : Point { -1, -1 };
}

void BuildRoute(DistanceField &field, Point target, uint16_t traps)
{
	static DistanceField tileCost;
	for (auto &column : tileCost)
		column.fill(1);
	for (const DoorLock &lock : State.locks) {
		if (lock.active && InDungeonBounds(lock.position))
			tileCost[lock.position.x][lock.position.y] += LockRoutePenalty;
	}
	for (size_t i = 0; i < MaxTraps; i++) {
		if ((traps & (1U << i)) == 0)
			continue;
		const Point trap = State.traps[i].position;
		for (int dx = -TrapDangerRadius; dx <= TrapDangerRadius; dx++) {
			for (int dy = -TrapDangerRadius; dy <= TrapDangerRadius; dy++) {
				const Point tile = trap + Displacement { dx, dy };
				if (InDungeonBounds(tile))
					tileCost[tile.x][tile.y] += TrapRoutePenalty;
			}
		}
	}

	for (auto &column : field)
		column.fill(Unreachable);
	if (!InDungeonBounds(target))
		return;
	// Dijkstra over (distance << 16 | x << 8 | y).
	std::priority_queue<uint32_t, std::vector<uint32_t>, std::greater<>> open;
	field[target.x][target.y] = 0;
	open.push(static_cast<uint32_t>(target.x) << 8 | static_cast<uint32_t>(target.y));
	while (!open.empty()) {
		const uint32_t top = open.top();
		open.pop();
		const Point tile { static_cast<int>((top >> 8) & 0xFF), static_cast<int>(top & 0xFF) };
		const uint32_t distance = top >> 16;
		if (distance != field[tile.x][tile.y])
			continue;
		for (const Direction dir : { Direction::South, Direction::SouthWest, Direction::West, Direction::NorthWest, Direction::North, Direction::NorthEast, Direction::East, Direction::SouthEast }) {
			const Displacement step(dir);
			const Point neighbour = tile + step;
			if (!IsPassable(neighbour) || !CanStepDiagonal(tile, step))
				continue;
			const uint32_t next = distance + tileCost[neighbour.x][neighbour.y];
			if (next >= field[neighbour.x][neighbour.y])
				continue;
			field[neighbour.x][neighbour.y] = static_cast<uint16_t>(next);
			open.push(next << 16 | static_cast<uint32_t>(neighbour.x) << 8 | static_cast<uint32_t>(neighbour.y));
		}
	}
}

/** The route to `goal` for a hero who knows `knownTraps`. Valid until the next call that builds a route. */
const DistanceField &RouteField(HeroGoal goal, uint16_t knownTraps)
{
	uint16_t traps = 0;
	for (size_t i = 0; i < MaxTraps; i++) {
		if ((knownTraps & (1U << i)) != 0 && State.traps[i].active)
			traps |= static_cast<uint16_t>(1U << i);
	}
	const uint32_t sites = TrapSitesOf(traps);
	routeClock++;
	RouteEntry *slot = &Routes[0];
	for (RouteEntry &entry : Routes) {
		if (entry.valid && entry.goal == goal && entry.traps == traps && entry.trapSites == sites) {
			entry.lastUsed = routeClock;
			return entry.field;
		}
		if (!entry.valid || (slot->valid && entry.lastUsed < slot->lastUsed))
			slot = &entry;
	}
	BuildRoute(slot->field, GoalTile(goal), traps);
	slot->valid = true;
	slot->goal = goal;
	slot->traps = traps;
	slot->trapSites = sites;
	slot->lastUsed = routeClock;
	return slot->field;
}

bool IsLockedDoorAt(Point tile)
{
	Object *door = FindObjectAtPosition(tile);
	return door != nullptr && door->isDoor() && KeeperIsDoorClosed(*door) && FindLock(door->position) != nullptr;
}

/** A locked door next to `from` that the route says to go through: no open neighbour is any cheaper. */
std::optional<Point> LockedDoorOnRoute(const DistanceField &field, Point from)
{
	std::optional<Point> door;
	uint16_t doorValue = FieldAt(field, from);
	uint16_t openValue = Unreachable;
	for (const Direction dir : { Direction::NorthEast, Direction::SouthWest, Direction::North, Direction::East, Direction::South, Direction::West, Direction::NorthWest, Direction::SouthEast }) {
		const Point tile = from + dir;
		const uint16_t value = FieldAt(field, tile);
		if (value == Unreachable || !CanStepDiagonal(from, Displacement(dir)))
			continue;
		if (!IsLockedDoorAt(tile)) {
			openValue = std::min(openValue, value);
		} else if (value < doorValue) {
			door = tile;
			doorValue = value;
		}
	}
	if (door && doorValue <= openValue)
		return door;
	return std::nullopt;
}

bool IsAliveMonster(const Monster &monster)
{
	return monster.hitPoints > 0 && monster.mode != MonsterMode::Death && !monster.isInvalid && monster.position.tile != GolemHoldingCell;
}

/** Heroes aren't stupid: they won't step into a wall of fire (unless they're already in one). */
bool IsSafeStep(const Monster &monster, Direction dir)
{
	return !IsBurning(monster.position.tile + dir) || IsBurning(monster.position.tile);
}

/** Walk one step in dir, or the nearest turn that is open. */
bool StepTowards(Monster &monster, Direction dir)
{
	for (const Direction tryDir : { dir, Left(dir), Right(dir) }) {
		if (DirOK(monster, tryDir) && IsSafeStep(monster, tryDir)) {
			Walk(monster, tryDir);
			return true;
		}
	}
	return false;
}

/** Standing in fire: step out to the nearest tile that isn't burning. */
bool StepOutOfFire(Monster &monster)
{
	if (!IsBurning(monster.position.tile))
		return false;
	for (const Direction dir : { Direction::South, Direction::SouthWest, Direction::West, Direction::NorthWest, Direction::North, Direction::NorthEast, Direction::East, Direction::SouthEast }) {
		if (!IsBurning(monster.position.tile + dir) && DirOK(monster, dir)) {
			Walk(monster, dir);
			return true;
		}
	}
	return false;
}

bool FollowField(Monster &monster, const DistanceField &field)
{
	const Point position = monster.position.tile;
	const uint16_t here = FieldAt(field, position);
	std::optional<Direction> best;
	uint16_t bestValue = here;
	for (const Direction dir : { Direction::South, Direction::SouthWest, Direction::West, Direction::NorthWest, Direction::North, Direction::NorthEast, Direction::East, Direction::SouthEast }) {
		const uint16_t value = FieldAt(field, position + dir);
		if (value < bestValue && DirOK(monster, dir) && IsSafeStep(monster, dir)) {
			best = dir;
			bestValue = value;
		}
	}
	if (!best) {
		// Path blocked (by a monster, usually): sidestep to an equally good tile, or wait.
		for (const Direction dir : { Direction::South, Direction::SouthWest, Direction::West, Direction::NorthWest, Direction::North, Direction::NorthEast, Direction::East, Direction::SouthEast }) {
			if (FieldAt(field, position + dir) == here && DirOK(monster, dir) && IsSafeStep(monster, dir)) {
				best = dir;
				break;
			}
		}
	}
	if (!best)
		return false;
	Walk(monster, *best);
	return true;
}

void ApplyHeroStats(Monster &monster, HeroClassKind cls, bool champion)
{
	const int wave = std::max<int>(State.wave, 1);
	const HeroStatLine &stats = HeroStats[static_cast<size_t>(cls)];
	int hp = stats.lifeBase + stats.lifePerRaid * wave;
	monster.minDamage = static_cast<uint8_t>(stats.minDamageBase + HeroMinDamagePerTenRaids * wave / 10);
	monster.maxDamage = static_cast<uint8_t>(stats.maxDamageBase + HeroMaxDamagePerTenRaids * wave / 10);
	monster.armorClass = static_cast<uint8_t>(stats.armorBase + stats.armorPerRaid * wave);
	if (champion)
		hp = hp * 3 / 2;
	hp = ScaleByHeroStrength(hp);
	monster.minDamage = static_cast<uint8_t>(std::min(ScaleByHeroStrength(monster.minDamage), 255));
	monster.maxDamage = static_cast<uint8_t>(std::min(ScaleByHeroStrength(monster.maxDamage), 255));
	monster.armorClass = static_cast<uint8_t>(std::min(ScaleArmorByHeroStrength(monster.armorClass), 255));
	monster.maxHitPoints = hp << 6;
	monster.hitPoints = monster.maxHitPoints;
	monster.flags |= MFLAG_CAN_OPEN_DOOR;
}

void Announce(std::string_view message, UiFlags color = UiFlags::ColorRed)
{
	EventPlrMsg(message, color);
}

} // namespace

std::vector<HeroClassKind> PartyForWave(uint8_t wave)
{
	// How many comes from the level. Classes alternate, so bots and monster heroes both get a mix.
	std::vector<HeroClassKind> party;
	const int size = PartySizeForWave(wave);
	for (int i = 0; i < size; i++)
		party.push_back(static_cast<HeroClassKind>(i % NumHeroClasses));
	return party;
}

namespace {

std::optional<size_t> FindLevelMonsterType(_monster_id type)
{
	for (size_t i = 0; i < LevelMonsterTypeCount; i++) {
		if (LevelMonsterTypes[i].type == type)
			return i;
	}
	return std::nullopt;
}

bool IsFreeFloor(Point tile)
{
	return IsOpenFloor(tile) && dMonster[tile.x][tile.y] == 0 && dPlayer[tile.x][tile.y] == 0;
}

std::string_view ClassName(const HeroState &hero)
{
	return HeroClasses[static_cast<size_t>(hero.cls)].name;
}

void HeroEscapes(Monster &monster, HeroState &hero)
{
	NoteHeroLeft(hero.fleeing && !State.bossDown, std::max(hero.carried, 0));
	if (hero.carried > 0) {
		State.goldLost += hero.carried;
		Announce(fmt::format("A {} escaped with {} gold!", ClassName(hero), hero.carried));
	} else if (hero.fleeing) {
		Announce(fmt::format("A terrified {} fled the keep.", ClassName(hero)));
	}
	hero = {};
	M_ClearSquares(monster);
	monster.hitPoints = 0;
	monster.isInvalid = true;
}

void HeroSteals(Monster &monster, HeroState &hero)
{
	const int amount = std::min(ScaleByHeroStrength(HeroClasses[static_cast<size_t>(hero.cls)].carryCap), State.vaultGold);
	if (amount <= 0)
		return;
	State.vaultGold -= amount;
	hero.carried = amount;
	NoteGoldLooted(amount);
	Announce(fmt::format("A {} looted {} gold from the vault!", ClassName(hero), amount));
	KeeperAiDelay(monster, 10);
}

/** Opens closed doors next to the hero, except the ones the masters locked. Runs on every client. */
void HeroOpenDoors(const Monster &monster)
{
	for (const Direction dir : { Direction::NorthEast, Direction::SouthWest, Direction::North, Direction::East, Direction::South, Direction::West, Direction::NorthWest, Direction::SouthEast }) {
		Object *door = FindObjectAtPosition(monster.position.tile + dir);
		if (door == nullptr || !door->isDoor() || !KeeperIsDoorClosed(*door) || FindLock(door->position) != nullptr)
			continue;
		KeeperOperateDoor(*door, false);
	}
}

/** If the route goes through a locked door, hammer at it. */
bool TryBashLockedDoor(Monster &monster, HeroState &hero, const DistanceField &field)
{
	const std::optional<Point> door = LockedDoorOnRoute(field, monster.position.tile);
	if (!door)
		return false;
	monster.enemyPosition = *door;
	monster.flags |= MFLAG_NO_ENEMY; // swing at the door, not at a monster
	KeeperStartMeleeAttack(monster);
	StrikeLockedDoor(*door, BashBlows(hero.cls), fmt::format("A {}", ClassName(hero)));
	return true;
}

/** Greedy heroes detour to treasure piles they can see. @return true if the hero acted. */
bool TryGoForBait(Monster &monster, HeroState &hero)
{
	if (hero.carried != 0 || hero.fleeing)
		return false;
	const Point position = monster.position.tile;
	const std::optional<size_t> slot = FindBaitFor(hero.cls, position, hero.baitsVisited);
	if (!slot)
		return false;
	const Point bait = State.baits[*slot].position;
	if (position.WalkingDistance(bait) <= 1) {
		ConsumeBait(*slot, hero.baitsVisited, fmt::format("A {}", ClassName(hero)));
		KeeperAiDelay(monster, BaitLingerTicks);
		return true;
	}
	if (StepTowards(monster, GetDirection(position, bait)))
		return true;
	hero.baitsVisited |= static_cast<uint8_t>(1U << *slot); // can't reach it: give up on this pile
	return false;
}

/** Badly hurt heroes may lose their nerve (once): they drop any loot and run. With no friend near, they always do. */
void CheckFear(const Monster &monster, HeroState &hero)
{
	if (hero.champion || hero.fleeing || hero.fearRolled || monster.hitPoints * FearHealthDivisor >= monster.maxHitPoints)
		return;
	hero.fearRolled = true;
	if (NearestHealthyHero(monster.position.tile, FallBackAllyRange) && GenerateRnd(100) >= FearChancePercent)
		return;
	hero.fleeing = true;
	if (hero.carried > 0) {
		AddVaultGold(hero.carried);
		NoteGoldRecovered(hero.carried);
		Announce(fmt::format("A {} drops {} gold and flees in terror!", ClassName(hero), hero.carried), UiFlags::ColorGold);
		hero.carried = 0;
	} else {
		Announce(fmt::format("A {} flees in terror!", ClassName(hero)), UiFlags::ColorGold);
	}
}

/** Rogues spot traps near them and may disarm them (one roll per trap). */
void RogueCheckTraps(const Monster &monster, HeroState &hero)
{
	if (hero.cls != HeroClassKind::Rogue)
		return;
	for (size_t i = 0; i < MaxTraps; i++) {
		Trap &trap = State.traps[i];
		if (!trap.active || (hero.trapsSeen & (1U << i)) != 0 || monster.position.tile.WalkingDistance(trap.position) > RogueSpotRange)
			continue;
		hero.trapsSeen |= static_cast<uint16_t>(1U << i);
		hero.trapsKnown |= static_cast<uint16_t>(1U << i);
		if (GenerateRnd(100) < RogueDisarmPercent) {
			trap.active = false;
			Announce(fmt::format("A Rogue disarmed one of your {}s!", TrapKinds[trap.kind].name));
		}
	}
}

} // namespace

void StepHome(Monster &monster, Point home);

namespace {

/** A wounded hero in a fight breaks off, once, and runs to a healthy friend. @return true if it moved. */
bool TryFallBack(Monster &monster, HeroState &hero)
{
	if (hero.champion || hero.fleeing || hero.carried != 0)
		return false;
	if (hero.fallBackSteps == 0) {
		if (hero.fellBack || (monster.flags & MFLAG_NO_ENEMY) != 0 || monster.hitPoints * 100 >= monster.maxHitPoints * FallBackHealthPercent)
			return false;
		hero.fellBack = true;
		hero.fallBackSteps = FallBackMaxSteps;
	}
	const std::optional<Point> ally = NearestHealthyHero(monster.position.tile, FallBackAllyRange);
	if (!ally || monster.position.tile.WalkingDistance(*ally) <= FallBackArriveDistance) {
		hero.fallBackSteps = 0;
		return false;
	}
	hero.fallBackSteps--;
	StepHome(monster, *ally);
	return true;
}

} // namespace

void RegisterHeroMonsterData()
{
	for (size_t i = 0; i < NumHeroClasses; i++) {
		const HeroClassInfo &info = HeroClasses[i];
		MonsterData data;
		data.name = std::string(info.name);
		data.soundSuffix = std::string(info.soundSuffix);
		data.availability = MonsterAvailability::Never;
		data.width = 128;
		data.image = 0;
		// Stand, Walk, Attack, Hit, Death, Special. Real counts come from the sprites (InitHeroGFX).
		const int8_t frames[6] = { 10, 8, 16, 6, 20, 0 };
		const int8_t rates[6] = { 3, 1, 1, 1, 2, 1 };
		std::copy(std::begin(frames), std::end(frames), data.frames);
		std::copy(std::begin(rates), std::end(rates), data.rate);
		data.minDunLvl = 1;
		data.maxDunLvl = 1;
		data.level = 5;
		data.hitPointsMinimum = 40;
		data.hitPointsMaximum = 40;
		data.ai = MonsterAIID::KeeperHero;
		data.abilityFlags = MFLAG_CAN_OPEN_DOOR;
		data.intelligence = 2;
		data.toHit = 60;
		data.animFrameNum = info.attackActionFrame;
		data.minDamage = 3;
		data.maxDamage = 8;
		data.armorClass = 15;
		data.monsterClass = MonsterClass::Animal;
		data.selectionRegion = SelectionRegion::Bottom | SelectionRegion::Middle;
		data.exp = 0;
		HeroMonsterIds[i] = AddCustomMonsterData(std::move(data), StrCat("keeper\\", info.name));
	}

	MonsterData boss = MonstersData[BossBaseType];
	const std::string bossSprites = boss.spritePath();
	boss.name = "The Keep Lord";
	boss.availability = MonsterAvailability::Never;
	BossMonsterId = AddCustomMonsterData(std::move(boss), bossSprites);

	heroDataRegistered = true;
	InitRoster();
}

bool IsHeroType(_monster_id type)
{
	return HeroClassOf(type).has_value();
}

bool IsHero(const Monster &monster)
{
	return IsHeroType(monster.type().type);
}

bool InitHeroGFX(CMonster &monsterType)
{
	const std::optional<HeroClassKind> cls = HeroClassOf(monsterType.type);
	if (!cls)
		return false;
	const size_t c = static_cast<size_t>(*cls);
	const HeroClassInfo &info = HeroClasses[c];

	struct AnimSource {
		std::string_view prefix;
		std::string_view anim;
		uint16_t width;
	};
	const std::array<AnimSource, 5> sources { {
	    { info.prefix, "as", 96 },
	    { info.prefix, "aw", 96 },
	    { info.prefix, info.attackAnim, info.attackWidth },
	    { info.prefix, "ht", 96 },
	    { info.deathPrefix, "dt", 128 },
	} };

	for (size_t i = 0; i < sources.size(); i++) {
		if (!HeroSprites[c][i]) {
			const std::string path = StrCat("plrgfx\\", info.classPath, "\\", sources[i].prefix, "\\", sources[i].prefix, sources[i].anim);
			HeroSprites[c][i].emplace(LoadCl2Sheet(path.c_str(), sources[i].width));
		}
		AnimStruct &anim = monsterType.anims[i];
		anim.sprites = ClxSpriteListOrSheet { *HeroSprites[c][i] };
		anim.width = sources[i].width;
		anim.frames = static_cast<int8_t>((*HeroSprites[c][i]).sheet()[0].numSprites());
	}
	monsterType.anims[5].sprites = std::nullopt;
	return true;
}

namespace {

/** Level monster types from here on are lair kinds, loaded on demand. */
size_t firstDeferredType = SIZE_MAX;

} // namespace

void AddHeroMonsterTypes()
{
	// The raid can't run without these, so they go first and are always loaded.
	for (const _monster_id type : HeroMonsterIds)
		(void)AddMonsterType(type, PLACE_SPECIAL);
	(void)AddMonsterType(BossMonsterId, PLACE_SPECIAL);
	ChooseMuster();
	firstDeferredType = LevelMonsterTypeCount;
	for (const size_t kind : MusterKinds())
		(void)AddMonsterType(BuildKinds[kind].type, PLACE_SPECIAL);
}

bool IsDeferredMonsterType(size_t typeIndex)
{
	return typeIndex >= firstDeferredType;
}

void ClearDeferredMonsterTypes()
{
	firstDeferredType = SIZE_MAX;
}

void OnMonsterInit(Monster &monster)
{
	const size_t id = monster.getId();
	if (id >= MaxMonsters)
		return;
	Homes[id] = monster.position.tile;
	Roaming[id] = false;
	// The engine wakes a monster when a player sees it. There are no players to see it here, and a
	// monster that picks a hero as its first enemy would otherwise stay asleep until hit.
	if (currlevel == KeepLevel && !setlevel) {
		monster.activeForTicks = UINT8_MAX;
		if (monster.ai == MonsterAIID::Gargoyle) {
			// The engine starts these as stone statues until a player comes near. Lairs want them awake.
			monster.flags &= ~MFLAG_ALLOW_SPECIAL;
			monster.mode = MonsterMode::Stand;
			monster.changeAnimationData(MonsterGraphic::Stand);
		}
	}
	if (IsBoss(monster)) {
		Heroes[id] = {};
		monster.maxHitPoints = (TestLordHitPoints > 0 ? TestLordHitPoints : ScaleByHeroStrength(LordLifeBase + LordLifePerLevel * (CurrentLevelNumber() - 1))) << 6;
		monster.hitPoints = monster.maxHitPoints;
		monster.minDamage = static_cast<uint8_t>(std::min(ScaleByHeroStrength(12), 255));
		monster.maxDamage = static_cast<uint8_t>(std::min(ScaleByHeroStrength(24), 255));
		monster.armorClass = static_cast<uint8_t>(std::min(ScaleByHeroStrength(LordArmorBase), 255));
		OnKeepMonsterInit(monster);
		return;
	}
	const std::optional<HeroClassKind> cls = HeroClassOf(monster.type().type);
	if (!cls) {
		Heroes[id] = {};
		OnKeepMonsterInit(monster);
		return;
	}
	HeroState &hero = Heroes[id];
	hero = {};
	hero.active = true;
	hero.cls = *cls;
	hero.champion = State.wave >= NumWaves();
	ApplyHeroStats(monster, *cls, hero.champion);
}

void OnMonsterDeath(Monster &monster)
{
	const size_t id = monster.getId();
	for (size_t pid = 0; pid < State.possessed.size(); pid++) {
		if (State.possessed[pid] == static_cast<int16_t>(id))
			EndPossession(pid);
	}
	if (!IsHero(monster) && monster.type().type != MT_GOLEM)
		NoteKeepMonsterLost();
	if (IsBoss(monster)) {
		if (State.phase == Phase::Raid && !State.bossDown) {
			State.bossDown = true;
			Announce("The Keep Lord has fallen! Stop the heroes before they portal out!");
		}
		return;
	}
	if (id >= MaxMonsters || !Heroes[id].active)
		return;
	HeroState &hero = Heroes[id];
	const int bounty = ScaleByHeroStrength(HeroBountyBase + HeroBountyPerWave * State.wave);
	AddVaultGold(std::max(hero.carried, 0) + bounty);
	NoteHeroSlain(bounty, std::max(hero.carried, 0));
	if (hero.carried > 0)
		Announce(fmt::format("Recovered {} stolen gold!", hero.carried), UiFlags::ColorGold);
	hero = {};
}

void UpdateEnemy(Monster &monster)
{
	if (IsPossessed(monster))
		return; // the master picks its targets
	const bool hero = IsHero(monster);
	const Point position = monster.position.tile;
	const Point anchor = hero || Roaming[monster.getId()] ? position : Homes[monster.getId()];
	const int range = hero ? HeroSight : GuardRadius;

	// The Lord holds its ground near the vault: its death can lose the map, so it can't be lured off.
	const int chaseRange = IsBoss(monster) ? range : KeepMonsterChase;
	// Keep monsters answer to anything near their post, and anything they can actually see. What's in
	// sight comes first: otherwise the nearest hero behind a wall hides the one in the open.
	const auto canSee = [&](Point tile, int distance) {
		return distance <= 1 || LineClearMissile(position, tile);
	};
	const auto isBetter = [](bool seen, int distance, bool bestSeen, int bestDistance) {
		return (seen && !bestSeen) || (seen == bestSeen && distance < bestDistance);
	};

	int best = -1;
	int bestDistance = 0;
	bool bestSeen = false;
	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		const unsigned otherId = ActiveMonsters[i];
		const Monster &other = Monsters[otherId];
		if (&other == &monster || !IsAliveMonster(other) || !AreHostile(monster, other))
			continue;
		const Point tile = other.position.tile;
		const int distance = tile.WalkingDistance(position);
		const bool nearPost = tile.WalkingDistance(anchor) <= range;
		if (hero && !nearPost)
			continue;
		if (hero && (other.flags & MFLAG_HIDDEN) != 0)
			continue; // invisible ambushers (and fading Counselors) can't be seen
		if (!nearPost && tile.WalkingDistance(anchor) > chaseRange)
			continue;
		const bool seen = canSee(tile, distance);
		if ((hero || !nearPost) && !seen)
			continue;
		if (best == -1 || isBetter(seen, distance, bestSeen, bestDistance)) {
			best = static_cast<int>(otherId);
			bestDistance = distance;
			bestSeen = seen;
		}
	}

	if (!hero) {
		int bestBot = -1;
		int bestBotDistance = 0;
		bool bestBotSeen = false;
		for (size_t i = 0; i < MaxBots; i++) {
			const Player &bot = Players[FirstBotId + i];
			if (!IsLivingBot(bot))
				continue;
			const Point tile = bot.position.tile;
			const int distance = tile.WalkingDistance(position);
			const bool nearPost = tile.WalkingDistance(anchor) <= range;
			if (!nearPost && tile.WalkingDistance(anchor) > chaseRange)
				continue;
			const bool seen = canSee(tile, distance);
			if (!nearPost && !seen)
				continue;
			if (bestBot == -1 || isBetter(seen, distance, bestBotSeen, bestBotDistance)) {
				bestBot = static_cast<int>(FirstBotId + i);
				bestBotDistance = distance;
				bestBotSeen = seen;
			}
		}
		if (bestBot != -1 && (best == -1 || isBetter(bestBotSeen, bestBotDistance, bestSeen, bestDistance))) {
			const Player &bot = Players[bestBot];
			monster.flags &= ~(MFLAG_NO_ENEMY | MFLAG_TARGETS_MONSTER);
			monster.enemy = static_cast<uint8_t>(bestBot);
			monster.enemyPosition = bot.position.future;
			monster.position.last = monster.enemyPosition;
			return;
		}
	}

	if (best == -1) {
		monster.flags |= MFLAG_NO_ENEMY;
		return;
	}
	monster.flags &= ~MFLAG_NO_ENEMY;
	monster.flags |= MFLAG_TARGETS_MONSTER;
	monster.enemy = static_cast<uint8_t>(best);
	monster.enemyPosition = Monsters[best].position.future;
	monster.position.last = monster.enemyPosition;
}

void HeroAi(Monster &monster)
{
	if (monster.mode != MonsterMode::Stand)
		return;
	const size_t id = monster.getId();
	HeroState &hero = Heroes[id];
	if (!hero.active)
		return;

	HeroOpenDoors(monster);
	CheckFear(monster, hero);
	RogueCheckTraps(monster, hero);
	if (StepOutOfFire(monster))
		return;

	// The Lord is dead: the party's only goal is to get out through the portal.
	if (State.bossDown) {
		if (Portal.open) {
			if (FieldAt(ToPortal, monster.position.tile) <= 1) {
				State.portalEscape = true;
				Announce(fmt::format("A {} escaped through the portal!", ClassName(hero)));
				hero.carried = std::max(hero.carried, 0);
				HeroEscapes(monster, hero);
				return;
			}
			const DistanceField &route = RouteField(HeroGoal::Portal, hero.trapsKnown);
			if (TryBashLockedDoor(monster, hero, route))
				return;
			if (!FollowField(monster, route))
				KeeperAiDelay(monster, 5);
			return;
		}
		if (Portal.caster == static_cast<int>(id)) {
			// Channelling: stand still (the attack swing doubles as the casting gesture).
			monster.enemyPosition = monster.position.tile + monster.direction;
			KeeperAiDelay(monster, 10);
			return;
		}
	}

	if (!State.bossDown && TryFallBack(monster, hero))
		return;

	if (!hero.fleeing && (monster.flags & MFLAG_NO_ENEMY) == 0 && (monster.flags & MFLAG_TARGETS_MONSTER) != 0) {
		const Monster &enemy = Monsters[monster.enemy];
		if (IsAliveMonster(enemy)) {
			monster.enemyPosition = enemy.position.tile;
			const int distance = monster.position.tile.WalkingDistance(enemy.position.tile);
			switch (hero.cls) {
			case HeroClassKind::Warrior:
				if (distance < 2) {
					KeeperStartMeleeAttack(monster);
					return;
				}
				if (StepTowards(monster, GetDirection(monster.position.tile, enemy.position.tile)))
					return;
				break;
			case HeroClassKind::Rogue:
			case HeroClassKind::Sorcerer:
				if (distance <= HeroSight && LineClearMissile(monster.position.tile, enemy.position.tile)) {
					KeeperStartRangedAttack(monster, hero.cls == HeroClassKind::Rogue ? MissileID::Arrow : MissileID::Firebolt, 4);
					return;
				}
				if (StepTowards(monster, GetDirection(monster.position.tile, enemy.position.tile)))
					return;
				break;
			}
		}
	}

	// No fight while the portal is being opened: guard the caster.
	if (State.bossDown) {
		const std::optional<Point> caster = PortalCasterPosition();
		if (caster && monster.position.tile.WalkingDistance(*caster) > 2)
			StepTowards(monster, GetDirection(monster.position.tile, *caster));
		else
			KeeperAiDelay(monster, 5);
		return;
	}

	// No fight: go about the heist.
	if (hero.carried == 0 && !hero.fleeing) {
		if (TryGoForBait(monster, hero))
			return;
		if (FieldAt(ToVault, monster.position.tile) <= 1) {
			if (State.vaultGold > 0) {
				HeroSteals(monster, hero);
			} else {
				// Nothing left to steal: head home empty-handed.
				hero.carried = -1;
			}
			return;
		}
		if (hero.regroupBudget > 0 && IsAheadOfParty(monster.position.tile)) {
			hero.regroupBudget -= RegroupWaitTicks;
			KeeperAiDelay(monster, RegroupWaitTicks);
			return;
		}
		const DistanceField &route = RouteField(HeroGoal::Vault, hero.trapsKnown);
		if (TryBashLockedDoor(monster, hero, route))
			return;
		if (!FollowField(monster, route))
			KeeperAiDelay(monster, 5);
		return;
	}

	if (FieldAt(ToEntrance, monster.position.tile) <= 1) {
		HeroEscapes(monster, hero);
		return;
	}
	const DistanceField &route = RouteField(HeroGoal::Entrance, hero.trapsKnown);
	if (TryBashLockedDoor(monster, hero, route))
		return;
	if (!FollowField(monster, route))
		KeeperAiDelay(monster, 5);
}

namespace {

struct Order {
	bool hasDestination = false;
	Point destination;
	int target = -1;
};
std::array<Order, 4> Orders;
std::array<DistanceField, 4> OrderFields;

std::optional<size_t> PossessorOf(const Monster &monster)
{
	for (size_t pid = 0; pid < State.possessed.size(); pid++) {
		if (State.possessed[pid] == static_cast<int16_t>(monster.getId()))
			return pid;
	}
	return std::nullopt;
}

} // namespace

bool CanPossess(const Monster &monster)
{
	return IsAliveMonster(monster) && !IsHero(monster) && monster.type().type != MT_GOLEM && !IsPossessed(monster);
}

bool IsPossessed(const Monster &monster)
{
	return PossessorOf(monster).has_value();
}

void SetOrder(size_t playerId, Point tile, int targetMonster)
{
	if (playerId >= Orders.size() || State.possessed[playerId] < 0)
		return;
	Order &order = Orders[playerId];
	order.target = (targetMonster >= 0 && static_cast<size_t>(targetMonster) < MaxMonsters) ? targetMonster : -1;
	order.hasDestination = order.target < 0 && InDungeonBounds(tile);
	order.destination = tile;
	if (order.hasDestination)
		BuildField(OrderFields[playerId], tile);
}

void ClearOrder(size_t playerId)
{
	if (playerId < Orders.size())
		Orders[playerId] = {};
}

void EndPossession(size_t playerId)
{
	if (playerId >= State.possessed.size() || State.possessed[playerId] < 0)
		return;
	Monster &monster = Monsters[State.possessed[playerId]];
	State.possessed[playerId] = -1;
	ClearOrder(playerId);
	// Back to its lair duties: forget the master's target, guard from where it stands.
	monster.flags |= MFLAG_NO_ENEMY;
	if (monster.getId() < MaxMonsters)
		Homes[monster.getId()] = monster.position.tile;
}

void PossessedAi(Monster &monster)
{
	if (monster.mode != MonsterMode::Stand)
		return;
	const std::optional<size_t> pid = PossessorOf(monster);
	if (!pid)
		return;
	Order &order = Orders[*pid];
	const Point position = monster.position.tile;

	if (order.target >= 0) {
		const Monster &target = Monsters[order.target];
		if (!IsAliveMonster(target) || !AreHostile(monster, target)) {
			order.target = -1;
		} else {
			monster.enemy = static_cast<uint8_t>(order.target);
			monster.flags &= ~MFLAG_NO_ENEMY;
			monster.flags |= MFLAG_TARGETS_MONSTER;
			monster.enemyPosition = target.position.tile;
			const int distance = position.WalkingDistance(target.position.tile);
			if (distance >= 2 && distance <= 7 && LineClearMissile(position, target.position.tile) && KeeperStartOwnRangedAttack(monster))
				return;
			if (distance < 2) {
				KeeperStartMeleeAttack(monster);
				return;
			}
			StepTowards(monster, GetDirection(position, target.position.tile));
			return;
		}
	}

	if (order.hasDestination) {
		if (FieldAt(OrderFields[*pid], position) == 0 || !FollowField(monster, OrderFields[*pid]))
			order.hasDestination = false;
	}
}

std::string DescribeWave(uint8_t wave)
{
	const std::vector<HeroClassKind> party = PartyForWave(wave);
	std::array<int, NumHeroClasses> counts {};
	for (const HeroClassKind cls : party)
		counts[static_cast<size_t>(cls)]++;
	std::string text = wave >= NumWaves() ? "Champions: " : "Next raid: ";
	bool first = true;
	for (size_t i = 0; i < NumHeroClasses; i++) {
		if (counts[i] == 0)
			continue;
		text += fmt::format("{}{} {}{}", first ? "" : ", ", counts[i], HeroClasses[i].name, counts[i] > 1 ? "s" : "");
		first = false;
	}
	return text;
}

/** One step back towards the monster's post: a real path when it is far (rally points can be across the keep). */
void StepHome(Monster &monster, Point home)
{
	const Point position = monster.position.tile;
	if (position.WalkingDistance(home) > 2) {
		int8_t path[MaxPathLengthPlayer];
		/** Maps a path step to the direction to walk. */
		constexpr Direction StepDirections[9] = { Direction::South, Direction::NorthEast, Direction::NorthWest, Direction::SouthEast, Direction::SouthWest, Direction::North, Direction::East, Direction::South, Direction::West };
		const int steps = FindPath(CanStep, [](Point tile) { return IsPassable(tile); }, position, home, path, MaxPathLengthPlayer);
		if (steps > 0 && path[0] > 0 && path[0] < 9) {
			StepTowards(monster, StepDirections[path[0]]);
			return;
		}
	}
	StepTowards(monster, GetDirection(position, home));
}

void LeashMonsters()
{
	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		Monster &monster = Monsters[ActiveMonsters[i]];
		if (!IsAliveMonster(monster) || IsHero(monster) || IsPossessed(monster) || monster.mode != MonsterMode::Stand)
			continue;
		if ((monster.flags & MFLAG_NO_ENEMY) == 0)
			continue;
		const Point home = Homes[monster.getId()];
		if (monster.position.tile.WalkingDistance(home) <= 3)
			continue;
		StepHome(monster, home);
	}
}

void SetMonsterHome(size_t monsterId, Point tile)
{
	if (monsterId < MaxMonsters)
		Homes[monsterId] = tile;
}

void RefreshMonsterAfterSync(Monster &monster)
{
	// ResetGame forgot the heroes loaded with the level, and the wave they scale with wasn't known yet.
	// Everyone else keeps their stats: running their init again would upgrade them twice.
	if (HeroClassOf(monster.type().type))
		OnMonsterInit(monster);
	else
		SetMonsterHome(monster.getId(), monster.position.tile);
}

void SetMonsterRoaming(size_t monsterId, bool roaming)
{
	if (monsterId < MaxMonsters)
		Roaming[monsterId] = roaming;
}

bool ShouldGuard(const Monster &monster)
{
	return !IsHero(monster) && (monster.flags & MFLAG_NO_ENEMY) != 0 && monster.type().type != MT_GOLEM;
}

bool HuntsEnemy(const Monster &monster)
{
	return IsActive() && !IsHero(monster) && !IsBoss(monster) && !IsPossessed(monster) && monster.type().type != MT_GOLEM && (monster.flags & MFLAG_NO_ENEMY) == 0;
}

bool HoldsPost(const Monster &monster)
{
	return IsActive() && (IsPossessed(monster) || ShouldGuard(monster));
}

void GuardAi(Monster &monster)
{
	if (monster.mode != MonsterMode::Stand)
		return;
	const Point home = Homes[monster.getId()];
	if (monster.position.tile.WalkingDistance(home) > 1)
		StepHome(monster, home);
}

void ResetHeroes()
{
	Heroes.fill({});
	Portal = {};
	RemoveAllBots();
	pendingParty.clear();
	spawnCooldown = 0;
	RebuildPaths();
}

void RebuildPaths()
{
	BuildField(ToVault, State.vault);
	BuildField(ToEntrance, State.entrance);
	InvalidateRoutes();
}

void StartWave(uint8_t wave)
{
	pendingParty = PartyForWave(wave);
	spawnCooldown = 0;
}

RaidStatus UpdateRaid()
{
	if (!pendingParty.empty()) {
		if (spawnCooldown > 0) {
			spawnCooldown--;
		} else if (ActiveMonsterCount < MaxMonsters) {
			const HeroClassKind cls = pendingParty.front();
			const std::optional<size_t> typeIndex = FindLevelMonsterType(HeroMonsterIds[static_cast<size_t>(cls)]);
			const std::optional<Point> tile = FindClosestValidPosition(IsFreeFloor, State.entrance, 0, HeroSpawnRadius);
			if (tile) {
				if (const std::optional<size_t> slot = FreeBotSlot(); slot) {
					RequestSpawnBot(*slot, cls, *tile);
					pendingParty.erase(pendingParty.begin());
				} else if (typeIndex) {
					SpawnMonster(*tile, Direction::SouthEast, *typeIndex);
					pendingParty.erase(pendingParty.begin());
				}
			}
			spawnCooldown = SpawnIntervalTicks;
		}
		return RaidStatus::Ongoing;
	}
	return LivingHeroCount() == 0 ? RaidStatus::Repelled : RaidStatus::Ongoing;
}

bool AllThievesGone()
{
	return GoldInFlight() == 0;
}

int GoldInFlight()
{
	int total = 0;
	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		const unsigned id = ActiveMonsters[i];
		if (Heroes[id].active && Heroes[id].carried > 0 && IsAliveMonster(Monsters[id]))
			total += Heroes[id].carried;
	}
	return total + BotGoldInFlight();
}

int LivingHeroCount()
{
	int count = 0;
	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		const Monster &monster = Monsters[ActiveMonsters[i]];
		if (IsHero(monster) && IsAliveMonster(monster))
			count++;
	}
	return count + LivingBotCount();
}

bool IsBoss(const Monster &monster)
{
	return heroDataRegistered && monster.type().type == BossMonsterId;
}

const Monster *FindBoss()
{
	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		const Monster &monster = Monsters[ActiveMonsters[i]];
		if (IsBoss(monster) && IsAliveMonster(monster))
			return &monster;
	}
	return nullptr;
}

void UpdateBoss()
{
	if (State.bossDown || State.phase == Phase::Victory || State.phase == Phase::Defeat)
		return;
	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		const Monster &monster = Monsters[ActiveMonsters[i]];
		if (IsBoss(monster) && !monster.isInvalid && monster.hitPoints > 0)
			return; // alive (or still dying while bossDown propagates)
	}
	if (ActiveMonsterCount >= MaxMonsters)
		return;
	const std::optional<size_t> typeIndex = FindLevelMonsterType(BossMonsterId);
	const std::optional<Point> tile = FindClosestValidPosition(IsFreeFloor, State.vault, 1, 4);
	if (!typeIndex || !tile)
		return;
	SpawnMonster(*tile, Direction::South, *typeIndex);
}

namespace {

void OpenPortalAt(Point position)
{
	Portal.position = position;
	Portal.open = true;
	Portal.caster = -1;
	Portal.botCaster = -1;
	Portal.awaitingBotCast = false;
	BuildField(ToPortal, Portal.position);
	InvalidateRoutes();
	Announce("The heroes opened a portal out of the keep!");
}

bool IsCasterAlive()
{
	if (Portal.botCaster >= 0)
		return IsLivingBot(Players[Portal.botCaster]);
	if (Portal.caster >= 0) {
		const Monster &caster = Monsters[Portal.caster];
		return IsAliveMonster(caster) && IsHero(caster);
	}
	return false;
}

/** Picks who opens the portal: a real (bot) hero if any is alive, else the hero monster nearest the vault. */
bool PickPortalCaster()
{
	for (size_t i = 0; i < MaxBots; i++) {
		const Player &bot = Players[FirstBotId + i];
		if (IsLivingBot(bot)) {
			Portal.botCaster = static_cast<int>(FirstBotId + i);
			Portal.channel = PortalChannelTicks;
			Announce(fmt::format("{} begins to open a Town Portal! Kill them!", bot._pName));
			return true;
		}
	}
	int best = -1;
	uint16_t bestDistance = Unreachable;
	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		const unsigned id = ActiveMonsters[i];
		const Monster &monster = Monsters[id];
		if (!IsHero(monster) || !IsAliveMonster(monster) || !Heroes[id].active)
			continue;
		const uint16_t distance = FieldAt(ToVault, monster.position.tile);
		if (best == -1 || distance < bestDistance) {
			best = static_cast<int>(id);
			bestDistance = distance;
		}
	}
	if (best < 0)
		return false;
	Portal.caster = best;
	Portal.channel = PortalChannelTicks;
	Announce(fmt::format("A {} begins to open a Town Portal! Kill them!", ClassName(Heroes[best])));
	return true;
}

} // namespace

void UpdatePortal()
{
	if (!State.bossDown || State.phase != Phase::Raid || Portal.open)
		return;
	if ((Portal.caster >= 0 || Portal.botCaster >= 0) && !IsCasterAlive()) {
		Announce("The portal spell was interrupted!", UiFlags::ColorGold);
		Portal.caster = -1;
		Portal.botCaster = -1;
		Portal.awaitingBotCast = false;
	}
	if (Portal.caster < 0 && Portal.botCaster < 0) {
		PickPortalCaster();
		return;
	}

	if (Portal.awaitingBotCast) {
		// The bot is mid-cast: the portal opens where the real Town Portal missile lands.
		for (const Missile &missile : Missiles) {
			if (missile._mitype == MissileID::TownPortal && missile._misource == Portal.botCaster && !missile._miDelFlag) {
				OpenPortalAt(missile.position.tile);
				return;
			}
		}
		if (--Portal.castTimeout > 0)
			return;
		// The cast fizzled (blocked, interrupted by a hit...): open it by hand.
		const Point at = Players[Portal.botCaster].position.tile;
		Missile *missile = AddMissile(at, at, Direction::South, MissileID::TownPortal, TARGET_MONSTERS, -1, 0, 0);
		OpenPortalAt((missile != nullptr && !missile->_miDelFlag) ? Point(missile->position.tile) : at);
		return;
	}

	if (--Portal.channel > 0)
		return;

	if (Portal.botCaster >= 0) {
		BotCastTownPortal(Players[Portal.botCaster]);
		Portal.awaitingBotCast = true;
		Portal.castTimeout = 3 * 20;
		return;
	}
	const Point at = Monsters[Portal.caster].position.tile;
	Missile *missile = AddMissile(at, at, Direction::South, MissileID::TownPortal, TARGET_MONSTERS, -1, 0, 0);
	OpenPortalAt((missile != nullptr && !missile->_miDelFlag) ? Point(missile->position.tile) : at);
}

void ClosePortal()
{
	for (Missile &missile : Missiles) {
		if (missile._mitype == MissileID::TownPortal && (missile._misource == -1 || static_cast<size_t>(missile._misource) >= FirstBotId))
			missile.duration = 0;
	}
	Portal = {};
}

std::optional<Point> PortalCasterPosition()
{
	if (Portal.botCaster >= 0)
		return Players[Portal.botCaster].position.tile;
	if (Portal.caster >= 0)
		return Monsters[Portal.caster].position.tile;
	return std::nullopt;
}

bool IsPortalCaster(const Player &player)
{
	return Portal.botCaster >= 0 && static_cast<size_t>(Portal.botCaster) == player.getId();
}

bool IsPortalCaster(const Monster &monster)
{
	return Portal.caster >= 0 && static_cast<size_t>(Portal.caster) == monster.getId();
}

uint16_t GoalDistance(HeroGoal goal, Point tile)
{
	switch (goal) {
	case HeroGoal::Vault:
		return FieldAt(ToVault, tile);
	case HeroGoal::Entrance:
		return FieldAt(ToEntrance, tile);
	case HeroGoal::Portal:
		return Portal.open ? FieldAt(ToPortal, tile) : Unreachable;
	}
	return Unreachable;
}

std::optional<Point> GoalWaypoint(HeroGoal goal, Point from, int steps, uint16_t knownTraps)
{
	const DistanceField &route = RouteField(goal, knownTraps);
	Point at = from;
	for (int i = 0; i < steps; i++) {
		uint16_t best = FieldAt(route, at);
		if (best == 0 || best == Unreachable)
			break;
		std::optional<Point> next;
		for (const Direction dir : { Direction::South, Direction::SouthWest, Direction::West, Direction::NorthWest, Direction::North, Direction::NorthEast, Direction::East, Direction::SouthEast }) {
			const Point candidate = at + dir;
			const uint16_t value = FieldAt(route, candidate);
			if (value < best && CanStepDiagonal(at, Displacement(dir))) {
				next = candidate;
				best = value;
			}
		}
		if (!next || IsLockedDoorAt(*next))
			break;
		at = *next;
	}
	if (at == from)
		return std::nullopt;
	return at;
}

void OpenDoorsAround(Point tile)
{
	for (const Direction dir : { Direction::NorthEast, Direction::SouthWest, Direction::North, Direction::East, Direction::South, Direction::West, Direction::NorthWest, Direction::SouthEast }) {
		Object *door = FindObjectAtPosition(tile + dir);
		if (door == nullptr || !door->isDoor() || !KeeperIsDoorClosed(*door) || FindLock(door->position) != nullptr)
			continue;
		KeeperOperateDoor(*door, false);
	}
}

std::optional<Point> LockedDoorOnPath(HeroGoal goal, Point from, uint16_t knownTraps)
{
	return LockedDoorOnRoute(RouteField(goal, knownTraps), from);
}

void StrikeLockedDoor(Point tile, int blows, std::string_view who)
{
	Object *door = FindObjectAtPosition(tile);
	if (door == nullptr || !door->isDoor())
		return;
	DoorLock *lock = FindLock(door->position);
	if (lock == nullptr)
		return;
	lock->strength -= blows;
	if (lock->strength <= 0) {
		lock->active = false;
		KeeperOperateDoor(*door, false);
		Announce(fmt::format("{} smashed through a locked door!", who));
		RebuildPaths();
	}
}

int BashBlows(HeroClassKind cls)
{
	return cls == HeroClassKind::Warrior ? WarriorBashBlows : OtherBashBlows;
}

std::optional<size_t> FindBaitFor(HeroClassKind cls, Point position, uint8_t baitsVisited)
{
	int sight = 0;
	switch (cls) {
	case HeroClassKind::Warrior:
		sight = WarriorBaitSight;
		break;
	case HeroClassKind::Rogue:
		sight = RogueBaitSight;
		break;
	case HeroClassKind::Sorcerer:
		sight = SorcererBaitSight;
		break;
	}
	if (sight <= 0)
		return std::nullopt;
	std::optional<size_t> best;
	int bestDistance = 0;
	for (size_t i = 0; i < MaxBaits; i++) {
		const Bait &bait = State.baits[i];
		if (!bait.active || (baitsVisited & (1U << i)) != 0)
			continue;
		const int distance = position.WalkingDistance(bait.position);
		if (distance > sight || !LineClearMissile(position, bait.position))
			continue;
		if (!best || distance < bestDistance) {
			best = i;
			bestDistance = distance;
		}
	}
	return best;
}

void ConsumeBait(size_t slot, uint8_t &baitsVisited, std::string_view who)
{
	Bait &bait = State.baits[slot];
	baitsVisited |= static_cast<uint8_t>(1U << slot);
	if (--bait.uses <= 0)
		bait.active = false;
	Announce(fmt::format("{} wastes time on fool's gold.", who), UiFlags::ColorGold);
}

void NoteTrapFired(size_t trapSlot)
{
	if (trapSlot >= MaxTraps)
		return;
	const Point trap = State.traps[trapSlot].position;
	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		const unsigned id = ActiveMonsters[i];
		const Monster &monster = Monsters[id];
		if (!Heroes[id].active || !IsAliveMonster(monster))
			continue;
		const int distance = monster.position.tile.WalkingDistance(trap);
		if (distance <= TrapDangerRadius || (distance <= TrapWitnessRange && LineClearMissile(monster.position.tile, trap)))
			Heroes[id].trapsKnown |= static_cast<uint16_t>(1U << trapSlot);
	}
	BotsWitnessTrap(trapSlot, trap, TrapWitnessRange);
}

std::optional<Point> NearestHealthyHero(Point from, int range)
{
	std::optional<Point> best = NearestHealthyBot(from, range);
	int bestDistance = best ? from.WalkingDistance(*best) : 0;
	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		const unsigned id = ActiveMonsters[i];
		const Monster &monster = Monsters[id];
		if (!Heroes[id].active || Heroes[id].fleeing || Heroes[id].carried != 0 || !IsAliveMonster(monster))
			continue;
		if (monster.hitPoints * 100 < monster.maxHitPoints * HealthyAllyPercent)
			continue;
		const int distance = from.WalkingDistance(monster.position.tile);
		if (distance <= range && (!best || distance < bestDistance)) {
			best = Point(monster.position.tile);
			bestDistance = distance;
		}
	}
	return best;
}

bool IsAheadOfParty(Point position)
{
	const uint16_t mine = FieldAt(ToVault, position);
	if (mine == Unreachable)
		return false;
	uint16_t rear = BotRearVaultDistance();
	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		const unsigned id = ActiveMonsters[i];
		if (!Heroes[id].active || Heroes[id].fleeing || Heroes[id].carried != 0 || !IsAliveMonster(Monsters[id]))
			continue;
		const uint16_t distance = FieldAt(ToVault, Monsters[id].position.tile);
		if (distance != Unreachable)
			rear = std::max(rear, distance);
	}
	return rear > mine + RegroupLeadTiles;
}

int TakeFromVault(int carryCap)
{
	const int amount = std::min(carryCap, State.vaultGold);
	if (amount <= 0)
		return 0;
	State.vaultGold -= amount;
	return amount;
}

int CarryCapacity(HeroClassKind cls)
{
	return ScaleByHeroStrength(HeroClasses[static_cast<size_t>(cls)].carryCap);
}

bool IsKeepTarget(const Monster &monster)
{
	return IsAliveMonster(monster) && !IsHero(monster) && monster.type().type != MT_GOLEM && (monster.flags & MFLAG_HIDDEN) == 0;
}

std::optional<Point> PortalPosition()
{
	if (!Portal.open)
		return std::nullopt;
	return Portal.position;
}

int PortalChannelPercent()
{
	if (Portal.open || (Portal.caster < 0 && Portal.botCaster < 0))
		return -1;
	if (Portal.awaitingBotCast)
		return 100;
	return 100 * (PortalChannelTicks - Portal.channel) / PortalChannelTicks;
}

std::string_view HeroClassName(HeroClassKind cls)
{
	return HeroClasses[static_cast<size_t>(cls)].name;
}

std::optional<HeroSummary> GetHeroSummary(const Monster &monster)
{
	if (!IsHero(monster) || monster.hitPoints <= 0)
		return std::nullopt;
	const HeroState &hero = Heroes[monster.getId()];
	return HeroSummary { hero.cls, hero.carried, hero.champion, hero.fleeing };
}

std::string_view HeroTitle(const Monster &monster)
{
	const std::optional<HeroClassKind> cls = HeroClassOf(monster.type().type);
	return cls ? HeroClasses[static_cast<size_t>(*cls)].name : std::string_view {};
}

} // namespace keeper
} // namespace devilution
