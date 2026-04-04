"""
UDP Receiver — non-blocking asyncio UDP transport.

Packet binary layout (little-endian, packed):
  Offset  Size  Field
  0       4     camera_id     uint32
  4       4     frame_id      uint32
  8       2     packet_index  uint16
  10      2     total_packets uint16
  12      8     timestamp_ms  uint64   (ESP32 millis epoch NOT wall clock)
  20      N     jpeg payload
"""

import asyncio
import logging
import struct
import time
from camera_manager import CameraManager

log = logging.getLogger("udp_receiver")

HEADER_FMT    = "<IIHHQ"          # little-endian
HEADER_SIZE   = struct.calcsize(HEADER_FMT)   # 20 bytes
MIN_PKT_SIZE  = HEADER_SIZE + 1


class UDPProtocol(asyncio.DatagramProtocol):
    """asyncio UDP datagram protocol — zero-copy path to CameraManager."""

    def __init__(self, manager: CameraManager):
        self._manager = manager
        self._rx_count = 0
        self._err_count = 0

    def datagram_received(self, data: bytes, addr):
        if len(data) < MIN_PKT_SIZE:
            self._err_count += 1
            return
        try:
            camera_id, frame_id, pkt_idx, total_pkts, ts_ms = struct.unpack_from(
                HEADER_FMT, data, 0
            )
        except struct.error:
            self._err_count += 1
            return

        payload = data[HEADER_SIZE:]
        recv_ts = time.monotonic()

        self._manager.on_packet(
            camera_id=camera_id,
            frame_id=frame_id,
            pkt_idx=pkt_idx,
            total_pkts=total_pkts,
            ts_ms=ts_ms,
            payload=payload,
            source_ip=addr[0],
            recv_monotonic=recv_ts,
        )
        self._rx_count += 1

    def error_received(self, exc):
        log.warning(f"UDP error: {exc}")

    def connection_lost(self, exc):
        log.warning(f"UDP connection lost: {exc}")


class UDPReceiver:
    def __init__(self, manager: CameraManager, host: str, port: int):
        self._manager  = manager
        self._host     = host
        self._port     = port
        self._transport = None

    async def start(self):
        loop = asyncio.get_running_loop()
        self._transport, _ = await loop.create_datagram_endpoint(
            lambda: UDPProtocol(self._manager),
            local_addr=(self._host, self._port),
        )
        log.info(f"UDP receiver bound to {self._host}:{self._port}")

    async def stop(self):
        if self._transport:
            self._transport.close()
            log.info("UDP receiver closed")
