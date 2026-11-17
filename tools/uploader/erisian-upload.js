// SPDX-License-Identifier: MIT
// ERISIAN sample upload protocol (see app/storage/upload.h). Used by the
// uploader page and by tests/js/gen_frames.mjs.

export const PROTOCOL = 1;
export const CHUNK = 512;
export const MAX_BYTES = 96 * 1024;
export const CMD = { HELLO: 0x01, BEGIN: 0x10, DATA: 0x11, END: 0x12, ABORT: 0x13, ACK: 0x7f };
export const STATUS = ["ok", "bad frame", "unexpected command", "bad argument",
  "offset gap", "flash full", "flash error or unknown flash chip", "CRC mismatch"];

export function pack7(bytes) {
  const out = [];
  for (let i = 0; i < bytes.length; i += 7) {
    const group = bytes.subarray(i, i + 7);
    let high = 0;
    group.forEach((b, k) => { high |= (b >> 7) << (6 - k); });
    out.push(high, ...Array.from(group, (b) => b & 0x7f));
  }
  return Uint8Array.from(out);
}

export function unpack7(bytes) {
  const out = [];
  for (let i = 0; i < bytes.length; i += 8) {
    const high = bytes[i];
    for (let k = 0; k < 7 && i + 1 + k < bytes.length; k++)
      out.push(bytes[i + 1 + k] | (((high >> (6 - k)) & 1) << 7));
  }
  return Uint8Array.from(out);
}

const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();

export function crc32(bytes, crc = 0) {
  crc = ~crc >>> 0;
  for (const b of bytes) crc = CRC_TABLE[(crc ^ b) & 0xff] ^ (crc >>> 8);
  return ~crc >>> 0;
}

function u32(v) { return [v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff]; }

export function frame(cmd, payload = new Uint8Array(0)) {
  return Uint8Array.from([0xf0, 0x7d, 0x45, 0x52, cmd, ...pack7(payload), 0xf7]);
}

// Parse a RIFF WAV. Returns { rate, channels, bits, pcm: Int16Array } for
// 16-bit PCM, or throws with a message a person can act on.
export function parseWav(buffer) {
  const v = new DataView(buffer);
  const tag = (o) => String.fromCharCode(v.getUint8(o), v.getUint8(o + 1), v.getUint8(o + 2), v.getUint8(o + 3));
  if (buffer.byteLength < 12 || tag(0) !== "RIFF" || tag(8) !== "WAVE")
    throw new Error("not a WAV file");
  let fmt = null, data = null;
  for (let o = 12; o + 8 <= buffer.byteLength;) {
    const id = tag(o), size = v.getUint32(o + 4, true);
    if (id === "fmt ") fmt = { format: v.getUint16(o + 8, true), channels: v.getUint16(o + 10, true),
      rate: v.getUint32(o + 12, true), bits: v.getUint16(o + 22, true) };
    if (id === "data") data = { offset: o + 8, size: Math.min(size, buffer.byteLength - o - 8) };
    o += 8 + size + (size & 1);
  }
  if (!fmt || !data) throw new Error("WAV has no fmt or data chunk");
  if (fmt.format !== 1 || fmt.bits !== 16) throw new Error(`WAV must be 16-bit PCM (this one: format ${fmt.format}, ${fmt.bits}-bit)`);
  if (fmt.channels !== 1) throw new Error(`WAV must be mono (this one has ${fmt.channels} channels)`);
  if (fmt.rate !== 44100) throw new Error(`WAV must be 44.1 kHz (this one: ${fmt.rate} Hz)`);
  const frames = Math.floor(data.size / 2);
  const pcm = new Int16Array(frames);
  for (let i = 0; i < frames; i++) pcm[i] = v.getInt16(data.offset + 2 * i, true);
  return { rate: fmt.rate, channels: fmt.channels, bits: fmt.bits, pcm };
}

// Stored payload: 16-byte sample header + 16-bit LE PCM.
export function payload(pcm) {
  const out = new Uint8Array(16 + pcm.length * 2);
  const v = new DataView(out.buffer);
  v.setUint32(0, 0x4b535245, true); // "ERSK"
  v.setUint16(4, 1, true);
  v.setUint16(6, 1, true);
  v.setUint32(8, 44100, true);
  v.setUint32(12, pcm.length, true);
  for (let i = 0; i < pcm.length; i++) v.setInt16(16 + 2 * i, pcm[i], true);
  if (out.length > MAX_BYTES) throw new Error(`sample too long: ${out.length} bytes, at most ${MAX_BYTES}`);
  return out;
}

export function beginFrame(slot, data) {
  return frame(CMD.BEGIN, Uint8Array.from([slot, ...u32(data.length), ...u32(crc32(data))]));
}

export function dataFrame(data, offset) {
  return frame(CMD.DATA, Uint8Array.from([...u32(offset), ...data.subarray(offset, offset + CHUNK)]));
}

// Decode an ACK frame; null if it is not one of ours.
export function parseAck(bytes) {
  if (bytes.length < 6 || bytes[0] !== 0xf0 || bytes[1] !== 0x7d || bytes[2] !== 0x45 ||
      bytes[3] !== 0x52 || bytes[4] !== CMD.ACK) return null;
  const raw = unpack7(bytes.subarray(5, bytes.length - 1));
  if (raw.length < 6) return null;
  return { cmd: raw[0], status: raw[1], value: (raw[2] | raw[3] << 8 | raw[4] << 16 | raw[5] << 24) >>> 0 };
}
