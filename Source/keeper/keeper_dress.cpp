/**
 * @file keeper/keeper_dress.cpp
 *
 * The dressing pass. See keeper_dress.hpp.
 */
#include "keeper/keeper_dress.hpp"

#include <algorithm>
#include <array>
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

struct ThemeSpec {
	std::string_view name;
	/** Scenery and how often each comes up. */
	std::array<std::pair<MapObjectKind, int>, 5> kinds;
	/** Tried in order for the centrepiece. */
	std::array<MapObjectKind, 2> centre;
	/** The light on open floor, where no wall will take a torch. */
	MapObjectKind floorLight;
};

constexpr MapObjectKind None = MapObjectKind::Barrel; // a blank in the tables: the dressing never places barrels

constexpr std::array<ThemeSpec, NumDressThemes> Themes { {
    { "bones", { { { MapObjectKind::SkullPile, 4 }, { MapObjectKind::CrucifiedSkeleton, 2 }, { MapObjectKind::HeadlessBody, 2 }, { MapObjectKind::ImpaledBody, 1 }, { None, 0 } } }, { MapObjectKind::CrucifiedSkeleton, None }, MapObjectKind::SkullFire },
    { "library", { { { MapObjectKind::Bookcase, 4 }, { MapObjectKind::BookStand, 3 }, { MapObjectKind::SkullPile, 1 }, { None, 0 }, { None, 0 } } }, { MapObjectKind::AngelStatue, MapObjectKind::BookStand }, MapObjectKind::Candle },
    { "chapel", { { { MapObjectKind::Banner, 3 }, { MapObjectKind::BookStand, 2 }, { MapObjectKind::Bookcase, 1 }, { MapObjectKind::SkullPile, 1 }, { None, 0 } } }, { MapObjectKind::AngelStatue, MapObjectKind::BloodFountain }, MapObjectKind::Candle },
    { "armoury", { { { MapObjectKind::WeaponRack, 3 }, { MapObjectKind::ArmourStand, 3 }, { MapObjectKind::Banner, 2 }, { MapObjectKind::HeadlessBody, 1 }, { None, 0 } } }, { MapObjectKind::Banner, None }, MapObjectKind::Candle },
    { "torture", { { { MapObjectKind::TorturedBody, 3 }, { MapObjectKind::ImpaledBody, 3 }, { MapObjectKind::HeadlessBody, 2 }, { MapObjectKind::SkullPile, 1 }, { None, 0 } } }, { MapObjectKind::BloodFountain, None }, MapObjectKind::SkullFire },
    { "prison", { { { MapObjectKind::ImpaledBody, 3 }, { MapObjectKind::SkullPile, 2 }, { MapObjectKind::TorturedBody, 2 }, { MapObjectKind::CrucifiedSkeleton, 1 }, { None, 0 } } }, { MapObjectKind::CrucifiedSkeleton, None }, MapObjectKind::SkullFire },
    { "tomb", { { { MapObjectKind::SkullPile, 3 }, { MapObjectKind::Banner, 2 }, { MapObjectKind::CrucifiedSkeleton, 2 }, { MapObjectKind::HeadlessBody, 1 }, { MapObjectKind::BookStand, 1 } } }, { MapObjectKind::AngelStatue, MapObjectKind::CrucifiedSkeleton }, MapObjectKind::Candle },
    { "hell", { { { MapObjectKind::ImpaledBody, 3 }, { MapObjectKind::TorturedBody, 2 }, { MapObjectKind::HeadlessBody, 2 }, { MapObjectKind::SkullPile, 2 }, { MapObjectKind::CrucifiedSkeleton, 1 } } }, { MapObjectKind::BloodFountain, None }, MapObjectKind::SkullFire },
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
	case OBJ_TORCHL:
		return Light { object.position + Displacement { 1, 0 }, 8 };
	case OBJ_TORCHR:
		return Light { object.position + Displacement { 0, 1 }, 8 };
	case OBJ_TORCHL2:
	case OBJ_TORCHR2:
		return Light { object.position, 8 };
	case OBJ_L1LIGHT:
	case OBJ_CANDLE2:
	case OBJ_SKFIRE:
	case OBJ_BOOKCANDLE:
	case OBJ_STORYCANDLE:
		return Light { object.position, 5 };
	case OBJ_L5CANDLE:
		return Light { object.position, 3 };
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

class Dresser {
public:
	Dresser(KeepMap &map, const DressOptions &options)
	    : map_(map)
	    , options_(options)
	    , random_ { (map.decorSeed ^ (options.seed * 2654435761U)) | 1U }
	{
		for (const DressTheme theme : options.themes)
			themes_.push_back(&Themes[static_cast<size_t>(theme)]);
		if (themes_.empty())
			themes_.push_back(&Themes[static_cast<size_t>(DefaultDressTheme(map.tileset))]);
		const std::vector<MapObjectKind> palette = MapObjectPalette(map.tileset);
		for (const MapObjectKind kind : palette)
			available_[static_cast<size_t>(kind)] = true;
	}

	DressResult run()
	{
		Reinit();
		InitMapTriggers(map_.tileset);
		vault_ = FindVault(NoMapPosition);
		if (vault_ == NoMapPosition) {
			result_.notes.emplace_back("no vault stairs: nothing dressed");
			return result_;
		}
		const std::optional<RouteMeasure> measured = Measure();
		if (!measured || measured->route == Unreachable) {
			result_.notes.push_back(fmt::format("no route from the entrance ({}) to the vault at {},{}: nothing dressed", measured ? fmt::format("{},{} route {}", measured->arrival.x, measured->arrival.y, measured->route) : "nowhere to appear", vault_.x, vault_.y));
			return result_;
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
		PlaceLights();
		PlaceCentrepiece();
		PlaceScenery();
		Reinit();
		return result_;
	}

private:
	KeepMap &map_;
	const DressOptions &options_;
	DressRandom random_;
	std::vector<const ThemeSpec *> themes_;
	std::array<bool, NumMapObjectKinds> available_ {};
	DressResult result_;
	Point vault_ = NoMapPosition;
	RouteMeasure baseline_;
	/** [x][y] by megatile: part of a room (some 3 by 3 block of floor), and which one. */
	std::array<std::array<int, DMAXY>, DMAXX> room_ {};
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
		std::array<std::array<bool, DMAXY>, DMAXX> inBlock {};
		for (int x = 0; x + 2 < DMAXX; x++) {
			for (int y = 0; y + 2 < DMAXY; y++) {
				bool all = true;
				for (int dx = 0; dx < 3 && all; dx++) {
					for (int dy = 0; dy < 3 && all; dy++)
						all = map_.cell(x + dx, y + dy) == MapCell::Floor;
				}
				if (!all)
					continue;
				for (int dx = 0; dx < 3; dx++) {
					for (int dy = 0; dy < 3; dy++)
						inBlock[x + dx][y + dy] = true;
				}
			}
		}
		roomSizes_.assign(1, 0);
		for (int x = 0; x < DMAXX; x++) {
			for (int y = 0; y < DMAXY; y++) {
				if (!inBlock[x][y] || room_[x][y] != 0)
					continue;
				const int id = static_cast<int>(roomSizes_.size());
				roomSizes_.push_back(0);
				std::vector<Point> stack { { x, y } };
				room_[x][y] = id;
				while (!stack.empty()) {
					const Point cell = stack.back();
					stack.pop_back();
					roomSizes_[id]++;
					for (const Displacement step : { Displacement { 1, 0 }, Displacement { -1, 0 }, Displacement { 0, 1 }, Displacement { 0, -1 } }) {
						const Point next = cell + step;
						if (next.x < 0 || next.y < 0 || next.x >= DMAXX || next.y >= DMAXY || !inBlock[next.x][next.y] || room_[next.x][next.y] != 0)
							continue;
						room_[next.x][next.y] = id;
						stack.push_back(next);
					}
				}
			}
		}
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

	bool Available(MapObjectKind kind) const
	{
		return kind != None && available_[static_cast<size_t>(kind)];
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

	/** Adds it if the rules and the route check allow, and says so. */
	bool Place(const MapObject &object)
	{
		if (map_.objects.size() >= MaxDressedMapObjects || ActiveObjectCount >= MaxDressedLevelObjects)
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
			wanted[room] = std::clamp(roomSizes_[room] / 30, 1, MostPerRoom);
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

} // namespace

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
