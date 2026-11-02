/**
 * @file keeper/keeper_bots.hpp
 *
 * Real player heroes for The Dark Keep: AI-driven Player structs (class sprites, starting
 * gear, real attacks, spells and Town Portal) that raid the keep alongside the monster heroes.
 *
 * Bots live in Players[] after the network slots (ids MAX_PLRS .. MAX_PLRS + MaxBots - 1), so
 * two co-op masters and four bot heroes fit at once. No client owns a bot: every client spawns
 * it from the host's command and runs the same AI on it, the way monsters are simulated, so
 * the engine's "only the owning client applies this" checks treat bots as local everywhere
 * (see SimulatesLocally).
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "engine/point.hpp"
#include "keeper/keeper_heroes.hpp"
#include "multi.h"

namespace devilution {

struct Monster;
struct Player;

namespace keeper {

constexpr size_t MaxBots = 4;
constexpr size_t FirstBotId = MAX_PLRS;

/** A bot standing in a floor hazard. Burns don't stagger it: it walks straight out. */
bool IsHeroInHazard(const Player &player);
/** Moves a standing bot to `tile` at once (heroes changing places in a corridor). */
void PlaceBot(Player &bot, Point tile);
/** Caps a bot's block chance (percent); anyone else's passes through. */
int CapBotBlock(const Player &player, int chance);
/** True for the AI hero players (not the masters). */
bool IsBot(const Player &player);
/** True for an active bot hero that is still alive. */
bool IsLivingBot(const Player &player);
/** A joiner's copy of a bot: fights differ between clients, and it only dies when the host's does. */
bool HoldsBotAlive(const Player &player);
/** The host's word on a bot's life, for BroadcastState: its hit points, or a negative for no such bot or not yet known. */
int32_t BotLifeForSync(size_t slot);
void ApplyBotLifeSync(size_t slot, int32_t hitPoints);
struct BotSyncFacts {
	Point tile;
	uint8_t potions;
};
BotSyncFacts BotFactsForSync(size_t slot);
/** A joiner's copy of a bot follows the host's: left alone when near, walks over when not, jumps when far. */
void ApplyBotPositionSync(size_t slot, Point hostTile, uint8_t potions);
/** The host's copy got out: a joiner's does too, however far it had come, with the host's facts. */
void ApplyBotLooted(size_t slot, int amount);
void ApplyBotFled(size_t slot);
void ApplyBotEscaped(size_t slot, bool throughPortal, bool fleeing, int carried);
/** True if this client applies the player's damage/effects itself: the local master, or any bot. */
bool SimulatesLocally(const Player &player);

/** A free bot slot (host side), if any. */
std::optional<size_t> FreeBotSlot();
/** Every client: brings bot `slot` into the keep as a hero of class `cls` at `tile`. */
void SpawnBot(size_t slot, HeroClassKind cls, Point tile);
/** Every client: removes all bots (end of raid, new game). */
void RemoveAllBots();
/** Every client, every game tick: bot AI, potions, deaths and escapes. */
void UpdateBots();

struct BotSummary {
	HeroClassKind cls;
	/** Gold carried (-1: robbed nothing, but heading out anyway). */
	int carried;
	bool fleeing;
	int potions;
};
/** The bot's AI state, or nullopt if `player` isn't a living bot. */
std::optional<BotSummary> GetBotSummary(const Player &player);

int LivingBotCount();
int BotGoldInFlight();
/** Living bot nearest `from` within `range` tiles (for the keep's monsters to attack). */
const Player *NearestBot(Point from, int range);
/** Every client: the masters' Stone Curse turns a bot to stone for `ticks` game ticks. */
bool PetrifyBot(Player &bot, int ticks);
bool IsPetrifiedBot(const Player &player);
/** Any living bot within `distance` tiles of `tile`. */
bool IsBotNear(Point tile, int distance);
/** Every client: bots who can see trap `trapSlot` at `trapPosition` fire learn to route around it. */
void BotsWitnessTrap(size_t trapSlot, Point trapPosition, int range);
/** Healthy bot nearest `from` within `range`, still on the heist. */
std::optional<Point> NearestHealthyBot(Point from, int range);
/** Walking distance to the vault of the rearmost bot still heading there (0 = none). */
uint16_t BotRearVaultDistance();

/** The portal channel is done: the bot casts the real Town Portal spell. */
void BotCastTownPortal(Player &bot);

/** Factions: bot weapons and spells hit keep monsters; masters' spells hit hero monsters and bots. */
bool PlayerCanHitMonster(const Player &source, const Monster &target);
bool PlayerCanHitPlayer(const Player &source, const Player &target);
/** Hero monsters never hurt bots (same side); keep monsters and traps do. */
bool MonsterCanHitPlayer(const Monster &source, const Player &target);

} // namespace keeper
} // namespace devilution
