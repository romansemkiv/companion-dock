#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
# shellcheck disable=SC1091
source tools/idf-env.sh >/dev/null
cmake -S test/host -B build-host -G Ninja
cmake --build build-host
ctest --test-dir build-host --output-on-failure
# the web page: every inline script must compile (one stray quote blanks the page on the box)
node tools/check_pages.js
# ... and the page as the box embeds it (comments and indentation stripped, within its gzip limit,
# tools/pack_page.py, tested by tools/test_pack_page.py): its scripts must compile and run too
python3 -B tools/test_pack_page.py
mkdir -p build-host/www
python3 tools/pack_page.py components/webui/www/index.html build-host/www/index.html.gz --html build-host/www/index.html
node tools/check_pages.js build-host/www/index.html
