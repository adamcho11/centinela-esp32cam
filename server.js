"use strict";

const http = require("http");
const path = require("path");
const express = require("express");
const { WebSocketServer } = require("ws");

const PORT = Number(process.env.PORT || 3000);
const app = express();
const server = http.createServer(app);
const wss = new WebSocketServer({ noServer: true, maxPayload: 2 * 1024 * 1024 });

const clients = new Set();
let device = null;
let activeDownload = null;
const deviceState = {
  online: false,
  streaming: false,
  recording: false,
  sd: false,
  camera: false,
  board: "",
  ip: "",
};
const deviceStateFields = ["online", "streaming", "recording", "sd", "camera", "board", "ip"];

function isOpen(ws) {
  return ws && ws.readyState === 1;
}

function sendJson(ws, payload) {
  if (isOpen(ws)) ws.send(JSON.stringify(payload));
}

function sendToDevice(payload) {
  if (!isOpen(device)) return false;
  sendJson(device, payload);
  return true;
}

function broadcast(payload) {
  for (const ws of clients) sendJson(ws, payload);
}

function broadcastBinary(payload) {
  for (const ws of clients) {
    if (isOpen(ws)) ws.send(payload, { binary: true });
  }
}

function updateDeviceState(message) {
  for (const field of deviceStateFields) {
    if (Object.prototype.hasOwnProperty.call(message, field)) deviceState[field] = message[field];
  }
  deviceState.online = true;
}

function parsePacket(payload) {
  if (!Buffer.isBuffer(payload) || payload.length < 12) return null;
  if (payload.toString("ascii", 0, 4) !== "RCAM") return null;
  return {
    type: payload.readUInt8(4),
    sequence: payload.readUInt32BE(5),
    flags: payload.readUInt8(9),
    body: payload.subarray(12),
  };
}

function safeFilePath(value) {
  if (typeof value !== "string" || !value.trim()) return null;
  const normalized = value.trim().replaceAll("\\", "/");
  let decoded;
  try {
    decoded = decodeURIComponent(normalized);
  } catch {
    return null;
  }

  const parts = decoded.split("/").filter(Boolean);
  if (!parts.length || parts.length > 4) return null;
  if (parts.some((part) => part === "." || part === ".." || part.includes("\0"))) return null;

  const extension = path.extname(parts[parts.length - 1]).toLowerCase();
  const allowed = new Set([".avi", ".mjpeg", ".mjpg", ".jpg", ".jpeg", ".mp4", ".mov", ".mkv"]);
  if (!allowed.has(extension)) return null;
  return `/${parts.join("/")}`;
}

function contentType(file) {
  switch (path.extname(file).toLowerCase()) {
    case ".avi": return "video/x-msvideo";
    case ".mjpeg":
    case ".mjpg": return "video/x-motion-jpeg";
    case ".mp4": return "video/mp4";
    case ".mov": return "video/quicktime";
    case ".mkv": return "video/x-matroska";
    case ".jpg":
    case ".jpeg": return "image/jpeg";
    default: return "application/octet-stream";
  }
}

function downloadName(file) {
  return path.basename(file).replace(/[\r\n"\\]/g, "_");
}

function finishDownload(ok, error = "") {
  if (!activeDownload) return;
  const download = activeDownload;
  activeDownload = null;
  clearTimeout(download.timeout);

  if (download.kind === "http") {
    if (ok) {
      if (!download.res.writableEnded) download.res.end();
    } else if (!download.res.headersSent) {
      download.res.status(502).json({ ok: false, error: error || "download_failed" });
    } else if (!download.res.writableEnded) {
      download.res.destroy();
    }
  } else {
    sendJson(download.ws, {
      type: "download_done",
      id: download.id,
      file: download.file,
      ok,
      error: error || undefined,
    });
  }

  broadcast({
    type: "download_done",
    id: download.id,
    file: download.file,
    ok,
    error: error || undefined,
  });
}

function abortDownload(error) {
  if (!activeDownload) return;
  const id = activeDownload.id;
  sendToDevice({ cmd: "abort_download", id });
  finishDownload(false, error || "download_aborted");
}

function startHttpDownload(req, res) {
  const file = safeFilePath(req.params[0]);
  if (!file) return res.status(400).json({ error: "invalid_file" });
  if (!isOpen(device)) return res.status(502).json({ error: "device_offline" });
  if (activeDownload) return res.status(409).json({ error: "another_download_is_active" });

  const id = `http-${Date.now()}-${Math.random().toString(16).slice(2)}`;
  const download = {
    kind: "http",
    id,
    file,
    res,
    inline: req.query.inline === "1" || req.query.inline === "true",
    started: false,
    received: 0,
    size: 0,
    lastProgress: 0,
    timeout: null,
  };
  download.timeout = setTimeout(() => {
    if (activeDownload && activeDownload.id === id) abortDownload("device_timeout");
  }, 120000);
  activeDownload = download;

  res.on("close", () => {
    if (activeDownload && activeDownload.id === id) abortDownload("browser_aborted");
  });

  sendToDevice({ cmd: "download", id, file });
  return undefined;
}

function handleFileStart(message) {
  if (!activeDownload || String(message.id || "") !== activeDownload.id) return;

  const download = activeDownload;
  download.file = safeFilePath(message.file) || download.file;
  download.size = Number(message.size) || 0;
  download.started = true;

  if (download.kind === "http") {
    const headers = {
      "Content-Type": contentType(download.file),
      "Content-Disposition": `${download.inline ? "inline" : "attachment"}; filename="${downloadName(download.file)}"`,
      "Cache-Control": "no-store",
    };
    if (download.size > 0) headers["Content-Length"] = String(download.size);
    download.res.writeHead(200, headers);
  } else {
    sendJson(download.ws, {
      type: "download_start",
      id: download.id,
      file: download.file,
      size: download.size,
    });
  }

  broadcast({
    type: "download_start",
    id: download.id,
    file: download.file,
    size: download.size,
    kind: download.kind,
  });
}

function handleDeviceText(data) {
  let message;
  try {
    message = JSON.parse(data.toString("utf8"));
  } catch {
    return;
  }

  if (message.type === "hello" || message.type === "state") {
    updateDeviceState(message);
    broadcast({ type: "device_state", ...deviceState });
    return;
  }
  if (message.type === "stream_state") {
    deviceState.streaming = Boolean(message.active);
    broadcast({ type: "stream_state", active: deviceState.streaming });
    return;
  }
  if (message.type === "record_state") {
    deviceState.recording = Boolean(message.active);
    broadcast({ type: "record_state", active: deviceState.recording });
    return;
  }
  if (message.type === "files") {
    broadcast({ type: "files", files: Array.isArray(message.files) ? message.files : [] });
    return;
  }
  if (message.type === "file_start") {
    handleFileStart(message);
    return;
  }
  if (message.type === "file_end") {
    if (activeDownload && String(message.id || "") === activeDownload.id) {
      finishDownload(Boolean(message.ok), message.error || "");
    }
    return;
  }
  if (message.type === "error") {
    broadcast({ type: "error", message: message.message || "device_error", id: message.id });
    if (activeDownload && String(message.id || "") === activeDownload.id) {
      finishDownload(false, message.message || "device_error");
    }
    return;
  }

  broadcast(message);
}

function handleDeviceBinary(data) {
  const packet = parsePacket(data);
  if (!packet) return;

  if (packet.type === 0) {
    broadcastBinary(data);
    return;
  }
  if (packet.type !== 1 || !activeDownload) return;

  const download = activeDownload;
  if (!download.started) return;
  download.received += packet.body.length;

  if (download.kind === "http") {
    if (!download.res.writableEnded) download.res.write(packet.body);
  } else if (isOpen(download.ws)) {
    download.ws.send(data, { binary: true });
  }

  if (download.received - download.lastProgress >= 65536 || (packet.flags & 1) !== 0) {
    download.lastProgress = download.received;
    const progress = {
      type: "download_progress",
      id: download.id,
      received: download.received,
      size: download.size,
    };
    if (download.kind === "ws") sendJson(download.ws, progress);
    broadcast(progress);
  }

  if ((packet.flags & 1) !== 0) finishDownload(true);
}

function handleClientText(ws, data) {
  let command;
  try {
    command = JSON.parse(data.toString("utf8"));
  } catch {
    return sendJson(ws, { type: "error", message: "invalid_json" });
  }

  if (!command || typeof command.cmd !== "string") {
    return sendJson(ws, { type: "error", message: "invalid_command" });
  }

  if (command.cmd === "get_state") {
    sendJson(ws, { type: "device_state", ...deviceState });
    return;
  }

  if (command.cmd === "download") {
    const file = safeFilePath(command.file);
    if (!file) return sendJson(ws, { type: "error", id: command.id, message: "invalid_file" });
    if (!isOpen(device)) return sendJson(ws, { type: "error", id: command.id, message: "device_offline" });
    if (activeDownload) return sendJson(ws, { type: "error", id: command.id, message: "another_download_is_active" });

    const id = String(command.id || `ws-${Date.now()}-${Math.random().toString(16).slice(2)}`);
    activeDownload = {
      kind: "ws",
      id,
      file,
      ws,
      started: false,
      received: 0,
      size: 0,
      lastProgress: 0,
      timeout: setTimeout(() => {
        if (activeDownload && activeDownload.id === id) abortDownload("device_timeout");
      }, 120000),
    };
    sendToDevice({ cmd: "download", id, file });
    return;
  }

  if (command.cmd === "abort_download") {
    if (activeDownload && activeDownload.kind === "ws" && activeDownload.ws === ws) {
      abortDownload("browser_aborted");
    }
    return;
  }

  if (!sendToDevice(command)) {
    sendJson(ws, { type: "error", id: command.id, message: "device_offline" });
  }
}

function handleDisconnect(ws) {
  clients.delete(ws);
  if (activeDownload && activeDownload.kind === "ws" && activeDownload.ws === ws) {
    abortDownload("browser_disconnected");
  }
  if (device === ws) {
    device = null;
    Object.assign(deviceState, { online: false, streaming: false, recording: false, sd: false, camera: false });
    if (activeDownload && activeDownload.kind === "http") finishDownload(false, "device_disconnected");
    broadcast({ type: "device_state", ...deviceState });
  }
}

server.on("upgrade", (req, socket, head) => {
  const requestUrl = new URL(req.url, `http://${req.headers.host || "localhost"}`);
  if (requestUrl.pathname !== "/ws") {
    socket.destroy();
    return;
  }

  const role = (requestUrl.searchParams.get("role") || "client").toLowerCase();
  wss.handleUpgrade(req, socket, head, (ws) => {
    ws.role = role;
    ws.isAlive = true;
    ws.on("pong", () => { ws.isAlive = true; });
    ws.on("error", () => {});
    ws.on("close", () => handleDisconnect(ws));

    if (role === "device") {
      if (device && device !== ws) device.close(1000, "replaced");
      device = ws;
      deviceState.online = true;
      broadcast({ type: "device_state", ...deviceState });
      ws.on("message", (data, isBinary) => {
        if (isBinary) handleDeviceBinary(data);
        else handleDeviceText(data);
      });
      return;
    }

    clients.add(ws);
    sendJson(ws, { type: "hello", role: "server", device: deviceState });
    ws.on("message", (data, isBinary) => {
      if (!isBinary) handleClientText(ws, data);
    });
  });
});

app.set("trust proxy", 1);
app.get("/health", (_req, res) => res.json({ ok: true, deviceOnline: deviceState.online }));
app.get("/api/download/*", startHttpDownload);
app.use(express.static(path.join(__dirname, "public")));
app.get("*", (_req, res) => res.sendFile(path.join(__dirname, "public", "index.html")));

const heartbeat = setInterval(() => {
  for (const ws of wss.clients) {
    if (!ws.isAlive) {
      ws.terminate();
      continue;
    }
    ws.isAlive = false;
    ws.ping();
  }
}, 30000);

server.listen(PORT, () => {
  console.log(`Centinela relay listening on port ${PORT}`);
});

function shutdown() {
  clearInterval(heartbeat);
  if (activeDownload) abortDownload("server_shutdown");
  wss.close();
  server.close(() => process.exit(0));
}

process.on("SIGTERM", shutdown);
process.on("SIGINT", shutdown);
