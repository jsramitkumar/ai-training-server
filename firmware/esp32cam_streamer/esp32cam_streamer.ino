/*
 * ESP32-CAM Ultra-Low-Latency UDP Streaming Firmware
 * ====================================================
 * Architecture: Fire-and-forget UDP | HTTP control on port 8080
 * UDP streaming port: 5000
 *
 * Wiring:
 *   GPIO4  → Flash LED (built-in on most ESP32-CAM boards)
 *   GPIO33 → Status LED (optional)
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include "esp_camera.h"
#include "esp_timer.h"
#include "driver/ledc.h"

// ─────────────────────────────────────────────
//  USER CONFIGURATION  (edit before flashing)
// ─────────────────────────────────────────────
#define WIFI_SSID        "Airtel_High Link"
#define WIFI_PASSWORD    "Shell@1245"
#define SERVER_IP        "192.168.1.4"   // Python server IP
#define SERVER_UDP_PORT  5000
#define CAMERA_ID        2                // Unique per device (1-255)
#define HTTP_PORT        8080
#define API_KEY          ""                // Leave empty to disable auth

// ─────────────────────────────────────────────
//  CAMERA PINOUT  (AI-Thinker ESP32-CAM)
// ─────────────────────────────────────────────
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

// ─────────────────────────────────────────────
//  PROTOCOL CONSTANTS
// ─────────────────────────────────────────────
#define UDP_PKT_SIZE     1400             // bytes per UDP packet
#define HEADER_SIZE      20               // bytes: cam_id(4)+frame_id(4)+pkt_idx(2)+tot_pkts(2)+ts(8)
#define PAYLOAD_SIZE     (UDP_PKT_SIZE - HEADER_SIZE)
#define MAX_FPS_LIMIT    30

// ─────────────────────────────────────────────
//  PACKET HEADER  (packed, little-endian)
// ─────────────────────────────────────────────
#pragma pack(push, 1)
struct PacketHeader {
    uint32_t camera_id;
    uint32_t frame_id;
    uint16_t packet_index;
    uint16_t total_packets;
    uint64_t timestamp_ms;
};
#pragma pack(pop)

static_assert(sizeof(PacketHeader) == HEADER_SIZE, "PacketHeader size mismatch");

// ─────────────────────────────────────────────
//  RUNTIME STATE
// ─────────────────────────────────────────────
WiFiUDP    udp;
WebServer  httpServer(HTTP_PORT);

volatile uint32_t g_frame_id        = 0;
volatile bool     g_streaming        = true;
volatile uint32_t g_fps_limit        = 15;          // target FPS cap
volatile bool     g_flash_on         = false;
volatile uint8_t  g_flash_brightness = 255;

// Camera config (mutable via HTTP)
struct CameraConfig {
    framesize_t  resolution    = FRAMESIZE_HD;      // 1280×720
    uint8_t      jpeg_quality  = 12;                // 0=best 63=worst
    int8_t       brightness    = 0;
    int8_t       contrast      = 0;
    int8_t       saturation    = 0;
    bool         awb           = true;
    bool         agc           = true;
    bool         aec           = true;
};
CameraConfig g_cfg;

// Runtime stats
struct Stats {
    uint32_t frames_sent     = 0;
    uint32_t frames_dropped  = 0;
    uint32_t packets_sent    = 0;
    uint32_t send_errors     = 0;
    uint32_t last_fps_time   = 0;
    uint32_t fps_counter     = 0;
    float    current_fps     = 0.0f;
};
Stats g_stats;

// ─────────────────────────────────────────────
//  FLASH CONTROL
// ─────────────────────────────────────────────
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
    g_flash_on = on;
    g_flash_brightness = brightness;
    uint32_t duty = on ? brightness : 0;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, FLASH_LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, FLASH_LEDC_CHANNEL);
}

// ─────────────────────────────────────────────
//  CAMERA INIT
// ─────────────────────────────────────────────
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
    s->set_vflip(s, 0);
    s->set_hmirror(s, 0);

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
}

// ─────────────────────────────────────────────
//  RESOLUTION HELPER
// ─────────────────────────────────────────────
framesize_t resolve_framesize(const String &res) {
    if (res == "96x96")   return FRAMESIZE_96X96;
    if (res == "QQVGA")   return FRAMESIZE_QQVGA;
    if (res == "QCIF")    return FRAMESIZE_QCIF;
    if (res == "HQVGA")   return FRAMESIZE_HQVGA;
    if (res == "240x240") return FRAMESIZE_240X240;
    if (res == "QVGA")    return FRAMESIZE_QVGA;
    if (res == "CIF")     return FRAMESIZE_CIF;
    if (res == "HVGA")    return FRAMESIZE_HVGA;
    if (res == "VGA")     return FRAMESIZE_VGA;
    if (res == "SVGA")    return FRAMESIZE_SVGA;
    if (res == "XGA")     return FRAMESIZE_XGA;
    if (res == "HD")      return FRAMESIZE_HD;
    if (res == "SXGA")    return FRAMESIZE_SXGA;
    if (res == "UXGA")    return FRAMESIZE_UXGA;
    return FRAMESIZE_HD;  // default 1280×720
}

String framesize_to_string(framesize_t fs) {
    switch (fs) {
        case FRAMESIZE_96X96:  return "96x96";
        case FRAMESIZE_QQVGA:  return "160x120";
        case FRAMESIZE_QCIF:   return "176x144";
        case FRAMESIZE_HQVGA:  return "240x176";
        case FRAMESIZE_240X240:return "240x240";
        case FRAMESIZE_QVGA:   return "320x240";
        case FRAMESIZE_CIF:    return "400x296";
        case FRAMESIZE_HVGA:   return "480x320";
        case FRAMESIZE_VGA:    return "640x480";
        case FRAMESIZE_SVGA:   return "800x600";
        case FRAMESIZE_XGA:    return "1024x768";
        case FRAMESIZE_HD:     return "1280x720";
        case FRAMESIZE_SXGA:   return "1280x1024";
        case FRAMESIZE_UXGA:   return "1600x1200";
        default:               return "1280x720";
    }
}

// ─────────────────────────────────────────────
//  AUTH CHECK
// ─────────────────────────────────────────────
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

// ─────────────────────────────────────────────
//  HTTP HANDLERS
// ─────────────────────────────────────────────
void handle_status() {
    if (!check_auth()) return;
    StaticJsonDocument<512> doc;
    doc["camera_id"]   = CAMERA_ID;
    doc["resolution"]  = framesize_to_string(g_cfg.resolution);
    doc["jpeg_quality"]= g_cfg.jpeg_quality;
    doc["fps_limit"]   = g_fps_limit;
    doc["fps_actual"]  = g_stats.current_fps;
    doc["flash"]       = g_flash_on;
    doc["flash_brightness"] = g_flash_brightness;
    doc["streaming"]   = g_streaming;
    doc["wifi_rssi"]   = WiFi.RSSI();
    doc["ip"]          = WiFi.localIP().toString();
    doc["frames_sent"] = g_stats.frames_sent;
    doc["frames_dropped"] = g_stats.frames_dropped;
    doc["packets_sent"]   = g_stats.packets_sent;
    doc["free_heap"]   = ESP.getFreeHeap();
    doc["psram_free"]  = psramFound() ? ESP.getFreePsram() : 0;
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
    if (doc.containsKey("resolution")) {
        g_cfg.resolution = resolve_framesize(doc["resolution"].as<String>());
        changed = true;
    }
    if (doc.containsKey("jpeg_quality")) {
        g_cfg.jpeg_quality = constrain((int)doc["jpeg_quality"], 4, 63);
        changed = true;
    }
    if (doc.containsKey("fps_limit")) {
        g_fps_limit = constrain((int)doc["fps_limit"], 1, MAX_FPS_LIMIT);
    }
    if (doc.containsKey("brightness")) {
        g_cfg.brightness = constrain((int)doc["brightness"], -2, 2);
        changed = true;
    }
    if (doc.containsKey("contrast")) {
        g_cfg.contrast = constrain((int)doc["contrast"], -2, 2);
        changed = true;
    }
    if (doc.containsKey("saturation")) {
        g_cfg.saturation = constrain((int)doc["saturation"], -2, 2);
        changed = true;
    }
    if (doc.containsKey("awb")) {
        g_cfg.awb = (bool)doc["awb"];
        changed = true;
    }
    if (doc.containsKey("agc")) {
        g_cfg.agc = (bool)doc["agc"];
        changed = true;
    }
    if (doc.containsKey("aec")) {
        g_cfg.aec = (bool)doc["aec"];
        changed = true;
    }
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
    if (state == "on") {
        flash_set(true, brightness);
    } else if (state == "off") {
        flash_set(false, brightness);
    } else if (state == "toggle") {
        flash_set(!g_flash_on, brightness);
    } else {
        httpServer.send(400, "application/json", "{\"error\":\"Invalid state\"}");
        return;
    }
    StaticJsonDocument<64> resp;
    resp["flash"] = g_flash_on;
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
    Serial.printf("[HTTP] Control server started on port %d\n", HTTP_PORT);
}

// ─────────────────────────────────────────────
//  UDP FRAME SENDER
// ─────────────────────────────────────────────
static uint8_t pkt_buf[UDP_PKT_SIZE];

void send_frame(const uint8_t *data, size_t len) {
    if (!g_streaming) return;

    uint32_t fid = ++g_frame_id;
    uint64_t ts  = (uint64_t)millis();

    uint16_t total_packets = (uint16_t)((len + PAYLOAD_SIZE - 1) / PAYLOAD_SIZE);
    size_t   offset        = 0;

    for (uint16_t i = 0; i < total_packets; i++) {
        size_t chunk = min((size_t)PAYLOAD_SIZE, len - offset);

        PacketHeader hdr;
        hdr.camera_id    = (uint32_t)CAMERA_ID;
        hdr.frame_id     = fid;
        hdr.packet_index = i;
        hdr.total_packets= total_packets;
        hdr.timestamp_ms = ts;

        memcpy(pkt_buf, &hdr, HEADER_SIZE);
        memcpy(pkt_buf + HEADER_SIZE, data + offset, chunk);

        udp.beginPacket(SERVER_IP, SERVER_UDP_PORT);
        udp.write(pkt_buf, HEADER_SIZE + chunk);
        int res = udp.endPacket();
        if (res == 0) g_stats.send_errors++;
        else g_stats.packets_sent++;

        offset += chunk;
    }

    g_stats.frames_sent++;
    g_stats.fps_counter++;

    uint32_t now = millis();
    if (now - g_stats.last_fps_time >= 1000) {
        g_stats.current_fps   = (float)g_stats.fps_counter * 1000.0f / (now - g_stats.last_fps_time);
        g_stats.fps_counter   = 0;
        g_stats.last_fps_time = now;
        Serial.printf("[UDP] FPS: %.1f | Frames: %u | Pkts: %u | Drops: %u\n",
                      g_stats.current_fps, g_stats.frames_sent,
                      g_stats.packets_sent, g_stats.frames_dropped);
    }
}

// ─────────────────────────────────────────────
//  WIFI
// ─────────────────────────────────────────────
void wifi_connect() {
    Serial.printf("[WiFi] Connecting to %s", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    uint32_t t = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - t > 15000) {
            Serial.println("\n[WiFi] Timeout — restarting");
            ESP.restart();
        }
        delay(250);
        Serial.print(".");
    }
    Serial.printf("\n[WiFi] Connected. IP: %s  RSSI: %d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
}

// ─────────────────────────────────────────────
//  SETUP
// ─────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    Serial.println("\n[BOOT] ESP32-CAM UDP Streamer starting...");

    flash_init();
    flash_set(false);

    wifi_connect();
    udp.begin(4999);    // local port (arbitrary, outgoing only)
    Serial.printf("[UDP] Streaming to %s:%d\n", SERVER_IP, SERVER_UDP_PORT);

    if (!camera_init()) {
        Serial.println("[FATAL] Camera init failed — halting");
        while (true) { delay(1000); }
    }

    setup_http_server();

    Serial.println("[BOOT] System ready. Streaming...");
}

// ─────────────────────────────────────────────
//  MAIN LOOP
// ─────────────────────────────────────────────
void loop() {
    // Enforce FPS cap
    static uint32_t last_frame_ms = 0;
    uint32_t frame_interval = 1000 / g_fps_limit;

    httpServer.handleClient();

    // WiFi watchdog
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[WiFi] Lost connection — reconnecting");
        WiFi.reconnect();
        uint32_t t = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t < 10000) {
            delay(250);
        }
        if (WiFi.status() != WL_CONNECTED) {
            ESP.restart();
        }
        return;
    }

    uint32_t now = millis();
    if (now - last_frame_ms < frame_interval) {
        // Yield to HTTP server while waiting
        return;
    }
    last_frame_ms = now;

    if (!g_streaming) return;

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        g_stats.frames_dropped++;
        return;
    }

    send_frame(fb->buf, fb->len);
    esp_camera_fb_return(fb);
}
