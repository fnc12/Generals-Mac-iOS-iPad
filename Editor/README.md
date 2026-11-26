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
    `map_players`, `map_warnings`, and the user's editing state `map_editor`, `map_selection`,
    `map_links`, `road_types` (`core/src/storage.h`).
  - Client → core: tab separated commands through the C ABI
    (`core/include/end_editor/editor_core_c.h`): `game.open`, `model.open`, `model.animation`,
    `map.open`, `maps.user`, `map.place`, `map.move`, `map.rotate`, `map.delete`, `map.undo`,
    `map.redo`, `map.save`, and the client's input: `map.press`/`drag`/`release`/`click_right`,
    `edit.*`, `placement.*`, `selection.*`, `waypoint.link`, `note.clear`.
  - Editing state and rules live in the core too (`core/src/map_editor.h`): the placement tool,
    a right click waiting for an object, the selection, mouse gestures, the clipboard and the
    note shown to the user. The client turns events into commands, adding only what needs its
    camera (the ground point under the cursor, the objects under it or inside a rubber band),
    and draws what the store says.
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
# every map saved unchanged and compared with the original; every map edited, saved, read back
build/editor-core/end-editor-cli ~/GeneralsX/GeneralsZH --base ~/GeneralsX/Generals --roundtrip-maps /tmp/rt
build/editor-core/end-editor-cli ~/GeneralsX/GeneralsZH --base ~/GeneralsX/Generals --edit-maps /tmp/ed

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

Two checks open the real window and drive it from inside the app (input through its own event
queue; for a machine nobody is watching, like the gui-sandbox VM):
`END Editor --perf <zh> <base> <map path> <out.txt>` measures idle, a drag over the map and
scrolling the map list and the object table: display-link frame intervals and missed vsyncs,
map frames, model changes and main-thread busy and CPU time.
`END Editor --ui-check <zh> <base> <map path> <template> <save name> <out.txt>` uses the window
only as a user does: events on the views and clicks on the real controls (the map list's search
field and row, the object picker's search field and row, the tool buttons, the inspector's
fields, the Edit menu), never model properties. It starts with a right click while no object is
chosen, then places, selects several, drags, turns, copies, pastes, duplicates, band-selects,
deletes, raises an object and sets its health in the inspector, places waypoints and a road,
undoes, redoes, saves and opens the saved map from the list; the exit code is the number of
failed checks. Not driven: the owner pop-ups (SwiftUI's pop-up is no NSPopUpButton and its open
menu runs its own event loop) and the system save panel (it runs out of process); the save uses
the command the panel's Save button ends in.

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

## What works (step 4)

- Smooth map view and lists. Two causes of lag, measured with `--perf` in the gui-sandbox VM
  (Tournament Desert, 784 objects): the client re-published the map's model count on every
  100 ms tick, so every view re-rendered and laid out 10 times a second (~400 ms/s of
  main-thread CPU while idle); and the map view drew every vsync although nothing moved, each
  frame waiting on the main thread for a drawable, which starved list scrolling next to it.
  Now the model changes only when the store does, `EditorModel` is `@Observable` (a view
  updates only for what it reads) and the map view skips frames that would look the same.
  Missed vsyncs in 4 s (median of 3 runs before, 9 after; the VM shares a busy host, so single
  runs vary): dragging the map 66 → 2 (46–48 fps → 59–60), scrolling the map list 97 → 3, the
  object table 133 → 7. Main-thread CPU (not skewed by the host's load): idle 396–413 →
  20–52 ms/s, dragging 399–427 → 57–73, map list 550–606 → 259–337, object table 716–741 →
  311–406.
- Placing objects: a right click (or ⌃-click) on the terrain places the chosen object template
  at that ground point, on the terrain, with the placement bar's angle (Stepper, 15° steps) and
  owner (a player of the map's SidesList; its default team becomes `originalOwner`; neutral
  first). The template comes from the bar's searchable list or the object selected in the
  Objects mode. Placed objects show at once with their model (loaded like the map's) and are
  selected; they get MapObject's default properties and a uniqueID like WorldBuilder's.
- Editing: a press on the selected object drags it over the ground (one move when released,
  also when another object is in front of it); `[` / `]` turn it by 15° (`{` / `}` by 1°), the
  inspector's Angle field and stepper set it; Delete removes it. Undo and redo (⌘Z, ⇧⌘Z,
  buttons in the placement bar) cover place, move, turn and delete; a text field being edited
  keeps its own undo.
- Saving: File > Save Map As… (⇧⌘S) writes the map with its edits, by default as
  `~/Library/Application Support/GeneralsX/GeneralsZH/Maps/<name>/<name>.map` (GlobalData's user
  data path on macOS plus MapCache's `Maps`, the folder the game scans for user maps; base
  Generals: `…/GeneralsX/Generals/Maps`). That folder is listed in the map list. The core never
  writes inside the game folders, so files in `.big` archives are never touched.
- The file: every chunk but ObjectsList is copied byte for byte, and so is every object that
  was not changed (also those the engine drops and chunks of other kinds inside ObjectsList); a
  move or a turn rewrites only the position and angle inside its chunk; placed objects are
  Object version 3 chunks; names they need are appended to the table of contents. Maps are
  written uncompressed (the game reads both). Checked: all 150 maps of the Zero Hour and base
  archives save unchanged byte for byte (`--roundtrip-maps`), and all 150 are edited (move,
  turn, delete, place, undo, redo), saved and read back as expected (`--edit-maps`). In the VM
  the game (GeneralsXZH, `-headless`) read a saved Tournament Desert with a placed tank into its
  user map cache as a 2-player multiplayer map with its start, tech and supply positions.

Not yet: moving or turning several objects at once, copy and paste, editing an object's
properties or owner after placing it, objects' height above the ground (moves keep it), placing
waypoints, roads and bridges (only object templates are offered; existing ones can be moved and
keep their flags), a map preview image for the game's map list. The release game has no `-file` option (it is
debug-only), so a saved map was not started in a game here.

## What works (step 5)

- The editing state moved into the core (`MapEditor`, `core/src/map_editor.h`), covered by core
  tests that drive whole user flows through the C ABI and read the store like the client
  (`core/tests/map_flow_tests.cpp`): e.g. no object chosen → `map.click_right x y` → the store's
  `map_editor` says picker and pending → `placement.template X` → the object is at x, y and
  selected. The client keeps no state of its own: tool, template, owner, angle, the waiting right
  click, the road start, selection, gesture, clipboard and the note all come from the store. A
  note stays until the next note or change of the map, so no store read can wipe it.
- Multi-selection: a click selects, ⇧-click adds or removes, ⇧-drag draws a rubber band that
  adds what is inside, a click on empty ground clears, ⌘A selects all. Dragging any selected
  object moves the whole selection; `[` `]` (`{` `}`) turn it about its middle; Delete removes
  it; each is one undo step. Esc cancels a drag or a started road.
- ⌘C, ⌘V (at the cursor, or the map's middle when the cursor is off the map, keeping the
  layout), ⌘D (next to the original). Like WorldBuilder, a road end copies its segment and
  waypoints are not copied; pasted objects get new uniqueIDs.
- Inspector for one or several objects: angle, height above the ground (moves keep it), owner
  (a player's team; the color follows), name, script, health %, the usual flags (enabled,
  indestructible, unsellable, powered, targetable, recruitable), and every property of the
  dictionary: edit, add (bool/int/real/string/unicode), remove.
- Tools in the placement bar: Object, Waypoint (a right click places "Waypoint <n>" with the
  next waypointID, linked from the selected waypoint, so clicks build a path; a right click on
  another waypoint links or unlinks it; "Link Waypoints" for two selected), Road and Bridge (a
  Roads.ini type; two right clicks place the segment's POINT1 and POINT2 objects like
  WorldBuilder's road tool). Links and segments are drawn as lines; deleting a waypoint takes its
  links, deleting a road end its segment.
- Save Map As (now a sheet on the window) also writes the preview the game's map lists show: `<name>.tga` next to the map,
  128×128, 32-bit, like WorldBuilder's (`MapPreview.cpp`: each tile's average color made lighter
  or darker by height, water areas blue, 3×3 averaging) and in the shipped previews' layout.
- The file: objects given properties (or a height while they are an older version) are written
  again as Object version 3; a height change of a version 3 object rewrites only its z; moves and
  turns patch in place; WaypointsList is rewritten only when links changed (appended when the
  map had none). Unchanged objects and chunks stay byte for byte, and the table of contents gets
  only the added names the file still uses, so undoing every edit saves the original bytes.
  Checked over all 150 maps: `--roundtrip-maps` (saved unchanged, byte for byte) and
  `--edit-maps` (now also raising and editing an object, pasting it, a waypoint path and a road,
  and the preview's size).

Checked in the gui-sandbox VM: `--ui-check` on Tournament Desert, 50 checks passed (owners are
covered by the core tests; the save panel itself is not tested).

Not yet: waypoint path labels and bidirectional flags are only editable as plain properties; road corner
types (angled, tight) and joining a new road to an existing end; landmark bridges (single
objects); the preview is not drawn from the models or roads. A saved map was not started in a
game here.

Next: terrain editing.
