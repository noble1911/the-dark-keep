/**
 * @file keeper/keeper_lobby.cpp
 *
 * A joiner's spell panel and the list of masters. See keeper_lobby.hpp.
 */
#include "keeper/keeper_lobby.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include "engine/palette.h"
#include "engine/render/primitive_render.hpp"
#include "engine/render/text_render.hpp"
#include "game_mode.hpp"
#include "keeper/keeper_campaign.hpp"
#include "keeper/keeper_input.hpp"
#include "keeper/keeper_profile.hpp"
#include "panels/spell_icons.hpp"
#include "player.h"
#include "utils/ui_fwd.h"

namespace devilution::keeper {

namespace {

constexpr int Padding = 5;
constexpr int TitleHeight = 16;
/** Two lines: a spell's flavour text wraps. */
constexpr int FooterHeight = 28;
constexpr int FooterLineHeight = 13;
constexpr int CellGap = 4;
constexpr int IconSize = SPLICONLENGTH + 4;
constexpr int CellHeight = IconSize + 26;
constexpr int ButtonWidth = 120;
constexpr int ButtonHeight = 16;
constexpr int ButtonGap = 2;
constexpr int MastersLeft = 6;
/** Below the engine's FPS counter. */
constexpr int MastersTop = 24;
constexpr int MastersWidth = 170;
constexpr int MastersRowHeight = 14;
constexpr int MastersTagWidth = 60;

constexpr uint8_t FrameColor = PAL16_YELLOW + 4;

bool panelOpen = false;
/** The panel opens by itself once the keep's state has arrived and a raid isn't on. */
bool promptPending = false;
/** A joiner's picks this session. Empty until the first one. */
std::optional<SpellBar> chosen;
/** What the last prompt offered: anything new means a new prompt. */
std::array<bool, NumSpellKinds> offered {};
bool rematchComing = false;

/** Only a joiner chooses, and raids lock the choice as they lock equipping. */
bool CanChoose()
{
	return gbIsMultiplayer && IsJoinedGame() && (State.phase == Phase::Setup || State.phase == Phase::Breather);
}

std::vector<size_t> AvailableSpells()
{
	std::vector<size_t> spells;
	for (size_t spell = 0; spell < NumSpellKinds; spell++) {
		if (IsSpellAvailable(spell))
			spells.push_back(spell);
	}
	return spells;
}

std::optional<size_t> SlotOf(const SpellBar &bar, size_t spell)
{
	for (size_t slot = 0; slot < NumSpellSlots; slot++) {
		if (bar[slot] == spell)
			return slot;
	}
	return std::nullopt;
}

Rectangle ButtonRect(const Rectangle &spellBar)
{
	return { { spellBar.position.x + spellBar.size.width - ButtonWidth, spellBar.position.y - ButtonHeight - ButtonGap }, { ButtonWidth, ButtonHeight } };
}

/** Sits on top of the spell bar it fills, one column to a slot. */
Rectangle PanelRect(const Rectangle &spellBar, size_t spellCount)
{
	const int columns = static_cast<int>(NumSpellSlots);
	const int rows = std::max((static_cast<int>(spellCount) + columns - 1) / columns, 1);
	const int height = 2 * Padding + TitleHeight + rows * (CellHeight + CellGap) + FooterHeight;
	const int bottom = ButtonRect(spellBar).position.y - ButtonGap;
	return { { spellBar.position.x - Padding, std::max(bottom - height, 0) }, { spellBar.size.width + 2 * Padding, height } };
}

Rectangle CellRect(const Rectangle &panel, size_t index)
{
	const int columns = static_cast<int>(NumSpellSlots);
	const int column = static_cast<int>(index) % columns;
	const int row = static_cast<int>(index) / columns;
	return { { panel.position.x + Padding + column * (IconSize + CellGap), panel.position.y + Padding + TitleHeight + row * (CellHeight + CellGap) }, { IconSize, CellHeight } };
}

std::optional<size_t> SpellAt(const Rectangle &panel, const std::vector<size_t> &spells, Point mouse)
{
	for (size_t i = 0; i < spells.size(); i++) {
		if (CellRect(panel, i).contains(mouse))
			return spells[i];
	}
	return std::nullopt;
}

/** Puts the spell in the first free slot, or takes it off. Does nothing when the bar is full. */
void ToggleSpell(SpellBar &bar, size_t spell)
{
	if (const std::optional<size_t> slot = SlotOf(bar, spell); slot) {
		bar[*slot] = std::nullopt;
	} else {
		const auto free = std::find(bar.begin(), bar.end(), std::nullopt);
		if (free == bar.end())
			return;
		*free = spell;
	}
	chosen = bar;
}

void DrawFrame(const Surface &out, const Rectangle &rect, uint8_t color)
{
	DrawHorizontalLine(out, rect.position, rect.size.width, color);
	DrawHorizontalLine(out, rect.position + Displacement { 0, rect.size.height - 1 }, rect.size.width, color);
	DrawVerticalLine(out, rect.position, rect.size.height, color);
	DrawVerticalLine(out, rect.position + Displacement { rect.size.width - 1, 0 }, rect.size.height, color);
}

void DrawPanel(const Surface &out, const Rectangle &rect)
{
	DrawHalfTransparentRectTo(out, rect.position.x, rect.position.y, rect.size.width, rect.size.height);
	DrawHalfTransparentRectTo(out, rect.position.x, rect.position.y, rect.size.width, rect.size.height);
	DrawFrame(out, rect, FrameColor);
}

/** Cuts `text` to `width`, marking the cut. Player names can be UTF-8: cuts fall between characters. */
std::string FitText(std::string_view text, int width)
{
	std::string fitted(text);
	if (GetLineWidth(fitted) <= width)
		return fitted;
	while (!fitted.empty() && GetLineWidth(fitted + ".") > width) {
		while (!fitted.empty() && (static_cast<unsigned char>(fitted.back()) & 0xC0) == 0x80)
			fitted.pop_back();
		if (!fitted.empty())
			fitted.pop_back();
	}
	return fitted + ".";
}

std::string FooterText(const SpellBar &bar, const std::vector<size_t> &spells, std::optional<size_t> hovered)
{
	if (spells.empty())
		return "The host's game has no spells.";
	const auto filled = static_cast<size_t>(NumSpellSlots - std::count(bar.begin(), bar.end(), std::nullopt));
	if (hovered) {
		if (!SlotOf(bar, *hovered) && filled == NumSpellSlots)
			return "The spell bar is full: take a spell off first.";
		return fmt::format("Level {}. {}", GameSpellLevel(*hovered), SpellKinds[*hovered].flavor);
	}
	return fmt::format("{} of {} slots filled. Click a spell to add or remove it. Esc closes; Choose spells brings it back until the raid.", filled, NumSpellSlots);
}

void DrawSpellCell(const Surface &out, const Rectangle &cell, size_t spell, const SpellBar &bar, bool hovered)
{
	const Rectangle icon { cell.position, { IconSize, IconSize } };
	FillRect(out, icon.position.x, icon.position.y, icon.size.width, icon.size.height, 0);
	SetSpellTrans(SpellType::Spell);
	const Surface area = out.subregion(icon.position.x, icon.position.y, icon.size.width, icon.size.height);
	DrawLargeSpellIcon(area, { (area.w() - SPLICONLENGTH) / 2, (area.h() + SPLICONLENGTH) / 2 }, SpellKinds[spell].icon);

	const std::optional<size_t> slot = SlotOf(bar, spell);
	if (slot) {
		// The key that will cast it, as on the spell bar below.
		DrawString(out, KeyLabel(KeeperAction::Spell, *slot), { icon.position + Displacement { 4, 3 }, { icon.size.width - 8, 14 } },
		    { .flags = UiFlags::FontSize12 | UiFlags::ColorWhite | UiFlags::Outlined });
	}
	DrawFrame(out, icon, slot ? PAL16_YELLOW + 2 : (hovered ? PAL16_GRAY : PAL16_GRAY + 8));
	if (slot)
		DrawFrame(out, { icon.position + Displacement { 1, 1 }, { icon.size.width - 2, icon.size.height - 2 } }, PAL16_YELLOW + 2);

	DrawString(out, FitText(SpellKinds[spell].name, cell.size.width), { cell.position + Displacement { 0, IconSize }, { cell.size.width, 13 } },
	    { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | (slot ? UiFlags::ColorGold : UiFlags::ColorWhite) });
	DrawString(out, fmt::format("{} mana", SpellManaCost(spell)), { cell.position + Displacement { 0, IconSize + 13 }, { cell.size.width, 13 } },
	    { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | UiFlags::ColorBlue });
}

} // namespace

void LobbyOnLevelLoaded(SpellBar &bar)
{
	panelOpen = false;
	promptPending = false;
	if (!IsJoinedGame()) {
		chosen.reset();
		return;
	}

	std::array<bool, NumSpellKinds> available {};
	bool anyAvailable = false;
	for (size_t spell = 0; spell < NumSpellKinds; spell++) {
		available[spell] = IsSpellAvailable(spell);
		anyAvailable = anyAvailable || available[spell];
	}
	promptPending = anyAvailable && (!chosen || available != offered);
	offered = available;

	if (!chosen)
		return;
	// The last game's picks, less anything this game doesn't have. If that takes them all, the
	// profile's choice stands; a bar the player emptied on purpose stays empty.
	SpellBar kept {};
	bool hadAny = false;
	bool keptAny = false;
	for (size_t slot = 0; slot < NumSpellSlots; slot++) {
		const std::optional<size_t> spell = (*chosen)[slot];
		if (!spell)
			continue;
		hadAny = true;
		if (available[*spell]) {
			kept[slot] = spell;
			keptAny = true;
		}
	}
	if (keptAny || !hadAny) {
		chosen = kept;
		bar = kept;
	} else {
		chosen.reset();
	}
}

void LobbyNoteRematch()
{
	rematchComing = true;
}

void LobbyOnGameEnded()
{
	if (!rematchComing)
		chosen.reset();
	rematchComing = false;
	panelOpen = false;
	promptPending = false;
}

void LobbyTick()
{
	if (!CanChoose()) {
		panelOpen = false;
		return;
	}
	// Until the keep's state arrives a joiner's phase is only a guess: it may have joined mid-raid.
	if (promptPending && !IsAwaitingKeepSnapshot()) {
		promptPending = false;
		panelOpen = true;
	}
}

bool LobbyOnEscape()
{
	if (!panelOpen || !CanChoose())
		return false;
	panelOpen = false;
	return true;
}

bool LobbyIsMouseOver(Point mouse, const Rectangle &spellBar)
{
	if (!CanChoose())
		return false;
	return ButtonRect(spellBar).contains(mouse) || (panelOpen && PanelRect(spellBar, AvailableSpells().size()).contains(mouse));
}

bool LobbyOnLeftClick(Point mouse, const Rectangle &spellBar, SpellBar &bar)
{
	if (!CanChoose())
		return false;
	if (ButtonRect(spellBar).contains(mouse)) {
		panelOpen = !panelOpen;
		promptPending = false;
		return true;
	}
	if (!panelOpen)
		return false;
	const std::vector<size_t> spells = AvailableSpells();
	const Rectangle panel = PanelRect(spellBar, spells.size());
	if (!panel.contains(mouse))
		return false;
	if (const std::optional<size_t> spell = SpellAt(panel, spells, mouse); spell)
		ToggleSpell(bar, *spell);
	return true;
}

void DrawLobby(const Surface &out, Point mouse, const Rectangle &spellBar, const SpellBar &bar)
{
	if (!CanChoose())
		return;

	const Rectangle button = ButtonRect(spellBar);
	DrawPanel(out, button);
	DrawString(out, panelOpen ? "Close spells (Esc)" : "Choose spells", { button.position + Displacement { 0, 1 }, { button.size.width, 14 } },
	    { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | (button.contains(mouse) ? UiFlags::ColorGold : UiFlags::ColorWhite) });
	if (!panelOpen)
		return;

	const std::vector<size_t> spells = AvailableSpells();
	const Rectangle panel = PanelRect(spellBar, spells.size());
	DrawPanel(out, panel);
	const int textWidth = panel.size.width - 2 * Padding;
	DrawString(out, "Spells in the host's game", { panel.position + Displacement { Padding, Padding - 1 }, { textWidth, 14 } },
	    { .flags = UiFlags::FontSize12 | UiFlags::ColorGold });

	const std::optional<size_t> hovered = SpellAt(panel, spells, mouse);
	for (size_t i = 0; i < spells.size(); i++)
		DrawSpellCell(out, CellRect(panel, i), spells[i], bar, hovered == spells[i]);

	const std::string footer = WordWrapString(FooterText(bar, spells, hovered), textWidth);
	DrawString(out, footer, { { panel.position.x + Padding, panel.position.y + panel.size.height - Padding - FooterHeight }, { textWidth, FooterHeight } },
	    { .flags = UiFlags::FontSize12 | UiFlags::ColorWhite, .lineHeight = FooterLineHeight });
}

void DrawMasters(const Surface &out)
{
	if (!gbIsMultiplayer)
		return;

	struct Row {
		std::string name;
		std::string tag;
		bool mine;
	};
	std::vector<Row> rows;
	for (size_t id = 0; id < NetPlayerSlots; id++) {
		const Player &player = Players[id];
		if (!player.plractive)
			continue;
		const bool mine = &player == MyPlayer;
		// Whoever creates a game takes the first network slot. A joiner never counts itself: if the
		// host has gone and this client took slot 0 over, there is no host any more.
		const bool host = IsJoinedGame() ? (id == 0 && !mine) : mine;
		std::string tag = host ? "host" : "";
		if (mine)
			tag += host ? ", you" : "you";
		if (!mine && (player._pLvlChanging || !player.isOnActiveLevel()))
			tag += tag.empty() ? "arriving" : ", arriving";
		rows.push_back({ player._pName, std::move(tag), mine });
	}

	const Rectangle panel { { MastersLeft, MastersTop }, { MastersWidth, 18 + static_cast<int>(rows.size()) * MastersRowHeight + 2 } };
	DrawPanel(out, panel);
	const int textWidth = panel.size.width - 8;
	Point line = panel.position + Displacement { 4, 2 };
	DrawString(out, "Masters", { line, { textWidth, 14 } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorGold });
	line.y += 16;
	for (const Row &row : rows) {
		const UiFlags color = row.mine ? UiFlags::ColorGold : UiFlags::ColorWhite;
		DrawString(out, FitText(row.name, textWidth - MastersTagWidth), { line, { textWidth - MastersTagWidth, 14 } }, { .flags = UiFlags::FontSize12 | color });
		DrawString(out, FitText(row.tag, MastersTagWidth), { line, { textWidth, 14 } }, { .flags = UiFlags::FontSize12 | UiFlags::AlignRight | color });
		line.y += MastersRowHeight;
	}
}

} // namespace devilution::keeper
