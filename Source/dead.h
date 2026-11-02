/**
 * @file dead.h
 *
 * Interface of functions for placing dead monsters.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/clx_sprite.hpp"
#include "engine/direction.hpp"
#include "engine/point.hpp"

namespace devilution {

struct CMonster;

static constexpr unsigned MaxCorpses = 31;

struct Corpse {
	OptionalClxSpriteListOrSheet sprites;
	int frame;
	uint16_t width;
	uint8_t translationPaletteIndex;

	/**
	 * @brief Returns the sprite list for a given direction.
	 *
	 * @param direction One of the 16 directions. Valid range: [0, 15].
	 * @return ClxSpriteList
	 */
	[[nodiscard]] ClxSpriteList spritesForDirection(Direction direction) const
	{
		return sprites->isSheet() ? sprites->sheet()[static_cast<size_t>(direction)] : sprites->list();
	}
};

extern Corpse Corpses[MaxCorpses];
extern int8_t stonendx;

void InitCorpses();
/** [DARK KEEP] Gives a type loaded after the level started a corpse. @return its id, 0 if the table is full. */
int8_t AddCorpseType(const CMonster &monsterType);
void AddCorpse(Point tilePosition, int8_t dv, Direction ddir);
void MoveLightsToCorpses();

} // namespace devilution
