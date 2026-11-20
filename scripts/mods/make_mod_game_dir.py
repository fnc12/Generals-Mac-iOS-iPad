#!/usr/bin/env python3
"""Build a per-mod "game folder" without copying or touching the original game.

    make_mod_game_dir.py [--exclude NAME ...] <original_game_dir> <mod_files_dir> <output_dir>

Big mods (Rise of the Reds, ShockWave, Contra, ...) expect their archives to sit
next to the original ones in the game folder, often with loose files under Data\\.
The engine loads the .big files of the game folder in sorted order and the first
one wins, which is why mod archives are named "!Mod_*.big". `-mod` cannot express
that (it loads one archive or one folder, last one wins, and skips loose files).

So each mod gets its own folder that looks like an installed game:
  - every directory is a real directory;
  - every original file is a link to the original (symlink on macOS/Linux,
    hard link on Windows, which needs no admin rights; same drive required);
  - the mod's files are linked on top, replacing originals with the same name;
  - GenLauncher's disabled archives "*.gib" become "*.big", and archive
    extensions are lower-cased (the engine matches ".big" case-sensitively on
    macOS/Linux).

--exclude NAME skips a top-level entry of the original game folder. Use
`--exclude ZH_Generals` for the Steam / EA Zero Hour folder: it contains the
whole base game, and the engine scans sub-folders for .big files, so base-game
archives would otherwise be loaded into Zero Hour (on Windows this breaks
Data\\INI\\Weapon.ini). An empty <mod_files_dir> gives a clean original game.

The output folder is recreated from scratch on every run. Point the game at it
with CNC_GENERALS_ZH_PATH (or CNC_GENERALS_PATH for the base game).
"""
import os
import shutil
import sys


def link(src, dst):
    if os.name == "nt":
        os.link(src, dst)
    else:
        os.symlink(os.path.abspath(src), dst)


def target_name(name):
    stem, ext = os.path.splitext(name)
    if ext.lower() in (".gib", ".big"):
        return stem + ".big"
    return name


def mirror(src_root, out_root, rename, exclude=()):
    count = 0
    skip = {e.lower() for e in exclude}
    for dirpath, dirnames, filenames in os.walk(src_root):
        rel = os.path.relpath(dirpath, src_root)
        if rel == ".":
            dirnames[:] = [d for d in dirnames if d.lower() not in skip]
            filenames = [f for f in filenames if f.lower() not in skip]
        out_dir = out_root if rel == "." else os.path.join(out_root, rel)
        os.makedirs(out_dir, exist_ok=True)
        for name in filenames:
            if name.endswith(".part"):
                continue
            dst = os.path.join(out_dir, target_name(name) if rename else name)
            # Case-insensitive replace: the mod's file wins over an original
            # with the same name in any letter case.
            for existing in os.listdir(out_dir):
                if existing.lower() == os.path.basename(dst).lower():
                    os.remove(os.path.join(out_dir, existing))
            link(os.path.join(dirpath, name), dst)
            count += 1
    return count


def main():
    args, exclude = sys.argv[1:], []
    while args[:1] == ["--exclude"] and len(args) > 1:
        exclude.append(args[1])
        args = args[2:]
    if len(args) != 3:
        sys.exit(__doc__)
    game, mod, out = args
    for p in (game, mod):
        if not os.path.isdir(p):
            sys.exit(f"not a directory: {p}")
    # The output is deleted and rebuilt: never let it be (or contain) an input.
    o = os.path.realpath(out)
    for p in (game, mod):
        r = os.path.realpath(p)
        if o == r or o.startswith(r + os.sep) or r.startswith(o + os.sep):
            sys.exit(f"output {out} overlaps input {p}")
    if os.path.exists(out):
        shutil.rmtree(out)  # removes links only, never what they point to
    n_game = mirror(game, out, rename=False, exclude=exclude)
    n_mod = mirror(mod, out, rename=True)
    print(f"{out}: {n_game} original files + {n_mod} mod files (links)")


if __name__ == "__main__":
    main()
