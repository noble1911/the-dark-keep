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
/** Once per rendered frame: drag strokes, and rebuilding the level after a change. */
void EditorUpdateFrame();

bool EditorOnKeyDown(SDL_Keycode key);
/** During a test run: F10 goes back to the editor. @return true if consumed. */
bool EditorOnTestKey(SDL_Keycode key);
bool EditorOnMouseDown(bool right);
bool EditorOnMouseWheel(int deltaY);

/** Grid, markers and brush preview, at full resolution over the level. */
void EditorDrawWorld(const Surface &out);
/** The tool panel, in UI pixels. */
void EditorDrawUi(const Surface &out);
/** During a test run: says how to get back. In UI pixels. */
void EditorDrawTestBanner(const Surface &out);

} // namespace devilution::keeper
