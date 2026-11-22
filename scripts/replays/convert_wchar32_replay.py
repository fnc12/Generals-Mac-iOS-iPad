#!/usr/bin/env python3
"""Convert a replay written by an old macOS/Linux build (4-byte wide chars) to the
retail layout (2-byte UTF-16 wide chars) that the engine reads now.

    convert_wchar32_replay.py <old.rep> <new.rep>

Before 2026-11-18 the macOS/Linux builds wrote replay wide strings and
ARGUMENTDATATYPE_WIDECHAR command arguments with sizeof(wchar_t) == 4. The game
commands themselves are the same, so only these fields are rewritten; every other
byte is copied as is. Layout: GeneralsMD/Code/GameEngine/Source/Common/Recorder.cpp
(startRecording / readReplayHeader / writeToFile).
"""
import struct
import sys

MAX_SLOTS = 8
GENREP = b"GENREP"

# GameMessageArgumentDataType (Common/MessageStream.h) -> size on disk
ARG_SIZES = {
    0: 4,   # INTEGER
    1: 4,   # REAL
    2: 1,   # BOOLEAN
    3: 4,   # OBJECTID
    4: 4,   # DRAWABLEID
    5: 4,   # TEAMID
    6: 12,  # LOCATION (Coord3D)
    7: 8,   # PIXEL (ICoord2D)
    8: 16,  # PIXELREGION (IRegion2D)
    9: 4,   # TIMESTAMP
}
ARG_WIDECHAR = 10


class Reader:
    def __init__(self, data):
        self.data, self.pos = data, 0

    def take(self, n):
        if self.pos + n > len(self.data):
            raise ValueError(f"unexpected end of file at offset {self.pos}")
        chunk = self.data[self.pos:self.pos + n]
        self.pos += n
        return chunk

    def u32(self):
        return struct.unpack("<I", self.take(4))[0]

    def wide32(self):
        """Read a 4-byte wide string up to its terminator, return the code points."""
        chars = []
        while True:
            c = self.u32()
            if c == 0:
                return chars
            chars.append(c)

    def ascii_z(self):
        end = self.data.index(b"\0", self.pos)
        chunk = self.data[self.pos:end + 1]
        self.pos = end + 1
        return chunk


def utf16(code_points):
    out = bytearray()
    for c in code_points:
        if c > 0xFFFF:
            raise ValueError(f"code point U+{c:X} does not fit the retail format")
        out += struct.pack("<H", c)
    return bytes(out + b"\0\0")


def convert(data):
    r, out = Reader(data), bytearray()
    if r.take(len(GENREP)) != GENREP:
        raise ValueError("not a replay (no GENREP)")
    out += GENREP
    # start/end time (int32), frame count, desync + quit flags, one flag per slot
    out += r.take(4 + 4 + 4 + 1 + 1 + MAX_SLOTS)
    out += utf16(r.wide32())          # replay name
    out += r.take(16)                 # SYSTEMTIME
    out += utf16(r.wide32())          # version string
    out += utf16(r.wide32())          # build time string
    out += r.take(4 + 4 + 4)          # version number, exe CRC, ini CRC
    game_options = r.ascii_z()
    if b"M=" not in game_options:
        raise ValueError("game options string not found where expected: is this an old-format replay?")
    out += game_options
    out += r.ascii_z()                # local player index
    out += r.take(4 + 4 + 4 + 4)      # difficulty, game mode, rank points, max FPS

    commands = 0
    while r.pos < len(data):
        out += r.take(4 + 4 + 4)      # frame, message type, player index
        num_types = r.take(1)
        out += num_types
        types = []
        for _ in range(num_types[0]):
            pair = r.take(2)
            out += pair
            types.append((pair[0], pair[1]))
        for arg_type, count in types:
            for _ in range(count):
                if arg_type == ARG_WIDECHAR:
                    out += utf16([r.u32()])[:2]
                elif arg_type in ARG_SIZES:
                    out += r.take(ARG_SIZES[arg_type])
                else:
                    raise ValueError(f"unknown argument type {arg_type} in command {commands}")
        commands += 1
    return bytes(out), commands


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    with open(sys.argv[1], "rb") as f:
        data = f.read()
    try:
        new, commands = convert(data)
    except ValueError as e:
        # Also what a replay that is already in the new format gives.
        sys.exit(f"{sys.argv[1]}: not an old-format replay ({e})")
    with open(sys.argv[2], "wb") as f:
        f.write(new)
    print(f"{sys.argv[2]}: {commands} commands, {len(data)} -> {len(new)} bytes")


if __name__ == "__main__":
    main()
