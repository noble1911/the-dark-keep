/**
 * @file keeper/keeper_hall.cpp
 *
 * The Keeper screen: the character panel, the monsters, spells and traps to unlock with Renown, and a
 * preview of what is selected. It draws itself rather than using DiabloUI's list items, in a dungeon
 * palette so monsters and spell icons show their true colours. The next menu screen restores the menu's.
 */
#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifdef USE_SDL3
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_timer.h>
#else
#include <SDL.h>
#endif

#include <fmt/format.h>

#include "DiabloUI/diabloui.h"
#include "controls/control_mode.hpp"
#include "controls/controller.h"
#include "controls/controller_motion.h"
#include "controls/game_controls.h"
#include "controls/input.h"
#include "controls/plrctrls.h"
#include "engine/dx.h"
#include "engine/palette.h"
#include "engine/point.hpp"
#include "engine/rectangle.hpp"
#include "engine/render/clx_render.hpp"
#include "engine/render/primitive_render.hpp"
#include "engine/render/text_render.hpp"
#include "engine/surface.hpp"
#include "keeper/keeper_game.hpp"
#include "keeper/keeper_menu.hpp"
#include "keeper/keeper_pad.hpp"
#include "keeper/keeper_profile.hpp"
#include "keeper/keeper_roster.hpp"
#include "keeper/keeper_tutorial.hpp"
#include "keeper/keeper_ui.hpp"
#include "panels/spell_icons.hpp"
#include "utils/display.h"
#include "utils/sdl_compat.h"

namespace devilution::keeper {

namespace {

constexpr int Margin = 10;
constexpr int CardWidth = 108;
/** Narrower than this and a family's tiers wrap onto more lines instead. */
constexpr int MinCardWidth = 76;
constexpr int CardHeight = 104;
constexpr int CardImageHeight = 66;
constexpr int ArrowWidth = 14;
constexpr int FamilyTitleHeight = 16;
constexpr int RowGap = 8;
constexpr int TabHeight = 26;
constexpr int ButtonHeight = 26;
constexpr int PreviewHeight = 170;
constexpr int LadderRowHeight = 16;

// The Monsters tab packs families into blocks, as many across as fit.
/** Narrowest a monster card may get before a family's tiers wrap onto a second line. */
constexpr int MonsterCardMinWidth = 72;
constexpr int MonsterCardMaxWidth = 96;
constexpr int MonsterCardHeight = 68;
constexpr int MonsterArrowWidth = 10;
constexpr int MonsterTitleHeight = 13;
constexpr int MonsterRowGap = 2;
constexpr int MonsterColumnGap = 8;
/** Room for a family's name and count above a one-card block. */
constexpr int MonsterTitleMinWidth = 84;
constexpr int GroupHeadingHeight = 15;
constexpr int ChipHeight = 20;
constexpr int ChipGap = 3;

// Index 255 (PAL16_GRAY + 15) is white in the dungeon palettes.
constexpr uint8_t PanelColor = PAL16_GRAY + 14;
constexpr uint8_t CardColor = PAL16_GRAY + 13;
constexpr uint8_t FrameColor = PAL16_GRAY + 9;
constexpr uint8_t GoldFrame = PAL16_YELLOW + 2;
constexpr uint8_t RedFrame = PAL16_RED + 6;

enum class Tab : uint8_t {
	Monsters,
	Spells,
	Traps,
};
constexpr std::array<std::string_view, 3> TabNames { "Monsters", "Spells", "Traps" };

/** Which families the Monsters tab lists: those with a tier that matches. */
enum class Filter : uint8_t {
	All,
	Owned,
	/** A tier that reads "Unlock": it can be bought this moment. */
	Afford,
	/** A tier that reads "Locked": not yours, and not yet buyable. */
	Locked,
};
constexpr std::array<std::string_view, 4> FilterNames { "All", "Owned", "Can afford", "Locked" };

struct Selection {
	Tab tab = Tab::Monsters;
	size_t index = 0;
};

Tab tab = Tab::Monsters;
Selection selected;
std::array<int, 3> scroll {};
bool endScreen = false;
bool clicked = false;
int wheel = 0;
bool confirmRespec = false;
Filter filter = Filter::All;
/** A jump button was pressed: the list scrolls to the start of this group when it next lays out. */
std::optional<DungeonGroup> pendingJump;
/** The group at the top of the list, for the jump buttons to light up. */
std::optional<DungeonGroup> topGroup;
/** A jump that couldn't bring its group to the top: it stays lit until the list moves. */
std::optional<DungeonGroup> jumpedGroup;
int jumpedOffset = 0;
std::string message;
uint32_t messageUntil = 0;

std::array<KindImage, NumLairKinds> cardImages;
/** The box each card image was made for; it is made again if the layout changes the box. */
std::array<std::pair<int, int>, NumLairKinds> cardImageBoxes {};
KindImage keeperImage;

// ---- controller ------------------------------------------------------------------------------
//
// Each frame lists what can be pressed. The focus is the mouse position, snapped to one of them, and
// pressing is a click there: the screen can't tell the difference.

/** Height of the line of prompts under the list. */
constexpr int PadPromptHeight = 18;
/** Room for a family's title when a card scrolls into view. */
constexpr int PadScrollMargin = FamilyTitleHeight + 2;

enum class PadPress : uint8_t {
	None,
	Confirm,
	Equip,
	Respec,
	PrevTab,
	NextTab,
};

std::vector<Rectangle> padTargets;
/** The cards among them, which show their details as soon as the focus lands on them. */
std::vector<bool> padIsCard;
/** Whether it is the card the details are showing, and so the one the buy button is for. */
std::vector<bool> padIsSelected;
/** Whether it scrolls with the list: the spell bar's slots don't. */
std::vector<bool> padInList;
std::array<Rectangle, TabNames.size()> padTabs {};
std::optional<Rectangle> padPrimary;
std::optional<Rectangle> padEquip;
Rectangle padRespec {};
Rectangle padList {};
Rectangle padFocus {};
PadPress padPress = PadPress::None;
bool padFocusFirstCard = false;
AxisDirectionRepeater padRepeater;

void PadAddTarget(const Rectangle &rect)
{
	padTargets.push_back(rect);
	padIsCard.push_back(false);
	padIsSelected.push_back(false);
	padInList.push_back(false);
}

void PadAddCard(const Rectangle &rect, bool isSelected, bool inList)
{
	padTargets.push_back(rect);
	padIsCard.push_back(true);
	padIsSelected.push_back(isSelected);
	padInList.push_back(inList);
}

// ---- drawing helpers -------------------------------------------------------------------------

void Fill(const Surface &out, const Rectangle &rect, uint8_t color)
{
	for (int y = std::max(rect.position.y, 0); y < std::min(rect.position.y + rect.size.height, out.h()); y++) {
		const int x = std::max(rect.position.x, 0);
		const int width = std::min(rect.position.x + rect.size.width, out.w()) - x;
		if (width > 0)
			std::memset(out.at(x, y), color, width);
	}
}

void Frame(const Surface &out, const Rectangle &rect, uint8_t color)
{
	DrawHorizontalLine(out, rect.position, rect.size.width, color);
	DrawHorizontalLine(out, rect.position + Displacement { 0, rect.size.height - 1 }, rect.size.width, color);
	DrawVerticalLine(out, rect.position, rect.size.height, color);
	DrawVerticalLine(out, rect.position + Displacement { rect.size.width - 1, 0 }, rect.size.height, color);
}

bool Inside(const Rectangle &rect, Point p)
{
	return p.x >= rect.position.x && p.y >= rect.position.y && p.x < rect.position.x + rect.size.width && p.y < rect.position.y + rect.size.height;
}

/** Cuts `text` down until it fits in `width` pixels, marking the cut. */
std::string Fit(std::string_view text, int width)
{
	std::string fitted(text);
	if (GetLineWidth(fitted) <= width)
		return fitted;
	while (!fitted.empty() && GetLineWidth(fitted + ".") > width)
		fitted.pop_back();
	return fitted + ".";
}

/**
 * Like Fit, but for names where the last word is what tells them apart ("Burning Axe", "Burning Bow"): the
 * longest word loses a letter at a time instead of the end of the line.
 */
std::string FitName(std::string_view text, int width)
{
	std::vector<std::string> words;
	for (size_t start = 0; start <= text.size();) {
		const size_t end = std::min(text.find(' ', start), text.size());
		words.emplace_back(text.substr(start, end - start));
		start = end + 1;
	}
	const auto join = [&]() {
		std::string line;
		for (const std::string &word : words)
			line += (line.empty() ? "" : " ") + word;
		return line;
	};
	while (GetLineWidth(join()) > width) {
		std::string *longest = nullptr;
		for (std::string &word : words) {
			if (word.size() > 2 && (longest == nullptr || word.size() > longest->size()))
				longest = &word;
		}
		if (longest == nullptr)
			return Fit(text, width);
		if (longest->back() == '.')
			longest->pop_back();
		longest->pop_back();
		longest->push_back('.');
	}
	return join();
}

void Panel(const Surface &out, const Rectangle &rect)
{
	Fill(out, rect, PanelColor);
	Frame(out, rect, PAL16_RED + 10);
	Frame(out, { rect.position + Displacement { 1, 1 }, { rect.size.width - 2, rect.size.height - 2 } }, PAL16_GRAY + 12);
}

bool Button(const Surface &out, const Rectangle &rect, std::string_view label, bool enabled, Point mouse)
{
	const bool hover = Inside(rect, mouse);
	if (enabled)
		PadAddTarget(rect);
	Fill(out, rect, enabled ? PAL16_RED + 12 : PAL16_GRAY + 13);
	Frame(out, rect, enabled && hover ? PAL16_YELLOW + 2 : FrameColor);
	DrawString(out, Fit(label, rect.size.width - 4), { rect.position + Displacement { 0, 6 }, { rect.size.width, 16 } },
	    { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | (enabled ? UiFlags::ColorGold : UiFlags::ColorBlack) });
	return hover && clicked;
}

void SpellIcon(const Surface &out, const Rectangle &area, SpellID icon)
{
	SetSpellTrans(SpellType::Spell);
	DrawLargeSpellIcon(out, { area.position.x + (area.size.width - SPLICONLENGTH) / 2, area.position.y + (area.size.height + SPLICONLENGTH) / 2 }, icon);
}

void Flash(std::string text)
{
	message = std::move(text);
	messageUntil = SDL_GetTicks() + 3000;
}

// ---- what is on each tab ------------------------------------------------------------------------

/** What a card shows, whichever tab it is on. */
struct CardInfo {
	std::string name;
	bool owned = false;
	bool canBuy = false;
	/** Shown instead of a state line: "Lv 3", or "Req 7". */
	std::string corner;
};

CardInfo MonsterInfo(size_t kind)
{
	const bool owned = OwnsKind(kind);
	return { std::string(BuildKinds[kind].name), owned, CanUnlockKind(kind), owned ? std::string() : fmt::format("{}R", KindUnlockCost(kind)) };
}

CardInfo SpellInfo(size_t spell)
{
	const int level = OwnedSpellLevel(spell);
	return { std::string(SpellKinds[spell].name), level > 0, CanRaiseSpell(spell), level > 0 ? fmt::format("Lv {}", level) : fmt::format("{}R", SpellNextCost(spell)) };
}

CardInfo TrapInfo(size_t trap)
{
	const int level = OwnedTrapLevel(trap);
	return { std::string(TrapKinds[trap].name), level > 0, CanRaiseTrap(trap), level > 0 ? fmt::format("Lv {}", level) : fmt::format("{}R", TrapNextCost(trap)) };
}

/** Draws a whole card on `out`, which is exactly its size. */
void DrawCardBody(const Surface &out, const CardInfo &info, bool isSelected, Point mouse, const std::optional<size_t> &kind, std::optional<SpellID> icon)
{
	const Rectangle rect { { 0, 0 }, { out.w(), out.h() } };
	Fill(out, rect, CardColor);
	// The monsters' cards are short; the spells' and traps' keep the roomy layout.
	const bool compact = out.h() < 100;
	const int imageHeight = compact ? out.h() - 33 : CardImageHeight;
	const int nameY = compact ? imageHeight + 5 : CardImageHeight + 8;
	const int stateY = nameY + (compact ? 13 : 13);
	const Rectangle image { rect.position + Displacement { 3, 3 }, { rect.size.width - 6, imageHeight } };
	Fill(out, image, 0);
	if (kind) {
		KindImage &card = cardImages[*kind];
		const std::pair<int, int> box { image.size.width, image.size.height };
		if (!card.built || cardImageBoxes[*kind] != box) {
			card = MakeKindImage(*kind, image.size.width, image.size.height);
			cardImageBoxes[*kind] = box;
		}
		DrawKindImage(out.subregion(image.position.x, image.position.y, image.size.width, image.size.height), card);
	} else if (icon) {
		SpellIcon(out.subregion(image.position.x, image.position.y, image.size.width, image.size.height), { { 0, 0 }, image.size }, *icon);
	}
	if (!info.owned) {
		// Not yet yours: a shadow of itself, darker still if it can't be bought yet.
		DrawHalfTransparentRectTo(out, image.position.x, image.position.y, image.size.width, image.size.height);
		if (!info.canBuy)
			DrawHalfTransparentRectTo(out, image.position.x, image.position.y, image.size.width, image.size.height);
	}
	if (!info.corner.empty()) {
		DrawString(out, info.corner, { image.position + Displacement { 2, 1 }, { image.size.width - 4, 14 } },
		    { .flags = UiFlags::FontSize12 | UiFlags::AlignRight | UiFlags::Outlined | (info.owned ? UiFlags::ColorWhite : (info.canBuy ? UiFlags::ColorGold : UiFlags::ColorRed)) });
	}
	DrawString(out, compact ? FitName(info.name, rect.size.width - 4) : Fit(info.name, rect.size.width - 4), { rect.position + Displacement { 0, nameY }, { rect.size.width, 14 } },
	    { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | (info.owned ? UiFlags::ColorGold : (info.canBuy ? UiFlags::ColorWhite : UiFlags::ColorUiSilverDark)) });
	const std::string_view state = info.owned ? "Owned" : (info.canBuy ? "Unlock" : "Locked");
	DrawString(out, state, { rect.position + Displacement { 0, stateY }, { rect.size.width, 14 } },
	    { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | (info.owned ? UiFlags::ColorWhite : (info.canBuy ? UiFlags::ColorGold : UiFlags::ColorUiSilverDark)) });

	uint8_t frame = info.owned ? PAL16_YELLOW + 6 : (info.canBuy ? PAL16_ORANGE + 4 : FrameColor);
	if (Inside(rect, mouse))
		frame = PAL16_GRAY;
	Frame(out, rect, frame);
	if (isSelected) {
		Frame(out, rect, GoldFrame);
		Frame(out, { rect.position + Displacement { 1, 1 }, { rect.size.width - 2, rect.size.height - 2 } }, GoldFrame);
	}
}

/**
 * Cards scroll partly out of the list, and a subregion can't reach outside its surface, so each is
 * drawn whole off-screen and only the part inside `out` is copied.
 */
void DrawCard(const Surface &out, const Rectangle &rect, const CardInfo &info, bool isSelected, Point mouse, const std::optional<size_t> &kind, std::optional<SpellID> icon)
{
	if (rect.position.x >= out.w() || rect.position.y >= out.h() || rect.position.x + rect.size.width <= 0 || rect.position.y + rect.size.height <= 0)
		return;
	static std::optional<OwnedSurface> surface;
	if (!surface || surface->w() != rect.size.width || surface->h() != rect.size.height)
		surface.emplace(rect.size.width, rect.size.height);
	const OwnedSurface &card = *surface;
	DrawCardBody(card, info, isSelected, mouse - Displacement { rect.position.x, rect.position.y }, kind, icon);
	const int left = std::max(rect.position.x, 0);
	const int right = std::min(rect.position.x + rect.size.width, out.w());
	const int top = std::max(rect.position.y, 0);
	const int bottom = std::min(rect.position.y + rect.size.height, out.h());
	for (int y = top; y < bottom; y++)
		std::memcpy(out.at(left, y), card.at(left - rect.position.x, y - rect.position.y), right - left);
}

/** Scrolls a tall list inside `area`; returns how far down it is drawn. */
int ApplyScroll(Tab which, int contentHeight, const Rectangle &area, Point mouse, int wheelStep = CardHeight + FamilyTitleHeight + RowGap)
{
	int &offset = scroll[static_cast<size_t>(which)];
	if (Inside(area, mouse))
		offset -= wheel * wheelStep;
	offset = std::clamp(offset, 0, std::max(contentHeight - area.size.height, 0));
	return offset;
}

void DrawScrollBar(const Surface &out, const Rectangle &area, int contentHeight, int offset)
{
	if (contentHeight <= area.size.height)
		return;
	const Rectangle track { { area.position.x + area.size.width - 5, area.position.y }, { 4, area.size.height } };
	Fill(out, track, PAL16_GRAY + 13);
	const int thumb = std::max(area.size.height * area.size.height / contentHeight, 16);
	const int top = (area.size.height - thumb) * offset / std::max(contentHeight - area.size.height, 1);
	Fill(out, { { track.position.x, track.position.y + top }, { 4, thumb } }, PAL16_YELLOW + 4);
}

/** One family: its tiers in a chain, each of which has to be unlocked before the next. */
struct FamilyRow {
	size_t family;
	DungeonGroup group;
	std::vector<size_t> kinds;
};

/** Every family the game offers, grouped by the dungeon they come from; within a group the roster's order stands. */
const std::vector<FamilyRow> &FamilyRows()
{
	static std::vector<FamilyRow> rows;
	if (rows.empty()) {
		for (size_t kind = 0; kind < NumLairKinds; kind++) {
			if (!IsKindOnRoster(kind))
				continue;
			const auto family = static_cast<size_t>(FamilyOfKind(kind));
			if (rows.empty() || rows.back().family != family)
				rows.push_back({ family, GroupOfFamily(static_cast<KindFamily>(family)), {} });
			rows.back().kinds.push_back(kind);
		}
		std::stable_sort(rows.begin(), rows.end(), [](const FamilyRow &a, const FamilyRow &b) { return a.group < b.group; });
	}
	return rows;
}

bool KindMatchesFilter(size_t kind)
{
	switch (filter) {
	case Filter::All:
		return true;
	case Filter::Owned:
		return OwnsKind(kind);
	case Filter::Afford:
		return !OwnsKind(kind) && CanUnlockKind(kind);
	case Filter::Locked:
		return !OwnsKind(kind) && !CanUnlockKind(kind);
	}
	return true;
}

/** The families with a tier that matches the filter. */
std::vector<const FamilyRow *> VisibleFamilies()
{
	std::vector<const FamilyRow *> visible;
	for (const FamilyRow &row : FamilyRows()) {
		if (std::any_of(row.kinds.begin(), row.kinds.end(), KindMatchesFilter))
			visible.push_back(&row);
	}
	return visible;
}

/** A small button of the bar above the Monsters list: a filter or a jump to a dungeon. */
struct Chip {
	Rectangle rect; // relative to the bar
	std::string label;
	bool isFilter;
	size_t index;
};

/** Filters, then jump buttons for the dungeons that have a family on show, wrapped to `width`. @return the bar's height */
int LayoutBar(int width, std::vector<Chip> &chips)
{
	chips.clear();
	std::array<bool, NumDungeonGroups> present {};
	for (const FamilyRow *row : VisibleFamilies())
		present[static_cast<size_t>(row->group)] = true;

	int x = 0;
	int y = 0;
	const auto add = [&](std::string label, bool isFilter, size_t index) {
		const int w = GetLineWidth(label) + 12;
		if (x > 0 && x + w > width) {
			x = 0;
			y += ChipHeight + ChipGap;
		}
		chips.push_back({ { { x, y }, { w, ChipHeight } }, std::move(label), isFilter, index });
		x += w + ChipGap;
	};
	for (size_t i = 0; i < FilterNames.size(); i++)
		add(std::string(FilterNames[i]), true, i);
	x += 8;
	for (size_t i = 0; i < NumDungeonGroups; i++) {
		if (present[i])
			add(std::string(GroupName(static_cast<DungeonGroup>(i))), false, i);
	}
	return y + ChipHeight;
}

void DrawBar(const Surface &out, const std::vector<Chip> &chips, Point origin, Point mouse)
{
	for (const Chip &chip : chips) {
		const Rectangle rect { origin + Displacement { chip.rect.position.x, chip.rect.position.y }, chip.rect.size };
		const bool active = chip.isFilter ? static_cast<size_t>(filter) == chip.index : (topGroup && static_cast<size_t>(*topGroup) == chip.index);
		PadAddTarget(rect);
		Fill(out, rect, active ? PAL16_RED + 12 : CardColor);
		Frame(out, rect, active ? PAL16_YELLOW + 4 : (Inside(rect, mouse) ? PAL16_GRAY : FrameColor));
		DrawString(out, chip.label, { rect.position + Displacement { 0, 3 }, { rect.size.width, 14 } },
		    { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | (active ? UiFlags::ColorGold : (chip.isFilter ? UiFlags::ColorWhite : UiFlags::ColorUiSilverDark)) });
		if (clicked && Inside(rect, mouse)) {
			if (chip.isFilter) {
				filter = static_cast<Filter>(chip.index);
				scroll[static_cast<size_t>(Tab::Monsters)] = 0;
				pendingJump = std::nullopt;
				jumpedGroup = std::nullopt;
			} else {
				pendingJump = static_cast<DungeonGroup>(chip.index);
			}
		}
	}
}

void DrawMonsters(const Surface &out, const Rectangle &area, Point mouse)
{
	const std::vector<const FamilyRow *> rows = VisibleFamilies();
	// Blocks across if four tiers fit in each at a readable size; otherwise one block and its tiers wrap.
	const int listWidth = area.size.width - 7 - 2;
	size_t mostTiers = 1;
	for (const FamilyRow &row : FamilyRows())
		mostTiers = std::max(mostTiers, row.kinds.size());
	const int tiers = static_cast<int>(mostTiers);
	const int minBlock = tiers * MonsterCardMinWidth + (tiers - 1) * MonsterArrowWidth;
	const int columns = std::max((listWidth + MonsterColumnGap) / (minBlock + MonsterColumnGap), 1);
	const int blockWidth = (listWidth - (columns - 1) * MonsterColumnGap) / columns;
	const int cardWidth = std::clamp((blockWidth - (tiers - 1) * MonsterArrowWidth) / tiers, MonsterCardMinWidth, MonsterCardMaxWidth);
	const int mostPerLine = std::max((listWidth + MonsterArrowWidth) / (cardWidth + MonsterArrowWidth), 1);

	// Blocks flow left to right, each as wide as its tiers: families with one or two tiers pack closer.
	struct Placed {
		const FamilyRow *row;
		int x;
		int y;
		int width;
		int perLine;
		int height;
	};
	struct Heading {
		DungeonGroup group;
		int y;
	};
	std::vector<Placed> placed;
	std::vector<Heading> headings;
	int y = 0;
	int x = 0;
	int column = 0;
	int rowHeight = 0;
	const auto endGridRow = [&]() {
		if (column > 0)
			y += rowHeight + MonsterRowGap;
		column = 0;
		x = 0;
		rowHeight = 0;
	};
	for (size_t i = 0; i < rows.size(); i++) {
		if (i == 0 || rows[i]->group != rows[i - 1]->group) {
			endGridRow();
			headings.push_back({ rows[i]->group, y });
			y += GroupHeadingHeight;
		}
		const int count = static_cast<int>(rows[i]->kinds.size());
		const int perLine = std::min(count, mostPerLine);
		const int lines = (count + perLine - 1) / perLine;
		const int width = std::max(perLine * cardWidth + (perLine - 1) * MonsterArrowWidth, MonsterTitleMinWidth);
		const int height = MonsterTitleHeight + lines * MonsterCardHeight + (lines - 1) * MonsterRowGap;
		if (column > 0 && x + width > listWidth)
			endGridRow();
		placed.push_back({ rows[i], x, y, width, perLine, height });
		rowHeight = std::max(rowHeight, height);
		x += width + MonsterColumnGap;
		column++;
	}
	endGridRow();
	const int content = y;

	int offset = ApplyScroll(Tab::Monsters, content, area, mouse, MonsterCardHeight + MonsterTitleHeight + MonsterRowGap);
	if (pendingJump) {
		for (const Heading &heading : headings) {
			if (heading.group == *pendingJump)
				offset = scroll[static_cast<size_t>(Tab::Monsters)] = std::clamp(heading.y, 0, std::max(content - area.size.height, 0));
		}
		// The last groups can't reach the top of the list: they still count as where it is.
		jumpedGroup = pendingJump;
		jumpedOffset = offset;
		pendingJump = std::nullopt;
	}
	if (jumpedGroup && jumpedOffset != offset)
		jumpedGroup = std::nullopt;
	topGroup = jumpedGroup;
	for (const Heading &heading : headings) {
		if (!jumpedGroup && heading.y <= offset + 4)
			topGroup = heading.group;
	}

	const Surface clip = out.subregion(area.position.x, area.position.y, area.size.width - 7, area.size.height);
	const Point localMouse { mouse.x - area.position.x, mouse.y - area.position.y };
	const bool mouseInArea = Inside(area, mouse);

	if (rows.empty()) {
		DrawString(clip, "No family matches this filter.", { { 4, 8 }, { clip.w() - 8, 16 } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorUiSilverDark });
		return;
	}

	for (const Heading &heading : headings) {
		const int top = heading.y - offset;
		if (top + GroupHeadingHeight <= 0 || top >= clip.h())
			continue;
		DrawString(clip, GroupName(heading.group), { { 2, top }, { clip.w() - 4, 14 } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorWhite });
		Fill(clip, { { 2, top + 14 }, { clip.w() - 4, 1 } }, FrameColor);
	}

	for (const Placed &block : placed) {
		const FamilyRow &row = *block.row;
		const int top = block.y - offset;
		const int perLine = block.perLine;
		const bool onShow = top + block.height > 0 && top < clip.h();
		// Every card is a target, on show or not: it's how the focus scrolls the list to them.
		for (size_t t = 0; t < row.kinds.size(); t++) {
			const Point at { 2 + block.x + static_cast<int>(t % perLine) * (cardWidth + MonsterArrowWidth), top + MonsterTitleHeight + static_cast<int>(t / perLine) * (MonsterCardHeight + MonsterRowGap) };
			PadAddCard({ area.position + Displacement { at.x, at.y }, { cardWidth, MonsterCardHeight } }, selected.tab == Tab::Monsters && selected.index == row.kinds[t], /*inList=*/true);
		}
		if (!onShow)
			continue;
		const int owned = static_cast<int>(std::count_if(row.kinds.begin(), row.kinds.end(), OwnsKind));
		DrawString(clip, Fit(FamilyName(static_cast<KindFamily>(row.family)), block.width - 30), { { 2 + block.x, top }, { block.width, 14 } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorGold });
		DrawString(clip, fmt::format("{}/{}", owned, row.kinds.size()), { { 2 + block.x, top }, { block.width - 2, 14 } },
		    { .flags = UiFlags::FontSize12 | UiFlags::AlignRight | (owned > 0 ? UiFlags::ColorWhite : UiFlags::ColorUiSilverDark) });
		for (size_t t = 0; t < row.kinds.size(); t++) {
			const size_t kind = row.kinds[t];
			const int x = 2 + block.x + static_cast<int>(t % perLine) * (cardWidth + MonsterArrowWidth);
			const int cardY = top + MonsterTitleHeight + static_cast<int>(t / perLine) * (MonsterCardHeight + MonsterRowGap);
			const Rectangle card { { x, cardY }, { cardWidth, MonsterCardHeight } };
			const bool isSelected = selected.tab == Tab::Monsters && selected.index == kind;
			DrawCard(clip, card, MonsterInfo(kind), isSelected, mouseInArea ? localMouse : Point { -1, -1 }, kind, std::nullopt);
			if (clicked && mouseInArea && Inside(card, localMouse))
				selected = { Tab::Monsters, kind };
			if (t + 1 < row.kinds.size() && (t + 1) % perLine != 0) {
				const int arrowY = cardY + MonsterCardHeight / 2 - 8;
				if (arrowY >= 0 && arrowY + 16 <= clip.h()) {
					DrawString(clip, ">", { { x + cardWidth, arrowY }, { MonsterArrowWidth, 16 } },
					    { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | (OwnsKind(kind) ? UiFlags::ColorGold : UiFlags::ColorUiSilverDark) });
				}
			}
		}
	}
	DrawScrollBar(out, area, content, offset);
}

void DrawSpellBarRow(const Surface &out, const Rectangle &area, Point mouse)
{
	DrawString(out, "Your spell bar", { area.position, { area.size.width, 14 } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorGold });
	const std::array<int8_t, NumSpellSlots> &bar = ProfileSpellBar();
	const int size = std::min(SPLICONLENGTH + 4, (area.size.width - 4) / static_cast<int>(NumSpellSlots));
	for (size_t i = 0; i < NumSpellSlots; i++) {
		const Rectangle slot { area.position + Displacement { static_cast<int>(i) * (size + 2), 16 }, { size, size } };
		Fill(out, slot, 0);
		if (bar[i] >= 0) {
			SpellIcon(out.subregion(slot.position.x, slot.position.y, slot.size.width, slot.size.height), { { 0, 0 }, slot.size }, SpellKinds[static_cast<size_t>(bar[i])].icon);
			PadAddCard(slot, selected.tab == Tab::Spells && selected.index == static_cast<size_t>(bar[i]), /*inList=*/false);
			if (clicked && Inside(slot, mouse))
				selected = { Tab::Spells, static_cast<size_t>(bar[i]) };
		}
		Frame(out, slot, bar[i] >= 0 ? PAL16_YELLOW + 6 : FrameColor);
	}
}

/** Spells and traps: a grid of cards. */
void DrawGrid(const Surface &out, const Rectangle &area, Point mouse)
{
	const bool spells = tab == Tab::Spells;
	const size_t count = spells ? NumSpellKinds : NumTrapKinds;
	const int columns = std::max((area.size.width - 7) / (CardWidth + 6), 1);
	const int content = (static_cast<int>(count) + columns - 1) / columns * (CardHeight + RowGap);
	const int offset = ApplyScroll(tab, content, area, mouse);
	const Surface clip = out.subregion(area.position.x, area.position.y, area.size.width - 7, area.size.height);
	const Point localMouse { mouse.x - area.position.x, mouse.y - area.position.y };
	const bool mouseInArea = Inside(area, mouse);
	for (size_t i = 0; i < count; i++) {
		const int column = static_cast<int>(i) % columns;
		const int row = static_cast<int>(i) / columns;
		const Rectangle card { { 2 + column * (CardWidth + 6), row * (CardHeight + RowGap) - offset }, { CardWidth, CardHeight } };
		PadAddCard({ area.position + Displacement { card.position.x, card.position.y }, card.size }, selected.tab == tab && selected.index == i, /*inList=*/true);
		const bool isSelected = selected.tab == tab && selected.index == i;
		DrawCard(clip, card, spells ? SpellInfo(i) : TrapInfo(i), isSelected, mouseInArea ? localMouse : Point { -1, -1 }, std::nullopt, spells ? SpellKinds[i].icon : TrapKinds[i].icon);
		if (clicked && mouseInArea && Inside(card, localMouse))
			selected = { tab, i };
	}
	DrawScrollBar(out, area, content, offset);
}

// ---- the panels ----------------------------------------------------------------------------------

void DrawCharacter(const Surface &out, const Rectangle &area, Point mouse)
{
	Panel(out, area);
	const int inner = area.size.width - 16;
	int y = area.position.y + 8;
	const int x = area.position.x + 8;
	DrawString(out, "The Keeper", { { x, y }, { inner, 30 } }, { .flags = UiFlags::FontSize24 | UiFlags::AlignCenter | UiFlags::ColorGold });
	y += 34;

	const Rectangle portrait { { x, y }, { inner, 130 } };
	Fill(out, portrait, 0);
	Frame(out, portrait, PAL16_YELLOW + 8);
	if (!keeperImage.built) {
		// The Keep Lord's own likeness.
		for (size_t kind = 0; kind < NumLairKinds; kind++) {
			if (BuildKinds[kind].type == MT_NBLACK) {
				keeperImage = MakeKindImage(kind, portrait.size.width - 8, portrait.size.height - 8);
				break;
			}
		}
	}
	DrawKindImage(out.subregion(x + 4, y + 4, portrait.size.width - 8, portrait.size.height - 8), keeperImage);
	y += portrait.size.height + 8;

	const int level = KeeperLevel();
	DrawString(out, fmt::format("Keeper level {}", level), { { x, y }, { inner, 16 } }, { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | UiFlags::ColorWhite });
	y += 17;
	const int from = XpForKeeperLevel(level);
	const int to = XpForKeeperLevel(level + 1);
	const Rectangle bar { { x, y }, { inner, 12 } };
	Fill(out, bar, PAL16_GRAY + 13);
	Fill(out, { bar.position, { inner * std::clamp(ProfileXp() - from, 0, to - from) / std::max(to - from, 1), bar.size.height } }, PAL16_YELLOW + 4);
	Frame(out, bar, FrameColor);
	y += 14;
	// A line that doesn't fit would wrap down over the Renown below it.
	std::string xpLine = fmt::format("{} / {} XP", ProfileXp(), to);
	if (GetLineWidth(xpLine) > inner)
		xpLine = Fit(fmt::format("{} XP", ProfileXp()), inner);
	DrawString(out, xpLine, { { x, y }, { inner, 16 } }, { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | UiFlags::ColorWhite });
	y += 22;

	DrawString(out, std::to_string(RenownAvailable()), { { x, y }, { inner, 30 } }, { .flags = UiFlags::FontSize30 | UiFlags::AlignCenter | UiFlags::ColorGold });
	y += 32;
	DrawString(out, "Renown to spend", { { x, y }, { inner, 16 } }, { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | UiFlags::ColorGold });
	y += 24;

	const ProfileStats &stats = GetProfileStats();
	const std::array<std::pair<std::string_view, int>, 6> lines { {
	    { "Maps cleared", stats.mapsCleared },
	    { "Maps lost", stats.mapsLost },
	    { "Raids survived", stats.raidsSurvived },
	    { "Heroes slain", stats.heroesSlain },
	    { "Monsters lost", stats.monstersLost },
	    { "Stars won", ProfileMedalCount() },
	} };
	for (const auto &[label, value] : lines) {
		DrawString(out, label, { { x, y }, { inner, 16 } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorWhite });
		DrawString(out, std::to_string(value), { { x, y }, { inner, 16 } }, { .flags = UiFlags::FontSize12 | UiFlags::AlignRight | UiFlags::ColorGold });
		y += 15;
	}

	const Rectangle difficultyButton { { x, area.position.y + area.size.height - 2 * ButtonHeight - 12 }, { inner, ButtonHeight } };
	if (Button(out, difficultyButton, fmt::format("Difficulty: {}", DifficultyName(ProfileDifficulty())), true, mouse)) {
		CycleProfileDifficulty();
		Flash(std::string(DifficultyBlurb(ProfileDifficulty())));
	}
	const Rectangle respec { { x, area.position.y + area.size.height - ButtonHeight - 8 }, { inner, ButtonHeight } };
	if (const std::string_view problem = ProfileProblem(); !problem.empty()) {
		const int bottom = respec.position.y - 2;
		const int room = std::min(bottom - y, 4 * 14);
		DrawString(out, WordWrapString(problem, inner), { { x, bottom - room }, { inner, room } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorRed, .lineHeight = 14 });
	}
	padRespec = respec;
	if (Button(out, respec, confirmRespec ? (IsPadInUse() ? "Press again to respec" : "Click again to respec") : "Respec (free)", true, mouse)) {
		if (confirmRespec) {
			RespecProfile();
			Flash("All your Renown is back");
		}
		confirmRespec = !confirmRespec;
	} else if (clicked) {
		confirmRespec = false;
	}
}

void DrawDetails(const Surface &out, const Rectangle &area, Point mouse)
{
	Panel(out, area);
	const int inner = area.size.width - 16;
	const int x = area.position.x + 8;
	int y = area.position.y + 8;

	std::string title;
	std::string subtitle;
	std::string body;
	std::string status;
	std::string action;
	bool canAct = false;

	if (selected.tab == Tab::Monsters) {
		const size_t kind = selected.index;
		const BuildInfo &info = BuildKinds[kind];
		title = std::string(info.name);
		subtitle = std::string(FamilyName(FamilyOfKind(kind)));
		body = fmt::format("Life {}   Hit {}-{}\nPop {}   Costs {} gold to place\n\n{}", KindLife(kind), KindMinDamage(kind), KindMaxDamage(kind), info.supply, info.cost, KindFlavor(kind));
		std::string_view reason;
		canAct = CanUnlockKind(kind, &reason);
		if (OwnsKind(kind)) {
			status = "You can build this.";
		} else {
			status = canAct ? std::string() : fmt::format("Needs keeper level {}, {} Renown. {}.", UnlockRankOf(kind), KindUnlockCost(kind), reason);
			action = fmt::format("Unlock: {} Renown", KindUnlockCost(kind));
		}
	} else {
		const bool spells = selected.tab == Tab::Spells;
		const size_t i = selected.index;
		const int level = spells ? OwnedSpellLevel(i) : OwnedTrapLevel(i);
		const int cost = spells ? SpellNextCost(i) : TrapNextCost(i);
		const int rank = spells ? SpellKinds[i].unlockRank : TrapKinds[i].unlockRank;
			title = std::string(spells ? SpellKinds[i].name : TrapKinds[i].name);
		subtitle = level > 0 ? fmt::format("Level {} of {}", level, MaxKeepLevel) : std::string("Not unlocked");
		body = spells
		    ? fmt::format("Costs {} mana\n\n{}", SpellManaCostAt(i, level), SpellKinds[i].flavor)
		    : fmt::format("Costs {} gold to place\n\n{}", TrapKinds[i].cost, TrapKinds[i].flavor);
		std::string_view reason;
		canAct = spells ? CanRaiseSpell(i, &reason) : CanRaiseTrap(i, &reason);
		if (level < MaxKeepLevel)
			action = fmt::format("{}: {} Renown", level == 0 ? "Unlock" : "Level up", cost);
		if (!canAct)
			status = level == 0 ? fmt::format("Needs keeper level {}. {}.", rank, reason) : fmt::format("{}.", reason);
	}
	// The family's ladder, under the text: every tier with its price and state. The picture gives up height for it when short.
	std::vector<size_t> ladder;
	if (selected.tab == Tab::Monsters) {
		for (size_t kind = 0; kind < NumLairKinds; kind++) {
			if (IsKindOnRoster(kind) && FamilyOfKind(kind) == FamilyOfKind(selected.index))
				ladder.push_back(kind);
		}
	}
	const int bottom = area.position.y + area.size.height - 8;
	const bool canEquip = selected.tab == Tab::Spells && OwnedSpellLevel(selected.index) > 0;
	const int buttonsTop = bottom - ButtonHeight * (canEquip ? 2 : 1) - (canEquip ? 4 : 0);
	const int statusHeight = status.empty() ? 0 : 42;
	const std::string wrapped = WordWrapString(body, inner);
	const int bodyLines = 1 + static_cast<int>(std::count(wrapped.begin(), wrapped.end(), '\n'));
	const int ladderHeight = ladder.empty() ? 0 : static_cast<int>(ladder.size() + 1) * LadderRowHeight + 6;
	const int previewHeight = ladder.empty() ? PreviewHeight : std::clamp(buttonsTop - statusHeight - y - 6 - (28 + 20) - bodyLines * 14 - ladderHeight - 4, 60, PreviewHeight);

	const Rectangle preview { { x, y }, { inner, previewHeight } };
	Fill(out, preview, 0);
	Frame(out, preview, PAL16_YELLOW + 8);
	const Surface stage = out.subregion(preview.position.x + 1, preview.position.y + 1, preview.size.width - 2, preview.size.height - 2);
	if (selected.tab == Tab::Monsters) {
		// The monster itself, standing and breathing.
		const OptionalClxSpriteList sprites = KindPortrait(selected.index);
		if (sprites && sprites->numSprites() > 0) {
			const ClxSprite sprite = (*sprites)[(SDL_GetTicks() / 120) % sprites->numSprites()];
			ClxDraw(stage, { (stage.w() - sprite.width()) / 2, stage.h() - 6 }, sprite);
		}
	} else {
		const bool spells = selected.tab == Tab::Spells;
		SpellIcon(stage, { { 0, 0 }, { stage.w(), stage.h() } }, spells ? SpellKinds[selected.index].icon : TrapKinds[selected.index].icon);
	}
	y += previewHeight + 6;
	DrawString(out, Fit(title, inner), { { x, y }, { inner, 26 } }, { .flags = UiFlags::FontSize24 | UiFlags::AlignCenter | UiFlags::ColorGold });
	y += 28;
	DrawString(out, Fit(subtitle, inner), { { x, y }, { inner, 16 } }, { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | UiFlags::ColorWhite });
	y += 20;

	const int bodyHeight = ladder.empty() ? buttonsTop - statusHeight - y - 4 : bodyLines * 14;
	DrawString(out, wrapped, { { x, y }, { inner, bodyHeight } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorWhite, .lineHeight = 14 });
	if (!ladder.empty()) {
		int rowY = y + bodyHeight + 6;
		const int rowsEnd = buttonsTop - statusHeight - 2;
		Fill(out, { { x, rowY - 3 }, { inner, 1 } }, FrameColor);
		DrawString(out, Fit(fmt::format("{} ladder", FamilyName(FamilyOfKind(selected.index))), inner), { { x, rowY }, { inner, 14 } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorGold });
		rowY += LadderRowHeight;
		for (const size_t kind : ladder) {
			if (rowY + LadderRowHeight > rowsEnd)
				break;
			const bool owned = OwnsKind(kind);
			const bool canBuy = CanUnlockKind(kind);
			const bool isSelected = kind == selected.index;
			const std::string state = owned ? "Owned" : fmt::format("{} {}R", canBuy ? "Unlock" : "Locked", KindUnlockCost(kind));
			const UiFlags color = owned ? UiFlags::ColorGold : (canBuy ? UiFlags::ColorWhite : UiFlags::ColorUiSilverDark);
			if (isSelected)
				Fill(out, { { x, rowY - 1 }, { inner, LadderRowHeight } }, CardColor);
			const int stateWidth = GetLineWidth(state);
			DrawString(out, Fit(std::string(isSelected ? "> " : "") + std::string(BuildKinds[kind].name), inner - stateWidth - 6), { { x + 2, rowY }, { inner, 14 } }, { .flags = UiFlags::FontSize12 | color });
			DrawString(out, state, { { x, rowY }, { inner - 2, 14 } }, { .flags = UiFlags::FontSize12 | UiFlags::AlignRight | color });
			rowY += LadderRowHeight;
		}
	}
	if (!status.empty()) {
		DrawString(out, WordWrapString(status, inner), { { x, buttonsTop - statusHeight }, { inner, statusHeight } },
		    { .flags = UiFlags::FontSize12 | (canAct || selected.tab == Tab::Monsters && OwnsKind(selected.index) ? UiFlags::ColorGold : UiFlags::ColorRed), .lineHeight = 14 });
	}

	int buttonY = buttonsTop;
	if (canEquip) {
		const size_t spell = selected.index;
		const std::array<int8_t, NumSpellSlots> &bar = ProfileSpellBar();
		const bool onBar = std::find(bar.begin(), bar.end(), static_cast<int8_t>(spell)) != bar.end();
		padEquip = Rectangle { { x, buttonY }, { inner, ButtonHeight } };
		if (Button(out, { { x, buttonY }, { inner, ButtonHeight } }, onBar ? "Take off spell bar" : "Put on spell bar", true, mouse) && !ToggleProfileSpell(spell))
			Flash("Your spell bar is full: take a spell off first");
		buttonY += ButtonHeight + 4;
	}
	if (!action.empty() && canAct)
		padPrimary = Rectangle { { x, buttonY }, { inner, ButtonHeight } };
	if (!action.empty() && Button(out, { { x, buttonY }, { inner, ButtonHeight } }, action, canAct, mouse) && canAct) {
		switch (selected.tab) {
		case Tab::Monsters:
			UnlockKind(selected.index);
			break;
		case Tab::Spells: {
			const bool wasOwned = OwnedSpellLevel(selected.index) > 0;
			if (RaiseSpell(selected.index) && !wasOwned)
				ToggleProfileSpell(selected.index); // onto the bar, if there is room
			break;
		}
		case Tab::Traps:
			RaiseTrap(selected.index);
			break;
		}
	}
}

void DrawScreen(const Surface &out, Point mouse)
{
	Fill(out, { { 0, 0 }, { out.w(), out.h() } }, 0);
	// On a wide screen the screen stays a sensible shape, in the middle. Wider than 1280 the list gains a third family across.
	const int width = std::min(out.w(), 1600);
	const int left = (out.w() - width) / 2;
	const int leftWidth = width >= 1000 ? 230 : 170;
	const int rightWidth = width >= 1000 ? 290 : 200;
	const bool clickedScreen = clicked;
	if (TutorialIsMouseOver(mouse))
		clicked = false; // the tip is on top of the list, and it is drawn after it
	const Rectangle character { { left + Margin, Margin }, { leftWidth, out.h() - 2 * Margin } };
	const Rectangle details { { left + width - rightWidth - Margin, Margin }, { rightWidth, out.h() - 2 * Margin - ButtonHeight - 6 } };
	const int middleLeft = character.position.x + leftWidth + Margin;
	const int middleWidth = details.position.x - Margin - middleLeft;

	DrawCharacter(out, character, mouse);

	const int tabWidth = (middleWidth - 4) / static_cast<int>(TabNames.size());
	for (size_t i = 0; i < TabNames.size(); i++) {
		const Rectangle rect { { middleLeft + static_cast<int>(i) * (tabWidth + 2), Margin }, { tabWidth, TabHeight } };
		padTabs[i] = rect;
		PadAddTarget(rect);
		const bool active = static_cast<size_t>(tab) == i;
		Fill(out, rect, active ? PAL16_RED + 12 : CardColor);
		Frame(out, rect, active ? PAL16_YELLOW + 4 : (Inside(rect, mouse) ? PAL16_GRAY : FrameColor));
		DrawString(out, TabNames[i], { rect.position + Displacement { 0, 6 }, { rect.size.width, 16 } },
		    { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | (active ? UiFlags::ColorGold : UiFlags::ColorWhite) });
		if (clicked && Inside(rect, mouse)) {
			tab = static_cast<Tab>(i);
			if (selected.tab != tab)
				selected = { tab, tab == Tab::Monsters ? BestiaryKinds().front() : 0 };
		}
	}

	std::vector<Chip> chips;
	const int barHeight = tab == Tab::Monsters ? LayoutBar(middleWidth, chips) : 0;
	const int listTop = Margin + TabHeight + 6 + (barHeight > 0 ? barHeight + 4 : 0);
	if (!chips.empty())
		DrawBar(out, chips, { middleLeft, Margin + TabHeight + 6 }, mouse);
	const int spellBarHeight = tab == Tab::Spells ? SPLICONLENGTH + 26 : 0;
	const int promptHeight = IsPadInUse() ? PadPromptHeight : 0;
	const Rectangle list { { middleLeft, listTop }, { middleWidth, out.h() - listTop - Margin - spellBarHeight - promptHeight } };
	Fill(out, list, 0);
	Frame(out, list, PAL16_GRAY + 12);
	const Rectangle inside { list.position + Displacement { 4, 4 }, { list.size.width - 8, list.size.height - 8 } };
	padList = inside;
	if (tab == Tab::Monsters)
		DrawMonsters(out, inside, mouse);
	else
		DrawGrid(out, inside, mouse);
	if (tab == Tab::Spells)
		DrawSpellBarRow(out, { { middleLeft, list.position.y + list.size.height + 4 }, { middleWidth, spellBarHeight - 4 } }, mouse);

	DrawDetails(out, details, mouse);

	const Rectangle done { { details.position.x, out.h() - Margin - ButtonHeight }, { rightWidth, ButtonHeight } };
	if (Button(out, done, fmt::format("Done ({})", InputName(PadAction::Cancel)), true, mouse))
		endScreen = true;

	DrawKeeperScreenTip(out, list, mouse, clickedScreen);

	if (!message.empty() && SDL_GetTicks() < messageUntil) {
		const int textWidth = GetLineWidth(message) + 16;
		const Rectangle line { { middleLeft + (middleWidth - textWidth) / 2, list.position.y + list.size.height - 26 }, { textWidth, 20 } };
		Fill(out, line, 0);
		Frame(out, line, RedFrame);
		DrawString(out, message, { line.position + Displacement { 0, 3 }, { line.size.width, 16 } }, { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | UiFlags::ColorGold });
	}

	if (IsPadInUse()) {
		if (padFocus.size.width > 0)
			PadDrawFocusFrame(out, padFocus);
		const std::string prompt = fmt::format("{}: buy   {}: spell bar   {}: respec   {} {}: tabs   {}: done",
		    InputName(PadAction::Confirm), PadButtonName(ControllerButton_BUTTON_X), PadButtonName(ControllerButton_BUTTON_Y), InputName(PadAction::PrevTab), InputName(PadAction::NextTab), InputName(PadAction::Cancel));
		DrawString(out, FitText(prompt, middleWidth), { { middleLeft, out.h() - Margin - PadPromptHeight + 3 }, { middleWidth, 14 } }, { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | UiFlags::ColorWhite });
	}
}

/** Moves the focus and works out where this frame's mouse is. @return true if it clicks there. */
bool PadFrame(Point &mouse)
{
	if (padTargets.empty())
		return false;
	std::optional<Rectangle> click;
	/** Scrolls a card of the list into view, so a click there lands on it. */
	const auto focusCard = [&](size_t index) {
		padFocus = padTargets[index];
		if (padInList[index]) {
			int &offset = scroll[static_cast<size_t>(tab)];
			const int top = padFocus.position.y - PadScrollMargin - padList.position.y;
			const int bottom = padFocus.position.y + padFocus.size.height - (padList.position.y + padList.size.height);
			const int move = top < 0 ? top : std::max(bottom, 0);
			offset += move;
			padFocus.position.y -= move;
		}
		click = padFocus;
	};
	if (padFocusFirstCard || padFocus.size.width == 0) {
		// The first card on show, or else the first there is. Landing on it selects it, so A buys what is framed.
		std::optional<size_t> first;
		for (size_t i = 0; i < padTargets.size() && !first; i++) {
			if (padIsCard[i] && padInList[i] && padList.contains(padTargets[i].position) && padList.contains(padTargets[i].position + Displacement { padTargets[i].size.width - 1, padTargets[i].size.height - 1 }))
				first = i;
		}
		for (size_t i = 0; i < padTargets.size() && !first; i++) {
			if (padIsCard[i])
				first = i;
		}
		if (first)
			focusCard(*first);
		else if (padFocus.size.width == 0)
			padFocus = padTargets.front();
		padFocusFirstCard = false;
	} else {
		// Scrolling and changing tabs move things about.
		padFocus = padTargets[PadNearestTarget(padTargets, padFocus.Center()).value_or(0)];
	}

	const AxisDirection dir = padRepeater.Get(GetLeftStickOrDpadDirection(false));
	if (dir.x != AxisDirectionX_NONE || dir.y != AxisDirectionY_NONE) {
		if (const std::optional<size_t> next = PadStepFocus(padTargets, padFocus, dir); next) {
			if (padIsCard[*next])
				focusCard(*next);
			else
				padFocus = padTargets[*next];
		}
	}

	switch (padPress) {
	case PadPress::Confirm: {
		const std::optional<size_t> at = PadNearestTarget(padTargets, padFocus.Center());
		if (at && padIsCard[*at] && padIsSelected[*at]) {
			// The card the details are showing: pressing it is the button that buys it.
			if (padPrimary && !click)
				click = padPrimary;
		} else {
			click = padFocus;
		}
		break;
	}
	case PadPress::Equip:
		if (padEquip)
			click = padEquip;
		break;
	case PadPress::Respec:
		click = padRespec;
		break;
	case PadPress::PrevTab:
	case PadPress::NextTab: {
		const size_t count = padTabs.size();
		const size_t next = (static_cast<size_t>(tab) + (padPress == PadPress::NextTab ? 1 : count - 1)) % count;
		click = padTabs[next];
		padFocusFirstCard = true;
		break;
	}
	case PadPress::None:
		break;
	}
	padPress = PadPress::None;
	mouse = click ? click->Center() : padFocus.Center();
	return click.has_value();
}

void PadHandleEvent(const SDL_Event &event)
{
	for (const ControllerButtonEvent ctrlEvent : ToControllerButtonEvents(event)) {
		if (ctrlEvent.button == ControllerButton_IGNORE)
			continue;
		DetectInputMethod(event, ctrlEvent);
		if (IsControllerMotion(event) || ctrlEvent.button == ControllerButton_NONE || ctrlEvent.up)
			continue;
		switch (TranslateTo(GamepadType, ctrlEvent.button)) {
		case ControllerButton_BUTTON_A:
			padPress = PadPress::Confirm;
			break;
		case ControllerButton_BUTTON_B:
		case ControllerButton_BUTTON_START:
		case ControllerButton_BUTTON_BACK:
			endScreen = true;
			break;
		case ControllerButton_BUTTON_X:
			padPress = PadPress::Equip;
			break;
		case ControllerButton_BUTTON_Y:
			padPress = PadPress::Respec;
			break;
		case ControllerButton_BUTTON_LEFTSHOULDER:
			padPress = PadPress::PrevTab;
			break;
		case ControllerButton_BUTTON_RIGHTSHOULDER:
			padPress = PadPress::NextTab;
			break;
		default:
			break;
		}
	}
}

bool HandleEvent(SDL_Event &event)
{
	switch (event.type) {
	case SDL_EVENT_GAMEPAD_AXIS_MOTION:
	case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
	case SDL_EVENT_GAMEPAD_BUTTON_UP:
	case SDL_EVENT_JOYSTICK_AXIS_MOTION:
	case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
	case SDL_EVENT_JOYSTICK_BUTTON_UP:
	case SDL_EVENT_JOYSTICK_HAT_MOTION:
		PadHandleEvent(event);
		return true;
	case SDL_EVENT_KEY_DOWN:
		if (SDLC_EventKey(event) == SDLK_ESCAPE) {
			endScreen = true;
			return true;
		}
		return false;
	case SDL_EVENT_MOUSE_BUTTON_DOWN:
		if (event.button.button == SDL_BUTTON_LEFT)
			clicked = true;
		return false;
	case SDL_EVENT_MOUSE_WHEEL:
		wheel += SDLC_EventWheelIntY(event);
		return true;
	default:
		return false;
	}
}

bool overlayOpen = false;

void ResetScreenState()
{
	if (selected.tab == Tab::Monsters && !BestiaryKinds().empty() && !IsKindOnRoster(selected.index))
		selected.index = BestiaryKinds().front();
	endScreen = false;
	confirmRespec = false;
	clicked = false;
	wheel = 0;
}

} // namespace

void OpenKeeperOverlay()
{
	ResetScreenState();
	overlayOpen = true;
}

void CloseKeeperOverlay()
{
	overlayOpen = false;
}

void FreeKeeperOverlayImages()
{
	CloseKeeperOverlay();
	cardImages = {};
	cardImageBoxes = {};
	keeperImage = {};
}

bool IsKeeperOverlayOpen()
{
	return overlayOpen;
}

void DrawKeeperOverlay(const Surface &out, Point mouse)
{
	DrawScreen(out, mouse);
	clicked = false;
	wheel = 0;
	if (endScreen)
		CloseKeeperOverlay();
}

void KeeperOverlayClick()
{
	clicked = true;
}

void KeeperOverlayWheel(int deltaY)
{
	wheel += deltaY;
}
#ifdef KEEPER_DEVCTL
void DevHallSelect(int whichTab, size_t index)
{
	tab = static_cast<Tab>(whichTab);
	selected = { tab, index };
}
#endif

void UiKeeperScreen()
{
	// UiPollAndRender draws and sends events to the last menu's items: the main menu has freed its own.
	UiInitList_clear();
	LoadPaletteAndInitBlending("levels\\l1data\\l1_1.pal");
	UpdateSystemPalette(logical_palette);
	UiRestartFadeIn();
	(void)LoadLargeSpellIcons();

	const int scale = KeeperUiScale();
	OwnedSurface canvas(gnScreenWidth / scale, gnScreenHeight / scale);
	ResetScreenState();
	padFocus = {};
	padPress = PadPress::None;
	padFocusFirstCard = false;

	while (!endScreen) {
		Point mouse { MousePosition.x / scale, MousePosition.y / scale };
		if (IsPadInUse())
			clicked = PadFrame(mouse);
		padTargets.clear();
		padIsCard.clear();
		padIsSelected.clear();
		padInList.clear();
		padPrimary = std::nullopt;
		padEquip = std::nullopt;
		DrawScreen(canvas, mouse);
		clicked = false;
		wheel = 0;

		// Blow the canvas up onto the screen, where the menu's cursor goes on top.
		const Surface screen(DiabloUiSurface());
		for (int y = 0; y < canvas.h(); y++) {
			const uint8_t *src = canvas.at(0, y);
			for (int repeat = 0; repeat < scale && y * scale + repeat < screen.h(); repeat++) {
				uint8_t *dst = screen.at(0, y * scale + repeat);
				for (int x = 0; x < canvas.w(); x++)
					std::memset(dst + x * scale, src[x], scale);
			}
		}
		UiPollAndRender(HandleEvent);
	}

	FreeLargeSpellIcons();
	cardImages = {};
	cardImageBoxes = {};
	keeperImage = {};
}

} // namespace devilution::keeper
