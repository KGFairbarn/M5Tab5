/**
 * M5Stack Tab5 - Ruby Hub (Math, Words & CYD Daily Planner)
 * Target Board: M5Tab5 (ESP32-P4)
 */

#include <Arduino.h>
#include <M5Unified.h>
#include <Preferences.h>
#include <WiFi.h>
#include <nvs_flash.h>
#include "Globals.h"
#if __has_include("Secrets.h")
#include "Secrets.h"
#else
#include "Secrets.h.example"
#endif
#include "MathGames.h"
#include "WordGames.h"
#include "DiceGame.h"
#include "DailyPlanner.h"
#include "Geocache.h"
#include "FlightRadar.h"
#include "MeshtasticBLE.h"

Preferences prefs;

// Define Global Extern Variables ONCE here
int sw = 720;
int sh = 1280;
AppState currentState = STATE_MAIN_MENU;

bool useAdd = true;
bool useSub = true;
bool useMult = false;
bool useDiv = false;
int maxNumLimit = 20;
bool useMultipleChoice = true;
String mathManualInput = "";
int spellShowTime = 3000;
bool soundEnabled = true;
int numDice = 2;
int numRolls = 5;

// Power & Wi-Fi Globals
int sleepTimeoutMins = 5;
unsigned long lastActivityTime = 0;
String wifiSSID = SECRET_WIFI_SSID;
String wifiPass = SECRET_WIFI_PASS;
bool wifiHardwareInitialized = false;

// Wi-Fi Editor State
int wifiEditField = 0; // 0 = SSID, 1 = Password
bool wifiCaps = false;
String wifiStatusMsg = "Tap a box above, type with keyboard, then tap CONNECT.";

void loadSettings() {
    prefs.begin("rubyhub", true);
    useAdd = prefs.getBool("useAdd", true);
    useSub = prefs.getBool("useSub", true);
    useMult = prefs.getBool("useMult", false);
    useDiv = prefs.getBool("useDiv", false);
    maxNumLimit = prefs.getInt("maxLim", 20);
    useMultipleChoice = prefs.getBool("multi", true);
    spellShowTime = prefs.getInt("spellT", 3000);
    soundEnabled = prefs.getBool("sound", true);
    numDice = prefs.getInt("nDice", 2);
    numRolls = prefs.getInt("nRolls", 5);
    sleepTimeoutMins = prefs.getInt("sleepM", 5);
    wifiSSID = prefs.getString("ssid", SECRET_WIFI_SSID);
    wifiPass = prefs.getString("pass", SECRET_WIFI_PASS);
    if (wifiSSID.length() == 0) {
        wifiSSID = SECRET_WIFI_SSID;
        wifiPass = SECRET_WIFI_PASS;
    }
    prefs.end();
}

void saveSettings() {
    prefs.begin("rubyhub", false);
    prefs.putBool("useAdd", useAdd);
    prefs.putBool("useSub", useSub);
    prefs.putBool("useMult", useMult);
    prefs.putBool("useDiv", useDiv);
    prefs.putInt("maxLim", maxNumLimit);
    prefs.putBool("multi", useMultipleChoice);
    prefs.putInt("spellT", spellShowTime);
    prefs.putBool("sound", soundEnabled);
    prefs.putInt("nDice", numDice);
    prefs.putInt("nRolls", numRolls);
    prefs.putInt("sleepM", sleepTimeoutMins);
    prefs.putString("ssid", wifiSSID);
    prefs.putString("pass", wifiPass);
    prefs.end();
}

void connectToWiFi() {
    if (wifiSSID.length() == 0) return;

#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 3, 0)
    if (!wifiHardwareInitialized) {
        // M5Stack Tab5 ESP32-C6 SDIO pins
        WiFi.setPins(12, 13, 11, 10, 9, 8, 15);
        wifiHardwareInitialized = true;
    }
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(wifiSSID.c_str(), wifiPass.c_str());
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 6000) {
        delay(200);
    }
    if (WiFi.status() == WL_CONNECTED) {
        configTzTime("EST5EDT,M3.2.0,M11.1.0", "pool.ntp.org", "time.nist.gov");
    }
#else
    Serial.println("Wi-Fi skipped: Arduino Core 3.2.0 lacks WiFi.setPins() for Tab5 SDIO.");
#endif
}

// ======================== SETUP ==================================
void setup() {
    delay(1500); 
    Serial.begin(115200);

    // Initialize NVS (Fixes the Preferences NOT_FOUND error)
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    auto cfg = M5.config();
    M5.begin(cfg);

    M5.Display.setRotation(0);
    sw = M5.Display.width();
    sh = M5.Display.height();
    if (sw > sh) {
        M5.Display.setRotation(1);
        sw = M5.Display.width();
        sh = M5.Display.height();
    }

    M5.Speaker.setVolume(120);
    randomSeed(esp_random());

    loadSettings();
#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 3, 0)
    if (!wifiHardwareInitialized) {
        // M5Stack Tab5 ESP32-C6 SDIO pins (required for both Wi-Fi and BLE via ESP-Hosted)
        WiFi.setPins(12, 13, 11, 10, 9, 8, 15);
        wifiHardwareInitialized = true;
    }
#endif
    connectToWiFi();

    lastActivityTime = millis();
    drawMainMenu();
}

void playTone(int freq, int duration) {
    if (soundEnabled && M5.Speaker.isEnabled()) M5.Speaker.tone(freq, duration);
}

// ======================== MENUS ==================================
void drawMainMenu() {
    M5.Display.fillScreen(C_BG);
    M5.Display.fillRoundRect(sw*0.08, 22, sw*0.84, 96, 22, C_BLUE);
    M5.Display.setTextDatum(middle_center);
    M5.Display.setFont(&fonts::FreeSansBold24pt7b);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.drawString("RUBY HUB", sw/2, 70);

    // Wi-Fi Status Indicator
    M5.Display.fillCircle(sw*0.08 + sw*0.84 - 25, 22 + 24, 11, (WiFi.status() == WL_CONNECTED) ? C_GREEN : C_RED);

    // 1. Math Explorer (y: 136..274)
    M5.Display.fillRoundRect(sw*0.08, 136, sw*0.84, 138, 22, C_ORANGE);
    M5.Display.setFont(&fonts::FreeSansBold24pt7b);
    M5.Display.drawString("MATH EXPLORER", sw/2, 184);
    M5.Display.setFont(&fonts::FreeSans12pt7b);
    M5.Display.drawString("Numbers, Logic & Dice", sw/2, 236);

    // 2. Word Explorer (y: 290..428)
    M5.Display.fillRoundRect(sw*0.08, 290, sw*0.84, 138, 22, C_PURPLE);
    M5.Display.setFont(&fonts::FreeSansBold24pt7b);
    M5.Display.drawString("WORD EXPLORER", sw/2, 338);
    M5.Display.setFont(&fonts::FreeSans12pt7b);
    M5.Display.drawString("Spelling, Vocab & Apple Tree", sw/2, 390);

    // 3. Daily Planner (y: 444..582)
    M5.Display.fillRoundRect(sw*0.08, 444, sw*0.84, 138, 22, C_TEAL);
    M5.Display.setFont(&fonts::FreeSansBold24pt7b);
    M5.Display.drawString("DAILY PLANNER", sw/2, 492);
    M5.Display.setFont(&fonts::FreeSans12pt7b);
    M5.Display.drawString("Weather, Tempest & Schedule", sw/2, 544);

    // 4. Geocache (y: 598..736)
    M5.Display.fillRoundRect(sw*0.08, 598, sw*0.84, 138, 22, C_GREEN);
    M5.Display.setFont(&fonts::FreeSansBold24pt7b);
    M5.Display.drawString("GEOCACHE", sw/2, 646);
    M5.Display.setFont(&fonts::FreeSans12pt7b);
    M5.Display.drawString("Offline Maps, Compass & GPX", sw/2, 698);

    // 5. Flight Radar (y: 752..890)
    M5.Display.fillRoundRect(sw*0.08, 752, sw*0.84, 138, 22, 0x02D4);
    M5.Display.setFont(&fonts::FreeSansBold24pt7b);
    M5.Display.drawString("FLIGHT RADAR", sw/2, 800);
    M5.Display.setFont(&fonts::FreeSans12pt7b);
    M5.Display.drawString("Live ADS-B Planes & Scope", sw/2, 852);

    // 6. Meshtastic (y: 906..1044)
    M5.Display.fillRoundRect(sw*0.08, 906, sw*0.84, 138, 22, C_BLUE);
    M5.Display.fillCircle(sw*0.08 + sw*0.84 - 25, 906 + 24, 10, isMeshBleConnected() ? C_GREEN : C_GRAY);
    M5.Display.setFont(&fonts::FreeSansBold24pt7b);
    M5.Display.drawString("MESHTASTIC", sw/2, 954);
    M5.Display.setFont(&fonts::FreeSans12pt7b);
    M5.Display.drawString("T114 BLE Chat, Nodes & GPS", sw/2, 1006);

    // 7. Settings (y: 1060..1198)
    M5.Display.fillRoundRect(sw*0.08, 1060, sw*0.84, 138, 22, 0x528A);
    M5.Display.setFont(&fonts::FreeSansBold24pt7b);
    M5.Display.drawString("SETTINGS", sw/2, 1108);
    M5.Display.setFont(&fonts::FreeSans12pt7b);
    M5.Display.drawString("Rules, Sleep Timer & Wi-Fi", sw/2, 1160);
}

void drawMathMenu() {
    M5.Display.fillScreen(C_BG);
    M5.Display.fillRoundRect(24, 24, 140, 64, 16, C_GRAY); M5.Display.setTextDatum(middle_center); M5.Display.setFont(&fonts::FreeSansBold12pt7b); M5.Display.setTextColor(0x3186); M5.Display.drawString("< BACK", 94, 56);
    M5.Display.setTextColor(C_ORANGE); M5.Display.setFont(&fonts::FreeSansBold24pt7b); M5.Display.drawString("MATH EXPLORER", sw/2, 140);
    M5.Display.fillRoundRect(sw*0.1, 200, sw*0.8, 160, 24, C_GREEN); M5.Display.setTextColor(TFT_WHITE); M5.Display.drawString("FLASH CARDS", sw/2, 280);
    M5.Display.fillRoundRect(sw*0.1, 400, sw*0.8, 160, 24, C_BLUE); M5.Display.drawString("TILE PUZZLE", sw/2, 480);
    M5.Display.fillRoundRect(sw*0.1, 600, sw*0.8, 160, 24, C_RED); M5.Display.drawString("DICE COUNTER", sw/2, 680);
}

void drawWordMenu() {
    M5.Display.fillScreen(C_BG);
    M5.Display.fillRoundRect(24, 24, 140, 64, 16, C_GRAY); M5.Display.setTextDatum(middle_center); M5.Display.setFont(&fonts::FreeSansBold12pt7b); M5.Display.setTextColor(0x3186); M5.Display.drawString("< BACK", 94, 56);
    M5.Display.setTextColor(C_PURPLE); M5.Display.setFont(&fonts::FreeSansBold24pt7b); M5.Display.drawString("WORD EXPLORER", sw/2, 140);
    M5.Display.fillRoundRect(sw*0.1, 220, sw*0.8, 180, 24, C_RED); M5.Display.setTextColor(TFT_WHITE); M5.Display.drawString("APPLE TREE", sw/2, 290);
    M5.Display.setFont(&fonts::FreeSans12pt7b); M5.Display.drawString("Find the missing letters", sw/2, 340);
    M5.Display.fillRoundRect(sw*0.1, 460, sw*0.8, 180, 24, C_ORANGE); M5.Display.setFont(&fonts::FreeSansBold24pt7b); M5.Display.drawString("SPELLING BEE", sw/2, 530);
    M5.Display.setFont(&fonts::FreeSans12pt7b); M5.Display.drawString("Memorize and Spell", sw/2, 580);
}

// ======================== SETTINGS SCREEN ========================
void drawSettings() {
    M5.Display.fillScreen(C_BG);
    M5.Display.fillRoundRect(sw*0.08, 15, sw*0.84, 65, 16, 0x528A);
    M5.Display.setTextDatum(middle_center); M5.Display.setFont(&fonts::FreeSansBold18pt7b); M5.Display.setTextColor(TFT_WHITE); M5.Display.drawString("RUBY HUB SETTINGS", sw/2, 47);
    
    // 1. Math Operations
    M5.Display.setTextColor(0x3186); M5.Display.setFont(&fonts::FreeSansBold12pt7b); M5.Display.drawString("MATH: ALLOWED OPERATIONS", sw/2, 105);
    int btnW = 120, btnH = 60, startX = 60, gap = 40, y1 = 125;
    M5.Display.fillRoundRect(startX, y1, btnW, btnH, 16, useAdd ? C_GREEN : 0xC618); M5.Display.setTextColor(TFT_WHITE); M5.Display.setFont(&fonts::FreeSansBold24pt7b); M5.Display.drawString("+", startX + btnW/2, y1 + btnH/2);
    M5.Display.fillRoundRect(startX + btnW + gap, y1, btnW, btnH, 16, useSub ? C_GREEN : 0xC618); M5.Display.drawString("-", startX + btnW + gap + btnW/2, y1 + btnH/2);
    M5.Display.fillRoundRect(startX + 2*(btnW + gap), y1, btnW, btnH, 16, useMult ? C_GREEN : 0xC618); M5.Display.drawString("x", startX + 2*(btnW + gap) + btnW/2, y1 + btnH/2 - 5);
    M5.Display.fillRoundRect(startX + 3*(btnW + gap), y1, btnW, btnH, 16, useDiv ? C_GREEN : 0xC618); M5.Display.drawString("/", startX + 3*(btnW + gap) + btnW/2, y1 + btnH/2);

    // 2. Flashcard Mode
    M5.Display.setTextColor(0x3186); M5.Display.setFont(&fonts::FreeSansBold12pt7b); M5.Display.drawString("MATH: FLASH CARD ENTRY", sw/2, 210);
    M5.Display.fillRoundRect(60, 225, 280, 55, 16, useMultipleChoice ? C_GREEN : 0xC618); M5.Display.setTextColor(TFT_WHITE); M5.Display.drawString("MULTI-CHOICE", 60 + 140, 225 + 27);
    M5.Display.fillRoundRect(380, 225, 280, 55, 16, !useMultipleChoice ? C_GREEN : 0xC618); M5.Display.drawString("KEYPAD", 380 + 140, 225 + 27);

    // 3. Max Number Limit
    M5.Display.setTextColor(0x3186); M5.Display.drawString("MATH: MAX NUMBER LIMIT", sw/2, 305);
    M5.Display.fillRoundRect(sw*0.15, 325, 120, 60, 16, C_RED); M5.Display.setTextColor(TFT_WHITE); M5.Display.setFont(&fonts::FreeSansBold24pt7b); M5.Display.drawString("-", sw*0.15 + 60, 355);
    M5.Display.fillRoundRect(sw*0.35, 325, sw*0.30, 60, 16, TFT_WHITE); M5.Display.drawRoundRect(sw*0.35, 325, sw*0.30, 60, 16, C_BLUE); M5.Display.setTextColor(C_BLUE); M5.Display.drawString(String(maxNumLimit), sw/2, 355);
    M5.Display.fillRoundRect(sw*0.85 - 120, 325, 120, 60, 16, C_GREEN); M5.Display.setTextColor(TFT_WHITE); M5.Display.drawString("+", sw*0.85 - 60, 355);

    // 4. Dice Game Settings
    M5.Display.setTextColor(0x3186); M5.Display.setFont(&fonts::FreeSansBold12pt7b); M5.Display.drawString("MATH: DICE SETTINGS (QTY & ROLLS)", sw/2, 410);
    M5.Display.fillRoundRect(40, 430, 80, 60, 16, C_RED); M5.Display.setTextColor(TFT_WHITE); M5.Display.setFont(&fonts::FreeSansBold24pt7b); M5.Display.drawString("-", 80, 460);
    M5.Display.fillRoundRect(130, 430, 100, 60, 16, TFT_WHITE); M5.Display.drawRoundRect(130, 430, 100, 60, 16, C_RED); M5.Display.setTextColor(C_RED); M5.Display.drawString(String(numDice), 180, 460);
    M5.Display.fillRoundRect(240, 430, 80, 60, 16, C_GREEN); M5.Display.setTextColor(TFT_WHITE); M5.Display.drawString("+", 280, 460);
    M5.Display.fillRoundRect(sw - 320, 430, 80, 60, 16, C_RED); M5.Display.setTextColor(TFT_WHITE); M5.Display.drawString("-", sw - 280, 460);
    M5.Display.fillRoundRect(sw - 230, 430, 100, 60, 16, TFT_WHITE); M5.Display.drawRoundRect(sw - 230, 430, 100, 60, 16, C_RED); M5.Display.setTextColor(C_RED); M5.Display.drawString(String(numRolls), sw - 180, 460);
    M5.Display.fillRoundRect(sw - 120, 430, 80, 60, 16, C_GREEN); M5.Display.setTextColor(TFT_WHITE); M5.Display.drawString("+", sw - 80, 460);

    // 5. Spelling Time Limit
    M5.Display.setTextColor(0x3186); M5.Display.setFont(&fonts::FreeSansBold12pt7b); M5.Display.drawString("WORD: SPELLING FLASHCARD TIMER", sw/2, 515);
    M5.Display.fillRoundRect(sw*0.15, 535, 120, 60, 16, C_RED); M5.Display.setTextColor(TFT_WHITE); M5.Display.setFont(&fonts::FreeSansBold24pt7b); M5.Display.drawString("-", sw*0.15 + 60, 565);
    M5.Display.fillRoundRect(sw*0.35, 535, sw*0.30, 60, 16, TFT_WHITE); M5.Display.drawRoundRect(sw*0.35, 535, sw*0.30, 60, 16, C_PURPLE); M5.Display.setTextColor(C_PURPLE); M5.Display.drawString(String(spellShowTime / 1000) + "s", sw/2, 565);
    M5.Display.fillRoundRect(sw*0.85 - 120, 535, 120, 60, 16, C_GREEN); M5.Display.setTextColor(TFT_WHITE); M5.Display.drawString("+", sw*0.85 - 60, 565);

    // 6. Audio Toggle
    M5.Display.setTextColor(0x3186); M5.Display.setFont(&fonts::FreeSansBold12pt7b); M5.Display.drawString("SYSTEM: AUDIO FX", sw/2, 620);
    M5.Display.fillRoundRect(60, 640, 280, 55, 16, soundEnabled ? C_GREEN : 0xC618); M5.Display.setTextColor(TFT_WHITE); M5.Display.drawString("SOUND ON", 60 + 140, 640 + 27);
    M5.Display.fillRoundRect(380, 640, 280, 55, 16, !soundEnabled ? C_RED : 0xC618); M5.Display.drawString("MUTE", 380 + 140, 640 + 27);

    // 7. Auto Shutdown / Inactivity Timeout
    M5.Display.setTextColor(0x3186); M5.Display.setFont(&fonts::FreeSansBold12pt7b); M5.Display.drawString("SYSTEM: AUTO SHUTDOWN (INACTIVITY)", sw/2, 720);
    M5.Display.fillRoundRect(sw*0.15, 740, 120, 60, 16, C_RED); M5.Display.setTextColor(TFT_WHITE); M5.Display.setFont(&fonts::FreeSansBold24pt7b); M5.Display.drawString("-", sw*0.15 + 60, 770);
    M5.Display.fillRoundRect(sw*0.35, 740, sw*0.30, 60, 16, TFT_WHITE); M5.Display.drawRoundRect(sw*0.35, 740, sw*0.30, 60, 16, C_TEAL); M5.Display.setTextColor(C_TEAL);
    M5.Display.setFont(&fonts::FreeSansBold18pt7b);
    String sleepLabel = (sleepTimeoutMins == 0) ? "OFF" : (String(sleepTimeoutMins) + " min");
    M5.Display.drawString(sleepLabel, sw/2, 770);
    M5.Display.fillRoundRect(sw*0.85 - 120, 740, 120, 60, 16, C_GREEN); M5.Display.setTextColor(TFT_WHITE); M5.Display.setFont(&fonts::FreeSansBold24pt7b); M5.Display.drawString("+", sw*0.85 - 60, 770);

    // 8. Wi-Fi Access Point Config Button
    M5.Display.fillRoundRect(sw*0.1, 825, sw*0.8, 65, 18, C_TEAL);
    M5.Display.setFont(&fonts::FreeSansBold18pt7b); M5.Display.setTextColor(TFT_WHITE);
    String wfBtn = "WIFI SETUP (" + (wifiSSID.length() > 0 ? wifiSSID : String("Not Set")) + ")";
    M5.Display.drawString(wfBtn, sw/2, 857);

    // 9. Save & Return
    M5.Display.fillRoundRect(sw*0.15, sh - 105, sw*0.70, 75, 22, C_BLUE);
    M5.Display.setFont(&fonts::FreeSansBold18pt7b); M5.Display.drawString("SAVE & RETURN", sw/2, sh - 67);
}

void handleSettingsTouch(int tx, int ty) {
    int btnW = 120, btnH = 60, startX = 60, gap = 40;
    if (ty >= 125 && ty <= 185) {
        if (tx >= startX && tx <= startX + btnW) { useAdd = !useAdd; playTone(523, 40); drawSettings(); return; }
        if (tx >= startX + btnW + gap && tx <= startX + 2*btnW + gap) { useSub = !useSub; playTone(523, 40); drawSettings(); return; }
        if (tx >= startX + 2*(btnW + gap) && tx <= startX + 2*(btnW + gap) + btnW) { useMult = !useMult; playTone(523, 40); drawSettings(); return; }
        if (tx >= startX + 3*(btnW + gap) && tx <= startX + 3*(btnW + gap) + btnW) { useDiv = !useDiv; playTone(523, 40); drawSettings(); return; }
    }
    if (ty >= 225 && ty <= 280) {
        if (tx >= 60 && tx <= 340) { useMultipleChoice = true; playTone(523, 40); drawSettings(); return; }
        if (tx >= 380 && tx <= 660) { useMultipleChoice = false; playTone(523, 40); drawSettings(); return; }
    }
    if (ty >= 325 && ty <= 385) {
        if (tx >= sw*0.15 && tx <= sw*0.15 + 120) { if (maxNumLimit > 5) maxNumLimit -= 5; playTone(440, 40); drawSettings(); return; }
        if (tx >= sw*0.85 - 120 && tx <= sw*0.85) { if (maxNumLimit < 995) maxNumLimit += 5; playTone(660, 40); drawSettings(); return; }
    }
    if (ty >= 430 && ty <= 490) {
        if (tx >= 40 && tx <= 120) { if (numDice > 1) numDice--; playTone(440, 40); drawSettings(); return; }
        if (tx >= 240 && tx <= 320) { if (numDice < 6) numDice++; playTone(660, 40); drawSettings(); return; }
        if (tx >= sw-320 && tx <= sw-240) { if (numRolls > 1) numRolls--; playTone(440, 40); drawSettings(); return; }
        if (tx >= sw-120 && tx <= sw-40) { if (numRolls < 20) numRolls++; playTone(660, 40); drawSettings(); return; }
    }
    if (ty >= 535 && ty <= 595) {
        if (tx >= sw*0.15 && tx <= sw*0.15 + 120) { if (spellShowTime > 1000) spellShowTime -= 1000; playTone(440, 40); drawSettings(); return; }
        if (tx >= sw*0.85 - 120 && tx <= sw*0.85) { if (spellShowTime < 10000) spellShowTime += 1000; playTone(660, 40); drawSettings(); return; }
    }
    if (ty >= 640 && ty <= 695) {
        if (tx >= 60 && tx <= 340) { soundEnabled = true; playTone(523, 40); drawSettings(); return; }
        if (tx >= 380 && tx <= 660) { soundEnabled = false; playTone(523, 40); drawSettings(); return; }
    }
    if (ty >= 740 && ty <= 800) {
        if (tx >= sw*0.15 && tx <= sw*0.15 + 120) {
            if (sleepTimeoutMins > 0) sleepTimeoutMins--;
            playTone(440, 40); drawSettings(); return;
        }
        if (tx >= sw*0.85 - 120 && tx <= sw*0.85) {
            if (sleepTimeoutMins < 60) sleepTimeoutMins++;
            playTone(660, 40); drawSettings(); return;
        }
    }
    if (tx >= sw*0.1 && tx <= sw*0.9 && ty >= 825 && ty <= 890) {
        playTone(660, 60);
        currentState = STATE_WIFI_SETUP;
        drawWifiSetup();
        return;
    }
    if (tx >= sw * 0.15 && tx <= sw * 0.85 && ty >= sh - 105 && ty <= sh - 30) {
        saveSettings();
        playTone(880, 80);
        currentState = STATE_MAIN_MENU;
        drawMainMenu();
        return;
    }
}

// ======================== WIFI SETUP SCREEN ======================
const char* kbRows[4][10] = {
    {"1","2","3","4","5","6","7","8","9","0"},
    {"q","w","e","r","t","y","u","i","o","p"},
    {"a","s","d","f","g","h","j","k","l","-"},
    {"z","x","c","v","b","n","m","_",".","!"}
};

void drawWifiSetup() {
    M5.Display.fillScreen(C_BG);
    M5.Display.setTextDatum(middle_center);

    M5.Display.fillRoundRect(24, 24, 140, 60, 16, C_GRAY);
    M5.Display.setFont(&fonts::FreeSansBold12pt7b);
    M5.Display.setTextColor(0x3186);
    M5.Display.drawString("< BACK", 94, 54);

    M5.Display.setFont(&fonts::FreeSansBold18pt7b);
    M5.Display.setTextColor(C_TEAL);
    M5.Display.drawString("WIFI ACCESS POINT", sw/2 + 40, 54);

    M5.Display.setTextDatum(middle_left);
    M5.Display.setFont(&fonts::FreeSansBold12pt7b);
    M5.Display.setTextColor(0x3186);
    M5.Display.drawString("Network SSID (Tap to edit):", 50, 115);
    M5.Display.fillRoundRect(40, 135, sw - 80, 70, 14, TFT_WHITE);
    M5.Display.drawRoundRect(40, 135, sw - 80, 70, 14, (wifiEditField == 0) ? C_ORANGE : C_GRAY);
    if (wifiEditField == 0) M5.Display.drawRoundRect(41, 136, sw - 82, 68, 14, C_ORANGE);
    M5.Display.setFont(&fonts::FreeSansBold18pt7b);
    M5.Display.setTextColor(0x1082);
    M5.Display.drawString(wifiSSID + (wifiEditField == 0 ? "_" : ""), 60, 170);

    M5.Display.setFont(&fonts::FreeSansBold12pt7b);
    M5.Display.setTextColor(0x3186);
    M5.Display.drawString("Password (Tap to edit):", 50, 235);
    M5.Display.fillRoundRect(40, 255, sw - 80, 70, 14, TFT_WHITE);
    M5.Display.drawRoundRect(40, 255, sw - 80, 70, 14, (wifiEditField == 1) ? C_ORANGE : C_GRAY);
    if (wifiEditField == 1) M5.Display.drawRoundRect(41, 256, sw - 82, 68, 14, C_ORANGE);
    M5.Display.setFont(&fonts::FreeSansBold18pt7b);
    M5.Display.setTextColor(0x1082);
    M5.Display.drawString(wifiPass + (wifiEditField == 1 ? "_" : ""), 60, 290);

    M5.Display.setTextDatum(middle_center);
    M5.Display.setFont(&fonts::FreeSans12pt7b);
    M5.Display.setTextColor(C_BLUE);
    M5.Display.drawString(wifiStatusMsg, sw/2, 360);

    M5.Display.fillRoundRect(40, 400, 420, 75, 18, C_GREEN);
    M5.Display.setFont(&fonts::FreeSansBold18pt7b);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.drawString("SAVE & CONNECT", 40 + 210, 437);

    M5.Display.fillRoundRect(480, 400, 200, 75, 18, C_RED);
    M5.Display.drawString("CLEAR", 480 + 100, 437);

    int startY = 510, keyW = 62, keyH = 75, gapX = 6, gapY = 12;
    int startX = (sw - (10 * keyW + 9 * gapX)) / 2;
    M5.Display.setFont(&fonts::FreeSansBold18pt7b);

    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 10; c++) {
            int kx = startX + c * (keyW + gapX);
            int ky = startY + r * (keyH + gapY);
            M5.Display.fillRoundRect(kx, ky, keyW, keyH, 10, TFT_WHITE);
            M5.Display.drawRoundRect(kx, ky, keyW, keyH, 10, C_GRAY);
            M5.Display.setTextColor(0x3186);
            String ch = kbRows[r][c];
            if (wifiCaps && ch[0] >= 'a' && ch[0] <= 'z') ch.toUpperCase();
            M5.Display.drawString(ch, kx + keyW/2, ky + keyH/2);
        }
    }

    int bY = startY + 4 * (keyH + gapY);
    M5.Display.fillRoundRect(startX, bY, 150, keyH, 10, wifiCaps ? C_ORANGE : C_BLUE);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.setFont(&fonts::FreeSansBold12pt7b);
    M5.Display.drawString(wifiCaps ? "CAPS ON" : "SHIFT", startX + 75, bY + keyH/2);

    M5.Display.fillRoundRect(startX + 165, bY, 340, keyH, 10, TFT_WHITE);
    M5.Display.drawRoundRect(startX + 165, bY, 340, keyH, 10, C_GRAY);
    M5.Display.setTextColor(0x3186);
    M5.Display.drawString("SPACE", startX + 165 + 170, bY + keyH/2);

    M5.Display.fillRoundRect(startX + 520, bY, 154, keyH, 10, C_RED);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.drawString("DEL", startX + 520 + 77, bY + keyH/2);
}

void handleWifiSetupTouch(int tx, int ty) {
    if (tx >= 24 && tx <= 164 && ty >= 24 && ty <= 84) {
        playTone(440, 60);
        currentState = STATE_SETTINGS;
        drawSettings();
        return;
    }
    if (tx >= 40 && tx <= sw - 40 && ty >= 135 && ty <= 205) {
        wifiEditField = 0; playTone(523, 40); drawWifiSetup(); return;
    }
    if (tx >= 40 && tx <= sw - 40 && ty >= 255 && ty <= 325) {
        wifiEditField = 1; playTone(523, 40); drawWifiSetup(); return;
    }
    if (tx >= 40 && tx <= 460 && ty >= 400 && ty <= 475) {
        playTone(660, 60);
        saveSettings();
        wifiStatusMsg = "Connecting to " + wifiSSID + "...";
        drawWifiSetup();
        connectToWiFi();
        if (WiFi.status() == WL_CONNECTED) {
            wifiStatusMsg = "Connected! IP: " + WiFi.localIP().toString();
            playTone(880, 150);
        } else {
            wifiStatusMsg = "Connection failed. Check SSID/Password.";
            playTone(220, 250);
        }
        drawWifiSetup();
        return;
    }
    if (tx >= 480 && tx <= 680 && ty >= 400 && ty <= 475) {
        playTone(440, 60);
        if (wifiEditField == 0) wifiSSID = ""; else wifiPass = "";
        drawWifiSetup();
        return;
    }

    int startY = 510, keyW = 62, keyH = 75, gapX = 6, gapY = 12;
    int startX = (sw - (10 * keyW + 9 * gapX)) / 2;

    for (int r = 0; r < 4; r++) {
        int ky = startY + r * (keyH + gapY);
        if (ty >= ky && ty <= ky + keyH) {
            for (int c = 0; c < 10; c++) {
                int kx = startX + c * (keyW + gapX);
                if (tx >= kx && tx <= kx + keyW) {
                    String ch = kbRows[r][c];
                    if (wifiCaps && ch[0] >= 'a' && ch[0] <= 'z') ch.toUpperCase();
                    if (wifiEditField == 0) wifiSSID += ch; else wifiPass += ch;
                    playTone(523, 30);
                    drawWifiSetup();
                    return;
                }
            }
        }
    }

    int bY = startY + 4 * (keyH + gapY);
    if (ty >= bY && ty <= bY + keyH) {
        if (tx >= startX && tx <= startX + 150) {
            wifiCaps = !wifiCaps; playTone(523, 40); drawWifiSetup(); return;
        }
        if (tx >= startX + 165 && tx <= startX + 505) {
            if (wifiEditField == 0) wifiSSID += " "; else wifiPass += " ";
            playTone(523, 30); drawWifiSetup(); return;
        }
        if (tx >= startX + 520 && tx <= startX + 674) {
            String &target = (wifiEditField == 0) ? wifiSSID : wifiPass;
            if (target.length() > 0) target.remove(target.length() - 1);
            playTone(440, 40); drawWifiSetup(); return;
        }
    }
}

// ======================== MAIN LOOP ==============================
void loop() {
    M5.update();
    loopWordGames();
    loopDiceGame();
    loopDailyPlanner();
    loopGeocache();
    loopFlightRadar();
    loopMeshtastic();

    auto t = M5.Touch.getDetail();
    if (t.wasPressed() || t.isPressed()) {
        lastActivityTime = millis();
    }

    // Auto-Shutdown on Inactivity
    if (sleepTimeoutMins > 0 && (millis() - lastActivityTime > (unsigned long)sleepTimeoutMins * 60000UL)) {
        M5.Display.fillScreen(TFT_BLACK);
        M5.Display.setTextDatum(middle_center);
        M5.Display.setFont(&fonts::FreeSansBold18pt7b);
        M5.Display.setTextColor(TFT_WHITE);
        M5.Display.drawString("Sleeping due to inactivity...", sw/2, sh/2);
        delay(800);
        M5.Display.setBrightness(0);
        M5.Display.sleep();
        M5.Power.powerOff();
        esp_deep_sleep_start();
    }

    if (!t.wasPressed()) return;
    int tx = t.x, ty = t.y;

    switch (currentState) {
        case STATE_MAIN_MENU:
            if (tx >= sw*0.08 && tx <= sw*0.92) {
                if (ty >= 136 && ty <= 274)        { playTone(660, 60); currentState = STATE_MATH_MENU; drawMathMenu(); }
                else if (ty >= 290 && ty <= 428)   { playTone(660, 60); currentState = STATE_WORD_MENU; drawWordMenu(); }
                else if (ty >= 444 && ty <= 582)   { playTone(660, 60); currentState = STATE_DAILY_PLANNER; initDailyPlanner(); }
                else if (ty >= 598 && ty <= 736)   { playTone(660, 60); currentState = STATE_GEOCACHE; initGeocache(); }
                else if (ty >= 752 && ty <= 890)   { playTone(660, 60); currentState = STATE_FLIGHT_RADAR; initFlightRadar(); }
                else if (ty >= 906 && ty <= 1044)  { playTone(660, 60); currentState = STATE_MESHTASTIC; initMeshtastic(); }
                else if (ty >= 1060 && ty <= 1198) { playTone(660, 60); currentState = STATE_SETTINGS; drawSettings(); }
            }
            break;

        case STATE_MATH_MENU:
            if (tx >= 24 && tx <= 164 && ty >= 24 && ty <= 88) { playTone(440, 60); currentState = STATE_MAIN_MENU; drawMainMenu(); break; }
            if (tx >= sw*0.1 && tx <= sw*0.9) {
                if (ty >= 200 && ty <= 360) { playTone(660, 60); currentState = STATE_MATH_FLASHCARDS; initFlashcards(); } 
                else if (ty >= 400 && ty <= 560) { playTone(660, 60); currentState = STATE_CROSSMATH; initCrossmath(); }
                else if (ty >= 600 && ty <= 760) { playTone(660, 60); currentState = STATE_MATH_DICE; initDiceGame(); }
            }
            break;

        case STATE_WORD_MENU:
            if (tx >= 24 && tx <= 164 && ty >= 24 && ty <= 88) { playTone(440, 60); currentState = STATE_MAIN_MENU; drawMainMenu(); break; }
            if (tx >= sw*0.1 && tx <= sw*0.9) {
                if (ty >= 220 && ty <= 400) { playTone(660, 60); currentState = STATE_WORD_APPLETREE; initAppleTree(); } 
                else if (ty >= 460 && ty <= 640) { playTone(660, 60); currentState = STATE_WORD_SPELLING; initSpelling(); }
            }
            break;

        case STATE_DAILY_PLANNER:     handleDailyPlannerTouch(tx, ty); break;
        case STATE_GEOCACHE:          handleGeocacheTouch(tx, ty); break;
        case STATE_FLIGHT_RADAR:      handleFlightRadarTouch(tx, ty); break;
        case STATE_MESHTASTIC:        handleMeshtasticTouch(tx, ty); break;
        case STATE_SETTINGS:          handleSettingsTouch(tx, ty); break;
        case STATE_WIFI_SETUP:        handleWifiSetupTouch(tx, ty); break;
        case STATE_MATH_FLASHCARDS:   handleFlashcardsTouch(tx, ty); break;
        case STATE_CROSSMATH:         handleCrossmathTouch(tx, ty); break;
        case STATE_MATH_DICE:         handleDiceTouch(tx, ty); break;
        case STATE_WORD_APPLETREE:    handleAppleTreeTouch(tx, ty); break;
        case STATE_WORD_SPELLING:     handleSpellingTouch(tx, ty); break;
    }
}