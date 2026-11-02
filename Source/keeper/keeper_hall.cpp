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
#include "controls/input.h"
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
#include "keeper/keeper_profile.hpp"
#include "keeper/keeper_roster.hpp"
#include "keeper/keeper_tutorial.hpp"
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
std::string message;
uint32_t messageUntil = 0;

std::array<KindImage, NumLairKinds> cardImages;
KindImage keeperImage;

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

void Panel(const Surface &out, const Rectangle &rect)
{
	Fill(out, rect, PanelColor);
	Frame(out, rect, PAL16_RED + 10);
	Frame(out, { rect.position + Displacement { 1, 1 }, { rect.size.width - 2, rect.size.height - 2 } }, PAL16_GRAY + 12);
}

bool Button(const Surface &out, const Rectangle &rect, std::string_view label, bool enabled, Point mouse)
{
	const bool hover = Inside(rect, mouse);
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
	const Rectangle image { rect.position + Displacement { 3, 3 }, { rect.size.width - 6, CardImageHeight } };
	Fill(out, image, 0);
	if (kind) {
		KindImage &card = cardImages[*kind];
		if (!card.built)
			card = MakeKindImage(*kind, image.size.width, image.size.height);
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
	DrawString(out, Fit(info.name, rect.size.width - 4), { rect.position + Displacement { 0, CardImageHeight + 8 }, { rect.size.width, 14 } },
	    { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | (info.owned ? UiFlags::ColorGold : (info.canBuy ? UiFlags::ColorWhite : UiFlags::ColorUiSilverDark)) });
	const std::string_view state = info.owned ? "Owned" : (info.canBuy ? "Unlock" : "Locked");
	DrawString(out, state, { rect.position + Displacement { 0, CardImageHeight + 21 }, { rect.size.width, 14 } },
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
	if (!surface || surface->w() != rect.size.width)
		surface.emplace(rect.size.width, CardHeight);
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
int ApplyScroll(Tab which, int contentHeight, const Rectangle &area, Point mouse)
{
	int &offset = scroll[static_cast<size_t>(which)];
	if (Inside(area, mouse))
		offset -= wheel * (CardHeight + FamilyTitleHeight + RowGap);
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

void DrawMonsters(const Surface &out, const Rectangle &area, Point mouse)
{
	// One row per family, its tiers in a chain: each must be unlocked before the next.
	struct Row {
		size_t family;
		std::vector<size_t> kinds;
	};
	static std::vector<Row> rows;
	if (rows.empty()) {
		for (size_t kind = 0; kind < NumLairKinds; kind++) {
			if (!IsKindOnRoster(kind))
				continue;
			const auto family = static_cast<size_t>(FamilyOfKind(kind));
			if (rows.empty() || rows.back().family != family)
				rows.push_back({ family, {} });
			rows.back().kinds.push_back(kind);
		}
	}
	// Four tiers across if they fit at a readable size: every tier has to be reachable on any screen.
	const int listWidth = area.size.width - 7 - 2;
	size_t mostTiers = 1;
	for (const Row &row : rows)
		mostTiers = std::max(mostTiers, row.kinds.size());
	const int tiers = static_cast<int>(mostTiers);
	const int cardWidth = std::clamp((listWidth - (tiers - 1) * ArrowWidth) / tiers, MinCardWidth, CardWidth);
	const size_t perLine = static_cast<size_t>(std::max((listWidth + ArrowWidth) / (cardWidth + ArrowWidth), 1));
	const auto rowHeight = [&](const Row &row) {
		return FamilyTitleHeight + static_cast<int>((row.kinds.size() + perLine - 1) / perLine) * (CardHeight + RowGap);
	};
	int content = 0;
	for (const Row &row : rows)
		content += rowHeight(row);
	const int offset = ApplyScroll(Tab::Monsters, content, area, mouse);
	const Surface clip = out.subregion(area.position.x, area.position.y, area.size.width - 7, area.size.height);
	const Point localMouse { mouse.x - area.position.x, mouse.y - area.position.y };
	const bool mouseInArea = Inside(area, mouse);

	int rowTop = -offset;
	for (const Row &row : rows) {
		const int y = rowTop;
		rowTop += rowHeight(row);
		if (rowTop < 0 || y >= area.size.height)
			continue;
		if (y >= 0)
			DrawString(clip, FamilyName(static_cast<KindFamily>(row.family)), { { 2, y + 1 }, { clip.w() - 4, 14 } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorGold });
		for (size_t t = 0; t < row.kinds.size(); t++) {
			const size_t kind = row.kinds[t];
			const int column = static_cast<int>(t % perLine);
			const int cardY = y + FamilyTitleHeight + static_cast<int>(t / perLine) * (CardHeight + RowGap);
			const int x = 2 + column * (cardWidth + ArrowWidth);
			const Rectangle card { { x, cardY }, { cardWidth, CardHeight } };
			const bool isSelected = selected.tab == Tab::Monsters && selected.index == kind;
			DrawCard(clip, card, MonsterInfo(kind), isSelected, mouseInArea ? localMouse : Point { -1, -1 }, kind, std::nullopt);
			if (clicked && mouseInArea && Inside(card, localMouse))
				selected = { Tab::Monsters, kind };
			if (t + 1 < row.kinds.size() && (t + 1) % perLine != 0) {
				const int arrowY = cardY + CardHeight / 2 - 8;
				if (arrowY >= 0 && arrowY + 16 <= clip.h()) {
					DrawString(clip, ">", { { x + cardWidth, arrowY }, { ArrowWidth, 16 } },
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
	DrawString(out, fmt::format("{} / {} XP", ProfileXp(), to), { { x, y }, { inner, 16 } }, { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | UiFlags::ColorWhite });
	y += 22;

	DrawString(out, std::to_string(RenownAvailable()), { { x, y }, { inner, 30 } }, { .flags = UiFlags::FontSize30 | UiFlags::AlignCenter | UiFlags::ColorGold });
	y += 32;
	DrawString(out, "Renown to spend", { { x, y }, { inner, 16 } }, { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | UiFlags::ColorGold });
	y += 24;

	const ProfileStats &stats = GetProfileStats();
	const std::array<std::pair<std::string_view, int>, 5> lines { {
	    { "Maps cleared", stats.mapsCleared },
	    { "Maps lost", stats.mapsLost },
	    { "Raids survived", stats.raidsSurvived },
	    { "Heroes slain", stats.heroesSlain },
	    { "Monsters lost", stats.monstersLost },
	} };
	for (const auto &[label, value] : lines) {
		DrawString(out, label, { { x, y }, { inner, 16 } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorWhite });
		DrawString(out, std::to_string(value), { { x, y }, { inner, 16 } }, { .flags = UiFlags::FontSize12 | UiFlags::AlignRight | UiFlags::ColorGold });
		y += 15;
	}

	const Rectangle respec { { x, area.position.y + area.size.height - ButtonHeight - 8 }, { inner, ButtonHeight } };
	if (Button(out, respec, confirmRespec ? "Click again to respec" : "Respec (free)", true, mouse)) {
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

	const Rectangle preview { { x, y }, { inner, PreviewHeight } };
	Fill(out, preview, 0);
	Frame(out, preview, PAL16_YELLOW + 8);
	const Surface stage = out.subregion(preview.position.x + 1, preview.position.y + 1, preview.size.width - 2, preview.size.height - 2);
	std::string title;
	std::string subtitle;
	std::string body;
	std::string status;
	std::string action;
	bool canAct = false;

	if (selected.tab == Tab::Monsters) {
		const size_t kind = selected.index;
		// The monster itself, standing and breathing.
		const OptionalClxSpriteList sprites = KindPortrait(kind);
		if (sprites && sprites->numSprites() > 0) {
			const ClxSprite sprite = (*sprites)[(SDL_GetTicks() / 120) % sprites->numSprites()];
			ClxDraw(stage, { (stage.w() - sprite.width()) / 2, stage.h() - 6 }, sprite);
		}
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
		SpellIcon(stage, { { 0, 0 }, { stage.w(), stage.h() } }, spells ? SpellKinds[i].icon : TrapKinds[i].icon);
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
	y += PreviewHeight + 6;
	DrawString(out, Fit(title, inner), { { x, y }, { inner, 26 } }, { .flags = UiFlags::FontSize24 | UiFlags::AlignCenter | UiFlags::ColorGold });
	y += 28;
	DrawString(out, Fit(subtitle, inner), { { x, y }, { inner, 16 } }, { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | UiFlags::ColorWhite });
	y += 20;

	const int bottom = area.position.y + area.size.height - 8;
	const bool canEquip = selected.tab == Tab::Spells && OwnedSpellLevel(selected.index) > 0;
	const int buttonsTop = bottom - ButtonHeight * (canEquip ? 2 : 1) - (canEquip ? 4 : 0);
	const int statusHeight = status.empty() ? 0 : 42;
	const std::string wrapped = WordWrapString(body, inner);
	DrawString(out, wrapped, { { x, y }, { inner, buttonsTop - statusHeight - y - 4 } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorWhite, .lineHeight = 14 });
	if (!status.empty()) {
		DrawString(out, WordWrapString(status, inner), { { x, buttonsTop - statusHeight }, { inner, statusHeight } },
		    { .flags = UiFlags::FontSize12 | (canAct || selected.tab == Tab::Monsters && OwnsKind(selected.index) ? UiFlags::ColorGold : UiFlags::ColorRed), .lineHeight = 14 });
	}

	int buttonY = buttonsTop;
	if (canEquip) {
		const size_t spell = selected.index;
		const std::array<int8_t, NumSpellSlots> &bar = ProfileSpellBar();
		const bool onBar = std::find(bar.begin(), bar.end(), static_cast<int8_t>(spell)) != bar.end();
		if (Button(out, { { x, buttonY }, { inner, ButtonHeight } }, onBar ? "Take off spell bar" : "Put on spell bar", true, mouse) && !ToggleProfileSpell(spell))
			Flash("Your spell bar is full: take a spell off first");
		buttonY += ButtonHeight + 4;
	}
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
	// On a wide screen the screen stays a sensible shape, in the middle.
	const int width = std::min(out.w(), 1280);
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

	const int listTop = Margin + TabHeight + 6;
	const int barHeight = tab == Tab::Spells ? SPLICONLENGTH + 26 : 0;
	const Rectangle list { { middleLeft, listTop }, { middleWidth, out.h() - listTop - Margin - barHeight } };
	Fill(out, list, 0);
	Frame(out, list, PAL16_GRAY + 12);
	const Rectangle inside { list.position + Displacement { 4, 4 }, { list.size.width - 8, list.size.height - 8 } };
	if (tab == Tab::Monsters)
		DrawMonsters(out, inside, mouse);
	else
		DrawGrid(out, inside, mouse);
	if (tab == Tab::Spells)
		DrawSpellBarRow(out, { { middleLeft, list.position.y + list.size.height + 4 }, { middleWidth, barHeight - 4 } }, mouse);

	DrawDetails(out, details, mouse);

	const Rectangle done { { details.position.x, out.h() - Margin - ButtonHeight }, { rightWidth, ButtonHeight } };
	if (Button(out, done, "Done (Esc)", true, mouse))
		endScreen = true;

	DrawKeeperScreenTip(out, list, mouse, clickedScreen);

	if (!message.empty() && SDL_GetTicks() < messageUntil) {
		const int textWidth = GetLineWidth(message) + 16;
		const Rectangle line { { middleLeft + (middleWidth - textWidth) / 2, list.position.y + list.size.height - 26 }, { textWidth, 20 } };
		Fill(out, line, 0);
		Frame(out, line, RedFrame);
		DrawString(out, message, { line.position + Displacement { 0, 3 }, { line.size.width, 16 } }, { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | UiFlags::ColorGold });
	}
}

bool HandleEvent(SDL_Event &event)
{
	switch (event.type) {
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

} // namespace

void UiKeeperScreen()
{
	// UiPollAndRender draws and sends events to the last menu's items: the main menu has freed its own.
	UiInitList_clear();
	LoadPaletteAndInitBlending("levels\\l1data\\l1_1.pal");
	UpdateSystemPalette(logical_palette);
	UiRestartFadeIn();
	(void)LoadLargeSpellIcons();

	const int scale = std::clamp(gnScreenHeight / 720, 1, 4);
	OwnedSurface canvas(gnScreenWidth / scale, gnScreenHeight / scale);
	if (selected.tab == Tab::Monsters && !BestiaryKinds().empty() && !IsKindOnRoster(selected.index))
		selected.index = BestiaryKinds().front();
	endScreen = false;
	confirmRespec = false;
	clicked = false;
	wheel = 0;

	while (!endScreen) {
		const Point mouse { MousePosition.x / scale, MousePosition.y / scale };
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
	keeperImage = {};
}

} // namespace devilution::keeper
