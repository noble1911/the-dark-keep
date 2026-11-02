/**
 * @file keeper/keeper_barricade.cpp
 *
 * See keeper_barricade.hpp.
 */
#include "keeper/keeper_barricade.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <queue>

#include "keeper/keeper_game.hpp"
#include "keeper/keeper_heroes.hpp"
#include "levels/gendung.h"
#include "levels/tile_properties.hpp"
#include "objects.h"

namespace devilution::keeper {

namespace {

std::array<std::vector<Point>, MaxLocks> barricades;

/** Solid tiles in a row thinner than this are a free-standing pillar or thorn, not the end of the neck. */
constexpr int MaxPillarThickness = 2;
/** How far round a barricade to look for a way past it. Beyond this it's a loop in the map, and a choice for heroes. */
constexpr int SealCheckRadius = 12;

/** Terrain only, not objects: the same on every client whatever has happened to a barrel. */
bool IsTerrainOpen(Point tile)
{
	return InDungeonBounds(tile) && !IsTileSolid(tile);
}

std::vector<Point> LineThrough(Point tile, Displacement step)
{
	std::vector<Point> line { tile };
	for (const int sign : { 1, -1 }) {
		const Displacement along { step.deltaX * sign, step.deltaY * sign };
		int solidRun = 0;
		for (Point at = tile + along; InDungeonBounds(at) && solidRun <= MaxPillarThickness; at += along) {
			// A pillar isn't the wall: stopping there would leave a gap round the far side of it.
			if (!IsTerrainOpen(at)) {
				solidRun++;
				continue;
			}
			solidRun = 0;
			line.push_back(at);
			if (static_cast<int>(line.size()) > MaxBarricadeWidth)
				return {};
		}
	}
	return line;
}

/** The line really shuts the way: no short walk from one side of it to the other, as there is round a pillar. */
bool Seals(const std::vector<Point> &line, Point tile, Displacement across)
{
	const Point start = tile + across;
	const Point goal = tile - across;
	if (!IsTerrainOpen(start) || !IsTerrainOpen(goal))
		return false; // along a wall, not across a neck
	constexpr int Reach = SealCheckRadius;
	std::array<std::array<bool, 2 * Reach + 1>, 2 * Reach + 1> seen {};
	const auto mark = [&](Point at) -> bool & { return seen[at.x - tile.x + Reach][at.y - tile.y + Reach]; };
	std::queue<Point> open;
	open.push(start);
	mark(start) = true;
	while (!open.empty()) {
		const Point at = open.front();
		open.pop();
		if (at == goal)
			return false;
		for (const Direction dir : { Direction::South, Direction::SouthWest, Direction::West, Direction::NorthWest, Direction::North, Direction::NorthEast, Direction::East, Direction::SouthEast }) {
			const Displacement step(dir);
			const Point next = at + step;
			if (std::abs(next.x - tile.x) > Reach || std::abs(next.y - tile.y) > Reach || mark(next) || !IsTerrainOpen(next) || !CanStepDiagonal(at, step))
				continue;
			if (std::find(line.begin(), line.end(), next) != line.end())
				continue;
			mark(next) = true;
			open.push(next);
		}
	}
	return true;
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
	if (!across.empty() && !Seals(across, tile, { 0, 1 }))
		across.clear();
	if (!down.empty() && !Seals(down, tile, { 1, 0 }))
		down.clear();
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
