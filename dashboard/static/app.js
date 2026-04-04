/**
 * ESP32-CAM Dashboard — app.js
 * ================================================
 * WebSocket frame/stats receiver · Canvas video + OSD · REST proxy
 */

"use strict";

// ─────────────────────────────────────────────────────────────────
//  CONFIG (persisted in localStorage)
// ─────────────────────────────────────────────────────────────────
const DEFAULTS = {
  serverUrl:   `ws://${location.hostname}:${location.port || 8000}/ws/stream`,
  apiKey:      "",
  gridCols:    2,
  osdVisible:  true,
  osdFontSize: 12,
};

function loadCfg() {
  try { return { ...DEFAULTS, ...JSON.parse(localStorage.getItem("esp32cam_cfg2") || "{}") }; }
  catch { return { ...DEFAULTS }; }
}
function saveCfg() { localStorage.setItem("esp32cam_cfg2", JSON.stringify(CFG)); }

let CFG = loadCfg();

// ─────────────────────────────────────────────────────────────────
//  STATE
// ─────────────────────────────────────────────────────────────────
const cameras   = new Map();
let ws          = null;
let reconnTimer = null;
let reconnDelay = 1000;
let osdVisible  = CFG.osdVisible;

// ─────────────────────────────────────────────────────────────────
//  WEBSOCKET
// ─────────────────────────────────────────────────────────────────
function connect() {
  if (ws) { try { ws.close(); } catch (_) {} }
  setWsStatus("connecting", "Connecting…");

  ws = new WebSocket(CFG.serverUrl);

  ws.addEventListener("open", () => {
    setWsStatus("online", "Connected");
    reconnDelay = 1000;
  });
  ws.addEventListener("message", (ev) => {
    try { dispatch(JSON.parse(ev.data)); } catch (_) {}
  });
  ws.addEventListener("close", () => {
    setWsStatus("offline", "Disconnected");
    scheduleReconnect();
  });
  ws.addEventListener("error", () => setWsStatus("offline", "Error"));
}

function scheduleReconnect() {
  clearTimeout(reconnTimer);
  reconnTimer = setTimeout(() => {
    reconnDelay = Math.min(reconnDelay * 1.5, 12000);
    connect();
  }, reconnDelay);
}

// ─────────────────────────────────────────────────────────────────
//  MESSAGE DISPATCH
// ─────────────────────────────────────────────────────────────────
function dispatch(msg) {
  if      (msg.type === "frame") onFrame(msg);
  else if (msg.type === "stats") onStats(msg.cameras);
}

// ─────────────────────────────────────────────────────────────────
//  FRAME HANDLER — decode JPEG → draw video + OSD on canvas
// ─────────────────────────────────────────────────────────────────
function onFrame(msg) {
  const { camera_id, frame_id, latency_ms, fps, data } = msg;
  if (!cameras.has(camera_id)) createCard(camera_id);
  const cam = cameras.get(camera_id);

  cam.fps        = fps;
  cam.latency_ms = latency_ms;
  cam.frame_id   = frame_id;
  cam.online     = true;
  cam.lastFrame  = performance.now();

  if (cam.paused) return;

  // base64 JPEG → Blob → ImageBitmap
  const bin  = atob(data);
  const buf  = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) buf[i] = bin.charCodeAt(i);

  createImageBitmap(new Blob([buf], { type: "image/jpeg" })).then((bmp) => {
    const { canvas, ctx } = cam;
    if (canvas.width !== bmp.width || canvas.height !== bmp.height) {
      canvas.width  = bmp.width;
      canvas.height = bmp.height;
    }
    ctx.drawImage(bmp, 0, 0);
    bmp.close();
    if (osdVisible) drawOSD(cam);
    setCardOnline(cam, true);
  }).catch(() => {});
}

// ─────────────────────────────────────────────────────────────────
//  OSD RENDERER — drawn directly on the video canvas
// ─────────────────────────────────────────────────────────────────
function drawOSD(cam) {
  const { ctx, canvas } = cam;
  const fs  = CFG.osdFontSize;
  const lh  = Math.round(fs * 1.6);
  const pad = 8;

  const rows = [
    { label: "CAM",  value: `#${cam.id}`,                        color: "#4da8ff" },
    { label: "FPS",  value: `${cam.fps.toFixed(1)}`,             color: getFpsColor(cam.fps) },
    { label: "LAT",  value: `${cam.latency_ms.toFixed(0)} ms`,   color: getLatColor(cam.latency_ms) },
    { label: "RES",  value: cam.resolution || "—",               color: "#dce8f5" },
    { label: "RSSI", value: cam.rssi != null ? `${cam.rssi} dBm` : "—", color: getRssiColor(cam.rssi) },
    { label: "FRM",  value: String(cam.frame_id || 0),           color: "#6b7f9a" },
  ];

  ctx.save();
  ctx.font = `bold ${fs}px 'Courier New', monospace`;

  // Measure for box width
  const labelW = ctx.measureText("RSSI").width + 4;
  let   maxValW = 0;
  for (const r of rows) {
    const w = ctx.measureText(r.value).width;
    if (w > maxValW) maxValW = w;
  }

  const sepW = ctx.measureText(" · ").width;
  const boxW = pad * 2 + labelW + sepW + maxValW;
  const boxH = pad * 2 + rows.length * lh - (lh - fs);
  const x    = pad;
  const y    = canvas.height - pad - boxH;

  // Background
  ctx.globalAlpha = 0.70;
  ctx.fillStyle   = "#080c11";
  roundRect(ctx, x, y, boxW, boxH, 5);
  ctx.fill();

  // Border
  ctx.globalAlpha = 0.50;
  ctx.strokeStyle = "#2a3547";
  ctx.lineWidth   = 1;
  roundRect(ctx, x, y, boxW, boxH, 5);
  ctx.stroke();

  ctx.globalAlpha = 1;
  ctx.font        = `bold ${fs}px 'Courier New', monospace`;

  for (let i = 0; i < rows.length; i++) {
    const { label, value, color } = rows[i];
    const ty = y + pad + i * lh + fs;

    ctx.fillStyle = "#5a6e88";
    ctx.fillText(label.padEnd(4), x + pad, ty);

    ctx.fillStyle = "#2a3547";
    ctx.fillText("·", x + pad + labelW, ty);

    ctx.fillStyle = color;
    ctx.fillText(value, x + pad + labelW + sepW, ty);
  }

  ctx.restore();
}

function roundRect(ctx, x, y, w, h, r) {
  ctx.beginPath();
  ctx.moveTo(x + r, y);
  ctx.lineTo(x + w - r, y); ctx.quadraticCurveTo(x + w, y,     x + w, y + r);
  ctx.lineTo(x + w, y + h - r); ctx.quadraticCurveTo(x + w, y + h, x + w - r, y + h);
  ctx.lineTo(x + r, y + h); ctx.quadraticCurveTo(x, y + h, x, y + h - r);
  ctx.lineTo(x, y + r); ctx.quadraticCurveTo(x, y, x + r, y);
  ctx.closePath();
}

function getFpsColor(fps) {
  if (fps >= 12) return "#3de07a";
  if (fps >=  6) return "#f0c040";
  return "#ff4d4d";
}
function getLatColor(ms) {
  if (ms <=  80) return "#3de07a";
  if (ms <= 160) return "#f0c040";
  return "#ff4d4d";
}
function getRssiColor(rssi) {
  if (rssi == null) return "#6b7f9a";
  if (rssi > -60)   return "#3de07a";
  if (rssi > -75)   return "#f0c040";
  return "#ff4d4d";
}

// ─────────────────────────────────────────────────────────────────
//  STATS HANDLER
// ─────────────────────────────────────────────────────────────────
function onStats(list) {
  let totalFps = 0, liveCams = 0;

  for (const s of list) {
    const { camera_id, fps, online, resolution, wifi_rssi, source_ip } = s;
    if (!cameras.has(camera_id)) createCard(camera_id);
    const cam = cameras.get(camera_id);

    if (resolution)  cam.resolution = resolution;
    if (wifi_rssi != null) cam.rssi = wifi_rssi;
    if (source_ip)   cam.sourceIp   = source_ip;

    setCardOnline(cam, online);

    const ipEl = cam.panel.querySelector(".ip-label");
    if (ipEl && cam.sourceIp) ipEl.textContent = cam.sourceIp;

    if (online) { totalFps += fps; liveCams++; }
  }

  el("camCount").textContent  = cameras.size;
  el("globalFps").textContent = liveCams ? (totalFps / liveCams).toFixed(1) : "0";
  el("emptyState").classList.toggle("hidden", cameras.size > 0);
}

// ─────────────────────────────────────────────────────────────────
//  CARD CREATION
// ─────────────────────────────────────────────────────────────────
function createCard(camera_id) {
  if (cameras.has(camera_id)) return cameras.get(camera_id);

  const tpl   = document.getElementById("camCardTemplate");
  const frag  = tpl.content.cloneNode(true);
  const panel = frag.querySelector(".cam-card");

  panel.dataset.id = camera_id;
  panel.querySelector(".cam-num").textContent = camera_id;

  const canvas = panel.querySelector(".vid-canvas");
  const ctx    = canvas.getContext("2d");

  const cam = {
    id: camera_id, panel, canvas, ctx,
    fps: 0, latency_ms: 0, frame_id: 0,
    resolution: "—", rssi: null, sourceIp: null,
    flashOn: false, online: false, paused: false, lastFrame: 0,
  };

  cameras.set(camera_id, cam);
  wireCard(cam);

  document.getElementById("camGrid").appendChild(frag);
  applyGridCols();
  el("emptyState").classList.add("hidden");
  return cam;
}

// ─────────────────────────────────────────────────────────────────
//  CARD WIRING
// ─────────────────────────────────────────────────────────────────
function wireCard(cam) {
  const p  = cam.panel;
  const id = cam.id;

  // Pause / resume
  const streamBtn = p.querySelector(".stream-toggle-btn");
  streamBtn.addEventListener("click", () => {
    cam.paused = !cam.paused;
    streamBtn.classList.toggle("is-paused", cam.paused);
    p.classList.toggle("is-paused", cam.paused);
    p.querySelector(".icon-pause").classList.toggle("hidden",  cam.paused);
    p.querySelector(".icon-play").classList.toggle("hidden",  !cam.paused);
    apiPost(`/api/cameras/${id}/stream`, { streaming: !cam.paused });
  });

  // Restart
  p.querySelector(".restart-btn").addEventListener("click", () => {
    if (!confirm(`Restart Camera #${id}?`)) return;
    apiPost(`/api/cameras/${id}/restart`, {});
  });

  // Flash OFF / ON
  p.querySelector(".flash-off-btn").addEventListener("click", () => {
    setFlashUI(cam, false);
    apiFlash(id, "off", getBri(p));
  });
  p.querySelector(".flash-on-btn").addEventListener("click", () => {
    setFlashUI(cam, true);
    apiFlash(id, "on", getBri(p));
  });

  // Brightness slider
  let bTimer = null;
  p.querySelector(".flash-brightness").addEventListener("input", (e) => {
    p.querySelector(".flash-bri-val").textContent = e.target.value;
    clearTimeout(bTimer);
    bTimer = setTimeout(() => apiFlash(id, cam.flashOn ? "on" : "off", parseInt(e.target.value)), 220);
  });

  // Config sliders — live label update
  p.querySelector(".quality-range").addEventListener("input", (e) =>
    p.querySelector(".quality-val").textContent = e.target.value);
  p.querySelector(".fps-range").addEventListener("input", (e) =>
    p.querySelector(".fps-limit-val").textContent = e.target.value);

  // Sensor sliders — live label update
  p.querySelector(".brightness-range").addEventListener("input", (e) =>
    p.querySelector(".brightness-val").textContent = e.target.value);
  p.querySelector(".contrast-range").addEventListener("input", (e) =>
    p.querySelector(".contrast-val").textContent = e.target.value);
  p.querySelector(".saturation-range").addEventListener("input", (e) =>
    p.querySelector(".saturation-val").textContent = e.target.value);

  // Apply config + sensor
  p.querySelector(".apply-btn").addEventListener("click", () => {
    apiPost(`/api/cameras/${id}/config`, {
      resolution:   p.querySelector(".res-select").value,
      jpeg_quality: parseInt(p.querySelector(".quality-range").value),
      fps_limit:    parseInt(p.querySelector(".fps-range").value),
      brightness:   parseInt(p.querySelector(".brightness-range").value),
      contrast:     parseInt(p.querySelector(".contrast-range").value),
      saturation:   parseInt(p.querySelector(".saturation-range").value),
      awb:          p.querySelector(".awb-toggle").checked,
      agc:          p.querySelector(".agc-toggle").checked,
      aec:          p.querySelector(".aec-toggle").checked,
    });
  });
}

function setFlashUI(cam, on) {
  cam.flashOn = on;
  const p = cam.panel;
  p.querySelector(".flash-off-btn").classList.toggle("active",  !on);
  p.querySelector(".flash-on-btn").classList.toggle("active",   on);
  p.querySelector(".flash-badge").classList.toggle("hidden",    !on);
}

function getBri(panel) {
  return parseInt(panel.querySelector(".flash-brightness").value);
}

// ─────────────────────────────────────────────────────────────────
//  ONLINE / OFFLINE STATE
// ─────────────────────────────────────────────────────────────────
function setCardOnline(cam, online) {
  if (cam.online === online) return;
  cam.online = online;
  cam.panel.classList.toggle("is-offline", !online);
  cam.panel.querySelector(".online-dot").classList.toggle("live", online);
}

// Watchdog — 5 s no frame → offline
setInterval(() => {
  const now = performance.now();
  for (const cam of cameras.values()) {
    if (cam.lastFrame && now - cam.lastFrame > 5000 && cam.online)
      setCardOnline(cam, false);
  }
}, 2000);

// ─────────────────────────────────────────────────────────────────
//  GRID COLUMNS
// ─────────────────────────────────────────────────────────────────
function applyGridCols() {
  document.getElementById("camGrid").style.setProperty("--cols", CFG.gridCols);
  document.querySelectorAll(".gcol-btn").forEach((b) =>
    b.classList.toggle("active", parseInt(b.dataset.cols) === CFG.gridCols));
}

// ─────────────────────────────────────────────────────────────────
//  WS STATUS BAR
// ─────────────────────────────────────────────────────────────────
function setWsStatus(cls, text) {
  el("wsDot").className    = `ws-dot ${cls}`;
  el("wsText").textContent = text;
}

// ─────────────────────────────────────────────────────────────────
//  API HELPERS
// ─────────────────────────────────────────────────────────────────
const BASE = `${location.protocol}//${location.hostname}:${location.port || 8000}`;

function apiHeaders() {
  const h = { "Content-Type": "application/json" };
  if (CFG.apiKey) h["X-API-Key"] = CFG.apiKey;
  return h;
}

async function apiPost(path, body) {
  try {
    const r = await fetch(`${BASE}${path}`, {
      method: "POST", headers: apiHeaders(), body: JSON.stringify(body),
    });
    if (!r.ok) { const e = await r.json().catch(() => {}); console.warn(`API ${path}:`, r.status, e); return null; }
    return r.json();
  } catch (e) { console.warn(`API fetch ${path}:`, e); return null; }
}

function apiFlash(id, state, brightness) {
  return apiPost(`/api/cameras/${id}/flash`, { state, brightness });
}

// ─────────────────────────────────────────────────────────────────
//  SETTINGS DRAWER
// ─────────────────────────────────────────────────────────────────
function initSettings() {
  const drawer = el("settingsDrawer");

  el("settingsBtn").addEventListener("click", () => {
    const open = drawer.classList.toggle("hidden") === false;
    el("settingsBtn").classList.toggle("active", open);
    if (open) {
      el("serverUrl").value    = CFG.serverUrl;
      el("apiKey").value       = CFG.apiKey;
      el("osdFontSize").value  = CFG.osdFontSize;
      el("osdDefault").checked = CFG.osdVisible;
    }
  });

  el("closeDrawer").addEventListener("click", () => {
    drawer.classList.add("hidden");
    el("settingsBtn").classList.remove("active");
  });

  el("saveSettings").addEventListener("click", () => {
    CFG.serverUrl   = el("serverUrl").value.trim()  || DEFAULTS.serverUrl;
    CFG.apiKey      = el("apiKey").value.trim();
    CFG.osdFontSize = parseInt(el("osdFontSize").value) || 12;
    CFG.osdVisible  = el("osdDefault").checked;
    osdVisible      = CFG.osdVisible;
    saveCfg();
    drawer.classList.add("hidden");
    el("settingsBtn").classList.remove("active");
    connect();
  });
}

// ─────────────────────────────────────────────────────────────────
//  OSD TOGGLE
// ─────────────────────────────────────────────────────────────────
function initOsdToggle() {
  const btn = el("osdToggleBtn");
  btn.classList.toggle("active", osdVisible);
  btn.addEventListener("click", () => {
    osdVisible = !osdVisible;
    btn.classList.toggle("active", osdVisible);
    CFG.osdVisible = osdVisible;
    saveCfg();
  });
}

// ─────────────────────────────────────────────────────────────────
//  GRID PICKER
// ─────────────────────────────────────────────────────────────────
function initGridPicker() {
  document.querySelectorAll(".gcol-btn").forEach((btn) => {
    btn.addEventListener("click", () => {
      CFG.gridCols = parseInt(btn.dataset.cols);
      saveCfg();
      applyGridCols();
    });
  });
}

// ─────────────────────────────────────────────────────────────────
//  UTIL
// ─────────────────────────────────────────────────────────────────
function el(id) { return document.getElementById(id); }

// ─────────────────────────────────────────────────────────────────
//  BOOT
// ─────────────────────────────────────────────────────────────────
document.addEventListener("DOMContentLoaded", () => {
  applyGridCols();
  initSettings();
  initOsdToggle();
  initGridPicker();
  connect();
});
