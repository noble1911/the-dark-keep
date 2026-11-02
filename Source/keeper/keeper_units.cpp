/**
 * @file keeper/keeper_units.cpp
 *
 * Unit overlays for The Dark Keep: floating hit/heal markers (the icon numbers from The Red
 * Portal), health and mana bars over units, and the hero inspect panel.
 */
#include "keeper/keeper_units.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "control/control.hpp"
#include "cursor.h"
#include "engine/palette.h"
#include "engine/render/primitive_render.hpp"
#include "engine/render/text_render.hpp"
#include "keeper/keeper.hpp"
#include "keeper/keeper_bots.hpp"
#include "keeper/keeper_game.hpp"
#include "keeper/keeper_heroes.hpp"
#include "monster.h"
#include "multi.h"
#include "options.h"
#include "player.h"
#include "qol/floatingnumbers.h"
#include "utils/ui_fwd.h"

namespace devilution::keeper {

namespace {

// ---- floating markers ------------------------------------------------------------

/** HP regained in one tick below this (in 1/64ths) is natural regeneration: no marker. */
constexpr int HealThreshold = 2 << 6;
/** Hits on the same unit within this many ticks add up into one number. */
constexpr uint32_t MergeTicks = 3;

struct Tracked {
	bool alive = false;
	int hitPoints = 0;
	/** Running total of the current damage (or heal) marker, and when it was last bumped. */
	int damageSum = 0;
	uint32_t damageTick = 0;
	int healSum = 0;
	uint32_t healTick = 0;
};

std::array<Tracked, MaxMonsters> monsterTrack;
std::array<Tracked, MaxBots> botTrack;
uint32_t markerTick = 0;

/** `ours`: the unit is on the keep's side (damage to it is bad news). */
void TrackUnit(Tracked &track, bool alive, int hitPoints, Point tile, bool ours, int markerId)
{
	if (track.alive) {
		const int delta = hitPoints - track.hitPoints;
		if (delta < 0) {
			track.damageSum = (markerTick - track.damageTick <= MergeTicks) ? track.damageSum - delta : -delta;
			track.damageTick = markerTick;
			const int shown = std::max(track.damageSum >> 6, 1);
			const UiFlags size = shown >= 50 ? UiFlags::FontSize24 : UiFlags::FontSize12;
			// Gold sword: damage the keep dealt. Red skull: damage the keep took.
			AddFloatingNumber(tile, {}, std::to_string(shown), size | (ours ? UiFlags::ColorRed : UiFlags::ColorGold), markerId, false,
			    ours ? FloatingNumberIcon::Taken : FloatingNumberIcon::Dealt);
		} else if (delta >= HealThreshold) {
			track.healSum = (markerTick - track.healTick <= MergeTicks) ? track.healSum + delta : delta;
			track.healTick = markerTick;
			AddFloatingNumber(tile, {}, fmt::format("+{}", track.healSum >> 6), UiFlags::FontSize12 | UiFlags::ColorWhite, markerId + 5000, false,
			    FloatingNumberIcon::Heal);
		}
	}
	track.alive = alive;
	track.hitPoints = alive ? hitPoints : 0;
}

bool IsAliveMonster(const Monster &monster)
{
	return monster.hitPoints > 0 && monster.mode != MonsterMode::Death && !monster.isInvalid;
}

// ---- palette colours --------------------------------------------------------------

struct BarColors {
	uint8_t ours;
	uint8_t theirs;
	uint8_t mana;
	uint8_t back;
};

/** Nearest palette entry to an RGB colour, skipping the animated low indices. */
uint8_t NearestColor(int r, int g, int b)
{
	int best = 32;
	int64_t bestDistance = INT64_MAX;
	for (int i = 32; i < 256; i++) {
		const SDL_Color &c = logical_palette[i];
		const int64_t dr = r - c.r;
		const int64_t dg = g - c.g;
		const int64_t db = b - c.b;
		const int64_t distance = 3 * dr * dr + 4 * dg * dg + 2 * db * db;
		if (distance < bestDistance) {
			bestDistance = distance;
			best = i;
		}
	}
	return static_cast<uint8_t>(best);
}

const BarColors &GetBarColors()
{
	static BarColors colors;
	static unsigned generation = ~0U;
	if (generation != PaletteGeneration) {
		colors = { NearestColor(64, 200, 64), NearestColor(216, 48, 40), NearestColor(72, 104, 240), NearestColor(12, 8, 8) };
		generation = PaletteGeneration;
	}
	return colors;
}

// ---- drawn units -------------------------------------------------------------------

struct DrawnUnit {
	bool isPlayer;
	size_t id;
	/** Bottom centre of the sprite, world buffer (unzoomed). */
	Point foot;
};

std::vector<DrawnUnit> drawnUnits;

struct Selection {
	bool isPlayer;
	size_t id;
};
std::optional<Selection> selected;

bool SelectionValid()
{
	if (!selected)
		return false;
	if (selected->isPlayer)
		return GetBotSummary(Players[selected->id]).has_value();
	return GetHeroSummary(Monsters[selected->id]).has_value();
}

bool IsSelected(bool isPlayer, size_t id)
{
	return selected && selected->isPlayer == isPlayer && selected->id == id;
}

constexpr int BarWidth = 36;
constexpr int HealthBarHeight = 4;
constexpr int ManaBarHeight = 3;
/** Bars sit this far above a unit's feet, just under the hero name labels. */
constexpr int BarRaise = 80;

void DrawBar(const Surface &out, Point topLeft, int height, int value, int max, uint8_t color)
{
	const BarColors &colors = GetBarColors();
	FillRect(out, topLeft.x - 1, topLeft.y - 1, BarWidth + 2, height + 2, colors.back);
	const int filled = max > 0 ? std::clamp(BarWidth * value / max, 0, BarWidth) : 0;
	if (filled > 0)
		FillRect(out, topLeft.x, topLeft.y, filled, height, color);
}

// ---- inspect panel -----------------------------------------------------------------

constexpr int InspectWidth = 230;
constexpr int InspectLineHeight = 13;

struct InspectLine {
	std::string text;
	UiFlags color;
};

std::vector<InspectLine> InspectLines()
{
	std::vector<InspectLine> lines;
	if (!SelectionValid())
		return lines;

	const auto carriedLine = [&lines](int carried, bool fleeing) {
		if (fleeing)
			lines.push_back({ "Fleeing in terror", UiFlags::ColorOrange });
		if (carried > 0)
			lines.push_back({ fmt::format("Carrying {} stolen gold", carried), UiFlags::ColorRed });
		else if (carried < 0)
			lines.push_back({ "Heading out empty-handed", UiFlags::ColorWhite });
	};

	if (selected->isPlayer) {
		const Player &bot = Players[selected->id];
		const BotSummary summary = *GetBotSummary(bot);
		lines.push_back({ bot._pName, UiFlags::ColorGold });
		lines.push_back({ fmt::format("Level {} {}", bot.getCharacterLevel(), HeroClassName(summary.cls)), UiFlags::ColorWhite });
		lines.push_back({ fmt::format("Life {}/{}   Mana {}/{}", bot._pHitPoints >> 6, bot._pMaxHP >> 6, bot._pMana >> 6, bot._pMaxMana >> 6), UiFlags::ColorWhite });
		lines.push_back({ fmt::format("Str {}  Mag {}  Dex {}  Vit {}", bot._pStrength, bot._pMagic, bot._pDexterity, bot._pVitality), UiFlags::ColorWhite });
		lines.push_back({ fmt::format("Damage {}-{}   Armor {}", bot._pIMinDam, bot._pIMaxDam, bot.GetArmor()), UiFlags::ColorWhite });
		lines.push_back({ fmt::format("Healing potions: {}", summary.potions), UiFlags::ColorWhite });
		carriedLine(summary.carried, summary.fleeing);
		lines.push_back({ "Gear:", UiFlags::ColorGold });
		bool anyGear = false;
		for (const Item &item : bot.InvBody) {
			if (item.isEmpty())
				continue;
			lines.push_back({ std::string("  ") + (item._iIdentified ? item._iIName : item._iName), item.getTextColor() });
			anyGear = true;
		}
		if (!anyGear)
			lines.push_back({ "  (none)", UiFlags::ColorWhite });
		return lines;
	}

	const Monster &hero = Monsters[selected->id];
	const HeroSummary summary = *GetHeroSummary(hero);
	lines.push_back({ std::string(HeroClassName(summary.cls)) + (summary.champion ? " (Champion)" : ""), UiFlags::ColorGold });
	lines.push_back({ fmt::format("Level {} lesser hero", hero.level(static_cast<_difficulty>(sgGameInitInfo.nDifficulty))), UiFlags::ColorWhite });
	lines.push_back({ fmt::format("Life {}/{}", hero.hitPoints >> 6, hero.maxHitPoints >> 6), UiFlags::ColorWhite });
	lines.push_back({ fmt::format("Damage {}-{}   Armor {}", hero.minDamage, hero.maxDamage, hero.armorClass), UiFlags::ColorWhite });
	carriedLine(summary.carried, summary.fleeing);
	lines.push_back({ "No gear: fights with its own strength", UiFlags::ColorWhite });
	return lines;
}

} // namespace

void NoteMonsterDrawn(const Monster &monster, Point renderPos, int spriteWidth)
{
	if (!IsActive() || drawnUnits.size() >= MaxMonsters + MaxBots)
		return;
	drawnUnits.push_back({ false, monster.getId(), renderPos + Displacement { spriteWidth / 2, 0 } });
}

void NotePlayerDrawn(const Player &player, Point renderPos, int spriteWidth)
{
	if (!IsActive() || !IsBot(player) || drawnUnits.size() >= MaxMonsters + MaxBots)
		return;
	drawnUnits.push_back({ true, player.getId(), renderPos + Displacement { spriteWidth / 2, 0 } });
}

void UpdateUnitMarkers()
{
	markerTick++;
	for (size_t i = 0; i < MaxMonsters; i++) {
		const Monster &monster = Monsters[i];
		const bool alive = IsAliveMonster(monster) && std::find(ActiveMonsters, ActiveMonsters + ActiveMonsterCount, static_cast<unsigned>(i)) != ActiveMonsters + ActiveMonsterCount;
		if (!alive && !monsterTrack[i].alive)
			continue;
		TrackUnit(monsterTrack[i], alive, monster.hitPoints, monster.position.tile, !IsHero(monster), static_cast<int>(i) + 1);
	}
	for (size_t i = 0; i < MaxBots && FirstBotId + i < Players.size(); i++) {
		const Player &bot = Players[FirstBotId + i];
		const bool alive = IsLivingBot(bot);
		TrackUnit(botTrack[i], alive, bot._pHitPoints, bot.position.tile, false, 1000 + static_cast<int>(i));
	}
}

void ResetUnits()
{
	monsterTrack = {};
	botTrack = {};
	drawnUnits.clear();
	selected = std::nullopt;
}

void DrawUnitBars(const Surface &out)
{
	const int zoom = *GetOptions().Graphics.zoom ? 2 : 1;
	const int maxY = GetMainPanel().position.y;
	const BarColors &colors = GetBarColors();
	for (const DrawnUnit &unit : drawnUnits) {
		int hp = 0;
		int maxHp = 0;
		int mana = 0;
		int maxMana = 0;
		bool hero = false;
		bool boss = false;
		if (unit.isPlayer) {
			const Player &bot = Players[unit.id];
			if (!IsLivingBot(bot))
				continue;
			hp = bot._pHitPoints;
			maxHp = bot._pMaxHP;
			mana = bot._pMana;
			maxMana = bot._pMaxMana;
			hero = true;
		} else {
			const Monster &monster = Monsters[unit.id];
			if (!IsAliveMonster(monster))
				continue;
			hp = monster.hitPoints;
			maxHp = monster.maxHitPoints;
			hero = IsHero(monster);
			boss = IsBoss(monster);
		}
		const bool hurt = hp < maxHp;
		const bool showMana = hero && maxMana > 0;
		const bool isSelected = IsSelected(unit.isPlayer, unit.id);
		if (!hurt && !showMana && !isSelected)
			continue;

		const int raise = BarRaise + (boss ? 8 : 0);
		const Point at { unit.foot.x * zoom - BarWidth / 2, (unit.foot.y - raise) * zoom };
		if (at.y < 0 || at.y + 12 >= maxY)
			continue;
		int y = at.y;
		if (hurt || isSelected) {
			DrawBar(out, { at.x, y }, HealthBarHeight, hp, maxHp, hero ? colors.theirs : colors.ours);
			y += HealthBarHeight + 2;
		}
		if (showMana) {
			DrawBar(out, { at.x, y }, ManaBarHeight, mana, maxMana, colors.mana);
			y += ManaBarHeight + 2;
		}
		if (isSelected) {
			// Gold brackets mark the hero being inspected.
			const uint8_t gold = PAL16_YELLOW + 2;
			FillRect(out, at.x - 3, at.y - 3, 4, 1, gold);
			FillRect(out, at.x - 3, at.y - 3, 1, y - at.y + 3, gold);
			FillRect(out, at.x + BarWidth - 1, at.y - 3, 4, 1, gold);
			FillRect(out, at.x + BarWidth + 2, at.y - 3, 1, y - at.y + 3, gold);
		}
	}
	drawnUnits.clear();
}

bool SelectHeroUnderCursor()
{
	if (pcursmonst >= 0 && GetHeroSummary(Monsters[pcursmonst])) {
		selected = Selection { false, static_cast<size_t>(pcursmonst) };
		return true;
	}
	if (PlayerUnderCursor != nullptr && GetBotSummary(*PlayerUnderCursor)) {
		selected = Selection { true, PlayerUnderCursor->getId() };
		return true;
	}
	return false;
}

void SelectHero(bool isPlayer, size_t id)
{
	if (id >= (isPlayer ? Players.size() : MaxMonsters))
		return;
	selected = Selection { isPlayer, id };
	if (!SelectionValid())
		selected = std::nullopt;
}

void ClearSelectedHero()
{
	selected = std::nullopt;
}

bool HasSelectedHero()
{
	return SelectionValid();
}

std::optional<Rectangle> InspectPanelRect(int left, int hudTop)
{
	const std::vector<InspectLine> lines = InspectLines();
	if (lines.empty())
		return std::nullopt;
	const int height = static_cast<int>(lines.size()) * InspectLineHeight + 12;
	return Rectangle { { left, hudTop - 8 - height }, { InspectWidth, height } };
}

void DrawInspectPanel(const Surface &out, int left, int hudTop)
{
	if (!SelectionValid()) {
		selected = std::nullopt;
		return;
	}
	const std::optional<Rectangle> rect = InspectPanelRect(left, hudTop);
	if (!rect)
		return;
	DrawHalfTransparentRectTo(out, rect->position.x, rect->position.y, rect->size.width, rect->size.height);
	DrawHalfTransparentRectTo(out, rect->position.x, rect->position.y, rect->size.width, rect->size.height);
	const uint8_t frame = PAL16_YELLOW + 4;
	FillRect(out, rect->position.x, rect->position.y, rect->size.width, 1, frame);
	FillRect(out, rect->position.x, rect->position.y + rect->size.height - 1, rect->size.width, 1, frame);
	FillRect(out, rect->position.x, rect->position.y, 1, rect->size.height, frame);
	FillRect(out, rect->position.x + rect->size.width - 1, rect->position.y, 1, rect->size.height, frame);

	Point at = rect->position + Displacement { 6, 6 };
	DrawString(out, "Esc: close", { at, { rect->size.width - 12, InspectLineHeight } }, { .flags = UiFlags::FontSize12 | UiFlags::AlignRight | UiFlags::ColorWhite });
	for (const InspectLine &line : InspectLines()) {
		DrawString(out, line.text, { at, { rect->size.width - 12, InspectLineHeight } }, { .flags = UiFlags::FontSize12 | line.color });
		at.y += InspectLineHeight;
	}
}

} // namespace devilution::keeper
