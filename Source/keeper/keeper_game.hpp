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
#include <string>
#include <string_view>

#include "engine/point.hpp"
#include "tables/monstdat.h"
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

/** Everything a master can place from the build row. */
enum class BuildKind : uint8_t {
	Zombies,
	Fallen,
	Skeletons,
	Archers,
	Overlord,
	FireTrap,
	LastLair = Overlord,
};
constexpr size_t NumBuildKinds = 6;

struct BuildInfo {
	std::string_view name;
	int cost;
	/** Lairs only: the monster type and how many the lair keeps alive. */
	_monster_id type;
	uint8_t count;
};
extern const std::array<BuildInfo, NumBuildKinds> BuildKinds;

constexpr bool IsLairKind(size_t kind)
{
	return kind <= static_cast<size_t>(BuildKind::LastLair);
}

struct SpellKind {
	std::string_view name;
	MissileID missile;
	int manaCost;
	int spellLevel;
	/** Cooldown in game ticks (per master). */
	int cooldownTicks;
};
constexpr size_t NumSpellKinds = 4;
extern const std::array<SpellKind, NumSpellKinds> SpellKinds;

constexpr int LockDoorCost = 50;
constexpr int BaitCost = 60;

constexpr size_t MaxLairs = 32;
constexpr size_t MaxLairMembers = 4;
constexpr size_t MaxTraps = 16;
constexpr size_t MaxLocks = 16;
constexpr size_t MaxBaits = 8;

struct Lair {
	bool active = false;
	uint8_t kind = 0;
	Point position;
	/** Ticks until a lair placed mid-raid finishes summoning. */
	int summonTicks = 0;
	/** Monster ids of the lair's members (host-side bookkeeping only; -1 = empty slot). */
	std::array<int16_t, MaxLairMembers> members {};
};

struct Trap {
	bool active = false;
	Point position;
	/** Ticks until the trap can fire again (also used while it is being summoned). */
	int cooldown = 0;
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
	std::array<Lair, MaxLairs> lairs {};
	std::array<Trap, MaxTraps> traps {};
	std::array<DoorLock, MaxLocks> locks {};
	std::array<Bait, MaxBaits> baits {};
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

/** Placement checks (run locally before sending; the host re-checks). */
bool CanBuildAt(size_t kind, Point tile, std::string_view *reason = nullptr);
bool CanLockDoorAt(Point tile, std::string_view *reason = nullptr);
bool CanPlaceBaitAt(Point tile, std::string_view *reason = nullptr);

/** The closed door object at/next to a tile, if any. */
Object *FindDoorAt(Point tile);
DoorLock *FindLock(Point doorPosition);

/** Requests (all are network commands, also in single player). */
void RequestBuild(size_t kind, Point tile);
void RequestLockDoor(Point doorPosition);
void RequestPlaceBait(Point tile);
void RequestCastSpell(size_t spell, Point tile);
void RequestStartRaid();

/** Handles CMD_KEEPER / CMD_KEEPER_STATE. @return bytes consumed */
size_t HandleNetCommand(Player &player, const TCmd *cmd, size_t maxCmdSize);

/** Faction rules: heroes vs. the keep's monsters. */
bool IsHero(const Monster &monster);
bool AreHostile(const Monster &a, const Monster &b);

/** Pays the keep (bounties, tribute, dropped loot). */
void AddVaultGold(int amount);

} // namespace keeper
} // namespace devilution
