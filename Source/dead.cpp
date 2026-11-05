/**
 * @file dead.cpp
 *
 * Implementation of functions for placing dead monsters.
 */
#include "dead.h"

#include <cstdint>
#include <cstring>

#include "diablo.h"
#include "headless_mode.hpp"
#include "keeper/keeper_heroes.hpp" // [DARK KEEP]
#include "levels/gendung.h"
#include "lighting.h"
#include "monster.h"
#include "tables/misdat.h"

namespace devilution {

Corpse Corpses[MaxCorpses];
int8_t stonendx;
uint16_t dCorpseType[MAXDUNX][MAXDUNY]; // [DARK KEEP]

namespace {
void InitDeadAnimationFromMonster(Corpse &corpse, const CMonster &mon)
{
	const AnimStruct &animData = mon.getAnimData(MonsterGraphic::Death);
	if (animData.sprites) {
		corpse.sprites.emplace(*animData.sprites);
	} else {
		corpse.sprites = std::nullopt;
	}
	corpse.frame = animData.frames - 1;
	corpse.width = animData.width;
}

void MoveLightToCorpse(Monster &monster)
{
	for (int dx = 0; dx < MAXDUNX; dx++) {
		for (int dy = 0; dy < MAXDUNY; dy++) {
			if ((dCorpse[dx][dy] & 0x1F) == monster.corpseId) {
				ChangeLightXY(monster.lightId, { dx, dy });
				return;
			}
		}
	}
	AddUnLight(monster.lightId);
}
} // namespace

void InitCorpses()
{
	int8_t mtypes[MaxMonsters] = {};
	memset(dCorpseType, 0, sizeof(dCorpseType)); // [DARK KEEP]

	int8_t nd = 0;

	for (size_t i = 0; i < LevelMonsterTypeCount; i++) {
		CMonster &monsterType = LevelMonsterTypes[i];
		if (mtypes[monsterType.type] != 0 || keeper::IsDeferredMonsterType(i)) // [DARK KEEP] those get theirs when loaded
			continue;

		InitDeadAnimationFromMonster(Corpses[nd], monsterType);
		Corpses[nd].translationPaletteIndex = 0;
		nd++;

		monsterType.corpseId = nd;
		mtypes[monsterType.type] = nd;
	}

	nd++; // Unused blood spatter

	if (!HeadlessMode)
		Corpses[nd].sprites.emplace(*GetMissileSpriteData(MissileGraphicID::StoneCurseShatter).sprites);
	Corpses[nd].frame = 11;
	Corpses[nd].width = 128;
	Corpses[nd].translationPaletteIndex = 0;
	nd++;

	stonendx = nd;

	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		auto &monster = Monsters[ActiveMonsters[i]];
		if (monster.isUnique()) {
			InitDeadAnimationFromMonster(Corpses[nd], monster.type());
			Corpses[nd].translationPaletteIndex = ActiveMonsters[i] + 1;
			nd++;

			monster.corpseId = nd;
		}
	}

	assert(static_cast<unsigned>(nd) <= MaxCorpses);
}

void AddCorpse(Point tilePosition, int8_t dv, Direction ddir)
{
	if (dv == 0) // [DARK KEEP] no corpse id: AddMonsterCorpse handles those
		return;
	dCorpse[tilePosition.x][tilePosition.y] = (dv & 0x1F) + (static_cast<int>(ddir) << 5);
	dCorpseType[tilePosition.x][tilePosition.y] = 0; // [DARK KEEP] this body covers any drawn from there
}

// [DARK KEEP] A keep's lair kinds come after the level's first 31 corpse ids: those are drawn by type.
// The marker in dCorpse doesn't depend on which types a client has loaded, or in what order, so
// scavengers and doors see the same bodies on every client (and in headless mode).
void AddMonsterCorpse(const Monster &monster)
{
	const Point tile = monster.position.tile;
	const int8_t id = monster.isUnique() && monster.corpseId != 0 ? monster.corpseId : monster.type().corpseId;
	if (id != 0) {
		AddCorpse(tile, id, monster.direction);
		return;
	}
	dCorpse[tile.x][tile.y] = CorpseByTypeMarker + (static_cast<int>(monster.direction) << 5);
	dCorpseType[tile.x][tile.y] = static_cast<uint16_t>((monster.levelType + 1) | (static_cast<unsigned>(monster.direction) << 8));
}

void AddBodyOfType(Point tilePosition, size_t typeIndex, Direction ddir)
{
	if (!InDungeonBounds(tilePosition) || typeIndex >= LevelMonsterTypeCount)
		return;
	dCorpseType[tilePosition.x][tilePosition.y] = static_cast<uint16_t>((typeIndex + 1) | (static_cast<unsigned>(ddir) << 8) | BodyWithoutMarker);
}

void ClearCorpseAt(Point tilePosition)
{
	if (!InDungeonBounds(tilePosition))
		return;
	dCorpse[tilePosition.x][tilePosition.y] = 0;
	dCorpseType[tilePosition.x][tilePosition.y] = 0;
}

std::optional<size_t> CorpseTypeAt(Point tilePosition)
{
	const uint16_t body = dCorpseType[tilePosition.x][tilePosition.y];
	const size_t type = body & 0xFF;
	if (type == 0 || type > LevelMonsterTypeCount)
		return std::nullopt;
	if ((body & BodyWithoutMarker) == 0 && dCorpse[tilePosition.x][tilePosition.y] == 0)
		return std::nullopt; // eaten
	return type - 1;
}

void MoveLightsToCorpses()
{
	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		auto &monster = Monsters[ActiveMonsters[i]];
		if (!monster.isUnique())
			continue;
		MoveLightToCorpse(monster);
	}
}

} // namespace devilution
