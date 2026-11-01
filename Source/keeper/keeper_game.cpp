/**
 * @file keeper/keeper_game.cpp
 *
 * The Dark Keep's simulation and network protocol. See keeper_game.hpp.
 */
#include "keeper/keeper_game.hpp"

#include <algorithm>
#include <cstring>

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
#include "utils/endian_swap.hpp"
#include "utils/str_cat.hpp"

namespace devilution::keeper {

const std::array<LairKind, NumLairKinds> LairKinds { {
	{ "Zombie Pit", MT_NZOMBIE, 3, 100 },
	{ "Fallen Den", MT_RFALLSP, 4, 120 },
	{ "Bone Crypt", MT_WSKELAX, 3, 180 },
	{ "Archer Roost", MT_WSKELBW, 2, 200 },
	{ "Overlord Lair", MT_FAT, 1, 260 },
} };

const std::array<SpellKind, NumSpellKinds> SpellKinds { {
	{ "Lightning Nova", MissileID::Nova, 35, 6, 20 * 6 },
	{ "Fire Wall", MissileID::FireWallControl, 25, 4, 20 * 4 },
	{ "Guardian", MissileID::Guardian, 30, 4, 20 * 8 },
	{ "Stone Curse", MissileID::StoneCurse, 20, 4, 20 * 5 },
} };

KeepState State;

namespace {

constexpr int TicksPerSecond = 20;
constexpr int SetupTicks = 4 * 60 * TicksPerSecond;
constexpr int BreatherTicks = 75 * TicksPerSecond;
constexpr int StartingVaultGold = 1200;
/** Mana regenerated per tick, in hundredths (100 = 1 mana per tick). */
constexpr int ManaRegenPerTick = 6; // 1.2 mana per second
/** Minimum spacing between lairs (tiles) and distance from the entrance/vault. */
constexpr int LairSpacing = 3;
constexpr int LairKeepOut = 5;
constexpr int StateBroadcastInterval = TicksPerSecond;

enum class Action : uint8_t {
	RequestBuild = 1,
	LairAdded,
	Cast,
	RequestStartRaid,
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
	cmd.reserved = 0;
	cmd.phaseTicks = Swap32LE(static_cast<uint32_t>(std::max(State.phaseTicks, 0)));
	cmd.vaultGold = static_cast<int32_t>(Swap32LE(static_cast<uint32_t>(State.vaultGold)));
	for (size_t i = 0; i < 4; i++)
		cmd.mana[i] = Swap16LE(static_cast<uint16_t>(State.mana[i] / ManaScale));
	NetSendHiPri(MyPlayerId, reinterpret_cast<std::byte *>(&cmd), sizeof(cmd));
	ticksUntilBroadcast = StateBroadcastInterval;
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

void SetPhase(Phase phase)
{
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

void RefillLairs();

void StartRaid()
{
	SetPhase(Phase::Raid);
	if (IsHost())
		StartWave(State.wave);
}

void HostBuildLair(const Player &requester, size_t kind, Point tile)
{
	if (kind >= LairKinds.size() || !CanBuildLairAt(kind, tile))
		return;
	const LairKind &lairKind = LairKinds[kind];
	if (State.vaultGold < lairKind.cost)
		return;

	const auto slot = std::find_if(State.lairs.begin(), State.lairs.end(), [](const Lair &lair) { return !lair.active; });
	if (slot == State.lairs.end())
		return;

	State.vaultGold -= lairKind.cost;
	const auto lairId = static_cast<uint8_t>(slot - State.lairs.begin());
	Send(Action::LairAdded, lairId, static_cast<uint8_t>(kind), tile, static_cast<uint16_t>(requester.getId()));
	BroadcastState();
}

void ApplyLairAdded(uint8_t lairId, uint8_t kind, Point tile)
{
	if (lairId >= MaxLairs || kind >= LairKinds.size())
		return;
	Lair &lair = State.lairs[lairId];
	lair.active = true;
	lair.kind = kind;
	lair.position = tile;
	lair.members.fill(-1);
	if (IsHost())
		RefillLairs();
}

bool IsMemberAlive(int16_t monsterId, _monster_id type)
{
	if (monsterId < 0)
		return false;
	const Monster &monster = Monsters[monsterId];
	return monster.hitPoints > 0 && monster.position.tile != GolemHoldingCell && monster.type().type == type && !monster.isInvalid;
}

/** Host only: tops every lair back up to full strength. */
void RefillLairs()
{
	for (Lair &lair : State.lairs) {
		if (!lair.active)
			continue;
		const LairKind &kind = LairKinds[lair.kind];
		const std::optional<size_t> typeIndex = FindLevelMonsterType(kind.type);
		if (!typeIndex)
			continue;
		for (size_t i = 0; i < kind.count && i < MaxLairMembers; i++) {
			if (IsMemberAlive(lair.members[i], kind.type))
				continue;
			lair.members[i] = -1;
			if (ActiveMonsterCount >= MaxMonsters)
				return;
			const std::optional<Point> spawnTile = FindClosestValidPosition(IsFreeFloor, lair.position, 0, 4);
			if (!spawnTile)
				break;
			lair.members[i] = static_cast<int16_t>(ActiveMonsters[ActiveMonsterCount]);
			SpawnMonster(*spawnTile, Direction::South, *typeIndex);
		}
	}
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

} // namespace

std::string_view PhaseName(Phase phase)
{
	switch (phase) {
	case Phase::Setup:
		return "Preparation";
	case Phase::Raid:
		return "Raid";
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

	if (!IsHost())
		return;

	switch (State.phase) {
	case Phase::Setup:
	case Phase::Breather:
		if (State.phaseTicks == 0)
			StartRaid();
		break;
	case Phase::Raid: {
		const RaidStatus status = UpdateRaid();
		if (status == RaidStatus::Repelled) {
			if (State.wave >= NumWaves) {
				SetPhase(Phase::Victory);
			} else {
				State.wave++;
				RefillLairs();
				SetPhase(Phase::Breather);
			}
		}
		break;
	}
	default:
		break;
	}

	if (State.phase != Phase::Victory && State.phase != Phase::Defeat && State.vaultGold <= 0 && State.phase == Phase::Raid && AllThievesGone())
		SetPhase(Phase::Defeat);

	if (--ticksUntilBroadcast <= 0)
		BroadcastState();
}

bool CanBuildLairAt(size_t kind, Point tile, std::string_view *reason)
{
	auto fail = [reason](std::string_view why) {
		if (reason != nullptr)
			*reason = why;
		return false;
	};
	if (kind >= LairKinds.size())
		return fail("Unknown lair");
	if (State.phase == Phase::Victory || State.phase == Phase::Defeat)
		return fail("The siege is over");
	if (!InDungeonBounds(tile) || !IsTileWalkable(tile) || IsObjectAtPosition(tile))
		return fail("Needs open floor");
	if (tile.WalkingDistance(State.entrance) < LairKeepOut)
		return fail("Too close to the entrance");
	if (tile.WalkingDistance(State.vault) < LairKeepOut - 2)
		return fail("Too close to the vault");
	for (const Lair &lair : State.lairs) {
		if (lair.active && lair.position.WalkingDistance(tile) < LairSpacing)
			return fail("Too close to another lair");
	}
	if (State.vaultGold < LairKinds[kind].cost)
		return fail("Not enough gold");
	return true;
}

void RequestBuildLair(size_t kind, Point tile)
{
	Send(Action::RequestBuild, static_cast<uint8_t>(kind), 0, tile);
}

void RequestCastSpell(size_t spell, Point tile)
{
	Send(Action::Cast, static_cast<uint8_t>(spell), 0, tile);
}

void RequestStartRaid()
{
	Send(Action::RequestStartRaid);
}

size_t HandleNetCommand(Player &player, const TCmd *cmd, size_t maxCmdSize)
{
	if (cmd->bCmd == CMD_KEEPER_STATE) {
		if (maxCmdSize < sizeof(TCmdKeeperState))
			return maxCmdSize;
		const auto &message = *reinterpret_cast<const TCmdKeeperState *>(cmd);
		if (&player != MyPlayer && !IsHost()) {
			State.phase = static_cast<Phase>(std::min<uint8_t>(message.phase, static_cast<uint8_t>(Phase::Defeat)));
			State.wave = message.wave;
			State.phaseTicks = static_cast<int>(Swap32LE(message.phaseTicks));
			State.vaultGold = static_cast<int32_t>(Swap32LE(static_cast<uint32_t>(message.vaultGold)));
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
			HostBuildLair(player, message.a, tile);
		break;
	case Action::LairAdded:
		ApplyLairAdded(message.a, message.b, tile);
		break;
	case Action::Cast:
		ApplyCast(player, message.a, tile);
		break;
	case Action::RequestStartRaid:
		if (IsHost() && (State.phase == Phase::Setup || State.phase == Phase::Breather))
			StartRaid();
		break;
	}
	return sizeof(message);
}

bool AreHostile(const Monster &a, const Monster &b)
{
	return IsHero(a) != IsHero(b);
}

} // namespace devilution::keeper
