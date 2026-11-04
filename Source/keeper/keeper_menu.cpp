/**
 * @file keeper/keeper_menu.cpp
 *
 * The master's menu screens, laid out like the game-creation screens in DiabloUI/multi/selgame.cpp.
 */
#include "keeper/keeper_menu.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
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
#include "keeper/keeper_goals.hpp"
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
	/** Stars earned, shown at the row's end out of NumLevelGoals. Negative: the row has no stars. */
	int stars = -1;
};

/** Where a screen's list sits. The default suits centred rows; the campaign's need room for their stars too. */
struct ListLayout {
	int x = 305;
	int width = 285;
	UiFlags flags = UiFlags::AlignCenter;
	int spacing = 1;
};

/** U+2605 and U+2606, drawn into the small font by scripts/dev/draw_star_glyphs.py: only that size has them. */
constexpr std::string_view FilledStar = "\xE2\x98\x85";
constexpr std::string_view HollowStar = "\xE2\x98\x86";
constexpr std::string_view FilledStars = "\xE2\x98\x85\xE2\x98\x85\xE2\x98\x85";
constexpr std::string_view HollowStars = "\xE2\x98\x86\xE2\x98\x86\xE2\x98\x86";
static_assert(NumLevelGoals * FilledStar.size() == FilledStars.size());
static_assert(NumLevelGoals * HollowStar.size() == HollowStars.size());

/** The first `count` stars of a run of them. */
std::string_view StarRun(std::string_view stars, size_t count)
{
	return stars.substr(0, count * FilledStar.size());
}

int CountStars(uint8_t medals)
{
	int count = 0;
	for (size_t goal = 0; goal < NumLevelGoals; goal++)
		count += (medals >> goal) & 1;
	return count;
}

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
int RunListScreen(const char *title, const char *heading, const std::vector<ScreenItem> &items, size_t initial, DescribeFn describeFn, const ListLayout &layout = {})
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
		if (item.stars >= 0) {
			const auto earned = static_cast<size_t>(item.stars);
			listItems.back()->trailingFormat = "{}{}";
			listItems.back()->trailingArgs.emplace_back(StarRun(FilledStars, earned), UiFlags::ColorUiGold);
			listItems.back()->trailingArgs.emplace_back(StarRun(HollowStars, NumLevelGoals - earned), UiFlags::ColorUiSilverDark);
		}
		rowSelectable.push_back(item.selectable);
	}
	dialog.push_back(std::make_unique<UiList>(listItems, VisibleRows, ui.x + layout.x, ui.y + 255, layout.width, 26, layout.flags | UiFlags::FontSize24, layout.spacing));

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
	if (level == 0) {
		SetLabel("Keeper");
		SetDescription(fmt::format("Unlock monsters, spells and traps with Renown.\n\nKeeper level: {}\nRenown to spend: {}", KeeperLevel(), RenownAvailable()));
		return;
	}
	SetLabel(fmt::format("Level {}", level));
	const bool locked = level > NextOpenLevel();
	const std::string status = locked ? fmt::format("Locked: clear level {} first", level - 1) : (level <= LevelsCleared() ? "Cleared" : "Not yet cleared");
	std::string stars;
	if (!locked) {
		stars = fmt::format("\nStars: {} of {}", CountStars(ProfileMedals(level)), NumLevelGoals);
		for (size_t goal = 0; goal < NumLevelGoals; goal++) {
			const bool earned = (ProfileMedals(level) & (1U << goal)) != 0;
			stars += fmt::format("\n{} {}", earned ? FilledStar : HollowStar, GoalInfo(goal).name);
		}
	}
	SetDescription(fmt::format("{}\n\nRaids to survive: {}\nStarting gold: {}\nHero strength: {}%\nDifficulty: {}\n{}{}", rules.name, rules.raids, rules.startingGold, rules.heroStrengthPercent, DifficultyName(ProfileDifficulty()), status, stars));
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
	SetLabel(kind == static_cast<int>(GameMode::Campaign) ? "Campaign" : "Custom Map");
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
		items.push_back({ "Continue", ContinueRow, UiFlags::ColorUiGold | UiFlags::AlignCenter });
	items.push_back({ "Keeper", 0, UiFlags::ColorUiSilver | UiFlags::AlignCenter });
	for (int level = 1; level <= NumCampaignLevels; level++) {
		const std::string_view name = CampaignLevelRules(level).name;
		const int stars = CountStars(ProfileMedals(level));
		if (level > rank)
			items.push_back({ fmt::format("{}. {}", level, name), level, UiFlags::ColorUiSilverDark, false });
		else if (level <= cleared)
			items.push_back({ fmt::format("{}. {}", level, name), level, UiFlags::ColorUiSilver, true, stars });
		else
			items.push_back({ fmt::format("{}. {}", level, name), level, UiFlags::ColorUiGold, true, stars });
	}
	size_t initial = offeredResume ? 0 : static_cast<size_t>(rank);
	int level = NothingChosen;
	// Left-aligned, so the stars line up in a column. Wider and tighter than other lists: the longest name still has to fit beside them.
	const ListLayout campaignLayout { .x = 283, .width = 305, .flags = UiFlags::None, .spacing = 0 };
	while (true) {
		level = RunListScreen("Campaign", "Select Level", items, initial, DescribeCampaignLevel, campaignLayout);
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
