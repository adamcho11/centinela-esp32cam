"use strict";

const http = require("http");
const path = require("path");
const crypto = require("crypto");
const express = require("express");
const { WebSocketServer } = require("ws");

const app = express();
const server = http.createServer(app);
const wss = new WebSocketServer({ noServer: true, maxPayload: 2 * 1024 * 1024 });
const PORT = Number(process.env.PORT || 3000);
const SESSION_MS = 30 * 60 * 1000;
const ADMIN_USER = process.env.ADMIN_USER || "admin";
const ADMIN_PASSWORD = process.env.ADMIN_PASSWORD || "change-me-now";
const DEVICE_TOKEN = process.env.DEVICE_TOKEN || "change-me-device-token";
const clients = new Set();
const sessions = new Map();
const state = { online: false, streaming: false, recording: false, sd: false, camera: false, board: "", ip: "" };
let device = null;
let download = null;

const open = (ws) => ws && ws.readyState === 1;
const json = (ws, value) => { if (open(ws)) ws.send(JSON.stringify(value)); };
const broadcast = (value) => clients.forEach((ws) => json(ws, value));
const toDevice = (value) => open(device) ? (json(device, value), true) : false;

function parseCookies(header = "") {
  return Object.fromEntries(header.split(";").map((part) => part.trim().split("=")).filter(([key, value]) => key && value).map(([key, value]) => [key, decodeURIComponent(value)]));
}

function sessionFromRequest(req) {
  const token = parseCookies(req.headers.cookie).centinela_session;
  const session = token && sessions.get(token);
  if (!session || session.expiresAt < Date.now()) { if (token) sessions.delete(token); return null; }
  session.expiresAt = Date.now() + SESSION_MS;
  return session;
}

function requireSession(req, res, next) {
  if (!sessionFromRequest(req)) return res.status(401).json({ error: "authentication_required" });
  next();
}

function rejectUpgrade(socket) {
  socket.write("HTTP/1.1 401 Unauthorized\r\nConnection: close\r\n\r\n");
  socket.destroy();
}

function safeFile(value) {
  if (typeof value !== "string") return null;
  let decoded;
  try { decoded = decodeURIComponent(value.trim().replaceAll("\\", "/")); } catch { return null; }
  const parts = decoded.split("/").filter(Boolean);
  const allowed = new Set([".avi", ".mjpeg", ".mjpg", ".jpg", ".jpeg", ".mp4", ".mov", ".mkv"]);
  if (!parts.length || parts.length > 4 || parts.some((part) => part === "." || part === ".." || part.includes("\0"))) return null;
  if (!allowed.has(path.extname(parts.at(-1)).toLowerCase())) return null;
  return `/${parts.join("/")}`;
}

function mime(file) {
  const ext = path.extname(file).toLowerCase();
  if (ext === ".avi") return "video/x-msvideo";
  if (ext === ".mjpeg" || ext === ".mjpg") return "video/x-motion-jpeg";
  if (ext === ".mp4") return "video/mp4";
  if (ext === ".mov") return "video/quicktime";
  if (ext === ".mkv") return "video/x-matroska";
  return "image/jpeg";
}

function packet(buffer) {
  if (!Buffer.isBuffer(buffer) || buffer.length < 12 || buffer.toString("ascii", 0, 4) !== "RCAM") return null;
  return { type: buffer[4], flags: buffer[9], body: buffer.subarray(12) };
}

function finishDownload(ok, error = "") {
  if (!download) return;
  const current = download;
  download = null;
  clearTimeout(current.timer);
  if (current.kind === "http") {
    if (ok && !current.res.writableEnded) current.res.end();
    else if (!ok && !current.res.headersSent) current.res.status(502).json({ ok: false, error });
    else if (!ok && !current.res.writableEnded) current.res.destroy();
  } else {
    json(current.ws, { type: "download_done", id: current.id, file: current.file, ok, error: error || undefined });
  }
  broadcast({ type: "download_done", id: current.id, file: current.file, ok, error: error || undefined });
}

function abortDownload(error) {
  if (!download) return;
  toDevice({ cmd: "abort_download", id: download.id });
  finishDownload(false, error);
}

function startDownloadResponse(req, res) {
  const file = safeFile(req.params[0]);
  if (!file) return res.status(400).json({ error: "invalid_file" });
  if (!open(device)) return res.status(502).json({ error: "device_offline" });
  if (download) return res.status(409).json({ error: "another_download_is_active" });
  const id = `http-${Date.now()}-${Math.random().toString(16).slice(2)}`;
  download = { kind: "http", id, file, res, started: false, received: 0, size: 0, lastProgress: 0, inline: req.query.inline === "1" };
  download.timer = setTimeout(() => download && download.id === id && abortDownload("device_timeout"), 120000);
  res.on("close", () => download && download.id === id && abortDownload("browser_aborted"));
  toDevice({ cmd: "download", id, file });
}

function deviceText(data) {
  let msg; try { msg = JSON.parse(data.toString()); } catch { return; }
  if (msg.type === "hello" || msg.type === "state") {
    for (const key of Object.keys(state)) if (Object.prototype.hasOwnProperty.call(msg, key)) state[key] = msg[key];
    state.online = true;
    broadcast({ type: "device_state", ...state });
  } else if (msg.type === "stream_state") {
    state.streaming = Boolean(msg.active); broadcast({ type: "stream_state", active: state.streaming });
  } else if (msg.type === "record_state") {
    state.recording = Boolean(msg.active); broadcast({ type: "record_state", active: state.recording });
  } else if (msg.type === "files") {
    broadcast({ type: "files", files: Array.isArray(msg.files) ? msg.files : [] });
  } else if (msg.type === "file_start") {
    if (!download || String(msg.id || "") !== download.id) return;
    download.file = safeFile(msg.file) || download.file; download.size = Number(msg.size) || 0; download.started = true;
    if (download.kind === "http") {
      const headers = { "Content-Type": mime(download.file), "Content-Disposition": `${download.inline ? "inline" : "attachment"}; filename="${path.basename(download.file).replace(/[\r\n"\\]/g, "_")}"`, "Cache-Control": "no-store" };
      if (download.size) headers["Content-Length"] = String(download.size);
      download.res.writeHead(200, headers);
    } else json(download.ws, { type: "download_start", id: download.id, file: download.file, size: download.size });
    broadcast({ type: "download_start", id: download.id, file: download.file, size: download.size, kind: download.kind });
  } else if (msg.type === "file_end" && download && String(msg.id || "") === download.id) {
    finishDownload(Boolean(msg.ok), msg.error || "");
  } else if (msg.type === "error") {
    broadcast({ type: "error", message: msg.message || "device_error", id: msg.id });
    if (download && String(msg.id || "") === download.id) finishDownload(false, msg.message || "device_error");
  } else broadcast(msg);
}

function deviceBinary(data) {
  const msg = packet(data);
  if (!msg) return;
  if (msg.type === 0) return clients.forEach((ws) => open(ws) && ws.send(data, { binary: true }));
  if (msg.type !== 1 || !download || !download.started) return;
  download.received += msg.body.length;
  if (download.kind === "http") { if (!download.res.writableEnded) download.res.write(msg.body); }
  else if (open(download.ws)) download.ws.send(data, { binary: true });
  if (download.received - download.lastProgress >= 65536 || msg.flags & 1) {
    download.lastProgress = download.received;
    const progress = { type: "download_progress", id: download.id, received: download.received, size: download.size };
    if (download.kind === "ws") json(download.ws, progress);
    broadcast(progress);
  }
  if (msg.flags & 1) finishDownload(true);
}

function clientText(ws, data) {
  let command; try { command = JSON.parse(data.toString()); } catch { return json(ws, { type: "error", message: "invalid_json" }); }
  if (!command || typeof command.cmd !== "string") return json(ws, { type: "error", message: "invalid_command" });
  if (command.cmd === "get_state") return json(ws, { type: "device_state", ...state });
  if (command.cmd === "download") {
    const file = safeFile(command.file);
    if (!file) return json(ws, { type: "error", id: command.id, message: "invalid_file" });
    if (!open(device)) return json(ws, { type: "error", id: command.id, message: "device_offline" });
    if (download) return json(ws, { type: "error", id: command.id, message: "another_download_is_active" });
    const id = String(command.id || `ws-${Date.now()}`);
    download = { kind: "ws", id, file, ws, started: false, received: 0, size: 0, lastProgress: 0 };
    download.timer = setTimeout(() => download && download.id === id && abortDownload("device_timeout"), 120000);
    return toDevice({ cmd: "download", id, file });
  }
  if (command.cmd === "abort_download") return download && download.kind === "ws" && download.ws === ws ? abortDownload("browser_aborted") : undefined;
  if (!toDevice(command)) json(ws, { type: "error", id: command.id, message: "device_offline" });
}

function disconnected(ws) {
  clients.delete(ws);
  if (download && download.kind === "ws" && download.ws === ws) abortDownload("browser_disconnected");
  if (device === ws) {
    device = null; Object.assign(state, { online: false, streaming: false, recording: false, sd: false, camera: false });
    if (download && download.kind === "http") finishDownload(false, "device_disconnected");
    broadcast({ type: "device_state", ...state });
  }
}

server.on("upgrade", (req, socket, head) => {
  const url = new URL(req.url, `http://${req.headers.host || "localhost"}`);
  if (url.pathname !== "/ws") return socket.destroy();
  const role = (url.searchParams.get("role") || "client").toLowerCase();
  if (role === "device" && url.searchParams.get("token") !== DEVICE_TOKEN) return rejectUpgrade(socket);
  if (role !== "device" && !sessionFromRequest(req)) return rejectUpgrade(socket);
  wss.handleUpgrade(req, socket, head, (ws) => {
    ws.isAlive = true; ws.on("pong", () => { ws.isAlive = true; }); ws.on("error", () => {}); ws.on("close", () => disconnected(ws));
    if (role === "device") {
      if (device && device !== ws) device.close(1000, "replaced"); device = ws; state.online = true;
      broadcast({ type: "device_state", ...state }); ws.on("message", (data, binary) => binary ? deviceBinary(data) : deviceText(data));
    } else {
      clients.add(ws); json(ws, { type: "hello", role: "server", device: state }); ws.on("message", (data, binary) => !binary && clientText(ws, data));
    }
  });
});

app.use(express.urlencoded({ extended: false }));
app.get("/health", (_req, res) => res.json({ ok: true, deviceOnline: state.online }));
app.get("/api/session", (req, res) => { const session = sessionFromRequest(req); res.json({ authenticated: Boolean(session), user: session ? session.user : null }); });
app.post("/api/login", (req, res) => {
  if (req.body.user !== ADMIN_USER || req.body.password !== ADMIN_PASSWORD) return res.status(401).json({ error: "invalid_credentials" });
  const token = crypto.randomBytes(32).toString("hex");
  sessions.set(token, { user: ADMIN_USER, expiresAt: Date.now() + SESSION_MS });
  res.setHeader("Set-Cookie", `centinela_session=${token}; HttpOnly; SameSite=Lax; Path=/; Max-Age=${SESSION_MS / 1000}${process.env.NODE_ENV === "production" ? "; Secure" : ""}`);
  res.json({ ok: true, user: ADMIN_USER });
});
app.post("/api/logout", (req, res) => {
  const token = parseCookies(req.headers.cookie).centinela_session;
  if (token) sessions.delete(token);
  res.setHeader("Set-Cookie", "centinela_session=; HttpOnly; SameSite=Lax; Path=/; Max-Age=0");
  res.json({ ok: true });
});
app.get("/api/download/*", requireSession, startDownloadResponse);
app.use(express.static(path.join(__dirname, "public")));
app.get("*", (_req, res) => res.sendFile(path.join(__dirname, "public", "index.html")));

const heartbeat = setInterval(() => wss.clients.forEach((ws) => { if (!ws.isAlive) ws.terminate(); else { ws.isAlive = false; ws.ping(); } }), 30000);
server.listen(PORT, () => console.log(`Centinela relay listening on port ${PORT}`));
function shutdown() { clearInterval(heartbeat); if (download) abortDownload("server_shutdown"); wss.close(); server.close(() => process.exit(0)); }
process.on("SIGTERM", shutdown); process.on("SIGINT", shutdown);
