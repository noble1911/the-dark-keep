/**
 * @file keeper/keeper_dress.cpp
 *
 * The dressing pass. See keeper_dress.hpp.
 */
#include "keeper/keeper_dress.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>

#include <fmt/format.h>

#include "engine/path.h"
#include "keeper/keeper_barricade.hpp"
#include "keeper/keeper_heroes.hpp"
#include "levels/gendung.h"
#include "levels/tile_properties.hpp"
#include "objects.h"

namespace devilution::keeper {

namespace {

/** The level's objects, at most, with its doors, when dressing is done: room under MaxLevelObjectsAtLoad. */
constexpr int MaxDressedLevelObjects = MaxLevelObjectsAtLoad - 5;
/** Stored objects, at most: room under MaxMapObjects for the author's own afterwards. */
constexpr size_t MaxDressedMapObjects = MaxMapObjects - 8;
/** Solid props keep this far (in tiles, either axis) from the heroes' route and from doors. */
constexpr int LaneClearance = 2;
/** Scenery props keep this far from each other. */
constexpr int ScenerySpacing = 4;
/** The engine's object light radii: a wall torch's, and every floor light's (candles, skull fires, braziers). */
constexpr int TorchRadius = 8;
constexpr int FloorLightRadius = 5;
/** Steps from a light that count as no light at all. */
constexpr int FarFromAnyLight = 40;
/** A stamp's room is this many megatiles across, at most, either axis: a bigger room gets a corner of it. */
constexpr int StampSpan = 10;
/** A stamp's room of this many megatiles gets each entry's count as written; bigger, up to twice as many. */
constexpr int StampNominalCells = 36;

struct ThemeSpec {
	std::string_view name;
	/** Scenery and how often each comes up. */
	std::array<std::pair<MapObjectKind, int>, 7> kinds;
	/** Tried in order for the centrepiece. */
	std::array<MapObjectKind, 2> centre;
	/** The light on open floor, where no wall will take a torch. */
	MapObjectKind floorLight;
};

constexpr MapObjectKind None = MapObjectKind::Barrel; // a blank in the tables: the dressing never places barrels

constexpr std::array<ThemeSpec, NumDressThemes> Themes { {
    { "bones", { { { MapObjectKind::SkullPile, 4 }, { MapObjectKind::CrucifiedSkeleton, 2 }, { MapObjectKind::HeadlessBody, 2 }, { MapObjectKind::ImpaledBody, 1 }, { MapObjectKind::Pod, 1 }, { MapObjectKind::MushroomPatch, 1 }, { None, 0 } } }, { MapObjectKind::CrucifiedSkeleton, None }, MapObjectKind::SkullFire },
    { "library", { { { MapObjectKind::Bookcase, 4 }, { MapObjectKind::BookStand, 3 }, { MapObjectKind::SkullPile, 1 }, { MapObjectKind::Bookshelf, 2 }, { MapObjectKind::Lectern, 1 }, { None, 0 }, { None, 0 } } }, { MapObjectKind::AngelStatue, MapObjectKind::BookStand }, MapObjectKind::Candle },
    { "chapel", { { { MapObjectKind::Banner, 3 }, { MapObjectKind::BookStand, 2 }, { MapObjectKind::Bookcase, 1 }, { MapObjectKind::SkullPile, 1 }, { MapObjectKind::Lectern, 1 }, { None, 0 }, { None, 0 } } }, { MapObjectKind::AngelStatue, MapObjectKind::BloodFountain }, MapObjectKind::Candle },
    { "armoury", { { { MapObjectKind::WeaponRack, 3 }, { MapObjectKind::ArmourStand, 3 }, { MapObjectKind::Banner, 2 }, { MapObjectKind::HeadlessBody, 1 }, { MapObjectKind::ClosedChest, 1 }, { None, 0 }, { None, 0 } } }, { MapObjectKind::Banner, None }, MapObjectKind::Candle },
    { "torture", { { { MapObjectKind::TorturedBody, 3 }, { MapObjectKind::ImpaledBody, 3 }, { MapObjectKind::HeadlessBody, 2 }, { MapObjectKind::SkullPile, 1 }, { MapObjectKind::HangingBody, 1 }, { None, 0 }, { None, 0 } } }, { MapObjectKind::BloodFountain, None }, MapObjectKind::SkullFire },
    { "prison", { { { MapObjectKind::ImpaledBody, 3 }, { MapObjectKind::SkullPile, 2 }, { MapObjectKind::TorturedBody, 2 }, { MapObjectKind::CrucifiedSkeleton, 1 }, { MapObjectKind::HangingBody, 1 }, { None, 0 }, { None, 0 } } }, { MapObjectKind::CrucifiedSkeleton, None }, MapObjectKind::SkullFire },
    { "tomb", { { { MapObjectKind::SkullPile, 3 }, { MapObjectKind::Banner, 2 }, { MapObjectKind::CrucifiedSkeleton, 2 }, { MapObjectKind::HeadlessBody, 1 }, { MapObjectKind::BookStand, 1 }, { MapObjectKind::Urn, 1 }, { MapObjectKind::ClosedChest, 1 } } }, { MapObjectKind::AngelStatue, MapObjectKind::CrucifiedSkeleton }, MapObjectKind::Candle },
    { "hell", { { { MapObjectKind::ImpaledBody, 3 }, { MapObjectKind::TorturedBody, 2 }, { MapObjectKind::HeadlessBody, 2 }, { MapObjectKind::SkullPile, 2 }, { MapObjectKind::CrucifiedSkeleton, 1 }, { MapObjectKind::HangingBody, 1 }, { None, 0 } } }, { MapObjectKind::BloodFountain, None }, MapObjectKind::SkullFire },
} };

/** The dressing's own numbers: never the game's generator, which co-op keeps in step. */
struct DressRandom {
	uint32_t state;
	uint32_t next()
	{
		state = state * 1664525U + 1013904223U;
		return state >> 8;
	}
	int below(int n)
	{
		return n <= 1 ? 0 : static_cast<int>(next() % static_cast<uint32_t>(n));
	}
};

using TileGrid = std::array<std::array<bool, MAXDUNY>, MAXDUNX>;
using TileCounts = std::array<std::array<uint16_t, MAXDUNY>, MAXDUNX>;

/**
 * What a prop must not change: where heroes appear, their route, what a lock on each door does, the way round
 * a neck shut along the route, and where barricades go on it.
 */
struct RouteMeasure {
	Point arrival;
	uint16_t route = Unreachable;
	std::vector<uint16_t> lockedRoutes;
	std::vector<uint16_t> shutRoutes;
	std::vector<std::vector<Point>> barricades;
	bool operator==(const RouteMeasure &other) const
	{
		return arrival == other.arrival && route == other.route && lockedRoutes == other.lockedRoutes && shutRoutes == other.shutRoutes && barricades == other.barricades;
	}
};

struct Light {
	Point lit;
	uint8_t radius;
};

std::optional<Light> LightOf(const Object &object)
{
	switch (object._otype) {
	// On a wall the camera sees, shining on the floor in front (+x, +y).
	case OBJ_TORCHL2:
		return Light { object.position + Displacement { 1, 0 }, 8 };
	case OBJ_TORCHR2:
		return Light { object.position + Displacement { 0, 1 }, 8 };
	// From the floor in front of a near wall, which is where they stand.
	case OBJ_TORCHL:
	case OBJ_TORCHR:
		return Light { object.position, 8 };
	case OBJ_L1LIGHT:
	case OBJ_CANDLE2:
	case OBJ_SKFIRE:
	case OBJ_BOOKCANDLE:
	case OBJ_STORYCANDLE:
		return Light { object.position, 5 };
	case OBJ_L5CANDLE:
		return Light { object.position, 3 };
	case OBJ_BCROSS:
	case OBJ_TBCROSS:
		return Light { object.position, 5 };
	default:
		return std::nullopt;
	}
}

Point CellOf(Point tile)
{
	return { (tile.x - 16) / 2, (tile.y - 16) / 2 };
}

bool InMapArea(Point tile)
{
	return tile.x >= 16 && tile.y >= 16 && tile.x < 16 + 2 * DMAXX && tile.y < 16 + 2 * DMAXY;
}

using RoomGrid = std::array<std::array<int, DMAXY>, DMAXX>;

/** [x][y] by megatile: which room (some 3 by 3 block of floor) a cell is part of, 0 for none; and each room's size. */
void MarkRoomsOf(const KeepMap &map, RoomGrid &rooms, std::vector<int> &sizes)
{
	for (auto &column : rooms)
		column.fill(0);
	std::array<std::array<bool, DMAXY>, DMAXX> inBlock {};
	for (int x = 0; x + 2 < DMAXX; x++) {
		for (int y = 0; y + 2 < DMAXY; y++) {
			bool all = true;
			for (int dx = 0; dx < 3 && all; dx++) {
				for (int dy = 0; dy < 3 && all; dy++)
					all = map.cell(x + dx, y + dy) == MapCell::Floor;
			}
			if (!all)
				continue;
			for (int dx = 0; dx < 3; dx++) {
				for (int dy = 0; dy < 3; dy++)
					inBlock[x + dx][y + dy] = true;
			}
		}
	}
	sizes.assign(1, 0);
	for (int x = 0; x < DMAXX; x++) {
		for (int y = 0; y < DMAXY; y++) {
			if (!inBlock[x][y] || rooms[x][y] != 0)
				continue;
			const int id = static_cast<int>(sizes.size());
			sizes.push_back(0);
			std::vector<Point> stack { { x, y } };
			rooms[x][y] = id;
			while (!stack.empty()) {
				const Point cell = stack.back();
				stack.pop_back();
				sizes[id]++;
				for (const Displacement step : { Displacement { 1, 0 }, Displacement { -1, 0 }, Displacement { 0, 1 }, Displacement { 0, -1 } }) {
					const Point next = cell + step;
					if (next.x < 0 || next.y < 0 || next.x >= DMAXX || next.y >= DMAXY || !inBlock[next.x][next.y] || rooms[next.x][next.y] != 0)
						continue;
					rooms[next.x][next.y] = id;
					stack.push_back(next);
				}
			}
		}
	}
}

/**
 * The cells of room `room` in a window at most `span` across round `at`: the whole room if it fits, otherwise
 * slid along each axis to the far wall (the one the camera sees) or else the near one, keeping `at` inside, so
 * it has walls to stand by.
 */
std::vector<Point> RoomWindow(const RoomGrid &rooms, int room, Point at, int span)
{
	std::vector<Point> cells;
	if (room == 0)
		return cells;
	Point low { DMAXX, DMAXY };
	Point high { -1, -1 };
	for (int x = 0; x < DMAXX; x++) {
		for (int y = 0; y < DMAXY; y++) {
			if (rooms[x][y] == room) {
				low = { std::min(low.x, x), std::min(low.y, y) };
				high = { std::max(high.x, x), std::max(high.y, y) };
			}
		}
	}
	const auto window = [span](int click, int first, int last) {
		if (last - first + 1 <= span)
			return std::pair { first, last };
		// To the far wall if the click is near enough: the walls the camera sees, which take the wall props and the
		// bookcases. Else to the near wall, else round the click.
		if (click - first < span)
			return std::pair { first, first + span - 1 };
		if (last - click < span)
			return std::pair { last - span + 1, last };
		return std::pair { click - span / 2, click - span / 2 + span - 1 };
	};
	const auto [left, right] = window(at.x, low.x, high.x);
	const auto [top, bottom] = window(at.y, low.y, high.y);
	const auto inWindow = [&, left = left, right = right, top = top, bottom = bottom](Point cell) {
		return cell.x >= left && cell.x <= right && cell.y >= top && cell.y <= bottom && rooms[cell.x][cell.y] == room;
	};
	// Only what joins `at` inside the window (or the room's cell nearest it): not the hall beyond a thin wall,
	// which the room reaches the long way round.
	Point start = NoMapPosition;
	for (int x = left; x <= right; x++) {
		for (int y = top; y <= bottom; y++) {
			if (inWindow({ x, y }) && (start == NoMapPosition || Point { x, y }.WalkingDistance(at) < start.WalkingDistance(at)))
				start = { x, y };
		}
	}
	if (start == NoMapPosition)
		return cells;
	std::array<std::array<bool, DMAXY>, DMAXX> seen {};
	std::vector<Point> stack { start };
	seen[start.x][start.y] = true;
	while (!stack.empty()) {
		const Point cell = stack.back();
		stack.pop_back();
		cells.push_back(cell);
		for (const Displacement step : { Displacement { 1, 0 }, Displacement { -1, 0 }, Displacement { 0, 1 }, Displacement { 0, -1 } }) {
			const Point next = cell + step;
			if (inWindow(next) && !seen[next.x][next.y]) {
				seen[next.x][next.y] = true;
				stack.push_back(next);
			}
		}
	}
	// In order, so a stamp's seed and spots don't depend on the walk.
	std::sort(cells.begin(), cells.end(), [](Point a, Point b) { return a.x != b.x ? a.x < b.x : a.y < b.y; });
	return cells;
}

class Dresser {
public:
	Dresser(KeepMap &map, const DressOptions &options)
	    : map_(map)
	    , options_(options)
	    , random_ { (map.decorSeed ^ (options.seed * 2654435761U)) | 1U }
	{
		themeIds_ = options.themes;
		if (themeIds_.empty())
			themeIds_.push_back(DefaultDressTheme(map.tileset));
		for (const DressTheme theme : themeIds_)
			themes_.push_back(&Themes[static_cast<size_t>(theme)]);
		const std::vector<MapObjectKind> palette = MapObjectPalette(map.tileset);
		for (const MapObjectKind kind : palette)
			available_[static_cast<size_t>(kind)] = true;
	}

	DressResult run()
	{
		if (!Prepare())
			return result_;
		if (options_.themedRooms > 0)
			StampThemedRooms();
		PlaceLights();
		PlaceCentrepiece();
		PlaceScenery();
		Reinit();
		return result_;
	}

	/** Lays the level out, measures the heroes' way and marks the rooms and lanes. False (with a note) if there's no way to measure. */
	bool Prepare()
	{
		Reinit();
		InitMapTriggers(map_.tileset);
		vault_ = FindVault(NoMapPosition);
		if (vault_ == NoMapPosition) {
			result_.notes.emplace_back("no vault stairs: nothing dressed");
			return false;
		}
		const std::optional<RouteMeasure> measured = Measure();
		if (!measured || measured->route == Unreachable) {
			result_.notes.push_back(fmt::format("no route from the entrance ({}) to the vault at {},{}: nothing dressed", measured ? fmt::format("{},{} route {}", measured->arrival.x, measured->arrival.y, measured->route) : "nowhere to appear", vault_.x, vault_.y));
			return false;
		}
		baseline_ = *measured;
		MarkRooms();
		MarkLanes();
		result_.lightsBefore = CountLevelLights();
		// What the author (or an earlier pass) already put in counts: dressing twice adds nothing more.
		for (const MapObject &object : map_.objects) {
			if (IsLegacyMapObject(object.kind))
				continue;
			if (GetMapObjectTraits(object.kind).placement == MapObjectPlacement::OnWall)
				wallProps_.push_back(object.position);
			else
				scenery_.push_back(object.position);
			if (GetMapObjectTraits(object.kind).light)
				continue;
			sceneryBefore_++;
			for (const MapObjectKind centre : themes_.front()->centre)
				hasCentrepiece_ = hasCentrepiece_ || object.kind == centre;
		}
		return true;
	}

	/** For a stamp placed by hand: the caps are what a map holds, not the dressing's. */
	void SetCaps(size_t mapObjects, int levelObjects)
	{
		maxMapObjects_ = mapObjects;
		maxLevelObjects_ = levelObjects;
	}

	const std::vector<std::string> &Notes() const { return result_.notes; }

	StampResult Stamp(const RoomStamp &stamp, const std::vector<Point> &cells, uint32_t seed);

	/** The level again with everything placed, as DressMap leaves it. */
	void Finish() const { Reinit(); }

private:
	KeepMap &map_;
	const DressOptions &options_;
	DressRandom random_;
	std::vector<const ThemeSpec *> themes_;
	std::vector<DressTheme> themeIds_;
	std::array<bool, NumMapObjectKinds> available_ {};
	DressResult result_;
	Point vault_ = NoMapPosition;
	RouteMeasure baseline_;
	/** [x][y] by megatile: part of a room (some 3 by 3 block of floor), and which one. */
	RoomGrid room_ {};
	std::vector<int> roomSizes_;
	/** Tiles solid props keep off: near the route or a door. */
	TileGrid lane_ {};
	/** Tiles near the route, for lighting the fights. */
	TileGrid nearRoute_ {};
	TileGrid nearDoor_ {};
	std::vector<Point> scenery_;
	std::vector<Point> wallProps_;
	int sceneryBefore_ = 0;
	bool hasCentrepiece_ = false;
	size_t maxMapObjects_ = MaxDressedMapObjects;
	int maxLevelObjects_ = MaxDressedLevelObjects;
	/** Rooms a theme-room stamp furnished: the scattered scenery and the centrepiece leave them be. */
	std::vector<bool> stampedRoom_;

	/**
	 * The level again, with map.objects as they are now. The layout too, not just the objects: placing a
	 * door changes the tile under it (the Catacombs' do), so InitObjects twice on one layout finds no doors.
	 */
	void Reinit() const
	{
		ApplyLayoutToLevel(map_, options_.decorated);
		std::memset(dObject, 0, sizeof(dObject));
		InitObjects();
	}

	std::optional<RouteMeasure> Measure() const
	{
		const Point spawn = MapSpawnTile(map_);
		const std::optional<Point> arrival = FindClosestValidPosition(IsOpenFloor, spawn, 0, HeroSpawnRadius);
		if (!arrival)
			return std::nullopt;
		RouteMeasure measure;
		measure.arrival = *arrival;
		static DistanceField field;
		BuildField(field, vault_);
		measure.route = field[arrival->x][arrival->y];
		for (int i = 0; i < ActiveObjectCount; i++) {
			const Object &object = Objects[ActiveObjects[i]];
			if (!object.isDoor())
				continue;
			static DistanceField locked;
			const Point door = object.position;
			BuildFieldAvoiding(locked, vault_, [door](Point tile) { return tile == door; });
			measure.lockedRoutes.push_back(locked[arrival->x][arrival->y]);
		}
		if (measure.route == Unreachable)
			return measure;
		std::vector<Point> route;
		for (Point at = *arrival; at != vault_;) {
			route.push_back(at);
			const std::optional<Point> next = Downhill(field, at);
			if (!next)
				break;
			at = *next;
		}
		// The way round a 9 by 9 square shut at points along the route, as dkmap_tool measures necks.
		for (size_t i = 6; i + 6 < route.size(); i += 6) {
			static DistanceField shut;
			const Point middle = route[i];
			BuildFieldAvoiding(shut, vault_, [middle](Point tile) { return std::abs(tile.x - middle.x) <= 4 && std::abs(tile.y - middle.y) <= 4; });
			measure.shutRoutes.push_back(shut[arrival->x][arrival->y]);
		}
		if (UsesBarricades()) {
			// Where a barricade would go along the route, and whether it could: a prop on its line would stop it.
			for (size_t i = 0; i < route.size(); i += 3) {
				std::vector<Point> line = BarricadeLine(route[i]);
				if (!line.empty() && !CanPlaceBarricadeLine(line))
					line.clear();
				measure.barricades.push_back(std::move(line));
			}
		}
		return measure;
	}

	/** The way a hero walks: the first step downhill, in dkmap_tool's order. */
	static std::optional<Point> Downhill(const DistanceField &field, Point at)
	{
		constexpr std::array<Direction, 8> Order { Direction::South, Direction::SouthWest, Direction::West, Direction::NorthWest, Direction::North, Direction::NorthEast, Direction::East, Direction::SouthEast };
		const uint16_t here = field[at.x][at.y];
		for (const Direction dir : Order) {
			const Displacement step(dir);
			const Point neighbour = at + step;
			if (!InDungeonBounds(neighbour) || field[neighbour.x][neighbour.y] >= here || !CanStepDiagonal(at, step))
				continue;
			return neighbour;
		}
		return std::nullopt;
	}

	void MarkRooms()
	{
		MarkRoomsOf(map_, room_, roomSizes_);
		stampedRoom_.assign(roomSizes_.size(), false);
	}

	int RoomOf(Point tile) const
	{
		if (!InMapArea(tile))
			return 0;
		const Point cell = CellOf(tile);
		return room_[cell.x][cell.y];
	}

	static void Dilate(const std::vector<Point> &tiles, int radius, TileGrid &out)
	{
		for (const Point tile : tiles) {
			for (int dx = -radius; dx <= radius; dx++) {
				for (int dy = -radius; dy <= radius; dy++) {
					const Point at = tile + Displacement { dx, dy };
					if (InDungeonBounds(at))
						out[at.x][at.y] = true;
				}
			}
		}
	}

	void MarkLanes()
	{
		static DistanceField field;
		BuildField(field, vault_);
		std::vector<Point> route;
		for (Point at = baseline_.arrival; at != vault_;) {
			route.push_back(at);
			const std::optional<Point> next = Downhill(field, at);
			if (!next)
				break;
			at = *next;
		}
		route.push_back(vault_);
		std::vector<Point> doors;
		for (int i = 0; i < ActiveObjectCount; i++) {
			const Object &object = Objects[ActiveObjects[i]];
			if (object.isDoor())
				doors.push_back(object.position);
		}
		Dilate(route, LaneClearance, lane_);
		Dilate(doors, LaneClearance, lane_);
		Dilate(route, 6, nearRoute_);
		Dilate(doors, 5, nearDoor_);
	}

	/**
	 * The tileset has it, and the level can load its art: a kind the map doesn't use yet mustn't take it past
	 * the engine's 40 object sprite files (the dressing and the stamps use fewer kinds instead).
	 */
	bool Available(MapObjectKind kind) const
	{
		if (kind == None || !available_[static_cast<size_t>(kind)])
			return false;
		const bool used = std::any_of(map_.objects.begin(), map_.objects.end(), [&](const MapObject &object) { return object.kind == kind; });
		return used || MapObjectFileCount(map_, kind) <= MaxLevelObjectFiles;
	}

	static bool HugsWall(Point tile)
	{
		for (int dx = -1; dx <= 1; dx++) {
			for (int dy = -1; dy <= 1; dy++) {
				if ((dx != 0 || dy != 0) && IsTileSolid(tile + Displacement { dx, dy }))
					return true;
			}
		}
		return false;
	}

	static bool IsDoorAt(Point tile)
	{
		const Object *object = FindObjectAtPosition(tile);
		return object != nullptr && object->isDoor();
	}

	static bool IsBare(Point tile)
	{
		return InDungeonBounds(tile) && IsTileNotSolid(tile) && dObject[tile.x][tile.y] == 0;
	}

	bool AtCap() const
	{
		return map_.objects.size() >= maxMapObjects_ || ActiveObjectCount >= maxLevelObjects_;
	}

	/** Adds it if the rules and the route check allow, and says so. */
	bool Place(const MapObject &object)
	{
		if (AtCap())
			return false;
		if (WhyMapObjectCantStand(map_, object))
			return false;
		const size_t leftOutBefore = LeftOutMapObjects().size();
		map_.objects.push_back(object);
		Reinit();
		bool ok = LeftOutMapObjects().size() == leftOutBefore;
		bool routeChanged = false;
		if (ok) {
			// Measured for every prop: a wall prop's floor tile can still stand on a barricade's line.
			const std::optional<RouteMeasure> now = Measure();
			routeChanged = !now || !(*now == baseline_);
			ok = !routeChanged;
		}
		if (!ok) {
			map_.objects.pop_back();
			Reinit();
			if (routeChanged)
				result_.rejectedForRoute++;
		}
		return ok;
	}

	/** The floor tile a placed prop is seen from. */
	static Point FrontOf(const MapObject &object)
	{
		if (GetMapObjectTraits(object.kind).placement != MapObjectPlacement::OnWall)
			return object.position;
		return object.position + (object.facesY() ? Displacement { 0, 1 } : Displacement { 1, 0 });
	}

	/** Every way a wall prop of this kind could go on the level now: the floor in front of a straight wall the camera sees. */
	std::vector<MapObject> WallSpots(MapObjectKind kind) const
	{
		std::vector<MapObject> spots;
		for (int y = 16; y < 16 + 2 * DMAXY; y++) {
			for (int x = 16; x < 16 + 2 * DMAXX; x++) {
				const Point floor { x, y };
				if (!IsBare(floor))
					continue;
				for (const bool facesY : { false, true }) {
					const Point wall = floor - (facesY ? Displacement { 0, 1 } : Displacement { 1, 0 });
					const MapObject spot { kind, wall, static_cast<uint8_t>(facesY ? MapObjectFacesY : 0) };
					if (!WhyMapObjectCantStand(map_, spot))
						spots.push_back(spot);
				}
			}
		}
		return spots;
	}

	/**
	 * Walking distance, through anything not solid, from the nearest light's pool (the stairs' included). A
	 * small light's pool is smaller, so its distance starts further out: a torch (radius 8) starts at 0, a
	 * candle or brazier (radius 5) at 3. Radius 5 lights only a tile or two in the keep's dark.
	 */
	void LightDistances(TileCounts &distance) const
	{
		for (auto &column : distance)
			column.fill(Unreachable);
		std::array<std::vector<Point>, TorchRadius + 1> starts;
		const auto seed = [&](Point tile, int radius) {
			if (InDungeonBounds(tile))
				starts[TorchRadius - std::clamp(radius, 0, TorchRadius)].push_back(tile);
		};
		seed(MapSpawnTile(map_), 8);
		seed(vault_, 6);
		for (int i = 0; i < ActiveObjectCount; i++) {
			if (const std::optional<Light> light = LightOf(Objects[ActiveObjects[i]]); light)
				seed(light->lit, light->radius);
		}
		std::vector<Point> frontier;
		for (uint16_t d = 0; !frontier.empty() || d < starts.size(); d++) {
			std::vector<Point> next;
			if (d < starts.size()) {
				for (const Point tile : starts[d]) {
					if (distance[tile.x][tile.y] > d) {
						distance[tile.x][tile.y] = d;
						frontier.push_back(tile);
					}
				}
			}
			for (const Point from : frontier) {
				for (int dx = -1; dx <= 1; dx++) {
					for (int dy = -1; dy <= 1; dy++) {
						const Point to = from + Displacement { dx, dy };
						// Through doors too: a closed door's tile is solid, and a room behind one is no less in need of light.
						if (!InDungeonBounds(to) || distance[to.x][to.y] <= d + 1 || (IsTileSolid(to) && !IsDoorAt(to)))
							continue;
						distance[to.x][to.y] = d + 1;
						next.push_back(to);
					}
				}
			}
			frontier = std::move(next);
		}
	}

	/** Lights, counted as torches: a small one is half of one. */
	static int TorchHalves()
	{
		int halves = 0;
		for (int i = 0; i < ActiveObjectCount; i++) {
			if (const std::optional<Light> light = LightOf(Objects[ActiveObjects[i]]); light)
				halves += light->radius >= TorchRadius ? 2 : 1;
		}
		return halves;
	}

	void PlaceLights()
	{
		const MapObjectKind floorLight = Available(themes_.front()->floorLight) ? themes_.front()->floorLight : MapObjectKind::Candle;
		std::vector<MapObject> candidates;
		if (Available(MapObjectKind::WallTorch))
			candidates = WallSpots(MapObjectKind::WallTorch);
		if (Available(floorLight)) {
			for (int y = 16; y < 16 + 2 * DMAXY; y++) {
				for (int x = 16; x < 16 + 2 * DMAXX; x++) {
					const Point tile { x, y };
					// Anywhere in a room off the lanes: the middle of a court no wall torch reaches is where it's needed.
					if (IsBare(tile) && RoomOf(tile) != 0 && !lane_[x][y])
						candidates.push_back({ floorLight, tile, 0 });
				}
			}
		}
		std::vector<bool> dead(candidates.size());
		int spacing = options_.lightSpacing;
		static TileCounts distance;
		while (TorchHalves() < 2 * options_.maxLights) {
			LightDistances(distance);
			int best = -1;
			int bestScore = 0;
			for (size_t i = 0; i < candidates.size(); i++) {
				if (dead[i])
					continue;
				const Point lit = FrontOf(candidates[i]);
				const bool torch = candidates[i].kind == MapObjectKind::WallTorch;
				// Not crowding a body or torch already on the wall.
				if (torch && !FarFromWallProps(candidates[i].position))
					continue;
				// No light reaches there at all: as dark as it gets.
				const int far = std::min<int>(distance[lit.x][lit.y], FarFromAnyLight);
				// A floor light's pool is smaller, so it wants more dark round it.
				const int d = far - (torch ? 0 : TorchRadius - FloorLightRadius);
				if (d < spacing)
					continue;
				// The biggest dark first; torches well before floor lights, which light little more than
				// themselves; and where the fights will be.
				const int score = 4 * d + (torch ? 40 : 0) + (nearRoute_[lit.x][lit.y] ? 10 : 0) + (nearDoor_[lit.x][lit.y] ? 6 : 0) + (!torch && HugsWall(lit) ? 4 : 0) + random_.below(4);
				if (score > bestScore) {
					bestScore = score;
					best = static_cast<int>(i);
				}
			}
			if (best < 0) {
				if (TorchHalves() < 2 * options_.minLights && spacing > 7) {
					spacing -= 2;
					continue;
				}
				break;
			}
			dead[best] = true;
			if (Place(candidates[best])) {
				result_.lightsAdded++;
				if (GetMapObjectTraits(candidates[best].kind).placement == MapObjectPlacement::OnWall)
					wallProps_.push_back(candidates[best].position);
				else
					scenery_.push_back(candidates[best].position);
			}
		}
	}

	bool FarFromScenery(Point tile) const
	{
		return std::none_of(scenery_.begin(), scenery_.end(), [&](Point other) { return std::max(std::abs(other.x - tile.x), std::abs(other.y - tile.y)) < ScenerySpacing; });
	}

	bool FarFromWallProps(Point tile) const
	{
		return std::none_of(wallProps_.begin(), wallProps_.end(), [&](Point other) { return std::max(std::abs(other.x - tile.x), std::abs(other.y - tile.y)) < 3; });
	}

	uint8_t RandomStyle(MapObjectKind kind)
	{
		return static_cast<uint8_t>(random_.below(GetMapObjectTraits(kind).styles));
	}

	/** The biggest room whose floor heroes don't cross, else the biggest: a fountain or statue in the middle of it. */
	void PlaceCentrepiece()
	{
		if (!options_.centrepiece || hasCentrepiece_ || roomSizes_.size() <= 1 || sceneryBefore_ >= options_.maxScenery)
			return;
		std::optional<MapObjectKind> kind;
		for (const MapObjectKind centre : themes_.front()->centre) {
			if (Available(centre)) {
				kind = centre;
				break;
			}
		}
		if (!kind)
			return;
		// Rooms by size, the biggest first; ties to the lower id, so it is the same every time.
		std::vector<int> rooms;
		for (int id = 1; id < static_cast<int>(roomSizes_.size()); id++)
			rooms.push_back(id);
		std::stable_sort(rooms.begin(), rooms.end(), [&](int a, int b) { return roomSizes_[a] > roomSizes_[b]; });
		for (const int room : rooms) {
			if (roomSizes_[room] < 12)
				break;
			if (stampedRoom_[room])
				continue;
			// The middle of the room's floor.
			long sumX = 0;
			long sumY = 0;
			int count = 0;
			for (int x = 0; x < DMAXX; x++) {
				for (int y = 0; y < DMAXY; y++) {
					if (room_[x][y] == room) {
						sumX += 16 + 2 * x;
						sumY += 16 + 2 * y;
						count++;
					}
				}
			}
			const Point middle { static_cast<int>(sumX / count) + 1, static_cast<int>(sumY / count) + 1 };
			std::vector<Point> tiles;
			for (int x = 16; x < 16 + 2 * DMAXX; x++) {
				for (int y = 16; y < 16 + 2 * DMAXY; y++) {
					const Point tile { x, y };
					if (RoomOf(tile) == room && IsBare(tile) && !lane_[x][y] && !HugsWall(tile) && tile.WalkingDistance(middle) <= 4)
						tiles.push_back(tile);
				}
			}
			std::stable_sort(tiles.begin(), tiles.end(), [&](Point a, Point b) { return a.WalkingDistance(middle) < b.WalkingDistance(middle); });
			for (const Point tile : tiles) {
				if (Place({ *kind, tile, RandomStyle(*kind) })) {
					scenery_.push_back(tile);
					result_.sceneryAdded++;
					return;
				}
			}
		}
	}

	MapObjectKind PickKind()
	{
		std::vector<std::pair<MapObjectKind, int>> weights;
		for (size_t t = 0; t < themes_.size(); t++) {
			for (const auto &[kind, weight] : themes_[t]->kinds) {
				if (!Available(kind) || weight == 0)
					continue;
				// A second theme comes up half as often as the first.
				weights.emplace_back(kind, t == 0 ? 2 * weight : weight);
			}
		}
		int total = 0;
		for (const auto &entry : weights)
			total += entry.second;
		if (total == 0)
			return None;
		int roll = random_.below(total);
		for (const auto &[kind, weight] : weights) {
			if (roll < weight)
				return kind;
			roll -= weight;
		}
		return weights.back().first;
	}

	/** Spots in `room` for a kind: against the walls, off the lanes, apart from the rest. */
	std::vector<MapObject> ScenerySpots(MapObjectKind kind, int room)
	{
		std::vector<MapObject> spots;
		const MapObjectPlacement placement = GetMapObjectTraits(kind).placement;
		if (placement == MapObjectPlacement::OnWall) {
			for (const MapObject &spot : WallSpots(kind)) {
				const Point front = FrontOf(spot);
				if (RoomOf(front) == room && FarFromScenery(front) && FarFromWallProps(spot.position))
					spots.push_back({ kind, spot.position, static_cast<uint8_t>(spot.variant | RandomStyle(kind)) });
			}
			return spots;
		}
		for (int y = 16; y < 16 + 2 * DMAXY; y++) {
			for (int x = 16; x < 16 + 2 * DMAXX; x++) {
				const Point tile { x, y };
				if (RoomOf(tile) != room || !IsBare(tile) || lane_[x][y] || !HugsWall(tile) || !FarFromScenery(tile))
					continue;
				if (placement == MapObjectPlacement::AgainstWall) {
					if (const std::optional<MapObject> snapped = SnapMapObject(kind, tile, RandomStyle(kind)); snapped && snapped->position == tile)
						spots.push_back(*snapped);
				} else {
					spots.push_back({ kind, tile, RandomStyle(kind) });
				}
			}
		}
		return spots;
	}

	/**
	 * The biggest rooms get a stamp each, for the themes (or the level's stamps) in turn, round the middle of the
	 * room. A room much bigger than a stamp's window (a hall, Hell's caverns) can take more than one, in corners of
	 * it far apart, once every room has had its first.
	 */
	void StampThemedRooms()
	{
		std::vector<int> rooms;
		for (int id = 1; id < static_cast<int>(roomSizes_.size()); id++)
			rooms.push_back(id);
		std::stable_sort(rooms.begin(), rooms.end(), [&](int a, int b) { return roomSizes_[a] > roomSizes_[b]; });
		struct Site {
			int room;
			Point at;
		};
		std::vector<Site> sites;
		std::vector<Site> extra;
		constexpr int CellsPerExtraSite = 150;
		constexpr int ExtraSitesApart = 8;
		for (const int room : rooms) {
			if (roomSizes_[room] < 12)
				break;
			long sumX = 0;
			long sumY = 0;
			for (int x = 0; x < DMAXX; x++) {
				for (int y = 0; y < DMAXY; y++) {
					if (room_[x][y] == room) {
						sumX += x;
						sumY += y;
					}
				}
			}
			const Point middle { static_cast<int>(sumX / roomSizes_[room]), static_cast<int>(sumY / roomSizes_[room]) };
			sites.push_back({ room, middle });
			// More sites in a big room: each the cell furthest from the room's sites so far, if far enough.
			std::vector<Point> taken { middle };
			for (int more = roomSizes_[room] / CellsPerExtraSite - 1; more > 0; more--) {
				std::optional<Point> best;
				int bestDistance = ExtraSitesApart - 1;
				for (int x = 0; x < DMAXX; x++) {
					for (int y = 0; y < DMAXY; y++) {
						if (room_[x][y] != room)
							continue;
						int nearest = 1000;
						for (const Point other : taken)
							nearest = std::min(nearest, std::max(std::abs(other.x - x), std::abs(other.y - y)));
						if (nearest > bestDistance) {
							bestDistance = nearest;
							best = Point { x, y };
						}
					}
				}
				if (!best)
					break;
				taken.push_back(*best);
				extra.push_back({ room, *best });
			}
		}
		sites.insert(sites.end(), extra.begin(), extra.end());
		int done = 0;
		for (const Site &site : sites) {
			if (done >= options_.themedRooms)
				break;
			const RoomStamp *stamp = nullptr;
			if (!options_.roomStamps.empty()) {
				// As the level asks, in turn: past the end of the list, round again.
				for (size_t s = 0; s < options_.roomStamps.size() && stamp == nullptr; s++) {
					const RoomStamp *named = RoomStampNamed(options_.roomStamps[(done + s) % options_.roomStamps.size()]);
					if (named != nullptr && RoomStampSuits(*named, map_.tileset))
						stamp = named;
				}
			}
			for (size_t t = 0; t < themeIds_.size() && stamp == nullptr; t++)
				stamp = RoomStampForTheme(themeIds_[(done + t) % themeIds_.size()], map_.tileset);
			if (stamp == nullptr)
				break;
			const std::vector<Point> cells = RoomWindow(room_, site.room, site.at, StampSpan);
			const StampResult stamped = Stamp(*stamp, cells, options_.seed + static_cast<uint32_t>(done) * 7919U);
			// A site the stamp couldn't furnish (a hall too narrow for it) doesn't use up a room: the next tries.
			if (stamped.placed > 0) {
				done++;
				// The room gets no scattered scenery if the stamp took most of it; a corner of a hall leaves the rest.
				if (3 * static_cast<int>(cells.size()) >= 2 * roomSizes_[site.room])
					stampedRoom_[site.room] = true;
				result_.stampedRooms++;
				result_.stampedProps += stamped.placed;
				int sumX = 0;
				int sumY = 0;
				for (const MapObject &object : stamped.added) {
					sumX += object.position.x;
					sumY += object.position.y;
				}
				const int count = static_cast<int>(stamped.added.size());
				result_.stampedSites.emplace_back(std::string(stamp->name), Point { sumX / count, sumY / count });
			}
			if (stamped.capped) {
				result_.notes.push_back(fmt::format("the object cap stopped the {} stamp", stamp->name));
				break;
			}
		}
	}

	/** The kinds an entry names that this build, the tileset and the art budget allow, in its order, with any style asked for. */
	std::vector<std::pair<MapObjectKind, std::optional<uint8_t>>> ResolveKinds(std::string_view names, StampResult &out) const
	{
		std::vector<std::pair<MapObjectKind, std::optional<uint8_t>>> kinds;
		while (!names.empty()) {
			const size_t bar = names.find('|');
			std::string_view name = names.substr(0, bar);
			names = bar == std::string_view::npos ? std::string_view() : names.substr(bar + 1);
			while (!name.empty() && name.front() == ' ')
				name.remove_prefix(1);
			while (!name.empty() && name.back() == ' ')
				name.remove_suffix(1);
			std::optional<uint8_t> style;
			if (const size_t slash = name.find('/'); slash != std::string_view::npos) {
				int number = 0;
				for (const char c : name.substr(slash + 1))
					number = std::isdigit(static_cast<unsigned char>(c)) != 0 ? number * 10 + (c - '0') : 255;
				name = name.substr(0, slash);
				if (number < 255)
					style = static_cast<uint8_t>(number);
			}
			if (name.empty())
				continue;
			const std::optional<MapObjectKind> kind = MapObjectKindNamed(name);
			if (!kind) {
				if (std::find(out.unknownNames.begin(), out.unknownNames.end(), name) == out.unknownNames.end())
					out.unknownNames.emplace_back(name);
				continue;
			}
			// Never the barrel, the chest or the brazier: those are the keep's own, and aren't judged as props.
			if (style && *style >= GetMapObjectTraits(*kind).styles)
				style.reset();
			const std::pair<MapObjectKind, std::optional<uint8_t>> choice { *kind, style };
			if (!IsLegacyMapObject(*kind) && Available(*kind) && std::find(kinds.begin(), kinds.end(), choice) == kinds.end())
				kinds.push_back(choice);
		}
		return kinds;
	}

	/** Where a stamp's prop of this kind could go in its area, before spacing: spots that pass WhyMapObjectCantStand. */
	std::vector<MapObject> StampSpots(MapObjectKind kind, StampSpot where, const TileGrid &area, Point centre) const
	{
		std::vector<MapObject> spots;
		const MapObjectTraits traits = GetMapObjectTraits(kind);
		for (int y = 16; y < 16 + 2 * DMAXY; y++) {
			for (int x = 16; x < 16 + 2 * DMAXX; x++) {
				const Point tile { x, y };
				if (!area[x][y] || !IsBare(tile))
					continue;
				if (traits.placement == MapObjectPlacement::OnWall) {
					// The floor is in the room; the wall the camera sees is behind it.
					for (const bool facesY : { false, true }) {
						const Point wall = tile - (facesY ? Displacement { 0, 1 } : Displacement { 1, 0 });
						const MapObject spot { kind, wall, static_cast<uint8_t>(facesY ? MapObjectFacesY : 0) };
						if (!WhyMapObjectCantStand(map_, spot))
							spots.push_back(spot);
					}
					continue;
				}
				if (traits.solid && lane_[x][y])
					continue;
				if (traits.placement == MapObjectPlacement::AgainstWall) {
					if (const std::optional<MapObject> snapped = SnapMapObject(kind, tile, 0); snapped && snapped->position == tile && !WhyMapObjectCantStand(map_, *snapped))
						spots.push_back(*snapped);
					continue;
				}
				const int fromCentre = std::max(std::abs(x - centre.x), std::abs(y - centre.y));
				switch (where) {
				case StampSpot::Edge:
					if (!HugsWall(tile))
						continue;
					break;
				case StampSpot::Centre:
					if (HugsWall(tile))
						continue;
					break;
				case StampSpot::Ring:
					if (HugsWall(tile) || fromCentre < 2 || fromCentre > 3)
						continue;
					break;
				case StampSpot::Scatter:
					break;
				}
				// The rules now, so the choice is among spots that can be: by an uneven wall (the Nest's) few can.
				if (const MapObject spot { kind, tile, 0 }; !WhyMapObjectCantStand(map_, spot))
					spots.push_back(spot);
			}
		}
		// No wall in reach (the middle of a cavern): a floor kind goes anywhere in it instead.
		if (spots.empty() && where == StampSpot::Edge && traits.placement == MapObjectPlacement::Floor)
			return StampSpots(kind, StampSpot::Scatter, area, centre);
		return spots;
	}

	void PlaceScenery()
	{
		std::vector<int> rooms;
		for (int id = 1; id < static_cast<int>(roomSizes_.size()); id++)
			rooms.push_back(id);
		std::stable_sort(rooms.begin(), rooms.end(), [&](int a, int b) { return roomSizes_[a] > roomSizes_[b]; });
		// Each room its share: one per 30 floor cells or so (a cavern that is all one room gets several), at
		// least one, round and round the rooms so the small ones get theirs before the big ones get more.
		constexpr int MostPerRoom = 12;
		std::vector<int> wanted(roomSizes_.size());
		for (const int room : rooms)
			wanted[room] = stampedRoom_[room] ? 0 : std::clamp(roomSizes_[room] / 30, 1, MostPerRoom);
		for (int round = 0; round < MostPerRoom; round++) {
			for (const int room : rooms) {
				if (sceneryBefore_ + result_.sceneryAdded >= options_.maxScenery)
					return;
				if (wanted[room] <= round)
					continue;
				for (int attempt = 0; attempt < 4; attempt++) {
					const MapObjectKind kind = PickKind();
					if (kind == None)
						return;
					const std::vector<MapObject> spots = ScenerySpots(kind, room);
					if (spots.empty())
						continue;
					const MapObject &spot = spots[random_.below(static_cast<int>(spots.size()))];
					if (Place(spot)) {
						scenery_.push_back(FrontOf(spot));
						if (GetMapObjectTraits(kind).placement == MapObjectPlacement::OnWall)
							wallProps_.push_back(spot.position);
						result_.sceneryAdded++;
						break;
					}
				}
			}
		}
	}
};

int Chebyshev(Point a, Point b)
{
	return std::max(std::abs(a.x - b.x), std::abs(a.y - b.y));
}

bool AllApart(const std::vector<Point> &others, Point tile, int distance)
{
	return std::none_of(others.begin(), others.end(), [&](Point other) { return Chebyshev(other, tile) < distance; });
}

int NearestOf(const std::vector<Point> &others, Point tile, int cap)
{
	int nearest = cap;
	for (const Point other : others)
		nearest = std::min(nearest, Chebyshev(other, tile));
	return nearest;
}

StampResult Dresser::Stamp(const RoomStamp &stamp, const std::vector<Point> &cells, uint32_t seed)
{
	StampResult out;
	const int routeRejectsBefore = result_.rejectedForRoute;
	static TileGrid area;
	for (auto &column : area)
		column.fill(false);
	long sumX = 0;
	long sumY = 0;
	int tiles = 0;
	// The room goes into the seed: the same cells, in any order, stamp the same.
	uint32_t roomHash = 2166136261U;
	std::vector<Point> sorted = cells;
	std::sort(sorted.begin(), sorted.end(), [](Point a, Point b) { return a.x != b.x ? a.x < b.x : a.y < b.y; });
	sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
	int cellCount = 0;
	for (const Point cell : sorted) {
		if (cell.x < 0 || cell.y < 0 || cell.x >= DMAXX || cell.y >= DMAXY)
			continue;
		cellCount++;
		roomHash = (roomHash ^ static_cast<uint32_t>(cell.x * 64 + cell.y)) * 16777619U;
		for (int dx = 0; dx < 2; dx++) {
			for (int dy = 0; dy < 2; dy++) {
				const Point tile = cell.megaToWorld() + Displacement { dx, dy };
				area[tile.x][tile.y] = true;
				sumX += tile.x;
				sumY += tile.y;
				tiles++;
			}
		}
	}
	if (tiles == 0) {
		out.notes.emplace_back("no room there");
		return out;
	}
	const Point middle { static_cast<int>(sumX / tiles), static_cast<int>(sumY / tiles) };
	// And the open floor round it: in some tilesets (the Catacombs) a wall is one tile thick, and the floor along
	// it lies in the rock's megatile.
	static TileGrid grown;
	grown = area;
	for (int x = 17; x < 16 + 2 * DMAXX - 1; x++) {
		for (int y = 17; y < 16 + 2 * DMAXY - 1; y++) {
			if (!area[x][y])
				continue;
			for (int dx = -1; dx <= 1; dx++) {
				for (int dy = -1; dy <= 1; dy++) {
					if (IsTileNotSolid({ x + dx, y + dy }))
						grown[x + dx][y + dy] = true;
				}
			}
		}
	}
	area = grown;
	DressRandom random { (map_.decorSeed ^ (seed * 2654435761U) ^ roomHash) | 1U };
	const int scale = std::clamp(cellCount * 100 / StampNominalCells, 100, 200);
	std::optional<Point> centre;
	// The stamp's own floor props, with the floor in front of its wall props; and its wall props.
	std::vector<Point> floorTaken;
	std::vector<Point> wallTaken;
	for (const RoomStampEntry &entry : stamp.entries) {
		const auto kinds = ResolveKinds(entry.kinds, out);
		if (kinds.empty()) {
			out.entriesWithoutKind++;
			continue;
		}
		const int low = std::min(entry.min, entry.max);
		int count = low + random.below(entry.max - low + 1);
		if (entry.spot == StampSpot::Edge || entry.spot == StampSpot::Scatter)
			count = (count * scale + 50) / 100;
		if (entry.spot == StampSpot::Centre && centre)
			count = 0; // one centrepiece a stamp
		out.wanted += count;
		const int apart = entry.apart != 0 ? entry.apart : stamp.spacing;
		std::vector<Point> mine;
		for (int i = 0; i < count && !out.capped; i++) {
			if (AtCap()) {
				out.capped = true;
				break;
			}
			std::vector<Point> refused;
			bool placed = false;
			for (int attempt = 0; attempt < 8 && !placed; attempt++) {
				std::optional<MapObject> best;
				std::optional<uint8_t> bestStyle;
				int bestScore = 0;
				for (const auto &[kind, style] : kinds) {
					const MapObjectTraits traits = GetMapObjectTraits(kind);
					const bool onWall = traits.placement == MapObjectPlacement::OnWall;
					for (const MapObject &spot : StampSpots(kind, entry.spot, area, centre.value_or(middle))) {
						if (std::find(refused.begin(), refused.end(), spot.position) != refused.end())
							continue;
						const Point front = FrontOf(spot);
						if (onWall) {
							// Not crowding another on the wall, and a light further off one.
							if (!AllApart(wallTaken, spot.position, 2) || !AllApart(wallProps_, spot.position, traits.light ? 3 : 2) || !AllApart(mine, spot.position, apart))
								continue;
						} else if (!AllApart(floorTaken, spot.position, stamp.spacing) || !AllApart(mine, spot.position, apart) || !AllApart(scenery_, spot.position, 2) || !AllApart(wallProps_, spot.position, 2)) {
							continue;
						}
						int score = 0;
						switch (entry.spot) {
						case StampSpot::Centre:
							score = 1000 - 4 * front.WalkingDistance(middle) + random.below(3);
							break;
						case StampSpot::Edge:
							// Spread round the room: the furthest from the entry's others.
							score = 1 + 4 * NearestOf(mine, front, 10) + random.below(8);
							break;
						case StampSpot::Ring:
							score = 1 + random.below(100);
							break;
						case StampSpot::Scatter:
							score = 1 + 2 * NearestOf(floorTaken, front, 6) + random.below(10);
							break;
						}
						if (score > bestScore) {
							bestScore = score;
							best = spot;
							bestStyle = style;
						}
					}
					if (best)
						break; // the first kind on the line with somewhere to go
				}
				if (!best)
					break;
				MapObject object = *best;
				const int rolled = random.below(GetMapObjectTraits(object.kind).styles);
				object.variant = static_cast<uint8_t>((object.variant & MapObjectFacesY) | bestStyle.value_or(static_cast<uint8_t>(rolled)));
				if (AtCap()) {
					out.capped = true;
					break;
				}
				if (!Place(object)) {
					refused.push_back(object.position);
					continue;
				}
				placed = true;
				const MapObjectTraits traits = GetMapObjectTraits(object.kind);
				const Point front = FrontOf(object);
				out.added.push_back(object);
				out.placed++;
				if (traits.light)
					out.lights++;
				mine.push_back(traits.placement == MapObjectPlacement::OnWall ? object.position : front);
				floorTaken.push_back(front);
				scenery_.push_back(front);
				if (traits.placement == MapObjectPlacement::OnWall) {
					wallTaken.push_back(object.position);
					wallProps_.push_back(object.position);
				}
				if (entry.spot == StampSpot::Centre && !centre)
					centre = object.position;
			}
			if (!placed)
				break; // nowhere left for this entry
		}
		if (out.capped)
			break;
	}
	out.rejectedForRoute = result_.rejectedForRoute - routeRejectsBefore;
	return out;
}

constexpr uint8_t TilesetBit(MapTileset tileset)
{
	return static_cast<uint8_t>(1U << static_cast<unsigned>(tileset));
}

constexpr uint8_t InCathedral = TilesetBit(MapTileset::Cathedral);
constexpr uint8_t InCatacombs = TilesetBit(MapTileset::Catacombs);
constexpr uint8_t InCaves = TilesetBit(MapTileset::Caves);
constexpr uint8_t InHell = TilesetBit(MapTileset::Hell);
constexpr uint8_t InNest = TilesetBit(MapTileset::Nest);
constexpr uint8_t InCrypt = TilesetBit(MapTileset::Crypt);
constexpr uint8_t InAll = InCathedral | InCatacombs | InCaves | InHell | InNest | InCrypt;

/**
 * The stamps, by prop id (MapObjectId) or name, '|' between choices: the first that this build, the tileset and
 * the level's art budget allow, with somewhere to go, is used. The cauldron and the empty pedestal aren't asked
 * for: in the keep's dark their art reads as a black blob. `id/N` asks for style N (closed_chest/2 is the
 * large chest, magic_circle/1 the red pentagram); without one the style is picked by the seed. Names that aren't
 * kinds are skipped. Entries go in order, so the centrepiece comes first where the ring round it needs it.
 */
std::vector<RoomStamp> MakeRoomStamps()
{
	using S = StampSpot;
	// clang-format off
	return {
		{ "library", "Library corner", InCathedral | InCatacombs | InCrypt, {
			{ "bookcase|bookshelf", S::Edge, 2, 4 },
			{ "bookshelf|bookcase", S::Edge, 1, 2, 3 },
			{ "lectern/1|book_stand", S::Centre, 1, 1 },
			{ "crypt_book_stand|story_book|book_stand", S::Edge, 0, 2, 4 },
			{ "crypt_candles|candles", S::Edge, 1, 2, 5 },
			{ "wall_torch", S::Edge, 0, 1, 6 },
			{ "urn", S::Scatter, 0, 1 },
		} },
		{ "shrine", "Shrine alcove", InCathedral | InCatacombs | InCaves | InCrypt, {
			{ "shrine|altar|angel_statue|blood_fountain", S::Centre, 1, 1 },
			{ "crypt_candles|candles|skull_fire", S::Ring, 2, 2, 3 },
			{ "pedestal/2|pedestal/1", S::Edge, 1, 2, 3 },
			{ "large_fountain|goat_shrine|tear_fountain", S::Scatter, 0, 1, 5 },
			{ "banner", S::Edge, 0, 1, 3 },
			{ "wall_torch", S::Edge, 1, 1, 6 },
		} },
		{ "chapel", "Chapel", InCathedral | InCatacombs | InCrypt, {
			{ "altar|angel_statue", S::Centre, 1, 1 },
			{ "crypt_candles|candles", S::Ring, 2, 2, 3 },
			{ "banner", S::Edge, 1, 2, 4 },
			{ "lectern|book_stand", S::Edge, 1, 1 },
			{ "pedestal/1|shrine", S::Edge, 0, 1, 4 },
			{ "magic_circle/0|magic_circle/2", S::Ring, 0, 1, 4 },
			{ "tear_fountain|angel_statue", S::Edge, 0, 1, 5 },
			{ "wall_torch", S::Edge, 1, 1, 6 },
		} },
		{ "crypt", "Crypt", InCathedral | InCatacombs | InCrypt, {
			{ "crypt_sarcophagus|sarcophagus|crucified_skeleton", S::Centre, 1, 1 },
			{ "crypt_sarcophagus|sarcophagus|skull_pile", S::Edge, 1, 2, 3 },
			{ "urn|skull_pile", S::Edge, 1, 3 },
			{ "crypt_book_stand|story_book", S::Edge, 0, 1, 4 },
			{ "closed_chest", S::Edge, 0, 1, 4 },
			{ "crucified_skeleton", S::Edge, 0, 1, 3 },
			{ "headless_body", S::Scatter, 0, 1 },
			{ "crypt_candles|candles", S::Edge, 1, 2, 4 },
		} },
		{ "torture", "Torture room", InCathedral | InCatacombs | InCaves | InHell | InCrypt, {
			{ "blood_fountain|hanging_body", S::Centre, 1, 1 },
			{ "impaled_body", S::Edge, 2, 3, 3 },
			{ "hanging_body|tortured_body", S::Edge, 1, 2, 3 },
			{ "tortured_body", S::Edge, 1, 1, 3 },
			{ "lever/1|lever", S::Edge, 0, 1, 4 },
			{ "headless_body", S::Scatter, 1, 2 },
			{ "wall_torch|skull_fire", S::Edge, 1, 1, 6 },
		} },
		{ "prison", "Prison cells", InCathedral | InCatacombs | InCaves | InHell | InCrypt, {
			{ "impaled_body", S::Edge, 2, 3, 3 },
			{ "hanging_body|tortured_body", S::Edge, 1, 1, 3 },
			{ "tortured_body", S::Edge, 1, 2, 3 },
			{ "lever", S::Edge, 0, 1, 4 },
			{ "skull_pile", S::Edge, 1, 1 },
			{ "headless_body", S::Scatter, 1, 1 },
			{ "wall_torch|skull_fire", S::Edge, 1, 1, 6 },
		} },
		{ "armoury", "Armoury", InCatacombs | InCaves | InHell | InCrypt, {
			{ "weapon_rack", S::Edge, 2, 3, 3 },
			{ "armour_stand", S::Edge, 2, 3, 3 },
			{ "banner", S::Edge, 0, 1 },
			{ "closed_chest/2|closed_chest", S::Centre, 0, 1 },
			{ "wall_torch|candles", S::Edge, 1, 2, 6 },
		} },
		{ "treasure", "Treasure room", InCathedral | InCatacombs | InCaves | InHell | InCrypt, {
			{ "closed_chest/2|angel_statue|blood_fountain", S::Centre, 1, 1 },
			{ "closed_chest/1|closed_chest/0", S::Edge, 2, 3, 3 },
			{ "pedestal/3|pedestal/2", S::Edge, 1, 2, 3 },
			{ "staff_stand|rock_stand", S::Edge, 0, 1, 4 },
			{ "urn|armour_stand", S::Scatter, 0, 1 },
			{ "banner", S::Edge, 0, 1, 3 },
			{ "crypt_candles|candles", S::Ring, 2, 2, 3 },
			{ "wall_torch", S::Edge, 1, 1, 6 },
		} },
		{ "altar", "Hell altar", InHell | InNest, {
			{ "altar|blood_fountain", S::Centre, 1, 1 },
			{ "magic_circle/1|magic_circle/3", S::Ring, 0, 1, 4 },
			{ "skull_fire", S::Ring, 2, 3, 3 },
			{ "burning_cross", S::Edge, 0, 1, 6 },
			{ "skull_switch|goat_shrine", S::Edge, 0, 1, 4 },
			{ "crucified_skeleton", S::Edge, 1, 2, 3 },
			{ "impaled_body", S::Edge, 0, 1, 3 },
			{ "tortured_body", S::Edge, 0, 1, 3 },
			{ "skull_pile|pod", S::Scatter, 1, 2 },
		} },
		{ "bones", "Bone room", InAll, {
			{ "skull_pile", S::Centre, 1, 1 },
			{ "skull_pile", S::Edge, 2, 3 },
			{ "crucified_skeleton", S::Edge, 1, 2, 3 },
			{ "headless_body", S::Scatter, 2, 3 },
			{ "pod|mushroom_patch", S::Scatter, 0, 1, 4 },
			{ "impaled_body", S::Edge, 0, 1 },
			{ "skull_fire", S::Edge, 1, 1, 5 },
		} },
	};
	// clang-format on
}

} // namespace

const std::vector<RoomStamp> &RoomStamps()
{
	static const std::vector<RoomStamp> stamps = MakeRoomStamps();
	return stamps;
}

const RoomStamp *RoomStampNamed(std::string_view name)
{
	for (const RoomStamp &stamp : RoomStamps()) {
		if (stamp.name == name)
			return &stamp;
	}
	return nullptr;
}

bool RoomStampSuits(const RoomStamp &stamp, MapTileset tileset)
{
	return (stamp.tilesets & TilesetBit(tileset)) != 0;
}

std::vector<const RoomStamp *> RoomStampsFor(MapTileset tileset)
{
	std::vector<const RoomStamp *> stamps;
	for (const RoomStamp &stamp : RoomStamps()) {
		if (RoomStampSuits(stamp, tileset))
			stamps.push_back(&stamp);
	}
	return stamps;
}

const RoomStamp *RoomStampForTheme(DressTheme theme, MapTileset tileset)
{
	constexpr std::array<std::string_view, NumDressThemes> ByTheme { "bones", "library", "chapel", "armoury", "torture", "prison", "crypt", "altar" };
	const RoomStamp *stamp = RoomStampNamed(ByTheme[static_cast<size_t>(theme)]);
	return stamp != nullptr && RoomStampSuits(*stamp, tileset) ? stamp : nullptr;
}

std::vector<Point> RoomCellsAt(const KeepMap &map, Point cell)
{
	if (cell.x < 0 || cell.y < 0 || cell.x >= DMAXX || cell.y >= DMAXY)
		return {};
	static RoomGrid rooms;
	std::vector<int> sizes;
	MarkRoomsOf(map, rooms, sizes);
	return RoomWindow(rooms, rooms[cell.x][cell.y], cell, StampSpan);
}

std::vector<Point> FloorCellsIn(const KeepMap &map, Point a, Point b)
{
	std::vector<Point> cells;
	for (int x = std::max(std::min(a.x, b.x), 0); x <= std::min(std::max(a.x, b.x), DMAXX - 1); x++) {
		for (int y = std::max(std::min(a.y, b.y), 0); y <= std::min(std::max(a.y, b.y), DMAXY - 1); y++) {
			if (map.cell(x, y) == MapCell::Floor)
				cells.push_back({ x, y });
		}
	}
	return cells;
}

StampResult StampRoom(KeepMap &map, const RoomStamp &stamp, const std::vector<Point> &cells, const StampOptions &options)
{
	DressOptions dress;
	dress.decorated = options.decorated;
	Dresser dresser(map, dress);
	if (!dresser.Prepare()) {
		StampResult result;
		for (std::string note : dresser.Notes()) {
			if (const size_t at = note.find("nothing dressed"); at != std::string::npos)
				note.replace(at, 15, "nothing stamped");
			result.notes.push_back(std::move(note));
		}
		return result;
	}
	dresser.SetCaps(options.maxMapObjects, options.maxLevelObjects);
	StampResult result = dresser.Stamp(stamp, cells, options.seed);
	dresser.Finish();
	return result;
}

std::string_view DressThemeName(DressTheme theme)
{
	return Themes[static_cast<size_t>(theme)].name;
}

std::optional<DressTheme> DressThemeNamed(std::string_view name)
{
	for (size_t i = 0; i < NumDressThemes; i++) {
		if (Themes[i].name == name)
			return static_cast<DressTheme>(i);
	}
	return std::nullopt;
}

DressTheme DefaultDressTheme(MapTileset tileset)
{
	switch (tileset) {
	case MapTileset::Catacombs:
		return DressTheme::Tomb;
	case MapTileset::Caves:
		return DressTheme::Prison;
	case MapTileset::Hell:
		return DressTheme::Hell;
	case MapTileset::Crypt:
		return DressTheme::Tomb;
	case MapTileset::Nest:
		return DressTheme::Bones;
	case MapTileset::Cathedral:
	default:
		return DressTheme::Chapel;
	}
}

int CountLevelLights()
{
	int count = 0;
	for (int i = 0; i < ActiveObjectCount; i++) {
		if (LightOf(Objects[ActiveObjects[i]]))
			count++;
	}
	return count;
}

DressResult DressMap(KeepMap &map, const DressOptions &options)
{
	Dresser dresser(map, options);
	return dresser.run();
}

} // namespace devilution::keeper
