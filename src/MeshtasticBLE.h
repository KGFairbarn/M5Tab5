#pragma once
#include "Globals.h"

struct MeshGpsData {
    bool valid = false;
    bool simulated = false;
    double lat = 43.1384;
    double lon = -70.9366;
    int32_t altMeters = 0;
    float speedMps = 0.0f;
    float courseDeg = 0.0f;
    int sats = 0;
    unsigned long lastFixMs = 0;
};

struct MeshNode {
    uint32_t nodeNum = 0;
    String shortName = "";
    String longName = "";
    bool hasPos = false;
    double lat = 0.0;
    double lon = 0.0;
    float snr = 0.0f;
    unsigned long lastHeardMs = 0;
};

extern MeshGpsData meshGps;
extern MeshNode meshNodes[24];
extern int meshNodeCount;

bool isMeshBleConnected();
String getMeshDeviceName();

void initMeshtastic();
void drawMeshtastic();
void handleMeshtasticTouch(int tx, int ty);
void loopMeshtastic();