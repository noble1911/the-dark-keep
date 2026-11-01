/**
 * @file keeper/keeper.cpp
 *
 * The Dark Keep: bodyless master, free camera, master HUD.
 */
#include "keeper/keeper.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#ifdef USE_SDL3
#include <SDL3/SDL_timer.h>
#endif

#include <fmt/format.h>

#include "control/control.hpp"
#include "diablo.h"
#include "engine/render/primitive_render.hpp"
#include "engine/render/text_render.hpp"
#include "game_mode.hpp"
#include "gmenu.h"
#include "init.hpp"
#include "levels/dun_tile.hpp"
#include "levels/gendung.h"
#include "lighting.h"
#include "player.h"
#include "utils/sdl_compat.h"
#include "utils/ui_fwd.h"

namespace devilution::keeper {

bool AutoStart = false;

namespace {

/** Ambient light level for the whole keep (0 = fully lit, 15 = pitch black). */
constexpr uint8_t AmbientLight = 3;
/** Camera speed in screen pixels per second. */
constexpr float CameraSpeed = 720.F;
/** Mouse distance from the window edge (pixels) that scrolls the camera. */
constexpr int EdgeScrollMargin = 6;

bool active = false;

/** Free camera position in world tile coordinates (fractional). */
float cameraX = 0.F;
float cameraY = 0.F;
uint32_t lastCameraTicks = 0;

/** Where the heroes enter (the level's up-stairs arrival point). */
Point keepEntrance;

Point ParkingTile(const Player &player)
{
	// Top-left corner of the map is always solid rock: nothing walks there and it is never drawn
	// in view of the playable area.
	return { 1 + static_cast<int>(player.getId()), 1 };
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
	return gbActive && !ChatFlag && !gmenu_is_active();
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

	// ViewPosition was set by the level generator to the up-stairs arrival point.
	keepEntrance = ViewPosition;

	for (Player &player : Players) {
		if (player.plractive && player.isOnActiveLevel())
			ParkPlayer(player);
	}

	RevealKeep();
	ApplyAmbientLight();
	CenterCameraOn(keepEntrance);
	lastCameraTicks = SDL_GetTicks();
}

void OnGameEnded()
{
	active = false;
}

void Tick()
{
	if (!IsActive())
		return;

	// Remote masters arrive later than us; park every body on the keep level, every tick (cheap, idempotent).
	for (Player &player : Players) {
		if (player.plractive && player.isOnActiveLevel() && !player._pLvlChanging)
			ParkPlayer(player);
	}
	RevealKeep();
}

void UpdateCamera()
{
	if (!IsActive())
		return;

	const uint32_t now = SDL_GetTicks();
	const float dt = std::min<uint32_t>(now - lastCameraTicks, 50) / 1000.F;
	lastCameraTicks = now;

	float sx = 0;
	float sy = 0;
	if (CanUseKeyboardCamera()) {
		const auto *keys = SDLC_GetKeyState();
		if (keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_LEFT]) sx -= 1;
		if (keys[SDL_SCANCODE_D] || keys[SDL_SCANCODE_RIGHT]) sx += 1;
		if (keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_UP]) sy -= 1;
		if (keys[SDL_SCANCODE_S] || keys[SDL_SCANCODE_DOWN]) sy += 1;

		if (MousePosition.x <= EdgeScrollMargin) sx = -1;
		if (MousePosition.x >= gnScreenWidth - 1 - EdgeScrollMargin) sx = 1;
		if (MousePosition.y <= EdgeScrollMargin) sy = -1;
		if (MousePosition.y >= gnScreenHeight - 1 - EdgeScrollMargin) sy = 1;
	}
	if (sx == 0 && sy == 0)
		return;

	// Screen pixels -> world tiles. One world step +x is (+32, +16) px on screen, +y is (-32, +16) px.
	const float dsx = sx * CameraSpeed * dt;
	const float dsy = sy * CameraSpeed * dt;
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

bool ShouldDrawPlayer(const Player & /*player*/)
{
	return !IsActive();
}

bool CanMonstersTargetPlayers()
{
	return !IsActive();
}

bool OnKeyDown(SDL_Keycode key)
{
	if (!IsActive())
		return false;

	switch (key) {
	// Camera keys are polled every frame; swallow them so they don't trigger hero hotkeys.
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
	case SDLK_C:
	case SDLK_B:
	case SDLK_Q:
	case SDLK_Y:
		return true;
	case SDLK_SPACE:
	case SDLK_HOME:
		CenterCameraOn(keepEntrance);
		return true;
	default:
		return false;
	}
}

bool OnLeftMouseDown(uint16_t /*modState*/)
{
	return IsActive();
}

bool OnRightMouseDown()
{
	return IsActive();
}

void DrawHud(const Surface &out)
{
	const Rectangle &mainPanel = GetMainPanel();
	const int top = mainPanel.position.y;
	const int height = gnScreenHeight - top;

	FillRect(out, 0, top, gnScreenWidth, height, 0);
	DrawHorizontalLine(out, { 0, top }, gnScreenWidth, PAL16_RED + 8);

	const int left = mainPanel.position.x + 12;
	DrawString(out, "THE DARK KEEP", { { left, top + 8 }, { 300, 30 } }, { .flags = UiFlags::FontSize24 | UiFlags::ColorRed });
	DrawString(out, "Setup phase", { { left, top + 44 }, { 300, 20 } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorGold });
	DrawString(out, "WASD / arrows / screen edge: move    Space: return to entrance",
	    { { left, top + height - 22 }, { mainPanel.size.width - 24, 16 } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorWhite });
}

} // namespace devilution::keeper
