/**
 * @file keeper/keeper_alerts.hpp
 *
 * Telling the master what is happening: banners, the strip of live alerts (the Lord under attack, heroes
 * near the vault, thieves running, the portal) and the sounds that go with them.
 *
 * It only reads the keep's state, on each client, and draws what it finds. It never changes the simulation
 * or takes a random number: the sounds it plays are ones the engine doesn't randomise.
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "engine/point.hpp"
#include "engine/surface.hpp"
#include "keeper/keeper_heroes.hpp"

namespace devilution::keeper {

/** A hero in the keep, monster or bot. */
struct HeroMark {
	Point tile;
	HeroClassKind cls;
	/** Gold carried (-1: robbed nothing, but heading out anyway). */
	int carried;
	bool fleeing;
	bool portalCaster;
};
/** Every living hero now. */
std::vector<HeroMark> LivingHeroMarks();

void AlertsOnLevelLoaded();
void AlertsTick();
void AlertsNoteSpellCast();
int SpellsCastThisMap();
/** Who was near the Keep Lord when he was last hurt, as words ("a Warrior and a Rogue"), or empty. */
std::string LordSlayers();
/** Keeper level when this map began. */
int KeeperLevelAtMapStart();

/** Unlocks this master could afford when the map began, for "newly affordable". */
struct UnlockItem {
	enum class Type : uint8_t {
		Monster,
		Spell,
		Trap,
	};
	Type type;
	size_t index;
	bool operator==(const UnlockItem &other) const = default;
};
/** What the master could unlock with Renown right now and couldn't when the map began. */
std::vector<UnlockItem> NewlyAffordableUnlocks();

/** Draws the banner, if one is up, with its top at `y`, in `left`..`left + width`. @return where the next thing goes. */
int DrawBanner(const Surface &out, int left, int width, int y);
/** Draws the live alerts. @return where the next thing goes. */
int DrawAlerts(const Surface &out, int left, int width, int y, Point mouse);
/** A click on an alert: the tile to look at. */
std::optional<Point> AlertsOnClick(Point mouse);
bool AlertsIsMouseOver(Point mouse);

} // namespace devilution::keeper
