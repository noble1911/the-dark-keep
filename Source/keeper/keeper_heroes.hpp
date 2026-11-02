/**
 * @file keeper/keeper_heroes.hpp
 *
 * AI heroes: raiding parties built from monster slots wearing the original hero
 * class sprites. They walk to the treasure vault, steal gold, and try to escape
 * back out the entrance; the keep's monsters (and the masters) try to stop them.
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/point.hpp"
#include "tables/monstdat.h"

namespace devilution {

struct CMonster;
struct Player;
struct Monster;

namespace keeper {

enum class HeroClassKind : uint8_t {
	Warrior,
	Rogue,
	Sorcerer,
};
constexpr size_t NumHeroClasses = 3;

/** What the HUD shows about a monster hero (see GetHeroSummary). */
struct HeroSummary {
	HeroClassKind cls;
	/** Gold carried (-1: robbed nothing, but heading out anyway). */
	int carried;
	bool champion;
	bool fleeing;
};

/** Number of raids to survive; the last one is the champions' raid. */
constexpr uint8_t NumWaves = 7;

/** Registers the hero monster types (called right after monstdat.tsv is loaded). */
void RegisterHeroMonsterData();

bool IsHeroType(_monster_id type);

/** Loads hero sprites for a level monster type. @return true if the type is a hero type (handled). */
bool InitHeroGFX(CMonster &monsterType);

/** Adds the hero types to the level's monster types (keep level only). */
void AddHeroMonsterTypes();

/** Called whenever a monster is (re)initialised on any client: applies wave stats to heroes, remembers homes. */
void OnMonsterInit(Monster &monster);

/** Called from MonsterDeath on every client. */
void OnMonsterDeath(Monster &monster);

/** The keeper's override of UpdateEnemy (faction + leash rules). */
void UpdateEnemy(Monster &monster);

/** MonsterAIID::KeeperHero */
void HeroAi(Monster &monster);

/** Keep monsters that lost their target drift back towards their lair. */
void LeashMonsters();
/** Where a keep monster stands guard (its lair, or the lair's rally point). */
void SetMonsterHome(size_t monsterId, Point tile);
/** A patrolling monster looks for enemies around itself: its "home" is only the next waypoint. */
void SetMonsterRoaming(size_t monsterId, bool roaming);

/** A keep monster with nothing to fight: it guards its lair instead of running its stock AI. */
bool ShouldGuard(const Monster &monster);
void GuardAi(Monster &monster);

void ResetHeroes();

/** Host: queues the party for a wave. */
void StartWave(uint8_t wave);

enum class RaidStatus : uint8_t {
	Ongoing,
	Repelled,
};

/** Host: spawns queued heroes and reports whether the raid is over. */
RaidStatus UpdateRaid();

/** True when no living hero is carrying stolen gold. */
bool AllThievesGone();

/** Gold currently carried by heroes (for the HUD). */
int GoldInFlight();

int LivingHeroCount();

std::string_view HeroTitle(const Monster &monster);
std::string_view HeroClassName(HeroClassKind cls);
/** The hero's state, or nullopt if `monster` isn't a living hero. */
std::optional<HeroSummary> GetHeroSummary(const Monster &monster);

/** Scouting report for a wave, e.g. "Next raid: 2 Warriors, 1 Rogue". */
std::string DescribeWave(uint8_t wave);

/** Recomputes the vault/entrance distance fields (after level load, or when doors change). */
void RebuildPaths();

// ---- the Keep Lord and the heroes' way out --------------------------------
/** The boss guarding the vault. Heroes win by killing it and escaping through a Town Portal. */
bool IsBoss(const Monster &monster);
/** The living Keep Lord, if any. */
const Monster *FindBoss();
/** Host only: (re)spawns the Keep Lord at the vault when there is none and it isn't down. */
void UpdateBoss();
/** Every client: once the Lord is down a hero channels a portal; then the party runs for it. */
void UpdatePortal();
/** Closes the heroes' portal and forgets any channel (end of raid). */
void ClosePortal();
/** Where the open portal stands, if any. */
std::optional<Point> PortalPosition();
/** Percent of the portal channel completed (-1 = nobody channelling). */
int PortalChannelPercent();
/** Where whoever is channelling the portal stands. */
std::optional<Point> PortalCasterPosition();
bool IsPortalCaster(const Player &player);
/** True for the hero monster channelling the portal. */
bool IsPortalCaster(const Monster &monster);

// ---- shared hero helpers (used by the bot heroes) -------------------------
enum class HeroGoal : uint8_t {
	Vault,
	Entrance,
	Portal,
};
/** Steps to the goal along the hero paths (0xFFFF = unreachable). */
uint16_t GoalDistance(HeroGoal goal, Point tile);
/** A tile up to `steps` steps closer to the goal along the hero paths. */
std::optional<Point> GoalWaypoint(HeroGoal goal, Point from, int steps);
/** Opens the unlocked closed doors next to a hero. */
void OpenDoorsAround(Point tile);
/** A locked, closed door next to `from` on the way to the goal. */
std::optional<Point> LockedDoorOnPath(HeroGoal goal, Point from);
void StrikeLockedDoor(Point doorTile, int blows, std::string_view who);
/** Takes up to `carryCap` gold out of the vault. @return the amount taken */
int TakeFromVault(int carryCap);
int CarryCapacity(HeroClassKind cls);
/** A living monster of the keep's side (heroes attack these). */
bool IsKeepTarget(const Monster &monster);
std::vector<HeroClassKind> PartyForWave(uint8_t wave);

// ---- possession -----------------------------------------------------------

/** One of the keep's own monsters, alive, not already possessed. */
bool CanPossess(const Monster &monster);
bool IsPossessed(const Monster &monster);
/** Replaces the monster's own AI while a master is in control. */
void PossessedAi(Monster &monster);
void SetOrder(size_t playerId, Point tile, int targetMonster);
void ClearOrder(size_t playerId);
void EndPossession(size_t playerId);

} // namespace keeper
} // namespace devilution
