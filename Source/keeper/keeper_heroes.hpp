/**
 * @file keeper/keeper_heroes.hpp
 *
 * AI heroes: raiding parties built from monster slots wearing the original hero
 * class sprites. They walk to the treasure vault, steal gold, and try to escape
 * back out the entrance; the keep's monsters (and the masters) try to stop them.
 */
#pragma once

#include <cstdint>
#include <string_view>

#include "tables/monstdat.h"

namespace devilution {

struct CMonster;
struct Monster;

namespace keeper {

enum class HeroClassKind : uint8_t {
	Warrior,
	Rogue,
	Sorcerer,
};
constexpr size_t NumHeroClasses = 3;

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

/** Recomputes the vault/entrance distance fields (after level load, or when doors change). */
void RebuildPaths();

} // namespace keeper
} // namespace devilution
