#pragma once
#include <Arduino.h>
#include <M5Unified.h>

// Global Colors
#define C_BG     0xF7BE
#define C_BLUE   0x3B96
#define C_GREEN  0x2589
#define C_RED    0xE244
#define C_ORANGE 0xF443
#define C_PURPLE 0x7BEF
#define C_BROWN  0x8A22
#define C_GRAY   0xDEDB
#define C_TREEG  0x05E0
#define C_TEAL   0x04D3
#define C_DARK   0x10C5
#define C_PANEL  0x2126
#define C_GOLD   0xFDC9
#define C_CYAN   0x56BF

// Application States
enum AppState { 
    STATE_MAIN_MENU, 
    STATE_MATH_MENU, STATE_MATH_FLASHCARDS, STATE_CROSSMATH, STATE_MATH_DICE,
    STATE_WORD_MENU, STATE_WORD_APPLETREE, STATE_WORD_SPELLING,
    STATE_DAILY_PLANNER,
    STATE_GEOCACHE,
    STATE_FLIGHT_RADAR,
    STATE_CAMERA,
    STATE_MESHTASTIC,
    STATE_SETTINGS,
    STATE_WIFI_SETUP
};

// Extern declarations (defined in main.cpp)
extern int sw;
extern int sh;
extern AppState currentState;

// Global Settings
extern bool useAdd;
extern bool useSub;
extern bool useMult;
extern bool useDiv;
extern int maxNumLimit;
extern bool useMultipleChoice;
extern String mathManualInput;
extern int spellShowTime;
extern bool soundEnabled;
extern int numDice;
extern int numRolls;

// Power & Wi-Fi Settings
extern int sleepTimeoutMins;
extern unsigned long lastActivityTime;
extern String wifiSSID;
extern String wifiPass;

// Shared Utility Functions
void playTone(int freq, int duration);
void saveSettings();
void connectToWiFi();
void drawMainMenu();
void drawMathMenu();
void drawWordMenu();
void drawSettings();
void drawWifiSetup();