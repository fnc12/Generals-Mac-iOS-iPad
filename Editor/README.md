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
    `map_links`, `road_types`, and the Play button's `play` (its settings, kept across
    sessions, and the last run), `factions` and `player_colors` (`core/src/storage.h`).
  - Client → core: tab separated commands through the C ABI
    (`core/include/end_editor/editor_core_c.h`): `game.open`, `model.open`, `model.animation`,
    `map.open`, `maps.user`, `map.place`, `map.move`, `map.rotate`, `map.delete`, `map.undo`,
    `map.redo`, `map.save`, `map.save_as`, `map.play`, `play.*`, and the client's input:
    `map.press`/`drag`/`release`/`click_right`, `edit.*`, `placement.*`, `selection.*`,
    `waypoint.link`, `note.clear`. Each command is parsed once, at that boundary, into a typed
    value (`core/src/commands.h`: a `std::variant` of one struct per command with its typed
    fields); a malformed one is refused with its error before anything runs, and the core's jobs
    and the map editor get only those values.
  - Editing state and rules live in the core too (`core/src/map_editor.h`): the placement tool,
    a right click waiting for an object, the selection, mouse gestures, the clipboard and the
    note shown to the user. The client turns events into commands, adding only what needs its
    camera (the ground point under the cursor, the objects under it or inside a rubber band),
    and draws what the store says.
  - `end_editor_update()` runs queued work; the client calls it once per tick.
- `macos/` — SwiftUI client. Links the core's static library and the system SQLite.
- `windows/` — WPF client (.NET 8, C#): `EndEditor/`. Calls the core's DLL (`end_editor_core_c.dll`,
  the same C ABI) through P/Invoke, reads the store with Microsoft.Data.Sqlite (read-only) and draws
  with Direct3D 11 (Vortice.Windows) in a Win32 child window; the Metal shaders are ported to HLSL
  (`Render/Shaders.cs`). Like the macOS client it decides nothing: input becomes the same commands.

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

Windows (Visual Studio 2022 or its Build Tools with the C++ x64 tools, CMake, the .NET 8 SDK or later):

```powershell
# core DLL + tests (run) + CLI with MSVC x64, then the app published self-contained:
# build\editor-windows\END Editor\END Editor.exe
powershell -ExecutionPolicy Bypass -File Editor\windows\build.ps1 [-Out <folder>] [-Build <folder>] [-SkipTests]
```

On Windows the core gets zlib from its sources when the system has none (one of the 150 maps is "ZL"
compressed), and the executables run with the UTF-8 code page (`core/src/utf8.manifest`, the app's
`app.manifest`): paths cross the C ABI as UTF-8 and the core opens files with narrow paths, so a
Documents folder like OneDrive's `Документы` works.

Code style: the C and C++ sources (`core/`, `macos/bridge.h`) are formatted with clang-format and
`Editor/.clang-format`, a copy of sqlite_orm's (v1.9.1). `Editor/format.sh` formats them in place;
`Editor/format.sh --check` changes nothing and fails when a file is not formatted (what CI can
run). It uses Homebrew's LLVM clang-format (`brew install llvm`), or the one in `CLANG_FORMAT`.
The store is read and written only through sqlite_orm (`core/src/storage.h`), no SQL text.

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
`END Editor --ui-check <zh> <base> <map path> <template> <save name> <game app> <out.txt>` uses
the window only as a user does, never model properties: mouse and key events posted to the app's
own queue at the places the views show things, clicks on the real controls, and for controls
SwiftUI draws itself (pop-ups, buttons in forms and popovers, steppers, segments) the
Accessibility API on the app's own process (AXUIElement, what VoiceOver uses: SwiftUI builds its
accessibility tree only for such a client; requests to the own process are answered in place, so
no permission is needed). A pop-up or menu is opened like that; once AppKit tracks its menu, the
item is chosen the way AppKit does after a click on it. It walks every feature of the
"Test coverage" table below that is marked "ui"; the exit code is the number of failed checks.
Run in the gui-sandbox VM (the game app given is the one Play starts):

```bash
open -W -n "END Editor.app" --args --ui-check ~/generalszh-test/GeneralsZH \
    ~/generalszh-test/home/GeneralsX/Generals "maps/tournament desert/tournament desert.map" \
    AmericaTankCrusader UICheckStep6 ~/editor-step6/GeneralsXZH.app ~/editor-step6/ui.txt
```

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
  height offset, in their owner's player color. A start waypoint (`Player_<n>_Start`) has a
  "Start n" label over it (with the Markers), drawn in the map's own frames so it follows the camera
  exactly; start position n has one color of eight (`startPositionColor`, written by the core into
  `map_objects.color` and `map_players.color`): its label, its waypoint and the objects it owns. Models load after the map shows, a slice per
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
  first). On a multiplayer map (two or more `Player_<N>_Start` waypoints) the owner can also be a
  start position, "Start 1", "Start 2"…: `originalOwner` is `teamStart<N>`, and GeneralsX's engine
  gives the object to whoever starts at start position N, or leaves it out when nobody starts
  there (`Core/GameEngine/Include/Common/StartPositionOwner.h`; the original game does not know
  these owners). The template comes from the bar's searchable list or the object selected in the
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

## What works (step 6)

- **Play** (toolbar ▶, ⌘R, Map > Play Map), like the World Editor's Test Map: the core writes the
  open map with its unsaved edits to a test copy in the game's user Maps folder,
  `Maps/__EditorTest/__EditorTest.map` (+ `.tga`; the user's own file and its "Edited" state are
  not touched, the copy is not listed among the user's maps), and starts the game app on it:
  `<app>/Contents/MacOS/run.sh -testMap <copy> -testMapFaction <f> -testMapAI <n>
  -testMapAIDifficulty <easy|medium|hard> -testMapStart <n>`, with the editor's game folders as
  `CNC_GENERALS_ZH_PATH` / `CNC_GENERALS_PATH` and `GENERALSX_USER_DATA_DIR` = the folder whose
  `Maps` has the copy. The game runs on its own (own session; its output goes to `play.log` next to
  the store); the `play` row says running / exited (exit code) / error. Play again closes the game
  the last Play started (its whole process group: `run.sh` runs the game as its child; killed when it
  does not quit within 3 s); Map > Stop Game closes it.
- **Play Settings** (toolbar ⚙): faction (`PlayerTemplate.ini`'s playable factions, or Random),
  color (`Multiplayer.ini`'s `MultiplayerColor` entries, or Random; `-testMapColor` gets the
  game's index, their order in the file: Zero Hour has Gold, Red, Blue, Green, Orange, SkyBlue,
  Purple, Pink = 0..7), start position (1–8 or random), AI opponents (0–7) and difficulty, the game app (Choose…,
  Default = `~/GeneralsX/Apps/GeneralsXZH.app`, `GeneralsX.app` for the base game). The defaults
  (FactionAmerica at position 1 against one medium AI) need no setup; the settings are stored by the
  core and kept across sessions.
- **The game** (`Core/GameEngine/Source/Common/TestMap.cpp`, release builds, Zero Hour and the base
  game): `-testMap <map>` skips the intro, the shell map and the main menu and starts the map once
  the client runs. A map with two or more start positions is a skirmish built from the options (the
  local player at the start position, the AI opponents capped at the free positions, the default
  cash, random colors and AI factions); any other map is started as a mission, like the debug-only
  `-file`. The map's cache entry is read again first (the game's map cache compares only file sizes,
  so a map saved over with a moved start waypoint kept its old start positions). `-testMapSeed` fixes
  the random seed. With `-headless` it runs `-testMapFrames` logic frames (default 300), prints one
  `TEST_MAP ok|error frames=… mode=skirmish|single_player players=… loaded_objects=… objects=…
  local_objects=… local_faction=…` line and exits 0 only when the map started, ran every frame and
  (skirmish) the local player owns objects. It is a second instance, so it starts while the game runs.
- **Save Map As** is an in-app sheet: a name (`map.save_as <name>` → `<user Maps>/<name>/<name>.map`
  and its 128×128 `.tga`), Save, Cancel, Replace when another map has that name (the core refuses
  until asked again with `replace`; `map.save_conflict` names the file), Other Location… for the
  system panel (any path, never into the game folders).
- The map view: a trackpad two-finger swipe pans, ⌘ + swipe, a mouse wheel or pinch zooms (merged
  from master); W3D environment-map passes are skipped in the preview (merged from master).

Checked (27.11.2026, on this Mac unless said otherwise):
- Core: `ctest` — 47 test cases, 1193 assertions, all pass (new: Play with a stand-in game app that
  records its arguments and environment, Play again, a game that ignores SIGTERM and runs its work in
  a child, Play settings and their persistence, Save Map As a name and the replace question).
- Game: `scripts/qa/editor/run-play-smoke-macos.sh` with the rebuilt GeneralsXZH — all 150 maps of
  the archives saved by the editor untouched and edited (an object moved, one turned, three
  AmericaTankCrusader placed, a path of three waypoints, a road segment) loaded and ran 300 frames
  headless with exit code 0: 150 of 150 passed (64 as skirmishes, 86 as missions), and every edited
  copy loaded exactly 3 objects more than its untouched copy; 300 runs in about 14 minutes.
- Replays with the rebuilt game: `scripts/qa/replays/run-replay-tests-macos.sh` — 3 of 3 pass
  (110, 155 and 220 CRC compares, all equal). The base game (`g_generals`) builds.
- gui-sandbox VM: `--ui-check` on Tournament Desert: the last two runs passed all 163 checks (the
  process is not trusted for accessibility; no permission was needed). Earlier runs found timing
  flakes (a click while the inspector column was still animating, a popover toggled closed); the
  check now waits for the layout and the controls instead of fixed delays. Play started the game in the VM (full
  screen, its Options.ini), which printed `TEST_MAP start map='…/maps/__editortest/__editortest.map'
  mode=skirmish positions=2 faction=FactionChina start=2 …` and created its swap chain: it went past
  its intro straight into the map. Nobody looked at the game's picture there; the check reads the
  process, its arguments and its log. After the check no game process was left.

Not done: Play on Windows and Linux (the core starts processes with posix_spawn; Windows reports
"not supported yet"); an option to start the game windowed (it uses its Options.ini); the map cache
problem above is fixed only for `-testMap` — the game's own map lists can still show stale start
positions for a user map saved over with the same size.

Next: terrain editing.

## Test coverage

Every user-facing feature and what checks it. Keep this table when adding a feature.

- **core**: `ctest --test-dir build/editor-core` (`core/tests/*.cpp`; `map_flow_tests.cpp` drives user
  flows through the same commands the client sends and reads the store like the client).
- **maps**: `end-editor-cli <zh> --base <base> --roundtrip-maps <dir>` / `--edit-maps <dir>` over all 150
  maps of the game's archives (real game data).
- **game**: `scripts/qa/editor/run-play-smoke-macos.sh`: the game (headless, `-testMap`) starts every map
  saved by the editor, untouched and edited.
- **ui**: `END Editor --ui-check` in the gui-sandbox VM: only real input (posted mouse and key events on
  the views, clicks and Accessibility presses on the real controls, menu items chosen the way AppKit
  does after a click); the step it checks is quoted.

| Feature | How the user does it | Covered by |
|---|---|---|
| **Game data** | | |
| Open the Zero Hour folder, find the base game folder | launch (last folder), Game Folders > Zero Hour Folder… / Base Generals Folder… | core (game folder scan, loose files win, archives), ui ("Game Folders > … opened a folder panel"); the folder picked in the system panel: not driven (the panel runs out of process) |
| Status line (objects, INI files, archives, warnings) | toolbar | core ("EditorCore writes the store a client reads"); display only |
| **Objects mode** | | |
| Switch Objects / Maps | toolbar segments | ui ("the toolbar's Objects segment is clicked"; Maps is the launch setting, Open Map File… switches back) |
| Filter by side | sidebar rows | ui ("the side … is clicked", "the table shows its … objects"), core (sides from INI) |
| Search name / model / KindOf | table search field | ui ("the object table's search field", "the search left … rows") |
| Sort by a column | column headers | ui ("the Side column's header sorted the table") |
| Object details (definition, side, model, KindOf, source) | select a row | core (ini_objects_tests: blocks, reskins, child objects), ui ("the details show its model and source") |
| Model preview: meshes, textures, bones, house color, hidden parts, environment-map passes skipped | select a row | core (model_tests: W3D, shaders and passes, textures, skeleton), ui ("the preview shows its model"); how it looks: `--snapshot` (not compared automatically) |
| Animation | pop-up | core ("EditorCore model commands"), ui ("the animation pop-up chose …", "the preview plays it") |
| Play / pause | button | ui ("play/pause is clicked", "it paused or played") |
| Hidden parts | checkbox | core (what the Draw module hides), ui ("Hidden parts is clicked") |
| Orbit and zoom the preview | drag, scroll | ui ("a drag on the preview turns the model", "scrolling zooms the preview") |
| Export GIF (square, wide) | menu > save panel | ui ("Export GIF > Square 800×800 opened the save panel for <suggested name>"); the file chosen in the panel: not driven (out of process); the GIF itself: `END Editor --gif` (same code, run by hand, not compared automatically) |
| **Maps mode** | | |
| Map list (archives, loose Maps folders, user Maps folder), search, open | sidebar | core ("EditorCore lists and opens maps", user maps listed), maps (all 150 open), ui ("the map … is clicked in the map list", the saved map opened from the list) |
| Open Map File… | Game Folders menu | ui ("Game Folders > Open Map File… opened a .map file panel", "switched to the Maps mode"); the file chosen: not driven (out of process); opening any .map path: core |
| Map view: terrain, blends, water, objects with models, markers, links | open a map | core ("Map scene: terrain, water, objects", map_tests), maps (all 150 without warnings); how it looks: `--map-snapshot` (not compared automatically) |
| Models / Markers / Water | checkboxes | ui ("the map.showModels checkbox turns it off and on", markers, water) |
| Pan, orbit, zoom | drag on empty ground, ⌥-drag; trackpad two-finger swipe pans, ⌘ + swipe, a mouse wheel or pinch zooms | ui ("a drag on empty ground pans the camera", "⌥-drag turns it", "a two-finger swipe pans … and does not zoom" (a CGEvent in pixel units, continuous: precise deltas), "a mouse wheel zooms … and does not pan" (line units), "⌘ with a two-finger swipe zooms"); pinch and the rotate gesture: not driven (there is no public API to make magnify/rotate events; they call the same camera functions, zoom and orbit) |
| Select, add to selection, rubber band, clear | click, ⇧-click, ⇧-drag, click on empty ground | core (Flow: clicks, shift clicks and a rubber band), ui ("⇧-click selected both", "the ⇧-drag band selected the duplicates", "a click on empty ground cleared the selection") |
| Select all | ⌘A | core, ui ("⌘A selected all … objects") |
| Place an object, the picker | right click or ⌃-click; the bar's object button | core (Flow: a right click with no object chosen asks for one), ui ("the picker's row … is clicked", "⌃-click placed a second one", "the bar's object button is clicked", "Esc closed it") |
| Start positions as owners (teamStart<N>) | bar and inspector pop-ups | core (Map players: a multiplayer map's start positions…, Flow: a multiplayer map's start positions own objects…), maps (--edit-maps: one object owned by teamStart1 per multiplayer map), game (Catch2 StartPositionOwner; -headless start_owned_* counts), ui ("the Owner pop-up lists the start positions", "the inspector's Owner pop-up chose Start 1", "it is Start 1's") |
| Start labels and colors: "Start N" over each start waypoint, start N's color on its label, waypoint and objects; the inspector's Start position | open a multiplayer map; Markers checkbox; click a start waypoint | core (Start positions: waypoint and team names, and one color each; Flow: a multiplayer map's start positions own objects… (colors); Flow: a start waypoint renamed or moved…), ui ("the map's start waypoints … named Player_N_Start", "each start position has its own color", "the map framed / after a pan / a turn / a zoom: … start labels, each at its waypoint's projected point", "each label is drawn in its start's color", "… behind the camera: … none for the waypoint behind the camera", "Markers off: no start labels …, their pixels gone", "a click on Player_1_Start: the inspector shows \"Start 1\"", "Start 1's object is drawn in Start 1's color") |
| Placement owner | bar pop-up | core (Map players, placement.owner), ui ("the placement bar's Owner pop-up chose …", "owned by the chosen team") |
| Placement angle | bar stepper | core, ui ("the bar's angle stepper is clicked up", "and down again") |
| Move the selection | drag a selected object | core (Flow: a drag on the selection moves it), maps, ui ("the drag moved both by the same offset", "on the ground there") |
| Turn the selection | `[` `]` (15°), `{` `}` (1°) | core, ui ("] turned the selection by 15°", "} turned it by 1° more", "{ and [ turned it back") |
| Delete | Delete key, inspector's Delete | core, maps, ui ("Delete removed the selection", "the inspector's Delete removed the clicked object") |
| Copy, paste at the cursor, duplicate | ⌘C, ⌘V, ⌘D | core (Flow: Delete, turn, copy, paste and duplicate), maps, ui ("⌘C copied 2 objects", "⌘V pasted …, around the cursor", "⌘D duplicated them") |
| Undo, redo | ⌘Z, Edit > Redo, the bar's buttons | core, maps, ui ("⌘Z brought it back", "Edit > Redo put it back", "the bar's Undo is clicked", "the bar's Redo is clicked"); ⇧⌘Z typed: not driven (posted key events with ⇧ do not match menu key equivalents; Edit > Redo is the same item) |
| Inspector: angle field and stepper, height, owner, name, script, health, flags | inspector | core (Flow: the inspector edits owner, height and properties), maps (raised, properties), ui ("angle typed", "the inspector's angle stepper", "height typed", "the inspector's Owner pop-up chose …", "name typed", "script typed", "health typed", "the Indestructible checkbox is clicked") |
| Inspector: properties — edit, add (type pop-up), remove | inspector | core, ui ("a new property's key typed", "its type chosen in the pop-up", "Add is clicked", "its value field is typed into", "its remove button is clicked") |
| Waypoint tool: paths, link two | Waypoint button, right clicks, Link Waypoints | core (Flow: waypoints are placed as linked paths), maps, ui ("three right clicks placed three waypoints", "linked into a path", "the inspector's Link Waypoints is clicked") |
| Road and bridge tools, road type, Esc | Road / Bridge buttons, pop-up, two right clicks | core (Flow: roads and bridges are segments), maps, ui ("the Road button chose the road tool", "the road type pop-up chose …", "the Bridge button chose the bridge tool", "Esc cancelled a started road", "the second right click placed the road segment") |
| Notes (what to do, what was refused) | the bar | core (Flow: notes last until the next note or map change), ui (the picker's note) |
| Save Map As (name, Save, Cancel, Replace) | ⇧⌘S / File > Save Map As… / the bar's Save As… | core (Flow: Save Map As a name …; Save Map As writes the map's preview), maps (byte for byte, edits read back), ui ("File > Save Map As… is chosen", "its Cancel is clicked", "saved as …", "with its 128×128 preview", "the sheet asks before it replaces …", "Replace saved over it"); ⇧⌘S typed: not driven (see ⇧⌘Z) |
| Save to another location | the sheet's Other Location… | ui ("Save As > Other Location… opened the save panel in the Maps folder"); the file chosen: not driven (out of process); saving to any path, never into the game folders: core |
| A saved map in the game | the game's map lists | game (every map loads and runs in the game, see Play) |
| **Play** | | |
| Play: save the map with its unsaved edits to the test copy and start the game on it | toolbar Play, ⌘R, Map > Play Map | core (Flow: Play saves the map with its unsaved edits …: the command line, the environment, the user's map left unsaved), ui ("the toolbar's Play is clicked", "the game runs", "with -testMap on the test copy and the settings", "the game started the test copy", "⌘R started the game again", "Map > Play Map started it again") |
| Play again / Stop Game | ⌘R again, Map > Stop Game | core (Flow: Play again closes the game the last Play started), ui ("the first game is closed", "the game ended") |
| Play settings: faction, color, start position, AI opponents, AI difficulty, game app, Default, Choose… | toolbar Play Settings | core (Flow: Play settings: defaults, refused values, kept across sessions; Commands: the Play button's settings), ui ("Faction: FactionChina chosen", "Color: Blue chosen", "Start position: 2 chosen", "the AI stepper's up arrow", "the difficulty's Hard is clicked", "the game app typed", "Default is clicked", "Play settings > Choose… opened an app panel"); the app chosen in that panel: not driven (out of process) |
| The game goes straight into the map (skirmish for maps with start positions, a mission otherwise) | — | game (all maps, untouched and edited: placed units, waypoints, a road), ui (the windowed game in the VM, see below) |

