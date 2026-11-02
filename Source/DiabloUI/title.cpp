#include <memory>
#include <optional>
#include <vector>

#ifdef USE_SDL3
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_rect.h>
#include <SDL3/SDL_timer.h>
#else
#include <SDL.h>
#endif

#include "DiabloUI/diabloui.h"
#include "DiabloUI/ui_flags.hpp"
#include "DiabloUI/ui_item.h"
#include "controls/input.h"
#include "controls/menu_controls.h"
#include "discord/discord.h"
#include "engine/clx_sprite.hpp"
#include "engine/load_clx.hpp"
#include "engine/load_pcx.hpp"
#include "engine/point.hpp"
#include "utils/algorithm/container.hpp"
#include "utils/language.h"
#include "utils/sdl_compat.h"
#include "utils/sdl_geometry.h"
#include "utils/ui_fwd.h"

namespace devilution {
namespace {

std::vector<std::unique_ptr<UiItemBase>> vecTitleScreen;

void TitleLoad()
{
	// [DARK KEEP] Our own title over the menu's background, where Hellfire's title art and logo were.
	ArtBackgroundWidescreen = LoadOptionalClx("ui_art\\mainmenuw.clx");
	LoadBackgroundArt("ui_art\\mainmenu");
}

void TitleFree()
{
	ArtBackground = std::nullopt;
	ArtBackgroundWidescreen = std::nullopt;

	vecTitleScreen.clear();
}

} // namespace

void UiTitleDialog()
{
	TitleLoad();
	const Point uiPosition = GetUIRectangle().position;
	UiAddBackground(&vecTitleScreen);
	vecTitleScreen.push_back(std::make_unique<UiArtText>("The Dark Keep", MakeSdlRect(uiPosition.x, uiPosition.y + 170, 640, 56), UiFlags::AlignCenter | UiFlags::FontSize46 | UiFlags::ColorUiGold, 4));
	vecTitleScreen.push_back(std::make_unique<UiArtText>("You are the dungeon. Keep your gold.", MakeSdlRect(uiPosition.x, uiPosition.y + 240, 640, 30), UiFlags::AlignCenter | UiFlags::FontSize24 | UiFlags::ColorUiSilver));
	vecTitleScreen.push_back(std::make_unique<UiArtText>("Built on DevilutionX. Plays with your own game data.", MakeSdlRect(uiPosition.x, uiPosition.y + 420, 640, 20), UiFlags::AlignCenter | UiFlags::FontSize12 | UiFlags::ColorUiSilverDark));

	bool endMenu = false;
	const Uint32 timeOut = SDL_GetTicks() + 7000;

	SDL_Event event;
	while (!endMenu && SDL_GetTicks() < timeOut) {
		UiRenderItems(vecTitleScreen);
		UiFadeIn();

		discord_manager::UpdateMenu();

		while (PollEvent(&event)) {
			if (c_any_of(GetMenuActions(event), [](MenuAction menuAction) { return menuAction != MenuAction_NONE; })) {
				endMenu = true;
				break;
			}
			switch (event.type) {
			case SDL_EVENT_KEY_DOWN:
			case SDL_EVENT_MOUSE_BUTTON_UP:
				endMenu = true;
				break;
			}
			UiHandleEvents(&event);
		}
	}

	TitleFree();
}

} // namespace devilution
