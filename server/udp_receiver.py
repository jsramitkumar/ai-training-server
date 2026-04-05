"""
UDP Receiver
============
Listens for ESP32-CAM UDP packets and passes fragments to CameraManager.

Packet format (little-endian, 9-byte header):
  [camera_id: 1B][frame_id: 4B][pkt_idx: 2B][total_pkts: 2B][JPEG payload...]
"""

import asyncio
import logging
import struct
import time
from typing import TYPE_CHECKING, Dict, Set

if TYPE_CHECKING:
    from camera_manager import CameraManager

log = logging.getLogger("udp_receiver")

HEADER_FMT  = "<BIHH"                      # camera_id(1) frame_id(4) pkt_idx(2) total_pkts(2)
HEADER_SIZE = struct.calcsize(HEADER_FMT)   # 9 bytes


class _Protocol(asyncio.DatagramProtocol):
    def __init__(self, manager: "CameraManager") -> None:
        self._manager      = manager
        self._total_pkts   = 0
        self._bad_pkts     = 0
        self._sources: Set[str] = set()
        self._window_pkts  = 0
        self._window_start = time.monotonic()
        # Rate-limit header-only / undersized warnings: log at most once per
        # 10 seconds per source IP, with a suppressed-count summary.
        self._noise_last_log: Dict[str, float] = {}   # ip -> last log time
        self._noise_count:    Dict[str, int]   = {}   # ip -> suppressed count
        _NOISE_RATE_LIMIT_S = 10.0
        self._noise_limit = _NOISE_RATE_LIMIT_S

    # -- public for diagnostics --
    @property
    def total_packets(self) -> int:
        return self._total_pkts

    @property
    def bad_packets(self) -> int:
        return self._bad_pkts

    @property
    def known_sources(self) -> Set[str]:
        return self._sources

    def _warn_noise(self, src_ip: str, msg: str) -> None:
        """Emit a warning for noisy/bad packets at most once per rate-limit window."""
        now = time.monotonic()
        last = self._noise_last_log.get(src_ip, 0.0)
        if now - last >= self._noise_limit:
            suppressed = self._noise_count.pop(src_ip, 0)
            suffix = f"  (+ {suppressed} suppressed)" if suppressed else ""
            log.warning(f"[UDP] {msg}{suffix}")
            self._noise_last_log[src_ip] = now
        else:
            self._noise_count[src_ip] = self._noise_count.get(src_ip, 0) + 1

    def datagram_received(self, data: bytes, addr: tuple) -> None:
        src_ip = addr[0]

        if src_ip not in self._sources:
            self._sources.add(src_ip)
            log.info(f"[UDP] First packet from {src_ip}  len={len(data)}B")

        if len(data) <= HEADER_SIZE:
            self._bad_pkts += 1
            if len(data) == HEADER_SIZE:
                # Header-only packet — firmware bug or wrong firmware flashed.
                try:
                    camera_id, frame_id, pkt_idx, total_pkts = struct.unpack_from(HEADER_FMT, data)
                    self._warn_noise(
                        src_ip,
                        f"Header-only packet (no JPEG payload) from {src_ip} "
                        f"cam={camera_id} frame={frame_id} — "
                        "ESP32 not reflashed with latest firmware?"
                    )
                except struct.error:
                    self._warn_noise(src_ip, f"Undersized packet from {src_ip}: {len(data)}B (need >{HEADER_SIZE}B)")
            else:
                self._warn_noise(src_ip, f"Undersized packet from {src_ip}: {len(data)}B (need >{HEADER_SIZE}B)")
            return

        try:
            camera_id, frame_id, pkt_idx, total_pkts = struct.unpack_from(HEADER_FMT, data)
        except struct.error as exc:
            self._bad_pkts += 1
            log.warning(f"[UDP] Header parse error from {src_ip}: {exc}")
            return

        self._total_pkts   += 1
        self._window_pkts  += 1

        # Log packet rate every 500 packets
        if self._total_pkts % 500 == 0:
            now     = time.monotonic()
            elapsed = now - self._window_start
            rate    = self._window_pkts / elapsed if elapsed > 0 else 0
            log.info(
                f"[UDP] Received {self._total_pkts} total pkts  "
                f"rate={rate:.0f} pkt/s  sources={self._sources}"
            )
            self._window_pkts  = 0
            self._window_start = now

        self._manager.on_packet(
            camera_id=camera_id,
            frame_id=frame_id,
            pkt_idx=pkt_idx,
            total_pkts=total_pkts,
            payload=data[HEADER_SIZE:],
            source_ip=src_ip,
        )

    def error_received(self, exc: Exception) -> None:
        log.warning(f"[UDP] Socket error: {exc}")

    def connection_lost(self, exc: Exception) -> None:
        pass


class UDPReceiver:
    def __init__(self, manager: "CameraManager", host: str, port: int) -> None:
        self._manager    = manager
        self._host       = host
        self._port       = port
        self._transport  = None
        self._protocol: _Protocol | None = None

    async def start(self) -> None:
        loop = asyncio.get_running_loop()
        proto = _Protocol(self._manager)
        self._transport, self._protocol = await loop.create_datagram_endpoint(
            lambda: proto,
            local_addr=(self._host, self._port),
        )
        log.info(f"[UDP] Listening on {self._host}:{self._port}  header={HEADER_SIZE}B")

    def get_diag(self) -> dict:
        if self._protocol is None:
            return {"listening": False}
        return {
            "listening":     True,
            "host":          self._host,
            "port":          self._port,
            "total_packets": self._protocol.total_packets,
            "bad_packets":   self._protocol.bad_packets,
            "sources":       list(self._protocol.known_sources),
        }

    def stop(self) -> None:
        if self._transport:
            self._transport.close()
            self._transport = None
