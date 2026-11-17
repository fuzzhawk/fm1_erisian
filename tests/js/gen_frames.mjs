// Writes the frame stream the uploader page would send for a synthetic kick,
// so tests/test_upload.c can check that the firmware side accepts it.
import { writeFileSync } from "node:fs";
import * as up from "../../tools/uploader/erisian-upload.js";

const out = process.argv[2];
const n = 40000;
const pcm = new Int16Array(n);
for (let i = 0; i < n; i++)
  pcm[i] = Math.round(30000 * Math.exp(-i / 6000) * Math.sin(2 * Math.PI * 55 * i / 44100 * (1 + 3 * Math.exp(-i / 800))));

// pack7/unpack7 round trip on awkward lengths
for (const len of [0, 1, 6, 7, 8, 13, 14, 15, 512, 516]) {
  const b = Uint8Array.from({ length: len }, (_, i) => (i * 97 + 200) & 0xff);
  const r = up.unpack7(up.pack7(b));
  if (r.length !== len || r.some((x, i) => x !== b[i])) throw new Error(`pack7 round trip failed at ${len}`);
}
if (up.crc32(new TextEncoder().encode("123456789")) !== 0xcbf43926) throw new Error("crc32");

// WAV parser on a generated file
const wav = new ArrayBuffer(44 + n * 2);
const v = new DataView(wav);
const put = (o, s) => [...s].forEach((c, i) => v.setUint8(o + i, c.charCodeAt(0)));
put(0, "RIFF"); v.setUint32(4, 36 + n * 2, true); put(8, "WAVE"); put(12, "fmt ");
v.setUint32(16, 16, true); v.setUint16(20, 1, true); v.setUint16(22, 1, true);
v.setUint32(24, 44100, true); v.setUint32(28, 88200, true); v.setUint16(32, 2, true);
v.setUint16(34, 16, true); put(36, "data"); v.setUint32(40, n * 2, true);
pcm.forEach((s, i) => v.setInt16(44 + 2 * i, s, true));
const parsed = up.parseWav(wav);
if (parsed.pcm.length !== n || parsed.pcm[1234] !== pcm[1234]) throw new Error("parseWav");

const data = up.payload(parsed.pcm);
const frames = [up.frame(up.CMD.HELLO), up.beginFrame(0, data)];
for (let o = 0; o < data.length; o += up.CHUNK) frames.push(up.dataFrame(data, o));
frames.push(up.frame(up.CMD.END));
for (const f of frames) if (f.length > 1024) throw new Error("frame too long for the FM-1 SysEx hook");

// File: u32 LE frame count, then per frame u16 LE length + bytes; then payload.
const parts = [Uint8Array.from([frames.length & 0xff, frames.length >> 8 & 0xff, 0, 0])];
for (const f of frames) parts.push(Uint8Array.from([f.length & 0xff, f.length >> 8]), f);
parts.push(data);
writeFileSync(out, Buffer.concat(parts.map((p) => Buffer.from(p))));
console.log(`js: ${frames.length} frames, payload ${data.length} bytes, crc ${up.crc32(data).toString(16)}`);
