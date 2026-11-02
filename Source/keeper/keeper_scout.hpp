/**
 * @file keeper/keeper_scout.hpp
 *
 * What a master can know before a raid: the heroes' expected route, and (see the scout card) who is coming.
 * All of it is read-only and local to this client: nothing here changes the simulation.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "engine/point.hpp"

namespace devilution::keeper {

// ---- route preview ------------------------------------------------------------------

/** Tiles between the dots marching along the drawn route, and how fast they move. */
constexpr size_t RouteMarchSpacing = 6;
constexpr uint32_t RouteMarchMilliseconds = 90;

struct RoutePreview {
	/** Where the party is expected to walk, with the locks as they stand. */
	std::vector<Point> route;
	/** Where it would walk with no locks; empty if that is the same way. */
	std::vector<Point> withoutLocks;
	/** Where it would walk with the hovered door locked too; empty if that is the same way. */
	std::vector<Point> withHoverLock;
	/** Locked doors `route` goes through: the way round is too long, so they get bashed. */
	std::vector<Point> bashedLocks;
	/** What locking the hovered door would do, in words. */
	std::string hoverVerdict;
	bool hoverTurnsThem = false;
};

/** The preview is on (the player's choice) and it's time to prepare: raids themselves stay a surprise. */
bool IsRoutePreviewVisible();
void ToggleRoutePreview();
bool IsRoutePreviewOn();
/** The current estimate. Worked out again only when a lock, or the hovered door, has changed. */
const RoutePreview &GetRoutePreview(std::optional<Point> hoverDoor);

} // namespace devilution::keeper
