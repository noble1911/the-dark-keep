/**
 * @file keeper/keeper.cpp
 *
 * The Dark Keep: bodyless master, free camera, master HUD and controls.
 */
#include "keeper/keeper.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#ifdef USE_SDL3
#include <SDL3/SDL_timer.h>
#endif

#include <fmt/format.h>

#include "control/control.hpp"
#include "controls/controller.h"
#include "cursor.h"
#include "diablo.h"
#include "engine/dx.h"
#include "engine/render/clx_render.hpp"
#include "engine/render/primitive_render.hpp"
#include "engine/render/scrollrt.h"
#include "engine/render/text_render.hpp"
#include "engine/trn.hpp"
#include "game_mode.hpp"
#include "gamemenu.h"
#include "gmenu.h"
#include "init.hpp"
#include "keeper/keeper_bots.hpp"
#include "keeper/keeper_editor.hpp"
#include "keeper/keeper_game.hpp"
#include "keeper/keeper_heroes.hpp"
#include "keeper/keeper_input.hpp"
#include "keeper/keeper_lobby.hpp"
#include "keeper/keeper_map.hpp"
#include "keeper/keeper_pad.hpp"
#include "keeper/keeper_profile.hpp"
#include "keeper/keeper_raid.hpp"
#include "keeper/keeper_roster.hpp"
#include "keeper/keeper_ui.hpp"
#include "keeper/keeper_units.hpp"
#include "keeper/keeper_upgrades.hpp"
#include "levels/dun_tile.hpp"
#include "levels/gendung.h"
#include "levels/trigs.h"
#include "lighting.h"
#include "monster.h"
#include "objects.h"
#include "options.h"
#include "panels/spell_icons.hpp"
#include "player.h"
#include "tables/itemdat.h"
#include "utils/sdl_compat.h"
#include "utils/ui_fwd.h"

namespace devilution::keeper {

bool AutoStart = false;
int TestLordHitPoints = 0;
int TestStartWave = 0;
int TestCampaignLevel = 0;
int TestRank = 0;
AutoNetMode AutoNet = AutoNetMode::None;
char AutoJoinAddress[128] = "127.0.0.1";

namespace {

/** Ambient light level for the whole keep (0 = fully lit, 15 = pitch black). */
constexpr uint8_t AmbientLight = 3;
/** Camera speed in screen pixels per second. */
constexpr float CameraSpeed = 720.F;
/** Mouse distance from the window edge (pixels) that scrolls the camera. */
constexpr int EdgeScrollMargin = 6;
/** A master's character level rises with the campaign, so their spells keep up with stronger heroes. */
constexpr int MasterLevelsPerCampaignLevel = 2;

bool active = false;
uint32_t tickCounter = 0;

/** Free camera position in world tile coordinates (fractional). */
float cameraX = 0.F;
float cameraY = 0.F;
uint32_t lastCameraTicks = 0;

enum class ToolType : uint8_t {
	None,
	Build,
	Spell,
	LockDoor,
	Bait,
	/** Upgrade cards are bought on click; this is never the armed tool. */
	Upgrade,
};

struct Tool {
	ToolType type = ToolType::None;
	size_t index = 0;
};

Tool currentTool;
/** The spell bar for this game: the profile's choice, less anything a host's game doesn't have, or a joiner's picks. */
SpellBar equippedSpells;

const Monster *MyPossessed();
void LoadSpellBar();
bool IsMouseOverUi();

/** Feedback line shown above the palette (e.g. why a placement failed). */
std::string feedback;
uint32_t feedbackUntil = 0;

void SetFeedback(std::string text)
{
	feedback = std::move(text);
	feedbackUntil = SDL_GetTicks() + 2500;
}

Point ParkingTile(const Player &player)
{
	// Top-left corner of the map is always solid rock: nothing walks there and it is never drawn
	// in view of the playable area.
	return { 2 + static_cast<int>(player.getId()), 1 };
}

void ClearPlayerFromMap(const Player &player)
{
	const auto id = static_cast<int8_t>(player.getId() + 1);
	for (int x = 0; x < MAXDUNX; x++) {
		for (int y = 0; y < MAXDUNY; y++) {
			if (dPlayer[x][y] == id || dPlayer[x][y] == -id)
				dPlayer[x][y] = 0;
		}
	}
}

/** Moves a player's (unused) body out of the playable area. Idempotent. */
void ParkPlayer(Player &player)
{
	const Point park = ParkingTile(player);
	if (player.position.tile == park && player.position.future == park)
		return;
	ClearPlayerFromMap(player);
	player.position.tile = park;
	player.position.future = park;
	player.position.old = park;
	player.position.temp = park;
	player.destAction = ACTION_NONE;
	player.walkpath[0] = WALK_NONE;
	if (&player == MyPlayer && player.lightId != NO_LIGHT)
		ChangeLightXY(player.lightId, park);
	ChangeVisionXY(player.getId(), park);
}

/** The master sees the whole keep. Re-applied every tick so player vision updates can't undo it. */
void RevealKeep()
{
	for (int x = 0; x < MAXDUNX; x++) {
		for (int y = 0; y < MAXDUNY; y++) {
			dFlags[x][y] |= DungeonFlag::Visible | DungeonFlag::Lit | DungeonFlag::Explored;
		}
	}
}

void ApplyAmbientLight()
{
	for (int x = 0; x < MAXDUNX; x++) {
		for (int y = 0; y < MAXDUNY; y++) {
			dPreLight[x][y] = std::min(dPreLight[x][y], AmbientLight);
			dLight[x][y] = std::min(dLight[x][y], AmbientLight);
		}
	}
}

void ClampCamera()
{
	// The dungeon proper spans world tiles [16, 16 + 2 * DMAXX).
	constexpr float Min = 16.F;
	constexpr float Max = 16.F + 2 * DMAXX - 1;
	cameraX = std::clamp(cameraX, Min, Max);
	cameraY = std::clamp(cameraY, Min, Max);
}

void CenterCameraOn(Point tile)
{
	cameraX = static_cast<float>(tile.x);
	cameraY = static_cast<float>(tile.y);
	ClampCamera();
	ViewPosition = tile;
}

bool CanUseKeyboardCamera()
{
	return gbActive && !ChatFlag && !gmenu_is_active() && !EditorCapturesKeys();
}

/** The engine's zoom renders a half-size view and doubles it. */
int ZoomFactor()
{
	return *GetOptions().Graphics.zoom ? 2 : 1;
}

void SetZoom(bool zoomed)
{
	if (*GetOptions().Graphics.zoom == zoomed)
		return;
	GetOptions().Graphics.zoom.SetValue(zoomed);
	CalcViewportGeometry();
	CancelBoxDrag(); // the box is in screen pixels, which no longer mean the same place
}

// ---- HUD layout ------------------------------------------------------------
//
// RTS-style command bar along the bottom of the screen (after the C&C sidebar, Total War's unit
// cards and Dungeon Keeper's panel tabs):
//
//   [ status: phase, raid, vault, mana ] [ tabs / scrollable card bar / scout line ] [ card info ]
//
// The bar is at most HudMaxWidth wide and centred, so it stays compact on ultrawide screens.

constexpr int HudMaxWidth = 1280;
constexpr int StatusWidth = 158;
constexpr int InfoWidth = 196;
constexpr int SectionGap = 8;
constexpr int TabHeight = 16;
constexpr int MaxTabWidth = 120;
constexpr int CardWidth = 62;
constexpr int CardHeight = 84;
constexpr int CardGap = 4;
constexpr int PortraitHeight = 56;
constexpr int ArrowWidth = 12;

enum class BarTab : uint8_t {
	Lairs,
	Traps,
	Upgrades,
};
constexpr size_t NumBarTabs = 3;
constexpr std::array<std::string_view, NumBarTabs> BarTabNames { "Lairs", "Traps & Tricks", "Upgrades" };

BarTab currentTab = BarTab::Lairs;
/** First visible card of each tab. */
std::array<int, NumBarTabs> tabScroll {};

/** The cards on a tab, in display order. Each card is the tool it selects. */
std::vector<Tool> BuildTabCards(BarTab tab)
{
	std::vector<Tool> cards;
	switch (tab) {
	case BarTab::Lairs:
		for (const size_t kind : MusterKinds())
			cards.push_back({ ToolType::Build, kind });
		break;
	case BarTab::Traps:
		for (size_t i = 0; i < NumBuildKinds; i++) {
			if (!IsLairKind(i) && IsTrapAvailable(i - NumLairKinds))
				cards.push_back({ ToolType::Build, i });
		}
		cards.push_back({ ToolType::LockDoor, 0 });
		cards.push_back({ ToolType::Bait, 0 });
		break;
	case BarTab::Upgrades:
		for (size_t i = 0; i < NumUpgrades; i++)
			cards.push_back({ ToolType::Upgrade, i });
		break;
	}
	return cards;
}

/** The cards are looked up many times a frame and only change when a level loads. */
std::array<std::vector<Tool>, NumBarTabs> tabCards;

void ClearTabCards()
{
	for (std::vector<Tool> &cards : tabCards)
		cards.clear();
}

const std::vector<Tool> &TabCards(BarTab tab)
{
	std::vector<Tool> &cards = tabCards[static_cast<size_t>(tab)];
	if (cards.empty())
		cards = BuildTabCards(tab);
	return cards;
}

BarTab TabOf(const Tool &tool)
{
	switch (tool.type) {
	case ToolType::Build:
		return IsLairKind(tool.index) ? BarTab::Lairs : BarTab::Traps;
	case ToolType::Upgrade:
		return BarTab::Upgrades;
	default:
		return BarTab::Traps;
	}
}

// ---- UI scale ------------------------------------------------------------------
//
// The HUD is laid out in "UI pixels" and drawn on a smaller off-screen surface that DrawHud blows
// up by a whole factor, so the bar stays readable on big screens (2x at 1440p, 3x at 4K). World
// overlays (labels, bars, placement previews) are drawn at full resolution.

int UiScale()
{
	return std::clamp(gnScreenHeight / 720, 1, 4);
}

int UiWidth()
{
	return gnScreenWidth / UiScale();
}

int UiHeight()
{
	return gnScreenHeight / UiScale();
}

/** The mouse in UI pixels. */
Point UiMouse()
{
	const int scale = UiScale();
	return { MousePosition.x / scale, MousePosition.y / scale };
}

/** Height of the command bar in UI pixels (the engine's control panel height). */
int HudHeight()
{
	return gnScreenHeight - GetMainPanel().position.y;
}

Rectangle HudRect()
{
	const int width = std::min<int>(UiWidth(), HudMaxWidth);
	return { { (UiWidth() - width) / 2, UiHeight() - HudHeight() }, { width, HudHeight() } };
}

/** The off-screen UI surface (UI pixels) when the HUD is scaled, and the view it was drawn over. */
std::optional<OwnedSurface> uiSurface;
std::vector<uint8_t> uiBackground;

/** A surface of UI-pixel size, for the raid panels' hit tests. */
Surface UiHitSurface()
{
	if (UiScale() > 1 && uiSurface)
		return *uiSurface;
	return GlobalBackBuffer();
}

/** Top of the command bar in screen pixels: world overlays stop here. */
int ScreenHudTop()
{
	return HudRect().position.y * UiScale();
}

Rectangle StatusRect()
{
	const Rectangle hud = HudRect();
	return { { hud.position.x + 6, hud.position.y }, { StatusWidth, hud.size.height } };
}

Rectangle InfoRect()
{
	const Rectangle hud = HudRect();
	return { { hud.position.x + hud.size.width - InfoWidth - 6, hud.position.y }, { InfoWidth, hud.size.height } };
}

Rectangle BarRect()
{
	const Rectangle status = StatusRect();
	const int left = status.position.x + status.size.width + SectionGap;
	const int right = InfoRect().position.x - SectionGap;
	return { { left, status.position.y }, { std::max(right - left, CardWidth + 2 * ArrowWidth + 4), status.size.height } };
}

Rectangle TabRect(size_t i)
{
	const Rectangle bar = BarRect();
	const int width = std::min(bar.size.width / static_cast<int>(NumBarTabs), MaxTabWidth);
	return { { bar.position.x + static_cast<int>(i) * width, bar.position.y + 4 }, { width - 3, TabHeight } };
}

Rectangle CardRowRect()
{
	const Rectangle bar = BarRect();
	return { { bar.position.x + ArrowWidth + 2, bar.position.y + TabHeight + 8 }, { bar.size.width - 2 * (ArrowWidth + 2), CardHeight } };
}

int VisibleCardCount()
{
	return std::max(1, (CardRowRect().size.width + CardGap) / (CardWidth + CardGap));
}

int MaxScroll(BarTab tab)
{
	return std::max(0, static_cast<int>(TabCards(tab).size()) - VisibleCardCount());
}

int &ScrollOf(BarTab tab)
{
	int &scroll = tabScroll[static_cast<size_t>(tab)];
	scroll = std::clamp(scroll, 0, MaxScroll(tab));
	return scroll;
}

Rectangle CardRect(int slot)
{
	const Rectangle row = CardRowRect();
	return { { row.position.x + slot * (CardWidth + CardGap), row.position.y }, { CardWidth, CardHeight } };
}

Rectangle LeftArrowRect()
{
	const Rectangle row = CardRowRect();
	return { { row.position.x - ArrowWidth - 2, row.position.y }, { ArrowWidth, CardHeight } };
}

Rectangle RightArrowRect()
{
	const Rectangle row = CardRowRect();
	return { { row.position.x + row.size.width + 2, row.position.y }, { ArrowWidth, CardHeight } };
}

Rectangle ReadyButtonRect()
{
	const Rectangle status = StatusRect();
	return { { status.position.x, status.position.y + status.size.height - 24 }, { StatusWidth, 20 } };
}

bool Contains(const Rectangle &rect, Point p)
{
	return p.x >= rect.position.x && p.y >= rect.position.y && p.x < rect.position.x + rect.size.width && p.y < rect.position.y + rect.size.height;
}

// Spell bar: the masters' spells sit in their own action bar above the info panel. Their
// hotkeys cast instantly at the cursor; clicking a slot arms it for one click in the keep.
constexpr int SpellSlotSize = 60;
constexpr int SpellSlotGap = 4;

Rectangle SpellSlotRect(size_t i)
{
	const Rectangle info = InfoRect();
	constexpr int Total = static_cast<int>(NumSpellSlots) * SpellSlotSize + (static_cast<int>(NumSpellSlots) - 1) * SpellSlotGap;
	const int left = info.position.x + info.size.width - Total;
	return { { left + static_cast<int>(i) * (SpellSlotSize + SpellSlotGap), info.position.y - SpellSlotSize - 8 }, { SpellSlotSize, SpellSlotSize } };
}

/** All the slots together. */
Rectangle SpellBarRect()
{
	const Rectangle first = SpellSlotRect(0);
	const Rectangle last = SpellSlotRect(NumSpellSlots - 1);
	return { first.position, { last.position.x + last.size.width - first.position.x, first.size.height } };
}

std::optional<size_t> SpellSlotAt(Point p)
{
	for (size_t i = 0; i < NumSpellSlots; i++) {
		if (Contains(SpellSlotRect(i), p))
			return i;
	}
	return std::nullopt;
}

/** The card under `p` on the current tab, if any. */
std::optional<Tool> CardAt(Point p)
{
	const std::vector<Tool> &cards = TabCards(currentTab);
	const int first = ScrollOf(currentTab);
	for (int slot = 0; slot < VisibleCardCount() && first + slot < static_cast<int>(cards.size()); slot++) {
		if (Contains(CardRect(slot), p))
			return cards[first + slot];
	}
	return std::nullopt;
}

/** Switches to the tool's tab and scrolls it into view (for hotkeys). */
void RevealCard(const Tool &tool)
{
	currentTab = TabOf(tool);
	const std::vector<Tool> &cards = TabCards(currentTab);
	const auto it = std::find_if(cards.begin(), cards.end(), [&](const Tool &card) { return card.type == tool.type && card.index == tool.index; });
	if (it == cards.end())
		return;
	const int index = static_cast<int>(it - cards.begin());
	int &scroll = ScrollOf(currentTab);
	if (index < scroll)
		scroll = index;
	else if (index >= scroll + VisibleCardCount())
		scroll = index - VisibleCardCount() + 1;
}

std::string FormatTicks(int ticks)
{
	const int seconds = (ticks + 19) / 20;
	return fmt::format("{}:{:02}", seconds / 60, seconds % 60);
}

// ---- cards -------------------------------------------------------------------

struct CardText {
	std::string name;
	std::string hotkey;
	std::string cost;
	std::string description;
	/** Affordable and off cooldown. */
	bool ready = false;
};

/** The key that picks a card: its slot's key while it is on show, plus the key that always picks it, if any. */
std::string CardHotkey(const Tool &tool)
{
	std::string hotkey;
	if (TabOf(tool) == currentTab) {
		const std::vector<Tool> &cards = TabCards(currentTab);
		const auto it = std::find_if(cards.begin(), cards.end(), [&](const Tool &card) { return card.type == tool.type && card.index == tool.index; });
		const int slot = static_cast<int>(it - cards.begin()) - ScrollOf(currentTab);
		if (it != cards.end() && slot >= 0 && slot < VisibleCardCount() && slot < static_cast<int>(NumCardSlotKeys))
			hotkey = InputName(KeeperAction::CardSlot, static_cast<size_t>(slot));
	}
	std::string direct;
	if (tool.type == ToolType::LockDoor)
		direct = InputName(KeeperAction::LockDoor);
	else if (tool.type == ToolType::Bait)
		direct = InputName(KeeperAction::Bait);
	if (!hotkey.empty() && !direct.empty())
		hotkey += "/";
	return hotkey + direct;
}

CardText DescribeCard(const Tool &tool)
{
	const size_t me = MyPlayerId;
	CardText text;
	switch (tool.type) {
	case ToolType::Build: {
		const BuildInfo &kind = BuildKinds[tool.index];
		text.name = std::string(kind.name);
		text.hotkey = CardHotkey(tool);
		text.cost = fmt::format("{}g", kind.cost);
		text.ready = State.vaultGold >= kind.cost;
		if (!IsLairKind(tool.index)) {
			const size_t trap = tool.index - NumLairKinds;
			text.description = fmt::format("Level {}. {}", State.trapLevels[trap], TrapKinds[trap].flavor);
		} else {
			text.ready = text.ready && SupplyUsed() + kind.supply <= SupplyCap();
			text.description = fmt::format("Pop {}  Life {}  Hit {}-{}\n{} Places one monster; if slain it returns after the raid.", kind.supply, KindLife(tool.index), KindMinDamage(tool.index), KindMaxDamage(tool.index), KindFlavor(tool.index));
		}
		break;
	}
	case ToolType::Spell: {
		const SpellKind &spell = SpellKinds[tool.index];
		text.name = std::string(spell.name);
		for (size_t slot = 0; slot < NumSpellSlots; slot++) {
			if (equippedSpells[slot] == tool.index)
				text.hotkey = InputName(KeeperAction::Spell, slot);
		}
		text.cost = fmt::format("{} mana", SpellManaCost(tool.index));
		text.ready = State.mana[me] >= SpellManaCostScaled(tool.index) && State.spellCooldown[me] == 0;
		text.description = fmt::format("Level {}. {} Your spells share a {}s recovery.", State.spellLevels[tool.index], spell.flavor, spell.cooldownTicks / 20);
		break;
	}
	case ToolType::LockDoor:
		text.name = "Lock";
		text.hotkey = CardHotkey(tool);
		text.cost = fmt::format("{}g", LockDoorCost);
		text.ready = State.vaultGold >= LockDoorCost;
		text.description = "Bars a door. Heroes have to bash it down; Warriors do it fastest.";
		break;
	case ToolType::Bait:
		text.name = "Bait";
		text.hotkey = CardHotkey(tool);
		text.cost = fmt::format("{}g", BaitCost);
		text.ready = State.vaultGold >= BaitCost;
		text.description = "A pile of fool's gold. Greedy heroes who see it detour to grab it.";
		break;
	case ToolType::Upgrade: {
		const auto upgrade = static_cast<UpgradeId>(tool.index);
		const UpgradeInfo &info = GetUpgradeInfo(upgrade);
		const int cost = UpgradeNextCost(upgrade);
		text.name = std::string(info.cardName);
		// The corner of the card shows the tier, where other cards show their hotkey.
		text.hotkey = fmt::format("{}/{}", UpgradeTier(upgrade), info.maxTier);
		text.cost = cost > 0 ? fmt::format("{}g", cost) : std::string("MAX");
		text.ready = CanBuyUpgrade(upgrade);
		text.description = UpgradeDescription(upgrade);
		break;
	}
	case ToolType::None:
		break;
	}
	return text;
}

/** Draws `sprite` centred in `area` (bottom-anchored with `drop` pixels cut off below). */
void DrawCentred(const Surface &area, ClxSprite sprite, int drop = 0)
{
	const int x = (area.w() - sprite.width()) / 2;
	const int y = drop == 0 ? (area.h() + sprite.height()) / 2 : area.h() + drop;
	ClxDraw(area, { x, y }, sprite);
}

std::array<KindImage, NumLairKinds> cardImages;

void DrawKindPortrait(const Surface &area, size_t kind)
{
	if (kind >= NumLairKinds)
		return;
	if (!cardImages[kind].built)
		cardImages[kind] = MakeKindImage(kind, area.w(), area.h());
	DrawKindImage(area, cardImages[kind]);
}

void DrawSpellPortrait(const Surface &area, SpellID spell)
{
	SetSpellTrans(SpellType::Spell);
	DrawLargeSpellIcon(area, { (area.w() - SPLICONLENGTH) / 2, (area.h() + SPLICONLENGTH) / 2 }, spell);
}

void DrawPortrait(const Surface &area, const Tool &tool)
{
	switch (tool.type) {
	case ToolType::Build:
		if (IsLairKind(tool.index))
			DrawKindPortrait(area, tool.index);
		else
			DrawSpellPortrait(area, TrapKinds[tool.index - NumLairKinds].icon);
		return;
	case ToolType::Spell:
		DrawSpellPortrait(area, SpellKinds[tool.index].icon);
		return;
	case ToolType::LockDoor:
		DrawCentred(area, GetInvItemSprite(CURSOR_FIRSTITEM + ICURS_LARGE_SHIELD));
		return;
	case ToolType::Bait:
		DrawCentred(area, GetInvItemSprite(CURSOR_FIRSTITEM + ICURS_GOLD_LARGE));
		return;
	case ToolType::Upgrade:
		DrawSpellPortrait(area, GetUpgradeInfo(static_cast<UpgradeId>(tool.index)).icon);
		return;
	case ToolType::None:
		return;
	}
}

void DrawCard(const Surface &out, const Rectangle &rect, const Tool &tool, bool hovered)
{
	const CardText text = DescribeCard(tool);
	const bool selected = currentTool.type == tool.type && currentTool.index == tool.index;

	FillRect(out, rect.position.x, rect.position.y, rect.size.width, rect.size.height, PAL16_GRAY + 14);
	const Rectangle portrait { rect.position + Displacement { 2, 2 }, { rect.size.width - 4, PortraitHeight } };
	FillRect(out, portrait.position.x, portrait.position.y, portrait.size.width, portrait.size.height, 0);
	DrawPortrait(out.subregion(portrait.position.x, portrait.position.y, portrait.size.width, portrait.size.height), tool);

	if (!text.ready) {
		// Greyed out like Dungeon Keeper's unaffordable rooms.
		DrawHalfTransparentRectTo(out, portrait.position.x, portrait.position.y, portrait.size.width, portrait.size.height);
		DrawHalfTransparentRectTo(out, portrait.position.x, portrait.position.y, portrait.size.width, portrait.size.height);
	}
	if (tool.type == ToolType::Spell && State.spellCooldown[MyPlayerId] > 0) {
		// C&C-style recharge: a shade that drains away as the spell recovers.
		const int cooldownTicks = SpellKinds[tool.index].cooldownTicks;
		const int shade = portrait.size.height * std::min(State.spellCooldown[MyPlayerId], cooldownTicks) / std::max(cooldownTicks, 1);
		DrawHalfTransparentRectTo(out, portrait.position.x, portrait.position.y, portrait.size.width, shade);
	}

	DrawString(out, FitText(text.hotkey, portrait.size.width - 6), { portrait.position + Displacement { 3, 1 }, { portrait.size.width - 6, 12 } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorWhite });
	DrawString(out, FitText(text.name, rect.size.width - 2), { rect.position + Displacement { 0, PortraitHeight + 3 }, { rect.size.width, 12 } },
	    { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | (text.ready ? UiFlags::ColorWhite : UiFlags::ColorBlack) });
	DrawString(out, FitText(text.cost, rect.size.width - 2), { rect.position + Displacement { 0, PortraitHeight + 15 }, { rect.size.width, 12 } },
	    { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | (text.ready ? UiFlags::ColorGold : UiFlags::ColorRed) });

	uint8_t frame = PAL16_GRAY + 8;
	if (selected)
		frame = PAL16_YELLOW + 2;
	else if (hovered)
		frame = PAL16_GRAY;
	else if (!text.ready)
		frame = PAL16_RED + 6;
	DrawFrame(out, rect, frame);
	if (selected)
		DrawFrame(out, { rect.position + Displacement { 1, 1 }, { rect.size.width - 2, rect.size.height - 2 } }, PAL16_YELLOW + 2);
}

void DrawArrow(const Surface &out, const Rectangle &rect, std::string_view glyph, bool enabled)
{
	FillRect(out, rect.position.x, rect.position.y, rect.size.width, rect.size.height, PAL16_GRAY + 14);
	DrawFrame(out, rect, enabled ? PAL16_GRAY + 4 : PAL16_GRAY + 12);
	DrawString(out, glyph, { rect.position + Displacement { 0, rect.size.height / 2 - 7 }, { rect.size.width, 14 } },
	    { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | (enabled ? UiFlags::ColorGold : UiFlags::ColorBlack) });
}

void DrawCardBar(const Surface &out)
{
	for (size_t i = 0; i < NumBarTabs; i++) {
		const Rectangle tab = TabRect(i);
		const bool selected = static_cast<size_t>(currentTab) == i;
		FillRect(out, tab.position.x, tab.position.y, tab.size.width, tab.size.height, selected ? PAL16_RED + 12 : PAL16_GRAY + 14);
		DrawFrame(out, tab, selected ? PAL16_YELLOW + 4 : PAL16_GRAY + 10);
		DrawString(out, FitText(BarTabNames[i], tab.size.width - 2), { tab.position + Displacement { 0, 2 }, { tab.size.width, 14 } },
		    { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | (selected ? UiFlags::ColorGold : UiFlags::ColorWhite) });
	}

	const std::vector<Tool> &cards = TabCards(currentTab);
	const int first = ScrollOf(currentTab);
	const std::optional<Tool> hovered = CardAt(UiMouse());
	for (int slot = 0; slot < VisibleCardCount() && first + slot < static_cast<int>(cards.size()); slot++) {
		const Tool &card = cards[first + slot];
		DrawCard(out, CardRect(slot), card, hovered && hovered->type == card.type && hovered->index == card.index);
	}
	DrawArrow(out, LeftArrowRect(), "<", first > 0);
	DrawArrow(out, RightArrowRect(), ">", first < MaxScroll(currentTab));
}

void DrawSpellBar(const Surface &out)
{
	const std::optional<size_t> hovered = SpellSlotAt(UiMouse());
	for (size_t i = 0; i < NumSpellSlots; i++) {
		const Rectangle slot = SpellSlotRect(i);
		FillRect(out, slot.position.x, slot.position.y, slot.size.width, slot.size.height, 0);
		const Rectangle icon { slot.position + Displacement { 2, 2 }, { slot.size.width - 4, slot.size.height - 4 } };
		const std::optional<size_t> spell = equippedSpells[i];
		bool armed = false;
		bool ready = true;
		if (spell) {
			const Tool tool { ToolType::Spell, *spell };
			const CardText text = DescribeCard(tool);
			ready = text.ready;
			armed = currentTool.type == ToolType::Spell && currentTool.index == *spell;
			DrawPortrait(out.subregion(icon.position.x, icon.position.y, icon.size.width, icon.size.height), tool);
			if (!text.ready)
				DrawHalfTransparentRectTo(out, icon.position.x, icon.position.y, icon.size.width, icon.size.height);
			if (State.spellCooldown[MyPlayerId] > 0) {
				const int cooldownTicks = SpellKinds[*spell].cooldownTicks;
				const int shade = icon.size.height * std::min(State.spellCooldown[MyPlayerId], cooldownTicks) / std::max(cooldownTicks, 1);
				DrawHalfTransparentRectTo(out, icon.position.x, icon.position.y, icon.size.width, shade);
			}
			DrawString(out, std::to_string(SpellManaCost(*spell)), { icon.position + Displacement { 0, icon.size.height - 14 }, { icon.size.width - 2, 14 } },
			    { .flags = UiFlags::FontSize12 | UiFlags::AlignRight | UiFlags::Outlined | (text.ready ? UiFlags::ColorBlue : UiFlags::ColorRed) });
		}
		DrawString(out, InputName(KeeperAction::Spell, i), { icon.position + Displacement { 2, 1 }, { icon.size.width - 4, 14 } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorWhite | UiFlags::Outlined });

		uint8_t frame = PAL16_GRAY + 8;
		if (armed)
			frame = PAL16_YELLOW + 2;
		else if (hovered == i)
			frame = PAL16_GRAY;
		else if (!ready)
			frame = PAL16_RED + 6;
		DrawFrame(out, slot, frame);
		if (armed)
			DrawFrame(out, { slot.position + Displacement { 1, 1 }, { slot.size.width - 2, slot.size.height - 2 } }, PAL16_YELLOW + 2);
	}
}

Rectangle SellButtonRect()
{
	const Rectangle info = InfoRect();
	return { { info.position.x + 6, info.position.y + info.size.height - 28 }, { info.size.width - 12, 18 } };
}

/** The selected structure, when exactly one is selected. */
std::optional<StructureRef> SelectedStructure()
{
	const std::vector<StructureRef> &selected = Selection();
	if (selected.size() != 1)
		return std::nullopt;
	return selected.front();
}

/** A living monster of a lair, if any. */
const Monster *LairMonster(size_t slot)
{
	const Lair &lair = State.lairs[slot];
	for (const int16_t member : lair.members) {
		if (member < 0)
			continue;
		const Monster &monster = Monsters[member];
		if (!monster.isInvalid && monster.hitPoints > 0 && monster.mode != MonsterMode::Death && monster.type().type == BuildKinds[lair.kind].type)
			return &monster;
	}
	return nullptr;
}

void DrawSellButton(const Surface &out, const std::string &label, bool canSell)
{
	const Rectangle button = SellButtonRect();
	FillRect(out, button.position.x, button.position.y, button.size.width, button.size.height, PAL16_RED + 12);
	DrawFrame(out, button, Contains(button, UiMouse()) ? PAL16_YELLOW + 2 : PAL16_GRAY + 8);
	DrawString(out, label, { button.position + Displacement { 0, 2 }, { button.size.width, 14 } },
	    { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | (canSell ? UiFlags::ColorGold : UiFlags::ColorBlack) });
}

/** "Del: " in front of a label, or nothing if selling has no key bound. */
std::string SellKeyPrefix()
{
	const std::string key = InputName(KeeperAction::Sell);
	return key.empty() ? std::string() : key + ": ";
}

/** Info panel for several selected structures: how many of what, and what selling them all pays. */
void DrawGroupPanel(const Surface &out, Point textPos, int textWidth)
{
	const std::vector<StructureRef> &selected = Selection();
	DrawString(out, fmt::format("{} selected", selected.size()), { textPos, { textWidth, 14 } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorGold });

	std::vector<int> kinds(BuildKinds.size());
	bool canSell = false;
	for (const StructureRef &ref : selected) {
		if (ref.type == StructureType::Lair)
			kinds[State.lairs[ref.slot].kind]++;
		canSell = canSell || CanSell(ref);
	}
	std::string summary;
	for (size_t i = 0; i < kinds.size(); i++) {
		if (kinds[i] > 0)
			summary += fmt::format("{}{} {}", summary.empty() ? "" : ", ", kinds[i], BuildKinds[i].name);
	}
	summary += fmt::format("\n{}: rally around a point. {}: add a waypoint.", InputName(PadAction::Order), InputName(PadAction::Waypoint));
	DrawString(out, FitLines(summary, textWidth, 70, 13), { textPos + Displacement { 0, 14 }, { textWidth, 70 } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorWhite, .lineHeight = 13 });
	DrawSellButton(out, fmt::format("{}sell all for {}g{}", SellKeyPrefix(), SelectionRefund(), State.phase == Phase::Raid ? " (half)" : ""), canSell);
}

/** Info panel for a selected structure: its state, rally point and the sell button. */
void DrawStructurePanel(const Surface &out, StructureRef ref, Point textPos, int textWidth)
{
	DrawString(out, StructureName(ref), { textPos, { textWidth, 14 } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorGold });
	std::string state;
	std::string help;
	switch (ref.type) {
	case StructureType::Lair: {
		const Lair &lair = State.lairs[ref.slot];
		if (lair.summonTicks > 0) {
			state = "Summoning...";
		} else if (const Monster *monster = LairMonster(ref.slot); monster != nullptr) {
			const std::string_view duty = lair.routeCount > 0 ? "On patrol" : (lair.hasRally ? "At rally point" : "Guarding lair");
			state = fmt::format("{} ({}%)", duty, monster->maxHitPoints > 0 ? 100 * monster->hitPoints / monster->maxHitPoints : 0);
		} else {
			state = "Slain: returns after the raid";
		}
		help = fmt::format("Pop {}. {}: rally point. {}: waypoint.{}", BuildKinds[lair.kind].supply, InputName(PadAction::Order), IsPadInUse() ? InputName(PadAction::Waypoint) : InputName(PadAction::Modifier), lair.hasRally || lair.routeCount > 0 ? " On lair: clear." : "");
		break;
	}
	case StructureType::Trap:
		state = State.traps[ref.slot].cooldown > 0 ? "Rearming" : "Armed";
		break;
	case StructureType::Bait:
		state = fmt::format("Can fool {} more heroes", State.baits[ref.slot].uses);
		break;
	case StructureType::Lock:
		state = fmt::format("Holds for {} more blows", State.locks[ref.slot].strength);
		break;
	}
	DrawString(out, state, { textPos + Displacement { 0, 14 }, { textWidth, 14 } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorWhite });
	if (!help.empty())
		DrawString(out, FitLines(help, textWidth, 52, 13), { textPos + Displacement { 0, 32 }, { textWidth, 52 } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorWhite, .lineHeight = 13 });

	DrawSellButton(out, fmt::format("{}sell for {}g{}", SellKeyPrefix(), RefundFor(ref), State.phase == Phase::Raid ? " (half)" : ""), CanSell(ref));
}

/** Sells everything selected (the sell key or button). */
void SellSelected()
{
	const std::vector<StructureRef> selected = Selection();
	if (selected.empty())
		return;
	std::string_view reason;
	if (!CanSell(selected.front(), &reason)) {
		SetFeedback(std::string(reason));
		return;
	}
	for (const StructureRef &ref : selected)
		RequestSell(ref);
	ClearSelection();
}

/** Right-hand panel: details of the hovered (or selected) card, otherwise the controls. */
void DrawInfoPanel(const Surface &out)
{
	const Rectangle info = InfoRect();
	FillRect(out, info.position.x, info.position.y + 4, info.size.width, info.size.height - 8, PAL16_GRAY + 14);
	DrawFrame(out, { info.position + Displacement { 0, 4 }, { info.size.width, info.size.height - 8 } }, PAL16_GRAY + 10);
	const Point textPos = info.position + Displacement { 6, 8 };
	const int textWidth = info.size.width - 12;

	std::optional<Tool> shown = CardAt(UiMouse());
	if (!shown) {
		if (const std::optional<size_t> slot = SpellSlotAt(UiMouse()); slot && equippedSpells[*slot])
			shown = Tool { ToolType::Spell, *equippedSpells[*slot] };
	}
	if (!shown && currentTool.type != ToolType::None)
		shown = currentTool;
	if (!shown) {
		if (Selection().size() > 1) {
			DrawGroupPanel(out, textPos, textWidth);
			return;
		}
		if (const std::optional<StructureRef> selected = SelectedStructure(); selected) {
			DrawStructurePanel(out, *selected, textPos, textWidth);
			return;
		}
		DrawString(out, "Keeper's orders", { textPos, { textWidth, 14 } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorGold });
		const std::string controls = IsPadInUse()
		    ? fmt::format("{}: the cards. {}: pick one, then place it.\n{}: aim  {}: move\n{}: gate or vault  {}: zoom\n{}: possess  {}: menu",
		          InputName(PadAction::Navigate), InputName(PadAction::Confirm), InputName(PadAction::MoveCursor), InputName(PadAction::MoveCamera), InputName(PadAction::Jump), InputName(PadAction::Zoom), InputName(PadAction::Possess), InputName(PadAction::Menu))
		    : fmt::format("Pick a card, then click the keep. Click a monster or hero for more. Drag: select.\n{}{}{}{}: move\n{}: gate  {}: vault\n{}: possess",
		          KeyLabel(KeeperAction::CameraUp), KeyLabel(KeeperAction::CameraLeft), KeyLabel(KeeperAction::CameraDown), KeyLabel(KeeperAction::CameraRight),
		          KeyLabel(KeeperAction::CenterEntrance), KeyLabel(KeeperAction::CenterVault), KeyLabel(KeeperAction::Possess));
		DrawString(out, WordWrapString(controls, textWidth),
		    { textPos + Displacement { 0, 16 }, { textWidth, info.size.height - 28 } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorWhite, .lineHeight = 13 });
		return;
	}
	const CardText text = DescribeCard(*shown);
	DrawString(out, FitText(text.name, textWidth * 2 / 3), { textPos, { textWidth, 14 } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorGold });
	if (!text.hotkey.empty())
		DrawString(out, FitText(fmt::format("[{}]", text.hotkey), textWidth / 3), { textPos, { textWidth, 14 } }, { .flags = UiFlags::FontSize12 | UiFlags::AlignRight | UiFlags::ColorWhite });
	DrawString(out, text.cost, { textPos + Displacement { 0, 14 }, { textWidth, 14 } }, { .flags = UiFlags::FontSize12 | (text.ready ? UiFlags::ColorGold : UiFlags::ColorRed) });
	const int textBottom = info.position.y + info.size.height - 8 - (textPos.y + 32);
	DrawString(out, FitLines(text.description, textWidth, textBottom, 13), { textPos + Displacement { 0, 32 }, { textWidth, textBottom } },
	    { .flags = UiFlags::FontSize12 | UiFlags::ColorWhite, .lineHeight = 13 });
}

// ---- world overlays ----------------------------------------------------------

void PutPixel(const Surface &out, int x, int y, uint8_t color, int maxY)
{
	if (x < 0 || y < 0 || x >= out.w() || y >= maxY)
		return;
	out.SetPixel({ x, y }, color);
}

void DrawLine(const Surface &out, Point a, Point b, uint8_t color, int maxY)
{
	int dx = std::abs(b.x - a.x);
	int dy = -std::abs(b.y - a.y);
	const int sx = a.x < b.x ? 1 : -1;
	const int sy = a.y < b.y ? 1 : -1;
	int err = dx + dy;
	Point p = a;
	while (true) {
		PutPixel(out, p.x, p.y, color, maxY);
		if (p == b)
			break;
		const int e2 = 2 * err;
		if (e2 >= dy) {
			err += dy;
			p.x += sx;
		}
		if (e2 <= dx) {
			err += dx;
			p.y += sy;
		}
	}
}

/**
 * DrawOverlays runs twice: its floor markers go in the world view between the floor and what stands
 * on it (walls hide them, and the view isn't zoomed yet), and its text goes over everything in the HUD.
 */
enum class OverlayPass : uint8_t {
	Floor,
	Text,
};
OverlayPass overlayPass = OverlayPass::Text;

int OverlayZoom()
{
	return overlayPass == OverlayPass::Floor ? 1 : ZoomFactor();
}

/** Screen position of the centre of a tile's floor diamond. */
Point TileCenter(Point tile)
{
	const int zoom = OverlayZoom();
	const Point halfCenter = GetScreenPosition(tile) + Displacement { TILE_WIDTH / 2, -TILE_HEIGHT / 2 };
	return { halfCenter.x * zoom, halfCenter.y * zoom };
}

/** A line along the floor between two tiles' centres. */
void DrawFloorLine(const Surface &out, Point from, Point to, uint8_t color)
{
	if (overlayPass == OverlayPass::Floor)
		DrawLine(out, TileCenter(from), TileCenter(to), color, out.h());
}

/** Outlines a tile's floor diamond (optionally grown by `radius` tiles). */
void DrawTileDiamond(const Surface &out, Point tile, uint8_t color, int radius = 0)
{
	if (overlayPass != OverlayPass::Floor)
		return;
	const int zoom = OverlayZoom();
	const int maxY = out.h();
	const int hw = TILE_WIDTH / 2 * (radius * 2 + 1) * zoom;
	const int hh = TILE_HEIGHT / 2 * (radius * 2 + 1) * zoom;
	const Point center = TileCenter(tile);
	const Point left = center + Displacement { -hw, 0 };
	const Point top = center + Displacement { 0, -hh };
	const Point right = center + Displacement { hw, 0 };
	const Point bottom = center + Displacement { 0, hh };
	DrawLine(out, left, top, color, maxY);
	DrawLine(out, top, right, color, maxY);
	DrawLine(out, right, bottom, color, maxY);
	DrawLine(out, bottom, left, color, maxY);
}

void DrawWorldLabel(const Surface &out, Point tile, std::string_view text, UiFlags color, int raise = 0)
{
	if (overlayPass != OverlayPass::Text)
		return;
	const int zoom = ZoomFactor();
	const Point base = GetScreenPosition(tile);
	const Point anchor = base + Displacement { TILE_WIDTH / 2, -TILE_HEIGHT };
	const Point at { anchor.x * zoom - 60, anchor.y * zoom - 4 - raise * zoom };
	if (at.y < 0 || at.y + 14 >= ScreenHudTop())
		return;
	DrawString(out, text, { at, { 120, 14 } }, { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | color | UiFlags::Outlined });
}

/** The hero a Stone Curse cast at `tile` would take (it petrifies the nearest one within 5 tiles). */
std::optional<Point> StoneCurseTarget(Point tile)
{
	std::optional<Point> best;
	int bestDistance = 6;
	auto consider = [&](Point position) {
		const int distance = position.WalkingDistance(tile);
		if (distance < bestDistance) {
			best = position;
			bestDistance = distance;
		}
	};
	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		const Monster &monster = Monsters[ActiveMonsters[i]];
		if (IsHero(monster) && monster.hitPoints > 0 && monster.mode != MonsterMode::Petrified)
			consider(monster.position.tile);
	}
	for (size_t i = 0; i < MaxBots; i++) {
		const Player &bot = Players[FirstBotId + i];
		if (IsLivingBot(bot) && !IsPetrifiedBot(bot))
			consider(bot.position.tile);
	}
	return best;
}

/** Fire Wall runs north-east / south-west from the target until something blocks it. */
void DrawFireWallPreview(const Surface &out, Point tile)
{
	DrawTileDiamond(out, tile, PAL16_ORANGE + 2);
	for (const Direction dir : { Direction::NorthEast, Direction::SouthWest }) {
		Point at = tile;
		for (int step = 0; step < 6; step++) {
			at += dir;
			if (!InDungeonBounds(at) || TileHasAny(at, TileProperties::BlockMissile))
				break;
			DrawTileDiamond(out, at, PAL16_ORANGE + 2);
		}
	}
}

/**
 * Placement preview under the cursor: the footprint (gold when it can go there, red with the reason
 * when not) and the reach of what is being placed. Lairs also get a ghost of the monster (DrawPlacementGhost).
 */
void DrawToolPreview(const Surface &out)
{
	const Point tile = cursPosition;
	std::string_view reason;
	switch (currentTool.type) {
	case ToolType::Build: {
		const bool ok = CanBuildAt(currentTool.index, tile, &reason);
		if (!IsLairKind(currentTool.index))
			DrawTileDiamond(out, tile, ok ? PAL16_ORANGE + 6 : PAL16_RED + 6, 1); // fires at heroes on or next to it
		DrawTileDiamond(out, tile, ok ? PAL16_YELLOW + 2 : PAL16_RED + 2);
		if (!ok)
			DrawWorldLabel(out, tile, reason, UiFlags::ColorRed);
		return;
	}
	case ToolType::Bait: {
		const bool ok = CanPlaceBaitAt(tile, &reason);
		if (ok)
			DrawTileDiamond(out, tile, PAL16_YELLOW + 8, 8); // heroes who can see it within 8 tiles detour
		DrawTileDiamond(out, tile, ok ? PAL16_YELLOW + 2 : PAL16_RED + 2);
		if (!ok)
			DrawWorldLabel(out, tile, reason, UiFlags::ColorRed);
		return;
	}
	case ToolType::LockDoor: {
		const bool ok = CanLockDoorAt(tile, &reason);
		const Object *door = FindDoorAt(tile);
		DrawTileDiamond(out, door != nullptr ? door->position : tile, ok ? PAL16_YELLOW + 2 : PAL16_RED + 2);
		if (!ok)
			DrawWorldLabel(out, tile, reason, UiFlags::ColorRed);
		return;
	}
	case ToolType::Spell:
		switch (static_cast<MissileID>(SpellKinds[currentTool.index].missile)) {
		case MissileID::FireWallControl:
			DrawFireWallPreview(out, tile);
			return;
		case MissileID::StoneCurse:
			DrawTileDiamond(out, tile, PAL16_BLUE + 6, 5);
			if (const std::optional<Point> target = StoneCurseTarget(tile); target)
				DrawTileDiamond(out, *target, PAL16_GRAY + 4);
			else
				DrawWorldLabel(out, tile, "No hero in reach", UiFlags::ColorRed);
			return;
		case MissileID::LightningWallControl:
			DrawFireWallPreview(out, tile);
			return;
		case MissileID::Apocalypse:
			DrawTileDiamond(out, tile, PAL16_ORANGE + 2, 8);
			DrawTileDiamond(out, tile, PAL16_BLUE + 2);
			return;
		case MissileID::ChainLightning:
			DrawTileDiamond(out, tile, PAL16_BLUE + 6, std::min<int>(SpellEngineLevel(currentTool.index) + 3, MaxCrawlRadius));
			DrawTileDiamond(out, tile, PAL16_BLUE + 2);
			return;
		case MissileID::Guardian:
			DrawTileDiamond(out, tile, PAL16_BLUE + 6, 6); // shoots heroes within 6 tiles
			DrawTileDiamond(out, tile, PAL16_BLUE + 2);
			return;
		default:
			DrawTileDiamond(out, tile, PAL16_BLUE + 4, 3);
			DrawTileDiamond(out, tile, PAL16_BLUE + 2);
			return;
		}
	case ToolType::Upgrade:
	case ToolType::None:
		return;
	}
}

/** A lair's patrol: the loop from its guard post through each waypoint and back again. */
void DrawPatrolRoute(const Surface &out, const Lair &lair)
{
	if (lair.routeCount == 0)
		return;
	const Point post = lair.hasRally ? lair.rally : lair.position;
	Point from = post;
	for (size_t i = 0; i < lair.routeCount; i++) {
		const Point waypoint = lair.route[i];
		DrawFloorLine(out, from, waypoint, PAL16_BLUE + 6);
		DrawTileDiamond(out, waypoint, PAL16_BLUE + 2);
		DrawWorldLabel(out, waypoint, std::to_string(i + 1), UiFlags::ColorBlue);
		from = waypoint;
	}
	DrawFloorLine(out, from, post, PAL16_BLUE + 6);
}

/** One selected structure: highlighted, with a lair's monster, rally point and patrol route. */
void DrawSelectedStructure(const Surface &out, StructureRef ref)
{
	const Point position = StructurePosition(ref);
	DrawTileDiamond(out, position, PAL16_YELLOW + 2);
	DrawTileDiamond(out, position, PAL16_YELLOW + 2, 1);
	if (ref.type != StructureType::Lair)
		return;
	const Lair &lair = State.lairs[ref.slot];
	if (const Monster *monster = LairMonster(ref.slot); monster != nullptr)
		DrawTileDiamond(out, monster->position.tile, PAL16_YELLOW + 4);
	if (lair.hasRally) {
		DrawFloorLine(out, position, lair.rally, PAL16_BLUE + 4);
		DrawTileDiamond(out, lair.rally, PAL16_BLUE + 2);
		DrawWorldLabel(out, lair.rally, "Rally point", UiFlags::ColorBlue);
	}
	DrawPatrolRoute(out, lair);
}

void DrawSelectionOverlay(const Surface &out)
{
	const std::vector<StructureRef> selected = Selection();
	for (const StructureRef &ref : selected)
		DrawSelectedStructure(out, ref);
	const std::vector<size_t> lairs = SelectedLairs();
	if (lairs.empty() || (lairs.size() == 1 && cursPosition == State.lairs[lairs.front()].position))
		return;
	// Where a right-click would send them.
	if (currentTool.type == ToolType::None && MyPossessed() == nullptr && !IsMouseOverUi())
		DrawTileDiamond(out, cursPosition, CanRallyTo(lairs.front(), cursPosition) ? PAL16_BLUE + 6 : PAL16_RED + 6);
}

/** The drag-select box, in screen pixels and clear of the command bar. */
void DrawBoxDrag(const Surface &out)
{
	if (gmenu_is_active())
		CancelBoxDrag();
	const std::optional<Rectangle> box = BoxDragRect(MousePosition, cameraX, cameraY, ZoomFactor());
	if (!box)
		return;
	const int maxY = ScreenHudTop();
	const Point topLeft = box->position;
	const Point topRight = topLeft + Displacement { box->size.width - 1, 0 };
	const Point bottomLeft = topLeft + Displacement { 0, box->size.height - 1 };
	const Point bottomRight = topRight + Displacement { 0, box->size.height - 1 };
	DrawLine(out, topLeft, topRight, PAL16_YELLOW + 2, maxY);
	DrawLine(out, topRight, bottomRight, PAL16_YELLOW + 2, maxY);
	DrawLine(out, bottomRight, bottomLeft, PAL16_YELLOW + 2, maxY);
	DrawLine(out, bottomLeft, topLeft, PAL16_YELLOW + 2, maxY);
}

/** True if a monster standing on `tile` shows inside `box` (screen pixels). */
bool IsMonsterInBox(Point tile, const Rectangle &box)
{
	// A monster is drawn upwards from its feet, about a tile wide and a tile and a half tall.
	const int zoom = ZoomFactor();
	const Point feet = TileCenter(tile);
	const int left = feet.x - TILE_WIDTH / 4 * zoom;
	const int right = feet.x + TILE_WIDTH / 4 * zoom;
	const int top = feet.y - 3 * TILE_HEIGHT * zoom / 2;
	return left < box.position.x + box.size.width && right >= box.position.x && top < box.position.y + box.size.height && feet.y >= box.position.y;
}

/** Selects the master's monsters in the box. A lair whose monster isn't there to box counts by its own tile. */
void SelectInBox(const Rectangle &box)
{
	std::vector<StructureRef> found;
	for (size_t slot = 0; slot < MaxLairs; slot++) {
		const Lair &lair = State.lairs[slot];
		if (!lair.active)
			continue;
		bool hit = false;
		bool anyAlive = false;
		for (const int16_t member : lair.members) {
			if (member < 0 || LairOfMonster(static_cast<size_t>(member)) != slot)
				continue;
			anyAlive = true;
			hit = hit || IsMonsterInBox(Monsters[member].position.tile, box);
		}
		if (!anyAlive)
			hit = Contains(box, TileCenter(lair.position));
		if (hit)
			found.push_back({ StructureType::Lair, slot });
	}
	SetSelection(std::move(found));
}

void DrawOverlays(const Surface &out)
{
	DrawTileDiamond(out, State.vault, PAL16_YELLOW + 2, 1);
	DrawWorldLabel(out, State.vault, fmt::format("Vault: {} gold", State.vaultGold), UiFlags::ColorGold);
	DrawTileDiamond(out, State.entrance, PAL16_BLUE + 2, 1);
	DrawWorldLabel(out, State.entrance, "Entrance", UiFlags::ColorBlue);
	if (const Monster *boss = FindBoss(); boss != nullptr)
		DrawWorldLabel(out, boss->position.tile, "The Keep Lord", UiFlags::ColorRed, 70);
	if (const std::optional<Point> portal = PortalPosition(); portal)
		DrawWorldLabel(out, *portal, "Heroes' portal", UiFlags::ColorBlue, 60);

	for (const Lair &lair : State.lairs) {
		if (!lair.active)
			continue;
		DrawTileDiamond(out, lair.position, PAL16_RED + 4);
		// Single-monster spawns are numerous, so only label the ones still summoning.
		if (lair.summonTicks > 0)
			DrawWorldLabel(out, lair.position, "Summoning...", UiFlags::ColorRed);
	}
	for (const Trap &trap : State.traps) {
		if (!trap.active)
			continue;
		DrawTileDiamond(out, trap.position, trap.cooldown > 0 ? PAL16_GRAY + 8 : PAL16_ORANGE + 2);
	}
	for (const Bait &bait : State.baits) {
		if (!bait.active)
			continue;
		DrawTileDiamond(out, bait.position, PAL16_YELLOW + 6);
		DrawWorldLabel(out, bait.position, "Fool's gold", UiFlags::ColorGold);
	}
	for (const DoorLock &lock : State.locks) {
		if (!lock.active)
			continue;
		DrawTileDiamond(out, lock.position, PAL16_RED + 2);
		DrawWorldLabel(out, lock.position, fmt::format("Locked ({})", lock.strength), UiFlags::ColorRed);
	}

	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		const Monster &monster = Monsters[ActiveMonsters[i]];
		if (!IsHero(monster) || monster.hitPoints <= 0)
			continue;
		DrawWorldLabel(out, monster.position.tile, HeroTitle(monster), UiFlags::ColorWhite, 64);
	}

	DrawSelectionOverlay(out);

	if (const Monster *possessed = MyPossessed(); possessed != nullptr) {
		DrawTileDiamond(out, possessed->position.tile, PAL16_ORANGE + 4);
		if (pcursmonst >= 0 && IsHero(Monsters[pcursmonst]))
			DrawTileDiamond(out, Monsters[pcursmonst].position.tile, PAL16_RED + 2);
		return;
	}

	if (currentTool.type == ToolType::None || IsMouseOverUi())
		return;
	DrawToolPreview(out);
}

void SelectTool(ToolType type, size_t index)
{
	if (currentTool.type == type && currentTool.index == index) {
		currentTool = {};
		return;
	}
	currentTool = { type, index };
	ClearSelection();
}

/** A click with no tool: one of the master's own monsters (its lair), or a structure on the tile. */
bool SelectStructureUnderCursor()
{
	std::optional<StructureRef> found;
	if (pcursmonst >= 0) {
		if (const std::optional<size_t> lair = LairOfMonster(static_cast<size_t>(pcursmonst)); lair)
			found = StructureRef { StructureType::Lair, *lair };
	}
	if (!found)
		found = FindStructureAt(cursPosition);
	if (found)
		SetSelection({ *found });
	else
		ClearSelection();
	return found.has_value();
}

/** Right-click with lairs selected: send their monsters to guard there, spread out (or back to their lairs). */
bool SetRallyPoint(Point tile)
{
	const std::vector<size_t> lairs = SelectedLairs();
	if (lairs.empty())
		return false;
	// One lair: on or next to it means "come home". A group may well want to gather near one of its lairs.
	const int homeDistance = lairs.size() == 1 ? 1 : 0;
	const bool home = std::any_of(lairs.begin(), lairs.end(), [&](size_t slot) { return tile.WalkingDistance(State.lairs[slot].position) <= homeDistance; });
	if (home) {
		for (const size_t slot : lairs) {
			if (State.lairs[slot].hasRally || State.lairs[slot].routeCount > 0)
				RequestRally(slot, std::nullopt);
		}
		return true;
	}
	std::string_view reason;
	if (!CanRallyTo(lairs.front(), tile, &reason)) {
		SetFeedback(std::string(reason));
		return true;
	}
	const std::vector<Point> tiles = SpreadTiles(tile, lairs.size());
	for (size_t i = 0; i < lairs.size(); i++)
		RequestRally(lairs[i], tiles[i % tiles.size()]);
	return true;
}

void BuyUpgradeCard(size_t index)
{
	const auto upgrade = static_cast<UpgradeId>(index);
	std::string_view reason;
	if (!CanBuyUpgrade(upgrade, &reason)) {
		SetFeedback(std::string(reason));
		return;
	}
	RequestBuyUpgrade(upgrade);
}

/** A card was clicked or its slot key pressed: buy an upgrade, arm a tool, or say why not. */
void ActivateCard(const Tool &card)
{
	if (card.type == ToolType::Upgrade) {
		BuyUpgradeCard(card.index);
	} else {
		SelectTool(card.type, card.index);
	}
}

/** Shift+right-click with lairs selected: add a waypoint to each one's patrol, spread out like a rally. */
bool AddPatrolWaypoint(Point tile)
{
	const std::vector<size_t> lairs = SelectedLairs();
	if (lairs.empty())
		return false;
	std::string_view reason;
	if (!CanRallyTo(lairs.front(), tile, &reason)) {
		SetFeedback(std::string(reason));
		return true;
	}
	const std::vector<Point> tiles = SpreadTiles(tile, lairs.size());
	for (size_t i = 0; i < lairs.size(); i++) {
		if (CanAddWaypoint(lairs[i], tiles[i % tiles.size()], &reason))
			RequestWaypoint(lairs[i], tiles[i % tiles.size()]);
		else
			SetFeedback(std::string(reason));
	}
	return true;
}

bool IsGameOver()
{
	return State.phase == Phase::Victory || State.phase == Phase::Defeat;
}

// ---- the choice after the siege -----------------------------------------------

constexpr int GameOverBannerHeight = 100;
constexpr int GameOverButtonWidth = 240;
constexpr int GameOverButtonHeight = 20;
constexpr int GameOverButtonGap = 12;

/** The next setup, worked out once when the game ends: the banner asks every frame, and it reads a map. */
std::optional<std::pair<bool, std::optional<KeepSetup>>> setupAfterGame;

/** What the "continue" button starts: only the game's creator gets one. */
std::optional<KeepSetup> GameOverContinue()
{
	if (IsJoinedGame())
		return std::nullopt;
	const bool won = State.phase == Phase::Victory;
	if (!setupAfterGame || setupAfterGame->first != won)
		setupAfterGame.emplace(won, ContinueSetup(won));
	return setupAfterGame->second;
}

void LoadSpellBar()
{
	equippedSpells = {};
	const std::array<int8_t, NumSpellSlots> &chosen = ProfileSpellBar();
	size_t used = 0;
	for (const int8_t spell : chosen) {
		if (spell >= 0 && IsSpellAvailable(static_cast<size_t>(spell)))
			equippedSpells[used++] = static_cast<size_t>(spell);
	}
	// In someone else's game the player's choices may not exist there: fill up from what does.
	if (used == 0) {
		for (size_t spell = 0; spell < NumSpellKinds && used < NumSpellSlots; spell++) {
			if (IsSpellAvailable(spell))
				equippedSpells[used++] = spell;
		}
	}
}

std::string GameOverContinueLabel(const KeepSetup &next)
{
	const std::string key = IsPadInUse() ? InputName(PadAction::Confirm) : std::string("Enter");
	if (IsEditorTestRun())
		return fmt::format("Back to editor ({})", key);
	if (State.phase != Phase::Victory)
		return fmt::format("Retry ({})", key);
	if (CurrentSetup().mode != GameMode::Campaign)
		return fmt::format("Play again ({})", key);
	return fmt::format("Next: {} ({})", CampaignLevelRules(next.campaignLevel).name, key);
}

Rectangle GameOverBanner(int width, int hudTop)
{
	return { { 0, hudTop / 2 - GameOverBannerHeight * 3 / 5 }, { width, GameOverBannerHeight } };
}

/** Button `index` of the `count` in the banner's row (the continue button, if any, comes first). */
Rectangle GameOverButton(int width, int hudTop, int index, int count)
{
	const Rectangle banner = GameOverBanner(width, hudTop);
	const int row = count * GameOverButtonWidth + (count - 1) * GameOverButtonGap;
	return { { (width - row) / 2 + index * (GameOverButtonWidth + GameOverButtonGap), banner.position.y + banner.size.height - GameOverButtonHeight - 6 },
		{ GameOverButtonWidth, GameOverButtonHeight } };
}

void ChooseGameOverContinue()
{
	if (IsEditorTestRun()) {
		EditorEndTestRun();
		return;
	}
	if (const std::optional<KeepSetup> next = GameOverContinue(); next)
		RequestRestart(*next);
}

const Monster *MyPossessed()
{
	const int16_t id = State.possessed[MyPlayerId];
	return id >= 0 ? &Monsters[id] : nullptr;
}

void TogglePossession()
{
	if (MyPossessed() != nullptr) {
		RequestRelease();
		return;
	}
	if (pcursmonst < 0 || !CanPossess(Monsters[pcursmonst])) {
		SetFeedback("Point at one of your monsters to possess it");
		return;
	}
	if (State.mana[MyPlayerId] < PossessManaCost * ManaScale) {
		SetFeedback("Not enough mana");
		return;
	}
	currentTool = {};
	RequestPossess(static_cast<size_t>(pcursmonst));
}

/** Casts spell `index` at `tile` if the master can; says why not otherwise. */
bool TryCastSpell(size_t index, Point tile)
{
	const size_t me = MyPlayerId;
	if (IsGameOver()) {
		SetFeedback("The siege is over");
		return false;
	}
	if (State.phase != Phase::Raid) {
		SetFeedback("Spells can only be cast during a raid");
		return false;
	}
	if (State.spellCooldown[me] > 0) {
		SetFeedback(fmt::format("Gathering power... {}", FormatTicks(State.spellCooldown[me])));
		return false;
	}
	if (State.mana[me] < SpellManaCostScaled(index)) {
		SetFeedback("Not enough mana");
		return false;
	}
	RequestCastSpell(index, tile);
	return true;
}

/** True if the mouse is over the HUD or one of the panels floating above it. */
bool IsMouseOverUi()
{
	if (UiMouse().y >= HudRect().position.y || SpellSlotAt(UiMouse()) || LobbyIsMouseOver(UiMouse(), SpellBarRect()))
		return true;
	const std::optional<Rectangle> inspect = InspectPanelRect(HudRect().position.x + 6, HudRect().position.y);
	if (inspect && Contains(*inspect, UiMouse()))
		return true;
	const Surface ui = UiHitSurface();
	const std::optional<Rectangle> roster = RaidRosterRect(ui);
	const std::optional<Rectangle> summary = RaidSummaryRect(ui, HudRect().position.y);
	return (roster && Contains(*roster, UiMouse())) || (summary && Contains(*summary, UiMouse()));
}

void UseToolAt(Point tile)
{
	switch (currentTool.type) {
	case ToolType::None:
		return;
	case ToolType::Build: {
		std::string_view reason;
		if (!CanBuildAt(currentTool.index, tile, &reason)) {
			SetFeedback(std::string(reason));
			return;
		}
		RequestBuild(currentTool.index, tile);
		return;
	}
	case ToolType::Bait: {
		std::string_view reason;
		if (!CanPlaceBaitAt(tile, &reason)) {
			SetFeedback(std::string(reason));
			return;
		}
		RequestPlaceBait(tile);
		return;
	}
	case ToolType::LockDoor: {
		std::string_view reason;
		if (!CanLockDoorAt(tile, &reason)) {
			SetFeedback(std::string(reason));
			return;
		}
		RequestLockDoor(FindDoorAt(tile)->position);
		return;
	}
	case ToolType::Spell:
		// One cast per arming, like a C&C support power.
		if (TryCastSpell(currentTool.index, tile))
			currentTool = {};
		return;
	case ToolType::Upgrade:
		return;
	}
}

} // namespace

bool IsActive()
{
	return active && gbRunGame && currlevel == KeepLevel && !setlevel;
}

void OnLevelLoaded()
{
	active = currlevel == KeepLevel && !setlevel;
	if (!active)
		return;

	setupAfterGame.reset();
	// ViewPosition was set by the level generator to the up-stairs arrival point.
	const Point entrance = ViewPosition;

	for (Player &player : Players) {
		if (player.plractive && player.isOnActiveLevel() && !IsBot(player))
			ParkPlayer(player);
	}

	RevealKeep();
	ApplyAmbientLight();
	ResetGame(entrance, FindVault(entrance));
	ResetUnits();
	LoadSpellBar();
	LobbyOnLevelLoaded(equippedSpells);
	CenterCameraOn(entrance);
	currentTool = {};
	ClearSelection();
	CancelBoxDrag();
	tickCounter = 0;
	lastCameraTicks = SDL_GetTicks();
	NoteMapLevelLoaded();
	ClearTabCards();
	// Built during the load rather than as cards scroll into view, which would stutter.
	if (!IsEditorActive()) {
		for (const size_t kind : MusterKinds()) {
			if (!cardImages[kind].built)
				cardImages[kind] = MakeKindImage(kind, CardWidth - 4, PortraitHeight);
		}
	}
	EditorOnLevelLoaded();
}

void OnGameEnded()
{
	EditorOnGameEnded();
	ForgetMapCache();
	SaveProfile();
	LobbyOnGameEnded();
	active = false;
	ClearSelection();
	CancelBoxDrag();
}

void Tick()
{
	if (!IsActive())
		return;

	// The host chose what comes next: leave, then StartGame's loop brings this client into it.
	if (TakeRestartRequest()) {
		LobbyNoteRematch();
		gamemenu_exit_game(false);
		ReturnToMainMenu = false;
		return;
	}

	// Remote masters arrive later than us; park every body on the keep level, every tick (cheap, idempotent).
	const auto masterLevel = static_cast<uint8_t>(1 + MasterLevelsPerCampaignLevel * (CurrentLevelNumber() - 1));
	for (Player &player : Players) {
		if (player.plractive && player.isOnActiveLevel() && !player._pLvlChanging && !IsBot(player)) {
			ParkPlayer(player);
			// Many spells scale with the caster's level as well as the spell's.
			if (player.getCharacterLevel() != masterLevel) {
				player.setCharacterLevel(masterLevel);
				// What the level changes too: a joiner checks the host's numbers against its own and drops it on any difference.
				CalcPlrInv(player, false);
			}
		}
	}
	RevealKeep();
	if (IsEditorActive())
		return; // a map being edited has no raid to simulate

	GameTick();
	LobbyTick();
	UpdateUnitMarkers();

	tickCounter++;
	if (tickCounter % 4 == 0) {
		for (size_t i = 0; i < ActiveMonsterCount; i++) {
			Monster &monster = Monsters[ActiveMonsters[i]];
			if (monster.hitPoints > 0 && monster.mode != MonsterMode::Death)
				UpdateEnemy(monster);
		}
	}
	if (tickCounter % 10 == 0)
		LeashMonsters();
}

uint32_t DevKeeperTicks()
{
	return tickCounter;
}

void DevCenterCamera(Point tile)
{
	CenterCameraOn(tile);
}

void UpdateCamera()
{
	if (!IsActive())
		return;
	EditorUpdateFrame();

	const uint32_t now = SDL_GetTicks();
	const float dt = std::min<uint32_t>(now - lastCameraTicks, 50) / 1000.F;
	lastCameraTicks = now;

	if (const Monster *possessed = MyPossessed(); possessed != nullptr) {
		// Ride along with the possessed monster.
		const float follow = std::min(1.F, dt * 8.F);
		cameraX += (static_cast<float>(possessed->position.tile.x) - cameraX) * follow;
		cameraY += (static_cast<float>(possessed->position.tile.y) - cameraY) * follow;
		ClampCamera();
		ViewPosition = { static_cast<int>(std::lround(cameraX)), static_cast<int>(std::lround(cameraY)) };
		return;
	}

	float sx = 0;
	float sy = 0;
	if (CanUseKeyboardCamera()) {
		const auto *keys = SDLC_GetKeyState();
		if (IsKeyHeld(KeeperAction::CameraLeft) || keys[SDL_SCANCODE_LEFT]) sx -= 1;
		if (IsKeyHeld(KeeperAction::CameraRight) || keys[SDL_SCANCODE_RIGHT]) sx += 1;
		if (IsKeyHeld(KeeperAction::CameraUp) || keys[SDL_SCANCODE_UP]) sy -= 1;
		if (IsKeyHeld(KeeperAction::CameraDown) || keys[SDL_SCANCODE_DOWN]) sy += 1;

		if (!IsPadInUse()) {
			if (MousePosition.x <= EdgeScrollMargin) sx = -1;
			if (MousePosition.x >= gnScreenWidth - 1 - EdgeScrollMargin) sx = 1;
			if (MousePosition.y <= EdgeScrollMargin) sy = -1;
			if (MousePosition.y >= gnScreenHeight - 1 - EdgeScrollMargin) sy = 1;
		}
		PadCameraPull(sx, sy);
	}
	if (sx == 0 && sy == 0)
		return;

	// Screen pixels -> world tiles. One world step +x is (+32, +16) px on screen, +y is (-32, +16) px.
	// Zoomed in, the same screen speed covers half the ground.
	const float dsx = sx * CameraSpeed * dt / static_cast<float>(ZoomFactor());
	const float dsy = sy * CameraSpeed * dt / static_cast<float>(ZoomFactor());
	cameraX += dsx / 64.F + dsy / 32.F;
	cameraY += -dsx / 64.F + dsy / 32.F;
	ClampCamera();

	ViewPosition = { static_cast<int>(std::lround(cameraX)), static_cast<int>(std::lround(cameraY)) };
}

Displacement CameraScrollOffset()
{
	if (!IsActive())
		return {};
	const float fx = cameraX - static_cast<float>(ViewPosition.x);
	const float fy = cameraY - static_cast<float>(ViewPosition.y);
	// Moving the camera by +fx/+fy shifts the world the opposite way on screen.
	return {
		-static_cast<int>(std::lround((fx - fy) * (TILE_WIDTH / 2))),
		-static_cast<int>(std::lround((fx + fy) * (TILE_HEIGHT / 2))),
	};
}

void ApplyViewShift(Point &position, Displacement &offset)
{
	if (!IsActive())
		return;
	// One extra tile of margin to the north-west (same trick the engine uses while the hero walks).
	position += Displacement(Direction::NorthWest);
	offset += Displacement { -TILE_WIDTH / 2, -TILE_HEIGHT / 2 };
	offset += CameraScrollOffset();
}

void GrowViewRange(int &rows, int &columns)
{
	if (!IsActive())
		return;
	rows += 3;
	columns += 2;
}

bool ShouldDrawPlayer(const Player &player)
{
	return !IsActive() || IsBot(player); // masters have no body; bot heroes do
}

bool CanMonstersTargetPlayers()
{
	return !IsActive();
}

void PerformKeeperAction(KeeperAction action, size_t index)
{
	switch (action) {
	case KeeperAction::CardSlot: {
		const std::vector<Tool> &cards = TabCards(currentTab);
		const size_t card = static_cast<size_t>(ScrollOf(currentTab)) + index;
		if (static_cast<int>(index) < VisibleCardCount() && card < cards.size())
			ActivateCard(cards[card]);
		return;
	}
	case KeeperAction::Spell:
		if (index >= NumSpellSlots)
			return;
		if (!equippedSpells[index]) {
			SetFeedback(std::string(EmptySpellSlotMessage()));
			return;
		}
		if (MyPossessed() == nullptr && !IsMouseOverUi())
			TryCastSpell(*equippedSpells[index], cursPosition);
		else
			SelectTool(ToolType::Spell, *equippedSpells[index]);
		return;
	case KeeperAction::LockDoor:
	case KeeperAction::Bait:
		SelectTool(action == KeeperAction::LockDoor ? ToolType::LockDoor : ToolType::Bait, 0);
		if (currentTool.type != ToolType::None)
			RevealCard(currentTool);
		return;
	case KeeperAction::NextTab:
		currentTab = static_cast<BarTab>((static_cast<size_t>(currentTab) + 1) % NumBarTabs);
		return;
	case KeeperAction::StartRaid:
		if (State.phase == Phase::Setup || State.phase == Phase::Breather)
			RequestStartRaid();
		return;
	case KeeperAction::CenterEntrance:
		CenterCameraOn(State.entrance);
		return;
	case KeeperAction::CenterVault:
		CenterCameraOn(State.vault);
		return;
	case KeeperAction::ZoomIn:
		SetZoom(true);
		return;
	case KeeperAction::ZoomOut:
		SetZoom(false);
		return;
	case KeeperAction::Possess:
		TogglePossession();
		return;
	case KeeperAction::Sell:
		SellSelected();
		return;
	// Polled every frame (see UpdateCamera).
	case KeeperAction::CameraLeft:
	case KeeperAction::CameraRight:
	case KeeperAction::CameraUp:
	case KeeperAction::CameraDown:
		return;
	}
}

bool OnKeyDown(SDL_Keycode key)
{
	if (!IsActive())
		return false;
	if (IsEditorActive())
		return EditorOnKeyDown(key);
	if (EditorOnTestKey(key))
		return true;

	if (IsGameOver() && (key == SDLK_RETURN || key == SDLK_KP_ENTER) && GameOverContinue()) {
		ChooseGameOverContinue();
		return true;
	}
	if (key == SDLK_ESCAPE && LobbyOnEscape())
		return true;
	if (DispatchBoundKey(static_cast<uint32_t>(key)))
		return true;

	switch (key) {
	// The default camera keys and the arrows: swallow them so they don't trigger hero hotkeys.
	case SDLK_W:
	case SDLK_A:
	case SDLK_S:
	case SDLK_D:
	case SDLK_UP:
	case SDLK_DOWN:
	case SDLK_LEFT:
	case SDLK_RIGHT:
	// Hero panels have no meaning for the master.
	case SDLK_I:
	case SDLK_B:
	case SDLK_Q:
	case SDLK_Y:
	case SDLK_0:
		return true;
	// Second keys for bound actions; these work unless bound to something else.
	case SDLK_HOME:
		CenterCameraOn(State.entrance);
		return true;
	case SDLK_KP_PLUS:
		SetZoom(true);
		return true;
	case SDLK_KP_MINUS:
		SetZoom(false);
		return true;
	case SDLK_BACKSPACE:
		SellSelected();
		return true;
	case SDLK_ESCAPE:
		if (IsBoxDragging()) {
			CancelBoxDrag();
			return true;
		}
		if (MyPossessed() != nullptr) {
			RequestRelease();
			return true;
		}
		if (currentTool.type != ToolType::None) {
			currentTool = {};
			return true;
		}
		if (!Selection().empty()) {
			ClearSelection();
			return true;
		}
		if (IsRaidSummaryVisible()) {
			DismissRaidSummary();
			return true;
		}
		if (HasSelectedHero()) {
			ClearSelectedHero();
			return true;
		}
		if (IsGameOver()) {
			gamemenu_exit_game(false);
			return true;
		}
		return false;
	default:
		return false;
	}
}

bool OnLeftMouseDown(uint16_t /*modState*/)
{
	if (!IsActive())
		return false;
	if (IsEditorActive())
		return EditorOnMouseDown(/*right=*/false);

	if (LobbyOnLeftClick(UiMouse(), SpellBarRect(), equippedSpells)) {
		if (currentTool.type == ToolType::Spell && std::find(equippedSpells.begin(), equippedSpells.end(), currentTool.index) == equippedSpells.end())
			currentTool = {}; // it just came off the bar
		return true;
	}
	if (IsGameOver()) {
		const int width = UiHitSurface().w();
		const int top = HudRect().position.y;
		const std::optional<KeepSetup> next = GameOverContinue();
		const int count = next ? 2 : 1;
		if (next && Contains(GameOverButton(width, top, 0, count), UiMouse())) {
			ChooseGameOverContinue();
			return true;
		}
		if (Contains(GameOverButton(width, top, count - 1, count), UiMouse())) {
			gamemenu_exit_game(false);
			return true;
		}
	}
	if (const std::optional<size_t> slot = SpellSlotAt(UiMouse()); slot) {
		if (equippedSpells[*slot])
			SelectTool(ToolType::Spell, *equippedSpells[*slot]);
		return true;
	}
	{
		const Surface ui = UiHitSurface();
		if (const std::optional<Rectangle> summary = RaidSummaryRect(ui, HudRect().position.y); summary && Contains(*summary, UiMouse())) {
			DismissRaidSummary();
			return true;
		}
		if (const std::optional<Point> heroTile = OnRaidRosterClick(ui, UiMouse()); heroTile) {
			CenterCameraOn(*heroTile);
			return true;
		}
	}
	if (IsMouseOverUi() && UiMouse().y < HudRect().position.y)
		return true; // the inspect panel
	if (UiMouse().y >= HudRect().position.y) {
		for (size_t i = 0; i < NumBarTabs; i++) {
			if (Contains(TabRect(i), UiMouse()))
				currentTab = static_cast<BarTab>(i);
		}
		// Long tabs (the Bestiary, a late-game Lairs tab): the arrows page through them.
		const int scrollStep = static_cast<int>(TabCards(currentTab).size()) > 2 * VisibleCardCount() ? VisibleCardCount() : 1;
		if (Contains(LeftArrowRect(), UiMouse()))
			ScrollOf(currentTab) -= scrollStep;
		if (Contains(RightArrowRect(), UiMouse()))
			ScrollOf(currentTab) += scrollStep;
		if (const std::optional<Tool> card = CardAt(UiMouse()); card)
			ActivateCard(*card);
		if (Contains(ReadyButtonRect(), UiMouse()) && (State.phase == Phase::Setup || State.phase == Phase::Breather))
			RequestStartRaid();
		if (Contains(SellButtonRect(), UiMouse()) && currentTool.type == ToolType::None && !CardAt(UiMouse()) && !Selection().empty())
			SellSelected();
		return true;
	}

	if (MyPossessed() != nullptr) {
		const bool attack = pcursmonst >= 0 && IsHero(Monsters[pcursmonst]);
		RequestOrder(cursPosition, attack ? pcursmonst : -1);
		return true;
	}
	// With no tool in hand a click inspects a hero, or selects one of the master's own monsters or structures.
	if (currentTool.type == ToolType::None) {
		// The click does its job now; if the mouse then moves away it turns into a drag-select.
		BeginBoxDrag(MousePosition, cameraX, cameraY);
		if (SelectHeroUnderCursor()) {
			ClearSelection();
			return true;
		}
		ClearSelectedHero();
		SelectStructureUnderCursor();
		return true;
	}
	UseToolAt(cursPosition);
	return true;
}

void OnLeftMouseUp()
{
	if (!IsActive() || !IsBoxDragging())
		return;
	const std::optional<Rectangle> box = BoxDragRect(MousePosition, cameraX, cameraY, ZoomFactor());
	CancelBoxDrag();
	if (!box || MyPossessed() != nullptr)
		return;
	ClearSelectedHero();
	SelectInBox(*box);
}

bool OnRightMouseDown()
{
	if (!IsActive())
		return false;
	if (IsEditorActive())
		return EditorOnMouseDown(/*right=*/true);
	if (LobbyIsMouseOver(UiMouse(), SpellBarRect()))
		return true;
	if (currentTool.type == ToolType::None && MyPossessed() == nullptr && !IsMouseOverUi()) {
		const bool shift = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
		if (shift ? AddPatrolWaypoint(cursPosition) : SetRallyPoint(cursPosition))
			return true;
	}
	if (currentTool.type == ToolType::None) {
		ClearSelectedHero();
		ClearSelection();
	}
	currentTool = {};
	return true;
}

bool OnMouseWheel(int deltaY)
{
	if (!IsActive() || deltaY == 0 || gmenu_is_active())
		return false;
	if (IsEditorActive())
		return EditorOnMouseWheel(deltaY);
	if (DispatchBoundKey(deltaY > 0 ? MouseScrollUpButton : MouseScrollDownButton))
		return true;
	if (Contains(BarRect(), UiMouse())) {
		// Over the card bar the wheel scrolls the cards instead of zooming.
		ScrollOf(currentTab) += deltaY > 0 ? -1 : 1;
		return true;
	}
	SetZoom(deltaY > 0);
	return true;
}

void DrawPlacementGhost(const Surface &out, Point tile, Point targetBufferPosition)
{
	if (!IsActive() || currentTool.type != ToolType::Build || !IsLairKind(currentTool.index) || tile != cursPosition || MyPossessed() != nullptr || IsMouseOverUi())
		return;
	const OptionalClxSpriteList sprites = KindPortrait(currentTool.index);
	if (!sprites || sprites->numSprites() == 0)
		return;
	const ClxSprite sprite = (*sprites)[0];
	const Point position = targetBufferPosition + Displacement { -CalculateSpriteTileCenterX(sprite.width()), 0 };
	if (CanBuildAt(currentTool.index, tile))
		ClxDrawBlended(out, position, sprite);
	else
		ClxDrawBlendedTRN(out, position, sprite, GetInfravisionTRN()); // red: can't go here
}

namespace {

void DrawUi(const Surface &out);

/**
 * Draws the UI at UiScale(): shrinks the screen into the UI surface (so translucent panels blend
 * with the view), draws the UI there, and copies back only the pixels the UI changed, blown up.
 */
void DrawScaledUi(const Surface &out, void (*drawUi)(const Surface &) = DrawUi)
{
	const int scale = UiScale();
	const int width = out.w() / scale;
	const int height = out.h() / scale;
	if (!uiSurface || uiSurface->w() != width || uiSurface->h() != height)
		uiSurface.emplace(width, height);
	const Surface &ui = *uiSurface;
	uiBackground.resize(static_cast<size_t>(width) * height);
	for (int y = 0; y < height; y++) {
		const uint8_t *src = out.at(0, y * scale);
		uint8_t *dst = ui.at(0, y);
		for (int x = 0; x < width; x++)
			dst[x] = src[x * scale];
		std::memcpy(&uiBackground[static_cast<size_t>(y) * width], dst, width);
	}

	drawUi(ui);

	for (int y = 0; y < height; y++) {
		const uint8_t *row = ui.at(0, y);
		const uint8_t *before = &uiBackground[static_cast<size_t>(y) * width];
		if (std::memcmp(row, before, width) == 0)
			continue;
		for (int x = 0; x < width; x++) {
			if (row[x] == before[x])
				continue;
			for (int dy = 0; dy < scale; dy++)
				std::memset(out.at(x * scale, y * scale + dy), row[x], scale);
		}
	}
}

} // namespace

void DrawFloorOverlays(const Surface &out)
{
	if (!IsActive() || IsEditorActive())
		return;
	overlayPass = OverlayPass::Floor;
	DrawOverlays(out);
	overlayPass = OverlayPass::Text;
}

void DrawHud(const Surface &out)
{
	if (IsEditorActive()) {
		EditorDrawWorld(out);
		const auto drawEditorUi = [](const Surface &ui) {
			EditorDrawUi(ui);
			PadDrawOverlay(ui);
		};
		if (UiScale() > 1)
			DrawScaledUi(out, drawEditorUi);
		else
			drawEditorUi(out);
		PadDrawCursor(out);
		return;
	}
	DrawOverlays(out);
	DrawBoxDrag(out);
	DrawUnitBars(out);
	if (UiScale() > 1)
		DrawScaledUi(out);
	else
		DrawUi(out);
	PadDrawCursor(out);
}

namespace {

void DrawUi(const Surface &out)
{
	const Rectangle hud = HudRect();
	const int top = hud.position.y;
	const int height = out.h() - top;

	FillRect(out, 0, top, out.w(), height, 0);
	DrawHorizontalLine(out, { 0, top }, out.w(), PAL16_RED + 8);

	const Rectangle status = StatusRect();
	const int left = status.position.x + 2;
	const int columnWidth = status.size.width - 4;
	const size_t me = MyPlayerId;

	// Status column: phase, raid, vault, heroes, mana.
	std::string phaseLine = std::string(PhaseName(State.phase));
	if (State.phase == Phase::Setup || State.phase == Phase::Breather)
		phaseLine += fmt::format("  {}", FormatTicks(State.phaseTicks));
	DrawString(out, phaseLine, { { left, top + 4 }, { columnWidth, 16 } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorRed });
	DrawString(out, fmt::format("Raid {}/{}", std::min<int>(State.wave, NumWaves()), NumWaves()), { { left, top + 19 }, { columnWidth, 16 } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorWhite });
	const int supply = SupplyUsed();
	DrawString(out, fmt::format("Pop {}/{}", supply, SupplyCap()), { { left, top + 19 }, { columnWidth, 16 } },
	    { .flags = UiFlags::FontSize12 | UiFlags::AlignRight | (supply >= SupplyCap() ? UiFlags::ColorRed : UiFlags::ColorWhite) });
	DrawString(out, fmt::format("Vault: {} gold", State.vaultGold), { { left, top + 34 }, { columnWidth, 16 } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorGold });
	const int inFlight = GoldInFlight();
	DrawString(out, fmt::format("Heroes {}  Loot {}", LivingHeroCount(), inFlight), { { left, top + 49 }, { columnWidth, 16 } }, { .flags = UiFlags::FontSize12 | (inFlight > 0 ? UiFlags::ColorRed : UiFlags::ColorWhite) });

	const int mana = State.mana[me] / ManaScale;
	const Rectangle manaRect { { left, top + 68 }, { columnWidth, 10 } };
	FillRect(out, manaRect.position.x, manaRect.position.y, manaRect.size.width, manaRect.size.height, PAL16_GRAY + 14);
	FillRect(out, manaRect.position.x, manaRect.position.y, manaRect.size.width * mana / MaxMana, manaRect.size.height, PAL16_BLUE + 4);
	DrawFrame(out, manaRect, PAL16_GRAY + 8);
	DrawString(out, fmt::format("Mana {}", mana), { manaRect.position + Displacement { 0, 12 }, { columnWidth, 14 } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorBlue });

	if (State.phase == Phase::Setup || State.phase == Phase::Breather) {
		const Rectangle ready = ReadyButtonRect();
		FillRect(out, ready.position.x, ready.position.y, ready.size.width, ready.size.height, PAL16_RED + 12);
		DrawFrame(out, ready, PAL16_YELLOW + 4);
		const std::string raidKey = InputName(KeeperAction::StartRaid);
		DrawString(out, raidKey.empty() ? std::string("Open the gates") : fmt::format("{}: {}", raidKey, IsPadInUse() ? "Open gates" : "Open the gates"), { ready.position + Displacement { 0, 3 }, { ready.size.width, 14 } }, { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | UiFlags::ColorGold });
	}

	DrawCardBar(out);
	DrawSpellBar(out);
	DrawInfoPanel(out);
	DrawInspectPanel(out, hud.position.x + 6, top);
	DrawRaidRoster(out, UiMouse());
	DrawRaidSummary(out, top);
	DrawMasters(out);
	DrawLobby(out, UiMouse(), SpellBarRect(), equippedSpells);

	// Scout line under the cards: the coming raid, the Lord's health, the portal.
	const Rectangle bar = BarRect();
	std::string scout = State.phase == Phase::Raid ? fmt::format("Raid {} in progress", State.wave) : (IsGameOver() ? std::string() : DescribeWave(State.wave));
	UiFlags scoutColor = UiFlags::ColorGold;
	if (const Monster *boss = FindBoss(); boss != nullptr && !IsGameOver()) {
		const int hp = boss->maxHitPoints > 0 ? 100 * boss->hitPoints / boss->maxHitPoints : 0;
		scout += fmt::format("  -  Keep Lord {}%", hp);
	} else if (State.bossDown && !IsGameOver()) {
		const int channel = PortalChannelPercent();
		scout = PortalPosition() ? std::string("The Lord is down - the heroes are fleeing through their portal!")
		    : channel >= 0       ? fmt::format("The Lord is down - a hero is opening a portal ({}%)!", channel)
		                         : std::string("The Lord is down - stop the heroes from escaping!");
		scoutColor = UiFlags::ColorRed;
	}
	if (const Monster *possessed = MyPossessed(); possessed != nullptr) {
		const int hp = possessed->maxHitPoints > 0 ? 100 * possessed->hitPoints / possessed->maxHitPoints : 0;
		scout = fmt::format("Possessing {} ({}%)  {}: move/attack  {}: release", possessed->name(), hp, IsPadInUse() ? InputName(PadAction::Confirm) : std::string("click"), InputName(KeeperAction::Possess));
	}
	DrawString(out, scout, { { bar.position.x, CardRowRect().position.y + CardHeight + 3 }, { bar.size.width, 14 } }, { .flags = UiFlags::FontSize12 | scoutColor });

	// Feedback floats just above the bar, like an RTS's "insufficient funds".
	if (!feedback.empty() && SDL_GetTicks() < feedbackUntil) {
		const int width = std::min<int>(GetLineWidth(feedback) + 16, out.w());
		const Rectangle line { { (out.w() - width) / 2, top - 22 }, { width, 18 } };
		DrawHalfTransparentRectTo(out, line.position.x, line.position.y, line.size.width, line.size.height);
		DrawString(out, feedback, { line.position + Displacement { 0, 2 }, { line.size.width, 14 } }, { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | UiFlags::ColorRed });
	}

	if (IsGameOver()) {
		const bool won = State.phase == Phase::Victory;
		const Rectangle banner = GameOverBanner(out.w(), top);
		DrawHalfTransparentRectTo(out, banner.position.x, banner.position.y, banner.size.width, banner.size.height);
		DrawString(out, won ? "THE KEEP STANDS" : "THE KEEP HAS FALLEN", { banner.position + Displacement { 0, 10 }, { banner.size.width, 30 } },
		    { .flags = UiFlags::FontSize30 | UiFlags::AlignCenter | (won ? UiFlags::ColorGold : UiFlags::ColorRed) });
		DrawString(out, (won ? fmt::format("All {} raids repelled with {} gold left.", NumWaves(), State.vaultGold) : std::string("The Keep Lord is slain and the heroes escaped.")) + fmt::format(" +{} XP.", MapXpEarned()) + (IsJoinedGame() ? " The host chooses what is next." : ""),
		    { banner.position + Displacement { 0, 50 }, { banner.size.width, 16 } }, { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | UiFlags::ColorWhite });
		const std::optional<KeepSetup> next = GameOverContinue();
		const int count = next ? 2 : 1;
		const auto drawButton = [&](int index, std::string_view label) {
			const Rectangle button = GameOverButton(out.w(), top, index, count);
			DrawHalfTransparentRectTo(out, button.position.x, button.position.y, button.size.width, button.size.height);
			const UiFlags color = Contains(button, UiMouse()) ? UiFlags::ColorGold : UiFlags::ColorWhite;
			DrawString(out, label, { button.position + Displacement { 0, 3 }, { button.size.width, 14 } }, { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | color });
		};
		if (next)
			drawButton(0, GameOverContinueLabel(*next));
		drawButton(count - 1, fmt::format("{} ({})", IsJoinedGame() ? "Leave" : "Menu", InputName(PadAction::Cancel)));
	}
	EditorDrawTestBanner(out);
	PadDrawOverlay(out);
}

} // namespace

// ---- controller -----------------------------------------------------------------

void PadHudTargets(std::vector<Rectangle> &targets)
{
	const Surface ui = UiHitSurface();
	const int top = HudRect().position.y;
	if (IsGameOver()) {
		const int count = GameOverContinue() ? 2 : 1;
		for (int i = 0; i < count; i++)
			targets.push_back(GameOverButton(ui.w(), top, i, count));
		return;
	}
	if (LobbyPadTargets(SpellBarRect(), targets))
		return; // the panel takes the clicks
	for (size_t i = 0; i < NumBarTabs; i++)
		targets.push_back(TabRect(i));
	const int first = ScrollOf(currentTab);
	const int cards = static_cast<int>(TabCards(currentTab).size());
	for (int slot = 0; slot < VisibleCardCount() && first + slot < cards; slot++)
		targets.push_back(CardRect(slot));
	for (size_t i = 0; i < NumSpellSlots; i++) {
		if (equippedSpells[i])
			targets.push_back(SpellSlotRect(i));
	}
	if (State.phase == Phase::Setup || State.phase == Phase::Breather)
		targets.push_back(ReadyButtonRect());
	if (currentTool.type == ToolType::None && !Selection().empty())
		targets.push_back(SellButtonRect());
	RaidRosterTargets(ui, targets);
	if (const std::optional<Rectangle> summary = RaidSummaryRect(ui, top); summary)
		targets.push_back(*summary);
}

bool PadHudNeedsFocus()
{
	return IsGameOver() || LobbyPanelOpen();
}

bool PadHudScroll(const Rectangle &focused, AxisDirection dir)
{
	if (dir.y != AxisDirectionY_NONE || focused.position.y != CardRowRect().position.y || focused.size.height != CardHeight)
		return false;
	int &scroll = ScrollOf(currentTab);
	const int before = scroll;
	if (dir.x == AxisDirectionX_RIGHT && focused.position.x == CardRect(VisibleCardCount() - 1).position.x)
		scroll = std::min(scroll + 1, MaxScroll(currentTab));
	else if (dir.x == AxisDirectionX_LEFT && focused.position.x == CardRect(0).position.x)
		scroll = std::max(scroll - 1, 0);
	return scroll != before;
}

Rectangle PadSwitchTab(int delta)
{
	currentTab = static_cast<BarTab>((static_cast<int>(currentTab) + delta + static_cast<int>(NumBarTabs)) % static_cast<int>(NumBarTabs));
	return CardRect(0);
}

void PadToggleZoom()
{
	SetZoom(ZoomFactor() == 1);
}

uint32_t PadToolStamp()
{
	return currentTool.type == ToolType::None ? 0 : (static_cast<uint32_t>(currentTool.type) << 16) + static_cast<uint32_t>(currentTool.index) + 1;
}

int PadViewBottom()
{
	return ScreenHudTop();
}

std::string PadHudPrompt(bool focused)
{
	const std::string confirm = InputName(PadAction::Confirm);
	const std::string cancel = InputName(PadAction::Cancel);
	if (IsControllerButtonPressed(ControllerButton_AXIS_TRIGGERRIGHT)) {
		std::string spells;
		for (size_t i = 0; i < NumSpellSlots; i++) {
			if (equippedSpells[i])
				spells += fmt::format("{}: {}   ", InputName(KeeperAction::Spell, i), SpellKinds[*equippedSpells[i]].name);
		}
		return spells.empty() ? std::string("No spells on the bar") : spells;
	}
	if (IsGameOver())
		return fmt::format("{}: move   {}: press   {}: back", InputName(PadAction::Navigate), confirm, cancel);
	if (focused)
		return fmt::format("{}: move   {}: press   {}: back to the map   {} {}: tabs", InputName(PadAction::Navigate), confirm, cancel, InputName(PadAction::PrevTab), InputName(PadAction::NextTab));
	if (MyPossessed() != nullptr)
		return fmt::format("{}: aim   {}: move or attack   {}: release", InputName(PadAction::MoveCursor), confirm, InputName(PadAction::Possess));
	switch (currentTool.type) {
	case ToolType::Build:
	case ToolType::Bait:
	case ToolType::LockDoor:
		return fmt::format("{}: aim   {}: place   {}: cancel", InputName(PadAction::MoveCursor), confirm, cancel);
	case ToolType::Spell:
		return fmt::format("{}: aim   {}: cast   {}: cancel", InputName(PadAction::MoveCursor), confirm, cancel);
	default:
		break;
	}
	if (!SelectedLairs().empty())
		return fmt::format("{}: rally   {}: waypoint   {}: sell   {}: deselect", InputName(PadAction::Order), InputName(PadAction::Waypoint), InputName(PadAction::Sell), cancel);
	return fmt::format("{}: select (hold and aim: box)   {}: possess   {}: cards   {}: spells   {}: open the gates   {}: menu",
	    confirm, InputName(PadAction::Possess), InputName(PadAction::Navigate), InputName(PadAction::SpellModifier), InputName(PadAction::StartRaid), InputName(PadAction::Menu));
}

void CenterCamera(Point tile)
{
	CenterCameraOn(tile);
}

void SetViewZoom(bool zoomed)
{
	SetZoom(zoomed);
}

bool IsViewZoomed()
{
	return ZoomFactor() > 1;
}

int UiPixelScale()
{
	return UiScale();
}

Point UiMousePosition()
{
	return UiMouse();
}

int UiBarHeight()
{
	return HudHeight();
}

} // namespace devilution::keeper
