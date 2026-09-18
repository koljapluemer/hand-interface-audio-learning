## Audio-Based Language Learning with a Haptic Interface

Experimenting with ESPs to make a haptic button interface, wearable on a finger.

---

## Internal: flashcard sync website (`code/scripts/006_esp_s3_touch_flashcard_sd`)

This build reads its cards from `/cards.tsv` on the SD card and has a small
on-device flow to edit that file from a phone. No app, no internet, no cloud.

### Card file

- `/cards.tsv` on a **FAT32** SD card. One card per line: `front<TAB>back`.
- Missing file → the device just shows "no flashcards" and still lets you into
  sync mode to add some. No card at all → "No SD card" screen, reset to retry.
- `cards.csv` in the script folder is a sample import (headerless, `front,back`).

### Entering sync mode

1. On the card (pre-reveal) screen, tap the **gear** in the top-right corner
   (tap area is the ~1 cm corner square).
2. Settings screen → **Sync mode**.
3. The device brings up a Wi-Fi access point and shows the join details.

The game loop is fully paused while sync mode is up, so there's nothing to
reason about re: the SPI/SD/Wi-Fi buses.

### Using the website

1. Join Wi-Fi network **`flashcards`**, password **`flashcards`**.
   (Phone will warn "no internet" — expected, the AP isn't a gateway.)
2. Open **`http://192.168.4.1`** in a browser.
3. Edit:
   - **+ Add card** — appends a blank row.
   - Edit front/back inline; **Delete** removes a row.
   - **Import CSV** — pick a headerless `.csv` (col 1 = front, col 2 = back;
     quoted fields / embedded commas handled). Rows are appended to the list
     for review — nothing is written yet.
   - **Save** — writes the whole list back to `/cards.tsv`. Blank rows are
     dropped; front/back are trimmed.
4. Back on the device, tap **Restart**. It reboots into the game with the new
   cards. (Long-press PWR still shuts the device down from any screen.)

### HTTP API (served from the device in sync mode)

| Method | Path | Body | Notes |
|---|---|---|---|
| `GET`  | `/`           | —   | the editor page (self-contained, inline CSS) |
| `GET`  | `/api/cards`  | —   | current `/cards.tsv` as `text/plain`, empty if none |
| `PUT`  | `/api/cards`  | TSV | overwrites `/cards.tsv`, returns `ok` |

CSV parsing and all CRUD happen in the browser; the device only ever stores
TSV. Changes are not live — they take effect on the next boot.

### Build / flash

```
arduino-cli compile --fqbn esp32:esp32:esp32s3 code/scripts/006_esp_s3_touch_flashcard_sd
arduino-cli upload  --fqbn esp32:esp32:esp32s3 --port /dev/ttyACM0 code/scripts/006_esp_s3_touch_flashcard_sd
```

Libraries: `GxEPD2`, `U8g2_for_Adafruit_GFX`, and the ESP32 core (`WiFi`,
`WebServer`, `SD_MMC`; needs core ≥ 2.0.7 for `SD_MMC.setPins()`).

---

## Internal: `code/scripts/007_esp_s3_touch_flashcard_ble` — build / flash

```
arduino-cli compile --fqbn "esp32:esp32:esp32s3:PSRAM=opi,PartitionScheme=default_8MB,FlashSize=8M" code/scripts/007_esp_s3_touch_flashcard_ble
arduino-cli upload  --fqbn "esp32:esp32:esp32s3:PSRAM=opi,PartitionScheme=default_8MB,FlashSize=8M" --port /dev/ttyACM0 code/scripts/007_esp_s3_touch_flashcard_ble
```

Board options must be set explicitly (OPI PSRAM, 8MB flash, 8MB-with-spiffs
partition scheme) — the generic `esp32:esp32:esp32s3` FQBN's defaults are
4MB flash / no PSRAM and will boot-loop on this board otherwise.
