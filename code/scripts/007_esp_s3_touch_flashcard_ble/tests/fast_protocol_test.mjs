import assert from 'node:assert/strict';

const PAYLOAD = 171;

function crc32(bytes) {
  let crc = 0xffffffff;
  for (const byte of bytes) {
    crc ^= byte;
    for (let bit = 0; bit < 8; bit++) crc = (crc >>> 1) ^ (crc & 1 ? 0xedb88320 : 0);
  }
  return (~crc) >>> 0;
}

class ReceiverModel {
  constructor(body) {
    this.expectedLength = body.length;
    this.expectedCrc = crc32(body);
    this.received = 0;
    this.buffer = new Uint8Array(body.length);
  }

  data(offset, payload) {
    if (offset === this.received && payload.length <= this.expectedLength - this.received) {
      this.buffer.set(payload, offset);
      this.received += payload.length;
      return { status: 0, next: this.received };
    }
    if (offset < this.received && offset + payload.length <= this.received) {
      return { status: 0, next: this.received };
    }
    return { status: 2, next: this.received };
  }

  commit() {
    return this.received === this.expectedLength && crc32(this.buffer) === this.expectedCrc;
  }
}

function frames(body) {
  const out = [];
  for (let offset = 0; offset < body.length; offset += PAYLOAD) {
    out.push({ offset, payload: body.slice(offset, Math.min(offset + PAYLOAD, body.length)) });
  }
  return out;
}

const body = Uint8Array.from({ length: 4619 }, (_, i) => (i * 31 + 7) & 0xff);
assert.equal(crc32(new TextEncoder().encode('123456789')), 0xcbf43926, 'standard CRC-32 vector');

{
  const rx = new ReceiverModel(body);
  for (const frame of frames(body)) assert.equal(rx.data(frame.offset, frame.payload).status, 0);
  assert.equal(rx.received, body.length);
  assert.equal(rx.commit(), true, 'complete ordered transfer commits');
}

{
  const rx = new ReceiverModel(body);
  const all = frames(body);
  rx.data(all[0].offset, all[0].payload);
  assert.equal(rx.data(all[0].offset, all[0].payload).next, PAYLOAD, 'duplicate is idempotent');
  assert.deepEqual(rx.data(all[2].offset, all[2].payload), { status: 2, next: PAYLOAD }, 'gap reports contiguous offset');
  for (const frame of all.slice(1)) rx.data(frame.offset, frame.payload);
  assert.equal(rx.commit(), true, 'retry from cumulative offset recovers a dropped frame');
}

{
  const rx = new ReceiverModel(body);
  const corrupted = body.slice();
  corrupted[2000] ^= 0x80;
  for (const frame of frames(corrupted)) rx.data(frame.offset, frame.payload);
  assert.equal(rx.commit(), false, 'whole-body CRC rejects corruption');
}

{
  const rx = new ReceiverModel(body);
  for (const frame of frames(body).slice(0, 5)) rx.data(frame.offset, frame.payload);
  assert.equal(rx.commit(), false, 'partial transfer cannot commit');
}

console.log('fast protocol model: all tests passed');
