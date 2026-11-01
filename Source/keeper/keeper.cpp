/**
 * @file keeper/keeper.cpp
 *
 * The Dark Keep: bodyless master, free camera, master HUD and controls.
 */
#include "keeper/keeper.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>

#ifdef USE_SDL3
#include <SDL3/SDL_timer.h>
#endif

#include <fmt/format.h>

#include "control/control.hpp"
#include "cursor.h"
#include "diablo.h"
#include "engine/render/primitive_render.hpp"
#include "engine/render/scrollrt.h"
#include "engine/render/text_render.hpp"
#include "game_mode.hpp"
#include "gmenu.h"
#include "init.hpp"
#include "keeper/keeper_game.hpp"
#include "keeper/keeper_heroes.hpp"
#include "levels/dun_tile.hpp"
#include "levels/gendung.h"
#include "levels/trigs.h"
#include "lighting.h"
#include "monster.h"
#include "options.h"
#include "player.h"
#include "utils/sdl_compat.h"
#include "utils/ui_fwd.h"

namespace devilution::keeper {

bool AutoStart = false;
AutoNetMode AutoNet = AutoNetMode::None;
char AutoJoinAddress[128] = "127.0.0.1";

namespace {

/** Ambient light level for the whole keep (0 = fully lit, 15 = pitch black). */
constexpr uint8_t AmbientLight = 3;
/** Camera speed in screen pixels per second. */
constexpr float CameraSpeed = 720.F;
/** Mouse distance from the window edge (pixels) that scrolls the camera. */
constexpr int EdgeScrollMargin = 6;

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
};

/** Second palette row: four spells, then the door lock and the bait pile. */
constexpr size_t NumActionButtons = NumSpellKinds + 2;
constexpr std::array<SDL_Keycode, NumActionButtons> ActionKeys { SDLK_Z, SDLK_X, SDLK_C, SDLK_F, SDLK_G, SDLK_T };
constexpr std::array<std::string_view, NumActionButtons> ActionKeyNames { "Z", "X", "C", "F", "G", "T" };

struct Tool {
	ToolType type = ToolType::None;
	size_t index = 0;
};

Tool currentTool;

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
}

Point FindVault(Point fallback)
{
	for (int i = 0; i < numtrigs; i++) {
		if (trigs[i]._tmsg == WM_DIABNEXTLVL)
			return trigs[i].position;
	}
	return fallback;
}

// ---- HUD layout ------------------------------------------------------------

constexpr int ButtonHeight = 34;
constexpr int LeftColumnWidth = 158;
constexpr int PaletteLeft = LeftColumnWidth + 8;
constexpr int PaletteWidth = 640 - PaletteLeft - 4;

Rectangle BuildButtonRect(size_t i)
{
	const Rectangle &panel = GetMainPanel();
	const int width = PaletteWidth / static_cast<int>(NumBuildKinds);
	return { { panel.position.x + PaletteLeft + static_cast<int>(i) * width, panel.position.y + 20 }, { width - 3, ButtonHeight } };
}

Rectangle ActionButtonRect(size_t i)
{
	const Rectangle &panel = GetMainPanel();
	const int width = PaletteWidth / static_cast<int>(NumActionButtons);
	return { { panel.position.x + PaletteLeft + static_cast<int>(i) * width, panel.position.y + 20 + ButtonHeight + 4 }, { width - 3, ButtonHeight } };
}

Rectangle ReadyButtonRect()
{
	const Rectangle &panel = GetMainPanel();
	return { { panel.position.x + 6, panel.position.y + panel.size.height - 24 }, { LeftColumnWidth, 20 } };
}

bool Contains(const Rectangle &rect, Point p)
{
	return p.x >= rect.position.x && p.y >= rect.position.y && p.x < rect.position.x + rect.size.width && p.y < rect.position.y + rect.size.height;
}

std::string FormatTicks(int ticks)
{
	const int seconds = (ticks + 19) / 20;
	return fmt::format("{}:{:02}", seconds / 60, seconds % 60);
}

void DrawFrame(const Surface &out, const Rectangle &rect, uint8_t color)
{
	DrawHorizontalLine(out, rect.position, rect.size.width, color);
	DrawHorizontalLine(out, rect.position + Displacement { 0, rect.size.height - 1 }, rect.size.width, color);
	DrawVerticalLine(out, rect.position, rect.size.height, color);
	DrawVerticalLine(out, rect.position + Displacement { rect.size.width - 1, 0 }, rect.size.height, color);
}

void DrawButton(const Surface &out, const Rectangle &rect, std::string_view title, std::string_view detail, bool selected, bool affordable)
{
	FillRect(out, rect.position.x, rect.position.y, rect.size.width, rect.size.height, selected ? PAL16_RED + 12 : PAL16_GRAY + 14);
	DrawFrame(out, rect, selected ? PAL16_YELLOW + 2 : PAL16_GRAY + 8);
	const UiFlags color = affordable ? UiFlags::ColorWhite : UiFlags::ColorBlack;
	DrawString(out, title, { rect.position + Displacement { 4, 3 }, { rect.size.width - 8, 14 } }, { .flags = UiFlags::FontSize12 | color });
	DrawString(out, detail, { rect.position + Displacement { 4, 18 }, { rect.size.width - 8, 14 } }, { .flags = UiFlags::FontSize12 | (affordable ? UiFlags::ColorGold : UiFlags::ColorBlack) });
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

/** Outlines a tile's floor diamond (optionally grown by `radius` tiles). */
void DrawTileDiamond(const Surface &out, Point tile, uint8_t color, int radius = 0)
{
	const int zoom = ZoomFactor();
	const Point base = GetScreenPosition(tile);
	const int maxY = GetMainPanel().position.y;
	const int hw = TILE_WIDTH / 2 * (radius * 2 + 1) * zoom;
	const int hh = TILE_HEIGHT / 2 * (radius * 2 + 1) * zoom;
	const Point halfCenter = base + Displacement { TILE_WIDTH / 2, -TILE_HEIGHT / 2 };
	const Point center { halfCenter.x * zoom, halfCenter.y * zoom };
	const Point left = center + Displacement { -hw, 0 };
	const Point top = center + Displacement { 0, -hh };
	const Point right = center + Displacement { hw, 0 };
	const Point bottom = center + Displacement { 0, hh };
	DrawLine(out, left, top, color, maxY);
	DrawLine(out, top, right, color, maxY);
	DrawLine(out, right, bottom, color, maxY);
	DrawLine(out, bottom, left, color, maxY);
}

void DrawWorldLabel(const Surface &out, Point tile, std::string_view text, UiFlags color, int raise = 0)
{
	const int zoom = ZoomFactor();
	const Point base = GetScreenPosition(tile);
	const Point anchor = base + Displacement { TILE_WIDTH / 2, -TILE_HEIGHT };
	const Point at { anchor.x * zoom - 60, anchor.y * zoom - 4 - raise * zoom };
	if (at.y < 0 || at.y + 14 >= GetMainPanel().position.y)
		return;
	DrawString(out, text, { at, { 120, 14 } }, { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | color | UiFlags::Outlined });
}

void DrawOverlays(const Surface &out)
{
	DrawTileDiamond(out, State.vault, PAL16_YELLOW + 2, 1);
	DrawWorldLabel(out, State.vault, fmt::format("Vault: {} gold", State.vaultGold), UiFlags::ColorGold);
	DrawTileDiamond(out, State.entrance, PAL16_BLUE + 2, 1);
	DrawWorldLabel(out, State.entrance, "Entrance", UiFlags::ColorBlue);

	for (const Lair &lair : State.lairs) {
		if (!lair.active)
			continue;
		DrawTileDiamond(out, lair.position, PAL16_RED + 4);
		DrawWorldLabel(out, lair.position, lair.summonTicks > 0 ? std::string_view("Summoning...") : BuildKinds[lair.kind].name, UiFlags::ColorRed);
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
		DrawWorldLabel(out, lock.position, fmt::format("Locked ({})", lock.strength), UiFlags::ColorRed);
	}

	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		const Monster &monster = Monsters[ActiveMonsters[i]];
		if (!IsHero(monster) || monster.hitPoints <= 0)
			continue;
		const int hpPercent = monster.maxHitPoints > 0 ? 100 * monster.hitPoints / monster.maxHitPoints : 0;
		DrawWorldLabel(out, monster.position.tile, fmt::format("{} {}%", HeroTitle(monster), hpPercent), UiFlags::ColorWhite, 64);
	}

	std::string_view reason;
	bool ok = true;
	switch (currentTool.type) {
	case ToolType::Build:
		ok = CanBuildAt(currentTool.index, cursPosition, &reason);
		break;
	case ToolType::Bait:
		ok = CanPlaceBaitAt(cursPosition, &reason);
		break;
	case ToolType::LockDoor:
		ok = CanLockDoorAt(cursPosition, &reason);
		break;
	case ToolType::Spell:
		DrawTileDiamond(out, cursPosition, PAL16_BLUE + 4, 1);
		return;
	case ToolType::None:
		return;
	}
	DrawTileDiamond(out, cursPosition, ok ? PAL16_GRAY : PAL16_RED + 2);
	if (!ok)
		DrawWorldLabel(out, cursPosition, reason, UiFlags::ColorRed);
}

void SelectTool(ToolType type, size_t index)
{
	if (currentTool.type == type && currentTool.index == index) {
		currentTool = {};
		return;
	}
	currentTool = { type, index };
}

void SelectActionButton(size_t i)
{
	if (i < NumSpellKinds)
		SelectTool(ToolType::Spell, i);
	else if (i == NumSpellKinds)
		SelectTool(ToolType::LockDoor, 0);
	else
		SelectTool(ToolType::Bait, 0);
}

bool IsGameOver()
{
	return State.phase == Phase::Victory || State.phase == Phase::Defeat;
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
	case ToolType::Spell: {
		const SpellKind &spell = SpellKinds[currentTool.index];
		const size_t me = MyPlayerId;
		if (IsGameOver()) {
			SetFeedback("The siege is over");
			return;
		}
		if (State.spellCooldown[me] > 0) {
			SetFeedback(fmt::format("Gathering power... {}", FormatTicks(State.spellCooldown[me])));
			return;
		}
		if (State.mana[me] < spell.manaCost * ManaScale) {
			SetFeedback("Not enough mana");
			return;
		}
		RequestCastSpell(currentTool.index, tile);
		return;
	}
	}
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
	const Point entrance = ViewPosition;

	for (Player &player : Players) {
		if (player.plractive && player.isOnActiveLevel())
			ParkPlayer(player);
	}

	RevealKeep();
	ApplyAmbientLight();
	ResetGame(entrance, FindVault(entrance));
	CenterCameraOn(entrance);
	currentTool = {};
	tickCounter = 0;
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

	GameTick();

	tickCounter++;
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

	if (key >= SDLK_1 && key <= SDLK_9) {
		const size_t index = static_cast<size_t>(key - SDLK_1);
		if (index < NumBuildKinds)
			SelectTool(ToolType::Build, index);
		return true;
	}
	for (size_t i = 0; i < NumActionButtons; i++) {
		if (key == ActionKeys[i]) {
			SelectActionButton(i);
			return true;
		}
	}

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
	case SDLK_B:
	case SDLK_Q:
	case SDLK_Y:
	case SDLK_0:
		return true;
	case SDLK_R:
		if (State.phase == Phase::Setup || State.phase == Phase::Breather)
			RequestStartRaid();
		return true;
	case SDLK_SPACE:
	case SDLK_HOME:
		CenterCameraOn(State.entrance);
		return true;
	case SDLK_V:
		CenterCameraOn(State.vault);
		return true;
	case SDLK_EQUALS:
	case SDLK_KP_PLUS:
		SetZoom(true);
		return true;
	case SDLK_MINUS:
	case SDLK_KP_MINUS:
		SetZoom(false);
		return true;
	case SDLK_ESCAPE:
		if (currentTool.type != ToolType::None) {
			currentTool = {};
			return true;
		}
		return false;
	default:
		return false;
	}
}

bool OnLeftMouseDown(uint16_t /*modState*/)
{
	if (!IsActive())
		return false;

	const Rectangle &panel = GetMainPanel();
	if (MousePosition.y >= panel.position.y) {
		for (size_t i = 0; i < NumBuildKinds; i++) {
			if (Contains(BuildButtonRect(i), MousePosition))
				SelectTool(ToolType::Build, i);
		}
		for (size_t i = 0; i < NumActionButtons; i++) {
			if (Contains(ActionButtonRect(i), MousePosition))
				SelectActionButton(i);
		}
		if (Contains(ReadyButtonRect(), MousePosition) && (State.phase == Phase::Setup || State.phase == Phase::Breather))
			RequestStartRaid();
		return true;
	}

	UseToolAt(cursPosition);
	return true;
}

bool OnRightMouseDown()
{
	if (!IsActive())
		return false;
	currentTool = {};
	return true;
}

bool OnMouseWheel(int deltaY)
{
	if (!IsActive() || deltaY == 0 || gmenu_is_active())
		return false;
	SetZoom(deltaY > 0);
	return true;
}

void DrawHud(const Surface &out)
{
	DrawOverlays(out);

	const Rectangle &mainPanel = GetMainPanel();
	const int top = mainPanel.position.y;
	const int height = gnScreenHeight - top;

	FillRect(out, 0, top, gnScreenWidth, height, 0);
	DrawHorizontalLine(out, { 0, top }, gnScreenWidth, PAL16_RED + 8);

	const int left = mainPanel.position.x + 8;
	const size_t me = MyPlayerId;

	// Left column: phase, wave, vault, mana.
	std::string phaseLine = std::string(PhaseName(State.phase));
	if (State.phase == Phase::Setup || State.phase == Phase::Breather)
		phaseLine += fmt::format("  {}", FormatTicks(State.phaseTicks));
	DrawString(out, phaseLine, { { left, top + 4 }, { LeftColumnWidth, 16 } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorRed });
	DrawString(out, fmt::format("Raid {} of {}", std::min<int>(State.wave, NumWaves), NumWaves), { { left, top + 19 }, { LeftColumnWidth, 16 } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorWhite });
	DrawString(out, fmt::format("Vault: {} gold", State.vaultGold), { { left, top + 34 }, { LeftColumnWidth, 16 } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorGold });
	const int inFlight = GoldInFlight();
	DrawString(out, fmt::format("Heroes {}  Loot {}", LivingHeroCount(), inFlight), { { left, top + 49 }, { LeftColumnWidth, 16 } }, { .flags = UiFlags::FontSize12 | (inFlight > 0 ? UiFlags::ColorRed : UiFlags::ColorWhite) });

	const int mana = State.mana[me] / ManaScale;
	const Rectangle manaRect { { left, top + 68 }, { LeftColumnWidth - 4, 10 } };
	FillRect(out, manaRect.position.x, manaRect.position.y, manaRect.size.width, manaRect.size.height, PAL16_GRAY + 14);
	FillRect(out, manaRect.position.x, manaRect.position.y, manaRect.size.width * mana / MaxMana, manaRect.size.height, PAL16_BLUE + 4);
	DrawFrame(out, manaRect, PAL16_GRAY + 8);
	DrawString(out, fmt::format("Mana {}", mana), { manaRect.position + Displacement { 0, 12 }, { LeftColumnWidth, 14 } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorBlue });

	if (State.phase == Phase::Setup || State.phase == Phase::Breather) {
		const Rectangle ready = ReadyButtonRect();
		FillRect(out, ready.position.x, ready.position.y, ready.size.width, ready.size.height, PAL16_RED + 12);
		DrawFrame(out, ready, PAL16_YELLOW + 4);
		DrawString(out, "R: Open the gates", { ready.position + Displacement { 0, 3 }, { ready.size.width, 14 } }, { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | UiFlags::ColorGold });
	}

	// Palette: build row (1-6), action row (Z X C F G T).
	for (size_t i = 0; i < NumBuildKinds; i++) {
		const BuildInfo &kind = BuildKinds[i];
		const bool selected = currentTool.type == ToolType::Build && currentTool.index == i;
		const std::string detail = IsLairKind(i) ? fmt::format("{}g x{}", kind.cost, kind.count) : fmt::format("{}g", kind.cost);
		DrawButton(out, BuildButtonRect(i), fmt::format("{} {}", i + 1, kind.name), detail, selected, State.vaultGold >= kind.cost);
	}
	for (size_t i = 0; i < NumActionButtons; i++) {
		std::string title;
		std::string detail;
		bool selected = false;
		bool ready = false;
		if (i < NumSpellKinds) {
			const SpellKind &spell = SpellKinds[i];
			title = fmt::format("{} {}", ActionKeyNames[i], spell.name);
			detail = fmt::format("{} mana", spell.manaCost);
			selected = currentTool.type == ToolType::Spell && currentTool.index == i;
			ready = State.mana[me] >= spell.manaCost * ManaScale && State.spellCooldown[me] == 0;
		} else if (i == NumSpellKinds) {
			title = fmt::format("{} Lock", ActionKeyNames[i]);
			detail = fmt::format("{}g", LockDoorCost);
			selected = currentTool.type == ToolType::LockDoor;
			ready = State.vaultGold >= LockDoorCost;
		} else {
			title = fmt::format("{} Bait", ActionKeyNames[i]);
			detail = fmt::format("{}g", BaitCost);
			selected = currentTool.type == ToolType::Bait;
			ready = State.vaultGold >= BaitCost;
		}
		DrawButton(out, ActionButtonRect(i), title, detail, selected, ready);
	}

	const int paletteX = mainPanel.position.x + PaletteLeft;
	if (!feedback.empty() && SDL_GetTicks() < feedbackUntil) {
		DrawString(out, feedback, { { paletteX, top + 4 }, { PaletteWidth, 14 } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorRed });
	} else {
		DrawString(out, "WASD move  Space entrance  V vault  Wheel zoom", { { paletteX, top + 4 }, { PaletteWidth, 14 } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorWhite });
	}
	const std::string scout = State.phase == Phase::Raid ? fmt::format("Raid {} in progress", State.wave) : (IsGameOver() ? std::string() : DescribeWave(State.wave));
	DrawString(out, scout, { { paletteX, top + 96 }, { PaletteWidth, 14 } }, { .flags = UiFlags::FontSize12 | UiFlags::ColorGold });

	if (IsGameOver()) {
		const bool won = State.phase == Phase::Victory;
		const Rectangle banner { { 0, top / 2 - 40 }, { gnScreenWidth, 80 } };
		DrawHalfTransparentRectTo(out, banner.position.x, banner.position.y, banner.size.width, banner.size.height);
		DrawString(out, won ? "THE KEEP STANDS" : "THE VAULT IS EMPTY", { banner.position + Displacement { 0, 10 }, { banner.size.width, 30 } },
		    { .flags = UiFlags::FontSize30 | UiFlags::AlignCenter | (won ? UiFlags::ColorGold : UiFlags::ColorRed) });
		DrawString(out, won ? fmt::format("All {} raids repelled with {} gold left. Esc for the menu.", NumWaves, State.vaultGold) : std::string("The heroes made off with your treasure. Esc for the menu."),
		    { banner.position + Displacement { 0, 50 }, { banner.size.width, 16 } }, { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | UiFlags::ColorWhite });
	}
}

} // namespace devilution::keeper
