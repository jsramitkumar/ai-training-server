"""
Central configuration — override via environment variables or .env file.
"""

import os
from dataclasses import dataclass, field


@dataclass
class Settings:
    # UDP
    UDP_HOST: str = field(default_factory=lambda: os.getenv("UDP_HOST", "0.0.0.0"))
    UDP_PORT: int = field(default_factory=lambda: int(os.getenv("UDP_PORT", "5000")))

    # HTTP / WebSocket
    HTTP_HOST: str = field(default_factory=lambda: os.getenv("HTTP_HOST", "0.0.0.0"))
    HTTP_PORT: int = field(default_factory=lambda: int(os.getenv("HTTP_PORT", "8000")))

    # Freshness threshold (ms)
    FRESHNESS_MS: int = field(default_factory=lambda: int(os.getenv("FRESHNESS_MS", "50")))

    # Frame-reassembly timeout (ms) — incomplete frames are dropped after this
    REASSEMBLY_TIMEOUT_MS: int = field(default_factory=lambda: int(os.getenv("REASSEMBLY_TIMEOUT_MS", "200")))

    # Max cameras
    MAX_CAMERAS: int = field(default_factory=lambda: int(os.getenv("MAX_CAMERAS", "20")))

    # Optional API key (empty = disabled)
    API_KEY: str = field(default_factory=lambda: os.getenv("API_KEY", ""))

    # JPEG decode quality hint for OpenCV
    JPEG_DECODE: bool = field(default_factory=lambda: os.getenv("JPEG_DECODE", "1") == "1")

    # WebSocket frame push interval (ms) — caps dashboard refresh rate
    WS_INTERVAL_MS: int = field(default_factory=lambda: int(os.getenv("WS_INTERVAL_MS", "30")))


settings = Settings()
