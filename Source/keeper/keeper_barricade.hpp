/**
 * @file keeper/keeper_barricade.hpp
 *
 * Barricades: what the Lock tool makes where the tileset has no door at all (Hell, the Nest). A line of
 * solid barriers across a neck, wall to wall, which counts as one locked door for heroes: the route
 * charges for going through it, they bash it, and it comes down all at once. Every client works out the
 * same line from the same terrain, so only the tile clicked travels.
 */
#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "engine/point.hpp"

namespace devilution::keeper {

/** Tiles: a neck wider than this can't be barred, so halls stay open and necks are worth finding. */
constexpr int MaxBarricadeWidth = 12;

/** The current level's tileset has no doors, so the Lock tool barricades instead. */
bool UsesBarricades();
/** The line across the narrowest way through `tile`, wall to wall. Empty if it's wider than the limit. */
std::vector<Point> BarricadeLine(Point tile);
/** @return false if something stands in the way. Keeps the line for this lock slot. */
bool PlaceBarricade(size_t lockSlot, Point tile);
void RemoveBarricade(size_t lockSlot);
/** A new level: the objects have gone with the old one. */
void ForgetBarricades();
bool IsBarricadeAt(Point tile);
/** The lock slot of the barricade with a barrier on `tile`. */
std::optional<size_t> BarricadeSlotAt(Point tile);
/** The tiles of this lock's barricade, none if it's on a door. */
const std::vector<Point> &BarricadeTiles(size_t lockSlot);

} // namespace devilution::keeper
