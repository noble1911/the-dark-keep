/**
 * @file keeper/keeper_game.cpp
 *
 * The Dark Keep's simulation and network protocol. See keeper_game.hpp.
 */
#include "keeper/keeper_game.hpp"

#include <algorithm>
#include <bitset>
#include <cstring>

#include <fmt/format.h>

#include "engine/direction.hpp"
#include "engine/path.h"
#include "gamemenu.h"
#include "keeper/keeper.hpp"
#include "keeper/keeper_barricade.hpp"
#include "keeper/keeper_bots.hpp"
#include "keeper/keeper_devctl.hpp"
#include "keeper/keeper_editor.hpp"
#include "keeper/keeper_flow.hpp"
#include "keeper/keeper_goals.hpp"
#include "keeper/keeper_heroes.hpp"
#include "keeper/keeper_coop.hpp"
#include "keeper/keeper_lobby.hpp"
#include "keeper/keeper_profile.hpp"
#include "keeper/keeper_raid.hpp"
#include "keeper/keeper_resume.hpp"
#include "keeper/keeper_upgrades.hpp"
#include "levels/gendung.h"
#include "levels/tile_properties.hpp"
#include "missiles.h"
#include "monster.h"
#include "msg.h"
#include "multi.h"
#include "objects.h"
#include "player.h"
#include "plrmsg.h"
#include "utils/endian_swap.hpp"

namespace devilution::keeper {

// Balance: name, missile, mana, engine spell level at level 1, cooldown, unlock rank, icon, flavour.
const std::array<SpellKind, NumSpellKinds> SpellKinds { {
	{ "Nova", MissileID::Nova, 35, 6, 20 * 6, 1, SpellID::Nova, "A ring of lightning bursts out around the target." },
	{ "Wall", MissileID::FireWallControl, 25, 4, 20 * 4, 1, SpellID::FireWall, "A wall of fire across a corridor." },
	{ "Guard", MissileID::Guardian, 30, 4, 20 * 8, 1, SpellID::Guardian, "Summons a guardian that spits fire at heroes for a while." },
	{ "Stone", MissileID::StoneCurse, 20, 4, 20 * 5, 1, SpellID::StoneCurse, "Turns the hero nearest the target to stone for a few seconds." },
	{ "Inferno", MissileID::InfernoControl, 15, 2, 20 * 2, 3, SpellID::Inferno, "A short jet of fire from the target towards the entrance." },
	{ "Fireball", MissileID::Fireball, 20, 3, 20 * 3, 5, SpellID::Fireball, "An explosion at the target, burning everything close by." },
	{ "Chain", MissileID::ChainLightning, 30, 3, 20 * 5, 8, SpellID::ChainLightning, "Lightning leaps from the target to every hero in reach." },
	{ "Shock Wall", MissileID::LightningWallControl, 40, 0, 20 * 5, 10, SpellID::LightningWall, "A wall of lightning. Heroes don't see this one coming." },
	{ "Wave", MissileID::FlameWaveControl, 25, 3, 20 * 4, 12, SpellID::FlameWave, "A wide wave of flame that rolls towards the entrance." },
	{ "Ring", MissileID::RingOfFire, 40, 3, 20 * 8, 15, SpellID::RingOfFire, "A ring of fire around the target, hemming heroes in." },
	{ "Apocalypse", MissileID::Apocalypse, 80, 1, 20 * 25, 18, SpellID::Apocalypse, "Fire falls on every hero within sight of the target." },
	{ "Golem", MissileID::Golem, 35, 1, 20 * 20, 6, SpellID::Golem, "Raises a golem that hunts heroes near the target. Cast it again to mend your golem and send it there." },
} };

KeepState State;

namespace {

constexpr int TicksPerSecond = 20;
constexpr int SetupTicks = 4 * 60 * TicksPerSecond;
constexpr int BreatherTicks = 75 * TicksPerSecond;
/** Mana regenerated per tick, in hundredths (100 = 1 mana per tick). */
constexpr int ManaRegenPerTick = 6; // 1.2 mana per second
/** Minimum spacing between placements (tiles). */
constexpr int PlacementSpacing = 1;
/** During a raid nothing can be placed this close to a hero, and lairs take a while to summon. */
constexpr int RaidBuildHeroDistance = 6;
constexpr int RaidSummonTicks = 3 * TicksPerSecond;
constexpr int LockStrength = 12;
constexpr int BaitUses = 3;
constexpr int StateBroadcastInterval = TicksPerSecond;
/** During a raid the broadcast also keeps the bots in step, so it runs twice as often. */
constexpr int RaidStateBroadcastInterval = TicksPerSecond / 2;
/** Net mana cost of possession, in hundredths per tick (on top of cancelling regeneration). */
constexpr int PossessDrainPerTick = 10;
constexpr int ManaCastTolerance = 5 * ManaScale;
/** Levels fill tiles 16 to 95, so this rules out nothing a hero can reach. */
constexpr int SpellEdgeMargin = 8;
/** Population cap: room for this many supply in raid 1, growing after every raid. */
constexpr int BaseSupply = 16;
constexpr int SupplyPerRaid = 4;
/** How long a host stays to tell joiners about the next game, and how often it says so. */
constexpr int RestartNoticeTicks = 20;
constexpr int RestartResendTicks = 4;
/** A joiner hears the word at any point in the host's notice, so it waits all of it and a little more. */
constexpr int RestartJoinerWaitTicks = RestartNoticeTicks + 20;
/** Later levels start roomier: their monsters take far more Pop. */
constexpr int SupplyPerLevel = 3;
/** Each trap level adds this much damage, and takes this much off its rearm time down to the floor (percent). */
constexpr int TrapDamagePerLevel = 15;
constexpr int TrapRearmPerLevel = 5;
constexpr int TrapRearmFloorPercent = 50;
constexpr int SnareSpellLevelBase = 2;
/** Apocalypse's damage ignores the engine's spell level, so its level raises it directly (percent). */
constexpr int ApocalypseDamagePerLevel = 8;
/**
 * The master's character level already carries a spell's damage up the campaign, so a raise gives the
 * engine only this fraction of a spell level: Nova and Fireball grow 12.5% per engine level, compounding.
 */
constexpr int RaisesPerEngineSpellLevel = 2;
/** Nova and Fireball on a raise that gives no engine level take this much more damage instead (percent): half of 12.5% compounding. */
constexpr int HalfEngineLevelDamagePercent = 6;
/**
 * Nova's damage, in percent. The engine fires its ring from the caster, but here it bursts on the target, so a
 * hero there can take most of its 36 balls: four casts killed a whole level 10 party (measured, docs/BALANCE.md).
 */
constexpr int NovaDamagePercent = 30;
/**
 * A Guardian's shots ignore its spell, so their damage is set here, plus a percentage per raise. Master
 * level is left out: it already gives a Guardian more shots.
 */
constexpr int GuardianDamageBase = 15;
constexpr int GuardianDamagePerRaise = 3;
/** A patrolling monster that hasn't moved for this long gives up on its waypoint and takes the next. */
constexpr int PatrolStuckTicks = 3 * TicksPerSecond;
/** A joiner runs the keep by itself if no host has sent it the keep after this long (the host left). */
constexpr int JoinSyncTimeoutTicks = 10 * TicksPerSecond;
/** "Spend it or guard it": gold left in the vault when a raid ends earns this, up to a cap scaled by the level's strength. */
constexpr int VaultInterestPercent = 5;
constexpr int VaultInterestMaxGold = 100;
/** Opening the gates early pays this for each second of preparation given up, up to a cap. */
constexpr int EarlyCallGoldPerSecond = 1;
constexpr int EarlyCallMaxGold = 60;

enum class Action : uint8_t {
	RequestBuild = 1,
	Built,
	Cast,
	RequestStartRaid,
	RequestLockDoor,
	DoorLocked,
	RequestBait,
	BaitPlaced,
	Possess,
	Release,
	Order,
	SpawnBot,
	RequestSell,
	Sold,
	RequestRally,
	Rallied,
	LairMember,
	RequestUpgrade,
	UpgradeBought,
	RequestWaypoint,
	WaypointAdded,
	PatrolLeg,
	// The keep as it stands, sent to one master who joined mid-game (see SendKeepSnapshot).
	SyncBegin,
	SyncMonsterPosition,
	SyncMonsterHitPoints,
	SyncLair,
	SyncRally,
	SyncWaypoint,
	SyncMember,
	SyncTrap,
	SyncLock,
	SyncBait,
	SyncPossessed,
	SyncBot,
	SyncEnd,
	Restart,
	SyncUpgrade,
	SyncHostSlot,
	BotEscaped,
	HeroEscaped,
	TrapDisarmed,
	BotLooted,
	BotFled,
	HeroLooted,
	HeroFled,
	/** A master opened or closed the Keeper screen between maps. */
	InKeeper,
	/** The host chose the next map: how long until it goes, in `w`. */
	NextMapSoon,
	// Mid-raid state for a master who joins, on top of the rest of the snapshot.
	SyncHero,
	SyncHeroMind,
	SyncBotState,
	SyncBotMind,
	SyncBotPetrified,
	SyncPortal,
	SyncPortalCaster,
	SyncHeroPace,
	SyncBotPace,
	RequestRetryRaid,
	RequestSpeed,
	SetSpeed,
	Ping,
	RaidResult,
	EarlyCallPaid,
};

#pragma pack(push, 1)
struct TCmdKeeper {
	_cmd_id bCmd;
	Action action;
	uint8_t a;
	uint8_t b;
	uint8_t x;
	uint8_t y;
	uint16_t w;
};

struct TCmdKeeperState {
	_cmd_id bCmd;
	uint8_t phase;
	uint8_t wave;
	uint8_t reserved;
	uint32_t phaseTicks;
	int32_t vaultGold;
	int32_t goldLost;
	uint16_t mana[4];
	/** The Keep Lord's hit points (host authoritative; -1 = no Lord). */
	int32_t bossHitPoints;
	/** Every upgrade's tier, so a client that joined late still gets them. */
	std::array<uint8_t, NumUpgrades> upgradeTiers;
	std::array<int32_t, MaxBots> botHitPoints;
	/** Where the host's bots are: see ApplyBotPositionSync. */
	std::array<std::array<uint8_t, 2>, MaxBots> botTiles;
	std::array<uint8_t, MaxBots> botPotions;
	int32_t retryGold;
	int32_t retryGoldLost;
	uint8_t speedLevel;
	uint8_t readyMask;
	uint16_t readyTicks;
	int32_t lastTribute;
	int32_t lastInterest;
	/** Goals lost so far: only the host's view of a raid decides them. */
	uint8_t failedGoals;
};
#pragma pack(pop)

// The high priority buffer stores each message's length in one byte, and CopyPacket trusts it.
static_assert(sizeof(TCmdKeeperState) <= 255);
static_assert(sizeof(TCmdKeeper) <= 255);

int ticksUntilBroadcast = 0;
/** This client joined a keep already in play, and doesn't act as its host until it has been told about it. */
bool awaitingKeepSnapshot = false;
int keepSnapshotTimeout = 0;
/** Monsters the host's snapshot listed; any others only exist in this client's stale copy of the level. */
std::bitset<MaxMonsters> snapshotMonsters;

/** Host only: how long each patrolling monster has stood still. */
struct PatrolWatch {
	Point position;
	int idleTicks = 0;
};
std::array<std::array<PatrolWatch, MaxLairMembers>, MaxLairs> patrolWatch;
/** This client already chose what comes after the siege (the choice changes the setup). */
bool restartChosen = false;
/** Host: the game it chose, and the ticks left before leaving so joiners surely got the word. */
KeepSetup restartSetup;
int restartCountdown = 0;
/** Joiner: ticks until it leaves, once the host has surely gone. */
int joinerRestartWait = 0;
/** Host: masters still have this long before the restart notice goes out, and it has run this long. */
int restartGraceTicks = 0;
int restartGraceElapsed = 0;
/** Host: goldLost when the raid began, to go back to on a retry. */
int retryGoldLost = 0;

/**
 * Turns ahead the host names for a speed change: long enough for the word to reach everyone first (a
 * high-priority message goes out within two ticks, a turn is eight). One arriving late just switches at
 * once; clients stay in step regardless, as none can run a turn ahead of the turns it has from the others.
 */
constexpr uint32_t SpeedSwitchLeadTurns = 2;
/** The named turn never came (its sender's turns stopped): switch anyway rather than wait for ever. */
constexpr int SpeedSwitchTimeoutTicks = 5 * TicksPerSecond;
/** How long the rest have once one master is ready. */
constexpr int ReadyVoteTimeoutTicks = 30 * TicksPerSecond;
/** A resumed keep gives the master at least this long to look around, even if the clock had run out. */
constexpr int ResumeMinPrepTicks = 30 * TicksPerSecond;
/** A speed change the host has announced, waiting for its turn. */
struct PendingSpeed {
	bool active = false;
	uint8_t level = 0;
	uint16_t atTurn = 0;
	uint8_t sender = 0;
	int ticksWaited = 0;
};
PendingSpeed pendingSpeed;
/** State snapshots that disagreed with this client's speed in a row. One can be from just before the switch. */
int speedMismatches = 0;

void Send(Action action, uint8_t a = 0, uint8_t b = 0, Point position = {}, uint16_t w = 0)
{
	TCmdKeeper cmd;
	cmd.bCmd = CMD_KEEPER;
	cmd.action = action;
	cmd.a = a;
	cmd.b = b;
	cmd.x = static_cast<uint8_t>(position.x);
	cmd.y = static_cast<uint8_t>(position.y);
	cmd.w = Swap16LE(w);
	NetSendHiPri(MyPlayerId, reinterpret_cast<std::byte *>(&cmd), sizeof(cmd));
}

void BroadcastState()
{
	TCmdKeeperState cmd;
	cmd.bCmd = CMD_KEEPER_STATE;
	cmd.phase = static_cast<uint8_t>(State.phase);
	cmd.wave = State.wave;
	cmd.reserved = static_cast<uint8_t>((State.bossDown ? 1 : 0) | (State.portalEscape ? 2 : 0) | (std::min(RaidMasters(), 7) << 2) | (static_cast<int>(GameDifficulty()) << 5));
	cmd.phaseTicks = Swap32LE(static_cast<uint32_t>(std::max(State.phaseTicks, 0)));
	cmd.vaultGold = static_cast<int32_t>(Swap32LE(static_cast<uint32_t>(State.vaultGold)));
	cmd.goldLost = static_cast<int32_t>(Swap32LE(static_cast<uint32_t>(State.goldLost)));
	for (size_t i = 0; i < 4; i++)
		cmd.mana[i] = Swap16LE(static_cast<uint16_t>(State.mana[i] / ManaScale));
	const Monster *boss = FindBoss();
	cmd.bossHitPoints = static_cast<int32_t>(Swap32LE(static_cast<uint32_t>(boss != nullptr ? boss->hitPoints : -1)));
	cmd.upgradeTiers = UpgradeTiers();
	for (size_t i = 0; i < MaxBots; i++) {
		cmd.botHitPoints[i] = static_cast<int32_t>(Swap32LE(static_cast<uint32_t>(BotLifeForSync(i))));
		const BotSyncFacts facts = BotFactsForSync(i);
		cmd.botTiles[i] = { static_cast<uint8_t>(facts.tile.x), static_cast<uint8_t>(facts.tile.y) };
		cmd.botPotions[i] = facts.potions;
	}
	cmd.retryGold = static_cast<int32_t>(Swap32LE(static_cast<uint32_t>(State.retryGold)));
	cmd.retryGoldLost = static_cast<int32_t>(Swap32LE(static_cast<uint32_t>(retryGoldLost)));
	cmd.speedLevel = static_cast<uint8_t>(GameSpeedLevel());
	cmd.readyMask = State.readyMask;
	cmd.readyTicks = Swap16LE(static_cast<uint16_t>(std::clamp(State.readyTicks, 0, 0xFFFF)));
	cmd.lastTribute = static_cast<int32_t>(Swap32LE(static_cast<uint32_t>(State.lastTribute)));
	cmd.lastInterest = static_cast<int32_t>(Swap32LE(static_cast<uint32_t>(State.lastInterest)));
	cmd.failedGoals = FailedGoals();
	DevctlNoteSent(sizeof(cmd) + sizeof(TPktHdr));
	NetSendHiPri(MyPlayerId, reinterpret_cast<std::byte *>(&cmd), sizeof(cmd));
	ticksUntilBroadcast = State.phase == Phase::Raid && DevctlBotSyncOn() ? RaidStateBroadcastInterval : StateBroadcastInterval;
}

void Announce(std::string_view message, UiFlags color = UiFlags::ColorGold)
{
	EventPlrMsg(message, color);
}

bool IsFreeFloor(Point tile)
{
	return InDungeonBounds(tile) && IsTileWalkable(tile) && dMonster[tile.x][tile.y] == 0 && dPlayer[tile.x][tile.y] == 0 && !IsObjectAtPosition(tile);
}

std::optional<size_t> FindLevelMonsterType(_monster_id type)
{
	for (size_t i = 0; i < LevelMonsterTypeCount; i++) {
		if (LevelMonsterTypes[i].type == type)
			return i;
	}
	return std::nullopt;
}

bool IsSiegeOver()
{
	return State.phase == Phase::Victory || State.phase == Phase::Defeat;
}

bool IsHeroNear(Point tile, int distance)
{
	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		const Monster &monster = Monsters[ActiveMonsters[i]];
		if (IsHero(monster) && monster.hitPoints > 0 && monster.position.tile.WalkingDistance(tile) < distance)
			return true;
	}
	return IsBotNear(tile, distance);
}

bool IsPlacementCrowded(Point tile)
{
	for (const Lair &lair : State.lairs) {
		if (lair.active && lair.position.WalkingDistance(tile) < PlacementSpacing)
			return true;
	}
	for (const Trap &trap : State.traps) {
		if (trap.active && trap.position.WalkingDistance(tile) < 2)
			return true;
	}
	for (const Bait &bait : State.baits) {
		if (bait.active && bait.position.WalkingDistance(tile) < 2)
			return true;
	}
	return false;
}

int CountMasters()
{
	int masters = 0;
	for (size_t id = 0; id < NetPlayerSlots; id++) {
		if (Players[id].plractive)
			masters++;
	}
	return masters;
}

int TributeFor(uint8_t wave)
{
	return ScaleByHeroStrength(100 + 25 * wave);
}

/**
 * What a retried raid takes back and what it leaves.
 *
 * Back as they were at the raid's start: the vault (less what was built in the raid: a lair bought with
 * bounty gold isn't free, and one sold mid-raid is gone) and the master's XP and stats from the raid. Each
 * attempt pays only if it isn't taken back, so retrying can't farm XP however often it's done; leaving
 * instead of retrying keeps that attempt's XP, as it always has.
 * Left alone: every structure, and what the raid used up of them (spent bait, broken locks, disarmed traps).
 *
 * Runs on every client. Nothing here is sent: each client has the same raiders to clear.
 */
void ClearLostRaid()
{
	for (size_t pid = 0; pid < State.possessed.size(); pid++)
		EndPossession(pid);
	ClearRaiders();
	// The Lord, if it fell, comes back through UpdateBoss now that the siege is on again.
	State.bossDown = false;
	State.portalEscape = false;
	for (Trap &trap : State.traps)
		trap.cooldown = 0;
	HealKeepMonsters();
	DismissRaidSummary();
	ProfileTakeBackRaid();
}

/** Runs on every client when the phase changes (locally on the host, from the snapshot elsewhere). */
void AnnouncePhase(Phase from, Phase to)
{
	if (from == to)
		return;
	// Only a retry leaves a defeat. Every client gets here, whichever way it hears of the new phase.
	if (from == Phase::Defeat)
		ClearLostRaid();
	// Raid stats: every client sees the phase change, so each keeps its own tally.
	if (from == Phase::Raid)
		NoteRaidEnded(to, to == Phase::Breather ? State.lastTribute : 0, to == Phase::Breather ? State.lastInterest : 0);
	if (to == Phase::Raid)
		NoteRaidStarted(State.wave);
	if (from == Phase::Raid) {
		ClosePortal();
		RemoveAllBots();
		DismissGolems();
	}
	switch (to) {
	case Phase::Raid:
		Announce(State.wave >= NumWaves() ? std::string("The champions have come for your gold!") : fmt::format("Raid {}: heroes approach the keep!", State.wave), UiFlags::ColorRed);
		break;
	case Phase::Breather:
		if (from == Phase::Defeat)
			Announce(fmt::format("The keep holds on: raid {} will come again.", State.wave), UiFlags::ColorRed);
		// Only coming out of a raid: a joiner's snapshot doesn't carry what the last raid paid.
		else if (from == Phase::Raid)
			Announce(State.lastInterest > 0 ? fmt::format("Raid {} is over. The dark lands pay {} gold: tribute, and {} interest on your vault.", State.wave - 1, State.lastTribute, State.lastInterest)
			                                : fmt::format("Raid {} is over. The dark lands pay {} gold in tribute.", State.wave - 1, State.lastTribute));
		break;
	case Phase::Victory:
		// Not for a master who connects after the win: seeing the keep stand isn't defending it.
		if (from == Phase::Raid) {
			NoteVictory();
			AwardLevelGoals();
		}
		Announce("The keep stands! Every raid has been repelled.");
		break;
	case Phase::Defeat:
		Announce("The Keep Lord is slain and the heroes got away. The keep has fallen.", UiFlags::ColorRed);
		break;
	case Phase::Setup:
		break;
	}
}

/**
 * This game may write and delete the resume slot. A game takes the slot if it resumed from it or found it
 * empty; otherwise it leaves the existing save alone until it survives a raid, and only then replaces it,
 * saying so. A replay's defeat, or a quit in its preparation, costs the other save nothing.
 */
bool ownsResumeSlot = false;

/** Only a single player's own campaign: progress is at stake nowhere else, and a test flag's game isn't a real one. */
bool CanSaveKeep()
{
	return !gbIsMultiplayer && CurrentSetup().mode == GameMode::Campaign && !IsEditorTestRun() && OverrideRank() == 0 && TestStartWave == 0;
}

/** Writes the keep as it stands. Called when not raiding: nothing about a raid is in the file. */
void SaveKeep()
{
	if (!CanSaveKeep() || !ownsResumeSlot)
		return;
	ResumeData data;
	const KeepSetup &setup = CurrentSetup();
	data.level = setup.campaignLevel;
	data.seed = setup.seed;
	data.mapFile = setup.mapFile;
	data.entrance = State.entrance;
	data.vault = State.vault;
	data.wave = State.wave;
	data.phase = static_cast<int>(State.phase);
	data.phaseTicks = State.phaseTicks;
	data.gold = State.vaultGold;
	data.goldLost = State.goldLost;
	data.mana = State.mana[MyPlayerId];
	data.mapXp = MapXpEarned();
	data.upgrades = UpgradeTiers();
	for (size_t slot = 0; slot < MaxLairs; slot++) {
		const Lair &lair = State.lairs[slot];
		if (!lair.active)
			continue;
		ResumeData::SavedLair saved;
		saved.kind = lair.kind;
		saved.monsterType = static_cast<int>(BuildKinds[lair.kind].type);
		saved.cost = RefundFor({ StructureType::Lair, slot });
		saved.position = lair.position;
		saved.hasRally = lair.hasRally;
		saved.rally = lair.rally;
		saved.route.assign(lair.route.begin(), lair.route.begin() + lair.routeCount);
		data.lairs.push_back(std::move(saved));
	}
	for (size_t slot = 0; slot < MaxTraps; slot++) {
		const Trap &trap = State.traps[slot];
		if (trap.active)
			data.traps.push_back({ trap.kind, trap.position, 0, RefundFor({ StructureType::Trap, slot }) });
	}
	for (size_t slot = 0; slot < MaxLocks; slot++) {
		const DoorLock &lock = State.locks[slot];
		if (lock.active)
			data.locks.push_back({ 0, lock.position, std::clamp(lock.strength, 0, 255), RefundFor({ StructureType::Lock, slot }) });
	}
	for (size_t slot = 0; slot < MaxBaits; slot++) {
		const Bait &bait = State.baits[slot];
		if (bait.active)
			data.baits.push_back({ 0, bait.position, std::clamp(bait.uses, 0, 255), RefundFor({ StructureType::Bait, slot }) });
	}
	WriteResumeFile(data);
}

void AnnounceReadyChanges(uint8_t before, uint8_t after)
{
	for (size_t id = 0; id < NetPlayerSlots; id++) {
		const auto bit = static_cast<uint8_t>(1U << id);
		if ((after & bit) != 0 && (before & bit) == 0 && Players[id].plractive)
			Announce(fmt::format("{} is ready for the raid.", Players[id]._pName));
	}
}

void SetPhase(Phase phase)
{
	if (phase == Phase::Breather && State.phase == Phase::Raid && !ownsResumeSlot && CanSaveKeep()) {
		ownsResumeSlot = true;
		Announce("Autosaved: this keep replaces your earlier Continue.");
	}
	State.readyMask = 0;
	State.readyTicks = 0;
	const bool leavingRaid = State.phase == Phase::Raid && phase != Phase::Raid;
	// AnnouncePhase takes the bots off the map: joiners first hear how their last fights ended.
	if (IsHost() && leavingRaid)
		BroadcastState();
	AnnouncePhase(State.phase, phase);
	State.phase = phase;
	switch (phase) {
	case Phase::Setup:
		State.phaseTicks = SetupTicks;
		break;
	case Phase::Breather:
		State.phaseTicks = BreatherTicks;
		break;
	default:
		State.phaseTicks = 0;
		break;
	}
	if (IsHost())
		BroadcastState();
	if (phase == Phase::Breather)
		SaveKeep();
	else if ((phase == Phase::Defeat || phase == Phase::Victory) && CanSaveKeep() && ownsResumeSlot)
		DeleteResumeFile(); // what's decided stays decided; a retry writes it again from its respite
	// Joiners take the host's numbers for the raid's summary: after the phase, as that is what ends their tally.
	if (leavingRaid && IsHost() && gbIsMultiplayer) {
		for (size_t field = 0; field < NumRaidResultFields; field++) {
			const auto value = static_cast<uint32_t>(RaidResultField(field));
			Send(Action::RaidResult, static_cast<uint8_t>(field), 0, { static_cast<int>((value >> 16) & 0xFF), static_cast<int>((value >> 24) & 0xFF) }, static_cast<uint16_t>(value & 0xFFFF));
		}
	}
}

bool IsMemberAlive(int16_t monsterId, _monster_id type)
{
	if (monsterId < 0)
		return false;
	const Monster &monster = Monsters[monsterId];
	return monster.hitPoints > 0 && monster.mode != MonsterMode::Death && monster.position.tile != GolemHoldingCell && monster.type().type == type && !monster.isInvalid;
}

/** Records a lair member; the id may have belonged to another lair's monster that has since died. */
void ClaimMonster(Lair &lair, size_t index, int16_t monsterId)
{
	for (Lair &other : State.lairs) {
		for (int16_t &member : other.members) {
			if (member == monsterId)
				member = -1;
		}
	}
	lair.members[index] = monsterId;
}

/** Host only: tops a lair back up to full strength. */
void RefillLair(Lair &lair)
{
	if (!lair.active || lair.summonTicks > 0)
		return;
	const BuildInfo &kind = BuildKinds[lair.kind];
	const std::optional<size_t> typeIndex = FindLevelMonsterType(kind.type);
	if (!typeIndex)
		return;
	for (size_t i = 0; i < kind.count && i < MaxLairMembers; i++) {
		if (IsMemberAlive(lair.members[i], kind.type))
			continue;
		lair.members[i] = -1;
		if (ActiveMonsterCount >= MaxMonsters)
			return;
		const std::optional<Point> spawnTile = FindClosestValidPosition(IsFreeFloor, lair.position, 0, 4);
		if (!spawnTile)
			return;
		const auto monsterId = static_cast<int16_t>(ActiveMonsters[ActiveMonsterCount]);
		ClaimMonster(lair, i, monsterId);
		SpawnMonster(*spawnTile, Direction::South, *typeIndex);
		// Everyone learns which monster belongs to the lair (for selling and its rally point).
		Send(Action::LairMember, static_cast<uint8_t>(&lair - State.lairs.data()), static_cast<uint8_t>(i), {}, static_cast<uint16_t>(monsterId + 1));
	}
}

/** Takes a sold lair's monster off the map on every client (no corpse, no bounty). */
void DismissMonster(int16_t monsterId)
{
	if (monsterId < 0 || static_cast<size_t>(monsterId) >= MaxMonsters)
		return;
	Monster &monster = Monsters[monsterId];
	if (monster.isInvalid || monster.hitPoints <= 0 || monster.mode == MonsterMode::Death || monster.position.tile == GolemHoldingCell)
		return;
	for (size_t pid = 0; pid < State.possessed.size(); pid++) {
		if (State.possessed[pid] == monsterId)
			EndPossession(pid);
	}
	M_ClearSquares(monster);
	monster.hitPoints = 0;
	monster.isInvalid = true;
}

/** Where a lair's monsters stand guard. */
Point GuardPost(const Lair &lair)
{
	return lair.hasRally ? lair.rally : lair.position;
}

/** Where a member heads on its current patrol leg. */
Point PatrolTarget(const Lair &lair, size_t member)
{
	const uint8_t leg = lair.patrolLeg[member];
	return leg > 0 && leg <= lair.routeCount ? lair.route[leg - 1] : GuardPost(lair);
}

/** Points a living member at its post (or waypoint); patrolling monsters look for enemies around themselves. */
void ApplyMemberPost(const Lair &lair, size_t member)
{
	const int16_t monsterId = lair.members[member];
	if (!IsMemberAlive(monsterId, BuildKinds[lair.kind].type))
		return;
	SetMonsterHome(static_cast<size_t>(monsterId), PatrolTarget(lair, member));
	SetMonsterRoaming(static_cast<size_t>(monsterId), lair.routeCount > 0);
}

void RefillLairs()
{
	for (Lair &lair : State.lairs)
		RefillLair(lair);
}

/** Host: pays for the time given up. Before the phase changes, which ends the timer. */
void PayEarlyCall(int bonus)
{
	if (!IsHost() || bonus <= 0)
		return;
	State.vaultGold += bonus;
	Send(Action::EarlyCallPaid, 0, 0, {}, static_cast<uint16_t>(bonus));
	BroadcastState();
}

void StartRaid()
{
	// Leaving mid-raid comes back to this moment, with what the respite built.
	SaveKeep();
	State.retryGold = State.vaultGold;
	retryGoldLost = State.goldLost;
	const int earlyCallBonus = EarlyCallBonusNow();
	SetPhase(Phase::Raid);
	PayEarlyCall(earlyCallBonus);
	if (IsHost())
		StartWave(State.wave);
}

/** Host: gold spent on (negative: got back from) a structure. A raid's own building survives a retry, so a retry doesn't refund it. */
void NoteRaidBuilding(int cost)
{
	if (State.phase == Phase::Raid)
		State.retryGold -= cost;
}

/** Host: the raid comes if everyone here is ready, or the vote has run out. @return true if it did. */
bool UpdateReadyVote()
{
	if (State.readyMask == 0)
		return false;
	// Someone who left doesn't stay ready for whoever gets their slot, and doesn't hold anyone either.
	for (size_t id = 0; id < NetPlayerSlots; id++) {
		if (!Players[id].plractive)
			State.readyMask &= static_cast<uint8_t>(~(1U << id));
	}
	if (State.readyMask == 0) {
		State.readyTicks = 0;
		return false;
	}
	if (ReadyMasterCount() < MastersInKeepCount() && --State.readyTicks > 0)
		return false;
	StartRaid();
	return true;
}

void HostVoteReady(const Player &voter)
{
	if (State.phase != Phase::Setup && State.phase != Phase::Breather)
		return;
	if (!gbIsMultiplayer) {
		StartRaid();
		return;
	}
	const auto bit = static_cast<uint8_t>(1U << voter.getId());
	if ((State.readyMask & bit) != 0) {
		// The host's second word is the override. Anyone else has changed their mind.
		if (&voter == MyPlayer) {
			StartRaid();
			return;
		}
		State.readyMask &= static_cast<uint8_t>(~bit);
		if (State.readyMask == 0)
			State.readyTicks = 0;
		BroadcastState();
		return;
	}
	if (State.readyMask == 0)
		State.readyTicks = ReadyVoteTimeoutTicks;
	AnnounceReadyChanges(State.readyMask, State.readyMask | bit);
	State.readyMask |= bit;
	if (!UpdateReadyVote())
		BroadcastState();
}

void AnnounceSpeed()
{
	Announce(fmt::format("Game speed: {}x", GameSpeedMultiplier()));
}

void HostRequestSpeed(uint8_t level)
{
	if (level >= GameSpeeds.size())
		return;
	if (!gbIsMultiplayer) {
		ApplyGameSpeedLevel(level);
		AnnounceSpeed();
		return;
	}
	Send(Action::SetSpeed, level, 0, {}, static_cast<uint16_t>(multi_last_turn_from(MyPlayerId) + SpeedSwitchLeadTurns));
}

void UpdatePendingSpeed()
{
	if (!pendingSpeed.active)
		return;
	const auto turnsLeft = static_cast<int16_t>(pendingSpeed.atTurn - static_cast<uint16_t>(multi_last_turn_from(pendingSpeed.sender)));
	const bool senderGone = pendingSpeed.sender >= NetPlayerSlots || !Players[pendingSpeed.sender].plractive;
	if (turnsLeft > 0 && !senderGone && ++pendingSpeed.ticksWaited < SpeedSwitchTimeoutTicks)
		return;
	pendingSpeed.active = false;
	speedMismatches = 0;
	ApplyGameSpeedLevel(pendingSpeed.level);
	DevNoteSpeedSwitch(pendingSpeed.level, multi_last_turn_from(pendingSpeed.sender));
	AnnounceSpeed();
}

void HostRetryRaid()
{
	// The restart's countdown would pull everyone out of the retried raid.
	if (State.phase != Phase::Defeat || restartChosen)
		return;
	State.vaultGold = std::max(State.retryGold, 0);
	State.goldLost = retryGoldLost;
	// Told first: the monsters RefillLairs raises must arrive after the raiders' slots are clear.
	SetPhase(Phase::Breather);
	RefillLairs();
}

void EndRaid()
{
	if (State.bossDown) {
		// The party was wiped out before anyone escaped: the Lord rises again (UpdateBoss respawns it).
		State.bossDown = false;
		State.portalEscape = false;
		Announce("The heroes were stopped. The Keep Lord rises again!", UiFlags::ColorRed);
	}
	if (State.wave >= NumWaves()) {
		SetPhase(Phase::Victory);
		return;
	}
	// Worked out here, from the host's own vault: a joiner's copy may be behind, so it is told the result.
	State.lastInterest = VaultInterestOn(State.vaultGold);
	State.lastTribute = TributeFor(State.wave) + State.lastInterest;
	State.vaultGold += State.lastTribute;
	State.wave++;
	for (Trap &trap : State.traps)
		trap.cooldown = 0;
	RefillLairs();
	SetPhase(Phase::Breather);
}

// ---- host-side request handling ----------------------------------------------

void ApplyBuilt(uint8_t slot, uint8_t kind, Point tile);
void ApplyDoorLocked(uint8_t slot, Point doorPosition);
void ApplyBaitPlaced(uint8_t slot, Point tile);

void HostBuild(const Player &requester, size_t kind, Point tile)
{
	if (kind >= BuildKinds.size() || !CanBuildAt(kind, tile))
		return;
	uint8_t slot = 0xFF;
	if (IsLairKind(kind)) {
		const auto it = std::find_if(State.lairs.begin(), State.lairs.end(), [](const Lair &lair) { return !lair.active; });
		if (it == State.lairs.end())
			return;
		slot = static_cast<uint8_t>(it - State.lairs.begin());
	} else {
		const auto it = std::find_if(State.traps.begin(), State.traps.end(), [](const Trap &trap) { return !trap.active; });
		if (it == State.traps.end())
			return;
		slot = static_cast<uint8_t>(it - State.traps.begin());
	}
	State.vaultGold -= BuildKinds[kind].cost;
	NoteRaidBuilding(BuildKinds[kind].cost);
	Send(Action::Built, slot, static_cast<uint8_t>(kind), tile, static_cast<uint16_t>(requester.getId()));
	// Now, not when the message comes back: requests that arrive together (two masters, or one quick one)
	// must see this one when they check its slot, its tile and the Pop it takes.
	ApplyBuilt(slot, static_cast<uint8_t>(kind), tile);
	BroadcastState();
}

void ApplyBuilt(uint8_t slot, uint8_t kind, Point tile)
{
	if (kind >= BuildKinds.size())
		return;
	const int summon = State.phase == Phase::Raid ? RaidSummonTicks : 0;
	if (IsLairKind(kind)) {
		if (slot >= MaxLairs)
			return;
		Lair &lair = State.lairs[slot];
		if (lair.active && lair.kind == kind && lair.position == tile)
			return; // a joiner already told by the snapshot
		lair = {};
		lair.active = true;
		lair.kind = kind;
		lair.position = tile;
		lair.summonTicks = summon;
		lair.members.fill(-1);
		if (IsHost())
			RefillLair(lair);
	} else {
		if (slot >= MaxTraps)
			return;
		Trap &trap = State.traps[slot];
		if (trap.active && trap.kind == kind - NumLairKinds && trap.position == tile)
			return;
		trap.active = true;
		trap.position = tile;
		trap.cooldown = summon;
		trap.kind = static_cast<uint8_t>(kind - NumLairKinds);
	}
}

void HostLockDoor(Point doorPosition)
{
	if (!CanLockDoorAt(doorPosition))
		return;
	const auto it = std::find_if(State.locks.begin(), State.locks.end(), [](const DoorLock &lock) { return !lock.active; });
	if (it == State.locks.end())
		return;
	const auto slot = static_cast<uint8_t>(it - State.locks.begin());
	ApplyDoorLocked(slot, doorPosition);
	if (!State.locks[slot].active)
		return; // no structure, no charge
	State.vaultGold -= LockDoorCost;
	NoteRaidBuilding(LockDoorCost);
	Send(Action::DoorLocked, slot, 0, doorPosition);
	BroadcastState();
}

void ApplyDoorLocked(uint8_t slot, Point doorPosition)
{
	if (slot >= MaxLocks || (State.locks[slot].active && State.locks[slot].position == doorPosition))
		return;
	if (UsesBarricades()) {
		if (!PlaceBarricade(slot, doorPosition))
			return;
	} else {
		Object *door = FindObjectAtPosition(doorPosition);
		if (door == nullptr || !door->isDoor())
			return;
		if (!KeeperIsDoorClosed(*door))
			KeeperOperateDoor(*door, false); // every client closes it locally
	}
	DoorLock &lock = State.locks[slot];
	lock.active = true;
	lock.position = doorPosition;
	lock.strength = LockStrength;
	RebuildPaths();
}

void HostPlaceBait(Point tile)
{
	if (!CanPlaceBaitAt(tile))
		return;
	const auto it = std::find_if(State.baits.begin(), State.baits.end(), [](const Bait &bait) { return !bait.active; });
	if (it == State.baits.end())
		return;
	const auto slot = static_cast<uint8_t>(it - State.baits.begin());
	State.vaultGold -= BaitCost;
	NoteRaidBuilding(BaitCost);
	Send(Action::BaitPlaced, slot, 0, tile);
	ApplyBaitPlaced(slot, tile);
	BroadcastState();
}

void ApplyBaitPlaced(uint8_t slot, Point tile)
{
	if (slot >= MaxBaits || (State.baits[slot].active && State.baits[slot].position == tile))
		return;
	Bait &bait = State.baits[slot];
	bait.active = true;
	bait.position = tile;
	bait.uses = BaitUses;
}

void ApplySold(uint8_t type, uint8_t slot);

void HostSell(StructureRef ref)
{
	if (!CanSell(ref))
		return;
	const int refund = RefundFor(ref);
	State.vaultGold += refund;
	NoteRaidBuilding(-refund);
	Send(Action::Sold, static_cast<uint8_t>(ref.type), static_cast<uint8_t>(ref.slot));
	// Now, not when the message comes back: a second request in the same turn mustn't refund again.
	ApplySold(static_cast<uint8_t>(ref.type), static_cast<uint8_t>(ref.slot));
	BroadcastState();
}

void ApplySold(uint8_t type, uint8_t slot)
{
	switch (static_cast<StructureType>(type)) {
	case StructureType::Lair:
		if (slot < MaxLairs) {
			Lair &lair = State.lairs[slot];
			for (const int16_t member : lair.members) {
				if (IsMemberAlive(member, BuildKinds[lair.kind].type))
					DismissMonster(member);
			}
			lair = {};
			lair.members.fill(-1);
		}
		break;
	case StructureType::Trap:
		if (slot < MaxTraps)
			State.traps[slot] = {};
		break;
	case StructureType::Bait:
		if (slot < MaxBaits)
			State.baits[slot] = {};
		break;
	case StructureType::Lock:
		if (slot < MaxLocks && State.locks[slot].active) {
			State.locks[slot] = {};
			RemoveBarricade(slot);
			RebuildPaths();
		}
		break;
	}
}

void HostRally(uint8_t slot, bool set, Point tile)
{
	if (slot >= MaxLairs || !State.lairs[slot].active || (set && !CanRallyTo(slot, tile)))
		return;
	Send(Action::Rallied, slot, set ? 1 : 0, tile);
}

void ApplyRally(uint8_t slot, bool set, Point tile)
{
	if (slot >= MaxLairs)
		return;
	Lair &lair = State.lairs[slot];
	lair.hasRally = set;
	lair.rally = set ? tile : Point {};
	// A new order replaces the old patrol, and clearing the rally point clears the route with it.
	lair.routeCount = 0;
	lair.patrolLeg.fill(0);
	for (size_t i = 0; i < MaxLairMembers; i++)
		ApplyMemberPost(lair, i);
}

void ApplyWaypoint(uint8_t slot, uint8_t index, Point tile)
{
	if (slot >= MaxLairs || index >= MaxPatrolPoints || !InDungeonBounds(tile))
		return;
	Lair &lair = State.lairs[slot];
	lair.route[index] = tile;
	lair.routeCount = std::max<uint8_t>(lair.routeCount, index + 1);
	for (size_t i = 0; i < MaxLairMembers; i++)
		ApplyMemberPost(lair, i);
}

void HostWaypoint(uint8_t slot, Point tile)
{
	if (!CanAddWaypoint(slot, tile))
		return;
	// Applied here too, so that a second request that arrives before this echo gets the next index.
	const uint8_t index = State.lairs[slot].routeCount;
	ApplyWaypoint(slot, index, tile);
	Send(Action::WaypointAdded, slot, index, tile);
}

void ApplyPatrolLeg(uint8_t slot, uint8_t member, uint8_t leg)
{
	if (slot >= MaxLairs || member >= MaxLairMembers)
		return;
	Lair &lair = State.lairs[slot];
	lair.patrolLeg[member] = leg <= lair.routeCount ? leg : 0;
	ApplyMemberPost(lair, member);
}

/** Host only: moves monsters that reached their waypoint (or can't) on to the next, for every client. */
void UpdatePatrols()
{
	for (size_t slot = 0; slot < MaxLairs; slot++) {
		const Lair &lair = State.lairs[slot];
		if (!lair.active || lair.routeCount == 0)
			continue;
		for (size_t i = 0; i < MaxLairMembers; i++) {
			const int16_t monsterId = lair.members[i];
			if (!IsMemberAlive(monsterId, BuildKinds[lair.kind].type))
				continue;
			const Monster &monster = Monsters[monsterId];
			if (IsPossessed(monster) || monster.mode != MonsterMode::Stand || (monster.flags & MFLAG_NO_ENEMY) == 0)
				continue;
			PatrolWatch &watch = patrolWatch[slot][i];
			if (watch.position != monster.position.tile) {
				watch.position = monster.position.tile;
				watch.idleTicks = 0;
			}
			const bool arrived = monster.position.tile.WalkingDistance(PatrolTarget(lair, i)) <= 1;
			if (!arrived && ++watch.idleTicks < PatrolStuckTicks)
				continue;
			watch.idleTicks = 0;
			const auto next = static_cast<uint8_t>((lair.patrolLeg[i] + 1) % (lair.routeCount + 1));
			ApplyPatrolLeg(static_cast<uint8_t>(slot), static_cast<uint8_t>(i), next);
			Send(Action::PatrolLeg, static_cast<uint8_t>(slot), static_cast<uint8_t>(i), { next, 0 });
		}
	}
}

void ApplyLairMember(uint8_t slot, uint8_t index, int monsterId)
{
	if (slot >= MaxLairs || index >= MaxLairMembers || monsterId < 0 || static_cast<size_t>(monsterId) >= MaxMonsters)
		return;
	ClaimMonster(State.lairs[slot], index, static_cast<int16_t>(monsterId));
	Lair &lair = State.lairs[slot];
	// A new monster starts its patrol over from the guard post.
	lair.patrolLeg[index] = 0;
	if (lair.hasRally || lair.routeCount > 0)
		ApplyMemberPost(lair, index);
}

// ---- resuming a saved keep -------------------------------------------------------

bool IsFloorForResume(Point tile)
{
	return InDungeonBounds(tile) && IsTileWalkable(tile) && !IsObjectAtPosition(tile);
}

/**
 * Puts the saved keep back. Single player, so the requests' host-side work is done directly. Anything that
 * can't come back (a monster respecced away since, a floor that has moved) is paid back at its respite price.
 */
void ApplyResume(const ResumeData &data)
{
	ownsResumeSlot = true;
	if (data.level != CurrentSetup().campaignLevel || data.entrance != State.entrance || data.vault != State.vault) {
		// The map isn't the one that was saved: this is a fresh start on it, and the file is no use.
		DeleteResumeFile();
		Announce("The saved keep no longer fits this map: starting the level afresh.", UiFlags::ColorRed);
		return;
	}
	State.wave = static_cast<uint8_t>(data.wave);
	State.phase = static_cast<Phase>(data.phase);
	State.phaseTicks = std::max(data.phaseTicks, ResumeMinPrepTicks);
	State.vaultGold = data.gold;
	State.retryGold = data.gold;
	State.goldLost = data.goldLost;
	State.mana[MyPlayerId] = data.mana;
	ProfileSetMapXp(data.mapXp);
	for (size_t i = 0; i < NumUpgrades; i++)
		SetUpgradeTierQuietly(static_cast<UpgradeId>(i), data.upgrades[i]);

	int refund = 0;
	uint8_t slot = 0;
	for (const ResumeData::SavedLair &saved : data.lairs) {
		const auto kind = static_cast<size_t>(saved.kind);
		if (!IsLairKind(kind) || !IsKindMustered(kind) || static_cast<int>(BuildKinds[kind].type) != saved.monsterType || !IsFloorForResume(saved.position)) {
			refund += saved.cost;
			continue;
		}
		ApplyBuilt(slot, static_cast<uint8_t>(kind), saved.position);
		if (saved.hasRally)
			ApplyRally(slot, true, saved.rally);
		for (size_t i = 0; i < saved.route.size(); i++)
			ApplyWaypoint(slot, static_cast<uint8_t>(i), saved.route[i]);
		slot++;
	}
	slot = 0;
	for (const ResumeData::SavedPiece &saved : data.traps) {
		if (static_cast<size_t>(saved.kind) >= NumTrapKinds || !IsTrapAvailable(static_cast<size_t>(saved.kind)) || !IsFloorForResume(saved.position)) {
			refund += saved.cost;
			continue;
		}
		ApplyBuilt(slot++, static_cast<uint8_t>(NumLairKinds + saved.kind), saved.position);
	}
	slot = 0;
	for (const ResumeData::SavedPiece &saved : data.locks) {
		ApplyDoorLocked(slot, saved.position);
		if (State.locks[slot].active) {
			State.locks[slot].strength = saved.amount;
			slot++;
		} else {
			refund += saved.cost;
		}
	}
	slot = 0;
	for (const ResumeData::SavedPiece &saved : data.baits) {
		if (!IsFloorForResume(saved.position)) {
			refund += saved.cost;
			continue;
		}
		ApplyBaitPlaced(slot, saved.position);
		State.baits[slot++].uses = saved.amount;
	}
	State.vaultGold += refund;
	State.retryGold += refund;
	if (refund > 0)
		Announce(fmt::format("Some of the saved keep couldn't come back: {} gold returned to the vault.", refund));
	Announce(fmt::format("Welcome back, master. Raid {} is coming.", State.wave));
	BroadcastState();
}

// ---- mid-game joiners ----------------------------------------------------------

/** Sends a run of commands to one player straight away, as many to a packet as fit. */
class DirectCommands {
public:
	explicit DirectCommands(size_t playerId)
	    : mask_(1U << playerId)
	{
	}

	void Add(Action action, uint8_t a = 0, uint8_t b = 0, uint8_t x = 0, uint8_t y = 0, uint16_t w = 0)
	{
		TCmdKeeper &cmd = commands_[count_++];
		cmd.bCmd = CMD_KEEPER;
		cmd.action = action;
		cmd.a = a;
		cmd.b = b;
		cmd.x = x;
		cmd.y = y;
		cmd.w = Swap16LE(w);
		if (count_ == commands_.size())
			Flush();
	}

	void Add(Action action, uint8_t a, uint8_t b, Point position, uint16_t w = 0)
	{
		Add(action, a, b, static_cast<uint8_t>(position.x), static_cast<uint8_t>(position.y), w);
	}

	void Flush()
	{
		if (count_ > 0)
			multi_send_msg_packet(mask_, reinterpret_cast<const std::byte *>(commands_.data()), count_ * sizeof(TCmdKeeper));
		count_ = 0;
	}

private:
	uint32_t mask_;
	size_t count_ = 0;
	/** A packet's body holds 493 bytes (TPkt::body). */
	std::array<TCmdKeeper, 493 / sizeof(TCmdKeeper)> commands_;
};

/** Carried gold fits in two bytes, with -1 ("robbed nothing, but heading out anyway") as zero. */
Point PackCarried(int carried)
{
	const int value = std::clamp(carried + 1, 0, 0xFFFF);
	return { value & 0xFF, value >> 8 };
}

int UnpackCarried(uint8_t low, uint8_t high)
{
	return (low | (high << 8)) - 1;
}

uint8_t PackHeroFlags(bool fleeing, bool fearRolled, bool fellBack)
{
	return static_cast<uint8_t>((fleeing ? 1 : 0) | (fearRolled ? 2 : 0) | (fellBack ? 4 : 0));
}

/** What a raid in progress needs beyond where everyone stands: the heroes' loot and morale, the bots' minds, the portal. */
void AddRaidJoinState(DirectCommands &out)
{
	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		const size_t id = ActiveMonsters[i];
		const std::optional<HeroSyncState> hero = GetHeroSyncState(id);
		if (!hero)
			continue;
		const auto w = static_cast<uint16_t>(id + 1);
		out.Add(Action::SyncHero, PackHeroFlags(hero->fleeing, hero->fearRolled, hero->fellBack), hero->baitsVisited, PackCarried(hero->carried), w);
		out.Add(Action::SyncHeroMind, static_cast<uint8_t>(hero->trapsKnown), static_cast<uint8_t>(hero->trapsKnown >> 8), static_cast<uint8_t>(hero->trapsSeen), static_cast<uint8_t>(hero->trapsSeen >> 8), w);
		const int regroup = std::clamp(hero->regroupBudget, 0, 0xFFFF);
		out.Add(Action::SyncHeroPace, static_cast<uint8_t>(std::clamp(hero->fallBackSteps, 0, 0xFF)), 0, static_cast<uint8_t>(regroup), static_cast<uint8_t>(regroup >> 8), w);
	}
	for (size_t i = 0; i < MaxBots; i++) {
		const std::optional<BotSyncState> bot = GetBotSyncState(i);
		if (!bot)
			continue;
		const auto a = static_cast<uint8_t>(i);
		out.Add(Action::SyncBotState, a, PackHeroFlags(bot->fleeing, bot->fearRolled, bot->fellBack), PackCarried(bot->carried), static_cast<uint16_t>(std::clamp(bot->potions, 0, 0xFFFF)));
		out.Add(Action::SyncBotMind, a, bot->baitsVisited, static_cast<uint8_t>(bot->trapsKnown), static_cast<uint8_t>(bot->trapsKnown >> 8), bot->trapsSeen);
		out.Add(Action::SyncBotPace, a, static_cast<uint8_t>(std::clamp(bot->fallBackThinks, 0, 0xFF)), static_cast<uint8_t>(std::clamp(bot->thinkCooldown, 0, 0xFF)), static_cast<uint8_t>(std::clamp(bot->baitLingerTicks, 0, 0xFF)), static_cast<uint16_t>(std::clamp(bot->regroupBudget, 0, 0xFFFF)));
		if (bot->petrifiedTicks > 0)
			out.Add(Action::SyncBotPetrified, a, 0, 0, 0, static_cast<uint16_t>(std::clamp(bot->petrifiedTicks, 0, 0xFFFF)));
	}
	const PortalSyncState portal = GetPortalSyncState();
	if (State.bossDown || portal.open) {
		const auto flags = static_cast<uint8_t>((portal.open ? 1 : 0) | (portal.awaitingBotCast ? 2 : 0) | (State.bossDown ? 4 : 0) | (State.portalEscape ? 8 : 0));
		out.Add(Action::SyncPortal, flags, static_cast<uint8_t>(std::clamp(portal.castTimeout, 0, 0xFF)), portal.position, static_cast<uint16_t>(std::clamp(portal.channel, 0, 0xFFFF)));
		out.Add(Action::SyncPortalCaster, static_cast<uint8_t>(portal.botSlot + 1), 0, 0, 0, static_cast<uint16_t>(portal.casterMonster + 1));
	}
}

/**
 * Tells a master who just arrived what the keep looks like. Most of it isn't in the engine's level
 * delta at all, and that delta only remembers a monster it never saw die as "spawned somewhere".
 */
void SendKeepSnapshot(size_t playerId)
{
	DirectCommands out(playerId);
	out.Add(Action::SyncBegin, static_cast<uint8_t>(State.phase), State.wave, static_cast<uint8_t>(RaidMasters()), static_cast<uint8_t>(GameDifficulty()), FailedGoals());
	out.Add(Action::SyncHostSlot, LobbyHostSlot());
	// Before the hit points: raising a tier adds life, and those are the final numbers.
	for (size_t i = 0; i < NumUpgrades; i++)
		out.Add(Action::SyncUpgrade, static_cast<uint8_t>(i), static_cast<uint8_t>(UpgradeTier(static_cast<UpgradeId>(i))));

	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		const unsigned id = ActiveMonsters[i];
		const Monster &monster = Monsters[id];
		if (monster.isInvalid || monster.hitPoints <= 0 || monster.mode == MonsterMode::Death || monster.position.tile == GolemHoldingCell)
			continue;
		const auto w = static_cast<uint16_t>(id + 1);
		out.Add(Action::SyncMonsterPosition, 0, 0, monster.position.tile, w);
		const auto hp = static_cast<uint32_t>(monster.hitPoints);
		out.Add(Action::SyncMonsterHitPoints, static_cast<uint8_t>(hp), static_cast<uint8_t>(hp >> 8), static_cast<uint8_t>(hp >> 16), static_cast<uint8_t>(hp >> 24), w);
	}
	for (size_t slot = 0; slot < MaxLairs; slot++) {
		const Lair &lair = State.lairs[slot];
		if (!lair.active)
			continue;
		const auto a = static_cast<uint8_t>(slot);
		out.Add(Action::SyncLair, a, lair.kind, lair.position, static_cast<uint16_t>(std::clamp(lair.summonTicks, 0, 0xFFFF)));
		if (lair.hasRally)
			out.Add(Action::SyncRally, a, 0, lair.rally);
		for (size_t i = 0; i < lair.routeCount; i++)
			out.Add(Action::SyncWaypoint, a, static_cast<uint8_t>(i), lair.route[i]);
		for (size_t i = 0; i < MaxLairMembers; i++) {
			if (lair.members[i] >= 0)
				out.Add(Action::SyncMember, a, static_cast<uint8_t>(i), lair.patrolLeg[i], 0, static_cast<uint16_t>(lair.members[i] + 1));
		}
	}
	for (size_t slot = 0; slot < MaxTraps; slot++) {
		const Trap &trap = State.traps[slot];
		if (trap.active)
			out.Add(Action::SyncTrap, static_cast<uint8_t>(slot), trap.kind, trap.position, static_cast<uint16_t>(std::clamp(trap.cooldown, 0, 0xFFFF)));
	}
	for (size_t slot = 0; slot < MaxLocks; slot++) {
		const DoorLock &lock = State.locks[slot];
		if (lock.active)
			out.Add(Action::SyncLock, static_cast<uint8_t>(slot), static_cast<uint8_t>(std::clamp(lock.strength, 0, 0xFF)), lock.position);
	}
	for (size_t slot = 0; slot < MaxBaits; slot++) {
		const Bait &bait = State.baits[slot];
		if (bait.active)
			out.Add(Action::SyncBait, static_cast<uint8_t>(slot), static_cast<uint8_t>(std::clamp(bait.uses, 0, 0xFF)), bait.position);
	}
	for (size_t pid = 0; pid < State.possessed.size(); pid++) {
		if (State.possessed[pid] >= 0)
			out.Add(Action::SyncPossessed, static_cast<uint8_t>(pid), 0, 0, 0, static_cast<uint16_t>(State.possessed[pid] + 1));
	}
	for (size_t i = 0; i < MaxBots && FirstBotId + i < Players.size(); i++) {
		const Player &bot = Players[FirstBotId + i];
		if (const std::optional<BotSummary> summary = GetBotSummary(bot); summary)
			out.Add(Action::SyncBot, static_cast<uint8_t>(i), static_cast<uint8_t>(summary->cls), bot.position.tile, static_cast<uint16_t>(std::clamp(bot._pHitPoints >> 6, 0, 0xFFFF)));
	}
	AddRaidJoinState(out);
	out.Add(Action::SyncEnd);
	out.Flush();
}

/** True if the given monster id is one this client has in play. */
bool IsMonsterInPlay(int monsterId)
{
	return monsterId >= 0 && static_cast<size_t>(monsterId) < MaxMonsters
	    && std::find(ActiveMonsters, ActiveMonsters + ActiveMonsterCount, static_cast<unsigned>(monsterId)) != ActiveMonsters + ActiveMonsterCount
	    && !Monsters[monsterId].isInvalid;
}

void ApplySyncBegin(uint8_t phaseId, uint8_t wave, uint8_t masters, uint8_t difficulty, uint8_t failedGoals)
{
	const Phase phase = static_cast<Phase>(std::min<uint8_t>(phaseId, static_cast<uint8_t>(Phase::Defeat)));
	State.wave = wave;
	SetFailedGoals(failedGoals);
	// Before the heroes in this snapshot are rebuilt: they scale with these.
	SetRaidMasters(masters);
	SetGameDifficulty(static_cast<Difficulty>(difficulty));
	AnnouncePhase(State.phase, phase);
	State.phase = phase;
	for (Lair &lair : State.lairs) {
		lair = {};
		lair.members.fill(-1);
	}
	State.traps = {};
	for (size_t slot = 0; slot < MaxLocks; slot++)
		RemoveBarricade(slot); // the host's list follows, and each barricade comes back with its lock
	State.locks = {};
	State.baits = {};
	State.possessed.fill(-1);
	snapshotMonsters.reset();
}

/** Puts a monster where the host has it: from the level delta it's still in the holding cell. */
void ApplySyncMonsterPosition(int monsterId, Point tile)
{
	if (!IsMonsterInPlay(monsterId) || !InDungeonBounds(tile))
		return;
	snapshotMonsters.set(static_cast<size_t>(monsterId));
	Monster &monster = Monsters[monsterId];
	M_ClearSquares(monster);
	monster.position.tile = tile;
	monster.position.old = tile;
	monster.position.future = tile;
	monster.occupyTile(tile, false);
	RefreshMonsterAfterSync(monster);
	M_StartStand(monster, monster.direction);
}

void ApplySyncEnd()
{
	// Sold lairs' monsters are still in the delta, as alive: the host never saw them die.
	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		const unsigned id = ActiveMonsters[i];
		Monster &monster = Monsters[id];
		// A golem that crumbled at the end of a raid is still in the delta too. An unused golem slot stays as it is.
		if (snapshotMonsters.test(id) || monster.isInvalid || monster.position.tile == GolemHoldingCell)
			continue;
		M_ClearSquares(monster);
		monster.hitPoints = 0;
		monster.isInvalid = true;
	}
	for (const Lair &lair : State.lairs) {
		if (!lair.active)
			continue;
		for (size_t i = 0; i < MaxLairMembers; i++)
			ApplyMemberPost(lair, i);
	}
	RebuildPaths();
	awaitingKeepSnapshot = false;
}

void ApplySnapshotEntry(const TCmdKeeper &message)
{
	const Point tile { message.x, message.y };
	const int monsterId = static_cast<int>(Swap16LE(message.w)) - 1;
	switch (message.action) {
	case Action::SyncBegin:
		ApplySyncBegin(message.a, message.b, message.x, message.y, static_cast<uint8_t>(Swap16LE(message.w)));
		break;
	case Action::SyncHostSlot:
		LobbySetHostSlot(message.a);
		break;
	case Action::SyncUpgrade:
		if (message.a < NumUpgrades)
			SetUpgradeTierQuietly(static_cast<UpgradeId>(message.a), message.b);
		break;
	case Action::SyncMonsterPosition:
		ApplySyncMonsterPosition(monsterId, tile);
		break;
	case Action::SyncMonsterHitPoints:
		if (IsMonsterInPlay(monsterId))
			Monsters[monsterId].hitPoints = static_cast<int>(message.a | (message.b << 8) | (message.x << 16) | (static_cast<uint32_t>(message.y) << 24));
		break;
	case Action::SyncLair:
		if (message.a < MaxLairs && message.b < BuildKinds.size() && IsLairKind(message.b)) {
			Lair &lair = State.lairs[message.a];
			lair.active = true;
			lair.kind = message.b;
			lair.position = tile;
			lair.summonTicks = Swap16LE(message.w);
		}
		break;
	case Action::SyncRally:
		if (message.a < MaxLairs) {
			State.lairs[message.a].hasRally = true;
			State.lairs[message.a].rally = tile;
		}
		break;
	case Action::SyncWaypoint:
		ApplyWaypoint(message.a, message.b, tile);
		break;
	case Action::SyncMember:
		if (message.a < MaxLairs && message.b < MaxLairMembers && monsterId >= 0 && static_cast<size_t>(monsterId) < MaxMonsters) {
			ClaimMonster(State.lairs[message.a], message.b, static_cast<int16_t>(monsterId));
			State.lairs[message.a].patrolLeg[message.b] = std::min<uint8_t>(message.x, MaxPatrolPoints);
		}
		break;
	case Action::SyncTrap:
		if (message.a < MaxTraps) {
			Trap &trap = State.traps[message.a];
			trap.active = true;
			trap.position = tile;
			trap.cooldown = Swap16LE(message.w);
			trap.kind = std::min<uint8_t>(message.b, NumTrapKinds - 1);
		}
		break;
	case Action::SyncLock:
		if (message.a < MaxLocks) {
			ApplyDoorLocked(message.a, tile);
			if (State.locks[message.a].active)
				State.locks[message.a].strength = message.b;
		}
		break;
	case Action::SyncBait:
		if (message.a < MaxBaits) {
			ApplyBaitPlaced(message.a, tile);
			State.baits[message.a].uses = message.b;
		}
		break;
	case Action::SyncPossessed:
		if (message.a < State.possessed.size() && IsMonsterInPlay(monsterId))
			State.possessed[message.a] = static_cast<int16_t>(monsterId);
		break;
	case Action::SyncBot:
		if (message.a < MaxBots && message.b < NumHeroClasses && InDungeonBounds(tile)) {
			SpawnBot(message.a, static_cast<HeroClassKind>(message.b), tile);
			Players[FirstBotId + message.a]._pHitPoints = Swap16LE(message.w) << 6;
		}
		break;
	case Action::SyncHero:
	case Action::SyncHeroMind:
	case Action::SyncHeroPace:
		if (IsMonsterInPlay(monsterId)) {
			std::optional<HeroSyncState> hero = GetHeroSyncState(static_cast<size_t>(monsterId));
			if (!hero)
				break;
			if (message.action == Action::SyncHero) {
				hero->fleeing = (message.a & 1) != 0;
				hero->fearRolled = (message.a & 2) != 0;
				hero->fellBack = (message.a & 4) != 0;
				hero->baitsVisited = message.b;
				hero->carried = UnpackCarried(message.x, message.y);
			} else if (message.action == Action::SyncHeroMind) {
				hero->trapsKnown = static_cast<uint16_t>(message.a | (message.b << 8));
				hero->trapsSeen = static_cast<uint16_t>(message.x | (message.y << 8));
			} else {
				hero->fallBackSteps = message.a;
				hero->regroupBudget = message.x | (message.y << 8);
			}
			ApplyHeroSyncState(static_cast<size_t>(monsterId), *hero);
		}
		break;
	case Action::SyncBotState:
	case Action::SyncBotMind:
	case Action::SyncBotPace:
	case Action::SyncBotPetrified:
		if (message.a < MaxBots) {
			std::optional<BotSyncState> bot = GetBotSyncState(message.a);
			if (!bot)
				break;
			if (message.action == Action::SyncBotState) {
				bot->fleeing = (message.b & 1) != 0;
				bot->fearRolled = (message.b & 2) != 0;
				bot->fellBack = (message.b & 4) != 0;
				bot->carried = UnpackCarried(message.x, message.y);
				bot->potions = Swap16LE(message.w);
			} else if (message.action == Action::SyncBotMind) {
				bot->baitsVisited = message.b;
				bot->trapsKnown = static_cast<uint16_t>(message.x | (message.y << 8));
				bot->trapsSeen = Swap16LE(message.w);
			} else if (message.action == Action::SyncBotPace) {
				bot->fallBackThinks = message.b;
				bot->thinkCooldown = message.x;
				bot->baitLingerTicks = message.y;
				bot->regroupBudget = Swap16LE(message.w);
			} else {
				bot->petrifiedTicks = Swap16LE(message.w);
			}
			ApplyBotSyncState(message.a, *bot);
		}
		break;
	case Action::SyncPortal: {
		State.bossDown = (message.a & 4) != 0;
		State.portalEscape = (message.a & 8) != 0;
		PortalSyncState portal = GetPortalSyncState();
		portal.open = (message.a & 1) != 0;
		portal.awaitingBotCast = (message.a & 2) != 0;
		portal.castTimeout = message.b;
		portal.channel = Swap16LE(message.w);
		portal.position = tile;
		ApplyPortalSyncState(portal);
		break;
	}
	case Action::SyncPortalCaster: {
		PortalSyncState portal = GetPortalSyncState();
		portal.botSlot = static_cast<int>(message.a) - 1;
		portal.casterMonster = monsterId;
		ApplyPortalSyncState(portal);
		break;
	}
	case Action::SyncEnd:
		ApplySyncEnd();
		break;
	default:
		break;
	}
}

/** True if another master is already in the keep, so this client has joined a game in play. */
bool IsAnotherMasterInKeep()
{
	for (size_t i = 0; i < NetPlayerSlots; i++) {
		const Player &other = Players[i];
		if (&other != MyPlayer && other.plractive && other.plrlevel == KeepLevel && !other.plrIsOnSetLevel)
			return true;
	}
	return false;
}

/**
 * True on the client that answers for the keep when `joiner` arrives. Not IsHost(): if the joiner took
 * a lower player id than the host's, the engine has just made it the level's owner, and it knows nothing.
 */
bool ShouldSendSnapshotTo(const Player &joiner)
{
	return !awaitingKeepSnapshot && MyPlayer->levelOwner(&joiner) == MyPlayer;
}

void HostBuyUpgrade(uint8_t id, uint8_t tierSeenByBuyer)
{
	if (id >= NumUpgrades)
		return;
	const auto upgrade = static_cast<UpgradeId>(id);
	// A second click, or another master, got there first: don't charge for a tier they didn't see.
	if (UpgradeTier(upgrade) != tierSeenByBuyer || !CanBuyUpgrade(upgrade))
		return;
	State.vaultGold -= UpgradeNextCost(upgrade);
	// Applied here at once: the host's own copy of the message arrives later, and a request handled
	// before then must see the new tier and price.
	const int tier = UpgradeTier(upgrade) + 1;
	ApplyUpgradeTier(upgrade, tier);
	Send(Action::UpgradeBought, id, static_cast<uint8_t>(tier));
	BroadcastState();
}

void ApplyCast(Player &caster, size_t spell, Point tile)
{
	// The engine's spells only ever start where a player can stand: right by the map's edge, far outside any
	// level, they read past its tables. A spell's own positions also travel as bytes, and would wrap there.
	if (spell >= SpellKinds.size() || tile.x < SpellEdgeMargin || tile.y < SpellEdgeMargin || tile.x >= MAXDUNX - SpellEdgeMargin || tile.y >= MAXDUNY - SpellEdgeMargin)
		return;
	const size_t pid = caster.getId();
	const SpellKind &kind = SpellKinds[spell];
	// Every client checks: mana is only in step to within a second's regeneration, hence the tolerance.
	if (State.phase != Phase::Raid || State.spellLevels[spell] == 0 || State.mana[pid] + ManaCastTolerance < SpellManaCostScaled(spell))
		return;
	State.mana[pid] = std::max(State.mana[pid] - SpellManaCostScaled(spell), 0);
	State.spellCooldown[pid] = kind.cooldownTicks;

	if (kind.missile == MissileID::Golem) {
		// The engine's cast of a second golem kills the first. Here it is mended, on every client alike.
		if (Monster *golem = FindGolemForPlayer(caster); golem != nullptr) {
			MendGolem(*golem, tile);
			return;
		}
	}

	const int level = State.spellLevels[spell];
	Direction dir = Direction::South;
	WorldTilePosition src = tile;
	WorldTilePosition dst = tile;
	switch (kind.missile) {
	case MissileID::FireWallControl:
	case MissileID::LightningWallControl:
		// A wall needs a direction: run it across the screen (north-east to south-west).
		dir = Direction::SouthEast;
		src = tile + Displacement(Direction::NorthWest);
		break;
	case MissileID::FlameWaveControl:
	case MissileID::InfernoControl:
		// These travel, and the master's body is parked out of the way: send them towards the raiders' way in.
		if (tile != State.entrance)
			dir = GetDirection(tile, State.entrance);
		// Both begin a step on from their source: starting a step short of the target is what puts it in the fire.
		src = tile + Opposite(dir);
		dst = tile;
		caster._pdir = dir; // the flame wave rolls the way its caster faces
		break;
	case MissileID::ChainLightning:
		// Its bolts can't strike the tile they leave from: they leave from beside the target, off any other hero if it can.
		{
			bool found = false;
			for (const Direction side : { Direction::NorthWest, Direction::NorthEast, Direction::SouthEast, Direction::SouthWest, Direction::North, Direction::East, Direction::South, Direction::West }) {
				const Point beside = tile + side;
				if (TileHasAny(beside, TileProperties::BlockMissile))
					continue;
				if (!found)
					src = beside;
				found = true;
				if (!IsHeroAt(beside)) {
					src = beside;
					break;
				}
			}
		}
		break;
	default:
		break;
	}
	Missile *missile = AddMissile(src, dst, dir, kind.missile, TARGET_MONSTERS, caster, 0, SpellEngineLevel(spell));
	if (missile == nullptr)
		return;
	if (kind.missile == MissileID::Fireball)
		missile->duration = 1; // it would fly on until it hit something: burst where the master pointed
	if (kind.missile == MissileID::Apocalypse)
		missile->_midam = missile->_midam * (100 + ApocalypseDamagePerLevel * (level - 1)) / 100;
	if (kind.missile == MissileID::Guardian)
		missile->_midam = GuardianDamageBase * (100 + GuardianDamagePerRaise * (level - 1)) / 100;
	if (kind.missile == MissileID::Nova)
		missile->_midam = std::max(missile->_midam * NovaDamagePercent / 100, 1);
	if ((kind.missile == MissileID::Nova || kind.missile == MissileID::Fireball) && (level - 1) % RaisesPerEngineSpellLevel != 0)
		missile->_midam = missile->_midam * (100 + HalfEngineLevelDamagePercent) / 100;
}

/** Sets off a trap of its kind at a hero on `victim`, on every client. */
void FireTrap(Trap &trap, Point victim)
{
	const TrapKind &kind = TrapKinds[trap.kind];
	const int level = State.trapLevels[trap.kind];
	const Direction dir = GetDirection(trap.position, victim);
	const int damage = ScaleByHeroStrength(kind.baseDamage + kind.damagePerWave * State.wave) * (100 + TrapDamagePerLevel * (level - 1)) / 100;
	switch (trap.kind) {
	case 0:
		AddMissile(trap.position + Displacement(Direction::NorthWest), trap.position, Direction::SouthEast, MissileID::FireWallControl, TARGET_PLAYERS, -1, 0, 2);
		AddMissile(trap.position, victim, dir, MissileID::Firebolt, TARGET_PLAYERS, -1, damage, 0);
		break;
	case 1:
		AddMissile(trap.position, victim, dir, MissileID::Arrow, TARGET_PLAYERS, -1, damage, 0);
		break;
	case 2:
		AddMissile(trap.position, victim, dir, MissileID::LightningControl, TARGET_PLAYERS, -1, damage, 0);
		break;
	case SnareTrapKind:
		// Stone Curse is a player's spell: every client lends it the same master.
		for (size_t pid = 0; pid < MAX_PLRS; pid++) {
			if (!Players[pid].plractive)
				continue;
			AddMissile(victim, victim, Direction::South, MissileID::StoneCurse, TARGET_MONSTERS, Players[pid], 0, SnareSpellLevelBase + level - 1);
			break;
		}
		break;
	}
	const int rearmPercent = std::max(100 - TrapRearmPerLevel * (level - 1), TrapRearmFloorPercent);
	trap.cooldown = kind.rearmTicks * rearmPercent / 100;
}

/** Traps fire on every client: their missiles are local effects, like any other trap in the game. */
void UpdateTraps()
{
	for (Trap &trap : State.traps) {
		if (!trap.active)
			continue;
		if (trap.cooldown > 0) {
			trap.cooldown--;
			continue;
		}
		// A snare has nothing to do to a hero who is already held.
		const bool snare = trap.kind == SnareTrapKind;
		std::optional<Point> victim;
		for (size_t i = 0; i < ActiveMonsterCount && !victim; i++) {
			const Monster &monster = Monsters[ActiveMonsters[i]];
			if (IsHero(monster) && monster.hitPoints > 0 && monster.position.tile.WalkingDistance(trap.position) <= 1 && !(snare && monster.mode == MonsterMode::Petrified))
				victim = monster.position.tile;
		}
		for (size_t i = 0; i < MaxBots && !victim; i++) {
			const Player &bot = Players[FirstBotId + i];
			if (IsLivingBot(bot) && bot.position.tile.WalkingDistance(trap.position) <= 1 && !(snare && IsPetrifiedBot(bot)))
				victim = bot.position.tile;
		}
		if (victim) {
			FireTrap(trap, *victim);
			NoteTrapFired(static_cast<size_t>(&trap - State.traps.data()));
		}
	}
}

} // namespace

std::string_view PhaseName(Phase phase)
{
	switch (phase) {
	case Phase::Setup:
		return "Preparation";
	case Phase::Raid:
		return "Raid!";
	case Phase::Breather:
		return "Respite";
	case Phase::Victory:
		return "Victory";
	case Phase::Defeat:
		return "Defeat";
	}
	return "";
}

bool IsHost()
{
	return !awaitingKeepSnapshot && MyPlayer != nullptr && MyPlayer->isLevelOwnedByLocalClient();
}

bool TakesMonsterSyncFrom(const Player &sender)
{
	if (!IsActive())
		return true;
	return !IsHost() && MyPlayer->levelOwner() == &sender;
}

bool IgnoresMonsterSyncMove(const Monster &monster)
{
	return IsPossessed(monster) || IsAnyOf(monster.mode, MonsterMode::MeleeAttack, MonsterMode::SpecialMeleeAttack, MonsterMode::RangedAttack, MonsterMode::SpecialRangedAttack);
}

bool IsAwaitingKeepSnapshot()
{
	return awaitingKeepSnapshot;
}

void OnPlayerJoinedLevel(const Player &player)
{
	if (!IsActive() || &player == MyPlayer || !player.plractive || !player.isOnActiveLevel() || player.getId() >= NetPlayerSlots)
		return;
	if (!ShouldSendSnapshotTo(player))
		return;
	SendKeepSnapshot(player.getId());
	BroadcastState();
}

void ResetGame(Point entrance, Point vault)
{
	State = {};
	ForgetBarricades();
	State.entrance = entrance;
	State.vault = vault;
	State.vaultGold = StartingGold();
	State.wave = static_cast<uint8_t>(std::clamp(TestStartWave, 1, NumWaves()));
	State.phase = Phase::Setup;
	State.phaseTicks = SetupTicks;
	LatchGameDifficulty();
	SetRaidMasters(CountMasters());
	SetFailedGoals(0);
	for (size_t i = 0; i < NumSpellKinds; i++)
		State.spellLevels[i] = static_cast<uint8_t>(GameSpellLevel(i));
	for (size_t i = 0; i < NumTrapKinds; i++)
		State.trapLevels[i] = static_cast<uint8_t>(GameTrapLevel(i));
	ProfileBeginMap();
	for (int &mana : State.mana)
		mana = MaxMana * ManaScale / 2;
	for (Lair &lair : State.lairs)
		lair.members.fill(-1);
	ticksUntilBroadcast = 0;
	ResetUpgrades();
	patrolWatch = {};
	// A joiner's fellow masters aren't marked active until after its first load, so go by how it got here.
	awaitingKeepSnapshot = IsJoinedGame() || IsAnotherMasterInKeep();
	keepSnapshotTimeout = JoinSyncTimeoutTicks;
	restartChosen = false;
	restartCountdown = 0;
	joinerRestartWait = 0;
	restartGraceTicks = 0;
	restartGraceElapsed = 0;
	pendingSpeed = {};
	speedMismatches = 0;
	ClearPings();
	retryGoldLost = 0;
	ownsResumeSlot = !HasResumeFile();
	OnKeepLoadedSpeed();
	ResetHeroes();
	ResetRaidStats();
}

void GameTick()
{
	// The word goes out repeatedly: a message can wait for the next turn, and a joiner may be loading.
	if (joinerRestartWait > 0 && --joinerRestartWait == 0)
		MarkRestartRequested();
	if (restartGraceTicks > 0) {
		// Long enough for everyone to see it coming, longer while someone is in the Keeper, within reason.
		const int pastMinimum = std::max(CoopMinGraceTicks - ++restartGraceElapsed, 0);
		restartGraceTicks = CoopAnyMasterInKeeper() ? restartGraceTicks - 1 : std::min(restartGraceTicks - 1, pastMinimum);
		if (restartGraceElapsed % TicksPerSecond == 1 || restartGraceTicks == 0)
			Send(Action::NextMapSoon, 0, 0, {}, static_cast<uint16_t>(restartGraceTicks));
		CoopNoteStarting(restartGraceTicks);
		if (restartGraceTicks == 0)
			restartCountdown = RestartNoticeTicks;
	}
	if (restartCountdown > 0) {
		if (restartCountdown % RestartResendTicks == 0)
			Send(Action::Restart);
		if (--restartCountdown == 0) {
			SetCurrentSetup(restartSetup);
			MarkRestartRequested();
		}
	}

	if (awaitingKeepSnapshot && --keepSnapshotTimeout <= 0)
		awaitingKeepSnapshot = false;

	if (std::optional<ResumeData> resume = TakePendingResume(); resume && CanSaveKeep())
		ApplyResume(*resume);

	UpdatePendingSpeed();
	if (!IsHost() && State.readyTicks > 0)
		State.readyTicks--;

	// Mana regenerates for every master, on every client (snapshots correct any drift).
	const int manaRegen = UpgradedManaRegen(ManaRegenPerTick);
	for (size_t i = 0; i < State.mana.size(); i++) {
		State.mana[i] = std::min(State.mana[i] + manaRegen, MaxMana * ManaScale);
		if (State.spellCooldown[i] > 0)
			State.spellCooldown[i]--;
	}

	const bool prep = State.phase == Phase::Setup || State.phase == Phase::Breather;
	if (State.phaseTicks > 0 && !(prep && IsPrepTimerHeld()))
		State.phaseTicks--;

	// Possession drains mana (2/s net); an empty pool breaks the link. Every client runs this.
	for (size_t i = 0; i < State.possessed.size(); i++) {
		if (State.possessed[i] < 0)
			continue;
		State.mana[i] -= manaRegen + PossessDrainPerTick;
		if (State.mana[i] <= 0) {
			State.mana[i] = 0;
			EndPossession(i);
		}
	}

	if (IsSiegeOver())
		return;

	UpdateTraps();
	if (State.phase == Phase::Raid)
		UpdateBots();
	UpdatePortal();

	for (Lair &lair : State.lairs) {
		if (!lair.active || lair.summonTicks == 0)
			continue;
		if (--lair.summonTicks == 0 && IsHost())
			RefillLair(lair);
	}

	if (!IsHost())
		return;

	UpdateBoss();
	UpdatePatrols();

	switch (State.phase) {
	case Phase::Setup:
	case Phase::Breather:
		// Only while preparing: the party a raid is scaled for holds until it ends, whoever comes or goes.
		SetRaidMasters(CountMasters());
		if (State.phaseTicks == 0)
			StartRaid();
		else
			UpdateReadyVote();
		break;
	case Phase::Raid:
		WatchLordForGoal();
		if (State.bossDown && State.portalEscape) {
			SetPhase(Phase::Defeat);
			break;
		}
		if (UpdateRaid() == RaidStatus::Repelled)
			EndRaid();
		break;
	default:
		break;
	}

	if (--ticksUntilBroadcast <= 0)
		BroadcastState();
}

void DevForceVictory()
{
	if (IsHost()) {
		SetPhase(Phase::Victory);
		BroadcastState();
	}
}

void OnLeavingKeep()
{
	if (!CanSaveKeep())
		return;
	if (State.phase == Phase::Setup || State.phase == Phase::Breather) {
		// Includes whatever this respite has built so far.
		SaveKeep();
	} else if (State.phase == Phase::Raid && ownsResumeSlot && HasResumeFile()) {
		// Continue brings the raid back: this attempt pays nothing, as for a retry.
		ProfileTakeBackRaid();
	}
}

void DevFireTrap(size_t slot, Point victim)
{
	if (slot < State.traps.size() && State.traps[slot].active)
		FireTrap(State.traps[slot], victim);
}

bool CanBuildAt(size_t kind, Point tile, std::string_view *reason)
{
	auto fail = [reason](std::string_view why) {
		if (reason != nullptr)
			*reason = why;
		return false;
	};
	if (kind >= BuildKinds.size())
		return fail("Unknown structure");
	if (IsLairKind(kind) && !IsKindMustered(kind))
		return fail("Not unlocked yet");
	if (!HasRoomToLoadKind(kind))
		return fail("Too many kinds of monster in this keep");
	// Late Pop caps pass MaxLairs on purpose: bigger monsters take fewer lairs for their Pop.
	if (IsLairKind(kind) && std::none_of(State.lairs.begin(), State.lairs.end(), [](const Lair &lair) { return !lair.active; }))
		return fail("No room for more lairs");
	if (!IsLairKind(kind) && std::none_of(State.traps.begin(), State.traps.end(), [](const Trap &trap) { return !trap.active; }))
		return fail("No room for more traps");
	if (IsSiegeOver())
		return fail("The siege is over");
	if (!InDungeonBounds(tile) || !IsTileWalkable(tile) || IsObjectAtPosition(tile))
		return fail("Needs open floor");
	if (tile.WalkingDistance(State.entrance) < EntranceKeepOut)
		return fail("Too close to the entrance");
	if (tile.WalkingDistance(State.vault) < VaultKeepOut)
		return fail("Too close to the vault");
	if (IsPlacementCrowded(tile))
		return fail("Too close to another structure");
	if (State.phase == Phase::Raid && IsHeroNear(tile, RaidBuildHeroDistance))
		return fail("Heroes are watching");
	if (State.vaultGold < BuildKinds[kind].cost)
		return fail("Not enough gold");
	if (!IsLairKind(kind) && !IsTrapAvailable(kind - NumLairKinds))
		return fail("Not unlocked yet");
	if (IsLairKind(kind) && SupplyUsed() + BuildKinds[kind].supply > SupplyCap())
		return fail("The keep is full (Pop)");
	return true;
}

/** Falls evenly to half at the top level, so that every raise of every spell makes it cheaper. */
int SpellManaCostScaledAt(size_t spell, int level)
{
	const int base = SpellKinds[spell].manaCost * ManaScale;
	return base - base / 2 * (std::clamp(level, 1, MaxKeepLevel) - 1) / (MaxKeepLevel - 1);
}

int SpellManaCostAt(size_t spell, int level)
{
	return (SpellManaCostScaledAt(spell, level) + ManaScale - 1) / ManaScale;
}

int SpellManaCostScaled(size_t spell)
{
	return SpellManaCostScaledAt(spell, State.spellLevels[spell]);
}

int SpellEngineLevel(size_t spell)
{
	return SpellKinds[spell].spellLevel + (std::max<int>(State.spellLevels[spell], 1) - 1) / RaisesPerEngineSpellLevel;
}

int SpellManaCost(size_t spell)
{
	return SpellManaCostAt(spell, State.spellLevels[spell]);
}

int SupplyUsed()
{
	int used = 0;
	for (const Lair &lair : State.lairs) {
		if (lair.active)
			used += BuildKinds[lair.kind].supply;
	}
	return used;
}

int SupplyCap()
{
	return BaseSupply + SupplyPerLevel * (CurrentLevelNumber() - 1) + SupplyPerRaid * (std::clamp<int>(State.wave, 1, NumWaves()) - 1);
}

bool IsStructureActive(StructureRef ref)
{
	switch (ref.type) {
	case StructureType::Lair:
		return ref.slot < MaxLairs && State.lairs[ref.slot].active;
	case StructureType::Trap:
		return ref.slot < MaxTraps && State.traps[ref.slot].active;
	case StructureType::Bait:
		return ref.slot < MaxBaits && State.baits[ref.slot].active;
	case StructureType::Lock:
		return ref.slot < MaxLocks && State.locks[ref.slot].active;
	}
	return false;
}

Point StructurePosition(StructureRef ref)
{
	if (!IsStructureActive(ref))
		return {};
	switch (ref.type) {
	case StructureType::Lair:
		return State.lairs[ref.slot].position;
	case StructureType::Trap:
		return State.traps[ref.slot].position;
	case StructureType::Bait:
		return State.baits[ref.slot].position;
	case StructureType::Lock:
		return State.locks[ref.slot].position;
	}
	return {};
}

std::string_view StructureName(StructureRef ref)
{
	switch (ref.type) {
	case StructureType::Lair:
		return ref.slot < MaxLairs ? BuildKinds[State.lairs[ref.slot].kind].name : "Lair";
	case StructureType::Trap:
		return "Fire trap";
	case StructureType::Bait:
		return "Fool's gold";
	case StructureType::Lock:
		return UsesBarricades() ? "Barricade" : "Door lock";
	}
	return "";
}

int RefundFor(StructureRef ref)
{
	if (!IsStructureActive(ref))
		return 0;
	int value = 0;
	switch (ref.type) {
	case StructureType::Lair:
		value = BuildKinds[State.lairs[ref.slot].kind].cost;
		break;
	case StructureType::Trap:
		value = TrapKinds[State.traps[ref.slot].kind].cost;
		break;
	case StructureType::Bait:
		value = BaitCost * State.baits[ref.slot].uses / BaitUses;
		break;
	case StructureType::Lock:
		value = LockDoorCost * State.locks[ref.slot].strength / LockStrength;
		break;
	}
	return State.phase == Phase::Raid ? value / 2 : value;
}

std::optional<StructureRef> FindStructureAt(Point tile)
{
	for (size_t i = 0; i < MaxLairs; i++) {
		if (State.lairs[i].active && State.lairs[i].position == tile)
			return StructureRef { StructureType::Lair, i };
	}
	for (size_t i = 0; i < MaxTraps; i++) {
		if (State.traps[i].active && State.traps[i].position == tile)
			return StructureRef { StructureType::Trap, i };
	}
	for (size_t i = 0; i < MaxBaits; i++) {
		if (State.baits[i].active && State.baits[i].position == tile)
			return StructureRef { StructureType::Bait, i };
	}
	if (const std::optional<size_t> barricade = BarricadeSlotAt(tile); barricade)
		return StructureRef { StructureType::Lock, *barricade };
	if (const Object *door = FindDoorAt(tile); door != nullptr) {
		for (size_t i = 0; i < MaxLocks; i++) {
			if (State.locks[i].active && State.locks[i].position == door->position)
				return StructureRef { StructureType::Lock, i };
		}
	}
	return std::nullopt;
}

std::optional<size_t> LairOfMonster(size_t monsterId)
{
	for (size_t i = 0; i < MaxLairs; i++) {
		const Lair &lair = State.lairs[i];
		if (!lair.active)
			continue;
		for (const int16_t member : lair.members) {
			// Ids are reused once a monster is gone, so only a living member of the right type counts.
			if (member >= 0 && static_cast<size_t>(member) == monsterId && IsMemberAlive(member, BuildKinds[lair.kind].type))
				return i;
		}
	}
	return std::nullopt;
}

bool CanSell(StructureRef ref, std::string_view *reason)
{
	auto fail = [reason](std::string_view why) {
		if (reason != nullptr)
			*reason = why;
		return false;
	};
	if (IsSiegeOver())
		return fail("The siege is over");
	if (!IsStructureActive(ref))
		return fail("Already gone");
	return true;
}

bool CanAddWaypoint(size_t lair, Point tile, std::string_view *reason)
{
	if (!CanRallyTo(lair, tile, reason))
		return false;
	if (State.lairs[lair].routeCount >= MaxPatrolPoints) {
		if (reason != nullptr)
			*reason = "The patrol route is full";
		return false;
	}
	return true;
}

bool CanRallyTo(size_t lair, Point tile, std::string_view *reason)
{
	auto fail = [reason](std::string_view why) {
		if (reason != nullptr)
			*reason = why;
		return false;
	};
	if (IsSiegeOver())
		return fail("The siege is over");
	if (lair >= MaxLairs || !State.lairs[lair].active)
		return fail("Select a monster first");
	if (!InDungeonBounds(tile) || !IsTileWalkable(tile, /*ignoreDoors=*/true))
		return fail("Needs open floor");
	return true;
}

void RequestBuyUpgrade(UpgradeId id)
{
	Send(Action::RequestUpgrade, static_cast<uint8_t>(id), static_cast<uint8_t>(UpgradeTier(id)));
}

void RequestSell(StructureRef ref)
{
	Send(Action::RequestSell, static_cast<uint8_t>(ref.type), static_cast<uint8_t>(ref.slot));
}

void RequestRally(size_t lair, std::optional<Point> tile)
{
	Send(Action::RequestRally, static_cast<uint8_t>(lair), tile ? 1 : 0, tile.value_or(Point {}));
}

void RequestWaypoint(size_t lair, Point tile)
{
	Send(Action::RequestWaypoint, static_cast<uint8_t>(lair), 0, tile);
}

Object *FindDoorAt(Point tile)
{
	for (const Point candidate : { tile, tile + Direction::North, tile + Direction::West, tile + Direction::NorthWest }) {
		if (!InDungeonBounds(candidate))
			continue;
		Object *object = FindObjectAtPosition(candidate);
		if (object != nullptr && object->isDoor())
			return object;
	}
	return nullptr;
}

DoorLock *FindLock(Point doorPosition)
{
	for (DoorLock &lock : State.locks) {
		if (lock.active && lock.position == doorPosition)
			return &lock;
	}
	return nullptr;
}

bool CanLockDoorAt(Point tile, std::string_view *reason)
{
	auto fail = [reason](std::string_view why) {
		if (reason != nullptr)
			*reason = why;
		return false;
	};
	if (IsSiegeOver())
		return fail("The siege is over");
	if (UsesBarricades()) {
		const std::vector<Point> line = BarricadeLine(tile);
		if (line.empty())
			return fail("Too wide to bar: find a neck");
		if (BarricadeSlotAt(tile))
			return fail("Already barred");
		if (!CanPlaceBarricadeLine(line))
			return fail("Something is in the way");
		for (const Point at : line) {
			if (BarricadeSlotAt(at))
				return fail("Already barred");
			if (IsPlacementCrowded(at))
				return fail("Too close to another structure");
			for (const Lair &lair : State.lairs) {
				if (lair.active && ((lair.hasRally && lair.rally == at) || std::find(lair.route.begin(), lair.route.begin() + lair.routeCount, at) != lair.route.begin() + lair.routeCount))
					return fail("One of your rally points is there");
			}
			if (dMonster[at.x][at.y] != 0 || dPlayer[at.x][at.y] != 0)
				return fail("Something is in the way");
			if (at.WalkingDistance(State.entrance) < EntranceKeepOut || at.WalkingDistance(State.vault) < VaultKeepOut)
				return fail("Too close to the stairs");
			if (IsHeroNear(at, 3))
				return fail("A hero is in the way");
		}
		if (State.vaultGold < LockDoorCost)
			return fail("Not enough gold");
		return true;
	}
	const Object *door = FindDoorAt(tile);
	if (door == nullptr)
		return fail("Point at a door");
	if (FindLock(door->position) != nullptr)
		return fail("Already locked");
	if (IsHeroNear(door->position, 3))
		return fail("A hero is in the doorway");
	if (State.vaultGold < LockDoorCost)
		return fail("Not enough gold");
	return true;
}

bool CanPlaceBaitAt(Point tile, std::string_view *reason)
{
	auto fail = [reason](std::string_view why) {
		if (reason != nullptr)
			*reason = why;
		return false;
	};
	if (IsSiegeOver())
		return fail("The siege is over");
	if (!InDungeonBounds(tile) || !IsTileWalkable(tile) || IsObjectAtPosition(tile))
		return fail("Needs open floor");
	if (tile.WalkingDistance(State.entrance) < EntranceKeepOut)
		return fail("Too close to the entrance");
	if (IsPlacementCrowded(tile))
		return fail("Too close to another structure");
	if (State.phase == Phase::Raid && IsHeroNear(tile, RaidBuildHeroDistance))
		return fail("Heroes are watching");
	if (State.vaultGold < BaitCost)
		return fail("Not enough gold");
	return true;
}

void RequestBuild(size_t kind, Point tile)
{
	Send(Action::RequestBuild, static_cast<uint8_t>(kind), 0, tile);
}

void RequestLockDoor(Point doorPosition)
{
	Send(Action::RequestLockDoor, 0, 0, doorPosition);
}

void RequestPlaceBait(Point tile)
{
	Send(Action::RequestBait, 0, 0, tile);
}

void RequestCastSpell(size_t spell, Point tile)
{
	Send(Action::Cast, static_cast<uint8_t>(spell), 0, tile);
}

void RequestRestart(const KeepSetup &next)
{
	if (restartChosen) {
		// Asked again while everyone was given time: start now.
		if (restartGraceTicks > 1)
			restartGraceTicks = 1;
		return;
	}
	restartChosen = true;
	if (!gbIsMultiplayer) {
		SetPostMapSetup(next); // by way of the character screen
		gamemenu_exit_game(false);
		return;
	}
	restartSetup = next;
	const bool alone = std::none_of(Players.begin(), Players.begin() + NetPlayerSlots, [](const Player &other) { return &other != MyPlayer && other.plractive; });
	if (alone) {
		restartCountdown = RestartNoticeTicks;
		return;
	}
	restartGraceTicks = CoopMaxGraceTicks;
	restartGraceElapsed = 0;
	CoopNoteStarting(restartGraceTicks);
	Send(Action::NextMapSoon, 0, 0, {}, static_cast<uint16_t>(restartGraceTicks));
}

void RequestKeeperStatus(bool inKeeper)
{
	Send(Action::InKeeper, inKeeper ? 1 : 0);
}

bool IsRestartComing()
{
	return joinerRestartWait > 0;
}

void RequestStartRaid()
{
	Send(Action::RequestStartRaid);
}

bool IsMasterVoting(size_t playerId)
{
	if (playerId >= NetPlayerSlots)
		return false;
	const Player &player = Players[playerId];
	return player.plractive && !player._pLvlChanging && player._pmode != PM_NEWLVL && player.plrlevel == KeepLevel && !player.plrIsOnSetLevel;
}

int MastersInKeepCount()
{
	int count = 0;
	for (size_t id = 0; id < NetPlayerSlots; id++) {
		if (IsMasterVoting(id))
			count++;
	}
	return count;
}

int ReadyMasterCount()
{
	int count = 0;
	for (size_t id = 0; id < NetPlayerSlots; id++) {
		if (IsMasterVoting(id) && IsMasterReady(id))
			count++;
	}
	return count;
}

bool IsMasterReady(size_t playerId)
{
	return playerId < NetPlayerSlots && (State.readyMask & (1U << playerId)) != 0;
}

void SendPing(Point tile)
{
	Send(Action::Ping, 0, 0, tile);
}

void RequestGameSpeed(size_t level)
{
	Send(Action::RequestSpeed, static_cast<uint8_t>(level));
}

void RequestRetryRaid()
{
	if (CanRetryRaid())
		Send(Action::RequestRetryRaid);
}

bool CanRetryRaid()
{
	return State.phase == Phase::Defeat && !restartChosen && !IsEditorTestRun() && (!IsJoinedGame() || IsHost());
}

void RequestPossess(size_t monsterId)
{
	Send(Action::Possess, 0, 0, {}, static_cast<uint16_t>(monsterId));
}

void RequestRelease()
{
	Send(Action::Release);
}

void RequestSpawnBot(size_t slot, HeroClassKind cls, Point tile)
{
	Send(Action::SpawnBot, static_cast<uint8_t>(slot), static_cast<uint8_t>(cls), tile);
}

void RequestOrder(Point tile, int targetMonster)
{
	Send(Action::Order, 0, 0, tile, static_cast<uint16_t>(targetMonster + 1));
}

int EarlyCallBonusNow()
{
	if (State.phase != Phase::Setup && State.phase != Phase::Breather)
		return 0;
	return std::min(State.phaseTicks / TicksPerSecond * EarlyCallGoldPerSecond, EarlyCallMaxGold);
}

int VaultInterestOn(int gold)
{
	return std::min(std::max(gold, 0) * VaultInterestPercent / 100, ScaleByHeroStrength(VaultInterestMaxGold));
}

void AddVaultGold(int amount)
{
	State.vaultGold += amount;
}

namespace {

/** Escape facts travel in `a` (bit 0: portal, bit 1: fleeing), with the gold split over x and y. */
void SendEscape(Action action, uint8_t b, uint16_t w, bool throughPortal, bool fleeing, int carried)
{
	const int gold = std::clamp(carried, 0, 0xFFFF);
	Send(action, static_cast<uint8_t>((throughPortal ? 1 : 0) | (fleeing ? 2 : 0)), b, { gold & 0xFF, gold >> 8 }, w);
}

} // namespace

void NotifyBotEscaped(size_t slot, bool throughPortal, bool fleeing, int carried)
{
	SendEscape(Action::BotEscaped, static_cast<uint8_t>(slot), 0, throughPortal, fleeing, carried);
}

namespace {

/** Gold travels in x and y, as it does for an escape. */
Point GoldAsTile(int gold)
{
	const int clamped = std::clamp(gold, 0, 0xFFFF);
	return { clamped & 0xFF, clamped >> 8 };
}

void AnnounceTrapDisarmed(const Trap &trap, std::optional<size_t> botSlot)
{
	const std::string_view name = TrapKinds[trap.kind].name;
	if (botSlot)
		Announce(fmt::format("{} disarmed one of your {}s!", Players[FirstBotId + *botSlot]._pName, name));
	else
		Announce(fmt::format("A Rogue disarmed one of your {}s!", name));
}

} // namespace

void DisarmTrap(size_t trapSlot, std::optional<size_t> botSlot)
{
	Trap &trap = State.traps[trapSlot];
	trap.active = false;
	AnnounceTrapDisarmed(trap, botSlot);
	Send(Action::TrapDisarmed, static_cast<uint8_t>(trapSlot), botSlot ? static_cast<uint8_t>(*botSlot + 1) : 0);
}

void NotifyBotLooted(size_t slot, int amount)
{
	Send(Action::BotLooted, 0, static_cast<uint8_t>(slot), GoldAsTile(amount));
}

void NotifyBotFled(size_t slot)
{
	Send(Action::BotFled, 0, static_cast<uint8_t>(slot));
}

void NotifyHeroLooted(size_t monsterId, int amount)
{
	Send(Action::HeroLooted, 0, 0, GoldAsTile(amount), static_cast<uint16_t>(monsterId));
}

void NotifyHeroFled(size_t monsterId)
{
	Send(Action::HeroFled, 0, 0, {}, static_cast<uint16_t>(monsterId));
}

void NotifyHeroEscaped(size_t monsterId, bool throughPortal, bool fleeing, int carried)
{
	SendEscape(Action::HeroEscaped, 0, static_cast<uint16_t>(monsterId), throughPortal, fleeing, carried);
}

size_t HandleNetCommand(Player &player, const TCmd *cmd, size_t maxCmdSize)
{
	if (cmd->bCmd == CMD_KEEPER_STATE) {
		if (maxCmdSize < sizeof(TCmdKeeperState))
			return maxCmdSize;
		const auto &message = *reinterpret_cast<const TCmdKeeperState *>(cmd);
		// Not while this client is loading or waiting for the level: ResetGame is still to come, and it hasn't got a keep to apply this to.
		if (gbBufferMsgs != 1 && IsActive() && &player != MyPlayer && !IsHost()) {
			const Phase phase = static_cast<Phase>(std::min<uint8_t>(message.phase, static_cast<uint8_t>(Phase::Defeat)));
			// Before the phase and the wave: leaving a raid removes the bots and moves the wave on, and a
			// death has to be counted first, at the raid's own bounty.
			if (phase == Phase::Raid) {
				for (size_t i = 0; i < MaxBots; i++) {
					const int32_t life = static_cast<int32_t>(Swap32LE(static_cast<uint32_t>(message.botHitPoints[i])));
					ApplyBotLifeSync(i, life);
					ApplyBotPositionSync(i, life, { message.botTiles[i][0], message.botTiles[i][1] }, message.botPotions[i]);
				}
			}
			State.wave = message.wave;
			// Before the phase: the end of a raid announces these.
			State.lastTribute = static_cast<int32_t>(Swap32LE(static_cast<uint32_t>(message.lastTribute)));
			State.lastInterest = static_cast<int32_t>(Swap32LE(static_cast<uint32_t>(message.lastInterest)));
			SetFailedGoals(message.failedGoals);
			SetRaidMasters((message.reserved >> 2) & 7);
			SetGameDifficulty(static_cast<Difficulty>((message.reserved >> 5) & 3));
			const bool wasBossDown = State.bossDown;
			State.bossDown = (message.reserved & 1) != 0;
			State.portalEscape = (message.reserved & 2) != 0;
			AnnouncePhase(State.phase, phase);
			if (wasBossDown && !State.bossDown && phase != Phase::Defeat && State.phase != Phase::Defeat)
				Announce("The heroes were stopped. The Keep Lord rises again!", UiFlags::ColorRed);
			State.phase = phase;
			State.phaseTicks = static_cast<int>(Swap32LE(message.phaseTicks));
			State.vaultGold = static_cast<int32_t>(Swap32LE(static_cast<uint32_t>(message.vaultGold)));
			State.goldLost = static_cast<int32_t>(Swap32LE(static_cast<uint32_t>(message.goldLost)));
			State.retryGold = static_cast<int32_t>(Swap32LE(static_cast<uint32_t>(message.retryGold)));
			retryGoldLost = static_cast<int32_t>(Swap32LE(static_cast<uint32_t>(message.retryGoldLost)));
			// A joiner who wasn't there for the change, or missed it. Waits for a second word: this one may have left the host before the switch.
			AnnounceReadyChanges(State.readyMask, message.readyMask);
			State.readyMask = message.readyMask;
			State.readyTicks = Swap16LE(message.readyTicks);
			speedMismatches = message.speedLevel != GameSpeedLevel() && !pendingSpeed.active ? speedMismatches + 1 : 0;
			if (speedMismatches >= 2) {
				speedMismatches = 0;
				ApplyGameSpeedLevel(message.speedLevel);
			}
			for (size_t i = 0; i < 4; i++)
				State.mana[i] = Swap16LE(message.mana[i]) * ManaScale;
			// Before the Lord's health below, which is capped by a maximum these can raise.
			SyncUpgradeTiers(message.upgradeTiers);
			// Fights with bots are simulated on every client; keep the Lord's health in step with the host.
			const int bossHitPoints = static_cast<int32_t>(Swap32LE(static_cast<uint32_t>(message.bossHitPoints)));
			if (const Monster *boss = FindBoss(); boss != nullptr && bossHitPoints > 0)
				const_cast<Monster *>(boss)->hitPoints = std::min(bossHitPoints, boss->maxHitPoints);
		}
		return sizeof(message);
	}

	if (maxCmdSize < sizeof(TCmdKeeper))
		return maxCmdSize;
	const auto &message = *reinterpret_cast<const TCmdKeeper *>(cmd);
	if (gbBufferMsgs == 1 || !IsActive())
		return sizeof(message);

	const Point tile { message.x, message.y };
	switch (message.action) {
	case Action::RequestBuild:
		if (IsHost())
			HostBuild(player, message.a, tile);
		break;
	// The host applied these as it sent them, and may have sold the structure since.
	case Action::Built:
		if (&player != MyPlayer)
			ApplyBuilt(message.a, message.b, tile);
		break;
	case Action::Cast:
		ApplyCast(player, message.a, tile);
		break;
	case Action::EarlyCallPaid:
		NoteEarlyCallBonus(Swap16LE(message.w));
		Announce(fmt::format("The gates open early: {} gold for the time you gave up.", Swap16LE(message.w)));
		break;
	case Action::RequestStartRaid:
		if (IsHost())
			HostVoteReady(player);
		break;
	case Action::Ping:
		ApplyPing(player.getId(), tile);
		break;
	case Action::RequestSpeed:
		if (IsHost())
			HostRequestSpeed(message.a);
		break;
	case Action::SetSpeed:
		if (message.a < GameSpeeds.size())
			pendingSpeed = { true, message.a, Swap16LE(message.w), static_cast<uint8_t>(player.getId()), 0 };
		break;
	case Action::RequestRetryRaid:
		if (IsHost())
			HostRetryRaid();
		break;
	case Action::RequestLockDoor:
		if (IsHost())
			HostLockDoor(tile);
		break;
	case Action::DoorLocked:
		if (&player != MyPlayer)
			ApplyDoorLocked(message.a, tile);
		break;
	case Action::RequestBait:
		if (IsHost())
			HostPlaceBait(tile);
		break;
	case Action::BaitPlaced:
		if (&player != MyPlayer)
			ApplyBaitPlaced(message.a, tile);
		break;
	case Action::Possess: {
		const size_t pid = player.getId();
		const size_t monsterId = Swap16LE(message.w);
		if (monsterId < MaxMonsters && CanPossess(Monsters[monsterId]) && State.mana[pid] >= PossessManaCost * ManaScale) {
			EndPossession(pid);
			State.mana[pid] -= PossessManaCost * ManaScale;
			State.possessed[pid] = static_cast<int16_t>(monsterId);
			ClearOrder(pid);
		}
		break;
	}
	case Action::Release:
		EndPossession(player.getId());
		break;
	case Action::Order:
		SetOrder(player.getId(), tile, static_cast<int>(Swap16LE(message.w)) - 1);
		break;
	case Action::SpawnBot:
		if (message.a < MaxBots && message.b < NumHeroClasses && InDungeonBounds(tile))
			SpawnBot(message.a, static_cast<HeroClassKind>(message.b), tile);
		break;
	case Action::RequestSell:
		if (IsHost() && message.a <= static_cast<uint8_t>(StructureType::Lock))
			HostSell({ static_cast<StructureType>(message.a), message.b });
		break;
	case Action::Sold:
		if (&player != MyPlayer) // the host did it as it sent this, and may have reused the slot since
			ApplySold(message.a, message.b);
		break;
	case Action::BotEscaped:
		if (&player != MyPlayer && message.b < MaxBots)
			ApplyBotEscaped(message.b, (message.a & 1) != 0, (message.a & 2) != 0, message.x | (message.y << 8));
		break;
	case Action::TrapDisarmed:
		if (&player != MyPlayer && message.a < MaxTraps && State.traps[message.a].active) {
			State.traps[message.a].active = false;
			AnnounceTrapDisarmed(State.traps[message.a], message.b > 0 && message.b <= MaxBots ? std::optional<size_t>(message.b - 1) : std::nullopt);
		}
		break;
	case Action::BotLooted:
		if (&player != MyPlayer && message.b < MaxBots)
			ApplyBotLooted(message.b, message.x | (message.y << 8));
		break;
	case Action::BotFled:
		if (&player != MyPlayer && message.b < MaxBots)
			ApplyBotFled(message.b);
		break;
	case Action::HeroLooted:
		if (&player != MyPlayer)
			ApplyHeroLooted(Swap16LE(message.w), message.x | (message.y << 8));
		break;
	case Action::HeroFled:
		if (&player != MyPlayer)
			ApplyHeroFled(Swap16LE(message.w));
		break;
	case Action::HeroEscaped:
		if (&player != MyPlayer)
			ApplyHeroEscaped(Swap16LE(message.w), (message.a & 1) != 0, (message.a & 2) != 0, message.x | (message.y << 8));
		break;
	case Action::RequestRally:
		if (IsHost())
			HostRally(message.a, message.b != 0, tile);
		break;
	case Action::Rallied:
		ApplyRally(message.a, message.b != 0, tile);
		break;
	case Action::LairMember:
		ApplyLairMember(message.a, message.b, static_cast<int>(Swap16LE(message.w)) - 1);
		break;
	case Action::RequestUpgrade:
		if (IsHost())
			HostBuyUpgrade(message.a, message.b);
		break;
	case Action::UpgradeBought:
		if (message.a < NumUpgrades)
			ApplyUpgradeTier(static_cast<UpgradeId>(message.a), message.b);
		break;
	case Action::RequestWaypoint:
		if (IsHost())
			HostWaypoint(message.a, tile);
		break;
	case Action::WaypointAdded:
		ApplyWaypoint(message.a, message.b, tile);
		break;
	case Action::PatrolLeg:
		ApplyPatrolLeg(message.a, message.b, message.x);
		break;
	case Action::SyncHero:
	case Action::SyncHeroMind:
	case Action::SyncBotState:
	case Action::SyncBotMind:
	case Action::SyncBotPetrified:
	case Action::SyncPortal:
	case Action::SyncPortalCaster:
	case Action::SyncHeroPace:
	case Action::SyncBotPace:
	case Action::SyncBegin:
	case Action::SyncHostSlot:
	case Action::SyncUpgrade:
	case Action::SyncMonsterPosition:
	case Action::SyncMonsterHitPoints:
	case Action::SyncLair:
	case Action::SyncRally:
	case Action::SyncWaypoint:
	case Action::SyncMember:
	case Action::SyncTrap:
	case Action::SyncLock:
	case Action::SyncBait:
	case Action::SyncPossessed:
	case Action::SyncBot:
	case Action::SyncEnd:
		ApplySnapshotEntry(message);
		break;
	case Action::InKeeper:
		CoopNoteInKeeper(player.getId(), message.a != 0);
		break;
	case Action::NextMapSoon:
		if (&player != MyPlayer)
			CoopNoteStarting(Swap16LE(message.w));
		break;
	case Action::RaidResult:
		if (&player != MyPlayer && !IsHost())
			ApplyRaidResultField(message.a, static_cast<int>(static_cast<uint32_t>(Swap16LE(message.w)) | (static_cast<uint32_t>(message.x) << 16) | (static_cast<uint32_t>(message.y) << 24)));
		break;
	case Action::Restart:
		// Only the game's creator picks what comes next; everyone else comes back to its new game.
		if (IsJoinedGame() && &player != MyPlayer && joinerRestartWait == 0)
			joinerRestartWait = RestartJoinerWaitTicks;
		break;
	}
	return sizeof(message);
}

bool IsHeroAt(Point tile)
{
	if (const int mid = dMonster[tile.x][tile.y] - 1; mid >= 0 && IsHero(Monsters[mid]) && Monsters[mid].hitPoints > 0)
		return true;
	const int pid = dPlayer[tile.x][tile.y] - 1;
	return pid >= 0 && IsLivingBot(Players[pid]);
}

bool IsBurning(Point tile)
{
	for (const Missile &missile : Missiles) {
		if (missile._mitype == MissileID::FireWall && !missile._miDelFlag && missile.position.tile == tile)
			return true;
	}
	return false;
}

bool IsHazardous(Point tile)
{
	for (const Missile &missile : Missiles) {
		if (IsAnyOf(missile._mitype, MissileID::FireWall, MissileID::LightningWall, MissileID::Inferno, MissileID::FlameWave) && !missile._miDelFlag && missile.position.tile == tile)
			return true;
	}
	return false;
}

bool AreHostile(const Monster &a, const Monster &b)
{
	return IsHero(a) != IsHero(b);
}

} // namespace devilution::keeper
