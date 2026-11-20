# Modding on macOS

How to make your own data mod and how to run existing big mods (Rise of the
Reds, ShockWave, Contra, ...) on macOS next to the unmodded game, without
changing the original game files.

You need:

- the macOS builds (`GeneralsX-macos-arm64.zip`, `GeneralsXZH-macos-arm64.zip`,
  see [Installation](INSTALLATION.md));
- your own copy of the game data (see [Getting the Game Files](GETTING_THE_GAME_FILES.md)).
  Game data is never part of a mod or of this repository: do not commit or
  share `.big` archives or `Data/` from the game.

---

## Two kinds of mods

| Kind | What it is | How it is loaded |
|---|---|---|
| One-file mod | One `.big` (or folder) with a few INI files that change existing objects | `-mod <path>` on the command line |
| Big mod | Many `.big` archives (often `!Mod_*.big`) plus loose files under `Data/`, made to be copied into the game folder | A separate game folder per mod made of links (see below) |

`-mod` does not work for big mods: it loads one archive or one folder, it has
the opposite priority (a big mod expects its archives to win because they sort
first), and it skips loose files.

---

## Part 1: your own one-file mod

The example is `mods/hello-zh` (and `mods/hello-generals` for the base game).
It is a single INI file and contains no game assets:

```ini
; mods/hello-zh/Data/INI/Object/zz_HelloMod.ini
Object ChinaVehicleDozer
  BuildCost = 123
  BuildTime = 1.0
End
```

Why this works:

- `INI::loadDirectory` loads files in sorted order, so a `zz_` file comes last;
- in release builds a second `Object X` block for an existing object does not
  create a new object: the new fields are parsed into the existing one. Only
  the fields you write change; everything else stays as the game defines it.

Steps:

1. Copy `mods/hello-zh` to `mods/<your-mod>-zh` and edit the INI. Keep the
   path inside the mod the same as in the game (`Data/INI/Object/...`).
2. Pack it:

   ```bash
   mkdir -p build/mods
   python3 scripts/mods/make_big.py mods/<your-mod>-zh/ build/mods/<YourMod>-zh.big
   ```

   The file name decides the game: `*-zh.big` is Zero Hour, `*-generals.big`
   is the base game.
3. Install the apps and launchers:

   ```bash
   ./scripts/mods/install-macos-launchers.sh
   ```

   This puts into `~/GeneralsX/Apps`: `GeneralsX.app`, `GeneralsXZH.app`
   and one small launcher app per mod, e.g. `GeneralsXZH + <YourMod>.app`,
   which starts the real app with `-mod ~/GeneralsX/Mods/<YourMod>-zh.big`.
4. Start the launcher from Finder, or:

   ```bash
   open ~/GeneralsX/Apps/"GeneralsXZH + <YourMod>.app"
   ```

To remove the mod, delete its launcher app and its `.big` in `~/GeneralsX/Mods`.

Check the hello mod: start a skirmish as China. Without the mod the dozer costs
1000; with it, 123.

---

## Part 2: big mods from the internet

1. Unpack the mod into its own folder `~/GeneralsX/ModFiles/<Mod>`, so that
   its `.big` files and its `Data/` are at the top of that folder (the same
   layout the mod expects inside the game folder). GenLauncher's disabled
   archives (`*.gib`) are fine; they are renamed to `.big` automatically.
2. Run the installer with `ZH_SOURCE` pointing at the original Zero Hour folder
   (Steam / EA layout, with `ZH_Generals/` inside):

   ```bash
   ZH_SOURCE=~/GeneralsX/steam/"Command & Conquer Generals - Zero Hour" \
       ./scripts/mods/install-macos-launchers.sh
   ```

3. Start `~/GeneralsX/Apps/GeneralsXZH + <Mod>.app`.

What the installer makes (nothing in `ZH_SOURCE` is changed):

| Path | Content |
|---|---|
| `~/GeneralsX/Games/ZH` | clean Zero Hour: symlinks to `ZH_SOURCE` without `ZH_Generals/`. Point `~/GeneralsX/GeneralsZH` here |
| `~/GeneralsX/Games/ZH-<Mod>` | Zero Hour + mod: symlinks to the originals, the mod's files linked on top |
| `~/GeneralsX/Profiles/<Mod>` | the mod's own settings, saves and replays (the launcher sets `HOME` to this folder) |
| `~/GeneralsX/Apps/GeneralsXZH + <Mod>.app` | runs Zero Hour from `Games/ZH-<Mod>` |

The game folders are rebuilt from scratch on every run, so re-run the installer
after you update a mod. To remove a mod, delete its launcher, `Games/ZH-<Mod>`
and `ModFiles/<Mod>` (and `Profiles/<Mod>` if you don't need its saves).

The folder builder can also be used on its own:

```bash
python3 scripts/mods/make_mod_game_dir.py --exclude ZH_Generals \
    <original game dir> <mod files dir> <output dir>
```

Mods are third-party work: use them for your own play, do not redistribute
them or bundle them with builds.

---

## Where things are

| What | Where |
|---|---|
| Settings, saves, replays (no mod) | `~/Library/Application Support/GeneralsX/GeneralsZH` (base game: `.../Generals`) |
| Settings, saves, replays (big mod) | `~/GeneralsX/Profiles/<Mod>/Library/Application Support/GeneralsX/...` |
| Zero Hour data used by the apps | `~/GeneralsX/GeneralsZH` (or `CNC_GENERALS_ZH_PATH`) |
| Base game data | `~/GeneralsX/Generals` (or `CNC_GENERALS_PATH`) |

Zero Hour also loads base game archives through `CNC_GENERALS_PATH`; the base
game data must be present for Zero Hour and its mods.

---

## Testing a mod without a window

A quick check that a mod loads and the game simulates: play a replay headless.

```bash
cd ~/GeneralsX/Apps/GeneralsXZH.app/Contents/MacOS
perl -e 'alarm 600; exec @ARGV' ./run.sh -headless -replay /path/to/game.rep
```

macOS has no `timeout` command; `perl ... alarm` stops the run after 600
seconds. For a big mod, run the launcher's `Contents/MacOS/launch` script with
the same arguments instead of `run.sh`.

Expected: the log shows the INI files being loaded and the replay simulating.
A replay recorded without the mod (or on another platform) stops with
`CRC Mismatch` after a while; that only means the game differs from the
recording, not that the mod is broken.

---

## Troubleshooting

**Zero Hour (or a mod) fails in `Data\INI\Weapon.ini` or behaves oddly.**
The Steam / EA Zero Hour folder contains the whole base game in `ZH_Generals/`.
The engine scans the game folder recursively for `.big` files, so pointing it
at that folder loads base game archives into Zero Hour. Use `Games/ZH` (made
with `--exclude ZH_Generals`) instead.

**The mod's archives are not loaded.** The engine matches the `.big` extension
case-sensitively on macOS. `make_mod_game_dir.py` lower-cases archive
extensions; if you copy files by hand, rename `*.BIG` to `*.big`.

**Campaign crashes in a big mod (e.g. Rise of the Reds).** Some mods ship a
stub campaign that runs the original maps; those maps reference command
buttons the mod removed, and the engine stops on an unknown button. Play
Skirmish in such mods.

**A mod uses damage types the game does not know (`HEAL`, `Surrender` in
Zero Hour Reborn).** Unknown names in `Armor.ini` are now skipped with a debug
log line. Older builds of this lineage either wrote them out of bounds or
aborted the whole file.

**Units look very dark.** Check with the unmodded game on a day map first.
Some mod textures are dark by design (e.g. Rise of the Reds' Russia), and
night maps darken everything.
