#!/usr/bin/env python3
"""Replace the logic CRCs stored in a replay with the ones the current build computes.

    rewrite_replay_crc.py <in.rep> <game.log> <out.rep>

Use it when a deliberate change of the simulation or of the CRC format (for example the
GeneralsX merge: GameMath, DeepCRC, RETAIL_COMPATIBLE_CRC=0) makes an existing test replay
desync. The player commands are kept as they are; only the CRC values change, so the
replay keeps testing that later builds simulate exactly like the current one.

<in.rep> must be in the current format (2-byte wide chars; convert old macOS/Linux
replays with convert_wchar32_replay.py first). <game.log> is the output of playing
<in.rep> with the current build and -ignoreReplaySyncErrors, which prints one line per
mismatch:

    CRC Mismatch in Frame <n> (ignored) InGame:<new> Replay:<old>

Every CRC command whose value is a known <old> is rewritten to <new>, for every player
(in a synced game all players send the same CRC). Layout: Recorder.cpp
(readReplayHeader / writeToFile); argument sizes: Common/MessageStream.h.
"""
import re
import struct
import sys

MAX_SLOTS = 8
GENREP = b"GENREP"
ARG_INTEGER = 0
ARG_BOOLEAN = 2
ARG_SIZES = {0: 4, 1: 4, 2: 1, 3: 4, 4: 4, 5: 4, 6: 12, 7: 8, 8: 16, 9: 4, 10: 2}
MISMATCH = re.compile(r"CRC Mismatch in Frame (\d+) \(ignored\) InGame:([0-9A-Fa-f]{8}) Replay:([0-9A-Fa-f]{8})")


def skip_utf16z(data, pos):
    while data[pos:pos + 2] != b"\0\0":
        pos += 2
    return pos + 2


def skip_asciiz(data, pos):
    return data.index(b"\0", pos) + 1


def header_end(data):
    if not data.startswith(GENREP):
        raise ValueError("not a replay (no GENREP)")
    pos = len(GENREP) + 4 + 4 + 4 + 1 + 1 + MAX_SLOTS
    pos = skip_utf16z(data, pos)        # replay name
    pos += 16                           # SYSTEMTIME
    pos = skip_utf16z(data, pos)        # version string
    pos = skip_utf16z(data, pos)        # build time string
    pos += 4 + 4 + 4                    # version number, exe CRC, ini CRC
    if b"M=" not in data[pos:data.index(b"\0", pos)]:
        raise ValueError("game options not found: old-format replay? convert it first")
    pos = skip_asciiz(data, pos)        # game options
    pos = skip_asciiz(data, pos)        # local player index
    return pos + 4 + 4 + 4 + 4          # difficulty, game mode, rank points, max FPS


def crc_map(log_path):
    mapping = {}
    with open(log_path, errors="replace") as f:
        for line in f:
            m = MISMATCH.search(line)
            if m:
                old, new = int(m.group(3), 16), int(m.group(2), 16)
                if mapping.get(old, new) != new:
                    raise ValueError(f"replay CRC {old:08X} maps to two different values")
                mapping[old] = new
    return mapping


def rewrite(data, mapping):
    out = bytearray(data)
    pos = header_end(data)
    rewritten, crc_types = 0, set()
    while pos < len(data):
        _frame, msg_type, _player = struct.unpack_from("<III", data, pos)
        pos += 12
        num_types = data[pos]
        pos += 1
        types = [(data[pos + 2 * i], data[pos + 2 * i + 1]) for i in range(num_types)]
        pos += 2 * num_types
        # MSG_LOGIC_CRC: the CRC as one integer; older replays add a boolean "is playback" flag.
        if types in ([(ARG_INTEGER, 1)], [(ARG_INTEGER, 1), (ARG_BOOLEAN, 1)]):
            value = struct.unpack_from("<I", data, pos)[0]
            if value in mapping:
                struct.pack_into("<I", out, pos, mapping[value])
                rewritten += 1
                crc_types.add(msg_type)
        for arg_type, count in types:
            if arg_type not in ARG_SIZES:
                raise ValueError(f"unknown argument type {arg_type} at offset {pos}")
            pos += ARG_SIZES[arg_type] * count
    if len(crc_types) > 1:
        raise ValueError(f"rewritten commands of several message types {sorted(crc_types)}")
    return bytes(out), rewritten


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    mapping = crc_map(sys.argv[2])
    if not mapping:
        sys.exit(f"{sys.argv[2]}: no ignored CRC mismatches (nothing to rewrite)")
    with open(sys.argv[1], "rb") as f:
        data = f.read()
    new, rewritten = rewrite(data, mapping)
    with open(sys.argv[3], "wb") as f:
        f.write(new)
    print(f"{sys.argv[3]}: {len(mapping)} CRC values, {rewritten} commands rewritten")


if __name__ == "__main__":
    main()
