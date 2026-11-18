// What the installer writes, and where. No DOM and no serial port in here: tools/test_installer.mjs runs it in Node.
import { md5 } from "./md5.mjs";
//
// A full image is the whole flash from 0x0: the bootloader at 0x2000, the partition table at 0x8000, then the
// partitions the table names, the first app slot last. Everything between the table and that app is blank in the
// image, the settings (NVS) included, which is why writing the image whole erases them.
//
// An update writes the same image in pieces and skips the settings: the app into the first slot, a blank boot record
// (otadata) so the bootloader starts that slot whichever one ran before, and the bootloader with the partition table
// only when the board's differ. It is what `idf.py flash` does.

const BOOT_AT = 0x2000;   /* the ESP32-P4's bootloader offset */
const TABLE_AT = 0x8000;  /* CONFIG_PARTITION_TABLE_OFFSET, the default */
const BOOT_END = TABLE_AT + 0x1000;
const SECTOR = 0x1000;
const IMAGE_MAGIC = 0xe9, DESC_MAGIC = 0xabcd5432;
const TYPE_APP = 0, TYPE_DATA = 1, SUB_OTADATA = 0, SUB_NVS = 2, SUB_OTA_0 = 0x10, SUB_OTA_END = 0x20;

const u32 = (b, at) => (b[at] | (b[at + 1] << 8) | (b[at + 2] << 16) | (b[at + 3] << 24)) >>> 0;
function cstr(b, at, len) {
  let s = "";
  for (let i = at; i < at + len && b[i]; i++) s += String.fromCharCode(b[i]);
  return s;
}

/* The entries of a partition table: 32 bytes each, AA 50 first; an MD5 line (EB EB) or blank flash ends them. */
function parseTable(bytes) {
  const entries = [];
  for (let at = 0; at + 32 <= bytes.length && bytes[at] === 0xaa && bytes[at + 1] === 0x50; at += 32)
    entries.push({ type: bytes[at + 2], subtype: bytes[at + 3], offset: u32(bytes, at + 4), size: u32(bytes, at + 8) });
  return entries;
}
const isOta = (e) => e.type === TYPE_APP && e.subtype >= SUB_OTA_0 && e.subtype < SUB_OTA_END;
const otaSlots = (entries) => entries.filter(isOta).sort((a, b) => a.subtype - b.subtype);
const same = (a, b) => a.type === b.type && a.subtype === b.subtype && a.offset === b.offset && a.size === b.size;

/* The image's own map: its table, the boot record and the first app slot. Throws on anything but a full image. */
function mapOf(full) {
  const notFull = () => new Error("this isn't a full image. Pick the file whose name ends in -full.bin.");
  if (full.length < BOOT_END || full[BOOT_AT] !== IMAGE_MAGIC) throw notFull();
  const table = parseTable(full.subarray(TABLE_AT, BOOT_END));
  const otadata = table.find((e) => e.type === TYPE_DATA && e.subtype === SUB_OTADATA);
  const app = otaSlots(table)[0];
  if (!otadata || !app || !table.some((e) => e.type === TYPE_DATA && e.subtype === SUB_NVS)) throw notFull();
  if (full.length < app.offset + 256 || full[app.offset] !== IMAGE_MAGIC) throw notFull();
  return { table, otadata, app };
}

export function checkFullImage(full) { mapOf(full); }

/* An app's name and version, from the descriptor that follows its image and segment headers. null: no app there. */
function appInfo(header) {
  if (header.length < 0x70 || header[0] !== IMAGE_MAGIC || u32(header, 0x20) !== DESC_MAGIC) return null;
  return { version: cstr(header, 0x30, 32), project: cstr(header, 0x50, 32) };
}

export function imageInfo(full) {
  const { app } = mapOf(full);
  return appInfo(full.subarray(app.offset, app.offset + 256));
}

function crc32(bytes, init) {
  let c = ~init >>> 0;
  for (const b of bytes) {
    c ^= b;
    for (let k = 0; k < 8; k++) c = c & 1 ? (c >>> 1) ^ 0xedb88320 : c >>> 1;
  }
  return ~c >>> 0;
}

/* The slot the bootloader starts: the boot record keeps two copies (records: the first 32 bytes of each); the valid
   one with the higher sequence wins, and with none the first slot runs (there is no factory app). A copy is valid
   when its checksum holds and its state isn't invalid (3) or aborted (4), the marks of a rolled-back update. */
function runningSlot(records, slots) {
  let best = 0;
  for (const r of records) {
    if (r.length < 32) continue;
    const seq = u32(r, 0), state = u32(r, 24);
    if (seq === 0xffffffff || state === 3 || state === 4) continue;
    if (crc32(r.subarray(0, 4), 0xffffffff) !== u32(r, 28)) continue;
    if (seq > best) best = seq;
  }
  return best ? (best - 1) % slots : 0;
}

/* Look at a board before writing. io.md5(address, size) is the flasher's MD5 (lowercase hex) of a stretch of the
   board's flash; io.read(address, size) returns up to 256 bytes of it and may throw.

   What decides whether an update is safe comes from checksums alone, the command every write is already verified
   with: the board's partition table must begin with the image's entries up to the first app slot (the settings, the
   boot record and where the app goes; what follows may differ, an older table has a smaller core dump partition,
   and an update brings it up to date). Reading is only for what the page tells the user and for refusing another
   project's app: a board whose flash can't be read is still updated (readFailed says the reads failed).

   table: "same", "other" or "blank". bootSame: its bootloader and partition table are the image's.
   slot, app: the slot it starts and the app in it ({ version, project }); app null: none there, or not read. */
export async function readBoard(full, io) {
  const { table, otadata, app } = mapOf(full);
  const lead = full.subarray(TABLE_AT, TABLE_AT + 32 * (table.indexOf(app) + 1));
  const board = { table: "other", bootSame: false, slot: null, app: null, readFailed: false };
  if ((await io.md5(TABLE_AT, lead.length)) !== md5(lead)) {
    if ((await io.md5(TABLE_AT, 32)) === md5(new Uint8Array(32).fill(0xff))) board.table = "blank";
    return board;
  }
  board.table = "same";
  board.bootSame = (await io.md5(BOOT_AT, BOOT_END - BOOT_AT)) === md5(full.subarray(BOOT_AT, BOOT_END));
  try {
    const slots = otaSlots(table);
    const records = [await io.read(otadata.offset, 32), await io.read(otadata.offset + SECTOR, 32)];
    const slot = runningSlot(records, slots.length);
    board.app = appInfo(await io.read(slots[slot].offset, 256));
    board.slot = slot;
  } catch {
    board.readFailed = true;
  }
  return board;
}

export function planFresh(full) {
  checkFullImage(full);
  return [{ name: "the firmware", address: 0, data: full }];
}

/* { ok: true, parts } or { ok: false, why }: "blank" (nothing on the board), "layout" (another partition layout:
   its settings area isn't this firmware's), "foreign" (another project's app runs there). A board with the right
   layout and no readable app is updated: that is what a flash that stopped halfway leaves behind. */
export function planUpdate(full, board) {
  const { otadata, app } = mapOf(full);
  if (board.table === "blank") return { ok: false, why: "blank" };
  if (board.table !== "same") return { ok: false, why: "layout" };
  if (board.app && board.app.project !== imageInfo(full).project) return { ok: false, why: "foreign" };
  const parts = [
    { name: "the firmware", address: app.offset, data: full.subarray(app.offset) },
    { name: "the boot record", address: otadata.offset, data: full.subarray(otadata.offset, otadata.offset + otadata.size) },
  ];
  /* last, and only when it changed: a stop before this point leaves a box that still starts */
  if (!board.bootSame) parts.push({ name: "the bootloader", address: BOOT_AT, data: full.subarray(BOOT_AT, BOOT_END) });
  return { ok: true, parts };
}
