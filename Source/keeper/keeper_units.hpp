/**
 * @file keeper/keeper_units.hpp
 *
 * Unit overlays for The Dark Keep: floating hit/heal markers, health and mana bars over
 * units, and inspecting a hero (click one to see its stats and gear).
 */
#pragma once

#include <cstddef>
#include <optional>

#include "engine/point.hpp"
#include "engine/rectangle.hpp"
#include "engine/surface.hpp"

namespace devilution {

struct Monster;
struct Player;

namespace keeper {

/** scrollrt: a unit's sprite was just drawn with its bottom-left at `renderPos` (world buffer, unzoomed). */
void NoteMonsterDrawn(const Monster &monster, Point renderPos, int spriteWidth);
void NotePlayerDrawn(const Player &player, Point renderPos, int spriteWidth);

/** Every game tick: spawns floating markers for HP lost or regained since the last tick. */
void UpdateUnitMarkers();
/** New level / new game: forget remembered HP and the selection. */
void ResetUnits();

/** Health bars over hurt units and mana bars over heroes, for the units drawn this frame. */
void DrawUnitBars(const Surface &out);

/** Selects the hero under the cursor, if any. */
bool SelectHeroUnderCursor();
/** Selects a hero by id (`isPlayer`: a bot in Players[], else Monsters[]); ignored if it isn't a living hero. */
void SelectHero(bool isPlayer, size_t id);
void ClearSelectedHero();
bool HasSelectedHero();
/** The inspect panel's rectangle in UI pixels, bottom-left at (left, hudTop) (nullopt when nothing is selected). */
std::optional<Rectangle> InspectPanelRect(int left, int hudTop);
void DrawInspectPanel(const Surface &out, int left, int hudTop);

} // namespace keeper
} // namespace devilution
