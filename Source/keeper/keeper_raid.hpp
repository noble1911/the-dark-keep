/**
 * @file keeper/keeper_raid.hpp
 *
 * Raid overlays for The Dark Keep: the raid roster (every hero in the raid at a glance)
 * and the end-of-raid summary, plus the per-raid stats behind it.
 *
 * Stats are tallied on every client from events every client simulates (hero AI, bot AI,
 * monster deaths, phase changes), so co-op clients see their own, near-identical numbers.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "engine/point.hpp"
#include "engine/rectangle.hpp"
#include "engine/surface.hpp"
#include "keeper/keeper_game.hpp"

namespace devilution::keeper {

// ---- overlays (drawn on the UI surface; `mouse` and `hudTop` are in its coordinates) ----

/** The roster of living raiders, top-right (only during a raid). */
void DrawRaidRoster(const Surface &out, Point mouse);
/** The roster's rectangle, for hit tests; nullopt when hidden. */
std::optional<Rectangle> RaidRosterRect(const Surface &out);
/** Click on the roster: selects the hero and returns the tile to centre the camera on. */
std::optional<Point> OnRaidRosterClick(const Surface &out, Point mouse);

/** The end-of-raid summary, centred above the HUD (until dismissed or the next raid). */
void DrawRaidSummary(const Surface &out, int hudTop);
std::optional<Rectangle> RaidSummaryRect(const Surface &out, int hudTop);
bool IsRaidSummaryVisible();
void DismissRaidSummary();
/** New game / level load: forget all raid stats and hide the summary. */
void ResetRaidStats();

/** The whole map so far, for the end-of-map screen. */
struct MapTotals {
	int slain = 0;
	int escaped = 0;
	int fled = 0;
	int goldLooted = 0;
	int goldCarriedOff = 0;
	int monstersLost = 0;
};
MapTotals GetMapTotals();

/**
 * Co-op: a joiner's own tally can miss what happened before it arrived or while it was loading, so at the
 * end of each raid the host sends its numbers and they replace the joiner's. XP stays each player's own.
 */
constexpr size_t NumRaidResultFields = 16;
int RaidResultField(size_t field);
void ApplyRaidResultField(size_t field, int value);

// ---- stat hooks (called from the simulation) -------------------------------------

/** Every client, when the phase changes to Raid. */
void NoteRaidStarted(uint8_t wave);
/** Every client, when the phase leaves Raid: `outcome` is the new phase. */
void NoteRaidEnded(Phase outcome, int tribute);
/** A hero died: the bounty paid and the stolen gold it dropped back into the vault. */
void NoteHeroSlain(int bounty, int recovered);
/** A hero left the keep (`fled`: in terror, empty-handed), carrying `gold` out. */
void NoteHeroLeft(bool fled, int gold);
void NoteGoldLooted(int amount);
/** Gold a terrified hero dropped (it goes back into the vault). */
void NoteGoldRecovered(int amount);
void NoteKeepMonsterLost();

} // namespace devilution::keeper
