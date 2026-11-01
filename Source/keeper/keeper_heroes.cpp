/**
 * @file keeper/keeper_heroes.cpp
 *
 * AI heroes for The Dark Keep. See keeper_heroes.hpp.
 */
#include "keeper/keeper_heroes.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <queue>
#include <vector>

#include <fmt/format.h>

#include "engine/direction.hpp"
#include "engine/load_cl2.hpp"
#include "engine/path.h"
#include "keeper/keeper.hpp"
#include "keeper/keeper_game.hpp"
#include "levels/gendung.h"
#include "levels/tile_properties.hpp"
#include "missiles.h"
#include "monster.h"
#include "objects.h"
#include "plrmsg.h"
#include "tables/playerdat.hpp"
#include "utils/str_cat.hpp"

namespace devilution {

// Exported from monster.cpp for the keeper's hero AI.
void KeeperStartMeleeAttack(Monster &monster);
void KeeperStartRangedAttack(Monster &monster, MissileID missileType, int dam);
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

std::array<_monster_id, NumHeroClasses> HeroMonsterIds {};
bool heroDataRegistered = false;

/** Owned sprite sheets per class: Stand, Walk, Attack, Hit, Death. Loaded once, reused across games. */
std::array<std::array<std::optional<OwnedClxSpriteListOrSheet>, 5>, NumHeroClasses> HeroSprites;

constexpr int HeroSight = 6;
constexpr int GuardRadius = 8;
constexpr int SpawnIntervalTicks = 15;

struct HeroState {
	bool active = false;
	HeroClassKind cls = HeroClassKind::Warrior;
	int carried = 0;
	bool champion = false;
};

std::array<HeroState, MaxMonsters> Heroes;
std::array<Point, MaxMonsters> Homes;

std::vector<HeroClassKind> pendingParty;
int spawnCooldown = 0;

constexpr uint16_t Unreachable = 0xFFFF;
using DistanceField = std::array<std::array<uint16_t, MAXDUNY>, MAXDUNX>;
DistanceField ToVault;
DistanceField ToEntrance;

std::optional<HeroClassKind> HeroClassOf(_monster_id type)
{
	for (size_t i = 0; i < NumHeroClasses; i++) {
		if (heroDataRegistered && HeroMonsterIds[i] == type)
			return static_cast<HeroClassKind>(i);
	}
	return std::nullopt;
}

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

void BuildField(DistanceField &field, Point target)
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
			if (!IsPassable(neighbour) || field[neighbour.x][neighbour.y] != Unreachable || !CanStepDiagonal(tile, step))
				continue;
			field[neighbour.x][neighbour.y] = next;
			open.push(neighbour);
		}
	}
}

uint16_t FieldAt(const DistanceField &field, Point tile)
{
	if (!InDungeonBounds(tile))
		return Unreachable;
	return field[tile.x][tile.y];
}

bool IsAliveMonster(const Monster &monster)
{
	return monster.hitPoints > 0 && monster.mode != MonsterMode::Death && !monster.isInvalid && monster.position.tile != GolemHoldingCell;
}

/** Walk one step in dir, or the nearest turn that is open. */
bool StepTowards(Monster &monster, Direction dir)
{
	for (const Direction tryDir : { dir, Left(dir), Right(dir) }) {
		if (DirOK(monster, tryDir)) {
			Walk(monster, tryDir);
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
		if (value < bestValue && DirOK(monster, dir)) {
			best = dir;
			bestValue = value;
		}
	}
	if (!best) {
		// Path blocked (by a monster, usually): sidestep to an equally good tile, or wait.
		for (const Direction dir : { Direction::South, Direction::SouthWest, Direction::West, Direction::NorthWest, Direction::North, Direction::NorthEast, Direction::East, Direction::SouthEast }) {
			if (FieldAt(field, position + dir) == here && DirOK(monster, dir)) {
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
	int hp = 0;
	switch (cls) {
	case HeroClassKind::Warrior:
		hp = 30 + 10 * wave;
		monster.minDamage = static_cast<uint8_t>(2 + wave);
		monster.maxDamage = static_cast<uint8_t>(6 + 2 * wave);
		monster.armorClass = static_cast<uint8_t>(10 + 3 * wave);
		break;
	case HeroClassKind::Rogue:
		hp = 20 + 7 * wave;
		monster.minDamage = static_cast<uint8_t>(1 + wave);
		monster.maxDamage = static_cast<uint8_t>(4 + 2 * wave);
		monster.armorClass = static_cast<uint8_t>(5 + 2 * wave);
		break;
	case HeroClassKind::Sorcerer:
		hp = 15 + 6 * wave;
		monster.minDamage = static_cast<uint8_t>(2 + wave);
		monster.maxDamage = static_cast<uint8_t>(5 + 2 * wave);
		monster.armorClass = static_cast<uint8_t>(3 + wave);
		break;
	}
	if (champion)
		hp = hp * 3 / 2;
	monster.maxHitPoints = hp << 6;
	monster.hitPoints = monster.maxHitPoints;
	monster.flags |= MFLAG_CAN_OPEN_DOOR;
}

void Announce(std::string_view message)
{
	EventPlrMsg(message, UiFlags::ColorRed);
}

std::vector<HeroClassKind> PartyForWave(uint8_t wave)
{
	using enum HeroClassKind;
	switch (wave) {
	case 1:
		return { Warrior, Rogue };
	case 2:
		return { Warrior, Rogue, Sorcerer };
	case 3:
		return { Warrior, Warrior, Rogue, Sorcerer };
	case 4:
		return { Warrior, Rogue, Rogue, Sorcerer };
	case 5:
		return { Warrior, Warrior, Rogue, Sorcerer, Sorcerer };
	case 6:
		return { Warrior, Warrior, Rogue, Rogue, Sorcerer, Sorcerer };
	default:
		return { Warrior, Warrior, Rogue, Rogue, Sorcerer, Sorcerer };
	}
}

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
	return InDungeonBounds(tile) && IsTileWalkable(tile) && dMonster[tile.x][tile.y] == 0 && dPlayer[tile.x][tile.y] == 0 && !IsObjectAtPosition(tile);
}

void HeroEscapes(Monster &monster, HeroState &hero)
{
	if (hero.carried > 0)
		Announce(fmt::format("A {} escaped with {} gold!", HeroClasses[static_cast<size_t>(hero.cls)].name, hero.carried));
	hero = {};
	M_ClearSquares(monster);
	monster.hitPoints = 0;
	monster.isInvalid = true;
}

void HeroSteals(Monster &monster, HeroState &hero)
{
	const int amount = std::min(HeroClasses[static_cast<size_t>(hero.cls)].carryCap, State.vaultGold);
	if (amount <= 0)
		return;
	State.vaultGold -= amount;
	hero.carried = amount;
	Announce(fmt::format("A {} looted {} gold from the vault!", HeroClasses[static_cast<size_t>(hero.cls)].name, amount));
	KeeperAiDelay(monster, 10);
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
	heroDataRegistered = true;
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

void AddHeroMonsterTypes()
{
	for (const LairKind &kind : LairKinds)
		(void)AddMonsterType(kind.type, PLACE_SPECIAL);
	for (const _monster_id type : HeroMonsterIds)
		(void)AddMonsterType(type, PLACE_SPECIAL);
}

void OnMonsterInit(Monster &monster)
{
	const size_t id = monster.getId();
	if (id >= MaxMonsters)
		return;
	Homes[id] = monster.position.tile;
	const std::optional<HeroClassKind> cls = HeroClassOf(monster.type().type);
	if (!cls) {
		Heroes[id] = {};
		return;
	}
	HeroState &hero = Heroes[id];
	hero = {};
	hero.active = true;
	hero.cls = *cls;
	hero.champion = State.wave >= NumWaves;
	ApplyHeroStats(monster, *cls, hero.champion);
}

void OnMonsterDeath(Monster &monster)
{
	const size_t id = monster.getId();
	if (id >= MaxMonsters || !Heroes[id].active)
		return;
	HeroState &hero = Heroes[id];
	const int bounty = 20 + 5 * State.wave;
	State.vaultGold += hero.carried + bounty;
	if (hero.carried > 0)
		Announce(fmt::format("Recovered {} stolen gold!", hero.carried));
	hero = {};
}

void UpdateEnemy(Monster &monster)
{
	const bool hero = IsHero(monster);
	const Point position = monster.position.tile;
	const Point anchor = hero ? position : Homes[monster.getId()];
	const int range = hero ? HeroSight : GuardRadius;

	int best = -1;
	int bestDistance = 0;
	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		const unsigned otherId = ActiveMonsters[i];
		const Monster &other = Monsters[otherId];
		if (&other == &monster || !IsAliveMonster(other) || !AreHostile(monster, other))
			continue;
		if (other.position.tile.WalkingDistance(anchor) > range)
			continue;
		const int distance = other.position.tile.WalkingDistance(position);
		if (hero && distance > 1 && !LineClearMissile(position, other.position.tile))
			continue;
		if (best == -1 || distance < bestDistance) {
			best = static_cast<int>(otherId);
			bestDistance = distance;
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

	MonstCheckDoors(monster);

	if ((monster.flags & MFLAG_NO_ENEMY) == 0 && (monster.flags & MFLAG_TARGETS_MONSTER) != 0) {
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

	// No fight: go about the heist.
	if (hero.carried == 0) {
		if (FieldAt(ToVault, monster.position.tile) <= 1) {
			if (State.vaultGold > 0) {
				HeroSteals(monster, hero);
			} else {
				// Nothing left to steal: head home empty-handed.
				hero.carried = -1;
			}
			return;
		}
		if (!FollowField(monster, ToVault))
			KeeperAiDelay(monster, 5);
		return;
	}

	if (FieldAt(ToEntrance, monster.position.tile) <= 1) {
		HeroEscapes(monster, hero);
		return;
	}
	if (!FollowField(monster, ToEntrance))
		KeeperAiDelay(monster, 5);
}

void LeashMonsters()
{
	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		Monster &monster = Monsters[ActiveMonsters[i]];
		if (!IsAliveMonster(monster) || IsHero(monster) || monster.mode != MonsterMode::Stand)
			continue;
		if ((monster.flags & MFLAG_NO_ENEMY) == 0)
			continue;
		const Point home = Homes[monster.getId()];
		if (monster.position.tile.WalkingDistance(home) <= 3)
			continue;
		StepTowards(monster, GetDirection(monster.position.tile, home));
	}
}

void ResetHeroes()
{
	Heroes.fill({});
	pendingParty.clear();
	spawnCooldown = 0;
	RebuildPaths();
}

void RebuildPaths()
{
	BuildField(ToVault, State.vault);
	BuildField(ToEntrance, State.entrance);
}

void StartWave(uint8_t wave)
{
	pendingParty = PartyForWave(wave);
	spawnCooldown = 0;
	Announce(wave >= NumWaves ? std::string_view("The champions have come for your gold!") : std::string_view("Heroes approach the keep!"));
}

RaidStatus UpdateRaid()
{
	if (!pendingParty.empty()) {
		if (spawnCooldown > 0) {
			spawnCooldown--;
		} else if (ActiveMonsterCount < MaxMonsters) {
			const HeroClassKind cls = pendingParty.front();
			const std::optional<size_t> typeIndex = FindLevelMonsterType(HeroMonsterIds[static_cast<size_t>(cls)]);
			const std::optional<Point> tile = FindClosestValidPosition(IsFreeFloor, State.entrance, 0, 6);
			if (typeIndex && tile) {
				SpawnMonster(*tile, Direction::SouthEast, *typeIndex);
				pendingParty.erase(pendingParty.begin());
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
	return total;
}

int LivingHeroCount()
{
	int count = 0;
	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		const Monster &monster = Monsters[ActiveMonsters[i]];
		if (IsHero(monster) && IsAliveMonster(monster))
			count++;
	}
	return count;
}

std::string_view HeroTitle(const Monster &monster)
{
	const std::optional<HeroClassKind> cls = HeroClassOf(monster.type().type);
	return cls ? HeroClasses[static_cast<size_t>(*cls)].name : std::string_view {};
}

} // namespace keeper
} // namespace devilution
