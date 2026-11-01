/**
 * @file keeper/keeper_game.cpp
 *
 * The Dark Keep's simulation and network protocol. See keeper_game.hpp.
 */
#include "keeper/keeper_game.hpp"

#include <algorithm>
#include <cstring>

#include <fmt/format.h>

#include "engine/direction.hpp"
#include "engine/path.h"
#include "keeper/keeper.hpp"
#include "keeper/keeper_heroes.hpp"
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

const std::array<BuildInfo, NumBuildKinds> BuildKinds { {
	{ "Zombie", 100, MT_NZOMBIE, 3 },
	{ "Fallen", 120, MT_RFALLSP, 4 },
	{ "Bones", 180, MT_WSKELAX, 3 },
	{ "Archer", 200, MT_WSKELBW, 2 },
	{ "Brute", 260, MT_FAT, 1 },
	{ "Trap", 80, MT_INVALID, 0 },
} };

const std::array<SpellKind, NumSpellKinds> SpellKinds { {
	{ "Nova", MissileID::Nova, 35, 6, 20 * 6 },
	{ "Wall", MissileID::FireWallControl, 25, 4, 20 * 4 },
	{ "Guard", MissileID::Guardian, 30, 4, 20 * 8 },
	{ "Stone", MissileID::StoneCurse, 20, 4, 20 * 5 },
} };

KeepState State;

namespace {

constexpr int TicksPerSecond = 20;
constexpr int SetupTicks = 4 * 60 * TicksPerSecond;
constexpr int BreatherTicks = 75 * TicksPerSecond;
constexpr int StartingVaultGold = 1200;
/** Mana regenerated per tick, in hundredths (100 = 1 mana per tick). */
constexpr int ManaRegenPerTick = 6; // 1.2 mana per second
/** Minimum spacing between placements (tiles) and distance from the entrance/vault. */
constexpr int PlacementSpacing = 3;
constexpr int EntranceKeepOut = 4;
constexpr int VaultKeepOut = 3;
/** During a raid nothing can be placed this close to a hero, and lairs take a while to summon. */
constexpr int RaidBuildHeroDistance = 6;
constexpr int RaidSummonTicks = 3 * TicksPerSecond;
constexpr int TrapRearmTicks = 8 * TicksPerSecond;
constexpr int LockStrength = 12;
constexpr int BaitUses = 3;
constexpr int StateBroadcastInterval = TicksPerSecond;
/** Net mana cost of possession, in hundredths per tick (on top of cancelling regeneration). */
constexpr int PossessDrainPerTick = 10;

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
};
#pragma pack(pop)

int ticksUntilBroadcast = 0;

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
	cmd.reserved = static_cast<uint8_t>((State.bossDown ? 1 : 0) | (State.portalEscape ? 2 : 0));
	cmd.phaseTicks = Swap32LE(static_cast<uint32_t>(std::max(State.phaseTicks, 0)));
	cmd.vaultGold = static_cast<int32_t>(Swap32LE(static_cast<uint32_t>(State.vaultGold)));
	cmd.goldLost = static_cast<int32_t>(Swap32LE(static_cast<uint32_t>(State.goldLost)));
	for (size_t i = 0; i < 4; i++)
		cmd.mana[i] = Swap16LE(static_cast<uint16_t>(State.mana[i] / ManaScale));
	NetSendHiPri(MyPlayerId, reinterpret_cast<std::byte *>(&cmd), sizeof(cmd));
	ticksUntilBroadcast = StateBroadcastInterval;
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
	return false;
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

int TributeFor(uint8_t wave)
{
	return 100 + 25 * wave;
}

/** Runs on every client when the phase changes (locally on the host, from the snapshot elsewhere). */
void AnnouncePhase(Phase from, Phase to)
{
	if (from == to)
		return;
	if (from == Phase::Raid)
		ClosePortal();
	switch (to) {
	case Phase::Raid:
		Announce(State.wave >= NumWaves ? std::string("The champions have come for your gold!") : fmt::format("Raid {}: heroes approach the keep!", State.wave), UiFlags::ColorRed);
		break;
	case Phase::Breather:
		if (State.wave > 1)
			Announce(fmt::format("Raid {} is over. The dark lands pay {} gold in tribute.", State.wave - 1, TributeFor(State.wave - 1)));
		break;
	case Phase::Victory:
		Announce("The keep stands! Every raid has been repelled.");
		break;
	case Phase::Defeat:
		Announce("The Keep Lord is slain and the heroes got away. The keep has fallen.", UiFlags::ColorRed);
		break;
	case Phase::Setup:
		break;
	}
}

void SetPhase(Phase phase)
{
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
}

bool IsMemberAlive(int16_t monsterId, _monster_id type)
{
	if (monsterId < 0)
		return false;
	const Monster &monster = Monsters[monsterId];
	return monster.hitPoints > 0 && monster.mode != MonsterMode::Death && monster.position.tile != GolemHoldingCell && monster.type().type == type && !monster.isInvalid;
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
		lair.members[i] = static_cast<int16_t>(ActiveMonsters[ActiveMonsterCount]);
		SpawnMonster(*spawnTile, Direction::South, *typeIndex);
	}
}

void RefillLairs()
{
	for (Lair &lair : State.lairs)
		RefillLair(lair);
}

void StartRaid()
{
	SetPhase(Phase::Raid);
	if (IsHost())
		StartWave(State.wave);
}

void EndRaid()
{
	const int tribute = TributeFor(State.wave);
	if (State.bossDown) {
		// The party was wiped out before anyone escaped: the Lord rises again (UpdateBoss respawns it).
		State.bossDown = false;
		State.portalEscape = false;
		Announce("The heroes were stopped. The Keep Lord rises again!", UiFlags::ColorRed);
	}
	if (State.wave >= NumWaves) {
		SetPhase(Phase::Victory);
		return;
	}
	State.vaultGold += tribute;
	State.wave++;
	for (Trap &trap : State.traps)
		trap.cooldown = 0;
	RefillLairs();
	SetPhase(Phase::Breather);
}

// ---- host-side request handling ----------------------------------------------

void HostBuild(const Player &requester, size_t kind, Point tile)
{
	if (kind >= BuildKinds.size() || !CanBuildAt(kind, tile))
		return;
	State.vaultGold -= BuildKinds[kind].cost;
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
	Send(Action::Built, slot, static_cast<uint8_t>(kind), tile, static_cast<uint16_t>(requester.getId()));
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
		trap.active = true;
		trap.position = tile;
		trap.cooldown = summon;
	}
}

void HostLockDoor(Point doorPosition)
{
	if (!CanLockDoorAt(doorPosition))
		return;
	const auto it = std::find_if(State.locks.begin(), State.locks.end(), [](const DoorLock &lock) { return !lock.active; });
	if (it == State.locks.end())
		return;
	State.vaultGold -= LockDoorCost;
	Send(Action::DoorLocked, static_cast<uint8_t>(it - State.locks.begin()), 0, doorPosition);
	BroadcastState();
}

void ApplyDoorLocked(uint8_t slot, Point doorPosition)
{
	if (slot >= MaxLocks)
		return;
	Object *door = FindObjectAtPosition(doorPosition);
	if (door == nullptr || !door->isDoor())
		return;
	if (!KeeperIsDoorClosed(*door))
		KeeperOperateDoor(*door, false); // every client closes it locally
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
	State.vaultGold -= BaitCost;
	Send(Action::BaitPlaced, static_cast<uint8_t>(it - State.baits.begin()), 0, tile);
	BroadcastState();
}

void ApplyBaitPlaced(uint8_t slot, Point tile)
{
	if (slot >= MaxBaits)
		return;
	Bait &bait = State.baits[slot];
	bait.active = true;
	bait.position = tile;
	bait.uses = BaitUses;
}

void ApplyCast(Player &caster, size_t spell, Point tile)
{
	if (spell >= SpellKinds.size() || !InDungeonBounds(tile))
		return;
	const size_t pid = caster.getId();
	const SpellKind &kind = SpellKinds[spell];
	State.mana[pid] = std::max(State.mana[pid] - kind.manaCost * ManaScale, 0);
	State.spellCooldown[pid] = kind.cooldownTicks;

	Direction dir = Direction::South;
	WorldTilePosition src = tile;
	if (kind.missile == MissileID::FireWallControl) {
		// A wall needs a direction: run it across the screen (north-east to south-west).
		dir = Direction::SouthEast;
		src = tile + Displacement(Direction::NorthWest);
	}
	AddMissile(src, tile, dir, kind.missile, TARGET_MONSTERS, caster, 0, kind.spellLevel);
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
		for (size_t i = 0; i < ActiveMonsterCount; i++) {
			const Monster &monster = Monsters[ActiveMonsters[i]];
			if (!IsHero(monster) || monster.hitPoints <= 0 || monster.position.tile.WalkingDistance(trap.position) > 1)
				continue;
			const Direction dir = GetDirection(trap.position, monster.position.tile);
			AddMissile(trap.position + Displacement(Direction::NorthWest), trap.position, Direction::SouthEast, MissileID::FireWallControl, TARGET_PLAYERS, -1, 0, 2);
			AddMissile(trap.position, monster.position.tile, dir, MissileID::Firebolt, TARGET_PLAYERS, -1, 20 + 5 * State.wave, 0);
			trap.cooldown = TrapRearmTicks;
			break;
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
	return MyPlayer != nullptr && MyPlayer->isLevelOwnedByLocalClient();
}

void ResetGame(Point entrance, Point vault)
{
	State = {};
	State.entrance = entrance;
	State.vault = vault;
	State.vaultGold = StartingVaultGold;
	State.wave = 1;
	State.phase = Phase::Setup;
	State.phaseTicks = SetupTicks;
	for (int &mana : State.mana)
		mana = MaxMana * ManaScale / 2;
	for (Lair &lair : State.lairs)
		lair.members.fill(-1);
	ticksUntilBroadcast = 0;
	ResetHeroes();
}

void GameTick()
{
	// Mana regenerates for every master, on every client (snapshots correct any drift).
	for (size_t i = 0; i < State.mana.size(); i++) {
		State.mana[i] = std::min(State.mana[i] + ManaRegenPerTick, MaxMana * ManaScale);
		if (State.spellCooldown[i] > 0)
			State.spellCooldown[i]--;
	}

	if (State.phaseTicks > 0)
		State.phaseTicks--;

	// Possession drains mana (2/s net); an empty pool breaks the link. Every client runs this.
	for (size_t i = 0; i < State.possessed.size(); i++) {
		if (State.possessed[i] < 0)
			continue;
		State.mana[i] -= ManaRegenPerTick + PossessDrainPerTick;
		if (State.mana[i] <= 0) {
			State.mana[i] = 0;
			EndPossession(i);
		}
	}

	if (IsSiegeOver())
		return;

	UpdateTraps();
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

	switch (State.phase) {
	case Phase::Setup:
	case Phase::Breather:
		if (State.phaseTicks == 0)
			StartRaid();
		break;
	case Phase::Raid:
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

bool CanBuildAt(size_t kind, Point tile, std::string_view *reason)
{
	auto fail = [reason](std::string_view why) {
		if (reason != nullptr)
			*reason = why;
		return false;
	};
	if (kind >= BuildKinds.size())
		return fail("Unknown structure");
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
	return true;
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

void RequestStartRaid()
{
	Send(Action::RequestStartRaid);
}

void RequestPossess(size_t monsterId)
{
	Send(Action::Possess, 0, 0, {}, static_cast<uint16_t>(monsterId));
}

void RequestRelease()
{
	Send(Action::Release);
}

void RequestOrder(Point tile, int targetMonster)
{
	Send(Action::Order, 0, 0, tile, static_cast<uint16_t>(targetMonster + 1));
}

void AddVaultGold(int amount)
{
	State.vaultGold += amount;
}

size_t HandleNetCommand(Player &player, const TCmd *cmd, size_t maxCmdSize)
{
	if (cmd->bCmd == CMD_KEEPER_STATE) {
		if (maxCmdSize < sizeof(TCmdKeeperState))
			return maxCmdSize;
		const auto &message = *reinterpret_cast<const TCmdKeeperState *>(cmd);
		if (&player != MyPlayer && !IsHost()) {
			const Phase phase = static_cast<Phase>(std::min<uint8_t>(message.phase, static_cast<uint8_t>(Phase::Defeat)));
			State.wave = message.wave;
			const bool wasBossDown = State.bossDown;
			State.bossDown = (message.reserved & 1) != 0;
			State.portalEscape = (message.reserved & 2) != 0;
			AnnouncePhase(State.phase, phase);
			if (wasBossDown && !State.bossDown && phase != Phase::Defeat)
				Announce("The heroes were stopped. The Keep Lord rises again!", UiFlags::ColorRed);
			State.phase = phase;
			State.phaseTicks = static_cast<int>(Swap32LE(message.phaseTicks));
			State.vaultGold = static_cast<int32_t>(Swap32LE(static_cast<uint32_t>(message.vaultGold)));
			State.goldLost = static_cast<int32_t>(Swap32LE(static_cast<uint32_t>(message.goldLost)));
			for (size_t i = 0; i < 4; i++)
				State.mana[i] = Swap16LE(message.mana[i]) * ManaScale;
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
	case Action::Built:
		ApplyBuilt(message.a, message.b, tile);
		break;
	case Action::Cast:
		ApplyCast(player, message.a, tile);
		break;
	case Action::RequestStartRaid:
		if (IsHost() && (State.phase == Phase::Setup || State.phase == Phase::Breather))
			StartRaid();
		break;
	case Action::RequestLockDoor:
		if (IsHost())
			HostLockDoor(tile);
		break;
	case Action::DoorLocked:
		ApplyDoorLocked(message.a, tile);
		break;
	case Action::RequestBait:
		if (IsHost())
			HostPlaceBait(tile);
		break;
	case Action::BaitPlaced:
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
	}
	return sizeof(message);
}

bool AreHostile(const Monster &a, const Monster &b)
{
	return IsHero(a) != IsHero(b);
}

} // namespace devilution::keeper
