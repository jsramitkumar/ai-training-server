"""
Central configuration — override via environment variables.
"""

import os
from dataclasses import dataclass, field


@dataclass
class Settings:
    # HTTP / WebSocket
    HTTP_HOST: str = field(default_factory=lambda: os.getenv("HTTP_HOST", "0.0.0.0"))
    HTTP_PORT: int = field(default_factory=lambda: int(os.getenv("HTTP_PORT", "8000")))

    # UDP receiver — ESP32-CAMs fire packets here
    UDP_HOST: str = field(default_factory=lambda: os.getenv("UDP_HOST", "0.0.0.0"))
    UDP_PORT: int = field(default_factory=lambda: int(os.getenv("UDP_PORT", "5000")))

    # Freshness threshold (ms): frames whose assembly took longer than this
    # (time from first packet received to last packet received) are discarded.
    FRESHNESS_MS: int = field(default_factory=lambda: int(os.getenv("FRESHNESS_MS", "300")))

    # Max cameras
    MAX_CAMERAS: int = field(default_factory=lambda: int(os.getenv("MAX_CAMERAS", "20")))

    # Optional API key (empty = disabled)
    API_KEY: str = field(default_factory=lambda: os.getenv("API_KEY", ""))

    # WebSocket frame push interval (ms) — caps dashboard refresh rate
    WS_INTERVAL_MS: int = field(default_factory=lambda: int(os.getenv("WS_INTERVAL_MS", "300")))


settings = Settings()
