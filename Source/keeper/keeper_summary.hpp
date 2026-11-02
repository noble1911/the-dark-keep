/**
 * @file keeper/keeper_summary.hpp
 *
 * The end-of-map screen, in the Keeper screen's style: how the map went, what it earned, what can be
 * bought now, why a defeat happened, and what to do next.
 */
#pragma once

#include <string>

#include "engine/point.hpp"
#include "engine/rectangle.hpp"
#include "engine/surface.hpp"

namespace devilution::keeper {

/** The map ended or a new one loaded: what it computes once is worked out again. */
void ForgetMapSummary();

/** The panel on `out`, which is the whole UI surface. */
Rectangle MapSummaryRect(const Surface &out);
/** Button `index` of `count` along the panel's bottom (the continue button, if any, comes first). */
Rectangle MapSummaryButton(const Surface &out, int index, int count);
/** `continueLabel` is empty if this master doesn't choose what comes next. */
void DrawMapSummary(const Surface &out, Point mouse, const std::string &continueLabel, const std::string &leaveLabel);

} // namespace devilution::keeper
