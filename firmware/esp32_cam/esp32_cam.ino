/*
 * AI-Thinker ESP32-CAM -> Centinela Render relay
 *
 * Librerias Arduino IDE:
 *   - ArduinoJson
 *   - ArduinoWebsockets (Gil Maimon)
 *
 * Selecciona AI Thinker ESP32-CAM, habilita PSRAM y usa una particion con
 * suficiente espacio para el firmware. Cambia WIFI_SSID, WIFI_PASSWORD y
 * WS_URL antes de compilar.
 */

#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "FS.h"
#include "SD_MMC.h"
#include "WiFi.h"
#include <ArduinoJson.h>
#include <ArduinoWebsockets.h>

using namespace websockets;

// AI-Thinker ESP32-CAM / OV2640
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

// AI-Thinker SD_MMC 1-bit: CLK=14, CMD=15, D0=2.
#define SD_CLK_GPIO       14
#define SD_CMD_GPIO       15
#define SD_D0_GPIO         2

#define DOWNLOAD_CHUNK_SIZE 2048
#define PACKET_HEADER_SIZE 12
#define MAX_CHUNKS_PER_LOOP 4

const char *WIFI_SSID = "CAMBIA_SSID";
const char *WIFI_PASSWORD = "CAMBIA_PASSWORD";
const char *WS_URL = "wss://TU-APP.onrender.com/ws?role=device";
const char *BOARD_ID = "esp32cam-01";

const uint32_t FRAME_INTERVAL_MS = 100;       // 10 FPS maximo
const uint32_t WIFI_RETRY_MS = 10000;
const uint32_t WS_RETRY_MS = 5000;
const uint32_t RECORD_MAX_MS = 15UL * 60UL * 1000UL;
const uint32_t RECORD_MIN_FREE_BYTES = 5UL * 1024UL * 1024UL;

WebsocketsClient socketClient;
bool cameraReady = false;
bool sdReady = false;
bool streaming = false;
bool recording = false;
bool downloadActive = false;
bool downloadAbort = false;
bool resumeStreamingAfterDownload = false;

uint32_t lastWifiAttempt = 0;
uint32_t lastSocketAttempt = 0;
uint32_t lastFrameAt = 0;
uint32_t liveSequence = 0;

File downloadFile;
String downloadId;
uint32_t downloadSize = 0;
uint32_t downloadSent = 0;
uint32_t downloadSequence = 0;
uint8_t downloadPacket[PACKET_HEADER_SIZE + DOWNLOAD_CHUNK_SIZE];

File recordingFile;
String recordingPath;
uint32_t recordingStartedAt = 0;
uint32_t recordingFrames = 0;
uint32_t recordingBytes = 0;

void writePacketHeader(uint8_t *packet, uint8_t type, uint32_t sequence, uint8_t flags) {
  packet[0] = 'R';
  packet[1] = 'C';
  packet[2] = 'A';
  packet[3] = 'M';
  packet[4] = type;
  packet[5] = (sequence >> 24) & 0xff;
  packet[6] = (sequence >> 16) & 0xff;
  packet[7] = (sequence >> 8) & 0xff;
  packet[8] = sequence & 0xff;
  packet[9] = flags;
  packet[10] = 0;
  packet[11] = 0;
}

void sendJson(JsonDocument &document) {
  if (!socketClient.available()) return;
  String output;
  output.reserve(1024);
  serializeJson(document, output);
  socketClient.send(output);
}

void sendError(const String &message, const String &id = String()) {
  DynamicJsonDocument document(2048);
  document["type"] = "error";
  document["message"] = message;
  if (id.length()) document["id"] = id;
  sendJson(document);
}

void sendStreamState() {
  DynamicJsonDocument document(1024);
  document["type"] = "stream_state";
  document["active"] = streaming;
  sendJson(document);
}

void sendRecordState() {
  DynamicJsonDocument document(1024);
  document["type"] = "record_state";
  document["active"] = recording;
  if (recording) document["file"] = recordingPath;
  sendJson(document);
}

void sendState() {
  DynamicJsonDocument document(2048);
  document["type"] = "state";
  document["online"] = true;
  document["board"] = BOARD_ID;
  document["ip"] = WiFi.localIP().toString();
  document["camera"] = cameraReady;
  document["sd"] = sdReady;
  document["streaming"] = streaming;
  document["recording"] = recording;
  document["download"] = downloadActive;
  sendJson(document);
}

String safePath(String value) {
  value.trim();
  value.replace('\\', '/');
  if (!value.length()) return String();

  String result;
  int start = 0;
  uint8_t parts = 0;
  while (start < value.length()) {
    int end = value.indexOf('/', start);
    if (end < 0) end = value.length();
    String part = value.substring(start, end);
    if (!part.length() || part == "." || part == ".." || part.indexOf('\0') >= 0) return String();
    result += "/";
    result += part;
    if (++parts > 4) return String();
    start = end + 1;
  }
  return result;
}

bool hasFreeSpace(uint32_t bytes) {
  if (!sdReady) return false;
  uint64_t total = SD_MMC.totalBytes();
  uint64_t used = SD_MMC.usedBytes();
  return total > used && (total - used) > bytes;
}

bool initCamera() {
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  const bool hasPsram = psramFound();
  config.frame_size = hasPsram ? FRAMESIZE_VGA : FRAMESIZE_QVGA;
  config.jpeg_quality = hasPsram ? 12 : 14;
  config.fb_count = hasPsram ? 2 : 1;
  config.fb_location = hasPsram ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

  esp_err_t error = esp_camera_init(&config);
  if (error != ESP_OK) {
    Serial.printf("Error de camara: 0x%x\n", error);
    return false;
  }
  return true;
}

bool initSdCard() {
  // La placa comparte lineas entre camara, SD y flash; usar solo D0 evita conflictos.
  SD_MMC.setPins(SD_CLK_GPIO, SD_CMD_GPIO, SD_D0_GPIO);
  if (!SD_MMC.begin("/sdcard", true)) {
    Serial.println("No se pudo montar la MicroSD en 1-bit");
    return false;
  }
  if (!SD_MMC.exists("/captures")) SD_MMC.mkdir("/captures");
  return true;
}

void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  const uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 20000) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi no disponible; se reintentara");
  }
}

bool connectSocket() {
  if (WiFi.status() != WL_CONNECTED || socketClient.available()) return socketClient.available();
  if (millis() - lastSocketAttempt < WS_RETRY_MS) return false;
  lastSocketAttempt = millis();

  String url = WS_URL;
  const bool tls = url.startsWith("wss://");
  const uint8_t prefixLength = tls ? 6 : 5;
  String address = url.substring(prefixLength);
  int slash = address.indexOf('/');
  String host = slash >= 0 ? address.substring(0, slash) : address;
  String path = slash >= 0 ? address.substring(slash) : "/";
  int colon = host.indexOf(':');
  uint16_t port = tls ? 443 : 80;
  if (colon >= 0) {
    port = static_cast<uint16_t>(host.substring(colon + 1).toInt());
    host = host.substring(0, colon);
  }

  Serial.printf("Conectando WebSocket a %s:%u%s\n", host.c_str(), port, path.c_str());
  // Render usa TLS. ArduinoWebsockets requiere connectSecure para wss://.
  // setInsecure evita tener que incrustar una CA que puede rotar; para un
  // despliegue de alta seguridad sustituye esto por socketClient.setCACert().
  socketClient.setInsecure();
  const bool connected = tls
    ? socketClient.connectSecure(host.c_str(), port, path.c_str())
    : socketClient.connect(host.c_str(), port, path.c_str());
  if (!connected) {
    Serial.println("Fallo de WebSocket");
    return false;
  }
  sendState();
  return true;
}

void sendLiveFrame(const uint8_t *jpeg, size_t jpegSize) {
  if (!socketClient.available()) return;
  const size_t packetSize = PACKET_HEADER_SIZE + jpegSize;
  uint8_t *packet = static_cast<uint8_t *>(psramFound() ? ps_malloc(packetSize) : malloc(packetSize));
  if (!packet) {
    Serial.println("Sin memoria para frame");
    return;
  }
  writePacketHeader(packet, 0, liveSequence++, 0);
  memcpy(packet + PACKET_HEADER_SIZE, jpeg, jpegSize);
  socketClient.sendBinary(reinterpret_cast<const char *>(packet), packetSize);
  free(packet);
}

void finishDownload(bool success, const String &error = String()) {
  if (!downloadActive) return;
  downloadActive = false;
  if (downloadFile) downloadFile.close();

  DynamicJsonDocument document(1024);
  document["type"] = "file_end";
  document["id"] = downloadId;
  document["ok"] = success;
  if (error.length()) document["error"] = error;
  sendJson(document);

  const bool shouldResume = resumeStreamingAfterDownload;
  resumeStreamingAfterDownload = false;
  downloadId = String();
  downloadSize = 0;
  downloadSent = 0;
  downloadSequence = 0;
  if (shouldResume) {
    streaming = true;
    sendStreamState();
  }
}

void startDownload(const String &requestedFile, String id) {
  if (downloadActive) {
    sendError("download_busy", id);
    return;
  }
  if (!sdReady) {
    sendError("sd_unavailable", id);
    return;
  }
  String filePath = safePath(requestedFile);
  if (!filePath.length()) {
    sendError("invalid_file", id);
    return;
  }
  File file = SD_MMC.open(filePath.c_str(), FILE_READ);
  if (!file || file.isDirectory()) {
    if (file) file.close();
    sendError("file_not_found", id);
    return;
  }

  if (recording) stopRecording();
  resumeStreamingAfterDownload = streaming;
  streaming = false;
  sendStreamState();

  downloadFile = file;
  downloadId = id.length() ? id : String(millis());
  downloadSize = file.size();
  downloadSent = 0;
  downloadSequence = 0;
  downloadAbort = false;
  downloadActive = true;

  DynamicJsonDocument document(1024);
  document["type"] = "file_start";
  document["id"] = downloadId;
  document["file"] = filePath;
  document["size"] = downloadSize;
  sendJson(document);
}

void sendNextDownloadChunk() {
  if (!downloadActive) return;
  if (downloadAbort) return finishDownload(false, "aborted");
  if (!socketClient.available()) return finishDownload(false, "connection_lost");

  const uint32_t remaining = downloadSize - downloadSent;
  const size_t requested = remaining > DOWNLOAD_CHUNK_SIZE ? DOWNLOAD_CHUNK_SIZE : remaining;
  const size_t readBytes = requested ? downloadFile.read(downloadPacket + PACKET_HEADER_SIZE, requested) : 0;
  if (readBytes == 0 && remaining > 0) return finishDownload(false, "sd_read_failed");

  downloadSent += readBytes;
  const bool last = downloadSent >= downloadSize;
  writePacketHeader(downloadPacket, 1, downloadSequence++, last ? 1 : 0);
  socketClient.sendBinary(reinterpret_cast<const char *>(downloadPacket), PACKET_HEADER_SIZE + readBytes);
  if (last) finishDownload(true);
}

void sendFileList() {
  if (!sdReady) {
    sendError("sd_unavailable");
    return;
  }

  DynamicJsonDocument document(8192);
  document["type"] = "files";
  JsonArray files = document["files"].to<JsonArray>();
  File directory = SD_MMC.open("/captures");
  if (!directory || !directory.isDirectory()) {
    if (directory) directory.close();
    sendError("captures_directory_missing");
    return;
  }

  uint8_t count = 0;
  while (count < 80) {
    File file = directory.openNextFile();
    if (!file) break;
    if (!file.isDirectory()) {
      JsonObject item = files.add<JsonObject>();
      String name = file.name();
      if (!name.startsWith("/")) name = String("/captures/") + name;
      item["name"] = name;
      item["size"] = file.size();
      count++;
    }
    file.close();
  }
  directory.close();
  sendJson(document);
}

void startRecording() {
  if (recording) return;
  if (!sdReady || !hasFreeSpace(RECORD_MIN_FREE_BYTES)) {
    sendError("sd_space_unavailable");
    return;
  }
  recordingPath = String("/captures/rec_") + String(millis()) + ".mjpeg";
  recordingFile = SD_MMC.open(recordingPath.c_str(), FILE_WRITE);
  if (!recordingFile) {
    sendError("record_open_failed");
    return;
  }
  recording = true;
  recordingStartedAt = millis();
  recordingFrames = 0;
  recordingBytes = 0;
  sendRecordState();
}

void stopRecording() {
  if (!recording) return;
  recordingFile.flush();
  recordingFile.close();
  recording = false;
  sendRecordState();
  sendFileList();
}

void deleteFile(const String &requestedFile) {
  String filePath = safePath(requestedFile);
  if (!filePath.length() || !sdReady || !SD_MMC.remove(filePath.c_str())) {
    sendError("delete_failed");
    return;
  }
  sendFileList();
}

void captureFrame() {
  if (!cameraReady) return;
  camera_fb_t *frame = esp_camera_fb_get();
  if (!frame) {
    Serial.println("No se pudo capturar frame");
    return;
  }

  if (recording) {
    recordingFile.write(frame->buf, frame->len);
    recordingBytes += frame->len;
    recordingFrames++;
    if ((recordingFrames % 10) == 0) recordingFile.flush();
    if (millis() - recordingStartedAt >= RECORD_MAX_MS || !hasFreeSpace(RECORD_MIN_FREE_BYTES)) stopRecording();
  }
  if (streaming) sendLiveFrame(frame->buf, frame->len);
  esp_camera_fb_return(frame);
}

void handleCommand(const String &text) {
  DynamicJsonDocument document(2048);
  if (deserializeJson(document, text)) {
    sendError("invalid_json");
    return;
  }

  String command = document["cmd"] | "";
  String id = document["id"] | "";
  if (command == "get_state") {
    sendState();
  } else if (command == "start_stream") {
    if (!cameraReady) return sendError("camera_unavailable", id);
    streaming = true;
    if (downloadActive) resumeStreamingAfterDownload = true;
    sendStreamState();
  } else if (command == "stop_stream") {
    streaming = false;
    resumeStreamingAfterDownload = false;
    sendStreamState();
  } else if (command == "list_files") {
    sendFileList();
  } else if (command == "download") {
    startDownload(document["file"] | "", id);
  } else if (command == "abort_download") {
    if (downloadActive && (!id.length() || id == downloadId)) downloadAbort = true;
  } else if (command == "start_record") {
    startRecording();
  } else if (command == "stop_record") {
    stopRecording();
  } else if (command == "delete") {
    deleteFile(document["file"] | "");
  } else {
    sendError("unknown_command", id);
  }
}

void onSocketMessage(WebsocketsMessage message) {
  if (message.isText()) handleCommand(message.data());
}

void onSocketEvent(WebsocketsEvent event, String data) {
  if (event == WebsocketsEvent::ConnectionOpened) Serial.println("WebSocket conectado");
  if (event == WebsocketsEvent::ConnectionClosed) Serial.println("WebSocket cerrado");
  if (event == WebsocketsEvent::GotPing) Serial.println("WebSocket ping");
  (void)data;
}

void maintainWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastWifiAttempt < WIFI_RETRY_MS) return;
  lastWifiAttempt = millis();
  Serial.println("Reintentando WiFi");
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\nCentinela ESP32-CAM relay");

  sdReady = initSdCard();
  cameraReady = initCamera();
  Serial.printf("Camara: %s | SD 1-bit: %s | PSRAM: %s\n", cameraReady ? "OK" : "ERROR", sdReady ? "OK" : "ERROR", psramFound() ? "OK" : "NO");

  socketClient.onMessage(onSocketMessage);
  socketClient.onEvent(onSocketEvent);
  socketClient.enableHeartbeat(20000);
  connectWifi();
}

void loop() {
  maintainWifi();
  if (!socketClient.available()) {
    connectSocket();
    delay(10);
    return;
  }

  socketClient.poll();
  if (downloadActive) {
    for (uint8_t i = 0; i < MAX_CHUNKS_PER_LOOP && downloadActive; i++) {
      sendNextDownloadChunk();
      socketClient.poll();
    }
    return;
  }

  const uint32_t now = millis();
  if ((streaming || recording) && now - lastFrameAt >= FRAME_INTERVAL_MS) {
    lastFrameAt = now;
    captureFrame();
  }
  delay(1);
}
