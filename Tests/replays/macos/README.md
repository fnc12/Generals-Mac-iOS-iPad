# macOS test replays (re-based)

These are the `macos_*.rep` recordings from
[GeneralsXReplays](https://github.com/fbraz3/GeneralsXReplays) (commit 113cabc,
`GeneralsXZH/Replays`, MIT License, see `LICENSE.GeneralsXReplays`), modified:

- converted from the old 4-byte wide char layout to the retail 2-byte layout
  (`scripts/replays/convert_wchar32_replay.py`);
- their logic CRCs replaced with the ones the build after the GeneralsX merge computes
  (`scripts/replays/rewrite_replay_crc.py`). The player commands are unchanged.

Why: the original recordings were made by an old macOS build whose float trig
(`cosf`, `atan2f`, ...), CRC format (no DeepCRC, `RETAIL_COMPATIBLE_CRC=1`) and headless
particle handling differ from the current code, so they desync from frame 100 on.
They are still needed: `GeneralsReplays/GeneralsXZH/Maps` holds the custom maps.

`scripts/qa/replays/run-replay-tests-macos.sh` uses a replay here instead of the
one with the same name in `GeneralsReplays`. Re-base again only after a deliberate
change of the simulation or of the CRC format: play the replay with
`-ignoreReplaySyncErrors`, then run `rewrite_replay_crc.py` on the log.
