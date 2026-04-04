"""
Camera Manager
==============
- Tracks all known cameras
- Reassembles UDP packet fragments into complete JPEG frames
- Applies freshness filter (drop frames older than FRESHNESS_MS)
- Maintains per-camera statistics
- Notifies WebSocket subscribers of new frames (base64 JPEG)
"""

import asyncio
import base64
import logging
import time
from collections import defaultdict
from dataclasses import dataclass, field
from typing import Callable, Dict, List, Optional, Set

from config import settings

log = logging.getLogger("camera_manager")

# ─────────────────────────────────────────────
#  TYPES
# ─────────────────────────────────────────────

FrameCallback = Callable[[int, bytes, dict], None]   # (camera_id, jpeg_bytes, meta)


@dataclass
class FrameBuffer:
    """Reassembly buffer for a single (camera_id, frame_id) pair."""
    camera_id:    int
    frame_id:     int
    total_pkts:   int
    ts_ms:        int                  # ESP32 timestamp
    recv_mono:    float                # first packet arrival (monotonic)
    chunks:       Dict[int, bytes] = field(default_factory=dict)

    @property
    def complete(self) -> bool:
        return len(self.chunks) == self.total_pkts

    def assemble(self) -> bytes:
        return b"".join(self.chunks[i] for i in range(self.total_pkts))


@dataclass
class CameraState:
    camera_id:     int
    source_ip:     str
    connected_at:  float = field(default_factory=time.monotonic)
    last_seen:     float = field(default_factory=time.monotonic)

    # Stats
    frames_received:  int = 0
    frames_dropped:   int = 0
    packets_received: int = 0
    packets_lost:     int = 0   # estimated: incomplete frames * missing pkts

    # Current frame info
    last_frame_id:    int   = 0
    last_latency_ms:  float = 0.0
    current_fps:      float = 0.0
    resolution:       str   = "unknown"
    jpeg_quality:     int   = 0
    wifi_rssi:        Optional[int] = None

    # FPS tracking
    _fps_counter:  int   = field(default=0, repr=False)
    _fps_window:   float = field(default_factory=time.monotonic, repr=False)

    # Latest JPEG frame (raw bytes)
    latest_frame:  Optional[bytes] = field(default=None, repr=False)

    def tick_fps(self):
        self._fps_counter += 1
        now = time.monotonic()
        elapsed = now - self._fps_window
        if elapsed >= 1.0:
            self.current_fps   = self._fps_counter / elapsed
            self._fps_counter  = 0
            self._fps_window   = now

    def to_dict(self) -> dict:
        return {
            "camera_id":        self.camera_id,
            "source_ip":        self.source_ip,
            "connected_at":     self.connected_at,
            "last_seen":        self.last_seen,
            "fps":              round(self.current_fps, 1),
            "latency_ms":       round(self.last_latency_ms, 1),
            "frames_received":  self.frames_received,
            "frames_dropped":   self.frames_dropped,
            "packets_received": self.packets_received,
            "packets_lost":     self.packets_lost,
            "resolution":       self.resolution,
            "jpeg_quality":     self.jpeg_quality,
            "wifi_rssi":        self.wifi_rssi,
            "online":           (time.monotonic() - self.last_seen) < 5.0,
        }


# ─────────────────────────────────────────────
#  MANAGER
# ─────────────────────────────────────────────

class CameraManager:
    def __init__(self):
        self._cameras:   Dict[int, CameraState]  = {}
        self._buffers:   Dict[tuple, FrameBuffer] = {}   # (camera_id, frame_id) → buffer
        self._subscribers: Set[asyncio.Queue]     = set()
        self._lock = asyncio.Lock()

        # Monotonic reference used to convert ESP32 millis to approximate wall clock delta
        self._mono_start = time.monotonic()
        self._server_start_epoch = time.time()

    # ── Packet ingestion (called from UDP protocol, NOT async) ──────────────

    def on_packet(
        self,
        camera_id: int,
        frame_id:  int,
        pkt_idx:   int,
        total_pkts:int,
        ts_ms:     int,
        payload:   bytes,
        source_ip: str,
        recv_monotonic: float,
    ):
        # Register new camera
        if camera_id not in self._cameras:
            state = CameraState(camera_id=camera_id, source_ip=source_ip)
            self._cameras[camera_id] = state
            log.info(f"[CAMERA] New camera #{camera_id} detected from {source_ip}")

        cam = self._cameras[camera_id]
        cam.last_seen      = recv_monotonic
        cam.packets_received += 1

        # Freshness pre-check: estimate current server-side "ESP millis" by
        # comparing elapsed monotonic time.  This handles the case where the
        # ESP32 millis and server wall clock have different epochs.
        # We track the skew lazily on the first complete frame.
        key = (camera_id, frame_id)

        if key not in self._buffers:
            if len(self._buffers) > settings.MAX_CAMERAS * 32:
                # Safety: evict oldest half of buffers
                sorted_keys = sorted(self._buffers.keys(), key=lambda k: self._buffers[k].recv_mono)
                for old_key in sorted_keys[: len(sorted_keys) // 2]:
                    del self._buffers[old_key]

            self._buffers[key] = FrameBuffer(
                camera_id=camera_id,
                frame_id=frame_id,
                total_pkts=total_pkts,
                ts_ms=ts_ms,
                recv_mono=recv_monotonic,
            )

        buf = self._buffers[key]
        if pkt_idx < total_pkts:
            buf.chunks[pkt_idx] = payload

        if buf.complete:
            self._on_frame_complete(cam, buf, recv_monotonic)
            del self._buffers[key]

        # Expire old incomplete buffers (non-blocking scan every ~60 packets)
        if cam.packets_received % 60 == 0:
            self._expire_buffers(recv_monotonic)

    def _on_frame_complete(self, cam: CameraState, buf: FrameBuffer, recv_mono: float):
        # Compute latency using monotonic receive time of first vs. last packet
        transport_ms = (recv_mono - buf.recv_mono) * 1000.0

        # Freshness check: compare age from first-packet arrival
        age_ms = (recv_mono - buf.recv_mono) * 1000.0

        # For inter-camera freshness, use server clock delta since first packet
        server_now_mono = time.monotonic()
        frame_age_ms    = (server_now_mono - buf.recv_mono) * 1000.0

        if frame_age_ms > settings.FRESHNESS_MS:
            cam.frames_dropped += 1
            log.debug(f"[CAM#{cam.camera_id}] Frame {buf.frame_id} stale ({frame_age_ms:.0f} ms) — discarded")
            return

        jpeg = buf.assemble()
        if len(jpeg) < 4 or jpeg[:2] != b"\xff\xd8":
            cam.frames_dropped += 1
            log.debug(f"[CAM#{cam.camera_id}] Frame {buf.frame_id} not valid JPEG — discarded")
            return

        cam.frames_received  += 1
        cam.last_frame_id     = buf.frame_id
        cam.last_latency_ms   = transport_ms
        cam.latest_frame      = jpeg
        cam.tick_fps()

        # Push to WebSocket subscribers (non-blocking — drop if queue full)
        if self._subscribers:
            b64 = base64.b64encode(jpeg).decode("ascii")
            msg = {
                "type":       "frame",
                "camera_id":  cam.camera_id,
                "frame_id":   buf.frame_id,
                "latency_ms": round(transport_ms, 1),
                "fps":        round(cam.current_fps, 1),
                "data":       b64,
            }
            for q in list(self._subscribers):
                try:
                    q.put_nowait(msg)
                except asyncio.QueueFull:
                    pass  # client too slow — drop frame for this subscriber

    def _expire_buffers(self, now_mono: float):
        timeout_s = settings.REASSEMBLY_TIMEOUT_MS / 1000.0
        expired   = [k for k, b in self._buffers.items()
                     if now_mono - b.recv_mono > timeout_s]
        for k in expired:
            buf = self._buffers.pop(k)
            missing = buf.total_pkts - len(buf.chunks)
            if k[0] in self._cameras:
                self._cameras[k[0]].frames_dropped  += 1
                self._cameras[k[0]].packets_lost     += missing
            log.debug(f"Expired incomplete frame ({buf.camera_id},{buf.frame_id}) "
                      f"— {len(buf.chunks)}/{buf.total_pkts} pkts received")

    # ── Public API ───────────────────────────────────────────────────────────

    def get_camera_ids(self) -> List[int]:
        return sorted(self._cameras.keys())

    def get_camera_state(self, camera_id: int) -> Optional[CameraState]:
        return self._cameras.get(camera_id)

    def get_all_camera_stats(self) -> List[dict]:
        return [cam.to_dict() for cam in self._cameras.values()]

    def get_latest_frame(self, camera_id: int) -> Optional[bytes]:
        cam = self._cameras.get(camera_id)
        return cam.latest_frame if cam else None

    def subscribe(self, q: asyncio.Queue):
        self._subscribers.add(q)

    def unsubscribe(self, q: asyncio.Queue):
        self._subscribers.discard(q)
