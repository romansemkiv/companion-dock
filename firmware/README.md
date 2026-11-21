# Firmware 0.2.9

| File | Write at | Use |
|---|---|---|
| `companion-dock-v0.2.9-full.bin` | `0x0` | A new board or a clean start. Erases the settings. |
| `companion-dock-v0.2.9.bin` | `0x20000`, or upload on the box's Firmware page | Updating a running box. The settings stay. |
| `bootloader.bin` | `0x2000` | File-by-file install |
| `partition-table.bin` | `0x8000` | File-by-file install |
| `ota_data_initial.bin` | `0xf000` | File-by-file install |

Flash settings: DIO, 80 MHz, 16 MB. `SHA256SUMS` has the checksums. The browser installer at
<https://romansemkiv.github.io/companion-dock/> flashes the same full image: all of it for a fresh install, or its
bootloader, partition table and app without the settings area for an update that keeps the settings. The image contains the
Barlow Condensed font, so `OFL.txt` goes with it, and `THIRD_PARTY_LICENSES.txt` holds the notices of every
third-party part in the image: pass both on with any `.bin` file. See [../docs/flashing.md](../docs/flashing.md) for the steps.
