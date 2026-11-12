#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#ifdef USE_SDL3
#include <SDL3/SDL_rect.h>
#include <SDL3/SDL_timer.h>
#else
#include <SDL.h>
#endif

#include "DiabloUI/diabloui.h"
#include "DiabloUI/ui_flags.hpp"
#include "DiabloUI/ui_item.h"
#include "engine/assets.hpp"
#include "engine/load_clx.hpp"
#include "engine/point.hpp"
#include "game_mode.hpp"
#include "utils/language.h"
#include "utils/ui_fwd.h"

namespace devilution {
namespace {
int mainmenu_attract_time_out; // seconds
uint32_t dwAttractTicks;

std::vector<std::unique_ptr<UiItemBase>> vecMainMenuDialog;
std::vector<std::unique_ptr<UiListItem>> vecMenuItems;

_mainmenu_selections MainMenuResult;

void UiMainMenuSelect(size_t value)
{
	MainMenuResult = (_mainmenu_selections)vecMenuItems[value]->m_value;
}

#ifndef NOEXIT
void MainmenuEsc()
{
	const std::size_t last = vecMenuItems.size() - 1;
	if (SelectedItem == last) {
		UiMainMenuSelect(last);
	} else {
		SelectedItem = last;
	}
}
#endif

void MainmenuLoad(const char *name)
{
	// [DARK KEEP] The dungeon master's menu.
	vecMenuItems.push_back(std::make_unique<UiListItem>(std::string_view("Campaign"), MAINMENU_SINGLE_PLAYER));
	vecMenuItems.push_back(std::make_unique<UiListItem>(std::string_view("Keeper"), MAINMENU_KEEPER));
	vecMenuItems.push_back(std::make_unique<UiListItem>(std::string_view("Custom Map"), MAINMENU_CUSTOM_MAP));
	vecMenuItems.push_back(std::make_unique<UiListItem>(std::string_view("Multiplayer"), MAINMENU_MULTIPLAYER));
	vecMenuItems.push_back(std::make_unique<UiListItem>(std::string_view("Map Editor"), MAINMENU_MAP_EDITOR));
	vecMenuItems.push_back(std::make_unique<UiListItem>(_("Settings"), MAINMENU_SETTINGS));
#ifndef NOEXIT
	vecMenuItems.push_back(std::make_unique<UiListItem>(std::string_view("Exit"), MAINMENU_EXIT_DIABLO));
#endif

	UiLoadMainMenuBackground();

	UiAddBackground(&vecMainMenuDialog);
	UiAddLogo(&vecMainMenuDialog);

	const Point uiPosition = GetUIRectangle().position;

	if (gbIsSpawn && gbIsHellfire) {
		const SDL_Rect rect1 = { (Sint16)(uiPosition.x), (Sint16)(uiPosition.y + 145), 640, 30 };
		vecMainMenuDialog.push_back(std::make_unique<UiArtText>(_("Shareware").data(), rect1, UiFlags::FontSize30 | UiFlags::ColorUiSilver | UiFlags::AlignCenter, 8));
	}

	vecMainMenuDialog.push_back(std::make_unique<UiList>(vecMenuItems, vecMenuItems.size(), uiPosition.x + 64, (uiPosition.y + 192), 510, 43, UiFlags::FontSize42 | UiFlags::ColorUiGold | UiFlags::AlignCenter, 5));

	const SDL_Rect rect2 = { 17, (Sint16)(UiScreenHeight() - 36), 605, 21 };
	vecMainMenuDialog.push_back(std::make_unique<UiArtText>(name, rect2, UiFlags::FontSize12 | UiFlags::ColorUiSilverDark));

#ifndef NOEXIT
	UiInitList(nullptr, UiMainMenuSelect, MainmenuEsc, vecMainMenuDialog, true);
#else
	UiInitList(nullptr, UiMainMenuSelect, nullptr, vecMainMenuDialog, true);
#endif
}

void MainmenuFree()
{
	ArtBackgroundWidescreen = std::nullopt;
	ArtBackground = std::nullopt;

	vecMainMenuDialog.clear();

	vecMenuItems.clear();
}

} // namespace

void mainmenu_restart_repintro()
{
	dwAttractTicks = SDL_GetTicks() + mainmenu_attract_time_out * 1000;
}

bool UiMainMenuDialog(const char *name, _mainmenu_selections *pdwResult, int attractTimeOut)
{
	MainMenuResult = MAINMENU_NONE;
	while (MainMenuResult == MAINMENU_NONE) {
		mainmenu_attract_time_out = attractTimeOut;
		MainmenuLoad(name);

		mainmenu_restart_repintro(); // for automatic starts

		while (MainMenuResult == MAINMENU_NONE) {
			UiClearScreen();
			UiPollAndRender();
			// [DARK KEEP] No attract mode: another game's intro has no business playing over an idle menu.
		}

		MainmenuFree();
	}

	*pdwResult = MainMenuResult;
	return true;
}

} // namespace devilution
