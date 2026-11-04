# Companion Dock manual

The full reference for the firmware. The [README](../README.md) is the short tour, and
[flashing.md](flashing.md) covers putting the firmware on a new board. On the network and on the decks the
device calls itself **Companion Box** (its default name and hotspot are `CompanionBox-XXXX`), and this manual
calls it "the box".

**Companion Dock** connects USB Elgato Stream Decks to a [Bitfocus Companion](https://bitfocus.io/companion)
server over the network. It is ESP-IDF 5.5 / FreeRTOS firmware for the **Waveshare ESP32-P4-WIFI6-POE-ETH**
(an ESP32-P4 with 32 MB of PSRAM, 16 MB of flash used, IP101 Ethernet, an ESP32-C6 for Wi-Fi 6, and
802.3af PoE). One PoE cable gives the box power and network. Plug Stream Decks into its USB-A port, directly
or through a USB 2.0 high-speed hub, and Companion sees each deck as if it were plugged into a PC running the
official Companion Satellite app. It replaces a Raspberry Pi with a PoE HAT: the box is smaller, starts in a
few seconds, has no SD card that can corrupt, and recovers by itself after power cuts, network outages and
Companion restarts.

- **Two connection modes** (see [Connection modes](#connection-modes)):
  - **Network Dock** (the default for a new or factory-reset box) makes the box look like Elgato Network
    Docks (port 5343, one dock per deck). Companion adds it as a remote surface and drives the decks with
    its own Stream Deck support.
  - **Satellite** speaks Companion's Satellite API. Companion finds the box and adds every deck by
    itself.
- Every current USB Stream Deck model (table in [Stream Deck support](#stream-deck-support)), 2 per box at once, or
  4 unlocked on the web page at your own risk ([Deck limit](#deck-limit))
- Each deck keeps its Companion page and configuration across replugs, hub ports and reboots, because its
  identity is its USB serial (`streamdeck:<serial>`, the same as the official satellite)
- Ethernet (DHCP or static, the preferred route) and a Wi-Fi station for places without a cable; failover
  between Ethernet and Wi-Fi is based on link state only. A second, **backup Wi-Fi network** can be set: the
  box joins it when the main one is out of reach (see *Backup Wi-Fi network* below)
- A **setup hotspot** with a captive portal, on all the time unless you turn it off. It has a random
  password by default, printed on the decks and over serial; it can be made open on the web page
- Zero-touch on a normal LAN in Satellite mode: DHCP and mDNS both ways. The box finds Companion, and
  Companion's *Surfaces → Discover* finds the box. The box connects by itself when it sees exactly one
  Companion, then stays pinned to that one. In Network Dock mode (the default) the docks are announced over
  mDNS and Companion adds each one once
- **A maintenance UI on the decks** while Companion isn't driving them, and on demand (the service button,
  the web page, the API) over Companion's own: the box's state and IP, the hotspot name and password,
  Network, Companion, Hotspot (with join QR codes) and Box screens, pages, holds for what restarts or cuts
  people off, and the settings PIN locking them, unlocked on the deck with a PIN pad
  ([Maintenance screens](#maintenance-screens-on-the-decks));
  "Reconnecting" while Companion is away, and custom messages from the web page or the API
- **Sounds and a status light**: the board's speaker chimes when the box is ready and when Companion
  connects, beeps at a problem, blips when a deck is plugged in or out and clicks on every key press; one
  NeoPixel on a header pin shows the status in colour. "Find my box" beeps and blinks
  ([Speaker and status light](#speaker-and-status-light))
- A web page for status, settings and firmware upload. Its top bar has the deck's
  network signs: Ethernet while linked, Wi-Fi and the hotspot, each with its own address (Wi-Fi with its
  bars, the hotspot with its devices), crossed when off or not joined, the one carrying the route edged in the
  accent, each a link to its page, the details (network, signal, DHCP or static) in its tooltip. It has an optional
  settings PIN (4-8 digits), with token sessions and a lock-out on wrong entries that grows with each lock;
  the same PIN unlocks the decks' locked keys on a PIN pad. Companion's *Discover* can still point a PIN-locked
  box at itself, unless that is turned off
- One JSON-RPC 2.0 API over HTTP and WebSocket, plus the Companion Satellite REST API
- **OTA updates** from the web page into A/B slots. A new image confirms itself after 30 s of healthy
  running; if it crashes or fails that check, the bootloader rolls back. The other slot then gets a working
  copy of the running firmware ([recovery slot](#recovery-slot))
- The ESP32-C6's ESP-Hosted firmware travels in the P4 image and is kept in step automatically
- Recovery without tools: RESET pressed 3 times, an optional service button, a task watchdog, a
  self-healing USB stack, a **core dump in flash** and a **restart breadcrumb** that names the last
  deliberate restart

## Hardware

**Board:** Waveshare ESP32-P4-WIFI6-POE-ETH (SKU 32832). The board on the bench has ESP32-P4 rev v3.2,
32 MB of flash (the partition table uses 16 MB) and 32 MB of PSRAM at 200 MHz. No other hardware is needed.
A case and a service button are optional. The 3D-printable [case](../case/README.md) has a Button variant whose key
is wired as the service button.

| Function | GPIO | Notes |
|---|---|---|
| Ethernet (IP101GRI, RMII) | MDC 31, MDIO 52, REF_CLK 50 (in), TX_EN 49, TXD0 34, TXD1 35, CRS_DV 28, RXD0 29, RXD1 30, PHY reset 51 | fixed on the board; the PHY address is probed |
| ESP32-C6 (Wi-Fi) | SDIO CLK 18, CMD 19, D0–D3 14–17, C6 reset 54 | fixed on the board; ESP-Hosted 3.0.9 over 4-bit SDIO at 40 MHz |
| USB-A (stacked with USB-C) | the P4's USB 2.0 high-speed OTG port | the only native USB host port: decks and hubs go here |
| Console | UART0 (TX 37 / RX 38) | the board's USB-C port (CH343P USB-UART): flashing, logs, core dumps |
| Speaker | ES8311 codec: I2C SDA 7 / SCL 8 (address 0x18), I2S MCLK 13, BCLK 12, WS 10, DOUT 9; amplifier enable 53 | fixed on the board; a speaker goes on the board's speaker connector ([Speaker and status light](#speaker-and-status-light)) |
| **Status light** (optional) | **23** (a setting; 26 before 0.2.1) | one WS2812B-type NeoPixel: 5 V, GND, data in on the pin |
| **Service button** (optional) | **32** | a push button to GND: header pin 25, with GND beside it on pin 26 (check the pinout of your board). Internal pull-up |
| BOOT button | 35 | not usable: the same pin is Ethernet TXD1 |
| RESET button | EN | pressed 3 times in a row: setup hotspot on for 15 minutes ([Recovery](#recovery)) |

**USB-A power.** The port's VBUS goes through a DIO7003 load switch: 2 A continuous and about 2.5 A
current limit, with reverse-current blocking. It is **always on, and its fault flag is not wired**, so the
firmware can neither power-cycle the port nor see an overcurrent. A "USB port reset" in the logs is a
reset of the root port by the USB host controller, not a power cut. A powered hub is allowed; the switch
blocks back-feed.

**PoE power budget (from the ratings; the deck counts tested on a box, see [Measurements](#measurements)).** 802.3af
gives about 12.95 W at the device, roughly 10–11 W at 5 V. Less about 1–1.5 W for the board, plan on
**about 1.5 A (7.5 W) for the hub and decks**. Decks enumerate at up to 500 mA (XL, Original v2) and the
Plus XL is rated 900 mA. So expect 2 decks at worst-case draw. On a box, PoE alone through an unpowered hub ran
2 XL and a Mini, or 3 XL while their pages changed little; more needs a powered hub. For a Wi-Fi-only install
powered from USB-C, use a 5 V supply of at least 3 A. The USB-A switch still caps the decks at about 2 A.

**Full-speed decks and hubs.** The P4's USB host has no Transaction Translator. A full-speed device
behind a high-speed hub does not work, while a full-speed device plugged straight into the box does. The
Stream Deck Mini family and the Pedal may be full-speed (a Mini tested through a hub worked). If one does not
appear on a hub, plug it into the box directly (the web page shows this hint).

## Speaker and status light

**Speaker.** The board has an ES8311 audio codec and an amplifier for a small speaker (a few watts,
4–8 Ω) on its speaker connector; plug one in. The amplifier is switched off 3 s after the last sound, so an
idle box doesn't hiss (the value trades the amplifier's switch-on pop against hiss and is to be tuned on
the board: `SPK_IDLE_US` in `components/feedback/speaker.c`). Its enable (GPIO53) is driven low from the
first line of the start and at every restart. Without a speaker the box works the same, silently. Find my
box and the play buttons on the page also beep while the speaker is turned off in the settings. A codec
that stops answering is closed with the amplifier off, logged once, and tried again with a sound after
10 s, then after twice as long each time up to 10 minutes; the page's play buttons say why.

**Status light.** Wire one WS2812B-type NeoPixel (GRB, such as a single-pixel breakout): **5 V** and
**GND** from the header, its **data input on GPIO23**, header pin 8 (a 330 Ω resistor in the data line helps on
long wires). Another pin can be set on the page: any of GPIO 0–5, 20–23, 26, 27, 33 or 46–48 (GPIO7/8 are the
speaker codec's I2C bus, GPIO32 the service button). Nothing breaks without a light on the pin. Before 0.2.1 the
default was GPIO26, and a box that saved it keeps it until you change it or reset the box.

| Sound | When |
|---|---|
| Ready: a rising two-note chime | the box finished starting (once per start) |
| Connected: one bright chime | Satellite: Companion connected. Network Dock: Companion starts driving a deck |
| Problem: a low double beep | Companion lost after it was connected, a deck stops responding or is quarantined, the network lost. Once when it begins, never again while it lasts; a second problem within 10 s of a beep stays quiet (the network going takes Companion with it) |
| Deck in / deck out: a rising / falling blip | a Stream Deck plugged in / removed |
| Key click: a short, quiet tick | every key press Companion gets (both modes). It is queued after the press went out, so it adds no latency, and it never cuts another sound off |
| Find my box: low-high beeps for 3 s | the *Find my box* button on the *Device* page, or `Device.Identify` |

A new sound replaces the one playing, unless that one ranks higher (Find my box > problem > ready and
connected > deck blips > key click): the deck blips after a USB reset don't cut the problem beep off.

| Light, highest priority first | Means |
|---|---|
| White, fast blink (30 s) | Find my box |
| Purple, fast blink | a firmware update: an upload being written, or the Wi-Fi chip's (C6) update at start-up |
| Red and purple, alternating | a deck problem: a deck is quarantined, or not responding or not taking images (until 10 s after its last failure), and 10 s after a USB reset that cured one |
| Red, slow blink | no network (no Ethernet or Wi-Fi address) |
| Blue, slow blink | the setup hotspot is held on (RESET ×3, the service button). The hotspot that is simply on by default doesn't count |
| Amber, slow blink | network up, Companion not connected (Network Dock: Companion drives no deck) |
| Green, dim | connected, all good |
| White, dim | starting up (also while the network comes up, 15 s at most) |

Slow is 500 ms on, 500 ms off; fast is 125 ms. Every sound event also flashes the light over the status
colour, which then returns: ready white ×2, connected green ×2, problem red/purple ×4, deck in cyan ×1,
deck out cyan ×2, key click white 60 ms (a click during a flash restarts it). It happens with the sound
muted too, only while the light is on and *Flash with each event* (`ledFlash`, on) is on; Find my box and a
firmware update are never interrupted. Settings, on the *Speaker & LED* page or with
`Feedback.SetConfig`, apply at once: the speaker on or off, the volume (50 % on a new box), each sound on
or off (all on), the light on or off (on), its pin (26) and brightness (30 %) and the flashes (on). The page's play buttons
sound at the slider's volume and flash the light.

## Backup Wi-Fi network

Settings: the web page's *Wi-Fi* page has three cards: **Wi-Fi**, the state (Connected, Connecting,
Not found, Off, Not set up or Radio down), the network joined (its name, *Primary* or *Backup*), the box's Wi-Fi
address and signal, the Wi-Fi chip's firmware and *Turn Wi-Fi on / off*; **Primary network**, the main pair's name,
*Scan for networks*, password and Save (*In use*, *Standby* or *Not set*); and **Backup network**, with its own
name, password and *Scan for networks* button (picking a result fills the backup name and focuses its password), or
`Wifi.SetConfig {"wifiBackupSsid", "wifiBackupPass"}`. The same rules as the main pair: a name of up to 32
bytes, a password empty or 8-63 characters, a missing password keeps the saved one, passwords are never
returned (`wifiBackupHasPass` says whether one is stored). The backup is only used while the main network has a name (with an empty main name the station is off). An empty backup name means no backup: the
box then uses the main network only, with a reconnect every 5 s. The page's Wi-Fi card and
`Wifi.GetStatus` say which network is in use (*Primary*, *Backup* or *None*).

Behaviour:
1. The box joins the main network.
2. If it cannot join it within **30 s** (not found, wrong password, associated but no IP address, or lost and not back), it tries the backup,
   and keeps alternating every 30 s while neither works. (Between the switches the usual 5 s reconnects go on.)
3. **On the backup it stays** for as long as it works: no switching back that would drop a show. When the
   backup drops, the box tries the main network first.
3a. **By hand**: the deck's Network screen (*Use*, hold 1.5 s), the page's *Use this network*, or
   `Wifi.Use {net}` makes the box join that network now (Wi-Fi drops for a moment). It is then the one in
   use: kept while it works, tried first after a loss (then the other). A change to either pair starts again
   from the main network; a hotspot-only restart keeps the one chosen.
4. Saving a change to either pair starts again from the main network. A change to the hotspot alone (switched
   on or off on the page or with `Ap.SetConfig`, its password, the 15-minute hotspot of RESET ×3, the
   service button and its end, the deck's *Always on* and *Turn off*) restarts Wi-Fi but keeps the
   network the station was on: on the backup it stays on the backup. The station's link still
   drops for the few seconds the restart takes.
5. Ethernet still takes the route whenever it has link.
6. A Wi-Fi scan and a switch never run at the same time: a switch that falls due during a scan waits for its
   end. As for any scan, **it briefly pauses the hotspot's clients, and the station's link may drop** for a
   moment.

The decision lives in `components/netmgr/wififo_logic.c` (pure, host-tested); `wifi.c` applies it.

**Wi-Fi off**: the *Wi-Fi* page's *Use Wi-Fi (the setup hotspot stays)* switch, the deck's *Wi-Fi*
toggle on the Network screen, or `Wifi.SetConfig {"wifiEnabled": false}` turns the station (the main and the
backup network) off: it disconnects and stays down, with no reconnect, no failover and no signal sampling. The
networks' names and passwords are kept (the page shows them dimmed, still editable), and *Scan for networks*
still works (the station comes up just for the scan). **The setup hotspot is untouched**: it keeps its own
settings, and RESET ×3 or the service button still force it on. Ethernet isn't affected; with Ethernet down
too, the decks show "No network" with the hotspot to join. Turned on again, the station starts from the main
network. `wifiEnabled` defaults to on (also for a config saved without it); `Wifi.GetStatus.enabled` and
`Sys.GetStatus.net.wifi_enabled` report it.

## Connection modes

The setting is on the web page's *Companion* page, or `Companion.SetConfig {"mode": "satellite" | "dock"}`.
Switching saves the mode and restarts the box. Over Wi-Fi the API answers again about 12 s later (the start
itself takes 4.5 s; the rest is Wi-Fi association).

| | **Satellite** | **Network Dock** (default) |
|---|---|---|
| Who connects | the box connects to Companion (TCP 16622, configurable) | Companion connects to the box (TCP 5343 + 2k) |
| Setup in Companion | none: decks appear by themselves (or *Surfaces → Discover*) | *Surfaces → Remote → Add Remote Surface* once per deck (or the discovered list) |
| Who draws the keys | Companion sends bitmaps; the box encodes JPEG/BMP for each model | Companion's own Stream Deck driver; the box passes HID reports through unchanged |
| Box-drawn screens | the maintenance screens, dimmed keys + "Reconnecting", custom messages | the maintenance screens, custom messages (the last images come back after) |
| Pick it when | the normal case: zero configuration, one TCP connection for all decks | you want Companion's own Stream Deck driver to draw every key, strip and info bar exactly as for a deck on the Companion machine, or the network lets Companion reach the box but not the box reach Companion |

**Satellite mode.** Companion 5.0 or newer is needed (API ≥ 1.11). With exactly one Companion visible
over mDNS (`_companion-satellite-tcp._tcp`) the box connects to it and stays pinned to that instance's
`id`. It follows that instance if its IP changes and never switches to another one by itself. With
several, the decks say "Choose Companion" with "Web page" and the box IP, and the web page lists them. You can also
set a host and port by hand. Companion's *Surfaces → Discover* lists the box (`_companion-satellite._tcp`
on port 80, TXT `restEnabled=true`, instance name = box name), and its *setup* button configures the box
through `POST /api/config`. With a settings PIN set, it still can while *Let Companion adopt this box without the
PIN* is on (the default, Device page, `companionAdopt`): then only the Companion address and port it posts apply.
Turned off, set the address on the box's page.
When the address it posts belongs to a Companion the box sees over mDNS, the box pins that instance, so it
follows it to a new IP like an automatic choice. Any other address is kept as a manual one.

**Network Dock mode.** Each deck gets its own dock, and the dock's port presents the deck itself: Companion
sees a Stream Deck 32 Module (or whatever the deck is) at that address, never a Network Dock, so its
*Surfaces* tab lists **one surface per deck**. Docks are counted from 1, as on the web page:
dock *n* listens on **5341 + 2n** while a deck is assigned to it. The box runs one dock per deck it
takes ([Deck limit](#deck-limit)): 2, or 4 unlocked:

| Dock | Add in Companion as |
|---|---|
| 1 (first deck) | `<box IP>` port **5343** |
| 2 | `<box IP>` port **5345** |
| 3 (deck limit 4) | port 5347 |
| 4 (deck limit 4) | port 5349 |

The first deck takes dock 1, so a one-deck box is reached at the default port. A deck keeps its dock across
reboots and replugs: the serial → dock map is saved, and a new deck takes the lowest free dock no other deck
has reserved (a reserved one only when all others are taken).

- **Discovery.** Each listening dock is announced over mDNS as `_elg._tcp` with TXT `dt=215` (Companion's
  discovery lists only Network Docks and Studios, so the box keeps that type) and `sn=<deck serial>`.
  The name is `<model> @ <box name>`, e.g. `Module 32 @ CompanionBox-8D1C`; a second deck of the same model
  makes both say their dock, `XL (dock 1) @ …` and `XL (dock 3) @ …`. A dock without a deck isn't announced. Companion's
  discovered list groups the box's docks by address and shows the first one's name, with the description
  "Stream Deck Network Dock".
- **Adding it.** In Companion 5: *Surfaces → Remote → Add Remote Surface* (or the discovered list), module
  *Elgato Stream Deck*, address = the box's IP, port = the dock's port. Companion keys the surface to the
  deck's own serial and drives it as a USB deck: images, keys, encoders, LCD strip, brightness. The decks'
  screens say what to enter: "Waiting for Companion", then "Remote surface, <box IP> port <dock port>".
- **A dock without a deck doesn't listen and isn't announced**, the first one included. With no deck
  plugged in, nothing is open and the web page says to plug one in. When a deck is
  unplugged its dock closes Companion's connection, stops listening and withdraws its mDNS service:
  Companion shows the deck's surface offline and keeps retrying every 5 s. When the deck is plugged back
  in it gets the same dock (the map), the port opens and is announced again, and Companion reconnects by
  itself, about 5 s later at most. No empty "Network Dock" is ever presented, so none appears in
  Companion.
- **One client per port.** Any TCP connection to a deck's port takes the deck over from Companion: a
  port check (`nc -z <box IP> 5343`) or a tool such as `dock_check` included. Companion is off the deck
  until it reconnects, about 5 s later.
- In this mode the Satellite client, Companion browsing and the `_companion-satellite._tcp` announcement
  are off. The deck reports its own serial and firmware version; Companion offers Elgato's firmware updater
  only for Network Docks and Studios, so never for a deck behind the box.

**`tools/dock_check`** checks dock mode with the library Companion itself uses
(`@elgato-stream-deck/tcp` 7.8.0, a development tool only, not in the firmware):

```sh
cd tools/dock_check && npm ci            # Node >= 22.18
node tools/dock_check/dock_check.mjs <box ip> [port] [--seconds N] [--hold] [--blank] [--repaint S]
```

It connects as Companion does and expects the deck itself on the port: its model, VID/PID, serial and
firmware, no child device and no Network Dock (a port with no deck
behind it isn't open, which fails as "no deck within 20 s"). It sets the brightness, fills every key with a distinct colour and times a report queued behind
those images (the page time). Then it clears the panel and logs key and encoder events for N seconds. It
exits 0 only when the deck appeared directly and every write resolved. One client per port: it takes the
deck over from Companion, which takes it back about 5 s later, so with Companion connected use
`--seconds 0`.

## Deck limit

The box takes **2 Stream Decks** at once: it powers its USB port from Power over Ethernet, which is sized for 2.
Every Elgato deck counts, the Pedal too. On the web page's **Device** page, *Allow up to 4 decks…*
raises the limit to **4** after a warning ("Allow up to 4 Stream Decks?": with more, the decks can drop out or the
box can restart; a powered USB hub gives the extra decks their own power; at your own risk), and restarts the box;
*Back to 2 decks…* undoes it. The setting is `deckLimit` (`Device.SetConfig`, 2 or 4), behind the settings PIN.

The box has one USB-A port, so two or more decks go through a USB 2.0 high-speed hub. How many decks the power
allows (the PoE row tested on a box, the others estimates from the ratings; see [the PoE power budget](#hardware) and
[Measurements](#measurements)):

| The decks are powered by | Budget for the hub and the decks | Decks |
|---|---|---|
| PoE (802.3af), through an unpowered hub | about 1.5 A (7.5 W), what is left once the board has its share | **Tested: 2 XL and a Mini**, or **3 XL** while their pages change little (the limit at 4) |
| USB-C, a 5 V supply of 3 A or more (Wi-Fi only), through an unpowered hub | about 2 A, the USB-A switch's limit | **3** at full draw, 4 often |
| A powered hub (with its own power supply), the box on PoE or USB-C | the hub's supply | **4**, the firmware's maximum |

*Full draw* is 500 mA per deck, the most a deck asks for when it enumerates (XL, Original v2); a Stream Deck + XL
is rated 900 mA and counts as almost two. An unpowered hub takes its own share from the same budget. Images drawn on
the keys take power, so 3 XL with busy pages may be more than PoE gives. A full-speed deck (some older Minis, the
Pedal) doesn't work behind a high-speed hub, so it only works plugged into the box directly, on its own.

A deck beyond the limit is ignored as long as it stays plugged in: it gets no dock and no screen, Companion never
sees it, the problem list says "Too many Stream Decks (max 2): <serial> ignored", and Home's banner says "Deck
limit reached (2). Unplug one, or allow up to 4 on Device." When a deck has been unplugged since, so there is room
again, the ignored one stays ignored until it is plugged in again: the banner then says "<model> is over the limit:
unplug it and plug it back in.". In Network Dock mode the box runs one dock per deck it
takes (2 or 4); in Satellite mode it adds at most that many decks to Companion.

## Maintenance screens on the decks

Every deck with a screen that Companion isn't driving shows the box's own maintenance screens.
Each key is one tile:
buttons look pressable (outline, icon, label), information is flat; status on top, the menu at the bottom,
Back at the top-left. The icons are [Phosphor](https://phosphoricons.com)'s, Regular weight, drawn from Phosphor's own
outlines: wifi-high / -medium / -low for the signal, wifi-slash, network (Ethernet), globe (Network),
hard-drive (Dock), broadcast (Hotspot), power (Restart), arrow-clockwise (Retry), arrows-clockwise (Sync),
toggle-left / -right, magnifying-glass (Auto find), webhooks-logo (Companion), and so on; Satellite mode is Lucide's
satellite (Phosphor has none), drawn in Phosphor's stroke.

- **Home**, by state: no network (*1 Join*: the hotspot's name and password, then its Wi-Fi join QR code; *2 Open*:
  the setup page's address, then its QR code; the Mini shows the join code on the page after its words),
  finding Companion, choose
  Companion (one button per Companion found: a tap pins it), Companion too old, reconnecting, waiting for
  Companion (dock mode: the box's IP and the deck's port to add in Companion), no free dock, and
  "Companion connected" (on demand only: a grey *Connected Companion* label, then the Companion's *IP*, and
  in dock mode its *PORT*; where the label can't share a page with the address, as on the Mini in
  dock mode, there is no label and the address's first key says *COMPANION IP*). The hotspot's name (captioned *HOTSPOT*) and password
  sit at the bottom while it is on.
- **Network**: Ethernet, the primary and backup Wi-Fi, the box's `.local` name, on a **fixed key map per model**:
  every key stays where it is whatever the state, so it can be pressed from memory. On the 32-key
  decks and the + XL one row per network, Ethernet / Primary / Backup in that order (the deck says
  *Primary*, as the web page does; the PIN pad "PIN to use Primary"): the icon, the name (two keys:
  the Wi-Fi name's words), the state, the row's action (Use DHCP / Make static, Use), Edit for Ethernet or the
  signal for the Wi-Fi network in use, the address on two keys; the **Wi-Fi** key in the status row (the + XL:
  top right). On the 15-key decks a page per network, the same order over two rows, the Wi-Fi key on Primary's
  page; on the + and the Neo a page per network (the signal stays in the icon's bars); on the Mini a page per
  network (icon, name, state, action) and then its details (Edit or the Wi-Fi key, the signal, the address). A
  Wi-Fi name that can't read at 16 px on its keys falls back to the flowing layout. The Ethernet row
  shows its link, *ADDRESS* *DHCP* or *Static*, and its IP while it carries the route; then **Use DHCP** (when
  static) or **Make static** (when DHCP), each a 1.5 s hold (let go early: "Hold 1.5 s" / "net resets", locked under the settings
  PIN). *Make static* keeps the saved static address when it is a valid set, else turns the current DHCP lease
  (address, mask, gateway, DNS) into the static set; a small tag on its button says which (*saved* or *lease*: an
  old saved set may be on another subnet); with neither (no cable, no lease, nothing saved) the
  button is disabled and says *Set on web page*. Both go through `Eth.SetConfig` (its checks, its apply) and
  end with a notice ("Static 192.168.8.50", "DHCP"; a notice takes the status row, strip or info bar
  over for about 2 s on every deck, or the Back key where there is no status row: green done, amber under way, red
  refused with a two-word reason such as "Upload running" or "PIN needed"). **Edit** (locked under the PIN too: the PIN pad first)
  opens the **static-address editor**: the IP address, the Mask, the Gateway and the DNS, one at a
  time, prefilled (the static set if valid, else the lease, else empty). An address goes in octet by octet
  with no dot key, the current octet outlined in cyan: the first digit typed replaces what was
  there; it moves on only after 3 digits, or when the next digit would pass 255 (that digit starts the next
  octet); **Next** goes to the next octet, then the next field (on an empty Gateway or DNS it skips it), but
  right after an automatic move, with nothing typed yet, Next only confirms it ("172 Next 16 Next 100 Next 5"
  gives 172.16.100.5) and ⌫ goes back and deletes the digit just typed; ⌫ deletes, back over octets and
  fields. The Mask is a prefix with − / + ("/24", shown as 255.255.255.0, /1 to /30). The deck checks it: the
  IP a host's (not 0.x, 127.x or 224 and up) and not its subnet's network or broadcast address, the Gateway in
  the subnet (not the IP, the network or the broadcast address) or empty, the DNS complete or empty; an error shows in the status ("Gateway not in subnet") and keeps the field. A summary then shows
  the four and **Save** (hold 1.5 s, "net resets"; static, through `Eth.SetConfig`); ✕ (or ⌫ held 1 s)
  cancels and changes nothing, and it cancels itself after 60 s without a press. Layouts: Module 32 / XL the
  octets on 4 keys of the top row and a phone pad (⌫ / 0 / Next; on the Mask, − / the prefix / +); + XL the same with the value on the strip;
  MK.2 / 15 the address on 2 keys (two octets each), digits 1-5 and 6-0; + and Neo the value on the strip or
  info bar and the digits on two pages (on the + a dial turn changes the current octet and a push is Next);
  Mini / Module 6 a roller per octet (− / the octet / +, held − / + repeat and speed up), Next, ⌫, ✕. Each Wi-Fi row says its state:
  the network joined *In use* (green), with its signal and its own IP whenever it has one, also
  while Ethernet carries the box's route (each row shows its own address, Ethernet's while linked with
  one, in the row's own keys of the key map, so both show at once); the other *Standby*. While neither is joined:
  *Joining* for the one being tried, *Not found* only when the radio reported no such network on its last
  attempt there, *Standby* for the one whose turn hasn't come, and *Radio down* on both when the Wi-Fi
  co-processor isn't answering; a backup that isn't set up says *Not set*; both say *Off* (grey) while Wi-Fi is
  turned off. The **Wi-Fi** toggle (a power switch, outlined while on, in its fixed key):
  turning it off is a 1.5 s hold ("Wi-Fi off"), on a tap; both locked under the settings PIN; the setup
  hotspot stays either way. The row not in use has **Use**
  (hold 1.5 s, "Wi-Fi blips"; locked under the settings PIN, which the PIN pad unlocks): the box joins
  that network now and keeps it as the one in use, as `Wifi.Use` does. **Companion**: the mode (Satellite
  with the satellite icon, Dock; hold the other one 1.5 s to switch; the box restarts), the port (dock mode) or the target (captioned
  *COMPANION*) with
  *Retry now* (while not connected) and *Choose*. *Choose* is there only with automatic discovery on: it
  opens the list of Companions found on the deck, and opening it changes nothing; a tap on one pins it, as
  *Choose* on the web page does. With a manual Companion address there is no *Choose*: the
  deck never rewrites the Companion settings by itself; *Auto find* is there instead, a tap that turns automatic
  discovery back on (as `Companion.SetConfig {companionAuto: true}`, "Auto find on"). **Set IP**
  (Satellite mode) sets the Companion's address from the deck: the same editor as Ethernet's static
  address with one field, "Companion IP" (prefilled with the Companion it connects to, or the saved address,
  when either is an IPv4 address), checked as a host's address (not 0.x, 127.x, 224.x and up), then a summary
  with the IP, the port it will use (the one set on the web page, 16622 by default) and **Save** (hold 1.5 s,
  "reconnects"), Back and Cancel. Save does what the web page's manual address does
  (`Companion.SetConfig {companionAuto: false, companionHost}`): automatic discovery off, the box connects to
  that address, and the deck says "Saved". *Set IP* and *Auto find* are locked under the settings PIN, and
  neither is shown in dock mode. **Hotspot**: name (captioned *HOTSPOT*) and
  password, *Turn off* (hold 1.5 s) or *Always on* (the hotspot setting on and kept, as the web page
  turns it on; locked under the settings PIN), *Show QR* (a code to join the hotspot and one for
  the setup page). **Box** (Phosphor's *Circuitry*): name, uptime, version, both firmware slots,
  the **Sync** button, always there: *Sync now* while the other slot differs, is empty or invalid;
  in sync its tag says *In sync* and a tap copies the running firmware again all the same; disabled with the
  reason in its tag while a copy runs (its percentage), over a newer image (*newer*: a forced copy stays on the
  web page) and while the firmware is pending verify or a copy waits behind an upload (*wait*); then *Boot
  other* (hold 2 s) over a newer image, *Dimmer* / *Brighter* (`deckBrightness`, default 70 %: the brightness of a deck
  while it shows the maintenance screens, the box's own or on demand over Companion, applied at once; closing
  on demand gives Companion's last brightness back), *Sound on/off* (`speakerEnabled`), *Flashes*
  (`ledFlash`, the status light's flashes: outlined while on, one word so it reads on one line on
  every key), *Restart* (hold 2 s).
- **The Companion icon**: the Companion button shows Phosphor's webhooks logo, in the key's ink like every icon
  (white, cyan when selected, dimmed when locked). No Bitfocus artwork is used.
- **Status icons**: the network and hotspot icons in the status row are buttons: a tap opens the
  Network or Hotspot screen (a click, brighter while held). On the + and + XL strip a tap on the icon does the
  same; the Neo's info bar isn't touch.
- **Signs**, right-aligned in a fixed order: the **hotspot**, always there (off: crossed, in grey: off
  by choice is normal); **Wi-Fi**, its bars when joined, crossed in grey when turned off (or no network is set up
  while another connection carries the route), crossed with a red slash when it is on but not joined, or when the box
  has no route at all (a problem); **Ethernet** while its cable is linked (both
  show when both are up). With no room for all, the hotspot goes first and the sign carrying the route stays the
  longest; a deck with room for one shows that one. The strip shows all three, the Neo's info bar two.
- **Readability**: on the small decks (15 keys or fewer: MK.2, Module 15, the Original, +, Neo,
  Mini, Module 6) no word, value or address is split over keys: a name, password or version stays on one key,
  on 2 lines at its natural break ("Companion" / "Box-8D1C"). A Wi-Fi name (the main, the backup, the hotspot's)
  is one word a key on every deck, its words separated by spaces only: "Studio
  WiFi" takes 2 keys, "My Wifi 5G" 3, "CompanionBox-8D1C" 1 ("Companion" / "Box-8D1C" on 2 lines inside it). The
  hotspot's password is one key on every deck too: on 1 line, or on 2 inside the key on the big decks ("k7m2" /
  "p9xq"). A word is never split over keys, unless it can't read at 16 px on 2 lines (the last resort); a word with no
  natural break wraps inside its key with nothing added (no '-' is ever added to a value: an SSID, a password,
  an address or a version); passwords stay whole. An IP address takes one key when it
  fits there on one line at 16 px, else two keys side by side, two octets each, one line each at one size:
  "192.168." | "8.225", the caption on the first, its port on the next key. Only a value too long even so (a
  32-character Wi-Fi name) still goes over keys. The 32-key decks and the + XL keep one octet per key. No word
  is split over keys; a word with no natural break that needs 2 lines on its key breaks at a syllable-like
  point with a hyphen ("Recon-" / "necting", never "Reconn" / "ecting"); a value is never given a hyphen. Text
  is never smaller than 16 px for values and words, 12 px for captions and 14 px for button labels, on every
  model, and nothing is drawn past a key's margin or a button's outline (a step's small sub-line is left out
  where it doesn't fit): what doesn't fit moves to the next page. These rules are the maintenance screens';
  the custom message (`Deck.ShowMessage`) and the "Reconnecting" overlay keep their own look.
- **Models**: every one with a screen. The status is on the top key row; on the + and + XL on the touch
  strip, on the Neo on its info bar. The menu is the bottom key row where there is room, else one *Menu*
  key (+, Neo; the Mini's in its status row) that opens a Menu screen. While waiting (Finding, Reconnecting,
  Waiting for Companion), the status glyph's dots pulse: at 5 fps on a key, at 2 fps on the strip and the
  info bar, which are drawn and sent whole each time.
- **Navigation**: Back on the top-left key; a "›" key turns pages (the menu row's right end on the big
  decks); dials turn pages and pushing one is Back (+, + XL); the Neo's touch keys turn pages (lit when
  there are pages); a sideways swipe on the strip turns them; the Mini's home turns by tapping any key
  that isn't a button. Home turns its pages by itself every 3 s (2 s with more than 3 pages) until a press,
  and again after 60 s without one; 60 s without a press goes home.
- **Holds**: what restarts the box or cuts people off needs a hold (the key fills from the bottom; let go
  early, it says what the hold is for). Every press clicks; a finished hold chimes.
- **Settings PIN**: with one set, the mode switch, *Turn off*, *Restart*, *Sync* (while it does
  something), *Boot other*, the hotspot's *Always on*, the Companion screen's *Set IP* and *Auto find*
  show a lock. Paging, the information, the QR codes, *Retry*, *Choose*,
  brightness and sound still work: the deck counts as physical access. **Pressing a locked key opens a PIN
  pad**: the digits show as dots only, with ⌫, OK and ✕ (Cancel). The right PIN unlocks that deck's
  locked keys (a confirm sound, "Unlocked"); the screen you came from comes back, and the key you pressed
  has not fired: press or hold it again as usual (the holds still apply). The unlock belongs to that deck
  and ends when the maintenance screen closes, after 60 s without a press (home), or after 5 minutes. A
  wrong PIN turns the status red ("Wrong PIN", the low tone); after 5 wrong in a row the deck is locked out
  for 60 s, then 120 s, 240 s… up to 1 h, counting down on the pad ("Locked · wait N s"); the right PIN
  resets it. The check runs off the deck's tasks ("Checking" meanwhile). If another deck's check is
  running, or none comes back within 15 s, the pad says "Busy, try again" in amber and keeps the digits
  typed. The pad cancels itself after 30 s without a press. Layouts: Module 32 / XL a phone pad with the dots in the status row; + XL the same with
  the dots on the strip; MK.2 / 15 ✕ ⌫ dots OK on top, 1-5 and 6-0 under them; + and Neo the digits on two
  pages (the page key, the dials or the touch keys turn them; hold ⌫ to cancel, which ⌫ shows with "HOLD" and
  a ✕) with the dots on the strip or info bar; Mini / Module 6 a digit roller (− / the digit, the title over it
  and the dots under it / +, then ⌫, *Next digit*, OK; hold ⌫ to cancel, shown the same), because 10 digits, ⌫,
  OK and a page key would need three pages of 6 keys. The pad says what it unlocks: "PIN to
  restart", "PIN: Wi-Fi off" (on the status row, over the dots, on the strip or info bar, over the roller's
  digit, shortened where a key is narrow). After the right PIN the key that opened it glows cyan for 3 s on the
  screen it came from, and the notice says what to do: "Unlocked · hold Restart", "Unlocked · press Sync now". A box whose secret
  is still a text password from an older firmware (no PIN) says "Set a PIN on the web page" instead.
- **On demand**, **a box menu per deck**: every deck with a screen has its own, and several
  can be open at once. It opens on a deck through the secret gesture made on that deck (below), that deck's
  *Show box menu* button in the web page's Home table, or `Deck.ShowMaintenance {"on": true, "serial": "…"}`; it
  closes on that deck by its *Close* key (✕, top-left of home), its *Close menu* button on the page,
  `{"on": false, "serial": "…"}`, 60 s without a press on it, or unplugging the deck. A deck plugged in, or
  plugged back in, always arrives closed. What names no deck: **a short press of the service
  button** (50 ms to 2 s) closes every open box menu, or, with none open, opens it on the biggest attached deck
  with a screen: the most keys, then the larger keys, then one with a touch strip or info bar, then
  the one plugged in first. So a + XL (36 keys) beats a Module 32 or XL (32), which beats an MK.2 (15), then a +
  (8, bigger keys than the Neo), the Neo, and the Mini (6); the Pedal never counts. `Deck.ShowMaintenance {"on":
  true}` without a serial opens on that same deck, `{"on": false}` closes them all (the page's banner, *Close
  all*). With no deck with a screen attached it doesn't open (the RPC says so). Every other deck stays as it
  was: one Companion drives keeps its images and presses, one the box drives keeps its own screen. On a deck
  the box menu is open on, if Companion drives it, no key press reaches Companion (keys it saw pressed are
  released first), Companion stays connected and its images keep updating the box's cache. On close,
  Companion's images come back from the cache. Until they have been written to the deck (not merely queued for
  it), its presses go nowhere, and a key held down through it reaches Companion only once it has been let go: a
  press meant for the maintenance screen doesn't fire a Companion button. A fast deck hands back as soon as its
  images are written; the wait is capped at 3 s, enough for the slow full-speed BMP decks (the
  original Stream Deck, the Mini) to rewrite every key. Should the deck still be writing at 3 s, its input goes
  back to Companion anyway. The hand-back belongs to its deck: opening or closing the box menu on another deck
  never cuts it short.
- **Secret gesture** (you can change it, below): by default **tap the top-right key**, then within
  **3 s** press the **top-right and bottom-right keys** together and hold both for **2 s**, while Companion drives
  the deck (either mode): the box menu opens on **that** deck (unless it is open there already; the service
  button and the RPC without a serial pick the biggest), with the click and the light's flash. The keys are
  the right column's top and bottom (the original Stream Deck's mirrored rows are taken care of): Module 32 / XL
  keys 8 and 32, + XL 9 and 36, MK.2 / 15 5 and 15, + / Neo 4 and 8, Mini 3 and 6; never the Pedal. **The first
  tap reaches Companion as a normal press** (it can't be told apart yet). **Only in the 3 s after that tap** is a
  key-down on either of the two keys held back from Companion for **120 ms**; if the other one goes down within
  that time, neither is sent. A lone press of one of them in those 3 s reaches Companion when the 120 ms are
  over (a release before then sends the press and the release together); outside those 3 s no key is ever
  delayed. **What starts the 3 s** is any top-right press that reached Companion, when its key comes up: a long
  hold too, and each such press inside the window starts it again. So a top-right key pressed again and again
  less than 3 s apart is always about 0.1 s late, and so is a bottom-right press within 3 s of a top-right one;
  a chord's own releases never start it. A chord let go before 2 s sends nothing and opens nothing; held 2 s it opens, and the keys' releases
  go to nobody. Every other key is never delayed (dock mode rewrites the deck's input reports with only the
  held-back bits cleared), and a consumed press never clicks. So **"tap top-right, then top-right + bottom-right"
  is reserved**. The gesture does nothing while the screen is open (close it as usual), or on a deck the box
  already shows its screen on; the PIN doesn't block opening (it only unlocks the locked actions).
- **Changing the gesture**, for a Companion page that uses those keys: Device → **Box menu gesture** on the web
  page (behind the settings PIN, like every setting; one setting for every deck, both modes, applied at once, no
  restart). **Open the box menu from the keys** turns it off: then no key is ever held back and the box menu opens
  only from Home's *Show box menu* or the service button. **Tap** picks the key tapped first, any of the four
  corners (they exist on every deck); **Then hold it with** the corner held with it (a different one); **Taps** 1,
  2 or 3 (each tap within 3 s of the one before, the other key not pressed between them; more taps make a match by
  accident less likely); **For** the hold, 2, 3 or 5 s. Everything above holds for the keys chosen: the taps reach
  Companion, only in the 3 s after the last tap are the two keys held back 0.1 s, the chord never reaches
  Companion. The default is the gesture above, so a box that never touches it works as before. The log says
  `box menu gesture: tap top-right, then hold top-right + bottom-right 2 s` (or `off`) at start and on each change.
- **When Companion lets go** of a deck in Network Dock mode, the deck shows the maintenance home ("Waiting
  for Companion", the box's IP and the deck's port) at once. In Satellite mode the last images still dim with "Reconnecting" over
  them for 30 s first.

## Build and flash

To put the prebuilt firmware on a board without building it, see [flashing.md](flashing.md).

```sh
source tools/idf-env.sh         # ESP-IDF 5.5.6 (edit it for your install, or source your own export.sh)
./build.sh                      # development build (build/)
./build.sh -p /dev/ttyUSB0      # build, flash (bootloader, partition table, otadata, app), monitor
./build.sh --release            # release build (build-release/), version exactly version.txt
./build.sh --test               # host unit tests and the web page check (tools/test-host.sh)
```

`build.sh` first builds the C6 firmware that the P4 image carries (`tools/build_c6.sh`, into `build-c6/`)
when it is missing or of another ESP-Hosted version than `dependencies.lock`. This needs the component
registry once. Then it configures CMake again and builds, so the version always names the checked-out
commit (plain `idf.py build` also configures again when HEAD moves: it watches HEAD's reflog, so a packed
branch ref or a git worktree work too; it does not see an uncommitted edit). A plain
`idf.py build` also works once `build-c6/` exists. Without it the image carries no C6 firmware and says so
(CMake warning).

**Version.** `version.txt` holds the next release. Development builds report
`<version>-dev+g<commit>[.dirty]` (e.g. `0.1.0-dev+g1a2b3c4`), which sorts before the release it leads to.
`./build.sh --release` (or `CBX_VERSION=x.y.z` in the environment, or `-DCBX_VERSION=x.y.z`) builds
exactly that version. A release refuses uncommitted changes, and its `sdkconfig` is made afresh from
`sdkconfig.defaults`, so nothing set by hand ships. The version is at most 31 characters (ESP-IDF's
limit; CMake stops otherwise). `Device.GetInfo.version` and the *Firmware* page show it.

**Build settings** (`sdkconfig.defaults`): -O2,
FreeRTOS at 1000 Hz, PSRAM for every allocation over 1 KB (task stacks may go there too),
stack-smashing protection, ELF core dumps to flash, the task watchdog (10 s, it restarts the box),
USB host hubs (multi-level) with a FIFO split (set in `usb_host_task.c`) that fits the decks'
1024-byte interrupt-OUT and 512-byte interrupt-IN packets, WebSocket support, 40 lwIP sockets for the docks, and ESP-Hosted over SDIO to the C6.

**First flash of a board.** `./build.sh -p PORT` writes everything. To give the box its Wi-Fi network
before it ever starts (no hotspot step), seed an NVS image and flash it after the firmware:

```sh
python tools/seed_config.py wifi.json nvs.bin           # {"ssid": "...", "pass": "..."} or config keys
python -m esptool --chip esp32p4 -p PORT write_flash 0x9000 nvs.bin
```

The tool never prints a value. Keep the JSON out of the repository.

**Chip revision.** This tree builds for ESP32-P4 v3.0 and later (`CONFIG_ESP32P4_REV_MIN_300`). esptool
prints the chip's revision at every flash (`Chip is ESP32-P4 (revision v3.2)`). A board with a v1.x chip
would need `CONFIG_ESP32P4_SELECTS_REV_LESS_V3`, which this tree does not set up.

**The C6's first flash, by hand.** The P4 image keeps the C6 on the ESP-Hosted version it carries (3.0.9)
over the SDIO link. An ESP-Hosted 3.x host cannot talk to a C6 on an older ESP-Hosted at all. If the C6
does not run 3.x (the web page's *Firmware* page shows it as *WiFi chip*; "not answering" means no answer),
flash it once over its programming header (H7 on the schematic: TX, RX, GND; hold the C6's BOOT, GPIO9, to
GND while it resets) with a USB-UART adapter:

```sh
tools/build_c6.sh
idf.py -C build-c6/cp -p /dev/ttyUSB0 flash
```

From then on the P4 keeps it in step, at most twice per power-on, and never while a new P4 image is
waiting for its confirmation. Any firmware built against the same ESP-Hosted version and example carries
the same C6 image, so switching the board between such firmwares never reflashes the C6.

**Updates over USB after OTA.** After an OTA update or a [sync](#recovery-slot) both slots hold a firmware,
and the box may boot from `ota_1` (the boot log and the *Firmware* page name the slot). `idf.py app-flash`
writes `ota_0` only. On a box running `ota_1` the old firmware keeps running after the flash. The new image in
`ota_0` counts as newer when its build stamp (the date and time in its app descriptor) is the same as the
running one's or later, which a newer build always has (a rebuild of the same commit keeps the stamp, and the
same stamp counts as newer), so the sync never overwrites it: the *Firmware* page shows *Other slot
newer*. Start it with **Boot other slot** (`Ota.BootOther`), or flash with `idf.py -p PORT flash`, which also
resets otadata so the box starts `ota_0`. Two cases where the sync **does** overwrite an app-flashed image 10
minutes after the start:
- its stamp is *earlier* than the running one's (an older checkout, or a build machine whose clock or time zone
  is behind: the stamp is the build machine's local time);
- otadata still marks that slot invalid or aborted from a rollback (the mark stays until the next sync or a full
  flash, and can't be told apart from a failed update).

In both cases use `idf.py -p PORT flash`. Uploading through the web page needs none of this.

**Partition table.** `coredump` (256 KB at 0x820000) is the last partition, after `ota_1` (a dump of 6 decks
and 6 docks is estimated at 48–60 KB, and one that doesn't fit is not saved). A box gets the table with a full
flash (`./build.sh -p PORT`, `idf.py -p PORT flash` or the merged image). The firmware finds the partition at
run time. The partition table is never updated over the air.

| Name | Type | Offset | Size |
|---|---|---|---|
| nvs | data/nvs | 0x9000 | 24 KB: settings, Wi-Fi, settings password, dock map, RESET count |
| otadata | data/ota | 0xf000 | 8 KB |
| phy_init | data/phy | 0x11000 | 4 KB |
| ota_0 | app | 0x20000 | 4 MB |
| ota_1 | app | 0x420000 | 4 MB |
| coredump | data/coredump | 0x820000 | 256 KB |

**Tests.** `tools/test-host.sh` (also `./build.sh --test`) builds every pure module with `-Wall -Wextra
-Werror` on the host and runs its Unity tests (27 suites: the Satellite codec against Companion's
conformance corpus, sessions, deck models and manifests, deck protocol builders and parsers, image
transforms, status screens, config, discovery selection, the USB policy, the dock protocol and cache, the
auth rules, the restart breadcrumb, the supervisor's decisions (C6 lost, OTA self-check, pin save), the
Satellite client's wait rule, …). Then it checks that every inline script of the web page compiles
and that every page renders in 10 box states.

## First setup

A box with no network yet shows a setup screen on its decks with two QR codes: scan the first to join the
box's hotspot and the second to open the setup page, then set up Ethernet or Wi-Fi there.

1. **Plug in PoE and the deck(s).** The box gets an address by DHCP. A new box is in Network Dock mode:
   the decks show "Waiting for Companion" and "Remote surface" with the box's IP and their dock's port, plus the
   hotspot name and its password.
2. **Network Dock mode** (the default): in Companion 5, *Surfaces → Remote → Add Remote Surface* with the
   address and port a deck shows (5343 for the first deck), once per deck. **Satellite mode** (choose it on
   the web page; the box restarts): with one Companion 5 on the subnet, the decks appear in Companion within
   seconds with no configuration, and they show "Finding Companion" until then. With several, choose
   one on the web page.
3. **The web page:** `http://<box IP>/` or `http://companionbox-xxxx.local/` (the box name as a host
   name). It covers status, the Companion choice or a manual address, Ethernet (DHCP or static), Wi-Fi,
   the hotspot, the box name, the settings password, messages to the decks, the speaker and the status
   light, Find my box, firmware upload and the log.
4. **No DHCP, or no cable yet:** join the setup hotspot (below), then set a static IP or Wi-Fi.

The page asks before anything that restarts the box (restart, mode switch, firmware install, factory reset)
or that can cut you off (network changes, the settings password).

## Setup hotspot

- SSID `CompanionBox-XXXX` (XXXX = the last 4 hex digits of the Ethernet MAC, uppercase). The box's default
  name is the SSID.
- **Password-protected by default** (WPA2). A random 10-character password (letters and digits without
  look-alikes) is generated at first start and after a factory reset; you can change it (8-63 characters).
  It can be made open on the web page (*Hotspot → Hotspot password* off); switched off, the password is
  kept for when it is switched on again.
- **On all the time** next to Ethernet and Wi-Fi unless turned off. It follows the station's channel. Turning
  Wi-Fi off leaves it as it is.
- Its name and password ("Open" / "No password" if it is made open) are shown on the decks' maintenance
  screens (home, whenever Companion isn't driving them; and *Hotspot*, with QR codes to join it and open the
  page), on the web page and on the serial console.
- Join it and the phone opens the page (captive portal); otherwise go to `http://192.168.4.1/`.
- Turned off and the box unreachable: press RESET 3 times (hotspot for 15 minutes) or hold the service
  button 5 s. That hotspot is open or has the password, as the settings say.

> **Open hotspot.** If you make the hotspot open, anyone in range can join it and open the web page, which
> changes every setting. For an install, keep the hotspot password on, or set a settings password
> (*Device → Box name and password*), or turn the hotspot off. Note that the decks show the hotspot
> password to anyone who can see them on the maintenance screens' home whenever Companion isn't driving
> a deck (the normal idle screen in Network Dock mode), and on the *Hotspot* screen.

## Recovery

| What | How | Does |
|---|---|---|
| **RESET ×3** | press RESET 3 times in a row, each press within 3 s of the last start | setup hotspot on for 15 minutes, even if it is turned off. Nothing is reset. A PoE cable replugged a few times does no more than this |
| **Service button, short press** | press the GPIO32 button for 50 ms to 2 s | the box menu on the biggest deck with a screen, or, with any open, every box menu closed ([Maintenance screens](#maintenance-screens-on-the-decks)); released between 2 s and 5 s it does nothing |
| **Service button 5 s** | hold the GPIO32 button 5 s, then release | setup hotspot on for 15 minutes |
| **Service button 10 s** | hold it 10 s, then release | **factory reset**: settings, Wi-Fi and settings password erased, then a restart. The dock map (which deck has which Network Dock port) is kept, so Companion's Remote Surface entries keep working |
| Factory reset from the page | *Device → Factory reset → Reset now*, or `Device.FactoryReset` | the same erase. It needs the settings password when one is set |
| Bad firmware | usually nothing | a new image is checked 30 s after its first start and every 30 s after: RAM, USB host, web server and API running, and the network it was uploaded over back (that interface has an address, or the hotspot runs). It is confirmed at the first check that passes. One uploaded over a hotspot that the settings have off (forced on by RESET ×3 or the service button) gets the hotspot forced on again until then. One that crashes, or still fails 5 minutes after the start, is rolled back by the bootloader. An image that breaks the network in a way the check doesn't see (e.g. it answers on the address but Companion can't reach it) is confirmed: then reflash over USB |
| Broken firmware slot | usually nothing | the bootloader validates the image it starts; when that fails it starts the other slot, which holds a copy. This holds for a bootloader built from this tree, and only for an image that fails validation, not for one that crashes ([recovery slot](#recovery-slot)) |
| USB reflash after an OTA or a sync | `idf.py -p PORT flash` or `./build.sh -p PORT`; or `idf.py app-flash`, then **Boot other slot** | the box may run `ota_1` (the boot log's first line and the *Firmware* page name the slot); `app-flash` writes `ota_0` only, so the box keeps running `ota_1` and shows the new image as *Other slot newer*: start it with **Boot other slot**. `flash` also resets otadata, so the box starts `ota_0` |
| Hung task | nothing to do | the task watchdog (10 s) restarts the box |
| Wi-Fi chip (C6) lost | nothing to do | with Ethernet up the box goes on over the cable. Without it (no link or no address for 3 minutes in a row) and with Wi-Fi or the hotspot configured, the box restarts, which resets the C6: at most 3 times per power-on, then once every 30 minutes |
| Wedged deck | nothing to do | the box retries, then resets the USB port (at most every 10 s box-wide). A deck that the reset doesn't cure is quarantined for 10 minutes (doubling up to 60), then retried |

> **Settings PIN warning.** If you set a settings PIN and forget it, the web page and the API stay locked,
> and the page's factory reset needs that PIN too. The ways back are:
> - the **service button** on GPIO32, held 10 s. **It is optional hardware, not on the board**: wire a
>   push button from GPIO32 to GND before you need it;
> - without the button, a **USB reflash**: erase the NVS partition over the USB-C port. That loses every
>   setting (Wi-Fi, static IP, Companion choice, box name, dock map). The box then starts like a new one,
>   with the hotspot on and a new random password (shown on the decks and over serial):
>
>   ```sh
>   python -m esptool --chip esp32p4 -p PORT erase_region 0x9000 0x6000   # the nvs partition only
>   ```
>
> The page asks for the PIN twice and warns before saving it.

**Core dump.** A crash (panic or watchdog) writes an ELF core dump to the `coredump` partition before the
restart. From then on **every** start logs `Found core dump … in flash`, until the dump is erased or a
newer crash replaces it, so that line alone does not mean a new crash. The breadcrumb line
(`breadcrumb: reset: panic …`, below) says whether this start followed one. To read the dump, connect the
board's USB-C port and run, from this tree, with the ELF of the firmware that crashed
(`build/companion_box.elf` of the same build):

```sh
idf.py coredump-info -p PORT          # the crashed task, its backtrace, every task's stack
idf.py coredump-debug -p PORT         # the same in GDB
```

Then clear it, so that the next crash stands out. This erases the `coredump` partition and nothing else:
parttool reads the partition table from the board and erases that partition's range (0x820000, 256 KB).

```sh
python $IDF_PATH/components/partition_table/parttool.py --port PORT erase_partition --partition-name coredump
```

The dump of the box with one deck and Companion connected is about 32 KB.

**Restart breadcrumb.** Before every deliberate restart the box records why (`Device.Restart`, `mode
switch`, `OTA`, `OTA self-check failed` (the self-check rolling back to the previous image), `factory
reset`, `service button reset`, `USB power-on failed`, `C6 update`, `C6 lost`, `boot other slot`) in RTC memory that survives the
restart. The next start logs the reset reason with it, for example
`breadcrumb: reset: software restart: mode switch`. `Device.GetInfo.lastReset`
(`{reason, breadcrumb, summary}`) and `Sys.GetStatus.lastReset` (the summary) report it. A crash after a
deliberate restart was requested shows as `panic, last deliberate restart: mode switch pending`: the box
crashed after `esp_restart` was called, either in the shutdown it runs or early in the next start-up,
before `app_main` reads the record. `… pending (scheduled)` means it crashed in the second between the
request and `esp_restart`. The core dump tells which. A power-on clears the record.

### Recovery slot

The box keeps a working copy of the running firmware in the other app slot. The *Firmware*
page shows both slots and the state of the copy, with **Sync now** and **Boot other slot**.

- **When.** 10 minutes after the start, when the running firmware isn't pending verify (on its first start
  after an update the copy waits for the self-check), no upload runs, and the other slot holds no firmware,
  a failed one (invalid, or marked invalid or aborted by a rollback), another project's (below) or an older
  one. Once per start; a failed copy is logged and tried again an hour later (an upload that fails after a
  copy destroyed it, so the copy is made again). **Sync now** (`Ota.Sync`) copies at once.
- **How.** A low-priority task copies the running image (about 3 MB) through the OTA API, which validates
  it, then compares both images' SHA-256. It writes 4 KB at a time and pauses after every 64 KB, so the box
  goes on working. It never changes which slot starts. How long it takes and the worst pause it causes are
  not measured yet; the log line at the end gives the duration and the longest single flash write.
- **Newer firmware is kept.** A valid image of this project in the other slot whose build stamp (the date
  and time in its app descriptor) is the same as the running one's or later, e.g. one written with
  `idf.py app-flash`, is never overwritten by the box: the state is *Other slot newer*, and **Boot other
  slot** starts it. **Sync now** asks first and then replaces it (`Ota.Sync {"force": true}`). Only an
  earlier stamp makes an image older. The stamp is the build machine's local time, so builds from machines
  with different clocks or time zones can compare the wrong way round.
- **Another project's firmware** (another firmware for this board), or an image without an
  app descriptor, counts as *Invalid*: the sync replaces it like a failed one, and **Boot other slot**
  never starts it. The page names its project, or says it has no app descriptor.
- **Never during an update.** An upload while a sync runs is refused (409, "the recovery slot is being
  synced"); a sync waits while an upload runs. An upload replaces the copy with the new firmware, and 10
  minutes after that firmware started healthy it is copied over the previous one: going back with **Boot
  other slot** works within those 10 minutes; later, upload the old `.bin`.
- **Boot other slot** (`Ota.BootOther`) makes the other slot the boot slot and restarts. That firmware then
  checks itself like an update and the box goes back if it fails. It is refused for an empty or invalid
  slot, another project's firmware, and while the running firmware is pending verify.

What the bootloader does with the copy, read from this tree's `sdkconfig` and the ESP-IDF 5.5.6 bootloader
source (`bootloader_utility.c`, `esp_image_format.c`), not tried on the board:

- `CONFIG_BOOTLOADER_SKIP_VALIDATE_ON_POWER_ON`, `…_ALWAYS` and `…_IN_DEEP_SLEEP` are off, and secure boot
  is off: at every start, power-on included, the bootloader validates the image it picked (header and chip,
  segments, checksum, the appended SHA-256) before it runs it. (The SHA-256 and the checksum are skipped
  while a JTAG debugger is attached.)
- When the picked slot fails that validation, the bootloader tries the other app slot, whatever otadata says
  about it, and starts it if it validates. otadata stays as it was, so every start tries the broken slot
  first; the box, now running the copy, finds the broken slot invalid and copies itself over it 10 minutes
  later.
- Not covered: firmware that validates but crashes or hangs. A new image that does that before it is
  confirmed is rolled back; a confirmed one restarts in a loop, and the copy is the same firmware. Then
  reflash over USB.
- The bootloader is the one the last full flash wrote (`idf.py flash`, `./build.sh -p`); OTA updates and
  `app-flash` never write it. The above holds for a bootloader built from this tree (ESP-IDF 5.5.6, these
  settings). The first lines of the boot log give the bootloader's ESP-IDF version and build time.

## API

Every method works the same over HTTP and the WebSocket. `Device.ListMethods` lists them all (39). With
a settings PIN set, everything except `Auth.GetStatus`, `Auth.Login` and the page itself needs a
session token.

| Group | Methods |
|---|---|
| Device | `Device.GetInfo` (name, id, MAC, model, hardware, version, ESP-IDF, build date, `auth_en`, `lastReset`) `Device.GetConfig` / `SetConfig` (`boxName`, `mdnsEnabled`, `deckBrightness` 10–100 in steps of 10, default 70: a deck's brightness while it shows the maintenance screens, also on demand over Companion; `deckLimit` 2 or 4, default 2: the decks taken at once; a change saves and restarts the box, `restart_required` / `restarting` / `delay_ms` as a surface mode change; `companionAdopt`, default true: with a settings PIN set, Companion's *Discover* may still set the Companion address through the Satellite REST API; the box menu gesture, applied at once: `gestureEnabled` default true, `gestureKeys` two different corners of `topLeft` `topRight` `bottomLeft` `bottomRight`, the first tapped, default `["topRight","bottomRight"]`, `gestureTaps` 1–3, default 1, `gestureHoldSeconds` 2, 3 or 5, default 2) `Device.Restart` `Device.FactoryReset` `Device.SetAuth` (`{pass, old_pass}`: set, change or remove the settings PIN; a new one must be 4 to 8 digits, "The PIN must be 4 to 8 digits"; `old_pass` is the current PIN, or an older firmware's text password) `Device.ListMethods` `Device.Identify` (Find my box: beeps for 3 s, the light blinks white for 30 s) |
| System | `Sys.GetStatus` (`name`, `uptime_s`, `mode`, `net` (`route`, `ip`: the route's address; `eth_addr`, `sta_addr`: each connection's own; `eth_link`, `eth_dhcp`, `wifi_connected`, `wifi_enabled`, `wifi_set` (a Wi-Fi network is set up), `radio_down`, `sta_not_found` (the network being tried wasn't found), `sta_net`, `sta_ssid`, `sta_rssi`, `ap_on`, `ap_forced`, `ap_ssid`, `ap_ip`, `ap_clients`), `companion`, `dock`, `decks`, `problems`, `message`, `maintenance` (the box menu is open on a deck), each of `decks` with `menu` (open on it), `screen` (it has one) and `ignored` (over the deck limit), `deckLimit` (this run's), `decksIgnored` (the decks attached over it), `maintenanceDeck` (the deck the service button would open on, or the first open one), `firmware`, `c6`, `heap`, `lastReset`, `rev` (`cfg`: the settings saves since the start, 0 after a restart; `found`, `slots`: fingerprints of `Companion.List` and of `Ota.GetStatus`'s slots; each changes when they do)) |
| Companion | `Companion.GetConfig` / `SetConfig` (`companionAuto`, `companionHost`, `companionPort`, `companionPinId`, `mode`: a changed mode restarts the box) `Companion.GetStatus` (connection state, Companion and API version, the docks in dock mode) `Companion.List` (instances seen over mDNS in the last 30 s) `Companion.Choose {id}` (pin one) |
| Network | `Eth.GetConfig` / `SetConfig` / `GetStatus` (`ethDhcp`, `ethIp`, `ethMask`, `ethGw`, `ethDns`), `Wifi.GetConfig` / `SetConfig` / `GetStatus` (`wifiSsid`, `wifiPass`, and the backup network's `wifiBackupSsid`, `wifiBackupPass`, and `wifiEnabled` (default true; false: the station is off and the hotspot stays); the passwords are never returned, only `wifiHasPass` and `wifiBackupHasPass`; `GetStatus` adds `net`: `"main"`, `"backup"` or `"none"` for the network joined, with its name in `sta_ssid` and its signal in `sta_rssi`, `backup`: whether one is set, and `enabled`: `wifiEnabled` as applied), `Wifi.Scan` (starts a scan, `{results: true}` fetches the networks in range; see below), `Wifi.Use {net}` (`"main"` or `"backup"`: join it now and keep it as the one in use; errors when Wi-Fi isn't running or no backup is set up), `Ap.GetConfig` / `SetConfig` / `GetStatus` (`apEnabled`, `apSecured`, `apPass`; `apSecured` true, the default: WPA2, `apPass` 8-63 characters; false: the hotspot is open and `apPass` is kept unused. The hotspot password is returned, as the page shows it) |
| Decks | `Deck.List` (model, serial, firmware, USB speed, hub depth and port) `Deck.ShowMessage {text, seconds}` (1–160 printable characters and newlines; 0 s = until cleared, at most 3600 s) `Deck.ClearMessage` `Deck.ShowMaintenance {on, serial?}` (the box menu on that deck, Companion's too, or closed on it; without `serial`, true opens on the biggest deck with a screen, false closes them all; `{on, open: [serials], serial, model}` back, `on` any open, `serial` / `model` the deck just opened; a serial not attached or without a screen, or no deck with a screen, is an error) |
| Speaker and light | `Feedback.GetConfig` / `SetConfig` (`speakerEnabled`, `speakerVolume` 0–100, `soundReady`, `soundConnected`, `soundProblem`, `soundDeckPlug`, `soundKeyClick`, `ledEnabled`, `ledFlash`, `ledPin`, `ledBrightness` 1–100; GetConfig also lists the pins `ledPin` may name in `ledPins`) `Feedback.Test {event, volume?}` (plays `ready`, `connected`, `problem`, `deckIn`, `deckOut`, `keyClick`, `confirm` (a hold done), `locked` (a locked button) or `identify` and flashes the light, also when that sound is off; `volume` previews another volume) |
| Firmware | `Ota.GetStatus` (both slots, the running one, pending verify, the last upload's progress, and the [recovery slot](#recovery-slot) as `sync`: `{state, pct, other: {label, project, version, date, time, valid, foreign}}`, state `in_sync`, `syncing`, `differs`, `other_newer`, `empty`, `invalid` or `waiting`; `valid`: a valid image of this project, which `Ota.BootOther` may start; `foreign`: another project's image or one without an app descriptor, shown as `invalid`) `Ota.Sync {force?}` (copy the running firmware into the other slot now; refused while an upload or a sync runs, while the firmware is pending verify, and over a newer image unless `force`) `Ota.BootOther` (make the other slot the boot slot and restart; refused for an empty or invalid slot, another project's firmware, and while pending verify) |
| Log | `Log.Get {since}` (the last 16 KB of log text, 8 KB per call; pass `seq` back) |
| Security | `Auth.GetStatus` (public: `auth_en`, `pin`: the secret is a PIN, false for an older firmware's text password; `authed`, `locked_s`) `Auth.Login {password}` (public: the PIN, or an older firmware's text password; a 12 h token) `Auth.Logout` |

Every `*.SetConfig` takes `dry_run`. Config keys are camelCase (as stored). Status and result keys are
snake_case, with one exception: `lastReset` (camelCase) in `Device.GetInfo` and `Sys.GetStatus`.

**`Wifi.Scan`** (the page's "Scan for networks" button next to the Wi-Fi network name) lists the networks the
box hears. It does not wait for the scan (3–6 s): `Wifi.Scan` starts one and answers at once with
`{"scanning": true}` (also when one is already running; scans never queue), and `Wifi.Scan {"results": true}`
starts nothing and returns the latest answer: `{results: [{ssid, rssi, auth, channel}], scanning, ageMs,
error?}`. Poll it (the page does, every 500 ms for up to 10 s) until `scanning` is false. `results` has one
entry per name (the strongest), strongest first, at most 30, hidden names left out; `ageMs` is -1 before the
first scan; `error` says why the last scan failed. `auth` is `open`, `wep`, `wpa_psk`, `wpa2_psk`,
`wpa_wpa2_psk`, `wpa2_enterprise`, `wpa3_psk`, `wpa2_wpa3_psk` or `other`. Within 3 s of the end of the last
scan, `Wifi.Scan` answers with that scan's results and `"cached": true` instead of starting another. It needs
the token when a settings password is set; without a working Wi-Fi chip it fails with "no Wi-Fi: …". The
scan holds the network manager's lock only to start and to finish, so saving settings never waits for it, and
it puts the radio back as it was: with Wi-Fi off the station is started just for the scan, and with the
hotspot alone it joins the hotspot for the scan (back after the scan, or after 10 s without an end). A new
Wi-Fi setting during a scan cancels it. **While it runs the hotspot's clients may stall for a few seconds (the
radio leaves their channel), and the station's link may drop briefly**, so a scan from a page opened over the
hotspot or the Wi-Fi link can pause that page for a moment.

**HTTP (port 80):** `POST /rpc` with a frame `{"id":1,"method":"…","params":{…}}`; `GET /rpc/Sys.GetStatus`
or `POST /rpc/<Method>` with the params as the JSON body. **WebSocket** `/rpc`: the same frames, plus
`NotifyStatus` pushes when the status changes, checked every second and at once after a settings save (the page
uses them to redraw as things change). The status's `rev` says what to read again: `cfg` counts the settings saves
since the start (any `*.SetConfig`, `Companion.Choose`, the settings PIN, from the page, another browser, a deck or
Companion's *Discover*), `found` and `slots` are fingerprints of `Companion.List` and of `Ota.GetStatus`'s slots.
An open page reads again only what changed, so a setting changed in another browser or on a deck shows without a
reload: its settings page is drawn again panel by panel, and a panel with unsaved changes (or the cursor in one of its
fields) stays as it is, with "Unsaved changes. Also changed elsewhere: Save keeps yours." beside its Save. The
Companions found and the firmware slots (the copy's progress too) follow the same way; the log is read once a second.
**Auth:** `Authorization: Bearer <token>` on HTTP, `"token"` in a WebSocket frame. Five wrong entries in a
row from one address lock it for 60 s; each further lock of that address is twice as long (120 s, 240 s, …,
up to 1 h) until a right entry (a 4-digit PIN is short). The decks count as one more client ("deck")
with the same rules; its entry is reserved, so the network can never crowd it out or hold it back.
A box-wide budget slows a flood from many addresses (the decks are outside it: their own lockout bounds them). When the table of 16 addresses is full of locked
ones, a new address is not refused: it is held to that box-wide budget (about one try a second). Tokens are kept
as SHA-256 hashes in RAM, so a restart signs everyone out. **Firmware:** `POST /ota` with the raw `.bin`
(the page's *Firmware* page does this). An upload must finish within 30 s plus 1 s per 8 KB, or it is cut
off (a slow hotspot link can hit this).
**Companion Satellite REST** (`companion-satellite` `rest.ts`): `GET/POST /api/host`, `/api/port`,
`/api/config`, `GET /api/connected`, `/api/status`, `/api/surfaces`, `POST /api/surfaces/rescan`. With a settings
PIN set they need a token, except while `companionAdopt` is on (the default) in Satellite mode: then a request
with no `Authorization` header at all and a Host that passes the rebinding check below answers without one, and an
unauthenticated `POST` applies only `host` and `port` (any other field is ignored, not refused; the host must be 1-63
printable characters with no spaces, else 400). A request with an invalid or expired token gets 401. Such an adopt
that changes the address is logged ("adopted by <IP>: Companion <host>:<port>") and the decks show "Adopted"; one
that keeps it writes nothing. At most 3 changes in a row, then one a minute; more get 429 "Too many changes" with
nothing applied. In dock mode they need the token.
**Browser protections:** cross-site requests without a token are refused. While no password is set, and for the
Satellite REST requests answered without the PIN, only an IP, a `.local` name or the box's own name is
accepted as Host (no DNS rebinding). The page has a CSP.

**Discovery:** mDNS host `<box name>.local` (lower case, other characters as `-`). Satellite mode:
`_companion-satellite._tcp` on port 80. Dock mode: `_elg._tcp` per listening dock.

## Stream Deck support

From `components/deckmodel/model.c` (VID 0x0fd9). The **Module 32** and the **XL** have been run on a box;
the rest follow the documented protocol (`@elgato-stream-deck/core`, Elgato's HID docs) and the official
Companion mapping (`companion-surface-elgato-stream-deck`). Unknown PIDs with VID 0x0fd9 are ignored and
listed on the status page.

| Model | PID | Keys | Key image | Extras | Status |
|---|---|---|---|---|---|
| Stream Deck (Original v1) | 0x0060 | 5×3 | 72 px BMP, mirrored | gen1, 8191-byte reports | protocol only |
| Stream Deck Mini | 0x0063, 0x0090 | 3×2 | 80 px BMP, rotated | gen1; may be full-speed (direct only) | **run on the box** (through a hub, with 2 XL) |
| Stream Deck Mini Discord Edition | 0x00B3 | 3×2 | 80 px BMP | as Mini | protocol only |
| Stream Deck Module 6 | 0x00B8 | 3×2 | 80 px BMP | as Mini | protocol only |
| Stream Deck (Original v2) | 0x006D | 5×3 | 72 px JPEG, 180° | gen2 | protocol only |
| Stream Deck MK.2 (and Scissor) | 0x0080, 0x00A5 | 5×3 | 72 px JPEG, 180° | gen2 | protocol only |
| Stream Deck Module 15 | 0x00B9, 0x00E4 | 5×3 | 72 px JPEG, 180° | gen2 | protocol only |
| Stream Deck XL (v1, v2) | 0x006C, 0x008F | 8×4 | 96 px JPEG, 180° | gen2 | **run on the box** |
| **Stream Deck Module 32** | 0x00BA | 8×4 | 96 px JPEG, 180° | gen2 | **run on the box** (both modes) |
| Stream Deck Neo | 0x009A | 4×2 | 96 px JPEG, 180° | 2 colour-only touch keys (2/0, 2/3), 248×58 info bar (2/1) | protocol only |
| Stream Deck + | 0x0084 | 4×2 | 120 px JPEG | 4 encoders (row 3), 800×100 strip as four 200×100 cells (row 2), swipe = page change | protocol only |
| Stream Deck + XL | 0x00C6 | 9×4 | 112 px JPEG, 90° CCW | 6 encoders, 1200×100 strip; not in the official satellite mapping yet (best effort, Plus pattern) | protocol only |
| Stream Deck Pedal | 0x0086 | 3 pedals | none | input only, no screens | protocol only |
| Stream Deck Studio | – | – | – | has its own Ethernet: Companion talks to it directly | not supported |

## Known limitations

- **Hardware-tested on the Module 32, the XL and the Mini.** Hubs and several decks at once work (2 XL and a Mini,
  3 XL, through one unpowered hub on PoE). Four decks at once and a full-speed deck have not been tried on a box
  yet.
- **USB FIFO and OUT/IN gate.** With ESP-IDF's periodic-OUT FIFO bias the RX FIFO is 130 lines, just one
  512-byte packet: the controller then closed the decks' interrupt-IN transfer every ~250 µs without
  receiving anything, so keys never worked, and the IDF driver could skip a packet of an interrupt-OUT
  report around those re-arms (no error, then a driver assert). The firmware sets its own split (RX 260,
  periodic TX 572 lines): an idle IN transfer now stays queued until the deck has data. OUT reports are
  still serialised across decks in a gate. An IN that completes while an OUT is in flight parks, and is
  re-armed within 10 ms after it. A queued IN is never halted or waited for. A deck that holds off its own
  OUT data (about 200 ms in 1 of 1000 reports on the Module 32) delays only the re-arm of a parked IN. A
  hub's interrupt pipe is outside the gate (unverified risk).
- **EP0 wedge.** On the Module 32, a feature GET_REPORT sent right after image data never completes, and
  the control endpoint stays stuck until the deck re-enumerates. The root cause is unknown. The firmware
  therefore reads every feature report a host may ask for at open time and never sends a live feature GET
  afterwards. In dock mode it answers Companion's GETs from these copies. Any new post-open feature GET
  must be checked against this first.
- **Quarantine thresholds were tuned on one Module 32.** A deck is quarantined when it stalls again
  within 30 s of re-attaching after the box's USB reset, or after 4 resets in 10 minutes. Healthy decks
  stall rarely (two stalls 21 s apart were seen on the Module 32, both cured by a reset). Other models,
  hubs and long soaks may need other numbers.
- The FIFO split caps interrupt-IN packets at 1032 bytes and periodic OUT at 2288 bytes. No known deck uses
  more, but one that did would fail to open.
- A full-speed deck on a hub is invisible to the firmware (ESP-IDF 5.5 doesn't report the failed
  enumeration), so the page only shows a hint.
- With a settings PIN set and *Let Companion adopt this box without the PIN* on (the default), anyone on
  the network can point the box at another Companion through the Satellite REST API, as Companion's *Discover →
  setup* does; nothing else is open. A website can't (the Host check stops DNS rebinding), and the changes
  are rate-limited (3, then one a minute) so they can't wear out the flash. Turn it off to require the PIN for that
  too.
- **Settings PIN.** A new secret is a PIN of 4 to 8 digits; the page's sign-in asks for it with a
  phone's number pad. A box that already has a text password from an earlier version keeps it: the page
  still signs in with it (`Auth.GetStatus.pin` false, a plain password field), and asks you to set a PIN,
  which the decks need to unlock.
- The partition table is never updated over the air (see *Partition table*).
- A PIN check (PBKDF2, 10 000 rounds, about 270 ms) runs on the web server's one task and blocks other
  requests while it runs ([Measurements](#measurements)); a deck's runs on the deck actions' worker.
- In dock mode, only key images are held during a custom message; strip and info-bar images keep updating.

## Layout

```
main/main.c               staged boot (NVS, RESET count, config, speaker and light, network, web, discovery, surfaces, button, supervisor)
main/supervisor.c         applies settings, picks Companion, hotspot state, status to the light, OTA self-check, C6-lost restart, task watchdog
main/supervisor_logic.c   the supervisor's decisions: C6 lost, OTA self-check, pin save (pure)
main/svcbtn.c             the GPIO32 service button: short press, 5 s, 10 s (svcbtn_logic.c: hold times, pure)
main/quickreset_logic.c   RESET pressed 3 times (pure)
components/breadcrumb/    restart breadcrumb in RTC memory (breadcrumb_logic.c: record and summary, pure)
components/boxconfig/     the config: JSON in NVS, defaults, validation, factory reset; led_pins.c: the light's pins (pure)
components/netmgr/        Ethernet (IP101), Wi-Fi station + hotspot through the C6, route policy, captive DNS
components/c6fw/          keeps the C6 on the ESP-Hosted firmware the image carries (c6fw_logic.c: decision, pure)
components/discovery/     mDNS: announce the box, browse Companion, pick and pin one (select.c, pure)
components/satproto/      Satellite API line codec, base64, line assembly, session state machine (pure)
components/satclient/     the TCP connection to Companion: handshake, ping, reconnect (satclient_logic.c: wait rule, pure)
components/usbdeck/       USB host with hubs, raw HID decks, OUT/IN gate, bus reset, quarantine (usbdeck_policy.c, pure)
components/deckmodel/     the model table (PID -> geometry, image format, extras) and layout manifests
components/deckproto/     Stream Deck HID reports: images, features, input, report parser (pure)
components/imgproc/       rotate/flip, BMP packing, the P4's hardware JPEG encoder
components/statusui/      messages and "Reconnecting" drawn into key images, the fitting rules (deck_font.c: generated, Barlow Condensed Bold)
components/deckui/        the maintenance screens: tiles, per-model plan, alignment, drawing, icons, QR (qrcodegen, MIT),
                          the screen builders and the navigation (all pure)
components/surfaces/      decks <-> Companion: registry, image worker, coalescing, input map, messages, problems,
                          the maintenance screens on the decks (maintmap.c: snapshot and input route, pure);
                          dockmode.c + dockcache.c for Network Dock mode
components/elgdock/       Network Dock mode: Cora protocol (cora.c), dock replies (dockproto.c), servers, passthrough
components/feedback/      the speaker (ES8311, speaker.c) and the status NeoPixel (led.c): which status and sound
                          (fbstatus.c), the sounds and their player (fbsound.c, tones.c, tonegen.c), all pure
components/rpc/           JSON-RPC dispatcher (rpc_core.c, pure)
components/slotsync/      the recovery slot: a copy of the running firmware in the other app slot (slotsync_logic.c: decisions, pure)
components/webui/         HTTP server, WebSocket, OTA upload, Satellite REST, captive portal, methods, auth, log ring
components/webui/www/index.html   the web page (embedded gzip; Phosphor icons, MIT)
tools/build_c6.sh         builds the C6's ESP-Hosted firmware into build-c6/
tools/dock_check/         Network Dock conformance check with @elgato-stream-deck/tcp (Node)
tools/seed_config.py      NVS image with a starting config (Wi-Fi), for the first flash
tools/check_pages.js      every inline script of the page compiles; every page renders in 10 box states
tools/pack_page.py        strips and gzips the page for the firmware, checks its 32 KB limit
tools/mock_box.py         a stand-in box that serves the page with sample data (no hardware needed)
tools/phosphor_icons.py   the deck's Phosphor icons (Regular SVGs; --weight bold too) into components/deckui/phosphor.c (generated)
tools/test-host.sh        host tests (CMake + Ninja + ctest, Unity from ESP-IDF) and the page check
tools/idf-env.sh          activates ESP-IDF 5.5.6 with its Python venv
test/host/                unit tests of the pure modules; fixtures/ holds Companion's Satellite conformance corpus
build.sh                  build, flash, release, tests
version.txt               the next release's version
partitions.csv            16 MB layout: nvs, otadata, phy_init, two 4 MB app slots, coredump
sdkconfig.defaults        the build settings (the generated sdkconfig is not in git)
docs/                     this manual, the flashing guide, screenshots
```

## Hardware test list

What has been run on a real box so far (the bench: the deck straight on the USB-A port, the box on Wi-Fi,
Companion 5.1.1 on the LAN), and what still needs a board to check. Contributions of test results are welcome,
in particular for the decks and setups not covered yet.

**Run on a box:**
- the Module 32 and the XL; on the Module 32, Satellite mode (handshake, `ADD-DEVICE`, images) and Network Dock
  mode (`dock_check` passes with the deck directly on 5343, Companion 5.1.1 lists one surface per deck, key
  presses reach Companion, a custom message over Companion's images and the replay after it);
- a hub with several decks at once: 2 XL and a Mini, and 3 XL, through one unpowered hub, the box on PoE alone;
- a clean boot log, the C6 on ESP-Hosted 3.0.9, the Wi-Fi station, NVS settings across restarts;
- mode switches both ways (24 restarts in a row with no panic), the restart breadcrumb, a core dump read back
  with `idf.py coredump-info`;
- OTA upload, the self-check confirming the new image, 409 for a second upload while one runs;
- the web page: RPC, REST, WebSocket with `NotifyStatus`, every page in dark and light at 1280 and 375 px with no
  console errors and no external requests; the sign-in lock-out; the page reconnecting after a restart;
- USB: the Module 32 opening under the FIFO split, 1.15 M reports and 28 detach/attach cycles with no restart,
  the quarantine and the 10 s reset floor with injected stalls, the deck record surviving a restart.

**Still to check on hardware:**
- four decks at once (deck limit 4), full-speed decks behind a hub and direct;
- the +, + XL, Neo, Mini and Pedal; the strip, the info bar, the dials and the touch keys; the PIN pad and the
  static-address editor on each layout; the QR codes scanned with a phone;
- PoE: board idle power, the PoE class negotiated, the most decks PoE alone can run ([Measurements](#measurements));
- Ethernet ↔ Wi-Fi failover, the backup Wi-Fi switching, a static IP without DHCP, the captive portal on an
  iPhone and an Android phone;
- a deliberately broken image rolling back (one that crashes, one that kills the network it came over), the C6
  update path, a power cut during a C6 update;
- RESET ×3, the service button (short press, 5 s, 10 s), factory reset;
- the recovery slot's timings (the first sync, how long it takes, the worst hiccup during it);
- the speaker: the amplifier's idle switch-off (`SPK_IDLE_US`, hiss against pop), each sound from its event; the
  status light's states by eye;
- Companion's *Surfaces → Discover* adopting a PIN-locked box, and two Companions on the LAN;
- the box menu gesture set on the web page, in both modes: off (the default gesture then opens nothing), other
  corners, 2 and 3 taps, the 3 and 5 s holds, and the default keys never late once the gesture moved off them.

## Measurements

| What | Value | How |
|---|---|---|
| Start to "ready" | 4.5 s, 4.3 s of it the network stage (C6 bring-up) | serial log, 0.1.0-dev, -O2 |
| Power-on to the deck added in Companion | 14.4 s | Wi-Fi, Module 32, Companion 5.1.1 |
| Restart (`Device.Restart` or a mode switch) to the API answering | about 12 s; Companion has the deck again 3–5 s later | Wi-Fi, 24 mode-switch restarts |
| The web page across a restart / an outage | reconnects 12–14 s after a restart; the "No answer from the box" chip shows 28–40 s after the last answer | Wi-Fi, browser; the chip waits for the 20 s RPC timeout while a silent WebSocket stays open |
| 32-key page, dock mode | 95 ms (96 ms in the -Og build: the USB bus sets the pace); since the FIFO fix 66–116 ms single runs, p50 101 / p90 180 / max 534 ms over a 10-minute soak with Companion retaking the dock between runs | `dock_check`: from the first of 32 key images to a report queued behind them coming back |
| USB client task at idle | 9.3 % of a core with one deck before the FIFO fix (4000 empty IN completions a second); 0.00–0.05 % after it | FreeRTOS run-time stats, temporary build, Module 32 |
| Settings password check (PBKDF2-HMAC-SHA256, 10 000 rounds) | 268 ms, timed on the box around the check; an `Auth.Login` round trip takes 270–320 ms above a plain call, depending on which core the web server's task runs on (core 0 also carries the USB client task) | 30 logins over one HTTP connection. The -O2 build did not change it: `auth_logic.c` was already compiled -O2 and its code is byte-identical (the image before: 270 ms) |
| Web page | 33,822 bytes gzipped as embedded (the limit is 40,960); served in about 80 ms when it was 25 KB, not timed since | Wi-Fi |
| Firmware upload (`POST /ota`, 2.56 MB) | 34–43 s (60–76 KB/s), then confirmed 30 s after the restart | Wi-Fi |
| Firmware image | 3.11 MB (0x2f83c0), 26 % of the 4 MB slot free; the speaker and light (I2S, I2C, RMT drivers and esp_codec_dev) add about 112 KB | 0.1.0, -O2 |
| Free RAM after start | internal 340 KB, PSRAM 32.2 MB | boot log |
| Core dump | 32 KB (30 tasks; dock mode 32 KB too); the partition is 256 KB | test crash, one deck |
| Lowest free task stack (bytes) | captive_dns 880, deck task 1420, tiT 1368, usb_client 1816, docktx 2168, sys_evt 2228, satclient 3224, surfaces 3276, imgwork 5972, httpd 4832 | high-water marks in both modes, Companion connected and `dock_check` run |
| Board idle power | not measured yet | |
| PoE class negotiated | not measured yet | |
| Decks on PoE alone | 2 XL and a Mini; 3 XL while their pages change little | an unpowered hub, deck limit 4 |

## Credits and licences

This firmware is under the PolyForm Shield 1.0.0 licence ([LICENSE](../LICENSE)): use, change and share it for
any purpose, except to provide a product that competes with it or with Companion Dock. The third-party parts and
their licences are listed in [THIRD_PARTY.md](../THIRD_PARTY.md).

- The Network Dock mode follows the Elgato TCP protocol as implemented by
  [`@elgato-stream-deck`](https://github.com/Julusian/node-elgato-stream-deck) (`packages/tcp`,
  `packages/core`; MIT), which Companion's surface module uses. Its TCP framing is ported to C in
  `components/elgdock/cora.h` / `cora.c`, which keep its MIT notice; otherwise it was the protocol reference only. `tools/dock_check` uses `@elgato-stream-deck/tcp` from npm as a test
  tool.
- The Satellite API follows [bitfocus/companion](https://github.com/bitfocus/companion)'s
  `satellite-protocol` (MIT). Its conformance corpus is in `test/host/fixtures/`.
- The web page's icons are from [Phosphor Icons](https://phosphoricons.com) (MIT, © 2023 Phosphor Icons), and so
  are the deck's: the Regular outlines of `@phosphor-icons/core` 2.1.1 (`assets/regular/<name>.svg`),
  flattened by `tools/phosphor_icons.py` into the generated `components/deckui/phosphor.c`, which carries
  Phosphor's MIT notice and the source names. Companion's icon, on the deck and the web page, is Phosphor's
  webhooks-logo; Satellite's is [Lucide](https://lucide.dev)'s satellite (ISC, © Lucide Contributors; lucide-react
  0.525.0), drawn as strokes on the deck (`components/deckui/icons.c`) and as an SVG symbol on the page.
- The deck text font is [Barlow Condensed](https://github.com/jpt/barlow) Bold, © 2017 The Barlow Project
  Authors, under the SIL Open Font License 1.1 (`tools/fonts/OFL.txt`). `tools/gen_deck_font.py`
  rasterises it into `components/statusui/deck_font.c`. The firmware image carries its glyphs, so a
  release `.bin` shipped on its own (an OTA download, a release) must include `tools/fonts/OFL.txt`.
- The QR codes on the decks are made with [QR Code generator](https://www.nayuki.io/page/qr-code-generator-library)
  by Project Nayuki (MIT, `components/deckui/qrcodegen/`).
- Host tests use Unity and cJSON as shipped with ESP-IDF.
- No code from GPL projects is used.

Not affiliated with or endorsed by Elgato / Corsair or Bitfocus AS. Elgato, Stream Deck and Network Dock are
trademarks of Corsair; Companion is a product of Bitfocus AS. The names only say what this works with, and the
Network Dock mode only speaks the same network protocol as Elgato's Network Dock.
