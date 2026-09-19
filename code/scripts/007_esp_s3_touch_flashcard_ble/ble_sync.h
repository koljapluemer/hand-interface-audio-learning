// BLE sync server (Nordic UART Service) that sync.html talks to.
//
// BLE contract (must match sync.html):
//   Device name : "Flashcards"
//   Service     : 6E400001-B5A3-F393-E0A9-E50E24DCCA9E   (Nordic UART Service)
//   RX (write)  : 6E400002-...   app -> device
//   TX (notify) : 6E400003-...   device -> app
//
// A single attribute value is capped at 512 bytes and the negotiated MTU can
// be much smaller than a card, so RX/TX carry a tiny framed protocol:
//
//   LIST (app -> device): single byte 0x10. Device replies with one notify
//     [0x10, len:u32 LE] then chunks (BLE_CHUNK bytes each) of the
//     concatenated per-card records [id:u32 LE][box:u16 LE][practiced:u8]
//     [frontLen:u16 LE][front][backLen:u16 LE][back] -- no bitmaps
//     (sync.html always re-renders those from text before any upload).
//
//   PUT_CARD (app -> device): one write [0x11, id:u32 LE, box:u16 LE,
//     practiced:u8, bodyLen:u32 LE] (id 0 = "assign me a new id"), then as
//     many writes as needed carrying raw body bytes (chunked at BLE_CHUNK):
//     [frontLen:u16 LE][front][backLen:u16 LE][back][frontBitmap][backBitmap]
//     until bodyLen bytes were sent. Streamed straight to /cards/<id>.bin.tmp
//     (never a whole card in RAM), atomically renamed on success.
//   PUT_CARD response: one notify [0x11, status, assignedId:u32 LE].
//     status 0 = saved OK, 1 = error (no SD / write failed).
//
//   DELETE_CARD (app -> device): one write [0x12, id:u32 LE]; reply [0x12, status].
//   DELETE_ALL  (app -> device): single byte 0x13; reply [0x13, status, deleted:u32 LE].
//
// Versioned fast PUT (when BLE_FAST_SYNC is enabled):
//   BEGIN 0x20 (acknowledged write): transferId, requested card id, metadata,
//     total body length and CRC32. Device replies with status + next offset.
//   DATA 0x21 (write without response): transferId, absolute byte offset and
//     payload. Four-frame bounded windows receive a cumulative next-offset
//     notification; duplicates are idempotent and gaps report the last
//     contiguous offset.
//   STATUS 0x22 (acknowledged write): recovers progress after a lost window
//     acknowledgment.
//   COMMIT 0x23 (acknowledged write): verifies exact length, record shape and
//     whole-body CRC before the existing atomic temp-file rename. The response
//     contains the committed card id. sync.html falls back to PUT_CARD if a
//     connected device does not answer BEGIN.
#pragma once

void bleSyncStart();   // init BLE, advertise; never torn down (leave via restart)
void bleSyncTick();    // call from loop() while in sync mode: runs deferred work
