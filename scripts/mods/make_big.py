#!/usr/bin/env python3
"""Pack a directory into a BIGF archive readable by the game engine.

Usage: make_big.py <source_dir> <output.big>

Every file under <source_dir> is stored with its path relative to
<source_dir>, using backslashes, e.g. Data\\INI\\Object\\Foo.ini.
Format (as read by Core/GameEngineDevice/Source/StdDevice/Common/StdBIGFileSystem.cpp):
  "BIGF", u32 LE archive size, u32 BE file count, u32 BE first-data offset,
  then per file: u32 BE offset, u32 BE size, NUL-terminated name.
"""
import os
import struct
import sys


def collect(src):
    files = []
    for root, _, names in os.walk(src):
        for name in names:
            if name.startswith('.'):
                continue
            path = os.path.join(root, name)
            rel = os.path.relpath(path, src).replace(os.sep, '\\')
            files.append((rel, path))
    files.sort(key=lambda f: f[0].lower())
    return files


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src, out = sys.argv[1], sys.argv[2]
    files = collect(src)
    if not files:
        sys.exit(f"no files under {src}")
    table_size = sum(8 + len(rel.encode('ascii')) + 1 for rel, _ in files)
    data_start = 16 + table_size + 8  # + "L253" marker and padding, as retail archives do
    blobs, table, offset = [], b'', data_start
    for rel, path in files:
        data = open(path, 'rb').read()
        table += struct.pack('>II', offset, len(data)) + rel.encode('ascii') + b'\0'
        blobs.append(data)
        offset += len(data)
    with open(out, 'wb') as f:
        f.write(b'BIGF' + struct.pack('<I', offset) + struct.pack('>II', len(files), data_start))
        f.write(table + b'L253' + b'\0' * 4)
        for blob in blobs:
            f.write(blob)
    print(f"{out}: {len(files)} files, {offset} bytes")


if __name__ == '__main__':
    main()
