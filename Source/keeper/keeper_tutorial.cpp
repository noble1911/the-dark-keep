/**
 * @file keeper/keeper_tutorial.cpp
 *
 * First-map tips, the help overlay and the HUD's hover hints. See keeper_tutorial.hpp.
 */
#include "keeper/keeper_tutorial.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <span>
#include <utility>

#ifdef USE_SDL3
#include <SDL3/SDL_timer.h>
#else
#include <SDL.h>
#endif

#include <fmt/format.h>

#include "cursor.h"
#include "diablo.h"
#include "engine/palette.h"
#include "engine/render/clx_render.hpp"
#include "engine/render/primitive_render.hpp"
#include "engine/render/text_render.hpp"
#include "keeper/keeper.hpp"
#include "keeper/keeper_alerts.hpp"
#include "keeper/keeper_campaign.hpp"
#include "keeper/keeper_editor.hpp"
#include "keeper/keeper_game.hpp"
#include "keeper/keeper_heroes.hpp"
#include "keeper/keeper_profile.hpp"
#include "keeper/keeper_raid.hpp"
#include "keeper/keeper_roster.hpp"
#include "keeper/keeper_ui.hpp"
#include "multi.h"
#include "panels/spell_icons.hpp"
#include "player.h"
#include "tables/itemdat.h"

namespace devilution::keeper {

namespace {

// ---- the words ------------------------------------------------------------------------------------

/** Tokens the texts may use, and what they stand for. */
struct InputToken {
	std::string_view token;
	KeeperAction action;
	size_t index;
};
constexpr std::array<InputToken, 14> InputTokens { {
	{ "raid", KeeperAction::StartRaid, 0 },
	{ "spell1", KeeperAction::Spell, 0 },
	{ "spell2", KeeperAction::Spell, 1 },
	{ "spell3", KeeperAction::Spell, 2 },
	{ "spell4", KeeperAction::Spell, 3 },
	{ "spell5", KeeperAction::Spell, 4 },
	{ "spell6", KeeperAction::Spell, 5 },
	{ "possess", KeeperAction::Possess, 0 },
	{ "tab", KeeperAction::NextTab, 0 },
	{ "sell", KeeperAction::Sell, 0 },
	{ "lock", KeeperAction::LockDoor, 0 },
	{ "bait", KeeperAction::Bait, 0 },
	{ "entrance", KeeperAction::CenterEntrance, 0 },
	{ "vault", KeeperAction::CenterVault, 0 },
} };

enum class TipTarget : uint8_t {
	None,
	Cards,
	Tabs,
	Ready,
	SpellBar,
	Roster,
};

/** Bits in the profile: keep the values, add new tips at the end. */
enum class Tip : uint8_t {
	PlaceMonster,
	OpenGates,
	CastSpell,
	Lord,
	Respite,
	Renown,
	Traps,
	Rally,
	Possess,
};

struct TipInfo {
	Tip tip;
	/** The first campaign level it is shown on (joiners see the basics on any level). */
	int fromLevel;
	std::string_view title;
	std::string_view text;
	TipTarget target;
	/** Seconds on screen before it counts as read; 0: until done or dismissed. */
	int seconds;
};

constexpr std::array<TipInfo, 8> GameTips { {
	{ Tip::PlaceMonster, 1, "Heroes are coming for your gold", "Pick a monster card, then click the floor between the entrance and the vault. Press {vault} to see the vault.", TipTarget::Cards, 0 },
	{ Tip::OpenGates, 1, "Ready when you are", "Place more if you can afford it. The gates open when the timer ends, or press {raid} to open them now.", TipTarget::Ready, 0 },
	{ Tip::CastSpell, 1, "Cast a spell", "Point at a hero and press {spell1}, or click a spell and then click the keep. Spells use mana.", TipTarget::SpellBar, 0 },
	{ Tip::Lord, 1, "Guard the Keep Lord", "If he falls, a hero opens a portal. If any hero escapes through it, the keep is lost. Kill them first!", TipTarget::Roster, 14 },
	{ Tip::Respite, 1, "A moment to rebuild", "Spend your gold before the next raid. Look through Traps & Tricks and Upgrades as well.", TipTarget::Tabs, 20 },
	{ Tip::Traps, 2, "Traps take no room", "On Traps & Tricks you can set traps on the heroes' path, and lock doors to slow them down.", TipTarget::Tabs, 25 },
	{ Tip::Rally, 2, "Move your monsters", "Click one of your monsters, then right-click the floor to choose where it stands guard.", TipTarget::None, 25 },
	{ Tip::Possess, 2, "Fight in person", "Point at one of your monsters and press {possess} to take control of it. It costs mana. Press it again to let go.", TipTarget::None, 20 },
} };

constexpr std::string_view RenownTipTitle = "Spend your Renown";
constexpr std::string_view RenownTipText = "You have {} Renown. Pick a card marked Unlock, then press its button on the right. It lasts for every map.";

struct HelpRow {
	UiIcon icon;
	/** For a row with no icon, the input it describes. */
	std::string_view title;
	std::string_view text;
};

constexpr std::array<HelpRow, 4> HelpGoal { {
	{ ItemIcon(ICURS_GOLD_LARGE), "Keep your gold", "Each level is a set of raids. Survive them all to clear the level. Heroes walk from the entrance to your vault to rob it." },
	{ MonsterIcon(MT_NBLACK), "Guard the Keep Lord", "If he is slain, a hero opens a portal. If any hero escapes through it, the keep is lost. Wipe out the party first and he rises again." },
	{ ItemIcon(ICURS_SHORT_BOW), "Stop the thieves", "A hero who reaches the vault takes gold and runs for the entrance. Kill the thief and the gold comes back, with a bounty." },
	{ ItemIcon(ICURS_CROWN), "Grow stronger", "Gold lasts for one map. XP lasts for ever: it raises your keeper level, and each level gives Renown to spend on the Keeper screen." },
} };

constexpr std::array<HelpRow, 4> HelpBuild { {
	{ MonsterIcon(MT_RFALLSP), "Lairs", "Monster cards. Pick one and click the floor. A slain monster comes back after the raid. Each takes up Pop, the room your keep has." },
	{ SpellIconOf(SpellID::RuneOfFire), "Traps & Tricks", "Traps fire at heroes who walk past. Locks hold a door shut for a while. Fool's gold draws greedy heroes away from their path." },
	{ SpellIconOf(SpellID::Identify), "Upgrades", "Spend gold to improve your lairs, traps, spells and the Keep Lord. They last for the current map." },
	{ ItemIcon(ICURS_GOLD_MEDIUM), "Selling", "Click what you placed, then press {sell} or its button. Between raids you get the full price back." },
} };

constexpr std::array<HelpRow, 4> HelpRaids { {
	{ SpellIconOf(SpellID::Nova), "Spells", "Point and press a spell's key, or click the spell and then the keep. They only work during a raid, and they use mana, which refills over time." },
	{ MonsterIcon(MT_NBLACK), "Your monsters", "Click one to see it. Right-click the floor to send it there. Hold Shift to add patrol points. Drag to select a group." },
	{ SpellIconOf(SpellID::Berserk), "Possess", "Point at your monster and press {possess} to fight as it. Click to move or attack. Press {possess} again to let go. It drains mana." },
	{ ItemIcon(ICURS_HELM), "The roster", "The list at the top right shows every hero in the raid and what they are doing. Click a name to jump to that hero." },
} };

constexpr std::array<HelpRow, 3> HelpKeeper { {
	{ ItemIcon(ICURS_BOOK_BLUE), "Keeper screen", "Reach it from the main menu, the level list, and between maps. Unlock monsters, spells and traps with Renown, and raise spells and traps." },
	{ SpellIconOf(SpellID::Fireball), "Spell bar", "You can bring six spells. Choose them on the Keeper screen's Spells tab. Respec is free, so try things out." },
	{ ItemIcon(ICURS_SCROLL_OF), "Co-op", "Masters share one keep and one vault. Each has their own mana. The game uses the host's unlocks, and each player keeps the XP they earn." },
} };

/** Input rows: `title` is the input's name, tokens included. */
constexpr std::array<HelpRow, 16> HelpKeys { {
	{ {}, "{camera}", "Move the view (arrows and the screen edge too)" },
	{ {}, "Wheel", "Zoom in and out, or scroll over the cards" },
	{ {}, "{entrance} / {vault}", "Jump to the entrance / the vault" },
	{ {}, "Left click", "Place what you picked, or select" },
	{ {}, "Right click", "Cancel what you picked, or send the selected monsters" },
	{ {}, "Shift + right", "Add a patrol point" },
	{ {}, "{tab}", "Next card tab" },
	{ {}, "{card1} - {card9}", "Pick a card on the current tab" },
	{ {}, "{spell1} {spell2} {spell3} {spell4} {spell5} {spell6}", "Cast a spell at the cursor" },
	{ {}, "{lock} / {bait}", "Pick a door lock / fool's gold" },
	{ {}, "{sell}", "Sell what is selected" },
	{ {}, "{possess}", "Possess or release a monster" },
	{ {}, "{raid}", "Open the gates: start the next raid now" },
	{ {}, "Esc", "Cancel, close, or the game menu" },
	{ {}, "Enter", "Chat, in co-op" },
	{ {}, "{help}", "This help" },
} };

struct HelpPage {
	std::string_view tab;
	std::span<const HelpRow> rows;
};
const std::array<HelpPage, 5> HelpPages { {
	{ "Goal", HelpGoal },
	{ "Building", HelpBuild },
	{ "Raids", HelpRaids },
	{ "Keeper", HelpKeeper },
	{ "Keys", HelpKeys },
} };

// Hover hints, in the order of HudArea.
constexpr std::array<std::pair<std::string_view, std::string_view>, 12> HudHints { {
	{ "Phase", "Preparation and Respite are for building. When the timer ends the gates open and a raid begins." },
	{ "Raids", "This raid and how many there are. Survive them all to clear the level." },
	{ "Pop", "The room your monsters take up, and all the room there is. It grows after each raid. Sell monsters to make room." },
	{ "Vault", "Your gold. It pays for everything you build this map, and it is what the heroes came for." },
	{ "Heroes and Loot", "Heroes alive in the keep, and the gold they are carrying. Kill a thief to get the gold back." },
	{ "Mana", "Pays for spells and possession. It refills over time." },
	{ "Open the gates", "Starts the next raid now instead of waiting for the timer. Press {raid}." },
	{ "Lairs", "Your monsters. Pick a card, then click the floor." },
	{ "Traps & Tricks", "Traps, door locks and fool's gold. Pick a card, then click the floor or a door. Traps take no Pop." },
	{ "Upgrades", "Click a card to buy it with gold, between raids. Upgrades last for this map." },
	{ "Help", "Controls and how to play. Press {help}." },
	{ "Scouts", "Who is coming in the next raid, and how the Keep Lord is." },
} };

constexpr std::string_view HideTipsLabel = "Hide all tips";
constexpr std::string_view TipsOnLabel = "Tips: on";
constexpr std::string_view TipsOffLabel = "Tips: off (click to replay)";
constexpr std::string_view HelpFooter = "{help} or Esc to close";

// Defeat: the first advice that applies comes first; two are shown.
constexpr std::string_view DefeatWhy = "The Keep Lord fell in raid {} to {}, and a hero got away through the portal.";
constexpr std::string_view AdviceNoMonsters = "You placed no monsters. Pick a Lairs card, then click the floor between the entrance and the Lord.";
constexpr std::string_view AdviceUnspent = "{} gold was still in the vault. Gold only fights once it is spent: build more before the gates open.";
constexpr std::string_view AdviceNoSpells = "You cast no spells. Point at a hero and press {spell1}. Stone holds a hero still while your monsters strike.";
constexpr std::string_view AdviceRenown = "You have {} Renown to spend. Unlock stronger monsters and spells on the Keeper screen.";
constexpr std::string_view AdviceNoTraps = "You set no traps, and they take no Pop. Put some on the heroes' path, and lock a door to slow them.";
constexpr std::string_view AdvicePortal = "When the Lord falls, one hero has to stand still to open the portal. Kill that hero and the fight goes on.";
constexpr std::string_view AdviceGather = "Move your monsters close together on one corridor, so the heroes meet them all at once.";
/** More unspent gold than this is worth mentioning (the cost of a couple of early monsters). */
constexpr int UnspentGoldWorthMentioning = 150;

// ---- icons -----------------------------------------------------------------------------------------

std::map<std::pair<int, int>, KindImage> iconImages;

std::optional<size_t> KindOfMonster(_monster_id type)
{
	for (size_t kind = 0; kind < NumLairKinds; kind++) {
		if (BuildKinds[kind].type == type)
			return kind;
	}
	return std::nullopt;
}

void DrawKindIcon(const Surface &area, size_t kind)
{
	const std::pair<int, int> key { static_cast<int>(kind), area.w() * 1000 + area.h() };
	auto it = iconImages.find(key);
	if (it == iconImages.end())
		it = iconImages.emplace(key, MakeKindImage(kind, area.w(), area.h())).first;
	DrawKindImage(area, it->second);
}

// ---- tips ------------------------------------------------------------------------------------------

constexpr int TipIconSize = 48;
constexpr int TipPadding = 6;
constexpr int TipLineHeight = 13;
constexpr int TipTitleHeight = 16;
constexpr int TipCloseSize = 14;

bool IsTipSeen(Tip tip)
{
	return (ProfileTipsSeen() & (1U << static_cast<unsigned>(tip))) != 0;
}

void MarkTipSeen(Tip tip)
{
	ProfileSetTipsSeen(ProfileTipsSeen() | (1U << static_cast<unsigned>(tip)));
}

void HideAllTips()
{
	ProfileSetTipsSeen(~0U);
}

const TipInfo *shownTip = nullptr;
int shownTicks = 0;
bool spellCastThisGame = false;
std::optional<Rectangle> tipRect;
std::optional<Rectangle> tipClose;
std::optional<Rectangle> tipHide;

bool AreTipsInScope()
{
	if (!IsActive() || IsEditorActive() || IsEditorTestRun() || IsAwaitingKeepSnapshot())
		return false;
	if (State.phase == Phase::Victory || State.phase == Phase::Defeat)
		return false;
	return CurrentSetup().mode == GameMode::Campaign || IsJoinedGame();
}

int CountActiveLairs()
{
	return static_cast<int>(std::count_if(State.lairs.begin(), State.lairs.end(), [](const Lair &lair) { return lair.active; }));
}

/** The player did it, seen or not: nothing left to teach. */
bool IsTipDone(const TipInfo &info)
{
	switch (info.tip) {
	case Tip::PlaceMonster:
		return CountActiveLairs() > 0;
	case Tip::OpenGates:
		return State.phase == Phase::Raid || State.wave > 1;
	case Tip::CastSpell:
		return spellCastThisGame;
	case Tip::Traps:
		return std::any_of(State.traps.begin(), State.traps.end(), [](const Trap &trap) { return trap.active; });
	case Tip::Rally:
		return std::any_of(State.lairs.begin(), State.lairs.end(), [](const Lair &lair) { return lair.active && (lair.hasRally || lair.routeCount > 0); });
	case Tip::Possess:
		return State.possessed[MyPlayerId] >= 0;
	default:
		return false;
	}
}

/** It is the moment for this tip. */
bool IsTipDue(const TipInfo &info)
{
	const bool building = State.phase == Phase::Setup || State.phase == Phase::Breather;
	const bool raiding = State.phase == Phase::Raid;
	switch (info.tip) {
	case Tip::PlaceMonster:
		return building;
	case Tip::OpenGates:
		return building && CountActiveLairs() > 0;
	case Tip::CastSpell:
		return raiding && LivingHeroCount() > 0;
	case Tip::Lord:
		return raiding && IsTipSeen(Tip::CastSpell);
	case Tip::Respite:
		return State.phase == Phase::Breather && !IsRaidSummaryVisible(); // the summary says it first
	case Tip::Traps:
		return building;
	case Tip::Rally:
		return building && CountActiveLairs() > 0;
	case Tip::Possess:
		return raiding && CountActiveLairs() > 0;
	default:
		return false;
	}
}

bool IsTipOnThisLevel(const TipInfo &info)
{
	if (IsJoinedGame())
		return true; // whatever level the host is on, this may be a joiner's first sight of the game
	const int level = CurrentLevelNumber();
	return level >= info.fromLevel && level <= 2;
}

Rectangle TargetRect(TipTarget target, const TipTargets &targets)
{
	switch (target) {
	case TipTarget::Cards:
		return targets.cards;
	case TipTarget::Tabs:
		return targets.tabs;
	case TipTarget::Ready:
		return targets.ready;
	case TipTarget::SpellBar:
		return targets.spellBar;
	case TipTarget::Roster:
		return targets.roster.value_or(Rectangle {});
	case TipTarget::None:
		break;
	}
	return {};
}

/** A gold frame that pulses round what the tip is about. */
void DrawPointer(const Surface &out, const Rectangle &rect)
{
	if (rect.size.width <= 0 || rect.size.height <= 0)
		return;
	const int pulse = (SDL_GetTicks() / 250) % 2;
	const Rectangle grown { rect.position + Displacement { -2 - pulse, -2 - pulse }, { rect.size.width + 4 + 2 * pulse, rect.size.height + 4 + 2 * pulse } };
	DrawFrame(out, grown, PAL16_YELLOW + 2);
	DrawFrame(out, { grown.position + Displacement { 1, 1 }, { grown.size.width - 2, grown.size.height - 2 } }, PAL16_YELLOW + 2);
}

UiIcon TipIcon(const TipInfo &info)
{
	switch (info.tip) {
	case Tip::PlaceMonster:
		return MusterKinds().empty() ? UiIcon {} : KindIcon(MusterKinds().front());
	case Tip::OpenGates:
		return ItemIcon(ICURS_LARGE_SHIELD);
	case Tip::CastSpell:
		return SpellIconOf(SpellID::Nova);
	case Tip::Lord:
		return MonsterIcon(MT_NBLACK);
	case Tip::Respite:
		return ItemIcon(ICURS_GOLD_LARGE);
	case Tip::Traps:
		return SpellIconOf(SpellID::RuneOfFire);
	case Tip::Rally:
		return MusterKinds().empty() ? UiIcon {} : KindIcon(MusterKinds().front());
	case Tip::Possess:
		return SpellIconOf(SpellID::Berserk);
	default:
		return {};
	}
}

/** How much room the tip's words take at `textWidth`: the title wraps too, clear of the close button. */
int TipHeight(std::string_view title, std::string_view text, int textWidth)
{
	const int titleLines = CountLinesOfText(WordWrapString(title, textWidth - TipCloseSize - 2));
	const int lines = titleLines + CountLinesOfText(WordWrapString(text, textWidth));
	const int textHeight = lines * TipLineHeight + 3 + TipLineHeight + 2; // the last line: the links
	return std::max(TipIconSize, textHeight) + 2 * TipPadding;
}

/** Draws a tip's box. The close and hide links are left in tipClose and tipHide. */
void DrawTipBox(const Surface &out, const Rectangle &rect, std::string_view title, const std::string &text, const UiIcon &icon, Point mouse)
{
	DrawDungeonPanel(out, rect);
	const Rectangle image { rect.position + Displacement { TipPadding, TipPadding }, { TipIconSize, TipIconSize } };
	FillRect(out, image.position.x, image.position.y, image.size.width, image.size.height, 0);
	DrawUiIcon(out.subregion(image.position.x, image.position.y, image.size.width, image.size.height), icon);
	DrawFrame(out, image, PAL16_YELLOW + 8);

	const int textX = image.position.x + TipIconSize + TipPadding;
	const int textWidth = rect.position.x + rect.size.width - TipPadding - textX;
	const std::string wrappedTitle = WordWrapString(title, textWidth - TipCloseSize - 2);
	const int titleHeight = CountLinesOfText(wrappedTitle) * TipLineHeight;
	DrawString(out, wrappedTitle, { { textX, rect.position.y + TipPadding - 1 }, { textWidth - TipCloseSize - 2, titleHeight + 2 } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorGold, .lineHeight = TipLineHeight });
	DrawString(out, WordWrapString(text, textWidth), { { textX, rect.position.y + TipPadding + titleHeight + 2 }, { textWidth, rect.size.height - TipPadding - titleHeight } },
	    { .flags = UiFlags::FontSize12 | UiFlags::ColorWhite, .lineHeight = TipLineHeight });

	const Rectangle close { { rect.position.x + rect.size.width - TipCloseSize - 3, rect.position.y + 3 }, { TipCloseSize, TipCloseSize } };
	tipClose = close;
	DrawFrame(out, close, close.contains(mouse) ? PAL16_YELLOW + 2 : PAL16_GRAY + 8);
	DrawString(out, "x", { close.position + Displacement { 0, 0 }, { TipCloseSize, TipCloseSize } }, { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | (close.contains(mouse) ? UiFlags::ColorGold : UiFlags::ColorWhite) });

	const int hideWidth = GetLineWidth(HideTipsLabel) + 4;
	const Rectangle hide { { rect.position.x + rect.size.width - hideWidth - TipPadding, rect.position.y + rect.size.height - TipLineHeight - 3 }, { hideWidth, TipLineHeight } };
	tipHide = hide;
	DrawString(out, HideTipsLabel, hide, { .flags = UiFlags::FontSize12 | UiFlags::AlignRight | (hide.contains(mouse) ? UiFlags::ColorGold : UiFlags::ColorWhite) });
}

// ---- help ------------------------------------------------------------------------------------------

bool helpOpen = false;
/** This client paused the game for the help (single player only). */
bool helpPaused = false;
size_t helpPage = 0;
int helpSubPage = 0;
int helpSubPageCount = 1;
std::optional<Rectangle> helpTipsButton;
std::optional<Rectangle> helpCloseButton;
std::optional<Rectangle> helpPrevButton;
std::optional<Rectangle> helpNextButton;
std::array<Rectangle, HelpPages.size()> helpTabs {};

constexpr int HelpMaxWidth = 640;
constexpr int HelpMaxHeight = 520;
constexpr int HelpMargin = 8;
constexpr int HelpTabHeight = 22;
constexpr int HelpIconSize = 56;
constexpr int HelpRowGap = 6;
constexpr int HelpKeyColumn = 150;
constexpr int HelpKeyRowHeight = 16;
constexpr int HelpFooterHeight = 26;

Rectangle HelpRect(const Surface &out)
{
	const int width = std::min(out.w() - 2 * HelpMargin, HelpMaxWidth);
	const int height = std::min(out.h() - 2 * HelpMargin, HelpMaxHeight);
	return { { (out.w() - width) / 2, (out.h() - height) / 2 }, { width, height } };
}

int HelpRowHeight(const HelpRow &row, int width)
{
	if (row.icon.type == UiIcon::Type::None)
		return HelpKeyRowHeight;
	const int textWidth = width - HelpIconSize - HelpMargin;
	const int lines = CountLinesOfText(WordWrapString(ExpandInputs(row.text), textWidth));
	return std::max(HelpIconSize, TipTitleHeight + lines * TipLineHeight) + HelpRowGap;
}

void DrawHelpRow(const Surface &out, const HelpRow &row, int x, int y, int width)
{
	if (row.icon.type == UiIcon::Type::None) {
		const std::string keys = FitText(ExpandInputs(row.title), HelpKeyColumn - 4);
		DrawString(out, keys, { { x, y }, { HelpKeyColumn, HelpKeyRowHeight } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorGold });
		DrawString(out, FitText(ExpandInputs(row.text), width - HelpKeyColumn), { { x + HelpKeyColumn, y }, { width - HelpKeyColumn, HelpKeyRowHeight } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorWhite });
		return;
	}
	const Rectangle image { { x, y }, { HelpIconSize, HelpIconSize } };
	FillRect(out, image.position.x, image.position.y, image.size.width, image.size.height, 0);
	DrawUiIcon(out.subregion(image.position.x, image.position.y, image.size.width, image.size.height), row.icon);
	DrawFrame(out, image, PAL16_YELLOW + 8);
	const int textX = x + HelpIconSize + HelpMargin;
	const int textWidth = width - HelpIconSize - HelpMargin;
	DrawString(out, ExpandInputs(row.title), { { textX, y }, { textWidth, TipTitleHeight } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorGold });
	DrawString(out, WordWrapString(ExpandInputs(row.text), textWidth), { { textX, y + TipTitleHeight }, { textWidth, HelpIconSize + 40 } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorWhite, .lineHeight = TipLineHeight });
}

/** Splits a page's rows over as many screens as they need at this height. @return each screen's first row. */
std::vector<size_t> HelpBreaks(const HelpPage &page, int width, int height)
{
	std::vector<size_t> breaks { 0 };
	int used = 0;
	for (size_t i = 0; i < page.rows.size(); i++) {
		const int rowHeight = HelpRowHeight(page.rows[i], width);
		if (used > 0 && used + rowHeight > height) {
			breaks.push_back(i);
			used = 0;
		}
		used += rowHeight;
	}
	return breaks;
}

void DrawHelpButton(const Surface &out, const Rectangle &rect, std::string_view label, bool enabled, Point mouse)
{
	FillRect(out, rect.position.x, rect.position.y, rect.size.width, rect.size.height, enabled ? PAL16_RED + 12 : PAL16_GRAY + 13);
	DrawFrame(out, rect, enabled && rect.contains(mouse) ? PAL16_YELLOW + 2 : PAL16_GRAY + 9);
	DrawString(out, FitText(label, rect.size.width - 4), { rect.position + Displacement { 0, (rect.size.height - 14) / 2 }, { rect.size.width, 14 } },
	    { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | (enabled ? UiFlags::ColorGold : UiFlags::ColorBlack) });
}

} // namespace

// ---- input names ----------------------------------------------------------------------------------

std::string InputLabel(KeeperAction action, size_t index)
{
	return KeyLabel(action, index);
}

std::string ExpandInputs(std::string_view text)
{
	std::string out;
	size_t at = 0;
	while (at < text.size()) {
		const size_t open = text.find('{', at);
		if (open == std::string_view::npos) {
			out += text.substr(at);
			break;
		}
		out += text.substr(at, open - at);
		const size_t close = text.find('}', open);
		if (close == std::string_view::npos) {
			out += text.substr(open);
			break;
		}
		const std::string_view token = text.substr(open + 1, close - open - 1);
		bool known = false;
		for (const InputToken &entry : InputTokens) {
			if (entry.token == token) {
				out += InputLabel(entry.action, entry.index);
				known = true;
				break;
			}
		}
		if (!known) {
			if (token == "camera") {
				out += InputLabel(KeeperAction::CameraUp) + InputLabel(KeeperAction::CameraLeft) + InputLabel(KeeperAction::CameraDown) + InputLabel(KeeperAction::CameraRight);
			} else if (token == "help") {
				out += InputLabel(KeeperAction::Help);
			} else if (token.size() == 5 && token.substr(0, 4) == "card") {
				out += InputLabel(KeeperAction::CardSlot, static_cast<size_t>(token[4] - '1'));
			} else {
				out += text.substr(open, close - open + 1);
			}
		}
		at = close + 1;
	}
	return out;
}

void DrawUiIcon(const Surface &area, const UiIcon &icon)
{
	switch (icon.type) {
	case UiIcon::Type::None:
		return;
	case UiIcon::Type::Spell:
		SetSpellTrans(SpellType::Spell);
		DrawLargeSpellIcon(area, { (area.w() - SPLICONLENGTH) / 2, (area.h() + SPLICONLENGTH) / 2 }, static_cast<SpellID>(icon.id));
		return;
	case UiIcon::Type::Item: {
		const ClxSprite sprite = GetInvItemSprite(CURSOR_FIRSTITEM + icon.id);
		if (sprite.width() <= area.w() && sprite.height() <= area.h()) {
			ClxDraw(area, { (area.w() - sprite.width()) / 2, (area.h() + sprite.height()) / 2 }, sprite);
			return;
		}
		// Long weapons don't fit a small box: shrink them, keeping their shape.
		OwnedSurface canvas(sprite.width(), sprite.height());
		for (int y = 0; y < canvas.h(); y++)
			std::memset(canvas.at(0, y), 0, canvas.w());
		ClxDraw(canvas, { 0, canvas.h() }, sprite);
		const int outHeight = std::min(area.h(), sprite.height() * area.w() / sprite.width());
		const int outWidth = std::min(area.w(), sprite.width() * area.h() / sprite.height());
		const int left = (area.w() - outWidth) / 2;
		const int top = (area.h() - outHeight) / 2;
		for (int y = 0; y < outHeight; y++) {
			const uint8_t *row = canvas.at(0, y * canvas.h() / outHeight);
			uint8_t *dst = area.at(left, top + y);
			for (int x = 0; x < outWidth; x++) {
				const uint8_t pixel = row[x * canvas.w() / outWidth];
				if (pixel != 0)
					dst[x] = pixel;
			}
		}
		return;
	}
	case UiIcon::Type::Monster:
		if (const std::optional<size_t> kind = KindOfMonster(static_cast<_monster_id>(icon.id)); kind)
			DrawKindIcon(area, *kind);
		return;
	case UiIcon::Type::Kind:
		DrawKindIcon(area, static_cast<size_t>(icon.id));
		return;
	}
}

// ---- tips ------------------------------------------------------------------------------------------

void TutorialOnLevelLoaded()
{
	shownTip = nullptr;
	shownTicks = 0;
	spellCastThisGame = false;
	tipRect = std::nullopt;
}

void TutorialNoteSpellCast()
{
	spellCastThisGame = true;
}

void TutorialTick()
{
	if (!AreTipsInScope()) {
		shownTip = nullptr;
		tipRect = std::nullopt;
		return;
	}
	for (const TipInfo &info : GameTips) {
		if (!IsTipSeen(info.tip) && IsTipOnThisLevel(info) && IsTipDone(info))
			MarkTipSeen(info.tip);
	}
	if (shownTip != nullptr) {
		shownTicks++;
		const bool timedOut = shownTip->seconds > 0 && shownTicks > shownTip->seconds * 20;
		if (timedOut)
			MarkTipSeen(shownTip->tip);
		if (timedOut || IsTipSeen(shownTip->tip) || !IsTipDue(*shownTip))
			shownTip = nullptr;
	}
	if (shownTip != nullptr)
		return;
	tipRect = std::nullopt;
	for (const TipInfo &info : GameTips) {
		if (!IsTipSeen(info.tip) && IsTipOnThisLevel(info) && IsTipDue(info)) {
			shownTip = &info;
			shownTicks = 0;
			return;
		}
	}
}

void DrawTutorialTip(const Surface &out, int left, int width, int y, Point mouse, const TipTargets &targets)
{
	tipRect = std::nullopt;
	if (shownTip == nullptr)
		return;
	const std::string text = ExpandInputs(shownTip->text);
	const int textWidth = width - TipIconSize - 3 * TipPadding;
	const Rectangle rect { { left, y }, { width, TipHeight(shownTip->title, text, textWidth) } };
	tipRect = rect;
	DrawPointer(out, TargetRect(shownTip->target, targets));
	DrawTipBox(out, rect, shownTip->title, text, TipIcon(*shownTip), mouse);
}

bool TutorialIsMouseOver(Point mouse)
{
	return tipRect && tipRect->contains(mouse);
}

bool TutorialOnClick(Point mouse)
{
	if (!TutorialIsMouseOver(mouse))
		return false;
	if (tipHide && tipHide->contains(mouse)) {
		HideAllTips();
		shownTip = nullptr;
		tipRect = std::nullopt;
	} else if (tipClose && tipClose->contains(mouse) && shownTip != nullptr) {
		MarkTipSeen(shownTip->tip);
		shownTip = nullptr;
		tipRect = std::nullopt;
	}
	return true;
}

bool TutorialOnEscape()
{
	if (shownTip == nullptr)
		return false;
	MarkTipSeen(shownTip->tip);
	shownTip = nullptr;
	tipRect = std::nullopt;
	return true;
}

bool DrawKeeperScreenTip(const Surface &out, const Rectangle &area, Point mouse, bool clicked)
{
	tipRect = std::nullopt;
	if (IsTipSeen(Tip::Renown))
		return false;
	const int renown = RenownAvailable();
	static int renownWhenShown = -1;
	if (renownWhenShown >= 0 && renown < renownWhenShown) {
		// Something was bought: the tip has done its job.
		MarkTipSeen(Tip::Renown);
		renownWhenShown = -1;
		return false;
	}
	if (renown <= 0)
		return false;
	renownWhenShown = renown;
	const std::string text = fmt::format(fmt::runtime(RenownTipText), renown);
	const int width = std::min(area.size.width, 420);
	const int height = TipHeight(RenownTipTitle, text, width - TipIconSize - 3 * TipPadding);
	const Rectangle rect { { area.position.x + (area.size.width - width) / 2, area.position.y + area.size.height - height - 2 }, { width, height } };
	tipRect = rect;
	// Not an item picture: those aren't loaded outside the game.
	DrawTipBox(out, rect, RenownTipTitle, text, MonsterIcon(MT_NBLACK), mouse);
	if (!clicked || !rect.contains(mouse))
		return false;
	if (tipHide && tipHide->contains(mouse))
		HideAllTips();
	else if (tipClose && tipClose->contains(mouse))
		MarkTipSeen(Tip::Renown);
	return true;
}

// ---- help ------------------------------------------------------------------------------------------

bool IsHelpOpen()
{
	return helpOpen;
}

void CloseHelp()
{
	if (!helpOpen)
		return;
	helpOpen = false;
	if (helpPaused)
		PauseMode = 0;
	helpPaused = false;
}

void ToggleHelp()
{
	if (helpOpen) {
		CloseHelp();
		return;
	}
	helpOpen = true;
	helpSubPage = 0;
	// Alone, reading costs nothing. In co-op the raid goes on without you.
	if (!gbIsMultiplayer && PauseMode == 0) {
		PauseMode = 2;
		helpPaused = true;
	}
}

void DrawHelp(const Surface &out, Point mouse)
{
	if (!helpOpen)
		return;
	const Rectangle rect = HelpRect(out);
	DrawDungeonPanel(out, rect);

	const int inner = rect.size.width - 2 * HelpMargin;
	const int x = rect.position.x + HelpMargin;
	int y = rect.position.y + HelpMargin;
	const int tabWidth = inner / static_cast<int>(HelpPages.size());
	for (size_t i = 0; i < HelpPages.size(); i++) {
		const Rectangle tab { { x + static_cast<int>(i) * tabWidth, y }, { tabWidth - 3, HelpTabHeight } };
		helpTabs[i] = tab;
		const bool active = i == helpPage;
		FillRect(out, tab.position.x, tab.position.y, tab.size.width, tab.size.height, active ? PAL16_RED + 12 : PAL16_GRAY + 13);
		DrawFrame(out, tab, active ? PAL16_YELLOW + 4 : (tab.contains(mouse) ? PAL16_GRAY : PAL16_GRAY + 9));
		DrawString(out, FitText(HelpPages[i].tab, tab.size.width - 4), { tab.position + Displacement { 0, 4 }, { tab.size.width, 14 } },
		    { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | (active ? UiFlags::ColorGold : UiFlags::ColorWhite) });
	}
	y += HelpTabHeight + HelpMargin;

	const int footerTop = rect.position.y + rect.size.height - HelpFooterHeight - HelpMargin;
	const HelpPage &page = HelpPages[helpPage];
	const std::vector<size_t> breaks = HelpBreaks(page, inner, footerTop - y);
	helpSubPageCount = static_cast<int>(breaks.size());
	helpSubPage = std::clamp(helpSubPage, 0, helpSubPageCount - 1);
	const size_t first = breaks[helpSubPage];
	const size_t last = helpSubPage + 1 < helpSubPageCount ? breaks[helpSubPage + 1] : page.rows.size();
	for (size_t i = first; i < last; i++) {
		DrawHelpRow(out, page.rows[i], x, y, inner);
		y += HelpRowHeight(page.rows[i], inner);
	}

	// Footer: tips switch, page arrows if the rows ran over, close.
	const int footerHeight = HelpFooterHeight - 4;
	const bool tipsHidden = ProfileTipsSeen() == ~0U;
	const std::string_view tipsLabel = tipsHidden ? TipsOffLabel : TipsOnLabel;
	const Rectangle tipsButton { { x, footerTop + 2 }, { std::min(GetLineWidth(tipsLabel) + 16, inner / 2 - 40), footerHeight } };
	helpTipsButton = tipsButton;
	DrawHelpButton(out, tipsButton, tipsLabel, true, mouse);

	const std::string closeLabel = ExpandInputs(HelpFooter);
	const int closeWidth = std::min(GetLineWidth(closeLabel) + 16, inner / 2 - 40);
	const Rectangle closeButton { { x + inner - closeWidth, footerTop + 2 }, { closeWidth, footerHeight } };
	helpCloseButton = closeButton;
	DrawHelpButton(out, closeButton, closeLabel, true, mouse);

	helpPrevButton = std::nullopt;
	helpNextButton = std::nullopt;
	if (helpSubPageCount > 1) {
		const int middle = x + inner / 2;
		const Rectangle prev { { middle - 34, footerTop + 2 }, { 24, footerHeight } };
		const Rectangle next { { middle + 10, footerTop + 2 }, { 24, footerHeight } };
		helpPrevButton = prev;
		helpNextButton = next;
		DrawHelpButton(out, prev, "<", helpSubPage > 0, mouse);
		DrawHelpButton(out, next, ">", helpSubPage + 1 < helpSubPageCount, mouse);
	}
}

void HelpOnClick(Point mouse)
{
	for (size_t i = 0; i < HelpPages.size(); i++) {
		if (helpTabs[i].contains(mouse)) {
			helpPage = i;
			helpSubPage = 0;
			return;
		}
	}
	if (helpPrevButton && helpPrevButton->contains(mouse)) {
		HelpOnWheel(1);
	} else if (helpNextButton && helpNextButton->contains(mouse)) {
		HelpOnWheel(-1);
	} else if (helpCloseButton && helpCloseButton->contains(mouse)) {
		CloseHelp();
	} else if (helpTipsButton && helpTipsButton->contains(mouse)) {
		ProfileSetTipsSeen(ProfileTipsSeen() == ~0U ? 0U : ~0U);
	}
}

void HelpOnWheel(int delta)
{
	helpSubPage = std::clamp(helpSubPage + (delta > 0 ? -1 : 1), 0, std::max(helpSubPageCount - 1, 0));
}

// ---- hover hints -----------------------------------------------------------------------------------

HudHint DescribeHudArea(HudArea area)
{
	const auto &[title, text] = HudHints[static_cast<size_t>(area)];
	return { std::string(title), ExpandInputs(text) };
}

// ---- after a defeat ---------------------------------------------------------------------------------

DefeatAdvice AdviseAfterDefeat(uint8_t wave)
{
	DefeatAdvice advice;
	const std::string slayers = LordSlayers();
	advice.why = fmt::format(fmt::runtime(DefeatWhy), wave, slayers.empty() ? std::string("the heroes") : slayers);

	const auto add = [&](std::string text) {
		if (advice.tryNext.size() < 2)
			advice.tryNext.push_back(ExpandInputs(text));
	};
	if (CountActiveLairs() == 0)
		add(std::string(AdviceNoMonsters));
	if (State.vaultGold > UnspentGoldWorthMentioning)
		add(fmt::format(fmt::runtime(AdviceUnspent), State.vaultGold));
	if (SpellsCastThisMap() == 0)
		add(std::string(AdviceNoSpells));
	if (const int renown = RenownAvailable(); renown > 0)
		add(fmt::format(fmt::runtime(AdviceRenown), renown));
	if (std::none_of(State.traps.begin(), State.traps.end(), [](const Trap &trap) { return trap.active; }))
		add(std::string(AdviceNoTraps));
	add(std::string(AdvicePortal));
	add(std::string(AdviceGather));
	return advice;
}

std::vector<std::string> CheckTextFits()
{
	std::vector<std::string> problems;
	// The narrowest tip box: 640 wide, with both the masters list and the roster showing.
	constexpr int NarrowTipWidth = 230;
	// Room left on a 640x480 screen: under a banner and two alerts, above the spell bar.
	constexpr int MaxTipHeight = 150;
	for (const TipInfo &info : GameTips) {
		const std::string text = ExpandInputs(info.text);
		const int height = TipHeight(info.title, text, NarrowTipWidth - TipIconSize - 3 * TipPadding);
		if (height > MaxTipHeight)
			problems.push_back(fmt::format("tip '{}' is {} tall", info.title, height));
	}
	// The narrowest help: a 640x480 screen.
	const int width = 640 - 2 * HelpMargin - 2 * HelpMargin;
	const int height = 480 - 2 * HelpMargin - 3 * HelpMargin - HelpTabHeight - HelpFooterHeight;
	for (const HelpPage &page : HelpPages) {
		for (const HelpRow &row : page.rows) {
			if (HelpRowHeight(row, width) > height)
				problems.push_back(fmt::format("help row '{}' is too tall", row.title));
			if (row.icon.type != UiIcon::Type::None)
				continue;
			if (GetLineWidth(ExpandInputs(row.title)) > HelpKeyColumn - 4)
				problems.push_back(fmt::format("help keys '{}' get cut", row.title));
			if (GetLineWidth(ExpandInputs(row.text)) > width - HelpKeyColumn)
				problems.push_back(fmt::format("help text '{}' gets cut", row.text));
		}
		if (HelpBreaks(page, width, height).size() > 1)
			problems.push_back(fmt::format("note: help page '{}' splits at 640x480", page.tab));
	}
	return problems;
}

} // namespace devilution::keeper
