/*
 * 006_esp_s3_touch_flashcard_sd
 *
 * Board: Waveshare ESP32-S3-ePaper-1.54
 *        (200x200 black/white e-paper, FT6336 capacitive touch, I2C 0x38,
 *         integrated microSD/TF slot on the ESP32-S3 SDMMC peripheral)
 *
 * Same {front, back} flashcard game as 005, but the cards no longer live in
 * a compile-time array -- they're read from "/cards.tsv" on the SD card at
 * boot, and there's a small on-device settings flow to manage them from a
 * phone:
 *
 *   - Prompt state: the front (big) on row 1; a small gear icon in the top-
 *     right corner (tap target ~1 cm) opens Settings; the whole lower half is
 *     the "Reveal" button.
 *   - Answer state: front, dashed rule, back (big). Lower-left = "Wrong",
 *     lower-right = "Correct". Either just advances to a new random card;
 *     nothing is persisted.
 *   - Settings: "Sync mode" and "Back".
 *   - Sync mode: brings up a Wi-Fi access point ("flashcards" / "flashcards")
 *     and a tiny web server. Join the AP, open http://192.168.4.1 in a phone
 *     browser, do CRUD on the list, optionally import a headerless CSV
 *     (col 1 = front, col 2 = back), press Save (writes /cards.tsv), then tap
 *     "Restart" on the device to reboot into the game with the new cards.
 *     The game loop is fully paused while sync mode is up -- no shared-bus or
 *     concurrency reasoning -- and the served page is self-contained (no CDN,
 *     no internet needed).
 *
 * Storage split: the e-paper is on SPI2 (SCK 12 / MOSI 13 / CS 11); the SD
 * card is on the S3's dedicated SDMMC peripheral in 1-bit mode (CLK 39 /
 * CMD 41 / D0 40, from Waveshare's 04_SD_Card example). Independent buses,
 * so nothing has to share a CS line. Card must be FAT32.
 *
 * Text goes through U8g2_for_Adafruit_GFX (same reason as 005): the "_tf"
 * fonts cover Unicode 0x20-0xFF, so German umlauts / eszett render from the
 * same two fonts, and UTF-8 stored in the file measures/prints correctly.
 *
 * Also latches battery power at boot and honors the PWR button (long-press to
 * shut down), including while sync mode is running.
 *
 * Libraries: GxEPD2, U8g2_for_Adafruit_GFX, and the ESP32 Arduino core
 * (WiFi, WebServer, SD_MMC). Needs core >= 2.0.7 for SD_MMC.setPins().
 * Arduino IDE settings: same as 003_esp_s3_touch_quadrants_on_serial.
 */

#include <Wire.h>
#include <SPI.h>
#include <math.h>
#include <vector>
#include <GxEPD2_BW.h>
#include <U8g2_for_Adafruit_GFX.h>
#include <WiFi.h>
#include <WebServer.h>
#include "SD_MMC.h"

// Big font for the front row / back row; small font for button labels and the
// settings/sync text. Both "_tf" fonts cover ASCII + Latin-1 Supplement.
static const uint8_t *FONT_BIG   = u8g2_font_9x15_tf;
static const uint8_t *FONT_SMALL = u8g2_font_6x12_tf;

// ---- Pin map (Waveshare user_config.h + 04_SD_Card/sdcard_bsp.cpp) ----
static const int PIN_TP_SDA  = 47;
static const int PIN_TP_SCL  = 48;
static const int PIN_TP_INT  = 21;   // active low; unused (we poll)
static const int PIN_TP_RST  = 7;
static const int PIN_EPD_PWR = 6;    // e-paper power rail, active LOW

static const int PIN_VBAT_PWR = 17;  // battery self-latch (see 005 notes)
static const int PIN_PWR_BTN  = 18;  // active LOW
static const uint32_t PWR_LONGPRESS_MS = 1000;

static const int PIN_EPD_CS   = 11;
static const int PIN_EPD_DC   = 10;
static const int PIN_EPD_RST  = 9;
static const int PIN_EPD_BUSY = 8;
static const int PIN_EPD_SCK  = 12;
static const int PIN_EPD_MOSI = 13;

// SD card on the SDMMC peripheral, 1-bit mode.
static const int PIN_SD_CLK = 39;
static const int PIN_SD_CMD = 41;
static const int PIN_SD_D0  = 40;

GxEPD2_BW<GxEPD2_154_D67, GxEPD2_154_D67::HEIGHT> display(
    GxEPD2_154_D67(PIN_EPD_CS, PIN_EPD_DC, PIN_EPD_RST, PIN_EPD_BUSY));

U8G2_FOR_ADAFRUIT_GFX u8f;

static const uint8_t FT6336_ADDR = 0x38;

// ---- Panel geometry ----
static const int W = 200, H = 200;
static const int MID_X = W / 2;

// Card screen (matches the 005 layout).
static const int ROW_WORD_Y = 32;   // big font, front
static const int DASH_Y     = 52;
static const int ROW_MEAN_Y = 84;   // big font, back (answer state)
static const int DIV_Y      = 112;
static const int BTN_Y = DIV_Y + 4;
static const int BTN_H = H - DIV_Y - 8;
static const int WIDE_BTN_X = 4,          WIDE_BTN_W  = W - 8;        // Reveal
static const int LEFT_BTN_X = 4,          LEFT_BTN_W  = MID_X - 8;    // Wrong
static const int RIGHT_BTN_X = MID_X + 4, RIGHT_BTN_W = MID_X - 8;    // Correct
static const int WIDE_CX  = WIDE_BTN_X + WIDE_BTN_W / 2;
static const int LEFT_CX  = LEFT_BTN_X + LEFT_BTN_W / 2;
static const int RIGHT_CX = RIGHT_BTN_X + RIGHT_BTN_W / 2;
static const int BTN_LABEL_Y = 163;

// Settings gear: drawn tiny in the top-right corner; the tap target is the
// ~1 cm (64 px) corner square, which is dead space in the prompt state
// (Reveal lives below DIV_Y, and the front row is never itself a tap target).
// A very wide word can render pixels under this square, so a deliberate tap
// straight on those letters would open Settings -- acceptable trade.
static const int GEAR_CX = 184, GEAR_CY = 13;
static const int GEAR_TOUCH_X = W - 64;   // 136
static const int GEAR_TOUCH_H = 64;

// Settings screen buttons.
static const int SET_SYNC_X = 16, SET_SYNC_Y = 52,  SET_SYNC_W = 168, SET_SYNC_H = 58;
static const int SET_BACK_X = 16, SET_BACK_Y = 126, SET_BACK_W = 168, SET_BACK_H = 58;

// Sync screen "Restart" button.
static const int SYNC_RST_X = 16, SYNC_RST_Y = 160, SYNC_RST_W = 168, SYNC_RST_H = 32;

// ---- Orientation calibration (validated in 003) ----
static const bool SWAP_XY = false;
static const bool FLIP_X  = false;
static const bool FLIP_Y  = false;

// ---- Screens / state ----
enum Screen { SCREEN_CARD, SCREEN_SETTINGS, SCREEN_SYNC };
enum CardPhase { PHASE_PROMPT, PHASE_ANSWER };
static Screen    screen = SCREEN_CARD;
static CardPhase phase  = PHASE_PROMPT;

// ---- Flashcards (loaded from SD) ----
struct Flashcard { String front, back; };
static std::vector<Flashcard> cards;
static int  currentIndex = 0;
static bool sdOk = false;

// ---- Wi-Fi / web ----
static const char *AP_SSID = "flashcards";
static const char *AP_PASS = "flashcards";   // >= 8 chars for WPA2
static WebServer server(80);

// The whole management page, served straight from flash. Deliberately no CDN
// and no framework: the AP has no internet, so everything it needs is inline.
static const char PAGE_HTML[] = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Flashcards</title>
<style>
  :root{--bg:#f4f4f5;--card:#fff;--line:#d4d4d8;--ink:#18181b;--muted:#71717a;
        --accent:#2563eb;--danger:#dc2626;--ok:#16a34a}
  *{box-sizing:border-box}
  body{margin:0;font:16px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;
       color:var(--ink);background:var(--bg)}
  header{position:sticky;top:0;background:var(--card);border-bottom:1px solid var(--line);
         padding:12px 16px;display:flex;align-items:center;gap:12px}
  header h1{font-size:18px;margin:0;flex:1}
  main{max-width:720px;margin:0 auto;padding:16px}
  .bar{display:flex;flex-wrap:wrap;gap:8px;margin-bottom:12px}
  button,.filebtn{font:inherit;min-height:44px;padding:0 16px;border-radius:8px;
                  border:1px solid var(--line);background:var(--card);color:var(--ink);
                  cursor:pointer;display:inline-flex;align-items:center}
  button:active,.filebtn:active{transform:translateY(1px)}
  .primary{background:var(--accent);border-color:var(--accent);color:#fff}
  .save{background:var(--ok);border-color:var(--ok);color:#fff}
  .filebtn input{display:none}
  #status{min-height:22px;margin-bottom:12px;font-size:14px;color:var(--muted)}
  #status.err{color:var(--danger)}
  #status.ok{color:var(--ok)}
  ul{list-style:none;margin:0;padding:0;display:flex;flex-direction:column;gap:10px}
  li{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:10px;
     display:grid;grid-template-columns:1fr 1fr auto;gap:8px;align-items:center}
  li input{width:100%;font:inherit;padding:10px;border:1px solid var(--line);
           border-radius:8px;background:#fff;color:var(--ink)}
  li input:focus{outline:2px solid var(--accent);outline-offset:0;border-color:var(--accent)}
  .del{min-height:40px;padding:0 12px;color:var(--danger)}
  .empty{text-align:center;color:var(--muted);padding:40px 0}
  @media (max-width:520px){
    li{grid-template-columns:1fr auto;grid-template-areas:"f d" "b d"}
    li .f{grid-area:f}li .b{grid-area:b}li .del{grid-area:d;height:100%}
  }
</style>
</head>
<body>
<header>
  <h1>Flashcards</h1>
  <button class="save" id="save">Save</button>
</header>
<main>
  <div class="bar">
    <button class="primary" id="add">+ Add card</button>
    <label class="filebtn">Import CSV<input type="file" id="csv" accept=".csv,text/csv,text/plain"></label>
  </div>
  <div id="status"></div>
  <ul id="list"></ul>
  <p class="empty" id="empty" hidden>No cards yet. Add one or import a CSV.</p>
</main>
<script>
let cards=[];
const list=document.getElementById('list');
const empty=document.getElementById('empty');
const statusEl=document.getElementById('status');

function setStatus(msg,kind){statusEl.textContent=msg||'';statusEl.className=kind||'';}

function render(){
  list.innerHTML='';
  empty.hidden=cards.length>0;
  cards.forEach((c,i)=>{
    const li=document.createElement('li');
    const f=document.createElement('input');
    f.className='f';f.placeholder='Front';f.value=c.f;
    f.addEventListener('input',()=>cards[i].f=f.value);
    const b=document.createElement('input');
    b.className='b';b.placeholder='Back';b.value=c.b;
    b.addEventListener('input',()=>cards[i].b=b.value);
    const d=document.createElement('button');
    d.className='del';d.textContent='Delete';d.setAttribute('aria-label','Delete card');
    d.addEventListener('click',()=>{cards.splice(i,1);render();});
    li.append(f,b,d);
    list.appendChild(li);
  });
}

document.getElementById('add').addEventListener('click',()=>{
  cards.push({f:'',b:''});
  render();
  list.lastElementChild.querySelector('input').focus();
});

document.getElementById('csv').addEventListener('change',async e=>{
  const file=e.target.files[0];
  e.target.value='';
  if(!file)return;
  try{
    const rows=parseCSV(await file.text());
    cards=cards.concat(rows);
    render();
    setStatus('Imported '+rows.length+' card(s). Review, then Save.','ok');
  }catch(err){
    setStatus('Import failed: '+err.message,'err');
  }
});

document.getElementById('save').addEventListener('click',async()=>{
  const clean=cards.map(c=>({f:c.f.trim(),b:c.b.trim()})).filter(c=>c.f||c.b);
  const body=clean.map(c=>c.f+'\t'+c.b).join('\n');
  setStatus('Saving...');
  try{
    const r=await fetch('/api/cards',{method:'PUT',headers:{'Content-Type':'text/plain'},body:body});
    if(!r.ok)throw new Error('HTTP '+r.status);
    cards=clean;render();
    setStatus('Saved '+clean.length+' card(s) to the SD card. Tap Restart on the device.','ok');
  }catch(err){
    setStatus('Save failed: '+err.message,'err');
  }
});

// Headerless CSV. Column 1 = front, column 2 = back; extra columns ignored.
// Handles quoted fields, "" escapes, and commas/newlines inside quotes.
function parseCSV(text){
  const out=[];
  let row=[],field='',q=false;
  for(let i=0;i<text.length;i++){
    const ch=text[i];
    if(q){
      if(ch==='"'){if(text[i+1]==='"'){field+='"';i++;}else q=false;}
      else field+=ch;
    }else if(ch==='"')q=true;
    else if(ch===','){row.push(field);field='';}
    else if(ch==='\n'||ch==='\r'){
      if(ch==='\r'&&text[i+1]==='\n')i++;
      row.push(field);field='';
      if(row.some(x=>x!==''))out.push(row);
      row=[];
    }else field+=ch;
  }
  if(field!==''||row.length){row.push(field);if(row.some(x=>x!==''))out.push(row);}
  return out.map(r=>({f:(r[0]||'').trim(),b:(r[1]||'').trim()}));
}

fetch('/api/cards').then(r=>r.text()).then(t=>{
  cards=t.split('\n').filter(l=>l!=='').map(l=>{
    const i=l.indexOf('\t');
    return i<0?{f:l,b:''}:{f:l.slice(0,i),b:l.slice(i+1)};
  });
  render();
}).catch(()=>{setStatus('Could not load current cards.','err');render();});
</script>
</body>
</html>)HTML";

// ---- Touch (FT6336) ----
static void ft6336Reset() {
  pinMode(PIN_TP_RST, OUTPUT);
  digitalWrite(PIN_TP_RST, HIGH); delay(20);
  digitalWrite(PIN_TP_RST, LOW);  delay(20);
  digitalWrite(PIN_TP_RST, HIGH); delay(150);
}

static bool ft6336Read(uint8_t reg, uint8_t *buf, uint8_t len) {
  Wire.beginTransmission(FT6336_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)FT6336_ADDR, (int)len) != len) return false;
  for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}

static bool readTouch(int &x, int &y) {
  uint8_t n = 0;
  if (!ft6336Read(0x02, &n, 1)) return false;
  if ((n & 0x0F) == 0) return false;

  uint8_t b[4];
  if (!ft6336Read(0x03, b, 4)) return false;   // XH, XL, YH, YL
  int rx = (((int)(b[0] & 0x0F)) << 8) | b[1];
  int ry = (((int)(b[2] & 0x0F)) << 8) | b[3];

  if (SWAP_XY) { int t = rx; rx = ry; ry = t; }
  if (FLIP_X) rx = (W - 1) - rx;
  if (FLIP_Y) ry = (H - 1) - ry;

  x = constrain(rx, 0, W - 1);
  y = constrain(ry, 0, H - 1);
  return true;
}

// One "true" per fresh finger-down, with the touch-down point. The single
// static is fine: loop() and syncLoop() never run at the same time.
static bool pollTapDown(int &x, int &y) {
  static bool was = false;
  bool now = readTouch(x, y);
  bool down = now && !was;
  was = now;
  return down;
}

static bool inRect(int x, int y, int rx, int ry, int rw, int rh) {
  return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

// ---- Text helpers (same pattern as 005) ----
static void selectFont(const uint8_t *font) {
  u8f.setFont(font);
  u8f.setFontMode(1);
}

static void printCentered(int cx, int baselineY, const char *text) {
  int w = u8f.getUTF8Width(text);
  u8f.setCursor(cx - w / 2, baselineY);
  u8f.print(text);
}

static void drawDashedLine(int y) {
  for (int x = 0; x < W; x += 8) {
    display.drawLine(x, y, min(x + 4, W - 1), y, GxEPD_BLACK);
  }
}

// ~14 px gear glyph centered on (cx, cy): eight spokes plus a hub ring.
static void drawGear(int cx, int cy) {
  for (int i = 0; i < 8; i++) {
    float a = i * (float)PI / 4.0f;
    int dx0 = (int)lroundf(3 * cosf(a)), dy0 = (int)lroundf(3 * sinf(a));
    int dx1 = (int)lroundf(7 * cosf(a)), dy1 = (int)lroundf(7 * sinf(a));
    display.drawLine(cx + dx0, cy + dy0, cx + dx1, cy + dy1, GxEPD_BLACK);
  }
  display.fillCircle(cx, cy, 4, GxEPD_WHITE);
  display.drawCircle(cx, cy, 4, GxEPD_BLACK);
  display.fillCircle(cx, cy, 1, GxEPD_BLACK);
}

// Instant touch-down acknowledgment: inverts a box via a fast partial refresh
// before the slower real transition. `label` may be nullptr (e.g. the gear).
static void flashBox(int x, int y, int w, int h, const char *label, const uint8_t *font) {
  display.setPartialWindow(x, y, w, h);
  display.firstPage();
  do {
    display.fillScreen(GxEPD_BLACK);
    if (label) {
      selectFont(font);
      u8f.setForegroundColor(GxEPD_WHITE);
      printCentered(x + w / 2, y + h / 2 + 5, label);
    }
  } while (display.nextPage());
  u8f.setForegroundColor(GxEPD_BLACK);
}

// ---- Screen drawing ----
static void drawCardPrompt() {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);

    if (cards.empty()) {
      selectFont(FONT_SMALL);
      printCentered(MID_X, 74, "No flashcards on SD.");
      printCentered(MID_X, 94, "Tap the gear, then");
      printCentered(MID_X, 110, "\"Sync mode\", to add some.");
    } else {
      selectFont(FONT_BIG);
      u8f.setCursor(6, ROW_WORD_Y);
      u8f.print(cards[currentIndex].front);

      display.drawLine(0, DIV_Y, W - 1, DIV_Y, GxEPD_BLACK);
      display.drawRect(WIDE_BTN_X, BTN_Y, WIDE_BTN_W, BTN_H, GxEPD_BLACK);
      selectFont(FONT_BIG);
      printCentered(WIDE_CX, BTN_LABEL_Y, "Reveal");
    }

    drawGear(GEAR_CX, GEAR_CY);
  } while (display.nextPage());
}

static void drawCardAnswer() {
  display.setPartialWindow(0, 0, W, H);
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);

    selectFont(FONT_BIG);
    u8f.setCursor(6, ROW_WORD_Y);
    u8f.print(cards[currentIndex].front);

    drawDashedLine(DASH_Y);

    selectFont(FONT_BIG);
    u8f.setCursor(6, ROW_MEAN_Y);
    u8f.print(cards[currentIndex].back);

    display.drawLine(0, DIV_Y, W - 1, DIV_Y, GxEPD_BLACK);
    display.drawLine(MID_X, DIV_Y, MID_X, H - 1, GxEPD_BLACK);
    display.drawRect(LEFT_BTN_X, BTN_Y, LEFT_BTN_W, BTN_H, GxEPD_BLACK);
    display.drawRect(RIGHT_BTN_X, BTN_Y, RIGHT_BTN_W, BTN_H, GxEPD_BLACK);

    selectFont(FONT_SMALL);
    printCentered(LEFT_CX, BTN_LABEL_Y, "Wrong");
    printCentered(RIGHT_CX, BTN_LABEL_Y, "Correct");
  } while (display.nextPage());
}

static void drawSettings() {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);

    selectFont(FONT_BIG);
    printCentered(MID_X, 28, "Settings");

    display.drawRect(SET_SYNC_X, SET_SYNC_Y, SET_SYNC_W, SET_SYNC_H, GxEPD_BLACK);
    printCentered(SET_SYNC_X + SET_SYNC_W / 2, SET_SYNC_Y + SET_SYNC_H / 2 + 5, "Sync mode");

    display.drawRect(SET_BACK_X, SET_BACK_Y, SET_BACK_W, SET_BACK_H, GxEPD_BLACK);
    printCentered(SET_BACK_X + SET_BACK_W / 2, SET_BACK_Y + SET_BACK_H / 2 + 5, "Back");
  } while (display.nextPage());
}

static void drawSyncScreen(IPAddress ip) {
  String url  = "http://" + ip.toString();
  String ssid = String("   \"") + AP_SSID + "\"";
  String pass = String("   pass: ") + AP_PASS;
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);

    selectFont(FONT_BIG);
    printCentered(MID_X, 22, "Sync mode");

    selectFont(FONT_SMALL);
    printCentered(MID_X, 46, "1. Join Wi-Fi");
    printCentered(MID_X, 62, ssid.c_str());
    printCentered(MID_X, 78, pass.c_str());
    printCentered(MID_X, 100, "2. Open in browser");
    printCentered(MID_X, 116, url.c_str());
    printCentered(MID_X, 140, "3. Edit + Save, then:");

    display.drawRect(SYNC_RST_X, SYNC_RST_Y, SYNC_RST_W, SYNC_RST_H, GxEPD_BLACK);
    selectFont(FONT_BIG);
    printCentered(SYNC_RST_X + SYNC_RST_W / 2, SYNC_RST_Y + SYNC_RST_H / 2 + 5, "Restart");
  } while (display.nextPage());
}

static void drawFatalSd() {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    selectFont(FONT_BIG);
    printCentered(MID_X, 90, "No SD card");
    selectFont(FONT_SMALL);
    printCentered(MID_X, 116, "Insert a FAT32 card,");
    printCentered(MID_X, 132, "then reset.");
  } while (display.nextPage());
}

// ---- Card data ----
static bool loadCards() {
  cards.clear();
  File f = SD_MMC.open("/cards.tsv", FILE_READ);
  if (!f) { Serial.println("no /cards.tsv yet"); return false; }
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.replace("\r", "");
    if (line.isEmpty()) continue;
    int t = line.indexOf('\t');
    if (t < 0) continue;
    Flashcard c;
    c.front = line.substring(0, t);  c.front.trim();
    c.back  = line.substring(t + 1); c.back.trim();
    if (c.front.isEmpty() && c.back.isEmpty()) continue;
    cards.push_back(c);
  }
  f.close();
  Serial.printf("loaded %u card(s)\n", (unsigned)cards.size());
  return !cards.empty();
}

static void pickNextCard() {
  currentIndex = cards.empty() ? 0 : (int)random(0, (long)cards.size());
}

// ---- Power button (works on the game loop and the sync loop) ----
static void powerOff() {
  Serial.println("PWR long-press -> shutting down");
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    selectFont(FONT_BIG);
    printCentered(MID_X, 100, "Bye!");
  } while (display.nextPage());
  digitalWrite(PIN_VBAT_PWR, LOW);
  while (true) delay(1000);
}

static void handlePowerButton() {
  static bool down = false;
  static uint32_t downAt = 0;
  if (digitalRead(PIN_PWR_BTN) == LOW) {
    if (!down) { down = true; downAt = millis(); }
    else if (millis() - downAt >= PWR_LONGPRESS_MS) powerOff();
  } else {
    down = false;
  }
}

// ---- Web handlers ----
static void handleRoot() {
  server.send(200, "text/html; charset=utf-8", PAGE_HTML);
}

static void handleGetCards() {
  String out;
  if (sdOk) {
    File f = SD_MMC.open("/cards.tsv", FILE_READ);
    if (f) { out = f.readString(); f.close(); }
  }
  server.send(200, "text/plain; charset=utf-8", out);
}

static void handlePutCards() {
  if (!sdOk) { server.send(500, "text/plain", "no sd"); return; }
  String body = server.hasArg("plain") ? server.arg("plain") : String();
  File f = SD_MMC.open("/cards.tsv", FILE_WRITE);   // "w" -> truncates
  if (!f) { server.send(500, "text/plain", "open failed"); return; }
  f.print(body);
  f.close();
  Serial.printf("wrote /cards.tsv (%u bytes)\n", (unsigned)body.length());
  server.send(200, "text/plain", "ok");
}

// ---- Sync mode: game paused, AP + web server up until "Restart" ----
static void syncLoop() {
  for (;;) {
    server.handleClient();
    handlePowerButton();
    int x, y;
    if (pollTapDown(x, y) && inRect(x, y, SYNC_RST_X, SYNC_RST_Y, SYNC_RST_W, SYNC_RST_H)) {
      flashBox(SYNC_RST_X, SYNC_RST_Y, SYNC_RST_W, SYNC_RST_H, "Restart", FONT_BIG);
      delay(300);
      ESP.restart();
    }
    delay(10);
  }
}

static void enterSyncMode() {
  screen = SCREEN_SYNC;
  Serial.println("entering sync mode");

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  delay(200);
  IPAddress ip = WiFi.softAPIP();     // 192.168.4.1
  Serial.print("AP up, IP: "); Serial.println(ip);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/cards", HTTP_GET, handleGetCards);
  server.on("/api/cards", HTTP_PUT, handlePutCards);
  server.onNotFound([]() { server.send(404, "text/plain", "not found"); });
  server.begin();

  drawSyncScreen(ip);
  syncLoop();                         // never returns (exits via ESP.restart)
}

// ---- Setup / loop ----
void setup() {
  pinMode(PIN_VBAT_PWR, OUTPUT);
  digitalWrite(PIN_VBAT_PWR, HIGH);  // latch battery power first
  pinMode(PIN_PWR_BTN, INPUT_PULLUP);

  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("006_esp_s3_touch_flashcard_sd");

  pinMode(PIN_EPD_PWR, OUTPUT);
  digitalWrite(PIN_EPD_PWR, LOW);    // e-paper rail ON (active low)

  pinMode(PIN_TP_INT, INPUT_PULLUP);
  ft6336Reset();
  Wire.begin(PIN_TP_SDA, PIN_TP_SCL, 400000);

  delay(100);
  SPI.begin(PIN_EPD_SCK, -1, PIN_EPD_MOSI, PIN_EPD_CS);
  display.init(0, true, 2, false);
  display.setRotation(0);

  u8f.begin(display);
  u8f.setFontMode(1);
  u8f.setForegroundColor(GxEPD_BLACK);

  // SD on the dedicated SDMMC peripheral, 1-bit mode -- separate from the
  // e-paper SPI bus.
  SD_MMC.setPins(PIN_SD_CLK, PIN_SD_CMD, PIN_SD_D0);
  sdOk = SD_MMC.begin("/sdcard", true);
  Serial.printf("SD mount: %s\n", sdOk ? "ok" : "FAILED");

  if (!sdOk) {
    drawFatalSd();
    while (true) { handlePowerButton(); delay(50); }
  }

  loadCards();

  randomSeed(esp_random());
  pickNextCard();
  screen = SCREEN_CARD;
  phase  = PHASE_PROMPT;
  drawCardPrompt();

  Serial.println("gear (top-right) = Settings; lower half = Reveal / Wrong / Correct");
}

void loop() {
  handlePowerButton();

  int x, y;
  if (pollTapDown(x, y)) {
    Serial.printf("tap (%3d,%3d) screen=%d phase=%d\n", x, y, (int)screen, (int)phase);

    if (screen == SCREEN_CARD && phase == PHASE_PROMPT) {
      if (x >= GEAR_TOUCH_X && y < GEAR_TOUCH_H) {
        flashBox(GEAR_CX - 16, 0, 32, GEAR_CY + 16, nullptr, FONT_SMALL);
        screen = SCREEN_SETTINGS;
        drawSettings();
      } else if (y >= DIV_Y && !cards.empty()) {
        flashBox(WIDE_BTN_X, BTN_Y, WIDE_BTN_W, BTN_H, "Reveal", FONT_BIG);
        phase = PHASE_ANSWER;
        drawCardAnswer();
      }
    } else if (screen == SCREEN_CARD && phase == PHASE_ANSWER) {
      if (y >= DIV_Y && x < MID_X) {
        flashBox(LEFT_BTN_X, BTN_Y, LEFT_BTN_W, BTN_H, "Wrong", FONT_SMALL);
        Serial.println("-> Wrong");
        pickNextCard(); phase = PHASE_PROMPT; drawCardPrompt();
      } else if (y >= DIV_Y && x >= MID_X) {
        flashBox(RIGHT_BTN_X, BTN_Y, RIGHT_BTN_W, BTN_H, "Correct", FONT_SMALL);
        Serial.println("-> Correct");
        pickNextCard(); phase = PHASE_PROMPT; drawCardPrompt();
      }
    } else if (screen == SCREEN_SETTINGS) {
      if (inRect(x, y, SET_SYNC_X, SET_SYNC_Y, SET_SYNC_W, SET_SYNC_H)) {
        flashBox(SET_SYNC_X, SET_SYNC_Y, SET_SYNC_W, SET_SYNC_H, "Sync mode", FONT_BIG);
        enterSyncMode();                // does not return
      } else if (inRect(x, y, SET_BACK_X, SET_BACK_Y, SET_BACK_W, SET_BACK_H)) {
        flashBox(SET_BACK_X, SET_BACK_Y, SET_BACK_W, SET_BACK_H, "Back", FONT_BIG);
        screen = SCREEN_CARD; phase = PHASE_PROMPT; drawCardPrompt();
      }
    }
  }

  delay(30);   // ~33 Hz poll; also debounces
}
