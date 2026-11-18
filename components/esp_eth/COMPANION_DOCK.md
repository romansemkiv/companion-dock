# esp_eth: ESP-IDF's, with one Companion Dock patch

This is ESP-IDF **v5.5.6**'s `components/esp_eth` (without its `test_apps`), © Espressif Systems, Apache-2.0
([`LICENSE`](LICENSE), ESP-IDF's), kept here so that it overrides the ESP-IDF copy (a project component wins over an
ESP-IDF one of the same name). It is modified: every change is in [`companion-dock.patch`](companion-dock.patch) and
marked `Companion Dock patch` in the source.

## Why

ESP-IDF's EMAC receive task walks every waiting DMA descriptor (one cache sync each) after **each** frame it
takes, then copies the frame into a new allocation. With the receive ring full that is about 100 µs a frame:
the box kept up with some 4000 frames a second. A broadcast flood beyond that filled the 30 receive buffers,
and the EMAC dropped whatever came next, the frames to the box (Companion's) among them, so Companion was
dropped too. The software filter after the copy (`components/netmgr/rxfilter.h`) could not help: the cost was
paid before it ran.

## What the patch does

Only while a receive filter is set (`esp_eth_mac_esp_set_rx_filter`, `components/netmgr/eth.c` sets it);
without one the driver is ESP-IDF's, unchanged:

- **The filter runs on the frame in its DMA buffer** (`emac_esp_dma_rx_filter`), before any allocation or copy.
  The descriptors and the frame's first 64 bytes are read, and a refused frame is given back to the DMA, through
  the internal RAM's **non-cacheable alias** (`CACHE_LL_L2MEM_NON_CACHE_ADDR`): no cache sync at all on that path
  (ESP-IDF's does four a frame). A kept frame goes on through ESP-IDF's own cached path, which invalidates before
  it reads and writes back what it writes.
- **A constant cost a frame:** the receive task drains the ring frame after frame until none is complete, without
  the walk of every waiting descriptor after each one. One receive poll demand per burst restarts a DMA that
  stopped on a full ring.
- **No interrupt per frame under load:** the ISR turns the receive interrupt off when it wakes the task; the task
  turns it on again once the ring is empty (a frame that came meanwhile raises it at once).
- **Fairness:** a ring that never empties gets a tick's pause every 4096 frames, so the tasks below the receive
  task on its core still run.
- **A deeper ring:** `ETH_DMA_RX_BUFFER_NUM` may be up to 128 (30 before); the box uses 64 (22 KB of internal RAM
  more than 20), so a moment's pause of the receive task never overflows it.

Measured on the box (ESP32-P4, 400 MHz) with a broadcast flood of the smallest frames: ESP-IDF's driver kept up with
about 4000 frames a second (about 100 µs a frame, the ring full). Patched, it takes a full 100 Mbit link of them,
**148 700 frames a second, at about 89 % of one core**, losing none but the odd frame while a log line is written.
The box stays reachable and discoverable through such a flood: ARP, DHCP and mDNS still pass the filter (each
within its budget). Only a flood beyond what this keeps up with (the EMAC losing frames for 300 ms on end) is
blocked in the EMAC (`components/netmgr/storm.h`).

## On an ESP-IDF upgrade

Copy the new ESP-IDF `components/esp_eth` here (without `test_apps`), apply `companion-dock.patch`
(`patch -p1 -d components/esp_eth < components/esp_eth/companion-dock.patch`), fix what does not apply, and
check the box under a broadcast flood (the web page's Performance page: *Lost: no buffer*, *Broadcast storms*).
Or drop this directory to go back to ESP-IDF's driver: `eth.c` then needs the receive filter moved back into an
input path (git history: before the in-place filter).
