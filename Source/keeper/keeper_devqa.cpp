/**
 * @file keeper/keeper_devqa.cpp
 *
 * Dev/test only. See keeper_devqa.hpp.
 */
#include "keeper/keeper_devqa.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <vector>

#include <fmt/format.h>

#include "dead.h"
#include "engine/direction.hpp"
#include "keeper/keeper.hpp"
#include "keeper/keeper_bots.hpp"
#include "keeper/keeper_campaign.hpp"
#include "keeper/keeper_game.hpp"
#include "keeper/keeper_heroes.hpp"
#include "keeper/keeper_roster.hpp"
#include "keeper/keeper_upgrades.hpp"
#include "levels/gendung.h"
#include "levels/tile_properties.hpp"
#include "missiles.h"
#include "monster.h"
#include "player.h"

namespace devilution {

void KeeperAiDelay(Monster &monster, int len);

namespace keeper {

namespace {

constexpr int QaLife = 30000;

enum class Sweep : uint8_t {
	None,
	Roster,
	Spells,
	Traps,
};

Sweep sweep = Sweep::None;
int current = 0;
int last = 0;
int stage = 0;
int stageTicks = 0;
std::string shots;
std::function<void(std::string_view)> out;
std::function<void(const std::string &)> takeShot;
Point arena;

/** A monster that stands still until hit, or that is simply tracked. */
struct Dummy {
	int id = -1;
	bool frozen = true;
	int peakLoss = 0;
	int petrifiedTicks = 0;
};

bool IsLive(const Dummy &dummy)
{
	if (dummy.id < 0)
		return false;
	const Monster &monster = Monsters[dummy.id];
	return !monster.isInvalid && monster.hitPoints > 0 && monster.mode != MonsterMode::Death;
}

std::optional<size_t> TypeIndexOf(_monster_id type)
{
	for (size_t i = 0; i < LevelMonsterTypeCount; i++) {
		if (LevelMonsterTypes[i].type == type)
			return i;
	}
	return std::nullopt;
}

std::optional<size_t> HeroTypeIndex()
{
	for (size_t i = 0; i < LevelMonsterTypeCount; i++) {
		if (IsHeroType(LevelMonsterTypes[i].type))
			return i;
	}
	return std::nullopt;
}

void Remove(Dummy &dummy)
{
	if (dummy.id >= 0) {
		Monster &monster = Monsters[dummy.id];
		if (!monster.isInvalid && monster.hitPoints > 0) {
			M_ClearSquares(monster);
			monster.hitPoints = 0;
			monster.isInvalid = true;
		}
	}
	dummy = {};
}

std::optional<Point> FreeTileNear(Point tile)
{
	return FindClosestValidPosition([](Point p) { return IsOpenFloor(p) && dMonster[p.x][p.y] == 0 && dPlayer[p.x][p.y] == 0; }, tile, 0, 6);
}

Dummy SpawnDummy(size_t typeIndex, Point tile, int minDamage, int maxDamage, bool frozen)
{
	Dummy dummy;
	const std::optional<Point> free = FreeTileNear(tile);
	if (!free || ActiveMonsterCount >= MaxMonsters)
		return dummy;
	dummy.id = static_cast<int>(ActiveMonsters[ActiveMonsterCount]);
	dummy.frozen = frozen;
	SpawnMonster(*free, Direction::South, typeIndex);
	Monster &monster = Monsters[dummy.id];
	monster.maxHitPoints = QaLife << 6;
	monster.hitPoints = monster.maxHitPoints;
	monster.minDamage = static_cast<uint8_t>(minDamage);
	monster.maxDamage = static_cast<uint8_t>(maxDamage);
	monster.minDamageSpecial = static_cast<uint8_t>(minDamage);
	monster.maxDamageSpecial = static_cast<uint8_t>(maxDamage);
	return dummy;
}

void Track(Dummy &dummy)
{
	if (dummy.id < 0)
		return;
	Monster &monster = Monsters[dummy.id];
	dummy.peakLoss = std::max(dummy.peakLoss, (monster.maxHitPoints - monster.hitPoints) >> 6);
	if (monster.mode == MonsterMode::Petrified)
		dummy.petrifiedTicks++;
	if (dummy.frozen && monster.mode == MonsterMode::Stand)
		KeeperAiDelay(monster, 1000);
}

void ClearKeep()
{
	for (size_t slot = 0; slot < State.lairs.size(); slot++) {
		if (State.lairs[slot].active)
			RequestSell({ StructureType::Lair, slot });
	}
}

bool FindArena()
{
	for (int radius = 6; radius >= 2; radius--) {
		for (int x = 8; x < MAXDUNX - 8; x++) {
			for (int y = 8; y < MAXDUNY - 8; y++) {
				const Point tile { x, y };
				if (tile.WalkingDistance(State.entrance) < 14 + radius || tile.WalkingDistance(State.vault) < 14 + radius)
					continue;
				bool open = true;
				for (int dx = -radius; dx <= radius && open; dx++) {
					for (int dy = -radius; dy <= radius && open; dy++)
						open = IsOpenFloor(tile + Displacement { dx, dy });
				}
				if (open) {
					arena = tile;
					out(fmt::format("qa arena={},{} radius={}", x, y, radius));
					return true;
				}
			}
		}
	}
	out("qa error no arena");
	return false;
}

/** Missiles out on the map, and whether any of them is off it or can't be drawn. */
struct MissileWatch {
	int outOfBounds = 0;
	int noSprite = 0;
	int peak = 0;
	void Update()
	{
		int count = 0;
		for (const Missile &missile : Missiles) {
			count++;
			if (!InDungeonBounds(missile.position.tile))
				outOfBounds++;
			if (missile._miDrawFlag && !missile._miAnimData)
				noSprite++;
		}
		peak = std::max(peak, count);
	}
};

// ---- roster ------------------------------------------------------------------------------------

enum RosterStage : uint8_t {
	RosterBuild,
	RosterAwaitSpawn,
	RosterIdle,
	RosterFight,
	RosterBack,
	RosterBot,
	RosterKill,
	RosterDying,
	RosterSold,
};

struct RosterCheck {
	int monster = -1;
	Point home;
	Point lastTile;
	int maxWander = 0;
	int maxJump = 0;
	int oob = 0;
	int solid = 0;
	int badFrame = 0;
	int noSprites = 0;
	int badEnemy = 0;
	uint32_t modes = 0;
	int idleFrames = 0;
	int idleFrame = -1;
	int idleWander = 0;
	int firstHit = -1;
	int heroLoss = 0;
	int friendLoss = 0;
	int backDistance = -1;
	int botHit = -1;
	int botLoss = 0;
	int killTicks = -1;
	bool forced = false;
	int deathTicks = -1;
	Point deathTile;
	Direction deathDir = Direction::South;
	MissileWatch missiles;
	Dummy hero;
	Dummy witness;
	int botStart = 0;
	std::string spriteProblems;
	int heroSwings = 0;
	int lowestLife = 0;
	int killDistance = 0;
};
int fightWindow = 500;
RosterCheck check;

Monster *RosterMonster()
{
	return check.monster >= 0 ? &Monsters[check.monster] : nullptr;
}

void WatchRosterMonster()
{
	Monster &monster = *RosterMonster();
	if (monster.isInvalid)
		return;
	const Point tile = monster.position.tile;
	if (!InDungeonBounds(tile)) {
		check.oob++;
		return;
	}
	if (TileHasAny(tile, TileProperties::Solid))
		check.solid++;
	const int jump = std::max(std::abs(tile.x - check.lastTile.x), std::abs(tile.y - check.lastTile.y));
	check.maxJump = std::max(check.maxJump, jump);
	check.lastTile = tile;
	check.maxWander = std::max(check.maxWander, tile.WalkingDistance(check.home));
	check.modes |= 1U << static_cast<unsigned>(monster.mode);
	if (!monster.animInfo.sprites) {
		check.noSprites++;
	} else {
		const int frames = static_cast<int>(monster.animInfo.sprites->numSprites());
		if (monster.animInfo.currentFrame < 0 || monster.animInfo.currentFrame >= frames || monster.animInfo.getFrameToUseForRendering() >= frames)
			check.badFrame++;
	}
	if ((monster.flags & MFLAG_NO_ENEMY) == 0) {
		if ((monster.flags & MFLAG_TARGETS_MONSTER) != 0) {
			if (monster.enemy >= MaxMonsters || !AreHostile(monster, Monsters[monster.enemy]))
				check.badEnemy++;
		} else if (monster.enemy < FirstBotId || monster.enemy >= FirstBotId + MaxBots) {
			check.badEnemy++; // masters are never targets
		}
	}
	check.missiles.Update();
}

std::string CheckSprites(const CMonster &type)
{
	std::string problems;
	constexpr std::array<std::string_view, 6> Names { "stand", "walk", "attack", "hit", "death", "special" };
	for (size_t a = 0; a < 6; a++) {
		const AnimStruct &anim = type.anims[a];
		if (anim.frames <= 0)
			continue;
		if (!anim.sprites) {
			problems += fmt::format("{}:none ", Names[a]);
			continue;
		}
		for (const Direction dir : { Direction::South, Direction::SouthWest, Direction::West, Direction::NorthWest, Direction::North, Direction::NorthEast, Direction::East, Direction::SouthEast }) {
			const OptionalClxSpriteList sprites = anim.spritesForDirection(dir);
			if (!sprites || static_cast<int>(sprites->numSprites()) < anim.frames) {
				problems += fmt::format("{}:{}<{}@{} ", Names[a], sprites ? static_cast<int>(sprites->numSprites()) : 0, anim.frames, static_cast<int>(dir));
				break;
			}
		}
	}
	return problems;
}

void FinishKind()
{
	const size_t kind = static_cast<size_t>(current);
	const BuildInfo &info = BuildKinds[kind];
	const CMonster *type = nullptr;
	if (const std::optional<size_t> index = TypeIndexOf(info.type))
		type = &LevelMonsterTypes[*index];
	const KindImage image = MakeKindImage(kind, 64, 64);
	std::string corpse = "none";
	if (type != nullptr && type->corpseId == 0 && check.deathTicks >= 0) {
		// Drawn by type (dCorpseType): which type lies on the death tile, and whether it can be drawn.
		const std::optional<size_t> atTile = CorpseTypeAt(check.deathTile);
		const AnimStruct &anim = type->getAnimData(MonsterGraphic::Death);
		const OptionalClxSpriteList sprites = anim.spritesForDirection(check.deathDir);
		corpse = fmt::format("bytype/tile{}/frame{}of{}", atTile ? static_cast<int>(*atTile) : -1, anim.frames - 1, sprites ? static_cast<int>(sprites->numSprites()) : 0);
	} else if (type != nullptr && type->corpseId > 0) {
		const Corpse &entry = Corpses[type->corpseId - 1];
		const int atTile = check.deathTicks >= 0 ? dCorpse[check.deathTile.x][check.deathTile.y] & 0x1F : -1;
		const int frames = entry.sprites ? static_cast<int>(entry.spritesForDirection(check.deathDir).numSprites()) : 0;
		corpse = fmt::format("{}/tile{}/frame{}of{}", type->corpseId, atTile, entry.frame, frames);
	}
	out(fmt::format("kind={} type={} name=\"{}\" data=\"{}\" family=\"{}\" rank={} cost={} pop={} life={} dmg={}-{} roster={} ai={} loaded={} sprites=\"{}\" stillFrames={} idleWander={} firstHit={} heroLoss={} friendLoss={} botHit={} botLoss={} modes={:x} wander={} jump={} oob={} solid={} badFrame={} noSprites={} badEnemy={} back={} killTicks={} forced={} deathTicks={} corpse={} imgw={} imgh={} misOOB={} misNoSprite={} misPeak={} swings={} lowest={}/{} heroFar={}",
	    kind, static_cast<int>(info.type), info.name, MonstersData[info.type].name, FamilyName(FamilyOfKind(kind)), UnlockRankOf(kind), info.cost, static_cast<int>(info.supply), KindLife(kind), KindMinDamage(kind), KindMaxDamage(kind), IsKindOnRoster(kind), static_cast<int>(MonstersData[info.type].ai), type != nullptr && type->animData != nullptr, check.spriteProblems, check.idleFrames, check.idleWander, check.firstHit, check.heroLoss, check.friendLoss, check.botHit, check.botLoss, check.modes, check.maxWander, check.maxJump, check.oob, check.solid, check.badFrame, check.noSprites, check.badEnemy, check.backDistance, check.killTicks, check.forced, check.deathTicks, corpse, image.width, image.height, check.missiles.outOfBounds, check.missiles.noSprite, check.missiles.peak, check.heroSwings, check.lowestLife >> 6, RosterMonster() != nullptr ? RosterMonster()->maxHitPoints >> 6 : 0, check.killDistance));
}

void NextRosterKind()
{
	FinishKind();
	ClearKeep();
	current++;
	stage = RosterSold;
	stageTicks = 0;
}

void RosterTick()
{
	State.vaultGold = 1000000;
	State.phaseTicks = 100000;
	stageTicks++;
	const std::optional<size_t> heroType = HeroTypeIndex();
	Monster *monster = RosterMonster();
	if (monster != nullptr && stage >= RosterIdle && stage <= RosterKill)
		WatchRosterMonster();
	Track(check.hero);
	Track(check.witness);

	switch (stage) {
	case RosterSold:
		if (stageTicks < 3)
			return;
		if (current > last) {
			out("qa rosterqa done");
			sweep = Sweep::None;
			return;
		}
		stage = RosterBuild;
		stageTicks = 0;
		[[fallthrough]];
	case RosterBuild: {
		check = {};
		for (auto &column : dCorpse)
			std::fill(std::begin(column), std::end(column), static_cast<uint8_t>(0));
		std::string_view reason;
		if (!CanBuildAt(static_cast<size_t>(current), arena, &reason)) {
			out(fmt::format("kind={} refused=\"{}\"", current, reason));
			current++;
			stage = RosterSold;
			return;
		}
		RequestBuild(static_cast<size_t>(current), arena);
		stage = RosterAwaitSpawn;
		stageTicks = 0;
		return;
	}
	case RosterAwaitSpawn: {
		for (const Lair &lair : State.lairs) {
			if (lair.active && lair.kind == current && lair.members[0] >= 0) {
				check.monster = lair.members[0];
				break;
			}
		}
		if (check.monster < 0) {
			if (stageTicks > 40) {
				out(fmt::format("kind={} problem=\"never spawned\"", current));
				NextRosterKind();
			}
			return;
		}
		monster = RosterMonster();
		check.home = arena;
		check.lastTile = monster->position.tile;
		if (const std::optional<size_t> index = TypeIndexOf(BuildKinds[current].type))
			check.spriteProblems = CheckSprites(LevelMonsterTypes[*index]);
		DevCenterCamera(arena);
		stage = RosterIdle;
		stageTicks = 0;
		return;
	}
	case RosterIdle: {
		const int frame = monster->animInfo.currentFrame;
		if (frame != check.idleFrame) {
			check.idleFrame = frame;
			check.idleFrames++;
		}
		if (stageTicks < 60)
			return;
		check.idleWander = check.maxWander;
		// A training dummy who can't hurt anything and a friend on the way: all that's hurt is by the monster.
		if (heroType)
			check.hero = SpawnDummy(*heroType, arena + Displacement { 4, 0 }, 0, 0, true);
		if (const std::optional<size_t> friendType = TypeIndexOf(BuildKinds[0].type))
			check.witness = SpawnDummy(*friendType, arena + Displacement { 2, 1 }, 0, 0, false);
		stage = RosterFight;
		stageTicks = 0;
		return;
	}
	case RosterFight: {
		if (IsLive(check.hero)) {
			const int loss = (Monsters[check.hero.id].maxHitPoints - Monsters[check.hero.id].hitPoints) >> 6;
			if (loss > 0 && check.firstHit < 0) {
				check.firstHit = stageTicks;
				if (!shots.empty())
					takeShot(fmt::format("{}\\kind_{:03}_fight.png", shots, current));
			}
			check.heroLoss = std::max(check.heroLoss, loss);
		}
		if (IsLive(check.witness))
			check.friendLoss = std::max(check.friendLoss, (Monsters[check.witness.id].maxHitPoints - Monsters[check.witness.id].hitPoints) >> 6);
		// Long enough to see what it does once it has made contact.
		if (stageTicks < (check.firstHit >= 0 ? check.firstHit + 120 : fightWindow))
			return;
		Remove(check.hero);
		Remove(check.witness);
		stage = RosterBack;
		stageTicks = 0;
		return;
	}
	case RosterBack:
		if (stageTicks < 200)
			return;
		check.backDistance = monster->position.tile.WalkingDistance(check.home);
		stage = RosterBot;
		stageTicks = 0;
		SpawnBot(0, HeroClassKind::Warrior, FreeTileNear(arena + Displacement { 4, 0 }).value_or(arena));
		{
			Player &bot = Players[FirstBotId];
			bot._pMaxHP = bot._pMaxHPBase = QaLife << 6;
			bot._pHitPoints = bot._pHPBase = QaLife << 6;
			check.botStart = bot._pHitPoints;
		}
		return;
	case RosterBot: {
		const Player &bot = Players[FirstBotId];
		const int loss = (check.botStart - bot._pHitPoints) >> 6;
		check.botLoss = std::max(check.botLoss, loss);
		if (loss > 0 && check.botHit < 0)
			check.botHit = stageTicks;
		if (stageTicks < 300 && check.botHit < 0)
			return;
		RemoveAllBots();
		// The real thing now: a hero who means it.
		if (heroType) {
			const int blow = std::clamp(monster->maxHitPoints / 64 / 2, 8, 255); // well above what it regenerates
			check.hero = SpawnDummy(*heroType, monster->position.tile + Displacement { 2, 0 }, blow, blow, false);
		}
		stage = RosterKill;
		stageTicks = 0;
		return;
	}
	case RosterKill: {
		if (IsLive(check.hero)) {
			const Monster &hero = Monsters[check.hero.id];
			if (hero.mode == MonsterMode::MeleeAttack && hero.animInfo.currentFrame == 0)
				check.heroSwings++;
			check.killDistance = std::max(check.killDistance, hero.position.tile.WalkingDistance(monster->position.tile));
		}
		check.lowestLife = check.lowestLife == 0 ? monster->hitPoints : std::min(check.lowestLife, monster->hitPoints);
		if (monster->mode == MonsterMode::Death || monster->hitPoints <= 0 || monster->isInvalid) {
			check.killTicks = stageTicks;
			check.deathTile = monster->position.tile;
			check.deathDir = monster->direction;
			stage = RosterDying;
			stageTicks = 0;
			Remove(check.hero);
			return;
		}
		if (stageTicks < 700)
			return;
		// It got away from the hero all this time: note it, then finish it by hand.
		check.forced = true;
		check.killTicks = stageTicks;
		check.deathTile = monster->position.tile;
		check.deathDir = monster->direction;
		monster->hitPoints = 0;
		M_StartKill(*monster, *MyPlayer);
		stage = RosterDying;
		stageTicks = 0;
		Remove(check.hero);
		return;
	}
	case RosterDying:
		if (monster->mode == MonsterMode::Death && !monster->isInvalid && stageTicks < 400) {
			check.deathTile = monster->position.tile;
			check.deathDir = monster->direction;
			return;
		}
		if (stageTicks < 3)
			return;
		check.deathTicks = stageTicks;
		if (!shots.empty())
			takeShot(fmt::format("{}\\kind_{:03}_corpse.png", shots, current));
		NextRosterKind();
		return;
	default:
		return;
	}
}

// ---- spells --------------------------------------------------------------------------------------

struct CastCheck {
	std::array<Dummy, 3> heroes;
	Dummy witness;
	Point target;
	int manaBefore = 0;
	int manaAfter = 0;
	int cooldown = 0;
	MissileWatch missiles;
	bool casted = false;
};
CastCheck cast;
int spellLevel = 1;
bool edgePass = false;
size_t edgeIndex = 0;
std::vector<Point> edgeTargets;

void ClearCast()
{
	for (Dummy &hero : cast.heroes)
		Remove(hero);
	Remove(cast.witness);
}

void SetUpCast(Point target)
{
	const std::optional<size_t> heroType = HeroTypeIndex();
	const std::optional<size_t> friendType = TypeIndexOf(BuildKinds[0].type);
	// New ones first: for a moment with no heroes at all the raid would end.
	ClearCast();
	CastCheck next;
	next.target = target;
	if (heroType) {
		next.heroes[0] = SpawnDummy(*heroType, arena, 0, 0, true);
		next.heroes[1] = SpawnDummy(*heroType, arena + Displacement { 2, 0 }, 0, 0, true);
		next.heroes[2] = SpawnDummy(*heroType, arena + Displacement { 5, 5 }, 0, 0, true);
	}
	if (friendType)
		next.witness = SpawnDummy(*friendType, arena + Displacement { 1, 1 }, 0, 0, true);
	cast = next;
}

void SpellsTick()
{
	State.phase = Phase::Raid;
	State.phaseTicks = 100000;
	stageTicks++;
	for (Dummy &hero : cast.heroes)
		Track(hero);
	Track(cast.witness);
	cast.missiles.Update();
	const size_t spell = static_cast<size_t>(current);
	if (stage == 0) {
		if (current > last) {
			if (!edgePass) {
				edgePass = true;
				current = 0;
				spellLevel = 1;
				edgeIndex = 0;
				edgeTargets = { { 0, 0 }, { MAXDUNX - 1, 0 }, { 0, MAXDUNY - 1 }, { MAXDUNX - 1, MAXDUNY - 1 }, { 1, 1 }, { MAXDUNX - 2, MAXDUNY - 2 }, { 56, 0 }, { 0, 56 }, { MAXDUNX - 1, 56 }, { 56, MAXDUNY - 1 }, { 8, 8 }, { MAXDUNX - 9, MAXDUNY - 9 }, { 8, MAXDUNY - 9 }, { MAXDUNX - 9, 8 }, { 16, 16 }, { 95, 95 }, State.entrance, State.vault };
				for (int x = 20; x < MAXDUNX; x++) {
					if (TileHasAny(Point { x, arena.y }, TileProperties::Solid)) {
						edgeTargets.push_back({ x, arena.y });
						break;
					}
				}
				return;
			}
			ClearCast();
			out("qa spellqa done");
			sweep = Sweep::None;
			return;
		}
		// Wait for what's left of the last spell to burn out, so it isn't counted against this one.
		if (stageTicks > 1 && !Missiles.empty() && stageTicks < 60)
			return;
		// A long wall can outlast the wait: it mustn't be counted against the next spell, or alter where that lands.
		if (!Missiles.empty()) {
			for (Missile &missile : Missiles)
				missile._miDelFlag = true;
			return;
		}
		const Point target = edgePass ? edgeTargets[edgeIndex] : arena;
		SetUpCast(target);
		State.spellLevels[spell] = static_cast<uint8_t>(spellLevel);
		State.mana[MyPlayerId] = MaxMana * ManaScale;
		State.spellCooldown[MyPlayerId] = 0;
		cast.manaBefore = State.mana[MyPlayerId];
		RequestCastSpell(spell, target);
		stage = 1;
		stageTicks = 0;
		return;
	}
	if (!cast.casted && State.spellCooldown[MyPlayerId] > 0) {
		cast.casted = true;
		cast.manaAfter = State.mana[MyPlayerId];
		cast.cooldown = State.spellCooldown[MyPlayerId];
	}
	const bool over = stageTicks > 20 && Missiles.empty();
	if (!over && stageTicks < (edgePass ? 40 : 260))
		return;
	out(fmt::format("spell={} name=\"{}\" level={} engine={} edge={} target={},{} casted={} manaCost={} manaExpected={} cooldown={} cooldownExpected={} hitTarget={} hitNear={} hitFar={} hitFriend={} stoneTarget={} stoneNear={} stoneFar={} stoneFriend={} ticks={} misOOB={} misNoSprite={} misPeak={}",
	    spell, SpellKinds[spell].name, spellLevel, SpellEngineLevel(spell), edgePass, cast.target.x, cast.target.y, cast.casted, cast.manaBefore - cast.manaAfter, SpellManaCostScaled(spell), cast.cooldown, SpellKinds[spell].cooldownTicks, cast.heroes[0].peakLoss, cast.heroes[1].peakLoss, cast.heroes[2].peakLoss, cast.witness.peakLoss, cast.heroes[0].petrifiedTicks, cast.heroes[1].petrifiedTicks, cast.heroes[2].petrifiedTicks, cast.witness.petrifiedTicks, stageTicks, cast.missiles.outOfBounds, cast.missiles.noSprite, cast.missiles.peak));
	stage = 0;
	stageTicks = 0;
	if (edgePass) {
		if (++edgeIndex >= edgeTargets.size()) {
			edgeIndex = 0;
			spellLevel = spellLevel == 1 ? MaxKeepLevel : 1;
			if (spellLevel == 1)
				current++;
		}
	} else if (++spellLevel > MaxKeepLevel) {
		spellLevel = 1;
		current++;
	}
}

// ---- traps ---------------------------------------------------------------------------------------

struct TrapCheck {
	Dummy hero;
	Dummy bystander;
	Dummy friend1;
	Dummy friend2;
	bool onBot = false;
	int botStart = 0;
	int botLoss = 0;
	int botStone = 0;
	int firedAt = -1;
	int rearm = 0;
	MissileWatch missiles;
};
TrapCheck trapCheck;
int trapLevel = 1;
bool trapOnBot = false;
bool trapBotOnly = false;

void TrapsTick()
{
	State.phaseTicks = 100000;
	stageTicks++;
	Track(trapCheck.hero);
	Track(trapCheck.bystander);
	Track(trapCheck.friend1);
	Track(trapCheck.friend2);
	trapCheck.missiles.Update();
	const std::optional<size_t> heroType = HeroTypeIndex();
	const std::optional<size_t> friendType = TypeIndexOf(BuildKinds[0].type);
	if (stage == 0) {
		if (current > last) {
			out("qa trapqa done");
			sweep = Sweep::None;
			return;
		}
		if (!Missiles.empty() && stageTicks < 400)
			return;
		trapCheck = {};
		trapCheck.onBot = trapOnBot;
		State.trapLevels[current] = static_cast<uint8_t>(trapLevel);
		// Traps go off as soon as a hero is near, so the bait is in place before the trap is.
		if (trapOnBot) {
			SpawnBot(0, HeroClassKind::Warrior, FreeTileNear(arena + Displacement { 1, 0 }).value_or(arena));
			Player &bot = Players[FirstBotId];
			bot._pMaxHP = bot._pMaxHPBase = QaLife << 6;
			bot._pHitPoints = bot._pHPBase = QaLife << 6;
			trapCheck.botStart = bot._pHitPoints;
		} else if (heroType) {
			trapCheck.hero = SpawnDummy(*heroType, arena + Displacement { 1, 0 }, 0, 0, true);
		}
		if (heroType)
			trapCheck.bystander = SpawnDummy(*heroType, arena + Displacement { 4, 0 }, 0, 0, true);
		if (friendType) {
			trapCheck.friend1 = SpawnDummy(*friendType, arena + Displacement { 0, 1 }, 0, 0, true);
			trapCheck.friend2 = SpawnDummy(*friendType, arena + Displacement { -1, 0 }, 0, 0, true);
		}
		Trap &trap = State.traps[0];
		trap = {};
		trap.active = true;
		trap.position = arena;
		trap.kind = static_cast<uint8_t>(current);
		stage = 1;
		stageTicks = 0;
		return;
	}
	if (trapCheck.onBot) {
		const Player &bot = Players[FirstBotId];
		trapCheck.botLoss = std::max(trapCheck.botLoss, (trapCheck.botStart - bot._pHitPoints) >> 6);
		if (IsPetrifiedBot(bot))
			trapCheck.botStone++;
	}
	Trap &trap = State.traps[0];
	if (trapCheck.firedAt < 0 && trapCheck.firedAt != -2) {
		if (trap.cooldown > 0) {
			trapCheck.firedAt = stageTicks;
			trapCheck.rearm = trap.cooldown;
			trap.active = false;
		} else if (stageTicks > 60) {
			trapCheck.firedAt = -2;
		}
		if (trapCheck.firedAt == -1)
			return;
	}
	if (stageTicks < 160 && (!Missiles.empty() || stageTicks < 60))
		return;
	const TrapKind &kind = TrapKinds[current];
	out(fmt::format("trap={} name=\"{}\" level={} onBot={} firedAt={} rearm={} rearmBase={} damageBase={} hit={} botHit={} stone={} botStone={} hitNear={} hitFriend1={} hitFriend2={} stoneFriend1={} stoneFriend2={} misOOB={} misNoSprite={} misPeak={}",
	    current, kind.name, trapLevel, trapCheck.onBot, trapCheck.firedAt, trapCheck.rearm, kind.rearmTicks, ScaleByHeroStrength(kind.baseDamage + kind.damagePerWave * State.wave), trapCheck.hero.peakLoss, trapCheck.botLoss, trapCheck.hero.petrifiedTicks, trapCheck.botStone, trapCheck.bystander.peakLoss, trapCheck.friend1.peakLoss, trapCheck.friend2.peakLoss, trapCheck.friend1.petrifiedTicks, trapCheck.friend2.petrifiedTicks, trapCheck.missiles.outOfBounds, trapCheck.missiles.noSprite, trapCheck.missiles.peak));
	trap = {};
	Remove(trapCheck.hero);
	Remove(trapCheck.bystander);
	Remove(trapCheck.friend1);
	Remove(trapCheck.friend2);
	if (trapCheck.onBot)
		RemoveAllBots();
	stage = 0;
	stageTicks = 0;
	if (trapBotOnly) {
		if (++trapLevel > MaxKeepLevel) {
			trapLevel = 1;
			current++;
		}
	} else if (!trapOnBot) {
		trapLevel++;
		if (trapLevel > MaxKeepLevel) {
			trapLevel = 1;
			trapOnBot = true;
		}
	} else if (trapLevel == 1) {
		trapLevel = MaxKeepLevel;
	} else {
		trapLevel = 1;
		trapOnBot = false;
		current++;
	}
}

} // namespace

void DevQaStart(std::string_view name, int first, int lastItem, int window, std::string shotDir, std::function<void(std::string_view)> output, std::function<void(const std::string &)> shot)
{
	out = std::move(output);
	takeShot = std::move(shot);
	shots = std::move(shotDir);
	if (shots == "-")
		shots.clear();
	stage = 0;
	stageTicks = 0;
	current = first;
	if (!IsActive()) {
		out("qa error not in the keep");
		return;
	}
	if (!FindArena())
		return;
	if (name == "rosterqa") {
		fightWindow = window > 0 ? window : 500;
		current = std::clamp(first, 0, static_cast<int>(NumLairKinds) - 1);
		last = lastItem >= 0 ? std::min(lastItem, static_cast<int>(NumLairKinds) - 1) : static_cast<int>(NumLairKinds) - 1;
		stage = RosterSold;
		check = {};
		sweep = Sweep::Roster;
	} else if (name == "spellqa") {
		current = std::clamp(first, 0, static_cast<int>(NumSpellKinds) - 1);
		last = lastItem >= 0 ? std::min(lastItem, static_cast<int>(NumSpellKinds) - 1) : static_cast<int>(NumSpellKinds) - 1;
		spellLevel = 1;
		edgePass = false;
		cast = {};
		sweep = Sweep::Spells;
	} else if (name == "trapqa") {
		current = std::clamp(first, 0, static_cast<int>(NumTrapKinds) - 1);
		last = lastItem >= 0 ? std::min(lastItem, static_cast<int>(NumTrapKinds) - 1) : static_cast<int>(NumTrapKinds) - 1;
		trapLevel = 1;
		trapBotOnly = window > 0;
		trapOnBot = trapBotOnly;
		trapCheck = {};
		sweep = Sweep::Traps;
	}
}

bool DevQaRunning()
{
	return sweep != Sweep::None;
}

void DevQaCheckStillInKeep()
{
	if (sweep != Sweep::None && !IsActive()) {
		out("qa error left the keep");
		sweep = Sweep::None;
	}
}

void DevQaTick()
{
	if (sweep == Sweep::None)
		return;
	switch (sweep) {
	case Sweep::Roster:
		RosterTick();
		break;
	case Sweep::Spells:
		SpellsTick();
		break;
	case Sweep::Traps:
		TrapsTick();
		break;
	default:
		break;
	}
}

} // namespace keeper
} // namespace devilution
