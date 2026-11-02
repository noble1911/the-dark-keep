/**
 * @file keeper/keeper_bots.cpp
 *
 * Real player heroes for The Dark Keep. See keeper_bots.hpp.
 */
#include "keeper/keeper_bots.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <string_view>

#include <fmt/format.h>

#include "engine/path.h"
#include "engine/random.hpp"
#include "items.h"
#include "keeper/keeper.hpp"
#include "keeper/keeper_game.hpp"
#include "keeper/keeper_raid.hpp"
#include "levels/gendung.h"
#include "levels/tile_properties.hpp"
#include "lighting.h"
#include "missiles.h"
#include "monster.h"
#include "player.h"
#include "plrmsg.h"
#include "spells.h"
#include "utils/utf8.hpp"

namespace devilution::keeper {

namespace {

constexpr int BotSight = 7;
constexpr int SorcererRange = 8;
constexpr int RogueRange = 9;
constexpr int ThinkInterval = 6;
constexpr int RogueDisarmPercent = 60;
constexpr int RogueSpotRange = 3;
constexpr int FearHealthDivisor = 4;
constexpr int FearChancePercent = 50;
/** Most AI decisions a bot spends falling back, about 9 steps. */
constexpr int FallBackMaxThinks = 12;
/** Near a known trap, walk the route tile by tile: the engine's path to a far waypoint would cut through it. */
constexpr int CarefulTrapRange = 9;

struct BotState {
	HeroClassKind cls = HeroClassKind::Warrior;
	/** Gold carried (-1: robbed nothing, but heading out anyway). */
	int carried = 0;
	bool fleeing = false;
	bool fearRolled = false;
	bool deathHandled = false;
	int potions = 0;
	int thinkCooldown = 0;
	/** Traps a Rogue has rolled to disarm. */
	uint16_t trapsSeen = 0;
	/** Traps this bot saw go off or spotted: its route avoids them. */
	uint16_t trapsKnown = 0;
	/** Fool's gold piles already visited. */
	uint8_t baitsVisited = 0;
	/** Game ticks left gawping at fool's gold. */
	int baitLingerTicks = 0;
	bool fellBack = false;
	int fallBackThinks = 0;
	int regroupBudget = RegroupBudgetTicks;
	/** Game ticks left turned to stone (the masters' Stone Curse). */
	int petrifiedTicks = 0;
};

std::array<BotState, MaxBots> Bots;
/** Host side: slots requested but not spawned yet (the command is still in flight). */
std::array<bool, MaxBots> PendingSpawn {};

constexpr std::array<std::array<std::string_view, 4>, NumHeroClasses> BotNames { {
	{ "Sir Aldric", "Brannoc", "Garrick", "Thorne" },
	{ "Lyra", "Vessa", "Kestrel", "Wren" },
	{ "Malachar", "Ysolde", "Corvin", "Isembard" },
} };

HeroClass ToHeroClass(HeroClassKind cls)
{
	switch (cls) {
	case HeroClassKind::Rogue:
		return HeroClass::Rogue;
	case HeroClassKind::Sorcerer:
		return HeroClass::Sorcerer;
	case HeroClassKind::Warrior:
	default:
		return HeroClass::Warrior;
	}
}

std::string_view ClassTitle(HeroClassKind cls)
{
	switch (cls) {
	case HeroClassKind::Rogue:
		return "Rogue";
	case HeroClassKind::Sorcerer:
		return "Sorcerer";
	case HeroClassKind::Warrior:
	default:
		return "Warrior";
	}
}

void Announce(std::string_view message, UiFlags color = UiFlags::ColorRed)
{
	EventPlrMsg(message, color);
}

size_t SlotOf(const Player &player)
{
	return player.getId() - FirstBotId;
}

/**
 * A percentile roll every client agrees on. Bots think outside the monster loop's per-monster seeds,
 * where the global RNG isn't in step between clients, so their decisions must not draw from it.
 */
int SyncedRoll(const Player &bot, uint32_t salt)
{
	uint32_t hash = (static_cast<uint32_t>(bot.getId()) + 1) * 2654435761U;
	hash ^= (static_cast<uint32_t>(State.wave) + 1) * 2246822519U;
	hash ^= (salt + 1) * 3266489917U;
	hash ^= hash >> 15;
	hash *= 2654435761U;
	hash ^= hash >> 13;
	return static_cast<int>(hash % 100);
}

constexpr uint32_t FearRollSalt = 0x46454152;

void ClearBotFromMap(Player &bot)
{
	const int id = static_cast<int>(bot.getId()) + 1;
	for (const Point tile : { Point(bot.position.tile), Point(bot.position.future), Point(bot.position.old) }) {
		if (!InDungeonBounds(tile))
			continue;
		if (std::abs(dPlayer[tile.x][tile.y]) == id)
			dPlayer[tile.x][tile.y] = 0;
		dFlags[tile.x][tile.y] &= ~DungeonFlag::DeadPlayer;
	}
}

void RemoveBot(Player &bot)
{
	if (!bot.plractive)
		return;
	ClearBotFromMap(bot);
	ClrPlrPath(bot);
	bot.destAction = ACTION_NONE;
	bot.plractive = false;
	Bots[SlotOf(bot)] = {};
}

void ScaleBot(Player &bot, bool champion)
{
	const int wave = std::max<int>(State.wave, 1);
	bot.setCharacterLevel(static_cast<uint8_t>(std::min(wave * 2 - 1, 30)));
	const int bonus = ScaleByHeroStrength(3 * (wave - 1));
	bot._pBaseStr += bonus;
	bot._pBaseDex += bonus;
	bot._pBaseVit += bonus;
	bot._pBaseMag += bonus;
	bot._pMaxHPBase = bot.calculateBaseLife();
	if (champion)
		bot._pMaxHPBase = bot._pMaxHPBase * 3 / 2;
	bot._pMaxHPBase = ScaleByHeroStrength(bot._pMaxHPBase);
	bot._pHPBase = bot._pMaxHPBase;
	bot._pMaxManaBase = std::max(bot.calculateBaseMana(), 60 << 6);
	bot._pManaBase = bot._pMaxManaBase;
	CalcPlrInv(bot, false);
	SetPlayerHitPoints(bot, bot._pMaxHP);
	bot._pMana = bot._pMaxMana;
}

void LearnSpell(Player &bot, SpellID spell, uint8_t level)
{
	bot._pMemSpells |= GetSpellBitmask(spell);
	bot._pSplLvl[static_cast<int8_t>(spell)] = std::max(bot._pSplLvl[static_cast<int8_t>(spell)], level);
}

void QueueSpell(Player &bot, SpellID spell)
{
	// Bots never run dry: they are heroes on a mission, not mana accountants.
	bot._pMana = bot._pMaxMana;
	bot._pManaBase = bot._pMaxManaBase;
	bot.queuedSpell.spellId = spell;
	bot.queuedSpell.spellType = SpellType::Spell;
	bot.queuedSpell.spellFrom = 0;
	bot.queuedSpell.spellLevel = bot.GetSpellLevel(spell);
}

Direction WalkDirection(int8_t step)
{
	switch (step) {
	case WALK_N: return Direction::North;
	case WALK_NE: return Direction::NorthEast;
	case WALK_E: return Direction::East;
	case WALK_SE: return Direction::SouthEast;
	case WALK_S: return Direction::South;
	case WALK_SW: return Direction::SouthWest;
	case WALK_W: return Direction::West;
	default: return Direction::NorthWest;
	}
}

/** Heroes aren't stupid: stop before walking into a wall of fire (unless already in one). */
void AvoidFire(Player &bot)
{
	if (bot.walkpath[0] == WALK_NONE || IsBurning(bot.position.future))
		return;
	if (IsBurning(bot.position.future + WalkDirection(bot.walkpath[0]))) {
		ClrPlrPath(bot);
		bot.destAction = ACTION_NONE;
	}
}

/** Standing in fire: walk to the nearest tile that isn't burning. */
bool StepOutOfFire(Player &bot)
{
	if (!IsBurning(bot.position.tile))
		return false;
	for (const Direction dir : { Direction::South, Direction::SouthWest, Direction::West, Direction::NorthWest, Direction::North, Direction::NorthEast, Direction::East, Direction::SouthEast }) {
		const Point tile = bot.position.tile + dir;
		if (!IsBurning(tile) && PosOkPlayer(bot, tile)) {
			MakePlrPath(bot, tile, false);
			bot.destAction = ACTION_NONE;
			return true;
		}
	}
	return false;
}

bool HasLineOfSight(Point from, Point to)
{
	return LineClearMissile(from, to);
}

/** Nearest keep monster the bot can see. */
Monster *FindTarget(const Player &bot, int range)
{
	Monster *best = nullptr;
	int bestDistance = 0;
	const Point position = bot.position.tile;
	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		Monster &monster = Monsters[ActiveMonsters[i]];
		if (!IsKeepTarget(monster))
			continue;
		const int distance = position.WalkingDistance(monster.position.tile);
		if (distance > range || (distance > 1 && !HasLineOfSight(position, monster.position.tile)))
			continue;
		if (best == nullptr || distance < bestDistance) {
			best = &monster;
			bestDistance = distance;
		}
	}
	return best;
}

void Attack(Player &bot, BotState &state, Monster &target)
{
	const size_t monsterId = target.getId();
	const int distance = bot.position.tile.WalkingDistance(target.position.tile);
	const bool clearShot = HasLineOfSight(bot.position.tile, target.position.tile);
	switch (state.cls) {
	case HeroClassKind::Sorcerer:
		if (distance <= SorcererRange && clearShot) {
			ClrPlrPath(bot);
			QueueSpell(bot, SpellID::Firebolt);
			bot.destAction = ACTION_SPELLMON;
			bot.destParam1 = static_cast<int>(monsterId);
			bot.destParam2 = bot.queuedSpell.spellLevel;
			return;
		}
		break;
	case HeroClassKind::Rogue:
		if (distance <= RogueRange && clearShot && bot.UsesRangedWeapon()) {
			ClrPlrPath(bot);
			bot.destAction = ACTION_RATTACKMON;
			bot.destParam1 = static_cast<int>(monsterId);
			return;
		}
		break;
	case HeroClassKind::Warrior:
		break;
	}
	// Melee (and casters without a clear shot close in).
	if (distance > 1)
		MakePlrPath(bot, target.position.future, false);
	bot.destAction = ACTION_ATTACKMON;
	bot.destParam1 = static_cast<int>(monsterId);
}

void Escape(Player &bot, BotState &state, bool throughPortal)
{
	NoteHeroLeft(!throughPortal && state.fleeing, std::max(state.carried, 0));
	if (throughPortal) {
		State.portalEscape = true;
		Announce(fmt::format("{} escaped through the portal!", bot._pName));
	}
	if (state.carried > 0) {
		State.goldLost += state.carried;
		Announce(fmt::format("{} escaped with {} gold!", bot._pName, state.carried));
	} else if (!throughPortal && state.fleeing) {
		Announce(fmt::format("{} fled the keep in terror.", bot._pName), UiFlags::ColorGold);
	}
	RemoveBot(bot);
}

void HandleDeath(Player &bot, BotState &state)
{
	if (state.deathHandled)
		return;
	state.deathHandled = true;
	const int bounty = 30 + 10 * State.wave;
	AddVaultGold(std::max(state.carried, 0) + bounty);
	NoteHeroSlain(bounty, std::max(state.carried, 0));
	if (state.carried > 0)
		Announce(fmt::format("{} has been slain! Recovered {} stolen gold.", bot._pName, state.carried), UiFlags::ColorGold);
	else
		Announce(fmt::format("{} has been slain!", bot._pName), UiFlags::ColorGold);
	state.carried = 0;
}

void DrinkIfHurt(Player &bot, BotState &state)
{
	if (state.potions <= 0 || bot._pHitPoints * 100 >= bot._pMaxHP * 35)
		return;
	state.potions--;
	const int heal = bot._pMaxHP / 2;
	bot._pHitPoints = std::min(bot._pHitPoints + heal, bot._pMaxHP);
	bot._pHPBase = std::min(bot._pHPBase + heal, bot._pMaxHPBase);
	PlaySfxLoc(SfxID::ItemPotion, bot.position.tile);
}

void CheckFear(Player &bot, BotState &state)
{
	if (state.fearRolled || state.fleeing || state.potions > 0 || bot._pHitPoints * FearHealthDivisor >= bot._pMaxHP)
		return;
	state.fearRolled = true;
	// With no friend near, a hero with no potions left always runs.
	if (NearestHealthyHero(bot.position.tile, FallBackAllyRange) && SyncedRoll(bot, FearRollSalt) >= FearChancePercent)
		return;
	state.fleeing = true;
	if (state.carried > 0) {
		AddVaultGold(state.carried);
		NoteGoldRecovered(state.carried);
		Announce(fmt::format("{} drops {} gold and flees in terror!", bot._pName, state.carried), UiFlags::ColorGold);
		state.carried = 0;
	} else {
		Announce(fmt::format("{} flees in terror!", bot._pName), UiFlags::ColorGold);
	}
}

void RogueCheckTraps(const Player &bot, BotState &state)
{
	if (state.cls != HeroClassKind::Rogue)
		return;
	for (size_t i = 0; i < MaxTraps; i++) {
		Trap &trap = State.traps[i];
		if (!trap.active || (state.trapsSeen & (1U << i)) != 0 || bot.position.tile.WalkingDistance(trap.position) > RogueSpotRange)
			continue;
		state.trapsSeen |= static_cast<uint16_t>(1U << i);
		state.trapsKnown |= static_cast<uint16_t>(1U << i);
		if (SyncedRoll(bot, static_cast<uint32_t>(i) + static_cast<uint32_t>(trap.position.x * 256 + trap.position.y) * 16) < RogueDisarmPercent) {
			trap.active = false;
			Announce(fmt::format("{} disarmed one of your Fire Traps!", bot._pName));
		}
	}
}

bool IsNearKnownTrap(Point tile, uint16_t knownTraps)
{
	for (size_t i = 0; i < MaxTraps; i++) {
		if ((knownTraps & (1U << i)) != 0 && State.traps[i].active && tile.WalkingDistance(State.traps[i].position) <= CarefulTrapRange)
			return true;
	}
	return false;
}

/** Walk towards a goal along the hero paths. @return false if there is no way forward. */
bool Travel(Player &bot, BotState &state, HeroGoal goal)
{
	const Point position = bot.position.tile;
	if (const std::optional<Point> door = LockedDoorOnPath(goal, position, state.trapsKnown); door) {
		ClrPlrPath(bot);
		bot.destAction = ACTION_ATTACK;
		bot.destParam1 = door->x;
		bot.destParam2 = door->y;
		StrikeLockedDoor(*door, BashBlows(state.cls), bot._pName);
		return true;
	}
	const bool careful = IsNearKnownTrap(position, state.trapsKnown);
	for (const int steps : { careful ? 1 : 7, careful ? 1 : 3, 1 }) {
		const std::optional<Point> waypoint = GoalWaypoint(goal, position, steps, state.trapsKnown);
		if (!waypoint)
			return false;
		ClrPlrPath(bot);
		MakePlrPath(bot, *waypoint, true);
		if (bot.walkpath[0] != WALK_NONE) {
			bot.destAction = ACTION_NONE;
			return true;
		}
	}
	return false;
}

/** Like the monster heroes: greedy bots detour to fool's gold they can see. @return true if the bot acted. */
bool TryGoForBait(Player &bot, BotState &state)
{
	const Point position = bot.position.tile;
	const std::optional<size_t> slot = FindBaitFor(state.cls, position, state.baitsVisited);
	if (!slot)
		return false;
	const Point bait = State.baits[*slot].position;
	ClrPlrPath(bot);
	bot.destAction = ACTION_NONE;
	if (position.WalkingDistance(bait) <= 1) {
		ConsumeBait(*slot, state.baitsVisited, bot._pName);
		state.baitLingerTicks = BaitLingerTicks;
		return true;
	}
	MakePlrPath(bot, bait, false);
	if (bot.walkpath[0] != WALK_NONE)
		return true;
	state.baitsVisited |= static_cast<uint8_t>(1U << *slot); // can't reach it: give up on this pile
	return false;
}

/** Out of potions and wounded in a fight: break off, once, and run to a healthy friend. @return true if it moved. */
bool TryFallBack(Player &bot, BotState &state)
{
	if (state.fleeing || state.carried != 0 || state.potions > 0 || State.wave >= NumWaves())
		return false;
	if (state.fallBackThinks == 0) {
		if (state.fellBack || bot._pHitPoints * 100 >= bot._pMaxHP * FallBackHealthPercent || FindTarget(bot, BotSight) == nullptr)
			return false;
		state.fellBack = true;
		state.fallBackThinks = FallBackMaxThinks;
	}
	const std::optional<Point> ally = NearestHealthyHero(bot.position.tile, FallBackAllyRange);
	if (!ally || bot.position.tile.WalkingDistance(*ally) <= FallBackArriveDistance) {
		state.fallBackThinks = 0;
		return false;
	}
	state.fallBackThinks--;
	ClrPlrPath(bot);
	bot.destAction = ACTION_NONE;
	MakePlrPath(bot, *ally, false);
	return true;
}

void Think(Player &bot, BotState &state)
{
	const Point position = bot.position.tile;

	// Out through the portal (once the Lord is dead) or the entrance (thieves and cowards).
	if (State.bossDown && PortalPosition() && GoalDistance(HeroGoal::Portal, position) <= 1) {
		Escape(bot, state, true);
		return;
	}
	if (!State.bossDown && (state.carried != 0 || state.fleeing) && GoalDistance(HeroGoal::Entrance, position) <= 1) {
		Escape(bot, state, false);
		return;
	}

	if (IsPortalCaster(bot) && !PortalPosition()) {
		// Channelling the portal: hold still and keep the gesture going.
		ClrPlrPath(bot);
		bot.destAction = ACTION_NONE;
		return;
	}

	const bool running = state.fleeing || (State.bossDown && PortalPosition());
	if (!State.bossDown && TryFallBack(bot, state))
		return;
	if (!running) {
		if (Monster *target = FindTarget(bot, BotSight); target != nullptr) {
			Attack(bot, state, *target);
			return;
		}
	}

	if (State.bossDown) {
		if (PortalPosition()) {
			Travel(bot, state, HeroGoal::Portal);
			return;
		}
		// Guard whoever is opening the portal.
		if (const std::optional<Point> caster = PortalCasterPosition(); caster && position.WalkingDistance(*caster) > 2)
			MakePlrPath(bot, *caster, false);
		return;
	}

	if (state.carried != 0 || state.fleeing) {
		Travel(bot, state, HeroGoal::Entrance);
		return;
	}

	if (TryGoForBait(bot, state))
		return;

	if (GoalDistance(HeroGoal::Vault, position) <= 1) {
		// Rogues are here for the gold; the others came for the Lord and wait for him at the vault.
		if (state.cls == HeroClassKind::Rogue && State.vaultGold > 0) {
			state.carried = TakeFromVault(CarryCapacity(state.cls));
			NoteGoldLooted(state.carried);
			Announce(fmt::format("{} looted {} gold from the vault!", bot._pName, state.carried));
		}
		return;
	}
	if (state.regroupBudget > 0 && IsAheadOfParty(position)) {
		// Called every tick while the bot stands idle.
		state.regroupBudget--;
		ClrPlrPath(bot);
		bot.destAction = ACTION_NONE;
		return;
	}
	Travel(bot, state, HeroGoal::Vault);
}

} // namespace

bool IsBot(const Player &player)
{
	return player.getId() >= FirstBotId;
}

bool IsLivingBot(const Player &player)
{
	return IsBot(player) && player.plractive && !player.hasNoLife() && player.isOnActiveLevel();
}

bool SimulatesLocally(const Player &player)
{
	return &player == MyPlayer || IsBot(player);
}

std::optional<size_t> FreeBotSlot()
{
	for (size_t i = 0; i < MaxBots; i++) {
		if (!Players[FirstBotId + i].plractive && !PendingSpawn[i]) {
			PendingSpawn[i] = true;
			return i;
		}
	}
	return std::nullopt;
}

void SpawnBot(size_t slot, HeroClassKind cls, Point tile)
{
	if (slot >= MaxBots)
		return;
	PendingSpawn[slot] = false;
	Player &bot = Players[FirstBotId + slot];
	RemoveBot(bot);

	// CreatePlayer reseeds the global RNG; keep the simulation's sequence intact.
	const uint32_t rngState = GetLCGEngineState();
	CreatePlayer(bot, ToHeroClass(cls));
	SetRndSeed(rngState);

	const std::string_view name = BotNames[static_cast<size_t>(cls)][slot];
	CopyUtf8(bot._pName, name, sizeof(bot._pName));
	bot.plractive = true;
	bot.friendlyMode = false;
	bot._pLvlChanging = false;
	bot.setLevel(currlevel);
	bot.position.tile = tile;
	bot.position.future = tile;
	bot.position.old = tile;

	const bool champion = State.wave >= NumWaves();
	ScaleBot(bot, champion);
	if (cls == HeroClassKind::Sorcerer)
		LearnSpell(bot, SpellID::Firebolt, static_cast<uint8_t>(std::min(1 + State.wave / 2, 15)));
	LearnSpell(bot, SpellID::TownPortal, 1);

	InitPlayerGFX(bot);
	InitPlayer(bot, true);
	SyncInitPlrPos(bot);

	BotState &state = Bots[slot];
	state = {};
	state.cls = cls;
	state.potions = 2 + State.wave / 2;

	Announce(fmt::format("{} the {} storms the keep!", bot._pName, ClassTitle(cls)));
}

void RemoveAllBots()
{
	for (size_t i = 0; i < MaxBots; i++) {
		if (FirstBotId + i < Players.size())
			RemoveBot(Players[FirstBotId + i]);
		Bots[i] = {};
		PendingSpawn[i] = false;
	}
}

void UpdateBots()
{
	if (Players.size() < FirstBotId + MaxBots)
		return;
	for (size_t i = 0; i < MaxBots; i++) {
		Player &bot = Players[FirstBotId + i];
		if (!bot.plractive || !bot.isOnActiveLevel())
			continue;
		BotState &state = Bots[i];
		if (bot.hasNoLife()) {
			HandleDeath(bot, state);
			continue;
		}
		if (state.petrifiedTicks > 0) {
			state.petrifiedTicks--;
			ClrPlrPath(bot);
			bot.destAction = ACTION_NONE;
			continue;
		}
		AvoidFire(bot);

		OpenDoorsAround(bot.position.tile);
		RogueCheckTraps(bot, state);
		DrinkIfHurt(bot, state);
		CheckFear(bot, state);

		if (bot._pmode != PM_STAND)
			continue; // mid-swing, mid-step, mid-cast or reeling
		if (state.thinkCooldown > 0 && (bot.walkpath[0] != WALK_NONE || bot.destAction != ACTION_NONE)) {
			state.thinkCooldown--;
			continue;
		}
		if (StepOutOfFire(bot)) {
			state.thinkCooldown = 0;
			continue;
		}
		if (state.baitLingerTicks > 0) {
			state.baitLingerTicks--;
			continue;
		}
		state.thinkCooldown = ThinkInterval;
		Think(bot, state);
		AvoidFire(bot);
	}
}

int LivingBotCount()
{
	int count = 0;
	for (size_t i = 0; i < MaxBots && FirstBotId + i < Players.size(); i++) {
		if (IsLivingBot(Players[FirstBotId + i]))
			count++;
	}
	return count;
}

int BotGoldInFlight()
{
	int total = 0;
	for (size_t i = 0; i < MaxBots && FirstBotId + i < Players.size(); i++) {
		if (IsLivingBot(Players[FirstBotId + i]) && Bots[i].carried > 0)
			total += Bots[i].carried;
	}
	return total;
}

const Player *NearestBot(Point from, int range)
{
	const Player *best = nullptr;
	int bestDistance = 0;
	for (size_t i = 0; i < MaxBots && FirstBotId + i < Players.size(); i++) {
		const Player &bot = Players[FirstBotId + i];
		if (!IsLivingBot(bot))
			continue;
		const int distance = bot.position.tile.WalkingDistance(from);
		if (distance > range)
			continue;
		if (best == nullptr || distance < bestDistance) {
			best = &bot;
			bestDistance = distance;
		}
	}
	return best;
}

std::optional<BotSummary> GetBotSummary(const Player &player)
{
	if (!IsLivingBot(player))
		return std::nullopt;
	const BotState &state = Bots[SlotOf(player)];
	return BotSummary { state.cls, state.carried, state.fleeing, state.potions };
}

bool PetrifyBot(Player &bot, int ticks)
{
	if (!IsLivingBot(bot))
		return false;
	BotState &state = Bots[SlotOf(bot)];
	if (state.petrifiedTicks > 0)
		return false; // doesn't stack
	state.petrifiedTicks = ticks;
	return true;
}

bool IsPetrifiedBot(const Player &player)
{
	return IsLivingBot(player) && Bots[SlotOf(player)].petrifiedTicks > 0;
}

bool IsBotNear(Point tile, int distance)
{
	for (size_t i = 0; i < MaxBots && FirstBotId + i < Players.size(); i++) {
		const Player &bot = Players[FirstBotId + i];
		if (IsLivingBot(bot) && bot.position.tile.WalkingDistance(tile) < distance)
			return true;
	}
	return false;
}

void BotsWitnessTrap(size_t trapSlot, Point trapPosition, int range)
{
	for (size_t i = 0; i < MaxBots && FirstBotId + i < Players.size(); i++) {
		const Player &bot = Players[FirstBotId + i];
		if (!IsLivingBot(bot))
			continue;
		const int distance = bot.position.tile.WalkingDistance(trapPosition);
		if (distance <= 1 || (distance <= range && HasLineOfSight(bot.position.tile, trapPosition)))
			Bots[i].trapsKnown |= static_cast<uint16_t>(1U << trapSlot);
	}
}

std::optional<Point> NearestHealthyBot(Point from, int range)
{
	std::optional<Point> best;
	int bestDistance = 0;
	for (size_t i = 0; i < MaxBots && FirstBotId + i < Players.size(); i++) {
		const Player &bot = Players[FirstBotId + i];
		if (!IsLivingBot(bot) || Bots[i].fleeing || Bots[i].carried != 0 || bot._pHitPoints * 100 < bot._pMaxHP * HealthyAllyPercent)
			continue;
		const int distance = from.WalkingDistance(bot.position.tile);
		if (distance <= range && (!best || distance < bestDistance)) {
			best = Point(bot.position.tile);
			bestDistance = distance;
		}
	}
	return best;
}

uint16_t BotRearVaultDistance()
{
	uint16_t rear = 0;
	for (size_t i = 0; i < MaxBots && FirstBotId + i < Players.size(); i++) {
		const Player &bot = Players[FirstBotId + i];
		if (!IsLivingBot(bot) || Bots[i].fleeing || Bots[i].carried != 0)
			continue;
		const uint16_t distance = GoalDistance(HeroGoal::Vault, bot.position.tile);
		if (distance != 0xFFFF)
			rear = std::max(rear, distance);
	}
	return rear;
}

void BotCastTownPortal(Player &bot)
{
	if (!IsLivingBot(bot))
		return;
	LearnSpell(bot, SpellID::TownPortal, 1);
	ClrPlrPath(bot);
	QueueSpell(bot, SpellID::TownPortal);
	bot.destAction = ACTION_SPELL;
	bot.destParam1 = bot.position.tile.x;
	bot.destParam2 = bot.position.tile.y;
	bot.destParam3 = bot.queuedSpell.spellLevel;
}

bool PlayerCanHitMonster(const Player &source, const Monster &target)
{
	return IsBot(source) ? !IsHero(target) : IsHero(target);
}

bool PlayerCanHitPlayer(const Player &source, const Player &target)
{
	return !IsBot(source) && IsBot(target);
}

bool MonsterCanHitPlayer(const Monster &source, const Player & /*target*/)
{
	return !IsHero(source);
}

} // namespace devilution::keeper
