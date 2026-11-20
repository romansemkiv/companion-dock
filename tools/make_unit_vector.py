#!/usr/bin/env python3
"""The unit record's shared test vector, test/vectors/unit-record-1.bin (made by this script; the
C reader's host test and the encoder's test both read it).

A unit record is the last 4 KB flash sector, 0xFFF000:
  0..7   magic "CDUNIT01" (ASCII)
  8..9   payload length, u16 little-endian, 1..1024
  10..13 CRC-32 of the payload, u32 little-endian: zlib.crc32 (IEEE, init and final xor 0xFFFFFFFF)
  14..15 zero (reserved)
  16..   payload: compact UTF-8 JSON
  rest   0xFF
The values are invented (the board serial is a test board's MAC).

  python3 tools/make_unit_vector.py OUT          write the vector to OUT
  python3 tools/make_unit_vector.py --check FILE exit 1 unless FILE is exactly the vector
"""
import json
import struct
import sys
import zlib

SECTOR = 4096
MAGIC = b"CDUNIT01"
FIELDS = {  # in this key order
    "v": 1,
    "board": "80f1b2d68d1c",
    "caseSn": "CD-TEST-0001",
    "caseType": "Standard",
    "edition": "standard",
    "at": "2026-11-19T12:00:00Z",
    "fw": "0.3.0",
}


def vector() -> bytes:
    payload = json.dumps(FIELDS, separators=(",", ":")).encode("utf-8")
    head = MAGIC + struct.pack("<HIH", len(payload), zlib.crc32(payload), 0)
    out = head + payload
    return out + b"\xff" * (SECTOR - len(out))


def main(argv) -> int:
    if len(argv) == 3 and argv[1] == "--check":
        with open(argv[2], "rb") as f:
            if f.read() != vector():
                print(f"{argv[2]} is not the vector tools/make_unit_vector.py makes", file=sys.stderr)
                return 1
        return 0
    if len(argv) == 2 and not argv[1].startswith("-"):
        with open(argv[1], "wb") as f:
            f.write(vector())
        return 0
    print("usage: make_unit_vector.py OUT | --check FILE", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
