# ESP32-CAM Ultra-Low-Latency Multi-Camera Streaming System

A production-ready fire-and-forget UDP video streaming stack for ESP32-CAM devices with a Python server and real-time web dashboard.

---

## Architecture

```
ESP32-CAM ─── UDP:5000 ──► Python Server ─── WebSocket ──► Browser Dashboard
               (frames)        │                             (live video grid)
                               │
                          HTTP:8000 (REST API + static files)
                               │
ESP32-CAM ◄── HTTP:8080 ───────┘  (control proxy: flash, config, restart)
```

---

## Directory Structure

```
ai-training-server/
├── firmware/
│   └── esp32cam_streamer/
│       └── esp32cam_streamer.ino   ← Flash this to each ESP32-CAM
│
├── server/
│   ├── main.py                     ← Python server entrypoint
│   ├── app.py                      ← FastAPI app (REST + WebSocket)
│   ├── udp_receiver.py             ← Asyncio UDP listener
│   ├── camera_manager.py           ← Frame reassembly + freshness filter
│   ├── config.py                   ← Settings (env-overridable)
│   └── requirements.txt
│
└── dashboard/
    ├── index.html                  ← Single-page dashboard
    └── static/
        ├── style.css
        └── app.js
```

---

## Quick Start

### 1. Server setup

```bash
cd server
pip install -r requirements.txt
python main.py
```

The server starts:
- **UDP listener**: `0.0.0.0:5000` (receives ESP32-CAM frames)
- **HTTP server**: `http://0.0.0.0:8000` (dashboard + REST API)

### 2. Firmware

1. Open `firmware/esp32cam_streamer/esp32cam_streamer.ino` in Arduino IDE
2. Install required libraries:
   - `ArduinoJson` (v6+)
   - `ESP32 Arduino core` (Espressif, via Board Manager)
3. Edit the configuration block at the top of the `.ino` file:

```cpp
#define WIFI_SSID       "YOUR_SSID"
#define WIFI_PASSWORD   "YOUR_PASSWORD"
#define SERVER_IP       "192.168.1.100"   // IP of your Python server
#define CAMERA_ID       1                 // Unique per device (1–255)
```

4. Select board: **AI Thinker ESP32-CAM** → Upload

Each device gets a unique `CAMERA_ID`. Repeat for every camera.

### 3. Dashboard

Open `http://<server-ip>:8000` in any browser.

---

## Configuration

### Server (environment variables / `.env` file)

| Variable               | Default | Description                              |
|------------------------|---------|------------------------------------------|
| `UDP_HOST`             | `0.0.0.0` | UDP bind address                       |
| `UDP_PORT`             | `5000`  | UDP port for ESP32-CAM frames            |
| `HTTP_HOST`            | `0.0.0.0` | HTTP bind address                      |
| `HTTP_PORT`            | `8000`  | Dashboard + REST API port                |
| `FRESHNESS_MS`         | `200`   | Max frame age before discard (ms)        |
| `REASSEMBLY_TIMEOUT_MS`| `500`   | Incomplete frame eviction timeout (ms)   |
| `MAX_CAMERAS`          | `20`    | Maximum concurrent cameras               |
| `API_KEY`              | *(empty)* | Optional REST/WS auth key             |
| `WS_INTERVAL_MS`       | `50`    | WebSocket stats push interval (ms)       |

Create a `.env` file next to `main.py` to override:

```env
API_KEY=mysecretkey123
HTTP_PORT=8080
FRESHNESS_MS=150
```

### Firmware

| Constant           | Default         | Description                         |
|--------------------|-----------------|-------------------------------------|
| `WIFI_SSID`        | –               | WiFi network name                   |
| `WIFI_PASSWORD`    | –               | WiFi password                       |
| `SERVER_IP`        | –               | Python server IP address            |
| `SERVER_UDP_PORT`  | `5000`          | Must match server `UDP_PORT`        |
| `CAMERA_ID`        | `1`             | Unique ID per device                |
| `HTTP_PORT`        | `8080`          | ESP32 control API port              |
| `API_KEY`          | *(empty)*       | Optional API key (match server key) |

---

## UDP Packet Protocol

All packets are binary, little-endian, packed (no padding).

```
Offset  Size  Type      Field
0       4     uint32    camera_id
4       4     uint32    frame_id
8       2     uint16    packet_index
10      2     uint16    total_packets
12      8     uint64    timestamp_ms   (ESP32 millis)
20      N     bytes     JPEG payload
────────────────────────────────
Total header: 20 bytes
Max packet:   1400 bytes  (safe below MTU)
Max payload:  1380 bytes  per packet
```

---

## REST API

Base URL: `http://<server>:8000`

All endpoints accept/return `application/json`.  
If `API_KEY` is set, include `X-API-Key: <key>` header.

| Method | Path                              | Description                        |
|--------|-----------------------------------|------------------------------------|
| GET    | `/api/cameras`                    | List all detected cameras          |
| GET    | `/api/cameras/{id}`               | Single camera stats                |
| GET    | `/api/cameras/{id}/snapshot`      | Latest JPEG frame (image/jpeg)     |
| GET    | `/api/cameras/{id}/status`        | Proxied status from ESP32          |
| POST   | `/api/cameras/{id}/config`        | Update resolution / quality / fps  |
| POST   | `/api/cameras/{id}/flash`         | Flash control                      |
| POST   | `/api/cameras/{id}/stream`        | Pause / resume streaming           |
| POST   | `/api/cameras/{id}/restart`       | Restart ESP32                      |

### POST /api/cameras/{id}/config

```json
{
  "resolution":   "HD",
  "jpeg_quality": 12,
  "fps_limit":    15,
  "brightness":   0,
  "contrast":     0,
  "saturation":   0,
  "awb":          true,
  "agc":          true,
  "aec":          true
}
```

Valid resolutions: `QVGA`, `VGA`, `SVGA`, `HD`, `SXGA`, `UXGA`

### POST /api/cameras/{id}/flash

```json
{ "state": "on",     "brightness": 200 }
{ "state": "off" }
{ "state": "toggle" }
```

---

## WebSocket API

Connect to `ws://<server>:8000/ws/stream`

Messages received (JSON):

```jsonc
// New frame
{
  "type":       "frame",
  "camera_id":  1,
  "frame_id":   4821,
  "latency_ms": 12.4,
  "fps":        14.2,
  "data":       "<base64 JPEG>"
}

// Stats push (every 1s)
{
  "type": "stats",
  "cameras": [
    {
      "camera_id":        1,
      "source_ip":        "192.168.1.42",
      "fps":              14.2,
      "latency_ms":       12.4,
      "frames_received":  8200,
      "frames_dropped":   14,
      "packets_received": 82140,
      "packets_lost":     0,
      "resolution":       "1280x720",
      "online":           true
    }
  ]
}

// Keepalive
{ "type": "ping" }
```

---

## Dashboard Features

| Feature               | Description                                       |
|-----------------------|---------------------------------------------------|
| Multi-camera grid     | 1–4 column layout, configurable in settings       |
| Live video            | Canvas-based JPEG rendering, ~15 fps target       |
| Per-camera HUD        | FPS, latency, resolution, signal strength         |
| Flash control         | ON / OFF with brightness slider                   |
| Camera configuration  | Resolution, JPEG quality, FPS limit               |
| Pause / Resume        | Per-camera stream toggle                          |
| Restart camera        | Remote ESP32 restart via HTTP proxy               |
| Offline detection     | Panel dims + overlay if no frame for 5 s          |
| Settings panel        | Server URL, API key, grid columns                 |

---

## Performance Targets

| Metric             | Target           |
|--------------------|------------------|
| Resolution         | 1280×720 (HD)    |
| Frame rate         | 10–15 fps        |
| End-to-end latency | < 200 ms         |
| UDP packet size    | 1400 bytes       |
| Concurrent cameras | Up to 20         |
| Freshness filter   | 200 ms threshold |

---

## Reliability Design

- **Fire-and-forget** UDP: no retransmits, no ACKs
- **Freshness filter**: frames older than 200 ms are silently dropped server-side
- **Incomplete frame eviction**: reassembly buffers time out after 500 ms
- **WiFi watchdog**: ESP32 auto-reconnects and reboots if reconnection fails
- **WebSocket backpressure**: slow browser clients have frames dropped (never block sender)
- **No frame backlog**: always process newest frame, skip stale ones

---

## Security Notes

- Enable `API_KEY` in production to secure control endpoints
- The server does **not** expose raw camera streams publicly; all clients go through the WebSocket
- The HTTP control proxy (`/api/cameras/{id}/config|flash|restart`) is authenticated if `API_KEY` is set
- Firmware API key (`#define API_KEY`) must match the server's `API_KEY` setting if used

---

## Troubleshooting

| Symptom                    | Likely cause / fix                                         |
|----------------------------|------------------------------------------------------------|
| No cameras appear          | Check `SERVER_IP` in firmware — must be Python server IP   |
| High latency               | Reduce `jpeg_quality` (higher number = more compression)   |
| Frames frequently dropped  | Increase `FRESHNESS_MS` or reduce camera fps_limit         |
| Flash not responding       | Check GPIO4 wiring; verify LEDC init in firmware           |
| Camera offline after 5 s   | Camera stopped transmitting; check WiFi; restart camera    |
| Can't connect to dashboard | Ensure port 8000 is not blocked by firewall                |
