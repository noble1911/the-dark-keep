/**
 * @file keeper/keeper_editor.hpp
 *
 * The map editor: paints a layout on the real level in the real renderer, using the keep's free
 * camera. keeper.cpp routes input and drawing here while CurrentSetup().mode is MapEditor.
 */
#pragma once

#ifdef USE_SDL3
#include <SDL3/SDL_keycode.h>
#else
#include <SDL.h>
#endif

#include <string>
#include <vector>

#include "controls/axis_direction.h"
#include "controls/controller_buttons.h"
#include "engine/rectangle.hpp"
#include "engine/surface.hpp"

namespace devilution::keeper {

/** The keep level is up and it's a map being edited (not a test run of it). */
bool IsEditorActive();
/** Typing a name, or holding Ctrl: WASD shouldn't move the camera. */
bool EditorCapturesKeys();

/** PrepareMapLevel, in editor mode: starts the session on first use and publishes the live map. */
void EditorPrepareSession();
void EditorOnLevelLoaded();
void EditorOnGameEnded();
/** The game is about to be freed while the level is still whole: last chance to autosave. */
void EditorBeforeGameFreed();
/** Once per rendered frame: drag strokes, and rebuilding the level after a change. */
void EditorUpdateFrame();

bool EditorOnKeyDown(SDL_Keycode key);
/** During a test run: F10 goes back to the editor. @return true if consumed. */
bool EditorOnTestKey(SDL_Keycode key);
/** A map from the editor is being played. */
bool IsEditorTestRun();
/** Ends the test run and goes back to the editor with the map as it was. */
void EditorEndTestRun();
bool EditorOnMouseDown(bool right);
bool EditorOnMouseWheel(int deltaY);

/** Controller: what can be pressed, in UI pixels. */
void EditorPadTargets(std::vector<Rectangle> &targets);
/** A list is up: only its rows can be pressed. */
bool EditorPadNeedsFocus();
/** Scrolls a list when the focus is at its end. @return true if it moved. */
bool EditorPadScroll(const Rectangle &focused, AxisDirection dir);
/** A button the editor has its own use for. `button` is as labelled on the controller. @return true if consumed. */
bool EditorPadButton(ControllerButton button, bool modifier, bool spellButton);
/** A name is being typed. */
bool EditorPadInText();
/** Stick or D-pad while a name is being typed: up and down change the letter, right adds one, left removes one. */
void EditorPadTextStep(AxisDirection dir);
/** Text coming back from the Switch's on-screen keyboard. @return true if consumed. */
bool EditorPadOnText(const SDL_Event &event);
/** What the buttons do right now, for the line of prompts. */
std::string EditorPadPrompt(bool focused);

/** Grid, markers and brush preview, at full resolution over the level. */
void EditorDrawWorld(const Surface &out);
/** The room the editor's strip takes at the foot of the screen, in UI pixels. */
int EditorStripHeight();
/** The tool panel, in UI pixels. */
void EditorDrawUi(const Surface &out);
/** During a test run: says how to get back. In UI pixels. */
void EditorDrawTestBanner(const Surface &out);

} // namespace devilution::keeper
