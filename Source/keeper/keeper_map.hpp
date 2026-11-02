/**
 * @file keeper/keeper_map.hpp
 *
 * .dkmap maps: the file format (docs/MAP-FORMAT.md), turning a layout into a real level with the
 * generators' own joining rules, loading a map into the keep, and handing it to co-op joiners.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <expected.hpp>

#include "engine/displacement.hpp"
#include "engine/point.hpp"
#include "levels/gendung_defs.hpp"

namespace devilution {

struct Player;
struct TCmd;

namespace keeper {

/** What the author painted on one megatile. The file stores these values; don't renumber them. */
enum class MapCell : uint8_t {
	Rock = 0,
	Floor = 1,
	/** Solid, with a door in it: needs floor on two opposite sides and wall on the other two. */
	Door = 2,
	/** Same, with an open archway (Cathedral and Crypt only). */
	Arch = 3,
};

/** Stored in the file; don't renumber. */
enum class MapTileset : uint8_t {
	Cathedral = 0,
	Catacombs = 1,
	Caves = 2,
	Hell = 3,
	Nest = 4,
	Crypt = 5,
};
constexpr size_t NumMapTilesets = 6;

/** Stored in the file; don't renumber. */
enum class MapObjectKind : uint8_t {
	Barrel = 0,
	Chest = 1,
	/** Cathedral only: its graphics aren't loaded elsewhere. */
	Brazier = 2,
};
constexpr size_t NumMapObjectKinds = 3;
constexpr size_t MaxMapObjects = 64;

struct MapObject {
	MapObjectKind kind = MapObjectKind::Barrel;
	/** World tile. */
	Point position;
};

constexpr int MapMaxName = 40;
constexpr int MapMaxAuthor = 24;
/** A map has to have at least this many floor megatiles to play. */
constexpr int MinMapFloorCells = 150;
/** Means "not placed" for the entrance and the vault. */
constexpr Point NoMapPosition { -1, -1 };

struct KeepMap {
	std::string name;
	std::string author;
	MapTileset tileset = MapTileset::Cathedral;
	/** Top-left megatile of each staircase's footprint. */
	Point entrance = NoMapPosition;
	Point vault = NoMapPosition;
	uint32_t decorSeed = 0;
	/** The author's layout, [x][y] by megatile. Editing starts from this. */
	std::array<std::array<uint8_t, DMAXY>, DMAXX> cells {};
	std::vector<MapObject> objects;
	/** Baked when saved: the finished `dungeon`, which is what the game loads. */
	std::array<std::array<uint8_t, DMAXY>, DMAXX> tiles {};
	/** Baked when saved: dTransVal for the 2 * DMAXX by 2 * DMAXY dungeon area. */
	std::array<std::array<int8_t, 2 * DMAXY>, 2 * DMAXX> trans {};
	/** Set when saved if it passed validation. The game refuses maps without it. */
	bool playable = false;

	[[nodiscard]] MapCell cell(int x, int y) const { return static_cast<MapCell>(cells[x][y]); }
	void setCell(int x, int y, MapCell value) { cells[x][y] = static_cast<uint8_t>(value); }
};

std::string_view MapTilesetName(MapTileset tileset);
std::string_view MapObjectName(MapObjectKind kind);
/** Doors exist in this tileset's art and object lists. */
bool MapTilesetHasDoors(MapTileset tileset);
bool MapTilesetHasArches(MapTileset tileset);
bool MapObjectFitsTileset(MapObjectKind kind, MapTileset tileset);

/** Footprint of a staircase in megatiles (what it reshapes). */
Size MapStairsSize(MapTileset tileset, bool entrance);
/** True if a staircase with its top-left at `position` fits inside the paintable area. */
bool MapStairsFit(MapTileset tileset, bool entrance, Point position);
/** Paintable megatiles: the outermost ring has to stay rock, or the joining rules run off the map. */
constexpr bool IsPaintableCell(int x, int y)
{
	return x >= 1 && y >= 1 && x < DMAXX - 1 && y < DMAXY - 1;
}

// ---- files ----------------------------------------------------------------------------------

std::vector<std::byte> SerializeMap(const KeepMap &map);
/** Never trusts its input: anything wrong comes back as a message for the player. */
tl::expected<KeepMap, std::string> ParseMap(const std::byte *data, size_t size);

/** File names of the player's own maps, sorted. */
/** A file name for a map name: only letters, digits, spaces, '-' and '_' survive. */
std::string MapFileName(std::string_view name);
/** Resolves `path` as given, then under the maps folder, then under the assets folder. */
tl::expected<std::vector<std::byte>, std::string> ReadMapBytes(const std::string &path);
tl::expected<void, std::string> WriteMapBytes(const std::string &path, const std::vector<std::byte> &bytes);

// ---- building a level ------------------------------------------------------------------------

/**
 * Turns the layout into `dungeon`, `dTransVal` and `dPiece` on the loaded tileset (which must be the
 * map's). `decorate` also adds the random wall and floor variants, seeded from decorSeed. Clears
 * dSpecial. Doesn't touch objects: call InitObjects() afterwards.
 */
void ApplyLayoutToLevel(const KeepMap &map, bool decorate);
/** Fills in map.tiles and map.trans. Leaves the decorated level in `dungeon`: reapply afterwards. */
void BakeMap(KeepMap &map);
/** Where heroes appear for this map: the world tile the staircase's generator would use. */
Point MapSpawnTile(const KeepMap &map);

struct MapProblem {
	std::string text;
	/** Stops the map being played; otherwise just a warning. */
	bool blocking = true;
};
/** Checks against the level as it stands, so ApplyLayoutToLevel + InitObjects must have run. */
std::vector<MapProblem> ValidateMap(const KeepMap &map);

/** The editor's map is built live instead of from a file while this is set. */
void SetLiveMap(const KeepMap *map, const bool *decorate);

/** LoadGameLevel, first thing: reads the setup's map, and sets leveltype from its tileset. */
void PrepareMapLevel();
/** CreateDungeon: @return true if the level came from a map rather than the generator. */
bool BuildMapLevel();
/** InitObjects: @return true if a map placed the level's objects. */
bool InitMapObjects();
/** OnLevelLoaded: says so on screen if the map couldn't be used. */
void NoteMapLevelLoaded();
/** True if the level on screen was built from a map rather than generated. */
bool IsLevelFromMap();

/** Reloads the keep level in place (new tileset, a test run, a map that just arrived). */
void ReloadKeepLevel();
/** A game ended: the next one reads its map from disk, which may have changed. */
void ForgetMapCache();

} // namespace keeper
} // namespace devilution
