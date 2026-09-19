#include <Arduino.h>
#include <algorithm>
#include <cstring>
#include "SD_MMC.h"
#include "config.h"
#include "card_store.h"

std::vector<Flashcard> cards;
bool sdOk = false;
uint32_t nextId = 1;

static const char *CARDS_DIR = "/cards";
static const char *CARD_MAGIC = "FC02";
static const size_t CARD_MAGIC_LEN = 4;

bool sdBegin() {
  // SD on the dedicated SDMMC peripheral, 1-bit mode -- separate from the
  // e-paper SPI bus.
  SD_MMC.setPins(PIN_SD_CLK, PIN_SD_CMD, PIN_SD_D0);
  sdOk = SD_MMC.begin("/sdcard", true);
  Serial.printf("SD mount: %s\n", sdOk ? "ok" : "FAILED");
  // FILE_WRITE never creates missing parent directories, so a fresh SD with no
  // /cards/ would make the very first PUT_CARD fail to open its temp file.
  if (sdOk && !SD_MMC.exists(CARDS_DIR)) SD_MMC.mkdir(CARDS_DIR);
  return sdOk;
}

void cardFilePath(uint32_t id, char *out, size_t outLen) {
  snprintf(out, outLen, "%s/%08X.bin", CARDS_DIR, (unsigned)id);
}

void cardTmpPath(uint32_t id, char *out, size_t outLen) {
  snprintf(out, outLen, "%s/%08X.bin.tmp", CARDS_DIR, (unsigned)id);
}

void encodeCardHeader(uint8_t *hdr, uint32_t id, int box, bool practiced) {
  memcpy(hdr, CARD_MAGIC, CARD_MAGIC_LEN);
  hdr[4] = (uint8_t)(id);         hdr[5] = (uint8_t)(id >> 8);
  hdr[6] = (uint8_t)(id >> 16);   hdr[7] = (uint8_t)(id >> 24);
  hdr[8] = (uint8_t)(box);        hdr[9] = (uint8_t)(box >> 8);
  hdr[10] = practiced ? 1 : 0;
}

// Parses the 8-hex-digit id out of a name as returned by File::name() (which
// may or may not include the "/cards/" prefix depending on core version --
// scan from the last '/'). False for anything that isn't one of ours.
bool parseIdFromFilename(const char *name, uint32_t &outId) {
  const char *slash = strrchr(name, '/');
  const char *base = slash ? slash + 1 : name;
  if (strlen(base) != 12 || strcmp(base + 8, ".bin") != 0) return false;
  char *end = nullptr;
  unsigned long v = strtoul(base, &end, 16);
  if (end != base + 8) return false;
  outId = (uint32_t)v;
  return true;
}

// Reads a u16-LE length prefix, then skips that many bytes.
static bool readLenPrefixedSkip(File &f, uint16_t &outLen) {
  uint8_t lenBuf[2];
  if (f.read(lenBuf, 2) != 2) return false;
  outLen = lenBuf[0] | ((uint16_t)lenBuf[1] << 8);
  return f.seek(f.position() + outLen);
}

// Parses one open card file's header + record shape. Validates that the
// record walks exactly to EOF, which also rejects old-layout files.
static bool loadOneCard(File &f, uint32_t idFromName, Flashcard &outCard) {
  uint8_t hdr[CARD_HEADER_BYTES];
  if (f.read(hdr, CARD_HEADER_BYTES) != CARD_HEADER_BYTES ||
      memcmp(hdr, CARD_MAGIC, CARD_MAGIC_LEN) != 0) {
    return false;
  }
  uint32_t idInFile = (uint32_t)hdr[4] | ((uint32_t)hdr[5] << 8) |
                       ((uint32_t)hdr[6] << 16) | ((uint32_t)hdr[7] << 24);
  if (idInFile != idFromName) {
    Serial.printf("cards/%08X.bin: id mismatch (file says %08X), trusting filename\n",
                  (unsigned)idFromName, (unsigned)idInFile);
  }
  outCard.id = idFromName;
  outCard.box = hdr[8] | (hdr[9] << 8);
  outCard.practiced = hdr[10] != 0;

  if (!readLenPrefixedSkip(f, outCard.frontLen)) return false;
  if (!readLenPrefixedSkip(f, outCard.backLen)) return false;
  if (!f.seek(f.position() + 2 * BMP_BYTES)) return false;
  return f.position() == f.size();
}

bool loadCards() {
  cards.clear();
  uint32_t maxId = 0;

  File dir = SD_MMC.open(CARDS_DIR);
  if (!dir || !dir.isDirectory()) {
    Serial.println("no /cards directory yet");
    if (dir) dir.close();
    nextId = 1;
    return false;
  }

  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    if (f.isDirectory()) { f.close(); continue; }
    uint32_t idFromName;
    if (!parseIdFromFilename(f.name(), idFromName)) {
      Serial.printf("cards: skipping unrecognized file %s\n", f.name());
      f.close();
      continue;
    }
    Flashcard c;
    bool ok = loadOneCard(f, idFromName, c);
    f.close();
    if (!ok) {
      Serial.printf("cards/%08X.bin: bad/truncated/old-format record, skipping\n", (unsigned)idFromName);
      continue;
    }
    cards.push_back(c);
    if (idFromName > maxId) maxId = idFromName;
  }
  dir.close();

  // openNextFile() order isn't id order; sort so "deck order" means creation order.
  std::sort(cards.begin(), cards.end(),
            [](const Flashcard &a, const Flashcard &b) { return a.id < b.id; });

  nextId = maxId + 1;
  Serial.printf("loaded %u card(s), nextId=%u\n", (unsigned)cards.size(), (unsigned)nextId);
  return !cards.empty();
}

// Atomic temp-file+rename rewrite of one card's box/practiced; everything
// after the header is copied through byte-for-byte. A crash mid-write can
// never lose more than the one card being graded.
bool saveCard(const Flashcard &c) {
  if (!sdOk) return false;
  char path[24], tmpPath[28];
  cardFilePath(c.id, path, sizeof(path));
  cardTmpPath(c.id, tmpPath, sizeof(tmpPath));

  File src = SD_MMC.open(path, FILE_READ);
  if (!src) { Serial.printf("saveCard %08X: source missing\n", (unsigned)c.id); return false; }

  SD_MMC.remove(tmpPath);   // stale leftover from an interrupted save, if any
  File dst = SD_MMC.open(tmpPath, FILE_WRITE);
  if (!dst) { src.close(); Serial.println("saveCard: open tmp FAILED"); return false; }

  uint8_t hdr[CARD_HEADER_BYTES];
  encodeCardHeader(hdr, c.id, c.box, c.practiced);
  bool ok = (dst.write(hdr, CARD_HEADER_BYTES) == CARD_HEADER_BYTES);

  src.seek(CARD_HEADER_BYTES);
  static uint8_t copyBuf[128];
  while (ok && src.available()) {
    size_t n = src.read(copyBuf, sizeof(copyBuf));
    if (n == 0) break;
    ok = (dst.write(copyBuf, n) == n);
  }

  dst.flush();
  dst.close();
  src.close();

  if (!ok) { SD_MMC.remove(tmpPath); return false; }
  SD_MMC.remove(path);
  if (!SD_MMC.rename(tmpPath, path)) {
    Serial.printf("saveCard %08X: rename FAILED\n", (unsigned)c.id);
    return false;
  }
  return true;
}

// No-op-on-failure read: callers leave the area blank rather than drawing garbage.
bool readCardBitmap(const Flashcard &c, bool front, uint8_t *buf) {
  char path[24];
  cardFilePath(c.id, path, sizeof(path));
  File f = SD_MMC.open(path, FILE_READ);
  if (!f) return false;
  uint32_t offset = CARD_HEADER_BYTES + 2 + c.frontLen + 2 + c.backLen;
  if (!front) offset += BMP_BYTES;
  bool ok = f.seek(offset) && f.read(buf, BMP_BYTES) == BMP_BYTES;
  f.close();
  return ok;
}

// Peeks the two text lengths out of a committed file, so the in-RAM cache
// always matches what's actually on disk.
bool readCardTextLens(uint32_t id, uint16_t &frontLen, uint16_t &backLen) {
  char path[24];
  cardFilePath(id, path, sizeof(path));
  File f = SD_MMC.open(path, FILE_READ);
  if (!f) return false;
  f.seek(CARD_HEADER_BYTES);
  bool ok = readLenPrefixedSkip(f, frontLen) && readLenPrefixedSkip(f, backLen);
  f.close();
  return ok;
}

void upsertCardInMemory(uint32_t id, int box, bool practiced, uint16_t frontLen, uint16_t backLen) {
  for (auto &c : cards) {
    if (c.id == id) {
      c.box = box; c.practiced = practiced; c.frontLen = frontLen; c.backLen = backLen;
      return;
    }
  }
  Flashcard c;
  c.id = id; c.box = box; c.practiced = practiced;
  c.frontLen = frontLen; c.backLen = backLen;
  cards.push_back(c);
}

bool removeCard(uint32_t id) {
  char path[24];
  cardFilePath(id, path, sizeof(path));
  bool ok = SD_MMC.remove(path);
  if (ok) {
    for (size_t i = 0; i < cards.size(); i++) {
      if (cards[i].id == id) { cards.erase(cards.begin() + i); break; }
    }
  }
  return ok;
}

uint32_t removeAllCards() {
  uint32_t deleted = 0;
  File dir = SD_MMC.open(CARDS_DIR);
  if (dir && dir.isDirectory()) {
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
      bool isDirEntry = f.isDirectory();
      uint32_t id;
      bool named = !isDirEntry && parseIdFromFilename(f.name(), id);
      f.close();
      if (!named) continue;
      char path[24];
      cardFilePath(id, path, sizeof(path));
      if (SD_MMC.remove(path)) deleted++;
    }
    dir.close();
  }
  cards.clear();
  nextId = 1;
  return deleted;
}
