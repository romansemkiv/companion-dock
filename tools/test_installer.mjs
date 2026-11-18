#!/usr/bin/env node
// Tests of the browser installer's flash plan (installer/flasher/plan.mjs): what an update writes, what it must
// leave alone (the settings in NVS) and when it must refuse. Run by tools/test-host.sh: node --test tools/test_installer.mjs
//
// The image is the release in firmware/ (latest.json names it). Expected bytes come from the release's separate
// files and from the addresses in partitions.csv and docs/flashing.md, never from plan.mjs itself.
import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { checkFullImage, imageInfo, planFresh, planUpdate, readBoard } from "../installer/flasher/plan.mjs";

const fw = (name) => new Uint8Array(readFileSync(fileURLToPath(new URL("../firmware/" + name, import.meta.url))));
const latest = JSON.parse(readFileSync(fileURLToPath(new URL("../firmware/latest.json", import.meta.url)), "utf8"));
const FULL = fw(latest.full), APP = fw(latest.app), BOOTLOADER = fw("bootloader.bin"), TABLE = fw("partition-table.bin");

// partitions.csv
const NVS = 0x9000, NVS_END = 0xf000, OTADATA = 0xf000, OTADATA_END = 0x11000, OTA_0 = 0x20000, OTA_1 = 0x420000;
const FLASH_SIZE = OTA_1 + 0x1000;

const u32 = (n) => [n & 255, (n >>> 8) & 255, (n >>> 16) & 255, (n >>> 24) & 255];
const ascii = (s, len) => Array.from({ length: len }, (_, i) => (i < s.length ? s.charCodeAt(i) : 0));
/* one partition table entry: magic AA 50, type, subtype, offset, size, a 16-byte label, flags */
const entry = (type, subtype, offset, size, label) => [0xaa, 0x50, type, subtype, ...u32(offset), ...u32(size), ...ascii(label, 16), ...u32(0)];
const table = (...entries) => { const t = new Uint8Array(0x1000).fill(0xff); t.set(entries.flat()); return t; };
/* one otadata record: sequence, 20 unused bytes, state (2 = valid), CRC32 of the sequence */
const otaRecord = (seq, crc) => [...u32(seq), ...Array(20).fill(0xff), ...u32(2), ...u32(crc)];

const SETTINGS = Uint8Array.from({ length: NVS_END - NVS }, (_, i) => (i * 7 + 3) & 0xff);

/* A box as the full image leaves it, with settings saved since. */
function box() {
  const flash = new Uint8Array(FLASH_SIZE).fill(0xff);
  flash.set(FULL);
  flash.set(SETTINGS, NVS);
  return flash;
}
const reader = (flash) => async (addr, size) => flash.slice(addr, addr + size);
function apply(flash, parts) {
  for (const p of parts) {
    assert.equal(p.address % 0x1000, 0, "a part starts on a flash sector");
    flash.set(p.data, p.address);
  }
  return flash;
}
const region = (flash, from, to) => Buffer.from(flash.subarray(from, to));
const blank = (flash, from, to) => flash.subarray(from, to).every((b) => b === 0xff);
/* an app header like the release's, with another version or project name (app descriptor: 0x30 and 0x50) */
function appHeader({ version, project }) {
  const h = APP.slice(0, 256);
  if (version) h.set(ascii(version, 32), 0x30);
  if (project) h.set(ascii(project, 32), 0x50);
  return h;
}

test("a full image is recognised and names its version", () => {
  checkFullImage(FULL);
  assert.equal(imageInfo(FULL).version, latest.version);
});

test("the app-only file is refused as a full image", () => {
  assert.throws(() => checkFullImage(APP), /-full\.bin/);
});

test("a fresh install writes the whole image at 0", () => {
  const parts = planFresh(FULL);
  assert.equal(parts.length, 1);
  assert.equal(parts[0].address, 0);
  assert.deepEqual(region(apply(new Uint8Array(FLASH_SIZE).fill(0xff), parts), 0, FULL.length), Buffer.from(FULL));
});

test("the board check reports the firmware a freshly installed box runs", async () => {
  const board = await readBoard(FULL, reader(box()));
  assert.equal(board.app.version, latest.version);
  assert.equal(board.slot, 0);
});

test("the board check follows the boot record to the second slot", async () => {
  const flash = box();
  flash.set(otaRecord(2, 0x55f63774), OTADATA);
  flash.set(appHeader({ version: "0.2.5" }), OTA_1);
  const board = await readBoard(FULL, reader(flash));
  assert.equal(board.slot, 1);
  assert.equal(board.app.version, "0.2.5");
});

test("a boot record with a wrong checksum is ignored", async () => {
  const flash = box();
  flash.set(otaRecord(2, 0x12345678), OTADATA);
  flash.set(appHeader({ version: "0.2.5" }), OTA_1);
  const board = await readBoard(FULL, reader(flash));
  assert.equal(board.slot, 0);
  assert.equal(board.app.version, latest.version);
});

test("of two boot records the later one wins", async () => {
  const flash = box();
  flash.set(otaRecord(2, 0x55f63774), OTADATA);
  flash.set(otaRecord(3, 0xed4a5011), OTADATA + 0x1000);
  flash.set(appHeader({ version: "0.2.5" }), OTA_1);
  const board = await readBoard(FULL, reader(flash));
  assert.equal(board.slot, 0);
});

test("an update keeps the settings and replaces the app", async () => {
  const flash = box();
  const plan = planUpdate(FULL, await readBoard(FULL, reader(flash)));
  assert.equal(plan.ok, true);
  apply(flash, plan.parts);
  assert.deepEqual(region(flash, NVS, NVS_END), Buffer.from(SETTINGS));
  assert.deepEqual(region(flash, OTA_0, OTA_0 + APP.length), Buffer.from(APP));
});

test("an update writes nothing into the settings area", async () => {
  const plan = planUpdate(FULL, await readBoard(FULL, reader(box())));
  for (const p of plan.parts)
    assert.ok(p.address + p.data.length <= NVS || p.address >= NVS_END, `0x${p.address.toString(16)} overlaps the settings`);
});

test("an update leaves an unchanged bootloader alone", async () => {
  const plan = planUpdate(FULL, await readBoard(FULL, reader(box())));
  assert.ok(plan.parts.every((p) => p.address >= OTADATA), "nothing below the boot record is written");
});

test("an update of a box running the second slot makes it start the new app", async () => {
  const flash = box();
  flash.set(otaRecord(2, 0x55f63774), OTADATA);
  flash.set(appHeader({ version: "0.2.5" }), OTA_1);
  const plan = planUpdate(FULL, await readBoard(FULL, reader(flash)));
  apply(flash, plan.parts);
  assert.ok(blank(flash, OTADATA, OTADATA_END), "the boot record is reset, so the box starts ota_0");
  assert.deepEqual(region(flash, OTA_0, OTA_0 + APP.length), Buffer.from(APP));
  assert.deepEqual(region(flash, NVS, NVS_END), Buffer.from(SETTINGS));
});

test("an update replaces a different bootloader, after the app", async () => {
  const flash = box();
  flash[0x2000 + 0x40] ^= 0x55; /* an older bootloader */
  const plan = planUpdate(FULL, await readBoard(FULL, reader(flash)));
  assert.equal(plan.ok, true);
  assert.ok(plan.parts.at(-1).address < NVS, "the bootloader goes last: a stop during the app leaves the box as it was");
  apply(flash, plan.parts);
  assert.deepEqual(region(flash, 0x2000, 0x2000 + BOOTLOADER.length), Buffer.from(BOOTLOADER));
  assert.deepEqual(region(flash, NVS, NVS_END), Buffer.from(SETTINGS));
});

test("an update brings an older partition table up to date", async () => {
  const flash = box();
  /* the table before the core dump partition grew to 256 KB */
  flash.set(table(
    entry(1, 2, 0x9000, 0x6000, "nvs"), entry(1, 0, 0xf000, 0x2000, "otadata"), entry(1, 1, 0x11000, 0x1000, "phy_init"),
    entry(0, 16, 0x20000, 0x400000, "ota_0"), entry(0, 17, 0x420000, 0x400000, "ota_1"), entry(1, 3, 0x820000, 0x10000, "coredump")), 0x8000);
  const plan = planUpdate(FULL, await readBoard(FULL, reader(flash)));
  assert.equal(plan.ok, true);
  apply(flash, plan.parts);
  assert.deepEqual(region(flash, 0x8000, 0x8000 + TABLE.length), Buffer.from(TABLE));
  assert.deepEqual(region(flash, NVS, NVS_END), Buffer.from(SETTINGS));
});

test("an update is still offered when the app is damaged (a flash that stopped halfway)", async () => {
  const flash = box();
  flash.fill(0xff, OTA_0, OTA_0 + 0x1000);
  const board = await readBoard(FULL, reader(flash));
  assert.equal(board.app, null);
  assert.equal(planUpdate(FULL, board).ok, true);
});

test("a blank board gets no update", async () => {
  const board = await readBoard(FULL, reader(new Uint8Array(FLASH_SIZE).fill(0xff)));
  assert.deepEqual(planUpdate(FULL, board), { ok: false, why: "blank" });
});

test("a board with another partition layout gets no update", async () => {
  const flash = box();
  /* ESP-IDF's stock "factory app, two OTA definitions" table: the settings area is smaller and otadata sits elsewhere */
  flash.set(table(
    entry(1, 2, 0x9000, 0x4000, "nvs"), entry(1, 0, 0xd000, 0x2000, "otadata"), entry(1, 1, 0xf000, 0x1000, "phy_init"),
    entry(0, 0, 0x10000, 0x100000, "factory"), entry(0, 16, 0x110000, 0x100000, "ota_0"), entry(0, 17, 0x210000, 0x100000, "ota_1")), 0x8000);
  assert.deepEqual(planUpdate(FULL, await readBoard(FULL, reader(flash))), { ok: false, why: "layout" });
});

test("a board running another project gets no update, even with the same layout", async () => {
  const flash = box();
  flash.set(appHeader({ project: "hello_world" }), OTA_0);
  assert.deepEqual(planUpdate(FULL, await readBoard(FULL, reader(flash))), { ok: false, why: "foreign" });
});
