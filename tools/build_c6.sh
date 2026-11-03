#!/usr/bin/env bash
# The ESP32-C6 firmware that this P4 image carries (components/c6fw keeps the
# C6 on it): the co-processor side of ESP-Hosted at exactly the host
# component's version (espressif/esp_hosted in dependencies.lock), built from
# Espressif's Wi-Fi + BLE example (Wi-Fi station and AP, BLE over the SDIO
# link, two OTA slots). Any P4 firmware that builds the same example at the
# same version carries the same C6 firmware, so switching the P4 between such
# firmwares never re-flashes the C6.
# This firmware never enables Bluetooth on the P4 side; the C6's BLE controller
# just idles. The C6's SDIO pins are its fixed ones, as the board wires them.
#
#   tools/build_c6.sh       then idf.py build (it picks the image up by itself)
#
# Output: build-c6/network_adapter.bin (the image), build-c6/version.txt (its
# ESP-Hosted version). Needs ESP-IDF 5.5 (tools/idf-env.sh is sourced when
# idf.py is not on PATH) and the component registry (the example's sources).
#
# First flash by hand: an ESP-Hosted 3.x host cannot talk to a C6 on an older
# ESP-Hosted at all, so the P4 cannot update it. If the C6 does not run 3.x,
# flash it once over its programming header (H7 on the schematic: TX, RX, GND;
# hold the C6's BOOT, GPIO9, to GND while it resets) with a USB-UART adapter:
#   idf.py -C build-c6/cp -p /dev/ttyUSB0 flash
# From then on the P4 keeps it in step.
set -euo pipefail
cd "$(dirname "$0")/.."

# The host's esp_hosted version: the C6 runs the same one.
ver="$(awk '/^  espressif\/esp_hosted:/{f=1;next} f&&/^  [^ ]/{f=0} f&&/^    version:/{print $2;exit}' dependencies.lock)"
case "$ver" in
    [0-9]*.[0-9]*.[0-9]*) ;;
    *) echo "no espressif/esp_hosted version in dependencies.lock (run idf.py reconfigure first)" >&2; exit 1 ;;
esac
if ! command -v idf.py >/dev/null; then
    # shellcheck disable=SC1091
    source tools/idf-env.sh >/dev/null
fi
command -v idf.py >/dev/null || { echo "idf.py not found: source tools/idf-env.sh first (ESP-IDF 5.5)" >&2; exit 1; }

out="build-c6"
src="$out/cp"                   # create-project-from-example names it after the example's folder
rm -rf "$src"
mkdir -p "$out"
(cd "$out" && idf.py create-project-from-example "espressif/esp_hosted=$ver:bluetooth/esp_hosted_nimble/bleprph_wifi_coex/cp")
# The example takes any esp_hosted: this one, exactly.
manifest="$src/main/idf_component.yml"
python3 - "$manifest" "$ver" <<'EOF'
import re, sys
p, ver = sys.argv[1], sys.argv[2]
s = open(p).read()
s2, n = re.subn(r"(espressif/esp_hosted:\s*\n\s*version:\s*)['\"]?[^'\"\n]*['\"]?", r'\g<1>"' + ver + '"', s)
if n != 1:
    sys.exit(f"{p}: no espressif/esp_hosted version to pin")
open(p, "w").write(s2)
EOF
(cd "$src" && idf.py set-target esp32c6 && idf.py build)
bin="$(python3 -c 'import json,sys;d=json.load(open(sys.argv[1]));print(d["build_dir"]+"/"+d["app_bin"])' "$src/build/project_description.json")"
cp "$bin" "$out/network_adapter.bin"
echo "$ver" > "$out/version.txt"
echo "C6 firmware: ESP-Hosted $ver, $(wc -c < "$out/network_adapter.bin") bytes -> $out/network_adapter.bin"
