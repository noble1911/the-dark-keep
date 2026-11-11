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
#include "keeper/keeper_alerts.hpp"
#include "keeper/keeper_barricade.hpp"
#ifdef KEEPER_DEVCTL
#include "keeper/keeper_devqa.hpp"
#endif
#include "keeper/keeper_bots.hpp"
#include "keeper/keeper_coop.hpp"
#include "keeper/keeper_editor.hpp"
#include "keeper/keeper_flow.hpp"
#include "keeper/keeper_game.hpp"
#include "keeper/keeper_heroes.hpp"
#include "keeper/keeper_input.hpp"
#include "keeper/keeper_lobby.hpp"
#include "keeper/keeper_map.hpp"
#include "keeper/keeper_menu.hpp"
#include "keeper/keeper_pad.hpp"
#include "keeper/keeper_profile.hpp"
#include "keeper/keeper_raid.hpp"
#include "keeper/keeper_scout.hpp"
#include "keeper/keeper_roster.hpp"
#include "keeper/keeper_ui.hpp"
#include "keeper/keeper_summary.hpp"
#include "keeper/keeper_tutorial.hpp"
#include "keeper/keeper_units.hpp"
#include "keeper/keeper_lighting.hpp"
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
int TestDifficulty = -1;
int TestRaidType = -1;
int TestCampaignLevel = 0;
std::string TestCustomMap;
int TestRank = 0;
int TestBuildStamp = 0;
AutoNetMode AutoNet = AutoNetMode::None;
char AutoJoinAddress[128] = "127.0.0.1";

namespace {

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
	return gbActive && !ChatFlag && !gmenu_is_active() && !EditorCapturesKeys() && !IsKeeperOverlayOpen() && !IsHelpOpen();
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
// One strip along the bottom of the screen, a tenth of its height at most, made of separate panels
// with the keep showing beside and between them:
//
//   [ status ] [ tabs ] scout line                   [        ]
//   [        ] [ < cards >         ]                 [ spells ]
//
// The strip is at most HudMaxWidth wide and centred. Details (a card's name and text, a selected
// lair and its sell button) come up in a panel above the strip only while they are asked for.

constexpr int HudMaxWidth = 1280;
/** A tenth of a 720 pixel high UI. A lower UI (a TV's, at twice the size) gets a lower strip. */
constexpr int StripMaxHeight = 72;
constexpr int StripMinHeight = 48;
constexpr int StatusWidth = 250;
constexpr int InfoWidth = 240;
constexpr int SectionGap = 6;
constexpr int TabHeight = 16;
constexpr int TabGap = 2;
constexpr int CardWidth = 46;
constexpr int CardGap = 2;
/** A card this high or higher has its price under the portrait; a lower one has it over the portrait's foot. */
constexpr int CardHeightWithPriceRow = 50;
constexpr int CardPriceRowHeight = 13;
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
	return KeeperUiScale();
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

/** Height of the strip in UI pixels. */
int StripHeight()
{
	return std::clamp(UiHeight() / 10, StripMinHeight, StripMaxHeight);
}

/** The strip's bounds: its panels are inside, and what is between them is the keep. */
Rectangle HudRect()
{
	const int width = std::min<int>(UiWidth(), HudMaxWidth);
	return { { (UiWidth() - width) / 2, UiHeight() - StripHeight() }, { width, StripHeight() } };
}

/** The off-screen UI surface (UI pixels) when the HUD is scaled, and the view it was drawn over. */
std::optional<OwnedSurface> uiSurface;
std::vector<uint8_t> uiBackground;
/** Solid panels drawn this frame, in UI pixels. See DrawScaledUi. */
std::vector<Rectangle> uiOpaque;

/** A solid panel covers the view even where it happens to be the view's colour. */
void NoteOpaqueUi(const Rectangle &rect)
{
	uiOpaque.push_back(rect);
}

/** A surface of UI-pixel size, for the raid panels' hit tests. */
Surface UiHitSurface()
{
	if (UiScale() > 1 && uiSurface)
		return *uiSurface;
	return GlobalBackBuffer();
}

/** Top of the strip in screen pixels: arrows at the edge of the view stay above it. The keep itself is drawn on to the foot of the screen. */
int ScreenHudTop()
{
	return HudRect().position.y * UiScale();
}

bool Contains(const Rectangle &rect, Point p)
{
	return p.x >= rect.position.x && p.y >= rect.position.y && p.x < rect.position.x + rect.size.width && p.y < rect.position.y + rect.size.height;
}

Rectangle StatusRect()
{
	const Rectangle hud = HudRect();
	return { hud.position, { std::min(StatusWidth, hud.size.width), hud.size.height } };
}

// Spell bar: the masters' spells are the right end of the strip. Their hotkeys cast instantly at the
// cursor; clicking a slot arms it for one click in the keep.
constexpr int SpellSlotSize = 44;
constexpr int SpellSlotGap = 2;
constexpr int SpellPanelPadding = 2;

Rectangle SpellSlotRect(size_t i)
{
	const Rectangle hud = HudRect();
	constexpr int Total = static_cast<int>(NumSpellSlots) * SpellSlotSize + (static_cast<int>(NumSpellSlots) - 1) * SpellSlotGap;
	const int left = hud.position.x + hud.size.width - SpellPanelPadding - Total;
	const int top = hud.position.y + hud.size.height - SpellPanelPadding - SpellSlotSize;
	return { { left + static_cast<int>(i) * (SpellSlotSize + SpellSlotGap), top }, { SpellSlotSize, SpellSlotSize } };
}

/** All the slots together. */
Rectangle SpellBarRect()
{
	const Rectangle first = SpellSlotRect(0);
	const Rectangle last = SpellSlotRect(NumSpellSlots - 1);
	return { first.position, { last.position.x + last.size.width - first.position.x, first.size.height } };
}

/** The slots and the panel behind them. */
Rectangle SpellPanelRect()
{
	const Rectangle bar = SpellBarRect();
	return { bar.position - Displacement { SpellPanelPadding, SpellPanelPadding }, { bar.size.width + 2 * SpellPanelPadding, bar.size.height + 2 * SpellPanelPadding } };
}

/**
 * Where a joiner's spell choice hangs: it is laid out a large icon to a column, which is wider than
 * the bar, so it gets a rectangle of its own width that ends where the bar does.
 */
Rectangle LobbyAnchorRect()
{
	constexpr int Width = static_cast<int>(NumSpellSlots) * (SPLICONLENGTH + 4) + (static_cast<int>(NumSpellSlots) - 1) * 4;
	const Rectangle panel = SpellPanelRect();
	const int right = panel.position.x + panel.size.width - 5;
	// The button goes over the slots if the strip has the room, otherwise over the strip.
	const int top = panel.position.y - HudRect().position.y >= 20 ? panel.position.y : HudRect().position.y;
	return { { std::max(right - Width, 5), top }, { Width, panel.size.height } };
}

// ---- card bar ------------------------------------------------------------------
//
// The middle of the strip: a row of tabs, and under it the current tab's cards. Everything about
// where a card is comes from CardRect(slot) and VisibleCardCount(); what the cards are comes from
// TabCards(tab).

/** The room between the status panel and the spells. */
Rectangle BarRect()
{
	const Rectangle status = StatusRect();
	const int left = status.position.x + status.size.width + SectionGap;
	const int right = SpellPanelRect().position.x - SectionGap;
	return { { left, status.position.y }, { std::max(right - left, CardWidth + 2 * (ArrowWidth + CardGap)), status.size.height } };
}

int CardHeight()
{
	return StripHeight() - TabHeight;
}

/** The portrait on a card: all of it but the frame and, where there is one, the price row. */
Size CardPortraitSize()
{
	return { CardWidth - 4, CardHeight() - 4 - (CardHeight() >= CardHeightWithPriceRow ? CardPriceRowHeight : 0) };
}

/** How many cards fit across `width`. */
int CardsAcross(int width)
{
	return std::max(1, (width + CardGap) / (CardWidth + CardGap));
}

/** More cards than the bar is wide: the tab scrolls, and has an arrow at each end. */
bool TabScrolls(BarTab tab)
{
	return static_cast<int>(TabCards(tab).size()) > CardsAcross(BarRect().size.width);
}

/** The room for the current tab's cards: between the arrows if there are any. */
Rectangle CardRowRect()
{
	const Rectangle bar = BarRect();
	const int arrows = TabScrolls(currentTab) ? ArrowWidth + CardGap : 0;
	return { { bar.position.x + arrows, bar.position.y + TabHeight }, { bar.size.width - 2 * arrows, CardHeight() } };
}

int VisibleCardCount()
{
	return CardsAcross(CardRowRect().size.width);
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
	return { { row.position.x + slot * (CardWidth + CardGap), row.position.y }, { CardWidth, row.size.height } };
}

/** How many cards are on show. */
int ShownCardCount()
{
	return std::min(VisibleCardCount(), static_cast<int>(TabCards(currentTab).size()) - ScrollOf(currentTab));
}

/** The cards on show. */
Rectangle ShownCardsRect()
{
	const Rectangle row = CardRowRect();
	const int count = std::max(ShownCardCount(), 1);
	return { row.position, { count * (CardWidth + CardGap) - CardGap, row.size.height } };
}

Rectangle LeftArrowRect()
{
	const Rectangle row = CardRowRect();
	return { { row.position.x - ArrowWidth - CardGap, row.position.y }, { ArrowWidth, row.size.height } };
}

Rectangle RightArrowRect()
{
	const Rectangle cards = ShownCardsRect();
	return { { cards.position.x + cards.size.width + CardGap, cards.position.y }, { ArrowWidth, cards.size.height } };
}

/** The cards and their arrows: the panel behind them is no wider. */
Rectangle CardPanelRect()
{
	const Rectangle cards = ShownCardsRect();
	if (!TabScrolls(currentTab))
		return cards;
	const Rectangle left = LeftArrowRect();
	const Rectangle right = RightArrowRect();
	return { left.position, { right.position.x + right.size.width - left.position.x, cards.size.height } };
}

/** A narrow bar (a TV's) has the first word only, which leaves the scout line some room. */
std::string_view TabName(size_t i)
{
	constexpr int NarrowBar = 600;
	const std::string_view name = BarTabNames[i];
	return BarRect().size.width < NarrowBar ? name.substr(0, name.find(' ')) : name;
}

/** As wide as its name, so the row has room left for the scout line. */
Rectangle TabRect(size_t i)
{
	const Rectangle bar = BarRect();
	int x = bar.position.x;
	for (size_t tab = 0; tab < i; tab++)
		x += GetLineWidth(TabName(tab)) + 12 + TabGap;
	return { { x, bar.position.y }, { GetLineWidth(TabName(i)) + 12, TabHeight } };
}

Rectangle TabsRect()
{
	const Rectangle first = TabRect(0);
	const Rectangle last = TabRect(NumBarTabs - 1);
	return { first.position, { last.position.x + last.size.width - first.position.x, first.size.height } };
}

// ---- status panel --------------------------------------------------------------
//
// Three rows: the phase, its clock, the speed toggle and the help button; then raid, pop and gold;
// then mana beside the gates button, or beside the count of raiders once they are in.

constexpr int StatusPadding = 4;
constexpr int StatusRows = 3;

/** The text box of a status row. */
Rectangle StatusRowRect(int row)
{
	const Rectangle status = StatusRect();
	const int pitch = (status.size.height - 4) / StatusRows;
	return { { status.position.x + StatusPadding, status.position.y + 2 + row * pitch + (pitch - 14) / 2 }, { status.size.width - 2 * StatusPadding, 14 } };
}

/** The "?" at the end of the phase row. */
Rectangle HelpButtonRect()
{
	const Rectangle row = StatusRowRect(0);
	return { { row.position.x + row.size.width - 18, row.position.y - 1 }, { 18, 16 } };
}

/** The speed toggle, just left of the help button. */
Rectangle SpeedButtonRect()
{
	const Rectangle help = HelpButtonRect();
	return { { help.position.x - 24, help.position.y }, { 22, 16 } };
}

/** The phase row up to the speed toggle: clicking the clock holds it. */
Rectangle PhaseLineRect()
{
	const Rectangle row = StatusRowRect(0);
	return { row.position + Displacement { 0, -1 }, { SpeedButtonRect().position.x - 2 - row.position.x, 16 } };
}

constexpr int ManaBarWidth = 62;

/** Mana: a bar with the number in it, at the start of the last row. */
Rectangle ManaRect()
{
	const Rectangle row = StatusRowRect(2);
	return { row.position + Displacement { 0, 1 }, { ManaBarWidth, 13 } };
}

/** The rest of the last row: the gates button while building, the raiders during a raid. */
Rectangle ReadyButtonRect()
{
	const Rectangle row = StatusRowRect(2);
	const int left = ManaRect().position.x + ManaBarWidth + 6;
	return { { left, row.position.y - 1 }, { row.position.x + row.size.width - left, 16 } };
}

bool IsBuildingPhase()
{
	return State.phase == Phase::Setup || State.phase == Phase::Breather;
}

/**
 * What the last frame drew that isn't at a fixed place: the hit tests ask this, so a click lands on
 * what the player saw.
 */
struct DrawnHud {
	/** Parts of the status panel and the scout line, for their hover text. */
	std::vector<std::pair<Rectangle, HudArea>> areas;
	/** The details panel above the strip, when it is up. */
	std::optional<Rectangle> info;
	/** Its sell button, when something that can be sold is selected. */
	std::optional<Rectangle> sell;
	/** The scout line beside the tabs. */
	std::optional<Rectangle> scout;
};
DrawnHud drawnHud;

/** True over one of the strip's panels or the details panel: not over the keep between them. */
bool IsMouseOverStrip(Point p)
{
	if (Contains(StatusRect(), p) || Contains(SpellPanelRect(), p) || Contains(CardPanelRect(), p) || Contains(TabsRect(), p))
		return true;
	return (drawnHud.scout && Contains(*drawnHud.scout, p)) || (drawnHud.info && Contains(*drawnHud.info, p));
}

/** What the status panel, tabs and scout line do, for the details panel. */
std::optional<HudArea> HudAreaAt(Point p)
{
	if (Contains(HelpButtonRect(), p))
		return HudArea::HelpButton;
	if (IsBuildingPhase() && Contains(ReadyButtonRect(), p))
		return HudArea::OpenGates;
	for (size_t i = 0; i < NumBarTabs; i++) {
		if (Contains(TabRect(i), p))
			return static_cast<HudArea>(static_cast<size_t>(HudArea::TabLairs) + i);
	}
	for (const auto &[rect, area] : drawnHud.areas) {
		if (Contains(rect, p))
			return area;
	}
	return std::nullopt;
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
	for (int slot = 0; slot < ShownCardCount(); slot++) {
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
		text.name = UsesBarricades() ? "Barricade" : "Lock";
		text.hotkey = CardHotkey(tool);
		text.cost = fmt::format("{}g", LockDoorCost);
		text.ready = State.vaultGold >= LockDoorCost;
		text.description = UsesBarricades() ? "Bars a neck, wall to wall. No doors here, so heroes bash it down or go round."
		                                    : "Bars a door. Heroes have to bash it down; Warriors do it fastest.";
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
Size cardImageSize;

void DrawKindPortrait(const Surface &area, size_t kind)
{
	if (kind >= NumLairKinds)
		return;
	// Made for the card's size, which changes with the screen's.
	if (!cardImages[kind].built || cardImageSize != Size { area.w(), area.h() }) {
		if (cardImageSize != Size { area.w(), area.h() })
			cardImages = {};
		cardImageSize = { area.w(), area.h() };
		cardImages[kind] = MakeKindImage(kind, area.w(), area.h());
	}
	DrawKindImage(area, cardImages[kind]);
}

/** The 56 pixel icon where it fits, otherwise the spell book's 37x38 one. */
void DrawSpellPortrait(const Surface &area, SpellID spell)
{
	SetSpellTrans(SpellType::Spell);
	if (area.w() >= SPLICONLENGTH && area.h() >= SPLICONLENGTH) {
		DrawLargeSpellIcon(area, { (area.w() - SPLICONLENGTH) / 2, (area.h() + SPLICONLENGTH) / 2 - 1 }, spell);
		return;
	}
	constexpr int Width = 37;
	constexpr int Height = 38;
	DrawSmallSpellIcon(area, { (area.w() - Width) / 2, (area.h() + Height) / 2 - 1 }, spell);
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

/** A solid panel of the strip. Black, so it is told from the keep by its frame. */
void DrawStripPanel(const Surface &out, const Rectangle &rect)
{
	FillRect(out, rect.position.x, rect.position.y, rect.size.width, rect.size.height, 0);
	DrawFrame(out, rect, PAL16_GRAY + 11);
	NoteOpaqueUi(rect);
}

/** A card is its portrait, the key that picks it and its price. Its name is in the details panel. */
void DrawCard(const Surface &out, const Rectangle &rect, const Tool &tool, bool hovered)
{
	const CardText text = DescribeCard(tool);
	const bool selected = currentTool.type == tool.type && currentTool.index == tool.index;

	FillRect(out, rect.position.x, rect.position.y, rect.size.width, rect.size.height, PAL16_GRAY + 14);
	const Rectangle portrait { rect.position + Displacement { 2, 2 }, CardPortraitSize() };
	FillRect(out, portrait.position.x, portrait.position.y, portrait.size.width, portrait.size.height, 0);
	DrawPortrait(out.subregion(portrait.position.x, portrait.position.y, portrait.size.width, portrait.size.height), tool);

	if (!text.ready) {
		// Greyed out like Dungeon Keeper's unaffordable rooms.
		DrawHalfTransparentRectTo(out, portrait.position.x, portrait.position.y, portrait.size.width, portrait.size.height);
		DrawHalfTransparentRectTo(out, portrait.position.x, portrait.position.y, portrait.size.width, portrait.size.height);
	}

	DrawString(out, FitText(text.hotkey, portrait.size.width - 2), { portrait.position + Displacement { 1, 0 }, { portrait.size.width - 2, 12 } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorWhite | UiFlags::Outlined });
	DrawString(out, FitText(text.cost, rect.size.width - 2), { { rect.position.x, rect.position.y + rect.size.height - CardPriceRowHeight - 1 }, { rect.size.width, 12 } },
	    { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | UiFlags::Outlined | (text.ready ? UiFlags::ColorGold : UiFlags::ColorRed) });

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

void DrawTabs(const Surface &out)
{
	for (size_t i = 0; i < NumBarTabs; i++) {
		const Rectangle tab = TabRect(i);
		const bool selected = static_cast<size_t>(currentTab) == i;
		FillRect(out, tab.position.x, tab.position.y, tab.size.width, tab.size.height, selected ? PAL16_RED + 12 : PAL16_GRAY + 14);
		DrawFrame(out, tab, selected ? PAL16_YELLOW + 4 : PAL16_GRAY + 10);
		NoteOpaqueUi(tab);
		DrawString(out, TabName(i), { tab.position + Displacement { 0, 1 }, { tab.size.width, 14 } },
		    { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | (selected ? UiFlags::ColorGold : UiFlags::ColorWhite) });
	}
}

/** The tabs, and the current tab's cards on a panel no wider than they are. */
void DrawCardBar(const Surface &out)
{
	DrawTabs(out);

	const Rectangle panel = CardPanelRect();
	FillRect(out, panel.position.x, panel.position.y, panel.size.width, panel.size.height, 0);
	NoteOpaqueUi(panel);

	const std::vector<Tool> &cards = TabCards(currentTab);
	const int first = ScrollOf(currentTab);
	const std::optional<Tool> hovered = CardAt(UiMouse());
	for (int slot = 0; slot < ShownCardCount(); slot++) {
		const Tool &card = cards[first + slot];
		DrawCard(out, CardRect(slot), card, hovered && hovered->type == card.type && hovered->index == card.index);
	}
	if (TabScrolls(currentTab)) {
		DrawArrow(out, LeftArrowRect(), "<", first > 0);
		DrawArrow(out, RightArrowRect(), ">", first < MaxScroll(currentTab));
	}
}

void DrawSpellBar(const Surface &out)
{
	DrawStripPanel(out, SpellPanelRect());
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
				// C&C-style recharge: a shade that drains away as the spell recovers.
				const int cooldownTicks = SpellKinds[*spell].cooldownTicks;
				const int shade = icon.size.height * std::min(State.spellCooldown[MyPlayerId], cooldownTicks) / std::max(cooldownTicks, 1);
				DrawHalfTransparentRectTo(out, icon.position.x, icon.position.y, icon.size.width, shade);
			}
			DrawString(out, std::to_string(SpellManaCost(*spell)), { icon.position + Displacement { 0, icon.size.height - 13 }, { icon.size.width - 1, 14 } },
			    { .flags = UiFlags::FontSize12 | UiFlags::AlignRight | UiFlags::Outlined | (text.ready ? UiFlags::ColorBlue : UiFlags::ColorRed) });
		}
		// A controller's "RT+X" is wider than a slot: RT is what brings the names up, so the button says enough.
		std::string key = InputName(KeeperAction::Spell, i);
		if (const size_t plus = key.rfind('+'); plus != std::string::npos && plus + 1 < key.size() && GetLineWidth(key) > icon.size.width - 2)
			key.erase(0, plus + 1);
		DrawString(out, key, { icon.position + Displacement { 1, 0 }, { icon.size.width - 2, 14 } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorWhite | UiFlags::Outlined });

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

/** "Del: " in front of a label, or nothing if selling has no key bound. */
std::string SellKeyPrefix()
{
	const std::string key = InputName(KeeperAction::Sell);
	return key.empty() ? std::string() : key + ": ";
}

// ---- details panel ---------------------------------------------------------------
//
// Up only while there is something to say: above the card, spell slot or part of the strip under the
// mouse, above the card of the tool in hand, or at the strip's right end for what is selected in the
// keep, which is the one that has a button.

struct InfoContent {
	std::string title;
	/** At the title's right: the key. */
	std::string corner;
	/** Under the title: a price or a state. */
	std::string second;
	UiFlags secondColor = UiFlags::ColorWhite;
	std::string body;
	/** The sell button's label, for a selection. */
	std::string sell;
	bool canSell = false;
	/** What the panel is above. */
	Rectangle anchor;
};

InfoContent CardInfo(const Tool &tool, const Rectangle &anchor)
{
	const CardText text = DescribeCard(tool);
	InfoContent info;
	info.title = text.name;
	if (!text.hotkey.empty())
		info.corner = fmt::format("[{}]", text.hotkey);
	info.second = text.cost;
	info.secondColor = text.ready ? UiFlags::ColorGold : UiFlags::ColorRed;
	info.body = text.description;
	info.anchor = anchor;
	return info;
}

/** Several selected structures: how many of what, and what selling them all pays. */
InfoContent GroupInfo(const Rectangle &anchor)
{
	const std::vector<StructureRef> &selected = Selection();
	InfoContent info;
	info.title = fmt::format("{} selected", selected.size());
	std::vector<int> kinds(BuildKinds.size());
	for (const StructureRef &ref : selected) {
		if (ref.type == StructureType::Lair)
			kinds[State.lairs[ref.slot].kind]++;
		info.canSell = info.canSell || CanSell(ref);
	}
	// The orders first: it is the list of what's selected that can run out of room.
	info.body = "Right-click: rally. Shift: waypoint.\n";
	bool firstKind = true;
	for (size_t i = 0; i < kinds.size(); i++) {
		if (kinds[i] > 0) {
			info.body += fmt::format("{}{} {}", firstKind ? "" : ", ", kinds[i], BuildKinds[i].name);
			firstKind = false;
		}
	}
	if (IsPadInUse())
		info.body += fmt::format("\n{}: rally around a point. {}: add a waypoint.", InputName(PadAction::Order), InputName(PadAction::Waypoint));
	info.sell = fmt::format("{}sell all for {}g{}", SellKeyPrefix(), SelectionRefund(), State.phase == Phase::Raid ? " (half)" : "");
	info.anchor = anchor;
	return info;
}

/** A selected structure: its state, its orders and the sell button. */
InfoContent StructureInfo(StructureRef ref, const Rectangle &anchor)
{
	InfoContent info;
	info.title = StructureName(ref);
	switch (ref.type) {
	case StructureType::Lair: {
		const Lair &lair = State.lairs[ref.slot];
		if (lair.summonTicks > 0) {
			info.second = "Summoning...";
		} else if (const Monster *monster = LairMonster(ref.slot); monster != nullptr) {
			const std::string_view duty = lair.routeCount > 0 ? "On patrol" : (lair.hasRally ? "At rally point" : "Guarding lair");
			info.second = fmt::format("{} ({}%)", duty, monster->maxHitPoints > 0 ? 100 * monster->hitPoints / monster->maxHitPoints : 0);
		} else {
			info.second = "Slain: returns after the raid";
		}
		info.body = fmt::format("Pop {}. {}: rally point. {}: waypoint.{}", BuildKinds[lair.kind].supply, InputName(PadAction::Order), IsPadInUse() ? InputName(PadAction::Waypoint) : InputName(PadAction::Modifier), lair.hasRally || lair.routeCount > 0 ? " On lair: clear." : "");
		break;
	}
	case StructureType::Trap:
		info.second = State.traps[ref.slot].cooldown > 0 ? "Rearming" : "Armed";
		break;
	case StructureType::Bait:
		info.second = fmt::format("Can fool {} more heroes", State.baits[ref.slot].uses);
		break;
	case StructureType::Lock:
		info.second = fmt::format("Holds for {} more blows", State.locks[ref.slot].strength);
		break;
	}
	info.sell = fmt::format("{}sell for {}g{}", SellKeyPrefix(), RefundFor(ref), State.phase == Phase::Raid ? " (half)" : "");
	info.canSell = CanSell(ref);
	info.anchor = anchor;
	return info;
}

/** Where the tool's card or spell slot is on the strip, if it is on show. */
std::optional<Rectangle> ToolAnchor(const Tool &tool)
{
	if (tool.type == ToolType::Spell) {
		for (size_t slot = 0; slot < NumSpellSlots; slot++) {
			if (equippedSpells[slot] == tool.index)
				return SpellSlotRect(slot);
		}
		return std::nullopt;
	}
	if (TabOf(tool) != currentTab)
		return std::nullopt;
	const std::vector<Tool> &cards = TabCards(currentTab);
	const int first = ScrollOf(currentTab);
	for (int slot = 0; slot < ShownCardCount(); slot++) {
		if (cards[first + slot].type == tool.type && cards[first + slot].index == tool.index)
			return CardRect(slot);
	}
	return std::nullopt;
}

/** What the details panel has to say now, if anything: the thing under the mouse, else the tool in hand, else the selection. */
std::optional<InfoContent> CurrentInfo()
{
	const Point mouse = UiMouse();
	if (const std::optional<Tool> card = CardAt(mouse); card)
		return CardInfo(*card, ToolAnchor(*card).value_or(CardPanelRect()));
	if (const std::optional<size_t> slot = SpellSlotAt(mouse); slot) {
		if (equippedSpells[*slot])
			return CardInfo({ ToolType::Spell, *equippedSpells[*slot] }, SpellSlotRect(*slot));
		return std::nullopt;
	}
	if (const std::optional<HudArea> area = HudAreaAt(mouse); area) {
		const HudHint hint = DescribeHudArea(*area);
		InfoContent info;
		info.title = hint.title;
		info.body = hint.text;
		// Over the mouse: the status panel is wider than the details are.
		info.anchor = { { mouse.x, HudRect().position.y }, { 1, 1 } };
		return info;
	}
	if (currentTool.type != ToolType::None)
		return CardInfo(currentTool, ToolAnchor(currentTool).value_or(CardPanelRect()));
	// A selection's panel has a button, so it stays in one place: over the spells.
	if (Selection().size() > 1)
		return GroupInfo(SpellPanelRect());
	if (const std::optional<StructureRef> selected = SelectedStructure(); selected)
		return StructureInfo(*selected, SpellPanelRect());
	return std::nullopt;
}

/** Draws the details panel above the strip, as high as what it says. */
void DrawInfoPanel(const Surface &out)
{
	const std::optional<InfoContent> info = CurrentInfo();
	if (!info)
		return;
	constexpr int Padding = 6;
	constexpr int LineHeight = 13;
	constexpr int SellHeight = 18;
	const int textWidth = InfoWidth - 2 * Padding;
	const int top = HudRect().position.y;
	const std::string body = info->body.empty() ? std::string() : FitLines(info->body, textWidth, std::max(top - 80, LineHeight), LineHeight);
	int height = 2 * Padding + 14;
	if (!info->second.empty())
		height += 14;
	if (!body.empty())
		height += 3 + CountLinesOfText(body) * LineHeight;
	if (!info->sell.empty())
		height += 4 + SellHeight;

	const int x = std::clamp(info->anchor.position.x + info->anchor.size.width / 2 - InfoWidth / 2, 2, std::max(out.w() - InfoWidth - 2, 2));
	const Rectangle panel { { x, std::max(top - 4 - height, 0) }, { InfoWidth, height } };
	FillRect(out, panel.position.x, panel.position.y, panel.size.width, panel.size.height, 0);
	DrawFrame(out, panel, PAL16_GRAY + 10);
	NoteOpaqueUi(panel);
	drawnHud.info = panel;

	Point at = panel.position + Displacement { Padding, Padding - 1 };
	const int cornerWidth = info->corner.empty() ? 0 : GetLineWidth(info->corner) + 6;
	DrawString(out, FitText(info->title, textWidth - cornerWidth), { at, { textWidth, 14 } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorGold });
	if (!info->corner.empty())
		DrawString(out, info->corner, { at, { textWidth, 14 } }, { .flags = UiFlags::FontSize12 | UiFlags::AlignRight | UiFlags::ColorWhite });
	at.y += 14;
	if (!info->second.empty()) {
		DrawString(out, FitText(info->second, textWidth), { at, { textWidth, 14 } }, { .flags = UiFlags::FontSize12 | info->secondColor });
		at.y += 14;
	}
	if (!body.empty()) {
		at.y += 3;
		const int bodyHeight = CountLinesOfText(body) * LineHeight;
		DrawString(out, body, { at, { textWidth, bodyHeight } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorWhite, .lineHeight = LineHeight });
		at.y += bodyHeight;
	}
	if (!info->sell.empty()) {
		const Rectangle button { { at.x, at.y + 4 }, { textWidth, SellHeight } };
		FillRect(out, button.position.x, button.position.y, button.size.width, button.size.height, PAL16_RED + 12);
		DrawFrame(out, button, Contains(button, UiMouse()) ? PAL16_YELLOW + 2 : PAL16_GRAY + 8);
		DrawString(out, FitText(info->sell, button.size.width - 4), { button.position + Displacement { 0, 2 }, { button.size.width, 14 } },
		    { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | (info->canSell ? UiFlags::ColorGold : UiFlags::ColorBlack) });
		drawnHud.sell = button;
	}
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

/** Wide enough for the longest refusal: narrower cut "Not enough gold" and "Vault: 615 gold" short. */
constexpr int WorldLabelWidth = 260;
/** Narrower than this and the top news doesn't go beside the scout card. */
constexpr int NewsMinWidthBesideCard = 280;

void DrawWorldLabel(const Surface &out, Point tile, std::string_view text, UiFlags color, int raise = 0)
{
	if (overlayPass != OverlayPass::Text)
		return;
	const int zoom = ZoomFactor();
	const Point base = GetScreenPosition(tile);
	const Point anchor = base + Displacement { TILE_WIDTH / 2, -TILE_HEIGHT };
	const Point at { anchor.x * zoom - WorldLabelWidth / 2, anchor.y * zoom - 4 - raise * zoom };
	if (at.y < 0 || at.y + 14 >= out.h())
		return;
	DrawString(out, text, { at, { WorldLabelWidth, 14 } }, { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | color | UiFlags::Outlined });
}

/** What the cursor's tile refuses: drawn after the scout card, which could otherwise hide it. */
std::optional<std::pair<Point, std::string>> refusalLabel;

void NoteRefusalLabel(Point tile, std::string_view reason)
{
	if (overlayPass == OverlayPass::Text)
		refusalLabel = { tile, std::string(reason) };
}

void DrawRefusalLabel(const Surface &out, int bottom)
{
	if (!refusalLabel)
		return;
	// This surface is the UI's, which is smaller than the world's when the UI is scaled up.
	const int scale = UiScale();
	const Point anchor = GetScreenPosition(refusalLabel->first) + Displacement { TILE_WIDTH / 2, -TILE_HEIGHT };
	const Point at { anchor.x * ZoomFactor() / scale - WorldLabelWidth / 2, anchor.y * ZoomFactor() / scale - 4 };
	if (at.y >= 0 && at.y + 14 < bottom)
		DrawString(out, refusalLabel->second, { at, { WorldLabelWidth, 14 } }, { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | UiFlags::ColorRed | UiFlags::Outlined });
	refusalLabel = std::nullopt;
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

/** The master's choice for wall spells (K): false runs them north-east / south-west, true the other way. */
bool wallsTurned = false;

/** Fire Wall runs north-east / south-west from the target (north-west / south-east turned) until something blocks it. */
void DrawFireWallPreview(const Surface &out, Point tile)
{
	DrawTileDiamond(out, tile, PAL16_ORANGE + 2);
	const std::array<Direction, 2> ends = wallsTurned ? std::array { Direction::NorthWest, Direction::SouthEast } : std::array { Direction::NorthEast, Direction::SouthWest };
	for (const Direction dir : ends) {
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
			NoteRefusalLabel(tile, reason);
		return;
	}
	case ToolType::Bait: {
		const bool ok = CanPlaceBaitAt(tile, &reason);
		if (ok)
			DrawTileDiamond(out, tile, PAL16_YELLOW + 8, 8); // heroes who can see it within 8 tiles detour
		DrawTileDiamond(out, tile, ok ? PAL16_YELLOW + 2 : PAL16_RED + 2);
		if (!ok)
			NoteRefusalLabel(tile, reason);
		return;
	}
	case ToolType::LockDoor: {
		const bool ok = CanLockDoorAt(tile, &reason);
		if (UsesBarricades()) {
			for (const Point at : BarricadeLine(tile))
				DrawTileDiamond(out, at, ok ? PAL16_YELLOW + 2 : PAL16_RED + 2);
			if (!ok)
				NoteRefusalLabel(tile, reason);
			return;
		}
		const Object *door = FindDoorAt(tile);
		DrawTileDiamond(out, door != nullptr ? door->position : tile, ok ? PAL16_YELLOW + 2 : PAL16_RED + 2);
		if (!ok)
			NoteRefusalLabel(tile, reason);
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
	const int maxY = out.h();
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

/** A ping in view: rings closing in on the tile. Out of view: an arrow at the edge of the screen pointing to it. */
void DrawPing(const Surface &out, const Ping &ping, uint32_t now)
{
	const int screenMargin = 28 * UiScale();
	const uint8_t color = PAL16_YELLOW + 2;
	const uint32_t left = ping.until - now;
	const int ring = static_cast<int>(left / 500 % 3);
	DrawTileDiamond(out, ping.tile, color, ring);
	if (ring != 0)
		DrawTileDiamond(out, ping.tile, color);
	if (overlayPass != OverlayPass::Text)
		return;
	const int maxY = ScreenHudTop();
	const Point at = TileCenter(ping.tile);
	const std::string_view name = Players[ping.player]._pName;
	if (at.x >= 0 && at.x < out.w() && at.y >= 0 && at.y < maxY) {
		DrawWorldLabel(out, ping.tile, name, UiFlags::ColorGold, 24);
		return;
	}
	const Point middle { out.w() / 2, maxY / 2 };
	const float dx = static_cast<float>(at.x - middle.x);
	const float dy = static_cast<float>(at.y - middle.y);
	const float length = std::max(std::sqrt(dx * dx + dy * dy), 1.F);
	const float scale = std::min(std::abs(dx) > 0.F ? (middle.x - screenMargin) / std::abs(dx) : 1e6F, std::abs(dy) > 0.F ? (middle.y - screenMargin) / std::abs(dy) : 1e6F);
	const Point tip { middle.x + static_cast<int>(dx * scale), middle.y + static_cast<int>(dy * scale) };
	const float ux = dx / length;
	const float uy = dy / length;
	for (int back = 12 * UiScale(); back <= 20 * UiScale(); back += 4) {
		const Point wingA { tip.x - static_cast<int>(ux * back - uy * back / 2), tip.y - static_cast<int>(uy * back + ux * back / 2) };
		const Point wingB { tip.x - static_cast<int>(ux * back + uy * back / 2), tip.y - static_cast<int>(uy * back - ux * back / 2) };
		DrawLine(out, tip, wingA, color, maxY);
		DrawLine(out, tip, wingB, color, maxY);
		DrawLine(out, wingA, wingB, color, maxY);
	}
	const Rectangle label { { std::clamp(tip.x - 60, 0, out.w() - 120), std::clamp(tip.y + (uy > 0 ? -38 : 22), 0, maxY - 14) }, { 120, 14 } };
	DrawString(out, name, label, { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | UiFlags::ColorGold | UiFlags::Outlined });
}

/** Raiders out of view: an arrow at the screen's edge, gold for one carrying your gold. */
void DrawHeroArrows(const Surface &out)
{
	if (overlayPass != OverlayPass::Text || State.phase != Phase::Raid)
		return;
	const int maxY = ScreenHudTop();
	const int size = 7 * UiScale();
	const int margin = 3 * size;
	const float centreX = static_cast<float>(out.w()) / 2.F;
	const float centreY = static_cast<float>(maxY) / 2.F;
	for (const HeroMark &hero : LivingHeroMarks()) {
		const Point at = TileCenter(hero.tile);
		if (at.x >= 0 && at.y >= 0 && at.x < out.w() && at.y < maxY)
			continue;
		const float dx = static_cast<float>(at.x) - centreX;
		const float dy = static_cast<float>(at.y) - centreY;
		const float length = std::max(std::sqrt(dx * dx + dy * dy), 1.F);
		const float reach = std::min(dx != 0.F ? (centreX - margin) / std::abs(dx) : 1e6F, dy != 0.F ? (centreY - margin) / std::abs(dy) : 1e6F);
		const float x = centreX + dx * reach;
		const float y = centreY + dy * reach;
		const float ux = dx / length;
		const float uy = dy / length;
		const Point tip { static_cast<int>(x + ux * size * 1.5F), static_cast<int>(y + uy * size * 1.5F) };
		const uint8_t color = hero.carried > 0 ? PAL16_YELLOW + 2 : PAL16_RED + 4;
		// Long and thin, or it wouldn't be clear which way it points. Filled by fanning lines from the tip across the base.
		for (float step = -size * 0.6F; step <= size * 0.6F; step += 0.5F) {
			const Point base { static_cast<int>(x - ux * size * 1.5F - uy * step), static_cast<int>(y - uy * size * 1.5F + ux * step) };
			DrawLine(out, tip, base, color, maxY);
		}
	}
}

/**
 * A route along the floor: black edged so it reads on any tileset, with dots marching towards the vault.
 * `dashed` routes are the what-ifs, so they don't get mistaken for the expected one.
 */
void DrawRouteLine(const Surface &out, const std::vector<Point> &route, uint8_t color, bool dashed)
{
	if (overlayPass != OverlayPass::Floor)
		return;
	const size_t march = SDL_GetTicks() / RouteMarchMilliseconds % RouteMarchSpacing;
	for (size_t i = 0; i + 1 < route.size(); i++) {
		if (dashed && i % 2 != 0)
			continue;
		const Point from = TileCenter(route[i]);
		const Point to = TileCenter(route[i + 1]);
		// Offsets on both axes: routes run in every direction on the screen.
		for (const Displacement edge : { Displacement { -2, 0 }, Displacement { 2, 0 }, Displacement { 0, -2 }, Displacement { 0, 2 } })
			DrawLine(out, from + edge, to + edge, 0, out.h());
		const bool marching = !dashed && (i + RouteMarchSpacing - march) % RouteMarchSpacing == 0;
		for (const Displacement body : { Displacement { -1, 0 }, Displacement { 1, 0 }, Displacement { 0, -1 }, Displacement { 0, 1 }, Displacement { 0, 0 } })
			DrawLine(out, from + body, to + body, marching ? PAL16_GRAY : color, out.h());
	}
}

/** The prep-time estimate of where the party will walk, and what locking the door under the lock tool would do. */
void DrawRoutePreview(const Surface &out)
{
	if (!IsRoutePreviewVisible())
		return;
	std::optional<Point> hoverDoor;
	if (currentTool.type == ToolType::LockDoor && !IsMouseOverUi() && CanLockDoorAt(cursPosition)) {
		if (const Object *door = FindDoorAt(cursPosition); door != nullptr)
			hoverDoor = door->position;
	}
	const RoutePreview &preview = GetRoutePreview(hoverDoor);
	if (preview.route.empty())
		return;
	DrawRouteLine(out, preview.withoutLocks, PAL16_BLUE + 4, true);
	DrawRouteLine(out, preview.withHoverLock, PAL16_RED + 2, true);
	DrawRouteLine(out, preview.route, PAL16_YELLOW + 2, false);
	DrawWorldLabel(out, preview.route[preview.route.size() / 2], fmt::format("Route estimate: {} steps", preview.route.size() - 1), UiFlags::ColorGold);
	if (!preview.withoutLocks.empty())
		DrawWorldLabel(out, preview.withoutLocks[preview.withoutLocks.size() / 2], "Their way with no locks", UiFlags::ColorBlue);
	for (const Point door : preview.bashedLocks)
		DrawWorldLabel(out, door, "They'd bash this", UiFlags::ColorRed, 16);
	if (hoverDoor)
		DrawWorldLabel(out, *hoverDoor, preview.hoverVerdict, preview.hoverTurnsThem ? UiFlags::ColorGold : UiFlags::ColorWhite, 16);
}

void DrawOverlays(const Surface &out)
{
	const uint32_t pingNow = SDL_GetTicks();
	for (const Ping &ping : Pings()) {
		if (ping.until > pingNow)
			DrawPing(out, ping, pingNow);
	}

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
		DrawWorldLabel(out, lock.position, fmt::format("{} ({})", UsesBarricades() ? "Barred" : "Locked", lock.strength), UiFlags::ColorRed);
	}

	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		const Monster &monster = Monsters[ActiveMonsters[i]];
		if (!IsHero(monster) || monster.hitPoints <= 0)
			continue;
		DrawWorldLabel(out, monster.position.tile, HeroTitle(monster), UiFlags::ColorWhite, 64);
	}

	DrawSelectionOverlay(out);
	DrawRoutePreview(out);
	DrawHeroArrows(out);

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
	// With a raid to retry that one has the main button, and this is the long way round.
	if (State.phase != Phase::Victory)
		return CanRetryRaid() ? std::string(IsPadInUse() ? "Restart level" : "Restart level (L)") : fmt::format("Retry ({})", key);
	if (CurrentSetup().mode != GameMode::Campaign)
		return fmt::format("Play again ({})", key);
	return fmt::format("Next: {} ({})", CampaignLevelRules(next.campaignLevel).name, key);
}

/** The banner's buttons, in order. */
enum class GameOverChoice : uint8_t {
	RetryRaid,
	Continue,
	Keeper,
	Leave,
};

std::vector<GameOverChoice> GameOverChoices()
{
	std::vector<GameOverChoice> choices;
	if (CanRetryRaid())
		choices.push_back(GameOverChoice::RetryRaid);
	if (GameOverContinue())
		choices.push_back(GameOverChoice::Continue);
	if (gbIsMultiplayer)
		choices.push_back(GameOverChoice::Keeper);
	choices.push_back(GameOverChoice::Leave);
	return choices;
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
	RequestCastSpell(index, tile, wallsTurned);
	AlertsNoteSpellCast();
	return true;
}

/** True if the mouse is over one of the strip's panels or a panel floating above it: false over the keep between them. */
bool IsMouseOverUi()
{
	if (IsMouseOverStrip(UiMouse()) || LobbyIsMouseOver(UiMouse(), LobbyAnchorRect()))
		return true;
	const std::optional<Rectangle> inspect = InspectPanelRect(HudRect().position.x, HudRect().position.y);
	if (inspect && Contains(*inspect, UiMouse()))
		return true;
	const Surface ui = UiHitSurface();
	if (IsHelpOpen() || (IsGameOver() && Contains(MapSummaryRect(ui), UiMouse())) || TutorialIsMouseOver(UiMouse()) || AlertsIsMouseOver(UiMouse()))
		return true;
	const std::optional<Rectangle> roster = RaidRosterRect(ui);
	const std::optional<Rectangle> summary = RaidSummaryRect(ui, HudRect().position.y);
	const std::optional<Rectangle> scoutCard = ScoutCardRect(ui, HudRect().position.y);
	return (roster && Contains(*roster, UiMouse())) || (summary && Contains(*summary, UiMouse())) || (scoutCard && Contains(*scoutCard, UiMouse()));
}

/** Single player opens the gates. In co-op this says where the vote stands and what a press would do. */
std::string ReadyButtonLabel()
{
	if (!gbIsMultiplayer)
		return IsPadInUse() ? "Open gates" : "Open the gates";
	const std::string count = fmt::format("{}/{}", ReadyMasterCount(), MastersInKeepCount());
	if (!IsMasterReady(MyPlayerId))
		return fmt::format("Ready {}", count);
	const int seconds = (State.readyTicks + 19) / 20;
	return IsHost() ? fmt::format("Start now {} {}s", count, seconds) : fmt::format("Not ready {} {}s", count, seconds);
}

void ToggleHeldTimer()
{
	if (!CanHoldPrepTimer()) {
		SetFeedback("Masters decide together in co-op: the timer keeps running");
		return;
	}
	TogglePrepTimerHold();
	SetFeedback(IsPrepTimerHeld() ? "Timer held: the raid waits until you open the gates" : "Timer running");
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
	// Leaving an editor test run comes through here without ending the game: the help mustn't stay open, or paused.
	CloseHelp();
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
	ResetGame(entrance, FindVault(entrance));
	LightingOnLevelLoaded(State.entrance, State.vault);
	ResetUnits();
	LoadSpellBar();
	LobbyOnLevelLoaded(equippedSpells);
	CoopOnLevelLoaded();
	AlertsOnLevelLoaded();
	TutorialOnLevelLoaded();
	ForgetMapSummary();
	CenterCameraOn(entrance);
	currentTool = {};
	ClearSelection();
	CancelBoxDrag();
	tickCounter = 0;
	lastCameraTicks = SDL_GetTicks();
	NoteMapLevelLoaded();
	ClearTabCards();
	// A new game starts on the monsters, wherever the last one left the bar.
	currentTab = BarTab::Lairs;
	tabScroll.fill(0);
	// Built during the load rather than as cards scroll into view, which would stutter.
	if (!IsEditorActive()) {
		for (const size_t kind : MusterKinds()) {
			const Size portrait = CardPortraitSize();
			if (cardImageSize != portrait) {
				cardImages = {};
				cardImageSize = portrait;
			}
			if (!cardImages[kind].built)
				cardImages[kind] = MakeKindImage(kind, portrait.width, portrait.height);
		}
	}
	EditorOnLevelLoaded();
	PadOnLevelLoaded();
}

void OnGameEnded()
{
	CloseHelp();
	PadOnGameEnded();
	EditorOnGameEnded();
	ForgetMapCache();
	if (active)
		OnLeavingKeep();
	SaveProfile();
	LobbyOnGameEnded();
	CoopOnGameEnded();
	OnGameEndedSpeed();
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
		CloseKeeperOverlay();
		gamemenu_exit_game(false);
		ReturnToMainMenu = false;
		return;
	}
	if (CoopHasHostLeft())
		return;
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
	LightingTick();
	if (IsEditorActive())
		return; // a map being edited has no raid to simulate

	GameTick();
	LobbyTick();
	CoopTick();
	UpdateUnitMarkers();
	AlertsTick();
	TutorialTick();

	tickCounter++;
#ifdef KEEPER_DEVCTL
	DevQaTick();
#endif
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

Point DevTileOnScreen(Point tile)
{
	return TileCenter(tile);
}

void UpdateCamera()
{
	if (!IsActive())
		return;
	EditorUpdateFrame();
	if (CoopHostLeftNoticeOver()) {
		gamemenu_exit_game(false);
		return;
	}

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
	case KeeperAction::CycleSpeed:
		CycleGameSpeed();
		return;
	case KeeperAction::TurnWalls:
		wallsTurned = !wallsTurned;
		SetFeedback(wallsTurned ? "Walls now run north-west to south-east" : "Walls now run north-east to south-west");
		return;
	case KeeperAction::Ping:
		if (IsMouseOverUi())
			SetFeedback("Point at the keep to mark a spot");
		else if (!RequestPing(cursPosition))
			SetFeedback("Wait a moment before marking again");
		return;
	case KeeperAction::HoldTimer:
		ToggleHeldTimer();
		return;
	case KeeperAction::Help:
		ToggleHelp();
		return;
	case KeeperAction::RoutePreview:
		ToggleRoutePreview();
		return;
	case KeeperAction::ScoutCard:
		ToggleScoutCard();
		if (IsScoutCardOn())
			LobbyOnEscape(); // a joiner's spell panel would cover the card's lower half
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

	// F1 always opens the help: the engine's own is for a hero's game.
	if (key == SDLK_F1) {
		ToggleHelp();
		return true;
	}
	if (IsHelpOpen()) {
		if (key == SDLK_ESCAPE || IsKeyBoundTo(KeeperAction::Help, static_cast<uint32_t>(key)))
			CloseHelp();
		return true; // nothing else happens behind the help
	}

	if (IsKeeperOverlayOpen()) {
		if (key == SDLK_ESCAPE)
			CloseKeeperOverlay();
		return true;
	}
	if (CoopHasHostLeft()) {
		if (key == SDLK_ESCAPE)
			gamemenu_exit_game(false);
		return true;
	}
	if (IsGameOver() && (key == SDLK_RETURN || key == SDLK_KP_ENTER)) {
		if (CanRetryRaid()) {
			RequestRetryRaid();
			return true;
		}
		if (GameOverContinue()) {
			ChooseGameOverContinue();
			return true;
		}
	}
	if (IsGameOver() && key == SDLK_L && CanRetryRaid() && GameOverContinue()) {
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
		if (TutorialOnEscape())
			return true;
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

	if (CoopHasHostLeft())
		return true; // nothing left to order about
	if (IsKeeperOverlayOpen()) {
		KeeperOverlayClick();
		return true;
	}
	if (IsHelpOpen()) {
		HelpOnClick(UiMouse());
		return true;
	}
	if (Contains(HelpButtonRect(), UiMouse())) {
		ToggleHelp();
		return true;
	}
	if (TutorialOnClick(UiMouse()))
		return true;
	if (const std::optional<Point> tile = AlertsOnClick(UiMouse()); tile) {
		CenterCameraOn(*tile);
		return true;
	}
	if (LobbyOnLeftClick(UiMouse(), LobbyAnchorRect(), equippedSpells)) {
		if (currentTool.type == ToolType::Spell && std::find(equippedSpells.begin(), equippedSpells.end(), currentTool.index) == equippedSpells.end())
			currentTool = {}; // it just came off the bar
		return true;
	}
	if (IsGameOver()) {
		const Surface ui = UiHitSurface();
		const std::vector<GameOverChoice> choices = GameOverChoices();
		for (size_t i = 0; i < choices.size(); i++) {
			if (!Contains(MapSummaryButton(ui, static_cast<int>(i), static_cast<int>(choices.size())), UiMouse()))
				continue;
			switch (choices[i]) {
			case GameOverChoice::RetryRaid:
				RequestRetryRaid();
				break;
			case GameOverChoice::Continue:
				ChooseGameOverContinue();
				break;
			case GameOverChoice::Keeper:
				CoopOpenKeeper();
				break;
			case GameOverChoice::Leave:
				gamemenu_exit_game(false);
				break;
			}
			return true;
		}
		if (Contains(MapSummaryRect(ui), UiMouse()))
			return true;
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
		if (OnScoutCardClick(ui, HudRect().position.y, UiMouse()))
			return true;
		if (const std::optional<Point> heroTile = OnRaidRosterClick(ui, UiMouse()); heroTile) {
			CenterCameraOn(*heroTile);
			return true;
		}
	}
	if (IsMouseOverStrip(UiMouse())) {
		for (size_t i = 0; i < NumBarTabs; i++) {
			if (Contains(TabRect(i), UiMouse()))
				currentTab = static_cast<BarTab>(i);
		}
		// Long tabs (the Bestiary, a late-game Lairs tab): the arrows page through them.
		const int scrollStep = static_cast<int>(TabCards(currentTab).size()) > 2 * VisibleCardCount() ? VisibleCardCount() : 1;
		if (TabScrolls(currentTab) && Contains(LeftArrowRect(), UiMouse()))
			ScrollOf(currentTab) -= scrollStep;
		else if (TabScrolls(currentTab) && Contains(RightArrowRect(), UiMouse()))
			ScrollOf(currentTab) += scrollStep;
		if (const std::optional<Tool> card = CardAt(UiMouse()); card)
			ActivateCard(*card);
		if (Contains(SpeedButtonRect(), UiMouse()))
			CycleGameSpeed();
		if (Contains(PhaseLineRect(), UiMouse()) && CanHoldPrepTimer() && (State.phase == Phase::Setup || State.phase == Phase::Breather))
			ToggleHeldTimer();
		if (Contains(ReadyButtonRect(), UiMouse()) && (State.phase == Phase::Setup || State.phase == Phase::Breather))
			RequestStartRaid();
		if (drawnHud.sell && Contains(*drawnHud.sell, UiMouse()) && currentTool.type == ToolType::None && !Selection().empty())
			SellSelected();
		return true;
	}
	if (IsMouseOverUi())
		return true; // the inspect panel

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
	if (CoopHasHostLeft() || IsKeeperOverlayOpen())
		return true;
	if (IsHelpOpen())
		return true; // nothing happens behind the help
	if (LobbyIsMouseOver(UiMouse(), LobbyAnchorRect()))
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
	if (IsKeeperOverlayOpen()) {
		KeeperOverlayWheel(deltaY);
		return true;
	}
	if (IsHelpOpen()) {
		HelpOnWheel(deltaY);
		return true;
	}
	if (DispatchBoundKey(deltaY > 0 ? MouseScrollUpButton : MouseScrollDownButton))
		return true;
	if (Contains(CardPanelRect(), UiMouse()) || Contains(TabsRect(), UiMouse())) {
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

	// The Keeper screen is opaque: a pixel it draws the same as the view's sample still has to cover the view.
	const bool opaque = IsKeeperOverlayOpen();
	uiOpaque.clear();
	drawUi(ui);

	// A solid panel that is the colour of the view's sample has to cover the rest of the view's pixels there too.
	for (const Rectangle &rect : uiOpaque) {
		for (int y = std::max(rect.position.y, 0); y < std::min(rect.position.y + rect.size.height, height); y++) {
			for (int x = std::max(rect.position.x, 0); x < std::min(rect.position.x + rect.size.width, width); x++)
				uiBackground[static_cast<size_t>(y) * width + x] = static_cast<uint8_t>(~*ui.at(x, y));
		}
	}
	uiOpaque.clear();

	for (int y = 0; y < height; y++) {
		const uint8_t *row = ui.at(0, y);
		const uint8_t *before = &uiBackground[static_cast<size_t>(y) * width];
		if (!opaque && std::memcmp(row, before, width) == 0)
			continue;
		for (int x = 0; x < width; x++) {
			if (!opaque && row[x] == before[x])
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

/** The status panel: phase and clock, raid, pop and gold, mana, and the gates button or the raiders. */
void DrawStatusPanel(const Surface &out)
{
	const size_t me = MyPlayerId;
	DrawStripPanel(out, StatusRect());
	const auto note = [](const Rectangle &rect, HudArea area) {
		drawnHud.areas.emplace_back(Rectangle { rect.position + Displacement { -2, -2 }, { rect.size.width + 4, rect.size.height + 4 } }, area);
	};

	// Row 1: the phase and its clock, the speed toggle, the help button.
	const Rectangle phaseRect = PhaseLineRect();
	std::string phaseLine = std::string(PhaseName(State.phase));
	if (IsBuildingPhase())
		phaseLine += IsPrepTimerHeld() ? std::string("  held") : fmt::format("  {}", FormatTicks(State.phaseTicks));
	DrawString(out, FitText(phaseLine, phaseRect.size.width), { phaseRect.position + Displacement { 0, 1 }, { phaseRect.size.width, 14 } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorRed });
	note(phaseRect, HudArea::Phase);
	const Rectangle speedButton = SpeedButtonRect();
	DrawFrame(out, speedButton, Contains(speedButton, UiMouse()) ? PAL16_YELLOW + 4 : PAL16_GRAY + 8);
	DrawString(out, fmt::format("{}x", GameSpeedMultiplier()), { speedButton.position + Displacement { 0, 1 }, { speedButton.size.width, 14 } },
	    { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | (GameSpeedLevel() > 0 ? UiFlags::ColorGold : UiFlags::ColorWhite) });
	note(speedButton, HudArea::Phase);
	const Rectangle help = HelpButtonRect();
	FillRect(out, help.position.x, help.position.y, help.size.width, help.size.height, PAL16_RED + 12);
	DrawFrame(out, help, Contains(help, UiMouse()) ? PAL16_YELLOW + 2 : PAL16_YELLOW + 4);
	DrawString(out, "?", { help.position + Displacement { 0, 1 }, { help.size.width, 14 } }, { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | UiFlags::ColorGold });

	// Row 2: raid and pop from the left, the vault's gold at the right.
	const Rectangle row2 = StatusRowRect(1);
	const int supply = SupplyUsed();
	const std::string goldLine = fmt::format("{}g", State.vaultGold);
	const std::string raidLine = fmt::format("Raid {}/{}", std::min<int>(State.wave, NumWaves()), NumWaves());
	// Late levels reach Raid 10/10 and Pop 100/100, which don't fit beside a full vault with their words.
	std::string popLine = fmt::format("Pop {}/{}", supply, SupplyCap());
	if (GetLineWidth(raidLine) + GetLineWidth(popLine) + GetLineWidth(goldLine) + 20 > row2.size.width)
		popLine = fmt::format("{}/{}", supply, SupplyCap());
	const int raidWidth = GetLineWidth(raidLine);
	const int popWidth = GetLineWidth(popLine);
	const int goldWidth = GetLineWidth(goldLine);
	const Rectangle popRect { row2.position + Displacement { raidWidth + 10, 0 }, { popWidth, 14 } };
	const Rectangle goldRect { row2.position + Displacement { row2.size.width - goldWidth, 0 }, { goldWidth, 14 } };
	DrawString(out, raidLine, row2, { .flags = UiFlags::FontSize12 | UiFlags::ColorWhite });
	DrawString(out, popLine, popRect, { .flags = UiFlags::FontSize12 | (supply >= SupplyCap() ? UiFlags::ColorRed : UiFlags::ColorWhite) });
	DrawString(out, goldLine, goldRect, { .flags = UiFlags::FontSize12 | UiFlags::ColorGold });
	note({ row2.position, { raidWidth + 5, 14 } }, HudArea::Raids);
	note({ popRect.position, { popWidth + 5, 14 } }, HudArea::Pop);
	note(goldRect, HudArea::Vault);

	// Row 3: mana, then the gates button while building, or the raiders and what they carry.
	const int mana = State.mana[me] / ManaScale;
	const Rectangle manaRect = ManaRect();
	FillRect(out, manaRect.position.x, manaRect.position.y, manaRect.size.width, manaRect.size.height, PAL16_GRAY + 14);
	FillRect(out, manaRect.position.x, manaRect.position.y, manaRect.size.width * mana / MaxMana, manaRect.size.height, PAL16_BLUE + 8);
	DrawFrame(out, manaRect, PAL16_GRAY + 8);
	DrawString(out, std::to_string(mana), { manaRect.position + Displacement { 0, 0 }, { manaRect.size.width, 14 } }, { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | UiFlags::ColorWhite | UiFlags::Outlined });
	note(manaRect, HudArea::Mana);

	const Rectangle ready = ReadyButtonRect();
	if (IsBuildingPhase()) {
		FillRect(out, ready.position.x, ready.position.y, ready.size.width, ready.size.height, PAL16_RED + 12);
		DrawFrame(out, ready, Contains(ready, UiMouse()) ? PAL16_YELLOW + 2 : PAL16_YELLOW + 4);
		const std::string raidKey = InputName(KeeperAction::StartRaid);
		// The key only if it fits: what the button does matters more.
		const std::string withKey = fmt::format("{}: {}", raidKey, ReadyButtonLabel());
		DrawString(out, raidKey.empty() || GetLineWidth(withKey) > ready.size.width - 4 ? FitText(ReadyButtonLabel(), ready.size.width - 4) : withKey, { ready.position + Displacement { 0, 1 }, { ready.size.width, 14 } }, { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | UiFlags::ColorGold });
	} else {
		const int inFlight = GoldInFlight();
		DrawString(out, FitText(fmt::format("Heroes {}  Loot {}", LivingHeroCount(), inFlight), ready.size.width), { ready.position + Displacement { 0, 1 }, { ready.size.width, 14 } }, { .flags = UiFlags::FontSize12 | (inFlight > 0 ? UiFlags::ColorRed : UiFlags::ColorWhite) });
		note(ready, HudArea::Heroes);
	}
}

/** The scout line beside the tabs: the coming raid, the Lord's health, the portal. As wide as what it says. */
void DrawScoutLine(const Surface &out)
{
	std::string scout = State.phase == Phase::Raid ? fmt::format("Raid {}", State.wave) : (IsGameOver() ? std::string() : DescribeWave(State.wave));
	// How to read more about the raid: the first thing to go if the line is too long.
	std::string hint;
	std::string lord;
	if (State.phase != Phase::Raid && !IsGameOver() && !IsScoutCardOn())
		hint = fmt::format("  -  {}: scout card", InputName(KeeperAction::ScoutCard));
	UiFlags scoutColor = UiFlags::ColorGold;
	if (const Monster *boss = FindBoss(); boss != nullptr && !IsGameOver()) {
		const int hp = boss->maxHitPoints > 0 ? 100 * boss->hitPoints / boss->maxHitPoints : 0;
		lord = fmt::format("  -  Keep Lord {}%", hp);
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
		hint.clear();
		lord.clear();
	}
	if (scout.empty() && lord.empty())
		return;

	// From the tabs to the end of the strip if nothing is in the way, otherwise to the spells.
	const Rectangle tabs = TabsRect();
	const Rectangle spells = SpellPanelRect();
	const int left = tabs.position.x + tabs.size.width + 8;
	const bool overSpells = spells.position.y >= tabs.position.y + TabHeight;
	int right = overSpells ? spells.position.x + spells.size.width : spells.position.x - SectionGap;
	// A joiner's "Choose spells" button is over the slots while the keep is being built.
	if (overSpells && gbIsMultiplayer && IsJoinedGame() && IsBuildingPhase())
		right -= 150;
	const int room = right - left - 8;
	if (room < 40)
		return;
	const std::string line = GetLineWidth(scout + hint + lord) <= room ? scout + hint + lord : FitText(scout + lord, room);
	const Rectangle rect { { left, tabs.position.y }, { GetLineWidth(line) + 8, TabHeight } };
	DrawHalfTransparentRectTo(out, rect.position.x, rect.position.y, rect.size.width, rect.size.height);
	DrawHalfTransparentRectTo(out, rect.position.x, rect.position.y, rect.size.width, rect.size.height);
	DrawString(out, line, { rect.position + Displacement { 4, 1 }, { rect.size.width - 4, 14 } }, { .flags = UiFlags::FontSize12 | scoutColor });
	drawnHud.scout = rect;
	drawnHud.areas.emplace_back(rect, HudArea::Scout);
}

void DrawUi(const Surface &out)
{
	if (IsKeeperOverlayOpen()) {
		DrawKeeperOverlay(out, UiMouse());
		DrawCoopKeeperBanner(out);
		return;
	}
	const Rectangle hud = HudRect();
	const int top = hud.position.y;
	drawnHud = {};

	DrawStatusPanel(out);

	DrawScoutCard(out, top, UiMouse()); // under the spell bar, which has to stay usable on a small screen
	DrawRefusalLabel(out, out.h());
	DrawCardBar(out);
	DrawSpellBar(out);
	DrawScoutLine(out);
	DrawInspectPanel(out, hud.position.x, top);
	DrawRaidRoster(out, UiMouse());
	DrawRaidSummary(out, top);
	DrawMasters(out);
	DrawLobby(out, UiMouse(), LobbyAnchorRect(), equippedSpells);
	DrawInfoPanel(out); // over the rest: it is what the player just asked about

	// News along the top, between the masters list and the raid roster.
	int pauseY = 2;
	int pauseCentre = out.w() / 2;
	if (!IsGameOver()) {
		const std::optional<Rectangle> roster = RaidRosterRect(out);
		int newsLeft = gbIsMultiplayer ? 198 : 6;
		int newsRight = roster ? roster->position.x - 6 : out.w() - 6;
		// Under the strips along the very top: the test-run banner, then the controller's prompts.
		int newsTop = 6 + (IsEditorTestRun() ? 20 : 0) + (IsPadInUse() ? 20 : 0);
		bool tipFits = true;
		SetScoutCardTop(newsTop);
		// The card stays put. The news goes beside it if that leaves it room, otherwise under it.
		if (const std::optional<Rectangle> scoutCard = ScoutCardRect(out, top); scoutCard) {
			if (scoutCard->position.x - 6 - newsLeft >= NewsMinWidthBesideCard) {
				newsRight = std::min(newsRight, scoutCard->position.x - 6);
			} else {
				newsLeft = 6;
				newsTop = scoutCard->position.y + scoutCard->size.height + 2;
				tipFits = false; // below the card a tip would run over the bar: it waits until the card is closed
			}
		}
		const int newsWidth = newsRight - newsLeft;
		pauseCentre = newsLeft + newsWidth / 2;
		int newsY = DrawBanner(out, newsLeft, newsWidth, newsTop);
		newsY = DrawAlerts(out, newsLeft, newsWidth, newsY, UiMouse());
		// The tip's frame goes round its target, a few pixels out: the strip is at the screen's edge, so it is drawn a little inside.
		const auto inside = [&out](Rectangle rect) {
			constexpr int Room = 4;
			rect.size.height = std::min(rect.size.height, out.h() - Room - rect.position.y);
			return rect;
		};
		const TipTargets targets {
			inside(ShownCardsRect()),
			TabsRect(),
			inside(ReadyButtonRect()),
			inside(SpellBarRect()),
			roster,
		};
		const int tipWidth = std::min(newsWidth, 380);
		if (tipFits)
			DrawTutorialTip(out, newsLeft + (newsWidth - tipWidth) / 2, tipWidth, newsY, UiMouse(), targets);
		else
			TutorialTipHidden();
		if (const std::optional<Rectangle> tip = TutorialTipRect(); tip)
			newsY = tip->position.y + tip->size.height + 2;
		pauseY = newsY > newsTop ? newsY : newsTop - 4;
	}

	// Feedback floats just above the bar, like an RTS's "insufficient funds".
	if (!feedback.empty() && SDL_GetTicks() < feedbackUntil) {
		const int width = std::min<int>(GetLineWidth(feedback) + 16, out.w());
		Rectangle line { { (out.w() - width) / 2, top - 22 }, { width, 18 } };
		// Clear of the details panel, which is up while a tool is in hand.
		if (drawnHud.info && line.position.x < drawnHud.info->position.x + drawnHud.info->size.width && drawnHud.info->position.x < line.position.x + line.size.width)
			line.position.y = std::max(drawnHud.info->position.y - 22, 0);
		DrawHalfTransparentRectTo(out, line.position.x, line.position.y, line.size.width, line.size.height);
		DrawString(out, feedback, { line.position + Displacement { 0, 2 }, { line.size.width, 14 } }, { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | UiFlags::ColorRed });
	}

	if (PauseMode != 0 && !gmenu_is_active()) {
		const std::string line = "Paused: orders still work";
		const int width = GetLineWidth(line) + 16;
		DrawHalfTransparentRectTo(out, pauseCentre - width / 2, pauseY, width, 18);
		DrawString(out, line, { { pauseCentre - width / 2, pauseY + 2 }, { width, 14 } }, { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | UiFlags::ColorGold });
	}

	if (IsGameOver() && !CoopHasHostLeft()) {
		std::vector<std::string> labels;
		for (const GameOverChoice choice : GameOverChoices()) {
			switch (choice) {
			case GameOverChoice::RetryRaid:
				labels.push_back(fmt::format("Retry ({})", IsPadInUse() ? InputName(PadAction::Confirm) : std::string("Enter")));
				break;
			case GameOverChoice::Continue:
				labels.push_back(CoopStartingTicks() > 0 ? fmt::format("Start now ({})", IsPadInUse() ? InputName(PadAction::Confirm) : std::string("Enter")) : GameOverContinueLabel(*GameOverContinue()));
				break;
			case GameOverChoice::Keeper:
				labels.push_back(fmt::format("Keeper: {} Renown", RenownAvailable()));
				break;
			case GameOverChoice::Leave:
				labels.push_back(fmt::format("{} ({})", IsJoinedGame() ? "Leave" : "Menu", InputName(PadAction::Cancel)));
				break;
			}
		}
		std::string note = gbIsMultiplayer ? CoopGameOverLine() : std::string();
		if (CanRetryRaid())
			note += fmt::format(" A retry starts with {} gold and gives back this raid's XP.", std::max(State.retryGold, 0));
		DrawMapSummary(out, UiMouse(), labels, note);
	}
	DrawCoopHostLeft(out, top);
	EditorDrawTestBanner(out);
	DrawHelp(out, UiMouse());
	PadDrawOverlay(out);
}

} // namespace

// ---- controller -----------------------------------------------------------------

void PadHudTargets(std::vector<Rectangle> &targets)
{
	const Surface ui = UiHitSurface();
	const int top = HudRect().position.y;
	if (IsGameOver()) {
		const int count = static_cast<int>(GameOverChoices().size());
		for (int i = 0; i < count; i++)
			targets.push_back(MapSummaryButton(ui, i, count));
		return;
	}
	if (LobbyPadTargets(LobbyAnchorRect(), targets))
		return; // the panel takes the clicks
	for (size_t i = 0; i < NumBarTabs; i++)
		targets.push_back(TabRect(i));
	for (int slot = 0; slot < ShownCardCount(); slot++)
		targets.push_back(CardRect(slot));
	for (size_t i = 0; i < NumSpellSlots; i++) {
		if (equippedSpells[i])
			targets.push_back(SpellSlotRect(i));
	}
	if (State.phase == Phase::Setup || State.phase == Phase::Breather)
		targets.push_back(ReadyButtonRect());
	if (drawnHud.sell && currentTool.type == ToolType::None && !Selection().empty())
		targets.push_back(*drawnHud.sell);
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
	if (dir.y != AxisDirectionY_NONE || focused.position.y != CardRowRect().position.y || focused.size.height != CardHeight() || focused.size.width != CardWidth)
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
	return ViewBottom();
}

int ViewBottom()
{
	if (IsEditorActive())
		return (UiHeight() - UiBarHeight()) * UiScale();
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
	if (LobbyPanelOpen())
		return fmt::format("{}: move   {}: add or remove   {}: close", InputName(PadAction::Navigate), confirm, cancel);
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
	// The map editor's bar: still the height of the engine's control panel.
	return gnScreenHeight - GetMainPanel().position.y;
}

} // namespace devilution::keeper
