"""
Ultra-Low-Latency Multi-Camera UDP Streaming Server
====================================================
Entrypoint: python main.py
"""

import asyncio
import logging
import signal
import sys
from app import create_app
from udp_receiver import UDPReceiver
from camera_manager import CameraManager
from config import settings

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
    datefmt="%H:%M:%S",
    handlers=[
        logging.StreamHandler(sys.stdout),
    ],
)
log = logging.getLogger("main")


async def run():
    manager = CameraManager()
    receiver = UDPReceiver(manager, settings.UDP_HOST, settings.UDP_PORT)
    app = create_app(manager)

    # Start UDP receiver
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

    log.info(f"UDP  listener : {settings.UDP_HOST}:{settings.UDP_PORT}")
    log.info(f"HTTP dashboard: http://{settings.HTTP_HOST}:{settings.HTTP_PORT}")

    loop = asyncio.get_running_loop()

    def _stop():
        log.info("Shutdown signal received")
        server.should_exit = True

    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, _stop)
        except NotImplementedError:
            pass  # Windows

    await server.serve()
    await receiver.stop()
    log.info("Server stopped cleanly")


if __name__ == "__main__":
    asyncio.run(run())
