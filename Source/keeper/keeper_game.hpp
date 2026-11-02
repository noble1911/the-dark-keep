/**
 * @file keeper/keeper_game.hpp
 *
 * The Dark Keep's simulation: phases, economy, lairs, traps, doors, bait,
 * master spells, and the network protocol that keeps co-op masters in step.
 *
 * Authority model: the "keep host" is the level owner (lowest active player id on
 * the keep). Masters send requests (build, lock, ready...) to everyone; only the
 * host acts on them and broadcasts the outcome (placement events, monster spawns
 * via the engine's own CMD_SPAWNMONSTER, and a periodic state snapshot). Spells
 * are the exception: like ordinary spell casts they run on every client.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "engine/point.hpp"
#include "tables/monstdat.h"
#include "keeper/keeper_heroes.hpp"
#include "keeper/keeper_roster.hpp"
#include "tables/spelldat.h"

namespace devilution {

struct Monster;
struct Object;
struct Player;
struct TCmd;

// Exported from objects.cpp for the keeper.
void KeeperOperateDoor(Object &door, bool sendflag);
bool KeeperIsDoorClosed(const Object &door);

namespace keeper {

enum class Phase : uint8_t {
	Setup,
	Raid,
	Breather,
	Victory,
	Defeat,
};

std::string_view PhaseName(Phase phase);

/**
 * Everything a master can place from the build row: the lair kinds of the roster (keeper_roster.hpp),
 * then the trap. Kind indexes aren't stable and only some kinds are loaded per level: use IsLairKind(),
 * BuildKind::FireTrap, MusterKinds() and IsKindMustered() rather than assuming a list.
 */
enum class BuildKind : uint8_t {
	LastLair = NumLairKinds - 1,
	FireTrap,
};

/** Trap types, in build-kind order after the lairs. Trap::kind and the trap levels index this. */
struct TrapKind {
	std::string_view name;
	int cost;
	int unlockRank;
	int rearmTicks;
	/** Damage at level 1: base + perWave * raid number, in whole hit points. */
	int baseDamage;
	int damagePerWave;
	SpellID icon;
	std::string_view flavor;
};
constexpr size_t NumTrapKinds = 4;
constexpr size_t SnareTrapKind = 3;
constexpr std::array<TrapKind, NumTrapKinds> TrapKinds { {
	{ "Fire Trap", 100, 1, 8 * 20, 20, 5, SpellID::RuneOfFire, "Breathes fire at heroes who walk past and leaves a burning wall." },
	{ "Arrow Trap", 75, 1, 3 * 20, 6, 2, SpellID::TrapDisarm, "A hidden bow: light damage, but ready again almost at once." },
	{ "Shock Trap", 140, 6, 6 * 20, 4, 2, SpellID::RuneOfLight, "Looses a bolt of lightning at the hero who set it off." },
	{ "Snare", 150, 11, 12 * 20, 0, 0, SpellID::RuneOfStone, "No damage: it holds the hero in place for your monsters to finish." },
} };
constexpr size_t NumBuildKinds = NumLairKinds + NumTrapKinds;

struct BuildInfo {
	std::string_view name;
	int cost;
	/** Lairs only: the monster type and how many the lair keeps alive. */
	_monster_id type;
	uint8_t count;
	/** Lairs only: room the monster takes up in the keep (see SupplyCap). */
	uint8_t supply;
};
/** Defined in keeper_roster.cpp; prices are computed from the monster data when it loads. */
extern std::array<BuildInfo, NumBuildKinds> BuildKinds;

constexpr bool IsLairKind(size_t kind)
{
	return kind <= static_cast<size_t>(BuildKind::LastLair);
}

struct SpellKind {
	std::string_view name;
	MissileID missile;
	int manaCost;
	/** The engine's spell level when this spell is at level 1. */
	int spellLevel;
	/** Cooldown in game ticks (per master). */
	int cooldownTicks;
	int unlockRank;
	SpellID icon;
	std::string_view flavor;
};
constexpr size_t NumSpellKinds = 11;
/** The spell bar: what a master has equipped from the Spellbook. */
constexpr size_t NumSpellSlots = 6;
extern const std::array<SpellKind, NumSpellKinds> SpellKinds;

/** Like spell levels in the base game: every spell and trap type levels up on its own, with Renown. */
constexpr int MaxKeepLevel = 15;
/** A spell's mana cost at its level: it falls as the spell levels up, as in the base game. */
int SpellManaCost(size_t spell);
int SpellManaCostAt(size_t spell, int level);
/** What a cast really takes, in hundredths like State.mana. The two above round it up for display. */
int SpellManaCostScaled(size_t spell);
/** The engine's spell level a spell casts at, for its level in this game. */
int SpellEngineLevel(size_t spell);

constexpr int LockDoorCost = 50;
/** Nothing can be placed this close to the entrance or the vault (tiles). */
constexpr int EntranceKeepOut = 4;
constexpr int VaultKeepOut = 3;
constexpr int BaitCost = 60;

constexpr size_t MaxLairs = 96;
constexpr size_t MaxLairMembers = 4;
constexpr size_t MaxTraps = 16;
constexpr size_t MaxLocks = 16;
constexpr size_t MaxBaits = 8;
/** Waypoints a lair's patrol route can hold, on top of its guard post. */
constexpr size_t MaxPatrolPoints = 6;

struct Lair {
	bool active = false;
	uint8_t kind = 0;
	Point position;
	/** Ticks until a lair placed mid-raid finishes summoning. */
	int summonTicks = 0;
	/** Monster ids of the lair's members (the host announces them to everyone; -1 = empty slot). */
	std::array<int16_t, MaxLairMembers> members {};
	/** Where the lair's monsters stand guard, if the master set a rally point. */
	bool hasRally = false;
	Point rally;
	/** Patrol: the monsters walk the loop guard post, route[0], route[1]... while they have no enemy. */
	std::array<Point, MaxPatrolPoints> route {};
	uint8_t routeCount = 0;
	/** The leg each member is on: 0 is the guard post, n is route[n - 1]. */
	std::array<uint8_t, MaxLairMembers> patrolLeg {};
};

struct Trap {
	bool active = false;
	Point position;
	/** Ticks until the trap can fire again (also used while it is being summoned). */
	int cooldown = 0;
	/** Index into TrapKinds. */
	uint8_t kind = 0;
};

struct DoorLock {
	bool active = false;
	Point position;
	/** Blows left before the door gives way. */
	int strength = 0;
};

struct Bait {
	bool active = false;
	Point position;
	/** Heroes that may still be fooled before the pile is spent. */
	int uses = 0;
};

/** Mana is stored in hundredths so per-tick regeneration stays integral. */
constexpr int ManaScale = 100;
constexpr int MaxMana = 100;

struct KeepState {
	Phase phase = Phase::Setup;
	/** Game ticks left in the current timed phase. */
	int phaseTicks = 0;
	/** 1-based number of the current (or next, while not raiding) wave. */
	uint8_t wave = 1;
	int vaultGold = 0;
	/** Gold carried out of the keep by escaped thieves, this game. */
	int goldLost = 0;
	std::array<int, 4> mana {};
	std::array<int, 4> spellCooldown {};
	/** Levels of every spell and trap type in this game (see GameSpellLevel). 0 = not available. */
	std::array<uint8_t, NumSpellKinds> spellLevels {};
	std::array<uint8_t, NumTrapKinds> trapLevels {};
	std::array<Lair, MaxLairs> lairs {};
	std::array<Trap, MaxTraps> traps {};
	std::array<DoorLock, MaxLocks> locks {};
	std::array<Bait, MaxBaits> baits {};
	/** Monster id each master is possessing (-1 = none). */
	std::array<int16_t, 4> possessed { -1, -1, -1, -1 };
	/** The Keep Lord has fallen this raid (heroes now try to portal out). */
	bool bossDown = false;
	/** A hero escaped through the portal after the Lord fell: the keep is lost. */
	bool portalEscape = false;
	/** Where heroes arrive (up-stairs) and where the treasure vault stands (down-stairs). */
	Point entrance;
	Point vault;
};

extern KeepState State;

/** Sets up a fresh keep (called when the keep level loads). */
void ResetGame(Point entrance, Point vault);

/** Game-logic tick: phases, regen, traps, summoning (host-driven parts only act on the host). */
void GameTick();

/** True on the client that owns the simulation of the keep (lowest player id on the level). */
bool IsHost();
/** A client that came into a game in play hasn't had the keep's state yet: its phase is only a guess. */
bool IsAwaitingKeepSnapshot();

/**
 * Keep monsters follow the host alone. Left to the engine's rule (whoever's player is nearest wins) a joiner
 * who no longer sees a hero could walk the host's Keep Lord away from that hero for ever, and the raid never ends.
 */
bool TakesMonsterSyncFrom(const Player &sender);
/** The host's position for this monster would interrupt what it is doing: a master drives it, or it is in mid-attack. */
bool IgnoresMonsterSyncMove(const Monster &monster);

/** Placement checks (run locally before sending; the host re-checks). */
bool CanBuildAt(size_t kind, Point tile, std::string_view *reason = nullptr);
bool CanLockDoorAt(Point tile, std::string_view *reason = nullptr);
bool CanPlaceBaitAt(Point tile, std::string_view *reason = nullptr);

/** The closed door object at/next to a tile, if any. */
Object *FindDoorAt(Point tile);
DoorLock *FindLock(Point doorPosition);

/** Population: room the keep's lairs take up, and how much the keep holds (grows every raid). */
int SupplyUsed();
int SupplyCap();

/** Something a master placed, for selling and rally points. */
enum class StructureType : uint8_t {
	Lair,
	Trap,
	Bait,
	Lock,
};
struct StructureRef {
	StructureType type;
	size_t slot;
	bool operator==(const StructureRef &other) const = default;
};
bool IsStructureActive(StructureRef ref);
Point StructurePosition(StructureRef ref);
std::string_view StructureName(StructureRef ref);
/** Gold back for selling: the full price outside a raid, half during one (bait and locks pro rata). */
int RefundFor(StructureRef ref);
/** The structure on (or, for locks, the door at) `tile`. */
std::optional<StructureRef> FindStructureAt(Point tile);
/** The lair a keep monster belongs to. */
std::optional<size_t> LairOfMonster(size_t monsterId);
bool CanSell(StructureRef ref, std::string_view *reason = nullptr);
bool CanRallyTo(size_t lair, Point tile, std::string_view *reason = nullptr);
bool CanAddWaypoint(size_t lair, Point tile, std::string_view *reason = nullptr);

/** Requests (all are network commands, also in single player). */
void RequestSell(StructureRef ref);
/** Sends lair `lair`'s monsters to guard `tile` (nullopt: back to the lair). Either way this ends its patrol. */
void RequestRally(size_t lair, std::optional<Point> tile);
/** Adds `tile` to the end of the patrol route of lair `lair`. */
void RequestWaypoint(size_t lair, Point tile);
void RequestBuild(size_t kind, Point tile);
void RequestLockDoor(Point doorPosition);
void RequestPlaceBait(Point tile);
void RequestCastSpell(size_t spell, Point tile);
void RequestStartRaid();
/** The game's creator, once the siege is over: everyone leaves and meets again in `next`. */
void RequestRestart(const KeepSetup &next);
void RequestPossess(size_t monsterId);
void RequestRelease();
/** Possessed monster: walk to `tile`, or attack `targetMonster` (-1 = just walk). */
void RequestOrder(Point tile, int targetMonster);
/** Host: every client brings bot hero `slot` into the keep. */
void RequestSpawnBot(size_t slot, HeroClassKind cls, Point tile);

constexpr int PossessManaCost = 10;

/** Dev/test (--keeper-devctl): sets off a trap at a tile, whoever stands there. */
void DevFireTrap(size_t slot, Point victim);

/** Host: a hero left the keep (`carried` is its gold). Joiners can't work this out for themselves. */
/** Host: a Rogue disarmed a trap (botSlot empty: a monster hero's). Everyone hears of it. */
void DisarmTrap(size_t trapSlot, std::optional<size_t> botSlot);
void NotifyBotLooted(size_t slot, int amount);
void NotifyBotFled(size_t slot);
void NotifyHeroLooted(size_t monsterId, int amount);
void NotifyHeroFled(size_t monsterId);
void NotifyBotEscaped(size_t slot, bool throughPortal, bool fleeing, int carried);
void NotifyHeroEscaped(size_t monsterId, bool throughPortal, bool fleeing, int carried);

/** Handles CMD_KEEPER / CMD_KEEPER_STATE. @return bytes consumed */
size_t HandleNetCommand(Player &player, const TCmd *cmd, size_t maxCmdSize);

/** A player finished loading a level: if it's a master arriving in the keep, whoever runs the keep sends them all of it. */
void OnPlayerJoinedLevel(const Player &player);

/** Faction rules: heroes vs. the keep's monsters. */
bool IsHero(const Monster &monster);
bool AreHostile(const Monster &a, const Monster &b);
/** True if a wall of fire burns on `tile` (heroes step out of it and won't walk into it). */
bool IsBurning(Point tile);
/** A living hero (monster or bot) stands on `tile`. */
bool IsHeroAt(Point tile);

/** Pays the keep (bounties, tribute, dropped loot). */
void AddVaultGold(int amount);

} // namespace keeper
} // namespace devilution
