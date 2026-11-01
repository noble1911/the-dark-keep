/**
 * @file keeper/keeper_game.hpp
 *
 * The Dark Keep's simulation: phases, economy, lairs, master spells, and the
 * network protocol that keeps co-op masters in step.
 *
 * Authority model: the "keep host" is the level owner (lowest active player id on
 * the keep). Masters send requests (build, ready...) to everyone; only the host
 * acts on them and broadcasts the outcome (lair records, monster spawns via the
 * engine's own CMD_SPAWNMONSTER, and a periodic state snapshot). Spells are the
 * exception: like ordinary spell casts they run on every client.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "engine/point.hpp"
#include "tables/monstdat.h"
#include "tables/spelldat.h"

namespace devilution {

struct Monster;
struct Player;
struct TCmd;

namespace keeper {

enum class Phase : uint8_t {
	Setup,
	Raid,
	Breather,
	Victory,
	Defeat,
};

std::string_view PhaseName(Phase phase);

struct LairKind {
	std::string_view name;
	_monster_id type;
	uint8_t count;
	int cost;
};

struct SpellKind {
	std::string_view name;
	MissileID missile;
	int manaCost;
	int spellLevel;
	/** Cooldown in game ticks (per master). */
	int cooldownTicks;
};

constexpr size_t NumLairKinds = 5;
constexpr size_t NumSpellKinds = 4;
extern const std::array<LairKind, NumLairKinds> LairKinds;
extern const std::array<SpellKind, NumSpellKinds> SpellKinds;

constexpr size_t MaxLairs = 32;
constexpr size_t MaxLairMembers = 4;

struct Lair {
	bool active = false;
	uint8_t kind = 0;
	Point position;
	/** Monster ids of the lair's members (host-side bookkeeping only; -1 = empty slot). */
	std::array<int16_t, MaxLairMembers> members {};
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
	std::array<int, 4> mana {};
	std::array<int, 4> spellCooldown {};
	std::array<Lair, MaxLairs> lairs {};
	/** Where heroes arrive (up-stairs) and where the treasure vault stands (down-stairs). */
	Point entrance;
	Point vault;
};

extern KeepState State;

/** Sets up a fresh keep (called when the keep level loads). */
void ResetGame(Point entrance, Point vault);

/** Game-logic tick: phases, regen, lairs, heroes (host-driven parts only act on the host). */
void GameTick();

/** True on the client that owns the simulation of the keep (lowest player id on the level). */
bool IsHost();

/** @return true if a lair of this kind could be built here (checked locally before sending). */
bool CanBuildLairAt(size_t kind, Point tile, std::string_view *reason = nullptr);

/** Requests (all are network commands, also in single player). */
void RequestBuildLair(size_t kind, Point tile);
void RequestCastSpell(size_t spell, Point tile);
void RequestStartRaid();

/** Handles CMD_KEEPER / CMD_KEEPER_STATE. @return bytes consumed */
size_t HandleNetCommand(Player &player, const TCmd *cmd, size_t maxCmdSize);

/** Faction rules: heroes vs. the keep's monsters. */
bool IsHero(const Monster &monster);
bool AreHostile(const Monster &a, const Monster &b);

} // namespace keeper
} // namespace devilution
