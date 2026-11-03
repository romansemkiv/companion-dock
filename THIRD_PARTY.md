# Third-party software

The firmware's own code is under the PolyForm Shield 1.0.0 licence ([LICENSE](LICENSE)). These parts come from other projects
and keep their own licences. The firmware image (`firmware/*.bin`) contains the ones marked **in the image**,
so pass their notices on with the binaries.

## In this repository

| What | Where | Licence | Notes |
|---|---|---|---|
| [Barlow Condensed](https://github.com/jpt/barlow) Bold, © 2017 The Barlow Project Authors | `tools/fonts/BarlowCondensed-Bold.ttf`, rasterised into `components/statusui/deck_font.c` | SIL Open Font License 1.1 (`tools/fonts/OFL.txt`) | **in the image**: the deck text. Ship `tools/fonts/OFL.txt` with any binary |
| [Lucide](https://lucide.dev) satellite icon (lucide-react 0.525.0), © Lucide Contributors (portions © 2013-2022 Cole Bemis, Feather) | `components/deckui/icons.c` (as strokes), `i-satellite` in `components/webui/www/index.html` | ISC | **in the image**: the Satellite mode icon |
| [Phosphor Icons](https://phosphoricons.com), © 2023 Phosphor Icons | `components/deckui/phosphor.c` / `.h` (outlines from `@phosphor-icons/core` 2.1.1), the icons in `components/webui/www/index.html` | MIT (notice in `phosphor.c`) | **in the image**: deck and web page icons |
| [QR Code generator library](https://www.nayuki.io/page/qr-code-generator-library), © Project Nayuki | `components/deckui/qrcodegen/` (v1.8.0, unmodified) | MIT (`components/deckui/qrcodegen/LICENSE`) | **in the image**: the QR codes on the decks |
| [@elgato-stream-deck/tcp](https://github.com/Julusian/node-elgato-stream-deck) 7.8.0, © 2024 Julian Waller | `components/elgdock/cora.h` / `cora.c`: the Network Dock TCP framing, ported to C | MIT (notice in `cora.h`) | **in the image** |
| Companion Satellite conformance corpus, from [bitfocus/companion](https://github.com/bitfocus/companion) `satellite-protocol` | `test/host/fixtures/satellite-protocol-corpus.json` | MIT | host tests only |

## Pulled in at build time

These are fetched by ESP-IDF's component manager (`dependencies.lock`) or come with ESP-IDF. They are not
committed here, but their code ends up in the image.

| Component | Version | Licence |
|---|---|---|
| [ESP-IDF](https://github.com/espressif/esp-idf) | 5.5.6 | Apache-2.0. It brings its own third-party parts (FreeRTOS, lwIP, Mbed TLS, newlib, cJSON and others), listed in ESP-IDF's `COPYRIGHT.rst` |
| cJSON (part of ESP-IDF) | as in ESP-IDF 5.5.6 | MIT |
| [espressif/esp_hosted](https://components.espressif.com/components/espressif/esp_hosted) | 3.0.9 | Apache-2.0 for the parts used here (its Linux kernel driver, GPL-2.0, is not built); bundles protobuf-c (BSD-3-Clause). The ESP32-C6 firmware the image carries is built from its co-processor example (`tools/build_c6.sh`) |
| [espressif/esp_wifi_remote](https://components.espressif.com/components/espressif/esp_wifi_remote) | 1.6.5 | Apache-2.0 |
| [espressif/wifi_remote_over_eppp](https://components.espressif.com/components/espressif/wifi_remote_over_eppp) | 0.3.3 | Apache-2.0 |
| [espressif/eppp_link](https://components.espressif.com/components/espressif/eppp_link) | 1.1.6 | Apache-2.0 |
| [espressif/esp_serial_slave_link](https://components.espressif.com/components/espressif/esp_serial_slave_link) | 1.1.2 | Apache-2.0 |
| [espressif/mdns](https://components.espressif.com/components/espressif/mdns) | 1.14.1 | Apache-2.0 |
| [espressif/esp_codec_dev](https://components.espressif.com/components/espressif/esp_codec_dev) | 1.6.2 | Apache-2.0 |
| Unity (part of ESP-IDF) | as in ESP-IDF 5.5.6 | MIT; host tests only |

`tools/dock_check` installs `@elgato-stream-deck/tcp` 7.8.0 (MIT) from npm. It is a development tool and
none of it is in the firmware.

## The browser installer

| What | Where | Licence |
|---|---|---|
| [esptool-js](https://github.com/espressif/esptool-js) 0.7.1, © Espressif Systems; bundles [pako](https://github.com/nodeca/pako) 2.1.0, © 2014-2017 Vitaly Puzrin and Andrei Tuputcyn | `installer/flasher/esptool-js-0.7.1.mjs` | Apache-2.0 ([`esptool-js-LICENSE.txt`](installer/flasher/esptool-js-LICENSE.txt)); pako MIT ([`pako-LICENSE.txt`](installer/flasher/pako-LICENSE.txt)) |

## Protocols

The Network Dock mode follows the Elgato TCP protocol as implemented in
[`@elgato-stream-deck`](https://github.com/Julusian/node-elgato-stream-deck) (MIT), and the Satellite mode
follows Companion's Satellite API. Only the TCP framing is ported from `@elgato-stream-deck/tcp` (above); otherwise
both served as references, and no other code from them is in the firmware.

## Trademarks

Not affiliated with or endorsed by Elgato / Corsair, Bitfocus AS, Waveshare or Adafruit. Elgato, Stream Deck and
Network Dock are trademarks of Corsair; Companion is a product of Bitfocus AS; Waveshare and Adafruit are
trademarks of their owners. The names only say what the firmware works with, and its Network Dock mode only speaks
the same network protocol as Elgato's Network Dock.

## Notices to ship with the binaries

`firmware/THIRD_PARTY_LICENSES.txt` holds the notices of every third-party part linked into the images (the
ESP32-P4 app and bootloader and the ESP32-C6 firmware they carry): the Apache-2.0 text, and the MIT, BSD, ISC and
OFL notices those licences ask to go with a binary copy. Pass it on with any `.bin` file. `tools/third_party_licenses.py`
writes it from the linker maps of a build: run it after every release build.
