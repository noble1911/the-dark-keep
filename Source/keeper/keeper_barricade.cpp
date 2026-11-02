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

/** Up to this many solid tiles in a row may be a free-standing pillar or thorn, not the end of the neck. */
constexpr int MaxPillarThickness = 2;
/** How far to look for the way round one. */
constexpr int MaxPillarWalk = 6;
/** How far beyond a barricade's ends to look for a way past it. Further than this it's a loop in the map, and a choice for heroes. */
constexpr int SealCheckMargin = 6;

/** Terrain only, not objects: the same on every client whatever has happened to a barrel. */
bool IsTerrainOpen(Point tile)
{
	return InDungeonBounds(tile) && !IsTileSolid(tile);
}

/** A short walk joins them: the two sides of a pillar, not of a wall between two halls. */
bool IsShortWalk(Point from, Point to)
{
	constexpr int Reach = MaxPillarWalk;
	std::array<std::array<bool, 2 * Reach + 1>, 2 * Reach + 1> seen {};
	const auto mark = [&](Point at) -> bool & { return seen[at.x - from.x + Reach][at.y - from.y + Reach]; };
	std::queue<Point> open;
	open.push(from);
	mark(from) = true;
	while (!open.empty()) {
		const Point at = open.front();
		open.pop();
		if (at == to)
			return true;
		for (const Direction dir : { Direction::South, Direction::SouthWest, Direction::West, Direction::NorthWest, Direction::North, Direction::NorthEast, Direction::East, Direction::SouthEast }) {
			const Displacement step(dir);
			const Point next = at + step;
			if (std::abs(next.x - from.x) > Reach || std::abs(next.y - from.y) > Reach || mark(next) || !IsTerrainOpen(next) || !CanStepDiagonal(at, step))
				continue;
			mark(next) = true;
			open.push(next);
		}
	}
	return false;
}

std::vector<Point> LineThrough(Point tile, Displacement step)
{
	std::vector<Point> line { tile };
	for (const int sign : { 1, -1 }) {
		const Displacement along { step.deltaX * sign, step.deltaY * sign };
		int solidRun = 0;
		Point lastOpen = tile;
		for (Point at = tile + along; InDungeonBounds(at) && solidRun <= MaxPillarThickness; at += along) {
			if (!IsTerrainOpen(at)) {
				solidRun++;
				continue;
			}
			// A pillar isn't the wall: stopping there would leave a gap round the far side of it. A wall as
			// thin as a pillar is still a wall if the far side is another hall.
			if (solidRun > 0 && !IsShortWalk(lastOpen, at))
				break;
			solidRun = 0;
			lastOpen = at;
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
	// Round the ends of the line, not across the map: another neck beyond the wall is a choice, not a leak.
	int left = tile.x;
	int right = tile.x;
	int top = tile.y;
	int bottom = tile.y;
	for (const Point at : line) {
		left = std::min(left, at.x);
		right = std::max(right, at.x);
		top = std::min(top, at.y);
		bottom = std::max(bottom, at.y);
	}
	left -= SealCheckMargin;
	right += SealCheckMargin;
	top -= SealCheckMargin;
	bottom += SealCheckMargin;
	static std::array<std::array<bool, MAXDUNY>, MAXDUNX> seen;
	for (auto &column : seen)
		column.fill(false);
	const auto mark = [&](Point at) -> bool & { return seen[at.x][at.y]; };
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
			if (next.x < left || next.x > right || next.y < top || next.y > bottom || !IsTerrainOpen(next) || mark(next) || !CanStepDiagonal(at, step))
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
	if (line.empty() || !CanPlaceBarricadeLine(line))
		return false;
	barricades[lockSlot].clear();
	for (const Point at : line) {
		// A chest already there is part of the wall (CanPlaceBarricadeLine has seen to the rest).
		if (IsObjectAtPosition(at))
			continue;
		AddObject(OBJ_KEEPBARRICADE, at);
		if (!IsBarricadeAt(at)) {
			RemoveBarricade(lockSlot); // a line with a hole in it is no barricade
			return false;
		}
		barricades[lockSlot].push_back(at);
	}
	return !barricades[lockSlot].empty();
}

bool CanPlaceBarricadeLine(const std::vector<Point> &line)
{
	int needed = 0;
	for (const Point at : line) {
		const Object *object = FindObjectAtPosition(at);
		if (object == nullptr)
			needed++;
		else if (!object->_oSolidFlag || object->IsBreakable())
			return false; // something that's already gone, or soon could be, can't be part of a wall
	}
	return needed > 0 && ActiveObjectCount + needed <= MAXOBJECTS;
}

void RemoveBarricade(size_t lockSlot)
{
	if (lockSlot >= MaxLocks)
		return;
	for (const Point at : barricades[lockSlot]) {
		Object *object = FindObjectAtPosition(at);
		if (object != nullptr && object->_otype == OBJ_KEEPBARRICADE) {
			// Gone now, not next tick: whoever removes it rebuilds the heroes' routes straight away.
			object->_oDelFlag = true;
			object->_oSolidFlag = false;
			dObject[at.x][at.y] = 0;
		}
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
