#include "Geocache.h"
#include "MeshtasticBLE.h"
#include "MapTiles.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <math.h>

// ======================== GITHUB GEOCACHE URLs ========================
// Upload geocaches.gpx (from c:geo or Geocaching.com) OR geocaches.json to your GitHub main branch.
// SYNC GH checks geocaches.gpx first and automatically falls back to geocaches.json.
const char* GITHUB_GPX_URL      = "https://raw.githubusercontent.com/KGFairbarn/M5Tab5/refs/heads/main/geocaches.gpx";
const char* GITHUB_GEOCACHE_URL = "https://raw.githubusercontent.com/KGFairbarn/M5Tab5/refs/heads/main/geocaches.json";

// ======================== DATA STRUCTURES =============================
struct GeocacheItem {
    String code;
    String name;
    String hint;
    double lat = 0.0;
    double lon = 0.0;
    float difficulty = 1.5f;
    float terrain = 1.5f;
    bool found = false;
};

static const int MAX_CACHES = 50;
static GeocacheItem caches[MAX_CACHES];
static int cacheCount = 0;
static int selectedCache = 0;
static int listPage = 0;
static int gcTab = 0; // 0 = MAP, 1 = COMPASS, 2 = LIST
static bool hintRevealed = false;
static bool cachesInitialized = false;
static String syncStatusMsg = "Tap UPLOAD (Wi-Fi) or SYNC GH to load caches.";

// Direct Wi-Fi GPX Upload Server State
static WebServer* gpxServer = nullptr;
static bool uploadServerActive = false;
static String uploadUrlStr = "";
static String uploadLogMsg = "Waiting for file from your browser...";
static bool uploadReceivedOk = false;

// Slippy Map & Offline SD Tile State
static int mapZoom = 15; // Web Mercator zoom 12..17
static MapLayer mapLayer = MAP_LAYER_SAT; // Default Esri Satellite (same as bizzyboat.live)
static bool mapFollowGps = true;
static bool mapDlOverlayActive = false;
static bool mapDlRunning = false;
static String mapDlStatusMsg = "Choose a layer & tap a download button below.";
static MapDownloadProgress mapDlProg;

// Breadcrumb Trail
static const int MAX_TRAIL = 60;
static double trailLat[MAX_TRAIL];
static double trailLon[MAX_TRAIL];
static int trailCount = 0;
static unsigned long lastUiRefreshMs = 0;

// ======================== GEO MATH HELPERS ============================
static double calcDistanceMeters(double lat1, double lon1, double lat2, double lon2) {
    const double R = 6371000.0;
    double dLat = (lat2 - lat1) * DEG_TO_RAD;
    double dLon = (lon2 - lon1) * DEG_TO_RAD;
    double a = sin(dLat / 2.0) * sin(dLat / 2.0) +
               cos(lat1 * DEG_TO_RAD) * cos(lat2 * DEG_TO_RAD) *
               sin(dLon / 2.0) * sin(dLon / 2.0);
    double c = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
    return R * c;
}

static double calcBearingDeg(double lat1, double lon1, double lat2, double lon2) {
    double y = sin((lon2 - lon1) * DEG_TO_RAD) * cos(lat2 * DEG_TO_RAD);
    double x = cos(lat1 * DEG_TO_RAD) * sin(lat2 * DEG_TO_RAD) -
               sin(lat1 * DEG_TO_RAD) * cos(lat2 * DEG_TO_RAD) * cos((lon2 - lon1) * DEG_TO_RAD);
    double brg = atan2(y, x) * RAD_TO_DEG;
    if (brg < 0) brg += 360.0;
    return brg;
}

static String bearingToCardinal(double deg) {
    const char* dirs[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    int idx = (int)floor((deg + 22.5) / 45.0) % 8;
    return String(dirs[idx]);
}

static String formatDistance(double meters) {
    double feet = meters * 3.28084;
    if (feet < 1000.0) return String((int)round(feet)) + " ft";
    double miles = meters / 1609.344;
    return String(miles, 2) + " mi";
}

// ======================== STORAGE & GITHUB SYNC =======================
static void loadFoundFlags() {
    Preferences p;
    p.begin("gc_found", true);
    uint64_t mask = p.getULong64("mask64", (uint64_t)p.getUInt("mask", 0));
    p.end();
    for (int i = 0; i < cacheCount && i < 64; i++) {
        caches[i].found = ((mask >> i) & 1ULL) != 0;
    }
}

static void saveFoundFlags() {
    uint64_t mask = 0;
    for (int i = 0; i < cacheCount && i < 64; i++) {
        if (caches[i].found) mask |= (1ULL << i);
    }
    Preferences p;
    p.begin("gc_found", false);
    p.putULong64("mask64", mask);
    p.end();
}

static void clearFoundFlags() {
    for (int i = 0; i < MAX_CACHES; i++) caches[i].found = false;
    Preferences p;
    p.begin("gc_found", false);
    p.putULong64("mask64", 0);
    p.putUInt("mask", 0);
    p.end();
}

static bool saveCachesJsonToNvs(const String &json) {
    Preferences p;
    if (!p.begin("gc_data", false)) return false;
    p.clear();
    const size_t CHUNK = 3000;
    int chunks = (int)((json.length() + CHUNK - 1) / CHUNK);
    p.putInt("chunks", chunks);
    bool ok = true;
    for (int i = 0; i < chunks; i++) {
        String key = "c" + String(i);
        size_t start = (size_t)i * CHUNK;
        size_t end = start + CHUNK;
        if (end > json.length()) end = json.length();
        String part = json.substring(start, end);
        if (p.putString(key.c_str(), part) == 0) {
            ok = false;
            break;
        }
    }
    p.end();
    return ok;
}

static bool parseGeocacheJson(const String &payload) {
    JsonDocument doc;
    if (deserializeJson(doc, payload)) return false;
    JsonArray arr = doc["caches"].as<JsonArray>();
    if (arr.isNull() || arr.size() == 0) return false;

    cacheCount = 0;
    for (JsonObject obj : arr) {
        if (cacheCount >= MAX_CACHES) break;
        GeocacheItem &c = caches[cacheCount];
        String fallbackCode = "GC" + String(1000 + cacheCount);
        c.code       = obj["code"].is<const char*>() ? String(obj["code"].as<const char*>()) : fallbackCode;
        c.name       = obj["name"] | "Unnamed Cache";
        c.hint       = obj["hint"] | "No hint provided.";
        if (c.name.length() > 44) c.name = c.name.substring(0, 41) + "...";
        if (c.hint.length() > 110) c.hint = c.hint.substring(0, 107) + "...";
        c.lat        = obj["lat"] | 0.0;
        c.lon        = obj["lon"] | 0.0;
        c.difficulty = obj["d"] | 1.5f;
        c.terrain    = obj["t"] | 1.5f;
        c.found      = false;
        if (c.lat != 0.0 || c.lon != 0.0) cacheCount++;
    }
    if (cacheCount > 0) {
        if (selectedCache >= cacheCount) selectedCache = 0;
        loadFoundFlags();
        return true;
    }
    return false;
}

static void loadDefaultCaches() {
    cacheCount = 3;
    caches[0] = {"GC1001", "Pine Trail Stump",     "Under mossy bark on north side", 43.1404, -70.9342, 1.5f, 2.0f, false};
    caches[1] = {"GC1002", "Stone Wall Crossing",  "Behind the flat gray capstone",  43.1362, -70.9398, 2.0f, 2.0f, false};
    caches[2] = {"GC1003", "Cedar Brook Overlook", "Hanging in cedar at eye level",  43.1425, -70.9415, 2.5f, 3.0f, false};
    loadFoundFlags();
}

static void loadSavedCaches() {
    Preferences p;
    p.begin("gc_data", true);
    int chunks = p.getInt("chunks", 0);
    String savedJson;
    if (chunks > 0 && chunks <= 16) {
        savedJson.reserve(chunks * 3000);
        for (int i = 0; i < chunks; i++) {
            String key = "c" + String(i);
            savedJson += p.getString(key.c_str(), "");
        }
    } else {
        savedJson = p.getString("json", "");
    }
    p.end();
    if (savedJson.length() > 10 && parseGeocacheJson(savedJson)) {
        syncStatusMsg = "Loaded " + String(cacheCount) + " cached waypoints from flash.";
    } else {
        loadDefaultCaches();
        syncStatusMsg = "Starter caches. Tap UPLOAD (Wi-Fi) or SYNC GH.";
    }
}

static String decodeXmlEntities(String s) {
    s.replace("&amp;", "&");
    s.replace("&quot;", "\"");
    s.replace("&apos;", "'");
    s.replace("&#39;", "'");
    s.replace("&lt;", "<");
    s.replace("&gt;", ">");
    s.replace("\r", " ");
    s.replace("\n", " ");
    s.trim();
    return s;
}

static String extractXmlAttr(const String &tag, const char* attr) {
    String key = String(attr) + "=\"";
    int pos = tag.indexOf(key);
    char quote = '"';
    if (pos < 0) {
        key = String(attr) + "='";
        pos = tag.indexOf(key);
        quote = '\'';
    }
    if (pos < 0) return "";
    int start = pos + key.length();
    int end = tag.indexOf(quote, start);
    if (end < 0) return "";
    return tag.substring(start, end);
}

static String extractXmlTag(const String &xml, const char* tagName) {
    String openTag = "<" + String(tagName);
    int start = xml.indexOf(openTag);
    if (start < 0) return "";
    int gt = xml.indexOf('>', start + openTag.length());
    if (gt < 0) return "";
    if (xml[gt - 1] == '/') return ""; // Self-closing empty tag
    String closeTag = "</" + String(tagName) + ">";
    int end = xml.indexOf(closeTag, gt + 1);
    if (end < 0) return "";
    String val = xml.substring(gt + 1, end);
    if (val.startsWith("<![CDATA[") && val.endsWith("]]>")) {
        val = val.substring(9, val.length() - 3);
    }
    return decodeXmlEntities(val);
}

static String serializeCachesToJson() {
    JsonDocument doc;
    JsonArray arr = doc["caches"].to<JsonArray>();
    for (int i = 0; i < cacheCount; i++) {
        JsonObject o = arr.add<JsonObject>();
        o["code"] = caches[i].code;
        o["name"] = caches[i].name;
        o["hint"] = caches[i].hint;
        o["lat"]  = caches[i].lat;
        o["lon"]  = caches[i].lon;
        o["d"]    = caches[i].difficulty;
        o["t"]    = caches[i].terrain;
    }
    String out;
    serializeJson(doc, out);
    return out;
}

static void parseGpxWaypointBlock(const String &wpt) {
    if (cacheCount >= MAX_CACHES) return;
    int headerEnd = wpt.indexOf('>');
    if (headerEnd < 0) return;
    String openTag = wpt.substring(0, headerEnd + 1);

    String latStr = extractXmlAttr(openTag, "lat");
    String lonStr = extractXmlAttr(openTag, "lon");
    if (latStr.length() == 0 || lonStr.length() == 0) return;

    // Skip child waypoints (e.g. parking / trailhead) if explicitly marked as Waypoint|
    String wptType = extractXmlTag(wpt, "type");
    if (wptType.startsWith("Waypoint|")) return;

    double lat = latStr.toDouble();
    double lon = lonStr.toDouble();
    if (lat == 0.0 && lon == 0.0) return;

    String code = extractXmlTag(wpt, "name");
    String title = extractXmlTag(wpt, "groundspeak:name");
    if (title.length() == 0) title = extractXmlTag(wpt, "urlname");
    if (title.length() == 0) title = extractXmlTag(wpt, "desc");
    if (title.length() == 0) title = code.length() ? code : String("Unnamed Cache");
    if (code.length() == 0) code = "GC" + String(1000 + cacheCount);

    String hint = extractXmlTag(wpt, "groundspeak:encoded_hints");
    if (hint.length() == 0) hint = extractXmlTag(wpt, "cmt");
    if (hint.length() == 0) hint = "No hint provided.";
    if (hint.length() > 140) hint = hint.substring(0, 137) + "...";

    String dStr = extractXmlTag(wpt, "groundspeak:difficulty");
    String tStr = extractXmlTag(wpt, "groundspeak:terrain");
    float diff = dStr.length() ? dStr.toFloat() : 1.5f;
    float terr = tStr.length() ? tStr.toFloat() : 1.5f;

    GeocacheItem &c = caches[cacheCount];
    c.code = code;
    c.name = title;
    c.hint = hint;
    c.lat = lat;
    c.lon = lon;
    c.difficulty = (diff > 0.0f) ? diff : 1.5f;
    c.terrain = (terr > 0.0f) ? terr : 1.5f;
    c.found = false;
    cacheCount++;
}

static bool streamParseGpx(HTTPClient &http) {
    WiFiClient* stream = http.getStreamPtr();
    if (!stream) return false;

    cacheCount = 0;
    String rxBuf;
    String wptBuf;
    rxBuf.reserve(4096);
    wptBuf.reserve(4096);
    bool inWpt = false;
    bool skipInner = false;
    String skipEndTag = "";
    unsigned long startMs = millis();
    unsigned long lastByteMs = millis();
    uint8_t chunk[512];

    auto tryStripBlock = [&](const char* openPrefix, const char* closeTag, int wEnd) -> bool {
        int pos = rxBuf.indexOf(openPrefix);
        if (pos < 0 || (wEnd >= 0 && pos >= wEnd)) return false;
        int gt = rxBuf.indexOf('>', pos);
        if (gt < 0) {
            // Wait for complete opening tag (flush safe prefix before pos)
            if (pos > 0 && wptBuf.length() < 8192) wptBuf += rxBuf.substring(0, pos);
            rxBuf.remove(0, pos);
            return false;
        }
        // Keep everything before this skipped block
        if (pos > 0 && wptBuf.length() < 8192) wptBuf += rxBuf.substring(0, pos);
        if (rxBuf[gt - 1] == '/') {
            // Self-closing tag (e.g. <groundspeak:logs/>)
            rxBuf.remove(0, gt + 1);
            return true;
        }
        int endPos = rxBuf.indexOf(closeTag, gt + 1);
        if (endPos >= 0) {
            rxBuf.remove(0, endPos + (int)strlen(closeTag));
        } else {
            rxBuf.remove(0, gt + 1);
            skipInner = true;
            skipEndTag = closeTag;
        }
        return true;
    };

    while (millis() - startMs < 30000UL && millis() - lastByteMs < 8000UL && cacheCount < MAX_CACHES) {
        lastActivityTime = millis();
        int avail = stream->available();
        if (avail <= 0) {
            if (!http.connected()) break;
            delay(2);
            continue;
        }
        int toRead = (avail > (int)sizeof(chunk)) ? (int)sizeof(chunk) : avail;
        int n = stream->readBytes(chunk, toRead);
        if (n <= 0) continue;
        lastByteMs = millis();

        rxBuf.concat((const char*)chunk, (unsigned int)n);

        bool progress = true;
        while (progress) {
            progress = false;
            if (!inWpt) {
                int wStart = rxBuf.indexOf("<wpt");
                if (wStart >= 0) {
                    rxBuf.remove(0, wStart);
                    wptBuf = "";
                    inWpt = true;
                    skipInner = false;
                    progress = true;
                } else if (rxBuf.length() > 16) {
                    rxBuf.remove(0, rxBuf.length() - 16);
                }
            } else if (skipInner) {
                int endPos = rxBuf.indexOf(skipEndTag);
                if (endPos >= 0) {
                    rxBuf.remove(0, endPos + skipEndTag.length());
                    skipInner = false;
                    progress = true;
                } else if (rxBuf.length() > 40) {
                    rxBuf.remove(0, rxBuf.length() - 40);
                }
            } else {
                int wEnd = rxBuf.indexOf("</wpt>");
                if (tryStripBlock("<groundspeak:logs", "</groundspeak:logs>", wEnd)) {
                    progress = true;
                    continue;
                }
                if (tryStripBlock("<groundspeak:long_description", "</groundspeak:long_description>", wEnd)) {
                    progress = true;
                    continue;
                }
                if (tryStripBlock("<groundspeak:short_description", "</groundspeak:short_description>", wEnd)) {
                    progress = true;
                    continue;
                }
                if (wEnd >= 0) {
                    wptBuf += rxBuf.substring(0, wEnd + 6);
                    rxBuf.remove(0, wEnd + 6);
                    parseGpxWaypointBlock(wptBuf);
                    wptBuf = "";
                    inWpt = false;
                    progress = true;
                } else if (rxBuf.length() > 256) {
                    // Flush safe prefix into wptBuf, keeping tail for split tag detection
                    size_t keep = 48;
                    size_t flushLen = rxBuf.length() - keep;
                    if (wptBuf.length() < 8192) wptBuf += rxBuf.substring(0, flushLen);
                    rxBuf.remove(0, flushLen);
                }
            }
        }
    }

    if (cacheCount > 0) {
        if (selectedCache >= cacheCount) selectedCache = 0;
        loadFoundFlags();
        return true;
    }
    return false;
}

static void syncCachesFromGitHub() {
    syncStatusMsg = "Connecting to GitHub...";
    drawGeocache();

    if (WiFi.status() != WL_CONNECTED) {
        connectToWiFi();
    }
    if (WiFi.status() != WL_CONNECTED) {
        syncStatusMsg = "Wi-Fi offline! Connect in Settings -> Wi-Fi.";
        playTone(220, 200);
        drawGeocache();
        return;
    }

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setTimeout(12000);

    // 1. Try geocaches.gpx first (direct GPX from c:geo or Geocaching.com)
    syncStatusMsg = "Checking GitHub for geocaches.gpx...";
    drawGeocache();
    if (http.begin(client, GITHUB_GPX_URL)) {
        int gpxCode = http.GET();
        if (gpxCode == HTTP_CODE_OK) {
            syncStatusMsg = "Converting geocaches.gpx on Tab5...";
            drawGeocache();
            bool parsed = streamParseGpx(http);
            http.end();
            if (parsed) {
                clearFoundFlags();
                String compactJson = serializeCachesToJson();
                saveCachesJsonToNvs(compactJson);
                selectedCache = 0;
                listPage = 0;
                syncStatusMsg = "Synced " + String(cacheCount) + " caches from geocaches.gpx!";
                playTone(880, 120);
                drawGeocache();
                return;
            }
            syncStatusMsg = "GPX parse failed; trying geocaches.json...";
            drawGeocache();
        } else {
            http.end();
        }
    }

    // 2. Fall back to geocaches.json
    syncStatusMsg = "Syncing geocaches.json from GitHub...";
    drawGeocache();
    if (!http.begin(client, GITHUB_GEOCACHE_URL)) {
        syncStatusMsg = "HTTPS init failed.";
        drawGeocache();
        return;
    }

    int code = http.GET();
    if (code == HTTP_CODE_OK) {
        String payload = http.getString();
        if (parseGeocacheJson(payload)) {
            clearFoundFlags();
            saveCachesJsonToNvs(serializeCachesToJson());
            selectedCache = 0;
            listPage = 0;
            syncStatusMsg = "Synced " + String(cacheCount) + " caches from geocaches.json!";
            playTone(880, 120);
        } else {
            syncStatusMsg = "JSON parse error in geocaches.json";
            playTone(220, 200);
        }
    } else {
        syncStatusMsg = "GitHub HTTP Error: " + String(code);
        playTone(220, 200);
    }
    http.end();
    drawGeocache();
}

// ======================== DIRECT WI-FI GPX UPLOAD =====================
static const char GPX_UPLOAD_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Tab5 Geocache Upload</title>
<style>
  *{box-sizing:border-box}
  body{margin:0;padding:20px;font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif;background:#0e1726;color:#f1f5f9;display:flex;justify-content:center}
  .card{width:100%;max-width:540px;background:#1b263b;border:1px solid #2e4057;border-radius:18px;padding:24px;box-shadow:0 12px 30px rgba(0,0,0,.4)}
  h1{margin:0 0 6px;font-size:22px;color:#ffd166}
  .sub{margin:0 0 18px;font-size:14px;color:#94a3b8}
  .drop{border:2px dashed #38bdf8;border-radius:14px;padding:34px 18px;text-align:center;cursor:pointer;background:#121d2f;transition:.15s}
  .drop.over{border-color:#22c55e;background:#132a24}
  .drop strong{display:block;font-size:17px;color:#fff;margin-bottom:6px}
  .drop span{font-size:13px;color:#94a3b8}
  input[type=file]{display:none}
  .btn{margin-top:14px;width:100%;padding:13px;border:none;border-radius:12px;font-size:16px;font-weight:700;cursor:pointer;background:#22c55e;color:#052e16}
  .btn:disabled{opacity:.45;cursor:not-allowed}
  .status{margin-top:16px;padding:12px 14px;border-radius:10px;background:#0f172a;font-size:14px;line-height:1.45;white-space:pre-wrap}
  .ok{color:#4ade80;border:1px solid #166534}
  .err{color:#f87171;border:1px solid #7f1d1d}
  .list{margin-top:14px;max-height:240px;overflow:auto;font-size:13px;background:#0f172a;border-radius:10px;padding:10px 12px;display:none}
  .row{padding:5px 0;border-bottom:1px solid #1e293b}
  .row:last-child{border-bottom:none}
  .code{color:#38bdf8;font-weight:700;margin-right:8px}
</style>
</head>
<body>
<div class="card">
  <h1>M5Tab5 Geocache Uploader</h1>
  <p class="sub">Drop a <b>.gpx</b>, Geocaching.com <b>.zip</b>, or <b>.json</b> file to send directly to your Tab5.</p>
  <label id="drop" class="drop">
    <strong>Drop GPX / ZIP / JSON file here</strong>
    <span>or tap to browse your computer or phone</span>
    <input id="file" type="file" accept=".gpx,.zip,.json,.xml,application/zip,application/json,text/xml">
  </label>
  <button id="sendBtn" class="btn" disabled>Send to Tab5</button>
  <div id="status" class="status">Ready. Export "Download GPX" from Geocaching.com and drop it above.</div>
  <div id="list" class="list"></div>
</div>
<script>
const MAX_CACHES = 50;
const drop = document.getElementById('drop');
const fileIn = document.getElementById('file');
const statusEl = document.getElementById('status');
const listEl = document.getElementById('list');
const sendBtn = document.getElementById('sendBtn');
let pendingCaches = null;
let pendingLabel = '';

function setStatus(msg, cls) {
  statusEl.textContent = msg;
  statusEl.className = 'status' + (cls ? ' ' + cls : '');
}

async function extractGpxFromZip(buf) {
  const dv = new DataView(buf);
  const u8 = new Uint8Array(buf);
  let eocd = -1;
  const minPos = Math.max(0, buf.byteLength - 65557);
  for (let i = buf.byteLength - 22; i >= minPos; i--) {
    if (dv.getUint32(i, true) === 0x06054b50) { eocd = i; break; }
  }
  if (eocd < 0) throw new Error('Invalid ZIP file');
  const cdCount = dv.getUint16(eocd + 10, true);
  let p = dv.getUint32(eocd + 16, true);
  const entries = [];
  for (let i = 0; i < cdCount; i++) {
    if (dv.getUint32(p, true) !== 0x02014b50) break;
    const method = dv.getUint16(p + 10, true);
    const compSize = dv.getUint32(p + 20, true);
    const nameLen = dv.getUint16(p + 28, true);
    const extraLen = dv.getUint16(p + 30, true);
    const commentLen = dv.getUint16(p + 32, true);
    const localOff = dv.getUint32(p + 42, true);
    const name = new TextDecoder().decode(u8.subarray(p + 46, p + 46 + nameLen));
    p += 46 + nameLen + extraLen + commentLen;
    if (name.toLowerCase().endsWith('.gpx') && !name.startsWith('__MACOSX/')) {
      entries.push({ name, method, compSize, localOff });
    }
  }
  if (!entries.length) throw new Error('No .gpx file found inside ZIP');
  const pick = entries.find(e => !e.name.toLowerCase().endsWith('-wpts.gpx')) || entries[0];
  const lName = dv.getUint16(pick.localOff + 26, true);
  const lExtra = dv.getUint16(pick.localOff + 28, true);
  const dataOff = pick.localOff + 30 + lName + lExtra;
  const comp = u8.subarray(dataOff, dataOff + pick.compSize);
  let text = '';
  if (pick.method === 0) {
    text = new TextDecoder().decode(comp);
  } else if (pick.method === 8) {
    if (typeof DecompressionStream === 'undefined') {
      throw new Error('Browser cannot unzip. Please unzip first and drop the .gpx file.');
    }
    const ds = new DecompressionStream('deflate-raw');
    text = await new Response(new Blob([comp]).stream().pipeThrough(ds)).text();
  } else {
    throw new Error('Unsupported ZIP compression method ' + pick.method);
  }
  return { text, name: pick.name };
}

function childText(el, localName) {
  if (!el) return '';
  for (const ch of el.children) {
    if (ch.localName === localName) {
      return (ch.textContent || '').replace(/\s+/g, ' ').trim();
    }
  }
  return '';
}

function parseGpx(text) {
  const xml = new DOMParser().parseFromString(text, 'text/xml');
  const wpts = Array.from(xml.getElementsByTagNameNS('*', 'wpt'));
  const out = [];
  for (const w of wpts) {
    const type = childText(w, 'type');
    if (type.startsWith('Waypoint|')) continue;
    const lat = parseFloat(w.getAttribute('lat'));
    const lon = parseFloat(w.getAttribute('lon'));
    if (!isFinite(lat) || !isFinite(lon) || (lat === 0 && lon === 0)) continue;

    let cacheEl = null;
    for (const ch of w.children) {
      if (ch.localName === 'cache') { cacheEl = ch; break; }
    }
    const code = childText(w, 'name') || ('GC' + (1000 + out.length));
    let name = childText(cacheEl, 'name') || childText(w, 'urlname') || childText(w, 'desc') || code;
    let hint = childText(cacheEl, 'encoded_hints') || childText(w, 'cmt') || 'No hint provided.';
    const d = parseFloat(childText(cacheEl, 'difficulty')) || 1.5;
    const t = parseFloat(childText(cacheEl, 'terrain')) || 1.5;
    if (name.length > 44) name = name.slice(0, 41) + '...';
    if (hint.length > 110) hint = hint.slice(0, 107) + '...';
    out.push({ code, name, lat: +lat.toFixed(6), lon: +lon.toFixed(6), d, t, hint });
  }
  return out;
}

async function handleFile(file) {
  if (!file) return;
  pendingCaches = null;
  sendBtn.disabled = true;
  listEl.style.display = 'none';
  setStatus('Reading ' + file.name + '...', '');
  try {
    const lower = file.name.toLowerCase();
    let caches = [];
    let label = file.name;
    if (lower.endsWith('.zip')) {
      const res = await extractGpxFromZip(await file.arrayBuffer());
      label = res.name;
      caches = parseGpx(res.text);
    } else if (lower.endsWith('.json')) {
      const obj = JSON.parse(await file.text());
      caches = Array.isArray(obj.caches) ? obj.caches : [];
    } else {
      caches = parseGpx(await file.text());
    }
    if (!caches.length) throw new Error('No geocaches found in ' + label);
    const total = caches.length;
    if (caches.length > MAX_CACHES) caches = caches.slice(0, MAX_CACHES);
    pendingCaches = caches;
    pendingLabel = label;

    listEl.innerHTML = caches.map(c =>
      '<div class="row"><span class="code">' + c.code + '</span>' +
      c.name.replace(/&/g,'&amp;').replace(/</g,'&lt;') + '</div>'
    ).join('');
    listEl.style.display = 'block';
    sendBtn.disabled = false;
    const note = total > MAX_CACHES ? (' (sending first ' + MAX_CACHES + ' of ' + total + ')') : '';
    setStatus('Parsed ' + caches.length + ' caches from ' + label + note + '. Uploading...', '');
    await uploadPending();
  } catch (err) {
    setStatus('Error: ' + err.message, 'err');
  }
}

async function uploadPending() {
  if (!pendingCaches) return;
  sendBtn.disabled = true;
  setStatus('Sending ' + pendingCaches.length + ' caches to Tab5...', '');
  try {
    const res = await fetch('/api/caches', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ source: pendingLabel, caches: pendingCaches })
    });
    const txt = await res.text();
    if (!res.ok) throw new Error(txt || ('HTTP ' + res.status));
    setStatus('Success! ' + txt + '\nYou can now tap STOP & VIEW CACHES on your Tab5.', 'ok');
  } catch (err) {
    setStatus('Upload failed: ' + err.message, 'err');
  } finally {
    sendBtn.disabled = false;
  }
}

fileIn.addEventListener('change', () => handleFile(fileIn.files[0]));
sendBtn.addEventListener('click', uploadPending);
drop.addEventListener('dragover', e => { e.preventDefault(); drop.classList.add('over'); });
drop.addEventListener('dragleave', () => drop.classList.remove('over'));
drop.addEventListener('drop', e => {
  e.preventDefault();
  drop.classList.remove('over');
  handleFile(e.dataTransfer.files[0]);
});
</script>
</body>
</html>)rawliteral";

static void stopUploadServer() {
    if (gpxServer) {
        gpxServer->stop();
        delete gpxServer;
        gpxServer = nullptr;
    }
    uploadServerActive = false;
}

static void startUploadServer() {
    uploadLogMsg = "Connecting to Wi-Fi...";
    uploadReceivedOk = false;
    drawGeocache();

    if (WiFi.status() != WL_CONNECTED) {
        connectToWiFi();
    }
    if (WiFi.status() != WL_CONNECTED) {
        syncStatusMsg = "Wi-Fi offline! Connect in Settings -> Wi-Fi.";
        playTone(220, 200);
        drawGeocache();
        return;
    }

    stopUploadServer();
    gpxServer = new WebServer(80);

    gpxServer->on("/", HTTP_GET, []() {
        gpxServer->send_P(200, "text/html", GPX_UPLOAD_HTML);
    });

    gpxServer->on("/api/caches", HTTP_POST, []() {
        String body = gpxServer->arg("plain");
        if (body.length() < 10) {
            gpxServer->send(400, "text/plain", "Empty payload");
            return;
        }
        String srcName = "Wi-Fi upload";
        {
            JsonDocument meta;
            if (!deserializeJson(meta, body) && meta["source"].is<const char*>()) {
                srcName = String(meta["source"].as<const char*>());
            }
        }
        if (parseGeocacheJson(body)) {
            clearFoundFlags();
            String compact = serializeCachesToJson();
            bool saved = saveCachesJsonToNvs(compact);
            selectedCache = 0;
            listPage = 0;
            hintRevealed = false;
            uploadReceivedOk = true;
            String msg = "Saved " + String(cacheCount) + " caches from " + srcName + (saved ? "!" : " (RAM only)");
            uploadLogMsg = msg;
            syncStatusMsg = msg;
            gpxServer->send(200, "text/plain", msg);
            playTone(880, 100);
            delay(50);
            playTone(1175, 140);
            drawGeocache();
        } else {
            uploadLogMsg = "Upload error: invalid cache data";
            gpxServer->send(400, "text/plain", "Could not parse geocaches");
            playTone(220, 180);
            drawGeocache();
        }
    });

    gpxServer->onNotFound([]() {
        gpxServer->sendHeader("Location", "/");
        gpxServer->send(302, "text/plain", "Redirecting...");
    });

    gpxServer->begin();
    uploadServerActive = true;
    uploadUrlStr = "http://" + WiFi.localIP().toString();
    uploadLogMsg = "Waiting for GPX / ZIP / JSON from browser...";
    syncStatusMsg = "Wi-Fi Upload ready at " + uploadUrlStr;
    playTone(784, 80);
    drawGeocache();
}

static void drawUploadOverlay() {
    M5.Display.fillRoundRect(20, 175, sw - 40, 845, 18, C_PANEL);
    M5.Display.drawRoundRect(20, 175, sw - 40, 845, 18, uploadReceivedOk ? C_GREEN : C_CYAN);

    M5.Display.setTextDatum(middle_center);
    M5.Display.setFont(&fonts::FreeSansBold18pt7b);
    M5.Display.setTextColor(C_GOLD);
    M5.Display.drawString("DIRECT WI-FI GPX UPLOAD", sw / 2, 232);

    // Status pill
    M5.Display.fillRoundRect(185, 275, 350, 44, 22, uploadReceivedOk ? C_GREEN : C_TEAL);
    M5.Display.setFont(&fonts::FreeSansBold12pt7b);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.drawString(uploadReceivedOk ? "UPLOAD COMPLETE!" : "WEB SERVER ACTIVE", sw / 2, 297);

    // URL Box
    M5.Display.fillRoundRect(46, 350, sw - 92, 130, 16, C_DARK);
    M5.Display.drawRoundRect(46, 350, sw - 92, 130, 16, C_GOLD);
    M5.Display.setFont(&fonts::FreeSans12pt7b);
    M5.Display.setTextColor(C_GRAY);
    M5.Display.drawString("Open this address in your PC or phone browser:", sw / 2, 388);
    M5.Display.setFont(&fonts::FreeSansBold24pt7b);
    M5.Display.setTextColor(C_GOLD);
    M5.Display.drawString(uploadUrlStr, sw / 2, 442);

    // Instructions
    M5.Display.setTextDatum(middle_left);
    M5.Display.setFont(&fonts::FreeSansBold12pt7b);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.drawString("1. On Geocaching.com, click Download GPX", 64, 540);
    M5.Display.drawString("2. Open the address above on the same Wi-Fi", 64, 595);
    M5.Display.drawString("3. Drop the .gpx, .zip, or .json file onto the page", 64, 650);

    M5.Display.setFont(&fonts::FreeSans9pt7b);
    M5.Display.setTextColor(C_CYAN);
    M5.Display.drawString("Works with Geocaching.com ZIPs directly (no unzip needed)!", 64, 705);
    M5.Display.drawString("Saves up to 50 caches to flash for offline navigation.", 64, 738);

    // Live Upload Status Box
    M5.Display.fillRoundRect(46, 790, sw - 92, 110, 14, C_DARK);
    M5.Display.setTextDatum(middle_center);
    M5.Display.setFont(&fonts::FreeSansBold12pt7b);
    M5.Display.setTextColor(uploadReceivedOk ? C_GREEN : C_CYAN);
    M5.Display.drawString(uploadLogMsg.substring(0, 46), sw / 2, 830);
    M5.Display.setFont(&fonts::FreeSans9pt7b);
    M5.Display.setTextColor(C_GRAY);
    M5.Display.drawString("Current caches loaded: " + String(cacheCount) + "  |  Screen stays awake", sw / 2, 874);

    // Stop / View Caches Button (y: 1045..1140)
    M5.Display.fillRoundRect(60, 1045, sw - 120, 95, 18, uploadReceivedOk ? C_GREEN : C_RED);
    M5.Display.setFont(&fonts::FreeSansBold18pt7b);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.drawString(uploadReceivedOk ? "DONE - VIEW CACHES" : "STOP UPLOAD SERVER", sw / 2, 1092);
}

// ======================== MAP & COMPASS DRAWING =======================
static void getMapCenter(double &cLat, double &cLon) {
    if (mapFollowGps && meshGps.valid) {
        cLat = meshGps.lat;
        cLon = meshGps.lon;
    } else if (cacheCount > 0) {
        cLat = caches[selectedCache].lat;
        cLon = caches[selectedCache].lon;
    } else if (meshGps.valid) {
        cLat = meshGps.lat;
        cLon = meshGps.lon;
    } else {
        cLat = 43.2189;
        cLon = -71.1069;
    }
}

static void latLonToMapPx(double lat, double lon, double cLat, double cLon,
                          int cx, int cy, int zoom, int &px, int &py) {
    double cTx = lonToTileX(cLon, zoom);
    double cTy = latToTileY(cLat, zoom);
    double pTx = lonToTileX(lon, zoom);
    double pTy = latToTileY(lat, zoom);
    px = cx + (int)round((pTx - cTx) * 256.0);
    py = cy + (int)round((pTy - cTy) * 256.0);
}

static void drawMapDlProgressBox() {
    M5.Display.fillRoundRect(46, 795, sw - 92, 130, 14, C_DARK);
    M5.Display.setTextDatum(middle_center);
    M5.Display.setFont(&fonts::FreeSansBold12pt7b);
    M5.Display.setTextColor(mapDlRunning ? C_GOLD : C_CYAN);
    M5.Display.drawString(mapDlStatusMsg.substring(0, 48), sw / 2, 830);

    // Progress Bar
    int bx = 76, by = 860, bw = sw - 152, bh = 24;
    M5.Display.fillRoundRect(bx, by, bw, bh, 8, C_PANEL);
    int pct = (mapDlProg.total > 0) ? (mapDlProg.done * 100 / mapDlProg.total) : 0;
    if (pct > 100) pct = 100;
    if (pct > 0) {
        int fw = (bw * pct) / 100;
        if (fw < 12) fw = 12;
        M5.Display.fillRoundRect(bx, by, fw, bh, 8, C_GREEN);
    }

    M5.Display.setFont(&fonts::FreeSans9pt7b);
    M5.Display.setTextColor(C_GRAY);
    if (mapDlProg.total > 0) {
        String det = "z" + String(mapDlProg.zoom) + " | " + String(mapDlProg.done) + "/" + String(mapDlProg.total) +
                     " | New:" + String(mapDlProg.downloaded) +
                     " | Cached:" + String(mapDlProg.skipped) +
                     " | Err:" + String(mapDlProg.errors);
        M5.Display.drawString(det, sw / 2, 904);
    } else {
        M5.Display.drawString("Tiles saved to /maps/" + String(getMapLayerShort(mapLayer)) + "/ on microSD", sw / 2, 904);
    }
}

static void drawMapDlOverlay() {
    M5.Display.fillRoundRect(20, 175, sw - 40, 845, 18, C_PANEL);
    M5.Display.drawRoundRect(20, 175, sw - 40, 845, 18, mapDlRunning ? C_GOLD : C_TEAL);

    M5.Display.setTextDatum(middle_center);
    M5.Display.setFont(&fonts::FreeSansBold18pt7b);
    M5.Display.setTextColor(C_GOLD);
    M5.Display.drawString("OFFLINE SD MAP TILES", sw / 2, 224);

    // SD Card Status Banner (tap to retry mount if missing)
    bool sdOk = isMapSdReady();
    M5.Display.fillRoundRect(46, 260, sw - 92, 54, 12, sdOk ? 0x0B44 : 0x6000);
    M5.Display.drawRoundRect(46, 260, sw - 92, 54, 12, sdOk ? C_GREEN : C_RED);
    M5.Display.setFont(&fonts::FreeSansBold12pt7b);
    M5.Display.setTextColor(TFT_WHITE);
    String sdLbl = sdOk ? getMapSdStatusText() : "No microSD detected - Insert FAT32 & tap here";
    M5.Display.drawString(sdLbl, sw / 2, 287);

    // Center Coordinates Summary
    double cLat, cLon;
    getMapCenter(cLat, cLon);
    M5.Display.setFont(&fonts::FreeSans9pt7b);
    M5.Display.setTextColor(C_CYAN);
    String ctrLbl = String((mapFollowGps && meshGps.valid) ? "Center: GPS (" : "Center: Selected Cache (") +
                    String(cLat, 4) + ", " + String(cLon, 4) + ")";
    M5.Display.drawString(ctrLbl, sw / 2, 342);

    // Layer Selector Row (y: 375..445)
    const char* lNames[3] = {"SAT (Esri)", "TOPO (Esri)", "OSM"};
    for (int i = 0; i < 3; i++) {
        int lx = 46 + i * 212;
        bool active = ((int)mapLayer == i);
        M5.Display.fillRoundRect(lx, 375, 200, 70, 14, active ? C_GREEN : C_DARK);
        M5.Display.drawRoundRect(lx, 375, 200, 70, 14, active ? C_GOLD : C_GRAY);
        M5.Display.setFont(&fonts::FreeSansBold12pt7b);
        M5.Display.setTextColor(TFT_WHITE);
        M5.Display.drawString(lNames[i], lx + 100, 410);
    }

    // Download Preset Buttons
    M5.Display.fillRoundRect(46, 470, sw - 92, 86, 16, mapDlRunning ? 0x3186 : C_TEAL);
    M5.Display.setFont(&fonts::FreeSansBold18pt7b);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.drawString("DL AREA PACK (z13 - z16)", sw / 2, 502);
    M5.Display.setFont(&fonts::FreeSans9pt7b);
    M5.Display.setTextColor(C_GOLD);
    M5.Display.drawString("~68 tiles around center (~1 MB) - best all-around offline pack", sw / 2, 536);

    M5.Display.fillRoundRect(46, 574, sw - 92, 86, 16, mapDlRunning ? 0x3186 : C_BLUE);
    M5.Display.setFont(&fonts::FreeSansBold18pt7b);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.drawString("DL ALL CACHES (z13 - z15)", sw / 2, 606);
    M5.Display.setFont(&fonts::FreeSans9pt7b);
    M5.Display.setTextColor(C_GOLD);
    M5.Display.drawString("Covers bounding box of all " + String(cacheCount) + " loaded caches (max 180 tiles)", sw / 2, 640);

    M5.Display.fillRoundRect(46, 678, sw - 92, 86, 16, mapDlRunning ? 0x3186 : 0x528A);
    M5.Display.setFont(&fonts::FreeSansBold18pt7b);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.drawString("DL CLOSE-UP (z17 DETAIL)", sw / 2, 710);
    M5.Display.setFont(&fonts::FreeSans9pt7b);
    M5.Display.setTextColor(C_GOLD);
    M5.Display.drawString("25 ultra-detailed tiles right at center (~0.5 MB)", sw / 2, 744);

    // Live Progress Box
    drawMapDlProgressBox();

    // Bottom Action Button (y: 1045..1140)
    M5.Display.fillRoundRect(60, 1045, sw - 120, 95, 18, mapDlRunning ? C_RED : C_GREEN);
    M5.Display.setFont(&fonts::FreeSansBold18pt7b);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.drawString(mapDlRunning ? "CANCEL DOWNLOAD" : "BACK TO MAP", sw / 2, 1092);
}

static bool mapDownloadProgressCallback(const MapDownloadProgress &p) {
    mapDlProg = p;
    mapDlStatusMsg = String("Downloading ") + getMapLayerShort(mapLayer) + " z" + String(p.zoom) + "...";
    drawMapDlProgressBox();

    // Check touch for CANCEL button (y: 1045..1140)
    M5.update();
    auto t = M5.Touch.getDetail();
    if (t.wasPressed() && t.x >= 60 && t.x <= sw - 60 && t.y >= 1045 && t.y <= 1140) {
        mapDlStatusMsg = "Cancelling download...";
        drawMapDlProgressBox();
        return false;
    }
    return true;
}

static void startMapDownload(int mode) {
    if (mapDlRunning) return;
    if (!initMapSdCard(true)) {
        mapDlStatusMsg = "microSD not found! Insert FAT32 card first.";
        playTone(220, 200);
        drawGeocache();
        return;
    }

    mapDlRunning = true;
    mapDlProg = MapDownloadProgress{};
    mapDlStatusMsg = "Connecting to Wi-Fi...";
    drawGeocache();

    if (WiFi.status() != WL_CONNECTED) {
        connectToWiFi();
    }
    if (WiFi.status() != WL_CONNECTED) {
        mapDlRunning = false;
        mapDlStatusMsg = "Wi-Fi offline! Connect in Settings -> Wi-Fi.";
        playTone(220, 200);
        drawGeocache();
        return;
    }

    double cLat, cLon;
    getMapCenter(cLat, cLon);
    MapDownloadProgress stats;
    bool ok = false;

    if (mode == 0) {
        ok = downloadMapAreaToSd(cLat, cLon, 13, 16, mapLayer, mapDownloadProgressCallback, stats);
    } else if (mode == 1) {
        if (cacheCount > 0) {
            double minLat = caches[0].lat, maxLat = caches[0].lat;
            double minLon = caches[0].lon, maxLon = caches[0].lon;
            for (int i = 1; i < cacheCount; i++) {
                if (caches[i].lat < minLat) minLat = caches[i].lat;
                if (caches[i].lat > maxLat) maxLat = caches[i].lat;
                if (caches[i].lon < minLon) minLon = caches[i].lon;
                if (caches[i].lon > maxLon) maxLon = caches[i].lon;
            }
            ok = downloadMapBoundsToSd(minLat, minLon, maxLat, maxLon, 13, 15, mapLayer, mapDownloadProgressCallback, stats);
        } else {
            ok = downloadMapAreaToSd(cLat, cLon, 13, 15, mapLayer, mapDownloadProgressCallback, stats);
        }
    } else {
        ok = downloadMapAreaToSd(cLat, cLon, 17, 17, mapLayer, mapDownloadProgressCallback, stats);
    }

    mapDlProg = stats;
    mapDlRunning = false;
    if (!ok) {
        mapDlStatusMsg = "Download failed (check Wi-Fi & SD card).";
        playTone(220, 200);
    } else if (stats.cancelled) {
        mapDlStatusMsg = "Cancelled (" + String(stats.downloaded) + " saved to SD).";
        playTone(440, 100);
    } else {
        mapDlStatusMsg = "Done! " + String(stats.downloaded) + " new, " + String(stats.skipped) + " cached on SD.";
        playTone(880, 100);
        delay(50);
        playTone(1175, 140);
    }
    drawGeocache();
}

static void drawMapTab() {
    const int mapX = 20, mapY = 175, mapS = 680;
    const int cx = mapX + mapS / 2;
    const int cy = mapY + mapS / 2;

    double cLat, cLon;
    getMapCenter(cLat, cLon);

    // Dark Topo Radar Background + Offline SD Map Tiles
    M5.Display.fillRoundRect(mapX, mapY, mapS, mapS, 18, 0x0882);
    int tilesDrawn = drawOfflineMapTiles(mapX, mapY, mapS, mapS, cLat, cLon, mapZoom, mapLayer);

    if (tilesDrawn == 0) {
        // Fallback Grid Crosshairs & Range Rings when no tiles cached yet
        M5.Display.drawFastHLine(mapX + 10, cy, mapS - 20, 0x1945);
        M5.Display.drawFastVLine(cx, mapY + 10, mapS - 20, 0x1945);
        for (int r = 1; r <= 3; r++) {
            int rr = (int)((mapS / 2 - 28) * r / 3.0f);
            M5.Display.drawCircle(cx, cy, rr, 0x1A06);
        }
        M5.Display.setTextDatum(middle_center);
        M5.Display.setFont(&fonts::FreeSans9pt7b);
        M5.Display.setTextColor(C_GRAY);
        String hint = isMapSdReady() ? ("No " + String(getMapLayerShort(mapLayer)) + " z" + String(mapZoom) + " tiles on SD - tap DL MAP")
                                     : "Insert microSD & tap DL MAP for offline tiles";
        M5.Display.drawString(hint, cx, mapY + mapS - 46);
    }
    M5.Display.drawRoundRect(mapX, mapY, mapS, mapS, 18, C_TEAL);

    // Cardinal Labels (with dark backing pill so visible over satellite tiles)
    M5.Display.setTextDatum(middle_center);
    M5.Display.setFont(&fonts::FreeSansBold12pt7b);
    M5.Display.fillRoundRect(cx - 18, mapY + 6, 36, 26, 8, C_DARK);
    M5.Display.setTextColor(C_RED);  M5.Display.drawString("N", cx, mapY + 19);

    // Draw Walked Breadcrumb Trail
    if (trailCount > 1) {
        int prevX = -1, prevY = -1;
        for (int i = 0; i < trailCount; i++) {
            int px, py;
            latLonToMapPx(trailLat[i], trailLon[i], cLat, cLon, cx, cy, mapZoom, px, py);
            if (px > mapX + 10 && px < mapX + mapS - 10 && py > mapY + 10 && py < mapY + mapS - 10) {
                if (prevX >= 0) {
                    M5.Display.drawLine(prevX, prevY, px, py, C_CYAN);
                    M5.Display.drawLine(prevX + 1, prevY, px + 1, py, C_CYAN);
                }
                prevX = px; prevY = py;
            } else {
                prevX = -1;
            }
        }
    }

    // Draw Nearby Meshtastic Nodes (Cyan dots with dark halo)
    for (int i = 0; i < meshNodeCount; i++) {
        if (!meshNodes[i].hasPos) continue;
        int px, py;
        latLonToMapPx(meshNodes[i].lat, meshNodes[i].lon, cLat, cLon, cx, cy, mapZoom, px, py);
        if (px > mapX + 18 && px < mapX + mapS - 18 && py > mapY + 18 && py < mapY + mapS - 18) {
            M5.Display.fillCircle(px, py, 9, TFT_BLACK);
            M5.Display.fillCircle(px, py, 7, C_CYAN);
            M5.Display.setFont(&fonts::FreeSans9pt7b);
            M5.Display.setTextColor(C_CYAN);
            M5.Display.drawString(meshNodes[i].shortName, px, py - 18);
        }
    }

    // Compute User GPS pixel position (if valid)
    int userPx = cx, userPy = cy;
    bool userInView = false;
    if (meshGps.valid) {
        latLonToMapPx(meshGps.lat, meshGps.lon, cLat, cLon, cx, cy, mapZoom, userPx, userPy);
        userInView = (userPx > mapX + 14 && userPx < mapX + mapS - 14 && userPy > mapY + 14 && userPy < mapY + mapS - 14);
    }

    // Draw Bearing Line from User GPS to Selected Cache
    if (cacheCount > 0 && userInView) {
        GeocacheItem &tgt = caches[selectedCache];
        int tx, ty;
        latLonToMapPx(tgt.lat, tgt.lon, cLat, cLon, cx, cy, mapZoom, tx, ty);
        tx = constrain(tx, mapX + 16, mapX + mapS - 16);
        ty = constrain(ty, mapY + 16, mapY + mapS - 16);
        M5.Display.drawLine(userPx, userPy, tx, ty, TFT_BLACK);
        M5.Display.drawLine(userPx + 1, userPy, tx + 1, ty, C_GOLD);
        M5.Display.drawLine(userPx, userPy + 1, tx, ty + 1, C_GOLD);
    }

    // Draw Geocache Pins (with dark outline + label pill for readability over satellite imagery)
    for (int i = 0; i < cacheCount; i++) {
        int px, py;
        latLonToMapPx(caches[i].lat, caches[i].lon, cLat, cLon, cx, cy, mapZoom, px, py);
        if (px < mapX + 20 || px > mapX + mapS - 20 || py < mapY + 20 || py > mapY + mapS - 34) continue;

        uint16_t pinCol = caches[i].found ? C_GRAY : C_GREEN;
        if (i == selectedCache) {
            M5.Display.drawCircle(px, py, 20, TFT_BLACK);
            M5.Display.drawCircle(px, py, 19, C_GOLD);
            M5.Display.drawCircle(px, py, 18, C_GOLD);
            pinCol = C_GOLD;
        }
        M5.Display.fillCircle(px, py, 14, TFT_BLACK);
        M5.Display.fillCircle(px, py, 11, pinCol);
        M5.Display.setFont(&fonts::FreeSans9pt7b);
        int lw = caches[i].code.length() * 10 + 10;
        M5.Display.fillRoundRect(px - lw / 2, py + 15, lw, 20, 6, C_DARK);
        M5.Display.setTextColor(i == selectedCache ? C_GOLD : TFT_WHITE);
        M5.Display.drawString(caches[i].code, px, py + 25);
    }

    // Draw User Position Marker
    if (userInView) {
        M5.Display.fillCircle(userPx, userPy, 14, TFT_BLACK);
        M5.Display.fillCircle(userPx, userPy, 12, TFT_WHITE);
        M5.Display.fillCircle(userPx, userPy, 9, C_BLUE);
    }

    // Map Controls Row (y: 868..934)
    M5.Display.setTextDatum(middle_center);
    M5.Display.setFont(&fonts::FreeSansBold18pt7b);
    M5.Display.fillRoundRect(20, 868, 92, 66, 14, C_PANEL);
    M5.Display.setTextColor(mapZoom > 12 ? TFT_WHITE : C_GRAY);
    M5.Display.drawString("-", 66, 901);

    M5.Display.fillRoundRect(120, 868, 120, 66, 14, C_PANEL);
    M5.Display.setFont(&fonts::FreeSansBold12pt7b);
    M5.Display.setTextColor(C_CYAN);
    String ctrTag = (mapFollowGps && meshGps.valid) ? "GPS" : "TGT";
    M5.Display.drawString("z" + String(mapZoom) + " " + ctrTag, 180, 901);

    M5.Display.fillRoundRect(248, 868, 92, 66, 14, C_PANEL);
    M5.Display.setFont(&fonts::FreeSansBold18pt7b);
    M5.Display.setTextColor(mapZoom < 17 ? TFT_WHITE : C_GRAY);
    M5.Display.drawString("+", 294, 901);

    M5.Display.fillRoundRect(352, 868, 162, 66, 14, C_BLUE);
    M5.Display.setFont(&fonts::FreeSansBold12pt7b);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.drawString(getMapLayerShort(mapLayer), 433, 901);

    M5.Display.fillRoundRect(526, 868, 174, 66, 14, C_TEAL);
    M5.Display.drawString("DL MAP", 613, 901);

    // Selected Cache Summary Card (y: 948..1148)
    M5.Display.fillRoundRect(20, 948, sw - 40, 200, 18, C_PANEL);
    if (cacheCount > 0) {
        GeocacheItem &c = caches[selectedCache];
        double refLat = meshGps.valid ? meshGps.lat : cLat;
        double refLon = meshGps.valid ? meshGps.lon : cLon;
        double distM = calcDistanceMeters(refLat, refLon, c.lat, c.lon);
        double brg   = calcBearingDeg(refLat, refLon, c.lat, c.lon);

        M5.Display.setTextDatum(middle_left);
        M5.Display.setFont(&fonts::FreeSansBold18pt7b);
        M5.Display.setTextColor(C_GOLD);
        M5.Display.drawString(c.name.substring(0, 22), 42, 990);

        M5.Display.setFont(&fonts::FreeSansBold12pt7b);
        M5.Display.setTextColor(TFT_WHITE);
        String stat = c.code + "  |  " + formatDistance(distM) + "  |  " + String((int)round(brg)) + " deg " + bearingToCardinal(brg);
        M5.Display.drawString(stat, 42, 1042);

        M5.Display.setFont(&fonts::FreeSans9pt7b);
        M5.Display.setTextColor(C_CYAN);
        M5.Display.drawString("D " + String(c.difficulty, 1) + " / T " + String(c.terrain, 1) + (c.found ? "  [FOUND]" : ""), 42, 1086);

        M5.Display.setTextDatum(middle_center);
        M5.Display.fillRoundRect(510, 1055, 172, 72, 14, C_GREEN);
        M5.Display.setFont(&fonts::FreeSansBold12pt7b);
        M5.Display.setTextColor(TFT_WHITE);
        M5.Display.drawString("NAVIGATE >", 596, 1091);
    }
}

static void drawCompassTab() {
    if (cacheCount == 0) return;
    GeocacheItem &c = caches[selectedCache];
    double distM = calcDistanceMeters(meshGps.lat, meshGps.lon, c.lat, c.lon);
    double targetBrg = calcBearingDeg(meshGps.lat, meshGps.lon, c.lat, c.lon);

    // If moving (>0.8 m/s), rotate dial relative to travel course; otherwise North-Up
    bool courseUp = (meshGps.valid && !meshGps.simulated && meshGps.speedMps > 0.8f);
    double arrowDeg = courseUp ? (targetBrg - meshGps.courseDeg) : targetBrg;
    while (arrowDeg < 0) arrowDeg += 360.0;
    while (arrowDeg >= 360.0) arrowDeg -= 360.0;

    // Target Header Card
    M5.Display.fillRoundRect(20, 175, sw - 40, 105, 16, C_PANEL);
    M5.Display.setTextDatum(middle_center);
    M5.Display.setFont(&fonts::FreeSansBold18pt7b);
    M5.Display.setTextColor(C_GOLD);
    M5.Display.drawString(c.name.substring(0, 26), sw / 2, 210);
    M5.Display.setFont(&fonts::FreeSans12pt7b);
    M5.Display.setTextColor(C_CYAN);
    M5.Display.drawString(c.code + " (" + String(c.lat, 5) + ", " + String(c.lon, 5) + ")", sw / 2, 252);

    // Compass Dial
    const int cx = sw / 2, cy = 530, r = 215;
    M5.Display.fillCircle(cx, cy, r, C_PANEL);
    M5.Display.drawCircle(cx, cy, r, C_TEAL);
    M5.Display.drawCircle(cx, cy, r - 3, C_TEAL);
    M5.Display.drawCircle(cx, cy, r - 38, 0x2988);

    // Degree Ticks & N/E/S/W
    double dialOffset = courseUp ? -meshGps.courseDeg : 0.0;
    for (int d = 0; d < 360; d += 15) {
        double rad = (d + dialOffset - 90.0) * DEG_TO_RAD;
        int rIn = (d % 90 == 0) ? (r - 28) : (r - 14);
        M5.Display.drawLine(cx + cos(rad) * rIn, cy + sin(rad) * rIn,
                            cx + cos(rad) * (r - 4), cy + sin(rad) * (r - 4),
                            (d == 0) ? C_RED : C_GRAY);
    }

    const char* card[4] = {"N", "E", "S", "W"};
    M5.Display.setFont(&fonts::FreeSansBold18pt7b);
    for (int i = 0; i < 4; i++) {
        double rad = (i * 90.0 + dialOffset - 90.0) * DEG_TO_RAD;
        M5.Display.setTextColor(i == 0 ? C_RED : C_GRAY);
        M5.Display.drawString(card[i], cx + cos(rad) * (r - 56), cy + sin(rad) * (r - 56));
    }

    // Large Navigation Arrow pointing to Cache
    double aRad = (arrowDeg - 90.0) * DEG_TO_RAD;
    int tipX = cx + (int)(cos(aRad) * (r - 42));
    int tipY = cy + (int)(sin(aRad) * (r - 42));
    int leftX = cx + (int)(cos(aRad + 2.45) * 85);
    int leftY = cy + (int)(sin(aRad + 2.45) * 85);
    int rightX = cx + (int)(cos(aRad - 2.45) * 85);
    int rightY = cy + (int)(sin(aRad - 2.45) * 85);
    int tailX = cx - (int)(cos(aRad) * 35);
    int tailY = cy - (int)(sin(aRad) * 35);

    uint16_t arrowCol = (distM < 15.0) ? C_GOLD : C_GREEN;
    M5.Display.fillTriangle(tipX, tipY, leftX, leftY, tailX, tailY, arrowCol);
    M5.Display.fillTriangle(tipX, tipY, rightX, rightY, tailX, tailY, arrowCol);
    M5.Display.fillCircle(cx, cy, 14, TFT_WHITE);

    // Distance & Bearing Readout
    M5.Display.setFont(&fonts::FreeSansBold24pt7b);
    M5.Display.setTextColor(arrowCol);
    M5.Display.drawString(formatDistance(distM), sw / 2, 795);

    M5.Display.setFont(&fonts::FreeSansBold12pt7b);
    M5.Display.setTextColor(TFT_WHITE);
    String modeStr = courseUp ? "COURSE-UP (Moving)" : "NORTH-UP (Face North)";
    M5.Display.drawString("Bearing: " + String((int)round(targetBrg)) + " deg " + bearingToCardinal(targetBrg) + "  |  " + modeStr, sw / 2, 855);

    // Hint Box (Tap to Reveal)
    M5.Display.fillRoundRect(20, 895, sw - 40, 125, 16, C_PANEL);
    M5.Display.setFont(&fonts::FreeSansBold12pt7b);
    M5.Display.setTextColor(C_GOLD);
    M5.Display.drawString(hintRevealed ? "HINT:" : "HINT (TAP TO REVEAL)", sw / 2, 930);
    if (hintRevealed) {
        M5.Display.setFont(&fonts::FreeSans12pt7b);
        M5.Display.setTextColor(TFT_WHITE);
        M5.Display.drawString(c.hint.substring(0, 44), sw / 2, 982);
    }

    // Action Buttons
    M5.Display.fillRoundRect(20, 1045, 330, 95, 18, c.found ? 0x528A : C_GREEN);
    M5.Display.setFont(&fonts::FreeSansBold18pt7b);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.drawString(c.found ? "FOUND!" : "MARK FOUND", 185, 1092);

    M5.Display.fillRoundRect(370, 1045, 330, 95, 18, C_BLUE);
    M5.Display.drawString("NEXT CACHE >", 535, 1092);
}

static void drawListTab() {
    M5.Display.fillRoundRect(20, 175, sw - 40, 880, 16, C_PANEL);
    M5.Display.setTextDatum(middle_center);
    M5.Display.setFont(&fonts::FreeSansBold18pt7b);
    M5.Display.setTextColor(C_GOLD);
    M5.Display.drawString("GEOCACHES (" + String(cacheCount) + ")", sw / 2, 215);

    int perPage = 6;
    int startIdx = listPage * perPage;
    for (int row = 0; row < perPage; row++) {
        int idx = startIdx + row;
        if (idx >= cacheCount) break;
        int y = 255 + row * 124;
        bool sel = (idx == selectedCache);
        M5.Display.fillRoundRect(38, y, sw - 76, 110, 14, sel ? C_BLUE : C_DARK);
        if (sel) M5.Display.drawRoundRect(38, y, sw - 76, 110, 14, C_GOLD);

        double distM = calcDistanceMeters(meshGps.lat, meshGps.lon, caches[idx].lat, caches[idx].lon);
        double brg   = calcBearingDeg(meshGps.lat, meshGps.lon, caches[idx].lat, caches[idx].lon);

        M5.Display.setTextDatum(middle_left);
        M5.Display.setFont(&fonts::FreeSansBold12pt7b);
        M5.Display.setTextColor(caches[idx].found ? C_GRAY : TFT_WHITE);
        M5.Display.drawString(caches[idx].name.substring(0, 26), 58, y + 36);

        M5.Display.setFont(&fonts::FreeSans9pt7b);
        M5.Display.setTextColor(C_CYAN);
        String sub = caches[idx].code + " | D" + String(caches[idx].difficulty, 1) + "/T" + String(caches[idx].terrain, 1) +
                     (caches[idx].found ? " | FOUND" : "");
        M5.Display.drawString(sub, 58, y + 78);

        M5.Display.setTextDatum(middle_right);
        M5.Display.setFont(&fonts::FreeSansBold12pt7b);
        M5.Display.setTextColor(C_GOLD);
        M5.Display.drawString(formatDistance(distM) + " " + bearingToCardinal(brg), sw - 58, y + 55);
    }

    // Page & Sync Status Footer
    M5.Display.setTextDatum(middle_center);
    M5.Display.setFont(&fonts::FreeSans9pt7b);
    M5.Display.setTextColor(C_CYAN);
    M5.Display.drawString(syncStatusMsg.substring(0, 56), sw / 2, 1025);

    M5.Display.fillRoundRect(20, 1070, 220, 75, 14, C_PANEL);
    M5.Display.setFont(&fonts::FreeSansBold12pt7b);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.drawString("< PREV", 130, 1107);

    M5.Display.fillRoundRect(250, 1070, 220, 75, 14, C_TEAL);
    M5.Display.drawString("SYNC GITHUB", 360, 1107);

    M5.Display.fillRoundRect(480, 1070, 220, 75, 14, C_PANEL);
    M5.Display.drawString("NEXT >", 590, 1107);
}

void initGeocache() {
    if (!cachesInitialized) {
        loadSavedCaches();
        cachesInitialized = true;
    }
    initMapSdCard(false);
    drawGeocache();
}

void drawGeocache() {
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
    M5.Display.drawString("GEOCACHE", 276, 45);

    M5.Display.fillRoundRect(406, 15, 144, 60, 14, uploadServerActive ? C_GREEN : C_BLUE);
    M5.Display.setFont(&fonts::FreeSansBold12pt7b);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.drawString(uploadServerActive ? "WEB ON" : "UPLOAD", 478, 45);

    M5.Display.fillRoundRect(560, 15, 144, 60, 14, C_TEAL);
    M5.Display.drawString("SYNC GH", 632, 45);

    // GPS Status Strip (y: 100..160)
    M5.Display.fillRoundRect(20, 100, 555, 60, 12, C_PANEL);
    M5.Display.fillCircle(44, 130, 8, meshGps.valid ? (meshGps.simulated ? C_ORANGE : C_GREEN) : C_RED);
    M5.Display.setTextDatum(middle_left);
    M5.Display.setFont(&fonts::FreeSans9pt7b);
    M5.Display.setTextColor(TFT_WHITE);
    if (meshGps.valid) {
        String gStr = String(meshGps.simulated ? "SIM GPS: " : "T114 GPS: ") +
                      String(meshGps.lat, 5) + ", " + String(meshGps.lon, 5) +
                      " | Sats: " + String(meshGps.sats);
        M5.Display.drawString(gStr, 64, 130);
    } else {
        M5.Display.drawString("No T114 GPS Fix (Connect on Meshtastic page or tap SIM)", 64, 130);
    }

    // SIM GPS Toggle Button
    M5.Display.setTextDatum(middle_center);
    M5.Display.fillRoundRect(588, 100, 112, 60, 12, meshGps.simulated ? C_ORANGE : C_PANEL);
    M5.Display.drawRoundRect(588, 100, 112, 60, 12, C_ORANGE);
    M5.Display.setFont(&fonts::FreeSansBold12pt7b);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.drawString("SIM", 644, 130);

    // Draw Active Tab (or Overlay)
    if (uploadServerActive)      drawUploadOverlay();
    else if (mapDlOverlayActive) drawMapDlOverlay();
    else if (gcTab == 0)         drawMapTab();
    else if (gcTab == 1)         drawCompassTab();
    else                         drawListTab();

    // Bottom 3-Tab Navigation Bar
    const char* tabs[3] = {"MAP", "COMPASS", "CACHES"};
    for (int i = 0; i < 3; i++) {
        int bx = 20 + i * 232;
        bool active = (!uploadServerActive && !mapDlOverlayActive && gcTab == i);
        M5.Display.fillRoundRect(bx, 1172, 216, 84, 16, active ? C_GREEN : C_PANEL);
        M5.Display.setFont(&fonts::FreeSansBold18pt7b);
        M5.Display.setTextColor(active ? TFT_WHITE : C_GRAY);
        M5.Display.drawString(tabs[i], bx + 108, 1214);
    }
}

void handleGeocacheTouch(int tx, int ty) {
    // Top Bar
    if (ty < 90) {
        if (tx >= 16 && tx <= 146) {
            stopUploadServer();
            mapDlOverlayActive = false;
            playTone(440, 60);
            currentState = STATE_MAIN_MENU;
            drawMainMenu();
            return;
        }
        if (tx >= 406 && tx <= 550) {
            mapDlOverlayActive = false;
            playTone(660, 60);
            if (uploadServerActive) {
                stopUploadServer();
                drawGeocache();
            } else {
                startUploadServer();
            }
            return;
        }
        if (tx >= 560 && tx <= 704) {
            stopUploadServer();
            mapDlOverlayActive = false;
            playTone(660, 60);
            syncCachesFromGitHub();
            return;
        }
        return;
    }

    // SIM GPS Toggle (useful for testing indoors)
    if (tx >= 588 && tx <= 700 && ty >= 100 && ty <= 160) {
        playTone(587, 50);
        meshGps.simulated = !meshGps.simulated;
        if (meshGps.simulated) {
            meshGps.valid = true;
            if (cacheCount > 0) {
                meshGps.lat = caches[selectedCache].lat - 0.0010;
                meshGps.lon = caches[selectedCache].lon - 0.0012;
            } else {
                meshGps.lat = 43.2189;
                meshGps.lon = -71.1069;
            }
            meshGps.sats = 9;
            meshGps.lastFixMs = millis();
        } else if (!isMeshBleConnected()) {
            meshGps.valid = false;
        }
        drawGeocache();
        return;
    }

    // Bottom Navigation Tabs
    if (ty >= 1170) {
        for (int i = 0; i < 3; i++) {
            int bx = 20 + i * 232;
            if (tx >= bx && tx <= bx + 216) {
                stopUploadServer();
                mapDlOverlayActive = false;
                gcTab = i;
                playTone(523, 40);
                drawGeocache();
                return;
            }
        }
        return;
    }

    // Direct Wi-Fi Upload Overlay Button
    if (uploadServerActive) {
        if (tx >= 60 && tx <= sw - 60 && ty >= 1045 && ty <= 1140) {
            bool goList = uploadReceivedOk;
            stopUploadServer();
            if (goList) gcTab = 2;
            playTone(587, 60);
            drawGeocache();
        }
        return;
    }

    // Offline SD Map Downloader Overlay
    if (mapDlOverlayActive) {
        // Retry SD card mount banner
        if (tx >= 46 && tx <= sw - 46 && ty >= 260 && ty <= 314) {
            bool ok = initMapSdCard(true);
            mapDlStatusMsg = ok ? "microSD mounted! Ready to download tiles." : "Still no microSD detected (must be FAT32).";
            playTone(ok ? 784 : 220, 80);
            drawGeocache();
            return;
        }
        // Layer selector row (y: 375..445)
        if (ty >= 375 && ty <= 445) {
            for (int i = 0; i < 3; i++) {
                int lx = 46 + i * 212;
                if (tx >= lx && tx <= lx + 200) {
                    mapLayer = (MapLayer)i;
                    mapDlProg = MapDownloadProgress{};
                    mapDlStatusMsg = String("Selected ") + getMapLayerName(mapLayer);
                    playTone(587, 40);
                    drawGeocache();
                    return;
                }
            }
        }
        // Preset download buttons
        if (tx >= 46 && tx <= sw - 46) {
            if (ty >= 470 && ty <= 556) { playTone(660, 50); startMapDownload(0); return; }
            if (ty >= 574 && ty <= 660) { playTone(660, 50); startMapDownload(1); return; }
            if (ty >= 678 && ty <= 764) { playTone(660, 50); startMapDownload(2); return; }
        }
        // Back to Map button
        if (tx >= 60 && tx <= sw - 60 && ty >= 1045 && ty <= 1140) {
            mapDlOverlayActive = false;
            playTone(523, 50);
            drawGeocache();
            return;
        }
        return;
    }

    // Tab 0: MAP
    if (gcTab == 0) {
        // Tap directly on a map pin to select that cache
        if (tx >= 20 && tx <= 700 && ty >= 175 && ty <= 855) {
            int cx = 20 + 340, cy = 175 + 340;
            double cLat, cLon;
            getMapCenter(cLat, cLon);
            int bestIdx = -1;
            float bestDistPx = 45.0f;
            for (int i = 0; i < cacheCount; i++) {
                int px, py;
                latLonToMapPx(caches[i].lat, caches[i].lon, cLat, cLon, cx, cy, mapZoom, px, py);
                float d = hypotf((float)(tx - px), (float)(ty - py));
                if (d < bestDistPx) { bestDistPx = d; bestIdx = i; }
            }
            if (bestIdx >= 0) {
                selectedCache = bestIdx;
                hintRevealed = false;
                playTone(660, 40);
                drawGeocache();
            }
            return;
        }
        // Map Controls Row (y: 868..934)
        if (ty >= 868 && ty <= 934) {
            if (tx >= 20 && tx <= 112 && mapZoom > 12) { mapZoom--; playTone(440, 40); drawGeocache(); return; }
            if (tx >= 120 && tx <= 240) { mapFollowGps = !mapFollowGps; playTone(523, 40); drawGeocache(); return; }
            if (tx >= 248 && tx <= 340 && mapZoom < 17) { mapZoom++; playTone(660, 40); drawGeocache(); return; }
            if (tx >= 352 && tx <= 514) {
                mapLayer = (MapLayer)(((int)mapLayer + 1) % 3);
                playTone(587, 40);
                drawGeocache();
                return;
            }
            if (tx >= 526 && tx <= 700) {
                initMapSdCard(true);
                mapDlOverlayActive = true;
                mapDlProg = MapDownloadProgress{};
                mapDlStatusMsg = isMapSdReady() ? "Choose a layer & tap a download button below."
                                                : "Insert FAT32 microSD & tap banner above.";
                playTone(660, 50);
                drawGeocache();
                return;
            }
        }
        // Navigate button on summary card
        if (tx >= 510 && tx <= 682 && ty >= 1055 && ty <= 1127) {
            gcTab = 1;
            playTone(660, 50);
            drawGeocache();
            return;
        }
        return;
    }

    // Tab 1: COMPASS
    if (gcTab == 1) {
        // Reveal Hint
        if (tx >= 20 && tx <= sw - 20 && ty >= 895 && ty <= 1020) {
            hintRevealed = !hintRevealed;
            playTone(587, 40);
            drawGeocache();
            return;
        }
        // Mark Found
        if (tx >= 20 && tx <= 350 && ty >= 1045 && ty <= 1140 && cacheCount > 0) {
            caches[selectedCache].found = !caches[selectedCache].found;
            saveFoundFlags();
            playTone(caches[selectedCache].found ? 988 : 440, 100);
            drawGeocache();
            return;
        }
        // Next Cache
        if (tx >= 370 && tx <= 700 && ty >= 1045 && ty <= 1140 && cacheCount > 0) {
            selectedCache = (selectedCache + 1) % cacheCount;
            hintRevealed = false;
            playTone(660, 50);
            drawGeocache();
            return;
        }
        return;
    }

    // Tab 2: LIST
    if (gcTab == 2) {
        int startIdx = listPage * 6;
        for (int row = 0; row < 6; row++) {
            int idx = startIdx + row;
            if (idx >= cacheCount) break;
            int y = 255 + row * 124;
            if (tx >= 38 && tx <= sw - 38 && ty >= y && ty <= y + 110) {
                selectedCache = idx;
                hintRevealed = false;
                gcTab = 1; // Jump right into Compass navigation
                playTone(660, 50);
                drawGeocache();
                return;
            }
        }
        if (ty >= 1070 && ty <= 1145) {
            if (tx >= 20 && tx <= 240 && listPage > 0) { listPage--; playTone(440, 40); drawGeocache(); return; }
            if (tx >= 250 && tx <= 470) { playTone(660, 60); syncCachesFromGitHub(); return; }
            if (tx >= 480 && tx <= 700 && (listPage + 1) * 6 < cacheCount) { listPage++; playTone(660, 40); drawGeocache(); return; }
        }
    }
}

void loopGeocache() {
    // Service Direct Wi-Fi GPX Upload Server when active
    if (uploadServerActive && gpxServer) {
        if (currentState != STATE_GEOCACHE) {
            stopUploadServer();
        } else {
            gpxServer->handleClient();
            lastActivityTime = millis();
        }
    }

    // Record breadcrumb trail whenever GPS moves > 8m
    if (meshGps.valid && !meshGps.simulated) {
        bool addPt = (trailCount == 0);
        if (!addPt) {
            double d = calcDistanceMeters(trailLat[trailCount - 1], trailLon[trailCount - 1], meshGps.lat, meshGps.lon);
            if (d >= 8.0) addPt = true;
        }
        if (addPt) {
            if (trailCount >= MAX_TRAIL) {
                for (int i = 1; i < MAX_TRAIL; i++) { trailLat[i - 1] = trailLat[i]; trailLon[i - 1] = trailLon[i]; }
                trailCount = MAX_TRAIL - 1;
            }
            trailLat[trailCount] = meshGps.lat;
            trailLon[trailCount] = meshGps.lon;
            trailCount++;
        }
    }

    // Live UI refresh every 4 seconds when viewing Map or Compass with active GPS
    if (currentState == STATE_GEOCACHE && !uploadServerActive && !mapDlOverlayActive && meshGps.valid && (gcTab == 0 || gcTab == 1)) {
        if (millis() - lastUiRefreshMs > 4000UL) {
            lastUiRefreshMs = millis();
            drawGeocache();
        }
    }
}