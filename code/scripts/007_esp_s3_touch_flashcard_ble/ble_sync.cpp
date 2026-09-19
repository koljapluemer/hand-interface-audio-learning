#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <cstring>
#include "SD_MMC.h"
#include "config.h"
#include "card_store.h"
#include "ble_sync.h"

static BLEServer *pServer = nullptr;
static BLECharacteristic *pTxCharacteristic = nullptr;
static volatile bool bleConnected = false;
// Set by TxCallbacks::onStatus() whenever an indicate() fails to queue (stack
// congested) or times out waiting for the peer's confirmation --
// sendChunkReliably() polls it to back off and retry only when needed.
static volatile bool notifyCongested = false;

// PUT_CARD bodies (and LIST responses) are streamed to/from the SD card in
// BLE_CHUNK-sized pieces rather than buffered whole in RAM -- heap pressure
// from whole-deck buffering once caused a reboot right after a large save.
enum RxState { RX_IDLE, RX_BODY };
static RxState rxState = RX_IDLE;
static size_t rxExpected = 0;
static size_t rxReceived = 0;
static File putFile;         // open across a PUT_CARD's onWrite() calls
static bool putOk = false;   // false if sdOk was false, open failed, or a write came up short
static uint32_t putCardId = 0;
static int      putCardBox = 0;
static bool     putCardPracticed = false;
static uint32_t pendingDeleteId = 0;

// RxCallbacks::onWrite() runs on the NimBLE host task with a small stack.
// Calling back into the BLE stack from there (e.g. a retry loop of indicate()
// calls to send a file) overflows it -- confirmed here: a reliable crash right
// after connect, regardless of chunk size or connection params, with no
// backtrace because the overflow corrupts the stack the panic handler would
// unwind. So onWrite() only sets one of these flags (or streams raw PUT_CARD
// bytes to an already-open File, which doesn't touch the BLE stack); the real
// send/finish/delete work happens in bleSyncTick() on the main Arduino task.
enum PendingBleAction {
  PENDING_NONE, PENDING_SEND_LIST, PENDING_FINISH_PUT_CARD,
  PENDING_DELETE_CARD, PENDING_DELETE_ALL
};
static volatile PendingBleAction pendingAction = PENDING_NONE;

static inline size_t minSize(size_t a, size_t b) { return a < b ? a : b; }

// Sends one payload via indicate() rather than notify(): notify() is
// fire-and-forget at the ATT level, so a silently dropped packet permanently
// truncates the transfer. indicate() waits for the peer's confirmation (up to
// 1000 ms), so a drop surfaces here as a retryable failure. Also retries with
// a short backoff if the local send queue is full, and yields a little even on
// the fast path. Returns false if the client disconnected mid-retry.
static bool sendChunkReliably(const uint8_t *data, size_t n) {
  for (;;) {
    if (!bleConnected) return false;
    notifyCongested = false;
    pTxCharacteristic->setValue(data, n);
    pTxCharacteristic->indicate();
    if (notifyCongested) {
      delay(15);   // out of send buffers, or peer didn't confirm in time --
      continue;    // back off and retry this same chunk
    }
    delay(2);
    return true;
  }
}

static void putU32(uint8_t *out, uint32_t v) {
  out[0] = (uint8_t)v;         out[1] = (uint8_t)(v >> 8);
  out[2] = (uint8_t)(v >> 16); out[3] = (uint8_t)(v >> 24);
}

// LIST: streams id+box+practiced+front+back text for every loaded card,
// batched into BLE_CHUNK-sized sends.
static void sendListOverBle() {
  size_t total = 0;
  for (auto &c : cards) total += 4 + 2 + 1 + 2 + c.frontLen + 2 + c.backLen;

  uint8_t header[5];
  header[0] = 0x10;
  putU32(header + 1, (uint32_t)total);
  if (!sendChunkReliably(header, sizeof(header))) {
    Serial.println("BLE: LIST aborted, client disconnected");
    return;
  }

  static uint8_t chunkBuf[BLE_CHUNK];   // reused across calls; never on the stack
  size_t chunkLen = 0;
  auto flushChunk = [&]() -> bool {
    if (chunkLen == 0) return true;
    bool ok = sendChunkReliably(chunkBuf, chunkLen);
    chunkLen = 0;
    return ok;
  };
  auto feed = [&](const uint8_t *data, size_t n) -> bool {
    while (n > 0) {
      size_t take = minSize(BLE_CHUNK - chunkLen, n);
      memcpy(chunkBuf + chunkLen, data, take);
      chunkLen += take; data += take; n -= take;
      if (chunkLen == BLE_CHUNK && !flushChunk()) return false;
    }
    return true;
  };

  size_t sent = 0;
  static uint8_t textBuf[128];
  for (auto &c : cards) {
    char path[24];
    cardFilePath(c.id, path, sizeof(path));
    File f = SD_MMC.open(path, FILE_READ);
    if (!f) { Serial.printf("BLE: LIST skip %08X, open failed\n", (unsigned)c.id); continue; }

    uint8_t rec[9];
    putU32(rec, c.id);
    rec[4] = (uint8_t)(c.box);      rec[5] = (uint8_t)(c.box >> 8);
    rec[6] = c.practiced ? 1 : 0;
    rec[7] = (uint8_t)(c.frontLen); rec[8] = (uint8_t)(c.frontLen >> 8);
    if (!feed(rec, sizeof(rec))) { f.close(); flushChunk(); return; }

    f.seek(CARD_HEADER_BYTES + 2);   // front text starts right after its length prefix
    uint16_t remaining = c.frontLen;
    bool ok = true;
    while (ok && remaining > 0) {
      size_t n = minSize(sizeof(textBuf), remaining);
      ok = (f.read(textBuf, n) == n) && feed(textBuf, n);
      remaining -= n;
    }
    if (!ok) { f.close(); flushChunk(); return; }

    uint8_t backLenBytes[2] = { (uint8_t)(c.backLen), (uint8_t)(c.backLen >> 8) };
    if (!feed(backLenBytes, sizeof(backLenBytes))) { f.close(); flushChunk(); return; }
    f.seek(CARD_HEADER_BYTES + 2 + c.frontLen + 2);   // back text
    remaining = c.backLen;
    while (ok && remaining > 0) {
      size_t n = minSize(sizeof(textBuf), remaining);
      ok = (f.read(textBuf, n) == n) && feed(textBuf, n);
      remaining -= n;
    }
    f.close();
    if (!ok) { flushChunk(); return; }

    sent += 4 + 2 + 1 + 2 + c.frontLen + 2 + c.backLen;
  }
  flushChunk();
  Serial.printf("BLE: sent LIST, %u bytes / %u card(s)\n", (unsigned)sent, (unsigned)cards.size());
}

static void finishPutCard() {
  if (putFile) putFile.close();

  char path[24], tmpPath[28];
  cardFilePath(putCardId, path, sizeof(path));
  cardTmpPath(putCardId, tmpPath, sizeof(tmpPath));
  if (putOk) {
    SD_MMC.remove(path);
    putOk = SD_MMC.rename(tmpPath, path);
  }
  if (!putOk) SD_MMC.remove(tmpPath);

  Serial.printf("BLE: wrote card %08X (%u bytes) -> %s\n",
                (unsigned)putCardId, (unsigned)rxReceived, putOk ? "ok" : "FAILED");

  if (putOk) {
    uint16_t frontLen = 0, backLen = 0;
    readCardTextLens(putCardId, frontLen, backLen);
    upsertCardInMemory(putCardId, putCardBox, putCardPracticed, frontLen, backLen);
  }

  uint8_t resp[6];
  resp[0] = 0x11;
  resp[1] = putOk ? 0 : 1;
  putU32(resp + 2, putCardId);
  sendChunkReliably(resp, sizeof(resp));

  rxState = RX_IDLE;
}

static void deleteCardOnDevice(uint32_t id) {
  bool ok = removeCard(id);
  Serial.printf("BLE: delete card %08X -> %s\n", (unsigned)id, ok ? "ok" : "FAILED");
  uint8_t resp[2] = { 0x12, (uint8_t)(ok ? 0 : 1) };
  sendChunkReliably(resp, sizeof(resp));
}

static void deleteAllCardsOnDevice() {
  uint32_t deleted = removeAllCards();
  Serial.printf("BLE: delete all -> %u file(s) removed\n", (unsigned)deleted);

  uint8_t resp[6];
  resp[0] = 0x13; resp[1] = 0;
  putU32(resp + 2, deleted);
  sendChunkReliably(resp, sizeof(resp));
}

// indicate() reports its outcome via onStatus(): SUCCESS_INDICATE means
// confirmed delivery, ERROR_GATT a full local send queue, ERROR_INDICATE_TIMEOUT
// a peer that never confirmed -- anything but success means "back off and retry".
class TxCallbacks : public BLECharacteristicCallbacks {
  void onStatus(BLECharacteristic *pCharacteristic, Status s, uint32_t code) override {
    if (s != SUCCESS_NOTIFY && s != SUCCESS_INDICATE) notifyCongested = true;
  }
};

class RxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) override {
    uint8_t *data = pCharacteristic->getData();
    size_t len = pCharacteristic->getLength();
    if (len == 0) return;

    if (rxState == RX_IDLE) {
      if (data[0] == 0x10) {                        // LIST
        pendingAction = PENDING_SEND_LIST;
      } else if (data[0] == 0x11 && len >= 12) {    // PUT_CARD header
        uint32_t reqId = (uint32_t)data[1] | ((uint32_t)data[2] << 8) |
                          ((uint32_t)data[3] << 16) | ((uint32_t)data[4] << 24);
        putCardBox = data[5] | (data[6] << 8);
        putCardPracticed = data[7] != 0;
        uint32_t bodyLen = (uint32_t)data[8] | ((uint32_t)data[9] << 8) |
                            ((uint32_t)data[10] << 16) | ((uint32_t)data[11] << 24);
        putCardId = (reqId == 0) ? nextId++ : reqId;
        rxExpected = bodyLen;
        rxReceived = 0;
        putOk = sdOk;
        if (putOk) {
          char tmpPath[28];
          cardTmpPath(putCardId, tmpPath, sizeof(tmpPath));
          SD_MMC.remove(tmpPath);
          putFile = SD_MMC.open(tmpPath, FILE_WRITE);
          if (!putFile) {
            putOk = false;
          } else {
            uint8_t hdr[CARD_HEADER_BYTES];
            encodeCardHeader(hdr, putCardId, putCardBox, putCardPracticed);
            putOk = (putFile.write(hdr, CARD_HEADER_BYTES) == CARD_HEADER_BYTES);
          }
        }
        if (bodyLen == 0) pendingAction = PENDING_FINISH_PUT_CARD;
        else rxState = RX_BODY;
      } else if (data[0] == 0x12 && len >= 5) {     // DELETE_CARD
        pendingDeleteId = (uint32_t)data[1] | ((uint32_t)data[2] << 8) |
                           ((uint32_t)data[3] << 16) | ((uint32_t)data[4] << 24);
        pendingAction = PENDING_DELETE_CARD;
      } else if (data[0] == 0x13) {                 // DELETE_ALL
        pendingAction = PENDING_DELETE_ALL;
      }
    } else {   // RX_BODY: raw body bytes, no framing -- streamed straight to
               // the temp file rather than buffered in RAM.
      size_t n = minSize(len, rxExpected - rxReceived);
      if (putOk && n > 0 && putFile.write(data, n) != n) putOk = false;
      rxReceived += n;
      if (rxReceived >= rxExpected) pendingAction = PENDING_FINISH_PUT_CARD;
    }
  }
};

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *pServer) override {
    bleConnected = true;
    rxState = RX_IDLE;
    if (putFile) putFile.close();   // in case a prior connection dropped mid-PUT
    Serial.println("BLE: central connected");
    // Deliberately no requestConnParams() here: calling into the GAP API from
    // within a BLE callback is the same class of risk as sending from onWrite().
    // If revisited, defer it to bleSyncTick().
  }
  void onDisconnect(BLEServer *pServer) override {
    bleConnected = false;
    Serial.println("BLE: central disconnected, re-advertising");
    pServer->startAdvertising();
  }
};

void bleSyncStart() {
  Serial.println("entering BLE sync mode");
  BLEDevice::init(DEVICE_NAME);
  BLEDevice::setMTU(247);

  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  BLEService *service = pServer->createService(NUS_SERVICE_UUID);

  // INDICATE alongside NOTIFY: sendChunkReliably() uses indicate() so every
  // chunk gets a real delivery confirmation.
  pTxCharacteristic = service->createCharacteristic(
    NUS_TX_UUID, BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_INDICATE);
  pTxCharacteristic->addDescriptor(new BLE2902());
  pTxCharacteristic->setCallbacks(new TxCallbacks());

  // Plain acknowledged WRITE: write-without-response was faster but silently
  // dropped data mid-upload (a save "completed" client-side, the device never
  // got the full body). Reliability matters more than the extra speed.
  BLECharacteristic *rxChar = service->createCharacteristic(NUS_RX_UUID, BLECharacteristic::PROPERTY_WRITE);
  rxChar->setCallbacks(new RxCallbacks());

  service->start();

  BLEAdvertising *adv = pServer->getAdvertising();
  adv->addServiceUUID(NUS_SERVICE_UUID);
  adv->start();
  Serial.println("BLE advertising as \"" DEVICE_NAME "\"");
}

void bleSyncTick() {
  // Snapshot-and-clear before dispatching in case the action itself takes a
  // while and bleConnected/rxState change during it.
  PendingBleAction action = pendingAction;
  pendingAction = PENDING_NONE;
  switch (action) {
    case PENDING_SEND_LIST:       sendListOverBle(); break;
    case PENDING_FINISH_PUT_CARD: finishPutCard(); break;
    case PENDING_DELETE_CARD:     deleteCardOnDevice(pendingDeleteId); break;
    case PENDING_DELETE_ALL:      deleteAllCardsOnDevice(); break;
    case PENDING_NONE: break;
  }
}
