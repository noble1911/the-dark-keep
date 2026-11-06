/**
 * @file keeper/keeper_dress.hpp
 *
 * The dressing pass: lights and scenery for a map, chosen and placed by rule from a seed, so a bare layout
 * looks lived in. dkmap_tool runs it on the campaign's levels (a `dress:` line in a level source) and the
 * editor's Dress button runs the same code. Everything it adds is ordinary props in map.objects, baked into
 * the file, so co-op clients all build the same level. See docs/MAP-FORMAT.md.
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "keeper/keeper_map.hpp"

namespace devilution::keeper {

/** What a level's scenery is about. Kinds the tileset hasn't got are skipped. */
enum class DressTheme : uint8_t {
	/** Bones and the dead: skull piles, crucified skeletons, headless bodies. */
	Bones,
	/** Bookcases and book stands, candles. */
	Library,
	/** Banners, book stands, an angel statue. */
	Chapel,
	/** Weapon racks and armour stands, banners. */
	Armoury,
	/** Tortured and impaled bodies, a blood fountain. */
	Torture,
	/** Impaled bodies on the walls, skull piles. */
	Prison,
	/** Banners, skull piles, crucified skeletons, an angel statue. */
	Tomb,
	/** Everything grim: bodies, impalings, skull fires, a blood fountain. */
	Hell,
};
constexpr size_t NumDressThemes = 8;

std::string_view DressThemeName(DressTheme theme);
std::optional<DressTheme> DressThemeNamed(std::string_view name);
/** The theme a tileset gets when nothing says otherwise (the editor's button). */
DressTheme DefaultDressTheme(MapTileset tileset);

struct DressOptions {
	/** One or two themes; the first leads. Empty: the tileset's default. */
	std::vector<DressTheme> themes;
	/** Mixed into the map's decoration seed: the same map and seed always dress the same. */
	uint32_t seed = 0;
	/**
	 * Light sources on the level when done (the tileset's own and earlier props included, the stairs' not), at
	 * most, counted as wall torches: a candle, skull fire or brazier lights a tile or two and counts as half.
	 */
	int maxLights = 15;
	/** Tries for at least this many, closing the spacing up if it has to. */
	int minLights = 8;
	/** Steps (a diagonal is one) between the floor two torches shine on: leaves dark between the pools. */
	int lightSpacing = 9;
	/** Scenery props on the level when done, at most (lights not counted, props already there counted). */
	int maxScenery = 18;
	/** A centrepiece (blood fountain, angel statue, ...) in the biggest room, if the theme has one. */
	bool centrepiece = true;
	/**
	 * Judge it on the level with its wall and floor variants, as a saved map is built (the editor may be
	 * showing it plain). A variant can stand where a prop would, so this is what the game will judge too.
	 */
	bool decorated = true;
	/**
	 * The biggest rooms, this many, get a theme-room stamp each (RoomStampForTheme, the themes taken in turn)
	 * before the lights and scenery go in. A stamped room gets no scattered scenery or centrepiece, and its
	 * props don't count against maxScenery. 0, the default, dresses exactly as before stamps existed.
	 */
	int themedRooms = 0;
};

struct DressResult {
	/** Light sources there before (braziers, the tileset's own, hand-placed props). */
	int lightsBefore = 0;
	int lightsAdded = 0;
	int sceneryAdded = 0;
	/** Solid props the route check took back out. */
	int rejectedForRoute = 0;
	/** Props the theme-room stamps placed (themedRooms), lights among them, and how many rooms got one. */
	int stampedProps = 0;
	int stampedRooms = 0;
	std::vector<std::string> notes;
};

/**
 * Adds lights, then scenery, to `map.objects`. `map` must be the live map (SetLiveMap) with its tileset loaded:
 * the level is laid out from it again for each prop, and every rule is judged on that level. Each prop must
 * pass WhyMapObjectCantStand with everything before it standing, and is taken back out if it changes where
 * heroes appear, their shortest route, what locking any door does to it, or where a barricade could go on it.
 * Solid props keep two tiles off that route and off doors, so the walking lanes stay clear. Leaves the level
 * laid out with all its objects.
 * Deterministic: no game random numbers are used.
 */
DressResult DressMap(KeepMap &map, const DressOptions &options);

/** Light sources standing on the level now, the tileset's own included. */
int CountLevelLights();

// ---- theme-room stamps -----------------------------------------------------------------------------
//
// A stamp is a themed group of props for one room (a library corner, a torture room, ...), placed at once by
// the same rules and route checks as the dressing pass. Stamps name their props by MapObjectName, never by
// enum value: a name this build doesn't know, or a kind the tileset hasn't got, is skipped, and the next
// name in its list is tried, so a stamp written for kinds still to come still makes a room of today's.

/** Where in the room an entry's props go. The kind's own placement still decides how (wall, against, floor). */
enum class StampSpot : uint8_t {
	/** Round the room's edge: a wall kind on the walls, anything else on the floor by a wall. */
	Edge,
	/** The middle of the room, off the walls. The first one placed is the stamp's centrepiece. */
	Centre,
	/** Two or three tiles round the centrepiece (or the room's middle, without one). */
	Ring,
	/** Anywhere on the room's floor. */
	Scatter,
};

struct RoomStampEntry {
	/** MapObjectNames, '|' between them, any case: the first this build has, and the tileset, is used. */
	std::string_view kinds;
	StampSpot spot = StampSpot::Edge;
	/** How many, from min to max, picked by the seed. Edge and Scatter entries scale up in a big room, to twice as many. */
	uint8_t min = 1;
	uint8_t max = 1;
	/** Tiles (either axis) these keep from each other; 0: the stamp's spacing. */
	uint8_t apart = 0;
};

struct RoomStamp {
	/** Short and lower case: what level sources and devctl call it. */
	std::string_view name;
	/** What the editor's list shows. */
	std::string_view label;
	/** MapTileset bits (1 << tileset): the tilesets it is meant for. */
	uint8_t tilesets = 0;
	std::vector<RoomStampEntry> entries;
	/** Tiles (either axis) between any two of its floor props, at least. */
	uint8_t spacing = 2;
};

/** The built-in stamps, in the editor's order. */
const std::vector<RoomStamp> &RoomStamps();
const RoomStamp *RoomStampNamed(std::string_view name);
bool RoomStampSuits(const RoomStamp &stamp, MapTileset tileset);
/** The stamps meant for a tileset, in order. */
std::vector<const RoomStamp *> RoomStampsFor(MapTileset tileset);
/** The stamp a dressing theme puts in a themed room, if it suits the tileset. */
const RoomStamp *RoomStampForTheme(DressTheme theme, MapTileset tileset);
/** A kind by its MapObjectName, any case. Nothing for a name this build doesn't have. */
std::optional<MapObjectKind> MapObjectKindNamed(std::string_view name);

/**
 * The room a click on `cell` (a megatile) means, by the map's layout alone: the megatiles of the room it is
 * in (floor that some 3 by 3 block of floor covers), at most `reach` megatiles away from it either axis, so a
 * cavern that is all one room gives the part round the click. Empty if the cell isn't in a room.
 */
std::vector<Point> RoomCellsAt(const KeepMap &map, Point cell, int reach = 5);
/** The floor megatiles in a rectangle (corners in either order), for a dragged stamp. */
std::vector<Point> FloorCellsIn(const KeepMap &map, Point a, Point b);

struct StampOptions {
	/** Mixed with the map's decoration seed and the room: the same map, room and seed always stamp the same. */
	uint32_t seed = 0;
	bool decorated = true;
	/** Stored objects, at most, when done (default: what a map holds), and the level's with its doors. */
	size_t maxMapObjects = MaxMapObjects;
	int maxLevelObjects = MaxLevelObjectsAtLoad;
};

struct StampResult {
	/** What the stamp asked for (with the kinds there are) and what went in. */
	int wanted = 0;
	int placed = 0;
	int lights = 0;
	/** The object caps stopped it. */
	bool capped = false;
	/** Props the route check took back out. */
	int rejectedForRoute = 0;
	/** Names this build doesn't know (once each), and entries with no kind for this tileset at all. */
	std::vector<std::string> unknownNames;
	int entriesWithoutKind = 0;
	/** What was added, in order. */
	std::vector<MapObject> added;
	/** Why nothing went in, when it can't (no stairs, no route). */
	std::vector<std::string> notes;
};

/**
 * Stamps a room: adds the stamp's props on the room's megatiles `cells`, each judged as the dressing pass
 * judges its own (WhyMapObjectCantStand with everything before it standing, taken back out if it changes the
 * heroes' arrival, route, a lock's detour, a neck's way round or a barricade spot; solid props keep 2 tiles off
 * the route and off doors). Same preconditions as DressMap: `map` is the live map with its tileset loaded.
 * Leaves the level laid out with all its objects. Deterministic: no game random numbers are used.
 */
StampResult StampRoom(KeepMap &map, const RoomStamp &stamp, const std::vector<Point> &cells, const StampOptions &options);

} // namespace devilution::keeper
