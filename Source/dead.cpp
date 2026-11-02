/**
 * @file dead.cpp
 *
 * Implementation of functions for placing dead monsters.
 */
#include "dead.h"

#include <cstdint>

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

/** [DARK KEEP] The next free slot in Corpses. */
int8_t nextCorpse = 0;

int8_t AddCorpseType(const CMonster &monsterType)
{
	if (static_cast<unsigned>(nextCorpse) >= MaxCorpses)
		return 0; // corpse ids are 5 bits: this type just leaves no corpse
	InitDeadAnimationFromMonster(Corpses[nextCorpse], monsterType);
	Corpses[nextCorpse].translationPaletteIndex = 0;
	return ++nextCorpse;
}

void InitCorpses()
{
	int8_t mtypes[MaxMonsters] = {};

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
	nextCorpse = nd; // [DARK KEEP]
}

void AddCorpse(Point tilePosition, int8_t dv, Direction ddir)
{
	if (dv == 0) // [DARK KEEP] a type past the end of the corpse table
		return;
	dCorpse[tilePosition.x][tilePosition.y] = (dv & 0x1F) + (static_cast<int>(ddir) << 5);
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
