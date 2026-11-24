#include "CameraApp.h"
#include "MapTiles.h" // Reuses Tab5 SPI microSD init (SCK=43, MISO=39, MOSI=44, CS=42)
#include <SD.h>
#include <driver/ledc.h>
#include <esp_cam_ctlr.h>
#include <esp_cam_ctlr_csi.h>
#include <driver/isp.h>
#include <driver/jpeg_encode.h>
#include <esp_cache.h>
#include <esp_ldo_regulator.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <vector>

// ======================== SC202CS / SC2356 SENSOR =====================
// Official Espressif SC2356/SC202CS 1280x720 RAW8 30fps 1-lane 576Mbps 24MHz-input table
struct CamRegVal {
    uint16_t reg;
    uint8_t  val;
};

static const uint16_t SC202CS_REG_END        = 0xFFFF;
static const uint16_t SC202CS_REG_SLEEP_MODE = 0x0100;
static const uint8_t  SC202CS_I2C_ADDR       = 0x36;
static const uint8_t  PI4IOE_1_ADDR          = 0x43; // Tab5 IO Expander #1 (Pin 6 = CAM_EN/RST)

static const CamRegVal SC202CS_1280X720_RAW8[] = {
    {0x0103, 0x01}, {0x0100, 0x00},
    {0x36e9, 0x80}, {0x36ea, 0x06}, {0x36eb, 0x0a}, {0x36ec, 0x01}, {0x36ed, 0x18}, {0x36e9, 0x24},
    {0x301f, 0x18}, {0x3031, 0x08}, {0x3037, 0x00},
    {0x3200, 0x00}, {0x3201, 0xa0}, {0x3202, 0x00}, {0x3203, 0xf0},
    {0x3204, 0x05}, {0x3205, 0xa7}, {0x3206, 0x03}, {0x3207, 0xc7},
    {0x3208, 0x05}, {0x3209, 0x00}, {0x320a, 0x02}, {0x320b, 0xd0},
    {0x3210, 0x00}, {0x3211, 0x04}, {0x3212, 0x00}, {0x3213, 0x04},
    {0x3301, 0xff}, {0x3304, 0x68}, {0x3306, 0x40}, {0x3308, 0x08}, {0x3309, 0xa8}, {0x330b, 0xd0},
    {0x330c, 0x18}, {0x330d, 0xff}, {0x330e, 0x20}, {0x331e, 0x59}, {0x331f, 0x99}, {0x3333, 0x10},
    {0x335e, 0x06}, {0x335f, 0x08}, {0x3364, 0x1f}, {0x337c, 0x02}, {0x337d, 0x0a}, {0x338f, 0xa0},
    {0x3390, 0x01}, {0x3391, 0x03}, {0x3392, 0x1f}, {0x3393, 0xff}, {0x3394, 0xff}, {0x3395, 0xff},
    {0x33a2, 0x04}, {0x33ad, 0x0c}, {0x33b1, 0x20}, {0x33b3, 0x38}, {0x33f9, 0x40}, {0x33fb, 0x48},
    {0x33fc, 0x0f}, {0x33fd, 0x1f}, {0x349f, 0x03}, {0x34a6, 0x03}, {0x34a7, 0x1f}, {0x34a8, 0x38},
    {0x34a9, 0x30}, {0x34ab, 0xd0}, {0x34ad, 0xd8}, {0x34f8, 0x1f}, {0x34f9, 0x20},
    {0x3630, 0xa0}, {0x3631, 0x92}, {0x3632, 0x64}, {0x3633, 0x43}, {0x3637, 0x49}, {0x363a, 0x85},
    {0x363c, 0x0f}, {0x3650, 0x31}, {0x3670, 0x0d}, {0x3674, 0xc0}, {0x3675, 0xa0}, {0x3676, 0xa0},
    {0x3677, 0x92}, {0x3678, 0x96}, {0x3679, 0x9a}, {0x367c, 0x03}, {0x367d, 0x0f}, {0x367e, 0x01},
    {0x367f, 0x0f}, {0x3698, 0x83}, {0x3699, 0x86}, {0x369a, 0x8c}, {0x369b, 0x94}, {0x36a2, 0x01},
    {0x36a3, 0x03}, {0x36a4, 0x07}, {0x36ae, 0x0f}, {0x36af, 0x1f}, {0x36bd, 0x22}, {0x36be, 0x22},
    {0x36bf, 0x22}, {0x36d0, 0x01},
    {0x370f, 0x02}, {0x3721, 0x6c}, {0x3722, 0x8d}, {0x3725, 0xc5}, {0x3727, 0x14}, {0x3728, 0x04},
    {0x37b7, 0x04}, {0x37b8, 0x04}, {0x37b9, 0x06}, {0x37bd, 0x07}, {0x37be, 0x0f},
    {0x3901, 0x02}, {0x3903, 0x40}, {0x3905, 0x8d}, {0x3907, 0x00}, {0x3908, 0x41}, {0x391f, 0x41},
    {0x3933, 0x80}, {0x3934, 0x02}, {0x3937, 0x6f}, {0x393a, 0x01}, {0x393d, 0x01}, {0x393e, 0xc0},
    {0x39dd, 0x41},
    {0x3e00, 0x00}, {0x3e01, 0x4d}, {0x3e02, 0xc0}, {0x3e09, 0x00},
    {0x4509, 0x28}, {0x450d, 0x61},
    {SC202CS_REG_END, 0x00}
};

// ======================== CAMERA PIPELINE STATE =======================
static const int CAM_W = 1280;
static const int CAM_H = 720;
static const size_t CAM_FB_SIZE = (size_t)CAM_W * (size_t)CAM_H * 2; // RGB565

static const int VF_X = 20;
static const int VF_Y = 154;
static const int VF_W = 680;
static const int VF_H = 820;

static esp_cam_ctlr_handle_t    camHandle   = nullptr;
static isp_proc_handle_t        ispHandle   = nullptr;
static jpeg_encoder_handle_t    jpegHandle  = nullptr;
static esp_ldo_channel_handle_t mipiLdo     = nullptr;

// Ping-pong DMA framebuffers (exact stock M5Tab5 continuous CSI DMA pattern!)
static uint8_t*  camFb[2]    = { nullptr, nullptr };
static uint8_t*  jpgOutBuf   = nullptr;
static size_t    jpgOutCap   = 0;
static uint16_t* previewBuf  = nullptr; // VF_W x VF_H RGB565 PSRAM preview buffer

static volatile int      dmaWriteIdx  = 0;
static volatile int      dmaReadyIdx  = -1;
static volatile uint32_t dmaFrameSeq  = 0;
static uint32_t          lastShownSeq = 0;
static bool camXclkStarted = false;
static bool camHwReady   = false;
static bool camAppActive = false;
static String camErrorMsg = "";
static String camToastMsg = "";
static unsigned long camToastUntilMs = 0;

// Camera App Modes & Controls
static int  camMode      = 0; // 0 = PHOTO, 1 = VIDEO, 2 = GALLERY
static int  camRotIdx    = 1; // 0=0deg, 1=90deg (portrait), 2=180deg, 3=270deg
static int  evBias       = 0; // -3..+3 exposure compensation
static int  aeExposure   = 1100; // SC202CS shutter (valid range: 16..1240, < VTS=1250)
static int  aeGainIdx    = 2;    // Index into SC202_GAINS (0..9)
static int  aeFrameCount = 0;
static float wbGainR     = 1.25f;
static float wbGainB     = 1.45f;

// FPS counter
static unsigned long fpsWindowMs = 0;
static int fpsFrames = 0;
static int liveFps = 0;

// ======================== AVI VIDEO RECORDER ==========================
struct AviIndexEntry {
    uint32_t offset; // Relative to start of 'movi' list payload
    uint32_t size;
};

static bool     recActive    = false;
static File     recFile;
static String   recPath      = "";
static uint32_t recFrames    = 0;
static uint32_t recMoviBytes = 0;
static unsigned long recStartMs = 0;
static std::vector<AviIndexEntry> recIndex;

// ======================== GALLERY STATE ===============================
struct MediaItem {
    String path;
    String name;
    bool isVideo;
    size_t sizeBytes;
};

static std::vector<MediaItem> galleryItems;
static int galleryIdx = 0;
static bool videoPlaying = false;
static File playFile;
static uint32_t playNextFrameMs = 0;
static uint32_t playFrameIntervalMs = 90;
static uint32_t playDataEndOffset = 0;
static uint8_t* galleryBuf = nullptr;
static const size_t GALLERY_BUF_CAP = 262144; // 256 KB in PSRAM

// ======================== LOW-LEVEL I2C HELPERS =======================
static bool sc202csWriteReg(uint16_t reg, uint8_t val) {
    uint8_t buf[3] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF), val };
    if (!M5.In_I2C.start(SC202CS_I2C_ADDR, false, 100000)) return false;
    bool ok = M5.In_I2C.write(buf, 3);
    M5.In_I2C.stop();
    return ok;
}

static bool sc202csReadReg(uint16_t reg, uint8_t &val) {
    uint8_t addr[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };
    if (!M5.In_I2C.start(SC202CS_I2C_ADDR, false, 100000)) return false;
    if (!M5.In_I2C.write(addr, 2)) { M5.In_I2C.stop(); return false; }
    if (!M5.In_I2C.restart(SC202CS_I2C_ADDR, true, 100000)) { M5.In_I2C.stop(); return false; }
    bool ok = M5.In_I2C.read(&val, 1, true);
    M5.In_I2C.stop();
    return ok;
}

static void setTab5CameraPower(bool enable) {
    // PI4IOE5V6408-1 (0x43) Pin 6 = CAM_EN / CAM_RST
    uint8_t dir = 0, hiZ = 0, out = 0;
    M5.In_I2C.readRegister(PI4IOE_1_ADDR, 0x03, &dir, 1, 400000);
    M5.In_I2C.readRegister(PI4IOE_1_ADDR, 0x07, &hiZ, 1, 400000);
    M5.In_I2C.readRegister(PI4IOE_1_ADDR, 0x05, &out, 1, 400000);

    dir |= (1U << 6);  // Pin 6 as output
    hiZ &= ~(1U << 6); // Push-pull enabled
    M5.In_I2C.writeRegister(PI4IOE_1_ADDR, 0x03, &dir, 1, 400000);
    M5.In_I2C.writeRegister(PI4IOE_1_ADDR, 0x07, &hiZ, 1, 400000);

    if (enable) {
        // Clean reset pulse LOW -> HIGH
        out &= ~(1U << 6);
        M5.In_I2C.writeRegister(PI4IOE_1_ADDR, 0x05, &out, 1, 400000);
        delay(15);
        out |= (1U << 6);
        M5.In_I2C.writeRegister(PI4IOE_1_ADDR, 0x05, &out, 1, 400000);
        delay(30);
    } else {
        out &= ~(1U << 6);
        M5.In_I2C.writeRegister(PI4IOE_1_ADDR, 0x05, &out, 1, 400000);
    }
}

// Official SC202CS gain tuples: {0x3e07 dig_fine, 0x3e06 dig_coarse, 0x3e09 analog_gain}
struct Sc202GainTuple { uint8_t dFine; uint8_t dCoarse; uint8_t aGain; };
static const Sc202GainTuple SC202_GAINS[10] = {
    {0x80, 0x00, 0x00}, // 1.0x
    {0xa0, 0x00, 0x00}, // 1.25x
    {0xc0, 0x00, 0x00}, // 1.5x
    {0xe0, 0x00, 0x00}, // 1.75x
    {0x80, 0x00, 0x01}, // 2.0x
    {0xc0, 0x00, 0x01}, // 3.0x
    {0x80, 0x00, 0x03}, // 4.0x
    {0xc0, 0x00, 0x03}, // 6.0x
    {0x80, 0x00, 0x07}, // 8.0x
    {0x80, 0x00, 0x0f}  // 16.0x
};

static void sc202csApplyExposure(int expVal, int gainStep) {
    // SC202CS exposure MUST stay within [8 .. VTS-6 = 1244] — exceeding VTS stalls frame output!
    int val = constrain(expVal, 16, 1240);
    uint8_t r0 = (val >> 12) & 0x0F;
    uint8_t r1 = (val >> 4)  & 0xFF;
    uint8_t r2 = (val & 0x0F) << 4;
    const Sc202GainTuple &g = SC202_GAINS[constrain(gainStep, 0, 9)];
    sc202csWriteReg(0x3e00, r0);
    sc202csWriteReg(0x3e01, r1);
    sc202csWriteReg(0x3e02, r2);
    sc202csWriteReg(0x3e07, g.dFine);
    sc202csWriteReg(0x3e06, g.dCoarse);
    sc202csWriteReg(0x3e09, g.aGain);
}

// ======================== STOCK M5TAB5 24MHz XCLK =====================
// Exact stock M5Tab5-UserDemo bsp_cam_osc_init() on GPIO 36 (uses LEDC_TIMER_2/CH_2 to avoid LCD backlight!)
static void startTab5CamXclk() {
    if (camXclkStarted) return;
    ledc_timer_config_t timer_conf = {};
    timer_conf.duty_resolution = LEDC_TIMER_1_BIT;
    timer_conf.freq_hz         = 24000000;
    timer_conf.speed_mode      = LEDC_LOW_SPEED_MODE;
    timer_conf.deconfigure     = false;
    timer_conf.clk_cfg         = LEDC_AUTO_CLK;
    timer_conf.timer_num       = LEDC_TIMER_2;
    esp_err_t e1 = ledc_timer_config(&timer_conf);

    ledc_channel_config_t ch_conf = {};
    ch_conf.gpio_num   = 36; // M5Tab5 Camera XCLK input!
    ch_conf.speed_mode = LEDC_LOW_SPEED_MODE;
    ch_conf.channel    = LEDC_CHANNEL_2;
    ch_conf.intr_type  = LEDC_INTR_DISABLE;
    ch_conf.timer_sel  = LEDC_TIMER_2;
    ch_conf.duty       = 1;
    ch_conf.hpoint     = 0;
    ch_conf.sleep_mode = LEDC_SLEEP_MODE_KEEP_ALIVE;
    esp_err_t e2 = ledc_channel_config(&ch_conf);
    Serial.printf("[CAM] XCLK 24MHz GPIO36 timer=%d ch=%d\n", (int)e1, (int)e2);
    camXclkStarted = (e1 == ESP_OK && e2 == ESP_OK);
    delay(10);
}

static uint32_t srgbGammaOp(uint32_t x) {
    if (x == 0) return 0;
    if (x >= 256) return 256;
    double y = pow((double)x / 256.0, 1.0 / 2.15) * 256.0;
    uint32_t yi = (uint32_t)round(y);
    return (yi >= 256) ? 255 : yi;
}

static void applyIspWhiteBalance(float gR, float gB) {
    if (!ispHandle) return;
    esp_isp_ccm_config_t ccm = {};
    ccm.saturation = true;
    ccm.flags.update_once_configured = 1;
    ccm.matrix[0][0] = constrain(gR, 0.7f, 2.6f);
    ccm.matrix[1][1] = 1.0f;
    ccm.matrix[2][2] = constrain(gB, 0.7f, 2.8f);
    esp_isp_ccm_configure(ispHandle, &ccm);
}

// ======================== CSI ISR CALLBACKS ===========================
static bool IRAM_ATTR camOnGetNewTrans(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, void *user_data) {
    trans->buffer = camFb[dmaWriteIdx];
    trans->buflen = CAM_FB_SIZE;
    return false;
}

static bool IRAM_ATTR camOnTransFinished(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, void *user_data) {
    if (trans->received_size >= CAM_FB_SIZE / 2) {
        dmaReadyIdx = dmaWriteIdx;
        dmaWriteIdx = 1 - dmaWriteIdx;
        dmaFrameSeq++;
    }
    return false;
}

// ======================== CAMERA HW INIT / DEINIT =====================
static void stopCameraHardware() {
    sc202csWriteReg(SC202CS_REG_SLEEP_MODE, 0x00);
    delay(10);
    if (camHandle) {
        esp_cam_ctlr_stop(camHandle);
        esp_cam_ctlr_disable(camHandle);
        esp_cam_ctlr_del(camHandle);
        camHandle = nullptr;
    }
    if (ispHandle) {
        esp_isp_disable(ispHandle);
        esp_isp_del_processor(ispHandle);
        ispHandle = nullptr;
    }
    if (jpegHandle) {
        jpeg_del_encoder_engine(jpegHandle);
        jpegHandle = nullptr;
    }
    for (int i = 0; i < 2; i++) {
        if (camFb[i]) { free(camFb[i]); camFb[i] = nullptr; }
    }
    if (jpgOutBuf)  { free(jpgOutBuf);  jpgOutBuf = nullptr; jpgOutCap = 0; }
    if (previewBuf) { free(previewBuf); previewBuf = nullptr; }
    if (galleryBuf) { free(galleryBuf); galleryBuf = nullptr; }

    dmaReadyIdx  = -1;
    dmaWriteIdx  = 0;
    dmaFrameSeq  = 0;
    lastShownSeq = 0;
    camHwReady   = false;
}

static bool startCameraHardware() {
    if (camHwReady) return true;
    camErrorMsg = "";

    // 1. Start 24 MHz XCLK on GPIO 36 (stock M5Tab5 bsp_cam_osc_init!) & release CAM_RST on PI4IOE-1 pin 6
    startTab5CamXclk();
    setTab5CameraPower(true);

    // 2. Verify SC202CS/SC2356 sensor on I2C 0x36
    uint8_t idH = 0, idL = 0;
    if (!sc202csReadReg(0x3107, idH) || !sc202csReadReg(0x3108, idL)) {
        camErrorMsg = "Camera I2C (0x36) not responding";
        return false;
    }
    Serial.printf("[CAM] Sensor ID: 0x%02X%02X\n", idH, idL);

    // 3. Upload stock M5Tab5 1280x720 RAW8 1-lane 576Mbps register sequence (stream stays OFF until CSI+ISP ready!)
    for (size_t i = 0; SC202CS_1280X720_RAW8[i].reg != SC202CS_REG_END; i++) {
        sc202csWriteReg(SC202CS_1280X720_RAW8[i].reg, SC202CS_1280X720_RAW8[i].val);
        if (SC202CS_1280X720_RAW8[i].reg == 0x0103) delay(15);
    }
    aeExposure = 1100;
    aeGainIdx  = 2;
    wbGainR    = 1.25f;
    wbGainB    = 1.45f;
    sc202csApplyExposure(aeExposure, aeGainIdx);

    // 4. Ensure MIPI PHY LDO VO3 (2500mV) is powered
    if (!mipiLdo) {
        esp_ldo_channel_config_t ldoCfg = {};
        ldoCfg.chan_id = 3;
        ldoCfg.voltage_mv = 2500;
        esp_ldo_acquire_channel(&ldoCfg, &mipiLdo);
    }

    // 5. Create MIPI-CSI Controller (exact stock M5Tab5 esp_video_csi_device.c config!)
    esp_cam_ctlr_csi_config_t csiCfg = {};
    csiCfg.ctlr_id                = 0;
    csiCfg.clk_src                = MIPI_CSI_PHY_CLK_SRC_DEFAULT;
    csiCfg.h_res                  = CAM_W;
    csiCfg.v_res                  = CAM_H;
    csiCfg.data_lane_num          = 1;
    csiCfg.lane_bit_rate_mbps     = 576;
    csiCfg.input_data_color_type  = CAM_CTLR_COLOR_RAW8;
    csiCfg.output_data_color_type = CAM_CTLR_COLOR_RGB565;
    csiCfg.queue_items            = 1;
    csiCfg.byte_swap_en           = 0;
    csiCfg.bk_buffer_dis          = 1; // Continuous callback-driven ping-pong DMA (no blocking receive!)

    esp_err_t err = esp_cam_new_csi_ctlr(&csiCfg, &camHandle);
    if (err != ESP_OK) {
        camErrorMsg = "CSI init failed (" + String((int)err) + ")";
        stopCameraHardware();
        return false;
    }

    // 6. Allocate 2 ping-pong DMA-aligned PSRAM framebuffers + preview buffer
    camFb[0] = (uint8_t*)esp_cam_ctlr_alloc_buffer(camHandle, CAM_FB_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    camFb[1] = (uint8_t*)esp_cam_ctlr_alloc_buffer(camHandle, CAM_FB_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    previewBuf = (uint16_t*)ps_malloc((size_t)VF_W * (size_t)VF_H * sizeof(uint16_t));
    if (!camFb[0] || !camFb[1] || !previewBuf) {
        camErrorMsg = "PSRAM framebuffer alloc failed";
        stopCameraHardware();
        return false;
    }
    memset(camFb[0], 0, CAM_FB_SIZE);
    memset(camFb[1], 0, CAM_FB_SIZE);
    esp_cache_msync(camFb[0], CAM_FB_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    esp_cache_msync(camFb[1], CAM_FB_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    dmaWriteIdx  = 0;
    dmaReadyIdx  = -1;
    dmaFrameSeq  = 0;
    lastShownSeq = 0;

    esp_cam_ctlr_evt_cbs_t cbs = {};
    cbs.on_get_new_trans  = camOnGetNewTrans;
    cbs.on_trans_finished = camOnTransFinished;
    if (esp_cam_ctlr_register_event_callbacks(camHandle, &cbs, nullptr) != ESP_OK ||
        esp_cam_ctlr_enable(camHandle) != ESP_OK ||
        esp_cam_ctlr_start(camHandle) != ESP_OK) {
        camErrorMsg = "CSI start failed";
        stopCameraHardware();
        return false;
    }

    // 7. Create & enable ESP32-P4 Hardware ISP AFTER CSI start (exact stock esp_video order!)
    esp_isp_processor_cfg_t ispCfg = {};
    ispCfg.clk_src                = ISP_CLK_SRC_DEFAULT;
    ispCfg.clk_hz                 = 80 * 1000 * 1000;
    ispCfg.input_data_source      = ISP_INPUT_DATA_SOURCE_CSI;
    ispCfg.input_data_color_type  = ISP_COLOR_RAW8;
    ispCfg.output_data_color_type = ISP_COLOR_RGB565;
    ispCfg.yuv_range              = ISP_COLOR_RANGE_FULL;
    ispCfg.yuv_std                = ISP_YUV_CONV_STD_BT601;
    ispCfg.has_line_start_packet  = false;
    ispCfg.has_line_end_packet    = false;
    ispCfg.h_res                  = CAM_W;
    ispCfg.v_res                  = CAM_H;
    ispCfg.bayer_order            = COLOR_RAW_ELEMENT_ORDER_BGGR;

    err = esp_isp_new_processor(&ispCfg, &ispHandle);
    if (err != ESP_OK || esp_isp_enable(ispHandle) != ESP_OK) {
        camErrorMsg = "ISP init failed (" + String((int)err) + ")";
        stopCameraHardware();
        return false;
    }

    // Enable ISP sRGB Gamma + CCM White Balance (stock M5Tab5 rev<300 pipeline!)
    isp_gamma_curve_points_t gammaPts = {};
    if (esp_isp_gamma_fill_curve_points(srgbGammaOp, &gammaPts) == ESP_OK) {
        esp_isp_gamma_configure(ispHandle, COLOR_COMPONENT_R, &gammaPts);
        esp_isp_gamma_configure(ispHandle, COLOR_COMPONENT_G, &gammaPts);
        esp_isp_gamma_configure(ispHandle, COLOR_COMPONENT_B, &gammaPts);
        esp_isp_gamma_enable(ispHandle);
    }
    applyIspWhiteBalance(wbGainR, wbGainB);
    esp_isp_ccm_enable(ispHandle);

    // 8. Create Hardware JPEG Encoder Engine
    jpeg_encode_engine_cfg_t engCfg = {};
    engCfg.intr_priority = 0;
    engCfg.timeout_ms    = 100;
    if (jpeg_new_encoder_engine(&engCfg, &jpegHandle) == ESP_OK) {
        jpeg_encode_memory_alloc_cfg_t outMemCfg = {};
        outMemCfg.buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER;
        jpgOutBuf = (uint8_t*)jpeg_alloc_encoder_mem(256 * 1024, &outMemCfg, &jpgOutCap);
    }

    // 9. FINALLY start SC202CS MIPI stream (after CSI + ISP are running!)
    sc202csWriteReg(SC202CS_REG_SLEEP_MODE, 0x01);
    delay(20);

    camHwReady = true;
    fpsWindowMs = millis();
    fpsFrames = 0;
    return true;
}

// ======================== AUTO EXPOSURE & AWB =========================
static void runSimpleAeAwb(const uint16_t* rgb565) {
    aeFrameCount++;
    if ((aeFrameCount & 3) != 0) return; // Run every 4th frame

    uint64_t sumR = 0, sumG = 0, sumB = 0;
    uint32_t samples = 0;
    for (int y = CAM_H / 5; y < (CAM_H * 4) / 5; y += 24) {
        const uint16_t* row = rgb565 + y * CAM_W;
        for (int x = CAM_W / 5; x < (CAM_W * 4) / 5; x += 24) {
            uint16_t p = row[x];
            uint8_t r = ((p >> 11) & 0x1F) << 3;
            uint8_t g = ((p >> 5)  & 0x3F) << 2;
            uint8_t b = ( p        & 0x1F) << 3;
            sumR += r; sumG += g; sumB += b;
            samples++;
        }
    }
    if (samples == 0) return;

    float avgR = (float)sumR / samples;
    float avgG = (float)sumG / samples;
    float avgB = (float)sumB / samples;
    float luma = 0.299f * avgR + 0.587f * avgG + 0.114f * avgB;
    float target = constrain(115.0f + evBias * 22.0f, 55.0f, 195.0f);

    if (luma < target - 16.0f) {
        if (aeExposure < 1200) aeExposure = min(1240, aeExposure + 80);
        else if (aeGainIdx < 9) aeGainIdx++;
        sc202csApplyExposure(aeExposure, aeGainIdx);
    } else if (luma > target + 18.0f) {
        if (aeGainIdx > 0) aeGainIdx--;
        else if (aeExposure > 40) aeExposure = max(20, aeExposure - 70);
        sc202csApplyExposure(aeExposure, aeGainIdx);
    }

    // Smooth Gray-World White Balance via ISP CCM (the method stock M5Tab5 uses!)
    if (ispHandle && avgR > 18.0f && avgG > 18.0f && avgB > 18.0f && (aeFrameCount & 7) == 0) {
        float targetR = constrain(wbGainR * (avgG / avgR), 0.75f, 2.5f);
        float targetB = constrain(wbGainB * (avgG / avgB), 0.75f, 2.7f);
        wbGainR = wbGainR * 0.8f + targetR * 0.2f;
        wbGainB = wbGainB * 0.8f + targetB * 0.2f;
        applyIspWhiteBalance(wbGainR, wbGainB);
    }
}

// ======================== SD CARD & AVI HELPERS =======================
static void setCamToast(const String &msg, unsigned long ms = 2400) {
    camToastMsg = msg;
    camToastUntilMs = millis() + ms;
}

static bool ensureDcimDir() {
    if (!initMapSdCard(false) && !initMapSdCard(true)) return false;
    if (!SD.exists("/DCIM")) SD.mkdir("/DCIM");
    return true;
}

static String nextMediaPath(const char* prefix, const char* ext) {
    for (int n = 1; n <= 9999; n++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "/DCIM/%s_%04d.%s", prefix, n, ext);
        if (!SD.exists(buf)) return String(buf);
    }
    return String("/DCIM/") + prefix + "_9999." + ext;
}

static uint8_t* getLatestReadyFrame() {
    int idx = dmaReadyIdx;
    if (idx < 0 || idx > 1 || !camFb[idx]) return nullptr;
    return camFb[idx];
}

static bool encodeCurrentFrameToJpeg(uint8_t quality, uint32_t &outLen) {
    uint8_t* fb = getLatestReadyFrame();
    if (!jpegHandle || !fb || !jpgOutBuf) return false;
    jpeg_encode_cfg_t cfg = {};
    cfg.width         = CAM_W;
    cfg.height        = CAM_H;
    cfg.src_type      = JPEG_ENCODE_IN_FORMAT_RGB565;
    cfg.sub_sample    = JPEG_DOWN_SAMPLING_YUV420;
    cfg.image_quality = quality;

    outLen = 0;
    esp_err_t err = jpeg_encoder_process(jpegHandle, &cfg, fb, CAM_FB_SIZE, jpgOutBuf, jpgOutCap, &outLen);
    return (err == ESP_OK && outLen > 256);
}

static void capturePhotoToSd() {
    if (!camHwReady) { setCamToast("Camera not ready"); playTone(220, 150); return; }
    if (!ensureDcimDir()) { setCamToast("Insert FAT32 microSD card!"); playTone(220, 200); return; }

    uint32_t jpgLen = 0;
    if (!encodeCurrentFrameToJpeg(85, jpgLen)) {
        setCamToast("Hardware JPEG encode failed!");
        playTone(220, 200);
        return;
    }

    String path = nextMediaPath("IMG", "JPG");
    File f = SD.open(path, FILE_WRITE);
    if (!f) {
        setCamToast("SD write error!");
        playTone(220, 200);
        return;
    }
    size_t w = f.write(jpgOutBuf, jpgLen);
    f.close();

    if (w == jpgLen) {
        // Shutter flash + click
        M5.Display.fillRect(VF_X, VF_Y, VF_W, VF_H, TFT_WHITE);
        playTone(1400, 45);
        delay(55);
        playTone(900, 35);
        setCamToast("Saved " + path.substring(6) + " (" + String(jpgLen / 1024) + " KB)");
    } else {
        SD.remove(path);
        setCamToast("SD full or write failed!");
        playTone(220, 200);
    }
}

// Write little-endian helpers for standard RIFF MJPEG AVI container
static void writeU32LE(uint8_t* p, uint32_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}
static void writeU16LE(uint8_t* p, uint16_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF;
}

static void buildAviHeader(uint8_t hdr[224], uint32_t riffSize, uint32_t totalFrames,
                           uint32_t fps, uint32_t moviSize) {
    memset(hdr, 0, 224);
    if (fps < 1) fps = 10;
    uint32_t usPerFrame = 1000000UL / fps;

    memcpy(hdr + 0,   "RIFF", 4); writeU32LE(hdr + 4, riffSize); memcpy(hdr + 8, "AVI ", 4);
    memcpy(hdr + 12,  "LIST", 4); writeU32LE(hdr + 16, 192);     memcpy(hdr + 20, "hdrl", 4);
    memcpy(hdr + 24,  "avih", 4); writeU32LE(hdr + 28, 56);
    writeU32LE(hdr + 32, usPerFrame);       // dwMicroSecPerFrame
    writeU32LE(hdr + 36, 1500000);          // dwMaxBytesPerSec
    writeU32LE(hdr + 44, 0x10);             // dwFlags: AVIF_HASINDEX
    writeU32LE(hdr + 48, totalFrames);      // dwTotalFrames
    writeU32LE(hdr + 56, 1);                // dwStreams
    writeU32LE(hdr + 60, 128 * 1024);       // dwSuggestedBufferSize
    writeU32LE(hdr + 64, CAM_W);            // dwWidth
    writeU32LE(hdr + 68, CAM_H);            // dwHeight

    memcpy(hdr + 88,  "LIST", 4); writeU32LE(hdr + 92, 116);     memcpy(hdr + 96, "strl", 4);
    memcpy(hdr + 100, "strh", 4); writeU32LE(hdr + 104, 56);
    memcpy(hdr + 108, "vids", 4); memcpy(hdr + 112, "MJPG", 4);
    writeU32LE(hdr + 128, 1);               // dwScale
    writeU32LE(hdr + 132, fps);             // dwRate
    writeU32LE(hdr + 140, totalFrames);     // dwLength
    writeU32LE(hdr + 144, 128 * 1024);      // dwSuggestedBufferSize
    writeU32LE(hdr + 148, 8000);            // dwQuality

    memcpy(hdr + 164, "strf", 4); writeU32LE(hdr + 168, 40);
    writeU32LE(hdr + 172, 40);              // biSize
    writeU32LE(hdr + 176, CAM_W);           // biWidth
    writeU32LE(hdr + 180, CAM_H);           // biHeight
    writeU16LE(hdr + 184, 1);               // biPlanes
    writeU16LE(hdr + 186, 24);              // biBitCount
    memcpy(hdr + 188, "MJPG", 4);           // biCompression
    writeU32LE(hdr + 192, CAM_W * CAM_H * 3);

    memcpy(hdr + 212, "LIST", 4); writeU32LE(hdr + 216, moviSize); memcpy(hdr + 220, "movi", 4);
}

static void startVideoRecording() {
    if (recActive) return;
    if (!camHwReady) { setCamToast("Camera not ready"); playTone(220, 150); return; }
    if (!ensureDcimDir()) { setCamToast("Insert FAT32 microSD card!"); playTone(220, 200); return; }

    recPath = nextMediaPath("VID", "AVI");
    recFile = SD.open(recPath, FILE_WRITE);
    if (!recFile) {
        setCamToast("SD write error!");
        playTone(220, 200);
        return;
    }

    uint8_t hdr[224];
    buildAviHeader(hdr, 0, 0, 10, 4);
    recFile.write(hdr, sizeof(hdr));
    recFrames = 0;
    recMoviBytes = 4; // Includes "movi" 4CC
    recIndex.clear();
    recIndex.reserve(600);
    recStartMs = millis();
    recActive = true;
    playTone(988, 90);
}

static void stopVideoRecording() {
    if (!recActive) return;
    recActive = false;

    uint32_t elapsedMs = millis() - recStartMs;
    uint32_t actualFps = (elapsedMs >= 500 && recFrames > 0)
                         ? (uint32_t)constrain((int)roundf((float)recFrames * 1000.0f / (float)elapsedMs), 1, 30)
                         : 10;

    // Append standard AVI idx1 index chunk
    uint32_t idxBytes = (uint32_t)recIndex.size() * 16;
    uint8_t idxHdr[8] = { 'i','d','x','1', 0,0,0,0 };
    writeU32LE(idxHdr + 4, idxBytes);
    recFile.write(idxHdr, 8);

    uint8_t entry[16];
    for (const auto &e : recIndex) {
        memcpy(entry, "00dc", 4);
        writeU32LE(entry + 4, 0x10); // AVIIF_KEYFRAME
        writeU32LE(entry + 8, e.offset);
        writeU32LE(entry + 12, e.size);
        recFile.write(entry, 16);
    }

    // Patch final sizes & real FPS at file offset 0
    uint32_t riffSize = 212 + recMoviBytes + 8 + idxBytes;
    uint8_t hdr[224];
    buildAviHeader(hdr, riffSize, recFrames, actualFps, recMoviBytes);
    recFile.seek(0);
    recFile.write(hdr, sizeof(hdr));
    recFile.close();
    recIndex.clear();

    playTone(587, 100);
    setCamToast("Saved " + recPath.substring(6) + " (" + String(recFrames) + "f @ " + String(actualFps) + "fps)");
}

static void appendVideoFrame() {
    if (!recActive || !recFile) return;
    uint32_t jpgLen = 0;
    if (!encodeCurrentFrameToJpeg(68, jpgLen)) return;

    uint8_t chunkHdr[8] = { '0','0','d','c', 0,0,0,0 };
    writeU32LE(chunkHdr + 4, jpgLen);
    uint32_t relOffset = recMoviBytes; // Offset from 'movi' start

    recFile.write(chunkHdr, 8);
    recFile.write(jpgOutBuf, jpgLen);
    uint32_t padded = jpgLen;
    if (jpgLen & 1U) {
        uint8_t zero = 0;
        recFile.write(&zero, 1);
        padded++;
    }
    recMoviBytes += 8 + padded;
    recFrames++;
    if (recIndex.size() < 7200) {
        recIndex.push_back({ relOffset, jpgLen });
    }
}

// ======================== GALLERY & AVI PLAYER ========================
static void stopVideoPlayback() {
    videoPlaying = false;
    if (playFile) playFile.close();
}

static void scanGalleryFiles() {
    stopVideoPlayback();
    galleryItems.clear();
    if (!ensureDcimDir()) return;

    File dir = SD.open("/DCIM");
    if (!dir) return;
    File f = dir.openNextFile();
    while (f) {
        if (!f.isDirectory()) {
            String name = String(f.name());
            int slash = name.lastIndexOf('/');
            if (slash >= 0) name = name.substring(slash + 1);
            String up = name;
            up.toUpperCase();
            if (up.endsWith(".JPG") || up.endsWith(".JPEG") || up.endsWith(".AVI")) {
                MediaItem it;
                it.name = name;
                it.path = "/DCIM/" + name;
                it.isVideo = up.endsWith(".AVI");
                it.sizeBytes = f.size();
                galleryItems.push_back(it);
            }
        }
        f.close();
        f = dir.openNextFile();
    }
    dir.close();

    // Sort newest (highest filename) first
    for (size_t i = 0; i < galleryItems.size(); i++) {
        for (size_t j = i + 1; j < galleryItems.size(); j++) {
            if (galleryItems[j].name > galleryItems[i].name) {
                MediaItem tmp = galleryItems[i];
                galleryItems[i] = galleryItems[j];
                galleryItems[j] = tmp;
            }
        }
    }
    if (galleryIdx >= (int)galleryItems.size()) galleryIdx = 0;
}

static uint32_t readU32LE(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void drawJpegScaledInViewport(const uint8_t* data, size_t len) {
    // Draw 1280x720 JPEG centered & scaled neatly to fit VF_W x VF_H
    M5.Display.setClipRect(VF_X + 2, VF_Y + 2, VF_W - 4, VF_H - 4);
    float sc = (float)(VF_W - 8) / (float)CAM_W; // ~0.525 -> 672 x 378
    int dy = VF_Y + (VF_H - (int)(CAM_H * sc)) / 2;
    M5.Display.drawJpg(data, len, VF_X + 4, dy, VF_W - 8, VF_H - 8, 0, 0, sc, sc);
    M5.Display.clearClipRect();
}

static void showCurrentGalleryItem() {
    stopVideoPlayback();
    M5.Display.fillRoundRect(VF_X, VF_Y, VF_W, VF_H, 16, TFT_BLACK);
    M5.Display.drawRoundRect(VF_X, VF_Y, VF_W, VF_H, 16, C_TEAL);

    if (!isMapSdReady()) {
        M5.Display.setTextDatum(middle_center);
        M5.Display.setFont(&fonts::FreeSansBold18pt7b);
        M5.Display.setTextColor(C_ORANGE);
        M5.Display.drawString("No microSD card detected", sw / 2, VF_Y + VF_H / 2);
        return;
    }
    if (galleryItems.empty()) {
        M5.Display.setTextDatum(middle_center);
        M5.Display.setFont(&fonts::FreeSansBold18pt7b);
        M5.Display.setTextColor(C_GRAY);
        M5.Display.drawString("No photos or videos in /DCIM yet", sw / 2, VF_Y + VF_H / 2);
        return;
    }

    if (!galleryBuf) galleryBuf = (uint8_t*)ps_malloc(GALLERY_BUF_CAP);
    if (!galleryBuf) return;

    MediaItem &it = galleryItems[galleryIdx];
    File f = SD.open(it.path, FILE_READ);
    if (!f) return;

    if (!it.isVideo) {
        size_t sz = f.size();
        if (sz > 128 && sz <= GALLERY_BUF_CAP) {
            size_t got = f.read(galleryBuf, sz);
            drawJpegScaledInViewport(galleryBuf, got);
        }
        f.close();
    } else {
        // Read first frame of AVI as thumbnail
        uint8_t hdr[224];
        if (f.read(hdr, 224) == 224 && memcmp(hdr, "RIFF", 4) == 0 && memcmp(hdr + 220, "movi", 4) == 0) {
            uint8_t ch[8];
            if (f.read(ch, 8) == 8 && memcmp(ch, "00dc", 4) == 0) {
                uint32_t fsz = readU32LE(ch + 4);
                if (fsz > 128 && fsz <= GALLERY_BUF_CAP) {
                    size_t got = f.read(galleryBuf, fsz);
                    drawJpegScaledInViewport(galleryBuf, got);
                }
            }
        }
        f.close();
        // Play icon overlay badge
        M5.Display.fillCircle(sw / 2, VF_Y + VF_H / 2, 46, C_DARK);
        M5.Display.drawCircle(sw / 2, VF_Y + VF_H / 2, 46, C_GOLD);
        M5.Display.fillTriangle(sw / 2 - 14, VF_Y + VF_H / 2 - 22,
                                sw / 2 - 14, VF_Y + VF_H / 2 + 22,
                                sw / 2 + 24, VF_Y + VF_H / 2, C_GOLD);
    }

    // Metadata caption strip at bottom of viewport
    M5.Display.fillRect(VF_X + 4, VF_Y + VF_H - 44, VF_W - 8, 40, C_DARK);
    M5.Display.setTextDatum(middle_center);
    M5.Display.setFont(&fonts::FreeSansBold12pt7b);
    M5.Display.setTextColor(TFT_WHITE);
    String cap = String(galleryIdx + 1) + "/" + String(galleryItems.size()) + "  " +
                 it.name + "  (" + String(it.sizeBytes / 1024) + " KB)";
    M5.Display.drawString(cap, sw / 2, VF_Y + VF_H - 24);
}

static void startVideoPlayback() {
    if (galleryItems.empty() || !galleryItems[galleryIdx].isVideo) return;
    if (!galleryBuf) galleryBuf = (uint8_t*)ps_malloc(GALLERY_BUF_CAP);
    if (!galleryBuf) return;

    stopVideoPlayback();
    playFile = SD.open(galleryItems[galleryIdx].path, FILE_READ);
    if (!playFile) return;

    uint8_t hdr[224];
    if (playFile.read(hdr, 224) != 224 || memcmp(hdr, "RIFF", 4) != 0) {
        playFile.close();
        return;
    }
    uint32_t usPerFrame = readU32LE(hdr + 32);
    uint32_t moviSize   = readU32LE(hdr + 216);
    playFrameIntervalMs = (usPerFrame >= 30000 && usPerFrame <= 500000) ? (usPerFrame / 1000) : 90;
    playDataEndOffset   = 220 + moviSize;
    playNextFrameMs     = millis();
    videoPlaying        = true;
}

// ======================== LIVE PREVIEW BLIT ===========================
static void renderLivePreviewFrame(const uint8_t* fbPtr) {
    if (!fbPtr || !previewBuf) return;
    const uint16_t* src = (const uint16_t*)fbPtr;

    // Fast nearest-neighbor scale + rotation into VF_W x VF_H RGB565 buffer
    if (camRotIdx == 0 || camRotIdx == 2) {
        // Landscape fit (1280x720 -> 680x382 centered in 680x820)
        const int dstH = 382;
        const int yPad = (VF_H - dstH) / 2;
        memset(previewBuf, 0, (size_t)VF_W * (size_t)VF_H * 2);
        for (int dy = 0; dy < dstH; dy++) {
            int sy = (camRotIdx == 0) ? ((dy * CAM_H) / dstH) : (CAM_H - 1 - (dy * CAM_H) / dstH);
            const uint16_t* sRow = src + sy * CAM_W;
            uint16_t* dRow = previewBuf + (dy + yPad) * VF_W;
            for (int dx = 0; dx < VF_W; dx++) {
                int sx = (camRotIdx == 0) ? ((dx * CAM_W) / VF_W) : (CAM_W - 1 - (dx * CAM_W) / VF_W);
                dRow[dx] = sRow[sx];
            }
        }
    } else {
        // Portrait fill (90 or 270 deg: 720x1280 -> 680x820 center crop!)
        const int cropW = 868; // Keeps aspect ratio of 680x820 from 720x1280
        const int xOff  = (CAM_W - cropW) / 2;
        for (int dy = 0; dy < VF_H; dy++) {
            int sx = xOff + (dy * cropW) / VF_H;
            if (camRotIdx == 3) sx = CAM_W - 1 - sx;
            uint16_t* dRow = previewBuf + dy * VF_W;
            for (int dx = 0; dx < VF_W; dx++) {
                int sy = (dx * CAM_H) / VF_W;
                if (camRotIdx == 1) sy = CAM_H - 1 - sy;
                dRow[dx] = src[sy * CAM_W + sx];
            }
        }
    }

    // Push as native little-endian rgb565_t so LovyanGFX does NOT byte-swap into rainbow colors!
    M5.Display.pushImage(VF_X, VF_Y, VF_W, VF_H, (const lgfx::rgb565_t*)previewBuf);

    // Recording Overlay Banner
    if (recActive) {
        uint32_t sec = (millis() - recStartMs) / 1000;
        char tbuf[32];
        snprintf(tbuf, sizeof(tbuf), "REC %02u:%02u | %u f", (unsigned)(sec / 60), (unsigned)(sec % 60), (unsigned)recFrames);
        M5.Display.fillRoundRect(VF_X + 16, VF_Y + 14, 240, 42, 12, C_RED);
        M5.Display.fillCircle(VF_X + 38, VF_Y + 35, 8, ((millis() / 400) & 1) ? TFT_WHITE : C_RED);
        M5.Display.setTextDatum(middle_left);
        M5.Display.setFont(&fonts::FreeSansBold12pt7b);
        M5.Display.setTextColor(TFT_WHITE);
        M5.Display.drawString(tbuf, VF_X + 56, VF_Y + 35);
    }

    // Brief Toast Overlay
    if (camToastMsg.length() && millis() < camToastUntilMs) {
        M5.Display.fillRoundRect(VF_X + 30, VF_Y + VF_H - 64, VF_W - 60, 48, 12, C_DARK);
        M5.Display.drawRoundRect(VF_X + 30, VF_Y + VF_H - 64, VF_W - 60, 48, 12, C_GOLD);
        M5.Display.setTextDatum(middle_center);
        M5.Display.setFont(&fonts::FreeSansBold12pt7b);
        M5.Display.setTextColor(C_GOLD);
        M5.Display.drawString(camToastMsg, sw / 2, VF_Y + VF_H - 40);
    }
}

// ======================== UI DRAWING ==================================
static void drawCameraControls() {
    // Bottom Controls Zone (y: 988..1160)
    M5.Display.fillRect(0, 984, sw, 182, C_DARK);
    M5.Display.setTextDatum(middle_center);

    if (camMode == 2) {
        // Gallery Controls
        bool hasItems = !galleryItems.empty();
        bool isVid = hasItems && galleryItems[galleryIdx].isVideo;
        M5.Display.setFont(&fonts::FreeSansBold12pt7b);

        M5.Display.fillRoundRect(20, 1020, 155, 84, 16, C_PANEL);
        M5.Display.setTextColor(hasItems ? TFT_WHITE : C_GRAY);
        M5.Display.drawString("< PREV", 97, 1062);

        M5.Display.fillRoundRect(188, 1020, 166, 84, 16, isVid ? (videoPlaying ? C_ORANGE : C_GREEN) : C_PANEL);
        M5.Display.setTextColor(TFT_WHITE);
        M5.Display.drawString(isVid ? (videoPlaying ? "PAUSE" : "PLAY") : "PHOTO", 271, 1062);

        M5.Display.fillRoundRect(366, 1020, 166, 84, 16, hasItems ? C_RED : C_PANEL);
        M5.Display.drawString("DELETE", 449, 1062);

        M5.Display.fillRoundRect(545, 1020, 155, 84, 16, C_PANEL);
        M5.Display.setTextColor(hasItems ? TFT_WHITE : C_GRAY);
        M5.Display.drawString("NEXT >", 622, 1062);
    } else {
        // Photo / Video Shutter Row + EV +/- buttons
        M5.Display.fillRoundRect(24, 1030, 128, 72, 16, C_PANEL);
        M5.Display.setFont(&fonts::FreeSansBold12pt7b);
        M5.Display.setTextColor(TFT_WHITE);
        M5.Display.drawString("EV -", 88, 1066);

        // Big Center Shutter / Record Button
        int scx = sw / 2, scy = 1068;
        M5.Display.drawCircle(scx, scy, 58, TFT_WHITE);
        M5.Display.drawCircle(scx, scy, 56, TFT_WHITE);
        if (camMode == 0) {
            M5.Display.fillCircle(scx, scy, 48, TFT_WHITE);
            M5.Display.setTextColor(C_DARK);
            M5.Display.setFont(&fonts::FreeSansBold12pt7b);
            M5.Display.drawString("SNAP", scx, scy);
        } else {
            if (recActive) {
                M5.Display.fillRoundRect(scx - 26, scy - 26, 52, 52, 10, C_RED);
            } else {
                M5.Display.fillCircle(scx, scy, 48, C_RED);
                M5.Display.setTextColor(TFT_WHITE);
                M5.Display.setFont(&fonts::FreeSansBold12pt7b);
                M5.Display.drawString("REC", scx, scy);
            }
        }

        M5.Display.fillRoundRect(sw - 152, 1030, 128, 72, 16, C_PANEL);
        M5.Display.setFont(&fonts::FreeSansBold12pt7b);
        M5.Display.setTextColor(TFT_WHITE);
        M5.Display.drawString("EV +", sw - 88, 1066);

        // Exposure indicator caption
        M5.Display.setFont(&fonts::FreeSans9pt7b);
        M5.Display.setTextColor(C_CYAN);
        String evLbl = "EV " + String(evBias >= 0 ? "+" : "") + String(evBias) +
                       "  |  HD 1280x720  |  " + String(liveFps) + " fps";
        M5.Display.drawString(evLbl, sw / 2, 1142);
    }

    // Bottom 3-Mode Bar (PHOTO | VIDEO | GALLERY)
    const char* modes[3] = {"PHOTO", "VIDEO", "GALLERY"};
    for (int i = 0; i < 3; i++) {
        int bx = 20 + i * 232;
        bool act = (camMode == i);
        uint16_t col = act ? (i == 1 ? C_RED : C_GREEN) : C_PANEL;
        M5.Display.fillRoundRect(bx, 1176, 216, 80, 16, col);
        M5.Display.setFont(&fonts::FreeSansBold18pt7b);
        M5.Display.setTextColor(act ? TFT_WHITE : C_GRAY);
        M5.Display.drawString(modes[i], bx + 108, 1216);
    }
}

void drawCameraApp() {
    M5.Display.fillScreen(C_DARK);
    M5.Display.setTextDatum(middle_center);

    // Top Bar (y: 0..84)
    M5.Display.fillRect(0, 0, sw, 84, 0x0862);
    M5.Display.fillRoundRect(16, 12, 130, 60, 14, C_PANEL);
    M5.Display.setFont(&fonts::FreeSansBold12pt7b);
    M5.Display.setTextColor(C_GOLD);
    M5.Display.drawString("< HUB", 81, 42);

    M5.Display.setFont(&fonts::FreeSansBold18pt7b);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.drawString("CAMERA", sw / 2, 42);

    M5.Display.fillRoundRect(sw - 156, 12, 140, 60, 14, C_BLUE);
    M5.Display.setFont(&fonts::FreeSansBold12pt7b);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.drawString("ROT " + String(camRotIdx * 90), sw - 86, 42);

    // SD Card & Hardware Status Strip (y: 92..144)
    M5.Display.fillRoundRect(20, 92, sw - 40, 50, 12, C_PANEL);
    bool sdOk = isMapSdReady();
    M5.Display.fillCircle(44, 117, 8, sdOk ? C_GREEN : C_RED);
    M5.Display.setTextDatum(middle_left);
    M5.Display.setFont(&fonts::FreeSans9pt7b);
    M5.Display.setTextColor(TFT_WHITE);
    String st = sdOk ? (getMapSdStatusText() + "  |  Saves to /DCIM/")
                     : "No microSD detected - Insert FAT32 card & tap here";
    M5.Display.drawString(st, 64, 117);

    // Viewport Frame
    if (camMode == 2) {
        showCurrentGalleryItem();
    } else if (!camHwReady) {
        M5.Display.fillRoundRect(VF_X, VF_Y, VF_W, VF_H, 16, TFT_BLACK);
        M5.Display.drawRoundRect(VF_X, VF_Y, VF_W, VF_H, 16, C_RED);
        M5.Display.setTextDatum(middle_center);
        M5.Display.setFont(&fonts::FreeSansBold18pt7b);
        M5.Display.setTextColor(C_RED);
        M5.Display.drawString("Camera Init Failed", sw / 2, VF_Y + VF_H / 2 - 30);
        M5.Display.setFont(&fonts::FreeSans12pt7b);
        M5.Display.setTextColor(C_GRAY);
        M5.Display.drawString(camErrorMsg, sw / 2, VF_Y + VF_H / 2 + 20);
    } else {
        M5.Display.drawRoundRect(VF_X - 2, VF_Y - 2, VF_W + 4, VF_H + 4, 16, C_TEAL);
    }

    drawCameraControls();
}

// ======================== PUBLIC LIFECYCLE ============================
void initCameraApp() {
    camAppActive = true;
    camMode = 0;
    initMapSdCard(true);
    startCameraHardware();
    drawCameraApp();
}

void handleCameraAppTouch(int tx, int ty) {
    // Top Bar
    if (ty < 84) {
        if (tx >= 16 && tx <= 146) {
            if (recActive) stopVideoRecording();
            stopVideoPlayback();
            stopCameraHardware();
            camAppActive = false;
            playTone(440, 60);
            currentState = STATE_MAIN_MENU;
            drawMainMenu();
            return;
        }
        if (tx >= sw - 156 && tx <= sw - 16) {
            camRotIdx = (camRotIdx + 1) % 4;
            playTone(587, 40);
            drawCameraApp();
            return;
        }
        return;
    }

    // Tap SD status strip to retry microSD mount
    if (ty >= 92 && ty <= 144) {
        bool ok = initMapSdCard(true);
        playTone(ok ? 784 : 220, 70);
        if (camMode == 2 && ok) scanGalleryFiles();
        drawCameraApp();
        return;
    }

    // Bottom 3-Mode Tabs (PHOTO | VIDEO | GALLERY)
    if (ty >= 1172) {
        for (int i = 0; i < 3; i++) {
            int bx = 20 + i * 232;
            if (tx >= bx && tx <= bx + 216) {
                if (recActive) stopVideoRecording();
                stopVideoPlayback();
                camMode = i;
                playTone(523, 45);
                if (camMode == 2) scanGalleryFiles();
                drawCameraApp();
                return;
            }
        }
        return;
    }

    // Gallery Controls (y: 1020..1104)
    if (camMode == 2) {
        if (ty >= 1020 && ty <= 1104 && !galleryItems.empty()) {
            if (tx >= 20 && tx <= 175) {
                galleryIdx = (galleryIdx - 1 + (int)galleryItems.size()) % (int)galleryItems.size();
                playTone(440, 40);
                showCurrentGalleryItem();
                drawCameraControls();
                return;
            }
            if (tx >= 188 && tx <= 354 && galleryItems[galleryIdx].isVideo) {
                playTone(660, 45);
                if (videoPlaying) stopVideoPlayback();
                else startVideoPlayback();
                drawCameraControls();
                return;
            }
            if (tx >= 366 && tx <= 532) {
                stopVideoPlayback();
                SD.remove(galleryItems[galleryIdx].path);
                playTone(300, 90);
                scanGalleryFiles();
                showCurrentGalleryItem();
                drawCameraControls();
                return;
            }
            if (tx >= 545 && tx <= 700) {
                galleryIdx = (galleryIdx + 1) % (int)galleryItems.size();
                playTone(660, 40);
                showCurrentGalleryItem();
                drawCameraControls();
                return;
            }
        }
        // Tap viewport to toggle video play/pause
        if (ty >= VF_Y && ty <= VF_Y + VF_H && !galleryItems.empty() && galleryItems[galleryIdx].isVideo) {
            if (videoPlaying) stopVideoPlayback();
            else startVideoPlayback();
            drawCameraControls();
        }
        return;
    }

    // Photo / Video Shutter & EV Buttons (y: 1005..1130)
    if (ty >= 1005 && ty <= 1130) {
        if (tx >= 24 && tx <= 152) {
            if (evBias > -3) evBias--;
            playTone(440, 40);
            drawCameraControls();
            return;
        }
        if (tx >= sw - 152 && tx <= sw - 24) {
            if (evBias < 3) evBias++;
            playTone(660, 40);
            drawCameraControls();
            return;
        }
        if (tx >= sw / 2 - 68 && tx <= sw / 2 + 68) {
            if (camMode == 0) {
                capturePhotoToSd();
            } else {
                if (recActive) stopVideoRecording();
                else startVideoRecording();
                drawCameraControls();
            }
            return;
        }
    }
}

void loopCameraApp() {
    if (currentState != STATE_CAMERA || !camAppActive) return;

    // Gallery Video Playback Loop
    if (camMode == 2) {
        if (videoPlaying && playFile && millis() >= playNextFrameMs) {
            playNextFrameMs = millis() + playFrameIntervalMs;
            lastActivityTime = millis();
            if (playFile.position() + 8 >= playDataEndOffset) {
                // Loop finished -> stop cleanly
                stopVideoPlayback();
                showCurrentGalleryItem();
                drawCameraControls();
                return;
            }
            uint8_t ch[8];
            if (playFile.read(ch, 8) == 8 && memcmp(ch, "00dc", 4) == 0) {
                uint32_t fsz = readU32LE(ch + 4);
                if (fsz > 128 && fsz <= GALLERY_BUF_CAP) {
                    size_t got = playFile.read(galleryBuf, fsz);
                    if (fsz & 1U) playFile.seek(playFile.position() + 1);
                    drawJpegScaledInViewport(galleryBuf, got);
                } else {
                    stopVideoPlayback();
                    drawCameraControls();
                }
            } else {
                stopVideoPlayback();
                drawCameraControls();
            }
        }
        return;
    }

    // Live Camera Viewfinder + Video Recording Loop (continuous ping-pong DMA — zero blocking receive!)
    if (!camHwReady || !camHandle) return;

    uint32_t seq = dmaFrameSeq;
    if (seq != lastShownSeq) {
        lastShownSeq = seq;
        uint8_t* fb = getLatestReadyFrame();
        if (!fb) return;

        esp_cache_msync(fb, CAM_FB_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
        runSimpleAeAwb((const uint16_t*)fb);
        renderLivePreviewFrame(fb);

        if (recActive) {
            lastActivityTime = millis();
            appendVideoFrame();
        }

        fpsFrames++;
        unsigned long now = millis();
        if (now - fpsWindowMs >= 1000UL) {
            liveFps = fpsFrames;
            fpsFrames = 0;
            fpsWindowMs = now;
            // Refresh just the tiny EV / FPS readout strip (no flicker)
            M5.Display.fillRect(160, 1130, sw - 320, 26, C_DARK);
            M5.Display.setTextDatum(middle_center);
            M5.Display.setFont(&fonts::FreeSans9pt7b);
            M5.Display.setTextColor(C_CYAN);
            String evLbl = "EV " + String(evBias >= 0 ? "+" : "") + String(evBias) +
                           "  |  HD 1280x720  |  " + String(liveFps) + " fps";
            M5.Display.drawString(evLbl, sw / 2, 1142);
        }
    }
}
