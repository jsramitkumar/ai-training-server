"""
Multi-Camera UDP Streaming Server
==================================
Entrypoint: python main.py
"""

import asyncio
import logging
import signal
import sys

from app import create_app
from camera_manager import CameraManager
from udp_receiver import UDPReceiver
from config import settings

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
    datefmt="%H:%M:%S",
    handlers=[logging.StreamHandler(sys.stdout)],
)
log = logging.getLogger("main")


async def run():
    manager  = CameraManager()
    receiver = UDPReceiver(manager, settings.UDP_HOST, settings.UDP_PORT)
    app      = create_app(manager, receiver)

    await receiver.start()

    import uvicorn
    config = uvicorn.Config(
        app,
        host=settings.HTTP_HOST,
        port=settings.HTTP_PORT,
        log_level="warning",
        ws_ping_interval=None,
        ws_ping_timeout=None,
    )
    server = uvicorn.Server(config)

    log.info(f"HTTP dashboard : http://{settings.HTTP_HOST}:{settings.HTTP_PORT}")
    log.info(f"UDP receiver   : udp://{settings.UDP_HOST}:{settings.UDP_PORT}")
    log.info(f"Freshness limit: {settings.FRESHNESS_MS} ms")
    log.info(f"Diagnostics    : http://{settings.HTTP_HOST}:{settings.HTTP_PORT}/api/diag")

    loop = asyncio.get_running_loop()

    async def _stats_logger():
        """Log pipeline summary every 5 seconds."""
        while True:
            await asyncio.sleep(5)
            d    = manager.get_diag()
            udpd = receiver.get_diag()
            cams = ", ".join(
                f"cam{c['camera_id']}({c['fps']}fps drop={c['frames_dropped']})"
                for c in d["cameras"]
            ) or "none"
            log.info(
                f"[STATS] udp_pkts={udpd['total_packets']}  "
                f"assembled={d['global_frames_assembled']}  "
                f"pushed={d['global_frames_pushed']}  "
                f"stale={d['global_frames_stale']}  "
                f"ws_subs={d['ws_subscribers']}  "
                f"cameras=[{cams}]"
            )

    asyncio.create_task(_stats_logger())

    def _stop():
        log.info("Shutdown signal received")
        server.should_exit = True

    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, _stop)
        except NotImplementedError:
            pass  # Windows does not support add_signal_handler

    await server.serve()
    receiver.stop()
    log.info("Server stopped cleanly")


if __name__ == "__main__":
    asyncio.run(run())
