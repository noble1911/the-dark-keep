/**
 * @file keeper/keeper_summary.cpp
 *
 * The end-of-map screen. See keeper_summary.hpp.
 */
#include "keeper/keeper_summary.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include "engine/palette.h"
#include "engine/render/primitive_render.hpp"
#include "engine/render/text_render.hpp"
#include "keeper/keeper_alerts.hpp"
#include "keeper/keeper_campaign.hpp"
#include "keeper/keeper_editor.hpp"
#include "keeper/keeper_game.hpp"
#include "keeper/keeper_profile.hpp"
#include "keeper/keeper_raid.hpp"
#include "keeper/keeper_roster.hpp"
#include "keeper/keeper_tutorial.hpp"
#include "keeper/keeper_ui.hpp"
#include "multi.h"

namespace devilution::keeper {

namespace {

constexpr int MaxWidth = 620;
constexpr int MaxHeight = 500;
constexpr int Margin = 8;
constexpr int PortraitWidth = 96;
constexpr int PortraitHeight = 104;
constexpr int LineHeight = 14;
constexpr int ButtonHeight = 24;
constexpr int ButtonGap = 12;
constexpr int ButtonMaxWidth = 280;
constexpr int UnlockCardWidth = 76;
constexpr int UnlockCardHeight = 92;
constexpr int UnlockImageHeight = 56;
constexpr int UnlockGap = 6;
constexpr size_t MaxAdviceLines = 3;
/** Roughly the height of the command bar: the panel centres itself above it if there is room. */
constexpr int HudBarAllowance = 128;

/** Worked out once: the panel is drawn every frame, and these look through the whole roster. */
struct Report {
	Phase phase = Phase::Setup;
	std::vector<UnlockItem> unlocks;
	DefeatAdvice advice;
};
std::optional<Report> report;

const Report &GetReport()
{
	if (!report || report->phase != State.phase) {
		report.emplace();
		report->phase = State.phase;
		report->unlocks = NewlyAffordableUnlocks();
		if (State.phase == Phase::Defeat)
			report->advice = AdviseAfterDefeat(State.wave);
	}
	return *report;
}

std::string UnlockName(const UnlockItem &item)
{
	switch (item.type) {
	case UnlockItem::Type::Monster:
		return std::string(BuildKinds[item.index].name);
	case UnlockItem::Type::Spell:
		return std::string(SpellKinds[item.index].name);
	case UnlockItem::Type::Trap:
		return std::string(TrapKinds[item.index].name);
	}
	return {};
}

int UnlockCost(const UnlockItem &item)
{
	switch (item.type) {
	case UnlockItem::Type::Monster:
		return KindUnlockCost(item.index);
	case UnlockItem::Type::Spell:
		return SpellNextCost(item.index);
	case UnlockItem::Type::Trap:
		return TrapNextCost(item.index);
	}
	return 0;
}

UiIcon UnlockPicture(const UnlockItem &item)
{
	switch (item.type) {
	case UnlockItem::Type::Monster:
		return KindIcon(item.index);
	case UnlockItem::Type::Spell:
		return SpellIconOf(SpellKinds[item.index].icon);
	case UnlockItem::Type::Trap:
		return SpellIconOf(TrapKinds[item.index].icon);
	}
	return {};
}

void DrawUnlockCard(const Surface &out, const Rectangle &rect, const UnlockItem &item)
{
	FillRect(out, rect.position.x, rect.position.y, rect.size.width, rect.size.height, PAL16_GRAY + 13);
	const Rectangle image { rect.position + Displacement { 2, 2 }, { rect.size.width - 4, UnlockImageHeight } };
	FillRect(out, image.position.x, image.position.y, image.size.width, image.size.height, 0);
	DrawUiIcon(out.subregion(image.position.x, image.position.y, image.size.width, image.size.height), UnlockPicture(item));
	DrawString(out, FitText(UnlockName(item), rect.size.width - 2), { rect.position + Displacement { 0, UnlockImageHeight + 4 }, { rect.size.width, LineHeight } },
	    { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | UiFlags::ColorWhite });
	DrawString(out, fmt::format("{} Renown", UnlockCost(item)), { rect.position + Displacement { 0, UnlockImageHeight + 18 }, { rect.size.width, LineHeight } },
	    { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | UiFlags::ColorGold });
	DrawFrame(out, rect, PAL16_ORANGE + 4);
}

/** The XP bar and the keeper level it fills. */
int DrawProgress(const Surface &out, int x, int y, int width)
{
	const int level = KeeperLevel();
	const bool levelled = level > KeeperLevelAtMapStart();
	DrawString(out, levelled ? fmt::format("Keeper level {}: LEVEL UP!", level) : fmt::format("Keeper level {}", level), { { x, y }, { width, LineHeight } },
	    { .flags = UiFlags::FontSize12 | (levelled ? UiFlags::ColorGold : UiFlags::ColorWhite) });
	DrawString(out, fmt::format("+{} XP", MapXpEarned()), { { x, y }, { width, LineHeight } }, { .flags = UiFlags::FontSize12 | UiFlags::AlignRight | UiFlags::ColorGold });
	y += LineHeight + 1;
	const int from = XpForKeeperLevel(level);
	const int to = XpForKeeperLevel(level + 1);
	const Rectangle bar { { x, y }, { width, 10 } };
	FillRect(out, bar.position.x, bar.position.y, bar.size.width, bar.size.height, PAL16_GRAY + 13);
	FillRect(out, bar.position.x, bar.position.y, width * std::clamp(ProfileXp() - from, 0, to - from) / std::max(to - from, 1), bar.size.height, PAL16_YELLOW + 4);
	DrawFrame(out, bar, PAL16_GRAY + 9);
	return y + bar.size.height + 4;
}

} // namespace

void ForgetMapSummary()
{
	report = std::nullopt;
}

Rectangle MapSummaryRect(const Surface &out)
{
	const int width = std::min(out.w() - 2 * Margin, MaxWidth);
	const Report &info = GetReport();
	const int inner = width - 2 * Margin;
	int content = 8 + 36 + LineHeight + 6 + PortraitHeight + 6;
	if (State.phase == Phase::Defeat) {
		content += CountLinesOfText(FitLines(info.advice.why, inner, 2 * LineHeight, LineHeight)) * LineHeight + 4 + LineHeight + 4;
		for (const std::string &advice : info.advice.tryNext)
			content += CountLinesOfText(WordWrapString(advice, inner - 10)) * LineHeight;
	}
	if (RenownAvailable() > 0)
		content += LineHeight + 1 + (info.unlocks.empty() ? 0 : UnlockCardHeight + 4);
	content += LineHeight + 6 + ButtonHeight + Margin; // the next step and the buttons
	const int height = std::min({ content, out.h() - 2 * Margin, MaxHeight });
	// Over the view, not the bar, when it fits.
	const int hudless = out.h() - 2 * Margin;
	const int y = std::max((std::min(hudless, out.h() - HudBarAllowance) - height) / 2, Margin);
	return { { (out.w() - width) / 2, y }, { width, height } };
}

Rectangle MapSummaryButton(const Surface &out, int index, int count)
{
	const Rectangle panel = MapSummaryRect(out);
	const int width = std::min((panel.size.width - 2 * Margin - (count - 1) * ButtonGap) / count, ButtonMaxWidth);
	const int row = count * width + (count - 1) * ButtonGap;
	return { { panel.position.x + (panel.size.width - row) / 2 + index * (width + ButtonGap), panel.position.y + panel.size.height - ButtonHeight - Margin }, { width, ButtonHeight } };
}

void DrawMapSummary(const Surface &out, Point mouse, const std::string &continueLabel, const std::string &leaveLabel)
{
	const Report &info = GetReport();
	const bool won = State.phase == Phase::Victory;
	const Rectangle panel = MapSummaryRect(out);
	DrawDungeonPanel(out, panel);

	const int x = panel.position.x + Margin;
	const int inner = panel.size.width - 2 * Margin;
	int y = panel.position.y + 8;
	DrawString(out, won ? "THE KEEP STANDS" : "THE KEEP HAS FALLEN", { { panel.position.x, y }, { panel.size.width, 34 } },
	    { .flags = UiFlags::FontSize30 | UiFlags::AlignCenter | (won ? UiFlags::ColorGold : UiFlags::ColorRed) });
	y += 36;
	const std::string where = CurrentSetup().mode == GameMode::Campaign ? fmt::format("Level {}: {}", CurrentSetup().campaignLevel, CurrentRules().name) : std::string("Custom Map");
	DrawString(out, FitText(where, inner), { { panel.position.x, y }, { panel.size.width, LineHeight } }, { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | UiFlags::ColorWhite });
	y += LineHeight + 6;

	// The Lord on the left, the numbers beside him.
	const Rectangle portrait { { x, y }, { PortraitWidth, PortraitHeight } };
	FillRect(out, portrait.position.x, portrait.position.y, portrait.size.width, portrait.size.height, 0);
	DrawUiIcon(out.subregion(x + 2, y + 2, PortraitWidth - 4, PortraitHeight - 4), MonsterIcon(MT_NBLACK));
	DrawFrame(out, portrait, PAL16_YELLOW + 8);

	const MapTotals totals = GetMapTotals();
	const int statX = x + PortraitWidth + Margin;
	const int statWidth = inner - PortraitWidth - Margin;
	const int raids = won ? NumWaves() : std::max(static_cast<int>(State.wave) - 1, 0);
	const std::array<std::tuple<std::string_view, std::string, UiFlags>, 5> stats { {
		{ "Raids survived", fmt::format("{} of {}", raids, NumWaves()), won ? UiFlags::ColorGold : UiFlags::ColorWhite },
		{ "Heroes slain", std::to_string(totals.slain), UiFlags::ColorGold },
		{ "Heroes escaped", std::to_string(totals.escaped + totals.fled), totals.escaped > 0 ? UiFlags::ColorRed : UiFlags::ColorWhite },
		{ "Gold carried off", std::to_string(totals.goldCarriedOff), totals.goldCarriedOff > 0 ? UiFlags::ColorRed : UiFlags::ColorWhite },
		{ "Your monsters lost", std::to_string(totals.monstersLost), totals.monstersLost > 0 ? UiFlags::ColorRed : UiFlags::ColorWhite },
	} };
	int statY = y;
	for (const auto &[label, value, color] : stats) {
		DrawString(out, label, { { statX, statY }, { statWidth, LineHeight } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorWhite });
		DrawString(out, value, { { statX, statY }, { statWidth, LineHeight } }, { .flags = UiFlags::FontSize12 | UiFlags::AlignRight | color });
		statY += LineHeight;
	}
	statY += 2;
	DrawProgress(out, statX, statY, statWidth);
	y += PortraitHeight + 6;

	const int bottom = MapSummaryButton(out, 0, 1).position.y - 18;

	if (!won) {
		// Why, and something to try.
		const std::string why = FitLines(info.advice.why, inner, 2 * LineHeight, LineHeight);
		DrawString(out, why, { { x, y }, { inner, 2 * LineHeight } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorRed, .lineHeight = LineHeight });
		y += CountLinesOfText(why) * LineHeight + 4;
		DrawString(out, "Try this next time", { { x, y }, { inner, LineHeight } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorGold });
		y += LineHeight;
		size_t linesLeft = MaxAdviceLines + 1;
		for (const std::string &advice : info.advice.tryNext) {
			const std::string text = WordWrapString(advice, inner - 10);
			const size_t lines = static_cast<size_t>(CountLinesOfText(text));
			if (lines > linesLeft)
				break;
			linesLeft -= lines;
			DrawString(out, ">", { { x, y }, { 10, LineHeight } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorGold });
			DrawString(out, text, { { x + 10, y }, { inner - 10, static_cast<int>(lines) * LineHeight } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorWhite, .lineHeight = LineHeight });
			y += static_cast<int>(lines) * LineHeight;
		}
		y += 4;
	}

	// What the map's XP can buy now.
	const int renown = RenownAvailable();
	const bool showCards = !info.unlocks.empty() && y + LineHeight + UnlockCardHeight <= bottom;
	if (renown > 0) {
		DrawString(out, showCards ? fmt::format("{} Renown to spend. New within reach:", renown) : fmt::format("{} Renown to spend on the Keeper screen", renown), { { x, y }, { inner, LineHeight } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorGold });
		y += LineHeight + 1;
		if (showCards) {
			const int fits = std::max((inner + UnlockGap) / (UnlockCardWidth + UnlockGap), 1);
			const int count = std::min(static_cast<int>(info.unlocks.size()), fits);
			const int row = count * UnlockCardWidth + (count - 1) * UnlockGap;
			for (int i = 0; i < count; i++) {
				DrawUnlockCard(out, { { x + (inner - row) / 2 + i * (UnlockCardWidth + UnlockGap), y }, { UnlockCardWidth, UnlockCardHeight } }, info.unlocks[i]);
			}
		}
	}

	// The next step.
	std::string next;
	if (IsJoinedGame())
		next = renown > 0 ? "The host chooses what is next. Spend Renown from the main menu's Keeper screen." : "The host chooses what is next.";
	else if (!continueLabel.empty() && renown > 0 && !IsEditorTestRun())
		next = gbIsMultiplayer ? "Spend Renown from the main menu's Keeper screen." : "The Keeper screen comes next: spend your Renown there.";
	if (!next.empty())
		DrawString(out, FitText(next, inner), { { panel.position.x, bottom }, { panel.size.width, LineHeight } }, { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | UiFlags::ColorWhite });

	const int count = continueLabel.empty() ? 1 : 2;
	const auto drawButton = [&](int index, std::string_view label, bool primary) {
		const Rectangle button = MapSummaryButton(out, index, count);
		FillRect(out, button.position.x, button.position.y, button.size.width, button.size.height, primary ? PAL16_RED + 12 : PAL16_GRAY + 13);
		DrawFrame(out, button, button.contains(mouse) ? PAL16_YELLOW + 2 : (primary ? PAL16_YELLOW + 4 : PAL16_GRAY + 9));
		DrawString(out, FitText(label, button.size.width - 4), { button.position + Displacement { 0, 5 }, { button.size.width, LineHeight } },
		    { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | (primary || button.contains(mouse) ? UiFlags::ColorGold : UiFlags::ColorWhite) });
	};
	if (!continueLabel.empty())
		drawButton(0, continueLabel, true);
	drawButton(count - 1, leaveLabel, continueLabel.empty());
}

} // namespace devilution::keeper
