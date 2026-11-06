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
};

struct DressResult {
	/** Light sources there before (braziers, the tileset's own, hand-placed props). */
	int lightsBefore = 0;
	int lightsAdded = 0;
	int sceneryAdded = 0;
	/** Solid props the route check took back out. */
	int rejectedForRoute = 0;
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

} // namespace devilution::keeper
