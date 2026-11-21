# Flashing the firmware

This page puts the Companion Dock firmware on a Waveshare ESP32-P4-WIFI6-POE-ETH board. You only need it once
per board. After that, updates go through the box's web page, or over USB from the project's browser installer; both
keep the settings (see [Updating a running box](#updating-a-running-box)).

The files are in the [`firmware/`](../firmware) folder of this repository:

| File | What it is |
|---|---|
| `companion-dock-v0.2.9-full.bin` | the whole flash in one file, written at `0x0`. Use this one on a new board |
| `companion-dock-v0.2.9.bin` | the app alone. Use this one for updates through the web page |
| `bootloader.bin`, `partition-table.bin`, `ota_data_initial.bin` | the other parts, for flashing file by file |
| `SHA256SUMS` | checksums of all of the above |

## What you need

- the board;
- a USB-C cable that carries **data** (some cables only charge);
- a computer with Chrome or Edge (for way 1) or Python 3 (for way 2).

## Which port

Use the board's **USB-C** port. It goes to a CH343P USB-serial chip. The **USB-A** port next to it is for the
Stream Decks and can't flash the board.

- macOS and Linux need no driver. The board shows up as `/dev/cu.usbmodem…` (macOS) or `/dev/ttyACM0` (Linux).
- Windows 10 and 11 normally install a driver by themselves and show a new COM port. If no COM port appears,
  install WCH's CH343 driver from the [WCH download page](https://www.wch-ic.com/downloads/CH343SER_EXE.html).

## Way 1: in the browser

Nothing to install. It needs Chrome or Edge on a desktop computer (they have Web Serial; Safari and Firefox
don't).

> **The full image erases the box's settings.** It writes the whole flash, settings area (NVS) included, so
> Wi-Fi, the PIN and every other setting go back to defaults. Use it on a new board or for a clean start. To
> update a box that already runs this firmware, use the project's installer's **Update** or the
> [web page](#updating-a-running-box) instead.

You can use either of two pages:

- **The project's installer**, <https://romansemkiv.github.io/companion-dock/>. It loads the latest
  firmware by itself and checks it. Press **Connect**: the page reads what is on the board and offers what fits.
  **Fresh install** writes the full image and erases the settings; it installs the latest release, or an earlier
  one picked in the list under it. **Update**, offered on a board that already has Companion Dock on it, keeps
  the settings and is always the latest release (see [Updating a running box](#updating-a-running-box)). Either
  way it verifies what was written.
- **Espressif's web flasher**, <https://espressif.github.io/esptool-js/>. The board's chip is an ESP32-P4
  revision v3.2, which needs esptool-js 0.7.0 or newer; older versions can't switch this revision's flash on
  and hang after connecting. Espressif's page doesn't show its version and normally runs the latest release.
  If it hangs on this board, use the project's installer, which carries esptool-js 0.7.1, or way 2.

With Espressif's page:

1. Plug the board in, set the baud rate to **460800** and press **Connect**. Pick the board's port in the
   browser's list.
2. Under the flash address, enter **`0x0`** and choose the file **`companion-dock-v0.2.9-full.bin`**.
3. Press **Program** and wait until it says it is done.
4. Press **Disconnect**, then press the board's **RESET** button or unplug and replug it.

## Way 2: the command line

Install esptool (4.12 is the first version that knows the P4's revision v3.2):

```sh
python3 -m pip install "esptool>=4.12"
```

> **The full image erases the box's settings.** It writes the whole flash, settings area (NVS) included, so
> Wi-Fi, the PIN and every other setting go back to defaults. Use it on a new board or for a clean start. To
> update a box that already runs this firmware, use the [web page](#updating-a-running-box) instead.

Then, from the `firmware/` folder:

```sh
esptool.py --chip esp32p4 -p <PORT> -b 460800 write_flash 0x0 companion-dock-v0.2.9-full.bin
```

`<PORT>` is the board's port, for example `/dev/cu.usbmodem1101` on macOS, `/dev/ttyACM0` on Linux or `COM3` on
Windows. (esptool 5 also accepts `esptool` and `write-flash`; the names above still work.)

To write the parts one by one instead (the same result):

```sh
esptool.py --chip esp32p4 -p <PORT> -b 460800 write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m \
  0x2000 bootloader.bin \
  0x8000 partition-table.bin \
  0xf000 ota_data_initial.bin \
  0x20000 companion-dock-v0.2.9.bin
```

Press **RESET** or replug the board when it finishes.

## If it won't connect

- Try another cable. A charge-only cable gives no port at all.
- Use another USB port on the computer, directly, without a hub.
- Close anything else that may hold the port: serial monitors, the Arduino IDE, another browser tab with a
  flasher open.
- Put the board in download mode by hand: hold **BOOT**, press and release **RESET**, then release **BOOT**, and
  connect again. BOOT only matters at reset; the firmware itself doesn't use the button.

## After flashing

With no network set up yet, the decks show the setup screen with two QR codes. Scan the first to join the box's
hotspot and the second to open the setup page, then join your network there. The manual's
[First setup](manual.md#first-setup) section has the details, including adding the box in Companion.

To check that it worked: the web page shows the version on its **Firmware** page, and the deck's **Box** screen
shows it too.

## Updating a running box

**Over the network.** Open the box's web page, go to **Firmware**, choose **`companion-dock-v0.2.9.bin`** and
upload it. The settings stay. The box restarts into the new firmware, checks that it runs properly, and goes back
to the previous one if it doesn't. See the manual's [Recovery slot](manual.md#recovery-slot) section for how the
two firmware slots work.

**Over USB.** Open the project's installer, <https://romansemkiv.github.io/companion-dock/>, connect the board's
USB-C port, press **Connect** and choose **Update**. The settings stay. Use this for a box you can't reach on the
network. The page offers Update only when the board's partition table has the settings area (NVS), the boot record
(otadata) and the first app slot where this firmware has them, and no other project's app runs on it. It then
writes three parts of the full image and nothing else:

| Part | Where | Why |
|---|---|---|
| the app | `0x20000` (`ota_0`) | the new firmware |
| a blank boot record | `0xf000` (otadata) | the box starts `ota_0`, whichever slot it ran before |
| the bootloader and the partition table | `0x2000` to `0x9000` | written last, and only when the board's differ |

The settings area at `0x9000` is never written. This is what `idf.py flash` does. There is no rollback on this
path. The other slot keeps the previous firmware until the [recovery copy](manual.md#recovery-slot) replaces it,
about 10 minutes after the box starts.
