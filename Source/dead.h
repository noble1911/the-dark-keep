/**
 * @file dead.h
 *
 * Interface of functions for placing dead monsters.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "engine/clx_sprite.hpp"
#include "engine/direction.hpp"
#include "engine/point.hpp"
#include "levels/gendung_defs.hpp"

namespace devilution {

struct CMonster;
struct Monster;

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

/**
 * [DARK KEEP] Bodies of monster types without a corpse id. dCorpse has 5 bits for one, and a keep lists
 * far more than 31 types (the whole lair roster), so a type the level didn't start with leaves its body
 * here instead, by level monster type: bits 0-7 type index + 1 (0: none), bits 8-10 direction, and
 * BodyWithoutMarker. dCorpse still gets CorpseByTypeMarker, so scavengers and doors see a body as before;
 * the drawing comes from here while dCorpse is set (a scavenger eating it clears dCorpse).
 */
extern uint16_t dCorpseType[MAXDUNX][MAXDUNY];
/** [DARK KEEP] The dCorpse id of a body drawn from dCorpseType. */
constexpr int8_t CorpseByTypeMarker = 0x1F;
/** [DARK KEEP] A dCorpseType body with no dCorpse marker (a slain bot hero): drawn only, the simulation never sees it. */
constexpr uint16_t BodyWithoutMarker = 1U << 11;

void InitCorpses();
void AddCorpse(Point tilePosition, int8_t dv, Direction ddir);
/** [DARK KEEP] Where a monster that has died leaves its body: its corpse id, or dCorpseType for a type without one. */
void AddMonsterCorpse(const Monster &monster);
/** [DARK KEEP] Leaves the body of a level monster type that nothing simulated died as (a bot hero, drawn as its class). */
void AddBodyOfType(Point tilePosition, size_t typeIndex, Direction ddir);
/** [DARK KEEP] The level monster type whose body lies here, if it is one drawn from dCorpseType. */
std::optional<size_t> CorpseTypeAt(Point tilePosition);
/** [DARK KEEP] Takes away whatever body lies here (a door shutting over it). */
void ClearCorpseAt(Point tilePosition);
void MoveLightsToCorpses();

} // namespace devilution
