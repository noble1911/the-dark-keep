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
#include "keeper/keeper_map.hpp"
#include "keeper/keeper_game.hpp"
#include "keeper/keeper_profile.hpp"
#include "keeper/keeper_resume.hpp"
#include "keeper/keeper_roster.hpp"
#include "utils/ui_fwd.h"
#include "utils/utf8.hpp"

namespace devilution::keeper {

namespace {

constexpr int DescriptionWidth = 205;
constexpr size_t VisibleRows = 6;
constexpr int NothingChosen = -1;
/** The Continue row of the campaign list; levels and the Keeper row are small numbers. */
constexpr int ContinueRow = 1000;

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

/** What Continue would bring back, while the campaign list is up. */
std::optional<ResumeData> offeredResume;

void DescribeCampaignLevel(int level)
{
	if (level == ContinueRow && offeredResume) {
		const LevelRules &saved = CampaignLevelRules(offeredResume->level);
		SetLabel("Continue");
		SetDescription(fmt::format("{}\n\nRaid {} of {} is coming.\nVault: {} gold\nLairs: {}\n\nPicks up where you left the keep.", saved.name, offeredResume->wave, saved.raids, offeredResume->gold, offeredResume->lairs.size()));
		return;
	}
	const LevelRules &rules = CampaignLevelRules(level);
	SetLabel(fmt::format("Level {}", KeeperLevel()));
	if (level == 0) {
		SetDescription(fmt::format("Your character screen.\n\nRenown to spend: {}", RenownAvailable()));
		return;
	}
	const std::string status = level > NextOpenLevel() ? fmt::format("Locked: clear level {} first", level - 1) : (level <= LevelsCleared() ? "Cleared *" : "Not yet cleared");
	SetDescription(fmt::format("{}\n\nRaids: {}\nStarting gold: {}\nHero strength: {}%\n{}", rules.name, rules.raids, rules.startingGold, rules.heroStrengthPercent, status));
}

/** The files on the Custom Map screen. */
std::vector<std::string> customMaps;

/** Empty if it can be played; otherwise what's wrong with it, for the player. */
std::string CustomMapProblem(const std::string &path)
{
	const tl::expected<std::vector<std::byte>, std::string> bytes = ReadMapBytes(path);
	if (!bytes.has_value())
		return bytes.error();
	const tl::expected<void, std::string> usable = CheckMapBytes(bytes->data(), bytes->size());
	return usable.has_value() ? std::string() : usable.error();
}

void DescribeCustomMap(int value)
{
	SetLabel("Custom Map");
	const std::string path = MapsFolder() + customMaps[static_cast<size_t>(value)];
	std::string about;
	if (const tl::expected<std::vector<std::byte>, std::string> bytes = ReadMapBytes(path); bytes.has_value()) {
		if (const tl::expected<KeepMap, std::string> map = ParseMap(bytes->data(), bytes->size()); map.has_value())
			about = fmt::format("{}{}\n", MapTilesetName(map->tileset), map->author.empty() ? std::string() : fmt::format("\nBy {}", map->author));
	}
	const std::string problem = CustomMapProblem(path);
	SetDescription(fmt::format("{}Raids: {}\nStarting gold: {}{}", about, CustomMapRules().raids, CustomMapRules().startingGold, problem.empty() ? std::string() : fmt::format("\n\nCan't be played: {}", problem)));
}

void DescribeGameKind(int kind)
{
	SetLabel(fmt::format("Level {}", KeeperLevel()));
	if (kind == static_cast<int>(GameMode::Campaign))
		SetDescription("Defend a campaign level you have reached, with friends. Levels cleared here count for you, the host.");
	else
		SetDescription("Defend a map from your maps folder. It is sent to everyone who joins.");
}

} // namespace

namespace {

bool CampaignLevelSelect(bool offerContinue)
{
	const int cleared = LevelsCleared();
	const int rank = NextOpenLevel();
	std::vector<ScreenItem> items;
	ClearPendingResume();
	offeredResume = offerContinue ? ReadResumeFile() : std::nullopt;
	const size_t firstRows = offeredResume ? 1 : 0;
	if (offeredResume)
		items.push_back({ "Continue", ContinueRow, UiFlags::ColorUiGold });
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
	size_t initial = offeredResume ? 0 : static_cast<size_t>(rank);
	int level = NothingChosen;
	while (true) {
		level = RunListScreen("Campaign", "Select Level", items, initial, DescribeCampaignLevel);
		if (level != 0)
			break;
		UiKeeperScreen();
		initial = firstRows;
	}
	if (level == NothingChosen)
		return false;
	if (level == ContinueRow) {
		SetCurrentSetup(SetupForCampaignLevel(offeredResume->level));
		SetPendingResume(*offeredResume);
		return true;
	}
	SetCurrentSetup(SetupForCampaignLevel(level));
	return true;
}

} // namespace

bool UiCampaignLevelSelect()
{
	return CampaignLevelSelect(/*offerContinue=*/true);
}

bool UiCustomMapSelect()
{
	const std::string folder = MapsFolder();
	customMaps = ListMapFiles(/*includeScratch=*/false);
	const std::vector<std::string> &maps = customMaps;
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
	size_t initial = 0;
	while (true) {
		const int index = RunListScreen("Custom Map", "Select Map", items, initial, DescribeCustomMap);
		if (index == NothingChosen)
			return false;
		initial = static_cast<size_t>(index);
		const std::string path = folder + maps[initial];
		// Rather than quietly playing a random keep, or sending a joiner something they would refuse.
		if (const std::string problem = CustomMapProblem(path); !problem.empty()) {
			const std::string message = fmt::format("This map can't be played: {}.", problem);
			UiSelOkDialog("Custom Map", message.c_str(), false);
			continue;
		}
		SetCurrentSetup(SetupForCustomMap(path));
		return true;
	}
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
		if (kind == static_cast<int>(GameMode::Campaign) ? CampaignLevelSelect(/*offerContinue=*/false) : UiCustomMapSelect())
			return true;
	}
}

} // namespace devilution::keeper
