/**
 * @file keeper/keeper_lighting.cpp
 *
 * The keep's ambient, fixed lights and heroes' torchlight. See keeper_lighting.hpp.
 */
#include "keeper/keeper_lighting.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

#include <fmt/format.h>

#include "engine/displacement.hpp"
#include "engine/world_tile.hpp"
#include "keeper/keeper.hpp"
#include "keeper/keeper_bots.hpp"
#include "keeper/keeper_editor.hpp"
#include "keeper/keeper_game.hpp"
#include "levels/gendung.h"
#include "lighting.h"
#include "monster.h"
#include "objects.h"
#include "player.h"
#include "utils/is_of.hpp"

namespace devilution::keeper {

namespace {

/**
 * How dark the keep is away from any light, on the engine's scale (0 = fully lit, 15 = black), while the
 * masters prepare and once a raid is on. The Catacombs' and Caves' art is darker to begin with.
 */
struct Ambient {
	uint8_t prep;
	uint8_t raid;
};
constexpr Ambient CathedralAmbient { 6, 8 };
constexpr Ambient CatacombsAmbient { 4, 6 };
constexpr Ambient CavesAmbient { 4, 6 };
constexpr Ambient HellAmbient { 5, 7 };
/** Their light tables never go fully dark, and lava stays bright. */
constexpr Ambient CryptAmbient { 6, 8 };
constexpr Ambient NestAmbient { 5, 7 };
/** A map being edited is about its layout: lit enough to work on. A test run (F8) shows the real dark. */
constexpr uint8_t EditorAmbient = 3;
/** When a raid starts or ends, the ambient moves one step every this many game ticks. */
constexpr int AmbientStepTicks = 3;

/** Baked into the static light, so they cost no light slot. The entrance is the raid's portal. */
constexpr uint8_t EntranceLightRadius = 8;
constexpr uint8_t VaultLightRadius = 6;

/** A hero's torch. The base game's player light is 10; a whole party of those would light a room. */
constexpr uint8_t HeroLightRadius = 8;
/**
 * The engine's MAXLIGHTS (32) slots are shared with missiles (a fire wall takes one per segment), unique
 * monsters and berserkers. Heroes keep their light for their whole raid, so they never take the last few:
 * a raid at its fullest still has room for its spells.
 */
constexpr size_t MaxHeroLights = 12;
constexpr int EngineLightReserve = 10;

/** Monsters in the keep are drawn this many steps brighter than their tile, as the engine draws other players. */
constexpr int UnitLightBoost = 5;

/** The static light (map objects, lava, the entrance and vault) before the ambient. */
uint8_t staticLight[MAXDUNX][MAXDUNY];
bool haveStatic = false;
uint8_t currentAmbient = 0;
int ambientStepWait = 0;
int devPrepAmbient = -1;
int devRaidAmbient = -1;

struct HeroLight {
	bool bot = false;
	/** Player id for a bot, monster id for a monster hero. */
	size_t index = 0;
	int lightId = NO_LIGHT;
};
std::array<HeroLight, MaxHeroLights> heroLights;
size_t heroLightCount = 0;
/** Heroes who wanted a light and found no free slot, last tick. */
size_t heroesInDark = 0;

Ambient TilesetAmbient()
{
	switch (leveltype) {
	case DTYPE_CATACOMBS:
		return CatacombsAmbient;
	case DTYPE_CAVES:
		return CavesAmbient;
	case DTYPE_HELL:
		return HellAmbient;
	case DTYPE_CRYPT:
		return CryptAmbient;
	case DTYPE_NEST:
		return NestAmbient;
	default:
		return CathedralAmbient;
	}
}

uint8_t TargetAmbient()
{
	if (IsEditorActive())
		return EditorAmbient;
	const Ambient ambient = TilesetAmbient();
	const bool raid = State.phase == Phase::Raid;
	const int dev = raid ? devRaidAmbient : devPrepAmbient;
	if (dev >= 0)
		return static_cast<uint8_t>(std::min<int>(dev, LightsMax));
	return raid ? ambient.raid : ambient.prep;
}

void StampLight(Point tile, uint8_t radius)
{
	if (!InDungeonBounds(tile))
		return;
	// The engine's own way of baking a map object's light: into dPreLight rather than dLight.
	const bool wasLoading = LoadingMapObjects;
	LoadingMapObjects = true;
	DoLighting(tile, radius, {});
	LoadingMapObjects = wasLoading;
}

/** dPreLight holds the level's static light, as the engine built it: adds the fixed lights and keeps a copy. */
void CaptureStaticLight(Point entrance, Point vault)
{
	StampLight(entrance, EntranceLightRadius);
	if (vault != entrance)
		StampLight(vault, VaultLightRadius);
	std::memcpy(staticLight, dPreLight, sizeof(staticLight));
	haveStatic = true;
}

void ApplyAmbient(uint8_t ambient)
{
	currentAmbient = ambient;
	if (!haveStatic)
		return;
	for (int x = 0; x < MAXDUNX; x++) {
		for (int y = 0; y < MAXDUNY; y++) {
			dPreLight[x][y] = std::min(staticLight[x][y], ambient);
			dLight[x][y] = dPreLight[x][y];
		}
	}
	// Every moving light draws itself again over the new floor, before anything is drawn.
	UpdateLighting = true;
	ProcessLightList();
}

void ReleaseHeroLight(size_t slot)
{
	AddUnLight(heroLights[slot].lightId);
	heroLights[slot] = heroLights[--heroLightCount];
	heroLights[heroLightCount] = {};
}

void ForgetHeroLights()
{
	// The engine's InitLighting has already emptied the light list on a level load.
	heroLights = {};
	heroLightCount = 0;
}

bool IsLitBot(const Player &player)
{
	return IsLivingBot(player);
}

bool IsLitMonsterHero(size_t id)
{
	if (id >= MaxMonsters)
		return false;
	const Monster &monster = Monsters[id];
	return monster.hitPoints > 0 && monster.mode != MonsterMode::Death && monster.animInfo.sprites && IsHero(monster) && InDungeonBounds(monster.position.tile);
}

void MoveLight(int lightId, Point tile, WorldTileDisplacement walkOffset)
{
	if (Lights[lightId].position.tile != tile)
		ChangeLightXY(lightId, tile);
	ChangeLightOffset(lightId, walkOffset.screenToLight());
}

void FollowHero(const HeroLight &light)
{
	if (light.bot) {
		const Player &bot = Players[light.index];
		const WorldTileDisplacement offset = bot.isWalking() ? bot.position.CalculateWalkingOffset(bot._pdir, bot.AnimInfo) : WorldTileDisplacement {};
		MoveLight(light.lightId, bot.position.tile, offset);
	} else {
		const Monster &monster = Monsters[light.index];
		const WorldTileDisplacement offset = monster.isWalking() ? monster.position.CalculateWalkingOffset(monster.direction, monster.animInfo) : WorldTileDisplacement {};
		MoveLight(light.lightId, monster.position.tile, offset);
	}
}

bool HasHeroLight(bool bot, size_t index)
{
	return std::any_of(heroLights.begin(), heroLights.begin() + heroLightCount, [&](const HeroLight &light) { return light.bot == bot && light.index == index; });
}

void TryLightHero(bool bot, size_t index, Point tile)
{
	if (HasHeroLight(bot, index))
		return;
	if (heroLightCount >= MaxHeroLights || ActiveLightCount >= MAXLIGHTS - EngineLightReserve) {
		heroesInDark++;
		return;
	}
	const int lightId = AddLight(tile, HeroLightRadius);
	if (lightId == NO_LIGHT) {
		heroesInDark++;
		return;
	}
	heroLights[heroLightCount++] = { bot, index, lightId };
	FollowHero(heroLights[heroLightCount - 1]);
}

void UpdateHeroLights()
{
	for (size_t slot = 0; slot < heroLightCount;) {
		const HeroLight &light = heroLights[slot];
		const bool alive = light.bot ? light.index < Players.size() && IsLitBot(Players[light.index]) : IsLitMonsterHero(light.index);
		if (!alive) {
			ReleaseHeroLight(slot);
			continue;
		}
		FollowHero(light);
		slot++;
	}

	heroesInDark = 0;
	// Bots first: they lead their party.
	for (size_t i = 0; i < MaxBots && FirstBotId + i < Players.size(); i++) {
		const Player &bot = Players[FirstBotId + i];
		if (IsLitBot(bot))
			TryLightHero(true, FirstBotId + i, bot.position.tile);
	}
	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		const size_t id = ActiveMonsters[i];
		if (IsLitMonsterHero(id))
			TryLightHero(false, id, Monsters[id].position.tile);
	}
}

/** A master's body waits in the rock at the map's corner: its light would only take a slot. */
void FreeMasterLights()
{
	for (Player &player : Players) {
		if (IsBot(player) || player.lightId == NO_LIGHT)
			continue;
		AddUnLight(player.lightId);
		player.lightId = NO_LIGHT;
	}
}

} // namespace

void LightingOnLevelLoaded(Point entrance, Point vault)
{
	ForgetHeroLights();
	FreeMasterLights();
	CaptureStaticLight(entrance, vault);
	ApplyAmbient(TargetAmbient());
	ambientStepWait = 0;
}

void LightingBeforeLevelRebuild()
{
	// The rebuild bakes the map's objects and lava into dLight, as a fresh level does before SavePreLighting.
	std::memset(dLight, LightsMax, sizeof(dLight));
}

void LightingAfterLevelRebuild(Point entrance, Point vault)
{
	SavePreLighting();
	CaptureStaticLight(entrance, vault);
	ApplyAmbient(currentAmbient);
}

void LightingTick()
{
	UpdateHeroLights();

	const uint8_t target = TargetAmbient();
	if (target == currentAmbient) {
		ambientStepWait = 0;
	} else if (ambientStepWait > 0) {
		ambientStepWait--;
	} else {
		ApplyAmbient(currentAmbient < target ? currentAmbient + 1 : currentAmbient - 1);
		ambientStepWait = AmbientStepTicks - 1;
	}
	// Hero lights moved this tick after the engine's pass: draw them now, not a tick late.
	ProcessLightList();
}

int MonsterLightIndex(int lightTableIndex)
{
	if (!IsActive())
		return lightTableIndex;
	return std::max(lightTableIndex - UnitLightBoost, 0);
}

void DevSetAmbient(int prep, int raid)
{
	devPrepAmbient = prep;
	devRaidAmbient = raid;
	ApplyAmbient(TargetAmbient());
}

std::string DevLightAt(Point tile)
{
	if (!InDungeonBounds(tile))
		return "lightat out of bounds";
	int dimmest = 0;
	int lit = 0;
	for (int x = 0; x < MAXDUNX; x++) {
		for (int y = 0; y < MAXDUNY; y++) {
			dimmest = std::max<int>(dimmest, staticLight[x][y]);
			lit += staticLight[x][y] < currentAmbient ? 1 : 0;
		}
	}
	return fmt::format("lightat {},{} light={} pre={} static={} tilesBrighterThanAmbient={} dimmestStatic={}", tile.x, tile.y, dLight[tile.x][tile.y], dPreLight[tile.x][tile.y], staticLight[tile.x][tile.y], lit, dimmest);
}

std::string DevLightingReport()
{
	const Ambient ambient = TilesetAmbient();
	return fmt::format("lighting ambient={} target={} tileset={}/{} heroLights={} heroesInDark={} activeLights={}/{}",
	    currentAmbient, TargetAmbient(), ambient.prep, ambient.raid, heroLightCount, heroesInDark, ActiveLightCount, MAXLIGHTS);
}

} // namespace devilution::keeper
