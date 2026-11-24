#include "FlightRadar.h"
#include "MeshtasticBLE.h"
#include "MapTiles.h"
#include <SD.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <math.h>
#include <time.h>
#include <esp_heap_caps.h>

// ======================== CONFIGURATION ===============================
// Default center: Barrington / Portsmouth, NH (auto-uses live T114 GPS when locked)
static const double DEFAULT_RADAR_LAT = 43.2189;
static const double DEFAULT_RADAR_LON = -71.1069;

static const int RANGE_KM_LEVELS[4] = {25, 50, 100, 150};
static const int RANGE_TILE_ZOOM[4] = {10,  9,   8,   7};

static const int MAX_PLANES = 40;
static const int MAX_PLANE_TRAIL = 10;

// OWLOS Visual Modes
enum OwlosLook : uint8_t {
    LOOK_RADAR   = 0,  // Circular ATC scope over dark tinted map
    LOOK_MAP     = 1,  // Full square dark tinted map + planes
    LOOK_CLASSIC = 2   // Pure ATC scope (grid + rings + sweep, no map)
};

enum OwlosHue : uint8_t {
    HUE_ARCTIC   = 0,  // Ice cyan on deep midnight navy (OWLOS default)
    HUE_PHOSPHOR = 1,  // Classic ATC neon green
    HUE_AMBER    = 2,  // Aviation HUD amber
    HUE_CRIMSON  = 3,  // Tactical stealth red
    HUE_SYNTH    = 4   // Synthwave magenta/cyan
};

enum OwlosViewMode : uint8_t {
    VIEW_MAIN     = 0, // Radar scope + OWLOS Flights panel (default)
    VIEW_LIST     = 1, // Expanded flight list + detailed telemetry card
    VIEW_SETTINGS = 2  // OWLOS Setup/View settings panel
};

struct OwlosTheme {
    const char* name;
    uint16_t bg;
    uint16_t grid;
    uint16_t panelBg;
    uint16_t accent;
    uint16_t accentDim;
    uint16_t ring;
    uint16_t text;
    uint16_t muted;
    // Map tint RGB curves (low -> high luminance)
    uint8_t mapR0, mapG0, mapB0;
    uint8_t mapR1, mapG1, mapB1;
    // Sweep RGB
    uint8_t swR, swG, swB;
};

static const OwlosTheme THEMES[5] = {
    // 0: ARCTIC (matches OWLOS screenshot)
    { "ARCTIC",
      0x0042, 0x08C5, 0x08A4, 0x5F1F, 0x1AEF, 0x1B92, 0xE7BF, 0x6C95,
      4, 14, 28,   115, 224, 255,   80, 220, 255 },
    // 1: PHOSPHOR
    { "PHOSPHOR",
      0x0060, 0x0141, 0x0101, 0x3FE9, 0x1344, 0x1425, 0xD7FB, 0x54CA,
      3, 16, 6,    95, 245, 130,    57, 255, 106 },
    // 2: AMBER
    { "AMBER",
      0x1040, 0x20C0, 0x1880, 0xFD84, 0x7A61, 0x8AE1, 0xFF9A, 0xAD26,
      18, 10, 2,   255, 185, 60,    255, 176, 32 },
    // 3: CRIMSON
    { "CRIMSON",
      0x1000, 0x2841, 0x2021, 0xF9CB, 0x78A4, 0x8905, 0xFF1C, 0xA368,
      20, 4, 7,    255, 88, 118,    255, 59, 92 },
    // 4: SYNTH
    { "SYNTH",
      0x0823, 0x1866, 0x1045, 0xFA7B, 0x6932, 0x7995, 0xF73F, 0x93D5,
      13, 5, 26,   235, 110, 255,   255, 79, 216 }
};

// ======================== DATA STRUCTURES =============================
struct AircraftItem {
    String hex;
    String callsign;
    String reg;
    String typeCode;
    String desc;
    String squawk;
    double lat = 0.0;
    double lon = 0.0;
    int altFt = 0;
    bool onGround = false;
    float speedKt = 0.0f;
    float trackDeg = 0.0f;
    int vertRateFpm = 0;
    float distKm = 0.0f;
    float bearingDeg = 0.0f;
    bool conflict = false;
};

struct PlaneTrail {
    String hex;
    double lat[MAX_PLANE_TRAIL];
    double lon[MAX_PLANE_TRAIL];
    int count = 0;
    unsigned long lastSeenMs = 0;
};

// Static storage (off loopTask stack!)
static AircraftItem planes[MAX_PLANES];
static AircraftItem tempPlanes[MAX_PLANES];
static PlaneTrail planeTrails[MAX_PLANES];
static int planeCount = 0;
static int planeTrailCount = 0;
static int selectedPlane = 0;
static String selectedHex = "";
static int listPage = 0;

// Settings (persisted to NVS)
static OwlosLook owlosLook = LOOK_RADAR;
static OwlosHue  owlosHue  = HUE_ARCTIC;
static int       rangeIdx  = 1; // 50 KM default (matches OWLOS screenshot)
static bool      optVectors   = true;
static bool      optUtcClock  = true;
static bool      optDataBlock = true;
static bool      optConflict  = true;
static bool      optSweep     = true;
static bool      prefsLoaded  = false;

static OwlosViewMode viewMode = VIEW_MAIN;
static bool radarActive = false;
static bool radarFetchOk = false;
static String radarSource = "adsb.lol";
static String radarStatusLine = "Connecting to ADS-B...";
static unsigned long lastAdsbFetchMs = 0;
static const unsigned long ADSB_REFRESH_MS = 20000UL;

// Sweep Animation & PSRAM Canvas State
static const int SCOPE_X = 20;
static const int SCOPE_Y = 72;
static const int SCOPE_W = 680;
static const int SCOPE_H = 660;
static const int SCOPE_CX = SCOPE_W / 2;
static const int SCOPE_CY = SCOPE_H / 2;
static const int SCOPE_R  = 292; // Circular radar bezel radius

static LGFX_Sprite* scopeSprite = nullptr;
static uint8_t*     mapLumBuf   = nullptr; // 680x660 grayscale basemap in PSRAM for instant hue re-tinting
static uint16_t*    mapTintBuf  = nullptr; // 680x660 tinted RGB565 basemap in PSRAM
static bool         mapHasData  = false;
static bool         mapNeedFetch = true;
static int          mapLoadedRangeIdx = -1;
static double       mapLoadedLat = 0.0, mapLoadedLon = 0.0;
static OwlosHue     mapTintedHue = (OwlosHue)255;
static bool         spriteSwapBytes = true;

static float         sweepDeg = 42.0f;
static unsigned long lastAnimMs = 0;

// ======================== PREFERENCES =================================
static void loadOwlosPrefs() {
    if (prefsLoaded) return;
    prefsLoaded = true;
    Preferences p;
    p.begin("owlos_cfg", true);
    owlosLook    = (OwlosLook)(p.getUChar("look", LOOK_RADAR) % 3);
    owlosHue     = (OwlosHue)(p.getUChar("hue", HUE_ARCTIC) % 5);
    rangeIdx     = p.getInt("rng", 1);
    if (rangeIdx < 0 || rangeIdx > 3) rangeIdx = 1;
    optVectors   = p.getBool("vec", true);
    optUtcClock  = p.getBool("utc", true);
    optDataBlock = p.getBool("blk", true);
    optConflict  = p.getBool("cnf", true);
    optSweep     = p.getBool("swp", true);
    p.end();
}

static void saveOwlosPrefs() {
    Preferences p;
    p.begin("owlos_cfg", false);
    p.putUChar("look", (uint8_t)owlosLook);
    p.putUChar("hue", (uint8_t)owlosHue);
    p.putInt("rng", rangeIdx);
    p.putBool("vec", optVectors);
    p.putBool("utc", optUtcClock);
    p.putBool("blk", optDataBlock);
    p.putBool("cnf", optConflict);
    p.putBool("swp", optSweep);
    p.end();
}

// ======================== GEO & COLOR HELPERS =========================
static void getRadarCenter(double &lat, double &lon, bool &usingGps) {
    if (meshGps.valid && !meshGps.simulated) {
        lat = meshGps.lat;
        lon = meshGps.lon;
        usingGps = true;
    } else {
        lat = DEFAULT_RADAR_LAT;
        lon = DEFAULT_RADAR_LON;
        usingGps = false;
    }
}

static double calcDistKm(double lat1, double lon1, double lat2, double lon2) {
    const double R_KM = 6371.0;
    double dLat = (lat2 - lat1) * DEG_TO_RAD;
    double dLon = (lon2 - lon1) * DEG_TO_RAD;
    double a = sin(dLat / 2.0) * sin(dLat / 2.0) +
               cos(lat1 * DEG_TO_RAD) * cos(lat2 * DEG_TO_RAD) *
               sin(dLon / 2.0) * sin(dLon / 2.0);
    return R_KM * 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
}

static double calcBearing(double lat1, double lon1, double lat2, double lon2) {
    double y = sin((lon2 - lon1) * DEG_TO_RAD) * cos(lat2 * DEG_TO_RAD);
    double x = cos(lat1 * DEG_TO_RAD) * sin(lat2 * DEG_TO_RAD) -
               sin(lat1 * DEG_TO_RAD) * cos(lat2 * DEG_TO_RAD) * cos((lon2 - lon1) * DEG_TO_RAD);
    double brg = atan2(y, x) * RAD_TO_DEG;
    if (brg < 0) brg += 360.0;
    return brg;
}

static const char* degToCardinal(float deg) {
    static const char* dirs[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    if (!isfinite(deg)) deg = 0.0f;
    int idx = ((int)floorf((deg + 22.5f) / 45.0f)) & 7;
    return dirs[idx];
}

// Exact OWLOS height color coding (matches screenshot bottom-left legend)
static uint16_t owlosHeightColor(const AircraftItem &ac) {
    if (ac.squawk == "7500" || ac.squawk == "7600" || ac.squawk == "7700") return 0xF800; // Emergency red
    if (ac.conflict && optConflict) return 0xF8E7; // Conflict alert red
    if (ac.onGround || ac.altFt <= 150) return 0x8D16; // GND steel gray
    if (ac.altFt < 10000) return 0xFD88; // <10K warm amber/gold
    if (ac.altFt < 25000) return 0x4F1F; // <25K bright sky cyan
    return TFT_WHITE;                    // HIGH (>=25K) crisp white
}

static String formatOwlosHeight(const AircraftItem &ac) {
    if (ac.onGround || ac.altFt <= 100) return "GND";
    if (ac.altFt >= 10000) {
        int fl = (ac.altFt + 50) / 100;
        char buf[10];
        snprintf(buf, sizeof(buf), "FL%03d", fl);
        return String(buf);
    }
    return String(ac.altFt);
}

static inline uint16_t packRgb565(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t v = ((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | (b >> 3);
    return spriteSwapBytes ? __builtin_bswap16(v) : v;
}

static void drawChamferedButton(LovyanGFX &gfx, int x, int y, int w, int h, int cut,
                                uint16_t fillCol, uint16_t borderCol, const String &label,
                                uint16_t textCol, const lgfx::IFont* font = &fonts::FreeSansBold12pt7b) {
    // Chamfered HUD box (45-degree cuts at top-left & bottom-right like OWLOS)
    gfx.fillRect(x + cut, y, w - cut, cut, fillCol);
    gfx.fillRect(x, y + cut, w, h - 2 * cut, fillCol);
    gfx.fillRect(x, y + h - cut, w - cut, cut, fillCol);
    gfx.fillTriangle(x + cut, y, x, y + cut, x + cut, y + cut, fillCol);
    gfx.fillTriangle(x + w - cut, y + h, x + w, y + h - cut, x + w - cut, y + h - cut, fillCol);
    gfx.drawLine(x + cut, y, x + w, y, borderCol);
    gfx.drawLine(x + w, y, x + w, y + h - cut, borderCol);
    gfx.drawLine(x + w, y + h - cut, x + w - cut, y + h, borderCol);
    gfx.drawLine(x + w - cut, y + h, x, y + h, borderCol);
    gfx.drawLine(x, y + h, x, y + cut, borderCol);
    gfx.drawLine(x, y + cut, x + cut, y, borderCol);

    gfx.setTextDatum(middle_center);
    gfx.setFont(font);
    gfx.setTextColor(textCol);
    gfx.drawString(label, x + w / 2, y + h / 2 + 1);
}

// ======================== PSRAM SPRITE & DARK MAP =====================
static bool ensureScopeBuffers() {
    if (!scopeSprite) {
        scopeSprite = new LGFX_Sprite(&M5.Display);
        scopeSprite->setPsram(true);
        scopeSprite->setColorDepth(16);
        if (!scopeSprite->createSprite(SCOPE_W, SCOPE_H)) {
            delete scopeSprite;
            scopeSprite = nullptr;
            return false;
        }
        // Detect sprite buffer endianness once at runtime
        scopeSprite->drawPixel(0, 0, TFT_RED);
        uint16_t* raw = (uint16_t*)scopeSprite->getBuffer();
        spriteSwapBytes = (raw && raw[0] != 0xF800);
    }
    size_t pxCount = (size_t)SCOPE_W * (size_t)SCOPE_H;
    if (!mapLumBuf)  mapLumBuf  = (uint8_t*)ps_malloc(pxCount);
    if (!mapTintBuf) mapTintBuf = (uint16_t*)ps_malloc(pxCount * sizeof(uint16_t));
    return (scopeSprite != nullptr && mapLumBuf != nullptr && mapTintBuf != nullptr);
}

static void releaseScopeBuffers() {
    if (scopeSprite) {
        scopeSprite->deleteSprite();
        delete scopeSprite;
        scopeSprite = nullptr;
    }
    if (mapLumBuf)  { free(mapLumBuf);  mapLumBuf = nullptr; }
    if (mapTintBuf) { free(mapTintBuf); mapTintBuf = nullptr; }
    mapHasData = false;
    mapTintedHue = (OwlosHue)255;
}

static void retintDarkBasemap() {
    if (!mapHasData || !mapLumBuf || !mapTintBuf) return;
    const OwlosTheme &th = THEMES[owlosHue];
    size_t total = (size_t)SCOPE_W * (size_t)SCOPE_H;

    // Precompute 256-entry RGB565 LUT for instantaneous re-tinting (< 4 ms!)
    uint16_t lut[256];
    for (int i = 0; i < 256; i++) {
        // Boost dark map contrast so roads & city labels glow softly like OWLOS
        int c = (i < 14) ? 0 : ((i - 14) * 280 / 241);
        if (c > 255) c = 255;
        uint8_t r = (uint8_t)(th.mapR0 + ((th.mapR1 - th.mapR0) * c) / 255);
        uint8_t g = (uint8_t)(th.mapG0 + ((th.mapG1 - th.mapG0) * c) / 255);
        uint8_t b = (uint8_t)(th.mapB0 + ((th.mapB1 - th.mapB0) * c) / 255);
        lut[i] = packRgb565(r, g, b);
    }
    for (size_t i = 0; i < total; i++) {
        mapTintBuf[i] = lut[mapLumBuf[i]];
    }
    mapTintedHue = owlosHue;
}

static String buildDarkTileSdPath(int z, int x, int y) {
    return "/maps/owlos_osm/" + String(z) + "/" + String(x) + "/" + String(y) + ".tile";
}

static void ensureDarkTileSdDir(int z, int x) {
    if (!isMapSdReady()) return;
    if (!SD.exists("/maps")) SD.mkdir("/maps");
    if (!SD.exists("/maps/owlos_osm")) SD.mkdir("/maps/owlos_osm");
    String p1 = "/maps/owlos_osm/" + String(z);
    if (!SD.exists(p1)) SD.mkdir(p1);
    String p2 = p1 + "/" + String(x);
    if (!SD.exists(p2)) SD.mkdir(p2);
}

static size_t downloadSingleTile(WiFiClientSecure &client, HTTPClient &http, const String &url, uint8_t* outBuf, size_t maxLen) {
    if (!http.begin(client, url)) return 0;
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        if (code < 0) http.end();
        return 0;
    }
    int clen = http.getSize();
    WiFiClient* st = http.getStreamPtr();
    size_t got = 0;
    unsigned long t0 = millis(), lastRx = millis();
    while (millis() - t0 < 6000UL && millis() - lastRx < 2500UL) {
        if (clen > 0 && (int)got >= clen) break;
        int av = st->available();
        if (av > 0) {
            size_t sp = maxLen - got;
            if (sp == 0) break;
            int n = st->readBytes(outBuf + got, ((size_t)av > sp) ? sp : (size_t)av);
            if (n > 0) { got += (size_t)n; lastRx = millis(); }
        } else {
            if (!http.connected()) break;
            delay(2);
        }
    }
    bool validPng = (got >= 128 && outBuf[0] == 0x89 && outBuf[1] == 0x50);
    bool validJpg = (got >= 128 && outBuf[0] == 0xFF && outBuf[1] == 0xD8);
    return (validPng || validJpg) ? got : 0;
}

static void fetchDarkBasemapForScope() {
    mapNeedFetch = false;
    if (!ensureScopeBuffers()) return;

    double cLat, cLon;
    bool usingGps;
    getRadarCenter(cLat, cLon, usingGps);

    int z = RANGE_TILE_ZOOM[rangeIdx];
    int rangeKm = RANGE_KM_LEVELS[rangeIdx];
    double cTx = lonToTileX(cLon, z);
    double cTy = latToTileY(cLat, z);
    int tileCx = (int)floor(cTx);
    int tileCy = (int)floor(cTy);
    int maxIdx = (1 << z) - 1;

    // Native tile scale vs exact radar scope scale
    double kmPerTile = 40075.016686 * cos(cLat * DEG_TO_RAD) / (double)(1 << z);
    double nativePxPerKm = 256.0 / kmPerTile;
    double radarPxPerKm  = (double)SCOPE_R / (double)rangeKm;
    float  scale         = (float)(radarPxPerKm / nativePxPerKm);

    // Render 3x3 OpenStreetMap tiles (inverted to dark OWLOS theme — zero API key!)
    LGFX_Sprite* tileCanvas = new LGFX_Sprite(&M5.Display);
    tileCanvas->setPsram(true);
    tileCanvas->setColorDepth(16);
    if (!tileCanvas->createSprite(768, 768)) {
        delete tileCanvas;
        return;
    }
    tileCanvas->fillSprite(TFT_WHITE); // White inverts cleanly to pure dark bg

    uint8_t* imgBuf = (uint8_t*)ps_malloc(131072);
    if (!imgBuf) {
        tileCanvas->deleteSprite();
        delete tileCanvas;
        return;
    }

    initMapSdCard(false);
    WiFiClientSecure* client = nullptr;
    HTTPClient http;
    http.setReuse(true);
    http.setTimeout(6000);
    http.setUserAgent("M5Tab5-OWLOS-Radar/1.0 (https://github.com/KGFairbarn/M5Tab5)");

    int loadedTiles = 0;
    for (int dy = -1; dy <= 1; dy++) {
        int ty = tileCy + dy;
        if (ty < 0 || ty > maxIdx) continue;
        for (int dx = -1; dx <= 1; dx++) {
            int tx = ((tileCx + dx) % (maxIdx + 1) + (maxIdx + 1)) % (maxIdx + 1);
            size_t imgLen = 0;

            // 1. Check SD cache first
            String sdPath = buildDarkTileSdPath(z, tx, ty);
            if (isMapSdReady() && SD.exists(sdPath)) {
                File f = SD.open(sdPath, FILE_READ);
                if (f) {
                    size_t sz = f.size();
                    if (sz >= 128 && sz <= 131072) imgLen = f.read(imgBuf, sz);
                    f.close();
                }
            }

            // 2. Otherwise download key-free OpenStreetMap tile (fallback: Esri World Topo)
            if (imgLen < 128 && WiFi.status() == WL_CONNECTED) {
                if (!client) {
                    client = new WiFiClientSecure();
                    client->setInsecure();
                }
                String osmUrl = "https://tile.openstreetmap.org/" + String(z) + "/" + String(tx) + "/" + String(ty) + ".png";
                imgLen = downloadSingleTile(*client, http, osmUrl, imgBuf, 131072);
                if (imgLen < 128) {
                    http.end();
                    String esriUrl = "https://server.arcgisonline.com/ArcGIS/rest/services/World_Topo_Map/MapServer/tile/" +
                                     String(z) + "/" + String(ty) + "/" + String(tx);
                    imgLen = downloadSingleTile(*client, http, esriUrl, imgBuf, 131072);
                }
                if (imgLen >= 128 && isMapSdReady()) {
                    ensureDarkTileSdDir(z, tx);
                    File f = SD.open(sdPath, FILE_WRITE);
                    if (f) { f.write(imgBuf, imgLen); f.close(); }
                }
                delay(60);
            }

            if (imgLen >= 128) {
                if (imgBuf[0] == 0x89 && imgBuf[1] == 0x50) {
                    tileCanvas->drawPng(imgBuf, imgLen, (dx + 1) * 256, (dy + 1) * 256);
                    loadedTiles++;
                } else if (imgBuf[0] == 0xFF && imgBuf[1] == 0xD8) {
                    tileCanvas->drawJpg(imgBuf, imgLen, (dx + 1) * 256, (dy + 1) * 256);
                    loadedTiles++;
                }
            }
            lastActivityTime = millis();
        }
    }

    http.end();
    if (client) delete client;
    free(imgBuf);

    if (loadedTiles > 0) {
        // Project tileCanvas onto scopeSprite at exact scale & sub-tile offset
        float pivotX = (float)((cTx - (tileCx - 1)) * 256.0);
        float pivotY = (float)((cTy - (tileCy - 1)) * 256.0);
        scopeSprite->fillSprite(TFT_WHITE);
        tileCanvas->setPivot(pivotX, pivotY);
        tileCanvas->pushRotateZoom(scopeSprite, SCOPE_CX, SCOPE_CY, 0.0f, scale, scale);

        // Invert luminance (bright OSM land -> dark navy; dark roads & town names -> glowing cyan/green/amber!)
        uint16_t* raw = (uint16_t*)scopeSprite->getBuffer();
        size_t total = (size_t)SCOPE_W * (size_t)SCOPE_H;
        for (size_t i = 0; i < total; i++) {
            uint16_t v = spriteSwapBytes ? __builtin_bswap16(raw[i]) : raw[i];
            uint8_t r = ((v >> 11) & 0x1F) << 3;
            uint8_t g = ((v >> 5)  & 0x3F) << 2;
            uint8_t b = ( v        & 0x1F) << 3;
            uint8_t lum = (uint8_t)((r * 77 + g * 150 + b * 29) >> 8);
            mapLumBuf[i] = (uint8_t)(255 - lum);
        }
        mapHasData = true;
        mapLoadedRangeIdx = rangeIdx;
        mapLoadedLat = cLat;
        mapLoadedLon = cLon;
        retintDarkBasemap();
    }

    tileCanvas->deleteSprite();
    delete tileCanvas;
}

// ======================== LIVE ADS-B API FETCH ========================
static void recordPlaneTrail(const String &hex, double lat, double lon) {
    unsigned long now = millis();
    int idx = -1;
    for (int i = 0; i < planeTrailCount; i++) {
        if (planeTrails[i].hex == hex) { idx = i; break; }
    }
    if (idx < 0) {
        if (planeTrailCount < MAX_PLANES) {
            idx = planeTrailCount++;
        } else {
            idx = 0;
            for (int i = 1; i < planeTrailCount; i++) {
                if (planeTrails[i].lastSeenMs < planeTrails[idx].lastSeenMs) idx = i;
            }
        }
        planeTrails[idx].hex = hex;
        planeTrails[idx].count = 0;
    }

    PlaneTrail &tr = planeTrails[idx];
    tr.lastSeenMs = now;
    if (tr.count > 0 && calcDistKm(tr.lat[tr.count - 1], tr.lon[tr.count - 1], lat, lon) < 0.08) return;
    if (tr.count >= MAX_PLANE_TRAIL) {
        for (int i = 1; i < MAX_PLANE_TRAIL; i++) {
            tr.lat[i - 1] = tr.lat[i];
            tr.lon[i - 1] = tr.lon[i];
        }
        tr.count = MAX_PLANE_TRAIL - 1;
    }
    tr.lat[tr.count] = lat;
    tr.lon[tr.count] = lon;
    tr.count++;
}

struct PsramJsonAllocator : ArduinoJson::Allocator {
    void* allocate(size_t size) override {
        void* p = ps_malloc(size);
        return p ? p : malloc(size);
    }
    void deallocate(void* ptr) override { free(ptr); }
    void* reallocate(void* ptr, size_t new_size) override {
        void* p = ps_realloc(ptr, new_size);
        return p ? p : realloc(ptr, new_size);
    }
};
static PsramJsonAllocator psramJsonAlloc;

static bool fetchFromEndpoint(const String &url, const char* srcLabel, double cLat, double cLon, int &lastHttpCode) {
    Serial.printf("[RADAR] fetch %s heap=%u psram=%u\n", srcLabel, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
    WiFiClientSecure* client = new WiFiClientSecure();
    if (!client) return false;
    client->setInsecure();

    HTTPClient http;
    http.setTimeout(9000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setUserAgent("M5Tab5-OWLOS-Radar/1.0");

    if (!http.begin(*client, url)) {
        delete client;
        lastHttpCode = -10;
        return false;
    }
    http.addHeader("Accept-Encoding", "identity");

    int code = http.GET();
    lastHttpCode = code;
    if (code != HTTP_CODE_OK) {
        http.end();
        delete client;
        return false;
    }

    // Read response into PSRAM buffer (zero internal SRAM spike!)
    const size_t MAX_JSON_BYTES = 163840;
    char* jsonBuf = (char*)ps_malloc(MAX_JSON_BYTES + 1);
    if (!jsonBuf) {
        http.end();
        delete client;
        lastHttpCode = -21;
        return false;
    }
    size_t got = 0;
    int clen = http.getSize();
    WiFiClient* st = http.getStreamPtr();
    unsigned long t0 = millis(), lastRx = millis();
    while (millis() - t0 < 9000UL && millis() - lastRx < 3000UL) {
        if (clen > 0 && (int)got >= clen) break;
        int av = st->available();
        if (av > 0) {
            size_t sp = MAX_JSON_BYTES - got;
            if (sp == 0) break;
            int n = st->readBytes(jsonBuf + got, ((size_t)av > sp) ? sp : (size_t)av);
            if (n > 0) { got += (size_t)n; lastRx = millis(); }
        } else {
            if (!http.connected()) break;
            delay(2);
        }
    }
    jsonBuf[got] = '\0';
    http.end();
    delete client;

    if (got < 8) {
        free(jsonBuf);
        lastHttpCode = -20;
        return false;
    }

    // If chunked response slipped through, skip leading chunk-size hex line before '{'
    char* jsonStart = strchr(jsonBuf, '{');
    if (!jsonStart) {
        free(jsonBuf);
        lastHttpCode = -22;
        return false;
    }

    JsonDocument filter(&psramJsonAlloc);
    JsonObject acF = filter["ac"].add<JsonObject>();
    acF["hex"]          = true;
    acF["flight"]       = true;
    acF["r"]            = true;
    acF["t"]            = true;
    acF["desc"]         = true;
    acF["lat"]          = true;
    acF["lon"]          = true;
    acF["alt_baro"]     = true;
    acF["gs"]           = true;
    acF["track"]        = true;
    acF["true_heading"] = true;
    acF["baro_rate"]    = true;
    acF["squawk"]       = true;
    filter["aircraft"] = filter["ac"];

    JsonDocument doc(&psramJsonAlloc);
    DeserializationError err = deserializeJson(doc, (const char*)jsonStart, DeserializationOption::Filter(filter));
    free(jsonBuf);

    if (err) {
        Serial.printf("[RADAR] json err: %s\n", err.c_str());
        lastHttpCode = -30;
        return false;
    }
    JsonArray arr = doc["ac"].as<JsonArray>();
    if (arr.isNull()) arr = doc["aircraft"].as<JsonArray>();
    if (arr.isNull()) {
        lastHttpCode = -31;
        return false;
    }

    int count = 0;
    for (JsonObject o : arr) {
        if (!o["lat"].is<double>() || !o["lon"].is<double>()) continue;
        double lat = o["lat"].as<double>();
        double lon = o["lon"].as<double>();
        if (lat == 0.0 && lon == 0.0) continue;

        AircraftItem item;
        item.hex = o["hex"] | "";
        item.hex.toUpperCase();
        item.callsign = o["flight"] | "";
        item.callsign.trim();
        item.reg = o["r"] | "";
        item.reg.trim();
        item.typeCode = o["t"] | "";
        item.typeCode.trim();
        item.desc = o["desc"] | "";
        item.desc.trim();
        item.squawk = o["squawk"] | "";
        item.squawk.trim();
        if (item.callsign.length() == 0) {
            item.callsign = item.reg.length() ? item.reg : (item.hex.length() ? item.hex : String("UNKNOWN"));
        }

        item.lat = lat;
        item.lon = lon;
        if (o["alt_baro"].is<const char*>()) {
            String altStr = o["alt_baro"].as<const char*>();
            item.onGround = (altStr == "ground");
            item.altFt = 0;
        } else {
            item.altFt = o["alt_baro"] | 0;
            item.onGround = false;
        }
        item.speedKt     = o["gs"] | 0.0f;
        item.trackDeg    = o["track"].is<float>() ? o["track"].as<float>() : (o["true_heading"] | 0.0f);
        item.vertRateFpm = o["baro_rate"] | 0;
        item.distKm      = (float)calcDistKm(cLat, cLon, lat, lon);
        item.bearingDeg  = (float)calcBearing(cLat, cLon, lat, lon);
        item.conflict    = false;

        if (count < MAX_PLANES) {
            int pos = count++;
            while (pos > 0 && tempPlanes[pos - 1].distKm > item.distKm) {
                tempPlanes[pos] = tempPlanes[pos - 1];
                pos--;
            }
            tempPlanes[pos] = item;
        } else if (item.distKm < tempPlanes[MAX_PLANES - 1].distKm) {
            int pos = MAX_PLANES - 1;
            while (pos > 0 && tempPlanes[pos - 1].distKm > item.distKm) {
                tempPlanes[pos] = tempPlanes[pos - 1];
                pos--;
            }
            tempPlanes[pos] = item;
        }
    }

    // Conflict check (< 5 km lateral AND < 1000 ft vertical, both airborne)
    for (int i = 0; i < count; i++) {
        if (tempPlanes[i].onGround || tempPlanes[i].altFt < 500) continue;
        for (int j = i + 1; j < count; j++) {
            if (tempPlanes[j].onGround || tempPlanes[j].altFt < 500) continue;
            if (abs(tempPlanes[i].altFt - tempPlanes[j].altFt) < 1000) {
                if (calcDistKm(tempPlanes[i].lat, tempPlanes[i].lon, tempPlanes[j].lat, tempPlanes[j].lon) < 5.0) {
                    tempPlanes[i].conflict = true;
                    tempPlanes[j].conflict = true;
                }
            }
        }
    }

    planeCount = count;
    for (int i = 0; i < planeCount; i++) {
        planes[i] = tempPlanes[i];
        if (planes[i].hex.length() > 0) recordPlaneTrail(planes[i].hex, planes[i].lat, planes[i].lon);
    }

    int foundIdx = -1;
    if (selectedHex.length() > 0) {
        for (int i = 0; i < planeCount; i++) {
            if (planes[i].hex == selectedHex) { foundIdx = i; break; }
        }
    }
    if (foundIdx >= 0) selectedPlane = foundIdx;
    else { selectedPlane = 0; selectedHex = (planeCount > 0) ? planes[0].hex : ""; }
    if (listPage * 7 >= planeCount) listPage = 0;

    radarSource = srcLabel;
    radarFetchOk = true;
    radarStatusLine = String("LIVE SKY  ") + srcLabel;
    return true;
}

static void refreshAdsbData() {
    lastAdsbFetchMs = millis();
    if (WiFi.status() != WL_CONNECTED) connectToWiFi();
    if (WiFi.status() != WL_CONNECTED) {
        radarFetchOk = false;
        radarStatusLine = "WI-FI OFFLINE";
        return;
    }

    double cLat, cLon;
    bool usingGps;
    getRadarCenter(cLat, cLon, usingGps);
    int distNm = (int)ceil((double)RANGE_KM_LEVELS[rangeIdx] / 1.852);
    String latS = String(cLat, 4);
    String lonS = String(cLon, 4);

    int lastCode = 0;
    bool ok = fetchFromEndpoint("https://api.adsb.lol/v2/point/" + latS + "/" + lonS + "/" + String(distNm),
                                "adsb.lol", cLat, cLon, lastCode);
    if (!ok) {
        ok = fetchFromEndpoint("https://opendata.adsb.fi/api/v2/lat/" + latS + "/lon/" + lonS + "/dist/" + String(distNm),
                               "adsb.fi", cLat, cLon, lastCode);
    }
    if (!ok) {
        radarFetchOk = false;
        radarStatusLine = "FEED ERR (" + String(lastCode) + ") - TAP SYNC";
    }
}

// ======================== OWLOS SCOPE RENDERING =======================
static void latLonToScopePx(double lat, double lon, double cLat, double cLon,
                            int rangeKm, int &px, int &py) {
    // Web Mercator offset matching our scaled CartoDB tiles!
    int z = RANGE_TILE_ZOOM[rangeIdx];
    double kmPerTile = 40075.016686 * cos(cLat * DEG_TO_RAD) / (double)(1 << z);
    double radarPxPerTile = kmPerTile * ((double)SCOPE_R / (double)rangeKm);
    double dxTiles = lonToTileX(lon, z) - lonToTileX(cLon, z);
    double dyTiles = latToTileY(lat, z) - latToTileY(cLat, z);
    px = SCOPE_CX + (int)round(dxTiles * radarPxPerTile);
    py = SCOPE_CY + (int)round(dyTiles * radarPxPerTile);
}

static void drawGlowingPlane(LGFX_Sprite &spr, int px, int py, float trackDeg,
                             uint16_t col, bool selected, bool conflict) {
    if (!isfinite(trackDeg)) trackDeg = 0.0f;
    double rad = (trackDeg - 90.0) * DEG_TO_RAD;
    double ca = cos(rad), sa = sin(rad);
    auto rx = [&](float lx, float ly) -> int { return px + (int)round(lx * ca - ly * sa); };
    auto ry = [&](float lx, float ly) -> int { return py + (int)round(lx * sa + ly * ca); };

    // Soft radar bloom glow behind aircraft (iconic OWLOS look)
    spr.fillCircle(px, py, 14, THEMES[owlosHue].accentDim);
    spr.fillCircle(px, py, 9,  col);

    // Heading velocity vector
    if (optVectors) {
        spr.drawLine(px, py, rx(28, 0), ry(28, 0), col);
    }

    // Crisp airplane silhouette: fuselage + swept wings + tailplane
    spr.drawLine(rx(-12, 0), ry(-12, 0), rx(14, 0), ry(14, 0), col);
    spr.drawLine(rx(-11, 1), ry(-11, 1), rx(13, 1), ry(13, 1), col);
    spr.drawLine(rx(-11, -1), ry(-11, -1), rx(13, -1), ry(13, -1), col);

    spr.fillTriangle(rx(5, 0), ry(5, 0), rx(-4, -14), ry(-4, -14), rx(-4, 14), ry(-4, 14), col);
    spr.fillTriangle(rx(-7, 0), ry(-7, 0), rx(-12, -6), ry(-12, -6), rx(-12, 6), ry(-12, 6), col);

    if (conflict && optConflict) {
        spr.drawCircle(px, py, 24, 0xF800);
        spr.drawCircle(px, py, 23, 0xF800);
    }
    if (selected) {
        int b = 22, arm = 8;
        uint16_t sc = 0xFFE0;
        spr.drawFastHLine(px - b, py - b, arm, sc); spr.drawFastVLine(px - b, py - b, arm, sc);
        spr.drawFastHLine(px + b - arm, py - b, arm, sc); spr.drawFastVLine(px + b, py - b, arm, sc);
        spr.drawFastHLine(px - b, py + b, arm, sc); spr.drawFastVLine(px - b, py + b - arm, arm, sc);
        spr.drawFastHLine(px + b - arm, py + b, arm, sc); spr.drawFastVLine(px + b, py + b - arm, arm, sc);
    }
}

static void renderOwlosScopeFrame() {
    if (!ensureScopeBuffers()) return;
    const OwlosTheme &th = THEMES[owlosHue];
    LGFX_Sprite &spr = *scopeSprite;

    if (mapHasData && mapTintedHue != owlosHue) {
        retintDarkBasemap();
    }

    // 1. Fill scope canvas with themed dark map or blueprint grid
    bool useMap = (owlosLook != LOOK_CLASSIC && mapHasData && mapTintBuf != nullptr);
    if (useMap) {
        uint16_t* dst = (uint16_t*)spr.getBuffer();
        if (owlosLook == LOOK_MAP) {
            memcpy(dst, mapTintBuf, (size_t)SCOPE_W * (size_t)SCOPE_H * sizeof(uint16_t));
        } else {
            // Circular mask: blueprint bg outside SCOPE_R, tinted dark map inside!
            uint16_t bgRaw   = spriteSwapBytes ? __builtin_bswap16(th.bg)   : th.bg;
            uint16_t gridRaw = spriteSwapBytes ? __builtin_bswap16(th.grid) : th.grid;
            int r2 = SCOPE_R * SCOPE_R;
            for (int y = 0; y < SCOPE_H; y++) {
                int dy = y - SCOPE_CY;
                int dy2 = dy * dy;
                uint16_t* rowDst = dst + y * SCOPE_W;
                const uint16_t* rowMap = mapTintBuf + y * SCOPE_W;
                bool yGrid = ((y % 28) == 0);
                if (dy2 > r2) {
                    for (int x = 0; x < SCOPE_W; x++) {
                        rowDst[x] = (yGrid || (x % 28) == 0) ? gridRaw : bgRaw;
                    }
                } else {
                    int span = (int)sqrtf((float)(r2 - dy2));
                    int xL = SCOPE_CX - span; if (xL < 0) xL = 0;
                    int xR = SCOPE_CX + span; if (xR > SCOPE_W) xR = SCOPE_W;
                    for (int x = 0; x < xL; x++) rowDst[x] = (yGrid || (x % 28) == 0) ? gridRaw : bgRaw;
                    memcpy(rowDst + xL, rowMap + xL, (size_t)(xR - xL) * sizeof(uint16_t));
                    for (int x = xR; x < SCOPE_W; x++) rowDst[x] = (yGrid || (x % 28) == 0) ? gridRaw : bgRaw;
                }
            }
        }
    } else {
        spr.fillSprite(th.bg);
        for (int x = 0; x < SCOPE_W; x += 28) spr.drawFastVLine(x, 0, SCOPE_H, th.grid);
        for (int y = 0; y < SCOPE_H; y += 28) spr.drawFastHLine(0, y, SCOPE_W, th.grid);
    }

    int rangeKm = RANGE_KM_LEVELS[rangeIdx];
    double cLat, cLon;
    bool usingGps;
    getRadarCenter(cLat, cLon, usingGps);

    // 2. Glowing Radar Sweep Beam (in RADAR & CLASSIC looks)
    if (optSweep && owlosLook != LOOK_MAP) {
        // Subtle 24-degree trailing phosphor glow using radial rays
        for (int step = 22; step >= 1; step -= 2) {
            double ang = (sweepDeg - (float)step - 90.0) * DEG_TO_RAD;
            float fade = (float)(24 - step) / 24.0f;
            fade = fade * fade * 0.45f;
            uint8_t r = (uint8_t)(th.swR * fade);
            uint8_t g = (uint8_t)(th.swG * fade);
            uint8_t b = (uint8_t)(th.swB * fade);
            uint16_t segCol = M5.Display.color565(r, g, b);
            spr.drawLine(SCOPE_CX, SCOPE_CY,
                         SCOPE_CX + (int)(cos(ang) * (SCOPE_R - 2)),
                         SCOPE_CY + (int)(sin(ang) * (SCOPE_R - 2)), segCol);
        }
        // Bright leading beam
        double lead = (sweepDeg - 90.0) * DEG_TO_RAD;
        int lx = SCOPE_CX + (int)(cos(lead) * (SCOPE_R - 1));
        int ly = SCOPE_CY + (int)(sin(lead) * (SCOPE_R - 1));
        spr.drawLine(SCOPE_CX, SCOPE_CY, lx, ly, TFT_WHITE);
        spr.drawLine(SCOPE_CX + 1, SCOPE_CY, lx, ly, th.accent);
    }

    // 3. Concentric Range Rings, Crosshairs & OWLOS Compass Bezel
    if (owlosLook != LOOK_MAP) {
        spr.drawFastHLine(SCOPE_CX - SCOPE_R, SCOPE_CY, SCOPE_R * 2, th.accentDim);
        spr.drawFastVLine(SCOPE_CX, SCOPE_CY - SCOPE_R, SCOPE_R * 2, th.accentDim);

        // 4 Concentric Range Rings with exact OWLOS km labels (e.g. 12.5 / 25 / 37.5 / 50)
        spr.setFont(&fonts::FreeSans9pt7b);
        spr.setTextDatum(middle_left);
        for (int i = 1; i <= 4; i++) {
            int rr = (SCOPE_R * i) / 4;
            spr.drawCircle(SCOPE_CX, SCOPE_CY, rr, (i == 4) ? th.accent : th.ring);
            if (i == 4) spr.drawCircle(SCOPE_CX, SCOPE_CY, rr - 1, th.accent);
            if (i < 4) {
                float kmVal = (float)rangeKm * (float)i / 4.0f;
                String lbl = (fabsf(kmVal - roundf(kmVal)) < 0.1f) ? String((int)roundf(kmVal)) : String(kmVal, 1);
                int lx = SCOPE_CX + (int)(rr * 0.68f);
                int ly = SCOPE_CY + (int)(rr * 0.68f);
                spr.fillRect(lx - 2, ly - 8, lbl.length() * 10 + 4, 16, th.bg);
                spr.setTextColor(th.text);
                spr.drawString(lbl, lx, ly);
            }
        }

        // Outer Degree Bezel: fine 5-deg ticks, medium 10-deg ticks, bold 30-deg ticks + numbers
        for (int d = 0; d < 360; d += 5) {
            double rad = (d - 90.0) * DEG_TO_RAD;
            int len = (d % 30 == 0) ? 14 : ((d % 10 == 0) ? 9 : 5);
            uint16_t tcol = (d % 30 == 0) ? th.accent : th.accentDim;
            spr.drawLine(SCOPE_CX + (int)(cos(rad) * (SCOPE_R - len)),
                         SCOPE_CY + (int)(sin(rad) * (SCOPE_R - len)),
                         SCOPE_CX + (int)(cos(rad) * SCOPE_R),
                         SCOPE_CY + (int)(sin(rad) * SCOPE_R), tcol);
            if (d % 30 == 0 && d % 90 != 0) {
                spr.setTextDatum(middle_center);
                spr.setFont(&fonts::FreeSans9pt7b);
                spr.setTextColor(th.muted);
                int tx = SCOPE_CX + (int)(cos(rad) * (SCOPE_R + 18));
                int ty = SCOPE_CY + (int)(sin(rad) * (SCOPE_R + 18));
                char db[4]; snprintf(db, sizeof(db), "%03d", d);
                spr.drawString(db, tx, ty);
            }
        }

        // Cardinal Labels (N/E/S/W)
        spr.setTextDatum(middle_center);
        spr.setFont(&fonts::FreeSansBold12pt7b);
        spr.setTextColor(th.text);
        spr.drawString("N", SCOPE_CX, SCOPE_CY - SCOPE_R - 17);
        spr.drawString("S", SCOPE_CX, SCOPE_CY + SCOPE_R + 17);
        spr.drawString("W", SCOPE_CX - SCOPE_R - 17, SCOPE_CY);
        spr.drawString("E", SCOPE_CX + SCOPE_R + 17, SCOPE_CY);
    }

    // Center Home Crosshair
    spr.fillCircle(SCOPE_CX, SCOPE_CY, 5, th.accent);
    spr.drawCircle(SCOPE_CX, SCOPE_CY, 9, TFT_WHITE);

    // 4. Aircraft Trails
    for (int i = 0; i < planeCount; i++) {
        for (int t = 0; t < planeTrailCount; t++) {
            if (planeTrails[t].hex != planes[i].hex) continue;
            PlaneTrail &tr = planeTrails[t];
            if (tr.count < 2) break;
            int prevX = 0, prevY = 0;
            for (int k = 0; k < tr.count; k++) {
                int px, py;
                latLonToScopePx(tr.lat[k], tr.lon[k], cLat, cLon, rangeKm, px, py);
                if (k > 0) spr.drawLine(prevX, prevY, px, py, th.accentDim);
                prevX = px; prevY = py;
            }
            break;
        }
    }

    // 5. Aircraft Icons & OWLOS Framed Data Blocks
    for (int i = 0; i < planeCount; i++) {
        int px, py;
        latLonToScopePx(planes[i].lat, planes[i].lon, cLat, cLon, rangeKm, px, py);
        if (owlosLook != LOOK_MAP) {
            int dx = px - SCOPE_CX, dy = py - SCOPE_CY;
            if (dx * dx + dy * dy > (SCOPE_R - 6) * (SCOPE_R - 6)) continue;
        } else {
            if (px < 10 || px > SCOPE_W - 10 || py < 10 || py > SCOPE_H - 10) continue;
        }

        bool sel = (i == selectedPlane);
        uint16_t hCol = owlosHeightColor(planes[i]);
        drawGlowingPlane(spr, px, py, planes[i].trackDeg, hCol, sel, planes[i].conflict);

        // Framed OWLOS Data Block (top-left or top-right offset so it doesn't cover the plane)
        if (optDataBlock && (i < 12 || sel)) {
            int bw = 96, bh = 36;
            int bx = (px > SCOPE_CX) ? (px - bw - 16) : (px + 16);
            int by = (py > SCOPE_CY) ? (py - bh - 10) : (py + 12);
            bx = constrain(bx, 6, SCOPE_W - bw - 6);
            by = constrain(by, 44, SCOPE_H - bh - 36);

            // Leader line from plane to box
            spr.drawLine(px, py, (bx > px) ? bx : (bx + bw), by + bh / 2, sel ? 0xFFE0 : th.accentDim);

            spr.fillRect(bx, by, bw, bh, th.bg);
            spr.drawRect(bx, by, bw, bh, sel ? 0xFFE0 : hCol);

            spr.setTextDatum(top_left);
            spr.setFont(&fonts::FreeSansBold9pt7b);
            spr.setTextColor(sel ? 0xFFE0 : hCol);
            spr.drawString(planes[i].callsign.substring(0, 7), bx + 5, by + 3);

            spr.setFont(&fonts::Font0);
            spr.setTextColor(th.text);
            String hStr = formatOwlosHeight(planes[i]);
            if (planes[i].vertRateFpm > 300) hStr += "^";
            else if (planes[i].vertRateFpm < -300) hStr += "v";
            spr.drawString(hStr, bx + 5, by + 22);
        }
    }

    // 6. OWLOS Corner HUD Overlays (exact match to screenshot!)
    // Top-left: LIVE SKY / source
    spr.setTextDatum(top_left);
    spr.setFont(&fonts::FreeSansBold9pt7b);
    spr.setTextColor(th.text);
    spr.drawString(radarFetchOk ? "LIVE SKY" : "NO FEED", 10, 8);
    spr.setFont(&fonts::Font0);
    spr.setTextColor(th.muted);
    spr.drawString(radarSource + " " + String(rangeKm) + "KM", 10, 28);

    // Top-right chamfered look badge + NORTH UP
    const char* lookLbl = (owlosLook == LOOK_RADAR) ? "RADAR" : ((owlosLook == LOOK_MAP) ? "MAP" : "CLASSIC");
    drawChamferedButton(spr, SCOPE_W - 114, 6, 106, 30, 7, th.panelBg, th.accent, lookLbl, th.accent, &fonts::FreeSansBold9pt7b);
    spr.setTextDatum(top_right);
    spr.setFont(&fonts::Font0);
    spr.setTextColor(th.muted);
    spr.drawString("NORTH UP", SCOPE_W - 10, 40);

    // Bottom-left: HEIGHT Legend (GND / <10K / <25K / HIGH)
    spr.setTextDatum(bottom_left);
    spr.setFont(&fonts::Font0);
    spr.setTextColor(th.muted);
    spr.drawString("HEIGHT", 10, SCOPE_H - 22);
    int lx = 10, ly = SCOPE_H - 10;
    spr.fillCircle(lx + 4, ly, 4, 0x8D16); spr.setTextColor(th.muted); spr.drawString("GND",  lx + 11, ly + 4);
    spr.fillCircle(lx + 42, ly, 4, 0xFD88); spr.drawString("<10K", lx + 49, ly + 4);
    spr.fillCircle(lx + 88, ly, 4, 0x4F1F); spr.drawString("<25K", lx + 95, ly + 4);
    spr.fillCircle(lx + 134, ly, 4, TFT_WHITE); spr.drawString("HIGH", lx + 141, ly + 4);

    // Bottom-right: Range & Attribution
    spr.setTextDatum(bottom_right);
    spr.setTextColor(th.text);
    spr.drawString(String(rangeKm) + " KM", SCOPE_W - 10, SCOPE_H - 8);
    if (useMap) {
        spr.setTextColor(th.muted);
        spr.drawString("OpenStreetMap", SCOPE_W - 10, SCOPE_H - 22);
    }

    // Outer HUD Frame Border
    spr.drawRect(0, 0, SCOPE_W, SCOPE_H, th.accentDim);

    // Atomic flicker-free push!
    spr.pushSprite(SCOPE_X, SCOPE_Y);
}

// ======================== LOWER PANELS & VIEWS ========================
static void drawTopHudBar() {
    const OwlosTheme &th = THEMES[owlosHue];
    M5.Display.fillRect(0, 0, sw, 64, th.panelBg);
    M5.Display.drawFastHLine(0, 63, sw, th.accent);

    drawChamferedButton(M5.Display, 14, 10, 118, 44, 8, th.bg, th.accent, "< HUB", th.accent);

    // Title + Cyber Diagonal Hatch Lines
    M5.Display.setTextDatum(middle_left);
    M5.Display.setFont(&fonts::FreeSansBold12pt7b);
    M5.Display.setTextColor(th.text);
    M5.Display.drawString("OWLOS FLIGHT RADAR", 148, 32);

    // Diagonal hatch lines right of title
    for (int hx = 434; hx < 516; hx += 8) {
        M5.Display.drawLine(hx, 40, hx + 6, 24, th.accentDim);
    }

    // UTC Clock or Refresh button on right
    if (optUtcClock) {
        struct tm utc;
        time_t now = time(nullptr);
        if (now > 100000 && gmtime_r(&now, &utc)) {
            char tb[12];
            snprintf(tb, sizeof(tb), "%02d:%02dZ", utc.tm_hour, utc.tm_min);
            drawChamferedButton(M5.Display, 530, 10, 82, 44, 7, th.bg, th.accentDim, tb, th.muted, &fonts::FreeSansBold9pt7b);
        }
    }
    drawChamferedButton(M5.Display, 620, 10, 86, 44, 8, th.bg, th.accent, "SYNC", th.accent, &fonts::FreeSansBold9pt7b);
}

static void drawBottomActionBar() {
    const OwlosTheme &th = THEMES[owlosHue];
    const char* labels[5] = {"HUE", "LOOK", "RANGE", "LIST", "SET"};
    for (int i = 0; i < 5; i++) {
        int bx = 20 + i * 138;
        bool active = (i == 3 && viewMode == VIEW_LIST) || (i == 4 && viewMode == VIEW_SETTINGS);
        drawChamferedButton(M5.Display, bx, 1188, 128, 72, 10,
                            active ? th.accent : th.panelBg,
                            th.accent, labels[i],
                            active ? th.bg : th.accent);
    }
}

static void drawOwlosFlightsPanel() {
    const OwlosTheme &th = THEMES[owlosHue];
    const int px = 20, py = 744, pw = 680, ph = 432;

    M5.Display.fillRect(px, py, pw, ph, th.panelBg);
    M5.Display.drawRect(px, py, pw, ph, th.accentDim);

    // Subtle blueprint grid inside table
    for (int gx = px + 28; gx < px + pw; gx += 28) M5.Display.drawFastVLine(gx, py + 36, ph - 36, th.grid);
    for (int gy = py + 36; gy < py + ph; gy += 28) M5.Display.drawFastHLine(px, gy, pw, th.grid);

    // Header bar: OWLOS FLIGHTS //////////////////////////
    M5.Display.fillRect(px + 1, py + 1, pw - 2, 34, th.bg);
    M5.Display.drawFastHLine(px, py + 35, pw, th.accentDim);
    M5.Display.setTextDatum(middle_left);
    M5.Display.setFont(&fonts::FreeSansBold9pt7b);
    M5.Display.setTextColor(th.accent);
    M5.Display.drawString("OWLOS FLIGHTS", px + 14, py + 18);
    for (int hx = px + 185; hx < px + pw - 24; hx += 8) {
        M5.Display.drawLine(hx, py + 26, hx + 6, py + 10, th.accentDim);
    }

    // Count + Coordinates + Selected Plane Strip (y: py+38 .. py+118)
    M5.Display.setTextDatum(middle_left);
    M5.Display.setFont(&fonts::FreeSansBold24pt7b);
    M5.Display.setTextColor(th.text);
    M5.Display.drawString(String(planeCount), px + 18, py + 76);

    M5.Display.setFont(&fonts::FreeSansBold9pt7b);
    M5.Display.setTextColor(th.accent);
    M5.Display.drawString("AIRCRAFT", px + 98, py + 62);
    M5.Display.setTextColor(th.muted);
    M5.Display.drawString("IN " + String(RANGE_KM_LEVELS[rangeIdx]) + " KM", px + 98, py + 86);

    double cLat, cLon;
    bool usingGps;
    getRadarCenter(cLat, cLon, usingGps);
    String coordStr = String(fabs(cLat), 2) + (cLat >= 0 ? "N " : "S ") +
                      String(fabs(cLon), 2) + (cLon >= 0 ? "E" : "W");
    M5.Display.setTextDatum(middle_right);
    M5.Display.setTextColor(th.muted);
    M5.Display.drawString(coordStr, px + pw - 16, py + 58);

    // Selected aircraft quick readout on right
    if (planeCount > 0 && selectedPlane < planeCount) {
        AircraftItem &sel = planes[selectedPlane];
        String quick = sel.callsign + " | " + String((int)round(sel.speedKt)) + "kt | HDG " +
                       String((int)round(sel.trackDeg)) + " " + degToCardinal(sel.trackDeg);
        M5.Display.setTextColor(0xFFE0);
        M5.Display.drawString(quick, px + pw - 16, py + 88);
    }

    // Table Columns Header (y: py + 124)
    M5.Display.drawFastHLine(px + 12, py + 114, pw - 24, th.accentDim);
    M5.Display.setFont(&fonts::Font0);
    M5.Display.setTextColor(th.muted);
    M5.Display.setTextDatum(middle_left);
    M5.Display.drawString("NEAREST", px + 20, py + 128);
    M5.Display.drawString("TYPE",    px + 280, py + 128);
    M5.Display.drawString("HEIGHT",  px + 420, py + 128);
    M5.Display.setTextDatum(middle_right);
    M5.Display.drawString("KM",      px + pw - 20, py + 128);
    M5.Display.drawFastHLine(px + 12, py + 140, pw - 24, th.accentDim);

    // Nearest 6 Aircraft Rows (46px each, y: py+146 .. py+422)
    int rows = (planeCount < 6) ? planeCount : 6;
    for (int i = 0; i < rows; i++) {
        AircraftItem &ac = planes[i];
        int ry = py + 146 + i * 46;
        bool sel = (i == selectedPlane);
        if (sel) {
            M5.Display.fillRect(px + 8, ry, pw - 16, 42, th.bg);
            M5.Display.drawRect(px + 8, ry, pw - 16, 42, 0xFFE0);
        }

        uint16_t hCol = owlosHeightColor(ac);
        M5.Display.fillCircle(px + 24, ry + 21, 6, hCol);

        M5.Display.setTextDatum(middle_left);
        M5.Display.setFont(&fonts::FreeSansBold12pt7b);
        M5.Display.setTextColor(sel ? 0xFFE0 : th.text);
        M5.Display.drawString(ac.callsign.substring(0, 9), px + 40, ry + 21);

        M5.Display.setFont(&fonts::FreeSans12pt7b);
        M5.Display.setTextColor(th.muted);
        M5.Display.drawString(ac.typeCode.length() ? ac.typeCode.substring(0, 6) : "----", px + 280, ry + 21);

        M5.Display.setTextColor(hCol);
        M5.Display.drawString(formatOwlosHeight(ac), px + 420, ry + 21);

        M5.Display.setTextDatum(middle_right);
        M5.Display.setTextColor(th.text);
        M5.Display.drawString(String(ac.distKm, 1), px + pw - 20, ry + 21);
    }

    if (planeCount == 0) {
        M5.Display.setTextDatum(middle_center);
        M5.Display.setFont(&fonts::FreeSansBold12pt7b);
        M5.Display.setTextColor(th.muted);
        M5.Display.drawString(radarStatusLine, px + pw / 2, py + 260);
    }
}

static void drawExpandedListView() {
    const OwlosTheme &th = THEMES[owlosHue];
    M5.Display.fillRect(20, 74, 680, 1100, th.panelBg);
    M5.Display.drawRect(20, 74, 680, 1100, th.accentDim);

    // Selected Aircraft Detail Card at Top (y: 88..288)
    M5.Display.fillRect(36, 88, 648, 198, th.bg);
    M5.Display.drawRect(36, 88, 648, 198, th.accent);
    if (planeCount > 0 && selectedPlane < planeCount) {
        AircraftItem &ac = planes[selectedPlane];
        uint16_t hCol = owlosHeightColor(ac);
        M5.Display.setTextDatum(middle_left);
        M5.Display.setFont(&fonts::FreeSansBold24pt7b);
        M5.Display.setTextColor(0xFFE0);
        M5.Display.drawString(ac.callsign, 56, 132);

        M5.Display.setFont(&fonts::FreeSansBold12pt7b);
        M5.Display.setTextColor(th.accent);
        String idLine = (ac.typeCode.length() ? ac.typeCode : String("AIRCRAFT")) +
                        (ac.reg.length() ? ("  |  " + ac.reg) : "") + "  |  " + ac.hex;
        M5.Display.drawString(idLine, 290, 132);

        if (ac.desc.length()) {
            M5.Display.setFont(&fonts::FreeSans9pt7b);
            M5.Display.setTextColor(th.muted);
            M5.Display.drawString(ac.desc.substring(0, 56), 56, 174);
        }

        M5.Display.setFont(&fonts::FreeSansBold12pt7b);
        M5.Display.setTextColor(hCol);
        String alt = ac.onGround ? "GROUND" : (String(ac.altFt) + " ft (" + formatOwlosHeight(ac) + ")");
        if (!ac.onGround && abs(ac.vertRateFpm) > 150) {
            alt += (ac.vertRateFpm > 0 ? " +" : " ") + String(ac.vertRateFpm) + " fpm";
        }
        M5.Display.drawString("ALT: " + alt, 56, 214);

        M5.Display.setTextColor(th.text);
        int mph = (int)round(ac.speedKt * 1.15078f);
        String spd = "SPD: " + String((int)round(ac.speedKt)) + " kt (" + String(mph) + " mph)  |  HDG " +
                     String((int)round(ac.trackDeg)) + " " + degToCardinal(ac.trackDeg) + "  |  " +
                     String(ac.distKm, 1) + " km";
        M5.Display.drawString(spd, 56, 254);
    } else {
        M5.Display.setTextDatum(middle_center);
        M5.Display.setFont(&fonts::FreeSansBold12pt7b);
        M5.Display.setTextColor(th.muted);
        M5.Display.drawString("No aircraft currently in range", sw / 2, 186);
    }

    // Full Paginated List (7 rows, y: 304..1074)
    int perPage = 7;
    int start = listPage * perPage;
    for (int r = 0; r < perPage; r++) {
        int idx = start + r;
        if (idx >= planeCount) break;
        AircraftItem &ac = planes[idx];
        int ry = 306 + r * 108;
        bool sel = (idx == selectedPlane);
        M5.Display.fillRect(36, ry, 648, 96, sel ? th.bg : 0x0842);
        M5.Display.drawRect(36, ry, 648, 96, sel ? 0xFFE0 : th.accentDim);

        uint16_t hCol = owlosHeightColor(ac);
        M5.Display.fillRect(38, ry + 6, 8, 84, hCol);

        M5.Display.setTextDatum(middle_left);
        M5.Display.setFont(&fonts::FreeSansBold18pt7b);
        M5.Display.setTextColor(sel ? 0xFFE0 : th.text);
        M5.Display.drawString(ac.callsign, 60, ry + 32);

        M5.Display.setFont(&fonts::FreeSans9pt7b);
        M5.Display.setTextColor(th.muted);
        String sub = (ac.typeCode.length() ? ac.typeCode : String("----")) +
                     (ac.reg.length() ? (" | " + ac.reg) : "") +
                     " | " + String((int)round(ac.speedKt)) + " kt | HDG " +
                     String((int)round(ac.trackDeg)) + " " + degToCardinal(ac.trackDeg);
        M5.Display.drawString(sub, 60, ry + 70);

        M5.Display.setTextDatum(middle_right);
        M5.Display.setFont(&fonts::FreeSansBold12pt7b);
        M5.Display.setTextColor(hCol);
        M5.Display.drawString(formatOwlosHeight(ac), 668, ry + 32);
        M5.Display.setTextColor(th.accent);
        M5.Display.drawString(String(ac.distKm, 1) + " km", 668, ry + 70);
    }

    // Page Controls (y: 1086..1156)
    drawChamferedButton(M5.Display, 36,  1086, 200, 68, 10, th.bg, th.accent, "< PREV", listPage > 0 ? th.accent : th.muted);
    drawChamferedButton(M5.Display, 260, 1086, 200, 68, 10, th.bg, th.accent, "RADAR",  th.text);
    drawChamferedButton(M5.Display, 484, 1086, 200, 68, 10, th.bg, th.accent, "NEXT >", ((listPage + 1) * 7 < planeCount) ? th.accent : th.muted);
}

static void drawOwlosSettingsView() {
    const OwlosTheme &th = THEMES[owlosHue];
    M5.Display.fillRect(20, 74, 680, 1100, th.panelBg);
    M5.Display.drawRect(20, 74, 680, 1100, th.accent);

    M5.Display.setTextDatum(middle_left);
    M5.Display.setFont(&fonts::FreeSansBold18pt7b);
    M5.Display.setTextColor(th.accent);
    M5.Display.drawString("OWLOS RADAR VIEW SETTINGS", 48, 122);

    // 1. LOOK (y: 190..260)
    M5.Display.setFont(&fonts::FreeSansBold12pt7b);
    M5.Display.setTextColor(th.muted);
    M5.Display.drawString("Look", 48, 174);
    const char* lookNames[3] = {"RADAR", "MAP", "CLASSIC"};
    for (int i = 0; i < 3; i++) {
        bool act = ((int)owlosLook == i);
        drawChamferedButton(M5.Display, 48 + i * 212, 198, 196, 68, 10,
                            act ? th.accent : th.bg, th.accent,
                            lookNames[i], act ? th.bg : th.text);
    }

    // 2. COLOURS (2 rows: 3 + 2, y: 315..475)
    M5.Display.setTextDatum(middle_left);
    M5.Display.setFont(&fonts::FreeSansBold12pt7b);
    M5.Display.setTextColor(th.muted);
    M5.Display.drawString("Colours", 48, 304);
    for (int i = 0; i < 5; i++) {
        int col = i % 3;
        int row = i / 3;
        bool act = ((int)owlosHue == i);
        drawChamferedButton(M5.Display, 48 + col * 212, 328 + row * 82, 196, 68, 10,
                            act ? THEMES[i].accent : th.bg, THEMES[i].accent,
                            THEMES[i].name, act ? th.bg : THEMES[i].accent);
    }

    // 3. RANGE (y: 540..610)
    M5.Display.setTextDatum(middle_left);
    M5.Display.setFont(&fonts::FreeSansBold12pt7b);
    M5.Display.setTextColor(th.muted);
    M5.Display.drawString("Range: " + String(RANGE_KM_LEVELS[rangeIdx]) + " km", 48, 526);
    for (int i = 0; i < 4; i++) {
        bool act = (rangeIdx == i);
        drawChamferedButton(M5.Display, 48 + i * 160, 550, 146, 68, 10,
                            act ? th.accent : th.bg, th.accent,
                            String(RANGE_KM_LEVELS[i]) + " KM", act ? th.bg : th.text);
    }

    // 4. ON THE RADAR toggles (y: 675..845)
    M5.Display.setTextDatum(middle_left);
    M5.Display.setFont(&fonts::FreeSansBold12pt7b);
    M5.Display.setTextColor(th.muted);
    M5.Display.drawString("On the radar", 48, 662);

    struct ToggleItem { const char* lbl; bool val; };
    ToggleItem toggles[5] = {
        {"VECTORS",    optVectors},
        {"UTC CLOCK",  optUtcClock},
        {"DATA BLOCK", optDataBlock},
        {"CONFLICT",   optConflict},
        {"SWEEP",      optSweep}
    };
    for (int i = 0; i < 5; i++) {
        int col = i % 3;
        int row = i / 3;
        drawChamferedButton(M5.Display, 48 + col * 212, 688 + row * 84, 196, 68, 10,
                            toggles[i].val ? th.accent : th.bg, th.accent,
                            toggles[i].lbl, toggles[i].val ? th.bg : th.text);
    }

    // Map Tile Reload Button (y: 900..968)
    drawChamferedButton(M5.Display, 48, 898, 624, 68, 10, th.bg, th.accent,
                        "RELOAD DARK MAP TILES OVER WI-FI", th.accent);

    // Done Button (y: 1050..1130)
    drawChamferedButton(M5.Display, 140, 1048, 440, 82, 12, th.accent, th.accent,
                        "DONE - BACK TO RADAR", th.bg, &fonts::FreeSansBold18pt7b);
}

// ======================== PUBLIC LIFECYCLE ============================
void initFlightRadar() {
    loadOwlosPrefs();
    radarActive = true;
    viewMode = VIEW_MAIN;
    mapNeedFetch = true;
    ensureScopeBuffers();
    drawFlightRadar();
    refreshAdsbData();
    if (owlosLook != LOOK_CLASSIC) {
        fetchDarkBasemapForScope();
    }
    lastAdsbFetchMs = millis(); // Reset refresh timer AFTER tile download finishes
    lastAnimMs = millis();
    drawFlightRadar();
}

void drawFlightRadar() {
    const OwlosTheme &th = THEMES[owlosHue];
    M5.Display.fillScreen(th.bg);
    drawTopHudBar();

    if (viewMode == VIEW_MAIN) {
        renderOwlosScopeFrame();
        drawOwlosFlightsPanel();
    } else if (viewMode == VIEW_LIST) {
        drawExpandedListView();
    } else {
        drawOwlosSettingsView();
    }
    drawBottomActionBar();
}

void handleFlightRadarTouch(int tx, int ty) {
    // Top Bar
    if (ty < 64) {
        if (tx >= 14 && tx <= 132) {
            radarActive = false;
            releaseScopeBuffers();
            playTone(440, 60);
            currentState = STATE_MAIN_MENU;
            drawMainMenu();
            return;
        }
        if (tx >= 620 && tx <= 706) {
            playTone(660, 60);
            refreshAdsbData();
            drawFlightRadar();
            return;
        }
        return;
    }

    // Bottom OWLOS Chamfered Action Bar (HUE | LOOK | RANGE | LIST | SET)
    if (ty >= 1185) {
        for (int i = 0; i < 5; i++) {
            int bx = 20 + i * 138;
            if (tx >= bx && tx <= bx + 128) {
                playTone(587, 45);
                if (i == 0) {
                    owlosHue = (OwlosHue)(((int)owlosHue + 1) % 5);
                    saveOwlosPrefs();
                    drawFlightRadar();
                } else if (i == 1) {
                    owlosLook = (OwlosLook)(((int)owlosLook + 1) % 3);
                    saveOwlosPrefs();
                    viewMode = VIEW_MAIN;
                    if (owlosLook != LOOK_CLASSIC && !mapHasData) fetchDarkBasemapForScope();
                    drawFlightRadar();
                } else if (i == 2) {
                    rangeIdx = (rangeIdx + 1) % 4;
                    saveOwlosPrefs();
                    mapNeedFetch = true;
                    refreshAdsbData();
                    if (owlosLook != LOOK_CLASSIC) fetchDarkBasemapForScope();
                    drawFlightRadar();
                } else if (i == 3) {
                    viewMode = (viewMode == VIEW_LIST) ? VIEW_MAIN : VIEW_LIST;
                    drawFlightRadar();
                } else if (i == 4) {
                    viewMode = (viewMode == VIEW_SETTINGS) ? VIEW_MAIN : VIEW_SETTINGS;
                    drawFlightRadar();
                }
                return;
            }
        }
        return;
    }

    // VIEW_MAIN Touch Handling
    if (viewMode == VIEW_MAIN) {
        // Tap plane directly on radar scope
        if (tx >= SCOPE_X && tx <= SCOPE_X + SCOPE_W && ty >= SCOPE_Y && ty <= SCOPE_Y + SCOPE_H && planeCount > 0) {
            int relX = tx - SCOPE_X;
            int relY = ty - SCOPE_Y;
            double cLat, cLon;
            bool usingGps;
            getRadarCenter(cLat, cLon, usingGps);
            int rangeKm = RANGE_KM_LEVELS[rangeIdx];

            int bestIdx = -1;
            float bestDist = 48.0f;
            for (int i = 0; i < planeCount; i++) {
                int px, py;
                latLonToScopePx(planes[i].lat, planes[i].lon, cLat, cLon, rangeKm, px, py);
                float d = hypotf((float)(relX - px), (float)(relY - py));
                if (d < bestDist) { bestDist = d; bestIdx = i; }
            }
            if (bestIdx >= 0) {
                selectedPlane = bestIdx;
                selectedHex = planes[bestIdx].hex;
                playTone(660, 40);
                renderOwlosScopeFrame();
                drawOwlosFlightsPanel();
            }
            return;
        }

        // Tap row inside OWLOS FLIGHTS table (y: 744+146 .. 744+422)
        if (tx >= 20 && tx <= 700 && ty >= 890 && ty <= 1166) {
            int row = (ty - 890) / 46;
            if (row >= 0 && row < planeCount && row < 6) {
                selectedPlane = row;
                selectedHex = planes[row].hex;
                playTone(660, 40);
                renderOwlosScopeFrame();
                drawOwlosFlightsPanel();
            }
            return;
        }
        return;
    }

    // VIEW_LIST Touch Handling
    if (viewMode == VIEW_LIST) {
        int start = listPage * 7;
        for (int r = 0; r < 7; r++) {
            int idx = start + r;
            if (idx >= planeCount) break;
            int ry = 306 + r * 108;
            if (tx >= 36 && tx <= 684 && ty >= ry && ty <= ry + 96) {
                selectedPlane = idx;
                selectedHex = planes[idx].hex;
                viewMode = VIEW_MAIN;
                playTone(660, 50);
                drawFlightRadar();
                return;
            }
        }
        if (ty >= 1086 && ty <= 1154) {
            if (tx >= 36 && tx <= 236 && listPage > 0) { listPage--; playTone(440, 40); drawFlightRadar(); return; }
            if (tx >= 260 && tx <= 460) { viewMode = VIEW_MAIN; playTone(523, 40); drawFlightRadar(); return; }
            if (tx >= 484 && tx <= 684 && (listPage + 1) * 7 < planeCount) { listPage++; playTone(660, 40); drawFlightRadar(); return; }
        }
        return;
    }

    // VIEW_SETTINGS Touch Handling
    if (viewMode == VIEW_SETTINGS) {
        // Look row (y: 198..266)
        if (ty >= 198 && ty <= 266) {
            for (int i = 0; i < 3; i++) {
                int bx = 48 + i * 212;
                if (tx >= bx && tx <= bx + 196) {
                    owlosLook = (OwlosLook)i;
                    saveOwlosPrefs();
                    playTone(587, 40);
                    drawFlightRadar();
                    return;
                }
            }
        }
        // Colours rows (y: 328..478)
        for (int i = 0; i < 5; i++) {
            int col = i % 3, row = i / 3;
            int bx = 48 + col * 212, by = 328 + row * 82;
            if (tx >= bx && tx <= bx + 196 && ty >= by && ty <= by + 68) {
                owlosHue = (OwlosHue)i;
                saveOwlosPrefs();
                playTone(587, 40);
                drawFlightRadar();
                return;
            }
        }
        // Range row (y: 550..618)
        if (ty >= 550 && ty <= 618) {
            for (int i = 0; i < 4; i++) {
                int bx = 48 + i * 160;
                if (tx >= bx && tx <= bx + 146) {
                    rangeIdx = i;
                    mapNeedFetch = true;
                    saveOwlosPrefs();
                    playTone(587, 40);
                    drawFlightRadar();
                    return;
                }
            }
        }
        // On the radar toggles (y: 688..840)
        for (int i = 0; i < 5; i++) {
            int col = i % 3, row = i / 3;
            int bx = 48 + col * 212, by = 688 + row * 84;
            if (tx >= bx && tx <= bx + 196 && ty >= by && ty <= by + 68) {
                if (i == 0) optVectors   = !optVectors;
                if (i == 1) optUtcClock  = !optUtcClock;
                if (i == 2) optDataBlock = !optDataBlock;
                if (i == 3) optConflict  = !optConflict;
                if (i == 4) optSweep     = !optSweep;
                saveOwlosPrefs();
                playTone(587, 40);
                drawFlightRadar();
                return;
            }
        }
        // Reload dark map tiles
        if (tx >= 48 && tx <= 672 && ty >= 898 && ty <= 966) {
            playTone(660, 50);
            fetchDarkBasemapForScope();
            viewMode = VIEW_MAIN;
            drawFlightRadar();
            return;
        }
        // Done button
        if (tx >= 140 && tx <= 580 && ty >= 1048 && ty <= 1130) {
            viewMode = VIEW_MAIN;
            if (mapNeedFetch) {
                refreshAdsbData();
                if (owlosLook != LOOK_CLASSIC) fetchDarkBasemapForScope();
            }
            playTone(660, 50);
            drawFlightRadar();
            return;
        }
    }
}

void loopFlightRadar() {
    if (currentState != STATE_FLIGHT_RADAR || !radarActive) return;

    unsigned long now = millis();

    // Smooth flicker-free rotating radar sweep in PSRAM sprite
    if (viewMode == VIEW_MAIN && optSweep && owlosLook != LOOK_MAP) {
        if (now - lastAnimMs >= 140UL) {
            lastAnimMs = now;
            sweepDeg += 5.0f;
            if (sweepDeg >= 360.0f) sweepDeg -= 360.0f;
            renderOwlosScopeFrame();
            yield();
        }
    }

    // Periodic live ADS-B refresh every 20 seconds
    if (now - lastAdsbFetchMs >= ADSB_REFRESH_MS) {
        refreshAdsbData();
        lastAdsbFetchMs = millis();
        if (viewMode == VIEW_MAIN) {
            drawTopHudBar();
            renderOwlosScopeFrame();
            drawOwlosFlightsPanel();
        } else if (viewMode == VIEW_LIST) {
            drawFlightRadar();
        }
    }
}
