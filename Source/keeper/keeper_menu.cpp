/**
 * @file keeper/keeper_menu.cpp
 *
 * The master's menu screens, laid out like the game-creation screens in DiabloUI/multi/selgame.cpp.
 */
#include "keeper/keeper_menu.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifdef USE_SDL3
#include <SDL3/SDL_rect.h>
#else
#include <SDL.h>
#endif

#include <fmt/format.h>

#include "DiabloUI/diabloui.h"
#include "DiabloUI/scrollbar.h"
#include "DiabloUI/selok.h"
#include "DiabloUI/ui_flags.hpp"
#include "DiabloUI/ui_item.h"
#include "engine/point.hpp"
#include "engine/render/text_render.hpp"
#include "keeper/keeper_campaign.hpp"
#include "keeper/keeper_game.hpp"
#include "keeper/keeper_profile.hpp"
#include "keeper/keeper_roster.hpp"
#include "utils/ui_fwd.h"
#include "utils/utf8.hpp"

namespace devilution::keeper {

namespace {

constexpr int DescriptionWidth = 205;
constexpr size_t VisibleRows = 6;
constexpr int NothingChosen = -1;

struct ScreenItem {
	std::string text;
	int value;
	UiFlags flags;
	/** Locked rows stay reachable, so the player can read what lies ahead, but can't be chosen. */
	bool selectable = true;
};

using DescribeFn = void (*)(int value);

char screenLabel[32];
char screenDescription[512];
std::vector<std::unique_ptr<UiListItem>> listItems;
std::vector<std::unique_ptr<UiItemBase>> dialog;
std::vector<bool> rowSelectable;
DescribeFn describe = nullptr;
bool endScreen = false;
int chosenValue = NothingChosen;

void SetLabel(std::string_view text)
{
	CopyUtf8(screenLabel, text, sizeof(screenLabel));
}

void SetDescription(std::string_view text)
{
	CopyUtf8(screenDescription, WordWrapString(text, DescriptionWidth), sizeof(screenDescription));
}

void OnFocus(size_t index)
{
	if (describe != nullptr)
		describe(listItems[index]->m_value);
}

void OnSelect(size_t index)
{
	if (!rowSelectable[index])
		return;
	chosenValue = listItems[index]->m_value;
	endScreen = true;
}

void OnEsc()
{
	chosenValue = NothingChosen;
	endScreen = true;
}

/** Shows a list with a description panel. @return the chosen item's value, or NothingChosen. */
int RunListScreen(const char *title, const char *heading, const std::vector<ScreenItem> &items, size_t initial, DescribeFn describeFn)
{
	LoadBackgroundArt("ui_art\\selgame");
	LoadScrollBar();
	UiAddBackground(&dialog);
	UiAddLogo(&dialog);

	const Point ui = GetUIRectangle().position;

	const SDL_Rect scrollRect = { (Sint16)(ui.x + 590), (Sint16)(ui.y + 244), 25, 178 };
	dialog.push_back(std::make_unique<UiScrollbar>((*ArtScrollBarBackground)[0], (*ArtScrollBarThumb)[0], *ArtScrollBarArrow, scrollRect));

	const SDL_Rect titleRect = { (Sint16)(ui.x + 24), (Sint16)(ui.y + 161), 590, 35 };
	dialog.push_back(std::make_unique<UiArtText>(title, titleRect, UiFlags::AlignCenter | UiFlags::FontSize30 | UiFlags::ColorUiSilver, 3));

	const SDL_Rect labelRect = { (Sint16)(ui.x + 34), (Sint16)(ui.y + 211), 205, 33 };
	dialog.push_back(std::make_unique<UiArtText>(screenLabel, labelRect, UiFlags::AlignCenter | UiFlags::FontSize30 | UiFlags::ColorUiSilver, 3));

	const SDL_Rect descriptionRect = { (Sint16)(ui.x + 35), (Sint16)(ui.y + 256), DescriptionWidth, 192 };
	dialog.push_back(std::make_unique<UiArtText>(screenDescription, descriptionRect, UiFlags::FontSize12 | UiFlags::ColorUiSilverDark, 1, 16));

	const SDL_Rect headingRect = { (Sint16)(ui.x + 300), (Sint16)(ui.y + 211), 295, 33 };
	dialog.push_back(std::make_unique<UiArtText>(heading, headingRect, UiFlags::AlignCenter | UiFlags::FontSize30 | UiFlags::ColorUiSilver, 3));

	for (const ScreenItem &item : items) {
		listItems.push_back(std::make_unique<UiListItem>(std::string_view(item.text), item.value, item.flags));
		rowSelectable.push_back(item.selectable);
	}
	dialog.push_back(std::make_unique<UiList>(listItems, VisibleRows, ui.x + 305, ui.y + 255, 285, 26, UiFlags::AlignCenter | UiFlags::FontSize24));

	const SDL_Rect okRect = { (Sint16)(ui.x + 299), (Sint16)(ui.y + 427), 140, 35 };
	dialog.push_back(std::make_unique<UiArtTextButton>("OK", &UiFocusNavigationSelect, okRect, UiFlags::AlignCenter | UiFlags::VerticalCenter | UiFlags::FontSize30 | UiFlags::ColorUiGold));

	const SDL_Rect cancelRect = { (Sint16)(ui.x + 449), (Sint16)(ui.y + 427), 140, 35 };
	dialog.push_back(std::make_unique<UiArtTextButton>("CANCEL", &UiFocusNavigationEsc, cancelRect, UiFlags::AlignCenter | UiFlags::VerticalCenter | UiFlags::FontSize30 | UiFlags::ColorUiGold));

	describe = describeFn;
	chosenValue = NothingChosen;
	endScreen = false;
	UiInitList(OnFocus, OnSelect, OnEsc, dialog, true, nullptr, nullptr, initial);

	while (!endScreen) {
		UiClearScreen();
		UiPollAndRender();
	}

	UiInitList_clear();
	describe = nullptr;
	dialog.clear();
	listItems.clear();
	rowSelectable.clear();
	ArtBackground = std::nullopt;
	UnloadScrollBar();
	return chosenValue;
}

void DescribeCampaignLevel(int level)
{
	const LevelRules &rules = CampaignLevelRules(level);
	SetLabel(fmt::format("Level {}", KeeperLevel()));
	if (level == 0) {
		SetDescription(fmt::format("Your character screen.\n\nRenown to spend: {}", RenownAvailable()));
		return;
	}
	const std::string status = level > NextOpenLevel() ? fmt::format("Locked: clear level {} first", level - 1) : (level <= LevelsCleared() ? "Cleared *" : "Not yet cleared");
	SetDescription(fmt::format("{}\n\nRaids: {}\nStarting gold: {}\nHero strength: {}%\n{}", rules.name, rules.raids, rules.startingGold, rules.heroStrengthPercent, status));
}

void DescribeCustomMap(int /*value*/)
{
	SetLabel("Custom Map");
	SetDescription(fmt::format("Raids: {}\nStarting gold: {}", CustomMapRules().raids, CustomMapRules().startingGold));
}

void DescribeGameKind(int kind)
{
	SetLabel(fmt::format("Level {}", KeeperLevel()));
	if (kind == static_cast<int>(GameMode::Campaign))
		SetDescription("Defend a campaign level you have reached, with friends. Levels cleared here count for you, the host.");
	else
		SetDescription("Defend a map from your maps folder. It is sent to everyone who joins.");
}

// ---- the character screen ----------------------------------------------------------------------

std::string RenownLine()
{
	return fmt::format("Renown to spend: {}", RenownAvailable());
}

void DescribeKeeperRow(int row)
{
	SetLabel(fmt::format("Level {}", KeeperLevel()));
	const ProfileStats &stats = GetProfileStats();
	const int level = KeeperLevel();
	const std::string progress = fmt::format("XP {} / {}\n{}\n\nMaps cleared: {}\nMaps lost: {}\nRaids survived: {}\nHeroes slain: {}\nMonsters lost: {}",
	    ProfileXp(), XpForKeeperLevel(level + 1), RenownLine(), stats.mapsCleared, stats.mapsLost, stats.raidsSurvived, stats.heroesSlain, stats.monstersLost);
	(void)row;
	SetDescription(progress);
}

/** The Bestiary's rows, in unlock order. */
void DescribeKindRow(int kindValue)
{
	const auto kind = static_cast<size_t>(kindValue);
	SetLabel(fmt::format("Renown {}", RenownAvailable()));
	std::string status;
	std::string_view reason;
	if (OwnsKind(kind))
		status = "Unlocked";
	else if (CanUnlockKind(kind, &reason))
		status = fmt::format("Press OK to unlock: {} Renown", KindUnlockCost(kind));
	else
		status = fmt::format("{} Renown, keeper level {}\n{}", KindUnlockCost(kind), UnlockRankOf(kind), reason);
	SetDescription(fmt::format("{}\n{}\nPop {}  Life {}  Hit {}-{}  Gold {}\n\n{}\n\n{}", BuildKinds[kind].name, FamilyName(FamilyOfKind(kind)),
	    BuildKinds[kind].supply, KindLife(kind), KindMinDamage(kind), KindMaxDamage(kind), BuildKinds[kind].cost, KindFlavor(kind), status));
}

void RunBestiary()
{
	size_t initial = 0;
	while (true) {
		std::vector<ScreenItem> items;
		const std::vector<size_t> &kinds = BestiaryKinds();
		for (const size_t kind : kinds) {
			const UiFlags color = OwnsKind(kind) ? UiFlags::ColorUiSilver : (CanUnlockKind(kind) ? UiFlags::ColorUiGold : UiFlags::ColorUiSilverDark);
			items.push_back({ std::string(BuildKinds[kind].name) + (OwnsKind(kind) ? " *" : ""), static_cast<int>(kind), color });
		}
		const int chosen = RunListScreen("Bestiary", "Monsters", items, initial, DescribeKindRow);
		if (chosen == NothingChosen)
			return;
		const auto it = std::find(kinds.begin(), kinds.end(), static_cast<size_t>(chosen));
		initial = static_cast<size_t>(it - kinds.begin());
		UnlockKind(static_cast<size_t>(chosen));
	}
}

std::string RaiseStatus(int level, int cost, int unlockRank, bool canRaise, std::string_view reason)
{
	if (canRaise)
		return fmt::format("Press OK to {}: {} Renown", level == 0 ? "unlock" : "level up", cost);
	if (level >= MaxKeepLevel)
		return std::string(reason);
	return fmt::format("{} Renown{}\n{}", cost, level == 0 ? fmt::format(", keeper level {}", unlockRank) : std::string(), reason);
}

void DescribeSpellRow(int value)
{
	const auto spell = static_cast<size_t>(value);
	const SpellKind &kind = SpellKinds[spell];
	const int level = OwnedSpellLevel(spell);
	SetLabel(fmt::format("Renown {}", RenownAvailable()));
	std::string_view reason;
	const bool canRaise = CanRaiseSpell(spell, &reason);
	SetDescription(fmt::format("{}{}\nMana {}\n\n{}\n\n{}", kind.name, level > 0 ? fmt::format(", level {}", level) : std::string(), SpellManaCostAt(spell, level), kind.flavor,
	    RaiseStatus(level, SpellNextCost(spell), kind.unlockRank, canRaise, reason)));
}

void RunSpellbook()
{
	size_t initial = 0;
	while (true) {
		std::vector<ScreenItem> items;
		for (size_t spell = 0; spell < NumSpellKinds; spell++) {
			const int level = OwnedSpellLevel(spell);
			const UiFlags color = CanRaiseSpell(spell) ? UiFlags::ColorUiGold : (level > 0 ? UiFlags::ColorUiSilver : UiFlags::ColorUiSilverDark);
			items.push_back({ level > 0 ? fmt::format("{}  Lv {}", SpellKinds[spell].name, level) : std::string(SpellKinds[spell].name), static_cast<int>(spell), color });
		}
		const int chosen = RunListScreen("Spellbook", "Spells", items, initial, DescribeSpellRow);
		if (chosen == NothingChosen)
			return;
		initial = static_cast<size_t>(chosen);
		const bool wasOwned = OwnedSpellLevel(initial) > 0;
		if (RaiseSpell(initial) && !wasOwned)
			ToggleProfileSpell(initial); // onto the bar, if there is room
	}
}

void DescribeTrapRow(int value)
{
	const auto trap = static_cast<size_t>(value);
	const TrapKind &kind = TrapKinds[trap];
	const int level = OwnedTrapLevel(trap);
	SetLabel(fmt::format("Renown {}", RenownAvailable()));
	std::string_view reason;
	const bool canRaise = CanRaiseTrap(trap, &reason);
	SetDescription(fmt::format("{}{}\nGold {}\n\n{}\n\n{}", kind.name, level > 0 ? fmt::format(", level {}", level) : std::string(), kind.cost, kind.flavor,
	    RaiseStatus(level, TrapNextCost(trap), kind.unlockRank, canRaise, reason)));
}

void RunTraps()
{
	size_t initial = 0;
	while (true) {
		std::vector<ScreenItem> items;
		for (size_t trap = 0; trap < NumTrapKinds; trap++) {
			const int level = OwnedTrapLevel(trap);
			const UiFlags color = CanRaiseTrap(trap) ? UiFlags::ColorUiGold : (level > 0 ? UiFlags::ColorUiSilver : UiFlags::ColorUiSilverDark);
			items.push_back({ level > 0 ? fmt::format("{}  Lv {}", TrapKinds[trap].name, level) : std::string(TrapKinds[trap].name), static_cast<int>(trap), color });
		}
		const int chosen = RunListScreen("Traps", "Trap Types", items, initial, DescribeTrapRow);
		if (chosen == NothingChosen)
			return;
		initial = static_cast<size_t>(chosen);
		RaiseTrap(initial);
	}
}

bool IsOnSpellBar(size_t spell)
{
	const std::array<int8_t, NumSpellSlots> &bar = ProfileSpellBar();
	return std::find(bar.begin(), bar.end(), static_cast<int8_t>(spell)) != bar.end();
}

void DescribeBarRow(int value)
{
	const auto spell = static_cast<size_t>(value);
	const std::array<int8_t, NumSpellSlots> &bar = ProfileSpellBar();
	const auto used = std::count_if(bar.begin(), bar.end(), [](int8_t slot) { return slot >= 0; });
	SetLabel(fmt::format("{} / {}", used, NumSpellSlots));
	SetDescription(fmt::format("{}, level {}\n\n{}\n\nPress OK to {} your spell bar.", SpellKinds[spell].name, OwnedSpellLevel(spell), SpellKinds[spell].flavor, IsOnSpellBar(spell) ? "take it off" : "put it on"));
}

void RunSpellBar()
{
	size_t initial = 0;
	while (true) {
		std::vector<ScreenItem> items;
		for (size_t spell = 0; spell < NumSpellKinds; spell++) {
			if (OwnedSpellLevel(spell) == 0)
				continue;
			const bool on = IsOnSpellBar(spell);
			items.push_back({ std::string(SpellKinds[spell].name) + (on ? " *" : ""), static_cast<int>(spell), on ? UiFlags::ColorUiGold : UiFlags::ColorUiSilver });
		}
		const int chosen = RunListScreen("Spell Bar", "Choose Spells", items, initial, DescribeBarRow);
		if (chosen == NothingChosen)
			return;
		for (size_t i = 0; i < items.size(); i++) {
			if (items[i].value == chosen)
				initial = i;
		}
		if (!ToggleProfileSpell(static_cast<size_t>(chosen)))
			UiSelOkDialog("Spell Bar", "Your spell bar is full. Take a spell off first.", false);
	}
}

void DescribeRespec(int value)
{
	SetLabel(fmt::format("Renown {}", RenownAvailable()));
	SetDescription(value == 1 ? "Gives back all your Renown. Your starting monsters, spells and traps stay; everything else locks again and spells return to level 1. It's free, and you keep your XP." : "Keeps everything as it is.");
}

void RunRespec()
{
	const std::vector<ScreenItem> items {
		{ "No", 0, UiFlags::ColorUiGold },
		{ "Yes, respec", 1, UiFlags::ColorUiGold },
	};
	if (RunListScreen("Respec", "Are you sure?", items, 0, DescribeRespec) == 1)
		RespecProfile();
}

} // namespace

void UiKeeperScreen()
{
	enum Row : int {
		BestiaryRow,
		SpellbookRow,
		TrapsRow,
		SpellBarRow,
		RespecRow,
	};
	size_t initial = 0;
	while (true) {
		size_t ownedKinds = 0;
		for (const size_t kind : BestiaryKinds()) {
			if (OwnsKind(kind))
				ownedKinds++;
		}
		size_t ownedSpells = 0;
		for (size_t spell = 0; spell < NumSpellKinds; spell++)
			ownedSpells += OwnedSpellLevel(spell) > 0 ? 1 : 0;
		size_t ownedTraps = 0;
		for (size_t trap = 0; trap < NumTrapKinds; trap++)
			ownedTraps += OwnedTrapLevel(trap) > 0 ? 1 : 0;
		const std::vector<ScreenItem> items {
			{ fmt::format("Bestiary  {}/{}", ownedKinds, BestiaryKinds().size()), BestiaryRow, UiFlags::ColorUiGold },
			{ fmt::format("Spellbook  {}/{}", ownedSpells, NumSpellKinds), SpellbookRow, UiFlags::ColorUiGold },
			{ fmt::format("Traps  {}/{}", ownedTraps, NumTrapKinds), TrapsRow, UiFlags::ColorUiGold },
			{ "Spell Bar", SpellBarRow, UiFlags::ColorUiGold },
			{ "Respec", RespecRow, UiFlags::ColorUiSilver },
		};
		const int row = RunListScreen("Keeper", "Keeper", items, initial, DescribeKeeperRow);
		if (row == NothingChosen)
			return;
		initial = static_cast<size_t>(row);
		switch (row) {
		case BestiaryRow:
			RunBestiary();
			break;
		case SpellbookRow:
			RunSpellbook();
			break;
		case TrapsRow:
			RunTraps();
			break;
		case SpellBarRow:
			RunSpellBar();
			break;
		case RespecRow:
			RunRespec();
			break;
		}
	}
}

bool UiCampaignLevelSelect()
{
	const int cleared = LevelsCleared();
	const int rank = NextOpenLevel();
	std::vector<ScreenItem> items;
	items.push_back({ "Keeper", 0, UiFlags::ColorUiSilver });
	for (int level = 1; level <= NumCampaignLevels; level++) {
		const std::string_view name = CampaignLevelRules(level).name;
		if (level > rank)
			items.push_back({ fmt::format("{}. {}", level, name), level, UiFlags::ColorUiSilverDark, false });
		else if (level <= cleared)
			items.push_back({ fmt::format("{}. {} *", level, name), level, UiFlags::ColorUiSilver });
		else
			items.push_back({ fmt::format("{}. {}", level, name), level, UiFlags::ColorUiGold });
	}
	size_t initial = static_cast<size_t>(rank);
	int level = NothingChosen;
	while (true) {
		level = RunListScreen("Campaign", "Select Level", items, initial, DescribeCampaignLevel);
		if (level != 0)
			break;
		UiKeeperScreen();
		initial = 0;
	}
	if (level == NothingChosen)
		return false;
	SetCurrentSetup(SetupForCampaignLevel(level));
	return true;
}

bool UiCustomMapSelect()
{
	const std::string folder = MapsFolder();
	const std::vector<std::string> maps = ListMapFiles(/*includeScratch=*/false);
	if (maps.empty()) {
		const std::string message = fmt::format("No maps found. Put .dkmap files in {}", folder);
		UiSelOkDialog("Custom Map", message.c_str(), false);
		return false;
	}
	std::vector<ScreenItem> items;
	for (size_t i = 0; i < maps.size(); i++) {
		const std::string_view file = maps[i];
		items.push_back({ std::string(file.substr(0, file.rfind('.'))), static_cast<int>(i), UiFlags::ColorUiGold });
	}
	const int index = RunListScreen("Custom Map", "Select Map", items, 0, DescribeCustomMap);
	if (index == NothingChosen)
		return false;
	SetCurrentSetup(SetupForCustomMap(folder + maps[static_cast<size_t>(index)]));
	return true;
}

bool UiHostGameSelect()
{
	const std::vector<ScreenItem> kinds {
		{ "Campaign", static_cast<int>(GameMode::Campaign), UiFlags::ColorUiGold },
		{ "Custom Map", static_cast<int>(GameMode::CustomMap), UiFlags::ColorUiGold },
	};
	size_t initial = 0;
	while (true) {
		const int kind = RunListScreen("Create Game", "Select Game", kinds, initial, DescribeGameKind);
		if (kind == NothingChosen)
			return false;
		initial = static_cast<size_t>(kind);
		if (kind == static_cast<int>(GameMode::Campaign) ? UiCampaignLevelSelect() : UiCustomMapSelect())
			return true;
	}
}

} // namespace devilution::keeper
