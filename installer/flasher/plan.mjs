// What the installer writes, and where. No DOM and no serial port in here: tools/test_installer.mjs runs it in Node.
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

/* The slot the bootloader starts: the boot record keeps two copies, a sector each; the valid one with the higher
   sequence wins, and with none the first slot runs (there is no factory app). A copy is valid when its checksum
   holds and its state isn't invalid (3) or aborted (4), the marks of a rolled-back update. */
function runningSlot(otadata, slots) {
  let best = 0;
  for (let at = 0; at + 32 <= otadata.length && at < 2 * SECTOR; at += SECTOR) {
    const seq = u32(otadata, at), state = u32(otadata, at + 24);
    if (seq === 0xffffffff || state === 3 || state === 4) continue;
    if (crc32(otadata.subarray(at, at + 4), 0xffffffff) !== u32(otadata, at + 28)) continue;
    if (seq > best) best = seq;
  }
  return best ? (best - 1) % slots : 0;
}

/* Look at a board before writing: read(address, size) returns that much of its flash.
   table: "same" (laid out like the image, so its settings are where this firmware keeps them), "other" or "blank".
   slot, app: the slot it starts and the app in it ({ version, project }), or null when no app is there. */
export async function readBoard(full, read) {
  const { table, app } = mapOf(full);
  const boot = await read(BOOT_AT, BOOT_END - BOOT_AT);
  const theirs = parseTable(boot.subarray(TABLE_AT - BOOT_AT));
  const board = { boot, table: "blank", slot: null, app: null };
  if (!theirs.length) return board;
  /* the same up to the first app slot: the settings, the boot record and where the app goes. What follows may
     differ (an older table has a smaller core dump partition) and an update brings it up to date. */
  const needed = table.filter((e) => e.offset <= app.offset);
  board.table = needed.every((e) => theirs.some((t) => same(t, e))) ? "same" : "other";
  if (board.table !== "same") return board;
  const od = theirs.find((e) => e.type === TYPE_DATA && e.subtype === SUB_OTADATA);
  const slots = otaSlots(theirs);
  board.slot = runningSlot(await read(od.offset, od.size), slots.length);
  board.app = appInfo(await read(slots[board.slot].offset, 256));
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
  const boot = full.subarray(BOOT_AT, BOOT_END);
  if (boot.length !== board.boot.length || boot.some((b, i) => b !== board.boot[i]))
    parts.push({ name: "the bootloader", address: BOOT_AT, data: boot });
  return { ok: true, parts };
}
