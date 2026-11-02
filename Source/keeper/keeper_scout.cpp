/**
 * @file keeper/keeper_scout.cpp
 *
 * The route preview and scout card. See keeper_scout.hpp.
 */
#include "keeper/keeper_scout.hpp"

#include <algorithm>
#include <utility>

#include <fmt/format.h>

#include "keeper/keeper_campaign.hpp"
#include "keeper/keeper_game.hpp"
#include "keeper/keeper_heroes.hpp"

namespace devilution::keeper {

namespace {

bool routePreviewOn = true;

RoutePreview preview;
bool previewValid = false;
uint32_t previewKey = 0;

/** Changes whenever what the route depends on does: the locks, the two ends, the door being hovered. */
uint32_t RouteKey(std::optional<Point> hoverDoor)
{
	const auto mix = [](uint32_t hash, Point tile) {
		return (hash ^ static_cast<uint32_t>(tile.x * 256 + tile.y + 1)) * 16777619U;
	};
	uint32_t key = 2166136261U ^ CurrentSetup().seed;
	key = mix(key, State.entrance);
	key = mix(key, State.vault);
	for (const DoorLock &lock : State.locks) {
		if (lock.active)
			key = mix(key, lock.position);
	}
	return hoverDoor ? mix(key, *hoverDoor) : key;
}

bool RouteHas(const std::vector<Point> &route, Point tile)
{
	return std::find(route.begin(), route.end(), tile) != route.end();
}

void BuildPreview(std::optional<Point> hoverDoor)
{
	preview = {};
	preview.route = EstimateHeroRoute({});
	std::vector<Point> open = EstimateHeroRoute({ .ignoreLocks = true });
	if (open != preview.route)
		preview.withoutLocks = std::move(open);
	for (const DoorLock &lock : State.locks) {
		if (lock.active && RouteHas(preview.route, lock.position))
			preview.bashedLocks.push_back(lock.position);
	}
	if (!hoverDoor)
		return;
	std::vector<Point> locked = EstimateHeroRoute({ .extraLock = hoverDoor });
	if (locked != preview.route) {
		const int extra = static_cast<int>(locked.size()) - static_cast<int>(preview.route.size());
		preview.withHoverLock = std::move(locked);
		preview.hoverVerdict = fmt::format("Turns them away: {:+} steps", extra);
		preview.hoverTurnsThem = true;
	} else if (RouteHas(preview.route, *hoverDoor)) {
		preview.hoverVerdict = "They'd just bash it down";
	} else {
		preview.hoverVerdict = "Not on their route";
	}
}

} // namespace

bool IsRoutePreviewVisible()
{
	return routePreviewOn && (State.phase == Phase::Setup || State.phase == Phase::Breather);
}

void ToggleRoutePreview()
{
	routePreviewOn = !routePreviewOn;
}

bool IsRoutePreviewOn()
{
	return routePreviewOn;
}

const RoutePreview &GetRoutePreview(std::optional<Point> hoverDoor)
{
	const uint32_t key = RouteKey(hoverDoor);
	if (!previewValid || key != previewKey) {
		BuildPreview(hoverDoor);
		previewKey = key;
		previewValid = true;
	}
	return preview;
}

} // namespace devilution::keeper
