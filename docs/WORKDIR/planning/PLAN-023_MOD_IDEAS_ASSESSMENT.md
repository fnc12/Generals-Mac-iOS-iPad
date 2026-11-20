# PLAN-023: Mod Ideas Assessment (step 4)

Status: assessment only, nothing implemented. Owner decides what to start.

Working name of the custom build: "Empires Never Die" (END). No EA marks in
names; players use their own copy of the game data (see the project rules).

## Legend

- **Data**: INI, maps (WorldBuilder scripts), models, textures. No engine change.
- **Code**: engine change in this fork (GPL, open source).
- **Size**: S = days, M = 1-2 sprints, L = several sprints, XL = a project of its own.
- **MP**: multiplayer and replays. Every player must run the same build and the
  same data; a code change that touches game logic changes the CRC.

Known blocker for all MP items: Windows (x87/MSVC lineage) and macOS arm64
builds still go out of sync at the first CRC check (`CRC Mismatch in Frame
~110`, see the 2026-11-18 diary entry). Same-platform games and replays are
fine; cross-platform play needs the determinism work first.

## What the engine already has (checked in code)

- Per-map `map.ini` overrides (seen in retail `Maps\USA01`): objects, weapons,
  command buttons can be changed for one map only.
- Script engine (`GeneralsMD/Code/GameEngine/Include/GameLogic/Scripts.h`):
  counters (`SET/INCREMENT/DECREMENT_COUNTER`, `COUNTER` condition),
  `PLAYER_GIVE_MONEY` / `PLAYER_SET_MONEY`, area enter/exit conditions,
  "attacked by player / object type", `BUILT_BY_PLAYER`, waypoint paths,
  camera moves, letterbox and cinematic text, `FREEZE_TIME`,
  `NAMED/TEAM_SET_STEALTH_ENABLED`, ownership transfer, unit spawn at a location,
  player rank level and experience switches. No teleport action.
- Script actions and conditions are saved in maps by internal name, not by
  enum index (`Scripts.cpp`, `m_internalNameKey`), and WorldBuilder takes its
  list from the engine (`TheScriptEngine->getActionTemplate`). New actions can
  be added without breaking old maps, and the editor picks them up.
- Water movement: `LOCOMOTORSURFACE_WATER` in `LocomotorSet.h`, units without a
  water locomotor are handled underwater in `PhysicsUpdate.cpp`.
- WorldBuilder source is in `GeneralsMD/Code/Tools/WorldBuilder` (MFC, Windows).
- Headless replay runs (`-headless -replay`) already work on macOS and Windows
  and are the base for autotests.

## Ideas

| # | Idea | Data / Code | Size | MP | Notes |
|---|------|-------------|------|----|-------|
| 1 | Foundation: own menu (no broken campaign buttons), one-click launcher per platform, mod folders | Code + data | M | none | Launchers exist (PLAN step 3). Menu = WND files + small code to hide missing campaigns |
| 2 | Autotests: every map loads, every mod reaches the menu, replays simulate N frames | Code (scripts, small engine flags) | M | none | Extends the headless replay checks used for the mods |
| 3 | Hormuz scenario map (USA/Israel vs Iran, oil, ships) | Data first | M | yes | Map + scripts + `map.ini`. First content target |
| 3a | Oil income grows with fighting | Data (approximation), Code (exact) | S / M | yes | Scripts: count attacks/kills in an area into a counter, pay `PLAYER_GIVE_MONEY` on a timer. Exact version (derrick income multiplier) needs a new script action or module |
| 4 | Naval units | Data + models | M | yes | Water locomotion exists; new models and balance. Naval AI (pathing on water, attack on shore) to be checked; may need code |
| 5 | Drones (Shahed / Geran-style kamikaze, others) | Data | S | yes | Built from existing missile / suicide / drone behaviours |
| 6 | Faction that lives only in a map (e.g. Iran) | Data (as a re-skinned existing side), Code (real new side) | S / L | yes | Faction list is global and chosen before the map loads. A map-only faction needs engine work in the skirmish setup |
| 7 | Fourth faction in the global list | Data (heavy) + small code | L | yes | Mostly content: units, buildings, command sets, AI, UI art, voices |
| 8 | Custom models from existing ones | Data + tools | M | yes | W3D via Blender plugin; primitive model editor later (idea 13) |
| 9 | RPG / stealth missions (Burton, detection, crates) | Data | M per mission | single player | Stealth, detectors, crates, area triggers all exist |
| 9a | "Second map" (tunnels) | Data (closed area of the same map), Code (teleport action) | S / S | single player | One map per game; fake with a sealed area plus spawn/transfer, or add a small teleport script action |
| 10 | Tower defense | Data | M | yes | Waves via scripts and waypoint paths; towers are buildings |
| 11 | Three-lane "dota" | Data + some code | L | yes | Lanes and creeps = scripts; hero leveling = veterancy; items/skills shop needs code or heavy tricks |
| 12 | Daily content (videos, posts EN/RU) | Process | ongoing | n/a | Drafts from the dev diary; owner publishes |
| 13 | Native editor (WC3-style) with a primitive model editor | Code | XL | n/a | C++ core + SQLite, thin native clients (C#, Swift, gtkmm), no Qt. Start after ideas 1-3 show what is missing |
| 14 | Cross-platform MP (Win + Mac) | Code | L | yes | Determinism gap above. Needed before public MP |

## Proposed order

1. Step 5 of the plan: `docs/modding-on-mac.md` (see the AGENTS.md note: docs
   may not go into `docs/` root, so it will likely live in `docs/HOWTO/`).
2. Ideas 1 + 2 (foundation and autotests). Everything later is checked by them.
3. Idea 3 with 3a (script version), 5 and 4 on existing models: the Hormuz
   prototype. First video material.
4. Idea 6 (data version: re-skinned side) and 8 (first custom models).
5. Idea 9 / 9a: one stealth mission as a single-player showcase.
6. Ideas 10 and 11 as separate game modes.
7. Ideas 13 and 14 when the content shows what the editor must do and when MP
   matters.

## Open questions for the owner

- Real names: countries are fine in practice; weapon product names of private
  companies (e.g. LUCAS) are riskier than state products. Framing of the
  current-war scenario ("inspired by", no real leaders) to be decided before
  the first video.
- Intro tagline without the word "Generals" (project rule on EA marks).
- Is the editor (idea 13) part of this plan or a separate project?
