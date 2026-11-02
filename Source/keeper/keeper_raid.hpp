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

// ---- stat hooks (called from the simulation) -------------------------------------

/** Every client, when the phase changes to Raid. */
void NoteRaidStarted(uint8_t wave);
/** Every client, when the phase leaves Raid: `outcome` is the new phase. */
void NoteRaidEnded(Phase outcome, int tribute, int interest);
/** The host paid for opening the gates early. */
void NoteEarlyCallBonus(int amount);
/** A hero died: the bounty paid and the stolen gold it dropped back into the vault. */
void NoteHeroSlain(int bounty, int recovered);
/** A hero left the keep (`fled`: in terror, empty-handed), carrying `gold` out. */
void NoteHeroLeft(bool fled, int gold);
void NoteGoldLooted(int amount);
/** Gold a terrified hero dropped (it goes back into the vault). */
void NoteGoldRecovered(int amount);
void NoteKeepMonsterLost();

} // namespace devilution::keeper
