/*
 * ╔══════════════════════════════════════════════════════════════════════╗
 * ║              POCKET-AI TERMINAL  —  Firmware v1.0                   ║
 * ║  Hardware : ESP8266 D1 Mini Lite                                     ║
 * ║  Display  : 0.91" SSD1306 OLED 128×32  (I²C: SCL=D1, SDA=D2)       ║
 * ║  Buttons  : D5=PREV  D6=NEXT  D7=SELECT  D0=FUNC (ext pull-up)      ║
 * ║  Libraries: U8g2 · ArduinoJson · ESP8266WiFi · WiFiClientSecure      ║
 * ╚══════════════════════════════════════════════════════════════════════╝
 *
 * Button contract
 *  PREV   – carousel left / menu up
 *  NEXT   – carousel right / menu down
 *  SELECT – confirm char / confirm menu item / dismiss response
 *  FUNC   – short press → backspace | long press (800 ms) → SEND
 *           (layout cycling is done via a special [⇧] carousel entry)
 *
 * Deep-sleep: D0 is GPIO16 which must be bridged to RST for wake-up.
 */

// ─── Core Libraries ────────────────────────────────────────────────────────
#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <EEPROM.h>

// ─── Network Libraries ─────────────────────────────────────────────────────
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <ESP8266HTTPClient.h>

// ─── JSON ──────────────────────────────────────────────────────────────────
#include <ArduinoJson.h>

// ═══════════════════════════════════════════════════════════════════════════
// SECTION 1 ─ HARDWARE PIN MAP
// ═══════════════════════════════════════════════════════════════════════════
#define PIN_PREV     D5   // INPUT_PULLUP
#define PIN_NEXT     D6   // INPUT_PULLUP
#define PIN_SELECT   D7   // INPUT_PULLUP
#define PIN_FUNC     D0   // External pull-up, also GPIO16 / RST bridge

// ═══════════════════════════════════════════════════════════════════════════
// SECTION 2 ─ DISPLAY
// ═══════════════════════════════════════════════════════════════════════════
// Full-buffer mode → smooth redraws; uses ~512 B of RAM for the frame buffer.
U8G2_SSD1306_128X32_UNIVISION_F_HW_I2C display(U8G2_R0, U8X8_PIN_NONE);

// ═══════════════════════════════════════════════════════════════════════════
// SECTION 3 ─ EEPROM LAYOUT  (512 bytes total)
// ═══════════════════════════════════════════════════════════════════════════
#define EE_SIZE         512
#define EE_MAGIC_VAL    0xCA
#define EE_MAGIC        0    //   1 byte
#define EE_SSID         1    //  33 bytes  (32 + NUL)
#define EE_PASS         34   //  65 bytes  (64 + NUL)
#define EE_APIKEY       99   // 101 bytes  (100 + NUL)
#define EE_TEMP         200  //   1 byte   (value × 10, e.g. 7 → 0.7)
#define EE_DARKMODE     201  //   1 byte   (0 or 1)
// 202-511 reserved for conversation snapshots

// ═══════════════════════════════════════════════════════════════════════════
// SECTION 4 ─ TIMING CONSTANTS
// ═══════════════════════════════════════════════════════════════════════════
#define DEBOUNCE_MS          40
#define LONG_PRESS_MS       800
#define IDLE_SLEEP_MS    120000UL   // 2 minutes
#define TYPEWRITER_MS        28     // ms per character reveal
#define PRED_TRIGGER_MS    1400     // pause before showing prediction
#define CAROUSEL_ANIM_MS     60     // frame delay for slide animation

// ═══════════════════════════════════════════════════════════════════════════
// SECTION 5 ─ KEYBOARD LAYOUTS
// The sentinel character '\x01' marks the layout-switch slot [⇧].
// ═══════════════════════════════════════════════════════════════════════════
// '\x01' = [⇧] shift key (displayed as an up-arrow glyph)
static const char LAYOUT_LOWER[] PROGMEM =
    "abcdefghijklmnopqrstuvwxyz .,!?\x01";
static const char LAYOUT_UPPER[] PROGMEM =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ .,!?\x01";
static const char LAYOUT_NUM[]   PROGMEM =
    "0123456789 +-*/=@#$%&()[]{}:;'\"\x01";

static const char* const LAYOUTS[3] = {
    LAYOUT_LOWER, LAYOUT_UPPER, LAYOUT_NUM
};
static const char* const LAYOUT_LABELS[3] = { "abc", "ABC", "123" };
#define LAYOUT_COUNT 3

// ═══════════════════════════════════════════════════════════════════════════
// SECTION 6 ─ PREDICTIVE TEXT
// ═══════════════════════════════════════════════════════════════════════════
static const char* const PREDICTIONS[] = {
    "the", "and", "is", "it", "what", "how", "are", "you",
    "that", "this", "for", "was", "can", "with", "have"
};
#define PRED_COUNT 15

// ═══════════════════════════════════════════════════════════════════════════
// SECTION 7 ─ APPLICATION STATE MACHINE
// ═══════════════════════════════════════════════════════════════════════════
enum class AppState : uint8_t {
    SPLASH,
    WIFI_CONNECT,
    AP_MODE,
    MAIN_MENU,
    KEYBOARD,
    CONFIRMING,     // "Send?" prompt
    SENDING,
    RESPONSE,
    SETTINGS,
    SYS_INFO,
};

static AppState g_state     = AppState::SPLASH;
static AppState g_prevState = AppState::SPLASH;

// ─── Inline state transition helper ────────────────────────────────────────
inline void goTo(AppState next) {
    g_prevState = g_state;
    g_state     = next;
}

// ═══════════════════════════════════════════════════════════════════════════
// SECTION 8 ─ GLOBAL DATA
// ═══════════════════════════════════════════════════════════════════════════

// ── Config ──────────────────────────────────────────────────────────────
char  g_ssid   [33]  = "";
char  g_pass   [65]  = "";
char  g_apiKey [101] = "";
float g_temp         = 0.7f;
bool  g_darkMode     = false;

// ── Input buffer ────────────────────────────────────────────────────────
#define MAX_INPUT 220
char    g_input[MAX_INPUT + 1] = "";
uint8_t g_inputLen             = 0;

// ── Response buffer ─────────────────────────────────────────────────────
#define MAX_RESP 700
char     g_resp   [MAX_RESP + 1] = "";
uint16_t g_respLen               = 0;
uint16_t g_twPos                 = 0;   // typewriter position
uint32_t g_twLastMs              = 0;

// ── Keyboard state ──────────────────────────────────────────────────────
uint8_t  g_layout     = 0;
int16_t  g_carIdx     = 0;      // current carousel index
int16_t  g_carAnim    = 0;      // pixel offset for slide animation (×1)
int8_t   g_animDir    = 0;      // +1 right, -1 left, 0 still
uint32_t g_animStart  = 0;

// ── Prediction state ────────────────────────────────────────────────────
int8_t   g_predIdx    = -1;     // -1 = no prediction
uint32_t g_lastKeyMs  = 0;

// ── Menu state ──────────────────────────────────────────────────────────
static const char* const MENU_ITEMS[]     = { "New Chat", "Settings", "System Info" };
static const char* const SETTINGS_ITEMS[] = {
    "API Key (WebUI)", "Temperature", "Dark Mode",
    "Reset WiFi",      "< Back"
};
#define MENU_COUNT     3
#define SETTINGS_COUNT 5
int8_t g_menuIdx     = 0;
int8_t g_settingsIdx = 0;

// ── Conversation history (ring) ──────────────────────────────────────────
#define MAX_HIST     3
#define HIST_MSG_LEN 90
struct HistEntry { char role[12]; char text[HIST_MSG_LEN + 1]; };
static HistEntry g_hist[MAX_HIST];
static uint8_t   g_histCount = 0;

// ── Activity timestamp for deep-sleep ───────────────────────────────────
static uint32_t g_lastActivity = 0;

// ═══════════════════════════════════════════════════════════════════════════
// SECTION 9 ─ BUTTON DRIVER  (non-blocking, edge-detect + long-press)
// ═══════════════════════════════════════════════════════════════════════════
struct BtnState {
    uint8_t  pin;
    bool     isDown;       // current debounced state
    bool     prevDown;
    bool     edgeFall;     // true for ONE loop() cycle on press
    bool     edgeRise;     // true for ONE loop() cycle on release
    bool     longFired;    // true after long press threshold, cleared on release
    uint32_t downSince;
};

static BtnState g_btns[4] = {
    { PIN_PREV,   false, false, false, false, false, 0 },
    { PIN_NEXT,   false, false, false, false, false, 0 },
    { PIN_SELECT, false, false, false, false, false, 0 },
    { PIN_FUNC,   false, false, false, false, false, 0 },
};
#define B_PREV   0
#define B_NEXT   1
#define B_SEL    2
#define B_FUNC   3

static void pollButtons() {
    uint32_t now = millis();
    for (auto& b : g_btns) {
        bool raw = (digitalRead(b.pin) == LOW);
        b.edgeFall = false;
        b.edgeRise = false;

        if (raw != b.isDown) {
            // Simple debounce: require state stable for DEBOUNCE_MS
            // We track via prevDown + timestamp
        }

        // Basic debounce using prevDown
        if (raw && !b.prevDown) {
            // Falling edge (button pressed)
            b.isDown    = true;
            b.edgeFall  = true;
            b.longFired = false;
            b.downSince = now;
            g_lastActivity = now;
        } else if (!raw && b.prevDown) {
            // Rising edge (button released)
            b.isDown   = false;
            b.edgeRise = true;
        }
        b.prevDown = raw;

        // Long press
        if (b.isDown && !b.longFired && (now - b.downSince) >= LONG_PRESS_MS) {
            b.longFired = true;
        }
    }
}

// Convenience accessors
inline bool btnPress  (int i) { return g_btns[i].edgeFall; }
inline bool btnRelease(int i) { return g_btns[i].edgeRise; }
inline bool btnLong   (int i) { return g_btns[i].longFired && g_btns[i].isDown; }

// ═══════════════════════════════════════════════════════════════════════════
// SECTION 10 ─ EEPROM HELPERS
// ═══════════════════════════════════════════════════════════════════════════
static void eeReadStr(int addr, char* buf, int maxLen) {
    for (int i = 0; i < maxLen; i++) buf[i] = char(EEPROM.read(addr + i));
    buf[maxLen] = '\0';
}
static void eeWriteStr(int addr, const char* buf, int maxLen) {
    for (int i = 0; i < maxLen; i++)
        EEPROM.write(addr + i, uint8_t(buf[i]));
}

static void loadConfig() {
    EEPROM.begin(EE_SIZE);
    if (EEPROM.read(EE_MAGIC) != EE_MAGIC_VAL) return;   // first boot
    eeReadStr(EE_SSID,   g_ssid,   32);
    eeReadStr(EE_PASS,   g_pass,   64);
    eeReadStr(EE_APIKEY, g_apiKey, 100);
    uint8_t tv = EEPROM.read(EE_TEMP);
    g_temp     = (tv == 0xFF) ? 0.7f : float(tv) / 10.0f;
    g_darkMode = (EEPROM.read(EE_DARKMODE) == 1);
}

static void saveConfig() {
    EEPROM.write(EE_MAGIC, EE_MAGIC_VAL);
    eeWriteStr(EE_SSID,   g_ssid,   32);
    eeWriteStr(EE_PASS,   g_pass,   64);
    eeWriteStr(EE_APIKEY, g_apiKey, 100);
    EEPROM.write(EE_TEMP,     uint8_t(g_temp * 10.0f));
    EEPROM.write(EE_DARKMODE, g_darkMode ? 1 : 0);
    EEPROM.commit();
}

// ═══════════════════════════════════════════════════════════════════════════
// SECTION 11 ─ DISPLAY HELPERS
// ═══════════════════════════════════════════════════════════════════════════
static void dispHeader(const char* title) {
    display.drawBox(0, 0, 128, 10);
    display.setDrawColor(0);
    display.setFont(u8g2_font_5x7_tr);
    int16_t w = display.getStrWidth(title);
    display.drawStr((128 - w) / 2, 8, title);
    display.setDrawColor(1);
}

static void dispStr(uint8_t x, uint8_t y, const char* s) {
    display.drawStr(x, y, s);
}

static void dispCentered(uint8_t y, const char* s) {
    int16_t w = display.getStrWidth(s);
    display.drawStr((128 - w) / 2, y, s);
}

// ─── Word-wrap renderer ──────────────────────────────────────────────────
// Renders up to `visibleLen` chars of `text` into 128×32, line-height 9px.
// Returns total line count (for scroll calculation).
static uint8_t renderWrapped(const char* text, uint16_t visibleLen,
                              uint8_t startLine, uint8_t maxLines) {
    display.setFont(u8g2_font_5x7_tr);
    const uint8_t LINE_PX  = 9;
    const uint8_t LINE_CHR = 21;   // ~128px / 6px per char

    uint8_t totalLines = 0;
    char    line[LINE_CHR + 1];
    uint16_t pos = 0;

    while (pos < visibleLen) {
        uint16_t end = pos + LINE_CHR;
        if (end > visibleLen) end = visibleLen;

        // Try to break at space
        if (end < visibleLen) {
            uint16_t brk = end;
            while (brk > pos && text[brk] != ' ') --brk;
            if (brk > pos) end = brk;
        }

        uint16_t len = end - pos;
        memcpy(line, text + pos, len);
        line[len] = '\0';

        if (totalLines >= startLine && totalLines < startLine + maxLines) {
            uint8_t row = uint8_t(totalLines - startLine);
            display.drawStr(0, (row + 1) * LINE_PX, line);
        }
        totalLines++;
        pos = end + (text[end] == ' ' ? 1 : 0);
    }
    return totalLines;
}

// ═══════════════════════════════════════════════════════════════════════════
// SECTION 12 ─ SCREEN RENDERERS
// ═══════════════════════════════════════════════════════════════════════════

// ── 12a  Splash ─────────────────────────────────────────────────────────
static void screenSplash() {
    display.clearBuffer();
    display.setFont(u8g2_font_7x13B_tr);
    dispCentered(15, "Pocket-AI");
    display.setFont(u8g2_font_5x7_tr);
    dispCentered(27, "v1.0  booting...");
    display.sendBuffer();
}

// ── 12b  WiFi Connecting ─────────────────────────────────────────────────
static void screenConnecting(const char* ssid) {
    display.clearBuffer();
    display.setFont(u8g2_font_5x7_tr);
    dispCentered(10, "Connecting to:");
    dispCentered(20, ssid);
    static uint8_t dots = 0;
    static uint32_t last = 0;
    if (millis() - last > 400) { dots = (dots + 1) % 4; last = millis(); }
    char d[5] = ""; for (int i = 0; i < dots; i++) strcat(d, ".");
    dispCentered(30, d);
    display.sendBuffer();
}

// ── 12c  AP Mode ─────────────────────────────────────────────────────────
static void screenAPMode() {
    display.clearBuffer();
    dispHeader("WiFi Setup");
    display.setFont(u8g2_font_5x7_tr);
    dispCentered(19, "Join: PocketAI-Setup");
    dispCentered(29, "then visit 192.168.4.1");
    display.sendBuffer();
}

// ── 12d  Main Menu ───────────────────────────────────────────────────────
static void screenMainMenu() {
    display.clearBuffer();
    dispHeader("POCKET-AI");
    display.setFont(u8g2_font_6x10_tr);
    // Current item (highlighted)
    display.drawBox(0, 10, 128, 12);
    display.setDrawColor(0);
    dispStr(4, 20, ">");
    dispStr(13, 20, MENU_ITEMS[g_menuIdx]);
    display.setDrawColor(1);
    // Next item
    display.setFont(u8g2_font_5x7_tr);
    int8_t nextIdx = (g_menuIdx + 1) % MENU_COUNT;
    dispStr(13, 31, MENU_ITEMS[nextIdx]);
    display.sendBuffer();
}

// ── 12e  Settings ────────────────────────────────────────────────────────
static void screenSettings() {
    display.clearBuffer();
    dispHeader("SETTINGS");
    display.setFont(u8g2_font_6x10_tr);
    display.drawBox(0, 10, 128, 12);
    display.setDrawColor(0);
    dispStr(4, 20, ">");
    dispStr(13, 20, SETTINGS_ITEMS[g_settingsIdx]);
    display.setDrawColor(1);
    display.setFont(u8g2_font_5x7_tr);
    int8_t next = (g_settingsIdx + 1) % SETTINGS_COUNT;
    dispStr(13, 31, SETTINGS_ITEMS[next]);
    // Show current temperature value inline
    if (g_settingsIdx == 1) {
        char tv[8]; snprintf(tv, sizeof(tv), "[%.1f]", g_temp);
        display.setFont(u8g2_font_5x7_tr);
        display.setDrawColor(0);
        dispStr(100, 20, tv);
        display.setDrawColor(1);
    }
    display.sendBuffer();
}

// ── 12f  Keyboard (Linear Carousel) ─────────────────────────────────────
static void screenKeyboard() {
    display.clearBuffer();

    // ── Top 11px: text input area ────────────────────────────────────────
    display.setFont(u8g2_font_5x7_tr);

    // Show tail of input (last 21 chars)
    char disp[23] = "";
    if (g_inputLen > 21) {
        memcpy(disp, g_input + g_inputLen - 21, 21);
        disp[21] = '\0';
    } else {
        memcpy(disp, g_input, g_inputLen);
        disp[g_inputLen] = '\0';
    }
    // Blinking cursor
    bool cursor = ((millis() / 450) & 1) == 0;
    if (cursor && strlen(disp) < 22) strcat(disp, "|");
    dispStr(0, 8, disp);

    // Layout label (top-right)
    display.setFont(u8g2_font_4x6_tr);
    dispStr(108, 8, LAYOUT_LABELS[g_layout]);

    // Separator
    display.drawLine(0, 11, 127, 11);

    // ── Prediction bubble ─────────────────────────────────────────────────
    if (g_predIdx >= 0) {
        const char* pred = PREDICTIONS[g_predIdx];
        display.setFont(u8g2_font_5x7_tr);
        uint8_t pw = display.getStrWidth(pred) + 6;
        display.drawRFrame(1, 13, pw, 10, 2);
        dispStr(4, 21, pred);
        display.setFont(u8g2_font_4x6_tr);
        dispStr(pw + 4, 21, "SEL=use PREV/NEXT=cycle");
        display.sendBuffer();
        return;
    }

    // ── Carousel strip (bottom 20px, y=12..31) ───────────────────────────
    // Architecture: draw chars at offset positions from center (x=64).
    // Centre slot is 16px wide (highlighted box); side slots 11px each.
    // Animation: g_carAnim slides from ±11 to 0 over CAROUSEL_ANIM_MS ms.

    const char* layout  = LAYOUTS[g_layout];
    const int   llen    = strlen_P(layout);
    const uint8_t CY    = 30;   // baseline y
    const uint8_t CTRX  = 64;

    // Animate slide
    int16_t animOff = 0;
    if (g_animDir != 0) {
        uint32_t elapsed = millis() - g_animStart;
        if (elapsed >= CAROUSEL_ANIM_MS) {
            g_animDir = 0;
        } else {
            float t = float(elapsed) / CAROUSEL_ANIM_MS;
            animOff = int16_t(g_animDir * 11 * (1.0f - t));
        }
    }

    for (int off = -4; off <= 4; off++) {
        int ci = ((g_carIdx + off) % llen + llen) % llen;
        char ch = pgm_read_byte(&layout[ci]);

        int16_t x = CTRX + off * 11 + animOff;
        if (x < 2 || x > 125) continue;

        if (off == 0) {
            // Centre: large, inverted highlight box
            display.setFont(u8g2_font_7x13B_tr);
            display.drawBox(CTRX - 8, 13, 16, 19);
            display.setDrawColor(0);
            if (ch == '\x01') {
                // Draw shift arrow glyph
                display.drawStr(CTRX - 5, CY, "\x1E");   // or use "^"
                // Fallback plain text
                display.setFont(u8g2_font_5x7_tr);
                display.drawStr(CTRX - 5, CY, "[S]");
            } else {
                char cs[2] = { ch, '\0' };
                uint8_t cw = display.getStrWidth(cs);
                display.drawStr(x - cw / 2, CY, cs);
            }
            display.setDrawColor(1);
        } else {
            // Side chars: small, dimmer (no fill)
            display.setFont(u8g2_font_5x7_tr);
            char cs[2] = { (ch == '\x01' ? '^' : ch), '\0' };
            uint8_t cw = display.getStrWidth(cs);
            display.drawStr(x - cw / 2, CY - 1, cs);
        }
    }

    display.sendBuffer();
}

// ── 12g  Sending / Thinking ──────────────────────────────────────────────
static void screenSending() {
    static uint8_t  frame   = 0;
    static uint32_t lastFr  = 0;
    if (millis() - lastFr > 350) { frame = (frame + 1) % 4; lastFr = millis(); }

    display.clearBuffer();
    display.setFont(u8g2_font_5x7_tr);
    dispCentered(12, "AI is thinking");
    // Animated ellipsis
    char d[5] = ""; for (uint8_t i = 0; i < frame; i++) strcat(d, ".");
    dispCentered(24, d);
    // Wi-Fi signal bars (decorative)
    for (uint8_t i = 0; i < 4; i++) {
        uint8_t h = 2 + i * 3;
        display.drawBox(108 + i * 4, 32 - h, 3, h);
    }
    display.sendBuffer();
}

// ── 12h  Response (typewriter + auto-scroll) ─────────────────────────────
static void screenResponse() {
    // Advance typewriter
    uint32_t now = millis();
    if (g_twPos < g_respLen && now - g_twLastMs >= TYPEWRITER_MS) {
        g_twPos++;
        g_twLastMs = now;
    }

    display.clearBuffer();

    // Calculate which lines to show
    const uint8_t MAX_LINES = 3;
    uint8_t total = renderWrapped(g_resp, g_twPos, 0, 255);  // count only
    uint8_t startLine = (total > MAX_LINES) ? total - MAX_LINES : 0;
    renderWrapped(g_resp, g_twPos, startLine, MAX_LINES);

    // Status bar at bottom
    display.setFont(u8g2_font_4x6_tr);
    if (g_twPos >= g_respLen) {
        dispStr(0,  32, "[SEL=menu]");
        // Scroll bar done
        display.drawBox(120, 26, 7, 6);
        display.setDrawColor(0);
        dispStr(121, 32, "OK");
        display.setDrawColor(1);
    } else {
        // Progress dots
        uint8_t pct = uint8_t(float(g_twPos) / float(g_respLen) * 127);
        display.drawBox(0, 31, pct, 1);
    }
    display.sendBuffer();
}

// ── 12i  System Info ─────────────────────────────────────────────────────
static void screenSysInfo() {
    display.clearBuffer();
    dispHeader("SYSTEM INFO");
    display.setFont(u8g2_font_5x7_tr);
    char buf[32];
    snprintf(buf, sizeof(buf), "Free RAM: %u B", (unsigned)ESP.getFreeHeap());
    dispStr(0, 19, buf);
    snprintf(buf, sizeof(buf), "WiFi: %d dBm  %s",
             (int)WiFi.RSSI(), WiFi.localIP().toString().c_str());
    dispStr(0, 29, buf);
    display.sendBuffer();
}

// ── 12j  Confirm Send prompt ─────────────────────────────────────────────
static void screenConfirm() {
    display.clearBuffer();
    display.setFont(u8g2_font_5x7_tr);
    dispCentered(10, "Send this message?");
    // Show last 20 chars of input
    char preview[22] = "\"";
    uint8_t start = (g_inputLen > 18) ? g_inputLen - 18 : 0;
    strncat(preview, g_input + start, 18);
    strcat(preview, "\"");
    dispCentered(21, preview);
    dispCentered(31, "[SEL=YES  FUNC=NO]");
    display.sendBuffer();
}

// ═══════════════════════════════════════════════════════════════════════════
// SECTION 13 ─ CAPTIVE-PORTAL WEB SERVER
// ═══════════════════════════════════════════════════════════════════════════
static ESP8266WebServer g_webServer(80);
static DNSServer         g_dns;

// Stored in PROGMEM to save RAM
static const char PORTAL_HTML[] PROGMEM = R"(
<!DOCTYPE html>
<html><head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Pocket-AI Setup</title>
<style>
  *{box-sizing:border-box}
  body{margin:0;padding:24px 16px;background:#111;color:#0f0;font-family:monospace}
  h1{color:#0f0;font-size:20px;margin-bottom:4px}
  p{color:#0a0;font-size:12px;margin:0 0 16px}
  label{display:block;font-size:13px;margin-bottom:4px}
  input{width:100%;padding:8px;background:#1a1a1a;color:#0f0;
        border:1px solid #0f0;border-radius:4px;font-family:monospace;
        font-size:14px;margin-bottom:12px}
  button{width:100%;padding:12px;background:#0f0;color:#111;
         border:none;border-radius:4px;font-size:16px;font-weight:bold;cursor:pointer}
  button:active{background:#0a0}
</style></head>
<body>
<h1>&#x1F916; Pocket-AI Setup</h1>
<p>Enter your WiFi credentials and API key below.</p>
<form method="POST" action="/save">
  <label>WiFi Network (SSID)</label>
  <input name="ssid" placeholder="MyNetwork" required>
  <label>WiFi Password</label>
  <input name="pass" type="password" placeholder="leave blank if open">
  <label>Groq or OpenAI API Key</label>
  <input name="apikey" placeholder="gsk_... or sk-...">
  <button type="submit">&#x2714; Save &amp; Connect</button>
</form>
</body></html>
)";

static const char PORTAL_OK[] PROGMEM = R"(
<!DOCTYPE html><html><body style="background:#111;color:#0f0;font-family:monospace;
text-align:center;padding-top:60px">
<h2>&#x2714; Saved!</h2><p>Device is rebooting. Reconnect to your normal WiFi.</p>
</body></html>
)";

static void handlePortalRoot() {
    String html;
    html.reserve(strlen_P(PORTAL_HTML));
    // Read from PROGMEM
    PGM_P p = PORTAL_HTML;
    char c;
    while ((c = pgm_read_byte(p++))) html += c;
    g_webServer.send(200, "text/html", html);
}

static void handlePortalSave() {
    if (g_webServer.hasArg("ssid"))
        strncpy(g_ssid,   g_webServer.arg("ssid").c_str(),   32);
    if (g_webServer.hasArg("pass"))
        strncpy(g_pass,   g_webServer.arg("pass").c_str(),   64);
    if (g_webServer.hasArg("apikey"))
        strncpy(g_apiKey, g_webServer.arg("apikey").c_str(), 100);
    saveConfig();

    String ok;
    PGM_P p = PORTAL_OK;
    char c;
    while ((c = pgm_read_byte(p++))) ok += c;
    g_webServer.send(200, "text/html", ok);
    delay(1500);
    ESP.restart();
}

static void handlePortalNotFound() {
    g_webServer.sendHeader("Location", "http://192.168.4.1/");
    g_webServer.send(302, "text/plain", "");
}

static void startAPMode() {
    WiFi.mode(WIFI_AP);
    WiFi.softAP("PocketAI-Setup", "12345678");
    g_dns.start(53, "*", IPAddress(192, 168, 4, 1));
    g_webServer.on("/",     HTTP_GET,  handlePortalRoot);
    g_webServer.on("/save", HTTP_POST, handlePortalSave);
    g_webServer.onNotFound(handlePortalNotFound);
    g_webServer.begin();
    goTo(AppState::AP_MODE);
}

// ═══════════════════════════════════════════════════════════════════════════
// SECTION 14 ─ WIFI CONNECTION
// ═══════════════════════════════════════════════════════════════════════════
static bool connectWiFi() {
    if (g_ssid[0] == '\0') return false;
    WiFi.mode(WIFI_STA);
    WiFi.begin(g_ssid, g_pass);
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > 12000) return false;
        screenConnecting(g_ssid);
        delay(200);
    }
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// SECTION 15 ─ API CALL  (Groq / OpenAI compatible)
// ═══════════════════════════════════════════════════════════════════════════
/*
 * Configuration: swap host/path/model for OpenAI or another provider.
 *
 *  Groq  (fast, free tier) :  api.groq.com   /openai/v1/chat/completions
 *  OpenAI                  :  api.openai.com  /v1/chat/completions
 *
 *  client.setInsecure() skips certificate verification.
 *  For production, pin the root CA cert instead.
 */
#define API_HOST   "api.groq.com"
#define API_PATH   "/openai/v1/chat/completions"
#define API_MODEL  "llama-3.1-8b-instant"

static bool callAPI() {
    g_respLen = 0;
    g_twPos   = 0;
    g_resp[0] = '\0';

    // ── Guard: need at least 22 KB free heap for TLS ──────────────────────
    uint32_t freeHeap = ESP.getFreeHeap();
    Serial.printf("[API] Free heap before call: %u B\n", freeHeap);
    if (freeHeap < 22000) {
        Serial.println(F("[API] ERR: not enough heap for TLS"));
        strncpy(g_resp, "[ERR] Low memory. Restart device.", MAX_RESP);
        g_respLen = strlen(g_resp);
        return false;
    }

    // ── Guard: WiFi must be connected ────────────────────────────────────
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println(F("[API] ERR: WiFi not connected"));
        strncpy(g_resp, "[ERR] WiFi lost. Check router.", MAX_RESP);
        g_respLen = strlen(g_resp);
        return false;
    }

    // ── Build JSON payload ────────────────────────────────────────────────
    static char body[700];

    {
        StaticJsonDocument<640> doc;
        doc["model"]       = API_MODEL;
        doc["max_tokens"]  = 200;
        doc["temperature"] = g_temp;
        doc["stream"]      = false;

        JsonArray msgs = doc.createNestedArray("messages");

        { JsonObject s = msgs.createNestedObject();
          s["role"]    = "system";
          s["content"] = "Concise AI on a tiny OLED. Max 180 chars. No markdown."; }

        for (uint8_t i = 0; i < g_histCount; i++) {
            JsonObject h = msgs.createNestedObject();
            h["role"]    = g_hist[i].role;
            h["content"] = g_hist[i].text;
        }

        { JsonObject u = msgs.createNestedObject();
          u["role"]    = "user";
          u["content"] = g_input; }

        size_t blen = serializeJson(doc, body, sizeof(body));
        Serial.printf("[API] JSON body (%u B): %.80s...\n", blen, body);
    }

    // ── Retry loop (up to 3 attempts) ────────────────────────────────────
    String url = String("https://") + API_HOST + API_PATH;
    bool   success = false;

    for (uint8_t attempt = 1; attempt <= 3 && !success; attempt++) {
        Serial.printf("[API] Attempt %u/3 — heap: %u B\n",
                      attempt, ESP.getFreeHeap());

        if (attempt > 1) {
            for (uint8_t i = 0; i < 25; i++) {
                screenSending();
                delay(50);
                ESP.wdtFeed();
            }
        }

        WiFiClientSecure client;
        client.setInsecure();

        // ═══════════════════════════════════════════════════════════════════
        // THE FIX: Force TLS 1.2 ONLY.
        //
        // ESP8266 Arduino 3.x defaults to max version BR_TLS13.
        // bearSSL's TLS 1.3 ClientHello sends a malformed
        // supported_versions extension that Cloudflare (fronting
        // api.groq.com) rejects with "fatal alert – Decoding err".
        //
        // Clamping to TLS 1.2 sends a clean ClientHello that
        // Cloudflare accepts on every attempt.
        // ═══════════════════════════════════════════════════════════════════
        client.setSSLVersion(BR_TLS12, BR_TLS12);

        HTTPClient http;
        http.setTimeout(20000);

        if (!http.begin(client, url)) {
            Serial.println(F("[API] HTTP begin failed"));
            continue;
        }

        http.addHeader("Content-Type",  "application/json");
        http.addHeader("Authorization", String("Bearer ") + String(g_apiKey));

        ESP.wdtFeed();
        screenSending();
        int httpCode = http.POST((uint8_t*)body, strlen(body));
        ESP.wdtFeed();

        Serial.printf("[API] HTTP response code: %d\n", httpCode);

        if (httpCode <= 0) {
            Serial.printf("[API] HTTP error: %s\n",
                          http.errorToString(httpCode).c_str());
            http.end();
            continue;
        }

        String responseBody = http.getString();
        http.end();
        success = true;

        Serial.printf("[API] Body (%u B): %.120s\n",
                      responseBody.length(), responseBody.c_str());

        if (httpCode != 200) {
            Serial.printf("[API] Non-200 response: %d\n", httpCode);
            snprintf(g_resp, MAX_RESP, "[ERR] HTTP %d: %.60s",
                     httpCode, responseBody.c_str());
            g_respLen = strlen(g_resp);
            return false;
        }

               // Filter: only parse the content string, skip id/model/usage to save RAM
        StaticJsonDocument<128> filter;
        filter["choices"][0]["message"]["content"] = true;

        StaticJsonDocument<512> resp;
        DeserializationError err = deserializeJson(resp, responseBody, 
                                                   DeserializationOption::Filter(filter));
        if (err) {
            Serial.printf("[API] JSON parse error: %s\n", err.c_str());
            snprintf(g_resp, MAX_RESP, "[ERR] JSON: %s | %.60s",
                     err.c_str(), responseBody.c_str());
            g_respLen = strlen(g_resp);
            return false;
        }

        const char* content = resp["choices"][0]["message"]["content"];
        if (!content) {
            const char* apiErr = resp["error"]["message"];
            Serial.printf("[API] No content. Error: %s\n",
                          apiErr ? apiErr : "none");
            snprintf(g_resp, MAX_RESP, "[ERR] %s",
                     apiErr ? apiErr : "Empty AI response.");
            g_respLen = strlen(g_resp);
            return false;
        }

        strncpy(g_resp, content, MAX_RESP);
        g_resp[MAX_RESP] = '\0';
        g_respLen = strlen(g_resp);
        Serial.printf("[API] Response: %s\n", g_resp);
    }

    if (!success) {
        strncpy(g_resp, "[ERR] Cannot reach API. Check WiFi/key.", MAX_RESP);
        g_respLen = strlen(g_resp);
        return false;
    }

    // ── Store in history ring ─────────────────────────────────────────────
    if (g_histCount >= MAX_HIST) {
        memmove(&g_hist[0], &g_hist[1], sizeof(HistEntry) * (MAX_HIST - 1));
        g_histCount = MAX_HIST - 1;
    }
    snprintf(g_hist[g_histCount].role, 12, "user");
    strncpy(g_hist[g_histCount].text, g_input, HIST_MSG_LEN);
    g_histCount++;

    if (g_histCount < MAX_HIST) {
        snprintf(g_hist[g_histCount].role, 12, "assistant");
        strncpy(g_hist[g_histCount].text, g_resp, HIST_MSG_LEN);
        g_histCount++;
    }

    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// SECTION 16 ─ PREDICTION ENGINE
// ═══════════════════════════════════════════════════════════════════════════
static void updatePrediction() {
    // Only activate after a pause in typing
    if (millis() - g_lastKeyMs < PRED_TRIGGER_MS) {
        g_predIdx = -1;
        return;
    }
    if (g_inputLen == 0) { g_predIdx = -1; return; }

    // Extract last partial word
    char word[20] = "";
    int i = g_inputLen - 1;
    while (i >= 0 && g_input[i] != ' ') i--;
    uint8_t wlen = g_inputLen - i - 1;
    if (wlen == 0 || wlen > 19) { g_predIdx = -1; return; }
    memcpy(word, g_input + i + 1, wlen);
    word[wlen] = '\0';

    // If prediction already shown, keep cycling (done via PREV/NEXT)
    if (g_predIdx >= 0) return;

    // Find first matching prefix
    for (uint8_t p = 0; p < PRED_COUNT; p++) {
        if (strncasecmp(PREDICTIONS[p], word, wlen) == 0) {
            g_predIdx = p;
            return;
        }
    }
    g_predIdx = -1;
}

static void acceptPrediction() {
    if (g_predIdx < 0) return;
    const char* pred = PREDICTIONS[g_predIdx];

    // Remove last partial word
    while (g_inputLen > 0 && g_input[g_inputLen - 1] != ' ') {
        g_input[--g_inputLen] = '\0';
    }
    // Append prediction + space
    uint8_t plen = strlen(pred);
    if (g_inputLen + plen + 1 < MAX_INPUT) {
        strncat(g_input, pred, plen);
        g_input[g_inputLen + plen]     = ' ';
        g_input[g_inputLen + plen + 1] = '\0';
        g_inputLen += plen + 1;
    }
    g_predIdx   = -1;
    g_lastKeyMs = millis();
}

// ═══════════════════════════════════════════════════════════════════════════
// SECTION 17 ─ STATE HANDLERS (called each loop)
// ═══════════════════════════════════════════════════════════════════════════

// ── 17a  Main Menu ───────────────────────────────────────────────────────
static void handleMainMenu() {
    screenMainMenu();
    if (btnPress(B_NEXT)) g_menuIdx = (g_menuIdx + 1) % MENU_COUNT;
    if (btnPress(B_PREV)) g_menuIdx = (g_menuIdx - 1 + MENU_COUNT) % MENU_COUNT;
    if (btnPress(B_SEL)) {
        switch (g_menuIdx) {
            case 0:  // New Chat
                memset(g_input, 0, sizeof(g_input));
                g_inputLen   = 0;
                g_carIdx     = 0;
                g_layout     = 0;
                g_predIdx    = -1;
                g_lastKeyMs  = millis();
                goTo(AppState::KEYBOARD);
                break;
            case 1:  // Settings
                g_settingsIdx = 0;
                goTo(AppState::SETTINGS);
                break;
            case 2:  // System Info
                goTo(AppState::SYS_INFO);
                break;
        }
    }
}

// ── 17b  Keyboard ────────────────────────────────────────────────────────
static void handleKeyboard() {
    updatePrediction();
    screenKeyboard();

    const char* layout = LAYOUTS[g_layout];
    const int   llen   = strlen_P(layout);

    // Prediction active → PREV/NEXT cycles predictions, SEL accepts
    if (g_predIdx >= 0) {
        if (btnPress(B_NEXT)) g_predIdx = (g_predIdx + 1) % PRED_COUNT;
        if (btnPress(B_PREV)) g_predIdx = (g_predIdx - 1 + PRED_COUNT) % PRED_COUNT;
        if (btnPress(B_SEL))  acceptPrediction();
        // FUNC cancels prediction
        if (btnPress(B_FUNC)) g_predIdx = -1;
        return;
    }

    // Normal carousel navigation
    if (btnPress(B_NEXT)) {
        g_carIdx   = (g_carIdx + 1) % llen;
        g_animDir  = -1;   // next char slides from right
        g_animStart = millis();
        g_lastKeyMs = millis();
    }
    if (btnPress(B_PREV)) {
        g_carIdx   = (g_carIdx - 1 + llen) % llen;
        g_animDir  = +1;   // prev char slides from left
        g_animStart = millis();
        g_lastKeyMs = millis();
    }

    if (btnPress(B_SEL)) {
        char ch = pgm_read_byte(&layout[g_carIdx]);
        if (ch == '\x01') {
            // Shift key: cycle layout
            g_layout  = (g_layout + 1) % LAYOUT_COUNT;
            g_carIdx  = 0;
        } else {
            // Append character
            if (g_inputLen < MAX_INPUT) {
                g_input[g_inputLen++] = ch;
                g_input[g_inputLen]   = '\0';
                g_lastKeyMs = millis();
            }
        }
    }

    // FUNC: short = backspace, long = go to confirm/send
    if (btnRelease(B_FUNC) && !g_btns[B_FUNC].longFired) {
        // Short press released without long fire → backspace
        if (g_inputLen > 0) {
            g_input[--g_inputLen] = '\0';
            g_lastKeyMs = millis();
        }
    }
    if (btnLong(B_FUNC) && g_inputLen > 0) {
        // First frame of long press → confirm screen
        goTo(AppState::CONFIRMING);
    }
}

// ── 17c  Confirm Send ────────────────────────────────────────────────────
static void handleConfirming() {
    screenConfirm();
    if (btnPress(B_SEL))  { goTo(AppState::SENDING); }
    if (btnPress(B_FUNC)) { goTo(AppState::KEYBOARD); }  // cancel
}
// ── 17d  Sending (blocking API call, non-blocking display) ───────────────
// This handler is called once; the API call is synchronous but the display
// continues to animate via screenSending() called in the render pass.
// For a fully non-blocking version, use async TCP (ESPAsyncTCP library).
static bool g_sendDone   = false;
static bool g_sendCalled = false;

static void handleSending() {
    screenSending();
    if (!g_sendCalled) {
        g_sendCalled = true;
        callAPI();
        g_sendDone   = true;
        g_twPos      = 0;
        g_twLastMs   = millis();
        g_sendCalled = false;
        goTo(AppState::RESPONSE);
    }
}

// ── 17e  Response ────────────────────────────────────────────────────────
static void handleResponse() {
    screenResponse();
    // Once typewriter finishes, any button returns to menu
    if (g_twPos >= g_respLen) {
        if (btnPress(B_SEL) || btnPress(B_FUNC) || btnPress(B_NEXT)) {
            goTo(AppState::MAIN_MENU);
        }
    }
}

// ── 17f  Settings ────────────────────────────────────────────────────────
static void handleSettings() {
    screenSettings();
    if (btnPress(B_NEXT)) g_settingsIdx = (g_settingsIdx + 1) % SETTINGS_COUNT;
    if (btnPress(B_PREV)) g_settingsIdx = (g_settingsIdx - 1 + SETTINGS_COUNT) % SETTINGS_COUNT;

    if (btnPress(B_SEL)) {
        switch (g_settingsIdx) {
            case 0:  // API Key via web UI
                startAPMode();
                break;
            case 1:  // Temperature cycle: 0.3 → 0.7 → 1.0 → 0.3
                if      (g_temp < 0.5f) g_temp = 0.7f;
                else if (g_temp < 0.9f) g_temp = 1.0f;
                else                    g_temp = 0.3f;
                saveConfig();
                break;
            case 2:  // Dark mode toggle
                g_darkMode = !g_darkMode;
                display.setContrast(g_darkMode ? 60 : 255);
                saveConfig();
                break;
            case 3:  // Reset WiFi
                g_ssid[0] = '\0';
                g_pass[0] = '\0';
                saveConfig();
                startAPMode();
                break;
            case 4:  // Back
                goTo(AppState::MAIN_MENU);
                break;
        }
    }
    if (btnPress(B_FUNC)) goTo(AppState::MAIN_MENU);
}

// ── 17g  AP Mode ─────────────────────────────────────────────────────────
static void handleAPMode() {
    g_dns.processNextRequest();
    g_webServer.handleClient();
    screenAPMode();
}

// ── 17h  System Info ─────────────────────────────────────────────────────
static void handleSysInfo() {
    screenSysInfo();
    if (btnPress(B_SEL) || btnPress(B_FUNC) || btnPress(B_PREV)) {
        goTo(AppState::MAIN_MENU);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// SECTION 18 ─ DEEP SLEEP
// ═══════════════════════════════════════════════════════════════════════════
static void checkSleep() {
    // Skip sleep during active states
    if (g_state == AppState::AP_MODE     ||
        g_state == AppState::SENDING     ||
        g_state == AppState::WIFI_CONNECT) return;

    if (millis() - g_lastActivity > IDLE_SLEEP_MS) {
        display.clearBuffer();
        display.setFont(u8g2_font_5x7_tr);
        dispCentered(12, "Going to sleep...");
        dispCentered(24, "D0 = wake");
        display.sendBuffer();
        delay(900);
        display.setPowerSave(1);       // OLED off
        // GPIO16 (D0) must be bridged to RST for wake
        ESP.deepSleep(0);              // indefinite; wake via RST pulse
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// SECTION 19 ─ SETUP
// ═══════════════════════════════════════════════════════════════════════════
// extern "C" is REQUIRED: ESP8266 Arduino 3.x core_esp8266_main.cpp
// declares setup() and loop() as extern "C", so the linker looks for
// unmangled C symbols.  Without this the linker emits:
//   "undefined reference to 'setup'"  /  "undefined reference to 'loop'"
extern "C" void setup() {
    Serial.begin(115200);
    Serial.println(F("\n=== Pocket-AI Terminal booting ==="));

    // ── Button pins ──────────────────────────────────────────────────────
    pinMode(PIN_PREV,   INPUT_PULLUP);
    pinMode(PIN_NEXT,   INPUT_PULLUP);
    pinMode(PIN_SELECT, INPUT_PULLUP);
    pinMode(PIN_FUNC,   INPUT);        // External pull-up; GPIO16 wake pin

    // ── Display ──────────────────────────────────────────────────────────
    Wire.begin(/* SDA= */ D2, /* SCL= */ D1);
    display.begin();
    display.setFlipMode(0);
    display.setBusClock(400000);       // 400 kHz I²C
    goTo(AppState::SPLASH);
    screenSplash();
    delay(1200);

    // ── Load config ──────────────────────────────────────────────────────
    loadConfig();
    display.setContrast(g_darkMode ? 60 : 255);
    Serial.printf("SSID: [%s]  Key: [%s]\n", g_ssid, g_apiKey[0] ? "SET" : "EMPTY");

    // ── WiFi ─────────────────────────────────────────────────────────────
    if (g_ssid[0] == '\0' || !connectWiFi()) {
        Serial.println(F("WiFi failed → AP mode"));
        startAPMode();
    } else {
        Serial.print(F("WiFi connected, IP: "));
        Serial.println(WiFi.localIP());
        goTo(AppState::MAIN_MENU);
    }

    g_lastActivity = millis();
}

// ═══════════════════════════════════════════════════════════════════════════
// SECTION 20 ─ MAIN LOOP  (state machine dispatcher)
// ═══════════════════════════════════════════════════════════════════════════
extern "C" void loop() {
    pollButtons();
    checkSleep();

    switch (g_state) {
        case AppState::SPLASH:       /* handled in setup */     break;
        case AppState::WIFI_CONNECT: /* handled in setup */     break;
        case AppState::AP_MODE:      handleAPMode();            break;
        case AppState::MAIN_MENU:    handleMainMenu();          break;
        case AppState::KEYBOARD:     handleKeyboard();          break;
        case AppState::CONFIRMING:   handleConfirming();        break;
        case AppState::SENDING:      handleSending();           break;
        case AppState::RESPONSE:     handleResponse();          break;
        case AppState::SETTINGS:     handleSettings();          break;
        case AppState::SYS_INFO:     handleSysInfo();           break;
    }

    // ~55 fps cap keeps the MCU from burning cycles for nothing
    delay(18);
}
