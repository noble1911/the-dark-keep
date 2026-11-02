/**
 * @file keeper/keeper_summary.hpp
 *
 * The end-of-map screen, in the Keeper screen's style: how the map went, what it earned, what can be
 * bought now, why a defeat happened, and what to do next.
 */
#pragma once

#include <string>
#include <vector>

#include "engine/point.hpp"
#include "engine/rectangle.hpp"
#include "engine/surface.hpp"

namespace devilution::keeper {

/** The map ended or a new one loaded: what it computes once is worked out again. */
void ForgetMapSummary();

/** The panel on `out`, which is the whole UI surface. */
Rectangle MapSummaryRect(const Surface &out);
/** Button `index` of `count` along the panel's bottom (the main choice comes first). */
Rectangle MapSummaryButton(const Surface &out, int index, int count);
/** `labels` are the buttons, the main choice first. `note` is a line about them, such as what a retry costs. */
void DrawMapSummary(const Surface &out, Point mouse, const std::vector<std::string> &labels, const std::string &note);

} // namespace devilution::keeper
