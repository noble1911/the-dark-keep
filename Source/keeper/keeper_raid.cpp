/**
 * @file keeper/keeper_raid.cpp
 *
 * Raid roster and end-of-raid summary for The Dark Keep. See keeper_raid.hpp.
 *
 * Where the numbers come from (all hooks run on every client):
 * - slain, bounties, recovered gold: OnMonsterDeath (monster heroes) and the bots' HandleDeath;
 * - escaped / fled and gold carried off: HeroEscapes and the bots' Escape;
 * - gold looted: HeroSteals and the bots' vault visit; dropped gold: the CheckFear rolls;
 * - keep monsters lost: OnMonsterDeath; tribute: the phase change (TributeFor, deterministic).
 * Approximate in co-op: fear rolls and the bots' loot use each client's own RNG / vault view,
 * and the host's snapshots correct the vault but not these tallies, so a client's numbers can
 * differ slightly from the host's.
 */
#include "keeper/keeper_raid.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "engine/palette.h"
#include "engine/render/primitive_render.hpp"
#include "engine/render/text_render.hpp"
#include "keeper/keeper_bots.hpp"
#include "keeper/keeper_heroes.hpp"
#include "keeper/keeper_units.hpp"
#include "monster.h"
#include "player.h"
#include "utils/ui_fwd.h"

namespace devilution::keeper {

namespace {

// ---- stats -------------------------------------------------------------------------

struct RaidStats {
	uint8_t wave = 0;
	Phase outcome = Phase::Breather;
	int slain = 0;
	int escaped = 0;
	int fled = 0;
	int goldLooted = 0;
	int goldCarriedOff = 0;
	int goldRecovered = 0;
	int bounties = 0;
	int tribute = 0;
	int monstersLost = 0;
};

/** The raid in progress (only counted while `raidActive`). */
RaidStats current;
bool raidActive = false;
/** The last finished raid, shown by the summary panel. */
RaidStats finished;
bool summaryVisible = false;

// ---- shared drawing ----------------------------------------------------------------

constexpr uint8_t FrameColor = PAL16_YELLOW + 4;

void DrawPanel(const Surface &out, const Rectangle &rect)
{
	DrawHalfTransparentRectTo(out, rect.position.x, rect.position.y, rect.size.width, rect.size.height);
	DrawHalfTransparentRectTo(out, rect.position.x, rect.position.y, rect.size.width, rect.size.height);
	FillRect(out, rect.position.x, rect.position.y, rect.size.width, 1, FrameColor);
	FillRect(out, rect.position.x, rect.position.y + rect.size.height - 1, rect.size.width, 1, FrameColor);
	FillRect(out, rect.position.x, rect.position.y, 1, rect.size.height, FrameColor);
	FillRect(out, rect.position.x + rect.size.width - 1, rect.position.y, 1, rect.size.height, FrameColor);
}

// ---- roster ------------------------------------------------------------------------

constexpr int RosterWidth = 200;
constexpr int RosterMargin = 6;
constexpr int RosterPadding = 4;
constexpr int RowHeight = 14;
constexpr size_t MaxRows = 12;
/** Columns, relative to the row's text start. */
constexpr int NameWidth = 68;
constexpr int BarX = 70;
constexpr int RosterBarWidth = 26;
constexpr int StateX = 100;

struct RosterRow {
	bool isPlayer;
	size_t id;
	std::string name;
	UiFlags nameColor;
	int hitPoints;
	int maxHitPoints;
	std::string state;
	UiFlags stateColor;
	Point tile;
};

struct RowState {
	std::string text;
	UiFlags color;
};

/** The parts of a hero's state both kinds share (portal, terror, loot), if any applies. */
std::optional<RowState> CommonState(bool petrified, bool portalCaster, bool fleeing, int carried)
{
	if (petrified)
		return RowState { "petrified", UiFlags::ColorBlue };
	if (portalCaster && !PortalPosition())
		return RowState { "opening portal", UiFlags::ColorRed };
	if (State.bossDown)
		return PortalPosition() ? RowState { "escaping", UiFlags::ColorRed } : RowState { "guarding", UiFlags::ColorWhite };
	if (fleeing)
		return RowState { "fleeing", UiFlags::ColorOrange };
	if (carried > 0)
		return RowState { fmt::format("{}g to exit", carried), UiFlags::ColorRed };
	if (carried < 0)
		return RowState { "leaving", UiFlags::ColorWhite };
	return std::nullopt;
}

RowState GoalState(bool fighting, Point tile)
{
	if (fighting)
		return { "fighting", UiFlags::ColorWhite };
	if (GoalDistance(HeroGoal::Vault, tile) <= 1)
		return { "at the vault", UiFlags::ColorRed };
	return { "to vault", UiFlags::ColorGold };
}

std::vector<RosterRow> RosterRows()
{
	std::vector<RosterRow> rows;
	for (size_t i = 0; i < MaxBots && FirstBotId + i < Players.size() && rows.size() < MaxRows; i++) {
		const Player &bot = Players[FirstBotId + i];
		const std::optional<BotSummary> summary = GetBotSummary(bot);
		if (!summary)
			continue;
		const bool fighting = bot.destAction == ACTION_ATTACKMON || bot.destAction == ACTION_RATTACKMON || bot.destAction == ACTION_SPELLMON;
		const RowState state = CommonState(IsPetrifiedBot(bot), IsPortalCaster(bot), summary->fleeing, summary->carried)
		                           .value_or(GoalState(fighting, bot.position.tile));
		rows.push_back({ true, bot.getId(), bot._pName, UiFlags::ColorGold, bot._pHitPoints, bot._pMaxHP, state.text, state.color, bot.position.tile });
	}
	for (size_t i = 0; i < ActiveMonsterCount && rows.size() < MaxRows; i++) {
		const Monster &monster = Monsters[ActiveMonsters[i]];
		const std::optional<HeroSummary> summary = GetHeroSummary(monster);
		if (!summary || monster.isInvalid || monster.mode == MonsterMode::Death)
			continue;
		const bool fighting = (monster.flags & MFLAG_NO_ENEMY) == 0 && (monster.flags & MFLAG_TARGETS_MONSTER) != 0;
		const RowState state = CommonState(monster.mode == MonsterMode::Petrified, IsPortalCaster(monster), summary->fleeing, summary->carried)
		                           .value_or(GoalState(fighting, monster.position.tile));
		rows.push_back({ false, monster.getId(), std::string(HeroClassName(summary->cls)), summary->champion ? UiFlags::ColorYellow : UiFlags::ColorWhite,
		    monster.hitPoints, monster.maxHitPoints, state.text, state.color, monster.position.tile });
	}
	return rows;
}

Rectangle RosterRect(const Surface &out, size_t rowCount)
{
	const int height = 2 * RosterPadding + static_cast<int>(rowCount + 1) * RowHeight;
	return { { out.w() - RosterWidth - RosterMargin, RosterMargin }, { RosterWidth, height } };
}

/** Row `index` (0 = first hero, below the header). */
Rectangle RowRect(const Rectangle &roster, size_t index)
{
	return { { roster.position.x + 1, roster.position.y + RosterPadding + static_cast<int>(index + 1) * RowHeight }, { roster.size.width - 2, RowHeight } };
}

// ---- summary -----------------------------------------------------------------------

constexpr int SummaryWidth = 250;
constexpr int SummaryLineHeight = 14;
constexpr int SummaryTitleHeight = 22;

struct SummaryLine {
	std::string label;
	std::string value;
	UiFlags color;
};

std::vector<SummaryLine> SummaryLines()
{
	const RaidStats &s = finished;
	const int net = s.goldRecovered + s.bounties + s.tribute - s.goldLooted;
	std::vector<SummaryLine> lines {
		{ "Heroes slain", std::to_string(s.slain), UiFlags::ColorGold },
		{ "Heroes escaped", std::to_string(s.escaped), s.escaped > 0 ? UiFlags::ColorRed : UiFlags::ColorWhite },
		{ "Heroes fled in terror", std::to_string(s.fled), UiFlags::ColorWhite },
		{ "Gold stolen from the vault", std::to_string(s.goldLooted), s.goldLooted > 0 ? UiFlags::ColorRed : UiFlags::ColorWhite },
		{ "  ...carried out of the keep", std::to_string(s.goldCarriedOff), s.goldCarriedOff > 0 ? UiFlags::ColorRed : UiFlags::ColorWhite },
		{ "Gold recovered", std::to_string(s.goldRecovered), UiFlags::ColorGold },
		{ "Bounties earned", std::to_string(s.bounties), UiFlags::ColorGold },
	};
	if (s.outcome == Phase::Breather)
		lines.push_back({ "Tribute paid", std::to_string(s.tribute), UiFlags::ColorGold });
	lines.push_back({ "Keep monsters lost", std::to_string(s.monstersLost), s.monstersLost > 0 ? UiFlags::ColorRed : UiFlags::ColorWhite });
	lines.push_back({ "Net gold this raid", fmt::format("{:+}", net), net >= 0 ? UiFlags::ColorGold : UiFlags::ColorRed });
	return lines;
}

std::string SummaryTitle()
{
	switch (finished.outcome) {
	case Phase::Victory:
		return fmt::format("Raid {}: the keep stands!", finished.wave);
	case Phase::Defeat:
		return fmt::format("Raid {}: the keep has fallen", finished.wave);
	default:
		return fmt::format("Raid {} repelled", finished.wave);
	}
}

} // namespace

// ---- roster API --------------------------------------------------------------------

std::optional<Rectangle> RaidRosterRect(const Surface &out)
{
	if (State.phase != Phase::Raid)
		return std::nullopt;
	return RosterRect(out, RosterRows().size());
}

void DrawRaidRoster(const Surface &out, Point mouse)
{
	if (State.phase != Phase::Raid)
		return;
	const std::vector<RosterRow> rows = RosterRows();
	const Rectangle rect = RosterRect(out, rows.size());
	DrawPanel(out, rect);

	const int textX = rect.position.x + 6;
	const int textWidth = rect.size.width - 12;
	const int headerY = rect.position.y + RosterPadding;
	DrawString(out, fmt::format("Raid {}", State.wave), { { textX, headerY }, { textWidth, RowHeight } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorGold });
	const int left = raidActive ? current.escaped + current.fled : 0;
	const int slain = raidActive ? current.slain : 0;
	DrawString(out, fmt::format("{} slain  {} escaped", slain, left), { { textX, headerY }, { textWidth, RowHeight } },
	    { .flags = UiFlags::FontSize12 | UiFlags::AlignRight | (left > 0 ? UiFlags::ColorRed : UiFlags::ColorWhite) });

	for (size_t i = 0; i < rows.size(); i++) {
		const RosterRow &row = rows[i];
		const Rectangle rowRect = RowRect(rect, i);
		if (rowRect.contains(mouse)) {
			FillRect(out, rowRect.position.x, rowRect.position.y, rowRect.size.width, 1, FrameColor);
			FillRect(out, rowRect.position.x, rowRect.position.y + rowRect.size.height - 1, rowRect.size.width, 1, FrameColor);
		}
		const int y = rowRect.position.y;
		DrawString(out, row.name, { { textX, y }, { NameWidth, RowHeight } }, { .flags = UiFlags::FontSize12 | row.nameColor });

		const int barX = textX + BarX;
		const int barY = y + 5;
		FillRect(out, barX - 1, barY - 1, RosterBarWidth + 2, 6, PAL16_GRAY + 14);
		const int filled = row.maxHitPoints > 0 ? std::clamp(RosterBarWidth * row.hitPoints / row.maxHitPoints, 0, RosterBarWidth) : 0;
		if (filled > 0)
			FillRect(out, barX, barY, filled, 4, PAL16_RED + 2);

		DrawString(out, row.state, { { textX + StateX, y }, { textWidth - StateX, RowHeight } }, { .flags = UiFlags::FontSize12 | UiFlags::AlignRight | row.stateColor });
	}
}

std::optional<Point> OnRaidRosterClick(const Surface &out, Point mouse)
{
	if (State.phase != Phase::Raid)
		return std::nullopt;
	const std::vector<RosterRow> rows = RosterRows();
	const Rectangle rect = RosterRect(out, rows.size());
	for (size_t i = 0; i < rows.size(); i++) {
		if (!RowRect(rect, i).contains(mouse))
			continue;
		SelectHero(rows[i].isPlayer, rows[i].id);
		return rows[i].tile;
	}
	return std::nullopt;
}

// ---- summary API -------------------------------------------------------------------

bool IsRaidSummaryVisible()
{
	return summaryVisible;
}

void DismissRaidSummary()
{
	summaryVisible = false;
}

std::optional<Rectangle> RaidSummaryRect(const Surface &out, int hudTop)
{
	if (!summaryVisible)
		return std::nullopt;
	const int lineCount = static_cast<int>(SummaryLines().size());
	const int height = 8 + SummaryTitleHeight + lineCount * SummaryLineHeight + 6 + SummaryLineHeight + 6;
	const int width = std::min(SummaryWidth, out.w());
	int y = (hudTop - height) / 2;
	if (finished.outcome == Phase::Victory || finished.outcome == Phase::Defeat) {
		// Keep clear of the game-over banner across the middle of the screen.
		y = std::min(hudTop / 2 + 48, hudTop - height - 4);
	}
	return Rectangle { { (out.w() - width) / 2, std::max(y, 4) }, { width, height } };
}

void DrawRaidSummary(const Surface &out, int hudTop)
{
	const std::optional<Rectangle> rect = RaidSummaryRect(out, hudTop);
	if (!rect)
		return;
	DrawPanel(out, *rect);

	const int textX = rect->position.x + 10;
	const int textWidth = rect->size.width - 20;
	int y = rect->position.y + 8;
	const UiFlags titleColor = finished.outcome == Phase::Defeat ? UiFlags::ColorRed : UiFlags::ColorGold;
	DrawString(out, SummaryTitle(), { { rect->position.x, y }, { rect->size.width, SummaryTitleHeight } }, { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | titleColor });
	y += SummaryTitleHeight;
	for (const SummaryLine &line : SummaryLines()) {
		DrawString(out, line.label, { { textX, y }, { textWidth, SummaryLineHeight } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorWhite });
		DrawString(out, line.value, { { textX, y }, { textWidth, SummaryLineHeight } }, { .flags = UiFlags::FontSize12 | UiFlags::AlignRight | line.color });
		y += SummaryLineHeight;
	}
	y += 6;
	DrawString(out, "Click or Esc to close", { { rect->position.x, y }, { rect->size.width, SummaryLineHeight } }, { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | UiFlags::ColorWhite });
}

void ResetRaidStats()
{
	current = {};
	finished = {};
	raidActive = false;
	summaryVisible = false;
}

// ---- stat hooks --------------------------------------------------------------------

void NoteRaidStarted(uint8_t wave)
{
	current = {};
	current.wave = wave;
	raidActive = true;
	summaryVisible = false;
}

void NoteRaidEnded(Phase outcome, int tribute)
{
	if (!raidActive)
		return;
	current.outcome = outcome;
	current.tribute = tribute;
	finished = current;
	raidActive = false;
	summaryVisible = true;
}

void NoteHeroSlain(int bounty, int recovered)
{
	if (!raidActive)
		return;
	current.slain++;
	current.bounties += bounty;
	current.goldRecovered += recovered;
}

void NoteHeroLeft(bool fled, int gold)
{
	if (!raidActive)
		return;
	if (fled)
		current.fled++;
	else
		current.escaped++;
	current.goldCarriedOff += gold;
}

void NoteGoldLooted(int amount)
{
	if (raidActive)
		current.goldLooted += amount;
}

void NoteGoldRecovered(int amount)
{
	if (raidActive)
		current.goldRecovered += amount;
}

void NoteKeepMonsterLost()
{
	if (raidActive)
		current.monstersLost++;
}

} // namespace devilution::keeper
