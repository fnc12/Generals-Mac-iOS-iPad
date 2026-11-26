# END Editor

The editor of "Empires Never Die" (see `docs/WORKDIR/planning/PLAN-023_MOD_IDEAS_ASSESSMENT.md`,
idea 13). macOS first; Windows (C#) and Linux (gtkmm) clients later. No Qt.

## Layout

- `core/` — headless C++20 library with all editor logic. It reads the game data itself
  (`.big` archives and loose files) and does not link the game engine.
  - Core → client: a SQLite file (WAL). The core is the only writer; the client reads it with
    its own connection each tick. Tables: `status`, `objects`, `warnings`, the preview's
    `model`, `model_parts`, `model_bones`, `model_textures`, `model_animations`, and the map's
    `maps`, `map`, `map_textures`, `map_terrain`, `map_water`, `map_objects`,
    `map_object_properties`, `map_models`, `map_model_parts`, `map_model_textures`,
    `map_warnings` (`core/src/storage.h`).
  - Client → core: tab separated commands through the C ABI
    (`core/include/end_editor/editor_core_c.h`): `game.open`, `model.open`, `model.animation`,
    `map.open`.
  - `end_editor_update()` runs queued work; the client calls it once per tick.
- `macos/` — SwiftUI client. Links the core's static library and the system SQLite.

## Build

```bash
# core + tests + command line harness
cmake -S Editor/core -B build/editor-core -DCMAKE_BUILD_TYPE=Debug
cmake --build build/editor-core
ctest --test-dir build/editor-core --output-on-failure
build/editor-core/end-editor-cli ~/GeneralsX/GeneralsZH --base ~/GeneralsX/Generals   # what the store holds
build/editor-core/end-editor-cli ~/GeneralsX/GeneralsZH --base ~/GeneralsX/Generals \
    --object AmericaTankCrusader          # one object's model: parts, textures, animations
build/editor-core/end-editor-cli ~/GeneralsX/GeneralsZH --base ~/GeneralsX/Generals --all-models
build/editor-core/end-editor-cli ~/GeneralsX/GeneralsZH --base ~/GeneralsX/Generals \
    --map "maps/tournament desert/tournament desert.map"   # one map: sizes, objects, models, warnings
build/editor-core/end-editor-cli ~/GeneralsX/GeneralsZH --base ~/GeneralsX/Generals --all-maps

# macOS app -> build/editor-macos/END Editor.app
Editor/macos/build.sh
```

The tests make their own small archives, W3D files, textures and maps (plain and
RefPack-compressed, `core/tests/test_map_writer.h`) and need no game data. The core links zlib
when CMake finds it (always on macOS); without it the rare "ZL" compressed maps are reported as
unsupported. The app
opens the last game folder it used, or `~/GeneralsX/GeneralsZH`. Most models and textures are
in the base game's archives, so the base Generals folder is read too: found next to the Zero
Hour folder ("Generals" or Steam's "Command and Conquer Generals") or chosen in "Game Folders".

`END Editor --snapshot <zh> <base> <model> <animation|-> <frame> <out.png>` renders one frame
without a window (checks on a machine nobody is watching).
`END Editor --gif <zh> <base> <model> <animation> <square|wide> <out.gif>` writes the same GIF
as the preview's "Export GIF" button, from the default camera.
`END Editor --map-snapshot <zh> <base> <map path> <out.png> [--top] [--look <x> <y> <distance>
<yaw°> <pitch°>] [--size <w>x<h>] [--pick <x> <y>]` opens a map, waits for all its models and
renders it: the default view, top-down, or looking at a world point. `--pick` clicks at a pixel
(origin top left), prints the selected object and its properties and draws its box. The map
path is one of the `maps` table (`maps/<name>/<name>.map`) or a .map file.

## What works (step 1)

- List of all object templates (units, buildings, props...) with side, editor sorting,
  buildable status, model and KindOf; search and filter by side.
- `ObjectReskin` and `ChildObject` inherit missing fields from their parents.
- Loose `Data/INI/Object` files win over archived ones; among archives the first in name
  order wins.

## What works (step 2)

- Preview of the selected object's model (Metal): textures (DDS DXT1/3/5, TGA), the bone
  hierarchy, every animation of its skeleton, play/pause, orbit and zoom.
- Drawn like the game: shader blend modes (solid, alpha test, glow, see-through), player
  color ("ZHC" textures, "HOUSECOLOR" meshes), muzzle flashes and `HideSubObject` parts hidden
  while idle (a checkbox shows them).
- 997 of 1020 object models load; the others have no file or no meshes in the game either.
- "Export GIF": one cycle of the selected animation (every frame, real timing, looping) from
  the current camera, square 800×800 or 16:9 1280×720, drawn at 2× and scaled down.

Not yet: condition states (damaged models, the INI's animation per state), the INI's texture
replacements, particle effects, LODs other than the most detailed one.

## What works (step 3)

- "Maps" mode (toolbar): every map of the game's archives (MapsZH.big, base maps.big) and loose
  `Maps` folders, with search; "Open Map File…" for any other .map.
- The core reads .map files itself: RefPack ("EAR") and zlib ("ZL") unpacking, the DataChunk
  table of contents and nested chunks, HeightMapData, BlendTileData, WorldInfo, SidesList
  (players, teams), ObjectsList, PolygonTriggers and GlobalLighting. All 171 maps of the Zero
  Hour and base archives open with no warnings, and every chunk read is read to its last byte.
- Terrain: the height map as a mesh in 32×32-cell chunks, with the map's texture classes
  (Terrain.ini; names missing from Zero Hour's copy are taken from the base game's) packed into
  one atlas; each cell shows its quarter tile, blend layers and 3-way blends with the engine's
  per-corner weights and diagonal flips, cliff cells with their stretched UVs. Lit with the
  map's terrain light for its time of day; the border outside the playable area is darker.
- Water areas and rivers (water polygon triggers) as flat see-through surfaces with Water.ini's
  standing water texture and color.
- Objects: templates resolved against the INI (W3DTreeDraw's `ModelName` too), drawn with their
  models in the bind pose, instanced, turned by their angle, standing on the ground plus their
  height offset, in their owner's player color. Models load after the map shows, a slice per
  update. Waypoints, road and bridge points, sounds and objects without a model are markers.
- Camera: drag pans, ⌥-drag (or ⌘-drag) turns and tilts, scroll or pinch zooms, the rotate
  gesture turns. A click selects the nearest object under the cursor (box test in its own
  space); the inspector shows template, kind, position, angle, owner, model and every property.

Simplified: blending interpolates the corner weights across each triangle (the engine's
non-shader path) and skips custom blend edge textures; the triangle diagonal of plain cells
follows the smaller height step; standing water is one flat color and opacity (no depth fade,
reflections or waves); objects stand still (no animations, condition states or the INI's
texture replacements, so environment-mapped glass shows its reflection texture); roads, bridges,
scorch marks, lights, the sky and shadows are not drawn; passability, scripts and build lists
are not read.

Next: placing objects (right-click), then saving.
