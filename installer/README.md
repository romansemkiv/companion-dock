# Installer

The browser installer at <https://romansemkiv.github.io/companion-dock/>: it flashes the latest release from
[`firmware/`](../firmware) onto a board over USB, from Chrome or Edge, with nothing to install. How to use it is in
the main [README](../README.md#1-flash-the-firmware).

- `index.html` sends the visitor on to `flasher/`.
- `flasher/index.html` is the installer page. It reads `../firmware/latest.json`, downloads the full image it names,
  checks its SHA-256 and writes it at `0x0` with [esptool-js](https://github.com/espressif/esptool-js) 0.7.1
  (`flasher/esptool-js-0.7.1.mjs`, Apache-2.0, bundling pako, MIT: their licences are next to it). It flashes at
  115200 baud and restarts the board itself, as esptool-js's own baud change and hard reset don't work on this board.
- `.github/workflows/pages.yml` publishes `installer/` and `firmware/` together as the site on every push that
  touches them.

To try a change locally, serve the two folders the way the site has them, for example:

```sh
mkdir -p /tmp/site && cp -R installer/. /tmp/site/ && cp -R firmware /tmp/site/firmware
python3 -m http.server -d /tmp/site 8000   # then open http://localhost:8000/ in Chrome
```
