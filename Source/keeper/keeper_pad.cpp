/**
 * @file keeper/keeper_pad.cpp
 *
 * See keeper_pad.hpp.
 */
#include "keeper/keeper_pad.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include <fmt/format.h>

#include "control/control.hpp"
#include "controls/control_mode.hpp"
#include "controls/controller.h"
#include "controls/controller_motion.h"
#include "controls/plrctrls.h"
#include "cursor.h"
#include "diablo.h"
#include "engine/palette.h"
#include "engine/render/clx_render.hpp"
#include "engine/render/primitive_render.hpp"
#include "engine/render/text_render.hpp"
#include "gamemenu.h"
#include "keeper/keeper.hpp"
#include "keeper/keeper_bots.hpp"
#include "keeper/keeper_editor.hpp"
#include "keeper/keeper_game.hpp"
#include "keeper/keeper_heroes.hpp"
#include "keeper/keeper_input.hpp"
#include "keeper/keeper_ui.hpp"
#include "levels/gendung.h"
#include "monster.h"
#include "options.h"
#include "player.h"
#include "utils/display.h"
#include "utils/is_of.hpp"
#include "utils/sdl_compat.h"

namespace devilution::keeper {

namespace {

/** Full stick crosses the screen's height in this long. */
constexpr float CursorCrossSeconds = 1.1F;
/** A Switch doubles the UI from this screen height, where a computer waits for 1440p. */
constexpr int TvDoubleHeight = 1080;
/** Anything straight ahead beats anything off to the side, however near. */
constexpr int OffToTheSide = 100000;
/** How far from the cursor a monster or hero counts as under it, in tiles. */
constexpr int SoftTargetTiles = 2;

/** Held with other buttons. The triggers aren't swapped on any layout. */
constexpr ControllerButton ModifierButton = ControllerButton_AXIS_TRIGGERLEFT;
constexpr ControllerButton SpellButton = ControllerButton_AXIS_TRIGGERRIGHT;

/** Spell slots, by the button held with the spell button. */
constexpr std::array<ControllerButton, NumSpellSlots> SpellButtons {
	ControllerButton_BUTTON_X,
	ControllerButton_BUTTON_Y,
	ControllerButton_BUTTON_B,
	ControllerButton_BUTTON_A,
	ControllerButton_BUTTON_LEFTSHOULDER,
	ControllerButton_BUTTON_RIGHTSHOULDER,
};

/** The focus is on something in the HUD, and the mouse position is parked on it. */
bool focused = false;
Rectangle focusRect {};
/** Where the cursor goes back to. */
Point cursorOnMap;
/** Left-stick overshoot past the edge of the view, pushing the camera. */
float pushX = 0.F;
float pushY = 0.F;
float cursorRemainderX = 0.F;
float cursorRemainderY = 0.F;
uint32_t lastFrameTicks = 0;
AxisDirectionRepeater focusRepeater;
/** We hold down Shift for the modifier, and have to let go of it. */
bool holdingShift = false;
/** The tool that was armed before Confirm pressed something. */
std::optional<uint32_t> toolBeforeConfirm;
bool leftDown = false;
bool rightDown = false;
bool jumpToVault = false;

/** `button` is named by what's printed on an Xbox pad; on a Switch that's the button in the other corner, printed the same. */
std::string Combo(ControllerButton modifier, ControllerButton button)
{
	const std::string name(ToString(GamepadType, TranslateTo(GamepadType, button)));
	if (modifier == ControllerButton_NONE)
		return name;
	return fmt::format("{}+{}", ToString(GamepadType, modifier), name);
}

Point ScreenCenterOf(const Rectangle &rect)
{
	const int scale = UiPixelScale();
	const Point center = rect.Center();
	return { center.x * scale + scale / 2, center.y * scale + scale / 2 };
}

void CollectTargets(std::vector<Rectangle> &targets)
{
	targets.clear();
	if (IsEditorActive())
		EditorPadTargets(targets);
	else
		PadHudTargets(targets);
}

bool NeedsFocus()
{
	return IsEditorActive() ? EditorPadNeedsFocus() : PadHudNeedsFocus();
}

void FocusOn(const Rectangle &rect)
{
	focusRect = rect;
	SetCursorPos(ScreenCenterOf(rect));
}

void EnterFocus()
{
	std::vector<Rectangle> targets;
	CollectTargets(targets);
	if (targets.empty())
		return;
	if (!focused)
		cursorOnMap = MousePosition;
	focused = true;
	if (focusRect.size.width == 0 && !IsEditorActive())
		focusRect = PadSwitchTab(0);
	const Point from = focusRect.size.width > 0 ? focusRect.Center() : targets.front().Center();
	FocusOn(targets[PadNearestTarget(targets, from).value_or(0)]);
}

void LeaveFocus()
{
	if (!focused)
		return;
	focused = false;
	SetCursorPos(cursorOnMap);
}

void MoveFocus(AxisDirection dir)
{
	std::vector<Rectangle> targets;
	CollectTargets(targets);
	if (targets.empty()) {
		LeaveFocus();
		return;
	}
	if (IsEditorActive() ? EditorPadScroll(focusRect, dir) : PadHudScroll(focusRect, dir))
		return; // the row moved under the focus
	if (const std::optional<size_t> next = PadStepFocus(targets, focusRect, dir); next)
		FocusOn(targets[*next]);
}

/** What had the focus may have moved or gone: the cards change with the tab, and buttons come and go. */
void KeepFocusOnSomething()
{
	std::vector<Rectangle> targets;
	CollectTargets(targets);
	if (targets.empty()) {
		LeaveFocus();
		return;
	}
	FocusOn(targets[PadNearestTarget(targets, focusRect.Center()).value_or(0)]);
}

/** Over the map, the engine has to read the mouse position as it does a mouse user's: it picks what's under it. */
void KeepMousePicking()
{
	if (ControlMode != ControlTypes::KeyboardAndMouse) {
		ControlMode = ControlTypes::KeyboardAndMouse;
		CalculatePanelAreas();
	}
}

AxisDirection DpadDirection()
{
	AxisDirection dir { AxisDirectionX_NONE, AxisDirectionY_NONE };
	if (IsControllerButtonPressed(ControllerButton_BUTTON_DPAD_LEFT))
		dir.x = AxisDirectionX_LEFT;
	else if (IsControllerButtonPressed(ControllerButton_BUTTON_DPAD_RIGHT))
		dir.x = AxisDirectionX_RIGHT;
	if (IsControllerButtonPressed(ControllerButton_BUTTON_DPAD_UP))
		dir.y = AxisDirectionY_UP;
	else if (IsControllerButtonPressed(ControllerButton_BUTTON_DPAD_DOWN))
		dir.y = AxisDirectionY_DOWN;
	return dir;
}

/** The left trigger is Shift, so the code that asks about Shift needs nothing new. */
void UpdateModifier(bool wanted)
{
	if (wanted == holdingShift)
		return;
	holdingShift = wanted;
	const SDL_Keymod mods = SDL_GetModState();
	SDL_SetModState(static_cast<SDL_Keymod>(wanted ? mods | KMOD_LSHIFT : mods & ~KMOD_LSHIFT));
}

void MoveCursor(float dt)
{
	const float magnitude = std::sqrt(leftStickX * leftStickX + leftStickY * leftStickY);
	if (magnitude <= 0.F)
		return;
	// Slow near the middle, for aiming at a tile; fast at the rim.
	const float speed = magnitude * static_cast<float>(gnScreenHeight) / CursorCrossSeconds;
	cursorRemainderX += leftStickX * speed * dt;
	cursorRemainderY -= leftStickY * speed * dt;
	const int dx = static_cast<int>(cursorRemainderX);
	const int dy = static_cast<int>(cursorRemainderY);
	cursorRemainderX -= static_cast<float>(dx);
	cursorRemainderY -= static_cast<float>(dy);

	const int maxX = gnScreenWidth - 1;
	const int maxY = PadViewBottom() - 1;
	const Point to { MousePosition.x + dx, MousePosition.y + dy };
	if ((to.x <= 0 && leftStickX < 0) || (to.x >= maxX && leftStickX > 0))
		pushX = leftStickX;
	if ((to.y <= 0 && leftStickY > 0) || (to.y >= maxY && leftStickY < 0))
		pushY = -leftStickY;
	SetCursorPos({ std::clamp(to.x, 0, maxX), std::clamp(to.y, 0, maxY) });
}

void PressMouse(GameAction &action, uint8_t mouseButton, bool up)
{
	action = GameActionSendKey { static_cast<uint32_t>(mouseButton) | KeymapperMouseButtonMask, up };
}

/** @param button As labelled on the controller. */
void HandlePress(ControllerButton button, GameAction &action)
{
	const bool modifier = IsControllerButtonPressed(ModifierButton);
	const bool editor = IsEditorActive();
	if (!editor && IsControllerButtonPressed(SpellButton)) {
		const auto slot = std::find(SpellButtons.begin(), SpellButtons.end(), button);
		if (slot != SpellButtons.end())
			PerformKeeperAction(KeeperAction::Spell, static_cast<size_t>(slot - SpellButtons.begin()));
		return;
	}
	if (editor && EditorPadButton(button, modifier, IsControllerButtonPressed(SpellButton)))
		return;

	switch (button) {
	case ControllerButton_BUTTON_A:
		toolBeforeConfirm = PadToolStamp();
		leftDown = true;
		PressMouse(action, SDL_BUTTON_LEFT, /*up=*/false);
		return;
	case ControllerButton_BUTTON_X:
		if (focused)
			return;
		rightDown = true;
		PressMouse(action, SDL_BUTTON_RIGHT, /*up=*/false);
		return;
	case ControllerButton_BUTTON_B:
		if (modifier && !editor) {
			PerformKeeperAction(KeeperAction::Sell, 0);
		} else if (focused && !NeedsFocus()) {
			LeaveFocus();
		} else {
			OnKeyDown(SDLK_ESCAPE);
		}
		return;
	case ControllerButton_BUTTON_Y:
		PerformKeeperAction(KeeperAction::Possess, 0);
		return;
	case ControllerButton_BUTTON_LEFTSHOULDER:
	case ControllerButton_BUTTON_RIGHTSHOULDER:
		focusRect = PadSwitchTab(button == ControllerButton_BUTTON_LEFTSHOULDER ? -1 : 1);
		EnterFocus();
		return;
	case ControllerButton_BUTTON_BACK:
		PerformKeeperAction(KeeperAction::StartRaid, 0);
		return;
	case ControllerButton_BUTTON_START:
		if (modifier && IsEditorTestRun()) {
			EditorEndTestRun();
			return;
		}
		// Not in the middle of a drag: its end would go to the menu.
		if (leftDown || rightDown)
			return;
		LastPlayerAction = PlayerActionType::None;
		gamemenu_on();
		return;
	case ControllerButton_BUTTON_LEFTSTICK:
		PadToggleZoom();
		return;
	case ControllerButton_BUTTON_RIGHTSTICK:
		PerformKeeperAction(jumpToVault ? KeeperAction::CenterVault : KeeperAction::CenterEntrance, 0);
		jumpToVault = !jumpToVault;
		return;
	default:
		// The D-pad is polled.
		return;
	}
}

bool IsControllerEvent(const SDL_Event &event)
{
	switch (event.type) {
	case SDL_EVENT_GAMEPAD_AXIS_MOTION:
	case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
	case SDL_EVENT_GAMEPAD_BUTTON_UP:
	case SDL_EVENT_JOYSTICK_AXIS_MOTION:
	case SDL_EVENT_JOYSTICK_BALL_MOTION:
	case SDL_EVENT_JOYSTICK_HAT_MOTION:
	case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
	case SDL_EVENT_JOYSTICK_BUTTON_UP:
		return true;
	default:
		return false;
	}
}

} // namespace

#ifdef KEEPER_DEVCTL
bool DevPadEngineOnly = false;
bool DevTvUiScale = false;
#endif

int KeeperUiScale()
{
#ifdef __SWITCH__
	constexpr bool Tv = true;
#elif defined(KEEPER_DEVCTL)
	const bool Tv = DevTvUiScale;
#else
	constexpr bool Tv = false;
#endif
	return std::clamp((gnScreenHeight + (Tv && gnScreenHeight >= TvDoubleHeight ? 720 : 0)) / 720, 1, 4);
}

bool IsPadInUse()
{
	return ControlDevice == ControlTypes::Gamepad;
}

bool PadOwnsSticks()
{
#ifdef KEEPER_DEVCTL
	if (DevPadEngineOnly)
		return false;
#endif
	return IsActive();
}

std::string InputName(PadAction action)
{
	if (!IsPadInUse()) {
		switch (action) {
		case PadAction::Confirm:
			return "Click";
		case PadAction::Cancel:
		case PadAction::Menu:
			return "Esc";
		case PadAction::Order:
			return "Right-click";
		case PadAction::Waypoint:
			return "Shift+right-click";
		case PadAction::Possess:
			return KeyLabel(KeeperAction::Possess);
		case PadAction::Sell:
			return KeyLabel(KeeperAction::Sell);
		case PadAction::PrevTab:
		case PadAction::NextTab:
			return KeyLabel(KeeperAction::NextTab);
		case PadAction::StartRaid:
			return KeyLabel(KeeperAction::StartRaid);
		case PadAction::Zoom:
			return "Wheel";
		case PadAction::Jump:
			return KeyLabel(KeeperAction::CenterEntrance);
		case PadAction::Navigate:
		case PadAction::MoveCursor:
			return "Mouse";
		case PadAction::MoveCamera:
			return fmt::format("{}{}{}{}", KeyLabel(KeeperAction::CameraUp), KeyLabel(KeeperAction::CameraLeft), KeyLabel(KeeperAction::CameraDown), KeyLabel(KeeperAction::CameraRight));
		case PadAction::SpellModifier:
			return {};
		case PadAction::Modifier:
			return "Shift";
		case PadAction::EndTest:
			return "F10";
		}
		return {};
	}
	switch (action) {
	case PadAction::Confirm:
		return Combo(ControllerButton_NONE, ControllerButton_BUTTON_A);
	case PadAction::Cancel:
		return Combo(ControllerButton_NONE, ControllerButton_BUTTON_B);
	case PadAction::Order:
		return Combo(ControllerButton_NONE, ControllerButton_BUTTON_X);
	case PadAction::Waypoint:
		return Combo(ModifierButton, ControllerButton_BUTTON_X);
	case PadAction::Possess:
		return Combo(ControllerButton_NONE, ControllerButton_BUTTON_Y);
	case PadAction::Sell:
		return Combo(ModifierButton, ControllerButton_BUTTON_B);
	case PadAction::PrevTab:
		return Combo(ControllerButton_NONE, ControllerButton_BUTTON_LEFTSHOULDER);
	case PadAction::NextTab:
		return Combo(ControllerButton_NONE, ControllerButton_BUTTON_RIGHTSHOULDER);
	case PadAction::StartRaid:
		return Combo(ControllerButton_NONE, ControllerButton_BUTTON_BACK);
	case PadAction::Menu:
		return Combo(ControllerButton_NONE, ControllerButton_BUTTON_START);
	case PadAction::Zoom:
		return Combo(ControllerButton_NONE, ControllerButton_BUTTON_LEFTSTICK);
	case PadAction::Jump:
		return Combo(ControllerButton_NONE, ControllerButton_BUTTON_RIGHTSTICK);
	case PadAction::Navigate:
		return "D-pad";
	case PadAction::MoveCursor:
		return "Left stick";
	case PadAction::MoveCamera:
		return "Right stick";
	case PadAction::SpellModifier:
		return std::string(ToString(GamepadType, SpellButton));
	case PadAction::Modifier:
		return std::string(ToString(GamepadType, ModifierButton));
	case PadAction::EndTest:
		return Combo(ModifierButton, ControllerButton_BUTTON_START);
	}
	return {};
}

std::string InputName(KeeperAction action, size_t index)
{
	if (!IsPadInUse())
		return KeyLabel(action, index);
	switch (action) {
	case KeeperAction::Spell:
		return index < SpellButtons.size() ? Combo(SpellButton, SpellButtons[index]) : std::string();
	case KeeperAction::NextTab:
		return InputName(PadAction::NextTab);
	case KeeperAction::StartRaid:
		return InputName(PadAction::StartRaid);
	case KeeperAction::CenterEntrance:
	case KeeperAction::CenterVault:
		return InputName(PadAction::Jump);
	case KeeperAction::ZoomIn:
	case KeeperAction::ZoomOut:
		return InputName(PadAction::Zoom);
	case KeeperAction::Possess:
		return InputName(PadAction::Possess);
	case KeeperAction::Sell:
		return InputName(PadAction::Sell);
	case KeeperAction::CameraLeft:
	case KeeperAction::CameraRight:
	case KeeperAction::CameraUp:
	case KeeperAction::CameraDown:
		return InputName(PadAction::MoveCamera);
	// Cards have no button of their own: the focus goes to them.
	case KeeperAction::CardSlot:
	case KeeperAction::LockDoor:
	case KeeperAction::Bait:
		return {};
	}
	return {};
}

bool PadOnEvent(const SDL_Event &event, ControllerButtonEvent ctrlEvent, GameAction &action)
{
	if (!PadOwnsSticks())
		return false;
	if (IsEditorActive() && EditorPadOnText(event))
		return true;
	if (!IsControllerEvent(event) || ctrlEvent.button == ControllerButton_IGNORE)
		return false;

	const ControllerButton button = TranslateTo(GamepadType, ctrlEvent.button);
	// The mouse buttons let go whatever else is going on, so a drag can't stick.
	if (ctrlEvent.up && button == ControllerButton_BUTTON_A && leftDown) {
		leftDown = false;
		PressMouse(action, SDL_BUTTON_LEFT, /*up=*/true);
		return true;
	}
	if (ctrlEvent.up && button == ControllerButton_BUTTON_X && rightDown) {
		rightDown = false;
		PressMouse(action, SDL_BUTTON_RIGHT, /*up=*/true);
		return true;
	}
	// Menus have their own controls, and they're the engine's.
	if (InGameMenu())
		return false;

	DetectInputMethod(event, ctrlEvent);
	KeepMousePicking();
	if (!IsControllerMotion(event) && ctrlEvent.button != ControllerButton_NONE && !ctrlEvent.up)
		HandlePress(button, action);
	return true;
}

void PadEveryFrame()
{
	const uint32_t now = SDL_GetTicks();
	const float dt = std::min<uint32_t>(now - lastFrameTicks, 50) / 1000.F;
	lastFrameTicks = now;
	pushX = 0.F;
	pushY = 0.F;
	if (!PadOwnsSticks() || InGameMenu()) {
		UpdateModifier(false);
		return;
	}
	UpdateModifier(IsPadInUse() && IsControllerButtonPressed(ModifierButton));
	if (!IsPadInUse()) {
		focused = false;
		return;
	}
	KeepMousePicking();

	if (IsEditorActive() && EditorPadInText()) {
		const AxisDirection dir = focusRepeater.Get(GetLeftStickOrDpadDirection(false));
		if (dir.x != AxisDirectionX_NONE || dir.y != AxisDirectionY_NONE)
			EditorPadTextStep(dir);
		return;
	}

	if (NeedsFocus() && !focused)
		EnterFocus();

	// A card that arms a tool hands over to the map, ready to place it.
	if (toolBeforeConfirm) {
		const uint32_t stamp = PadToolStamp();
		if (focused && !NeedsFocus() && stamp != 0 && stamp != *toolBeforeConfirm)
			LeaveFocus();
		toolBeforeConfirm = std::nullopt;
	}

	const AxisDirection dir = focusRepeater.Get(DpadDirection());
	const bool pressed = dir.x != AxisDirectionX_NONE || dir.y != AxisDirectionY_NONE;
	if (focused && (leftStickX != 0 || leftStickY != 0) && !NeedsFocus())
		LeaveFocus();
	if (focused) {
		KeepFocusOnSomething();
		if (pressed)
			MoveFocus(dir);
		return;
	}
	if (pressed) {
		EnterFocus();
		return;
	}
	MoveCursor(dt);
}

void PadCameraPull(float &sx, float &sy)
{
	if (!IsPadInUse())
		return;
	sx = std::clamp(sx + rightStickX + pushX, -1.F, 1.F);
	sy = std::clamp(sy - rightStickY + pushY, -1.F, 1.F);
}

void PadAfterCursMove()
{
	if (!PadOwnsSticks() || !IsPadInUse() || focused || pcursmonst >= 0 || PlayerUnderCursor != nullptr || cursPosition.x < 0 || IsEditorActive())
		return;
	int best = SoftTargetTiles + 1;
	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		const Monster &monster = Monsters[ActiveMonsters[i]];
		if (monster.hitPoints <= 0 || monster.mode == MonsterMode::Death || monster.type().type == MT_GOLEM)
			continue;
		const int distance = monster.position.tile.WalkingDistance(cursPosition);
		if (distance < best) {
			best = distance;
			pcursmonst = static_cast<int>(ActiveMonsters[i]);
		}
	}
	for (size_t i = 0; i < MaxBots; i++) {
		const Player &bot = Players[FirstBotId + i];
		if (!IsLivingBot(bot))
			continue;
		const int distance = bot.position.tile.WalkingDistance(cursPosition);
		if (distance < best) {
			best = distance;
			pcursmonst = -1;
			PlayerUnderCursor = &bot;
		}
	}
}

bool PadHidesEngineCursor()
{
	return PadOwnsSticks() && IsPadInUse();
}

void PadDrawCursor(const Surface &out)
{
	if (!PadOwnsSticks() || !IsPadInUse() || focused || InGameMenu() || pcurs <= CURSOR_NONE)
		return;
	const ClxSprite sprite = GetInvItemSprite(pcurs);
	ClxDraw(out, MousePosition + Displacement { 0, sprite.height() - 1 }, sprite);
}

void PadDrawOverlay(const Surface &out)
{
	if (!PadOwnsSticks() || !IsPadInUse() || InGameMenu())
		return;
	if (focused)
		PadDrawFocusFrame(out, focusRect);
	if (IsEditorActive())
		PadDrawPromptStrip(out, EditorPadPrompt(focused), 4);
	else
		PadDrawPromptStrip(out, PadHudPrompt(focused), IsEditorTestRun() ? 24 : 4);
}

std::optional<size_t> PadStepFocus(const std::vector<Rectangle> &targets, const Rectangle &from, AxisDirection dir)
{
	const bool horizontal = dir.y == AxisDirectionY_NONE;
	const int sign = horizontal ? (dir.x == AxisDirectionX_RIGHT ? 1 : -1) : (dir.y == AxisDirectionY_DOWN ? 1 : -1);
	const Point fromCenter = from.Center();
	std::optional<size_t> best;
	int bestScore = std::numeric_limits<int>::max();
	for (size_t i = 0; i < targets.size(); i++) {
		const Rectangle &target = targets[i];
		const Point center = target.Center();
		const int along = sign * (horizontal ? center.x - fromCenter.x : center.y - fromCenter.y);
		if (along <= 0)
			continue;
		// How far off to the side, not counting any overlap: a wide button under a narrow card is straight down.
		const int low = horizontal ? std::max(target.position.y, from.position.y) : std::max(target.position.x, from.position.x);
		const int high = horizontal ? std::min(target.position.y + target.size.height, from.position.y + from.size.height) : std::min(target.position.x + target.size.width, from.position.x + from.size.width);
		const int aside = low - high;
		int score = along;
		if (aside >= 0) {
			// Off to the side: only as a last resort, and only if it is clear past this one.
			const int gap = horizontal
			    ? (sign > 0 ? target.position.x - (from.position.x + from.size.width) : from.position.x - (target.position.x + target.size.width))
			    : (sign > 0 ? target.position.y - (from.position.y + from.size.height) : from.position.y - (target.position.y + target.size.height));
			if (gap < 0)
				continue;
			score += OffToTheSide + 3 * aside;
		}
		if (score < bestScore) {
			bestScore = score;
			best = i;
		}
	}
	return best;
}

std::optional<size_t> PadNearestTarget(const std::vector<Rectangle> &targets, Point point)
{
	std::optional<size_t> best;
	int bestDistance = std::numeric_limits<int>::max();
	for (size_t i = 0; i < targets.size(); i++) {
		if (targets[i].contains(point))
			return i;
		const Point center = targets[i].Center();
		const int distance = std::abs(center.x - point.x) + std::abs(center.y - point.y);
		if (distance < bestDistance) {
			bestDistance = distance;
			best = i;
		}
	}
	return best;
}

void PadDrawFocusFrame(const Surface &out, const Rectangle &rect)
{
	// Pulses, so it stands out from the selected card's gold frame.
	const uint8_t color = (SDL_GetTicks() / 250) % 2 == 0 ? PAL16_YELLOW : PAL16_ORANGE + 2;
	DrawFrame(out, { rect.position - Displacement { 1, 1 }, { rect.size.width + 2, rect.size.height + 2 } }, color);
	DrawFrame(out, { rect.position - Displacement { 2, 2 }, { rect.size.width + 4, rect.size.height + 4 } }, color);
}

void PadDrawPromptStrip(const Surface &out, std::string_view text, int y)
{
	const std::string fitted = FitText(text, out.w() - 16);
	const int width = GetLineWidth(fitted) + 16;
	const Rectangle strip { { (out.w() - width) / 2, y }, { width, 18 } };
	DrawHalfTransparentRectTo(out, strip.position.x, strip.position.y, strip.size.width, strip.size.height);
	DrawHalfTransparentRectTo(out, strip.position.x, strip.position.y, strip.size.width, strip.size.height);
	DrawString(out, fitted, { strip.position + Displacement { 0, 2 }, { strip.size.width, 14 } }, { .flags = UiFlags::FontSize12 | UiFlags::AlignCenter | UiFlags::ColorWhite });
}

} // namespace devilution::keeper
