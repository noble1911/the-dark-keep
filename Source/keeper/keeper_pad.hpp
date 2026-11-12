/**
 * @file keeper/keeper_pad.hpp
 *
 * Controller support for the keep. A controller drives the same code paths as the mouse: the left stick
 * moves the mouse position, the D-pad snaps it between the things on screen you can press (the "focus"),
 * and A and X are the left and right mouse buttons. So anything the mouse can do, a controller can do,
 * and a new button only has to be listed as a focus target.
 *
 * InputName() is for anyone writing on-screen text: it names the button or key for an action, for
 * whichever device is in use.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifdef USE_SDL3
#include <SDL3/SDL_events.h>
#else
#include <SDL.h>
#endif

#include "controls/axis_direction.h"
#include "controls/controller_buttons.h"
#include "controls/game_controls.h"
#include "engine/point.hpp"
#include "engine/rectangle.hpp"
#include "engine/surface.hpp"

namespace devilution::keeper {

enum class KeeperAction : uint8_t;

/** What a player does, whatever they do it with. */
enum class PadAction : uint8_t {
	/** Left click, or press what has the focus. */
	Confirm,
	/** Backs out one step: the focus, a tool, a selection, a panel. */
	Cancel,
	/** Right click: rally the selected monsters, or a right-drag in the editor. */
	Order,
	/** Shift+right click: add a patrol waypoint. */
	Waypoint,
	Possess,
	Sell,
	PrevTab,
	NextTab,
	StartRaid,
	Menu,
	Zoom,
	/** Jumps the view between the entrance and the vault. */
	Jump,
	/** Moves the focus. */
	Navigate,
	MoveCursor,
	MoveCamera,
	/** Hold, with a face or shoulder button, to cast. */
	SpellModifier,
	/** Shift: the editor's tools, and with the buttons for a waypoint and for selling. */
	Modifier,
	/** Back to the editor from a test run. */
	EndTest,
};

/** The last input came from a controller. */
bool IsPadInUse();
/** The keep is up and uses the sticks itself: the engine's are for walking a hero. */
bool PadOwnsSticks();
#ifdef KEEPER_DEVCTL
/** Dev/test: hands the controller back to the engine, to see what it would do without all this. */
extern bool DevPadEngineOnly;
/** Dev/test: the Switch's UI scale, on a computer. */
extern bool DevTvUiScale;
#endif

/**
 * @brief The name of the button or key for an action, for whichever device is in use: "A" or "Click",
 * "ZL+X" or "Shift+right-click". Controller buttons come as the layout's glyphs (Switch, Xbox,
 * PlayStation), which DrawString draws in the 12, 24 and 30 point fonts.
 */
std::string InputName(PadAction action);
/** The glyph or name for a button, given by what's printed on an Xbox pad: A is the button labelled A, wherever the layout puts it. */
std::string PadButtonName(ControllerButton button);
/** The same for a rebindable keeper action: its key, or its button. Empty if nothing does it directly. */
std::string InputName(KeeperAction action, size_t index = 0);

/**
 * @brief Runs a controller event in the keep. GameEventHandler calls this ahead of the engine's own
 * controller handling, which is all about walking a hero.
 * @param action Set to a mouse button for GameEventHandler to press, exactly as a real one.
 * @return true if the event was dealt with: the engine shouldn't see it.
 */
bool PadOnEvent(const SDL_Event &event, ControllerButtonEvent ctrlEvent, GameAction &action);
/** Whether the engine should leave the keep's mouse picking alone for this event. */
bool PadKeepsMouseMode(const SDL_Event &event);
/** The keep is (re)loaded: nothing carries over but buttons that are still held. */
void PadOnLevelLoaded();
/** The game is over: nothing carries over to the next. */
void PadOnGameEnded();
/** Each frame: moves the cursor and the focus, which are polled. */
void PadEveryFrame();
/** After the engine has picked what's under the cursor: a stick can't aim like a mouse, so nearby counts too. */
void PadAfterCursMove();
/** The keep draws its own cursor for a controller: the hardware one stays with the real mouse. */
bool PadHidesEngineCursor();
/** Draws the cursor, in screen pixels. */
void PadDrawCursor(const Surface &out);
/** The focus frame and the line of prompts, in UI pixels. */
void PadDrawOverlay(const Surface &out);
/** The sticks' pull on the camera this frame: the right stick, and the cursor pushing at the edge of the view. */
void PadCameraPull(float &sx, float &sy);

/**
 * How many screen pixels a UI pixel is, for the HUD and the keeper screen: 2x from 1440p, 3x at 4K. A
 * Switch is across the room on a TV, where a 12-pixel font at 1080p can't be read, so it is 2x from 1080p.
 */
int KeeperUiScale();

// ---- shared with the keeper screen, which runs its own loop ----

/** The target to go to from `from` in direction `dir`, if there is one. */
std::optional<size_t> PadStepFocus(const std::vector<Rectangle> &targets, const Rectangle &from, AxisDirection dir);
/** The index of the target holding `point`, or else the nearest. nullopt if there are none. */
std::optional<size_t> PadNearestTarget(const std::vector<Rectangle> &targets, Point point);
/** A frame around what has the focus. */
void PadDrawFocusFrame(const Surface &out, const Rectangle &rect);
/** A line of prompts, on a dark strip. */
void PadDrawPromptStrip(const Surface &out, std::string_view text, int y);

// ---- defined in keeper.cpp, which owns the HUD ----

/** Everything that can be pressed right now, in UI pixels. */
void PadHudTargets(std::vector<Rectangle> &targets);
/** Something is up that takes all input (the end of the game, a joiner's spell panel). */
bool PadHudNeedsFocus();
/** Scrolls the card row, when the focus is at its end. @return true if it moved. */
bool PadHudScroll(const Rectangle &focused, AxisDirection dir);
/** Moves `delta` tabs along. @return where the tab's first card is. */
Rectangle PadSwitchTab(int delta);
/** The other button on a focused target: on a family's card, its next tier. */
void PadHudSecondary(const Rectangle &focused);
void PadToggleZoom();
/** Identifies the armed tool; 0 if there is none. */
uint32_t PadToolStamp();
/** The bottom of the world view, in screen pixels. */
int PadViewBottom();
/** What the buttons do right now, for the line of prompts. */
std::string PadHudPrompt(bool focused);

} // namespace devilution::keeper
