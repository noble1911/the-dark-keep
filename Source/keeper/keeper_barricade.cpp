/**
 * @file keeper/keeper_barricade.cpp
 *
 * See keeper_barricade.hpp.
 */
#include "keeper/keeper_barricade.hpp"

#include <algorithm>
#include <array>

#include "keeper/keeper_game.hpp"
#include "levels/gendung.h"
#include "levels/tile_properties.hpp"
#include "objects.h"

namespace devilution::keeper {

namespace {

std::array<std::vector<Point>, MaxLocks> barricades;

/** Terrain only, not objects: the same on every client whatever has happened to a barrel. */
bool IsTerrainOpen(Point tile)
{
	return InDungeonBounds(tile) && !IsTileSolid(tile);
}

std::vector<Point> LineThrough(Point tile, Displacement step)
{
	std::vector<Point> line { tile };
	for (const int sign : { 1, -1 }) {
		for (Point at = tile + Displacement { step.deltaX * sign, step.deltaY * sign }; IsTerrainOpen(at); at += Displacement { step.deltaX * sign, step.deltaY * sign }) {
			line.push_back(at);
			if (static_cast<int>(line.size()) > MaxBarricadeWidth)
				return {};
		}
	}
	return line;
}

} // namespace

bool UsesBarricades()
{
	return IsNoneOf(leveltype, DTYPE_CATHEDRAL, DTYPE_CATACOMBS, DTYPE_CAVES, DTYPE_CRYPT);
}

std::vector<Point> BarricadeLine(Point tile)
{
	if (!IsTerrainOpen(tile))
		return {};
	std::vector<Point> across = LineThrough(tile, { 1, 0 });
	std::vector<Point> down = LineThrough(tile, { 0, 1 });
	if (across.empty())
		return down;
	if (down.empty() || across.size() <= down.size())
		return across;
	return down;
}

bool PlaceBarricade(size_t lockSlot, Point tile)
{
	if (lockSlot >= MaxLocks)
		return false;
	const std::vector<Point> line = BarricadeLine(tile);
	if (line.empty())
		return false;
	barricades[lockSlot].clear();
	for (const Point at : line) {
		// A chest or barrel already there is part of the wall.
		if (IsObjectAtPosition(at))
			continue;
		AddObject(OBJ_KEEPBARRICADE, at);
		if (IsBarricadeAt(at))
			barricades[lockSlot].push_back(at);
	}
	return !barricades[lockSlot].empty();
}

void RemoveBarricade(size_t lockSlot)
{
	if (lockSlot >= MaxLocks)
		return;
	for (const Point at : barricades[lockSlot]) {
		Object *object = FindObjectAtPosition(at);
		if (object != nullptr && object->_otype == OBJ_KEEPBARRICADE)
			object->_oDelFlag = true;
	}
	barricades[lockSlot].clear();
}

void ForgetBarricades()
{
	for (std::vector<Point> &line : barricades)
		line.clear();
}

bool IsBarricadeAt(Point tile)
{
	if (!InDungeonBounds(tile))
		return false;
	const Object *object = FindObjectAtPosition(tile);
	return object != nullptr && object->_otype == OBJ_KEEPBARRICADE && !object->_oDelFlag;
}

std::optional<size_t> BarricadeSlotAt(Point tile)
{
	for (size_t slot = 0; slot < MaxLocks; slot++) {
		if (std::find(barricades[slot].begin(), barricades[slot].end(), tile) != barricades[slot].end())
			return slot;
	}
	return std::nullopt;
}

const std::vector<Point> &BarricadeTiles(size_t lockSlot)
{
	return barricades[lockSlot];
}

} // namespace devilution::keeper
