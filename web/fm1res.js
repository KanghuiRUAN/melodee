// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// The resource region (firmware/src/res.c): Rubik, the factory sounds and the radio program, written after an
// update through editor command 82. The file is build/melodee-res.bin: a 32-byte header, then the payload.
import { bkPack } from "./fm1backup.js";

export const RES_COMMAND = 82;
const r35 = (a, at) => (a[at] | a[at + 1] << 7 | a[at + 2] << 14 | a[at + 3] << 21 | a[at + 4] << 28) >>> 0;

export function resStatus(reply) {
  if (!reply || reply.length !== 12) throw new Error("This firmware has no resource command");
  return { rc: reply[0], installed: !!reply[1], length: r35(reply, 2), crc: r35(reply, 7) };
}

// request([cmd, args], {timeout}) -> reply bytes (fm1backup.js BackupConnection). Resolves "current" when the
// device already has these resources, "written" after a verified write.
export async function installResources(request, region, onProgress = () => {}) {
  const head = new DataView(region.buffer, region.byteOffset, 32);
  if (head.getUint32(0, true) !== 0x3153524d) throw new Error("Not a Melodee resource file");
  const length = head.getUint32(4, true), crc = head.getUint32(8, true), payload = region.subarray(32, 32 + length);
  const ask = async (args, timeout = 3000) => resStatus(await request([RES_COMMAND, args], { timeout }));
  const now = await ask([0]);
  if (now.installed && now.crc === crc) return "current";
  if (now.length !== length || now.crc !== crc) throw new Error("These resources belong to another firmware build");
  const begin = await ask([1], 30000);                 /* old project copies moved out, 14 sectors erased */
  if (begin.rc === 4) throw new Error("Stop playback and switch Bluetooth off, then retry");
  if (begin.rc) throw new Error(`Resource write refused (${begin.rc})`);
  for (let off = 0; off < length; off += 256) {
    const r = await ask([2, off & 127, off >> 7 & 127, off >> 14 & 127, ...bkPack(payload.subarray(off, off + 256))]);
    if (r.rc) throw new Error(`Resource write failed at ${off} (${r.rc})`);
    onProgress(Math.min(length, off + 256), length);
  }
  const end = await ask([3], 10000);
  if (end.rc || !end.installed) throw new Error(`Resource check failed (${end.rc})`);
  return "written";
}
