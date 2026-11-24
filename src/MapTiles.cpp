#include "MapTiles.h"
#include "Globals.h"
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <math.h>
#include <esp_heap_caps.h>

// M5Stack Tab5 microSD SPI pins (avoids SDMMC Slot 1 conflict with ESP32-C6 Wi-Fi SDIO)
static const int TAB5_SD_CS_PIN   = 42;
static const int TAB5_SD_SCK_PIN  = 43;
static const int TAB5_SD_MOSI_PIN = 44;
static const int TAB5_SD_MISO_PIN = 39;

static bool sdMounted = false;
static bool sdTriedOnce = false;
static uint64_t sdTotalMB = 0;

static uint8_t* tileBuf = nullptr;
static const size_t TILE_BUF_SIZE = 131072; // 128 KB (handles any 256x256 JPG or PNG)

static uint8_t* getTileBuffer() {
    if (!tileBuf) {
        tileBuf = (uint8_t*)ps_malloc(TILE_BUF_SIZE);
        if (!tileBuf) tileBuf = (uint8_t*)malloc(TILE_BUF_SIZE);
    }
    return tileBuf;
}

bool initMapSdCard(bool forceRetry) {
    if (sdMounted && !forceRetry) return true;
    if (sdTriedOnce && !forceRetry) return sdMounted;
    sdTriedOnce = true;

    if (sdMounted) {
        SD.end();
        sdMounted = false;
    }

    SPI.begin(TAB5_SD_SCK_PIN, TAB5_SD_MISO_PIN, TAB5_SD_MOSI_PIN, TAB5_SD_CS_PIN);
    if (!SD.begin(TAB5_SD_CS_PIN, SPI, 20000000)) {
        // Retry at 10 MHz for wider card compatibility
        if (!SD.begin(TAB5_SD_CS_PIN, SPI, 10000000)) {
            sdMounted = false;
            sdTotalMB = 0;
            return false;
        }
    }

    if (SD.cardType() == CARD_NONE) {
        SD.end();
        sdMounted = false;
        sdTotalMB = 0;
        return false;
    }

    sdMounted = true;
    sdTotalMB = SD.cardSize() / (1024ULL * 1024ULL);
    return true;
}

bool isMapSdReady() {
    return sdMounted;
}

String getMapSdStatusText() {
    if (!sdMounted) return "No microSD (insert FAT32 card)";
    if (sdTotalMB >= 1024) {
        return "microSD Ready (" + String((float)sdTotalMB / 1024.0f, 1) + " GB)";
    }
    return "microSD Ready (" + String((uint32_t)sdTotalMB) + " MB)";
}

const char* getMapLayerName(MapLayer layer) {
    switch (layer) {
        case MAP_LAYER_SAT:  return "SAT (Esri Imagery)";
        case MAP_LAYER_TOPO: return "TOPO (Esri Topo)";
        case MAP_LAYER_OSM:  return "OSM (Streets/Trails)";
    }
    return "SAT";
}

const char* getMapLayerShort(MapLayer layer) {
    switch (layer) {
        case MAP_LAYER_SAT:  return "SAT";
        case MAP_LAYER_TOPO: return "TOPO";
        case MAP_LAYER_OSM:  return "OSM";
    }
    return "SAT";
}

static const char* getLayerDir(MapLayer layer) {
    switch (layer) {
        case MAP_LAYER_SAT:  return "sat";
        case MAP_LAYER_TOPO: return "topo";
        case MAP_LAYER_OSM:  return "osm";
    }
    return "sat";
}

static const char* getLayerExt(MapLayer layer) {
    return (layer == MAP_LAYER_OSM) ? "png" : "jpg";
}

static String buildTilePath(MapLayer layer, int z, int x, int y) {
    return String("/maps/") + getLayerDir(layer) + "/" + String(z) + "/" + String(x) + "/" + String(y) + "." + getLayerExt(layer);
}

static String buildTileUrl(MapLayer layer, int z, int x, int y) {
    switch (layer) {
        case MAP_LAYER_SAT:
            // Esri ArcGIS World Imagery (same imagery layer as bizzyboat.live) — note z/y/x order!
            return "https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/" +
                   String(z) + "/" + String(y) + "/" + String(x);
        case MAP_LAYER_TOPO:
            // Esri ArcGIS World Topo Map — note z/y/x order!
            return "https://server.arcgisonline.com/ArcGIS/rest/services/World_Topo_Map/MapServer/tile/" +
                   String(z) + "/" + String(y) + "/" + String(x);
        case MAP_LAYER_OSM:
            return "https://tile.openstreetmap.org/" + String(z) + "/" + String(x) + "/" + String(y) + ".png";
    }
    return "";
}

static void ensureTileDir(MapLayer layer, int z, int x) {
    String p1 = "/maps";
    if (!SD.exists(p1)) SD.mkdir(p1);
    String p2 = p1 + "/" + getLayerDir(layer);
    if (!SD.exists(p2)) SD.mkdir(p2);
    String p3 = p2 + "/" + String(z);
    if (!SD.exists(p3)) SD.mkdir(p3);
    String p4 = p3 + "/" + String(x);
    if (!SD.exists(p4)) SD.mkdir(p4);
}

double lonToTileX(double lon, int z) {
    double n = (double)(1 << z);
    return (lon + 180.0) / 360.0 * n;
}

double latToTileY(double lat, int z) {
    double clamped = constrain(lat, -85.0511, 85.0511);
    double latRad = clamped * DEG_TO_RAD;
    double n = (double)(1 << z);
    return (1.0 - log(tan(latRad) + 1.0 / cos(latRad)) / M_PI) / 2.0 * n;
}

static bool isValidTileImage(const uint8_t* data, size_t len, MapLayer layer) {
    if (!data || len < 128) return false;
    if (layer == MAP_LAYER_OSM) {
        return (data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E && data[3] == 0x47); // PNG
    }
    return (data[0] == 0xFF && data[1] == 0xD8); // JPEG
}

int drawOfflineMapTiles(int mapX, int mapY, int mapW, int mapH,
                        double centerLat, double centerLon, int zoom, MapLayer layer) {
    if (!sdMounted) return 0;
    uint8_t* buf = getTileBuffer();
    if (!buf) return 0;

    int cx = mapX + mapW / 2;
    int cy = mapY + mapH / 2;
    double cTx = lonToTileX(centerLon, zoom);
    double cTy = latToTileY(centerLat, zoom);

    int maxIdx = (1 << zoom) - 1;
    int xMin = (int)floor(cTx - (mapW / 2.0) / 256.0);
    int xMax = (int)floor(cTx + (mapW / 2.0) / 256.0);
    int yMin = (int)floor(cTy - (mapH / 2.0) / 256.0);
    int yMax = (int)floor(cTy + (mapH / 2.0) / 256.0);

    M5.Display.setClipRect(mapX + 2, mapY + 2, mapW - 4, mapH - 4);
    int drawn = 0;

    for (int ty = yMin; ty <= yMax; ty++) {
        if (ty < 0 || ty > maxIdx) continue;
        for (int tx = xMin; tx <= xMax; tx++) {
            int wx = ((tx % (maxIdx + 1)) + (maxIdx + 1)) % (maxIdx + 1);
            int drawX = cx + (int)round((tx - cTx) * 256.0);
            int drawY = cy + (int)round((ty - cTy) * 256.0);

            String path = buildTilePath(layer, zoom, wx, ty);
            if (!SD.exists(path)) continue;

            File f = SD.open(path, FILE_READ);
            if (!f) continue;
            size_t sz = f.size();
            if (sz >= 128 && sz <= TILE_BUF_SIZE) {
                size_t got = f.read(buf, sz);
                f.close();
                if (got == sz) {
                    if (buf[0] == 0xFF && buf[1] == 0xD8) {
                        M5.Display.drawJpg(buf, got, drawX, drawY);
                        drawn++;
                    } else if (buf[0] == 0x89 && buf[1] == 0x50 && buf[2] == 0x4E && buf[3] == 0x47) {
                        M5.Display.drawPng(buf, got, drawX, drawY);
                        drawn++;
                    }
                }
            } else {
                f.close();
            }
        }
    }

    M5.Display.clearClipRect();
    return drawn;
}

struct TileRange {
    int z;
    int x0, x1;
    int y0, y1;
};

static bool runTileDownloadList(const TileRange* ranges, int rangeCount,
                                MapLayer layer, MapProgressCb cb, MapDownloadProgress &stats) {
    stats = MapDownloadProgress{};
    if (!initMapSdCard(false)) return false;
    if (WiFi.status() != WL_CONNECTED) return false;

    uint8_t* buf = getTileBuffer();
    if (!buf) return false;

    for (int i = 0; i < rangeCount; i++) {
        stats.total += (ranges[i].x1 - ranges[i].x0 + 1) * (ranges[i].y1 - ranges[i].y0 + 1);
    }
    if (stats.total <= 0) return false;

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setReuse(true);
    http.setTimeout(10000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setUserAgent("M5Tab5-RubyHub-OfflineMaps/1.0");

    for (int r = 0; r < rangeCount; r++) {
        int z = ranges[r].z;
        stats.zoom = z;
        int maxIdx = (1 << z) - 1;

        for (int x = ranges[r].x0; x <= ranges[r].x1; x++) {
            int wx = ((x % (maxIdx + 1)) + (maxIdx + 1)) % (maxIdx + 1);
            bool dirReady = false;

            for (int y = ranges[r].y0; y <= ranges[r].y1; y++) {
                if (y < 0 || y > maxIdx) {
                    stats.done++;
                    continue;
                }
                lastActivityTime = millis();

                String path = buildTilePath(layer, z, wx, y);
                if (SD.exists(path)) {
                    stats.skipped++;
                    stats.done++;
                    if (cb && !cb(stats)) {
                        stats.cancelled = true;
                        http.end();
                        return true;
                    }
                    continue;
                }

                String url = buildTileUrl(layer, z, wx, y);
                bool ok = false;
                if (http.begin(client, url)) {
                    int code = http.GET();
                    if (code == HTTP_CODE_OK) {
                        int contentLen = http.getSize();
                        WiFiClient* stream = http.getStreamPtr();
                        size_t totalRead = 0;
                        unsigned long tStart = millis();
                        unsigned long lastRx = millis();
                        bool overflow = false;

                        while (millis() - tStart < 10000UL && millis() - lastRx < 4000UL) {
                            if (contentLen > 0 && (int)totalRead >= contentLen) break;
                            int avail = stream->available();
                            if (avail > 0) {
                                size_t space = TILE_BUF_SIZE - totalRead;
                                if (space == 0) { overflow = true; break; }
                                size_t toRead = (size_t)avail > space ? space : (size_t)avail;
                                int n = stream->readBytes(buf + totalRead, toRead);
                                if (n > 0) {
                                    totalRead += (size_t)n;
                                    lastRx = millis();
                                }
                            } else {
                                if (!http.connected()) break;
                                delay(2);
                            }
                        }

                        if (!overflow && isValidTileImage(buf, totalRead, layer)) {
                            if (!dirReady) {
                                ensureTileDir(layer, z, wx);
                                dirReady = true;
                            }
                            File f = SD.open(path, FILE_WRITE);
                            if (f) {
                                size_t written = f.write(buf, totalRead);
                                f.close();
                                if (written == totalRead) {
                                    ok = true;
                                } else {
                                    SD.remove(path);
                                }
                            }
                        }
                    } else if (code < 0) {
                        // Reset keep-alive socket after transport error
                        http.end();
                    }
                }

                if (ok) stats.downloaded++;
                else    stats.errors++;
                stats.done++;

                if (cb && !cb(stats)) {
                    stats.cancelled = true;
                    http.end();
                    return true;
                }

                // Polite pacing for OpenStreetMap
                if (layer == MAP_LAYER_OSM && ok) delay(120);
            }
        }
    }

    http.end();
    return true;
}

bool downloadMapAreaToSd(double centerLat, double centerLon,
                         int minZoom, int maxZoom, MapLayer layer,
                         MapProgressCb cb, MapDownloadProgress &outStats) {
    TileRange ranges[8];
    int count = 0;
    for (int z = minZoom; z <= maxZoom && count < 8; z++) {
        int cx = (int)floor(lonToTileX(centerLon, z));
        int cy = (int)floor(latToTileY(centerLat, z));
        int rad = (minZoom == maxZoom) ? 2 : ((z <= 14) ? 1 : 2);
        int maxIdx = (1 << z) - 1;
        ranges[count++] = {
            z,
            cx - rad, cx + rad,
            constrain(cy - rad, 0, maxIdx),
            constrain(cy + rad, 0, maxIdx)
        };
    }
    return runTileDownloadList(ranges, count, layer, cb, outStats);
}

bool downloadMapBoundsToSd(double minLat, double minLon, double maxLat, double maxLon,
                           int minZoom, int maxZoom, MapLayer layer,
                           MapProgressCb cb, MapDownloadProgress &outStats) {
    TileRange ranges[8];
    int count = 0;
    int budgetRemaining = 180; // Safety cap so huge spans don't stall for hours

    for (int z = minZoom; z <= maxZoom && count < 8 && budgetRemaining > 0; z++) {
        int maxIdx = (1 << z) - 1;
        int x0 = (int)floor(lonToTileX(minLon, z)) - 1;
        int x1 = (int)floor(lonToTileX(maxLon, z)) + 1;
        // Note: higher lat = smaller tile Y in Web Mercator
        int y0 = constrain((int)floor(latToTileY(maxLat, z)) - 1, 0, maxIdx);
        int y1 = constrain((int)floor(latToTileY(minLat, z)) + 1, 0, maxIdx);

        int w = x1 - x0 + 1;
        int h = y1 - y0 + 1;
        if (w * h > budgetRemaining) {
            // Clamp around bounding-box center to stay inside tile budget
            int cx = (x0 + x1) / 2;
            int cy = (y0 + y1) / 2;
            int rad = (int)floor((sqrt((double)budgetRemaining) - 1.0) / 2.0);
            if (rad < 1) rad = 1;
            x0 = cx - rad; x1 = cx + rad;
            y0 = constrain(cy - rad, 0, maxIdx);
            y1 = constrain(cy + rad, 0, maxIdx);
            w = x1 - x0 + 1;
            h = y1 - y0 + 1;
        }
        budgetRemaining -= (w * h);
        ranges[count++] = { z, x0, x1, y0, y1 };
    }
    return runTileDownloadList(ranges, count, layer, cb, outStats);
}
