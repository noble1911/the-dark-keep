/**
 * @file keeper/keeper_heroes.hpp
 *
 * AI heroes: raiding parties built from monster slots wearing the original hero
 * class sprites. They walk to the treasure vault, steal gold, and try to escape
 * back out the entrance; the keep's monsters (and the masters) try to stop them.
 */
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <function_ref.hpp>

#include "engine/displacement.hpp"
#include "engine/point.hpp"
#include "keeper/keeper_campaign.hpp"
#include "levels/dun_tile.hpp" // IWYU pragma: keep
#include "levels/gendung_defs.hpp"
#include "tables/monstdat.h"

namespace devilution {

struct CMonster;
struct Player;
struct Monster;

namespace keeper {

// ---- how heroes get around: the map validator and the map tool use these too ---------------------

/** Steps to a target, per tile. */
using DistanceField = std::array<std::array<uint16_t, MAXDUNY>, MAXDUNX>;
constexpr uint16_t Unreachable = 0xFFFF;
/** A locked door is bashed only if the open way round is longer than this many extra steps. */
constexpr uint16_t LockRoutePenalty = 40;
/** A raid's heroes appear on free floor up to this far from the entrance, nearest first. */
constexpr unsigned HeroSpawnRadius = 6;

/** A hero can stand here, or will open the door here. */
bool IsPassable(Point tile);
bool CanStepDiagonal(Point from, Displacement step);
void BuildField(DistanceField &field, Point target);
/** BuildField, with some tiles shut. */
void BuildFieldAvoiding(DistanceField &field, Point target, tl::function_ref<bool(Point)> isShut);
/** Floor a hero could appear on, if nothing else is standing there. */
bool IsOpenFloor(Point tile);

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

/** Registers the hero monster types (called right after monstdat.tsv is loaded). */
void RegisterHeroMonsterData();

bool IsHeroType(_monster_id type);

/** Loads hero sprites for a level monster type. @return true if the type is a hero type (handled). */
bool InitHeroGFX(CMonster &monsterType);

/** Adds the hero types and this level's muster to the level's monster types (keep level only). */
void AddHeroMonsterTypes();
/** A keep's lair kinds are listed at level load, but only loaded when one first spawns (EnsureMonsterTypeLoaded). */
bool IsDeferredMonsterType(size_t typeIndex);
void ClearDeferredMonsterTypes();

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
/** A joiner's snapshot moved this monster: rebuilds what the join lost, without touching upgraded stats. */
void RefreshMonsterAfterSync(Monster &monster);
/** A patrolling monster looks for enemies around itself: its "home" is only the next waypoint. */
void SetMonsterRoaming(size_t monsterId, bool roaming);

/** A keep monster with nothing to fight: it guards its lair instead of running its stock AI. */
bool ShouldGuard(const Monster &monster);
/** A keep monster with a target: every kind finds its way there, not only those the base game lets search. */
bool HuntsEnemy(const Monster &monster);
/** True while a monster's own path planning must stay out of the way: it guards its post, or a master drives it. */
bool HoldsPost(const Monster &monster);
void GuardAi(Monster &monster);

void ResetHeroes();

/** What a hero has been through, for a master joining mid-raid. */
struct HeroSyncState {
	bool fleeing = false;
	bool fearRolled = false;
	bool fellBack = false;
	int carried = 0;
	uint8_t baitsVisited = 0;
	uint16_t trapsKnown = 0;
	uint16_t trapsSeen = 0;
	int fallBackSteps = 0;
	int regroupBudget = 0;
};
/** Nullopt if the monster isn't an active, living hero. */
std::optional<HeroSyncState> GetHeroSyncState(size_t monsterId);
void ApplyHeroSyncState(size_t monsterId, const HeroSyncState &state);

/** The heroes' way out, for a master joining mid-raid. */
struct PortalSyncState {
	bool open = false;
	bool awaitingBotCast = false;
	/** Bot slot or monster id channelling (-1 = none). */
	int botSlot = -1;
	int casterMonster = -1;
	int channel = 0;
	int castTimeout = 0;
	Point position;
};
PortalSyncState GetPortalSyncState();
void ApplyPortalSyncState(const PortalSyncState &state);/** The host's copy of a monster hero got out: a joiner's does too, with the host's facts. */
void ApplyHeroEscaped(size_t monsterId, bool throughPortal, bool fleeing, int carried);

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

/** Recomputes the vault/entrance distance fields and drops the cached routes (after level load, or when locks change). */
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
/** Plain walking distance to the goal, ignoring traps and locks (0xFFFF = unreachable). For "have I arrived?". */
uint16_t GoalDistance(HeroGoal goal, Point tile);
/**
 * A tile up to `steps` steps along the route to the goal, for a hero who knows the traps in `knownTraps`
 * (bits of State.traps). The route shuns those traps and locked doors, and stops short of a locked door.
 */
std::optional<Point> GoalWaypoint(HeroGoal goal, Point from, int steps, uint16_t knownTraps);
/** Opens the unlocked closed doors next to a hero. */
void OpenDoorsAround(Point tile);
/** A locked, closed door next to `from` that is this hero's cheapest way on: time to bash it. */
std::optional<Point> LockedDoorOnPath(HeroGoal goal, Point from, uint16_t knownTraps);
void StrikeLockedDoor(Point doorTile, int blows, std::string_view who);
/** Damage one swing does to a locked door. */
int BashBlows(HeroClassKind cls);

/** How long a hero stands gawping at fool's gold, in game ticks. */
constexpr int BaitLingerTicks = 40;
/** The fool's gold pile (slot in State.baits) this class would go for from `position`, if any. */
std::optional<size_t> FindBaitFor(HeroClassKind cls, Point position, uint8_t baitsVisited);
/** The hero reached the pile: uses it up and marks it visited. */
void ConsumeBait(size_t slot, uint8_t &baitsVisited, std::string_view who);

/** Every client, from UpdateTraps: heroes who can see the trap learn to route around it. */
void NoteTrapFired(size_t trapSlot);

// Morale, shared by both kinds of hero. No healer class exists, so a wounded hero falls back on a healthy ally.
constexpr int FallBackHealthPercent = 40;
constexpr int HealthyAllyPercent = 60;
constexpr int FallBackAllyRange = 12;
constexpr int FallBackArriveDistance = 2;
/** Living hero with at least HealthyAllyPercent health, still on the heist, nearest `from` within `range`. */
std::optional<Point> NearestHealthyHero(Point from, int range);
/** Ticks of waiting each hero has to spend on letting the party catch up: it can never stall the raid for long. */
constexpr int RegroupBudgetTicks = 10 * 20;
/** True if a hero at `position` is well ahead of the rearmost hero still heading for the vault. */
bool IsAheadOfParty(Point position);
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
