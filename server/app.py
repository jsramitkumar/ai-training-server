"""
FastAPI application — HTTP REST API, WebSocket stream, static dashboard.
"""

import asyncio
import base64
import json
import logging
from pathlib import Path
from typing import Optional

import httpx
from fastapi import Depends, FastAPI, HTTPException, Request, WebSocket, WebSocketDisconnect, status
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import HTMLResponse, JSONResponse, Response
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

from camera_manager import CameraManager
from config import settings

log = logging.getLogger("app")

STATIC_DIR = Path(__file__).parent.parent / "dashboard"

# ─────────────────────────────────────────────
#  REQUEST / RESPONSE MODELS
# ─────────────────────────────────────────────

class CameraConfigRequest(BaseModel):
    resolution:   Optional[str]  = None
    jpeg_quality: Optional[int]  = None
    fps_limit:    Optional[int]  = None
    brightness:   Optional[int]  = None
    contrast:     Optional[int]  = None
    saturation:   Optional[int]  = None
    awb:          Optional[bool] = None
    agc:          Optional[bool] = None
    aec:          Optional[bool] = None


class FlashRequest(BaseModel):
    state:      str = "off"      # on | off | toggle
    brightness: Optional[int]   = None


class StreamControlRequest(BaseModel):
    streaming: bool


# ─────────────────────────────────────────────
#  AUTH DEPENDENCY
# ─────────────────────────────────────────────

async def verify_api_key(request: Request):
    if not settings.API_KEY:
        return
    key = request.headers.get("X-API-Key", "")
    if key != settings.API_KEY:
        raise HTTPException(status_code=401, detail="Invalid or missing API key")


# ─────────────────────────────────────────────
#  APP FACTORY
# ─────────────────────────────────────────────

def create_app(manager: CameraManager) -> FastAPI:
    app = FastAPI(title="ESP32-CAM Streaming Server", version="1.0.0")

    app.add_middleware(
        CORSMiddleware,
        allow_origins=["*"],
        allow_methods=["*"],
        allow_headers=["*"],
    )

    # ── REST: system ────────────────────────────────────────────────────────

    @app.get("/api/cameras", dependencies=[Depends(verify_api_key)])
    async def list_cameras():
        return manager.get_all_camera_stats()

    @app.get("/api/cameras/{camera_id}", dependencies=[Depends(verify_api_key)])
    async def get_camera(camera_id: int):
        cam = manager.get_camera_state(camera_id)
        if cam is None:
            raise HTTPException(status_code=404, detail="Camera not found")
        return cam.to_dict()

    @app.get("/api/cameras/{camera_id}/snapshot", dependencies=[Depends(verify_api_key)])
    async def snapshot(camera_id: int):
        jpeg = manager.get_latest_frame(camera_id)
        if jpeg is None:
            raise HTTPException(status_code=404, detail="No frame available")
        return Response(content=jpeg, media_type="image/jpeg")

    # ── REST: camera config proxy → ESP32 HTTP ──────────────────────────────

    @app.post("/api/cameras/{camera_id}/config", dependencies=[Depends(verify_api_key)])
    async def camera_config(camera_id: int, body: CameraConfigRequest):
        cam = manager.get_camera_state(camera_id)
        if cam is None:
            raise HTTPException(status_code=404, detail="Camera not found")
        return await _proxy_post(cam.source_ip, "/camera/config", body.dict(exclude_none=True))

    @app.post("/api/cameras/{camera_id}/flash", dependencies=[Depends(verify_api_key)])
    async def flash_control(camera_id: int, body: FlashRequest):
        cam = manager.get_camera_state(camera_id)
        if cam is None:
            raise HTTPException(status_code=404, detail="Camera not found")
        payload = {"state": body.state}
        if body.brightness is not None:
            payload["brightness"] = body.brightness
        return await _proxy_post(cam.source_ip, "/flash", payload)

    @app.post("/api/cameras/{camera_id}/stream", dependencies=[Depends(verify_api_key)])
    async def stream_control(camera_id: int, body: StreamControlRequest):
        cam = manager.get_camera_state(camera_id)
        if cam is None:
            raise HTTPException(status_code=404, detail="Camera not found")
        return await _proxy_post(cam.source_ip, "/stream/control", {"streaming": body.streaming})

    @app.post("/api/cameras/{camera_id}/restart", dependencies=[Depends(verify_api_key)])
    async def restart_camera(camera_id: int):
        cam = manager.get_camera_state(camera_id)
        if cam is None:
            raise HTTPException(status_code=404, detail="Camera not found")
        return await _proxy_post(cam.source_ip, "/restart", {})

    @app.get("/api/cameras/{camera_id}/status", dependencies=[Depends(verify_api_key)])
    async def camera_device_status(camera_id: int):
        cam = manager.get_camera_state(camera_id)
        if cam is None:
            raise HTTPException(status_code=404, detail="Camera not found")
        return await _proxy_get(cam.source_ip, "/status")

    # ── WebSocket: unified frame stream ─────────────────────────────────────

    @app.websocket("/ws/stream")
    async def ws_stream(websocket: WebSocket):
        """
        Subscribers receive JSON messages:
          { "type": "frame", "camera_id": N, "frame_id": N,
            "latency_ms": F, "fps": F, "data": "<base64 JPEG>" }
          { "type": "stats", "cameras": [...] }
        """
        await websocket.accept()
        q: asyncio.Queue = asyncio.Queue(maxsize=60)
        manager.subscribe(q)
        log.info(f"WS client connected: {websocket.client}")

        async def _stats_pusher():
            while True:
                await asyncio.sleep(1.0)
                try:
                    await websocket.send_json({
                        "type": "stats",
                        "cameras": manager.get_all_camera_stats(),
                    })
                except Exception:
                    break

        stats_task = asyncio.create_task(_stats_pusher())
        try:
            while True:
                try:
                    msg = await asyncio.wait_for(q.get(), timeout=5.0)
                    await websocket.send_json(msg)
                except asyncio.TimeoutError:
                    # Send keepalive ping
                    try:
                        await websocket.send_json({"type": "ping"})
                    except Exception:
                        break
                except asyncio.CancelledError:
                    break
        except WebSocketDisconnect:
            pass
        except Exception as exc:
            log.debug(f"WS stream error: {exc}")
        finally:
            stats_task.cancel()
            manager.unsubscribe(q)
            log.info(f"WS client disconnected: {websocket.client}")

    # ── Background: poll camera /status every 30 s ─────────────────────────

    async def _poll_status_loop():
        """Refresh resolution + wifi_rssi from each ESP32 every 30 s."""
        while True:
            await asyncio.sleep(30)
            for camera_id in manager.get_camera_ids():
                cam = manager.get_camera_state(camera_id)
                if cam is None:
                    continue
                try:
                    url = f"http://{cam.source_ip}:8080/status"
                    async with httpx.AsyncClient(timeout=3.0) as client:
                        resp = await client.get(url)
                        resp.raise_for_status()
                        data = resp.json()
                    if "resolution" in data:
                        cam.resolution = data["resolution"]
                    if "wifi_rssi" in data:
                        cam.wifi_rssi = data["wifi_rssi"]
                    if "jpeg_quality" in data:
                        cam.jpeg_quality = data["jpeg_quality"]
                except Exception as exc:
                    log.debug(f"[CAM#{camera_id}] status poll failed: {exc}")

    @app.on_event("startup")
    async def _start_status_poller():
        asyncio.create_task(_poll_status_loop())

    # ── Static dashboard ────────────────────────────────────────────────────

    if STATIC_DIR.exists():
        app.mount("/static", StaticFiles(directory=str(STATIC_DIR / "static")), name="static")

        @app.get("/", response_class=HTMLResponse)
        async def dashboard():
            index = STATIC_DIR / "index.html"
            return HTMLResponse(content=index.read_text(encoding="utf-8"))
    else:
        @app.get("/", response_class=HTMLResponse)
        async def dashboard_missing():
            return HTMLResponse("<h1>Dashboard not found</h1><p>Place dashboard/ next to server/</p>")

    return app


# ─────────────────────────────────────────────
#  PROXY HELPERS
# ─────────────────────────────────────────────

async def _proxy_post(ip: str, path: str, payload: dict) -> dict:
    url = f"http://{ip}:8080{path}"
    try:
        async with httpx.AsyncClient(timeout=3.0) as client:
            resp = await client.post(url, json=payload)
            resp.raise_for_status()
            return resp.json()
    except httpx.HTTPStatusError as exc:
        raise HTTPException(status_code=exc.response.status_code, detail=str(exc))
    except Exception as exc:
        raise HTTPException(status_code=502, detail=f"Camera unreachable: {exc}")


async def _proxy_get(ip: str, path: str) -> dict:
    url = f"http://{ip}:8080{path}"
    try:
        async with httpx.AsyncClient(timeout=3.0) as client:
            resp = await client.get(url)
            resp.raise_for_status()
            return resp.json()
    except httpx.HTTPStatusError as exc:
        raise HTTPException(status_code=exc.response.status_code, detail=str(exc))
    except Exception as exc:
        raise HTTPException(status_code=502, detail=f"Camera unreachable: {exc}")
