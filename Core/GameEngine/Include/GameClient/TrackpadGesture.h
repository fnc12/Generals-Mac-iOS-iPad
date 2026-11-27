/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// GeneralsX @feature fnc12 27/11/2026 Native macOS trackpad camera control.
//
// The decisions behind trackpad camera control, as small pure functions so the unit tests
// (Tests/Catch2/trackpad_gesture_tests.cpp) can check them without the game:
//
//   * which scroll events are a trackpad pan (precise deltas) and which stay a mouse wheel zoom;
//   * who gets a gesture: a scrolling list box under the cursor, the camera, or nobody;
//   * how a pan of the fingers turns into a camera move and a pinch into a zoom.
//
// Everything here is client-side camera state only. Nothing reaches game logic, the network,
// the recorder or the CRC.
//
// This header is standalone (no engine types) on purpose.

#pragma once

#include <cstdlib>

namespace TrackpadGesture
{

// Kinds of gesture carried by MSG_RAW_MOUSE_WHEEL argument 3. The values equal MOUSE_GESTURE_* in
// GameClient/Mouse.h. A plain mouse wheel message has only 3 arguments and no kind.
enum Kind
{
	KIND_NONE = 0,
	KIND_PAN = 1,    ///< two-finger swipe; x/y = screen pixels the content should follow
	KIND_PINCH = 2,  ///< pinch; x = scale change since the last update (> 1 = spread = zoom in)
};

// Who handles a gesture.
enum Route
{
	ROUTE_IGNORE,  ///< nobody: destroy the message (shell, menus, disabled input, pinch over a list)
	ROUTE_GUI,     ///< the list box under the cursor scrolls
	ROUTE_CAMERA,  ///< the tactical view pans or zooms
};

struct Context
{
	bool shellActive;          ///< main menu / shell screens are up
	bool inputEnabled;         ///< TheInGameUI accepts input (false in cinematics etc.)
	bool modalWindowActive;    ///< a modal window (in-game options, message box) is up
	bool overScrollableGui;    ///< the cursor is over a list box (or a window inside one)
};

/// A scroll event from the OS is a trackpad pan when the device reports precise (pixel) deltas
/// (NSEvent.hasPreciseScrollingDeltas on macOS: trackpads, Magic Mouse). A classic line-based
/// mouse wheel is not precise and keeps zooming exactly as before.
inline bool isPanScroll(bool hasPreciseScrollingDeltas)
{
	return hasPreciseScrollingDeltas;
}

/// A precise event with no movement (gesture begin/end markers) carries nothing to do.
inline bool hasPanMovement(float dx, float dy)
{
	return dx != 0.0f || dy != 0.0f;
}

inline Route route(Kind kind, const Context &c)
{
	if (kind == KIND_PAN)
	{
		// A list box under the cursor scrolls, in game and in the shell alike.
		if (c.overScrollableGui)
			return ROUTE_GUI;
	}
	else if (kind == KIND_PINCH)
	{
		// Pinching over a list does nothing (it neither zooms the list nor the map behind it).
		if (c.overScrollableGui)
			return ROUTE_IGNORE;
	}
	else
	{
		return ROUTE_IGNORE;
	}

	// The camera only moves in game, with the UI accepting input and no modal window up.
	if (c.shellActive || !c.inputEnabled || c.modalWindowActive)
		return ROUTE_IGNORE;

	return ROUTE_CAMERA;
}

/// Turns a pan over a list box into whole wheel steps. contentDy is how far (game pixels) the
/// content should move: > 0 = down (show earlier rows) = wheel up = positive steps. The remainder
/// is kept in accum so slow swipes still scroll; reversing direction drops the old remainder.
inline int guiWheelSteps(float &accum, float contentDy, float pixelsPerStep)
{
	if (!(pixelsPerStep > 0.0f))
		return 0;

	if ((accum > 0.0f && contentDy < 0.0f) || (accum < 0.0f && contentDy > 0.0f))
		accum = 0.0f;

	accum += contentDy;
	const int steps = (int)(accum / pixelsPerStep);  // truncates toward zero
	accum -= steps * pixelsPerStep;
	return steps;
}

struct Vec2
{
	float x;
	float y;
};

/// Camera move for a pan. worldPerPixelX/Y are the world offsets of one screen pixel to the right
/// and one pixel down at the cursor (measured on the terrain). For the map to follow the fingers
/// the terrain point under the cursor must move by (dx, dy) pixels, so the camera moves the
/// opposite way. speed 1 = exactly under the fingers.
inline Vec2 panCameraDelta(Vec2 worldPerPixelX, Vec2 worldPerPixelY, float dx, float dy, float speed)
{
	Vec2 d;
	d.x = -(worldPerPixelX.x * dx + worldPerPixelY.x * dy) * speed;
	d.y = -(worldPerPixelX.y * dx + worldPerPixelY.y * dy) * speed;
	return d;
}

/// One world-per-pixel column of the pan Jacobian, from two terrain points probePixels apart.
inline Vec2 worldPerPixel(Vec2 worldAtAnchor, Vec2 worldAtProbe, float probePixels)
{
	Vec2 d = { 0.0f, 0.0f };
	if (probePixels != 0.0f)
	{
		d.x = (worldAtProbe.x - worldAtAnchor.x) / probePixels;
		d.y = (worldAtProbe.y - worldAtAnchor.y) / probePixels;
	}
	return d;
}

/// Change of the camera height for one pinch update. Spreading the fingers (scale > 1) brings the
/// camera closer in proportion, like zooming a map: scale 2 halves the height. A single update is
/// clamped so a glitchy event cannot jump the camera.
inline float pinchZoomHeightDelta(float currentHeight, float scale)
{
	if (!(scale > 0.0f) || !(currentHeight > 0.0f))
		return 0.0f;
	if (scale < 0.5f)
		scale = 0.5f;
	else if (scale > 2.0f)
		scale = 2.0f;
	return currentHeight * (1.0f / scale - 1.0f);
}

// GeneralsX @feature fnc12 27/11/2026 Pinch zoom anchored at the cursor.
//
// A pinch zooms about the point under the cursor, like maps in every Mac app: the terrain point
// under the cursor stays under it. The view does it in three steps (W3DView::zoomAt):
//   1. pick the terrain point A under the cursor with the camera as it is;
//   2. zoom (the new height takes effect at once for a pinch, no easing), rebuild the camera;
//   3. cast the cursor's pick ray again and cross it with the horizontal plane at A's height:
//      that is where A would have to be to sit under the cursor now. The camera only translates in
//      x/y, which moves that crossing by the same offset, so moving it by A - crossing puts A
//      exactly back under the cursor, on hills too (it is a plane through A, not the terrain).

struct Vec3
{
	float x;
	float y;
	float z;
};

/// Where the segment start -> end (a pick ray) crosses the horizontal plane at height z. FALSE when
/// it runs parallel to the plane or does not reach it (the plane is behind the start or past the end),
/// e.g. the cursor looks at the sky.
inline bool segmentAtHeight(Vec3 start, Vec3 end, float z, Vec2 &out)
{
	const float dz = end.z - start.z;
	if (dz > -1e-6f && dz < 1e-6f)
		return false;
	const float t = (z - start.z) / dz;
	if (!(t >= 0.0f && t <= 1.0f))
		return false;
	out.x = start.x + (end.x - start.x) * t;
	out.y = start.y + (end.y - start.y) * t;
	return true;
}

/// Camera move that brings the anchor back under the cursor after a zoom. anchorWorld = the point
/// that was under the cursor before; rayAtAnchorHeight = where the cursor's ray crosses the anchor's
/// height after the zoom (segmentAtHeight).
inline Vec2 pinchAnchorCameraDelta(Vec2 anchorWorld, Vec2 rayAtAnchorHeight)
{
	Vec2 d;
	d.x = anchorWorld.x - rayAtAnchorHeight.x;
	d.y = anchorWorld.y - rayAtAnchorHeight.y;
	return d;
}

/// The camera is moved sideways only when the zoom really changed the height. At the minimum or
/// maximum height the clamp swallows the pinch; moving the camera then would make it drift.
inline bool pinchChangedHeight(float heightBefore, float heightAfter)
{
	const float d = heightAfter - heightBefore;
	return d > 1e-3f || d < -1e-3f;
}

/// Debug log of gestures and camera moves on stderr, off by default. Enable with the environment
/// variable GENERALSX_TRACKPAD_LOG=1.
inline bool logEnabled()
{
	static const bool enabled = []() {
		const char *value = std::getenv("GENERALSX_TRACKPAD_LOG");
		return value != nullptr && value[0] != '\0' && value[0] != '0';
	}();
	return enabled;
}

/// Testing aid: a pinch cannot be synthesized from outside the game, so the environment variable
/// GENERALSX_TRACKPAD_TEST_PINCH="scale,frames[,x,y]" makes the game pinch by itself once it is in
/// a match: one pinch update of `scale` per frame for `frames` frames, then the same back with
/// 1/scale, at game pixel (x, y) (default: the cursor). Off (and never parsed) without the variable.
struct TestPinch
{
	float scale;
	int frames;
	int x;
	int y;
	bool atCursor;
};

inline bool parseTestPinch(const char *value, TestPinch &out)
{
	if (value == nullptr || value[0] == '\0')
		return false;
	char *end = nullptr;
	out.scale = std::strtof(value, &end);
	if (end == value || !(out.scale > 0.0f) || *end != ',')
		return false;
	const char *p = end + 1;
	out.frames = (int)std::strtol(p, &end, 10);
	if (end == p || out.frames <= 0)
		return false;
	out.atCursor = true;
	out.x = out.y = 0;
	if (*end == ',')
	{
		p = end + 1;
		out.x = (int)std::strtol(p, &end, 10);
		if (end == p || *end != ',')
			return false;
		p = end + 1;
		out.y = (int)std::strtol(p, &end, 10);
		if (end == p)
			return false;
		out.atCursor = false;
	}
	return *end == '\0';
}

/// Converts a delta in OS window points into game pixels (the game renders at its own internal
/// resolution, aspect-fit into the window).
inline float pointsToGamePixels(float points, float gamePixels, float viewportPoints)
{
	if (!(viewportPoints > 0.0f))
		return points;
	return points * gamePixels / viewportPoints;
}

} // namespace TrackpadGesture
