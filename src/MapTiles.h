#pragma once
#include <Arduino.h>

enum MapLayer : uint8_t {
    MAP_LAYER_SAT  = 0,  // Esri World Imagery (same satellite tiles as bizzyboat.live)
    MAP_LAYER_TOPO = 1,  // Esri World Topo Map (roads, trails, contours)
    MAP_LAYER_OSM  = 2   // OpenStreetMap Standard
};

struct MapDownloadProgress {
    int zoom       = 0;
    int done       = 0;
    int total      = 0;
    int downloaded = 0;
    int skipped    = 0;
    int errors     = 0;
    bool cancelled = false;
};

// Return false from callback to cancel download early
typedef bool (*MapProgressCb)(const MapDownloadProgress &prog);

bool initMapSdCard(bool forceRetry = false);
bool isMapSdReady();
String getMapSdStatusText();

const char* getMapLayerName(MapLayer layer);
const char* getMapLayerShort(MapLayer layer);

// Web Mercator slippy-map projection helpers
double lonToTileX(double lon, int z);
double latToTileY(double lat, int z);

// Renders cached offline tiles from SD card clipped to (mapX, mapY, mapW, mapH).
// Returns count of tiles successfully drawn from SD.
int drawOfflineMapTiles(int mapX, int mapY, int mapW, int mapH,
                        double centerLat, double centerLon, int zoom, MapLayer layer);

// Downloads multi-zoom offline tile pack around centerLat/centerLon to SD card.
bool downloadMapAreaToSd(double centerLat, double centerLon,
                         int minZoom, int maxZoom, MapLayer layer,
                         MapProgressCb cb, MapDownloadProgress &outStats);

// Downloads offline tiles covering a lat/lon bounding box to SD card (capped for safety).
bool downloadMapBoundsToSd(double minLat, double minLon, double maxLat, double maxLon,
                           int minZoom, int maxZoom, MapLayer layer,
                           MapProgressCb cb, MapDownloadProgress &outStats);
