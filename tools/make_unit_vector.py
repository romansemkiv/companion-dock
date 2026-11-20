#!/usr/bin/env python3
"""The unit record's shared test files in test/vectors/ (made by this script; the C reader's host
test and the encoder's test both read them):
  unit-record-1.bin   a whole 4096-byte sector holding one record
  unit-payloads.json  payloads, each with what a reader must make of it under a correct header
                      ("ok", with its caseSn, or "unreadable"), so both readers are held to the same bytes

A unit record is the last 4 KB flash sector, 0xFFF000:
  0..7   magic "CDUNIT01" (ASCII)
  8..9   payload length, u16 little-endian, 1..1024
  10..13 CRC-32 of the payload, u32 little-endian: zlib.crc32 (IEEE, init and final xor 0xFFFFFFFF)
  14..15 zero (reserved)
  16..   payload: compact UTF-8 JSON, an object from its first byte to its last, nested at most 4 deep,
         no key twice at its top level, no \\u0000, no control character inside a string
  rest   0xFF
The values are invented (the board serial is a test board's MAC).

  python3 tools/make_unit_vector.py DIR          write both files into DIR
  python3 tools/make_unit_vector.py --check DIR  exit 1 unless DIR's files are exactly these
"""
import json
import os
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


def rec(**over) -> bytes:
    return json.dumps({**FIELDS, **over}, separators=(",", ":")).encode("utf-8")


def plus(extra: bytes, **over) -> bytes:
    """The record with extra (',"key":value...') before its closing brace."""
    return rec(**over)[:-1] + extra + b"}"


def vector() -> bytes:
    payload = rec()
    head = MAGIC + struct.pack("<HIH", len(payload), zlib.crc32(payload), 0)
    out = head + payload
    return out + b"\xff" * (SECTOR - len(out))


def cases():
    deep = (1024 - len(plus(b',"x":'))) // 2  # as deep as a 1024-byte payload goes
    longest = plus(b',"x":"' + b"x" * (1024 - len(plus(b',"x":""'))) + b'"')
    assert len(longest) == 1024 and len(plus(b',"x":' + b"[" * deep + b"]" * deep)) <= 1024
    ok, bad = "ok", "unreadable"
    return [
        ("the record", rec(), ok, "CD-TEST-0001"),
        ("a 1024-byte payload", longest, ok, "CD-TEST-0001"),
        ("other keys", plus(b',"x":{"a":[1,null,true]},"y":"z"'), ok, "CD-TEST-0001"),
        ("v written 1.0", b'{"v":1.0' + rec()[len(b'{"v":1'):], ok, "CD-TEST-0001"),
        ("an escape in caseSn", rec(caseSn="@BC").replace(b"@", b"\\u0041"), ok, "ABC"),
        ("an escaped backslash before u0000", rec(caseSn="a\\u0000b"), ok, "a\\u0000b"),
        ("brackets inside a string", rec(caseSn="[[[[[[{{{{"), ok, "[[[[[[{{{{"),
        ("the same key in two nested objects", plus(b',"x":{"a":1},"y":{"a":2}'), ok, "CD-TEST-0001"),
        ("nested 4 deep", plus(b',"x":[[[]]]'), ok, "CD-TEST-0001"),
        ("UTF-8 in another key", plus(',"x":"é"'.encode("utf-8")), ok, "CD-TEST-0001"),
        ("invalid UTF-8 in another key", plus(b',"x":"\xff"'), ok, "CD-TEST-0001"),
        ("a \\u0000 in caseSn", rec(caseSn="a@b").replace(b"@", b"\\u0000"), bad, None),
        ("a \\u0000 in another key", plus(b',"x":"\\u0000"'), bad, None),
        ("a \\u0000 in a key", plus(b',"\\u0000":1'), bad, None),
        ("nested 5 deep", plus(b',"x":[[[[]]]]'), bad, None),
        ("nested 6 deep, an object in arrays", plus(b',"a":[[[[{}]]]]'), bad, None),
        ("nested %d deep" % deep, plus(b',"x":' + b"[" * deep + b"]" * deep), bad, None),
        ("1000 [", b"[" * 1000, bad, None),
        ("a space after it", rec() + b" ", bad, None),
        ("a newline after it", rec() + b"\n", bad, None),
        ("a space before it", b" " + rec(), bad, None),
        ("a byte order mark before it", b"\xef\xbb\xbf" + rec(), bad, None),
        ("a second object after it", rec() + b"{}", bad, None),
        ("cut short", rec()[:-1], bad, None),
        ("caseSn twice", plus(b',"caseSn":"CD-TEST-0002"'), bad, None),
        ("caseSn twice, once escaped", plus(b',"case\\u0053n":"CD-TEST-0002"'), bad, None),
        ("another key twice", plus(b',"x":1,"x":1'), bad, None),
        ("a tab inside a string", plus(b',"x":"a\tb"'), bad, None),
        ("a NUL byte inside a string", plus(b',"x":"a\x00b"'), bad, None),
        ("a NUL byte after it", rec() + b"\x00", bad, None),
        ("an array", b"[1]", bad, None),
        ("not JSON", b"hello", bad, None),
        ("v 2", rec(v=2), bad, None),
        ("caseSn not ASCII", rec(caseSn="CD-TÉST"), bad, None),
        ("caseSn 33 characters", rec(caseSn="x" * 33), bad, None),
    ]


def payloads_json() -> bytes:
    out = [{"name": n, "status": s, **({"caseSn": sn} if sn is not None else {}), "hex": p.hex()}
           for n, p, s, sn in cases()]
    return (json.dumps({"board": FIELDS["board"], "cases": out}, indent=1) + "\n").encode("utf-8")


FILES = {"unit-record-1.bin": vector, "unit-payloads.json": payloads_json}


def main(argv) -> int:
    if len(argv) == 3 and argv[1] == "--check":
        bad = 0
        for name, make in FILES.items():
            path = os.path.join(argv[2], name)
            with open(path, "rb") as f:
                if f.read() != make():
                    print(f"{path} is not what tools/make_unit_vector.py makes", file=sys.stderr)
                    bad = 1
        return bad
    if len(argv) == 2 and not argv[1].startswith("-"):
        for name, make in FILES.items():
            with open(os.path.join(argv[1], name), "wb") as f:
                f.write(make())
        return 0
    print("usage: make_unit_vector.py DIR | --check DIR", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
