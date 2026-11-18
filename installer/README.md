# Installer

The browser installer at <https://romansemkiv.github.io/companion-dock/>: it flashes the latest release from
[`firmware/`](../firmware) onto a board over USB, from Chrome or Edge, with nothing to install. It installs afresh
(the settings are erased) or updates a box that already runs Companion Dock (the settings stay). How to use it is in
the main [README](../README.md#1-flash-the-firmware).

- `index.html` sends the visitor on to `flasher/`.
- `flasher/index.html` is the installer page. It reads `../firmware/latest.json`, downloads the full image it names
  and checks its SHA-256. On **Connect** it reads the board's bootloader, partition table, boot record and app
  header, and offers **Update** when the board is laid out for this firmware, **Fresh install** always. It writes
  with [esptool-js](https://github.com/espressif/esptool-js) 0.7.1 (`flasher/esptool-js-0.7.1.mjs`, Apache-2.0,
  bundling pako, MIT: their licences are next to it), at 115200 baud, and restarts the board itself, as esptool-js's
  own baud change and hard reset don't work on this board.
- `flasher/plan.mjs` decides what is written: the whole image at `0x0` for a fresh install; for an update the app,
  a blank boot record and, when they differ, the bootloader and the partition table, never the settings area. It
  has no DOM and no serial port in it, and `tools/test_installer.mjs` tests it against the release in `firmware/`:
  `node --test tools/test_installer.mjs` (part of `tools/test-host.sh`).
- A fresh install can write an earlier release: the page lists `../firmware/versions.json`, which
  `tools/installer_versions.py` builds at deploy time together with each earlier release's full image, out of this
  repository's history (`firmware/` itself holds only the latest). An update is always the latest. Without the file
  the page offers the latest alone. `tools/test_installer_versions.py` tests the script.
- `flasher/fonts/` holds the page's fonts, Barlow, Barlow Semi Condensed and JetBrains Mono (SIL OFL 1.1,
  `flasher/fonts/OFL.txt`). The page looks like the project's site, <https://dock.semkivlabs.tech/>.
- `.github/workflows/pages.yml` publishes `installer/` and `firmware/` together as the site, with the earlier
  releases added, on every push that touches them.

To try a change locally, serve the two folders the way the site has them, for example:

```sh
mkdir -p /tmp/site && cp -R installer/. /tmp/site/ && cp -R firmware /tmp/site/firmware
python3 tools/installer_versions.py /tmp/site/firmware   # the earlier releases, for the version list
python3 -m http.server -d /tmp/site 8000   # then open http://localhost:8000/ in Chrome
```
