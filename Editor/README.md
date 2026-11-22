# END Editor

The editor of "Empires Never Die" (see `docs/WORKDIR/planning/PLAN-023_MOD_IDEAS_ASSESSMENT.md`,
idea 13). macOS first; Windows (C#) and Linux (gtkmm) clients later. No Qt.

## Layout

- `core/` — headless C++20 library with all editor logic. It reads the game data itself
  (`.big` archives and loose files) and does not link the game engine.
  - Core → client: a SQLite file (WAL). The core is the only writer; the client reads it with
    its own connection each tick. Tables: `status`, `objects`, `warnings` (`core/src/storage.h`).
  - Client → core: tab separated commands through the C ABI
    (`core/include/end_editor/editor_core_c.h`), e.g. `game.open\t<folder>`.
  - `end_editor_update()` runs queued work; the client calls it once per tick.
- `macos/` — SwiftUI client. Links the core's static library and the system SQLite.

## Build

```bash
# core + tests + command line harness
cmake -S Editor/core -B build/editor-core -DCMAKE_BUILD_TYPE=Debug
cmake --build build/editor-core
ctest --test-dir build/editor-core --output-on-failure
build/editor-core/end-editor-cli ~/GeneralsX/GeneralsZH     # prints what the store holds

# macOS app -> build/editor-macos/END Editor.app
Editor/macos/build.sh
```

The tests make their own small archives and need no game data. The app opens the last game
folder it used, or `~/GeneralsX/GeneralsZH`; "Open Game Folder" picks another one (the folder
with the Zero Hour `.big` files).

## What works (step 1)

- List of all object templates (units, buildings, props...) with side, editor sorting,
  buildable status, model and KindOf; search and filter by side.
- `ObjectReskin` and `ChildObject` inherit missing fields from their parents.
- Loose `Data/INI/Object` files win over archived ones; among archives the first in name
  order wins.

Next: model preview (W3D), animations, then the map.
