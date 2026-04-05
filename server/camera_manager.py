"""
Camera Manager
==============
- Assembles UDP packet fragments into JPEG frames
- Freshness filter: frames whose assembly time exceeds FRESHNESS_MS are discarded
- Shows only the latest frame per camera (older assembled frames are skipped)
- Notifies WebSocket subscribers of new frames (base64 JPEG)
"""

import asyncio
import base64
import logging
import time
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Set

from config import settings

log = logging.getLogger("camera_manager")


# ---------------------------------------------------------------------------
#  FRAME BUFFER  (one per in-flight frame)
# ---------------------------------------------------------------------------

@dataclass
class FrameBuffer:
    camera_id:  int
    frame_id:   int
    total_pkts: int
    first_recv: float           # time.time() when the first packet arrived
    chunks:     Dict[int, bytes] = field(default_factory=dict)

    @property
    def complete(self) -> bool:
        return len(self.chunks) == self.total_pkts

    def assemble(self) -> bytes:
        # Build the JPEG by concatenating chunks in strict index order.
        # Using sorted() is a cheap safety net against any non-contiguous
        # edge cases; under normal operation order is always 0..N-1.
        return b"".join(self.chunks[i] for i in sorted(self.chunks))

    def is_contiguous(self) -> bool:
        """True when received packet indices form a gap-free 0..N-1 sequence."""
        return set(self.chunks.keys()) == set(range(self.total_pkts))


# ---------------------------------------------------------------------------
#  CAMERA STATE
# ---------------------------------------------------------------------------

@dataclass
class CameraState:
    camera_id:        int
    source_ip:        str
    connected_at:     float = field(default_factory=time.monotonic)
    last_seen:        float = field(default_factory=time.monotonic)

    # Counters
    frames_received:  int   = 0
    frames_dropped:   int   = 0
    packets_received: int   = 0

    # Latest frame info
    last_frame_id:    int   = 0
    last_latency_ms:  float = 0.0
    current_fps:      float = 0.0
    resolution:       str   = "unknown"
    jpeg_quality:     int   = 0
    wifi_rssi:        Optional[int] = None
    vflip:            bool  = False
    hmirror:          bool  = False
    special_effect:   int   = 0
    wb_mode:          int   = 0

    # FPS tracking (private)
    _fps_counter: int   = field(default=0, repr=False)
    _fps_window:  float = field(default_factory=time.monotonic, repr=False)

    # Latest JPEG bytes
    latest_frame: Optional[bytes] = field(default=None, repr=False)

    def tick_fps(self) -> None:
        self._fps_counter += 1
        now     = time.monotonic()
        elapsed = now - self._fps_window
        if elapsed >= 1.0:
            self.current_fps  = self._fps_counter / elapsed
            self._fps_counter = 0
            self._fps_window  = now

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
            "resolution":       self.resolution,
            "jpeg_quality":     self.jpeg_quality,
            "wifi_rssi":        self.wifi_rssi,
            "vflip":            self.vflip,
            "hmirror":          self.hmirror,
            "special_effect":   self.special_effect,
            "wb_mode":          self.wb_mode,
            "online":           (time.monotonic() - self.last_seen) < 5.0,
        }


# ---------------------------------------------------------------------------
#  CAMERA MANAGER
# ---------------------------------------------------------------------------

class CameraManager:
    def __init__(self) -> None:
        self._cameras:          Dict[int, CameraState]   = {}
        self._buffers:          Dict[tuple, FrameBuffer] = {}   # (camera_id, frame_id)
        self._latest_frame_id:  Dict[int, int]           = {}   # highest delivered frame_id
        self._subscribers:      Set[asyncio.Queue]        = set()

        # Diagnostics — global counters
        self._diag_pkts_in:         int = 0   # total on_packet calls
        self._diag_frames_complete: int = 0   # frames fully assembled
        self._diag_frames_stale:    int = 0   # dropped: too slow to assemble
        self._diag_frames_ooo:      int = 0   # dropped: older than latest delivered
        self._diag_frames_bad_jpeg: int = 0   # dropped: bad JPEG header
        self._diag_frames_pushed:   int = 0   # actually sent to WS subscribers
        self._diag_ws_subs:         int = 0   # current subscriber count

    # -----------------------------------------------------------------------
    #  Packet ingestion  (called from UDP receiver thread — NOT async)
    # -----------------------------------------------------------------------

    def on_packet(
        self,
        camera_id:  int,
        frame_id:   int,
        pkt_idx:    int,
        total_pkts: int,
        payload:    bytes,
        source_ip:  str,
    ) -> None:
        now = time.time()
        self._diag_pkts_in += 1

        # Auto-register new camera
        if camera_id not in self._cameras:
            self._cameras[camera_id]         = CameraState(camera_id=camera_id, source_ip=source_ip)
            self._latest_frame_id[camera_id] = -1
            log.info(f"[CAMERA] New camera #{camera_id} from {source_ip}")

        cam = self._cameras[camera_id]
        cam.last_seen         = time.monotonic()
        cam.packets_received += 1

        # Log every 1000 packets per camera
        if cam.packets_received % 1000 == 0:
            log.info(
                f"[CAM#{camera_id}] {cam.packets_received} pkts  "
                f"delivered={cam.frames_received}  dropped={cam.frames_dropped}  "
                f"fps={cam.current_fps:.1f}  in_flight_bufs={sum(1 for k in self._buffers if k[0]==camera_id)}"
            )

        # Sanity checks
        if total_pkts == 0 or pkt_idx >= total_pkts:
            log.warning(f"[CAM#{camera_id}] Malformed packet: pkt_idx={pkt_idx} total_pkts={total_pkts}")
            return

        # Reject empty payload — firmware should never send header-only packets
        # but guard here too to avoid storing empty chunks that corrupt frames.
        if not payload:
            log.warning(f"[CAM#{camera_id}] Empty payload in frame={frame_id} pkt={pkt_idx}/{total_pkts}")
            return

        # Discard packets belonging to frames we already delivered
        latest = self._latest_frame_id[camera_id]
        if frame_id <= latest:
            return

        key = (camera_id, frame_id)

        if key not in self._buffers:
            # Periodic eviction: clear all incomplete buffers older than FRESHNESS_MS
            if len(self._buffers) > settings.MAX_CAMERAS * 8:
                self._evict_stale(now)
            self._buffers[key] = FrameBuffer(
                camera_id=camera_id,
                frame_id=frame_id,
                total_pkts=total_pkts,
                first_recv=now,
            )

        buf = self._buffers[key]
        if pkt_idx not in buf.chunks:
            buf.chunks[pkt_idx] = payload

        if buf.complete:
            self._diag_frames_complete += 1
            assembly_ms = (now - buf.first_recv) * 1000.0
            del self._buffers[key]
            if assembly_ms <= settings.FRESHNESS_MS:
                self._deliver(cam, buf, assembly_ms)
            else:
                self._diag_frames_stale += 1
                cam.frames_dropped += 1
                log.warning(
                    f"[CAM#{camera_id}] Frame {frame_id} STALE: "
                    f"assembly took {assembly_ms:.0f} ms (limit {settings.FRESHNESS_MS} ms)  "
                    f"{buf.total_pkts} pkts  {len(buf.chunks)} received"
                )

        # Evict stale incomplete buffers every 200 packets per camera
        if cam.packets_received % 200 == 0:
            self._evict_stale(now)

    # -----------------------------------------------------------------------
    #  Internal helpers
    # -----------------------------------------------------------------------

    def _evict_stale(self, now: float) -> None:
        """Remove incomplete buffers whose first packet arrived too long ago."""
        threshold = settings.FRESHNESS_MS / 1000.0
        stale = [k for k, b in self._buffers.items() if now - b.first_recv > threshold]
        for k in stale:
            cam_id = k[0]
            buf    = self._buffers.pop(k)
            if cam_id in self._cameras:
                self._cameras[cam_id].frames_dropped += 1
            log.debug(
                f"[CAM#{cam_id}] Evicted incomplete buffer frame={k[1]}  "
                f"{len(buf.chunks)}/{buf.total_pkts} pkts received"
            )

    def _deliver(self, cam: CameraState, buf: FrameBuffer, latency_ms: float) -> None:
        """Validate, update state, and push frame to WebSocket subscribers."""
        # Ensure every expected packet index is present before assembling.
        # If any chunk is missing the JPEG will have a data hole and be corrupt.
        if not buf.is_contiguous():
            missing = sorted(set(range(buf.total_pkts)) - set(buf.chunks.keys()))
            self._diag_frames_bad_jpeg += 1
            cam.frames_dropped += 1
            log.warning(
                f"[CAM#{cam.camera_id}] Frame {buf.frame_id} has gaps — "
                f"missing pkt indices {missing[:10]}{'...' if len(missing)>10 else ''}  "
                f"({len(buf.chunks)}/{buf.total_pkts} received)"
            )
            return

        jpeg = buf.assemble()

        # Validate JPEG SOI (FF D8) and EOI (FF D9) markers.
        # A missing EOI means the last packet(s) were lost; the image would
        # render as truncated or garbled at the bottom.
        if len(jpeg) < 4 or jpeg[:2] != b"\xff\xd8":
            self._diag_frames_bad_jpeg += 1
            cam.frames_dropped += 1
            log.warning(
                f"[CAM#{cam.camera_id}] Frame {buf.frame_id} missing JPEG SOI: "
                f"{jpeg[:4].hex() if jpeg else 'empty'}  size={len(jpeg)}B"
            )
            return

        # EOI (FF D9) is intentionally NOT required.  The ESP32-IDF camera
        # driver does not always append the EOI marker, but browsers and the
        # Canvas API decode the JPEG correctly without it.

        # Accept only frames strictly newer than the last delivered
        if buf.frame_id <= self._latest_frame_id.get(cam.camera_id, -1):
            self._diag_frames_ooo += 1
            cam.frames_dropped += 1
            log.debug(f"[CAM#{cam.camera_id}] Frame {buf.frame_id} out-of-order (latest={self._latest_frame_id.get(cam.camera_id)}), skipping")
            return

        self._latest_frame_id[cam.camera_id] = buf.frame_id
        cam.frames_received  += 1
        cam.last_frame_id     = buf.frame_id
        cam.last_latency_ms   = latency_ms
        cam.latest_frame      = jpeg
        cam.tick_fps()

        # Log every 30 delivered frames (~1s at 30fps)
        if cam.frames_received % 30 == 1:
            log.info(
                f"[CAM#{cam.camera_id}] Delivering frame {buf.frame_id}  "
                f"size={len(jpeg)}B  latency={latency_ms:.0f}ms  fps={cam.current_fps:.1f}  "
                f"subscribers={len(self._subscribers)}"
            )

        self._diag_frames_pushed += 1

        if self._subscribers:
            b64 = base64.b64encode(jpeg).decode("ascii")
            msg = {
                "type":       "frame",
                "camera_id":  cam.camera_id,
                "frame_id":   buf.frame_id,
                "latency_ms": round(latency_ms, 1),
                "fps":        round(cam.current_fps, 1),
                "data":       b64,
            }
            for q in list(self._subscribers):
                try:
                    q.put_nowait(msg)
                except asyncio.QueueFull:
                    pass    # slow client — skip frame rather than block
        else:
            log.debug(f"[CAM#{cam.camera_id}] Frame {buf.frame_id} assembled but no WS subscribers")

    # -----------------------------------------------------------------------
    #  Public API
    # -----------------------------------------------------------------------

    def get_camera_ids(self) -> List[int]:
        return sorted(self._cameras.keys())

    def get_camera_state(self, camera_id: int) -> Optional[CameraState]:
        return self._cameras.get(camera_id)

    def get_all_camera_stats(self) -> List[dict]:
        return [cam.to_dict() for cam in self._cameras.values()]

    def get_latest_frame(self, camera_id: int) -> Optional[bytes]:
        cam = self._cameras.get(camera_id)
        return cam.latest_frame if cam else None

    def get_diag(self) -> dict:
        """Return detailed diagnostic snapshot for /api/diag."""
        cameras_diag = []
        for cam in self._cameras.values():
            in_flight = sum(1 for k in self._buffers if k[0] == cam.camera_id)
            cameras_diag.append({
                "camera_id":        cam.camera_id,
                "source_ip":        cam.source_ip,
                "packets_received": cam.packets_received,
                "frames_delivered": cam.frames_received,
                "frames_dropped":   cam.frames_dropped,
                "fps":              round(cam.current_fps, 1),
                "latency_ms":       round(cam.last_latency_ms, 1),
                "last_frame_id":    cam.last_frame_id,
                "last_frame_bytes": len(cam.latest_frame) if cam.latest_frame else 0,
                "in_flight_bufs":   in_flight,
                "online":           (time.monotonic() - cam.last_seen) < 5.0,
            })
        return {
            "cameras":              cameras_diag,
            "global_pkts_in":       self._diag_pkts_in,
            "global_frames_assembled": self._diag_frames_complete,
            "global_frames_stale":  self._diag_frames_stale,
            "global_frames_ooo":    self._diag_frames_ooo,
            "global_frames_bad_jpeg": self._diag_frames_bad_jpeg,
            "global_frames_pushed": self._diag_frames_pushed,
            "ws_subscribers":       len(self._subscribers),
            "open_buffers":         len(self._buffers),
        }

    def subscribe(self, q: asyncio.Queue) -> None:
        self._subscribers.add(q)
        self._diag_ws_subs = len(self._subscribers)
        log.info(f"[WS] Client subscribed  total_subscribers={len(self._subscribers)}")

    def unsubscribe(self, q: asyncio.Queue) -> None:
        self._subscribers.discard(q)
        self._diag_ws_subs = len(self._subscribers)
        log.info(f"[WS] Client unsubscribed  total_subscribers={len(self._subscribers)}")
