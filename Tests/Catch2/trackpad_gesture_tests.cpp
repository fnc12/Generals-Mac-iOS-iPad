// GeneralsX @feature fnc12 27/11/2026 The decisions behind native macOS trackpad camera control
// (Core/GameEngine/Include/GameClient/TrackpadGesture.h): trackpad vs mouse wheel, who gets a
// gesture (list box, camera, nobody), pan -> camera move, pinch -> zoom.
// GeneralsX @feature fnc12 27/11/2026 Pinch zoom anchored at the cursor.
#include <catch2/catch_all.hpp>

#include "GameClient/TrackpadGesture.h"

using namespace TrackpadGesture;
using Catch::Approx;

namespace {
    Context inGame() {
        Context c;
        c.shellActive = false;
        c.inputEnabled = true;
        c.modalWindowActive = false;
        c.overScrollableGui = false;
        return c;
    }
}

TEST_CASE("Trackpad: precise scrolls pan, line-based wheels keep zooming") {
    CHECK(isPanScroll(true));
    CHECK_FALSE(isPanScroll(false));
    CHECK(hasPanMovement(0.0f, -3.5f));
    CHECK(hasPanMovement(2.0f, 0.0f));
    CHECK_FALSE(hasPanMovement(0.0f, 0.0f));  // gesture begin/end markers carry no movement
}

TEST_CASE("Trackpad: routing in game") {
    const Context c = inGame();
    CHECK(route(KIND_PAN, c) == ROUTE_CAMERA);
    CHECK(route(KIND_PINCH, c) == ROUTE_CAMERA);
    CHECK(route(KIND_NONE, c) == ROUTE_IGNORE);
}

TEST_CASE("Trackpad: a list box under the cursor takes the swipe, never the pinch") {
    Context c = inGame();
    c.overScrollableGui = true;
    CHECK(route(KIND_PAN, c) == ROUTE_GUI);
    CHECK(route(KIND_PINCH, c) == ROUTE_IGNORE);

    // Lists scroll in the shell and in modal menus too.
    c.shellActive = true;
    CHECK(route(KIND_PAN, c) == ROUTE_GUI);
    c.shellActive = false;
    c.modalWindowActive = true;
    CHECK(route(KIND_PAN, c) == ROUTE_GUI);
}

TEST_CASE("Trackpad: the shell, modal windows and disabled input never move the camera") {
    for (int kind = KIND_PAN; kind <= KIND_PINCH; ++kind) {
        CAPTURE(kind);
        Context c = inGame();
        c.shellActive = true;
        CHECK(route((Kind)kind, c) == ROUTE_IGNORE);

        c = inGame();
        c.modalWindowActive = true;
        CHECK(route((Kind)kind, c) == ROUTE_IGNORE);

        c = inGame();
        c.inputEnabled = false;
        CHECK(route((Kind)kind, c) == ROUTE_IGNORE);
    }
}

TEST_CASE("Trackpad: list box rows follow the fingers in whole steps") {
    float accum = 0.0f;
    // Content moving down (fingers down with natural scrolling) = wheel up = earlier rows.
    CHECK(guiWheelSteps(accum, 45.0f, 20.0f) == 2);
    CHECK(accum == Approx(5.0f));
    // Slow swipes add up.
    CHECK(guiWheelSteps(accum, 10.0f, 20.0f) == 0);
    CHECK(guiWheelSteps(accum, 6.0f, 20.0f) == 1);
    CHECK(accum == Approx(1.0f));
    // Reversing drops the old remainder.
    CHECK(guiWheelSteps(accum, -19.0f, 20.0f) == 0);
    CHECK(accum == Approx(-19.0f));
    CHECK(guiWheelSteps(accum, -2.0f, 20.0f) == -1);
    CHECK(accum == Approx(-1.0f));
    // Nonsense step size does nothing.
    CHECK(guiWheelSteps(accum, 100.0f, 0.0f) == 0);
}

TEST_CASE("Trackpad: pan moves the camera so the terrain follows the fingers") {
    // Top-down camera, 2 world units per pixel, screen x = world x, screen y (down) = world -y.
    const Vec2 perX = { 2.0f, 0.0f };
    const Vec2 perY = { 0.0f, -2.0f };

    // Fingers (content) move 10 px right: the camera moves 20 world units left.
    Vec2 d = panCameraDelta(perX, perY, 10.0f, 0.0f, 1.0f);
    CHECK(d.x == Approx(-20.0f));
    CHECK(d.y == Approx(0.0f));

    // Content moves 5 px down: the camera moves north (+y world) by 10.
    d = panCameraDelta(perX, perY, 0.0f, 5.0f, 1.0f);
    CHECK(d.x == Approx(0.0f));
    CHECK(d.y == Approx(10.0f));

    // The terrain point under the cursor ends up exactly dx/dy pixels away.
    const float dx = 7.0f, dy = -3.0f;
    d = panCameraDelta(perX, perY, dx, dy, 1.0f);
    const float terrainMovedX = -d.x, terrainMovedY = -d.y;  // relative to the camera
    CHECK(terrainMovedX == Approx(perX.x * dx + perY.x * dy));
    CHECK(terrainMovedY == Approx(perX.y * dx + perY.y * dy));

    // A rotated camera (screen right = world +y) is handled by the measured Jacobian.
    const Vec2 rotX = { 0.0f, 1.5f };
    const Vec2 rotY = { 1.5f, 0.0f };
    d = panCameraDelta(rotX, rotY, 4.0f, 0.0f, 1.0f);
    CHECK(d.x == Approx(0.0f));
    CHECK(d.y == Approx(-6.0f));

    // speed scales linearly.
    d = panCameraDelta(perX, perY, 10.0f, 0.0f, 0.5f);
    CHECK(d.x == Approx(-10.0f));
}

TEST_CASE("Trackpad: the pan Jacobian from two terrain probes") {
    const Vec2 a = { 100.0f, 200.0f };
    const Vec2 b = { 164.0f, 200.0f };
    Vec2 j = worldPerPixel(a, b, 32.0f);
    CHECK(j.x == Approx(2.0f));
    CHECK(j.y == Approx(0.0f));
    // Probing to the left (negative step) gives the same per-pixel slope.
    const Vec2 c = { 36.0f, 200.0f };
    j = worldPerPixel(a, c, -32.0f);
    CHECK(j.x == Approx(2.0f));
    j = worldPerPixel(a, b, 0.0f);
    CHECK(j.x == 0.0f);
    CHECK(j.y == 0.0f);
}

TEST_CASE("Trackpad: pinch zooms like any Mac app") {
    // Spreading the fingers (scale > 1) lowers the camera = zoom in; scale 2 halves the height.
    CHECK(pinchZoomHeightDelta(300.0f, 2.0f) == Approx(-150.0f));
    CHECK(pinchZoomHeightDelta(300.0f, 1.1f) < 0.0f);
    // Pinching in (scale < 1) raises it = zoom out.
    CHECK(pinchZoomHeightDelta(300.0f, 0.5f) == Approx(300.0f));
    CHECK(pinchZoomHeightDelta(300.0f, 0.9f) > 0.0f);
    CHECK(pinchZoomHeightDelta(300.0f, 1.0f) == Approx(0.0f));
    // A spread then the same pinch back ends where it started.
    float h = 300.0f;
    h += pinchZoomHeightDelta(h, 1.25f);
    h += pinchZoomHeightDelta(h, 0.8f);
    CHECK(h == Approx(300.0f));
    // Glitchy values are clamped or ignored.
    CHECK(pinchZoomHeightDelta(300.0f, 50.0f) == Approx(-150.0f));
    CHECK(pinchZoomHeightDelta(300.0f, 0.01f) == Approx(300.0f));
    CHECK(pinchZoomHeightDelta(300.0f, 0.0f) == 0.0f);
    CHECK(pinchZoomHeightDelta(300.0f, -1.0f) == 0.0f);
    CHECK(pinchZoomHeightDelta(0.0f, 2.0f) == 0.0f);
}

namespace {
    // A pinhole camera for the anchoring tests: the zoom scales the camera's distance to the pivot P
    // (on the ground) by k and keeps its orientation, so the ray through a screen pixel keeps its
    // direction and only its start moves.
    Vec3 scaleAbout(Vec3 p, Vec3 c, float k) {
        Vec3 r = { p.x + (c.x - p.x) * k, p.y + (c.y - p.y) * k, p.z + (c.z - p.z) * k };
        return r;
    }
    Vec3 along(Vec3 start, Vec3 dir, float t) {
        Vec3 r = { start.x + dir.x * t, start.y + dir.y * t, start.z + dir.z * t };
        return r;
    }

    // Zooms by k with the anchor pixel's ray direction dir, anchors, and returns where the ray of
    // that pixel then crosses the anchor's height (must be the anchor again).
    Vec2 zoomAndAnchor(Vec3 pivot, Vec3 camera, Vec3 anchor, float k, Vec2 *cameraMove = nullptr) {
        const Vec3 dir = { anchor.x - camera.x, anchor.y - camera.y, anchor.z - camera.z };
        const Vec3 zoomed = scaleAbout(pivot, camera, k);
        Vec2 hit = { 0.0f, 0.0f };
        REQUIRE(segmentAtHeight(zoomed, along(zoomed, dir, 10.0f), anchor.z, hit));
        const Vec2 anchorXY = { anchor.x, anchor.y };
        const Vec2 move = pinchAnchorCameraDelta(anchorXY, hit);
        if (cameraMove)
            *cameraMove = move;
        const Vec3 moved = { zoomed.x + move.x, zoomed.y + move.y, zoomed.z };
        Vec2 after = { 0.0f, 0.0f };
        REQUIRE(segmentAtHeight(moved, along(moved, dir, 10.0f), anchor.z, after));
        return after;
    }
}

TEST_CASE("Trackpad: a pick ray crosses a height") {
    const Vec3 a = { 0.0f, 0.0f, 100.0f };
    const Vec3 b = { 40.0f, -20.0f, 0.0f };
    Vec2 out = { 0.0f, 0.0f };
    REQUIRE(segmentAtHeight(a, b, 50.0f, out));
    CHECK(out.x == Approx(20.0f));
    CHECK(out.y == Approx(-10.0f));
    REQUIRE(segmentAtHeight(a, b, 0.0f, out));
    CHECK(out.x == Approx(40.0f));
    // Looking at the sky, along the horizon, or not reaching the height: no crossing.
    const Vec3 up = { 40.0f, 0.0f, 150.0f };
    CHECK_FALSE(segmentAtHeight(a, up, 0.0f, out));
    const Vec3 flat = { 40.0f, 0.0f, 100.0f };
    CHECK_FALSE(segmentAtHeight(a, flat, 0.0f, out));
    CHECK_FALSE(segmentAtHeight(a, b, -10.0f, out));
}

TEST_CASE("Trackpad: pinch keeps the terrain point under the cursor") {
    const Vec3 pivot = { 1000.0f, 800.0f, 10.0f };
    const Vec3 camera = { 1000.0f, 500.0f, 310.0f };  // behind the pivot, pitched down

    // A point off the screen centre, on flat ground at the pivot's height.
    const Vec3 flat = { 1150.0f, 900.0f, 10.0f };
    for (float k : { 0.5f, 0.8f, 0.97f, 1.03f, 1.25f, 2.0f }) {
        Vec2 move = { 0.0f, 0.0f };
        const Vec2 after = zoomAndAnchor(pivot, camera, flat, k, &move);
        CHECK(after.x == Approx(flat.x));
        CHECK(after.y == Approx(flat.y));
        // On flat ground this is the textbook zoom about a point: the camera moves (1 - k) of the
        // way from the pivot to the anchor.
        CHECK(move.x == Approx((1.0f - k) * (flat.x - pivot.x)));
        CHECK(move.y == Approx((1.0f - k) * (flat.y - pivot.y)));
    }

    // A point on a hill (above the pivot's ground) stays exactly under the cursor as well.
    const Vec3 hill = { 870.0f, 1020.0f, 55.0f };
    for (float k : { 0.6f, 1.1f, 1.7f }) {
        const Vec2 after = zoomAndAnchor(pivot, camera, hill, k);
        CHECK(after.x == Approx(hill.x));
        CHECK(after.y == Approx(hill.y));
    }

    // Zooming at the screen centre needs no sideways move: the old centred zoom is the special case.
    Vec2 move = { 1.0f, 1.0f };
    const Vec3 centre = pivot;
    zoomAndAnchor(pivot, camera, centre, 0.7f, &move);
    CHECK(move.x == Approx(0.0f).margin(1e-3));
    CHECK(move.y == Approx(0.0f).margin(1e-3));
}

TEST_CASE("Trackpad: without the anchor move the point under the cursor slides") {
    // What the centred zoom did: after zooming in, the ray of the same pixel lands elsewhere.
    const Vec3 pivot = { 1000.0f, 800.0f, 10.0f };
    const Vec3 camera = { 1000.0f, 500.0f, 310.0f };
    const Vec3 anchor = { 1150.0f, 900.0f, 10.0f };
    const Vec3 dir = { anchor.x - camera.x, anchor.y - camera.y, anchor.z - camera.z };
    const Vec3 zoomed = scaleAbout(pivot, camera, 0.5f);
    Vec2 hit = { 0.0f, 0.0f };
    REQUIRE(segmentAtHeight(zoomed, along(zoomed, dir, 10.0f), anchor.z, hit));
    CHECK(hit.x != Approx(anchor.x));
    CHECK(hit.x == Approx(1075.0f));  // halfway back to the pivot
}

TEST_CASE("Trackpad: a clamped pinch does not move the camera sideways") {
    CHECK(pinchChangedHeight(300.0f, 280.0f));
    CHECK(pinchChangedHeight(300.0f, 330.0f));
    CHECK_FALSE(pinchChangedHeight(300.0f, 300.0f));  // at the min / max height
    CHECK_FALSE(pinchChangedHeight(300.0f, 300.0001f));
}

TEST_CASE("Trackpad: the test pinch setting") {
    TestPinch t;
    REQUIRE(parseTestPinch("1.05,30", t));
    CHECK(t.scale == Approx(1.05f));
    CHECK(t.frames == 30);
    CHECK(t.atCursor);
    REQUIRE(parseTestPinch("0.9,10,200,150", t));
    CHECK(t.frames == 10);
    CHECK_FALSE(t.atCursor);
    CHECK(t.x == 200);
    CHECK(t.y == 150);
    CHECK_FALSE(parseTestPinch(nullptr, t));
    CHECK_FALSE(parseTestPinch("", t));
    CHECK_FALSE(parseTestPinch("1.05", t));
    CHECK_FALSE(parseTestPinch("0,10", t));
    CHECK_FALSE(parseTestPinch("1.05,0", t));
    CHECK_FALSE(parseTestPinch("1.05,10,200", t));
    CHECK_FALSE(parseTestPinch("1.05,10,200,150x", t));
}

TEST_CASE("Trackpad: window points to game pixels") {
    // 1440-point wide viewport showing a 1920-pixel game: 1 point = 1.333 game pixels.
    CHECK(pointsToGamePixels(3.0f, 1920.0f, 1440.0f) == Approx(4.0f));
    CHECK(pointsToGamePixels(3.0f, 800.0f, 1600.0f) == Approx(1.5f));
    CHECK(pointsToGamePixels(3.0f, 800.0f, 0.0f) == Approx(3.0f));
}
