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
#include "keeper/keeper_botgear.hpp"
#include "keeper/keeper_devctl.hpp"
#include "keeper/keeper_game.hpp"
#include "keeper/keeper_raidtypes.hpp"
#include "keeper/keeper_raid.hpp"
#include "levels/gendung.h"
#include "levels/tile_properties.hpp"
#include "lighting.h"
#include "missiles.h"
#include "monster.h"
#include "player.h"
#include "plrmsg.h"
#include "spells.h"
#include "utils/is_of.hpp"
#include "utils/utf8.hpp"

namespace devilution::keeper {

namespace {

constexpr int BotSight = 7;
constexpr int SorcererRange = 8;
constexpr int RogueRange = 9;
constexpr int ThinkInterval = 6;
constexpr int RogueDisarmPercent = 40;
constexpr int RogueSpotRange = 3;
constexpr int FearHealthDivisor = 4;
constexpr int FearChancePercent = 50;
/** Most AI decisions a bot spends falling back, about 9 steps. */
constexpr int FallBackMaxThinks = 12;
/** Near a known trap, walk the route tile by tile: the engine's path to a far waypoint would cut through it. */
constexpr int CarefulTrapRange = 9;
/** A joiner's copy of a bot, tiles from the host's: nearer than the first is left alone, then it walks over, then it jumps. */
constexpr int BotCorrectDistance = 2;
constexpr int BotSnapDistance = 5;
/** Far enough to jump even out of a swing or a spell, which otherwise get to finish. */
constexpr int BotForceSnapDistance = 10;
/** The most of a blow a bot's shield can ever turn: dexterity and level took a Warrior to 100%, immune to melee. */
constexpr int MaxBotBlockPercent = 75;
/** A Warrior prefers a monster on a softer friend over a nearer one on itself, by this many tiles. */
constexpr int WarriorProtectBonus = 3;

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
	int kiteCooldown = 0;
	int healCooldown = 0;
	/** Lines this hero has already said, a bit each: nobody repeats itself. */
	uint8_t said = 0;
	HeroProgress progress;
	HeroEngagement engagement;
	HeroWatchdog watchdog;
	bool watchdogDue = false;
	/** Reached the vault and came for the Lord: it goes where he is, not back to the vault. */
	bool huntingLord = false;
	/** HeroClock time until which it keeps clear of every floor hazard. */
	uint32_t hazardWaryUntil = 0;
};

constexpr uint8_t SaidDash = 1;
constexpr uint8_t SaidKite = 2;
constexpr uint8_t SaidHazard = 4;
constexpr uint8_t SaidElement = 8;
constexpr uint8_t SaidLord = 16;
constexpr uint8_t SaidProtect = 32;
constexpr uint8_t SaidFallBack = 64;
constexpr uint8_t SaidTrap = 128;

void SayOnce(const Player &bot, BotState &state, uint8_t bit, std::string_view line)
{
	if ((state.said & bit) == 0 && HeroSay(bot._pName, line))
		state.said |= bit;
}

std::array<BotState, MaxBots> Bots;
/** Host side: slots requested but not spawned yet (the command is still in flight). */
std::array<bool, MaxBots> PendingSpawn {};

constexpr std::array<std::array<std::string_view, 4>, NumHeroClasses> BotNames { {
	{ "Sir Aldric", "Brannoc", "Garrick", "Thorne" },
	{ "Lyra", "Vessa", "Kestrel", "Wren" },
	{ "Malachar", "Ysolde", "Corvin", "Isembard" },
	{ "Brother Cade", "Sister Ilse", "Osric", "Talia" },
} };

HeroClass ToHeroClass(HeroClassKind cls)
{
	switch (cls) {
	case HeroClassKind::Rogue:
		return HeroClass::Rogue;
	case HeroClassKind::Sorcerer:
		return HeroClass::Sorcerer;
	case HeroClassKind::Monk:
		return HeroClass::Monk;
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
	case HeroClassKind::Monk:
		return "Monk";
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

/**
 * Bot character levels gained per campaign level, and the engine's limit. Level is accuracy, but also
 * damage (strength * level), block chance and the monsters' chance to hit, so it is kept modest.
 */
constexpr int LevelsPerCampaignLevel = 1;
constexpr int MaxBotLevel = 50;
/** Stats gained per raid. Strength, dexterity and magic scale with the level's strength. */
constexpr int BotStatsPerRaid = 2;
/** Potions a bot carries: each heals half its life, so they count for as much as its life does. */
constexpr int BotPotionsBase = 1;
constexpr int BotRaidsPerPotion = 4;
/** Gold for killing a bot. Bots who run and get away pay nothing, which is a good third of them. */
constexpr int BotBountyBase = 54;
constexpr int BotBountyPerWave = 18;

void ScaleBot(Player &bot, bool champion)
{
	const int wave = std::max<int>(State.wave, 1);
	bot.setCharacterLevel(static_cast<uint8_t>(std::min(wave * 2 - 1, 30)));
	const int bonus = ScaleHeroCombat(BotStatsPerRaid * (wave - 1));
	bot._pBaseStr += bonus;
	bot._pBaseDex += bonus;
	// Vitality does not scale: its life is scaled below, and scaling both made bots grow on the square of the level's strength.
	bot._pBaseVit += BotStatsPerRaid * (wave - 1);
	bot._pBaseMag += bonus;
	bot._pMaxHPBase = bot.calculateBaseLife();
	if (champion)
		bot._pMaxHPBase = bot._pMaxHPBase * 3 / 2;
	bot._pMaxHPBase = ScaleHeroCombat(bot._pMaxHPBase);
	bot._pHPBase = bot._pMaxHPBase;
	bot._pMaxManaBase = std::max(bot.calculateBaseMana(), 60 << 6);
	bot._pManaBase = bot._pMaxManaBase;
	// Life is set: later levels only raise the level for accuracy against heavily armoured monsters.
	bot.setCharacterLevel(static_cast<uint8_t>(std::min(bot.getCharacterLevel() + LevelsPerCampaignLevel * (CurrentLevelNumber() - 1), MaxBotLevel)));
	CalcPlrInv(bot, false);
	SetPlayerHitPoints(bot, bot._pMaxHP);
	bot._pMana = bot._pMaxMana;
}

void LearnSpell(Player &bot, SpellID spell, uint8_t level)
{
	bot._pMemSpells |= GetSpellBitmask(spell);
	bot._pSplLvl[static_cast<int8_t>(spell)] = std::max(bot._pSplLvl[static_cast<int8_t>(spell)], level);
}

int BotFireboltLevel()
{
	return std::min(1 + State.wave / 2 + BotGearSpellLevels(), 15);
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

/**
 * Heroes aren't stupid: stop before walking into a wall of fire (unless already in one). Lightning takes them
 * by surprise, once: having just been caught in a hazard, a bot keeps clear of them all and doesn't pace back in.
 */
void AvoidFire(Player &bot, const BotState &state)
{
	const auto avoided = state.hazardWaryUntil > HeroClock() ? IsHazardous : IsBurning;
	if (bot.walkpath[0] == WALK_NONE || avoided(bot.position.future))
		return;
	if (avoided(bot.position.future + WalkDirection(bot.walkpath[0]))) {
		ClrPlrPath(bot);
		bot.destAction = ACTION_NONE;
	}
}

/** Standing in fire, lightning or any other floor hazard: walk to the nearest tile that is clear. */
bool StepOutOfHazard(Player &bot, BotState &state)
{
	if (!IsHazardous(bot.position.tile))
		return false;
	for (const Direction dir : { Direction::South, Direction::SouthWest, Direction::West, Direction::NorthWest, Direction::North, Direction::NorthEast, Direction::East, Direction::SouthEast }) {
		const Point tile = bot.position.tile + dir;
		if (IsHazardous(tile) || !CanStepDiagonal(bot.position.tile, Displacement(dir)) || !PosOkPlayer(bot, tile))
			continue;
		ClrPlrPath(bot);
		MakePlrPath(bot, tile, true); // true: all the way onto it, and without it a next-door tile is no steps at all
		if (bot.walkpath[0] == WALK_NONE)
			continue;
		bot.destAction = ACTION_NONE;
		state.hazardWaryUntil = HeroClock() + HazardWaryTicks;
		SayOnce(bot, state, SaidHazard, "It burns! Get clear!");
		return true;
	}
	return false;
}

bool HasLineOfSight(Point from, Point to)
{
	return LineClearMissile(from, to);
}

/** The monster is fighting a hero who isn't a Warrior. */
bool IsOnSofterFriend(const Monster &monster)
{
	if ((monster.flags & MFLAG_NO_ENEMY) != 0)
		return false;
	if ((monster.flags & MFLAG_TARGETS_MONSTER) != 0) {
		const std::optional<HeroSummary> hero = GetHeroSummary(Monsters[monster.enemy]);
		return hero && hero->cls != HeroClassKind::Warrior;
	}
	const std::optional<BotSummary> bot = monster.enemy < Players.size() ? GetBotSummary(Players[monster.enemy]) : std::nullopt;
	return bot && bot->cls != HeroClassKind::Warrior;
}

/** Nearest keep monster the bot can see. A Warrior goes first for those on a softer friend. */
Monster *FindTarget(const Player &bot, const BotState &state, int range)
{
	Monster *best = nullptr;
	int bestDistance = 0;
	const Point position = bot.position.tile;
	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		Monster &monster = Monsters[ActiveMonsters[i]];
		if (!IsKeepTarget(monster))
			continue;
		const int distance = position.WalkingDistance(monster.position.tile);
		if (distance > range || (distance > 1 && !HasLineOfSight(position, monster.position.tile)) || IsIgnoredTarget(state.engagement, monster, distance))
			continue;
		const int score = state.cls == HeroClassKind::Warrior && IsOnSofterFriend(monster) ? distance - WarriorProtectBonus : distance;
		if (best == nullptr || score < bestDistance) {
			best = &monster;
			bestDistance = score;
		}
	}
	return best;
}

/** A shooter with a monster at its elbow steps away. @return true if it did. */
bool StepBack(Player &bot, BotState &state, const Monster &threat)
{
	if (state.kiteCooldown > 0)
		return false;
	const Point position = bot.position.tile;
	const Direction away = GetDirection(threat.position.tile, position);
	for (const Direction dir : { away, Left(away), Right(away) }) {
		const Point tile = position + dir;
		if (tile.WalkingDistance(threat.position.tile) < 2 || IsHazardous(tile) || !CanStepDiagonal(position, Displacement(dir)) || !PosOkPlayer(bot, tile))
			continue;
		ClrPlrPath(bot);
		MakePlrPath(bot, tile, true);
		if (bot.walkpath[0] == WALK_NONE)
			continue;
		bot.destAction = ACTION_NONE;
		state.kiteCooldown = KiteCooldownTicks;
		if (state.cls == HeroClassKind::Sorcerer)
			SayOnce(bot, state, SaidKite, "Keep them off me!");
		return true;
	}
	return false;
}

/** What a Sorcerer casts at this monster, if anything: nobody keeps throwing fire at what fire can't hurt. */
std::optional<SpellID> SpellAgainst(const Monster &target)
{
	if ((target.resistance & IMMUNE_FIRE) == 0)
		return SpellID::Firebolt;
	if ((target.resistance & IMMUNE_LIGHTNING) == 0)
		return SpellID::Lightning;
	return std::nullopt;
}

void Attack(Player &bot, BotState &state, const Monster &target)
{
	const size_t monsterId = target.getId();
	const int distance = bot.position.tile.WalkingDistance(target.position.tile);
	if (!NoteEngagement(state.engagement, target, distance)) {
		// Not hurting it and no closer: leave it. The next decision goes on along the route.
		ClrPlrPath(bot);
		bot.destAction = ACTION_NONE;
		return;
	}
	HoldHeroProgress(state.progress);
	const bool clearShot = HasLineOfSight(bot.position.tile, target.position.tile);
	switch (state.cls) {
	case HeroClassKind::Sorcerer:
		if (distance <= 1 && StepBack(bot, state, target))
			return;
		if (const std::optional<SpellID> spell = SpellAgainst(target); spell && distance <= SorcererRange && clearShot) {
			if (*spell != SpellID::Firebolt)
				SayOnce(bot, state, SaidElement, "Fire won't touch it. Lightning, then!");
			ClrPlrPath(bot);
			QueueSpell(bot, *spell);
			bot.destAction = ACTION_SPELLMON;
			bot.destParam1 = static_cast<int>(monsterId);
			bot.destParam2 = bot.queuedSpell.spellLevel;
			return;
		}
		break;
	case HeroClassKind::Rogue:
		if (distance <= 1 && StepBack(bot, state, target))
			return;
		if (distance <= RogueRange && clearShot && bot.UsesRangedWeapon()) {
			ClrPlrPath(bot);
			bot.destAction = ACTION_RATTACKMON;
			bot.destParam1 = static_cast<int>(monsterId);
			return;
		}
		break;
	case HeroClassKind::Warrior:
		if (IsBoss(target))
			SayOnce(bot, state, SaidLord, "Face me, Lord of this keep!");
		else if (IsOnSofterFriend(target))
			SayOnce(bot, state, SaidProtect, "To me! Stand behind me!");
		break;
	case HeroClassKind::Monk:
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

/** Positions differ between clients, so only the host's copy decides it has got out. */
void LeaveKeep(Player &bot, BotState &state, bool throughPortal)
{
	if (HoldsBotAlive(bot)) {
		ClrPlrPath(bot);
		bot.destAction = ACTION_NONE;
		return;
	}
	NotifyBotEscaped(SlotOf(bot), throughPortal, state.fleeing, std::max(state.carried, 0));
	Escape(bot, state, throughPortal);
}

void HandleDeath(Player &bot, BotState &state)
{
	if (state.deathHandled)
		return;
	state.deathHandled = true;
	const int bounty = ScaleByHeroStrength(BotBountyBase + BotBountyPerWave * State.wave);
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

void BotStartsFleeing(const Player &bot, BotState &state)
{
	state.fearRolled = true;
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

void BotLoots(const Player &bot, BotState &state, int amount)
{
	state.carried = amount;
	NoteGoldLooted(amount);
	Announce(fmt::format("{} looted {} gold from the vault!", bot._pName, amount));
}

void CheckFear(Player &bot, BotState &state)
{
	if (HoldsBotAlive(bot)) // the host says when it runs
		return;
	if (state.fearRolled || state.fleeing || state.potions > 0 || bot._pHitPoints * FearHealthDivisor >= bot._pMaxHP)
		return;
	state.fearRolled = true;
	// With no friend near, a hero with no potions left always runs.
	if (NearestHealthyHero(bot.position.tile, FallBackAllyRange) && SyncedRoll(bot, FearRollSalt) >= RaidFearPercent(FearChancePercent))
		return;
	NotifyBotFled(SlotOf(bot));
	BotStartsFleeing(bot, state);
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
			if (!HoldsBotAlive(bot))
				DisarmTrap(i, SlotOf(bot));
		} else {
			SayOnce(bot, state, SaidTrap, "A trap! Mind your step.");
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
	// Warriors hold the line.
	if (state.cls == HeroClassKind::Warrior || state.fleeing || state.carried != 0 || state.potions > 0 || State.wave >= NumWaves())
		return false;
	if (state.fallBackThinks == 0) {
		if (state.fellBack || bot._pHitPoints * 100 >= bot._pMaxHP * FallBackHealthPercent || FindTarget(bot, state, BotSight) == nullptr)
			return false;
		state.fellBack = true;
		state.fallBackThinks = FallBackMaxThinks;
		SayOnce(bot, state, SaidFallBack, "I'm hurt! Falling back!");
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

/** On the run with the way shut by a monster: fight it rather than stand there. */
bool FightIfBlocked(Player &bot, BotState &state)
{
	Monster *target = FindTarget(bot, state, 1);
	if (target == nullptr)
		return false;
	Attack(bot, state, *target);
	return true;
}

void Think(Player &bot, BotState &state)
{
	const Point position = bot.position.tile;

	// Out through the portal (once the Lord is dead) or the entrance (thieves and cowards).
	if (State.bossDown && PortalPosition() && GoalDistance(HeroGoal::Portal, position) <= 1) {
		LeaveKeep(bot, state, true);
		return;
	}
	if (!State.bossDown && (state.carried != 0 || state.fleeing) && GoalDistance(HeroGoal::Entrance, position) <= 1) {
		LeaveKeep(bot, state, false);
		return;
	}

	if (IsPortalCaster(bot) && !PortalPosition()) {
		// Channelling the portal: hold still and keep the gesture going.
		ClrPlrPath(bot);
		bot.destAction = ACTION_NONE;
		return;
	}

	if (state.cls == HeroClassKind::Monk && state.healCooldown == 0 && HealWoundedAlly(position)) {
		state.healCooldown = MonkHealCooldownTicks;
		SayOnce(bot, state, SaidProtect, "Hold fast! I have you.");
	}

	// Thieves don't stop to fight: loot in hand, or the vault in reach.
	const bool thieving = !State.bossDown && (state.carried != 0 || IsThiefDashing(state.cls, state.carried, state.fleeing, position));
	if (thieving && state.carried == 0)
		SayOnce(bot, state, SaidDash, "The vault! That gold is mine!");
	const bool running = state.fleeing || thieving || (State.bossDown && PortalPosition());
	if (!State.bossDown && TryFallBack(bot, state))
		return;
	if (!running) {
		if (Monster *target = FindTarget(bot, state, BotSight); target != nullptr) {
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
		if (!BotSqueezeIfStuck(bot, state.progress, HeroGoal::Entrance, state.trapsKnown, true) && !Travel(bot, state, HeroGoal::Entrance))
			FightIfBlocked(bot, state);
		return;
	}

	if (state.huntingLord) {
		if (const Monster *lord = FindBoss(); lord != nullptr && !IsIgnoredTarget(state.engagement, *lord, position.WalkingDistance(lord->position.tile))) {
			Attack(bot, state, *lord);
			return;
		}
		state.huntingLord = false; // he hasn't risen yet, or can't be got at from here: back to the vault
	}

	if (TryGoForBait(bot, state))
		return;

	if (GoalDistance(HeroGoal::Vault, position) <= 1) {
		// Rogues are here for the gold, and go home when it's gone. The others came for the Lord, wherever he is.
		// On a joiner the host says what a Rogue took: ApplyBotLooted.
		if (state.cls == HeroClassKind::Rogue) {
			if (State.vaultGold > 0) {
				if (!HoldsBotAlive(bot)) {
					const int amount = TakeFromVault(CarryCapacity(state.cls));
					NotifyBotLooted(SlotOf(bot), amount);
					BotLoots(bot, state, amount);
				}
			} else {
				state.carried = -1;
			}
		} else if (const Monster *lord = FindBoss(); lord != nullptr) {
			state.huntingLord = true;
			Attack(bot, state, *lord);
		}
		return;
	}
	if (!thieving && state.regroupBudget > 0 && IsAheadOfParty(position)) {
		// Called every tick while the bot stands idle.
		state.regroupBudget--;
		HoldHeroProgress(state.progress);
		ClrPlrPath(bot);
		bot.destAction = ACTION_NONE;
		return;
	}
	if (!BotSqueezeIfStuck(bot, state.progress, HeroGoal::Vault, state.trapsKnown, false) && !Travel(bot, state, HeroGoal::Vault))
		FightIfBlocked(bot, state);
}

} // namespace

bool IsHeroInHazard(const Player &player)
{
	return IsLivingBot(player) && IsHazardous(player.position.tile);
}

void PlaceBot(Player &bot, Point tile)
{
	ClearBotFromMap(bot);
	ClrPlrPath(bot);
	bot.destAction = ACTION_NONE;
	bot.position.tile = tile;
	bot.position.future = tile;
	bot.position.old = tile;
	bot.occupyTile(tile, false);
}

int CapBotBlock(const Player &player, int chance)
{
	return IsBot(player) ? std::min(chance, MaxBotBlockPercent) : chance;
}

bool IsBot(const Player &player)
{
	return player.getId() >= FirstBotId;
}

bool IsLivingBot(const Player &player)
{
	return IsBot(player) && player.plractive && !player.hasNoLife() && player.isOnActiveLevel();
}

namespace {

constexpr int32_t BotLifeGone = -1;
constexpr int32_t BotLifeUnknown = -2;

} // namespace

bool HoldsBotAlive(const Player &player)
{
	return IsBot(player) && IsActive() && !IsHost();
}

int32_t BotLifeForSync(size_t slot)
{
	if (PendingSpawn[slot])
		return BotLifeUnknown; // its spawn message is already on its way, ahead of this one
	const Player &bot = Players[FirstBotId + slot];
	return bot.plractive ? std::max(bot._pHitPoints, 0) : BotLifeGone;
}

BotSyncFacts BotFactsForSync(size_t slot)
{
	const Player &bot = Players[FirstBotId + slot];
	return { bot.plractive ? Point(bot.position.tile) : Point {}, static_cast<uint8_t>(std::clamp(Bots[slot].potions, 0, 255)) };
}

namespace {

/** Puts a bot on a tile, standing, ready to think again. */
void PlaceBot(Player &bot, Point tile)
{
	ClrPlrPath(bot);
	bot.destAction = ACTION_NONE;
	bot.position.tile = tile;
	bot.position.future = tile;
	bot.position.old = tile;
	bot.position.temp = tile;
	StartStand(bot, bot._pdir);
}

/** Puts a joiner's copy on the host's tile (or the nearest free one). */
void SnapBot(Player &bot, Point tile)
{
	ClearBotFromMap(bot);
	const std::optional<Point> free = FindClosestValidPosition([&bot](Point position) { return PosOkPlayer(bot, position); }, tile, 0, 3);
	PlaceBot(bot, free.value_or(Point(bot.position.tile)));
}

} // namespace

void ApplyBotPositionSync(size_t slot, int32_t hostLife, Point hostTile, uint8_t potions)
{
	Player &bot = Players[FirstBotId + slot];
	// Negative life: the host has no such bot yet or any more, so its tile and potions mean nothing.
	if (hostLife < 0 || !IsLivingBot(bot) || !InDungeonBounds(hostTile))
		return;
	BotState &state = Bots[slot];
	if (potions < state.potions)
		PlaySfxLoc(SfxID::ItemPotion, bot.position.tile); // a joiner's copy doesn't drink for itself
	state.potions = potions;
	DevctlNoteBotDrift(Point(bot.position.tile).WalkingDistance(hostTile));
	if (!DevctlBotSyncOn())
		return;
	// Where it is heading: a copy one step from the host's tile is only showing network delay.
	const int drift = Point(bot.position.future).WalkingDistance(hostTile);
	if (drift < BotCorrectDistance)
		return;
	const bool midAction = IsAnyOf(bot._pmode, PM_ATTACK, PM_RATTACK, PM_SPELL, PM_GOTHIT);
	if (drift >= BotForceSnapDistance || (drift >= BotSnapDistance && !midAction)) {
		SnapBot(bot, hostTile);
		DevctlNoteBotFix(/*snap=*/true);
		return;
	}
	// Close enough to walk there. A copy already walking stays on its route: it is following the host's.
	if (bot._pmode == PM_STAND && state.petrifiedTicks == 0) {
		ClrPlrPath(bot);
		bot.destAction = ACTION_NONE;
		MakePlrPath(bot, hostTile, true);
		state.thinkCooldown = ThinkInterval;
		DevctlNoteBotFix(/*snap=*/false);
	}
}

void ApplyBotLifeSync(size_t slot, int32_t hitPoints)
{
	Player &bot = Players[FirstBotId + slot];
	if (hitPoints == BotLifeUnknown || !IsLivingBot(bot))
		return;
	if (hitPoints == BotLifeGone) {
		// Not there on the host, and ApplyBotEscaped didn't say why. This copy can't die, so it has to go.
		RemoveBot(bot);
		return;
	}
	DevctlNoteBotLifeDrift(std::abs(bot._pHitPoints - hitPoints) >> 6);
	if (hitPoints == 0) {
		SyncPlrKill(bot, DeathReason::MonsterOrTrap);
		HandleDeath(bot, Bots[slot]); // now: the raid may end before the next tick looks
	} else
		SetPlayerHitPoints(bot, std::min(hitPoints, bot._pMaxHP));
}

void ApplyBotLooted(size_t slot, int amount)
{
	const Player &bot = Players[FirstBotId + slot];
	if (IsLivingBot(bot))
		BotLoots(bot, Bots[slot], amount);
}

void ApplyBotFled(size_t slot)
{
	const Player &bot = Players[FirstBotId + slot];
	if (IsLivingBot(bot) && !Bots[slot].fleeing)
		BotStartsFleeing(bot, Bots[slot]);
}

void ApplyBotEscaped(size_t slot, bool throughPortal, bool fleeing, int carried)
{
	Player &bot = Players[FirstBotId + slot];
	if (!IsLivingBot(bot))
		return;
	BotState &state = Bots[slot];
	state.fleeing = fleeing;
	state.carried = carried;
	Escape(bot, state, throughPortal);
}

std::optional<BotSyncState> GetBotSyncState(size_t slot)
{
	if (slot >= MaxBots || !IsLivingBot(Players[FirstBotId + slot]))
		return std::nullopt;
	const BotState &state = Bots[slot];
	const uint32_t now = HeroClock();
	BotSyncState synced { state.fleeing, state.fearRolled, state.fellBack, state.carried, state.potions, state.petrifiedTicks, state.baitsVisited, state.trapsKnown, state.trapsSeen, state.baitLingerTicks, state.fallBackThinks, state.regroupBudget, state.thinkCooldown };
	synced.kiteCooldown = state.kiteCooldown;
	synced.healCooldown = state.healCooldown;
	synced.hazardWaryLeft = state.hazardWaryUntil > now ? static_cast<int>(state.hazardWaryUntil - now) : 0;
	synced.said = state.said;
	synced.huntingLord = state.huntingLord;
	if (state.engagement.ignored >= 0 && state.engagement.ignoredUntil > now) {
		synced.ignoredTarget = state.engagement.ignored;
		synced.ignoredLeft = static_cast<int>(state.engagement.ignoredUntil - now);
	}
	synced.progressGoal = static_cast<uint8_t>(state.progress.goal);
	synced.progressBest = state.progress.best;
	synced.progressAge = static_cast<int>(now - state.progress.since);
	synced.watchdogAge = static_cast<int>(now - state.watchdog.since);
	return synced;
}

void ApplyBotSyncState(size_t slot, const BotSyncState &synced)
{
	if (slot >= MaxBots)
		return;
	BotState &state = Bots[slot];
	state.fleeing = synced.fleeing;
	state.fearRolled = synced.fearRolled;
	state.fellBack = synced.fellBack;
	state.carried = synced.carried;
	state.potions = synced.potions;
	state.petrifiedTicks = synced.petrifiedTicks;
	state.baitsVisited = synced.baitsVisited;
	state.trapsKnown = synced.trapsKnown;
	state.trapsSeen = synced.trapsSeen;
	state.baitLingerTicks = synced.baitLingerTicks;
	state.fallBackThinks = synced.fallBackThinks;
	state.regroupBudget = synced.regroupBudget;
	state.thinkCooldown = synced.thinkCooldown;
	const uint32_t now = HeroClock();
	state.kiteCooldown = synced.kiteCooldown;
	state.healCooldown = synced.healCooldown;
	state.hazardWaryUntil = now + static_cast<uint32_t>(synced.hazardWaryLeft);
	state.said = synced.said;
	state.huntingLord = synced.huntingLord;
	state.engagement.ignored = synced.ignoredTarget;
	state.engagement.ignoredUntil = now + static_cast<uint32_t>(synced.ignoredLeft);
	state.progress.goal = static_cast<HeroGoal>(synced.progressGoal);
	state.progress.traps = synced.trapsKnown;
	state.progress.best = synced.progressBest;
	state.progress.since = now - static_cast<uint32_t>(synced.progressAge); // the clocks' difference is all that's read
	state.watchdog.tile = Players[FirstBotId + slot].position.tile;
	state.watchdog.since = now - static_cast<uint32_t>(synced.watchdogAge);
}

bool RunsThisRaid(const Player &player)
{
	return IsBot(player) && IsActive() && RaidWalkSkippedFrames() > 0;
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

std::optional<ScoutedHero> ScoutBot(HeroClassKind cls)
{
	for (size_t i = 0; i < MaxBots && FirstBotId + i < Players.size(); i++) {
		Player &bot = Players[FirstBotId + i];
		if (bot.plractive || PendingSpawn[i])
			continue;
		const uint32_t rngState = GetLCGEngineState();
		CreatePlayer(bot, ToHeroClass(cls));
		SetRndSeed(rngState);
		const bool champion = State.wave >= NumWaves();
		EquipBotGear(bot, cls);
		ScaleBot(bot, champion);
		ApplyRaidTypeToBot(bot);
		ScoutedHero hero;
		hero.cls = cls;
		hero.full = true;
		hero.level = bot.getCharacterLevel();
		hero.life = bot._pMaxHP >> 6;
		if (cls == HeroClassKind::Sorcerer) {
			// Sorcerers fight with Firebolt: this is its damage at the level SpawnBot teaches.
			hero.minDamage = bot._pMagic / 8 + BotFireboltLevel() + 1;
			hero.maxDamage = hero.minDamage + 9;
		} else {
			hero.minDamage = bot._pIMinDam + bot._pDamageMod;
			hero.maxDamage = bot._pIMaxDam + bot._pDamageMod;
		}
		hero.armor = bot.GetArmor();
		hero.potions = BotPotionsBase + State.wave / BotRaidsPerPotion;
		bot = {};
		return hero;
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
	EquipBotGear(bot, cls);
	ScaleBot(bot, champion);
	if (cls == HeroClassKind::Sorcerer) {
		const auto spellLevel = static_cast<uint8_t>(BotFireboltLevel());
		LearnSpell(bot, SpellID::Firebolt, spellLevel);
		LearnSpell(bot, SpellID::Lightning, spellLevel);
	}
	LearnSpell(bot, SpellID::TownPortal, 1);

	InitPlayerGFX(bot);
	InitPlayer(bot, true);
	ApplyRaidTypeToBot(bot);
	SyncInitPlrPos(bot);

	BotState &state = Bots[slot];
	state = {};
	state.cls = cls;
	state.potions = BotPotionsBase + State.wave / BotRaidsPerPotion;

	if (const std::string_view armor = BotGearArmorName(cls); armor.empty())
		Announce(fmt::format("{} the {} storms the keep!", bot._pName, ClassTitle(cls)));
	else
		Announce(fmt::format("{} the {} storms the keep in {}!", bot._pName, ClassTitle(cls), armor));
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
	TickHeroClock();
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
		AvoidFire(bot, state);

		OpenDoorsAround(bot.position.tile);
		RogueCheckTraps(bot, state);
		if (!HoldsBotAlive(bot)) // a joiner gets the result in the bot's life and potions
			DrinkIfHurt(bot, state);
		CheckFear(bot, state);

		if (state.kiteCooldown > 0)
			state.kiteCooldown--;
		if (state.healCooldown > 0)
			state.healCooldown--;

		if (HeroWatchdogFired(state.watchdog, bot.position.tile) && !State.bossDown)
			state.watchdogDue = true; // it may be mid-shot: moved when it next stands
		if (bot._pmode != PM_STAND)
			continue; // mid-swing, mid-step, mid-cast or reeling
		if (state.watchdogDue) {
			state.watchdogDue = false;
			BotNudgeAlong(bot, state.fleeing || state.carried != 0 ? HeroGoal::Entrance : HeroGoal::Vault, state.trapsKnown);
		}
		// First of all: an order given a moment ago is no reason to stand in a fire.
		if (StepOutOfHazard(bot, state)) {
			state.thinkCooldown = 0;
			continue;
		}
		if (state.thinkCooldown > 0 && (bot.walkpath[0] != WALK_NONE || bot.destAction != ACTION_NONE)) {
			state.thinkCooldown--;
			continue;
		}
		if (state.baitLingerTicks > 0) {
			state.baitLingerTicks--;
			continue;
		}
		state.thinkCooldown = ThinkInterval;
		Think(bot, state);
		AvoidFire(bot, state);
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
