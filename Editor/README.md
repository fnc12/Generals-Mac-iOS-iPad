# END Editor

The editor of "Empires Never Die" (see `docs/WORKDIR/planning/PLAN-023_MOD_IDEAS_ASSESSMENT.md`,
idea 13). macOS first; Windows (C#) and Linux (gtkmm) clients later. No Qt.

## Layout

- `core/` — headless C++20 library with all editor logic. It reads the game data itself
  (`.big` archives and loose files) and does not link the game engine.
  - Core → client: a SQLite file (WAL). The core is the only writer; the client reads it with
    its own connection each tick. Tables: `status`, `objects`, `warnings`, and the preview's
    `model`, `model_parts`, `model_bones`, `model_textures`, `model_animations`
    (`core/src/storage.h`).
  - Client → core: tab separated commands through the C ABI
    (`core/include/end_editor/editor_core_c.h`): `game.open`, `model.open`, `model.animation`.
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

# macOS app -> build/editor-macos/END Editor.app
Editor/macos/build.sh
```

The tests make their own small archives, W3D files and textures and need no game data. The app
opens the last game folder it used, or `~/GeneralsX/GeneralsZH`. Most models and textures are
in the base game's archives, so the base Generals folder is read too: found next to the Zero
Hour folder ("Generals" or Steam's "Command and Conquer Generals") or chosen in "Game Folders".

`END Editor --snapshot <zh> <base> <model> <animation|-> <frame> <out.png>` renders one frame
without a window (checks on a machine nobody is watching).

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

Not yet: condition states (damaged models, the INI's animation per state), the INI's texture
replacements, particle effects, LODs other than the most detailed one.

Next: the map.
