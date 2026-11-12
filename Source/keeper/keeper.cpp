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
#include "control/control_flasks.hpp"
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
//   (      ) [ tabs ] [ gates ] scout line          [        ] (      )
//   ( Lord ) [ cards                      ]         [ spells ] ( mana )
//
// The orbs at the ends are the Keep Lord's health and the master's mana, in Diablo's own flasks,
// which stand a little higher than the strip as they do there. The rest of the status is one line
// at the top right of the screen.
//
// A lair card is a family of monsters. It places the tier last used, and its other tiers are in a
// pop-up above it. Families are in the order of the dungeons they come from; when they don't all
// fit one page, each dungeon has a tab of its own.
//
// The strip is at most HudMaxWidth wide and centred. Details (a card's name and text, a selected
// lair and its sell button) come up in a panel above the strip only while they are asked for.

constexpr int HudMaxWidth = 1280;
/** A tenth of a 720 pixel high UI. A lower UI (a TV's, at twice the size) gets a lower strip. */
constexpr int StripMaxHeight = 72;
constexpr int StripMinHeight = 48;
/** The flask's art is this size, and is drawn at it where the strip is its full height. */
constexpr int OrbArtSize = 88;
/** The liquid's rows in the art: empty at the foot, full at the top. */
constexpr int OrbFullRow = 3;
constexpr int OrbEmptyRow = 85;
/** The glass in the art: a circle about this row, this far across. The Lord's and the mana flask differ by a column. */
constexpr int OrbCentreY = 44;
constexpr int OrbLifeCentreX = 42;
constexpr int OrbManaCentreX = 41;
constexpr int OrbGlassRadius = 41;
/** The rim drawn round the glass, in the art's pixels. */
constexpr int OrbRimWidth = 3;
/** The orbs stand this far off the foot of the screen, so the rim is whole. */
constexpr int OrbLift = 2;
/** The status line at the top right: a fortieth of the UI's height, within these. */
constexpr int StatusLineMinHeight = 12;
constexpr int StatusLineMaxHeight = 18;
constexpr int InfoWidth = 240;
constexpr int SectionGap = 6;
constexpr int TabHeight = 16;
constexpr int TabGap = 2;
constexpr int MaxCardWidth = 46;
/** A full page's cards narrow to this before the page scrolls: a monster is still told from the next at this size. */
constexpr int MinCardWidth = 36;
constexpr int CardGap = 2;
/** Between the families of one dungeon and the next. */
constexpr int DividerWidth = 5;
/** What the scout line keeps of the tab row where it can't run on over the spells. */
constexpr int MinScoutRoom = 200;
constexpr int TierPopupWidth = 190;
/** A tier pop-up opened by a key closes itself this long after the last step. */
constexpr uint32_t TierPopupTicks = 3000;
/** A card this high or higher has its price under the portrait; a lower one has it over the portrait's foot. */
constexpr int CardHeightWithPriceRow = 50;
constexpr int CardPriceRowHeight = 13;
constexpr int ArrowWidth = 12;

bool IsLairTool(const Tool &tool)
{
	return tool.type == ToolType::Build && IsLairKind(tool.index);
}

enum class PageType : uint8_t {
	Lairs,
	Traps,
	Upgrades,
};

/** What one tab of the card bar shows. Each card is the tool it selects; a lair card stands for its family (see CardTool). */
struct BarPage {
	PageType type = PageType::Lairs;
	/** The dungeon whose families a lair page shows. None for the one page of every family, when they all fit. */
	std::optional<DungeonGroup> group;
	std::vector<Tool> cards;
	/** True for a card that begins another dungeon's families: a divider goes before it. */
	std::vector<bool> divider;
};

/** Built when a level loads, and again if the bar's width changes. See Pages(). */
std::vector<BarPage> pages;
int pagesBarWidth = -1;
size_t currentPage = 0;
/** First visible card of each page, for a page too long for a small window. */
std::vector<int> pageScroll;

// ---- family tiers ---------------------------------------------------------------

/** The kind each family's card places: the one last used in this session. -1 until then. */
std::array<int, NumKindFamilies> familyTier = [] {
	std::array<int, NumKindFamilies> none {};
	none.fill(-1);
	return none;
}();

/** The family's kinds this level offers, lowest tier first. */
std::vector<size_t> FamilyTiers(KindFamily family)
{
	std::vector<size_t> tiers;
	for (const size_t kind : MusterKinds()) {
		if (FamilyOfKind(kind) == family)
			tiers.push_back(kind);
	}
	return tiers;
}

/** The kind the family's card places now: the last one used if this level has it, otherwise the highest it has. */
size_t FamilyKind(KindFamily family)
{
	const int last = familyTier[static_cast<size_t>(family)];
	if (last >= 0 && IsKindMustered(static_cast<size_t>(last)) && FamilyOfKind(static_cast<size_t>(last)) == family)
		return static_cast<size_t>(last);
	const std::vector<size_t> tiers = FamilyTiers(family);
	return tiers.empty() ? 0 : tiers.back();
}

/** The tool a card selects: for a lair card, its family's current tier. */
Tool CardTool(const Tool &card)
{
	if (!IsLairTool(card))
		return card;
	return { ToolType::Build, FamilyKind(FamilyOfKind(card.index)) };
}

/** True if `card` is the card that selects `tool`: any tier of a family is that family's card. */
bool IsCardOf(const Tool &card, const Tool &tool)
{
	if (card.type != tool.type)
		return false;
	if (IsLairTool(card) && IsLairTool(tool))
		return FamilyOfKind(card.index) == FamilyOfKind(tool.index);
	return card.index == tool.index;
}

/** The tier pop-up above a family's card. */
struct TierPopup {
	bool open = false;
	KindFamily family {};
	/** The mouse has been over the card or the pop-up: it closes when the mouse leaves. */
	bool mouseSeen = false;
	/** Opened by a key, with the mouse elsewhere: it closes at this time. */
	uint32_t until = 0;
};
TierPopup tierPopup;
/** How many times a tier was picked from the pop-up. See PadToolStamp. */
uint32_t tierPicks = 0;

void CloseTierPopup()
{
	tierPopup.open = false;
}

void OpenTierPopup(KindFamily family)
{
	if (!tierPopup.open || tierPopup.family != family)
		tierPopup.mouseSeen = false;
	tierPopup.open = true;
	tierPopup.family = family;
	tierPopup.until = SDL_GetTicks() + TierPopupTicks;
}

/**
 * The pages for a bar `barWidth` wide. One card to a family, in the order of the dungeons. A handful of
 * families share one page; more than fit, or more than there are card keys, and each dungeon gets its own.
 */
std::vector<BarPage> BuildPages(int barWidth)
{
	std::vector<Tool> lairs;
	for (const size_t kind : MusterKinds()) {
		const Tool card { ToolType::Build, kind };
		if (std::none_of(lairs.begin(), lairs.end(), [&](const Tool &other) { return IsCardOf(other, card); }))
			lairs.push_back(card);
	}
	const auto groupOf = [](const Tool &card) { return GroupOfFamily(FamilyOfKind(card.index)); };
	std::stable_sort(lairs.begin(), lairs.end(), [&](const Tool &a, const Tool &b) { return groupOf(a) < groupOf(b); });
	int groups = 0;
	for (size_t i = 0; i < lairs.size(); i++) {
		if (i == 0 || groupOf(lairs[i]) != groupOf(lairs[i - 1]))
			groups++;
	}
	const int count = static_cast<int>(lairs.size());
	const bool onePage = count <= static_cast<int>(NumCardSlotKeys)
	    && count * (MinCardWidth + CardGap) - CardGap + std::max(groups - 1, 0) * DividerWidth <= barWidth;

	std::vector<BarPage> built;
	for (size_t i = 0; i < lairs.size(); i++) {
		const bool newGroup = i == 0 || groupOf(lairs[i]) != groupOf(lairs[i - 1]);
		if (built.empty() || (newGroup && !onePage)) {
			built.emplace_back();
			if (!onePage)
				built.back().group = groupOf(lairs[i]);
		}
		built.back().divider.push_back(newGroup && !built.back().cards.empty());
		built.back().cards.push_back(lairs[i]);
	}
	if (built.empty())
		built.emplace_back();

	BarPage traps;
	traps.type = PageType::Traps;
	for (size_t i = 0; i < NumBuildKinds; i++) {
		if (!IsLairKind(i) && IsTrapAvailable(i - NumLairKinds))
			traps.cards.push_back({ ToolType::Build, i });
	}
	traps.cards.push_back({ ToolType::LockDoor, 0 });
	traps.cards.push_back({ ToolType::Bait, 0 });
	traps.divider.assign(traps.cards.size(), false);
	built.push_back(std::move(traps));

	BarPage upgrades;
	upgrades.type = PageType::Upgrades;
	for (size_t i = 0; i < NumUpgrades; i++)
		upgrades.cards.push_back({ ToolType::Upgrade, i });
	upgrades.divider.assign(upgrades.cards.size(), false);
	built.push_back(std::move(upgrades));
	return built;
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

/** The orbs shrink with the strip, and keep standing as far above it in proportion. */
int OrbSize()
{
	return OrbArtSize * StripHeight() / StripMaxHeight;
}

/** How far an orb stands above the strip. */
int OrbPoke()
{
	return OrbSize() + OrbLift - StripHeight();
}

/** The Keep Lord's orb at the left end of the strip, or the mana orb at the right end. */
Rectangle OrbRect(bool mana)
{
	const Rectangle hud = HudRect();
	const int size = OrbSize();
	return { { mana ? hud.position.x + hud.size.width - size : hud.position.x, hud.position.y + hud.size.height - size - OrbLift }, { size, size } };
}

/**
 * How far a point of an orb's square is from the middle of its glass, in half pixels of the art:
 * the glass, its rim and the hit test all measure with this.
 */
int OrbDistance(bool mana, int size, int x, int y)
{
	const int dx = (2 * x + 1) * OrbArtSize / size - (2 * (mana ? OrbManaCentreX : OrbLifeCentreX) + 1);
	const int dy = (2 * y + 1) * OrbArtSize / size - (2 * OrbCentreY + 1);
	int distance = 0;
	while (distance * distance < dx * dx + dy * dy)
		distance++;
	return distance;
}

int StatusLineHeight()
{
	return std::clamp(UiHeight() / 40, StatusLineMinHeight, StatusLineMaxHeight);
}

// Spell bar: the masters' spells are at the right of the strip, beside the mana they use. Their hotkeys
// cast instantly at the cursor; clicking a slot arms it for one click in the keep.
constexpr int SpellSlotSize = 44;
constexpr int SpellSlotGap = 2;
constexpr int SpellPanelPadding = 2;

Rectangle SpellSlotRect(size_t i)
{
	const Rectangle hud = HudRect();
	constexpr int Total = static_cast<int>(NumSpellSlots) * SpellSlotSize + (static_cast<int>(NumSpellSlots) - 1) * SpellSlotGap;
	const int left = OrbRect(/*mana=*/true).position.x - SectionGap - SpellPanelPadding - Total;
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
// The middle of the strip: a row of tabs, and under it the current page's cards. Everything about
// where a card is comes from CardRect(slot) and ShownCardCount(); what the cards are comes from
// BuildPages().

/** The room between the Lord's orb and the spells. */
Rectangle BarRect()
{
	const Rectangle orb = OrbRect(/*mana=*/false);
	const int left = orb.position.x + orb.size.width + SectionGap;
	const int right = SpellPanelRect().position.x - SectionGap;
	return { { left, HudRect().position.y }, { std::max(right - left, MinCardWidth + 2 * (ArrowWidth + CardGap)), HudRect().size.height } };
}

int CardHeight()
{
	return StripHeight() - TabHeight;
}

/** The portrait on a card of full width: all of it but the frame and, where there is one, the price row. */
Size CardPortraitSize()
{
	return { MaxCardWidth - 4, CardHeight() - 4 - (CardHeight() >= CardHeightWithPriceRow ? CardPriceRowHeight : 0) };
}

/** The bar's pages, for the bar as wide as it is now. */
const std::vector<BarPage> &Pages()
{
	const int width = BarRect().size.width;
	if (pages.empty() || pagesBarWidth != width) {
		// The window changed size: stay on the page that shows what the old one did.
		std::optional<BarPage> was;
		if (!pages.empty())
			was = pages[std::min(currentPage, pages.size() - 1)];
		pages = BuildPages(width);
		pagesBarWidth = width;
		pageScroll.assign(pages.size(), 0);
		currentPage = 0;
		for (size_t i = 0; was && i < pages.size(); i++) {
			if (pages[i].type == was->type && (pages[i].type != PageType::Lairs || pages[i].group == was->group || !pages[i].group)) {
				currentPage = i;
				break;
			}
		}
	}
	return pages;
}

/** The page on show. */
const BarPage &Page()
{
	const std::vector<BarPage> &all = Pages();
	currentPage = std::min(currentPage, all.size() - 1);
	return all[currentPage];
}

void ShowPage(size_t page)
{
	currentPage = page % Pages().size();
	CloseTierPopup();
}

/** The page with the tool's card. */
size_t PageOf(const Tool &tool)
{
	const std::vector<BarPage> &all = Pages();
	for (size_t i = 0; i < all.size(); i++) {
		if (std::any_of(all[i].cards.begin(), all[i].cards.end(), [&](const Tool &card) { return IsCardOf(card, tool); }))
			return i;
	}
	return currentPage;
}

int DividerCount(const BarPage &page)
{
	return static_cast<int>(std::count(page.divider.begin(), page.divider.end(), true));
}

/** Even at their narrowest the page's cards don't fit the bar (a small window): it scrolls, and has an arrow at each end. */
bool PageScrolls(const BarPage &page)
{
	const int count = static_cast<int>(page.cards.size());
	return count * (MinCardWidth + CardGap) - CardGap + DividerCount(page) * DividerWidth > BarRect().size.width;
}

/** The width of the cards on show: full if the page fits, otherwise as narrow as it takes. */
int CardWidth()
{
	const BarPage &page = Page();
	if (PageScrolls(page))
		return MinCardWidth;
	const int count = std::max(static_cast<int>(page.cards.size()), 1);
	return std::clamp((BarRect().size.width - DividerCount(page) * DividerWidth + CardGap) / count - CardGap, MinCardWidth, MaxCardWidth);
}

/** The room for the current page's cards: between the arrows if there are any. */
Rectangle CardRowRect()
{
	const Rectangle bar = BarRect();
	const int arrows = PageScrolls(Page()) ? ArrowWidth + CardGap : 0;
	return { { bar.position.x + arrows, bar.position.y + TabHeight }, { bar.size.width - 2 * arrows, CardHeight() } };
}

int VisibleCardCount()
{
	const BarPage &page = Page();
	if (!PageScrolls(page))
		return std::max(static_cast<int>(page.cards.size()), 1);
	return std::max(1, (CardRowRect().size.width + CardGap) / (MinCardWidth + CardGap));
}

int MaxScroll()
{
	return std::max(0, static_cast<int>(Page().cards.size()) - VisibleCardCount());
}

int &Scroll()
{
	Page();
	int &scroll = pageScroll[currentPage];
	scroll = std::clamp(scroll, 0, MaxScroll());
	return scroll;
}

/** How many cards are on show. */
int ShownCardCount()
{
	return std::min(VisibleCardCount(), static_cast<int>(Page().cards.size()) - Scroll());
}

/** Where the card in `slot` is: slot 0 is the first one on show. */
Rectangle CardRect(int slot)
{
	const Rectangle row = CardRowRect();
	const BarPage &page = Page();
	const int first = Scroll();
	int dividers = 0;
	for (int i = 1; i <= slot && first + i < static_cast<int>(page.divider.size()); i++) {
		if (page.divider[first + i])
			dividers++;
	}
	return { { row.position.x + slot * (CardWidth() + CardGap) + dividers * DividerWidth, row.position.y }, { CardWidth(), row.size.height } };
}

/** The cards on show. */
Rectangle ShownCardsRect()
{
	const Rectangle first = CardRect(0);
	const Rectangle last = CardRect(std::max(ShownCardCount(), 1) - 1);
	return { first.position, { last.position.x + last.size.width - first.position.x, first.size.height } };
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
	if (!PageScrolls(Page()))
		return cards;
	const Rectangle left = LeftArrowRect();
	const Rectangle right = RightArrowRect();
	return { left.position, { right.position.x + right.size.width - left.position.x, cards.size.height } };
}

// The tabs, one to a page. They share their row with the scout line, so they are as narrow as their
// names, the names get shorter when there are many, and on a narrow bar (a TV's) only the current
// page's name shows, between two arrows that step through the pages.

enum class TabStyle : uint8_t {
	Full,
	Short,
	Stepper,
};

std::string_view PageName(const BarPage &page, bool full)
{
	switch (page.type) {
	case PageType::Lairs:
		return page.group ? GroupName(*page.group) : std::string_view("Lairs");
	case PageType::Traps:
		return full ? "Traps & Tricks" : "Traps";
	case PageType::Upgrades:
		break;
	}
	return "Upgrades";
}

int TabWidth(const BarPage &page, bool full)
{
	return GetLineWidth(PageName(page, full)) + 12;
}

/** The most the gates button may take of the tab row. The tabs leave it this even during a raid, so they stay where they are. */
int GatesButtonRoom()
{
	return std::max(BarRect().size.width / 5, 100);
}

TabStyle CurrentTabStyle()
{
	const std::vector<BarPage> &all = Pages();
	const Rectangle bar = BarRect();
	// The scout line can run on over the spells where the strip is high enough to have room above them.
	const bool overSpells = SpellPanelRect().position.y >= bar.position.y + TabHeight;
	const int room = bar.size.width - GatesButtonRoom() - SectionGap - (overSpells ? 0 : MinScoutRoom);
	int full = 0;
	int brief = 0;
	for (const BarPage &page : all) {
		full += TabWidth(page, true) + TabGap;
		brief += TabWidth(page, false) + TabGap;
	}
	if (all.size() <= 3 && full <= room)
		return TabStyle::Full;
	return brief <= room ? TabStyle::Short : TabStyle::Stepper;
}

/** The arrows of the stepper, to the page before (-1) and after (1). Empty when every page has a tab. */
Rectangle TabStepRect(int direction)
{
	if (CurrentTabStyle() != TabStyle::Stepper)
		return {};
	const Rectangle bar = BarRect();
	if (direction < 0)
		return { bar.position, { ArrowWidth, TabHeight } };
	return { { bar.position.x + ArrowWidth + TabGap + TabWidth(Page(), false) + TabGap, bar.position.y }, { ArrowWidth, TabHeight } };
}

/** The page's tab. Empty for a page that has none: in the stepper only the current one does. */
Rectangle TabRect(size_t i)
{
	const std::vector<BarPage> &all = Pages();
	const Rectangle bar = BarRect();
	const TabStyle style = CurrentTabStyle();
	if (style == TabStyle::Stepper) {
		if (i != currentPage)
			return {};
		return { { bar.position.x + ArrowWidth + TabGap, bar.position.y }, { TabWidth(all[i], false), TabHeight } };
	}
	int x = bar.position.x;
	for (size_t tab = 0; tab < i; tab++)
		x += TabWidth(all[tab], style == TabStyle::Full) + TabGap;
	return { { x, bar.position.y }, { TabWidth(all[i], style == TabStyle::Full), TabHeight } };
}

Rectangle TabsRect()
{
	if (CurrentTabStyle() == TabStyle::Stepper) {
		const Rectangle next = TabStepRect(1);
		return { BarRect().position, { next.position.x + next.size.width - BarRect().position.x, TabHeight } };
	}
	const Rectangle first = TabRect(0);
	const Rectangle last = TabRect(Pages().size() - 1);
	return { first.position, { last.position.x + last.size.width - first.position.x, first.size.height } };
}

HudArea PageHudArea(const BarPage &page)
{
	switch (page.type) {
	case PageType::Lairs:
		return HudArea::TabLairs;
	case PageType::Traps:
		return HudArea::TabTraps;
	case PageType::Upgrades:
		break;
	}
	return HudArea::TabUpgrades;
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
	/** The tier pop-up, and each of its rows with the kind it picks. */
	std::optional<Rectangle> popup;
	std::vector<std::pair<Rectangle, size_t>> tiers;
	/** The status line at the top right, and the parts of it that take a click. */
	std::optional<Rectangle> status;
	Rectangle phase;
	Rectangle speed;
	Rectangle help;
};
DrawnHud drawnHud;

/** The "?" at the end of the status line. */
Rectangle HelpButtonRect()
{
	return drawnHud.help;
}

/** The speed toggle, just left of the help button. */
Rectangle SpeedButtonRect()
{
	return drawnHud.speed;
}

/** The phase and its clock: clicking the clock holds it. */
Rectangle PhaseLineRect()
{
	return drawnHud.phase;
}

std::string ReadyButtonLabel();

/** What the gates button says: with its key if the tab row has the room, in fewer words if it hasn't. */
std::string ReadyButtonText()
{
	const int room = GatesButtonRoom() - 12;
	const std::string label = ReadyButtonLabel();
	const std::string key = InputName(KeeperAction::StartRaid);
	if (const std::string withKey = fmt::format("{}: {}", key, label); !key.empty() && GetLineWidth(withKey) <= room)
		return withKey;
	if (GetLineWidth(label) <= room)
		return label;
	return FitText(gbIsMultiplayer ? label : std::string("Open gates"), room);
}

/** The gates button, after the tabs while the keep is being built. Empty during a raid. */
Rectangle ReadyButtonRect()
{
	if (!IsBuildingPhase())
		return {};
	const Rectangle tabs = TabsRect();
	return { { tabs.position.x + tabs.size.width + SectionGap, tabs.position.y }, { GetLineWidth(ReadyButtonText()) + 12, TabHeight } };
}

/** True over an orb's glass or rim: its square's corners are the keep. */
bool IsOverOrb(bool mana, Point p)
{
	const Rectangle orb = OrbRect(mana);
	const int margin = OrbRimWidth + 1;
	if (p.x < orb.position.x - margin || p.y < orb.position.y - margin || p.x >= orb.position.x + orb.size.width + margin || p.y >= orb.position.y + orb.size.height + margin)
		return false;
	return OrbDistance(mana, orb.size.width, p.x - orb.position.x, p.y - orb.position.y) <= 2 * (OrbGlassRadius + OrbRimWidth) + 1;
}

/** True over one of the strip's panels, the details panel or the status line: not over the keep between them. */
bool IsMouseOverStrip(Point p)
{
	if (IsOverOrb(false, p) || IsOverOrb(true, p) || Contains(SpellPanelRect(), p) || Contains(CardPanelRect(), p) || Contains(TabsRect(), p) || Contains(ReadyButtonRect(), p))
		return true;
	if (drawnHud.status && Contains(*drawnHud.status, p))
		return true;
	return (drawnHud.scout && Contains(*drawnHud.scout, p)) || (drawnHud.info && Contains(*drawnHud.info, p)) || (drawnHud.popup && Contains(*drawnHud.popup, p));
}

/** What the status panel, tabs and scout line do, for the details panel. */
std::optional<HudArea> HudAreaAt(Point p)
{
	if (Contains(HelpButtonRect(), p))
		return HudArea::HelpButton;
	if (IsBuildingPhase() && Contains(ReadyButtonRect(), p))
		return HudArea::OpenGates;
	for (size_t i = 0; i < Pages().size(); i++) {
		if (Contains(TabRect(i), p))
			return PageHudArea(Pages()[i]);
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

/** The card under `p` on the current page, if any. A lair card stands for its family: CardTool() is what it selects. */
std::optional<Tool> CardAt(Point p)
{
	const std::vector<Tool> &cards = Page().cards;
	const int first = Scroll();
	for (int slot = 0; slot < ShownCardCount(); slot++) {
		if (Contains(CardRect(slot), p))
			return cards[first + slot];
	}
	return std::nullopt;
}

/** The slot of the tool's card, if it is on show. */
std::optional<int> SlotOfTool(const Tool &tool)
{
	const std::vector<Tool> &cards = Page().cards;
	const int first = Scroll();
	for (int slot = 0; slot < ShownCardCount(); slot++) {
		if (IsCardOf(cards[first + slot], tool))
			return slot;
	}
	return std::nullopt;
}

/** Switches to the tool's page and scrolls it into view (for hotkeys). */
void RevealCard(const Tool &tool)
{
	ShowPage(PageOf(tool));
	const std::vector<Tool> &cards = Page().cards;
	const auto it = std::find_if(cards.begin(), cards.end(), [&](const Tool &card) { return IsCardOf(card, tool); });
	if (it == cards.end())
		return;
	const int index = static_cast<int>(it - cards.begin());
	int &scroll = Scroll();
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
	if (const std::optional<int> slot = SlotOfTool(tool); slot && *slot < static_cast<int>(NumCardSlotKeys))
		hotkey = InputName(KeeperAction::CardSlot, static_cast<size_t>(*slot));
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

/** Every mustered kind's portrait, made for a card of full width. A narrower card shows its middle. */
std::array<KindImage, NumLairKinds> cardImages;
Size cardImageSize;

const KindImage &CardImage(size_t kind)
{
	const Size size = CardPortraitSize();
	// Made for the card's size, which changes with the screen's.
	if (cardImageSize != size) {
		cardImages = {};
		cardImageSize = size;
	}
	if (!cardImages[kind].built)
		cardImages[kind] = MakeKindImage(kind, size.width, size.height);
	return cardImages[kind];
}

/** Draws the kind at the bottom middle of `area`, losing its sides if the area is narrower than a full card's. */
void DrawKindPortrait(const Surface &area, size_t kind)
{
	if (kind >= NumLairKinds)
		return;
	const KindImage &image = CardImage(kind);
	const int offsetX = (area.w() - image.width) / 2;
	const int offsetY = area.h() - image.height;
	for (int y = std::max(-offsetY, 0); y < image.height; y++) {
		const uint8_t *row = &image.pixels[static_cast<size_t>(y) * image.width];
		for (int x = std::max(-offsetX, 0); x < image.width && offsetX + x < area.w(); x++) {
			if (row[x] != 0)
				*area.at(offsetX + x, offsetY + y) = row[x];
		}
	}
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

/**
 * A card is its portrait, the key that picks it and its price. Its name is in the details panel.
 * A family with more than one tier has a pip for each at the right, the lit one being the tier the card places.
 */
void DrawCard(const Surface &out, const Rectangle &rect, const Tool &card, bool hovered)
{
	const Tool tool = CardTool(card);
	const CardText text = DescribeCard(tool);
	const bool selected = currentTool.type == tool.type && currentTool.index == tool.index;

	FillRect(out, rect.position.x, rect.position.y, rect.size.width, rect.size.height, PAL16_GRAY + 14);
	const Rectangle portrait { rect.position + Displacement { 2, 2 }, { rect.size.width - 4, CardPortraitSize().height } };
	FillRect(out, portrait.position.x, portrait.position.y, portrait.size.width, portrait.size.height, 0);
	DrawPortrait(out.subregion(portrait.position.x, portrait.position.y, portrait.size.width, portrait.size.height), tool);

	if (!text.ready) {
		// Greyed out like Dungeon Keeper's unaffordable rooms.
		DrawHalfTransparentRectTo(out, portrait.position.x, portrait.position.y, portrait.size.width, portrait.size.height);
		DrawHalfTransparentRectTo(out, portrait.position.x, portrait.position.y, portrait.size.width, portrait.size.height);
	}

	if (IsLairTool(card)) {
		const std::vector<size_t> tiers = FamilyTiers(FamilyOfKind(card.index));
		// The highest tier's pip is at the top, as it is in the pop-up.
		for (size_t i = 0; tiers.size() > 1 && i < tiers.size(); i++) {
			const size_t kind = tiers[tiers.size() - 1 - i];
			FillRect(out, portrait.position.x + portrait.size.width - 4, portrait.position.y + 1 + static_cast<int>(i) * 4, 3, 3, kind == tool.index ? PAL16_YELLOW + 2 : PAL16_GRAY + 9);
		}
	}

	DrawString(out, FitText(text.hotkey, portrait.size.width - 2), { portrait.position + Displacement { 1, 0 }, { portrait.size.width - 2, 12 } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorWhite | UiFlags::Outlined });
	// A narrow card has room for the number, if not for its "g".
	std::string cost = text.cost;
	if (GetLineWidth(cost) > rect.size.width - 2 && !cost.empty() && cost.back() == 'g')
		cost.pop_back();
	DrawString(out, FitText(cost, rect.size.width - 2), { { rect.position.x, rect.position.y + rect.size.height - CardPriceRowHeight - 1 }, { rect.size.width, 12 } },
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
	const std::vector<BarPage> &all = Pages();
	const TabStyle style = CurrentTabStyle();
	for (size_t i = 0; i < all.size(); i++) {
		const Rectangle tab = TabRect(i);
		if (tab.size.width == 0)
			continue;
		const bool selected = currentPage == i;
		FillRect(out, tab.position.x, tab.position.y, tab.size.width, tab.size.height, selected ? PAL16_RED + 12 : PAL16_GRAY + 14);
		DrawFrame(out, tab, selected ? PAL16_YELLOW + 4 : PAL16_GRAY + 10);
		NoteOpaqueUi(tab);
		DrawString(out, PageName(all[i], style == TabStyle::Full), { tab.position + Displacement { 0, 1 }, { tab.size.width, 14 } },
		    { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | (selected ? UiFlags::ColorGold : UiFlags::ColorWhite) });
	}
	if (style == TabStyle::Stepper) {
		for (const int direction : { -1, 1 }) {
			const Rectangle arrow = TabStepRect(direction);
			FillRect(out, arrow.position.x, arrow.position.y, arrow.size.width, arrow.size.height, PAL16_GRAY + 14);
			DrawFrame(out, arrow, Contains(arrow, UiMouse()) ? PAL16_YELLOW + 4 : PAL16_GRAY + 10);
			NoteOpaqueUi(arrow);
			DrawString(out, direction < 0 ? "<" : ">", { arrow.position + Displacement { 0, 1 }, { arrow.size.width, 14 } }, { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | UiFlags::ColorGold });
		}
	}
}

/** The tabs, and the current page's cards on a panel no wider than they are. */
void DrawCardBar(const Surface &out)
{
	DrawTabs(out);

	const Rectangle panel = CardPanelRect();
	FillRect(out, panel.position.x, panel.position.y, panel.size.width, panel.size.height, 0);
	NoteOpaqueUi(panel);

	const BarPage &page = Page();
	const int first = Scroll();
	const std::optional<Tool> hovered = CardAt(UiMouse());
	for (int slot = 0; slot < ShownCardCount(); slot++) {
		const Tool &card = page.cards[first + slot];
		const Rectangle rect = CardRect(slot);
		// Another dungeon's families begin here.
		if (slot > 0 && page.divider[first + slot])
			DrawVerticalLine(out, { rect.position.x - (CardGap + DividerWidth + 1) / 2, rect.position.y + 2 }, rect.size.height - 4, PAL16_GRAY + 5);
		DrawCard(out, rect, card, hovered && IsCardOf(*hovered, card));
	}
	if (PageScrolls(page)) {
		DrawArrow(out, LeftArrowRect(), "<", first > 0);
		DrawArrow(out, RightArrowRect(), ">", first < MaxScroll());
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
	/** The panel goes beside this instead: the tier pop-up, which is where it would be. */
	std::optional<Rectangle> beside;
	/** The panel goes under this instead: the status line at the top of the screen. */
	std::optional<Rectangle> below;
	/** An orb's panel clears the part of it that stands above the strip. */
	int raise = 0;
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
	if (IsLairTool(tool)) {
		const std::vector<size_t> tiers = FamilyTiers(FamilyOfKind(tool.index));
		if (tiers.size() > 1) {
			const size_t tier = static_cast<size_t>(std::find(tiers.begin(), tiers.end(), tool.index) - tiers.begin()) + 1;
			info.body += IsPadInUse()
			    ? fmt::format("\nTier {} of {}. {} on the card: the others.", tier, tiers.size(), InputName(PadAction::Order))
			    : fmt::format("\nTier {} of {}. Wheel or right-click the card: the others.", tier, tiers.size());
		}
	}
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
	if (const std::optional<int> slot = SlotOfTool(tool); slot)
		return CardRect(*slot);
	return std::nullopt;
}

/** What the details panel has to say now, if anything: the thing under the mouse, else the tool in hand, else the selection. */
std::optional<InfoContent> CurrentInfo()
{
	const Point mouse = UiMouse();
	// The tier pop-up is where a card's details would be: they go beside it, for the tier under the mouse or the one in use.
	if (tierPopup.open && drawnHud.popup) {
		size_t kind = FamilyKind(tierPopup.family);
		for (const auto &[row, rowKind] : drawnHud.tiers) {
			if (Contains(row, mouse))
				kind = rowKind;
		}
		InfoContent info = CardInfo({ ToolType::Build, kind }, *drawnHud.popup);
		info.beside = drawnHud.popup;
		return info;
	}
	if (const std::optional<Tool> card = CardAt(mouse); card)
		return CardInfo(CardTool(*card), ToolAnchor(*card).value_or(CardPanelRect()));
	if (const std::optional<size_t> slot = SpellSlotAt(mouse); slot) {
		if (equippedSpells[*slot])
			return CardInfo({ ToolType::Spell, *equippedSpells[*slot] }, SpellSlotRect(*slot));
		return std::nullopt;
	}
	if (IsOverOrb(false, mouse)) {
		InfoContent info;
		info.title = "Keep Lord";
		if (const Monster *boss = FindBoss(); boss != nullptr) {
			info.second = fmt::format("{} of {} life", boss->hitPoints >> 6, boss->maxHitPoints >> 6);
			info.secondColor = UiFlags::ColorRed;
		} else if (State.bossDown) {
			info.second = "Down";
			info.secondColor = UiFlags::ColorRed;
		}
		info.body = "If he falls, a hero opens a portal, and the keep is lost if any hero escapes through it. Wipe out the party first and he rises again.";
		info.anchor = OrbRect(false);
		info.raise = OrbPoke();
		return info;
	}
	if (const std::optional<HudArea> area = HudAreaAt(mouse); area) {
		const HudHint hint = DescribeHudArea(*area);
		InfoContent info;
		info.title = hint.title;
		info.body = hint.text;
		if (*area == HudArea::Mana) {
			info.second = fmt::format("{} of {}", State.mana[MyPlayerId] / ManaScale, MaxMana);
			info.secondColor = UiFlags::ColorBlue;
		}
		// A dungeon's tab says which dungeon: on a narrow bar its name may not be on show.
		for (size_t i = 0; i < Pages().size(); i++) {
			if (Pages()[i].group && Contains(TabRect(i), mouse))
				info.title = fmt::format("{}: {}", hint.title, GroupName(*Pages()[i].group));
		}
		// Over the mouse, or under it for the line at the top.
		info.anchor = { { mouse.x, HudRect().position.y }, { 1, 1 } };
		if (drawnHud.status && Contains(*drawnHud.status, mouse))
			info.below = drawnHud.status;
		if (*area == HudArea::Mana)
			info.raise = OrbPoke();
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

	int x = std::clamp(info->anchor.position.x + info->anchor.size.width / 2 - InfoWidth / 2, 2, std::max(out.w() - InfoWidth - 2, 2));
	// Between the orbs, which stand above the strip, unless it is an orb's own.
	if (info->raise == 0 && OrbRect(true).position.x - OrbRect(false).position.x - OrbRect(false).size.width >= InfoWidth + 4)
		x = std::clamp(x, OrbRect(false).position.x + OrbRect(false).size.width + 2, OrbRect(true).position.x - InfoWidth - 2);
	if (info->beside) {
		const int right = info->beside->position.x + info->beside->size.width + 2;
		x = right + InfoWidth <= out.w() ? right : std::max(info->beside->position.x - InfoWidth - 2, 0);
	}
	const Rectangle panel { { x, info->below ? info->below->position.y + info->below->size.height + 2 : std::max(top - 4 - height - info->raise, 0) }, { InfoWidth, height } };
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

// ---- tier pop-up ------------------------------------------------------------------
//
// A family's tiers, in a list that stands on its card: highest at the top. Opened by the wheel or a
// right click on the card, or by the card's key pressed again; a click on a row picks that tier.

void DrawTierPopup(const Surface &out)
{
	if (!tierPopup.open)
		return;
	const std::vector<size_t> tiers = FamilyTiers(tierPopup.family);
	const size_t current = FamilyKind(tierPopup.family);
	const std::optional<Rectangle> card = ToolAnchor({ ToolType::Build, current });
	if (!card || tiers.size() < 2) {
		CloseTierPopup();
		return;
	}
	const Size portrait = CardPortraitSize();
	const int rowHeight = std::max(portrait.height, 28) + 2;
	const int height = static_cast<int>(tiers.size()) * rowHeight + 2;
	const Rectangle popup { { std::clamp(card->position.x, 0, std::max(out.w() - TierPopupWidth, 0)), std::max(card->position.y - height, 0) }, { TierPopupWidth, height } };

	// It goes when the mouse leaves it and its card, or, if the mouse never came, a while after the last step.
	const Point mouse = UiMouse();
	const Rectangle cardZone { card->position + Displacement { -2, -2 }, { card->size.width + 4, card->size.height + 4 } };
	if (Contains(popup, mouse) || Contains(cardZone, mouse)) {
		tierPopup.mouseSeen = true;
	} else if (tierPopup.mouseSeen || SDL_GetTicks() > tierPopup.until) {
		CloseTierPopup();
		return;
	}

	FillRect(out, popup.position.x, popup.position.y, popup.size.width, popup.size.height, 0);
	DrawFrame(out, popup, PAL16_YELLOW + 4);
	NoteOpaqueUi(popup);
	drawnHud.popup = popup;
	for (size_t i = 0; i < tiers.size(); i++) {
		const size_t kind = tiers[tiers.size() - 1 - i];
		const Rectangle row { popup.position + Displacement { 1, 1 + static_cast<int>(i) * rowHeight }, { popup.size.width - 2, rowHeight } };
		const CardText text = DescribeCard({ ToolType::Build, kind });
		const bool hovered = Contains(row, mouse);
		if (kind == current)
			FillRect(out, row.position.x, row.position.y, row.size.width, row.size.height, PAL16_GRAY + 14);
		const Rectangle image { row.position + Displacement { 2, row.size.height - 1 - portrait.height }, portrait };
		DrawKindPortrait(out.subregion(image.position.x, image.position.y, image.size.width, image.size.height), kind);
		if (!text.ready) {
			DrawHalfTransparentRectTo(out, image.position.x, image.position.y, image.size.width, image.size.height);
			DrawHalfTransparentRectTo(out, image.position.x, image.position.y, image.size.width, image.size.height);
		}
		const int textLeft = image.position.x + image.size.width + 4;
		const int textWidth = row.position.x + row.size.width - 4 - textLeft;
		const int textTop = row.position.y + (row.size.height - 27) / 2;
		DrawString(out, FitText(text.name, textWidth), { { textLeft, textTop }, { textWidth, 14 } }, { .flags = UiFlags::FontSize12 | (kind == current ? UiFlags::ColorGold : UiFlags::ColorWhite) });
		DrawString(out, text.cost, { { textLeft, textTop + 13 }, { textWidth, 14 } }, { .flags = UiFlags::FontSize12 | (text.ready ? UiFlags::ColorGold : UiFlags::ColorRed) });
		if (kind == current || hovered)
			DrawFrame(out, row, kind == current ? PAL16_YELLOW + 2 : PAL16_GRAY);
		drawnHud.tiers.emplace_back(row, kind);
	}
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
	// A lair in hand is the tier its family's card places from now on.
	if (IsLairTool(currentTool))
		familyTier[static_cast<size_t>(FamilyOfKind(index))] = static_cast<int>(index);
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

/** Takes this kind in hand, as its family's tier. Unlike a card, never puts it back. */
void ArmKind(size_t kind)
{
	currentTool = {};
	SelectTool(ToolType::Build, kind);
}

/**
 * Moves a family's card to its next tier up or down, and shows the pop-up. The new tier is taken in hand
 * if `arm`, or if the family already was: what is in hand is always what its card shows.
 */
void StepTier(KindFamily family, int delta, bool wrap, bool arm)
{
	const std::vector<size_t> tiers = FamilyTiers(family);
	if (tiers.size() < 2)
		return;
	const int count = static_cast<int>(tiers.size());
	const int at = static_cast<int>(std::find(tiers.begin(), tiers.end(), FamilyKind(family)) - tiers.begin());
	const int next = wrap ? (at + delta + count) % count : std::clamp(at + delta, 0, count - 1);
	const size_t kind = tiers[next];
	familyTier[static_cast<size_t>(family)] = static_cast<int>(kind);
	OpenTierPopup(family);
	if (arm || (IsLairTool(currentTool) && FamilyOfKind(currentTool.index) == family))
		ArmKind(kind);
}

/** A row of the tier pop-up was clicked. */
void PickTier(size_t kind)
{
	tierPicks++;
	ArmKind(kind);
	CloseTierPopup();
}

bool HasTiers(const Tool &card)
{
	return IsLairTool(card) && FamilyTiers(FamilyOfKind(card.index)).size() > 1;
}

/** A card was clicked. With its family's pop-up open that is a choice, so the tier is taken in hand, never put back. */
void ClickCard(const Tool &card)
{
	const Tool tool = CardTool(card);
	if (IsLairTool(card) && tierPopup.open && tierPopup.family == FamilyOfKind(card.index)) {
		PickTier(tool.index);
		return;
	}
	CloseTierPopup();
	ActivateCard(tool);
}

/** A card's key was pressed. Pressed again with the family in hand, it steps down through the tiers and round. */
void PressCardKey(const Tool &card)
{
	if (HasTiers(card) && IsCardOf(card, currentTool)) {
		StepTier(FamilyOfKind(card.index), -1, /*wrap=*/true, /*arm=*/true);
		return;
	}
	CloseTierPopup();
	ActivateCard(CardTool(card));
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
	const std::optional<Rectangle> inspect = InspectPanelRect(HudRect().position.x, HudRect().position.y - OrbPoke());
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
	// A new game starts on the monsters, wherever the last one left the bar. The tiers last used stay.
	pages.clear();
	currentPage = 0;
	CloseTierPopup();
	// Built during the load rather than as cards scroll into view, which would stutter.
	if (!IsEditorActive()) {
		for (const size_t kind : MusterKinds())
			CardImage(kind);
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
		const std::vector<Tool> &cards = Page().cards;
		const size_t card = static_cast<size_t>(Scroll()) + index;
		if (static_cast<int>(index) < ShownCardCount() && card < cards.size())
			PressCardKey(cards[card]);
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
		ShowPage(currentPage + 1);
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
		if (tierPopup.open) {
			CloseTierPopup();
			return true;
		}
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
		// The pop-up stands over the tabs: a click on it is a tier, or nothing.
		if (drawnHud.popup && Contains(*drawnHud.popup, UiMouse())) {
			for (const auto &[row, kind] : drawnHud.tiers) {
				if (Contains(row, UiMouse())) {
					PickTier(kind);
					break;
				}
			}
			return true;
		}
		for (size_t i = 0; i < Pages().size(); i++) {
			if (Contains(TabRect(i), UiMouse()) && i != currentPage) {
				ShowPage(i);
				break;
			}
		}
		for (const int direction : { -1, 1 }) {
			if (Contains(TabStepRect(direction), UiMouse())) {
				ShowPage(currentPage + Pages().size() + direction);
				break;
			}
		}
		// A page too long for a small window: the arrows page through it.
		const int scrollStep = static_cast<int>(Page().cards.size()) > 2 * VisibleCardCount() ? VisibleCardCount() : 1;
		if (PageScrolls(Page()) && Contains(LeftArrowRect(), UiMouse()))
			Scroll() -= scrollStep;
		else if (PageScrolls(Page()) && Contains(RightArrowRect(), UiMouse()))
			Scroll() += scrollStep;
		if (const std::optional<Tool> card = CardAt(UiMouse()); card)
			ClickCard(*card);
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
	CloseTierPopup();

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
	// On a family's card: its tiers. Anywhere else puts the pop-up away, with whatever else it does.
	const bool overPopup = drawnHud.popup && Contains(*drawnHud.popup, UiMouse());
	if (const std::optional<Tool> card = CardAt(UiMouse()); card && !overPopup && HasTiers(*card)) {
		const KindFamily family = FamilyOfKind(card->index);
		if (tierPopup.open && tierPopup.family == family)
			CloseTierPopup();
		else
			OpenTierPopup(family);
		return true;
	}
	if (tierPopup.open) {
		CloseTierPopup();
		return true;
	}
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
	// Over a family's card or its pop-up the wheel steps through the tiers: up for a higher one.
	if (drawnHud.popup && tierPopup.open && Contains(*drawnHud.popup, UiMouse())) {
		StepTier(tierPopup.family, deltaY > 0 ? 1 : -1, /*wrap=*/false, /*arm=*/true);
		return true;
	}
	if (const std::optional<Tool> card = CardAt(UiMouse()); card && HasTiers(*card)) {
		StepTier(FamilyOfKind(card->index), deltaY > 0 ? 1 : -1, /*wrap=*/false, /*arm=*/true);
		return true;
	}
	if (Contains(CardPanelRect(), UiMouse()) || Contains(TabsRect(), UiMouse())) {
		// Elsewhere over the card bar it scrolls a page that scrolls, and never zooms.
		Scroll() += deltaY > 0 ? -1 : 1;
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

// ---- orbs ------------------------------------------------------------------------
//
// Diablo's flasks, cut from its control panel with the stone around them: the full one from the
// panel's art, the empty one from the cel the engine drains it with. Shrunk for a low strip.

struct OrbArt {
	int size = 0;
	std::vector<uint8_t> full;
	std::vector<uint8_t> empty;
};
std::array<OrbArt, 2> orbArt;

const OrbArt *GetOrbArt(bool mana)
{
	// Where the flasks are in the panel: see control_flasks.cpp.
	constexpr int LifeFlaskX = 96;
	constexpr int ManaFlaskX = 464;
	const std::optional<OwnedSurface> &emptyFlask = mana ? pManaBuff : pLifeBuff;
	if (!BottomBuffer || !emptyFlask)
		return nullptr;
	OrbArt &art = orbArt[mana ? 1 : 0];
	const int size = OrbSize();
	if (art.size != size) {
		art.size = size;
		art.full.resize(static_cast<size_t>(size) * size);
		art.empty.resize(art.full.size());
		for (int y = 0; y < size; y++) {
			for (int x = 0; x < size; x++) {
				const int artX = x * OrbArtSize / size;
				const int artY = y * OrbArtSize / size;
				art.full[static_cast<size_t>(y) * size + x] = *BottomBuffer->at((mana ? ManaFlaskX : LifeFlaskX) + artX, artY);
				art.empty[static_cast<size_t>(y) * size + x] = *emptyFlask->at(artX, artY);
			}
		}
	}
	return &art;
}

/** Draws an orb `percent` full, with `label` across it: the round glass alone, in a gold rim. */
void DrawOrb(const Surface &out, bool mana, int percent, std::string_view label, UiFlags labelColor)
{
	const Rectangle rect = OrbRect(mana);
	const int size = rect.size.width;
	const OrbArt *art = GetOrbArt(mana);
	const int liquidTop = OrbEmptyRow - (OrbEmptyRow - OrbFullRow) * std::clamp(percent, 0, 100) / 100;
	// Distances in half pixels of the art, like OrbDistance.
	const int glass = 2 * OrbGlassRadius + 1;
	const int rim = glass + 2 * OrbRimWidth;
	const int margin = OrbRimWidth + 1;
	for (int y = -margin; y < size + margin; y++) {
		const int screenY = rect.position.y + y;
		if (screenY < 0 || screenY >= out.h())
			continue;
		const int artY = std::clamp(y * OrbArtSize / size, 0, OrbArtSize - 1);
		const int scaledY = std::clamp(y, 0, size - 1);
		int first = out.w();
		int last = -1;
		for (int x = -margin; x < size + margin; x++) {
			const int screenX = rect.position.x + x;
			if (screenX < 0 || screenX >= out.w())
				continue;
			const int distance = OrbDistance(mana, size, x, y);
			if (distance > rim)
				continue;
			uint8_t color;
			if (distance <= glass) {
				if (art != nullptr) {
					const std::vector<uint8_t> &pixels = artY >= liquidTop && artY < OrbEmptyRow ? art->full : art->empty;
					color = pixels[static_cast<size_t>(scaledY) * size + std::clamp(x, 0, size - 1)];
				} else {
					color = 0;
				}
			} else if (distance <= glass + 2) {
				color = PAL16_YELLOW + 11; // the shadow where the glass meets the rim
			} else if (distance <= rim - 2) {
				// Lit from the top left, like the panel's own gold.
				color = (x + y < size - size / 4) ? PAL16_YELLOW + 3 : PAL16_YELLOW + 7;
			} else {
				color = PAL16_YELLOW + 12;
			}
			*out.at(screenX, screenY) = color;
			first = std::min(first, screenX);
			last = std::max(last, screenX);
		}
		if (last >= first)
			NoteOpaqueUi({ { first, screenY }, { last - first + 1, 1 } });
	}
	DrawString(out, label, { { rect.position.x, rect.position.y + size / 2 - 5 }, { size, 14 } }, { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | UiFlags::Outlined | labelColor });
}

void DrawOrbs(const Surface &out)
{
	// The Lord's: empty once he is down, which the scout line spells out.
	if (const Monster *boss = FindBoss(); boss != nullptr) {
		const int percent = boss->maxHitPoints > 0 ? static_cast<int>(100LL * std::max(boss->hitPoints, 0) / boss->maxHitPoints) : 0;
		DrawOrb(out, false, percent, fmt::format("{}%", percent), UiFlags::ColorWhite);
	} else {
		DrawOrb(out, false, 0, State.bossDown ? "Down" : "", UiFlags::ColorRed);
	}
	const int mana = State.mana[MyPlayerId] / ManaScale;
	DrawOrb(out, true, 100 * mana / MaxMana, std::to_string(mana), UiFlags::ColorWhite);
	const Rectangle manaOrb = OrbRect(true);
	drawnHud.areas.emplace_back(Rectangle { { manaOrb.position.x + manaOrb.size.width / 6, manaOrb.position.y }, { manaOrb.size.width * 2 / 3, manaOrb.size.height } }, HudArea::Mana);
}

// ---- status line -------------------------------------------------------------------

/**
 * One line at the top right, with its top at `top`: the phase and its clock, raid, pop, gold and what the
 * raiders carry, the raiders alive, the speed toggle and the help button. As wide as what it says.
 */
void DrawStatusLine(const Surface &out, int top)
{
	struct Item {
		std::string text;
		UiFlags color;
		std::optional<HudArea> area;
	};
	std::vector<Item> items;
	std::string phaseLine = std::string(PhaseName(State.phase));
	if (IsBuildingPhase())
		phaseLine += IsPrepTimerHeld() ? std::string(" held") : fmt::format(" {}", FormatTicks(State.phaseTicks));
	items.push_back({ phaseLine, UiFlags::ColorRed, HudArea::Phase });
	items.push_back({ fmt::format("Raid {}/{}", std::min<int>(State.wave, NumWaves()), NumWaves()), UiFlags::ColorWhite, HudArea::Raids });
	const int supply = SupplyUsed();
	items.push_back({ fmt::format("Pop {}/{}", supply, SupplyCap()), supply >= SupplyCap() ? UiFlags::ColorRed : UiFlags::ColorWhite, HudArea::Pop });
	items.push_back({ fmt::format("{}g", State.vaultGold), UiFlags::ColorGold, HudArea::Vault });
	if (State.phase == Phase::Raid) {
		// Gold on its way out of the keep, in red beside what is left.
		if (const int inFlight = GoldInFlight(); inFlight > 0)
			items.push_back({ fmt::format("-{}g", inFlight), UiFlags::ColorRed, HudArea::Heroes });
		items.push_back({ fmt::format("Heroes {}", LivingHeroCount()), UiFlags::ColorWhite, HudArea::Heroes });
	}

	constexpr int Gap = 10;
	constexpr int Padding = 4;
	constexpr int SpeedWidth = 22;
	constexpr int HelpWidth = 16;
	const int height = StatusLineHeight();
	const auto width = [&] {
		int total = 2 * Padding + SpeedWidth + 2 + HelpWidth;
		for (const Item &item : items)
			total += GetLineWidth(item.text) + Gap;
		return total;
	};
	// A window too narrow for it all: the raid count goes, then the pop's word.
	if (width() > out.w())
		items.erase(items.begin() + 1);
	if (width() > out.w())
		items[1].text = fmt::format("{}/{}", supply, SupplyCap());

	const Rectangle line { { std::max(out.w() - width(), 0), top }, { std::min(width(), out.w()), height } };
	DrawHalfTransparentRectTo(out, line.position.x, line.position.y, line.size.width, line.size.height);
	DrawHalfTransparentRectTo(out, line.position.x, line.position.y, line.size.width, line.size.height);
	drawnHud.status = line;

	const int textY = top + (height - 14) / 2;
	int x = line.position.x + Padding;
	for (const Item &item : items) {
		const Rectangle rect { { x, top }, { GetLineWidth(item.text), height } };
		DrawString(out, item.text, { { x, textY }, { rect.size.width, 14 } }, { .flags = UiFlags::FontSize12 | item.color });
		if (item.area)
			drawnHud.areas.emplace_back(Rectangle { { x - Gap / 2, top }, { rect.size.width + Gap, height } }, *item.area);
		if (item.area == HudArea::Phase)
			drawnHud.phase = { { x - Padding, top }, { rect.size.width + Padding + Gap / 2, height } };
		x += rect.size.width + Gap;
	}

	const Rectangle speedButton { { x, top }, { SpeedWidth, height } };
	DrawFrame(out, speedButton, Contains(speedButton, UiMouse()) ? PAL16_YELLOW + 4 : PAL16_GRAY + 8);
	DrawString(out, fmt::format("{}x", GameSpeedMultiplier()), { { speedButton.position.x, textY }, { speedButton.size.width, 14 } },
	    { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | (GameSpeedLevel() > 0 ? UiFlags::ColorGold : UiFlags::ColorWhite) });
	drawnHud.speed = speedButton;
	drawnHud.areas.emplace_back(speedButton, HudArea::Phase);

	const Rectangle help { { x + SpeedWidth + 2, top }, { HelpWidth, height } };
	FillRect(out, help.position.x, help.position.y, help.size.width, help.size.height, PAL16_RED + 12);
	DrawFrame(out, help, Contains(help, UiMouse()) ? PAL16_YELLOW + 2 : PAL16_YELLOW + 4);
	DrawString(out, "?", { { help.position.x, textY }, { help.size.width, 14 } }, { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | UiFlags::ColorGold });
	drawnHud.help = help;
}

/** The gates button, after the tabs while the keep is being built. */
void DrawReadyButton(const Surface &out)
{
	const Rectangle ready = ReadyButtonRect();
	if (ready.size.width == 0)
		return;
	FillRect(out, ready.position.x, ready.position.y, ready.size.width, ready.size.height, PAL16_RED + 12);
	DrawFrame(out, ready, Contains(ready, UiMouse()) ? PAL16_YELLOW + 2 : PAL16_YELLOW + 4);
	NoteOpaqueUi(ready);
	DrawString(out, ReadyButtonText(), { ready.position + Displacement { 0, 1 }, { ready.size.width, 14 } }, { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | UiFlags::ColorGold });
}

/**
 * The scout line beside the tabs: the coming raid while the keep is being built, and during a raid only
 * what the orbs and the status line don't say (the Lord is down, or a monster is possessed).
 */
void DrawScoutLine(const Surface &out)
{
	std::string scout = State.phase == Phase::Raid || IsGameOver() ? std::string() : DescribeWave(State.wave);
	// How to read more about the raid: the first thing to go if the line is too long.
	std::string hint;
	if (State.phase != Phase::Raid && !IsGameOver() && !IsScoutCardOn())
		hint = fmt::format("  -  {}: scout card", InputName(KeeperAction::ScoutCard));
	UiFlags scoutColor = UiFlags::ColorGold;
	if (State.bossDown && FindBoss() == nullptr && !IsGameOver()) {
		const int channel = PortalChannelPercent();
		scout = PortalPosition() ? std::string("The Lord is down - the heroes are fleeing through their portal!")
		    : channel >= 0       ? fmt::format("The Lord is down - a hero is opening a portal ({}%)!", channel)
		                         : std::string("The Lord is down - stop the heroes from escaping!");
		hint.clear();
		scoutColor = UiFlags::ColorRed;
	}
	if (const Monster *possessed = MyPossessed(); possessed != nullptr) {
		const int hp = possessed->maxHitPoints > 0 ? 100 * possessed->hitPoints / possessed->maxHitPoints : 0;
		scout = fmt::format("Possessing {} ({}%)  {}: move/attack  {}: release", possessed->name(), hp, IsPadInUse() ? InputName(PadAction::Confirm) : std::string("click"), InputName(KeeperAction::Possess));
		hint.clear();
	}
	if (scout.empty())
		return;

	// From the tabs and the gates button to the mana orb if nothing is in the way, otherwise to the spells.
	const Rectangle tabs = TabsRect();
	const Rectangle ready = ReadyButtonRect();
	const Rectangle spells = SpellPanelRect();
	const int left = (ready.size.width > 0 ? ready.position.x + ready.size.width : tabs.position.x + tabs.size.width) + 8;
	const bool overSpells = spells.position.y >= tabs.position.y + TabHeight;
	int right = overSpells ? spells.position.x + spells.size.width : spells.position.x - SectionGap;
	// A joiner's "Choose spells" button is over the slots while the keep is being built.
	if (overSpells && gbIsMultiplayer && IsJoinedGame() && IsBuildingPhase())
		right -= 150;
	const int room = right - left - 8;
	if (room < 40)
		return;
	// Too long: how to read more goes first, then the words that say what the line is.
	std::string line = scout + hint;
	if (GetLineWidth(line) > room)
		line = scout;
	constexpr std::string_view Lead = "Next raid: ";
	if (GetLineWidth(line) > room && line.compare(0, Lead.size(), Lead) == 0)
		line.erase(0, Lead.size());
	line = FitText(line, room);
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

	// The status line is under the strips along the very top (the test-run banner, then the controller's
	// prompts), and the raid roster and the scout card are under it.
	const int topStrips = (IsEditorTestRun() ? 1 : 0) + (IsPadInUse() ? 1 : 0);
	const int statusTop = topStrips == 0 ? 0 : 4 + 20 * topStrips;
	const int underStatus = statusTop + StatusLineHeight() + 2;
	SetRaidRosterTop(underStatus);
	SetScoutCardTop(underStatus);
	DrawStatusLine(out, statusTop);
	DrawOrbs(out);

	DrawScoutCard(out, top, UiMouse()); // under the spell bar, which has to stay usable on a small screen
	DrawRefusalLabel(out, out.h());
	DrawCardBar(out);
	DrawSpellBar(out);
	DrawReadyButton(out);
	DrawScoutLine(out);
	DrawInspectPanel(out, hud.position.x, top - OrbPoke());
	DrawRaidRoster(out, UiMouse());
	DrawRaidSummary(out, top);
	DrawMasters(out);
	DrawLobby(out, UiMouse(), LobbyAnchorRect(), equippedSpells);
	// Over the rest: they are what the player just asked about.
	DrawTierPopup(out);
	DrawInfoPanel(out);

	// News along the top, between the masters list and the raid roster.
	int pauseY = 2;
	int pauseCentre = out.w() / 2;
	if (!IsGameOver()) {
		const std::optional<Rectangle> roster = RaidRosterRect(out);
		int newsLeft = gbIsMultiplayer ? 198 : 6;
		int newsRight = roster ? roster->position.x - 6 : out.w() - 6;
		// Under the status line: on a narrow screen it reaches most of the way across.
		int newsTop = underStatus + 2;
		bool tipFits = true;
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
	for (size_t i = 0; i < Pages().size(); i++) {
		if (TabRect(i).size.width > 0)
			targets.push_back(TabRect(i));
	}
	for (int slot = 0; slot < ShownCardCount(); slot++)
		targets.push_back(CardRect(slot));
	for (size_t i = 0; i < NumSpellSlots; i++) {
		if (equippedSpells[i])
			targets.push_back(SpellSlotRect(i));
	}
	if (State.phase == Phase::Setup || State.phase == Phase::Breather)
		targets.push_back(ReadyButtonRect());
	if (drawnHud.status) {
		targets.push_back(SpeedButtonRect());
		targets.push_back(HelpButtonRect());
	}
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
	if (dir.y != AxisDirectionY_NONE || focused.position.y != CardRowRect().position.y || focused.size.height != CardHeight() || focused.size.width != CardWidth())
		return false;
	int &scroll = Scroll();
	const int before = scroll;
	if (dir.x == AxisDirectionX_RIGHT && focused.position.x == CardRect(VisibleCardCount() - 1).position.x)
		scroll = std::min(scroll + 1, MaxScroll());
	else if (dir.x == AxisDirectionX_LEFT && focused.position.x == CardRect(0).position.x)
		scroll = std::max(scroll - 1, 0);
	return scroll != before;
}

Rectangle PadSwitchTab(int delta)
{
	ShowPage(currentPage + Pages().size() + delta);
	return CardRect(0);
}

void PadHudSecondary(const Rectangle &focused)
{
	const std::vector<Tool> &cards = Page().cards;
	const int first = Scroll();
	for (int slot = 0; slot < ShownCardCount(); slot++) {
		const Rectangle rect = CardRect(slot);
		if (rect.position == focused.position && rect.size == focused.size && HasTiers(cards[first + slot]))
			StepTier(FamilyOfKind(cards[first + slot].index), -1, /*wrap=*/true, /*arm=*/false);
	}
}

void PadToggleZoom()
{
	SetZoom(ZoomFactor() == 1);
}

uint32_t PadToolStamp()
{
	// A tier picked from the pop-up counts as a new tool even if it is the one that was in hand.
	return currentTool.type == ToolType::None ? 0 : ((tierPicks & 0xFF) << 24) + (static_cast<uint32_t>(currentTool.type) << 16) + static_cast<uint32_t>(currentTool.index) + 1;
}

int PadViewBottom()
{
	return ViewBottom();
}

int MessagesBottom()
{
	return IsEditorActive() ? ViewBottom() : ScreenHudTop() - OrbPoke() * UiScale();
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
		return fmt::format("{}: move   {}: press   {}: a monster's other tiers   {}: back to the map   {} {}: tabs", InputName(PadAction::Navigate), confirm, InputName(PadAction::Order), cancel, InputName(PadAction::PrevTab), InputName(PadAction::NextTab));
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
