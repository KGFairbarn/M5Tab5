#include "MeshtasticBLE.h"
#include <sdkconfig.h>
#include <soc/soc_caps.h>
#include <vector>
#include <math.h>

#if (defined(SOC_BLE_SUPPORTED) || defined(CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE)) && \
    (defined(CONFIG_BLUEDROID_ENABLED) || defined(CONFIG_NIMBLE_ENABLED) || defined(CONFIG_BT_NIMBLE_ENABLED))
  #define MESH_BLE_AVAILABLE 1
  #include <BLEDevice.h>
  #include <BLEUtils.h>
  #include <BLEScan.h>
  #include <BLEClient.h>
  #include <BLESecurity.h>
  #if defined(CONFIG_NIMBLE_ENABLED) || defined(CONFIG_BT_NIMBLE_ENABLED)
    #define MESH_USE_RAW_NIMBLE 1
    #include <host/ble_hs.h>
    #include <host/ble_gap.h>
    #include <host/ble_gatt.h>
    #include <host/ble_att.h>
    #include <host/ble_uuid.h>
    #include <os/os_mbuf.h>
  #endif
  #if defined(CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE)
    #include <esp32-hal-hosted.h>
    #include <WiFi.h>
    #include <WiFiClientSecure.h>
    #include <HTTPClient.h>
  #endif
#endif

// ======================== MESHTASTIC BLE UUIDs ========================
static const char* MESH_SERVICE_UUID   = "6ba1b218-15a8-461f-9fa8-5dcae273eafd";
static const char* MESH_TORADIO_UUID   = "f75c76d2-129e-4dad-a1dd-7866124401e7";
static const char* MESH_FROMRADIO_UUID = "2c55e69e-4993-11ed-b878-0242ac120002";
static const char* MESH_FROMNUM_UUID   = "ed9da18c-a800-4f66-a670-aa7547e34453";

// ======================== SHARED GLOBALS ==============================
MeshGpsData meshGps;
MeshNode meshNodes[24];
int meshNodeCount = 0;

struct MeshChatMsg {
    String sender;
    String text;
    bool outgoing;
};

static const int MAX_CHAT = 12;
static MeshChatMsg chatLog[MAX_CHAT];
static int chatCount = 0;

// ======================== BLE & UI STATE ==============================
static bool bleInitialized = false;
static bool meshConnected = false;
static volatile bool meshNewDataFlag = false;
static uint32_t myNodeNum = 0;
static String connectedDevName = "";
static String bleStatusText = "Tap SCAN BLE to find your T114 V2";
static String c6OtaError = "";
static unsigned long lastPollMs = 0;

static const int MAX_SCANNED = 5;
static String scannedNames[MAX_SCANNED];
static String scannedAddrs[MAX_SCANNED];
static bool scannedIsMesh[MAX_SCANNED] = {false};
static int scannedRssi[MAX_SCANNED] = {0};
static int scannedCount = 0;

#if defined(MESH_BLE_AVAILABLE)
static BLEAdvertisedDevice* scannedDevs[MAX_SCANNED] = {nullptr};
static volatile int lastGapConnStatus = -1;
static volatile int lastGapDiscReason = -1;
static volatile int lastGapEncStatus = -1;

#if defined(MESH_USE_RAW_NIMBLE)
static volatile uint16_t meshConnHandle = BLE_HS_CONN_HANDLE_NONE;
static volatile bool meshOpDone = false;
static volatile int meshOpStatus = 0;
static volatile bool meshIntentionalDisconnect = false;
static volatile int meshSvcSeenCount = 0;
static volatile uint16_t meshSvcStart = 0;
static volatile uint16_t meshSvcEnd = 0;
static volatile uint16_t meshToRadioHandle = 0;
static volatile uint16_t meshFromRadioHandle = 0;
static volatile uint16_t meshFromNumHandle = 0;
static volatile uint16_t meshFromNumCccdHandle = 0;
static std::vector<uint8_t> meshReadBuf;
static ble_uuid_any_t meshSvcUuidNative;
static ble_uuid_any_t meshToRadioUuidNative;
static ble_uuid_any_t meshFromRadioUuidNative;
static ble_uuid_any_t meshFromNumUuidNative;
static bool meshUuidsInited = false;

static void initMeshNativeUuids() {
    if (meshUuidsInited) return;
    BLEUUID uSvc(MESH_SERVICE_UUID);
    BLEUUID uTo(MESH_TORADIO_UUID);
    BLEUUID uFrom(MESH_FROMRADIO_UUID);
    BLEUUID uNum(MESH_FROMNUM_UUID);
    if (uSvc.getNative())  meshSvcUuidNative = *uSvc.getNative();
    if (uTo.getNative())   meshToRadioUuidNative = *uTo.getNative();
    if (uFrom.getNative()) meshFromRadioUuidNative = *uFrom.getNative();
    if (uNum.getNative())  meshFromNumUuidNative = *uNum.getNative();
    meshUuidsInited = true;
}

static bool waitNimbleOp(uint32_t timeoutMs) {
    unsigned long start = millis();
    while (!meshOpDone && (millis() - start) < timeoutMs) {
        delay(5);
    }
    return meshOpDone;
}

static int meshClientGapCb(struct ble_gap_event *event, void *arg) {
    (void)arg;
    if (!event) return 0;

    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            lastGapConnStatus = event->connect.status;
            Serial.printf("[MeshBLE] GAP CONNECT status=%d handle=%d\n",
                          event->connect.status, event->connect.conn_handle);
            if (event->connect.status == 0) {
                meshConnHandle = event->connect.conn_handle;
            } else {
                meshConnHandle = BLE_HS_CONN_HANDLE_NONE;
            }
            meshOpStatus = event->connect.status;
            meshOpDone = true;
            return 0;

        case BLE_GAP_EVENT_DISCONNECT:
            lastGapDiscReason = event->disconnect.reason;
            Serial.printf("[MeshBLE] GAP DISCONNECT reason=%d (0x%x) intentional=%d\n",
                          event->disconnect.reason, event->disconnect.reason,
                          meshIntentionalDisconnect ? 1 : 0);
            meshConnHandle = BLE_HS_CONN_HANDLE_NONE;
            meshConnected = false;
            if (!meshIntentionalDisconnect) {
                bleStatusText = "Disconnected (r=" + String(event->disconnect.reason) + ")";
            }
            meshOpStatus = event->disconnect.reason;
            meshOpDone = true;
            return 0;

        case BLE_GAP_EVENT_CONN_UPDATE_REQ:
            return 0; // Accept peer connection parameter updates

        case BLE_GAP_EVENT_MTU:
            Serial.printf("[MeshBLE] MTU updated to %u\n", (unsigned)event->mtu.value);
            return 0;

        case BLE_GAP_EVENT_NOTIFY_RX:
            if (event->notify_rx.conn_handle == meshConnHandle) {
                meshNewDataFlag = true;
            }
            return 0;

        case BLE_GAP_EVENT_ENC_CHANGE:
            lastGapEncStatus = event->enc_change.status;
            Serial.printf("[MeshBLE] GAP ENC_CHANGE status=%d\n", event->enc_change.status);
            meshOpStatus = event->enc_change.status;
            meshOpDone = true;
            return 0;

        case BLE_GAP_EVENT_PASSKEY_ACTION: {
            struct ble_sm_io pkey = {};
            pkey.action = event->passkey.params.action;
            if (pkey.action == BLE_SM_IOACT_INPUT || pkey.action == BLE_SM_IOACT_DISP) {
                pkey.passkey = 123456;
            } else if (pkey.action == BLE_SM_IOACT_NUMCMP) {
                pkey.numcmp_accept = 1;
            }
            ble_sm_inject_io(event->passkey.conn_handle, &pkey);
            return 0;
        }

        default:
            return 0;
    }
}

static int meshSvcDiscCb(uint16_t conn_handle, const struct ble_gatt_error *error,
                         const struct ble_gatt_svc *service, void *arg) {
    (void)conn_handle; (void)arg;
    if (error->status == 0 && service) {
        meshSvcSeenCount = meshSvcSeenCount + 1;
        BLEUUID foundObj(service->uuid);
        Serial.printf("[MeshBLE] Service #%d %s handles %u..%u\n",
                      (int)meshSvcSeenCount, foundObj.toString().c_str(),
                      (unsigned)service->start_handle, (unsigned)service->end_handle);
        if (ble_uuid_cmp(&service->uuid.u, &meshSvcUuidNative.u) == 0) {
            meshSvcStart = service->start_handle;
            meshSvcEnd = service->end_handle;
            Serial.printf("[MeshBLE] Matched Meshtastic service %u..%u!\n",
                          (unsigned)meshSvcStart, (unsigned)meshSvcEnd);
        }
        return 0;
    }
    meshOpStatus = (error->status == BLE_HS_EDONE) ? 0 : error->status;
    meshOpDone = true;
    return 0;
}

static int meshChrDiscCb(uint16_t conn_handle, const struct ble_gatt_error *error,
                         const struct ble_gatt_chr *chr, void *arg) {
    (void)conn_handle; (void)arg;
    if (error->status == 0 && chr) {
        if (ble_uuid_cmp(&chr->uuid.u, &meshToRadioUuidNative.u) == 0) {
            meshToRadioHandle = chr->val_handle;
            Serial.printf("[MeshBLE] ToRadio handle=%u\n", (unsigned)meshToRadioHandle);
        } else if (ble_uuid_cmp(&chr->uuid.u, &meshFromRadioUuidNative.u) == 0) {
            meshFromRadioHandle = chr->val_handle;
            Serial.printf("[MeshBLE] FromRadio handle=%u\n", (unsigned)meshFromRadioHandle);
        } else if (ble_uuid_cmp(&chr->uuid.u, &meshFromNumUuidNative.u) == 0) {
            meshFromNumHandle = chr->val_handle;
            Serial.printf("[MeshBLE] FromNum handle=%u\n", (unsigned)meshFromNumHandle);
        }
        return 0;
    }
    meshOpStatus = (error->status == BLE_HS_EDONE) ? 0 : error->status;
    meshOpDone = true;
    return 0;
}

static int meshDscDiscCb(uint16_t conn_handle, const struct ble_gatt_error *error,
                         uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg) {
    (void)conn_handle; (void)chr_val_handle; (void)arg;
    if (error->status == 0 && dsc) {
        if (dsc->uuid.u.type == BLE_UUID_TYPE_16 && dsc->uuid.u16.value == 0x2902) {
            meshFromNumCccdHandle = dsc->handle;
            Serial.printf("[MeshBLE] FromNum CCCD handle=%u\n", (unsigned)meshFromNumCccdHandle);
        }
        return 0;
    }
    meshOpStatus = (error->status == BLE_HS_EDONE) ? 0 : error->status;
    meshOpDone = true;
    return 0;
}

static int meshWriteCb(uint16_t conn_handle, const struct ble_gatt_error *error,
                       struct ble_gatt_attr *attr, void *arg) {
    (void)conn_handle; (void)attr; (void)arg;
    meshOpStatus = error ? error->status : 0;
    meshOpDone = true;
    return 0;
}

static int meshReadLongCb(uint16_t conn_handle, const struct ble_gatt_error *error,
                          struct ble_gatt_attr *attr, void *arg) {
    (void)conn_handle; (void)arg;
    if (error->status == 0 && attr && attr->om) {
        uint16_t chunkLen = OS_MBUF_PKTLEN(attr->om);
        if (chunkLen > 0 && meshReadBuf.size() + chunkLen <= 512) {
            size_t oldSize = meshReadBuf.size();
            meshReadBuf.resize(oldSize + chunkLen);
            os_mbuf_copydata(attr->om, 0, chunkLen, meshReadBuf.data() + oldSize);
        }
        return 0;
    }
    meshOpStatus = (error->status == BLE_HS_EDONE) ? 0 : error->status;
    meshOpDone = true;
    return 0;
}

static int meshReadSingleCb(uint16_t conn_handle, const struct ble_gatt_error *error,
                            struct ble_gatt_attr *attr, void *arg) {
    (void)conn_handle; (void)arg;
    meshReadBuf.clear();
    if (error->status == 0 && attr && attr->om) {
        uint16_t total = OS_MBUF_PKTLEN(attr->om);
        if (total > 0 && total <= 512) {
            meshReadBuf.resize(total);
            os_mbuf_copydata(attr->om, 0, total, meshReadBuf.data());
        }
    }
    meshOpStatus = error ? error->status : 0;
    meshOpDone = true;
    return 0;
}

static bool nimbleWriteHandle(uint16_t handle, const uint8_t* data, size_t len) {
    if (meshConnHandle == BLE_HS_CONN_HANDLE_NONE || handle == 0) return false;
    meshOpDone = false;
    meshOpStatus = -1;
    uint16_t mtu = ble_att_mtu(meshConnHandle);
    int rc = 0;
    if (mtu > 3 && len > (size_t)(mtu - 3)) {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(data, (uint16_t)len);
        if (!om) return false;
        rc = ble_gattc_write_long(meshConnHandle, handle, 0, om, meshWriteCb, nullptr);
    } else {
        rc = ble_gattc_write_flat(meshConnHandle, handle, data, (uint16_t)len, meshWriteCb, nullptr);
    }
    if (rc != 0) {
        Serial.printf("[MeshBLE] write rc=%d\n", rc);
        return false;
    }
    return waitNimbleOp(3500) && (meshOpStatus == 0);
}

static bool nimbleReadFromRadio(std::vector<uint8_t> &out) {
    out.clear();
    if (meshConnHandle == BLE_HS_CONN_HANDLE_NONE || meshFromRadioHandle == 0) return false;
    meshReadBuf.clear();
    meshOpDone = false;
    meshOpStatus = -1;
    int rc = ble_gattc_read_long(meshConnHandle, meshFromRadioHandle, 0, meshReadLongCb, nullptr);
    if (rc != 0) {
        Serial.printf("[MeshBLE] read_long rc=%d\n", rc);
        return false;
    }
    if (!waitNimbleOp(3000)) return false;
    if (meshOpStatus == BLE_HS_ATT_ERR(BLE_ATT_ERR_ATTR_NOT_LONG)) {
        meshReadBuf.clear();
        meshOpDone = false;
        meshOpStatus = -1;
        rc = ble_gattc_read(meshConnHandle, meshFromRadioHandle, meshReadSingleCb, nullptr);
        if (rc != 0 || !waitNimbleOp(3000) || meshOpStatus != 0) return false;
    } else if (meshOpStatus != 0) {
        Serial.printf("[MeshBLE] read_long status=%d\n", meshOpStatus);
        return false;
    }
    out = meshReadBuf;
    return true;
}
#else
static BLEClient* pMeshClient = nullptr;
static BLERemoteCharacteristic* pToRadioChar = nullptr;
static BLERemoteCharacteristic* pFromRadioChar = nullptr;
static BLERemoteCharacteristic* pFromNumChar = nullptr;
#endif
#endif

static int meshTab = 2; // 0=CHAT, 1=NODES, 2=BLE/GPS
static String chatDraft = "";
static bool chatCaps = false;
static double prevGpsLat = 0.0, prevGpsLon = 0.0;
static bool hasPrevGps = false;

bool isMeshBleConnected() { return meshConnected; }
String getMeshDeviceName() { return connectedDevName; }

static String getBtSdkFlags() {
    String s = "SDK: ";
#ifdef CONFIG_BT_ENABLED
    s += "BT=ON ";
#else
    s += "BT=OFF ";
#endif
#ifdef CONFIG_BLUEDROID_ENABLED
    s += "Bluedroid=ON ";
#else
    s += "Bluedroid=OFF ";
#endif
#ifdef CONFIG_BT_NIMBLE_ENABLED
    s += "NimBLE=ON";
#else
    s += "NimBLE=OFF";
#endif
    return s;
}

// ======================== MINIMAL PROTOBUF HELPERS ====================
static bool pbReadVarint(const uint8_t* buf, size_t len, size_t &pos, uint64_t &out) {
    out = 0;
    int shift = 0;
    while (pos < len && shift < 64) {
        uint8_t b = buf[pos++];
        out |= ((uint64_t)(b & 0x7F)) << shift;
        if (!(b & 0x80)) return true;
        shift += 7;
    }
    return false;
}

static bool pbReadFixed32(const uint8_t* buf, size_t len, size_t &pos, uint32_t &out) {
    if (pos + 4 > len) return false;
    out = (uint32_t)buf[pos] | ((uint32_t)buf[pos + 1] << 8) |
          ((uint32_t)buf[pos + 2] << 16) | ((uint32_t)buf[pos + 3] << 24);
    pos += 4;
    return true;
}

static bool pbSkipField(const uint8_t* buf, size_t len, size_t &pos, uint8_t wireType) {
    uint64_t v = 0;
    if (wireType == 0) return pbReadVarint(buf, len, pos, v);
    if (wireType == 1) { if (pos + 8 > len) return false; pos += 8; return true; }
    if (wireType == 2) {
        if (!pbReadVarint(buf, len, pos, v)) return false;
        if (pos + (size_t)v > len) return false;
        pos += (size_t)v;
        return true;
    }
    if (wireType == 5) { if (pos + 4 > len) return false; pos += 4; return true; }
    return false;
}

static void pbWriteVarint(std::vector<uint8_t> &out, uint64_t val) {
    while (val >= 0x80) {
        out.push_back((uint8_t)((val & 0x7F) | 0x80));
        val >>= 7;
    }
    out.push_back((uint8_t)val);
}

static void pbWriteTag(std::vector<uint8_t> &out, uint32_t field, uint8_t wireType) {
    pbWriteVarint(out, ((uint64_t)field << 3) | wireType);
}

static void pbWriteFixed32(std::vector<uint8_t> &out, uint32_t val) {
    out.push_back((uint8_t)(val & 0xFF));
    out.push_back((uint8_t)((val >> 8) & 0xFF));
    out.push_back((uint8_t)((val >> 16) & 0xFF));
    out.push_back((uint8_t)((val >> 24) & 0xFF));
}

// ======================== NODE & CHAT STORAGE =========================
static int findOrCreateNode(uint32_t num) {
    for (int i = 0; i < meshNodeCount; i++) {
        if (meshNodes[i].nodeNum == num) return i;
    }
    if (meshNodeCount < 24) {
        int idx = meshNodeCount++;
        meshNodes[idx] = MeshNode();
        meshNodes[idx].nodeNum = num;
        char hexId[10];
        snprintf(hexId, sizeof(hexId), "!%04x", (unsigned)(num & 0xFFFF));
        meshNodes[idx].shortName = String(hexId);
        meshNodes[idx].longName = "Node " + String(hexId);
        return idx;
    }
    return -1;
}

static String lookupSenderName(uint32_t num) {
    for (int i = 0; i < meshNodeCount; i++) {
        if (meshNodes[i].nodeNum == num) {
            return meshNodes[i].shortName.length() ? meshNodes[i].shortName : meshNodes[i].longName;
        }
    }
    char hexId[10];
    snprintf(hexId, sizeof(hexId), "!%04x", (unsigned)(num & 0xFFFF));
    return String(hexId);
}

static void addChatMessage(const String &sender, const String &text, bool outgoing) {
    if (chatCount >= MAX_CHAT) {
        for (int i = 1; i < MAX_CHAT; i++) chatLog[i - 1] = chatLog[i];
        chatCount = MAX_CHAT - 1;
    }
    chatLog[chatCount].sender = sender;
    chatLog[chatCount].text = text;
    chatLog[chatCount].outgoing = outgoing;
    chatCount++;
}

static void updateLocalGpsFix(double lat, double lon, int32_t alt, float spd, float trk, int sats) {
    if (lat == 0.0 && lon == 0.0) return;
    if (hasPrevGps) {
        double dLat = (lat - prevGpsLat) * 111320.0;
        double dLon = (lon - prevGpsLon) * 111320.0 * cos(lat * DEG_TO_RAD);
        double stepM = sqrt(dLat * dLat + dLon * dLon);
        if (stepM > 3.0) {
            if (trk <= 0.01f) {
                double brg = atan2(dLon, dLat) * RAD_TO_DEG;
                if (brg < 0) brg += 360.0;
                meshGps.courseDeg = (float)brg;
            }
            prevGpsLat = lat;
            prevGpsLon = lon;
        }
    } else {
        prevGpsLat = lat;
        prevGpsLon = lon;
        hasPrevGps = true;
    }

    meshGps.valid = true;
    meshGps.simulated = false;
    meshGps.lat = lat;
    meshGps.lon = lon;
    if (alt != 0) meshGps.altMeters = alt;
    if (spd > 0.0f) meshGps.speedMps = spd;
    if (trk > 0.01f) meshGps.courseDeg = trk;
    if (sats > 0) meshGps.sats = sats;
    meshGps.lastFixMs = millis();
}

// ======================== MESHTASTIC PROTOBUF DECODER =================
static bool parsePositionPb(const uint8_t* buf, size_t len, double &lat, double &lon,
                            int32_t &alt, float &spd, float &trk, int &sats) {
    size_t pos = 0;
    bool gotLat = false, gotLon = false;
    lat = 0.0; lon = 0.0; alt = 0; spd = 0.0f; trk = 0.0f; sats = 0;

    while (pos < len) {
        uint64_t tag = 0;
        if (!pbReadVarint(buf, len, pos, tag)) break;
        uint32_t field = (uint32_t)(tag >> 3);
        uint8_t wire = (uint8_t)(tag & 0x07);

        if (field == 1 && wire == 5) {
            uint32_t raw = 0;
            if (pbReadFixed32(buf, len, pos, raw)) { lat = ((int32_t)raw) * 1e-7; gotLat = (raw != 0); }
        } else if (field == 2 && wire == 5) {
            uint32_t raw = 0;
            if (pbReadFixed32(buf, len, pos, raw)) { lon = ((int32_t)raw) * 1e-7; gotLon = (raw != 0); }
        } else if (field == 3 && wire == 0) {
            uint64_t v = 0;
            if (pbReadVarint(buf, len, pos, v)) alt = (int32_t)v;
        } else if (field == 15 && wire == 0) {
            uint64_t v = 0;
            if (pbReadVarint(buf, len, pos, v)) spd = (float)v;
        } else if (field == 16 && wire == 0) {
            uint64_t v = 0;
            if (pbReadVarint(buf, len, pos, v)) trk = ((float)v) * 0.01f;
        } else if (field == 19 && wire == 0) {
            uint64_t v = 0;
            if (pbReadVarint(buf, len, pos, v)) sats = (int)v;
        } else {
            if (!pbSkipField(buf, len, pos, wire)) break;
        }
    }
    return gotLat && gotLon;
}

static void parseUserPb(const uint8_t* buf, size_t len, String &longName, String &shortName) {
    size_t pos = 0;
    while (pos < len) {
        uint64_t tag = 0;
        if (!pbReadVarint(buf, len, pos, tag)) break;
        uint32_t field = (uint32_t)(tag >> 3);
        uint8_t wire = (uint8_t)(tag & 0x07);
        if ((field == 2 || field == 3) && wire == 2) {
            uint64_t slen = 0;
            if (!pbReadVarint(buf, len, pos, slen) || pos + slen > len) break;
            String s = "";
            for (size_t i = 0; i < (size_t)slen; i++) s += (char)buf[pos + i];
            pos += (size_t)slen;
            if (field == 2) longName = s;
            if (field == 3) shortName = s;
        } else {
            if (!pbSkipField(buf, len, pos, wire)) break;
        }
    }
}

static void parseNodeInfoPb(const uint8_t* buf, size_t len) {
    size_t pos = 0;
    uint32_t num = 0;
    String lName = "", sName = "";
    bool hasPos = false;
    double lat = 0, lon = 0; int32_t alt = 0; float spd = 0, trk = 0; int sats = 0;
    float snr = 0.0f;

    while (pos < len) {
        uint64_t tag = 0;
        if (!pbReadVarint(buf, len, pos, tag)) break;
        uint32_t field = (uint32_t)(tag >> 3);
        uint8_t wire = (uint8_t)(tag & 0x07);
        if (field == 1 && wire == 0) {
            uint64_t v = 0;
            if (pbReadVarint(buf, len, pos, v)) num = (uint32_t)v;
        } else if (field == 2 && wire == 2) {
            uint64_t subLen = 0;
            if (!pbReadVarint(buf, len, pos, subLen) || pos + subLen > len) break;
            parseUserPb(buf + pos, (size_t)subLen, lName, sName);
            pos += (size_t)subLen;
        } else if (field == 3 && wire == 2) {
            uint64_t subLen = 0;
            if (!pbReadVarint(buf, len, pos, subLen) || pos + subLen > len) break;
            hasPos = parsePositionPb(buf + pos, (size_t)subLen, lat, lon, alt, spd, trk, sats);
            pos += (size_t)subLen;
        } else if (field == 4 && wire == 5) {
            uint32_t raw = 0;
            if (pbReadFixed32(buf, len, pos, raw)) memcpy(&snr, &raw, 4);
        } else {
            if (!pbSkipField(buf, len, pos, wire)) break;
        }
    }

    if (num == 0) return;
    int idx = findOrCreateNode(num);
    if (idx >= 0) {
        if (lName.length()) meshNodes[idx].longName = lName;
        if (sName.length()) meshNodes[idx].shortName = sName;
        meshNodes[idx].snr = snr;
        meshNodes[idx].lastHeardMs = millis();
        if (hasPos) {
            meshNodes[idx].hasPos = true;
            meshNodes[idx].lat = lat;
            meshNodes[idx].lon = lon;
        }
    }
    if (hasPos && (num == myNodeNum || (myNodeNum == 0 && !meshGps.valid))) {
        updateLocalGpsFix(lat, lon, alt, spd, trk, sats);
    }
}

static void parseMeshPacketPb(const uint8_t* buf, size_t len) {
    size_t pos = 0;
    uint32_t fromNode = 0;
    uint32_t portNum = 0;
    const uint8_t* payloadPtr = nullptr;
    size_t payloadLen = 0;

    while (pos < len) {
        uint64_t tag = 0;
        if (!pbReadVarint(buf, len, pos, tag)) break;
        uint32_t field = (uint32_t)(tag >> 3);
        uint8_t wire = (uint8_t)(tag & 0x07);

        if (field == 1 && wire == 5) {
            pbReadFixed32(buf, len, pos, fromNode);
        } else if (field == 4 && wire == 2) {
            uint64_t dLen = 0;
            if (!pbReadVarint(buf, len, pos, dLen) || pos + dLen > len) break;
            size_t dEnd = pos + (size_t)dLen;
            while (pos < dEnd) {
                uint64_t dTag = 0;
                if (!pbReadVarint(buf, dEnd, pos, dTag)) break;
                uint32_t dField = (uint32_t)(dTag >> 3);
                uint8_t dWire = (uint8_t)(dTag & 0x07);
                if (dField == 1 && dWire == 0) {
                    uint64_t pv = 0;
                    if (pbReadVarint(buf, dEnd, pos, pv)) portNum = (uint32_t)pv;
                } else if (dField == 2 && dWire == 2) {
                    uint64_t pLen = 0;
                    if (!pbReadVarint(buf, dEnd, pos, pLen) || pos + pLen > dEnd) break;
                    payloadPtr = buf + pos;
                    payloadLen = (size_t)pLen;
                    pos += payloadLen;
                } else {
                    if (!pbSkipField(buf, dEnd, pos, dWire)) break;
                }
            }
            pos = dEnd;
        } else {
            if (!pbSkipField(buf, len, pos, wire)) break;
        }
    }

    if (!payloadPtr || payloadLen == 0) return;

    if (portNum == 1) {
        String msg = "";
        for (size_t i = 0; i < payloadLen; i++) {
            char c = (char)payloadPtr[i];
            if (c >= 32 && c <= 126) msg += c;
        }
        if (msg.length()) {
            addChatMessage(lookupSenderName(fromNode), msg, false);
            playTone(784, 80);
        }
    } else if (portNum == 3) {
        double lat = 0, lon = 0; int32_t alt = 0; float spd = 0, trk = 0; int sats = 0;
        if (parsePositionPb(payloadPtr, payloadLen, lat, lon, alt, spd, trk, sats)) {
            int idx = findOrCreateNode(fromNode);
            if (idx >= 0) {
                meshNodes[idx].hasPos = true;
                meshNodes[idx].lat = lat;
                meshNodes[idx].lon = lon;
                meshNodes[idx].lastHeardMs = millis();
            }
            if (fromNode == myNodeNum || fromNode == 0) {
                updateLocalGpsFix(lat, lon, alt, spd, trk, sats);
            }
        }
    }
}

static void parseFromRadioPb(const uint8_t* buf, size_t len) {
    size_t pos = 0;
    while (pos < len) {
        uint64_t tag = 0;
        if (!pbReadVarint(buf, len, pos, tag)) break;
        uint32_t field = (uint32_t)(tag >> 3);
        uint8_t wire = (uint8_t)(tag & 0x07);

        if (field == 2 && wire == 2) {
            uint64_t subLen = 0;
            if (!pbReadVarint(buf, len, pos, subLen) || pos + subLen > len) break;
            parseMeshPacketPb(buf + pos, (size_t)subLen);
            pos += (size_t)subLen;
        } else if (field == 3 && wire == 2) {
            uint64_t subLen = 0;
            if (!pbReadVarint(buf, len, pos, subLen) || pos + subLen > len) break;
            size_t end = pos + (size_t)subLen;
            while (pos < end) {
                uint64_t mTag = 0;
                if (!pbReadVarint(buf, end, pos, mTag)) break;
                uint32_t mField = (uint32_t)(mTag >> 3);
                uint8_t mWire = (uint8_t)(mTag & 0x07);
                if (mField == 1 && mWire == 0) {
                    uint64_t v = 0;
                    if (pbReadVarint(buf, end, pos, v)) myNodeNum = (uint32_t)v;
                } else {
                    if (!pbSkipField(buf, end, pos, mWire)) break;
                }
            }
            pos = end;
        } else if (field == 4 && wire == 2) {
            uint64_t subLen = 0;
            if (!pbReadVarint(buf, len, pos, subLen) || pos + subLen > len) break;
            parseNodeInfoPb(buf + pos, (size_t)subLen);
            pos += (size_t)subLen;
        } else {
            if (!pbSkipField(buf, len, pos, wire)) break;
        }
    }
}

// ======================== BLE TRANSPORT ===============================
#if defined(MESH_BLE_AVAILABLE) && !defined(MESH_USE_RAW_NIMBLE)
class MeshClientCallbacks : public BLEClientCallbacks {
    void onConnect(BLEClient* pclient) override {}
    void onDisconnect(BLEClient* pclient) override {
        meshConnected = false;
        bleStatusText = "Disconnected from T114";
    }
};

static void fromNumNotifyCallback(BLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify) {
    meshNewDataFlag = true;
}
#endif

static void sendWantConfig() {
#if defined(MESH_USE_RAW_NIMBLE)
    if (!meshConnected || meshToRadioHandle == 0) return;
    std::vector<uint8_t> pkt;
    pbWriteTag(pkt, 3, 0);
    pbWriteVarint(pkt, 69420);
    nimbleWriteHandle(meshToRadioHandle, pkt.data(), pkt.size());
    meshNewDataFlag = true;
#elif defined(MESH_BLE_AVAILABLE)
    if (!meshConnected || !pToRadioChar) return;
    std::vector<uint8_t> pkt;
    pbWriteTag(pkt, 3, 0);
    pbWriteVarint(pkt, 69420);
    pToRadioChar->writeValue(pkt.data(), pkt.size(), true);
    meshNewDataFlag = true;
#endif
}

static void sendMeshTextMessage(const String &text) {
#if defined(MESH_BLE_AVAILABLE)
    if (!meshConnected || text.length() == 0) return;
#if defined(MESH_USE_RAW_NIMBLE)
    if (meshToRadioHandle == 0) return;
#else
    if (!pToRadioChar) return;
#endif

    std::vector<uint8_t> dataMsg;
    pbWriteTag(dataMsg, 1, 0);
    pbWriteVarint(dataMsg, 1);
    pbWriteTag(dataMsg, 2, 2);
    pbWriteVarint(dataMsg, text.length());
    for (size_t i = 0; i < text.length(); i++) dataMsg.push_back((uint8_t)text[i]);

    std::vector<uint8_t> meshPkt;
    pbWriteTag(meshPkt, 2, 5);
    pbWriteFixed32(meshPkt, 0xFFFFFFFF);
    pbWriteTag(meshPkt, 4, 2);
    pbWriteVarint(meshPkt, dataMsg.size());
    meshPkt.insert(meshPkt.end(), dataMsg.begin(), dataMsg.end());
    pbWriteTag(meshPkt, 6, 5);
    pbWriteFixed32(meshPkt, esp_random());
    pbWriteTag(meshPkt, 9, 0);
    pbWriteVarint(meshPkt, 3);
    pbWriteTag(meshPkt, 10, 0);
    pbWriteVarint(meshPkt, 1);

    std::vector<uint8_t> toRadio;
    pbWriteTag(toRadio, 1, 2);
    pbWriteVarint(toRadio, meshPkt.size());
    toRadio.insert(toRadio.end(), meshPkt.begin(), meshPkt.end());

#if defined(MESH_USE_RAW_NIMBLE)
    nimbleWriteHandle(meshToRadioHandle, toRadio.data(), toRadio.size());
#else
    pToRadioChar->writeValue(toRadio.data(), toRadio.size(), true);
#endif
    addChatMessage("ME", text, true);
    playTone(880, 70);
#else
    (void)text;
    (void)pbWriteTag;
    (void)pbWriteFixed32;
#endif
}

static void drainFromRadio() {
#if defined(MESH_USE_RAW_NIMBLE)
    if (!meshConnected || meshFromRadioHandle == 0) return;
    int guard = 0;
    bool gotAny = false;
    std::vector<uint8_t> raw;
    while (guard < 40 && meshConnected) {
        if (!nimbleReadFromRadio(raw) || raw.empty()) break;
        parseFromRadioPb(raw.data(), raw.size());
        gotAny = true;
        guard++;
    }
    if (gotAny && currentState == STATE_MESHTASTIC) {
        drawMeshtastic();
    }
#elif defined(MESH_BLE_AVAILABLE)
    if (!meshConnected || !pFromRadioChar) return;
    int guard = 0;
    bool gotAny = false;
    while (guard < 40 && meshConnected) {
        String raw = pFromRadioChar->readValue();
        if (raw.length() == 0) break;
        parseFromRadioPb((const uint8_t*)raw.c_str(), raw.length());
        gotAny = true;
        guard++;
    }
    if (gotAny && currentState == STATE_MESHTASTIC) {
        drawMeshtastic();
    }
#else
    (void)parseFromRadioPb;
#endif
}

static void clearScanList() {
    for (int i = 0; i < MAX_SCANNED; i++) {
#if defined(MESH_BLE_AVAILABLE)
        if (scannedDevs[i]) { delete scannedDevs[i]; scannedDevs[i] = nullptr; }
#endif
        scannedNames[i] = "";
        scannedAddrs[i] = "";
        scannedIsMesh[i] = false;
        scannedRssi[i] = -127;
    }
    scannedCount = 0;
}

#if defined(MESH_BLE_AVAILABLE)
static bool hasMeshtasticNamePattern(const String &dName) {
    if (dName.length() == 0) return false;
    String lower = dName;
    lower.toLowerCase();
    if (lower.indexOf("mesh") >= 0 || lower.indexOf("t114") >= 0 ||
        lower.indexOf("heltec") >= 0 || lower.indexOf("rak") >= 0 ||
        lower.indexOf("t-beam") >= 0 || lower.indexOf("tbeam") >= 0 ||
        lower.indexOf("t-echo") >= 0 || lower.indexOf("techo") >= 0) {
        return true;
    }
    // Meshtastic default BLE name format: "<ShortName>_XXXX" (4 hex digits)
    int len = lower.length();
    if (len >= 6 && lower[len - 5] == '_') {
        bool allHex = true;
        for (int k = len - 4; k < len; k++) {
            char c = lower[k];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
                allHex = false;
                break;
            }
        }
        if (allHex) return true;
    }
    return false;
}

static bool isMeshtasticAdvertised(BLEAdvertisedDevice &dev) {
    if (dev.haveServiceUUID()) {
        if (dev.isAdvertisingService(BLEUUID(MESH_SERVICE_UUID))) return true;
        int uCount = dev.getServiceUUIDCount();
        for (int u = 0; u < uCount; u++) {
            if (dev.getServiceUUID(u).toString().equalsIgnoreCase(MESH_SERVICE_UUID)) {
                return true;
            }
        }
    }
    return hasMeshtasticNamePattern(dev.getName());
}

static String getHostedDiagStr() {
#if defined(CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE)
    uint32_t hMaj = 0, hMin = 0, hPat = 0;
    uint32_t sMaj = 0, sMin = 0, sPat = 0;
    hostedGetHostVersion(&hMaj, &hMin, &hPat);
    hostedGetSlaveVersion(&sMaj, &sMin, &sPat);
    return "C6:" + String(sMaj) + "." + String(sMin) + "." + String(sPat) +
           " H:" + String(hMaj) + "." + String(hMin) + "." + String(hPat) +
           " BT:" + String(hostedIsBLEActive() ? "ON" : "OFF");
#else
    return "BLE:" + String(BLEDevice::getInitialized() ? "ON" : "OFF");
#endif
}

static bool isC6Outdated() {
#if defined(CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE)
    if (!hostedIsInitialized()) return false;
    uint32_t hMaj = 0, hMin = 0, hPat = 0;
    uint32_t sMaj = 0, sMin = 0, sPat = 0;
    hostedGetHostVersion(&hMaj, &hMin, &hPat);
    hostedGetSlaveVersion(&sMaj, &sMin, &sPat);
    if (sMaj == 0 && sMin == 0 && sPat == 0) return false;
    uint32_t hVer = (hMaj << 16) | (hMin << 8) | hPat;
    uint32_t sVer = (sMaj << 16) | (sMin << 8) | sPat;
    return hVer > sVer;
#else
    return false;
#endif
}

static void drawC6OtaProgress(const String &msg, int pct, uint16_t barCol) {
    M5.Display.fillRoundRect(40, 290, sw - 80, 320, 14, C_DARK);
    M5.Display.drawRoundRect(40, 290, sw - 80, 320, 14, C_ORANGE);
    M5.Display.setTextDatum(middle_center);
    M5.Display.setFont(&fonts::FreeSansBold18pt7b);
    M5.Display.setTextColor(C_GOLD);
    M5.Display.drawString("UPDATING ESP32-C6", sw / 2, 340);

    M5.Display.setFont(&fonts::FreeSansBold12pt7b);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.drawString(msg.substring(0, 46), sw / 2, 405);

    int barX = 80, barY = 455, barW = sw - 160, barH = 44;
    M5.Display.fillRoundRect(barX, barY, barW, barH, 10, C_PANEL);
    M5.Display.drawRoundRect(barX, barY, barW, barH, 10, C_GRAY);
    if (pct > 0) {
        int fillW = (barW - 6) * constrain(pct, 0, 100) / 100;
        if (fillW > 0) M5.Display.fillRoundRect(barX + 3, barY + 3, fillW, barH - 6, 8, barCol);
    }

    M5.Display.setFont(&fonts::FreeSans9pt7b);
    M5.Display.setTextColor(C_ORANGE);
    M5.Display.drawString("DO NOT POWER OFF TAB5!", sw / 2, 550);
}

static void runC6FirmwareUpdate() {
#if defined(CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE)
    c6OtaError = "";
    bleStatusText = "Preparing C6 OTA update...";
    drawMeshtastic();
    drawC6OtaProgress("Checking Wi-Fi connection...", 5, C_CYAN);

    if (WiFi.status() != WL_CONNECTED) {
        connectToWiFi();
    }
    if (WiFi.status() != WL_CONNECTED) {
        c6OtaError = "Wi-Fi offline! Connect in Settings first.";
        bleStatusText = c6OtaError;
        playTone(220, 250);
        drawMeshtastic();
        return;
    }

    // Try the exact SDK URL from hostedGetUpdateURL() first, then fall back to published Espressif binaries
    const char* urls[4] = {
        hostedGetUpdateURL(),
        "https://espressif.github.io/arduino-esp32/hosted/esp32c6-v2.11.6.bin",
        "https://espressif.github.io/arduino-esp32/hosted/esp32c6-v2.9.6.bin",
        "https://espressif.github.io/arduino-esp32/hosted/esp32c6-v2.12.13.bin"
    };

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient https;
    https.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    https.setTimeout(15000);

    int httpCode = -1;
    String activeUrl = "";
    for (int attempt = 0; attempt < 4; attempt++) {
        activeUrl = String(urls[attempt]);
        Serial.println("[C6-OTA] Trying URL: " + activeUrl);
        drawC6OtaProgress("Connecting to firmware server...", 10 + attempt * 3, C_CYAN);
        https.end();
        if (!https.begin(client, activeUrl)) {
            c6OtaError = "HTTPS begin failed";
            continue;
        }
        httpCode = https.GET();
        Serial.printf("[C6-OTA] HTTP status: %d\n", httpCode);
        if (httpCode == HTTP_CODE_OK) break;
    }

    if (httpCode != HTTP_CODE_OK) {
        c6OtaError = "Download failed (HTTP " + String(httpCode) + ")";
        bleStatusText = c6OtaError;
        https.end();
        playTone(220, 300);
        drawMeshtastic();
        return;
    }

    int totalLen = https.getSize();
    Serial.printf("[C6-OTA] Firmware size: %d bytes\n", totalLen);
    if (totalLen < 100000) {
        c6OtaError = "Invalid firmware size: " + String(totalLen);
        bleStatusText = c6OtaError;
        https.end();
        playTone(220, 300);
        drawMeshtastic();
        return;
    }

    drawC6OtaProgress("Initializing C6 OTA partition...", 15, C_ORANGE);
    if (!hostedBeginUpdate()) {
        c6OtaError = "hostedBeginUpdate() failed on C6";
        bleStatusText = c6OtaError;
        https.end();
        playTone(220, 300);
        drawMeshtastic();
        return;
    }

    static const size_t OTA_BUF = 2048;
    uint8_t* buff = (uint8_t*)malloc(OTA_BUF);
    if (!buff) {
        c6OtaError = "Out of memory for OTA buffer";
        bleStatusText = c6OtaError;
        https.end();
        playTone(220, 300);
        drawMeshtastic();
        return;
    }

    WiFiClient* stream = https.getStreamPtr();
    int remaining = totalLen;
    int written = 0;
    int lastPct = -1;
    unsigned long lastDataMs = millis();

    while (remaining > 0) {
        lastActivityTime = millis();
        size_t avail = stream->available();
        if (avail > 0) {
            size_t toRead = avail;
            if (toRead > OTA_BUF) toRead = OTA_BUF;
            if ((int)toRead > remaining) toRead = (size_t)remaining;
            int n = stream->readBytes(buff, toRead);
            if (n <= 0) {
                c6OtaError = "Stream read error at " + String(written / 1024) + " KB";
                break;
            }
            if (!hostedWriteUpdate(buff, (uint32_t)n)) {
                c6OtaError = "C6 write failed at " + String(written / 1024) + " KB";
                break;
            }
            remaining -= n;
            written += n;
            lastDataMs = millis();

            int pct = (int)((int64_t)written * 100 / totalLen);
            if (pct >= lastPct + 2 || remaining == 0) {
                lastPct = pct;
                drawC6OtaProgress("Flashing C6: " + String(pct) + "% (" +
                                  String(written / 1024) + "/" + String(totalLen / 1024) + " KB)",
                                  pct, C_GREEN);
            }
        } else {
            if (!https.connected()) {
                c6OtaError = "Connection dropped at " + String(written / 1024) + " KB";
                break;
            }
            if (millis() - lastDataMs > 15000UL) {
                c6OtaError = "Download timeout at " + String(written / 1024) + " KB";
                break;
            }
            delay(2);
        }
    }

    free(buff);
    https.end();

    if (remaining == 0 && c6OtaError.length() == 0) {
        drawC6OtaProgress("Finalizing C6 firmware...", 100, C_GREEN);
        if (!hostedEndUpdate()) {
            c6OtaError = "hostedEndUpdate() failed!";
        } else {
            drawC6OtaProgress("Activating & rebooting Tab5...", 100, C_GREEN);
            hostedActivateUpdate();
            bleStatusText = "C6 updated! Rebooting...";
            playTone(880, 150);
            delay(1500);
            ESP.restart();
            return;
        }
    }

    bleStatusText = c6OtaError;
    Serial.println("[C6-OTA] ERROR: " + c6OtaError);
    playTone(220, 300);
    drawMeshtastic();
#endif
}
#endif

static void scanMeshtasticBLE() {
#if defined(MESH_BLE_AVAILABLE)
    bleStatusText = "Scanning BLE (6s)...";
    drawMeshtastic();

    if (!bleInitialized || !BLEDevice::getInitialized()) {
        Serial.println("[MeshBLE] Initializing BLEDevice...");
        BLEDevice::init("Tab5_RubyHub");
        if (!BLEDevice::getInitialized()) {
            bleStatusText = "BLE init FAILED! " + getHostedDiagStr();
            Serial.println("[MeshBLE] " + bleStatusText);
            playTone(220, 250);
            drawMeshtastic();
            return;
        }
        bleInitialized = true;
        BLEDevice::setMTU(256);
#if defined(MESH_USE_RAW_NIMBLE)
        ble_hs_cfg.sm_io_cap = BLE_HS_IO_KEYBOARD_DISPLAY;
        ble_hs_cfg.sm_bonding = 1;
        ble_hs_cfg.sm_mitm = 0;
        ble_hs_cfg.sm_sc = 1;
        ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
        ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
#endif
        Serial.println("[MeshBLE] BLE initialized OK: " + getHostedDiagStr());
    }

    clearScanList();
    BLEScan* pScan = BLEDevice::getScan();
    pScan->setActiveScan(true);
#if defined(CONFIG_NIMBLE_ENABLED) || defined(CONFIG_BT_NIMBLE_ENABLED)
    // Disable duplicate filtering so scan responses (name / 128-bit UUID) are merged reliably
    pScan->setDuplicateFilter(false);
#endif
    // 50% duty cycle gives reliable Wi-Fi + BLE coexistence on the ESP32-C6 coprocessor
    pScan->setInterval(100);
    pScan->setWindow(50);
    BLEScanResults* results = pScan->start(6, false);

    struct ScanEntry {
        int idx;
        int rssi;
        bool isMesh;
        bool hasName;
    };
    std::vector<ScanEntry> entries;
    int total = 0;
    int meshMatches = 0;

    if (results) {
        total = results->getCount();
        Serial.printf("[MeshBLE] Scan complete. Total BLE devices seen: %d (%s)\n",
                      total, getHostedDiagStr().c_str());
        for (int i = 0; i < total; i++) {
            BLEAdvertisedDevice dev = results->getDevice(i);
            String dName = dev.getName();
            bool isMesh = isMeshtasticAdvertised(dev);
            if (isMesh) meshMatches++;
            Serial.printf("  [%d] %s | %s | RSSI=%d | addrType=%u | uuids=%d | mesh=%d\n",
                          i, dName.length() ? dName.c_str() : "(unnamed)",
                          dev.getAddress().toString().c_str(), dev.getRSSI(),
                          (unsigned)dev.getAddressType(), dev.getServiceUUIDCount(), isMesh ? 1 : 0);
            entries.push_back({i, dev.getRSSI(), isMesh, dName.length() > 0});
        }

        // Sort: Meshtastic matches first, then named devices, then strongest RSSI
        for (size_t a = 0; a < entries.size(); a++) {
            for (size_t b = a + 1; b < entries.size(); b++) {
                bool swapIt = false;
                if (entries[b].isMesh != entries[a].isMesh) {
                    swapIt = entries[b].isMesh;
                } else if (entries[b].hasName != entries[a].hasName) {
                    swapIt = entries[b].hasName;
                } else if (entries[b].rssi > entries[a].rssi) {
                    swapIt = true;
                }
                if (swapIt) {
                    ScanEntry tmp = entries[a];
                    entries[a] = entries[b];
                    entries[b] = tmp;
                }
            }
        }

        for (size_t k = 0; k < entries.size() && scannedCount < MAX_SCANNED; k++) {
            BLEAdvertisedDevice dev = results->getDevice(entries[k].idx);
            String dName = dev.getName();
            scannedDevs[scannedCount] = new BLEAdvertisedDevice(dev);
            scannedIsMesh[scannedCount] = entries[k].isMesh;
            scannedRssi[scannedCount] = entries[k].rssi;
            if (entries[k].isMesh) {
                scannedNames[scannedCount] = "[MESH] " + (dName.length() ? dName : String("Meshtastic Node"));
            } else {
                scannedNames[scannedCount] = dName.length() ? dName : String("(Unnamed BLE)");
            }
            scannedAddrs[scannedCount] = dev.getAddress().toString();
            scannedCount++;
        }
    }
    pScan->clearResults();

    if (meshMatches > 0) {
        bleStatusText = "Found " + String(meshMatches) + " Mesh (" + String(total) + " BLE). Tap below!";
        playTone(784, 80);
    } else if (total > 0) {
        bleStatusText = "0 Mesh (" + String(total) + " BLE seen). Showing nearest:";
        playTone(587, 80);
    } else {
        bleStatusText = "0 BLE seen (" + getHostedDiagStr() + ")";
        playTone(220, 150);
    }
    drawMeshtastic();
#else
    (void)bleInitialized;
    (void)clearScanList;
    (void)MESH_SERVICE_UUID;
    (void)MESH_TORADIO_UUID;
    (void)MESH_FROMRADIO_UUID;
    (void)MESH_FROMNUM_UUID;
    bleStatusText = getBtSdkFlags();
    Serial.println(bleStatusText);
    playTone(220, 150);
    drawMeshtastic();
#endif
}

static void disconnectMeshtastic(bool keepStatus = false) {
#if defined(MESH_USE_RAW_NIMBLE)
    if (meshConnHandle != BLE_HS_CONN_HANDLE_NONE) {
        meshIntentionalDisconnect = true;
        ble_gap_terminate(meshConnHandle, BLE_ERR_REM_USER_CONN_TERM);
        delay(150);
        meshConnHandle = BLE_HS_CONN_HANDLE_NONE;
        meshIntentionalDisconnect = false;
    }
    meshSvcStart = 0;
    meshSvcEnd = 0;
    meshToRadioHandle = 0;
    meshFromRadioHandle = 0;
    meshFromNumHandle = 0;
    meshFromNumCccdHandle = 0;
#elif defined(MESH_BLE_AVAILABLE)
    if (pMeshClient && pMeshClient->isConnected()) {
        pMeshClient->disconnect();
    }
    pToRadioChar = nullptr;
    pFromRadioChar = nullptr;
    pFromNumChar = nullptr;
#endif
    meshConnected = false;
    connectedDevName = "";
    if (!keepStatus) bleStatusText = "Disconnected.";
}

static bool connectToScannedNode(int idx) {
#if defined(MESH_USE_RAW_NIMBLE)
    if (idx < 0 || idx >= scannedCount || !scannedDevs[idx]) return false;
    disconnectMeshtastic();

    BLEScan* pScan = BLEDevice::getScan();
    if (pScan) pScan->stop();
    ble_gap_disc_cancel();
    delay(120);

    String cleanName = scannedNames[idx];
    if (cleanName.startsWith("[MESH] ")) cleanName = cleanName.substring(7);

    bleStatusText = "Connecting to " + cleanName + "...";
    drawMeshtastic();

    ble_addr_t peerAddr;
    memcpy(peerAddr.val, scannedDevs[idx]->getAddress().getNative(), 6);
    peerAddr.type = scannedDevs[idx]->getAddress().getType();

    uint8_t ownAddrType = BLE_OWN_ADDR_PUBLIC;
    ble_hs_id_infer_auto(0, &ownAddrType);

    struct ble_gap_conn_params connParams = {};
    connParams.scan_itvl = 160;           // 100 ms
    connParams.scan_window = 80;          // 50 ms (clean Wi-Fi/BLE coexistence on C6)
    connParams.itvl_min = 40;             // 50 ms (extra breathing room for ESP-Hosted)
    connParams.itvl_max = 60;             // 75 ms
    connParams.latency = 0;
    connParams.supervision_timeout = 600; // 6000 ms
    connParams.min_ce_len = 0;
    connParams.max_ce_len = 0;

    initMeshNativeUuids();
    meshIntentionalDisconnect = false;

    unsigned long connStartMs = millis();
    bool linked = false;

    for (int attempt = 1; attempt <= 2 && !linked; attempt++) {
        if (attempt > 1) {
            bleStatusText = "Retrying " + cleanName + " (2/2)...";
            drawMeshtastic();
            delay(300);
        }
        lastGapConnStatus = -1;
        lastGapDiscReason = -1;
        meshOpDone = false;
        meshOpStatus = -1;

        int rc = ble_gap_connect(ownAddrType, &peerAddr, 10000, &connParams, meshClientGapCb, nullptr);
        if (rc == BLE_HS_EBUSY) {
            ble_gap_disc_cancel();
            delay(100);
            rc = ble_gap_connect(ownAddrType, &peerAddr, 10000, &connParams, meshClientGapCb, nullptr);
        }
        if (rc != 0) {
            Serial.printf("[MeshBLE] ble_gap_connect rc=%d\n", rc);
            lastGapConnStatus = rc;
            continue;
        }

        if (waitNimbleOp(11000) && meshOpStatus == 0 && meshConnHandle != BLE_HS_CONN_HANDLE_NONE) {
            linked = true;
        } else {
            ble_gap_conn_cancel();
        }
    }

    if (!linked) {
        unsigned long dt = (millis() - connStartMs) / 1000UL;
        bleStatusText = "BLE connect fail (c=" + String(lastGapConnStatus) + " d=" + String(lastGapDiscReason) + " " + String(dt) + "s)";
        Serial.println("[MeshBLE] " + bleStatusText);
        playTone(220, 200);
        drawMeshtastic();
        return false;
    }

    // Let nRF52840 settle before GATT discovery (skip active MTU exchange to avoid ATT stalls)
    delay(250);
    ble_att_set_preferred_mtu(256);

    if (meshConnHandle == BLE_HS_CONN_HANDLE_NONE) {
        bleStatusText = "Dropped after connect (d=" + String(lastGapDiscReason) + ")";
        playTone(220, 200);
        drawMeshtastic();
        return false;
    }

    // Discover Meshtastic GATT service by UUID first (1 single ATT transaction over SDIO)
    bleStatusText = "Discovering Meshtastic service...";
    drawMeshtastic();
    meshSvcSeenCount = 0;
    meshSvcStart = 0;
    meshSvcEnd = 0;
    meshOpDone = false;
    meshOpStatus = -1;
    int rc = ble_gattc_disc_svc_by_uuid(meshConnHandle, &meshSvcUuidNative.u, meshSvcDiscCb, nullptr);
    bool discOk = (rc == 0) && waitNimbleOp(8000);

    // If targeted lookup timed out or returned empty while still connected, try disc_all_svcs
    if ((meshSvcStart == 0 || meshSvcEnd == 0) && meshConnHandle != BLE_HS_CONN_HANDLE_NONE) {
        meshOpDone = false;
        meshOpStatus = -1;
        rc = ble_gattc_disc_all_svcs(meshConnHandle, meshSvcDiscCb, nullptr);
        discOk = (rc == 0) && waitNimbleOp(8000);
    }

    if (meshSvcStart == 0 || meshSvcEnd == 0) {
        bleStatusText = "Svc err (rc=" + String(rc) + " st=" + String(meshOpStatus) +
                        " n=" + String((int)meshSvcSeenCount) + " ok=" + String(discOk ? 1 : 0) +
                        " d=" + String(lastGapDiscReason) + ")";
        Serial.println("[MeshBLE] " + bleStatusText);
        disconnectMeshtastic(true);
        playTone(220, 200);
        drawMeshtastic();
        return false;
    }

    // Discover ToRadio / FromRadio / FromNum characteristics
    bleStatusText = "Discovering characteristics...";
    drawMeshtastic();
    meshToRadioHandle = 0;
    meshFromRadioHandle = 0;
    meshFromNumHandle = 0;
    meshFromNumCccdHandle = 0;
    meshOpDone = false;
    meshOpStatus = -1;
    rc = ble_gattc_disc_all_chrs(meshConnHandle, meshSvcStart, meshSvcEnd, meshChrDiscCb, nullptr);
    bool chrOk = (rc == 0) && waitNimbleOp(8000);

    if (meshToRadioHandle == 0 || meshFromRadioHandle == 0) {
        bleStatusText = "Chr err (rc=" + String(rc) + " st=" + String(meshOpStatus) +
                        " ok=" + String(chrOk ? 1 : 0) + " d=" + String(lastGapDiscReason) + ")";
        Serial.println("[MeshBLE] " + bleStatusText);
        disconnectMeshtastic(true);
        playTone(220, 200);
        drawMeshtastic();
        return false;
    }

    // Enable notifications on FromNum (if found)
    if (meshFromNumHandle != 0 && meshFromNumHandle < meshSvcEnd) {
        meshOpDone = false;
        if (ble_gattc_disc_all_dscs(meshConnHandle, meshFromNumHandle, meshSvcEnd, meshDscDiscCb, nullptr) == 0) {
            waitNimbleOp(3000);
        }
        if (meshFromNumCccdHandle != 0) {
            uint8_t cccdVal[2] = {0x01, 0x00};
            nimbleWriteHandle(meshFromNumCccdHandle, cccdVal, 2);
        }
    }

    meshConnected = true;
    connectedDevName = cleanName;
    bleStatusText = "Connected! Syncing NodeDB...";
    playTone(880, 100);
    drawMeshtastic();

    sendWantConfig();
    delay(250);
    drainFromRadio();
    bleStatusText = "Linked: " + connectedDevName + " (" + String(meshNodeCount) + " nodes)";
    drawMeshtastic();
    return true;
#elif defined(MESH_BLE_AVAILABLE)
    if (idx < 0 || idx >= scannedCount || !scannedDevs[idx]) return false;
    disconnectMeshtastic();

    String cleanName = scannedNames[idx];
    if (cleanName.startsWith("[MESH] ")) cleanName = cleanName.substring(7);

    bleStatusText = "Connecting to " + cleanName + "...";
    drawMeshtastic();

    if (!pMeshClient) {
        pMeshClient = BLEDevice::createClient();
        pMeshClient->setClientCallbacks(new MeshClientCallbacks());
    }
    if (!pMeshClient->connect(scannedDevs[idx])) {
        bleStatusText = "BLE connect failed! Set T114 Pairing = No PIN.";
        playTone(220, 200);
        drawMeshtastic();
        return false;
    }

    pMeshClient->setMTU(256);
    BLERemoteService* pService = pMeshClient->getService(BLEUUID(MESH_SERVICE_UUID));
    if (!pService) {
        bleStatusText = "Meshtastic GATT service not found.";
        pMeshClient->disconnect();
        drawMeshtastic();
        return false;
    }

    pToRadioChar   = pService->getCharacteristic(BLEUUID(MESH_TORADIO_UUID));
    pFromRadioChar = pService->getCharacteristic(BLEUUID(MESH_FROMRADIO_UUID));
    pFromNumChar   = pService->getCharacteristic(BLEUUID(MESH_FROMNUM_UUID));

    if (!pToRadioChar || !pFromRadioChar) {
        bleStatusText = "Missing ToRadio/FromRadio characteristics.";
        pMeshClient->disconnect();
        drawMeshtastic();
        return false;
    }

    if (pFromNumChar && pFromNumChar->canNotify()) {
        pFromNumChar->registerForNotify(fromNumNotifyCallback);
    }

    meshConnected = true;
    connectedDevName = cleanName;
    bleStatusText = "Connected! Syncing NodeDB...";
    playTone(880, 100);
    drawMeshtastic();

    sendWantConfig();
    delay(250);
    drainFromRadio();
    bleStatusText = "Linked: " + connectedDevName + " (" + String(meshNodeCount) + " nodes)";
    drawMeshtastic();
    return true;
#else
    (void)idx;
    return false;
#endif
}

// ======================== MESHTASTIC UI ===============================
static const char* mKb[3][10] = {
    {"q","w","e","r","t","y","u","i","o","p"},
    {"a","s","d","f","g","h","j","k","l","?"},
    {"z","x","c","v","b","n","m",".","!","-"}
};

void initMeshtastic() {
#if !defined(MESH_BLE_AVAILABLE)
    bleStatusText = getBtSdkFlags();
    Serial.println(bleStatusText);
#endif
    drawMeshtastic();
}

void drawMeshtastic() {
    M5.Display.fillScreen(C_DARK);
    M5.Display.setTextDatum(middle_center);

    // Top Bar
    M5.Display.fillRect(0, 0, sw, 90, 0x0862);
    M5.Display.fillRoundRect(20, 15, 150, 60, 14, C_PANEL);
    M5.Display.setFont(&fonts::FreeSansBold12pt7b);
    M5.Display.setTextColor(C_GOLD);
    M5.Display.drawString("< HUB", 95, 45);

    M5.Display.setFont(&fonts::FreeSansBold18pt7b);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.drawString("MESHTASTIC", sw / 2, 45);

    M5.Display.fillRoundRect(sw - 180, 15, 160, 60, 14, meshConnected ? C_RED : C_GREEN);
    M5.Display.setFont(&fonts::FreeSansBold12pt7b);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.drawString(meshConnected ? "DISCONN" : "SCAN BLE", sw - 100, 45);

    // Status Strip
    M5.Display.fillRoundRect(20, 102, sw - 40, 72, 14, C_PANEL);
    M5.Display.fillCircle(48, 138, 10, meshConnected ? C_GREEN : C_RED);
    M5.Display.setTextDatum(middle_left);
    M5.Display.setFont(bleStatusText.length() > 36 ? &fonts::FreeSans9pt7b : &fonts::FreeSansBold12pt7b);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.drawString(bleStatusText.substring(0, 54), 70, 124);

    M5.Display.setFont(&fonts::FreeSans9pt7b);
    M5.Display.setTextColor(C_CYAN);
    if (meshGps.valid) {
        String g = "T114 GPS: " + String(meshGps.lat, 5) + ", " + String(meshGps.lon, 5) +
                   " | Alt " + String(meshGps.altMeters) + "m | Sats: " + String(meshGps.sats);
        M5.Display.drawString(g, 70, 154);
    } else {
        M5.Display.drawString("T114 GPS: Waiting for satellite lock from node...", 70, 154);
    }
    M5.Display.setTextDatum(middle_center);

    // ================= TAB 0: CHAT =================
    if (meshTab == 0) {
        M5.Display.fillRoundRect(20, 188, sw - 40, 460, 16, C_PANEL);
        M5.Display.setTextDatum(middle_left);
        if (chatCount == 0) {
            M5.Display.setTextDatum(middle_center);
            M5.Display.setFont(&fonts::FreeSans12pt7b);
            M5.Display.setTextColor(C_GRAY);
            M5.Display.drawString("No LoRa messages yet.", sw / 2, 415);
        } else {
            int startIdx = (chatCount > 7) ? (chatCount - 7) : 0;
            for (int i = startIdx; i < chatCount; i++) {
                int row = i - startIdx;
                int y = 225 + row * 60;
                uint16_t col = chatLog[i].outgoing ? C_GOLD : C_CYAN;
                M5.Display.setFont(&fonts::FreeSansBold12pt7b);
                M5.Display.setTextColor(col);
                String line = "[" + chatLog[i].sender + "] " + chatLog[i].text;
                if (line.length() > 42) line = line.substring(0, 40) + "..";
                M5.Display.drawString(line, 38, y);
            }
        }

        M5.Display.fillRoundRect(20, 662, 520, 70, 14, TFT_WHITE);
        M5.Display.setTextDatum(middle_left);
        M5.Display.setFont(&fonts::FreeSansBold12pt7b);
        M5.Display.setTextColor(0x1082);
        M5.Display.drawString(chatDraft + "_", 38, 697);

        M5.Display.setTextDatum(middle_center);
        M5.Display.fillRoundRect(555, 662, 145, 70, 14, meshConnected ? C_GREEN : 0x528A);
        M5.Display.setTextColor(TFT_WHITE);
        M5.Display.drawString("SEND", 627, 697);

        int startY = 748, keyW = 62, keyH = 74, gapX = 6, gapY = 12;
        int startX = (sw - (10 * keyW + 9 * gapX)) / 2;
        M5.Display.setFont(&fonts::FreeSansBold18pt7b);
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 10; c++) {
                int kx = startX + c * (keyW + gapX);
                int ky = startY + r * (keyH + gapY);
                M5.Display.fillRoundRect(kx, ky, keyW, keyH, 10, C_PANEL);
                M5.Display.drawRoundRect(kx, ky, keyW, keyH, 10, C_CYAN);
                M5.Display.setTextColor(TFT_WHITE);
                String ch = mKb[r][c];
                if (chatCaps && ch[0] >= 'a' && ch[0] <= 'z') ch.toUpperCase();
                M5.Display.drawString(ch, kx + keyW / 2, ky + keyH / 2);
            }
        }
        int bY = startY + 3 * (keyH + gapY);
        M5.Display.setFont(&fonts::FreeSansBold12pt7b);
        M5.Display.fillRoundRect(startX, bY, 150, keyH, 10, chatCaps ? C_ORANGE : C_BLUE);
        M5.Display.setTextColor(TFT_WHITE); M5.Display.drawString("CAPS", startX + 75, bY + keyH / 2);

        M5.Display.fillRoundRect(startX + 165, bY, 340, keyH, 10, C_PANEL);
        M5.Display.drawRoundRect(startX + 165, bY, 340, keyH, 10, C_CYAN);
        M5.Display.drawString("SPACE", startX + 335, bY + keyH / 2);

        M5.Display.fillRoundRect(startX + 520, bY, 154, keyH, 10, C_RED);
        M5.Display.drawString("DEL", startX + 597, bY + keyH / 2);
    }

    // ================= TAB 1: NODES =================
    else if (meshTab == 1) {
        M5.Display.fillRoundRect(20, 188, sw - 40, 955, 16, C_PANEL);
        M5.Display.setFont(&fonts::FreeSansBold18pt7b);
        M5.Display.setTextColor(C_GOLD);
        M5.Display.drawString("MESH NODES (" + String(meshNodeCount) + ")", sw / 2, 228);

        if (meshNodeCount == 0) {
            M5.Display.setFont(&fonts::FreeSans12pt7b);
            M5.Display.setTextColor(C_GRAY);
            M5.Display.drawString("Connect to T114 V2 to load NodeDB.", sw / 2, 560);
        } else {
            M5.Display.setTextDatum(middle_left);
            int maxShow = (meshNodeCount < 9) ? meshNodeCount : 9;
            for (int i = 0; i < maxShow; i++) {
                int y = 275 + i * 92;
                M5.Display.fillRoundRect(38, y, sw - 76, 80, 12, C_DARK);
                M5.Display.setFont(&fonts::FreeSansBold12pt7b);
                bool isMe = (meshNodes[i].nodeNum == myNodeNum && myNodeNum != 0);
                M5.Display.setTextColor(isMe ? C_GREEN : TFT_WHITE);
                String hdr = "[" + meshNodes[i].shortName + "] " + meshNodes[i].longName + (isMe ? " (YOU)" : "");
                M5.Display.drawString(hdr.substring(0, 34), 56, y + 26);

                M5.Display.setFont(&fonts::FreeSans9pt7b);
                M5.Display.setTextColor(C_CYAN);
                String sub = meshNodes[i].hasPos
                    ? ("GPS: " + String(meshNodes[i].lat, 5) + ", " + String(meshNodes[i].lon, 5))
                    : "No GPS position";
                sub += "  |  SNR: " + String(meshNodes[i].snr, 1) + " dB";
                M5.Display.drawString(sub, 56, y + 58);
            }
            M5.Display.setTextDatum(middle_center);
        }
    }

    // ================= TAB 2: BLE & GPS =================
    else {
        M5.Display.fillRoundRect(20, 188, sw - 40, 480, 16, C_PANEL);
        M5.Display.setFont(&fonts::FreeSansBold18pt7b);
        M5.Display.setTextColor(C_GOLD);
        M5.Display.drawString("BLUETOOTH DEVICES (T114 V2)", sw / 2, 228);

        if (scannedCount == 0) {
            M5.Display.setFont(&fonts::FreeSans12pt7b);
            M5.Display.setTextColor(C_GRAY);
#if defined(MESH_BLE_AVAILABLE)
            if (isC6Outdated()) {
                M5.Display.setTextColor(C_ORANGE);
                M5.Display.setFont(&fonts::FreeSansBold12pt7b);
                M5.Display.drawString("ESP32-C6 Firmware Outdated!", sw / 2, 315);
                M5.Display.setFont(&fonts::FreeSans12pt7b);
                M5.Display.setTextColor(TFT_WHITE);
                M5.Display.drawString(getHostedDiagStr(), sw / 2, 365);
                M5.Display.setFont(&fonts::FreeSans9pt7b);
                M5.Display.setTextColor(C_GRAY);
                M5.Display.drawString("BLE requires updating the Tab5's onboard C6 chip.", sw / 2, 410);

                M5.Display.fillRoundRect(110, 450, 500, 84, 16, C_ORANGE);
                M5.Display.setFont(&fonts::FreeSansBold18pt7b);
                M5.Display.setTextColor(TFT_WHITE);
                M5.Display.drawString("UPDATE C6 FIRMWARE", sw / 2, 492);

                M5.Display.setFont(&fonts::FreeSans9pt7b);
                if (c6OtaError.length() > 0) {
                    M5.Display.setTextColor(C_RED);
                    M5.Display.drawString("ERR: " + c6OtaError.substring(0, 48), sw / 2, 570);
                } else {
                    M5.Display.setTextColor(C_GOLD);
                    M5.Display.drawString("Updates C6 to match Host over Wi-Fi (~30s, reboots)", sw / 2, 570);
                }
            } else {
                M5.Display.drawString("Tap SCAN BLE in top-right corner.", sw / 2, 390);
                M5.Display.setFont(&fonts::FreeSans9pt7b);
                M5.Display.drawString("Tip: Disconnect phone & set T114 Pairing = NO PIN.", sw / 2, 440);
            }
#else
            M5.Display.setTextColor(C_ORANGE);
            M5.Display.drawString("Bluedroid BLE not enabled in P4 SDK", sw / 2, 360);
            M5.Display.setFont(&fonts::FreeSansBold12pt7b);
            M5.Display.setTextColor(C_CYAN);
            M5.Display.drawString(getBtSdkFlags(), sw / 2, 420);
            M5.Display.setFont(&fonts::FreeSans9pt7b);
            M5.Display.setTextColor(C_GRAY);
            M5.Display.drawString("Read the flags above to pick NimBLE vs UART link.", sw / 2, 480);
#endif
        } else {
            for (int i = 0; i < scannedCount; i++) {
                int y = 268 + i * 76;
                M5.Display.fillRoundRect(40, y, sw - 80, 64, 12, scannedIsMesh[i] ? C_GREEN : C_BLUE);
                M5.Display.setTextDatum(middle_left);
                M5.Display.setFont(&fonts::FreeSansBold12pt7b);
                M5.Display.setTextColor(TFT_WHITE);
                M5.Display.drawString(scannedNames[i].substring(0, 24), 60, y + 32);
                M5.Display.setTextDatum(middle_right);
                M5.Display.setFont(&fonts::FreeSans9pt7b);
                M5.Display.drawString(String(scannedRssi[i]) + "dBm | " + scannedAddrs[i], sw - 60, y + 32);
            }
            M5.Display.setTextDatum(middle_center);
        }

        // Raw T114 GPS Telemetry Box
        M5.Display.fillRoundRect(20, 686, sw - 40, 455, 16, C_PANEL);
        M5.Display.setFont(&fonts::FreeSansBold18pt7b);
        M5.Display.setTextColor(C_CYAN);
        M5.Display.drawString("T114 GNSS TELEMETRY", sw / 2, 728);

        M5.Display.setTextDatum(middle_left);
        M5.Display.setFont(&fonts::FreeSansBold12pt7b);
        M5.Display.setTextColor(TFT_WHITE);
        M5.Display.drawString("Latitude:   " + (meshGps.valid ? String(meshGps.lat, 6) : String("--")), 50, 795);
        M5.Display.drawString("Longitude:  " + (meshGps.valid ? String(meshGps.lon, 6) : String("--")), 50, 845);
        M5.Display.drawString("Altitude:   " + (meshGps.valid ? (String(meshGps.altMeters) + " m") : String("--")), 50, 895);
        M5.Display.drawString("Course/Spd: " + (meshGps.valid ? (String(meshGps.courseDeg, 0) + " deg / " + String(meshGps.speedMps, 1) + " m/s") : String("--")), 50, 945);
        M5.Display.drawString("Satellites: " + (meshGps.valid ? String(meshGps.sats) : String("--")), 50, 995);

        M5.Display.setTextDatum(middle_center);
        M5.Display.fillRoundRect(160, 1050, 400, 68, 16, meshConnected ? C_TEAL : 0x528A);
        M5.Display.setTextColor(TFT_WHITE);
        M5.Display.drawString("REFRESH T114 POSITION", sw / 2, 1084);
    }

    // Bottom 3-Tab Navigation Bar
    const char* tabs[3] = {"CHAT", "NODES", "BLE / GPS"};
    for (int i = 0; i < 3; i++) {
        int bx = 20 + i * 232;
        bool active = (meshTab == i);
        M5.Display.fillRoundRect(bx, 1172, 216, 84, 16, active ? C_BLUE : C_PANEL);
        M5.Display.setFont(&fonts::FreeSansBold18pt7b);
        M5.Display.setTextColor(active ? TFT_WHITE : C_GRAY);
        M5.Display.drawString(tabs[i], bx + 108, 1214);
    }
}

void handleMeshtasticTouch(int tx, int ty) {
    if (ty < 90) {
        if (tx >= 20 && tx <= 170) {
            playTone(440, 60);
            currentState = STATE_MAIN_MENU;
            drawMainMenu();
            return;
        }
        if (tx >= sw - 180 && tx <= sw - 20) {
            playTone(660, 60);
            if (meshConnected) {
                disconnectMeshtastic();
                drawMeshtastic();
            } else {
                meshTab = 2;
                scanMeshtasticBLE();
            }
            return;
        }
        return;
    }

    if (ty >= 1170) {
        for (int i = 0; i < 3; i++) {
            int bx = 20 + i * 232;
            if (tx >= bx && tx <= bx + 216) {
                meshTab = i;
                playTone(523, 40);
                drawMeshtastic();
                return;
            }
        }
        return;
    }

    if (meshTab == 2) {
#if defined(MESH_BLE_AVAILABLE)
        if (scannedCount == 0 && isC6Outdated() && tx >= 60 && tx <= 660 && ty >= 430 && ty <= 560) {
            playTone(660, 80);
            runC6FirmwareUpdate();
            return;
        }
#endif
        for (int i = 0; i < scannedCount; i++) {
            int y = 268 + i * 76;
            if (tx >= 40 && tx <= sw - 40 && ty >= y && ty <= y + 64) {
                playTone(660, 50);
                connectToScannedNode(i);
                return;
            }
        }
        if (meshConnected && tx >= 160 && tx <= 560 && ty >= 1050 && ty <= 1118) {
            playTone(660, 50);
            sendWantConfig();
            delay(200);
            drainFromRadio();
            drawMeshtastic();
            return;
        }
        return;
    }

    if (meshTab == 0) {
        if (tx >= 555 && tx <= 700 && ty >= 662 && ty <= 732) {
            if (meshConnected && chatDraft.length() > 0) {
                sendMeshTextMessage(chatDraft);
                chatDraft = "";
                drawMeshtastic();
            } else {
                playTone(220, 120);
            }
            return;
        }

        int startY = 748, keyW = 62, keyH = 74, gapX = 6, gapY = 12;
        int startX = (sw - (10 * keyW + 9 * gapX)) / 2;
        for (int r = 0; r < 3; r++) {
            int ky = startY + r * (keyH + gapY);
            if (ty >= ky && ty <= ky + keyH) {
                for (int c = 0; c < 10; c++) {
                    int kx = startX + c * (keyW + gapX);
                    if (tx >= kx && tx <= kx + keyW) {
                        if (chatDraft.length() < 60) {
                            String ch = mKb[r][c];
                            if (chatCaps && ch[0] >= 'a' && ch[0] <= 'z') ch.toUpperCase();
                            chatDraft += ch;
                            playTone(523, 25);
                            drawMeshtastic();
                        }
                        return;
                    }
                }
            }
        }
        int bY = startY + 3 * (keyH + gapY);
        if (ty >= bY && ty <= bY + keyH) {
            if (tx >= startX && tx <= startX + 150) { chatCaps = !chatCaps; playTone(523, 35); drawMeshtastic(); return; }
            if (tx >= startX + 165 && tx <= startX + 505) { if (chatDraft.length() < 60) chatDraft += " "; playTone(523, 25); drawMeshtastic(); return; }
            if (tx >= startX + 520 && tx <= startX + 674) { if (chatDraft.length() > 0) chatDraft.remove(chatDraft.length() - 1); playTone(440, 35); drawMeshtastic(); return; }
        }
    }
}

void loopMeshtastic() {
    if (!meshConnected) return;
#if defined(MESH_USE_RAW_NIMBLE)
    if (meshConnHandle == BLE_HS_CONN_HANDLE_NONE) {
        meshConnected = false;
        bleStatusText = "Connection lost.";
        if (currentState == STATE_MESHTASTIC) drawMeshtastic();
        return;
    }
    if (meshNewDataFlag || millis() - lastPollMs > 4000UL) {
        meshNewDataFlag = false;
        lastPollMs = millis();
        drainFromRadio();
    }
#elif defined(MESH_BLE_AVAILABLE)
    if (pMeshClient && !pMeshClient->isConnected()) {
        meshConnected = false;
        bleStatusText = "Connection lost.";
        if (currentState == STATE_MESHTASTIC) drawMeshtastic();
        return;
    }
    if (meshNewDataFlag || millis() - lastPollMs > 4000UL) {
        meshNewDataFlag = false;
        lastPollMs = millis();
        drainFromRadio();
    }
#else
    (void)lastPollMs;
#endif
}