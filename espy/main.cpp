// ============================================================
// ESPY — Field Terminal Firmware v1.0.0
// Board  : ESP32-2432S028R (Cheap Yellow Display)
// Display: 2.8" 320×240 ILI9341, landscape, resistive touch (XPT2046)
// ============================================================

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <SD.h>
#include <ArduinoJson.h>
#include <TinyGPSPlus.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <time.h>

#include "config.h"

// ============================================================
// HARDWARE OBJECTS
// ============================================================

TFT_eSPI tft = TFT_eSPI();

// Touch uses its own SPI bus (VSPI) — separate from TFT (HSPI)
SPIClass touchSPI;   // No bus arg — lets ESP32 software-route SPI on custom pins without fighting VSPI/HSPI
SPIClass sdSPI(VSPI); // SD uses default VSPI pins
XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);

// GPS
TinyGPSPlus gps;
HardwareSerial gpsSerial(GPS_UART_NUM);
bool gpsAvailable    = false;
bool gpsFix          = false;
uint8_t gpsSats      = 0;
double  gpsLat       = 0.0;
double  gpsLng       = 0.0;
float   gpsAccM      = 999.0f;

// ============================================================
// APPLICATION STATE
// ============================================================

// --- Tabs ---
enum Tab { TAB_MISSION = 0, TAB_MAP, TAB_STOCK, TAB_DROP, TAB_LOG, TAB_STATUS };
Tab activeTab = TAB_MISSION;
Tab pendingTab = TAB_MISSION;
bool tabChanged = true;

// --- Scrolling ---
int scrollOffset[TAB_COUNT]    = {0};  // current scroll position (px) per tab
int tabContentHeight[TAB_COUNT] = {0}; // total content height (px) per tab, set by drawXxx()
#define SCROLL_ARROW_STEP 30           // px per arrow tap
#define SCROLL_DRAG_THRESHOLD 6        // px of finger movement before it's treated as a drag, not a tap
#define SCROLL_ARROW_W 22               // width of each scroll arrow button
#define SCROLL_ARROW_H 18               // height of each scroll arrow button

// --- WiFi / network ---
bool wifiConnected   = false;
unsigned long lastWifiRetry  = 0;
unsigned long lastStatusPost = 0;

// --- GPS ---
unsigned long lastGpsParse = 0;

// --- Drop marker ---
bool showingDropConfirm = false;
unsigned long dropBtnLowSince = 0;  // millis() when pin first read LOW, 0 if currently HIGH
bool dropBtnArmed = true;           // prevents re-trigger until pin goes HIGH again

// --- Day zero (set from SD config or default) ---
// Default: now minus 247 days for demo purposes
time_t dayZeroEpoch = 0;

// --- Mission data (loaded from SD) ---
String missionName        = "NO MISSION LOADED";
String missionDestination = "—";
String missionVehicle     = "—";
String missionDate        = "—";

// --- Wanted list ---
struct WantedItem {
    String id;
    String name;
    String priority;  // critical / high / medium / low
    bool   found;
};
WantedItem wantedItems[20];
int wantedCount = 0;

// --- Field markers dropped this session ---
int markersDroppedThisSession = 0;

// --- Resources ---
struct ResourceItem {
    String name;
    String unit;
    float  qty;
    int    daysRemaining;
    String status; // critical / warning
};
ResourceItem resources[20];
int resourceCount = 0;

// --- Field journal entries this session ---
int journalEntriesThisSession = 0;

// --- Synced markers from LMoE ---
struct SyncedMarker {
    String id;
    String name;
    String cat;
    String status;
    double lat;
    double lng;
};
SyncedMarker syncedMarkers[50];
int syncedMarkerCount = 0;

// --- Map metadata (from last sync) ---
struct MapMeta {
    double centreLat;
    double centreLng;
    int    zoom;
    String destination;
    String run;
    String generated;
    bool   valid;
};
MapMeta mapMeta = { 0, 0, 14, "", "", "", false };

// --- Map image available on SD ---
bool mapImageAvailable = false;

// --- Touch ---
unsigned long lastTouchTime = 0;
bool          touchActive   = false;   // finger currently down
bool          touchIsDrag   = false;   // moved past threshold -> treat as scroll, not tap
int           touchStartX = 0, touchStartY = 0; // where the press began (screen coords)
int           touchLastY  = 0;         // last Y seen, for computing drag delta

// ============================================================
// FORWARD DECLARATIONS
// ============================================================

// Core
void drawHeader();
void drawTabBar();
void drawContent();
void drawScrollArrows();

// Tab renderers
void drawMission();
void drawMap();
void drawStock();
void drawDrop();
void drawLog();
void drawStatus();

// Drop marker
void checkDropButton();
void handleDropMarker();

// WiFi
void connectWiFi();
// Returns true if LMoE responded with sync_requested=true
bool postEspyStatus();
void performSync();
bool syncPullLmoeData();
bool syncPushFieldData();
int countWantedUpdatesOnSD();
int countFilesInDir(const char* dirPath);

// GPS
void updateGPS();

// SD
void loadConfigFromSD();
void loadMissionFromSD();
void loadWantedFromSD();
void loadResourcesFromSD();
void loadSyncedMarkersFromSD();
void loadMapMetaFromSD();
void saveDropMarker(double lat, double lng);

// Time
String gmtTimeString();
String gmtDateString();
int    currentDayNumber();

// UI helpers
void drawSectionHeader(int y, const char* txt);
void drawDivider(int y);
void drawStatusDot(int x, int y, uint16_t col);

// Touch
bool getTouchXY(int &x, int &y);
void handleTouch();
void handleTouchMission(int tx, int ty);
void handleTouchStock(int tx, int ty);
void handleTouchDrop(int tx, int ty);
void handleTouchLog(int tx, int ty);

// Field data helpers
void saveWantedUpdate(String itemId);
void createBlankJournalEntry();
String isoTimestamp();
String gmtTimeShort();

// ============================================================
// INTERRUPT — DROP BUTTON
// ============================================================

// ============================================================
// DROP BUTTON — POLLED, DEBOUNCED
// ============================================================
// GPIO35 has no internal pull resistor. With nothing wired, it floats
// and may read LOW briefly/randomly. We require a SUSTAINED LOW for
// DROP_BTN_HOLD_MS before treating it as a genuine press, and require
// the pin to return HIGH before it can fire again (no auto-repeat).
// If DROP_BTN_ENABLED is 0 in config.h, this is skipped entirely.

#define DROP_BTN_HOLD_MS 50

void checkDropButton() {
#if DROP_BTN_ENABLED
    bool low = (digitalRead(DROP_BTN_PIN) == LOW);

    if (low) {
        if (dropBtnLowSince == 0) {
            dropBtnLowSince = millis();
        } else if (dropBtnArmed && (millis() - dropBtnLowSince >= DROP_BTN_HOLD_MS)) {
            dropBtnArmed = false;
            handleDropMarker();
        }
    } else {
        dropBtnLowSince = 0;
        dropBtnArmed = true;
    }
#endif
}

// ============================================================
// SETUP
// ============================================================

void setup() {
    Serial.begin(115200);
    Serial.println("[ESPY] Boot");

    // ---- RGB LED — blink once to show boot ----
    pinMode(LED_R_PIN, OUTPUT);
    pinMode(LED_G_PIN, OUTPUT);
    pinMode(LED_B_PIN, OUTPUT);
    digitalWrite(LED_R_PIN, LOW);   // Active LOW — turn red on
    digitalWrite(LED_G_PIN, HIGH);
    digitalWrite(LED_B_PIN, HIGH);
    delay(300);
    digitalWrite(LED_R_PIN, HIGH);  // Off

    // ---- Display ----
    tft.init();
    tft.setRotation(1);             // ST7789 driver — 1 = landscape (try 3 if upside down)
    tft.fillScreen(COL_BG);
    tft.setTextDatum(TL_DATUM);

    // Boot splash
    tft.setTextColor(COL_GREEN, COL_BG);
    tft.drawString("ESPY", 12, 10, 4);
    tft.setTextColor(COL_TEXT_SEC, COL_BG);
    tft.drawString("FIELD TERMINAL  v" FIRMWARE_VERSION, 12, 46, 2);
    tft.drawString(ESPY_ID, 12, 66, 2);
    tft.setTextColor(COL_TEXT_MUTE, COL_BG);
    tft.drawString("Initialising...", 12, 100, 2);

    // ---- Touch (own VSPI bus) ----
    // With the 2-arg constructor, ts.begin() internally calls
    // touchSPI.begin() with NO pin arguments (mosi/miso/sck default to 0
    // in the library), which resets VSPI to its DEFAULT pins — wiping out
    // our custom 25/32/39 assignment. Fix: re-call touchSPI.begin() with
    // our pins AFTER ts.begin(), so our assignment wins.
    pinMode(XPT2046_IRQ, INPUT_PULLUP); // attempt SW pull-up (may not work on GPIO36, but harmless)
    touchSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
    bool touchOk = ts.begin(touchSPI);
    touchSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS); // re-assert our pins
    ts.setRotation(1);
    Serial.printf("[TOUCH] ts.begin() returned %d\n", touchOk);
    Serial.printf("[TOUCH] IRQ pin %d idle state = %d (1=HIGH/idle, 0=LOW/triggered)\n",
                   XPT2046_IRQ, digitalRead(XPT2046_IRQ));

    // ---- SD card ----
    // Use a separate SPIClass instance (sdSPI) with explicit pins, so its
    // begin() doesn't clobber touchSPI's pin assignment on the shared
    // VSPI peripheral.
    tft.drawString("SD card...", 12, 120, 2);
    sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    if (!SD.begin(SD_CS, sdSPI)) {
        tft.setTextColor(COL_AMBER, COL_BG);
        tft.drawString("SD: not found — field data unsaved", 12, 120, 2);
        tft.setTextColor(COL_TEXT_MUTE, COL_BG);
        delay(1500);
    } else {
        tft.drawString("SD: OK", 12, 120, 2);
        loadConfigFromSD();
        loadMissionFromSD();
        loadWantedFromSD();
        loadResourcesFromSD();
        loadSyncedMarkersFromSD();
        loadMapMetaFromSD();
        mapImageAvailable = SD.exists("/LMOE/map.bmp");
    }

    // Re-assert touch SPI pins — sdSPI.begin() above reconfigures the
    // shared VSPI peripheral's pin matrix, which would otherwise leave
    // touch unable to communicate.
    touchSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);

    // ---- GPS ----
    tft.drawString("GPS...", 12, 140, 2);
    gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
    // We'll detect GPS availability after first parse attempt in loop()
    tft.drawString("GPS: pending fix", 12, 140, 2);

    // ---- Drop marker button ----
    // NOTE: GPIO35 is input-only with NO internal pull resistor on the ESP32.
    // Until an external button + pull-up (or pull-down) resistor is wired,
    // this pin floats and reads noise. We poll with debouncing instead of
    // using an interrupt, and require a SUSTAINED low reading before
    // treating it as a press. This still won't be perfectly silent on a
    // floating pin, but avoids the rapid-fire interrupt storm.
    pinMode(DROP_BTN_PIN, INPUT);

    // ---- WiFi ----
    tft.drawString("WiFi...", 12, 160, 2);
    connectWiFi();
    if (wifiConnected) {
        tft.setTextColor(COL_GREEN, COL_BG);
        tft.drawString("WiFi: connected", 12, 160, 2);
        tft.setTextColor(COL_TEXT_MUTE, COL_BG);
        // Sync NTP and set UK timezone — getLocalTime() will then return
        // UK wall-clock time (GMT in winter, BST in summer) automatically.
        // The display always LABELS this "GMT" regardless, per LMoE
        // convention — Bob operates on UK time wherever he is.
        configTzTime("GMT0BST,M3.5.0/1,M10.5.0", "pool.ntp.org", "time.nist.gov");
    } else {
        tft.drawString("WiFi: offline — field mode", 12, 160, 2);
    }

    // ---- Day zero default (247 days ago for demo) ----
    if (dayZeroEpoch == 0) {
        struct tm t;
        getLocalTime(&t);  // May be 1970 if no NTP yet — that's fine
        time_t now = mktime(&t);
        dayZeroEpoch = now - (247L * 86400L);
    }

    // Boot done
    delay(800);
    tft.fillScreen(COL_BG);

    // Draw initial UI
    drawHeader();
    drawTabBar();
    drawContent();
}

// ============================================================
// MAIN LOOP
// ============================================================

void loop() {
    // --- GPS parsing ---
    updateGPS();

    // --- Drop button (polled, debounced) ---
    checkDropButton();

    // --- Touch handling ---
    handleTouch();

    // --- Tab change ---
    if (tabChanged) {
        tabChanged = false;
        drawContent();
    }

    // --- WiFi reconnect ---
    if (!wifiConnected && (millis() - lastWifiRetry > WIFI_RETRY_INTERVAL_MS)) {
        lastWifiRetry = millis();
        connectWiFi();
        drawHeader();  // Refresh header WiFi indicator
    }

    // --- Periodic status POST to LMoE ---
    if (wifiConnected && (millis() - lastStatusPost > STATUS_POST_INTERVAL_MS)) {
        lastStatusPost = millis();
        if (postEspyStatus()) {
            performSync();
        }
    }

    // --- Periodic header refresh (clock) ---
    static unsigned long lastHeaderRefresh = 0;
    if (millis() - lastHeaderRefresh > 1000) {
        lastHeaderRefresh = millis();
        drawHeader();
    }

    delay(20);
}

// ============================================================
// GPS
// ============================================================

// Manual UTC struct-tm -> epoch conversion (equivalent to timegm(), which
// may not be available on all ESP32 Arduino core versions). GPS date/time
// fields are UTC and must NOT be passed through mktime() (which would
// apply the configured UK timezone offset, shifting by an hour in BST).
static time_t tmUtcToEpoch(struct tm *t) {
    static const int cumDays[] = {0,31,59,90,120,151,181,212,243,273,304,334};
    int year = t->tm_year + 1900;
    int mon  = t->tm_mon; // 0-11

    long days = 0;
    for (int y = 1970; y < year; y++) {
        days += (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 : 365;
    }
    days += cumDays[mon];
    if (mon > 1 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))) {
        days += 1; // leap day, only relevant for Mar-Dec of a leap year
    }
    days += (t->tm_mday - 1);

    return (time_t)days * 86400L + t->tm_hour * 3600L + t->tm_min * 60L + t->tm_sec;
}

void updateGPS() {
    while (gpsSerial.available()) {
        char c = gpsSerial.read();
        if (gps.encode(c)) {
            lastGpsParse = millis();
            if (!gpsAvailable) {
                gpsAvailable = true;
                Serial.println("[GPS] Module detected");
            }
            if (gps.location.isValid() && gps.location.age() < 2000) {
                gpsFix  = true;
                gpsLat  = gps.location.lat();
                gpsLng  = gps.location.lng();
                gpsSats = gps.satellites.isValid() ? gps.satellites.value() : 0;
                // Approximate accuracy from HDOP if available
                if (gps.hdop.isValid()) {
                    gpsAccM = gps.hdop.hdop() * 3.5f; // rough estimate
                }
                // Feed GPS time into system time. GPS date/time fields are
                // UTC — use tmUtcToEpoch() (not mktime()) so they're
                // interpreted as UTC regardless of the configured UK
                // timezone. mktime() would treat them as local time and
                // shift by an hour during BST.
                if (gps.date.isValid() && gps.time.isValid()) {
                    struct tm t = {};
                    t.tm_year = gps.date.year() - 1900;
                    t.tm_mon  = gps.date.month() - 1;
                    t.tm_mday = gps.date.day();
                    t.tm_hour = gps.time.hour();
                    t.tm_min  = gps.time.minute();
                    t.tm_sec  = gps.time.second();
                    time_t epoch = tmUtcToEpoch(&t);
                    struct timeval tv = { epoch, 0 };
                    settimeofday(&tv, nullptr);
                }
            } else {
                gpsFix  = false;
                gpsSats = gps.satellites.isValid() ? gps.satellites.value() : 0;
            }
        }
    }
    // If module was detected but no chars for 5s, mark unavailable
    if (gpsAvailable && (millis() - lastGpsParse > 5000)) {
        gpsAvailable = false;
        gpsFix       = false;
    }
}

// ============================================================
// TIME UTILITIES — UK wall-clock time (GMT/BST), always LABELLED "GMT"
// ============================================================
// getLocalTime() returns UK local time (GMT in winter, BST in summer)
// thanks to configTzTime("GMT0BST,M3.5.0/1,M10.5.0", ...) called after
// WiFi connects. Per LMoE convention, this is displayed and labelled
// "GMT" regardless of whether it's actually BST — Bob operates on UK
// time wherever he is. The underlying value is correct UK wall-clock
// time; only the label is technically inaccurate during BST, by design.

String gmtTimeString() {
    struct tm t;
    if (!getLocalTime(&t, 0)) return "--:--:--";
    char buf[12];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
    return String(buf);
}

String gmtTimeShort() {
    struct tm t;
    if (!getLocalTime(&t, 0)) return "--:--";
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
    return String(buf);
}

String gmtDateString() {
    struct tm t;
    if (!getLocalTime(&t, 0)) return "----/--/--";
    char buf[14];
    snprintf(buf, sizeof(buf), "%04d/%02d/%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    return String(buf);
}

// UK local time (GMT/BST as appropriate), always suffixed "GMT" per
// LMoE convention — Bob operates on UK time wherever he is.
// NOT a true UTC timestamp despite the similar ISO-ish format; consumers
// (LMoE sync) must treat the "GMT" suffix as "UK local time", not UTC.
String isoTimestamp() {
    struct tm t;
    if (!getLocalTime(&t, 0)) return "1970-01-01T00:00:00 GMT";
    char buf[26];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d GMT",
             t.tm_year+1900, t.tm_mon+1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec);
    return String(buf);
}

int currentDayNumber() {
    time_t now;
    time(&now);
    if (now < 1000000 || dayZeroEpoch == 0) return 247; // fallback
    return (int)((now - dayZeroEpoch) / 86400L) + 1;
}

// ============================================================
// DRAW — HEADER
// ============================================================

void drawHeader() {
    // Background bar
    tft.fillRect(0, 0, SCREEN_W, HEADER_H, COL_SURFACE);
    tft.drawFastHLine(0, HEADER_H - 1, SCREEN_W, COL_GREEN_DIM);

    // ESPY label
    tft.setTextColor(COL_GREEN, COL_SURFACE);
    tft.setTextDatum(ML_DATUM);
    tft.drawString("ESPY", 6, HEADER_H / 2, 2);

    // Divider
    tft.drawFastVLine(42, 4, HEADER_H - 8, COL_BORDER_BR);

    // Clock (GMT)
    String clk = gmtTimeString() + " GMT";
    tft.setTextColor(COL_GREEN, COL_SURFACE);
    tft.setTextDatum(ML_DATUM);
    tft.drawString(clk.c_str(), 48, HEADER_H / 2, 2);

    // Day counter
    char dayBuf[12];
    snprintf(dayBuf, sizeof(dayBuf), "DAY %d", currentDayNumber());
    tft.setTextColor(COL_AMBER, COL_SURFACE);
    tft.setTextDatum(ML_DATUM);
    tft.drawString(dayBuf, 160, HEADER_H / 2, 2);

    // GPS indicator (right side)
    int gpsX = SCREEN_W - 56;
    if (!gpsAvailable) {
        tft.setTextColor(COL_TEXT_MUTE, COL_SURFACE);
        tft.drawString("GPS --", gpsX, HEADER_H / 2, 1);
    } else if (!gpsFix) {
        tft.setTextColor(COL_AMBER, COL_SURFACE);
        char buf[10];
        snprintf(buf, sizeof(buf), "GPS S%d", gpsSats);
        tft.drawString(buf, gpsX, HEADER_H / 2, 1);
    } else {
        tft.setTextColor(COL_GREEN, COL_SURFACE);
        char buf[10];
        snprintf(buf, sizeof(buf), "GPS S%d", gpsSats);
        tft.drawString(buf, gpsX, HEADER_H / 2, 1);
    }

    // WiFi indicator
    int wifiX = SCREEN_W - 8;
    tft.setTextDatum(MR_DATUM);
    if (wifiConnected) {
        tft.setTextColor(COL_GREEN, COL_SURFACE);
        tft.drawString("W", wifiX, HEADER_H / 2, 1);
    } else {
        tft.setTextColor(COL_TEXT_MUTE, COL_SURFACE);
        tft.drawString("W", wifiX, HEADER_H / 2, 1);
    }

    tft.setTextDatum(TL_DATUM); // Reset datum
}

// ============================================================
// DRAW — TAB BAR
// ============================================================

struct TabDef {
    const char* icon;
    const char* label;
};

const TabDef TABS[TAB_COUNT] = {
    { "M", "MISSION" },
    { "M", "MAP"     },
    { "S", "STOCK"   },
    { "D", "DROP"    },
    { "L", "LOG"     },
    { "S", "STATUS"  },
};

void drawTabBar() {
    int y = SCREEN_H - TABBAR_H;
    int tabW = SCREEN_W / TAB_COUNT;  // 320/6 = 53px each

    tft.fillRect(0, y, SCREEN_W, TABBAR_H, COL_SURFACE);
    tft.drawFastHLine(0, y, SCREEN_W, COL_GREEN_DIM);

    for (int i = 0; i < TAB_COUNT; i++) {
        int x = i * tabW;
        bool sel = (i == (int)activeTab);

        // Active tab highlight
        if (sel) {
            tft.fillRect(x, y, tabW, TABBAR_H, COL_RAISED);
            tft.drawFastHLine(x, y, tabW, COL_GREEN);
        }

        // Tab label
        tft.setTextColor(sel ? COL_GREEN : COL_TEXT_MUTE, sel ? COL_RAISED : COL_SURFACE);
        tft.setTextDatum(MC_DATUM);
        tft.drawString(TABS[i].label, x + tabW / 2, y + TABBAR_H / 2, 1);

        // Divider between tabs
        if (i < TAB_COUNT - 1) {
            tft.drawFastVLine(x + tabW, y + 4, TABBAR_H - 8, COL_BORDER);
        }
    }

    tft.setTextDatum(TL_DATUM);
}

// ============================================================
// DRAW — CONTENT DISPATCHER
// ============================================================

void drawContent() {
    // Clear content area
    tft.fillRect(0, CONTENT_Y, SCREEN_W, CONTENT_H, COL_BG);

    // Set viewport to the content area. With vpDatum=true, (0,0) in all
    // subsequent drawing calls maps to the top-left of the content area
    // (i.e. screen position (0, CONTENT_Y)), and everything is clipped
    // to CONTENT_H tall — content can never overdraw the header or tab bar.
    tft.setViewport(0, CONTENT_Y, SCREEN_W, CONTENT_H, true);

    switch (activeTab) {
        case TAB_MISSION: drawMission(); break;
        case TAB_MAP:     drawMap();     break;
        case TAB_STOCK:   drawStock();   break;
        case TAB_DROP:    drawDrop();    break;
        case TAB_LOG:     drawLog();     break;
        case TAB_STATUS:  drawStatus();  break;
    }

    tft.resetViewport();

    drawScrollArrows();
}

// ============================================================
// SCROLL ARROWS — small up/down buttons, top-right of content area
// ============================================================
void drawScrollArrows() {
    int maxScroll = tabContentHeight[activeTab] - CONTENT_H;
    if (maxScroll <= 0) return; // content fits on screen — no arrows needed

    int ax = SCREEN_W - SCROLL_ARROW_W;
    int ayUp   = CONTENT_Y;
    int ayDown = CONTENT_Y + SCROLL_ARROW_H;

    bool canUp   = scrollOffset[activeTab] > 0;
    bool canDown = scrollOffset[activeTab] < maxScroll;

    // Background plates
    tft.fillRect(ax, ayUp,   SCROLL_ARROW_W, SCROLL_ARROW_H, COL_SURFACE);
    tft.fillRect(ax, ayDown, SCROLL_ARROW_W, SCROLL_ARROW_H, COL_SURFACE);
    tft.drawRect(ax, ayUp,   SCROLL_ARROW_W, SCROLL_ARROW_H, COL_BORDER);
    tft.drawRect(ax, ayDown, SCROLL_ARROW_W, SCROLL_ARROW_H, COL_BORDER);

    uint16_t upCol   = canUp   ? COL_GREEN : COL_TEXT_MUTE;
    uint16_t downCol = canDown ? COL_GREEN : COL_TEXT_MUTE;

    // Up triangle
    int cx = ax + SCROLL_ARROW_W / 2;
    tft.fillTriangle(cx, ayUp + 4, cx - 6, ayUp + 13, cx + 6, ayUp + 13, upCol);
    // Down triangle
    tft.fillTriangle(cx, ayDown + 14, cx - 6, ayDown + 5, cx + 6, ayDown + 5, downCol);
}

// ============================================================
// UI HELPERS
// ============================================================

void drawSectionHeader(int y, const char* txt) {
    tft.setTextColor(COL_TEXT_MUTE, COL_BG);
    tft.setTextDatum(TL_DATUM);
    tft.drawString(txt, 8, y, 1);
    tft.drawFastHLine(8, y + 10, SCREEN_W - 16, COL_BORDER);
}

void drawDivider(int y) {
    tft.drawFastHLine(8, y, SCREEN_W - 16, COL_BORDER);
}

void drawStatusDot(int x, int y, uint16_t col) {
    tft.fillCircle(x, y, 3, col);
}

// Draw a label + value row
void drawKV(int y, const char* label, const char* value, uint16_t valCol = COL_TEXT) {
    tft.setTextColor(COL_TEXT_MUTE, COL_BG);
    tft.setTextDatum(TL_DATUM);
    tft.drawString(label, 8, y, 1);
    tft.setTextColor(valCol, COL_BG);
    tft.drawString(value, 120, y, 1);
}

// Draw a card-style box
void drawCard(int x, int y, int w, int h, uint16_t borderCol = COL_BORDER_BR) {
    tft.fillRect(x, y, w, h, COL_SURFACE);
    tft.drawRect(x, y, w, h, borderCol);
}

// Draw a button
void drawButton(int x, int y, int w, int h, const char* label,
                uint16_t bgCol, uint16_t txtCol, uint8_t font = 2) {
    tft.fillRect(x, y, w, h, bgCol);
    tft.drawRect(x, y, w, h, COL_BORDER_BR);
    tft.setTextColor(txtCol, bgCol);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(label, x + w / 2, y + h / 2, font);
    tft.setTextDatum(TL_DATUM);
}

// Truncate string to fit width (pixel-based approximation for font 2)
String truncate(String s, int maxChars) {
    if ((int)s.length() <= maxChars) return s;
    return s.substring(0, maxChars - 1) + "~";
}

// ============================================================
// TAB 1 — MISSION
// ============================================================

void drawMission() {
    int y = -scrollOffset[TAB_MISSION] + 4;

    // Mission name header
    drawCard(4, y, SCREEN_W - 8, 34, COL_GREEN_DIM);
    tft.setTextColor(COL_GREEN, COL_SURFACE);
    tft.setTextDatum(TL_DATUM);
    tft.drawString(truncate(missionName, 28).c_str(), 10, y + 4, 2);
    tft.setTextColor(COL_TEXT_SEC, COL_SURFACE);
    tft.drawString(truncate(missionDestination, 36).c_str(), 10, y + 20, 1);
    y += 40;

    // Quick stats row
    char dayBuf[20];
    snprintf(dayBuf, sizeof(dayBuf), "DAY %d", currentDayNumber());
    drawKV(y, "Day",      dayBuf);
    y += 12;
    drawKV(y, "Vehicle",  missionVehicle.c_str());
    y += 12;
    drawKV(y, "Depart",   missionDate.c_str());
    y += 12;

    char markersBuf[20];
    snprintf(markersBuf, sizeof(markersBuf), "%d this session", markersDroppedThisSession);
    drawKV(y, "Markers dropped", markersBuf, COL_GREEN);
    y += 18;

    drawDivider(y);
    y += 6;

    // Wanted list (mission context)
    drawSectionHeader(y, "WANTED LIST");
    y += 14;

    if (wantedCount == 0) {
        tft.setTextColor(COL_TEXT_MUTE, COL_BG);
        tft.drawString("No wanted items loaded", 8, y, 1);
        y += 12;
    } else {
        int shownAny = 0;
        for (int i = 0; i < wantedCount; i++) {
            WantedItem& item = wantedItems[i];
            if (item.found) continue;
            shownAny++;

            // Priority dot
            uint16_t dotCol = COL_TEXT_MUTE;
            if (item.priority == "critical") dotCol = COL_RED;
            else if (item.priority == "high") dotCol = COL_AMBER;
            else if (item.priority == "medium") dotCol = COL_GREEN;

            drawStatusDot(12, y + 5, dotCol);

            tft.setTextColor(COL_TEXT, COL_BG);
            tft.drawString(truncate(item.name, 28).c_str(), 22, y, 1);

            // FOUND button (small, right side)
            tft.fillRect(SCREEN_W - 52, y - 1, 48, 13, COL_RAISED);
            tft.drawRect(SCREEN_W - 52, y - 1, 48, 13, COL_GREEN_DIM);
            tft.setTextColor(COL_GREEN, COL_RAISED);
            tft.setTextDatum(MC_DATUM);
            tft.drawString("FOUND", SCREEN_W - 28, y + 5, 1);
            tft.setTextDatum(TL_DATUM);

            y += 14;
        }
        if (shownAny == 0) {
            tft.setTextColor(COL_TEXT_MUTE, COL_BG);
            tft.drawString("All wanted items found", 8, y, 1);
            y += 12;
        }
    }

    y += 4;
    tabContentHeight[TAB_MISSION] = y + scrollOffset[TAB_MISSION];
}

// ============================================================
// WEB MERCATOR PROJECTION — lat/lng to pixel offset from centre
// ============================================================
// At zoom Z: one tile = 256px covers 360/2^Z degrees longitude.
// Pixel pitch: degrees per pixel = 360 / (256 * 2^Z) for longitude.
// Latitude uses Mercator projection (non-linear).
// Returns pixel offset from image centre (positive = right/down).

static void latLngToPixelOffset(
        double lat, double lng,
        double centreLat, double centreLng,
        int zoom, int imgW, int imgH,
        int& px, int& py) {

    // Mercator Y for a latitude (range 0..1 across full world height at this zoom)
    auto mercY = [](double latDeg) -> double {
        double latRad = latDeg * M_PI / 180.0;
        return (1.0 - log(tan(latRad) + 1.0 / cos(latRad)) / M_PI) / 2.0;
    };

    double scale = 256.0 * pow(2.0, zoom); // world size in pixels at this zoom

    // Centre in world pixel coords
    double cx = (centreLng + 180.0) / 360.0 * scale;
    double cy = mercY(centreLat) * scale;

    // Point in world pixel coords
    double wx = (lng + 180.0) / 360.0 * scale;
    double wy = mercY(lat) * scale;

    px = (int)round(wx - cx);
    py = (int)round(wy - cy);
}

// ============================================================
// TAB 2 — MAP
// ============================================================

void drawMap() {
    const int PLOT_W = SCREEN_W - 8;
    const int PLOT_H = 100;   // schematic plot height (used when no BMP)
    const int MAP_H  = 120;   // BMP display height (narrower to leave room for list)
    int y = -scrollOffset[TAB_MAP] + 4;

    // ---- Header: GPS status ----
    if (!gpsFix) {
        drawCard(4, y, SCREEN_W - 8, 34, COL_AMBER);
        tft.setTextColor(COL_AMBER, COL_SURFACE);
        tft.setTextDatum(TL_DATUM);
        tft.drawString("NO GPS FIX", 10, y + 4, 2);
        tft.setTextColor(COL_TEXT_SEC, COL_SURFACE);
        tft.drawString(gpsAvailable ? "Searching..." : "GPS module pending", 10, y + 22, 1);
        y += 40;
    }

    // ---- Mission destination label ----
    if (mapMeta.valid && mapMeta.destination.length() > 0) {
        drawSectionHeader(y, "DESTINATION");
        y += 14;
        tft.setTextColor(COL_GREEN, COL_BG);
        tft.drawString(truncate(mapMeta.destination, 38).c_str(), 8, y, 1);
        y += 12;
        tft.setTextColor(COL_TEXT_MUTE, COL_BG);
        tft.drawString(("Synced: " + truncate(mapMeta.generated, 30)).c_str(), 8, y, 1);
        y += 16;
    }

    // ---- Map image or schematic plot ----
    drawSectionHeader(y, mapImageAvailable ? "MAP (LAST SYNC)" : "TACTICAL PLOT");
    y += 14;

    if (mapImageAvailable) {
        // Draw BMP from SD — TFT_eSPI supports 24-bit BMP via drawBmpFile (SPIFFS)
        // We use a manual SD-based BMP reader since TFT_eSPI's built-in uses SPIFFS.
        // Read BMP header, then stream pixel rows.
        File bmpFile = SD.open("/LMOE/map.bmp", FILE_READ);
        if (bmpFile && bmpFile.size() > 54) {
            // Read header
            uint8_t hdr[54];
            bmpFile.read(hdr, 54);

            int bmpW = hdr[18] | (hdr[19]<<8) | (hdr[20]<<16) | (hdr[21]<<24);
            int bmpH = hdr[22] | (hdr[23]<<8) | (hdr[24]<<16) | (hdr[25]<<24);
            if (bmpH < 0) bmpH = -bmpH; // top-down BMP has negative height
            int pixelOffset = hdr[10] | (hdr[11]<<8) | (hdr[12]<<16) | (hdr[13]<<24);
            int rowSize = ((bmpW * 3 + 3) / 4) * 4; // padded row

            // Only render if dimensions match Espy screen width
            if (bmpW == SCREEN_W) {
                int drawH = min(bmpH, MAP_H);
                bmpFile.seek(pixelOffset);

                uint8_t* rowBuf = (uint8_t*)malloc(rowSize);
                if (rowBuf) {
                    tft.startWrite();
                    tft.setAddrWindow(0, y, bmpW, drawH);
                    for (int row = 0; row < drawH; row++) {
                        bmpFile.read(rowBuf, rowSize);
                        // BMP is BGR, TFT wants RGB565
                        for (int px = 0; px < bmpW; px++) {
                            uint8_t b = rowBuf[px*3];
                            uint8_t g = rowBuf[px*3+1];
                            uint8_t r = rowBuf[px*3+2];
                            uint16_t c = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
                            tft.pushColor(c);
                        }
                    }
                    tft.endWrite();
                    free(rowBuf);
                }

                // Overlay: plot synced markers on BMP
                if (mapMeta.valid) {
                    for (int i = 0; i < syncedMarkerCount; i++) {
                        int ox, oy;
                        latLngToPixelOffset(
                            syncedMarkers[i].lat, syncedMarkers[i].lng,
                            mapMeta.centreLat, mapMeta.centreLng,
                            mapMeta.zoom, SCREEN_W, MAP_H, ox, oy);
                        int dotX = SCREEN_W/2 + ox;
                        int dotY = y + MAP_H/2 + oy;
                        if (dotX >= 2 && dotX < SCREEN_W-2 && dotY >= y+2 && dotY < y+MAP_H-2) {
                            uint16_t col = COL_AMBER;
                            if (syncedMarkers[i].status == "critical") col = COL_RED;
                            else if (syncedMarkers[i].status == "secured") col = COL_GREEN;
                            tft.fillCircle(dotX, dotY, 4, col);
                            tft.drawCircle(dotX, dotY, 5, COL_BLACK);
                        }
                    }
                }

                // Bob's GPS position (green dot with ring)
                if (gpsFix && mapMeta.valid) {
                    int ox, oy;
                    latLngToPixelOffset(gpsLat, gpsLng,
                        mapMeta.centreLat, mapMeta.centreLng,
                        mapMeta.zoom, SCREEN_W, MAP_H, ox, oy);
                    int dotX = SCREEN_W/2 + ox;
                    int dotY = y + MAP_H/2 + oy;
                    if (dotX >= 4 && dotX < SCREEN_W-4 && dotY >= y+4 && dotY < y+MAP_H-4) {
                        tft.fillCircle(dotX, dotY, 5, COL_GREEN);
                        tft.drawCircle(dotX, dotY, 8, COL_GREEN_DIM);
                    }
                }

                // Destination crosshair (centre of BMP)
                {
                    int cx = SCREEN_W/2, cy2 = y + MAP_H/2;
                    tft.drawLine(cx-8, cy2, cx+8, cy2, COL_WHITE);
                    tft.drawLine(cx, cy2-8, cx, cy2+8, COL_WHITE);
                    tft.drawCircle(cx, cy2, 5, COL_WHITE);
                }

                y += drawH + 4;
            } else {
                // Wrong dimensions — fall through to schematic
                bmpFile.close();
                mapImageAvailable = false;
            }
        }
        if (bmpFile) bmpFile.close();
    }

    if (!mapImageAvailable) {
        // Schematic plot — dot map relative to Bob or map centre
        drawCard(4, y, PLOT_W, PLOT_H, COL_BORDER);
        int cx = SCREEN_W/2, cy = y + PLOT_H/2;

        // Grid lines
        tft.drawLine(cx, y, cx, y+PLOT_H, COL_RAISED);
        tft.drawLine(4, cy, SCREEN_W-4, cy, COL_RAISED);

        // Plot markers relative to GPS (or map meta centre)
        double refLat = gpsFix ? gpsLat : (mapMeta.valid ? mapMeta.centreLat : 51.2);
        double refLng = gpsFix ? gpsLng : (mapMeta.valid ? mapMeta.centreLng : 0.3);
        int refZoom   = mapMeta.valid ? mapMeta.zoom : 13;

        for (int i = 0; i < syncedMarkerCount; i++) {
            int ox, oy;
            latLngToPixelOffset(syncedMarkers[i].lat, syncedMarkers[i].lng,
                refLat, refLng, refZoom, PLOT_W, PLOT_H, ox, oy);
            int dotX = cx + ox;
            int dotY = cy + oy;
            if (dotX >= 6 && dotX < SCREEN_W-6 && dotY >= y+4 && dotY < y+PLOT_H-4) {
                uint16_t col = COL_AMBER;
                if (syncedMarkers[i].status == "critical") col = COL_RED;
                else if (syncedMarkers[i].status == "secured") col = COL_GREEN;
                tft.fillCircle(dotX, dotY, 3, col);
            }
        }

        // Bob's dot
        if (gpsFix) {
            tft.fillCircle(cx, cy, 5, COL_GREEN);
            tft.drawCircle(cx, cy, 7, COL_GREEN_DIM);
        } else {
            tft.setTextColor(COL_TEXT_MUTE, COL_SURFACE);
            tft.setTextDatum(MC_DATUM);
            tft.drawString("Sync map from LMoE", cx, cy - 8, 1);
            tft.drawString("for marker overlay", cx, cy + 4, 1);
            tft.setTextDatum(TL_DATUM);
        }

        y += PLOT_H + 4;
    }

    // ---- GPS coordinates ----
    drawDivider(y); y += 6;
    drawSectionHeader(y, "POSITION"); y += 14;
    if (gpsFix) {
        char latBuf[20], lngBuf[20], accBuf[20];
        snprintf(latBuf, sizeof(latBuf), "%.5f", gpsLat);
        snprintf(lngBuf, sizeof(lngBuf), "%.5f", gpsLng);
        snprintf(accBuf, sizeof(accBuf), "~%.0fm", gpsAccM);
        drawKV(y, "Latitude",  latBuf, COL_GREEN); y += 12;
        drawKV(y, "Longitude", lngBuf, COL_GREEN); y += 12;
        drawKV(y, "Accuracy",  accBuf, COL_TEXT_SEC); y += 12;
    } else {
        drawKV(y, "Latitude",  "—", COL_TEXT_MUTE); y += 12;
        drawKV(y, "Longitude", "—", COL_TEXT_MUTE); y += 12;
    }
    char satBuf[16];
    snprintf(satBuf, sizeof(satBuf), "%d", gpsSats);
    drawKV(y, "Satellites", satBuf, gpsSats > 3 ? COL_GREEN : gpsSats > 0 ? COL_AMBER : COL_TEXT_MUTE);
    y += 16;

    // ---- Synced markers list ----
    if (syncedMarkerCount > 0) {
        drawDivider(y); y += 6;
        drawSectionHeader(y, "SYNCED MARKERS"); y += 14;

        // Sort by distance if GPS fix available (simple approximation)
        for (int i = 0; i < syncedMarkerCount; i++) {
            SyncedMarker& m = syncedMarkers[i];
            uint16_t dotCol = COL_AMBER;
            if (m.status == "critical") dotCol = COL_RED;
            else if (m.status == "secured") dotCol = COL_GREEN;
            else if (m.status == "depleted") dotCol = COL_TEXT_MUTE;

            drawStatusDot(12, y + 5, dotCol);
            tft.setTextColor(COL_TEXT, COL_BG);
            tft.drawString(truncate(m.name, 24).c_str(), 22, y, 1);

            // Distance approximation if GPS fix
            if (gpsFix) {
                double dLat = (m.lat - gpsLat) * 111320.0;
                double dLng = (m.lng - gpsLng) * 111320.0 * cos(gpsLat * M_PI / 180.0);
                double dist = sqrt(dLat*dLat + dLng*dLng);
                char distBuf[12];
                if (dist < 1000) snprintf(distBuf, sizeof(distBuf), "%.0fm", dist);
                else             snprintf(distBuf, sizeof(distBuf), "%.1fkm", dist/1000.0);
                tft.setTextColor(COL_TEXT_SEC, COL_BG);
                tft.setTextDatum(MR_DATUM);
                tft.drawString(distBuf, SCREEN_W - 6, y + 5, 1);
                tft.setTextDatum(TL_DATUM);
            }
            y += 14;
        }
    }

    tabContentHeight[TAB_MAP] = y + scrollOffset[TAB_MAP];
}

// ============================================================
// TAB 3 — STOCK
// ============================================================

void drawStock() {
    int y = -scrollOffset[TAB_STOCK] + 4;

    drawSectionHeader(y, "CRITICAL / WARNING RESOURCES");
    y += 14;

    if (resourceCount == 0) {
        drawCard(4, y, SCREEN_W - 8, 30, COL_BORDER);
        tft.setTextColor(COL_TEXT_MUTE, COL_SURFACE);
        tft.setTextDatum(MC_DATUM);
        tft.drawString("No resource data — sync from LMoE", SCREEN_W / 2, y + 15, 1);
        tft.setTextDatum(TL_DATUM);
        y += 36;
    } else {
        for (int i = 0; i < resourceCount; i++) {
            ResourceItem& r = resources[i];
            uint16_t rowCol = (r.status == "critical") ? COL_RED : COL_AMBER;

            // Row background
            tft.fillRect(4, y, SCREEN_W - 8, 18, COL_SURFACE);
            tft.drawRect(4, y, SCREEN_W - 8, 18, rowCol);

            // Status dot
            drawStatusDot(12, y + 9, rowCol);

            // Name
            tft.setTextColor(COL_TEXT, COL_SURFACE);
            tft.drawString(truncate(r.name, 18).c_str(), 22, y + 5, 1);

            // Days remaining
            char buf[20];
            snprintf(buf, sizeof(buf), "%dd", r.daysRemaining);
            tft.setTextColor(rowCol, COL_SURFACE);
            tft.setTextDatum(MR_DATUM);
            tft.drawString(buf, SCREEN_W - 8, y + 9, 1);

            // Qty
            char qtyBuf[20];
            snprintf(qtyBuf, sizeof(qtyBuf), "%.0f %s", r.qty, r.unit.c_str());
            tft.setTextColor(COL_TEXT_SEC, COL_SURFACE);
            tft.setTextDatum(MR_DATUM);
            tft.drawString(qtyBuf, SCREEN_W - 40, y + 9, 1);
            tft.setTextDatum(TL_DATUM);

            y += 22;
        }
    }

    drawDivider(y); y += 6;

    // Wanted items with FOUND buttons
    drawSectionHeader(y, "FIND ON THIS RUN");
    y += 14;

    if (wantedCount == 0) {
        tft.setTextColor(COL_TEXT_MUTE, COL_BG);
        tft.drawString("No wanted list loaded", 8, y, 1);
        y += 12;
    } else {
        int shownAny = 0;
        for (int i = 0; i < wantedCount; i++) {
            WantedItem& item = wantedItems[i];
            if (item.found) continue;
            shownAny++;

            uint16_t dotCol = COL_TEXT_MUTE;
            if (item.priority == "critical") dotCol = COL_RED;
            else if (item.priority == "high") dotCol = COL_AMBER;

            drawStatusDot(12, y + 5, dotCol);
            tft.setTextColor(COL_TEXT, COL_BG);
            tft.drawString(truncate(item.name, 24).c_str(), 22, y, 1);

            // FOUND button
            tft.fillRect(SCREEN_W - 52, y - 1, 48, 13, COL_RAISED);
            tft.drawRect(SCREEN_W - 52, y - 1, 48, 13, COL_GREEN_DIM);
            tft.setTextColor(COL_GREEN, COL_RAISED);
            tft.setTextDatum(MC_DATUM);
            tft.drawString("FOUND", SCREEN_W - 28, y + 5, 1);
            tft.setTextDatum(TL_DATUM);
            y += 14;
        }
        if (shownAny == 0) {
            tft.setTextColor(COL_TEXT_MUTE, COL_BG);
            tft.drawString("All wanted items found", 8, y, 1);
            y += 12;
        }
    }

    y += 4;
    tabContentHeight[TAB_STOCK] = y + scrollOffset[TAB_STOCK];
}

// ============================================================
// TAB 4 — DROP
// ============================================================

void drawDrop() {
    int y = -scrollOffset[TAB_DROP] + 6;

    if (!gpsFix) {
        // Warning banner
        tft.fillRect(4, y, SCREEN_W - 8, 20, COL_AMBER);
        tft.setTextColor(COL_BLACK, COL_AMBER);
        tft.setTextDatum(MC_DATUM);
        tft.drawString("NO GPS FIX — COORDINATES UNAVAILABLE", SCREEN_W / 2, y + 10, 1);
        tft.setTextDatum(TL_DATUM);
        y += 26;
    }

    // Current position
    char latBuf[24], lngBuf[24];
    if (gpsFix) {
        snprintf(latBuf, sizeof(latBuf), "%.5f N", gpsLat);
        snprintf(lngBuf, sizeof(lngBuf), "%.5f E", gpsLng);
    } else {
        strcpy(latBuf, "--- no fix ---");
        strcpy(lngBuf, "--- no fix ---");
    }

    tft.setTextColor(COL_TEXT_MUTE, COL_BG);
    tft.drawString("CURRENT POSITION", 8, y, 1);
    y += 10;
    tft.setTextColor(gpsFix ? COL_GREEN : COL_TEXT_MUTE, COL_BG);
    tft.drawString(latBuf, 8, y, 2);
    y += 18;
    tft.drawString(lngBuf, 8, y, 2);
    y += 24;

    char satBuf[24];
    if (!gpsAvailable) {
        snprintf(satBuf, sizeof(satBuf), "GPS module not connected");
    } else {
        snprintf(satBuf, sizeof(satBuf), "%d satellites  ~%.0fm accuracy", gpsSats, gpsAccM);
    }
    tft.setTextColor(COL_TEXT_MUTE, COL_BG);
    tft.drawString(satBuf, 8, y, 1);
    y += 16;

    // Session counter
    char cntBuf[32];
    snprintf(cntBuf, sizeof(cntBuf), "%d markers dropped this session", markersDroppedThisSession);
    tft.setTextColor(COL_TEXT_SEC, COL_BG);
    tft.drawString(cntBuf, 8, y, 1);
    y += 14;

    // BIG DROP BUTTON
    int btnY = y + 4;
    int btnH = 70;

    uint16_t btnBg  = gpsFix ? 0xF3BF : COL_RAISED; // dark green when active (displays as #0441)
    uint16_t btnBrd = gpsFix ? COL_GREEN : COL_BORDER;
    uint16_t btnTxt = gpsFix ? COL_GREEN : COL_TEXT_MUTE;

    tft.fillRect(4, btnY, SCREEN_W - 8, btnH, btnBg);
    tft.drawRect(4, btnY, SCREEN_W - 8, btnH, btnBrd);
    if (gpsFix) tft.drawRect(6, btnY + 2, SCREEN_W - 12, btnH - 4, COL_GREEN_DIM);

    tft.setTextColor(btnTxt, btnBg);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("DROP MARKER HERE", SCREEN_W / 2, btnY + btnH / 2 - 8, 2);
    tft.setTextColor(COL_TEXT_MUTE, btnBg);
    tft.drawString("or press physical button", SCREEN_W / 2, btnY + btnH / 2 + 8, 1);
    tft.setTextDatum(TL_DATUM);

    y = btnY + btnH + 4;
    tabContentHeight[TAB_DROP] = y + scrollOffset[TAB_DROP];
}

// ============================================================
// TAB 5 — LOG
// ============================================================

void drawLog() {
    int y = -scrollOffset[TAB_LOG] + 4;

    drawSectionHeader(y, "FIELD JOURNAL");
    y += 14;

    // New entry button
    drawButton(4, y, SCREEN_W - 8, 20, "+ NEW JOURNAL ENTRY", COL_RAISED, COL_GREEN, 1);
    y += 26;

    // Session entries
    char sessStr[32];
    snprintf(sessStr, sizeof(sessStr), "%d entries this session", journalEntriesThisSession);
    drawKV(y, "Session", sessStr);
    y += 16;

    drawDivider(y); y += 6;

    // Template quick entries
    drawSectionHeader(y, "QUICK ENTRY TEMPLATES");
    y += 14;

    const char* templates[] = {
        "Weather observation",
        "Wildlife / movement noted",
        "Vehicle / fuel status",
        "Route note",
        "Location discovery",
    };
    for (int i = 0; i < 5; i++) {
        tft.fillRect(4, y, SCREEN_W - 8, 14, COL_SURFACE);
        tft.drawRect(4, y, SCREEN_W - 8, 14, COL_BORDER);
        drawStatusDot(12, y + 7, COL_TEXT_MUTE);
        tft.setTextColor(COL_TEXT_SEC, COL_SURFACE);
        tft.drawString(templates[i], 22, y + 3, 1);
        y += 17;
    }

    // Synced journal note
    y += 4;
    tft.setTextColor(COL_TEXT_MUTE, COL_BG);
    tft.drawString("Synced LMoE journal available after sync.", 8, y, 1);
    y += 12;

    tabContentHeight[TAB_LOG] = y + scrollOffset[TAB_LOG];
}

// ============================================================
// TAB 6 — STATUS
// ============================================================

void drawStatus() {
    int y = -scrollOffset[TAB_STATUS] + 4;

    // Large clock
    String clk = gmtTimeString();
    tft.setTextColor(COL_GREEN, COL_BG);
    tft.setTextDatum(TL_DATUM);
    tft.drawString((clk + " GMT").c_str(), 8, y, 4);
    y += 38;

    char dayBuf[20];
    snprintf(dayBuf, sizeof(dayBuf), "DAY %d", currentDayNumber());
    tft.setTextColor(COL_AMBER, COL_BG);
    tft.drawString(dayBuf, 8, y, 2);
    y += 22;

    drawDivider(y); y += 6;

    // GPS
    drawSectionHeader(y, "GPS"); y += 12;
    if (!gpsAvailable) {
        drawKV(y, "Module",    "Not detected — Neo-6M pending delivery", COL_TEXT_MUTE); y += 12;
    } else {
        char fixBuf[8];
        snprintf(fixBuf, sizeof(fixBuf), gpsFix ? "YES" : "NO");
        drawKV(y, "Fix",       fixBuf, gpsFix ? COL_GREEN : COL_AMBER); y += 12;
        char satBuf[8];
        snprintf(satBuf, sizeof(satBuf), "%d", gpsSats);
        drawKV(y, "Satellites", satBuf, COL_TEXT); y += 12;
        if (gpsFix) {
            char accBuf[16];
            snprintf(accBuf, sizeof(accBuf), "~%.0fm", gpsAccM);
            drawKV(y, "Accuracy", accBuf, COL_TEXT); y += 12;
        }
    }

    drawDivider(y); y += 6;

    // WiFi
    drawSectionHeader(y, "NETWORK"); y += 12;
    drawKV(y, "WiFi", wifiConnected ? "Connected — " WIFI_SSID : "Offline — field mode",
           wifiConnected ? COL_GREEN : COL_TEXT_MUTE); y += 12;
    if (wifiConnected) {
        String lmoeStr = "lmoe-main.local:" + String(LMOE_PORT);
        drawKV(y, "LMoE", lmoeStr.c_str(), COL_TEXT_SEC); y += 12;
    }

    drawDivider(y); y += 6;

    // Session
    drawSectionHeader(y, "SESSION"); y += 12;
    char mBuf[16], jBuf[16];
    snprintf(mBuf, sizeof(mBuf), "%d", markersDroppedThisSession);
    snprintf(jBuf, sizeof(jBuf), "%d", journalEntriesThisSession);
    drawKV(y, "Markers dropped", mBuf, COL_GREEN); y += 12;
    drawKV(y, "Journal entries", jBuf, COL_TEXT);  y += 12;

    drawDivider(y); y += 6;

    // Firmware
    drawSectionHeader(y, "SYSTEM"); y += 12;
    drawKV(y, "Firmware", FIRMWARE_VERSION, COL_TEXT_MUTE); y += 12;
    drawKV(y, "Device",   ESPY_ID,          COL_TEXT_MUTE); y += 12;
    drawKV(y, "Board",    "ESP32-2432S028R", COL_TEXT_MUTE); y += 12;
    drawKV(y, "Time src", gpsFix ? "GPS (accurate)" : (wifiConnected ? "NTP" : "RTC (drift)"),
           COL_TEXT_MUTE);
    y += 16;

    tabContentHeight[TAB_STATUS] = y + scrollOffset[TAB_STATUS];
}

// ============================================================
// TOUCH HANDLING
// ============================================================

bool getTouchXY(int &x, int &y) {
    // Re-assert touch SPI pins — any SD card read/write since the last
    // touch check (e.g. saving a marker) reconfigures the shared VSPI
    // peripheral via sdSPI, which would otherwise break touch silently.
    touchSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);

    bool irq = ts.tirqTouched();
    bool touched = ts.touched();

    if (!irq || !touched) return false;

    TS_Point p = ts.getPoint();

    int rawX = p.x;
    int rawY = p.y;

#if TOUCH_SWAP_XY
    int tmp = rawX; rawX = rawY; rawY = tmp;
#endif

    x = map(rawX, TOUCH_CAL_X_MIN, TOUCH_CAL_X_MAX, 0, SCREEN_W);
    y = map(rawY, TOUCH_CAL_Y_MIN, TOUCH_CAL_Y_MAX, 0, SCREEN_H);

#if TOUCH_INVERT_X
    x = SCREEN_W - x;
#endif
#if TOUCH_INVERT_Y
    y = SCREEN_H - y;
#endif

    x = constrain(x, 0, SCREEN_W - 1);
    y = constrain(y, 0, SCREEN_H - 1);
    return true;
}

void handleTouch() {
    unsigned long now = millis();
    if (now - lastTouchTime < TOUCH_DEBOUNCE_MS) return;
    lastTouchTime = now;

    int tx, ty;
    bool down = getTouchXY(tx, ty);

    if (!down) {
        // ---- Finger lifted ----
        if (touchActive && !touchIsDrag) {
            // This was a tap (no significant movement) — dispatch as a click
            int sx = touchStartX, sy = touchStartY;

            // Tab bar
            if (sy >= SCREEN_H - TABBAR_H) {
                int tabW = SCREEN_W / TAB_COUNT;
                int tappedTab = sx / tabW;
                if (tappedTab >= 0 && tappedTab < TAB_COUNT && tappedTab != (int)activeTab) {
                    activeTab  = (Tab)tappedTab;
                    tabChanged = true;
                    drawTabBar();
                }
            }
            // Scroll arrows (top-right of content area) — only active if
            // this tab's content actually overflows
            else if (sx >= SCREEN_W - SCROLL_ARROW_W && sy < CONTENT_Y + 2 * SCROLL_ARROW_H
                     && (tabContentHeight[activeTab] - CONTENT_H) > 0) {
                int maxScroll = tabContentHeight[activeTab] - CONTENT_H;
                if (sy < CONTENT_Y + SCROLL_ARROW_H) {
                    // Up arrow
                    scrollOffset[activeTab] -= SCROLL_ARROW_STEP;
                } else {
                    // Down arrow
                    scrollOffset[activeTab] += SCROLL_ARROW_STEP;
                }
                if (scrollOffset[activeTab] < 0) scrollOffset[activeTab] = 0;
                if (scrollOffset[activeTab] > maxScroll) scrollOffset[activeTab] = maxScroll;
                tabChanged = true;
            }
            // Content area — dispatch to per-tab handler
            else {
                // Convert to "natural" (unscrolled) content coordinate
                int cy = (sy - CONTENT_Y) + scrollOffset[activeTab];
                switch (activeTab) {
                    case TAB_MISSION: handleTouchMission(sx, cy); break;
                    case TAB_STOCK:   handleTouchStock(sx, cy);   break;
                    case TAB_DROP:    handleTouchDrop(sx, cy);    break;
                    case TAB_LOG:     handleTouchLog(sx, cy);     break;
                    default: break;
                }
            }
        }
        touchActive = false;
        touchIsDrag = false;
        return;
    }

    // ---- Finger down ----
    if (!touchActive) {
        // Press just started
        touchActive   = true;
        touchIsDrag   = false;
        touchStartX   = tx;
        touchStartY   = ty;
        touchLastY    = ty;
        return;
    }

    // ---- Finger held/moving ----
    int dy = ty - touchLastY;
    int totalDy = ty - touchStartY;

    bool tabScrollable = (tabContentHeight[activeTab] - CONTENT_H) > 0;

    if (!touchIsDrag && tabScrollable && abs(totalDy) > SCROLL_DRAG_THRESHOLD) {
        touchIsDrag = true;
    }

    if (touchIsDrag) {
        // Only scroll if the drag is within the content area (not tab bar)
        if (touchStartY < SCREEN_H - TABBAR_H) {
            int maxScroll = tabContentHeight[activeTab] - CONTENT_H;
            if (maxScroll < 0) maxScroll = 0;
            // Dragging finger UP (negative dy) reveals content below -> increase offset
            scrollOffset[activeTab] -= dy;
            if (scrollOffset[activeTab] < 0) scrollOffset[activeTab] = 0;
            if (scrollOffset[activeTab] > maxScroll) scrollOffset[activeTab] = maxScroll;
            tabChanged = true;
        }
    }

    touchLastY = ty;
}

// Per-tab touch — Mission (FOUND buttons)
void handleTouchMission(int tx, int ty) {
    int y = 4 + 40 + 12 + 12 + 12 + 18 + 6 + 14; // match drawMission() offsets (natural, scrollOffset=0)
    for (int i = 0; i < wantedCount; i++) {
        WantedItem& item = wantedItems[i];
        if (item.found) continue;
        // FOUND button region: SCREEN_W-52 to SCREEN_W-4, y-1 to y+12
        if (tx >= SCREEN_W - 52 && tx <= SCREEN_W - 4 && ty >= y - 1 && ty <= y + 12) {
            item.found = true;
            saveWantedUpdate(item.id);
            tabChanged = true; // Redraw
            return;
        }
        y += 14;
    }
}

// Per-tab touch — Stock (FOUND buttons)
void handleTouchStock(int tx, int ty) {
    int y = 4 + 14; // after "CRITICAL/WARNING RESOURCES" header (natural, scrollOffset=0)
    if (resourceCount == 0) {
        y += 36; // empty-state card
    } else {
        y += resourceCount * 22;
    }
    y += 6 + 14; // divider + "FIND ON THIS RUN" header

    for (int i = 0; i < wantedCount; i++) {
        WantedItem& item = wantedItems[i];
        if (item.found) continue;
        if (tx >= SCREEN_W - 52 && tx <= SCREEN_W - 4 && ty >= y - 1 && ty <= y + 12) {
            item.found = true;
            saveWantedUpdate(item.id);
            tabChanged = true;
            return;
        }
        y += 14;
    }
}

// Per-tab touch — Drop (big button)
void handleTouchDrop(int tx, int ty) {
    // The big button occupies the lower portion of this tab. Require the
    // tap to be below the position/info text (natural y > 60) so reading
    // the GPS coordinates doesn't accidentally trigger a drop.
    if (ty > 60) {
        handleDropMarker();
    }
}

// Per-tab touch — Log (new entry button)
void handleTouchLog(int tx, int ty) {
    int btnY = 4 + 14; // natural, scrollOffset=0 — matches drawLog()
    if (ty >= btnY && ty <= btnY + 20) {
        // TODO: open simple text-entry dialogue in a future session
        // For now, create a blank timestamped entry
        createBlankJournalEntry();
        tabChanged = true;
    }
}

// ============================================================
// DROP MARKER
// ============================================================

void handleDropMarker() {
    // Ensure full-screen drawing regardless of any prior viewport state
    tft.resetViewport();

    // Visual flash on the display regardless of which tab is active
    tft.fillRect(0, 0, SCREEN_W, HEADER_H, COL_GREEN);
    tft.setTextColor(COL_BLACK, COL_GREEN);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("MARKER DROPPED", SCREEN_W / 2, HEADER_H / 2, 2);
    tft.setTextDatum(TL_DATUM);

    double lat = gpsFix ? gpsLat : 0.0;
    double lng = gpsFix ? gpsLng : 0.0;

    saveDropMarker(lat, lng);
    markersDroppedThisSession++;

    // LED flash green
    digitalWrite(LED_G_PIN, LOW);
    delay(200);
    digitalWrite(LED_G_PIN, HIGH);

    delay(500);

    // Restore header and content
    drawHeader();
    drawTabBar();
    if (activeTab != TAB_DROP) drawContent();
    else { tabChanged = true; }
}

// ============================================================
// SD CARD — LOAD
// ============================================================

void loadConfigFromSD() {
    if (!SD.exists("/CONFIG/config.json")) return;
    File f = SD.open("/CONFIG/config.json", FILE_READ);
    if (!f) return;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) return;

    // Day zero from config
    const char* dz = doc["day_zero_epoch"];
    if (dz) dayZeroEpoch = (time_t)atol(dz);
}

void loadSyncedMarkersFromSD() {
    syncedMarkerCount = 0;
    if (!SD.exists("/LMOE/markers.json")) return;
    File f = SD.open("/LMOE/markers.json", FILE_READ);
    if (!f) return;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) return;

    JsonArray items = doc["markers"];
    for (JsonObject m : items) {
        if (syncedMarkerCount >= 50) break;
        syncedMarkers[syncedMarkerCount].id     = m["id"]     | "";
        syncedMarkers[syncedMarkerCount].name   = m["name"]   | "Unknown";
        syncedMarkers[syncedMarkerCount].cat    = m["cat"]    | "";
        syncedMarkers[syncedMarkerCount].status = m["status"] | "active";
        syncedMarkers[syncedMarkerCount].lat    = m["lat"]    | 0.0;
        syncedMarkers[syncedMarkerCount].lng    = m["lng"]    | 0.0;
        syncedMarkerCount++;
    }
    Serial.printf("[SD] Loaded %d synced markers\n", syncedMarkerCount);
}

void loadMapMetaFromSD() {
    mapMeta.valid = false;
    if (!SD.exists("/LMOE/map-meta.json")) return;
    File f = SD.open("/LMOE/map-meta.json", FILE_READ);
    if (!f) return;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) return;

    mapMeta.centreLat   = doc["lat"]         | 0.0;
    mapMeta.centreLng   = doc["lng"]         | 0.0;
    mapMeta.zoom        = doc["zoom"]        | 14;
    mapMeta.destination = doc["destination"] | "";
    mapMeta.run         = doc["run"]         | "";
    mapMeta.generated   = doc["generated"]   | "";
    mapMeta.valid       = (mapMeta.centreLat != 0.0 || mapMeta.centreLng != 0.0);
    Serial.printf("[SD] Map meta: %.4f, %.4f z%d\n",
                  mapMeta.centreLat, mapMeta.centreLng, mapMeta.zoom);
}

// Binary-safe HTTP GET to SD file — uses streaming to handle any content type.
// httpGetToFile() uses getString() which corrupts binary data (null bytes etc).
static bool httpGetBinaryToFile(const String& url, const char* sdPath) {
    HTTPClient http;
    http.begin(url);
    int code = http.GET();

    if (code != 200) {
        Serial.printf("[SYNC] Binary GET %s -> %d\n", url.c_str(), code);
        http.end();
        return false;
    }

    int length = http.getSize();
    WiFiClient* stream = http.getStreamPtr();

    File f = SD.open(sdPath, FILE_WRITE);
    if (!f) {
        Serial.printf("[SYNC] Failed to open %s for write\n", sdPath);
        http.end();
        return false;
    }

    uint8_t buf[256];
    int totalRead = 0;
    unsigned long start = millis();

    while ((length > 0 || length == -1) && stream->connected() && (millis() - start < 15000)) {
        int avail = stream->available();
        if (avail > 0) {
            int toRead = min(avail, (int)sizeof(buf));
            if (length != -1) toRead = min(toRead, length);
            int bytesRead = stream->readBytes(buf, toRead);
            f.write(buf, bytesRead);
            totalRead += bytesRead;
            if (length != -1) length -= bytesRead;
        } else {
            delay(1);
        }
    }

    f.close();
    http.end();
    Serial.printf("[SYNC] Binary GET %s -> %d bytes\n", sdPath, totalRead);
    return totalRead > 54; // BMP header is 54 bytes minimum
}
    if (!SD.exists("/LMOE/mission.json")) return;
    File f = SD.open("/LMOE/mission.json", FILE_READ);
    if (!f) return;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) return;

    missionName        = doc["run"]["name"]        | "Unnamed Run";
    missionDestination = doc["run"]["destination"] | "—";
    missionVehicle     = doc["run"]["vehicle"]     | "—";
    missionDate        = doc["run"]["date"]        | "—";
}

void loadWantedFromSD() {
    if (!SD.exists("/LMOE/wanted.json")) return;
    File f = SD.open("/LMOE/wanted.json", FILE_READ);
    if (!f) return;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) return;

    wantedCount = 0;
    JsonArray items = doc["items"];
    for (JsonObject item : items) {
        if (wantedCount >= 20) break;
        wantedItems[wantedCount].id       = item["id"]       | "";
        wantedItems[wantedCount].name     = item["name"]     | "Unknown";
        wantedItems[wantedCount].priority = item["priority"] | "medium";
        wantedItems[wantedCount].found    = false;
        wantedCount++;
    }
}

void loadResourcesFromSD() {
    if (!SD.exists("/LMOE/resources.json")) return;
    File f = SD.open("/LMOE/resources.json", FILE_READ);
    if (!f) return;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) return;

    resourceCount = 0;
    JsonArray items = doc["items"];
    for (JsonObject item : items) {
        if (resourceCount >= 20) break;
        resources[resourceCount].name          = item["name"]           | "Unknown";
        resources[resourceCount].unit          = item["unit"]           | "units";
        resources[resourceCount].qty           = item["qty"]            | 0.0f;
        resources[resourceCount].daysRemaining = item["days_remaining"] | 0;
        resources[resourceCount].status        = item["status"]         | "warning";
        resourceCount++;
    }
}

// ============================================================
// SD CARD — SAVE FIELD DATA
// ============================================================

void ensureDir(const char* path) {
    if (!SD.exists(path)) SD.mkdir(path);
}

void saveDropMarker(double lat, double lng) {
    ensureDir("/FIELD");
    ensureDir("/FIELD/markers");

    // Unique filename: marker_<millis>.json
    char fname[40];
    snprintf(fname, sizeof(fname), "/FIELD/markers/mk_%lu.json", millis());

    File f = SD.open(fname, FILE_WRITE);
    if (!f) {
        Serial.println("[SD] Failed to open marker file for write");
        return;
    }

    JsonDocument doc;
    char idBuf[24];
    snprintf(idBuf, sizeof(idBuf), "mk_%lu", millis());
    doc["id"]          = idBuf;
    doc["dropped_at"]  = isoTimestamp();
    doc["lat"]         = lat;
    doc["lng"]         = lng;
    doc["gps_accuracy"] = gpsAccM;
    doc["cat"]         = "unknown";
    doc["name"]        = "";
    doc["notes"]       = "";
    doc["status"]      = "unknown";
    doc["reviewed"]    = false;

    serializeJson(doc, f);
    f.close();
    Serial.printf("[SD] Marker saved: %s  lat=%.5f lng=%.5f\n", fname, lat, lng);
}

void saveWantedUpdate(String itemId) {
    ensureDir("/FIELD");

    File f = SD.open("/FIELD/wanted_updates.json", FILE_APPEND);
    if (!f) return;

    JsonDocument doc;
    doc["id"]             = itemId;
    doc["found"]          = true;
    doc["found_at_lat"]   = gpsFix ? gpsLat : 0.0;
    doc["found_at_lng"]   = gpsFix ? gpsLng : 0.0;
    doc["found_at_name"]  = "";
    doc["quantity_found"] = "";
    doc["notes"]          = "";

    serializeJson(doc, f);
    f.println(); // newline-delimited JSON
    f.close();
}

void createBlankJournalEntry() {
    ensureDir("/FIELD");
    ensureDir("/FIELD/journal");

    char fname[40];
    snprintf(fname, sizeof(fname), "/FIELD/journal/je_%lu.json", millis());

    File f = SD.open(fname, FILE_WRITE);
    if (!f) return;

    JsonDocument doc;
    char idBuf[24];
    snprintf(idBuf, sizeof(idBuf), "je_%lu", millis());
    doc["id"]         = idBuf;
    doc["created_at"] = isoTimestamp();
    doc["day"]        = currentDayNumber();
    doc["cat"]        = "log";
    doc["title"]      = "Field note";
    doc["body"]       = "";

    serializeJson(doc, f);
    f.close();
    journalEntriesThisSession++;
    Serial.printf("[SD] Journal entry created: %s\n", fname);
}

// ============================================================
// WIFI
// ============================================================

void connectWiFi() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - start < WIFI_TIMEOUT_MS)) {
        delay(250);
    }

    wifiConnected = (WiFi.status() == WL_CONNECTED);
    if (wifiConnected) {
        Serial.printf("[WiFi] Connected — IP: %s\n", WiFi.localIP().toString().c_str());
        // Post status immediately
        lastStatusPost = 0;
    } else {
        Serial.println("[WiFi] Could not connect — field mode");
        WiFi.disconnect(true);
    }
}

// ============================================================
// REST — POST ESPY STATUS TO LMOE
// ============================================================

// Count newline-delimited JSON entries in /FIELD/wanted_updates.json
int countWantedUpdatesOnSD() {
    if (!SD.exists("/FIELD/wanted_updates.json")) return 0;
    File f = SD.open("/FIELD/wanted_updates.json", FILE_READ);
    if (!f) return 0;
    int count = 0;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        if (line.length() > 2) count++; // skip blank trailing lines
    }
    f.close();
    return count;
}

// Count files in an SD directory matching a prefix (e.g. counting
// markers/journal entries pending sync)
int countFilesInDir(const char* dirPath) {
    if (!SD.exists(dirPath)) return 0;
    File dir = SD.open(dirPath);
    if (!dir || !dir.isDirectory()) return 0;
    int count = 0;
    File entry = dir.openNextFile();
    while (entry) {
        if (!entry.isDirectory()) count++;
        entry.close();
        entry = dir.openNextFile();
    }
    dir.close();
    return count;
}

bool postEspyStatus() {
    if (!wifiConnected) return false;

    String url = "http://" + String(LMOE_HOST) + ":" + String(LMOE_PORT) + "/api/espy/status";

    int markersPending = countFilesInDir("/FIELD/markers");
    int journalPending  = countFilesInDir("/FIELD/journal");
    int wantedPending   = countWantedUpdatesOnSD();

    JsonDocument doc;
    doc["type"]                    = "espy_status";
    doc["espy_id"]                 = ESPY_ID;
    doc["firmware_version"]        = FIRMWARE_VERSION;
    doc["connected_at"]            = isoTimestamp();
    doc["gps_fix"]                 = gpsFix;
    doc["gps_satellites"]          = gpsSats;
    doc["gps_accuracy"]            = (int)gpsAccM;
    doc["last_lat"]                = gpsLat;
    doc["last_lng"]                = gpsLng;
    doc["wifi_rssi"]               = WiFi.RSSI();
    doc["field_markers_pending"]   = markersPending;
    doc["field_journal_pending"]   = journalPending;
    doc["wanted_updates_pending"]  = wantedPending;

    String body;
    serializeJson(doc, body);

    HTTPClient http;
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    int code = http.POST(body);

    bool syncRequested = false;

    if (code > 0) {
        Serial.printf("[HTTP] Status POST → %d\n", code);
        String response = http.getString();
        JsonDocument respDoc;
        if (deserializeJson(respDoc, response) == DeserializationError::Ok) {
            syncRequested = respDoc["sync_requested"] | false;
        }
    } else {
        Serial.printf("[HTTP] Status POST failed: %s\n", http.errorToString(code).c_str());
    }
    http.end();

    return syncRequested;
}

// ============================================================
// SYNC — pull mission/wanted/resources from LMoE, push field data
// ============================================================

// Fetch a JSON endpoint and write the raw response body to an SD file.
// Returns true on success (HTTP 200 and non-empty body).
static bool httpGetToFile(const String& url, const char* sdPath) {
    HTTPClient http;
    http.begin(url);
    int code = http.GET();

    if (code != 200) {
        Serial.printf("[SYNC] GET %s -> %d\n", url.c_str(), code);
        http.end();
        return false;
    }

    String body = http.getString();
    http.end();

    if (body.length() < 2) {
        Serial.printf("[SYNC] GET %s -> empty body\n", url.c_str());
        return false;
    }

    File f = SD.open(sdPath, FILE_WRITE);
    if (!f) {
        Serial.printf("[SYNC] Failed to open %s for write\n", sdPath);
        return false;
    }
    f.print(body);
    f.close();
    return true;
}

// Pull mission.json, wanted.json, resources.json from LMoE and overwrite
// the SD copies, then reload into memory. Returns true if at least one
// file was successfully updated.
bool syncPullLmoeData() {
    String base = "http://" + String(LMOE_HOST) + ":" + String(LMOE_PORT) + "/api/espy/";

    ensureDir("/LMOE");

    bool gotMission   = httpGetToFile(base + "mission",   "/LMOE/mission.json");
    bool gotWanted    = httpGetToFile(base + "wanted",    "/LMOE/wanted.json");
    bool gotResources = httpGetToFile(base + "resources", "/LMOE/resources.json");
    bool gotMarkers   = httpGetToFile(base + "markers",   "/LMOE/markers.json");
    bool gotMapMeta   = httpGetToFile(base + "map-meta",  "/LMOE/map-meta.json");
    bool gotMapImage  = httpGetBinaryToFile(base + "map-image", "/LMOE/map.bmp");

    if (gotMission)   loadMissionFromSD();
    if (gotWanted)    loadWantedFromSD();
    if (gotResources) loadResourcesFromSD();
    if (gotMarkers)   loadSyncedMarkersFromSD();
    if (gotMapMeta)   loadMapMetaFromSD();
    if (gotMapImage)  mapImageAvailable = true;

    Serial.printf("[SYNC] Pull: mission=%d wanted=%d resources=%d markers=%d mapMeta=%d mapImage=%d\n",
                   gotMission, gotWanted, gotResources, gotMarkers, gotMapMeta, gotMapImage);

    return gotMission || gotWanted || gotResources;
}

// Read all files in an SD directory as a JSON array, appending each
// file's parsed contents as an element of `arr`. Used for markers and
// journal entries, which are stored as one file per entry.
static void appendDirAsJsonArray(const char* dirPath, JsonArray& arr) {
    if (!SD.exists(dirPath)) return;
    File dir = SD.open(dirPath);
    if (!dir || !dir.isDirectory()) return;

    File entry = dir.openNextFile();
    while (entry) {
        if (!entry.isDirectory()) {
            JsonDocument itemDoc;
            if (deserializeJson(itemDoc, entry) == DeserializationError::Ok) {
                arr.add(itemDoc.as<JsonObject>());
            }
        }
        entry.close();
        entry = dir.openNextFile();
    }
    dir.close();
}

// Delete all files in an SD directory (after a successful push)
static void clearDir(const char* dirPath) {
    if (!SD.exists(dirPath)) return;
    File dir = SD.open(dirPath);
    if (!dir || !dir.isDirectory()) return;

    File entry = dir.openNextFile();
    while (entry) {
        if (!entry.isDirectory()) {
            String name = String(dirPath) + "/" + entry.name();
            entry.close();
            SD.remove(name);
        } else {
            entry.close();
        }
        entry = dir.openNextFile();
    }
    dir.close();
}

// Push accumulated /FIELD/ data (markers, journal entries, wanted updates)
// to LMoE in a single request. On success, clears the pushed files from
// SD and resets session counters. Returns true on success.
bool syncPushFieldData() {
    JsonDocument doc;
    JsonArray markers = doc["markers"].to<JsonArray>();
    JsonArray journal = doc["journal"].to<JsonArray>();
    JsonArray wantedUpdates = doc["wanted_updates"].to<JsonArray>();

    appendDirAsJsonArray("/FIELD/markers", markers);
    appendDirAsJsonArray("/FIELD/journal", journal);

    // wanted_updates.json is newline-delimited JSON, not one-file-per-entry
    if (SD.exists("/FIELD/wanted_updates.json")) {
        File f = SD.open("/FIELD/wanted_updates.json", FILE_READ);
        if (f) {
            while (f.available()) {
                String line = f.readStringUntil('\n');
                if (line.length() < 2) continue;
                JsonDocument lineDoc;
                if (deserializeJson(lineDoc, line) == DeserializationError::Ok) {
                    wantedUpdates.add(lineDoc.as<JsonObject>());
                }
            }
            f.close();
        }
    }

    int markerCount = markers.size();
    int journalCount = journal.size();
    int wantedCount_ = wantedUpdates.size();

    if (markerCount == 0 && journalCount == 0 && wantedCount_ == 0) {
        Serial.println("[SYNC] Push: nothing to send");
        return true; // nothing to do is not a failure
    }

    String body;
    serializeJson(doc, body);

    String url = "http://" + String(LMOE_HOST) + ":" + String(LMOE_PORT) + "/api/espy/sync/push";
    HTTPClient http;
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    int code = http.POST(body);

    bool ok = false;
    if (code == 200) {
        Serial.printf("[SYNC] Push -> 200 (markers=%d journal=%d wanted=%d)\n",
                       markerCount, journalCount, wantedCount_);
        ok = true;
    } else {
        Serial.printf("[SYNC] Push -> %d\n", code);
    }
    http.end();

    if (ok) {
        clearDir("/FIELD/markers");
        clearDir("/FIELD/journal");
        if (SD.exists("/FIELD/wanted_updates.json")) SD.remove("/FIELD/wanted_updates.json");
        markersDroppedThisSession  = 0;
        journalEntriesThisSession  = 0;
    }

    return ok;
}

// Full sync cycle: pull fresh briefing data, push accumulated field data.
// Shows a brief on-screen indicator since this can take a few seconds.
void performSync() {
    Serial.println("[SYNC] Starting sync...");

    tft.resetViewport();
    tft.fillRect(0, 0, SCREEN_W, HEADER_H, COL_AMBER);
    tft.setTextColor(COL_BLACK, COL_AMBER);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("SYNCING WITH LMOE...", SCREEN_W / 2, HEADER_H / 2, 2);
    tft.setTextDatum(TL_DATUM);

    bool pullOk = syncPullLmoeData();
    bool pushOk = syncPushFieldData();

    Serial.printf("[SYNC] Complete: pull=%d push=%d\n", pullOk, pushOk);

    drawHeader();
    drawTabBar();
    tabChanged = true;
}
