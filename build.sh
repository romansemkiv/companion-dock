#!/usr/bin/env bash
# Build (and optionally flash) the Companion Box firmware on macOS / Linux.
# This tree has one board, the Waveshare ESP32-P4-WIFI6-POE-ETH (ESP-IDF 5.5.6).
#   ./build.sh                  development build (build/), version <version.txt>-dev+g<commit>[.dirty]
#                               (a CBX_VERSION in the environment is ignored, and said so)
#   ./build.sh -p PORT          build, flash (bootloader, partition table, otadata, app), monitor
#   ./build.sh --release        release build (build-release/, its sdkconfig made afresh from
#                               sdkconfig.defaults), version exactly version.txt (or $CBX_VERSION);
#                               refuses uncommitted changes. With -p PORT it flashes that build.
#   ./build.sh --test           host unit tests and the web page check (tools/test-host.sh)
# Every firmware build first makes the ESP32-C6 firmware that the image carries (build-c6/,
# tools/build_c6.sh) when it is missing or of another ESP-Hosted version than dependencies.lock, and
# configures CMake again: ESP-IDF works the version out only at configure time, so without that a
# build after a commit or an edit would still report the commit (and the ".dirty") it was configured at.
set -euo pipefail
cd "$(dirname "$0")"

port="" release=0
while [ $# -gt 0 ]; do
    case "$1" in
        -p|--port) [ $# -ge 2 ] || { echo "-p needs a port" >&2; exit 2; }; port="$2"; shift 2 ;;
        --release) release=1; shift ;;
        --test) exec tools/test-host.sh ;;
        -h|--help) sed -n '2,14p' "$0"; exit 0 ;;
        *) echo "unknown option $1 (see ./build.sh --help)" >&2; exit 2 ;;
    esac
done

if ! command -v idf.py >/dev/null; then
    set +u # ESP-IDF's export.sh reads unset variables
    # shellcheck disable=SC1091
    source tools/idf-env.sh >/dev/null
    set -u
fi
command -v idf.py >/dev/null || { echo "idf.py not found: source tools/idf-env.sh first (ESP-IDF 5.5.6)" >&2; exit 1; }
idf_ver="$(idf.py --version 2>/dev/null || true)"
case "$idf_ver" in
    *v5.5*) ;;
    *) echo "this firmware builds with ESP-IDF 5.5.6; the exported one is: $idf_ver" >&2; exit 1 ;;
esac

# The C6's firmware, built once per ESP-Hosted version
hosted="$(awk '/^  espressif\/esp_hosted:/{f=1;next} f&&/^  [^ ]/{f=0} f&&/^    version:/{print $2;exit}' dependencies.lock)"
if [ "$(cat build-c6/version.txt 2>/dev/null || true)" != "$hosted" ] || [ ! -f build-c6/network_adapter.bin ]; then
    tools/build_c6.sh
fi

if [ "$release" = 1 ]; then
    [ -z "$(git status --porcelain --untracked-files=no)" ] || {
        echo "a release builds committed sources only: commit or stash your changes first" >&2; exit 1; }
    ver="${CBX_VERSION:-$(head -n 1 version.txt)}"
    unset CBX_VERSION # passed as -DCBX_VERSION instead, which CMake announces as a release version
    dir=build-release
    rm -f "$dir/sdkconfig" # from sdkconfig.defaults alone: nothing set by hand ships
    args=(-B "$dir" -D SDKCONFIG="$dir/sdkconfig" -D CBX_VERSION="$ver")
else
    if [ -n "${CBX_VERSION:-}" ]; then
        echo "ignoring CBX_VERSION=$CBX_VERSION: a development build is named after the commit (--release for a release)" >&2
    fi
    unset CBX_VERSION
    dir=build
    args=(-B "$dir" -D CBX_VERSION=) # also clears one an earlier -DCBX_VERSION left in the cache
fi

idf.py "${args[@]}" reconfigure build
built="$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["project_version"])' "$dir/project_description.json")"
echo "firmware version: $built ($dir/)"
if [ -n "$port" ]; then
    idf.py "${args[@]}" -p "$port" flash monitor
fi
