// SD-backed card storage, one file per card.
//
// /cards/<id>.bin (id = 8 hex digits, so the basename is exactly 8.3):
//   [magic "FC02"][u32 id LE][u16 box LE][u8 practiced],
//   then [u16 frontLen LE][frontText UTF-8][u16 backLen LE][backText UTF-8]
//   [frontBitmap][backBitmap], each bitmap BMP_W x BMP_H, 1bpp, MSB-first,
//   rows padded to a byte -- exactly what Adafruit_GFX::drawBitmap() wants.
//
// "What cards exist" is just "what files are in /cards/", so there is no
// index to fall out of sync. Each file carries text, bitmaps AND learning
// state together, so editing a card can't orphan its progress. The text is
// carried purely as a courtesy to sync.html's edit UI; the device never
// renders it (sync.html renders both fields to canvas and ships pixels). The
// in-file id is redundant with the filename and only used to log a mismatch;
// the filename is authoritative.
//
// "FC02" = 200x92 bitmaps. Files from the older 190x36 layout ("FC01") fail
// magic/size validation and are skipped with a serial log; there is
// deliberately no migration -- wipe them with sync.html's Delete-all.
#pragma once
#include <Arduino.h>
#include <vector>

struct Flashcard {
  uint32_t id;
  int box; bool practiced;
  // Cached at load so bitmaps can be seeked to without re-reading the header.
  uint16_t frontLen, backLen;
};

extern std::vector<Flashcard> cards;   // sorted by id (= creation order)
extern bool sdOk;
extern uint32_t nextId;                // 0 is the wire protocol's "assign me an id"

constexpr size_t CARD_HEADER_BYTES = 4 + 4 + 2 + 1;   // magic + id + box + practiced

bool sdBegin();      // mount SD, make sure /cards exists; sets sdOk
bool loadCards();    // scan /cards into `cards`; false if none
bool saveCard(const Flashcard &c);   // atomically rewrite one card's box/practiced

// Reads the front or back bitmap (BMP_BYTES) of a card into `buf`.
bool readCardBitmap(const Flashcard &c, bool front, uint8_t *buf);

// Paths / on-disk header, shared with the BLE PUT_CARD writer.
void cardFilePath(uint32_t id, char *out, size_t outLen);
void cardTmpPath(uint32_t id, char *out, size_t outLen);
void encodeCardHeader(uint8_t *hdr, uint32_t id, int box, bool practiced);
bool parseIdFromFilename(const char *name, uint32_t &outId);

// In-memory index maintenance after BLE-side file changes.
bool readCardTextLens(uint32_t id, uint16_t &frontLen, uint16_t &backLen);
void upsertCardInMemory(uint32_t id, int box, bool practiced, uint16_t frontLen, uint16_t backLen);
bool removeCard(uint32_t id);            // file + index entry
uint32_t removeAllCards();               // every id-named file; returns count
