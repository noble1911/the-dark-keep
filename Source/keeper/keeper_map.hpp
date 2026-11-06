/**
 * @file keeper/keeper_map.hpp
 *
 * .dkmap maps: the file format (docs/MAP-FORMAT.md), turning a layout into a real level with the
 * generators' own joining rules, loading a map into the keep, and handing it to co-op joiners.
 */
#pragma once

#include <array>
#include <optional>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <expected.hpp>

#include "engine/displacement.hpp"
#include "engine/point.hpp"
#include "levels/gendung_defs.hpp"

namespace devilution {

struct Object;
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

/**
 * Stored in the file; don't renumber. Version 1 files hold only the first three. The rest are props
 * (version 2 to AngelStatue, version 3 after): scenery that never does anything. See docs/MAP-FORMAT.md.
 */
enum class MapObjectKind : uint8_t {
	Barrel = 0,
	Chest = 1,
	/** Cathedral only. */
	Brazier = 2,
	Candle = 3,
	SkullFire = 4,
	/** On a wall, facing the floor in front of it. */
	WallTorch = 5,
	Banner = 6,
	SkullPile = 7,
	CrucifiedSkeleton = 8,
	BookStand = 9,
	/** On the floor with its back to a wall. */
	Bookcase = 10,
	WeaponRack = 11,
	ArmourStand = 12,
	HeadlessBody = 13,
	TorturedBody = 14,
	/** On a wall, facing the floor in front of it. */
	ImpaledBody = 15,
	BloodFountain = 16,
	/** Crypt only. */
	CryptCandle = 17,
	AngelStatue = 18,
	// Version 3: more scenery. Every one is as inert as the props above, whatever its vanilla object does.
	/** Small, medium and large, shut: OBJ_CHEST1 to 3. Not the Chest above, which opens. */
	ClosedChest = 19,
	/** Two tiles: the stored one and the one at -y. */
	Sarcophagus = 20,
	CryptSarcophagus = 21,
	/** On the floor with its back to a wall. */
	Shrine = 22,
	GoatShrine = 23,
	/** Four tiles: the stored one and those at -x, -y and both. */
	LargeFountain = 24,
	TearFountain = 25,
	Pedestal = 26,
	Lectern = 27,
	StoryBook = 28,
	Lever = 29,
	SkullSwitch = 30,
	CryptLever = 31,
	CryptBookStand = 32,
	Altar = 33,
	StaffStand = 34,
	RockStand = 35,
	/** Painted on the floor: walked over. */
	MagicCircle = 36,
	BurningCross = 37,
	HangingBody = 38,
	/** On the floor with its back to a wall. */
	Bookshelf = 39,
	/** Four tiles, as the large fountain. */
	MushroomPatch = 40,
	Urn = 41,
	Pod = 42,
	Cauldron = 43,
};
constexpr size_t NumMapObjectKinds = 44;
/** Kinds 0 to this minus 1 are all a version 2 file can hold. */
constexpr size_t NumMapObjectKindsV2 = 19;
/** Barrel, chest and brazier: all a version 1 file can hold, placed exactly as they always were. */
constexpr size_t NumLegacyMapObjectKinds = 3;
constexpr size_t MaxMapObjectsV1 = 64;
/** Version 2 and 3 alike. */
constexpr size_t MaxMapObjects = 80;
/**
 * Doors and stored objects together, when the level is built: the rest of MAXOBJECTS (127) is for what the
 * game adds while it runs, Barricades above all. Props past this are left out.
 */
constexpr int MaxLevelObjectsAtLoad = 95;
/** A map travels in the join packet, whose frame limit is 64 KiB including its headers. */
constexpr size_t MaxMapBytes = 60000;

/** Where a kind stands. */
enum class MapObjectPlacement : uint8_t {
	/** On open floor. */
	Floor,
	/** On a wall tile, with open floor in front of it. */
	OnWall,
	/** On open floor, with a wall right behind it. */
	AgainstWall,
};

/** The editor's prop list is grouped by these. Not stored. */
enum class MapObjectGroup : uint8_t {
	Light,
	Wall,
	Furniture,
	Bodies,
	Clutter,
	SetPieces,
};
constexpr size_t NumMapObjectGroups = 6;

/** What a kind is like in play, for the editor, the docs and an automatic dressing pass. */
struct MapObjectTraits {
	MapObjectPlacement placement = MapObjectPlacement::Floor;
	/** Heroes walk round it. Missiles always pass props. */
	bool solid = true;
	/** A light source (AddObjectLight). */
	bool light = false;
	/** Something a player can use: only the barrel and the chest. Props are made inert. */
	bool interactive = false;
	/** Looks: MapObject::variant's low bits run from 0 to this minus 1. */
	uint8_t styles = 1;
	MapObjectGroup group = MapObjectGroup::Clutter;
	/** Tiles it stands on, the stored one included (MapObjectFootprint says which). */
	uint8_t tiles = 1;
};

/** For OnWall and AgainstWall kinds, this bit of MapObject::variant: the floor is at +y, not +x. */
constexpr uint8_t MapObjectFacesY = 0x80;
constexpr uint8_t MapObjectStyleMask = 0x7F;

struct MapObject {
	MapObjectKind kind = MapObjectKind::Barrel;
	/** World tile. For an OnWall kind, the wall tile. */
	Point position;
	/** Style in the low bits, and MapObjectFacesY. Always 0 for the legacy kinds. */
	uint8_t variant = 0;

	[[nodiscard]] uint8_t style() const { return variant & MapObjectStyleMask; }
	[[nodiscard]] bool facesY() const { return (variant & MapObjectFacesY) != 0; }
	bool operator==(const MapObject &other) const { return kind == other.kind && position == other.position && variant == other.variant; }
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
/**
 * A stable name for scripts and stamps: lower case with underscores ("wall_torch", "closed_chest"). It never
 * changes once released, unlike the name shown, which may be reworded.
 */
std::string_view MapObjectId(MapObjectKind kind);
/** By MapObjectId or MapObjectName, any case. */
std::optional<MapObjectKind> MapObjectKindNamed(std::string_view name);
std::string_view MapObjectGroupName(MapObjectGroup group);
MapObjectTraits GetMapObjectTraits(MapObjectKind kind);
/**
 * The tiles a kind blocks or covers, as offsets from its stored tile ({ 0, 0 } first). One tile for most; a
 * sarcophagus is two, a large fountain four. The engine's own footprints (AddSarcophagus, AddLargeFountain).
 */
std::vector<Displacement> MapObjectFootprint(MapObjectKind kind);
constexpr bool IsLegacyMapObject(MapObjectKind kind)
{
	return static_cast<size_t>(kind) < NumLegacyMapObjectKinds;
}
/** Doors exist in this tileset's art and object lists. */
bool MapTilesetHasDoors(MapTileset tileset);
bool MapTilesetHasArches(MapTileset tileset);
bool MapObjectFitsTileset(MapObjectKind kind, MapTileset tileset);
/** The kinds that fit a tileset and whose art the game data has, in palette order. */
std::vector<MapObjectKind> MapObjectPalette(MapTileset tileset);

/** Footprint of a staircase in megatiles (what it reshapes). */
Size MapStairsSize(MapTileset tileset, bool entrance);
/** True if a staircase with its top-left at `position` fits inside the paintable area. */
bool MapStairsFit(MapTileset tileset, bool entrance, Point position);
/**
 * True for stairs built into a wall (the Cathedral's and Crypt's up stairs, both of the Caves' and the Nest's), as
 * opposed to those that stand free in a room. The generator only ever puts these in a straight wall.
 */
bool MapStairsNeedWall(MapTileset tileset, bool entrance);
/** For the editor: where stairs that need a wall go. */
std::string MapStairsWallHint(MapTileset tileset, bool entrance);
/**
 * Why these stairs at `position` aren't set into a wall the way the generator's are, judged on the painted layout:
 * the rock behind them (their stamp's rock part) and one cell to either side of it must be rock already, and the
 * stamp's floor painted floor, so the stamp adds no free-standing wall. Nothing if they are, or stand free anyway.
 * Old maps that break it still load and play; the editor and ValidateMap say so.
 */
std::optional<std::string> MapStairsWallProblem(const KeepMap &map, bool entrance, Point position);
/** The nearest spot (by straight distance, up to `radius` cells away) where the stairs fit and pass the rule above. */
std::optional<Point> FindMapStairsSpot(const KeepMap &map, bool entrance, Point wanted, int radius);
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
/** The asset name of a campaign level's map. */
std::string CampaignMapAsset(int level);
/** A campaign map's asset name goes to the asset system; anything else is a file, as given or under the maps folder. */
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
/**
 * Bakes the map and judges the result, setting map.playable. What gets saved is the decorated level,
 * and decoration can matter (a candlestick can plug a narrow hall), so this rerolls decorSeed if the
 * decoration is what broke the map. The live map must be `map`, and the loaded tileset its own. Leaves
 * the decorated level in place: reapply afterwards.
 * @return map.playable
 */
bool BakePlayableMap(KeepMap &map);
/** True if the tile data in memory is this tileset's, as ApplyLayoutToLevel needs. */
bool IsMapTilesetLoaded(MapTileset tileset);
/** True if the game data has this tileset: the shareware data, say, stops after the Cathedral's. */
bool IsMapTilesetInstalled(MapTileset tileset);
/** True if the map can be read, is finished, and its tileset is installed. */
bool CanPlayMap(const std::string &path);
/** Everything the game can check of a map before building it, as for one that came from another player. */
tl::expected<void, std::string> CheckMapBytes(const std::byte *data, size_t size);
/** Sets leveltype and loads the tile data, for callers with no game level (LoadGameLevel does this itself). */
tl::expected<void, std::string> LoadMapTilesetData(MapTileset tileset);
/** The game's stairs triggers for the level as it stands. */
void InitMapTriggers(MapTileset tileset);
/** The tile of the level's first down-stairs trigger, which the game takes as the vault. */
Point FindVault(Point fallback);
/** Where heroes appear for this map: the world tile the staircase's generator would use. */
Point MapSpawnTile(const KeepMap &map);

struct MapProblem {
	std::string text;
	/** Stops the map being played; otherwise just a warning. */
	bool blocking = true;
};
/** Checks against the level as it stands, so ApplyLayoutToLevel + InitObjects must have run. */
std::vector<MapProblem> ValidateMap(const KeepMap &map);

// ---- props ----------------------------------------------------------------------------------------

/**
 * Where a prop of this kind would go for a click on `tile`, on the level as it stands: a wall kind snaps
 * to the wall beside the floor clicked (or takes the wall clicked) and gets its facing. Nothing if there's
 * no such wall. Other kinds come back as they are. Keeps `style`.
 */
std::optional<MapObject> SnapMapObject(MapObjectKind kind, Point tile, uint8_t style);
/**
 * Whether a prop may stand there, on the level as it stands (other objects included): nothing for yes,
 * or why not. The rules (docs/MAP-FORMAT.md): a solid prop never changes where heroes can get to, never
 * leaves a way through narrower than two tiles, and keeps off the stairs; a wall prop needs a straight
 * wall with floor in front. The legacy kinds aren't judged: they go anywhere on the floor, as in version 1.
 * The game judges each prop like this when it builds the level, and leaves out any that fail.
 */
std::optional<std::string> WhyMapObjectCantStand(const KeepMap &map, const MapObject &object);
/**
 * True for an object a map placed as a prop, on the keep level: it does nothing at all, whatever its type does
 * elsewhere. The engine's operate, break and update paths check this, so a shrine or chest can't be used, a
 * burning cross doesn't burn and a magic circle doesn't take anyone anywhere, however the request comes.
 */
bool IsInertMapProp(const Object &object);

/** Object sprite files the engine holds for a level (pObjCels). */
constexpr size_t MaxLevelObjectFiles = 40;
/**
 * Object sprite files a level built from this map loads: the keep's own and its tileset's, and the props'
 * (with `extra` too, if given). Over MaxLevelObjectFiles, kinds past what fits are left out.
 */
size_t MapObjectFileCount(const KeepMap &map, std::optional<MapObjectKind> extra = std::nullopt);
/** InitObjectGFX loaded this kind's art for the level on screen. */
bool IsMapObjectArtLoaded(MapObjectKind kind);
/**
 * The map uses a kind whose art the level on screen didn't ask for (the editor loads art when the level is
 * built): reloading the level would load it. Never true twice for the same kinds, so no reload loop.
 */
bool MapNeedsArtReload(const KeepMap &map);
/** The editor's chosen kind: its art is loaded with the level's, room permitting, so it shows when placed. */
void SetWantedMapObjectArt(std::optional<MapObjectKind> kind);

/** Props InitObjects left out of the level on screen, and why: for ValidateMap and the editor. */
const std::vector<std::pair<MapObject, std::string>> &LeftOutMapObjects();

/** The editor's map is built live instead of from a file while this is set. */
void SetLiveMap(const KeepMap *map, const bool *decorate);

/** LoadGameLevel, first thing: reads the setup's map, and sets leveltype from its tileset. */
void PrepareMapLevel();
/** CreateDungeon: @return true if the level came from a map rather than the generator. */
bool BuildMapLevel();
/** InitObjects: @return true if a map placed the level's objects. */
bool InitMapObjects();
/**
 * InitObjectGFX, on the keep level: adds the art of the props the level's map uses to the files to load
 * (`filesWidths`, `fileCount` long), as long as no more than `maxFiles` are loaded in all. In the editor, the
 * chosen kind's and then the rest of the tileset's, while there's room, so most can be placed without a reload.
 */
void AddMapObjectGraphics(uint16_t *filesWidths, size_t fileCount, size_t maxFiles);
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
