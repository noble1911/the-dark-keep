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
 * Fear rolls and the bots' loot use each client's own RNG / vault view, so during a raid a joiner's
 * tally can differ a little from the host's. When the raid ends the host's numbers replace it
 * (ApplyRaidResultField), so the summaries agree.
 */
#include "keeper/keeper_raid.hpp"

#include "keeper/keeper_profile.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "engine/palette.h"
#include "engine/render/primitive_render.hpp"
#include "engine/render/text_render.hpp"
#include "keeper/keeper_bots.hpp"
#include "keeper/keeper_goals.hpp"
#include "keeper/keeper_pad.hpp"
#include "keeper/keeper_heroes.hpp"
#include "keeper/keeper_ui.hpp"
#include "keeper/keeper_tutorial.hpp"
#include "keeper/keeper_units.hpp"
#include "monster.h"
#include "player.h"
#include "tables/itemdat.h"
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
	/** The part of the tribute that is interest on the vault. */
	int interest = 0;
	int earlyCall = 0;
	int monstersLost = 0;
	/** What this master earned in the raid (their own: never replaced by the host's). */
	int xp = 0;
};

/** The raid in progress (only counted while `raidActive`). */
RaidStats current;
bool raidActive = false;
/** The last finished raid, shown by the summary panel. */
RaidStats finished;
bool summaryVisible = false;
MapTotals totals;
int xpAtRaidStart = 0;

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

constexpr int SummaryWidth = 400;
constexpr int SummaryLineHeight = 14;
constexpr int SummaryTitleHeight = 30;
constexpr int SummaryTileHeight = 66;
constexpr int SummaryNextHeight = 30;
constexpr int SummaryButtonHeight = 22;

struct SummaryLine {
	std::string label;
	std::string value;
	UiFlags color;
};

std::vector<SummaryLine> SummaryLines()
{
	const RaidStats &s = finished;
	const int net = s.goldRecovered + s.bounties + s.tribute + s.earlyCall - s.goldLooted;
	std::vector<SummaryLine> lines {
		{ "Heroes fled in terror", std::to_string(s.fled), UiFlags::ColorWhite },
		{ "Gold stolen from the vault", std::to_string(s.goldLooted), s.goldLooted > 0 ? UiFlags::ColorRed : UiFlags::ColorWhite },
		{ "  ...carried out of the keep", std::to_string(s.goldCarriedOff), s.goldCarriedOff > 0 ? UiFlags::ColorRed : UiFlags::ColorWhite },
		{ "Gold recovered + bounties", std::to_string(s.goldRecovered + s.bounties), UiFlags::ColorGold },
		{ "Tribute paid", std::to_string(s.tribute), UiFlags::ColorGold },
		{ "Your monsters lost", std::to_string(s.monstersLost), s.monstersLost > 0 ? UiFlags::ColorRed : UiFlags::ColorWhite },
	};
	if (s.outcome == Phase::Breather)
		lines.push_back({ "Tribute paid", std::to_string(s.tribute), UiFlags::ColorGold });
	if (s.interest > 0)
		lines.push_back({ "  ...of it vault interest", std::to_string(s.interest), UiFlags::ColorGold });
	if (s.earlyCall > 0)
		lines.push_back({ "Early call bonus", std::to_string(s.earlyCall), UiFlags::ColorGold });
	lines.push_back({ "Keep monsters lost", std::to_string(s.monstersLost), s.monstersLost > 0 ? UiFlags::ColorRed : UiFlags::ColorWhite });
	lines.push_back({ "Net gold this raid", fmt::format("{:+}", net), net >= 0 ? UiFlags::ColorGold : UiFlags::ColorRed });
	if (s.outcome == Phase::Victory && GoalsApply()) {
		for (size_t goal = 0; goal < NumLevelGoals; goal++) {
			const bool earned = (FailedGoals() & (1U << goal)) == 0;
			lines.push_back({ fmt::format("Goal: {}", GoalInfo(goal).name), earned ? "Medal!" : "Missed", earned ? UiFlags::ColorGold : UiFlags::ColorRed });
		}
	}
	return lines;
}

/** One of the big numbers along the top of the summary. */
void DrawSummaryTile(const Surface &out, const Rectangle &rect, const UiIcon &icon, const std::string &value, std::string_view label, UiFlags color)
{
	FillRect(out, rect.position.x, rect.position.y, rect.size.width, rect.size.height, PAL16_GRAY + 13);
	DrawFrame(out, rect, PAL16_GRAY + 9);
	constexpr int IconSize = 28;
	DrawUiIcon(out.subregion(rect.position.x + 3, rect.position.y + 3, IconSize, IconSize), icon);
	// A big number when it fits; a number is never cut.
	const int valueWidth = rect.size.width - IconSize - 6;
	const bool big = GetLineWidth(value, GameFont24) <= valueWidth;
	DrawString(out, value, { { rect.position.x + IconSize + 3, rect.position.y + (big ? 4 : 10) }, { valueWidth, 28 } }, { .flags = (big ? UiFlags::FontSize24 : UiFlags::FontSize12) | UiFlags::AlignCenter | color });
	DrawString(out, FitText(label, rect.size.width - 2), { { rect.position.x, rect.position.y + rect.size.height - 22 }, { rect.size.width, SummaryLineHeight } }, { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | UiFlags::ColorWhite });
}

Rectangle SummaryButton(const Rectangle &panel)
{
	return { { panel.position.x + 10, panel.position.y + panel.size.height - SummaryButtonHeight - 8 }, { panel.size.width - 20, SummaryButtonHeight } };
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
			FillRect(out, rowRect.position.x, rowRect.position.y, rowRect.size.width, 1, PanelFrameColor);
			FillRect(out, rowRect.position.x, rowRect.position.y + rowRect.size.height - 1, rowRect.size.width, 1, PanelFrameColor);
		}
		const int y = rowRect.position.y;
		DrawString(out, FitText(row.name, NameWidth), { { textX, y }, { NameWidth, RowHeight } }, { .flags = UiFlags::FontSize12 | row.nameColor });

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

void RaidRosterTargets(const Surface &out, std::vector<Rectangle> &targets)
{
	if (State.phase != Phase::Raid)
		return;
	const std::vector<RosterRow> rows = RosterRows();
	const Rectangle rect = RosterRect(out, rows.size());
	for (size_t i = 0; i < rows.size(); i++)
		targets.push_back(RowRect(rect, i));
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
	const int height = 8 + SummaryTitleHeight + SummaryTileHeight + 6 + lineCount * SummaryLineHeight + 4 + SummaryNextHeight + SummaryButtonHeight + 8;
	const int width = std::min(SummaryWidth, out.w());
	return Rectangle { { (out.w() - width) / 2, std::max((hudTop - height) / 2, 4) }, { width, height } };
}

void DrawRaidSummary(const Surface &out, int hudTop)
{
	const std::optional<Rectangle> rect = RaidSummaryRect(out, hudTop);
	if (!rect)
		return;
	DrawDungeonPanel(out, *rect);

	const int textX = rect->position.x + 10;
	const int textWidth = rect->size.width - 20;
	int y = rect->position.y + 6;
	DrawString(out, fmt::format("Raid {} repelled", finished.wave), { { rect->position.x, y }, { rect->size.width, SummaryTitleHeight } }, { .flags = UiFlags::FontSize24 | UiFlags::AlignCenter | UiFlags::ColorGold });
	y += SummaryTitleHeight + 2;

	const RaidStats &s = finished;
	const int net = s.goldRecovered + s.bounties + s.tribute - s.goldLooted;
	const int tileWidth = (textWidth - 3 * 4) / 4;
	const auto tile = [&](int index) {
		return Rectangle { { textX + index * (tileWidth + 4), y }, { tileWidth, SummaryTileHeight - 6 } };
	};
	DrawSummaryTile(out, tile(0), ItemIcon(ICURS_SHORT_SWORD), std::to_string(s.slain), "Slain", UiFlags::ColorGold);
	DrawSummaryTile(out, tile(1), ItemIcon(ICURS_SHORT_BOW), std::to_string(s.escaped), "Escaped", s.escaped > 0 ? UiFlags::ColorRed : UiFlags::ColorWhite);
	DrawSummaryTile(out, tile(2), ItemIcon(ICURS_GOLD_LARGE), fmt::format("{:+}", net), "Gold", net >= 0 ? UiFlags::ColorGold : UiFlags::ColorRed);
	DrawSummaryTile(out, tile(3), ItemIcon(ICURS_BOOK_BLUE), fmt::format("+{}", s.xp), "XP", UiFlags::ColorGold);
	y += SummaryTileHeight;

	for (const SummaryLine &line : SummaryLines()) {
		DrawString(out, line.label, { { textX, y }, { textWidth, SummaryLineHeight } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorWhite });
		DrawString(out, line.value, { { textX, y }, { textWidth, SummaryLineHeight } }, { .flags = UiFlags::FontSize12 | UiFlags::AlignRight | line.color });
		y += SummaryLineHeight;
	}
	y += 4;
	DrawString(out, FitLines(fmt::format("{}. Spend your gold before the gates open.", DescribeWave(State.wave)), textWidth, SummaryNextHeight, 14), { { textX, y }, { textWidth, SummaryNextHeight } },
	    { .flags = UiFlags::FontSize12 | UiFlags::ColorGold, .lineHeight = 14 });

	const Rectangle button = SummaryButton(*rect);
	FillRect(out, button.position.x, button.position.y, button.size.width, button.size.height, PAL16_RED + 12);
	DrawFrame(out, button, PAL16_YELLOW + 4);
	DrawString(out, fmt::format("Back to the keep ({})", InputName(PadAction::Cancel)), { button.position + Displacement { 0, 4 }, { button.size.width, SummaryLineHeight } }, { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | UiFlags::ColorGold });
}

void ResetRaidStats()
{
	current = {};
	finished = {};
	raidActive = false;
	summaryVisible = false;
	totals = {};
}

MapTotals GetMapTotals()
{
	return totals;
}

int RaidResultField(size_t field)
{
	const std::array<int, NumRaidResultFields> fields { finished.slain, finished.escaped, finished.fled, finished.goldLooted, finished.goldCarriedOff, finished.goldRecovered, finished.bounties, finished.tribute, finished.monstersLost,
		totals.slain, totals.escaped, totals.fled, totals.goldLooted, totals.goldCarriedOff, totals.monstersLost, finished.wave };
	return field < fields.size() ? fields[field] : 0;
}

void ApplyRaidResultField(size_t field, int value)
{
	const std::array<int *, NumRaidResultFields - 1> fields { &finished.slain, &finished.escaped, &finished.fled, &finished.goldLooted, &finished.goldCarriedOff, &finished.goldRecovered, &finished.bounties, &finished.tribute, &finished.monstersLost,
		&totals.slain, &totals.escaped, &totals.fled, &totals.goldLooted, &totals.goldCarriedOff, &totals.monstersLost };
	if (field < fields.size())
		*fields[field] = value;
	else if (field == fields.size())
		finished.wave = static_cast<uint8_t>(value);
}

// ---- stat hooks --------------------------------------------------------------------

void NoteRaidStarted(uint8_t wave)
{
	current = {};
	current.wave = wave;
	raidActive = true;
	summaryVisible = false;
	ProfileMarkRaidStart();
	xpAtRaidStart = MapXpEarned();
}

void NoteEarlyCallBonus(int amount)
{
	if (raidActive)
		current.earlyCall += amount;
}

void NoteRaidEnded(Phase outcome, int tribute, int interest)
{
	if (!raidActive)
		return;
	current.outcome = outcome;
	current.tribute = tribute;
	current.interest = interest;
	if (outcome == Phase::Defeat)
		ProfileNoteDefeat();
	else
		ProfileNoteRaidSurvived(current.wave);
	current.xp = MapXpEarned() - xpAtRaidStart;
	finished = current;
	totals.slain += current.slain;
	totals.escaped += current.escaped;
	totals.fled += current.fled;
	totals.goldLooted += current.goldLooted;
	totals.goldCarriedOff += current.goldCarriedOff;
	totals.monstersLost += current.monstersLost;
	raidActive = false;
	// A lost or won map has its own screen, with these numbers in it.
	summaryVisible = outcome == Phase::Breather;
}

void NoteHeroSlain(int bounty, int recovered)
{
	if (!raidActive)
		return;
	current.slain++;
	ProfileNoteHeroSlain(current.wave);
	current.bounties += bounty;
	current.goldRecovered += recovered;
}

void NoteHeroLeft(bool fled, int gold)
{
	if (!raidActive)
		return;
	if (fled) {
		current.fled++;
	} else {
		current.escaped++;
		FailGoal(LevelGoal::NoEscapes);
	}
	current.goldCarriedOff += gold;
}

void NoteGoldLooted(int amount)
{
	if (amount > 0)
		FailGoal(LevelGoal::VaultUntouched);
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
	if (raidActive) {
		current.monstersLost++;
		ProfileNoteMonsterLost();
	}
}

} // namespace devilution::keeper
