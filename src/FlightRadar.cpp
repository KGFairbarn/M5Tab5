#include "FlightRadar.h"
#include "MeshtasticBLE.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <math.h>

// ======================== CONFIGURATION ===============================
// Default center: Barrington / Portsmouth, NH (automatically uses live T114 GPS when locked)
static const double DEFAULT_RADAR_LAT = 43.2189;
static const double DEFAULT_RADAR_LON = -71.1069;

static const int RANGE_NM_LEVELS[4] = {10, 25, 50, 100};
static int rangeIdx = 1; // Default 25 nm

static const int MAX_PLANES = 40;
static const int MAX_PLANE_TRAIL = 8;

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
    float distNm = 0.0f;
    float bearingDeg = 0.0f;
};

struct PlaneTrail {
    String hex;
    double lat[MAX_PLANE_TRAIL];
    double lon[MAX_PLANE_TRAIL];
    int count = 0;
    unsigned long lastSeenMs = 0;
};

static AircraftItem planes[MAX_PLANES];
static AircraftItem tempPlanes[MAX_PLANES]; // Static (off stack!) to prevent loopTask stack overflow
static int planeCount = 0;
static int selectedPlane = 0;
static String selectedHex = "";
static int flightListPage = 0;
static int radarTab = 0; // 0 = RADAR, 1 = FLIGHTS LIST
static bool showTrails = true;

static PlaneTrail planeTrails[MAX_PLANES];
static int planeTrailCount = 0;

static unsigned long lastRadarFetchMs = 0;
static const unsigned long RADAR_REFRESH_MS = 12000UL;
static bool radarInitialized = false;
static bool radarFetchOk = false;
static String radarSourceName = "adsb.lol";
static String radarStatusMsg = "Tap REFRESH or wait for Wi-Fi scan...";

// ======================== GEO MATH HELPERS ============================
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

static double calcDistNm(double lat1, double lon1, double lat2, double lon2) {
    const double R_NM = 3440.065;
    double dLat = (lat2 - lat1) * DEG_TO_RAD;
    double dLon = (lon2 - lon1) * DEG_TO_RAD;
    double a = sin(dLat / 2.0) * sin(dLat / 2.0) +
               cos(lat1 * DEG_TO_RAD) * cos(lat2 * DEG_TO_RAD) *
               sin(dLon / 2.0) * sin(dLon / 2.0);
    double c = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
    return R_NM * c;
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
    int idx = (int)floor((deg + 22.5f) / 45.0f) % 8;
    return dirs[idx];
}

static uint16_t altitudeColor(const AircraftItem &ac) {
    if (ac.squawk == "7500" || ac.squawk == "7600" || ac.squawk == "7700") return C_RED;
    if (ac.onGround || ac.altFt < 3000)  return C_ORANGE;
    if (ac.altFt < 15000)                return C_GREEN;
    if (ac.altFt < 30000)                return C_CYAN;
    return 0xE3BF; // Bright magenta/violet for high-altitude cruise
}

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
            // Evict oldest trail
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
    if (tr.count > 0) {
        double d = calcDistNm(tr.lat[tr.count - 1], tr.lon[tr.count - 1], lat, lon);
        if (d < 0.05) return; // Haven't moved meaningfully
    }
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

// ======================== LIVE ADS-B API FETCH ========================
static bool fetchFromEndpoint(const String &url, const char* srcLabel, double cLat, double cLon) {
    WiFiClientSecure* client = new WiFiClientSecure();
    if (!client) return false;
    client->setInsecure();

    HTTPClient http;
    http.useHTTP10(true); // Avoid chunked transfer framing so stream parses cleanly
    http.setTimeout(9000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setUserAgent("M5Tab5-FlightRadar/1.0");

    if (!http.begin(*client, url)) {
        delete client;
        return false;
    }
    http.addHeader("Accept-Encoding", "identity");

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        http.end();
        delete client;
        return false;
    }

    // Filter only the fields we display so large responses parse in minimal RAM
    JsonDocument filter;
    JsonObject acF = filter["ac"].add<JsonObject>();
    acF["hex"]       = true;
    acF["flight"]    = true;
    acF["r"]         = true;
    acF["t"]         = true;
    acF["desc"]      = true;
    acF["lat"]       = true;
    acF["lon"]       = true;
    acF["alt_baro"]  = true;
    acF["gs"]        = true;
    acF["track"]     = true;
    acF["baro_rate"] = true;
    acF["squawk"]    = true;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    http.end();
    delete client;

    if (err) return false;
    JsonArray arr = doc["ac"].as<JsonArray>();
    if (arr.isNull()) return false;

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
        item.trackDeg    = o["track"] | 0.0f;
        item.vertRateFpm = o["baro_rate"] | 0;
        item.distNm      = (float)calcDistNm(cLat, cLon, lat, lon);
        item.bearingDeg  = (float)calcBearing(cLat, cLon, lat, lon);

        // Insert sorted by closest distance (keeps nearest MAX_PLANES)
        if (count < MAX_PLANES) {
            int pos = count++;
            while (pos > 0 && tempPlanes[pos - 1].distNm > item.distNm) {
                tempPlanes[pos] = tempPlanes[pos - 1];
                pos--;
            }
            tempPlanes[pos] = item;
        } else if (item.distNm < tempPlanes[MAX_PLANES - 1].distNm) {
            int pos = MAX_PLANES - 1;
            while (pos > 0 && tempPlanes[pos - 1].distNm > item.distNm) {
                tempPlanes[pos] = tempPlanes[pos - 1];
                pos--;
            }
            tempPlanes[pos] = item;
        }
    }

    planeCount = count;
    for (int i = 0; i < planeCount; i++) {
        planes[i] = tempPlanes[i];
        if (planes[i].hex.length() > 0) {
            recordPlaneTrail(planes[i].hex, planes[i].lat, planes[i].lon);
        }
    }

    // Preserve currently selected aircraft by ICAO hex if still in range
    int foundIdx = -1;
    if (selectedHex.length() > 0) {
        for (int i = 0; i < planeCount; i++) {
            if (planes[i].hex == selectedHex) { foundIdx = i; break; }
        }
    }
    if (foundIdx >= 0) {
        selectedPlane = foundIdx;
    } else {
        selectedPlane = 0;
        selectedHex = (planeCount > 0) ? planes[0].hex : "";
    }
    if (flightListPage * 6 >= planeCount) flightListPage = 0;

    radarSourceName = srcLabel;
    radarFetchOk = true;
    radarStatusMsg = String(planeCount) + " planes in " + String(RANGE_NM_LEVELS[rangeIdx]) + " nm (" + radarSourceName + ")";
    return true;
}

static void refreshFlightRadar(bool showBusyBanner) {
    lastRadarFetchMs = millis();
    if (showBusyBanner) {
        radarStatusMsg = "Scanning ADS-B sky via Wi-Fi...";
        drawFlightRadar();
    }

    if (WiFi.status() != WL_CONNECTED) {
        connectToWiFi();
    }
    if (WiFi.status() != WL_CONNECTED) {
        radarFetchOk = false;
        radarStatusMsg = "Wi-Fi offline! Connect in Settings -> Wi-Fi.";
        drawFlightRadar();
        return;
    }

    double cLat, cLon;
    bool usingGps;
    getRadarCenter(cLat, cLon, usingGps);
    int rNm = RANGE_NM_LEVELS[rangeIdx];
    String coords = String(cLat, 4) + "/" + String(cLon, 4) + "/" + String(rNm);

    // 1. Primary: adsb.lol (OWLOS Flight Radar community feed)
    bool ok = fetchFromEndpoint("https://api.adsb.lol/v2/point/" + coords, "adsb.lol", cLat, cLon);
    // 2. Fallback: airplanes.live
    if (!ok) {
        ok = fetchFromEndpoint("https://api.airplanes.live/v2/point/" + coords, "airplanes.live", cLat, cLon);
    }
    if (!ok) {
        radarFetchOk = false;
        radarStatusMsg = "ADS-B feed error - tap REFRESH to retry";
    }
    drawFlightRadar();
}

// ======================== RADAR DRAWING ===============================
static void latLonToRadarPx(double lat, double lon, double cLat, double cLon,
                            int cx, int cy, float pxPerNm, int &px, int &py) {
    double dN_nm = (lat - cLat) * 60.0;
    double dE_nm = (lon - cLon) * 60.0 * cos(cLat * DEG_TO_RAD);
    px = cx + (int)round(dE_nm * pxPerNm);
    py = cy - (int)round(dN_nm * pxPerNm);
}

static void drawAirplaneIcon(int px, int py, float trackDeg, uint16_t col, bool selected) {
    double rad = (trackDeg - 90.0) * DEG_TO_RAD;
    double ca = cos(rad), sa = sin(rad);
    auto rotX = [&](float lx, float ly) -> int { return px + (int)round(lx * ca - ly * sa); };
    auto rotY = [&](float lx, float ly) -> int { return py + (int)round(lx * sa + ly * ca); };

    // Velocity heading vector (nose leader)
    M5.Display.drawLine(px, py, rotX(22, 0), rotY(22, 0), col);

    // Fuselage
    M5.Display.drawLine(rotX(-10, 0), rotY(-10, 0), rotX(12, 0), rotY(12, 0), col);
    M5.Display.drawLine(rotX(-10, 1), rotY(-10, 1), rotX(11, 1), rotY(11, 1), col);
    M5.Display.drawLine(rotX(-10, -1), rotY(-10, -1), rotX(11, -1), rotY(11, -1), col);

    // Swept main wings
    M5.Display.fillTriangle(rotX(4, 0), rotY(4, 0),
                            rotX(-3, -12), rotY(-3, -12),
                            rotX(-3, 12), rotY(-3, 12), col);

    // Tail stabilizer
    M5.Display.drawLine(rotX(-9, -5), rotY(-9, -5), rotX(-9, 5), rotY(-9, 5), col);
    M5.Display.drawLine(rotX(-10, -5), rotY(-10, -5), rotX(-10, 5), rotY(-10, 5), col);

    // Selected targeting reticle
    if (selected) {
        M5.Display.drawCircle(px, py, 20, C_GOLD);
        M5.Display.drawCircle(px, py, 19, C_GOLD);
    }
}

static void drawRadarScopeTab() {
    const int mapX = 20, mapY = 166, mapS = 680;
    const int cx = mapX + mapS / 2;
    const int cy = mapY + mapS / 2;
    const int maxR = mapS / 2 - 26;
    const int rangeNm = RANGE_NM_LEVELS[rangeIdx];
    const float pxPerNm = (float)maxR / (float)rangeNm;

    double cLat, cLon;
    bool usingGps;
    getRadarCenter(cLat, cLon, usingGps);

    // Classic OWLOS Dark ATC Radar Scope
    M5.Display.fillRoundRect(mapX, mapY, mapS, mapS, 18, 0x00A1);
    M5.Display.drawRoundRect(mapX, mapY, mapS, mapS, 18, 0x0540);

    // Subtle Crosshairs & Degree Ticks
    M5.Display.drawFastHLine(cx - maxR, cy, maxR * 2, 0x0240);
    M5.Display.drawFastVLine(cx, cy - maxR, maxR * 2, 0x0240);
    for (int d = 0; d < 360; d += 30) {
        double rad = (d - 90.0) * DEG_TO_RAD;
        int rIn = (d % 90 == 0) ? (maxR - 14) : (maxR - 8);
        M5.Display.drawLine(cx + cos(rad) * rIn, cy + sin(rad) * rIn,
                            cx + cos(rad) * maxR, cy + sin(rad) * maxR, 0x04A0);
    }

    // Concentric Range Rings (1/3, 2/3, 3/3)
    M5.Display.setTextDatum(middle_left);
    M5.Display.setFont(&fonts::FreeSans9pt7b);
    for (int i = 1; i <= 3; i++) {
        int rr = (maxR * i) / 3;
        M5.Display.drawCircle(cx, cy, rr, (i == 3) ? 0x0560 : 0x0320);
        int nmVal = (rangeNm * i) / 3;
        M5.Display.setTextColor(0x0660);
        M5.Display.drawString(String(nmVal) + "nm", cx + 6, cy - rr + 12);
    }

    // Cardinal Markers
    M5.Display.setTextDatum(middle_center);
    M5.Display.setFont(&fonts::FreeSansBold12pt7b);
    M5.Display.setTextColor(C_RED);    M5.Display.drawString("N", cx, mapY + 16);
    M5.Display.setTextColor(0x0660);   M5.Display.drawString("S", cx, mapY + mapS - 16);
    M5.Display.drawString("W", mapX + 16, cy);
    M5.Display.drawString("E", mapX + mapS - 16, cy);

    // Center Home / GPS Marker
    M5.Display.fillCircle(cx, cy, 6, C_BLUE);
    M5.Display.drawCircle(cx, cy, 8, TFT_WHITE);

    // Clip aircraft & trails inside radar box
    M5.Display.setClipRect(mapX + 4, mapY + 4, mapS - 8, mapS - 8);

    // Draw Aircraft Trails
    if (showTrails) {
        for (int i = 0; i < planeCount; i++) {
            for (int t = 0; t < planeTrailCount; t++) {
                if (planeTrails[t].hex != planes[i].hex) continue;
                PlaneTrail &tr = planeTrails[t];
                if (tr.count < 2) break;
                int prevX = 0, prevY = 0;
                for (int k = 0; k < tr.count; k++) {
                    int px, py;
                    latLonToRadarPx(tr.lat[k], tr.lon[k], cLat, cLon, cx, cy, pxPerNm, px, py);
                    if (k > 0) M5.Display.drawLine(prevX, prevY, px, py, 0x04A4);
                    prevX = px; prevY = py;
                }
                break;
            }
        }
    }

    // Draw Aircraft Icons + Callsign/Altitude Tags
    M5.Display.setTextDatum(middle_left);
    M5.Display.setFont(&fonts::FreeSans9pt7b);
    for (int i = 0; i < planeCount; i++) {
        int px, py;
        latLonToRadarPx(planes[i].lat, planes[i].lon, cLat, cLon, cx, cy, pxPerNm, px, py);
        if (px < mapX + 12 || px > mapX + mapS - 12 || py < mapY + 12 || py > mapY + mapS - 12) continue;

        bool sel = (i == selectedPlane);
        uint16_t col = sel ? C_GOLD : altitudeColor(planes[i]);
        drawAirplaneIcon(px, py, planes[i].trackDeg, col, sel);

        String fl = planes[i].onGround ? "GND" : String(planes[i].altFt / 100);
        String trend = (planes[i].vertRateFpm > 250) ? "+" : ((planes[i].vertRateFpm < -250) ? "-" : "");
        String tag = planes[i].callsign.substring(0, 7) + " " + fl + trend;
        M5.Display.setTextColor(sel ? C_GOLD : col);
        M5.Display.drawString(tag, px + 14, py - 8);
    }

    M5.Display.clearClipRect();

    // Range & Trail Controls Row (y: 856..918)
    M5.Display.setTextDatum(middle_center);
    M5.Display.setFont(&fonts::FreeSansBold12pt7b);
    M5.Display.fillRoundRect(20, 856, 155, 62, 14, C_PANEL);
    M5.Display.setTextColor(rangeIdx > 0 ? TFT_WHITE : C_GRAY);
    M5.Display.drawString("ZOOM IN", 97, 887);

    M5.Display.fillRoundRect(185, 856, 180, 62, 14, C_PANEL);
    M5.Display.setTextColor(C_CYAN);
    M5.Display.drawString(String(rangeNm) + " nm", 275, 887);

    M5.Display.fillRoundRect(375, 856, 155, 62, 14, C_PANEL);
    M5.Display.setTextColor(rangeIdx < 3 ? TFT_WHITE : C_GRAY);
    M5.Display.drawString("ZOOM OUT", 452, 887);

    M5.Display.fillRoundRect(540, 856, 160, 62, 14, showTrails ? C_TEAL : C_PANEL);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.drawString(showTrails ? "TRAILS ON" : "TRAILS OFF", 620, 887);

    // Selected Aircraft Telemetry Card (y: 930..1152)
    M5.Display.fillRoundRect(20, 930, sw - 40, 222, 18, C_PANEL);
    if (planeCount > 0 && selectedPlane < planeCount) {
        AircraftItem &ac = planes[selectedPlane];
        uint16_t aCol = altitudeColor(ac);

        M5.Display.setTextDatum(middle_left);
        M5.Display.setFont(&fonts::FreeSansBold18pt7b);
        M5.Display.setTextColor(C_GOLD);
        M5.Display.drawString(ac.callsign, 42, 968);

        M5.Display.setFont(&fonts::FreeSansBold12pt7b);
        M5.Display.setTextColor(C_CYAN);
        String meta = (ac.typeCode.length() ? ac.typeCode : String("AIRCRAFT"));
        if (ac.reg.length()) meta += "  |  " + ac.reg;
        meta += "  |  " + ac.hex;
        M5.Display.drawString(meta, 240, 968);

        if (ac.desc.length()) {
            M5.Display.setFont(&fonts::FreeSans9pt7b);
            M5.Display.setTextColor(C_GRAY);
            M5.Display.drawString(ac.desc.substring(0, 54), 42, 1002);
        }

        // Row 1 Telemetry: ALT & SPD
        M5.Display.setFont(&fonts::FreeSansBold12pt7b);
        M5.Display.setTextColor(aCol);
        String altStr = ac.onGround ? "GROUND" : (String(ac.altFt) + " ft");
        if (!ac.onGround && abs(ac.vertRateFpm) > 150) {
            altStr += (ac.vertRateFpm > 0 ? "  +" : "  ") + String(ac.vertRateFpm) + " fpm";
        }
        M5.Display.drawString("ALT: " + altStr, 42, 1044);

        M5.Display.setTextColor(TFT_WHITE);
        int mph = (int)round(ac.speedKt * 1.15078f);
        M5.Display.drawString("SPD: " + String((int)round(ac.speedKt)) + " kt (" + String(mph) + " mph)", 42, 1084);

        // Row 2 Telemetry: HDG, DIST, SQUAWK
        String navStr = "HDG: " + String((int)round(ac.trackDeg)) + " deg " + degToCardinal(ac.trackDeg) +
                        "  |  " + String(ac.distNm, 1) + " nm";
        if (ac.squawk.length()) navStr += "  |  SQK " + ac.squawk;
        M5.Display.setTextColor(C_CYAN);
        M5.Display.drawString(navStr, 42, 1124);

        // Quick Prev / Next Plane Buttons on right side of card
        M5.Display.setTextDatum(middle_center);
        M5.Display.fillRoundRect(536, 1020, 72, 112, 14, C_DARK);
        M5.Display.drawRoundRect(536, 1020, 72, 112, 14, C_GRAY);
        M5.Display.setFont(&fonts::FreeSansBold18pt7b);
        M5.Display.setTextColor(TFT_WHITE);
        M5.Display.drawString("<", 572, 1076);

        M5.Display.fillRoundRect(616, 1020, 72, 112, 14, C_DARK);
        M5.Display.drawRoundRect(616, 1020, 72, 112, 14, C_GRAY);
        M5.Display.drawString(">", 652, 1076);
    } else {
        M5.Display.setTextDatum(middle_center);
        M5.Display.setFont(&fonts::FreeSansBold12pt7b);
        M5.Display.setTextColor(C_GRAY);
        M5.Display.drawString("No aircraft in " + String(rangeNm) + " nm range", sw / 2, 1025);
        M5.Display.setFont(&fonts::FreeSans9pt7b);
        M5.Display.setTextColor(C_CYAN);
        M5.Display.drawString("Tap ZOOM OUT for wider sky or REFRESH to poll ADS-B", sw / 2, 1068);
    }
}

static void drawFlightsListTab() {
    M5.Display.fillRoundRect(20, 166, sw - 40, 890, 16, C_PANEL);
    M5.Display.setTextDatum(middle_center);
    M5.Display.setFont(&fonts::FreeSansBold18pt7b);
    M5.Display.setTextColor(C_GOLD);
    M5.Display.drawString("NEARBY FLIGHTS (" + String(planeCount) + ")", sw / 2, 206);

    int perPage = 6;
    int startIdx = flightListPage * perPage;
    for (int row = 0; row < perPage; row++) {
        int idx = startIdx + row;
        if (idx >= planeCount) break;
        AircraftItem &ac = planes[idx];
        int y = 246 + row * 124;
        bool sel = (idx == selectedPlane);

        M5.Display.fillRoundRect(38, y, sw - 76, 110, 14, sel ? C_BLUE : C_DARK);
        if (sel) M5.Display.drawRoundRect(38, y, sw - 76, 110, 14, C_GOLD);

        // Altitude color bar on left edge
        M5.Display.fillRoundRect(42, y + 8, 8, 94, 4, altitudeColor(ac));

        M5.Display.setTextDatum(middle_left);
        M5.Display.setFont(&fonts::FreeSansBold18pt7b);
        M5.Display.setTextColor(sel ? C_GOLD : TFT_WHITE);
        M5.Display.drawString(ac.callsign, 64, y + 36);

        M5.Display.setFont(&fonts::FreeSans9pt7b);
        M5.Display.setTextColor(C_CYAN);
        String sub = (ac.typeCode.length() ? ac.typeCode : String("----"));
        if (ac.reg.length()) sub += " | " + ac.reg;
        sub += " | " + String((int)round(ac.speedKt)) + " kt | " + String((int)round(ac.trackDeg)) + " deg " + degToCardinal(ac.trackDeg);
        M5.Display.drawString(sub, 64, y + 80);

        M5.Display.setTextDatum(middle_right);
        M5.Display.setFont(&fonts::FreeSansBold12pt7b);
        M5.Display.setTextColor(altitudeColor(ac));
        String altLbl = ac.onGround ? "GND" : (String(ac.altFt) + " ft");
        M5.Display.drawString(altLbl, sw - 58, y + 38);

        M5.Display.setTextColor(C_GOLD);
        M5.Display.drawString(String(ac.distNm, 1) + " nm", sw - 58, y + 78);
    }

    // Pagination Footer
    M5.Display.setTextDatum(middle_center);
    M5.Display.setFont(&fonts::FreeSansBold12pt7b);
    M5.Display.fillRoundRect(20, 1070, 220, 75, 14, C_PANEL);
    M5.Display.setTextColor(flightListPage > 0 ? TFT_WHITE : C_GRAY);
    M5.Display.drawString("< PREV", 130, 1107);

    M5.Display.fillRoundRect(250, 1070, 220, 75, 14, C_TEAL);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.drawString("REFRESH", 360, 1107);

    M5.Display.fillRoundRect(480, 1070, 220, 75, 14, C_PANEL);
    M5.Display.setTextColor((flightListPage + 1) * 6 < planeCount ? TFT_WHITE : C_GRAY);
    M5.Display.drawString("NEXT >", 590, 1107);
}

void initFlightRadar() {
    radarInitialized = true;
    refreshFlightRadar(true);
}

void drawFlightRadar() {
    M5.Display.fillScreen(C_DARK);
    M5.Display.setTextDatum(middle_center);

    // Top Bar
    M5.Display.fillRect(0, 0, sw, 90, 0x0862);
    M5.Display.fillRoundRect(16, 15, 130, 60, 14, C_PANEL);
    M5.Display.setFont(&fonts::FreeSansBold12pt7b);
    M5.Display.setTextColor(C_GOLD);
    M5.Display.drawString("< HUB", 81, 45);

    M5.Display.setFont(&fonts::FreeSansBold18pt7b);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.drawString("FLIGHT RADAR", sw / 2, 45);

    M5.Display.fillRoundRect(sw - 160, 15, 144, 60, 14, C_TEAL);
    M5.Display.setFont(&fonts::FreeSansBold12pt7b);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.drawString("REFRESH", sw - 88, 45);

    // Status & Altitude Legend Strip (y: 98..154)
    M5.Display.fillRoundRect(20, 98, sw - 40, 56, 12, C_PANEL);
    M5.Display.fillCircle(44, 126, 8, radarFetchOk ? C_GREEN : C_ORANGE);
    M5.Display.setTextDatum(middle_left);
    M5.Display.setFont(&fonts::FreeSans9pt7b);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.drawString(radarStatusMsg.substring(0, 38), 62, 126);

    // Compact Altitude Legend on right side of strip
    M5.Display.setTextDatum(middle_right);
    M5.Display.setTextColor(C_ORANGE); M5.Display.drawString("<3k",  530, 126);
    M5.Display.setTextColor(C_GREEN);  M5.Display.drawString("<15k", 588, 126);
    M5.Display.setTextColor(C_CYAN);   M5.Display.drawString("<30k", 646, 126);
    M5.Display.setTextColor(0xE3BF);   M5.Display.drawString("30k+", 692, 126);

    if (radarTab == 0) drawRadarScopeTab();
    else               drawFlightsListTab();

    // Bottom 2-Tab Navigation Bar
    M5.Display.setTextDatum(middle_center);
    M5.Display.setFont(&fonts::FreeSansBold18pt7b);
    M5.Display.fillRoundRect(20, 1172, 330, 84, 16, (radarTab == 0) ? C_GREEN : C_PANEL);
    M5.Display.setTextColor((radarTab == 0) ? TFT_WHITE : C_GRAY);
    M5.Display.drawString("RADAR SCOPE", 185, 1214);

    M5.Display.fillRoundRect(370, 1172, 330, 84, 16, (radarTab == 1) ? C_GREEN : C_PANEL);
    M5.Display.setTextColor((radarTab == 1) ? TFT_WHITE : C_GRAY);
    M5.Display.drawString("FLIGHTS (" + String(planeCount) + ")", 535, 1214);
}

void handleFlightRadarTouch(int tx, int ty) {
    // Top Bar
    if (ty < 90) {
        if (tx >= 16 && tx <= 146) {
            playTone(440, 60);
            currentState = STATE_MAIN_MENU;
            drawMainMenu();
            return;
        }
        if (tx >= sw - 160 && tx <= sw - 16) {
            playTone(660, 60);
            refreshFlightRadar(true);
            return;
        }
        return;
    }

    // Bottom Tabs
    if (ty >= 1170) {
        if (tx >= 20 && tx <= 350) { radarTab = 0; playTone(523, 40); drawFlightRadar(); return; }
        if (tx >= 370 && tx <= 700) { radarTab = 1; playTone(523, 40); drawFlightRadar(); return; }
        return;
    }

    // Tab 0: RADAR SCOPE
    if (radarTab == 0) {
        // Tap plane directly on radar scope
        if (tx >= 20 && tx <= 700 && ty >= 166 && ty <= 846 && planeCount > 0) {
            const int cx = 20 + 340, cy = 166 + 340;
            const float pxPerNm = (340.0f - 26.0f) / (float)RANGE_NM_LEVELS[rangeIdx];
            double cLat, cLon;
            bool usingGps;
            getRadarCenter(cLat, cLon, usingGps);

            int bestIdx = -1;
            float bestDist = 48.0f;
            for (int i = 0; i < planeCount; i++) {
                int px, py;
                latLonToRadarPx(planes[i].lat, planes[i].lon, cLat, cLon, cx, cy, pxPerNm, px, py);
                float d = hypotf((float)(tx - px), (float)(ty - py));
                if (d < bestDist) { bestDist = d; bestIdx = i; }
            }
            if (bestIdx >= 0) {
                selectedPlane = bestIdx;
                selectedHex = planes[bestIdx].hex;
                playTone(660, 40);
                drawFlightRadar();
            }
            return;
        }

        // Range & Trail Controls Row (y: 856..918)
        if (ty >= 856 && ty <= 918) {
            if (tx >= 20 && tx <= 175 && rangeIdx > 0) {
                rangeIdx--;
                playTone(660, 40);
                refreshFlightRadar(true);
                return;
            }
            if (tx >= 375 && tx <= 530 && rangeIdx < 3) {
                rangeIdx++;
                playTone(440, 40);
                refreshFlightRadar(true);
                return;
            }
            if (tx >= 540 && tx <= 700) {
                showTrails = !showTrails;
                playTone(587, 40);
                drawFlightRadar();
                return;
            }
        }

        // Prev / Next Plane Buttons on Telemetry Card
        if (planeCount > 0 && ty >= 1020 && ty <= 1132) {
            if (tx >= 536 && tx <= 608) {
                selectedPlane = (selectedPlane - 1 + planeCount) % planeCount;
                selectedHex = planes[selectedPlane].hex;
                playTone(523, 40);
                drawFlightRadar();
                return;
            }
            if (tx >= 616 && tx <= 688) {
                selectedPlane = (selectedPlane + 1) % planeCount;
                selectedHex = planes[selectedPlane].hex;
                playTone(660, 40);
                drawFlightRadar();
                return;
            }
        }
        return;
    }

    // Tab 1: FLIGHTS LIST
    if (radarTab == 1) {
        int startIdx = flightListPage * 6;
        for (int row = 0; row < 6; row++) {
            int idx = startIdx + row;
            if (idx >= planeCount) break;
            int y = 246 + row * 124;
            if (tx >= 38 && tx <= sw - 38 && ty >= y && ty <= y + 110) {
                selectedPlane = idx;
                selectedHex = planes[idx].hex;
                radarTab = 0; // Jump to Radar locked onto this flight
                playTone(660, 50);
                drawFlightRadar();
                return;
            }
        }
        if (ty >= 1070 && ty <= 1145) {
            if (tx >= 20 && tx <= 240 && flightListPage > 0) {
                flightListPage--;
                playTone(440, 40);
                drawFlightRadar();
                return;
            }
            if (tx >= 250 && tx <= 470) {
                playTone(660, 60);
                refreshFlightRadar(true);
                return;
            }
            if (tx >= 480 && tx <= 700 && (flightListPage + 1) * 6 < planeCount) {
                flightListPage++;
                playTone(660, 40);
                drawFlightRadar();
                return;
            }
        }
    }
}

void loopFlightRadar() {
    if (currentState != STATE_FLIGHT_RADAR || !radarInitialized) return;
    if (millis() - lastRadarFetchMs >= RADAR_REFRESH_MS) {
        refreshFlightRadar(false);
    }
}
