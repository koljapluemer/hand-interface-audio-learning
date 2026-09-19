#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <cstring>
#include "esp_rom_crc.h"
#include "SD_MMC.h"
#include "config.h"
#include "card_store.h"
#include "ble_sync.h"

static BLEServer *pServer = nullptr;
static BLECharacteristic *pTxCharacteristic = nullptr;
static volatile bool bleConnected = false;
static volatile bool connParamRequestPending = false;
static uint16_t bleConnHandle = 0;
// Set by TxCallbacks::onStatus() whenever an indicate() fails to queue (stack
// congested) or times out waiting for the peer's confirmation --
// sendChunkReliably() polls it to back off and retry only when needed.
static volatile bool notifyCongested = false;

// Buffer one PUT_CARD body in RAM, then write it to SD in one operation after
// all BLE chunks have arrived. Writing every 180-byte chunk synchronously from
// onWrite() delays that chunk's ATT response until the SD write completes and
// can make an otherwise tiny card transfer take tens of seconds. This remains
// bounded to one card (unlike the old whole-deck buffer that exhausted RAM).
enum RxState { RX_IDLE, RX_BODY };
static RxState rxState = RX_IDLE;
static size_t rxExpected = 0;
static size_t rxReceived = 0;
static std::vector<uint8_t> putBody;
static bool putOk = false;   // false if allocation, SD open, or write fails
// Body layout has two u16-sized text fields plus their prefixes and bitmaps.
// Reject anything larger before resizing the one-card buffer.
static constexpr size_t MAX_CARD_BODY_BYTES = 4 + 2 * 65535 + 2 * BMP_BYTES;
static uint32_t putCardId = 0;
static int      putCardBox = 0;
static bool     putCardPracticed = false;
static uint32_t pendingDeleteId = 0;

#if BLE_FAST_SYNC
static constexpr uint8_t FAST_BEGIN  = 0x20;
static constexpr uint8_t FAST_DATA   = 0x21;
static constexpr uint8_t FAST_STATUS = 0x22;
static constexpr uint8_t FAST_COMMIT = 0x23;
static constexpr uint8_t FAST_WINDOW = 4;
static bool fastActive = false;
static uint32_t fastTransferId = 0;
static uint32_t fastRequestedCardId = 0;
static uint32_t fastExpectedCrc = 0;
static uint8_t fastFramesSinceAck = 0;
static uint8_t fastReplyOpcode = 0;
static uint8_t fastReplyStatus = 0;
#endif

#if BLE_TIMING_DEBUG
struct PutTiming {
  uint32_t headerAt;
  uint32_t firstChunkAt;
  uint32_t previousChunkAt;
  uint32_t lastChunkAt;
  uint32_t copyUs;
  uint32_t maxCopyUs;
  uint32_t maxGapUs;
  uint32_t chunkCount;
};
static PutTiming putTiming = {};
#endif

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
  PENDING_DELETE_CARD, PENDING_DELETE_ALL,
#if BLE_FAST_SYNC
  PENDING_FAST_REPLY, PENDING_FAST_COMMIT
#endif
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

static uint32_t getU32(const uint8_t *in) {
  return (uint32_t)in[0] | ((uint32_t)in[1] << 8) |
         ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
}

#if BLE_FAST_SYNC
static void queueFastReply(uint8_t opcode, uint8_t status) {
  fastReplyOpcode = opcode;
  fastReplyStatus = status;
  pendingAction = PENDING_FAST_REPLY;
}

static void sendFastReply() {
  uint8_t resp[14];
  resp[0] = fastReplyOpcode;
  resp[1] = fastReplyStatus;
  putU32(resp + 2, fastTransferId);
  putU32(resp + 6, (uint32_t)rxReceived);
  size_t len = 10;
  if (fastReplyOpcode == FAST_COMMIT) {
    putU32(resp + 10, putCardId);
    len = 14;
  }
  sendChunkReliably(resp, len);
}

static bool validCardBodyShape() {
  if (putBody.size() < 4 + 2 * BMP_BYTES) return false;
  size_t frontLen = putBody[0] | ((size_t)putBody[1] << 8);
  size_t backLenOffset = 2 + frontLen;
  if (backLenOffset + 2 > putBody.size()) return false;
  size_t backLen = putBody[backLenOffset] | ((size_t)putBody[backLenOffset + 1] << 8);
  return backLenOffset + 2 + backLen + 2 * BMP_BYTES == putBody.size();
}

static void commitFastCard() {
  uint8_t status = 0;
  if (!fastActive || rxReceived != rxExpected || !putOk || !validCardBodyShape()) {
    status = 1;
  } else if (esp_rom_crc32_le(0, putBody.data(), putBody.size()) != fastExpectedCrc) {
    status = 2;
  }

  putCardId = fastRequestedCardId ? fastRequestedCardId : nextId;
  char path[24], tmpPath[28];
  cardFilePath(putCardId, path, sizeof(path));
  cardTmpPath(putCardId, tmpPath, sizeof(tmpPath));
  if (status == 0) {
    SD_MMC.remove(tmpPath);
    File f = SD_MMC.open(tmpPath, FILE_WRITE);
    if (!f) status = 3;
    else {
      uint8_t hdr[CARD_HEADER_BYTES];
      encodeCardHeader(hdr, putCardId, putCardBox, putCardPracticed);
      bool written = f.write(hdr, CARD_HEADER_BYTES) == CARD_HEADER_BYTES &&
                     f.write(putBody.data(), putBody.size()) == putBody.size();
      f.close();
      if (!written) status = 3;
    }
  }
  if (status == 0) {
    SD_MMC.remove(path);
    if (!SD_MMC.rename(tmpPath, path)) status = 3;
  }
  if (status != 0) SD_MMC.remove(tmpPath);

  if (status == 0) {
    if (!fastRequestedCardId) nextId++;
    uint16_t frontLen = 0, backLen = 0;
    readCardTextLens(putCardId, frontLen, backLen);
    upsertCardInMemory(putCardId, putCardBox, putCardPracticed, frontLen, backLen);
  }
  Serial.printf("BLE fast: commit card %08X (%u bytes, crc=%08X) -> %s\n",
                (unsigned)putCardId, (unsigned)rxReceived, (unsigned)fastExpectedCrc,
                status == 0 ? "ok" : "FAILED");
  fastReplyOpcode = FAST_COMMIT;
  fastReplyStatus = status;
  sendFastReply();
  fastActive = false;
  putBody.clear();
}
#endif

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
#if BLE_TIMING_DEBUG
  uint32_t finishAt = micros();
  uint32_t sdOpenUs = 0, sdWriteUs = 0, sdCloseUs = 0, sdRenameUs = 0;
#endif
  char path[24], tmpPath[28];
  cardFilePath(putCardId, path, sizeof(path));
  cardTmpPath(putCardId, tmpPath, sizeof(tmpPath));
  if (putOk) {
    SD_MMC.remove(tmpPath);
#if BLE_TIMING_DEBUG
    uint32_t phaseAt = micros();
#endif
    File f = SD_MMC.open(tmpPath, FILE_WRITE);
#if BLE_TIMING_DEBUG
    sdOpenUs = micros() - phaseAt;
#endif
    if (!f) {
      putOk = false;
    } else {
      uint8_t hdr[CARD_HEADER_BYTES];
      encodeCardHeader(hdr, putCardId, putCardBox, putCardPracticed);
#if BLE_TIMING_DEBUG
      phaseAt = micros();
#endif
      putOk = (f.write(hdr, CARD_HEADER_BYTES) == CARD_HEADER_BYTES) &&
              (putBody.empty() || f.write(putBody.data(), putBody.size()) == putBody.size());
#if BLE_TIMING_DEBUG
      sdWriteUs = micros() - phaseAt;
      phaseAt = micros();
#endif
      f.close();
#if BLE_TIMING_DEBUG
      sdCloseUs = micros() - phaseAt;
#endif
    }
  }
  if (putOk) {
#if BLE_TIMING_DEBUG
    uint32_t phaseAt = micros();
#endif
    SD_MMC.remove(path);
    putOk = SD_MMC.rename(tmpPath, path);
#if BLE_TIMING_DEBUG
    sdRenameUs = micros() - phaseAt;
#endif
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

#if BLE_TIMING_DEBUG
  uint32_t doneAt = micros();
  Serial.printf(
    "BLE_TIMING card=%08X bytes=%u chunks=%u header_to_first=%.1fms "
    "first_to_last=%.1fms max_gap=%.1fms copy_total=%.3fms max_copy=%.3fms "
    "finish_wait=%.1fms sd_open=%.1fms sd_write=%.1fms sd_close=%.1fms "
    "sd_rename=%.1fms finish_total=%.1fms\n",
    (unsigned)putCardId, (unsigned)rxReceived, (unsigned)putTiming.chunkCount,
    (putTiming.firstChunkAt - putTiming.headerAt) / 1000.0,
    (putTiming.lastChunkAt - putTiming.firstChunkAt) / 1000.0,
    putTiming.maxGapUs / 1000.0, putTiming.copyUs / 1000.0,
    putTiming.maxCopyUs / 1000.0, (finishAt - putTiming.lastChunkAt) / 1000.0,
    sdOpenUs / 1000.0, sdWriteUs / 1000.0, sdCloseUs / 1000.0,
    sdRenameUs / 1000.0, (doneAt - finishAt) / 1000.0);
#endif

  rxState = RX_IDLE;
  putBody.clear();
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
#if BLE_FAST_SYNC
      } else if (data[0] == FAST_BEGIN && len >= 20) {
        fastTransferId = getU32(data + 1);
        fastRequestedCardId = getU32(data + 5);
        putCardBox = data[9] | (data[10] << 8);
        putCardPracticed = data[11] != 0;
        rxExpected = getU32(data + 12);
        fastExpectedCrc = getU32(data + 16);
        rxReceived = 0;
        fastFramesSinceAck = 0;
        putOk = sdOk && fastTransferId != 0 && rxExpected <= MAX_CARD_BODY_BYTES;
        putBody.clear();
        if (putOk) putBody.resize(rxExpected);
        fastActive = putOk;
        queueFastReply(FAST_BEGIN, putOk ? 0 : 1);
      } else if (data[0] == FAST_DATA && len >= 9) {
        uint32_t transferId = getU32(data + 1);
        uint32_t offset = getU32(data + 5);
        size_t payloadLen = len - 9;
        if (!fastActive || transferId != fastTransferId) {
          queueFastReply(FAST_DATA, 1);
        } else if (offset == rxReceived && payloadLen <= rxExpected - rxReceived) {
          memcpy(putBody.data() + rxReceived, data + 9, payloadLen);
          rxReceived += payloadLen;
          fastFramesSinceAck++;
          if (fastFramesSinceAck >= FAST_WINDOW || rxReceived == rxExpected) {
            fastFramesSinceAck = 0;
            queueFastReply(FAST_DATA, 0);
          }
        } else if (offset < rxReceived && offset + payloadLen <= rxReceived) {
          // Harmless duplicate after a lost cumulative ACK.
          queueFastReply(FAST_DATA, 0);
        } else {
          // Gap, overlap, or out-of-bounds frame: report the contiguous offset.
          queueFastReply(FAST_DATA, 2);
        }
      } else if (data[0] == FAST_STATUS && len >= 5) {
        uint32_t transferId = getU32(data + 1);
        queueFastReply(FAST_STATUS,
                       fastActive && transferId == fastTransferId ? 0 : 1);
      } else if (data[0] == FAST_COMMIT && len >= 5) {
        uint32_t transferId = getU32(data + 1);
        if (fastActive && transferId == fastTransferId) pendingAction = PENDING_FAST_COMMIT;
        else queueFastReply(FAST_COMMIT, 1);
#endif
      } else if (data[0] == 0x11 && len >= 12) {    // PUT_CARD header
#if BLE_FAST_SYNC
        fastActive = false;                         // explicit legacy fallback
#endif
        uint32_t reqId = (uint32_t)data[1] | ((uint32_t)data[2] << 8) |
                          ((uint32_t)data[3] << 16) | ((uint32_t)data[4] << 24);
        putCardBox = data[5] | (data[6] << 8);
        putCardPracticed = data[7] != 0;
        uint32_t bodyLen = (uint32_t)data[8] | ((uint32_t)data[9] << 8) |
                            ((uint32_t)data[10] << 16) | ((uint32_t)data[11] << 24);
        putCardId = (reqId == 0) ? nextId++ : reqId;
        rxExpected = bodyLen;
        rxReceived = 0;
        putOk = sdOk && bodyLen <= MAX_CARD_BODY_BYTES;
        putBody.clear();
        if (putOk) putBody.resize(bodyLen);
#if BLE_TIMING_DEBUG
        putTiming = {};
        putTiming.headerAt = micros();
#endif
        if (bodyLen == 0) pendingAction = PENDING_FINISH_PUT_CARD;
        else rxState = RX_BODY;
      } else if (data[0] == 0x12 && len >= 5) {     // DELETE_CARD
        pendingDeleteId = (uint32_t)data[1] | ((uint32_t)data[2] << 8) |
                           ((uint32_t)data[3] << 16) | ((uint32_t)data[4] << 24);
        pendingAction = PENDING_DELETE_CARD;
      } else if (data[0] == 0x13) {                 // DELETE_ALL
        pendingAction = PENDING_DELETE_ALL;
      }
    } else {   // RX_BODY: raw body bytes, no framing -- copied into the
               // one-card RAM buffer and committed by finishPutCard().
#if BLE_TIMING_DEBUG
      uint32_t chunkAt = micros();
      if (putTiming.chunkCount == 0) putTiming.firstChunkAt = chunkAt;
      else {
        uint32_t gap = chunkAt - putTiming.previousChunkAt;
        if (gap > putTiming.maxGapUs) putTiming.maxGapUs = gap;
      }
#endif
      size_t n = minSize(len, rxExpected - rxReceived);
      if (putOk && n > 0) memcpy(putBody.data() + rxReceived, data, n);
#if BLE_TIMING_DEBUG
      uint32_t copyElapsed = micros() - chunkAt;
      putTiming.copyUs += copyElapsed;
      if (copyElapsed > putTiming.maxCopyUs) putTiming.maxCopyUs = copyElapsed;
      putTiming.previousChunkAt = chunkAt;
      putTiming.lastChunkAt = chunkAt;
      putTiming.chunkCount++;
#endif
      rxReceived += n;
      if (rxReceived >= rxExpected) pendingAction = PENDING_FINISH_PUT_CARD;
    }
  }
};

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *pServer) override {
    bleConnected = true;
    rxState = RX_IDLE;
    putBody.clear();                 // discard a prior connection's partial PUT
#if BLE_FAST_SYNC
    fastActive = false;
#endif
    Serial.println("BLE: central connected");
  }
  void onConnect(BLEServer *pServer, ble_gap_conn_desc *desc) override {
    bleConnHandle = desc->conn_handle;
    connParamRequestPending = true;
#if BLE_TIMING_DEBUG
    Serial.printf("BLE_CONN initial interval=%.2fms latency=%u timeout=%ums handle=%u\n",
                  desc->conn_itvl * 1.25, (unsigned)desc->conn_latency,
                  (unsigned)desc->supervision_timeout * 10, (unsigned)desc->conn_handle);
#endif
  }
  void onConnParamsUpdate(uint16_t conn_handle, uint16_t interval,
                          uint16_t latency, uint16_t timeout, uint8_t status) override {
#if BLE_TIMING_DEBUG
    Serial.printf("BLE_CONN updated status=%u interval=%.2fms latency=%u timeout=%ums handle=%u\n",
                  (unsigned)status, interval * 1.25, (unsigned)latency,
                  (unsigned)timeout * 10, (unsigned)conn_handle);
#endif
  }
  void onDisconnect(BLEServer *pServer) override {
    bleConnected = false;
    connParamRequestPending = false;
#if BLE_FAST_SYNC
    fastActive = false;
#endif
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
  BLECharacteristic *rxChar = service->createCharacteristic(
    NUS_RX_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  rxChar->setCallbacks(new RxCallbacks());

  service->start();

  BLEAdvertising *adv = pServer->getAdvertising();
  adv->addServiceUUID(NUS_SERVICE_UUID);
  adv->start();
  Serial.println("BLE advertising as \"" DEVICE_NAME "\"");
}

void bleSyncTick() {
  // GAP calls are deliberately made from the Arduino task, never from the
  // NimBLE callback. The central may accept, reject, or adjust this request.
  if (connParamRequestPending) {
    connParamRequestPending = false;
    bool queued = pServer->requestConnParams(bleConnHandle, 12, 24, 0, 400);
#if BLE_TIMING_DEBUG
    Serial.printf("BLE_CONN request 15-30ms latency=0 -> %s\n", queued ? "queued" : "FAILED");
#else
    (void)queued;
#endif
  }

  // Snapshot-and-clear before dispatching in case the action itself takes a
  // while and bleConnected/rxState change during it.
  PendingBleAction action = pendingAction;
  pendingAction = PENDING_NONE;
  switch (action) {
    case PENDING_SEND_LIST:       sendListOverBle(); break;
    case PENDING_FINISH_PUT_CARD: finishPutCard(); break;
    case PENDING_DELETE_CARD:     deleteCardOnDevice(pendingDeleteId); break;
    case PENDING_DELETE_ALL:      deleteAllCardsOnDevice(); break;
#if BLE_FAST_SYNC
    case PENDING_FAST_REPLY:      sendFastReply(); break;
    case PENDING_FAST_COMMIT:     commitFastCard(); break;
#endif
    case PENDING_NONE: break;
  }
}
