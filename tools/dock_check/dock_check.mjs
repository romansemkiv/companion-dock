#!/usr/bin/env node
// Network Dock mode conformance check (criterion 13). Connects to the box with the library
// Companion 5 itself uses for remote Stream Decks (@elgato-stream-deck/tcp, pinned in package.json):
// StreamDeckTcpConnectionManager with auto-connect to secondaries, as Companion's elgato-stream-deck
// surface module sets it up. Then it drives the deck as Companion does.
//
//   cd tools/dock_check && npm ci        (Node >= 22.18: the JPEG encoder it installs needs it)
//   node tools/dock_check/dock_check.mjs <box ip> [port] [--seconds N] [--hold] [--blank] [--repaint S]
//
// A dock's port presents its deck itself: the library must find the deck as the device on
// the port (its own model, serial and firmware), with no Network Dock and no secondary device. A
// Network Dock on the port (older firmware) fails the check. A port with no deck isn't
// open: the library keeps retrying and the check fails with "no deck within 20 s". Prints the deck (model, VID/PID, serial, firmware, the box's MAC), sets its brightness
// to 60, fills every key with a distinct colour (images over the passthrough), reads a report queued
// behind those images (so the time includes the deck taking them), clears the panel and then logs key
// / encoder events for N seconds (default 20). Exit 0 only when the deck appeared directly and every
// write resolved.
//
// One client per port: the check takes the deck over from Companion, which reconnects about 5 s
// later and takes it back, ending the check's connection. With Companion connected, keep the check
// under 5 s: --seconds 0 (or --hold --seconds 1).
// --hold: the colours stay up (no clear) and the connection stays open the N seconds, e.g. to show a
// box message over them (Deck.ShowMessage) and see them come back. --repaint S: S seconds after the
// first fill, every key is filled again with the colours shifted by half the palette (key i gets key
// i + n/2's colour), e.g. while a message is shown, to see the box keep Companion's newest images.
// --blank: clearPanel before the first fill, as Companion's surface module does on every connect.
import { DeviceModelId, StreamDeckTcpConnectionManager } from '@elgato-stream-deck/tcp'

const CONNECT_TIMEOUT_MS = 20000
const LOOK_MS = 3000 // colours stay up this long before the clear

function usage(why) {
    if (why) console.error(`error: ${why}`)
    console.error('usage: node dock_check.mjs <ip> [port] [--seconds N] [--hold] [--blank] [--repaint S]')
    process.exit(2)
}

const argv = process.argv.slice(2)
let ip = null
let port = 5343
let seconds = 20
let hold = false
let blank = false
let repaint = null
for (let i = 0; i < argv.length; i++) {
    if (argv[i] === '--seconds') {
        seconds = Number(argv[++i])
        if (!Number.isFinite(seconds) || seconds < 0) usage('--seconds needs a number >= 0')
    } else if (argv[i] === '--hold') {
        hold = true
    } else if (argv[i] === '--blank') {
        blank = true
    } else if (argv[i] === '--repaint') {
        repaint = Number(argv[++i])
        if (!Number.isFinite(repaint) || repaint < 0) usage('--repaint needs a number of seconds >= 0')
    } else if (!ip) {
        ip = argv[i]
    } else {
        port = Number(argv[i])
        if (!Number.isInteger(port) || port < 1 || port > 65535) usage('port must be 1-65535')
    }
}
if (!ip) usage('the box IP address is required')

const t0 = Date.now()
const log = (...a) => console.log(`[${((Date.now() - t0) / 1000).toFixed(2).padStart(6)} s]`, ...a)
const sleep = (ms) => new Promise((r) => setTimeout(r, ms))
const hex4 = (v) => `0x${v.toString(16).padStart(4, '0')}`

let failures = 0
let deck = null
let finished = false

async function step(name, fn) {
    const t = Date.now()
    try {
        const r = await fn()
        log(`ok   ${name} (${Date.now() - t} ms)`)
        return r
    } catch (e) {
        failures++
        log(`FAIL ${name}: ${e?.message ?? e}`)
        return undefined
    }
}

async function describe(sd, what) {
    const info = await step(`${what}: getHidDeviceInfo`, () => sd.getHidDeviceInfo())
    const serial = await step(`${what}: getSerialNumber`, () => sd.getSerialNumber())
    const fw = await step(`${what}: getFirmwareVersion`, () => sd.getFirmwareVersion())
    const ids = info ? `VID ${hex4(info.vendorId)} PID ${hex4(info.productId)}` : 'VID/PID ?'
    log(
        `${what.toUpperCase()}: ${sd.PRODUCT_NAME} (model ${sd.MODEL}) at ${sd.remoteAddress}:${sd.remotePort}, ` +
            `${ids}, serial ${serial ?? '?'}, firmware ${fw ?? '?'}`,
    )
}

function childText(info) {
    return info
        ? `VID ${hex4(info.vendorId)} PID ${hex4(info.productId)} serial ${info.serialNumber} port ${info.tcpPort}`
        : 'none'
}

function fail(why) {
    failures++
    log(`FAIL ${why}`)
}

// A distinct, saturated colour per key.
function colour(i, n) {
    const h = (i / n) * 6
    const x = Math.round(255 * (1 - Math.abs((h % 2) - 1)))
    const table = [
        [255, x, 0],
        [x, 255, 0],
        [0, 255, x],
        [0, x, 255],
        [x, 0, 255],
        [255, 0, x],
    ]
    return table[Math.floor(h) % 6]
}

function solid(w, h, [r, g, b]) {
    const buf = Buffer.alloc(w * h * 3)
    for (let p = 0; p < buf.length; p += 3) {
        buf[p] = r
        buf[p + 1] = g
        buf[p + 2] = b
    }
    return buf
}

// Fills every key: key i gets colour (i + shift) of the palette. Resolves with the number filled.
async function fillKeys(sd, keys, shift) {
    let ok = 0
    await Promise.all(
        keys.map((key, i) => {
            const rgb = colour((i + shift) % keys.length, keys.length)
            const p =
                key.feedbackType === 'lcd'
                    ? sd.fillKeyBuffer(key.index, solid(key.pixelSize.width, key.pixelSize.height, rgb))
                    : sd.fillKeyColor(key.index, ...rgb)
            return p.then(
                () => ok++,
                (e) => {
                    failures++
                    log(`FAIL fill key ${key.index}: ${e?.message ?? e}`)
                },
            )
        }),
    )
    return ok
}

async function exercise(sd) {
    await step('deck: setBrightness(60)', () => sd.setBrightness(60))
    if (blank) await step('deck: clearPanel first (--blank, as Companion does on connect)', () => sd.clearPanel())

    const keys = sd.CONTROLS.filter((c) => c.type === 'button' && c.feedbackType !== 'none')
    const lcds = sd.CONTROLS.filter((c) => c.type === 'lcd-segment')
    const t = Date.now()
    const lcdWrites = lcds.map((lcd) =>
        sd.fillLcd(lcd.id, solid(lcd.pixelSize.width, lcd.pixelSize.height, [255, 255, 255]), { format: 'rgb' }).then(
            () => log(`ok   fill LCD segment ${lcd.id}`),
            (e) => {
                failures++
                log(`FAIL fill LCD segment ${lcd.id}: ${e?.message ?? e}`)
            },
        ),
    )
    const [ok] = await Promise.all([fillKeys(sd, keys, 0), ...lcdWrites])
    log(`${ok}/${keys.length} keys filled with distinct colours: all writes handed to the socket in ${Date.now() - t} ms`)
    // The box runs a feature report read in order behind the image reports already queued for the
    // deck, so its answer marks the deck having taken the whole page.
    await step('deck: getSerialNumber behind the page (page on the deck)', () => sd.getSerialNumber())
    log(`page drawn: ${Date.now() - t} ms from the first write to the deck having all of it`)

    if (repaint !== null) {
        const at = t + repaint * 1000
        setTimeout(async () => {
            if (finished) return
            const shift = Math.floor(keys.length / 2)
            const t2 = Date.now()
            const n = await fillKeys(sd, keys, shift)
            log(`repainted ${n}/${keys.length} keys, colours shifted by ${shift}: handed to the socket in ${Date.now() - t2} ms`)
            await step('deck: getSerialNumber behind the repaint', () => sd.getSerialNumber())
        }, Math.max(0, at - Date.now()))
    }
    if (hold) {
        log(`--hold: the colours stay up and the connection open for ${seconds} s`)
    } else {
        log(`colours stay up ${LOOK_MS / 1000} s`)
        await sleep(LOOK_MS)
        await step('deck: clearPanel', () => sd.clearPanel())
    }

    for (const ev of ['down', 'up']) {
        sd.on(ev, (c) => log(`event: ${c.type} ${c.index} ${ev}`))
    }
    sd.on('rotate', (c, amount) => log(`event: encoder ${c.index} turned ${amount}`))
    for (const ev of ['lcdShortPress', 'lcdLongPress']) sd.on(ev, (c, pos) => log(`event: ${ev} at ${pos.x},${pos.y}`))
    sd.on('lcdSwipe', (c, from, to) => log(`event: swipe ${from.x},${from.y} -> ${to.x},${to.y}`))
    log(`logging key / encoder events for ${seconds} s (press keys now)`)
    await sleep(seconds * 1000)
    finish()
}

const manager = new StreamDeckTcpConnectionManager({ autoConnectToSecondaries: true })
manager.on('error', (m) => log(`library error: ${m}`))
manager.on('disconnected', (sd) => {
    log(`disconnected: ${sd.PRODUCT_NAME} ${sd.remoteAddress}:${sd.remotePort}`)
    if (sd === deck && !finished) fail('the deck connection dropped during the check (another client, e.g. Companion, took the port?)')
})
manager.on('connected', (sd) => {
    sd.on('error', (e) => log(`device error (${sd.PRODUCT_NAME}): ${e?.message ?? e}`))
    const run = async () => {
        if (sd.MODEL === DeviceModelId.NETWORK_DOCK) {
            await describe(sd, 'dock')
            const info = await step('dock: getChildDeviceInfo', () => sd.getChildDeviceInfo())
            fail(
                `${sd.remoteAddress}:${sd.remotePort} presents a Network Dock (child: ${childText(info)}), not a deck: ` +
                    'older firmware?',
            )
            finish()
            return
        }
        if (deck) {
            fail(`a second device: ${sd.PRODUCT_NAME} ${sd.remoteAddress}:${sd.remotePort} (a deck's port has no secondary)`)
            return
        }
        deck = sd
        await describe(sd, 'deck')
        const info = await step('deck: getChildDeviceInfo (the library asks it of every primary)', () =>
            sd.getChildDeviceInfo(),
        )
        if (info) fail(`the deck reports a child device: ${childText(info)}`)
        else log('deck: no child device, so the library opens no secondary connection')
        const mac = await step('deck: getMacAddress', () => sd.getMacAddress())
        if (mac) log(`deck: on the box with MAC ${mac}`)
        await exercise(sd)
    }
    run().catch((e) => {
        failures++
        log(`FAIL ${e?.stack ?? e}`)
        finish()
    })
})

function finish() {
    if (finished) return
    finished = true
    manager.disconnectFromAll()
    const pass = deck !== null && failures === 0
    log(`${pass ? 'PASS' : 'FAIL'}: deck ${deck ? 'appeared directly' : 'never appeared'}, ${failures} failure(s)`)
    setTimeout(() => process.exit(pass ? 0 : 1), 100)
}

log(`connecting to ${ip}:${port} with @elgato-stream-deck/tcp (auto-connect to secondaries, as Companion)`)
manager.connectTo(ip, port)
setTimeout(() => {
    if (!deck && !finished) {
        fail(`no deck within ${CONNECT_TIMEOUT_MS / 1000} s`)
        finish()
    }
}, CONNECT_TIMEOUT_MS)
