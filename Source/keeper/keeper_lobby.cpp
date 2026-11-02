/**
 * @file keeper/keeper_lobby.cpp
 *
 * A joiner's spell panel and the list of masters. See keeper_lobby.hpp.
 */
#include "keeper/keeper_lobby.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "engine/palette.h"
#include "engine/render/primitive_render.hpp"
#include "engine/render/text_render.hpp"
#include "game_mode.hpp"
#include "keeper/keeper_campaign.hpp"
#include "keeper/keeper_input.hpp"
#include "keeper/keeper_pad.hpp"
#include "keeper/keeper_profile.hpp"
#include "keeper/keeper_raid.hpp"
#include "keeper/keeper_ui.hpp"
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
constexpr int ButtonWidth = 144;
constexpr int ButtonHeight = 16;
constexpr int ButtonGap = 2;
constexpr int MastersLeft = 6;
/** Below the engine's FPS counter. */
constexpr int MastersTop = 24;
constexpr int MastersWidth = 186;
constexpr int MastersRowHeight = 14;
constexpr int MastersTagWidth = 76;

bool panelOpen = false;
/** The panel opens by itself once the keep's state has arrived and a raid isn't on. */
bool promptPending = false;
/** What this game has to offer a joiner; set when the keep loads. */
std::vector<size_t> availableSpells;
/** A joiner's picks this session. Empty until the first one. */
std::optional<SpellBar> chosen;
/** What the last prompt showed: only a different list prompts again. */
std::optional<std::vector<size_t>> promptedSpells;
bool rematchComing = false;

uint8_t hostSlot = NoHostSlot;
/** A joiner's fellow masters may not be marked active yet when it learns the slot. */
bool hostSeenActive = false;

/** Only a joiner chooses, raids lock the choice, and before the keep's state arrives the phase is only a guess. */
bool CanChoose()
{
	return gbIsMultiplayer && IsJoinedGame() && !IsAwaitingKeepSnapshot() && (State.phase == Phase::Setup || State.phase == Phase::Breather);
}

/** The raid summary shares the middle of the screen, and clicks go to the panel first. */
void OpenPanel()
{
	panelOpen = true;
	DismissRaidSummary();
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
Rectangle PanelRect(const Rectangle &spellBar)
{
	const int columns = static_cast<int>(NumSpellSlots);
	const int rows = std::max((static_cast<int>(availableSpells.size()) + columns - 1) / columns, 1);
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

std::optional<size_t> SpellAt(const Rectangle &panel, Point mouse)
{
	for (size_t i = 0; i < availableSpells.size(); i++) {
		if (CellRect(panel, i).contains(mouse))
			return availableSpells[i];
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

std::string FooterText(const SpellBar &bar, std::optional<size_t> hovered)
{
	if (availableSpells.empty())
		return "The host's game has no spells.";
	const auto filled = static_cast<size_t>(NumSpellSlots - std::count(bar.begin(), bar.end(), std::nullopt));
	if (hovered) {
		if (!SlotOf(bar, *hovered) && filled == NumSpellSlots)
			return "The spell bar is full: take a spell off first.";
		return fmt::format("Level {}. {}", GameSpellLevel(*hovered), SpellKinds[*hovered].flavor);
	}
	return fmt::format("{} of {} slots filled. {} a spell to add or remove it. {} closes; Choose spells brings it back until the raid.", filled, NumSpellSlots, InputName(PadAction::Confirm), InputName(PadAction::Cancel));
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
		DrawString(out, InputName(KeeperAction::Spell, *slot), { icon.position + Displacement { 4, 3 }, { icon.size.width - 8, 14 } },
		    { .flags = UiFlags::FontSize12 | UiFlags::ColorWhite | UiFlags::Outlined });
	}
	DrawFrame(out, icon, slot ? PAL16_YELLOW + 2 : (hovered ? PAL16_GRAY : PAL16_GRAY + 8));
	if (slot)
		DrawFrame(out, { icon.position + Displacement { 1, 1 }, { icon.size.width - 2, icon.size.height - 2 } }, PAL16_YELLOW + 2);

	DrawString(out, FitText(SpellKinds[spell].name, cell.size.width), { cell.position + Displacement { 0, IconSize }, { cell.size.width, 13 } },
	    { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | (slot ? UiFlags::ColorGold : UiFlags::ColorWhite) });
	DrawString(out, fmt::format("{} MP", SpellManaCost(spell)), { cell.position + Displacement { 0, IconSize + 13 }, { cell.size.width, 13 } },
	    { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | UiFlags::ColorBlue });
}

} // namespace

void LobbyOnLevelLoaded(SpellBar &bar)
{
	panelOpen = false;
	promptPending = false;
	availableSpells.clear();
	hostSlot = IsJoinedGame() ? NoHostSlot : static_cast<uint8_t>(MyPlayerId);
	hostSeenActive = false;
	if (!IsJoinedGame()) {
		LobbyForgetPicks();
		return;
	}

	for (size_t spell = 0; spell < NumSpellKinds; spell++) {
		if (IsSpellAvailable(spell))
			availableSpells.push_back(spell);
	}
	promptPending = !availableSpells.empty() && promptedSpells != availableSpells;

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
		if (IsSpellAvailable(*spell)) {
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
		LobbyForgetPicks();
	rematchComing = false;
	panelOpen = false;
	promptPending = false;
}

void LobbyForgetPicks()
{
	chosen.reset();
	promptedSpells.reset();
}

void LobbyTick()
{
	if (hostSlot != NoHostSlot) {
		if (Players[hostSlot].plractive)
			hostSeenActive = true;
		else if (hostSeenActive)
			hostSlot = NoHostSlot;
	}

	if (!CanChoose()) {
		panelOpen = false;
		return;
	}
	if (promptPending) {
		promptPending = false;
		promptedSpells = availableSpells;
		OpenPanel();
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
	return ButtonRect(spellBar).contains(mouse) || (panelOpen && PanelRect(spellBar).contains(mouse));
}

bool LobbyOnLeftClick(Point mouse, const Rectangle &spellBar, SpellBar &bar)
{
	if (!CanChoose())
		return false;
	if (ButtonRect(spellBar).contains(mouse)) {
		if (panelOpen)
			panelOpen = false;
		else
			OpenPanel();
		return true;
	}
	if (!panelOpen)
		return false;
	const Rectangle panel = PanelRect(spellBar);
	if (!panel.contains(mouse))
		return false;
	if (const std::optional<size_t> spell = SpellAt(panel, mouse); spell)
		ToggleSpell(bar, *spell);
	return true;
}

void DrawLobby(const Surface &out, Point mouse, const Rectangle &spellBar, const SpellBar &bar)
{
	if (!CanChoose())
		return;

	const Rectangle button = ButtonRect(spellBar);
	DrawPanel(out, button);
	DrawString(out, panelOpen ? fmt::format("Close spells ({})", InputName(PadAction::Cancel)) : std::string("Choose spells"), { button.position + Displacement { 0, 1 }, { button.size.width, 14 } },
	    { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | (button.contains(mouse) ? UiFlags::ColorGold : UiFlags::ColorWhite) });
	if (!panelOpen)
		return;

	const Rectangle panel = PanelRect(spellBar);
	DrawPanel(out, panel);
	const int textWidth = panel.size.width - 2 * Padding;
	DrawString(out, "Spells in the host's game", { panel.position + Displacement { Padding, Padding - 1 }, { textWidth, 14 } },
	    { .flags = UiFlags::FontSize12 | UiFlags::ColorGold });

	const std::optional<size_t> hovered = SpellAt(panel, mouse);
	for (size_t i = 0; i < availableSpells.size(); i++)
		DrawSpellCell(out, CellRect(panel, i), availableSpells[i], bar, hovered == availableSpells[i]);

	const std::string footer = FitLines(FooterText(bar, hovered), textWidth, FooterHeight, FooterLineHeight);
	DrawString(out, footer, { { panel.position.x + Padding, panel.position.y + panel.size.height - Padding - FooterHeight }, { textWidth, FooterHeight } },
	    { .flags = UiFlags::FontSize12 | UiFlags::ColorWhite, .lineHeight = FooterLineHeight });
}

bool LobbyPanelOpen()
{
	return panelOpen && CanChoose();
}

bool LobbyPadTargets(const Rectangle &spellBar, std::vector<Rectangle> &targets)
{
	if (!CanChoose())
		return false;
	if (panelOpen) {
		const Rectangle panel = PanelRect(spellBar);
		for (size_t i = 0; i < availableSpells.size(); i++)
			targets.push_back(CellRect(panel, i));
	}
	targets.push_back(ButtonRect(spellBar));
	return panelOpen;
}

std::string_view EmptySpellSlotMessage()
{
	if (!IsJoinedGame())
		return "No spell in that slot: equip one from the Spellbook";
	if (availableSpells.empty())
		return "The host's game has no spells";
	if (CanChoose())
		return "No spell in that slot: use Choose spells";
	// The last raid has no Breather after it.
	if (!IsAwaitingKeepSnapshot() && State.phase == Phase::Raid && State.wave < NumWaves())
		return "No spell in that slot: choose one after this raid";
	return "No spell in that slot";
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
		const bool host = id == hostSlot;
		std::string tag = host ? "host" : "";
		if (mine)
			tag += host ? ", you" : "you";
		if (!mine && (player._pLvlChanging || !player.isOnActiveLevel()))
			tag += tag.empty() ? "arriving" : ", arriving";
		// Everyone's hero is called the same unless they renamed it.
		const bool sameName = std::any_of(Players.begin(), Players.begin() + NetPlayerSlots, [&](const Player &other) {
			return &other != &player && other.plractive && std::string_view(other._pName) == player._pName;
		});
		rows.push_back({ sameName ? fmt::format("{} {}", player._pName, player.getId() + 1) : std::string(player._pName), std::move(tag), mine });
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

uint8_t LobbyHostSlot()
{
	return hostSlot;
}

void LobbySetHostSlot(uint8_t slot)
{
	hostSlot = slot < NetPlayerSlots ? slot : NoHostSlot;
	hostSeenActive = false;
}

} // namespace devilution::keeper
