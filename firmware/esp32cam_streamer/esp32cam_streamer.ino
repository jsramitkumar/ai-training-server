/*
 * ESP32-CAM UDP Streaming Firmware
 * ==================================
 * Streams JPEG frames via UDP (fire-and-forget, max FPS).
 * HTTP control server on port 8080.
 *
 * Packet format (little-endian, 9-byte header):
 *   [camera_id: 1B][frame_id: 4B][pkt_idx: 2B][total_pkts: 2B][JPEG chunk...]
 *
 * Wiring:
 *   GPIO4  -> Flash LED
 *   GPIO33 -> Status LED (optional)
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUDP.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include "esp_camera.h"
#include "driver/ledc.h"
#include "esp_wifi.h"    // direct IDF access for WIFI_PS_NONE

// -----------------------------------------------------------------------------
//  USER CONFIGURATION  (edit before flashing)
// -----------------------------------------------------------------------------
#define WIFI_SSID        "Airtel_High Link"
#define WIFI_PASSWORD    "Shell@1245"
#define SERVER_IP        "192.168.1.4"   // Python server IP
#define SERVER_UDP_PORT   5000            // must match config.py UDP_PORT
#define CAMERA_ID        2               // unique per device (1-255)
#define HTTP_PORT        8080
#define API_KEY          ""              // leave empty to disable

// -----------------------------------------------------------------------------
//  CAMERA PINOUT  (AI-Thinker ESP32-CAM)
// -----------------------------------------------------------------------------
#define CAM_PIN_PWDN     32
#define CAM_PIN_RESET    -1
#define CAM_PIN_XCLK      0
#define CAM_PIN_SIOD     26
#define CAM_PIN_SIOC     27
#define CAM_PIN_D7       35
#define CAM_PIN_D6       34
#define CAM_PIN_D5       39
#define CAM_PIN_D4       36
#define CAM_PIN_D3       21
#define CAM_PIN_D2       19
#define CAM_PIN_D1       18
#define CAM_PIN_D0        5
#define CAM_PIN_VSYNC    25
#define CAM_PIN_HREF     23
#define CAM_PIN_PCLK     22
#define FLASH_GPIO        4
#define FLASH_LEDC_CHANNEL LEDC_CHANNEL_7
#define FLASH_LEDC_TIMER   LEDC_TIMER_1

// -----------------------------------------------------------------------------
//  STREAMING CONSTANTS
// -----------------------------------------------------------------------------
#define TARGET_FPS        20
#define MAX_FPS_LIMIT     30
// Keep payload well under MTU (1500) so fragmentation never occurs:
//   IP(20) + UDP(8) + header(9) + payload(1400) = 1437B — safely under 1500
// Larger values risk IP fragmentation on WiFi which causes ~50% loss.
#define UDP_PAYLOAD_SIZE  1400
#define HEADER_SIZE       9              // camera_id(1)+frame_id(4)+pkt_idx(2)+total_pkts(2)
#define PKT_BUF_SIZE      (HEADER_SIZE + UDP_PAYLOAD_SIZE)
// Milliseconds to yield between packets.  MUST use delay() (not delayMicroseconds)
// so FreeRTOS yields to the LwIP TCP/IP task which drains the WiFi TX queue.
// Too small → TX queue overflows → packets silently dropped → corrupt frames.
// 2 ms @ 1400B = ~5.6 Mbps peak, matching 2.4GHz WiFi effective UDP throughput.
#define INTER_PACKET_DELAY_MS  2

// -----------------------------------------------------------------------------
//  RUNTIME STATE
// -----------------------------------------------------------------------------
WebServer httpServer(HTTP_PORT);
WiFiUDP   udp;

static uint8_t  g_pkt_buf[PKT_BUF_SIZE];
static uint32_t g_frame_id = 0;

volatile bool     g_streaming        = true;
volatile uint32_t g_fps_limit        = TARGET_FPS;
volatile bool     g_flash_on         = false;
volatile uint8_t  g_flash_brightness = 255;

struct CameraConfig {
    framesize_t  resolution     = FRAMESIZE_VGA;   // 640x480 default — reliable over WiFi
                                                   // HD (1280x720) can be set via HTTP API
    uint8_t      jpeg_quality   = 15;             // 0=best 63=worst
    int8_t       brightness     = 0;
    int8_t       contrast       = 0;
    int8_t       saturation     = 0;
    bool         awb            = true;
    bool         agc            = true;
    bool         aec            = true;
    bool         vflip          = false;
    bool         hmirror        = false;
    uint8_t      special_effect = 0;   // 0=Off 1=Neg 2=Gray 3=Red 4=Green 5=Blue 6=Sepia
    uint8_t      wb_mode        = 0;   // 0=Auto 1=Sunny 2=Cloudy 3=Office 4=Home
};
CameraConfig g_cfg;

struct Stats {
    uint32_t frames_sent    = 0;
    uint32_t frames_dropped = 0;
    uint32_t packets_sent   = 0;
    uint32_t last_fps_time  = 0;
    uint32_t fps_counter    = 0;
    float    current_fps    = 0.0f;
};
Stats g_stats;

// -----------------------------------------------------------------------------
//  FLASH CONTROL
// -----------------------------------------------------------------------------
void flash_init() {
    ledc_timer_config_t timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num       = FLASH_LEDC_TIMER,
        .freq_hz         = 5000,
        .clk_cfg         = LEDC_AUTO_CLK
    };
    ledc_timer_config(&timer);

    ledc_channel_config_t ch = {
        .gpio_num   = FLASH_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = FLASH_LEDC_CHANNEL,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = FLASH_LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0
    };
    ledc_channel_config(&ch);
}

void flash_set(bool on, uint8_t brightness = 255) {
    g_flash_on         = on;
    g_flash_brightness = brightness;
    uint32_t duty      = on ? brightness : 0;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, FLASH_LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, FLASH_LEDC_CHANNEL);
}

// -----------------------------------------------------------------------------
//  CAMERA INIT
// -----------------------------------------------------------------------------
bool camera_init() {
    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer   = LEDC_TIMER_0;
    config.pin_d0       = CAM_PIN_D0;
    config.pin_d1       = CAM_PIN_D1;
    config.pin_d2       = CAM_PIN_D2;
    config.pin_d3       = CAM_PIN_D3;
    config.pin_d4       = CAM_PIN_D4;
    config.pin_d5       = CAM_PIN_D5;
    config.pin_d6       = CAM_PIN_D6;
    config.pin_d7       = CAM_PIN_D7;
    config.pin_xclk     = CAM_PIN_XCLK;
    config.pin_pclk     = CAM_PIN_PCLK;
    config.pin_vsync    = CAM_PIN_VSYNC;
    config.pin_href     = CAM_PIN_HREF;
    config.pin_sccb_sda = CAM_PIN_SIOD;
    config.pin_sccb_scl = CAM_PIN_SIOC;
    config.pin_pwdn     = CAM_PIN_PWDN;
    config.pin_reset    = CAM_PIN_RESET;
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;
    config.grab_mode    = CAMERA_GRAB_LATEST;   // always newest frame

    if (psramFound()) {
        config.frame_size   = g_cfg.resolution;
        config.jpeg_quality = g_cfg.jpeg_quality;
        config.fb_count     = 2;
        config.fb_location  = CAMERA_FB_IN_PSRAM;
    } else {
        config.frame_size   = FRAMESIZE_SVGA;
        config.jpeg_quality = 20;
        config.fb_count     = 1;
        config.fb_location  = CAMERA_FB_IN_DRAM;
    }

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("[CAM] Init failed: 0x%x\n", err);
        return false;
    }

    sensor_t *s = esp_camera_sensor_get();
    s->set_framesize(s, g_cfg.resolution);
    s->set_quality(s, g_cfg.jpeg_quality);
    s->set_brightness(s, g_cfg.brightness);
    s->set_contrast(s, g_cfg.contrast);
    s->set_saturation(s, g_cfg.saturation);
    s->set_whitebal(s, g_cfg.awb ? 1 : 0);
    s->set_gain_ctrl(s, g_cfg.agc ? 1 : 0);
    s->set_exposure_ctrl(s, g_cfg.aec ? 1 : 0);
    s->set_vflip(s, g_cfg.vflip ? 1 : 0);
    s->set_hmirror(s, g_cfg.hmirror ? 1 : 0);
    s->set_special_effect(s, g_cfg.special_effect);
    s->set_wb_mode(s, g_cfg.wb_mode);

    Serial.println("[CAM] Initialized OK");
    return true;
}

void camera_apply_config() {
    sensor_t *s = esp_camera_sensor_get();
    if (!s) return;
    s->set_framesize(s, g_cfg.resolution);
    s->set_quality(s, g_cfg.jpeg_quality);
    s->set_brightness(s, g_cfg.brightness);
    s->set_contrast(s, g_cfg.contrast);
    s->set_saturation(s, g_cfg.saturation);
    s->set_whitebal(s, g_cfg.awb ? 1 : 0);
    s->set_gain_ctrl(s, g_cfg.agc ? 1 : 0);
    s->set_exposure_ctrl(s, g_cfg.aec ? 1 : 0);
    s->set_vflip(s, g_cfg.vflip ? 1 : 0);
    s->set_hmirror(s, g_cfg.hmirror ? 1 : 0);
    s->set_special_effect(s, g_cfg.special_effect);
    s->set_wb_mode(s, g_cfg.wb_mode);
}

// -----------------------------------------------------------------------------
//  RESOLUTION HELPERS
// -----------------------------------------------------------------------------
framesize_t resolve_framesize(const String &res) {
    if (res == "96x96")    return FRAMESIZE_96X96;
    if (res == "QQVGA")    return FRAMESIZE_QQVGA;
    if (res == "QCIF")     return FRAMESIZE_QCIF;
    if (res == "HQVGA")    return FRAMESIZE_HQVGA;
    if (res == "240x240")  return FRAMESIZE_240X240;
    if (res == "QVGA")     return FRAMESIZE_QVGA;
    if (res == "CIF")      return FRAMESIZE_CIF;
    if (res == "HVGA")     return FRAMESIZE_HVGA;
    if (res == "VGA")      return FRAMESIZE_VGA;
    if (res == "SVGA")     return FRAMESIZE_SVGA;
    if (res == "XGA")      return FRAMESIZE_XGA;
    if (res == "HD")       return FRAMESIZE_HD;
    if (res == "SXGA")     return FRAMESIZE_SXGA;
    if (res == "UXGA")     return FRAMESIZE_UXGA;
    return FRAMESIZE_HD;
}

String framesize_to_string(framesize_t fs) {
    switch (fs) {
        case FRAMESIZE_96X96:   return "96x96";
        case FRAMESIZE_QQVGA:   return "160x120";
        case FRAMESIZE_QCIF:    return "176x144";
        case FRAMESIZE_HQVGA:   return "240x176";
        case FRAMESIZE_240X240: return "240x240";
        case FRAMESIZE_QVGA:    return "320x240";
        case FRAMESIZE_CIF:     return "400x296";
        case FRAMESIZE_HVGA:    return "480x320";
        case FRAMESIZE_VGA:     return "640x480";
        case FRAMESIZE_SVGA:    return "800x600";
        case FRAMESIZE_XGA:     return "1024x768";
        case FRAMESIZE_HD:      return "1280x720";
        case FRAMESIZE_SXGA:    return "1280x1024";
        case FRAMESIZE_UXGA:    return "1600x1200";
        default:                return "1280x720";
    }
}

// -----------------------------------------------------------------------------
//  AUTH CHECK
// -----------------------------------------------------------------------------
bool check_auth() {
    if (strlen(API_KEY) == 0) return true;
    if (!httpServer.hasHeader("X-API-Key")) {
        httpServer.send(401, "application/json", "{\"error\":\"Unauthorized\"}");
        return false;
    }
    if (httpServer.header("X-API-Key") != String(API_KEY)) {
        httpServer.send(403, "application/json", "{\"error\":\"Forbidden\"}");
        return false;
    }
    return true;
}

// -----------------------------------------------------------------------------
//  HTTP HANDLERS
// -----------------------------------------------------------------------------
void handle_status() {
    if (!check_auth()) return;
    StaticJsonDocument<512> doc;
    doc["camera_id"]        = CAMERA_ID;
    doc["resolution"]       = framesize_to_string(g_cfg.resolution);
    doc["jpeg_quality"]     = g_cfg.jpeg_quality;
    doc["fps_limit"]        = g_fps_limit;
    doc["fps_actual"]       = g_stats.current_fps;
    doc["flash"]            = g_flash_on;
    doc["flash_brightness"] = g_flash_brightness;
    doc["streaming"]        = g_streaming;
    doc["wifi_rssi"]        = WiFi.RSSI();
    doc["ip"]               = WiFi.localIP().toString();
    doc["vflip"]            = g_cfg.vflip;
    doc["hmirror"]          = g_cfg.hmirror;
    doc["special_effect"]   = g_cfg.special_effect;
    doc["wb_mode"]          = g_cfg.wb_mode;
    doc["frames_sent"]      = g_stats.frames_sent;
    doc["frames_dropped"]   = g_stats.frames_dropped;
    doc["packets_sent"]     = g_stats.packets_sent;
    doc["fps"]              = g_stats.current_fps;
    doc["free_heap"]        = ESP.getFreeHeap();
    doc["psram_free"]       = psramFound() ? ESP.getFreePsram() : 0;
    String out;
    serializeJson(doc, out);
    httpServer.send(200, "application/json", out);
}

void handle_camera_config() {
    if (!check_auth()) return;
    if (httpServer.method() != HTTP_POST) {
        httpServer.send(405, "application/json", "{\"error\":\"Method Not Allowed\"}");
        return;
    }
    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, httpServer.arg("plain"));
    if (err) {
        httpServer.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
    }
    bool changed = false;
    if (doc.containsKey("resolution"))     { g_cfg.resolution     = resolve_framesize(doc["resolution"].as<String>()); changed = true; }
    if (doc.containsKey("jpeg_quality"))   { g_cfg.jpeg_quality   = constrain((int)doc["jpeg_quality"],   4, 63);       changed = true; }
    if (doc.containsKey("fps_limit"))      { g_fps_limit           = constrain((int)doc["fps_limit"],      1, MAX_FPS_LIMIT); }
    if (doc.containsKey("brightness"))     { g_cfg.brightness      = constrain((int)doc["brightness"],    -2, 2);        changed = true; }
    if (doc.containsKey("contrast"))       { g_cfg.contrast        = constrain((int)doc["contrast"],      -2, 2);        changed = true; }
    if (doc.containsKey("saturation"))     { g_cfg.saturation      = constrain((int)doc["saturation"],    -2, 2);        changed = true; }
    if (doc.containsKey("awb"))            { g_cfg.awb             = (bool)doc["awb"];                                    changed = true; }
    if (doc.containsKey("agc"))            { g_cfg.agc             = (bool)doc["agc"];                                    changed = true; }
    if (doc.containsKey("aec"))            { g_cfg.aec             = (bool)doc["aec"];                                    changed = true; }
    if (doc.containsKey("vflip"))          { g_cfg.vflip           = (bool)doc["vflip"];                                  changed = true; }
    if (doc.containsKey("hmirror"))        { g_cfg.hmirror         = (bool)doc["hmirror"];                                changed = true; }
    if (doc.containsKey("special_effect")) { g_cfg.special_effect  = constrain((int)doc["special_effect"], 0, 6);        changed = true; }
    if (doc.containsKey("wb_mode"))        { g_cfg.wb_mode         = constrain((int)doc["wb_mode"],         0, 4);       changed = true; }
    if (changed) camera_apply_config();
    httpServer.send(200, "application/json", "{\"status\":\"ok\"}");
}

void handle_flash() {
    if (!check_auth()) return;
    if (httpServer.method() != HTTP_POST) {
        httpServer.send(405, "application/json", "{\"error\":\"Method Not Allowed\"}");
        return;
    }
    StaticJsonDocument<128> doc;
    DeserializationError err = deserializeJson(doc, httpServer.arg("plain"));
    if (err) {
        httpServer.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
    }
    uint8_t brightness = g_flash_brightness;
    if (doc.containsKey("brightness")) {
        brightness = constrain((int)doc["brightness"], 0, 255);
    }
    String state = doc["state"] | "off";
    if      (state == "on")     flash_set(true,        brightness);
    else if (state == "off")    flash_set(false,       brightness);
    else if (state == "toggle") flash_set(!g_flash_on, brightness);
    else {
        httpServer.send(400, "application/json", "{\"error\":\"Invalid state\"}");
        return;
    }
    StaticJsonDocument<64> resp;
    resp["flash"]      = g_flash_on;
    resp["brightness"] = g_flash_brightness;
    String out;
    serializeJson(resp, out);
    httpServer.send(200, "application/json", out);
}

void handle_stream_control() {
    if (!check_auth()) return;
    if (httpServer.method() != HTTP_POST) {
        httpServer.send(405, "application/json", "{\"error\":\"Method Not Allowed\"}");
        return;
    }
    StaticJsonDocument<64> doc;
    DeserializationError err = deserializeJson(doc, httpServer.arg("plain"));
    if (err) {
        httpServer.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
    }
    if (doc.containsKey("streaming")) {
        g_streaming = (bool)doc["streaming"];
    }
    httpServer.send(200, "application/json", "{\"status\":\"ok\"}");
}

void handle_restart() {
    if (!check_auth()) return;
    httpServer.send(200, "application/json", "{\"status\":\"restarting\"}");
    delay(200);
    ESP.restart();
}

void handle_not_found() {
    httpServer.send(404, "application/json", "{\"error\":\"Not Found\"}");
}

void setup_http_server() {
    httpServer.on("/status",         HTTP_GET,  handle_status);
    httpServer.on("/camera/config",  HTTP_POST, handle_camera_config);
    httpServer.on("/flash",          HTTP_POST, handle_flash);
    httpServer.on("/stream/control", HTTP_POST, handle_stream_control);
    httpServer.on("/restart",        HTTP_POST, handle_restart);
    httpServer.onNotFound(handle_not_found);
    const char *headerKeys[] = {"X-API-Key"};
    httpServer.collectHeaders(headerKeys, 1);
    httpServer.begin();
    Serial.printf("[HTTP] Control server on port %d\n", HTTP_PORT);
}

// -----------------------------------------------------------------------------
//  WIFI — max TX power, zero power-save
// -----------------------------------------------------------------------------

// Apply all power / performance settings in one place so the same call
// can be reused after every reconnect.
static void wifi_set_max_power() {
    // Arduino layer
    WiFi.setSleep(false);                        // disable modem-sleep via Arduino
    WiFi.setTxPower(WIFI_POWER_19_5dBm);        // 19.5 dBm — hardware maximum

    // IDF layer (belt-and-suspenders: Arduino setSleep(false) maps to this
    // but only after association and doesn't persist across reconnect on all
    // IDF versions)
    esp_wifi_set_ps(WIFI_PS_NONE);              // NONE = no power saving at all

    // Bump the WiFi task CPU affinity to let it drain the TX queue faster.
    // This reduces mid-frame packet loss under burst load.
    esp_wifi_set_max_tx_power(84);              // 84 = 21 dBm in 0.25 dBm units
                                                // (hardware clips to 19.5 dBm safely)
}

void wifi_connect() {
    Serial.printf("[WiFi] Connecting to %s", WIFI_SSID);

    WiFi.persistent(false);          // don't write SSID/password to flash every boot
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);     // must be before begin()
    WiFi.setSleep(false);            // pre-connect hint
    WiFi.setTxPower(WIFI_POWER_19_5dBm);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    uint32_t t = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - t > 15000) {
            Serial.println("\n[WiFi] Timeout - restarting");
            ESP.restart();
        }
        delay(100);
        Serial.print(".");
    }

    // Lock in power settings now that the association is complete.
    // esp_wifi_set_ps() is only effective after the driver is in connected state.
    wifi_set_max_power();

    Serial.printf("\n[WiFi] Connected. IP: %s  RSSI: %d dBm  TxPwr: 19.5dBm  PS: NONE\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
}

// -----------------------------------------------------------------------------
//  UDP FRAME SENDER  (fire and forget)
// -----------------------------------------------------------------------------
void send_frame(camera_fb_t *fb) {
    // Require a plausible JPEG: SOI marker (FF D8) and at least 64 bytes
    if (fb->len < 64 || fb->buf[0] != 0xFF || fb->buf[1] != 0xD8) {
        Serial.printf("[UDP] Bad frame (len=%d hdr=0x%02x%02x) - skipping\n",
                      fb->len, fb->buf[0], fb->buf[1]);
        g_stats.frames_dropped++;
        return;
    }

    uint16_t total_pkts = (uint16_t)((fb->len + UDP_PAYLOAD_SIZE - 1) / UDP_PAYLOAD_SIZE);

    // Log every 30 frames (~1 second) for diagnostics
    if (g_stats.frames_sent % 30 == 0) {
        Serial.printf("[UDP] frame %u  len=%d  pkts=%u  heap=%u\n",
                      g_frame_id, fb->len, total_pkts, ESP.getFreeHeap());
    }

    for (uint16_t i = 0; i < total_pkts; i++) {
        size_t offset = (size_t)i * UDP_PAYLOAD_SIZE;
        size_t chunk  = (fb->len - offset < (size_t)UDP_PAYLOAD_SIZE)
                        ? (fb->len - offset)
                        : (size_t)UDP_PAYLOAD_SIZE;
        if (chunk == 0) break;

        // Build packet as ONE contiguous block: header then JPEG bytes.
        // We MUST NOT call udp.write() twice — the second call triggers
        // AsyncUDPMessage::realloc() which can silently fail and drop the
        // entire packet, producing corrupt / zero-payload datagrams.
        g_pkt_buf[0] = (uint8_t)CAMERA_ID;
        memcpy(g_pkt_buf + 1, &g_frame_id,        4);
        memcpy(g_pkt_buf + 5, &i,                  2);
        memcpy(g_pkt_buf + 7, &total_pkts,         2);
        memcpy(g_pkt_buf + HEADER_SIZE, fb->buf + offset, chunk);  // payload

        udp.beginPacket(SERVER_IP, SERVER_UDP_PORT);
        udp.write(g_pkt_buf, (size_t)(HEADER_SIZE + chunk));  // single atomic write
        udp.endPacket();

        g_stats.packets_sent++;

        // yield() lets the FreeRTOS LwIP task drain the WiFi TX queue between
        // packets.  Without this the TX ring overflows, silently dropping UDP
        // datagrams and producing frames with missing chunks (visual distortion).
        if (total_pkts > 1) {
            delay(INTER_PACKET_DELAY_MS);
        }
    }
    g_frame_id++;
}

// -----------------------------------------------------------------------------
//  SETUP
// -----------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    Serial.println("\n[BOOT] ESP32-CAM UDP Streamer starting...");

    flash_init();
    flash_set(false);

    wifi_connect();

    if (!camera_init()) {
        Serial.println("[FATAL] Camera init failed - halting");
        while (true) { delay(1000); }
    }

    udp.begin(5001);                // bind explicit local port for sending
    Serial.printf("[UDP] Will stream to %s:%d\n", SERVER_IP, SERVER_UDP_PORT);

    setup_http_server();

    Serial.printf("[BOOT] Ready. Streaming UDP to %s:%d  HTTP ctrl on :%d\n",
                  SERVER_IP, SERVER_UDP_PORT, HTTP_PORT);
}

// -----------------------------------------------------------------------------
//  MAIN LOOP
// -----------------------------------------------------------------------------
void loop() {
    static uint32_t last_frame_ms = 0;

    httpServer.handleClient();

    // WiFi watchdog
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[WiFi] Lost connection - waiting for auto-reconnect");
        uint32_t t = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t < 5000) {
            delay(100);
        }
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[WiFi] Auto-reconnect failed - restarting");
            ESP.restart();
        }
        wifi_set_max_power();   // re-lock 19.5dBm + WIFI_PS_NONE after reconnect
        Serial.printf("[WiFi] Reconnected. RSSI: %d dBm  TxPwr: 19.5dBm  PS: NONE\n", WiFi.RSSI());
        return;
    }

    if (!g_streaming) return;

    uint32_t now      = millis();
    uint32_t interval = 1000u / constrain(g_fps_limit, 1u, (uint32_t)MAX_FPS_LIMIT);
    if (now - last_frame_ms < interval) return;

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        g_stats.frames_dropped++;
        return;
    }

    send_frame(fb);
    esp_camera_fb_return(fb);
    last_frame_ms = now;

    g_stats.frames_sent++;
    g_stats.fps_counter++;
    if (now - g_stats.last_fps_time >= 1000) {
        g_stats.current_fps   = (float)g_stats.fps_counter * 1000.0f
                                / (float)(now - g_stats.last_fps_time);
        g_stats.fps_counter   = 0;
        g_stats.last_fps_time = now;
        Serial.printf("[UDP] FPS: %.1f  frames: %u  pkts: %u  heap: %u\n",
                      g_stats.current_fps, g_stats.frames_sent,
                      g_stats.packets_sent, ESP.getFreeHeap());
    }
}
