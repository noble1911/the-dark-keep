/**
 * @file mainmenu.cpp
 *
 * Implementation of functions for interacting with the main menu.
 */

#include <cstdint>
#include <string_view>

#ifdef USE_SDL3
#include <SDL3/SDL_surface.h>
#include <SDL3/SDL_timer.h>
#else
#include <SDL.h>
#endif

#include "DiabloUI/diabloui.h"
#include "DiabloUI/selok.h"
#include "DiabloUI/settingsmenu.h"
#include "engine/assets.hpp"
#include "engine/demomode.h"
#include "game_mode.hpp"
#include "init.hpp"
#include "movie.h"
#include "options.h"
#include "pfile.h"
#include "storm/storm_net.hpp"
#include "utils/language.h"
#include "keeper/keeper.hpp" // [DARK KEEP]
#include "keeper/keeper_campaign.hpp" // [DARK KEEP]
#include "keeper/keeper_menu.hpp" // [DARK KEEP]
#include "levels/gendung.h"
#include "utils/utf8.hpp"

namespace devilution {

uint32_t gSaveNumber;

namespace {

_music_id NextTrack()
{
	if (gbIsSpawn) {
		return TMUSIC_INTRO;
	}

	switch (sgnMusicTrack) {
	case TMUSIC_INTRO:
		return TMUSIC_CATACOMBS;
	case TMUSIC_CATACOMBS:
		return TMUSIC_CAVES;
	case TMUSIC_CAVES:
		return TMUSIC_HELL;
	case TMUSIC_HELL:
		return gbIsHellfire ? TMUSIC_NEST : TMUSIC_INTRO;
	case TMUSIC_NEST:
		return gbIsHellfire ? TMUSIC_CRYPT : TMUSIC_INTRO;
	default:
		return TMUSIC_INTRO;
	}
}

void RefreshMusic()
{
	music_start(NextTrack());
}

bool InitMenu(_selhero_selections type)
{
	bool success;

	if (type == SELHERO_PREVIOUS)
		return true;

	success = StartGame(type != SELHERO_CONTINUE, type != SELHERO_CONNECT);
	if (success)
		RefreshMusic();

	return success;
}

bool InitSinglePlayerMenu()
{
	gbIsMultiplayer = false;
	return InitMenu(SELHERO_NEW_DUNGEON);
}

bool InitMultiPlayerMenu()
{
#ifndef _DEBUG
	// [DARK KEEP] Upstream blocks multiplayer when *.lua/*.tsv/*.sol are loaded loose
	// (anti-cheat for vanilla games). This mod only plays against itself, ships its own
	// rule data, and local builds have no smpq to pack mods into MPQs -- so the check is off.
	if (false && IsAssetIntegrityViolated) {
		UiSelOkDialog(_("Multi Player Game").data(), _("Cannot play Multiplayer with overridden *.lua, *.tsv, or *.sol assets.").data(), false);
		return true;
	}
#endif
	gbIsMultiplayer = true;
	return InitMenu(SELHERO_CONNECT);
}

void PlayIntro()
{
	music_stop();
	if (gbIsHellfire)
		play_movie("gendata\\Hellfire.smk", true);
	else
		play_movie("gendata\\diablo1.smk", true);
	RefreshMusic();
}

bool DummyGetHeroInfo(_uiheroinfo * /*pInfo*/)
{
	return true;
}

// [DARK KEEP] The master has no body: a hidden "Keeper" hero is found (or created), no UI.
uint32_t AutoStartSaveNumber = UINT32_MAX;

bool FindAutoStartHero(_uiheroinfo *pInfo)
{
	if (std::string_view(pInfo->name) == "Keeper")
		AutoStartSaveNumber = pInfo->saveNumber;
	return true;
}

void SelectAutoStartHero()
{
	AutoStartSaveNumber = UINT32_MAX;
	pfile_ui_set_hero_infos(FindAutoStartHero);
	if (AutoStartSaveNumber == UINT32_MAX) {
		_uiheroinfo info {};
		info.saveNumber = pfile_ui_get_first_unused_save_num();
		CopyUtf8(info.name, "Keeper", sizeof(info.name));
		info.heroclass = HeroClass::Warrior;
		pfile_ui_save_create(&info);
		AutoStartSaveNumber = info.saveNumber;
	}
	gSaveNumber = AutoStartSaveNumber;
}

} // namespace

bool mainmenu_select_hero_dialog(GameData *gameData)
{
	if (demo::IsRunning()) {
		pfile_ui_set_hero_infos(DummyGetHeroInfo);
		gbLoadGame = true;
		return true;
	}
	// [DARK KEEP] No class, name or difficulty to choose; how hard a game is comes from the campaign.
	SelectAutoStartHero();
	gameData->nDifficulty = DIFF_NORMAL;
	gbLoadGame = false;
	return true;
}

void mainmenu_wait_for_button_sound()
{
	SDL_FillSurfaceRect(DiabloUiSurface(), nullptr, 0);
	UiFadeIn();
	SDL_Delay(350); // delay to let button pressed sound finish playing
}

void mainmenu_loop()
{
	bool done;

	RefreshMusic();
	done = false;

	// [DARK KEEP] Dev/test: the flags skip the menus that would choose the game.
	if (keeper::AutoStart || keeper::AutoNet != keeper::AutoNetMode::None)
		keeper::ApplyAutoStartSetup();

	do {
		// [DARK KEEP] Between maps: spend Renown, then on to the map that was chosen.
		if (const std::optional<keeper::KeepSetup> next = keeper::TakePostMapSetup(); next) {
			keeper::UiKeeperScreen();
			keeper::SetCurrentSetup(*next);
			if (!InitSinglePlayerMenu())
				done = true;
			continue;
		}
		_mainmenu_selections menu = MAINMENU_NONE;
		if (demo::IsRunning() || keeper::AutoStart)
			menu = MAINMENU_SINGLE_PLAYER;
		else if (keeper::AutoNet != keeper::AutoNetMode::None)
			menu = MAINMENU_MULTIPLAYER;
		else if (!UiMainMenuDialog(gszProductName, &menu, 30))
			app_fatal(_("Unable to display mainmenu"));

		switch (menu) {
		case MAINMENU_NONE:
			break;
		case MAINMENU_SINGLE_PLAYER:
			// [DARK KEEP] This entry is the Campaign; leaving an autostarted game lands on the main menu.
			if ((keeper::AutoStart || demo::IsRunning() || keeper::UiCampaignLevelSelect()) && !InitSinglePlayerMenu())
				done = true;
			keeper::AutoStart = false;
			break;
		case MAINMENU_KEEPER: // [DARK KEEP]
			keeper::UiKeeperScreen();
			break;
		case MAINMENU_CUSTOM_MAP: // [DARK KEEP]
			if (keeper::UiCustomMapSelect() && !InitSinglePlayerMenu())
				done = true;
			break;
		case MAINMENU_MAP_EDITOR: // [DARK KEEP]
			// INTEGRATION(editor): the editor starts here; keeper_editor reads CurrentSetup().mode == GameMode::MapEditor when the level loads.
			keeper::SetCurrentSetup(keeper::SetupForMapEditor());
			if (!InitSinglePlayerMenu())
				done = true;
			break;
		case MAINMENU_MULTIPLAYER:
			if (!InitMultiPlayerMenu())
				done = true;
			keeper::AutoNet = keeper::AutoNetMode::None; // [DARK KEEP]
			break;
		case MAINMENU_ATTRACT_MODE:
			if (gbIsSpawn && !HaveIntro())
				done = false;
			else if (gbActive)
				PlayIntro();
			break;
		case MAINMENU_SHOW_CREDITS:
			UiCreditsDialog();
			break;
		case MAINMENU_SHOW_SUPPORT:
			UiSupportDialog();
			break;
		case MAINMENU_EXIT_DIABLO:
			mainmenu_wait_for_button_sound();
			done = true;
			break;
		case MAINMENU_SETTINGS:
			UiSettingsMenu();
			break;
		}
	} while (!done);

	music_stop();
}

} // namespace devilution
