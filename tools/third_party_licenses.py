#!/usr/bin/env python3
"""The third-party notices for the firmware binaries (firmware/THIRD_PARTY_LICENSES.txt).

  third_party_licenses.py <out.txt> [--idf IDF_PATH] [--maps MAP ...]

The licences of the parts this firmware links into its images (the ESP32-P4 app and bootloader, the ESP32-C6
co-processor firmware it carries and its bootloader) ask for their notices to go with every binary copy: the MIT,
BSD and ISC ones their copyright and permission notices, Apache-2.0 a copy of the licence. This writes them all into
one text file, to ship next to the .bin files.

Each part names the archives it is linked from; a part none of the linker maps link (--maps, default: the four
images as ./build.sh and tools/build_c6.sh build them) is left out and said so. The texts are read from ESP-IDF
(--idf, default $IDF_PATH), managed_components/ and this repository; a text that is missing stops it (exit 1). Run
it after a release build, so the file matches the images shipped.
"""
import argparse
import glob
import os
import re
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
MAPS = ["build/companion_box.map", "build/bootloader/bootloader.map", "build-c6/cp/build/*.map",
        "build-c6/cp/build/bootloader/bootloader.map"]

MIT = """Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit
persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the
Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE."""

ISC = """Permission to use, copy, modify, and/or distribute this software for any purpose with or without fee is hereby
granted, provided that the above copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF
THIS SOFTWARE."""

BSD3 = """Redistribution and use in source and binary forms, with or without modification, are permitted provided that the
following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following
   disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following
   disclaimer in the documentation and/or other materials provided with the distribution.
3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote
   products derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE."""


def read(path):
    if not os.path.isfile(path):
        sys.exit("third_party_licenses: missing " + path)
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read().strip("\n")


def header_comment(path, start):
    """The C comment block of path that holds start, without its comment markers."""
    text = read(path)
    i = text.index(start)
    j = text.index("*/", i)
    lines = text[text.rindex("/*", 0, i) + 2:j].splitlines()
    return "\n".join(re.sub(r"^\s*\* ?", "", l).rstrip() for l in lines).strip("\n")


def parts(idf, mc):
    """(title, archives linked from (regex), text): the order they appear in the file."""
    c = os.path.join(idf, "components")
    return [
        ("FreeRTOS kernel, (C) 2021 Amazon.com, Inc. or its affiliates. MIT License", r"libfreertos\.a\(",
         "Copyright (C) 2021 Amazon.com, Inc. or its affiliates. All Rights Reserved.\n\n" +  # LICENSE.md has no line
         read(os.path.join(c, "freertos/FreeRTOS-Kernel/LICENSE.md"))),
        ("lwIP, (C) 2001, 2002 Swedish Institute of Computer Science. BSD-3-Clause", r"liblwip\.a\(",
         read(os.path.join(c, "lwip/lwip/COPYING"))),
        ("wpa_supplicant (hostap), (C) 2002-2022 Jouni Malinen and contributors. BSD-3-Clause (the BSD option of its "
         "dual licence)", r"libwpa_supplicant\.a\(", read(os.path.join(c, "wpa_supplicant/COPYING"))),
        ("cJSON, (C) 2009-2017 Dave Gamble and cJSON contributors. MIT License", r"libjson\.a\(",
         read(os.path.join(c, "json/cJSON/LICENSE"))),
        ("http_parser, (C) Igor Sysoev; Joyent, Inc. and other Node contributors. MIT License", r"libhttp_parser\.a\(",
         read(os.path.join(c, "http_parser/LICENSE.txt"))),
        ("TLSF allocator, (C) 2006-2016 Matthew Conte. BSD-3-Clause", r"tlsf\.c\.obj",
         "Copyright (c) 2006-2016, Matthew Conte\nAll rights reserved.\n\n" + BSD3),
        ("SD/MMC protocol definitions, (C) 2006 Uwe Stuehler (OpenBSD). ISC License", r"libsdmmc\.a\(",
         header_comment(os.path.join(c, "sdmmc/include/sd_protocol_defs.h"), "Copyright (c) 2006 Uwe Stuehler")),
        ("newlib C library (libc, libm), (C) the respective parties. BSD and similar licences", r"lib(c|m)\.a\(",
         read(os.path.join(c, "newlib/COPYING.NEWLIB"))),
        ("QR Code generator library 1.8.0, (C) Project Nayuki. MIT License", r"qrcodegen\.c\.obj",
         read(os.path.join(ROOT, "components/deckui/qrcodegen/LICENSE")).split("\n\n", 1)[1].strip("\n")),
        ("Phosphor Icons (@phosphor-icons/core 2.1.1), (C) 2023 Phosphor Icons. MIT License", r"libdeckui\.a\(",
         "Copyright (c) 2023 Phosphor Icons\n\n" + MIT),
        ("Lucide (the satellite icon, lucide-react 0.525.0), (C) Lucide Contributors. ISC License", r"libdeckui\.a\(",
         "ISC License\n\nCopyright (c) for portions of Lucide are held by Cole Bemis 2013-2022 as part of Feather (MIT). All "
         "other copyright (c) for Lucide are held by Lucide Contributors 2022.\n\n" + ISC),
        ("@elgato-stream-deck/tcp 7.8.0 (the Network Dock TCP framing, ported to C), (C) 2024 Julian Waller. "
         "MIT License", r"libelgdock\.a\(", "Copyright (c) 2024 Julian Waller\n\n" + MIT),
        ("Barlow Condensed Bold (the deck font), (C) 2017 The Barlow Project Authors. SIL Open Font License 1.1",
         r"libstatusui\.a\(", read(os.path.join(ROOT, "tools/fonts/OFL.txt"))),
    ]


APACHE = [  # (what, archives linked from)
    ("ESP-IDF 5.5.6, (C) Espressif Systems, with its prebuilt Wi-Fi, PHY, coexistence and Bluetooth controller "
     "libraries (the Wi-Fi library also carries parts of FreeBSD net80211, (C) 2004-2008 Sam Leffler, Errno "
     "Consulting, under the BSD licence)", r"libesp_system\.a\("),
    ("Mbed TLS (libmbedcrypto), (C) The Mbed TLS Contributors (the Apache-2.0 option of its dual licence)",
     r"libmbedcrypto\.a\("),
    ("espressif/esp_hosted {esp_hosted}, (C) Espressif Systems", r"libeh_[a-z_0-9]+\.a\("),
    ("espressif/esp_wifi_remote {esp_wifi_remote}, (C) Espressif Systems", r"libespressif__esp_wifi_remote\.a\("),
    ("espressif/mdns {mdns}, (C) Espressif Systems", r"libespressif__mdns\.a\("),
    ("espressif/esp_codec_dev {esp_codec_dev}, (C) Espressif Systems", r"libespressif__esp_codec_dev\.a\("),
    ("espressif/eppp_link {eppp_link}, (C) Espressif Systems", r"libespressif__eppp_link\.a\("),
    ("espressif/esp_serial_slave_link {esp_serial_slave_link}, (C) Espressif Systems",
     r"libespressif__esp_serial_slave_link\.a\("),
]


def versions():
    text = read(os.path.join(ROOT, "dependencies.lock"))
    out = {}
    for m in re.finditer(r"(?m)^  espressif/([a-z_]+):\n((?:    .*\n)+)", text + "\n"):
        v = re.search(r"\n    version: ([^\n]+)", "\n" + m.group(2))
        out[m.group(1)] = v.group(1).strip() if v else "?"
    return out


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("out")
    ap.add_argument("--idf", default=os.environ.get("IDF_PATH", ""))
    ap.add_argument("--maps", nargs="*")
    a = ap.parse_args(argv[1:])
    if not a.idf:
        sys.exit("third_party_licenses: no ESP-IDF: pass --idf or source tools/idf-env.sh")
    maps = a.maps or sorted(p for g in MAPS for p in glob.glob(os.path.join(ROOT, g)))
    if not maps:
        sys.exit("third_party_licenses: no linker maps: build first (./build.sh)")
    linked = "\n".join(read(m) for m in maps)
    mc = os.path.join(ROOT, "managed_components")
    ver = versions()
    sec = []
    left = []
    for title, rx, text in parts(a.idf, mc):
        if re.search(rx, linked):
            sec.append((title, text))
        else:
            left.append(title)
    apache = [what.format(**ver) for what, rx in APACHE if re.search(rx, linked)]
    left += [what.format(**ver) for what, rx in APACHE if not re.search(rx, linked)]
    rule = "=" * 118
    head = ("Third-party notices for the Companion Dock firmware images (firmware/*.bin)\n\n"
            "The firmware's own code is under the PolyForm Shield License 1.0.0 (LICENSE). The images also contain the\n"
            "parts below, each under its own licence; their notices follow. Generated by tools/third_party_licenses.py\n"
            "from the linker maps of the images.\n\n"
            "Also linked, with nothing to pass on: libgcc and libstdc++ (GPL-3.0 with the GCC Runtime Library\n"
            "Exception 3.1, which places no conditions on the images), and the Fast PBKDF2 code in ESP-IDF's\n"
            "wpa_supplicant port, (C) 2015 Joseph Birr-Pixton (CC0 1.0, public domain).\n")
    body = [head, rule, "Apache License 2.0", rule,
            "These parts are under the Apache License, Version 2.0 (the text follows):\n" +
            "\n".join("- " + w for w in apache) + "\n\n" + read(os.path.join(a.idf, "LICENSE"))]
    for title, text in sec:
        body += [rule, title, rule, text]
    with open(a.out, "w", encoding="utf-8") as f:
        f.write("\n\n".join(body) + "\n")
    print("third_party_licenses: %s: %d parts (+ %d under Apache-2.0) from %d linker maps" %
          (os.path.relpath(a.out), len(sec), len(apache), len(maps)))
    for t in left:
        print("  not linked, left out: " + t.split(",")[0])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
