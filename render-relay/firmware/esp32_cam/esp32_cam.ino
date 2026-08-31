/* AI-Thinker ESP32-CAM relay para Render.
   Librerias: ArduinoJson y ArduinoWebsockets de Gil Maimon. */
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "FS.h"
#include "SD_MMC.h"
#include "WiFi.h"
#include <ArduinoJson.h>
#include <ArduinoWebsockets.h>

using namespace websockets;

#define PWDN_GPIO_NUM 32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 0
#define SIOD_GPIO_NUM 26
#define SIOC_GPIO_NUM 27
#define Y9_GPIO_NUM 35
#define Y8_GPIO_NUM 34
#define Y7_GPIO_NUM 39
#define Y6_GPIO_NUM 36
#define Y5_GPIO_NUM 21
#define Y4_GPIO_NUM 19
#define Y3_GPIO_NUM 18
#define Y2_GPIO_NUM 5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM 23
#define PCLK_GPIO_NUM 22

// SD_MMC 1-bit en AI-Thinker: CLK 14, CMD 15, D0 2.
#define SD_CLK_GPIO 14
#define SD_CMD_GPIO 15
#define SD_D0_GPIO 2
#define HEADER_SIZE 12
#define CHUNK_SIZE 2048

const char *WIFI_SSID = "CAMBIA_SSID";
const char *WIFI_PASSWORD = "CAMBIA_PASSWORD";
const char *WS_URL = "wss://TU-APP.onrender.com/ws?role=device";
const char *BOARD_ID = "esp32cam-01";

WebsocketsClient ws;
bool cameraReady = false, sdReady = false, streaming = false, recording = false;
bool downloading = false, abortDownload = false, resumeStream = false;
uint32_t lastWifi = 0, lastWs = 0, lastFrame = 0, liveSeq = 0;
File downloadFile, recordFile;
String downloadId, recordPath;
uint32_t downloadSize = 0, downloadSent = 0, downloadSeq = 0;
uint32_t recordStarted = 0, recordFrames = 0;
uint8_t packet[HEADER_SIZE + CHUNK_SIZE];

void stopRecording();
void sendFileList();

void header(uint8_t *p, uint8_t type, uint32_t seq, uint8_t flags) {
  p[0]='R'; p[1]='C'; p[2]='A'; p[3]='M'; p[4]=type;
  p[5]=seq>>24; p[6]=seq>>16; p[7]=seq>>8; p[8]=seq; p[9]=flags; p[10]=0; p[11]=0;
}

void sendJson(JsonDocument &doc) {
  if (!ws.available()) return;
  String out; out.reserve(1024); serializeJson(doc, out); ws.send(out);
}

void errorMessage(const String &message, const String &id = String()) {
  DynamicJsonDocument doc(1024); doc["type"]="error"; doc["message"]=message;
  if (id.length()) doc["id"]=id; sendJson(doc);
}

void streamState() {
  DynamicJsonDocument doc(512); doc["type"]="stream_state"; doc["active"]=streaming; sendJson(doc);
}

void recordState() {
  DynamicJsonDocument doc(768); doc["type"]="record_state"; doc["active"]=recording;
  if (recording) doc["file"]=recordPath; sendJson(doc);
}

void deviceState() {
  DynamicJsonDocument doc(1024); doc["type"]="state"; doc["online"]=true; doc["board"]=BOARD_ID;
  doc["ip"]=WiFi.localIP().toString(); doc["camera"]=cameraReady; doc["sd"]=sdReady;
  doc["streaming"]=streaming; doc["recording"]=recording; doc["download"]=downloading; sendJson(doc);
}

String safePath(String value) {
  value.trim(); value.replace('\\','/'); if (!value.length()) return String();
  String result; int start=0; uint8_t count=0;
  while (start < value.length()) {
    int end=value.indexOf('/',start); if (end<0) end=value.length();
    String part=value.substring(start,end);
    if (!part.length() || part=="." || part==".." || part.indexOf('\0')>=0) return String();
    result += "/"; result += part; if (++count>4) return String(); start=end+1;
  }
  return result;
}

bool cameraInit() {
  camera_config_t c={}; c.ledc_channel=LEDC_CHANNEL_0; c.ledc_timer=LEDC_TIMER_0;
  c.pin_d0=Y2_GPIO_NUM; c.pin_d1=Y3_GPIO_NUM; c.pin_d2=Y4_GPIO_NUM; c.pin_d3=Y5_GPIO_NUM;
  c.pin_d4=Y6_GPIO_NUM; c.pin_d5=Y7_GPIO_NUM; c.pin_d6=Y8_GPIO_NUM; c.pin_d7=Y9_GPIO_NUM;
  c.pin_xclk=XCLK_GPIO_NUM; c.pin_pclk=PCLK_GPIO_NUM; c.pin_vsync=VSYNC_GPIO_NUM; c.pin_href=HREF_GPIO_NUM;
  c.pin_sccb_sda=SIOD_GPIO_NUM; c.pin_sccb_scl=SIOC_GPIO_NUM; c.pin_pwdn=PWDN_GPIO_NUM; c.pin_reset=RESET_GPIO_NUM;
  c.xclk_freq_hz=20000000; c.pixel_format=PIXFORMAT_JPEG;
  bool psram=psramFound(); c.frame_size=psram?FRAMESIZE_VGA:FRAMESIZE_QVGA; c.jpeg_quality=psram?12:14;
  c.fb_count=psram?2:1; c.fb_location=psram?CAMERA_FB_IN_PSRAM:CAMERA_FB_IN_DRAM; c.grab_mode=CAMERA_GRAB_WHEN_EMPTY;
  esp_err_t e=esp_camera_init(&c); if (e!=ESP_OK) { Serial.printf("Camera error 0x%x\n",e); return false; } return true;
}

bool sdInit() {
  SD_MMC.setPins(SD_CLK_GPIO, SD_CMD_GPIO, SD_D0_GPIO);
  if (!SD_MMC.begin("/sdcard", true)) return false;
  if (!SD_MMC.exists("/captures")) SD_MMC.mkdir("/captures"); return true;
}

void wifiConnect() {
  WiFi.mode(WIFI_STA); WiFi.begin(WIFI_SSID,WIFI_PASSWORD); uint32_t start=millis();
  while (WiFi.status()!=WL_CONNECTED && millis()-start<20000) { delay(250); Serial.print('.'); }
  Serial.println(); if (WiFi.status()==WL_CONNECTED) Serial.println(WiFi.localIP());
}

bool socketConnect() {
  if (WiFi.status()!=WL_CONNECTED || ws.available()) return ws.available();
  if (millis()-lastWs<5000) return false; lastWs=millis(); String url=WS_URL;
  bool tls=url.startsWith("wss://"); String address=url.substring(tls?6:5); int slash=address.indexOf('/');
  String host=slash>=0?address.substring(0,slash):address; String path=slash>=0?address.substring(slash):"/";
  int colon=host.indexOf(':'); uint16_t port=tls?443:80; if (colon>=0) { port=host.substring(colon+1).toInt(); host=host.substring(0,colon); }
  // Para produccion reemplaza setInsecure() por setCACert() con la CA de Render.
  ws.setInsecure(); bool ok=tls?ws.connectSecure(host.c_str(),port,path.c_str()):ws.connect(host.c_str(),port,path.c_str());
  if (ok) deviceState(); return ok;
}

void liveFrame(const uint8_t *data, size_t size) {
  size_t total=HEADER_SIZE+size; uint8_t *out=(uint8_t *)(psramFound()?ps_malloc(total):malloc(total)); if (!out) return;
  header(out,0,liveSeq++,0); memcpy(out+HEADER_SIZE,data,size); ws.sendBinary((const char *)out,total); free(out);
}

void finishDownload(bool ok, const String &reason=String()) {
  if (!downloading) return; downloading=false; if (downloadFile) downloadFile.close();
  DynamicJsonDocument doc(1024); doc["type"]="file_end"; doc["id"]=downloadId; doc["ok"]=ok; if(reason.length()) doc["error"]=reason; sendJson(doc);
  bool resume=resumeStream; resumeStream=false; downloadId=""; downloadSize=downloadSent=downloadSeq=0; if(resume){streaming=true;streamState();}
}

void startDownload(String requested, String id) {
  if (downloading) return errorMessage("download_busy",id); if (!sdReady) return errorMessage("sd_unavailable",id);
  String filePath=safePath(requested); if (!filePath.length()) return errorMessage("invalid_file",id);
  File f=SD_MMC.open(filePath.c_str(),FILE_READ); if(!f||f.isDirectory()){if(f)f.close();return errorMessage("file_not_found",id);}
  if(recording) stopRecording(); resumeStream=streaming; streaming=false; streamState(); downloadFile=f; downloadId=id.length()?id:String(millis());
  downloadSize=f.size(); downloadSent=downloadSeq=0; abortDownload=false; downloading=true;
  DynamicJsonDocument doc(1024); doc["type"]="file_start"; doc["id"]=downloadId; doc["file"]=filePath; doc["size"]=downloadSize; sendJson(doc);
}

void downloadChunk() {
  if(!downloading)return; if(abortDownload)return finishDownload(false,"aborted"); if(!ws.available())return finishDownload(false,"connection_lost");
  uint32_t remaining=downloadSize-downloadSent; size_t wanted=remaining>CHUNK_SIZE?CHUNK_SIZE:remaining; size_t n=wanted?downloadFile.read(packet+HEADER_SIZE,wanted):0;
  if(!n&&remaining)return finishDownload(false,"sd_read_failed"); downloadSent+=n; bool last=downloadSent>=downloadSize; header(packet,1,downloadSeq++,last?1:0); ws.sendBinary((const char *)packet,HEADER_SIZE+n); if(last)finishDownload(true);
}

void sendFileList() {
  if(!sdReady)return errorMessage("sd_unavailable"); DynamicJsonDocument doc(8192); doc["type"]="files"; JsonArray list=doc["files"].to<JsonArray>();
  File dir=SD_MMC.open("/captures"); if(!dir||!dir.isDirectory()){if(dir)dir.close();return errorMessage("captures_directory_missing");}
  uint8_t n=0; while(n<80){File f=dir.openNextFile();if(!f)break;if(!f.isDirectory()){JsonObject x=list.add<JsonObject>();String name=f.name();if(!name.startsWith("/"))name=String("/captures/")+name;x["name"]=name;x["size"]=f.size();n++;}f.close();}dir.close();sendJson(doc);
}

void startRecording() {
  if(recording)return; if(!sdReady)return errorMessage("sd_unavailable"); recordPath="/captures/rec_"+String(millis())+".mjpeg"; recordFile=SD_MMC.open(recordPath.c_str(),FILE_WRITE);
  if(!recordFile)return errorMessage("record_open_failed"); recording=true;recordStarted=millis();recordFrames=0;recordState();
}
void stopRecording() { if(!recording)return;recordFile.flush();recordFile.close();recording=false;recordState();sendFileList(); }

void capture() {
  camera_fb_t *f=esp_camera_fb_get(); if(!f)return; if(recording){recordFile.write(f->buf,f->len);recordFrames++;if(!(recordFrames%10))recordFile.flush();if(millis()-recordStarted>900000)stopRecording();}if(streaming)liveFrame(f->buf,f->len);esp_camera_fb_return(f);
}

void command(String raw) {
  DynamicJsonDocument doc(4096); if(deserializeJson(doc,raw))return; String cmd=doc["cmd"]|"";String id=doc["id"]|"";
  if(cmd=="get_state")deviceState();else if(cmd=="start_stream"){if(!cameraReady)return errorMessage("camera_unavailable",id);streaming=true;if(downloading)resumeStream=true;streamState();}
  else if(cmd=="stop_stream"){streaming=false;resumeStream=false;streamState();}else if(cmd=="list_files")sendFileList();else if(cmd=="download")startDownload(doc["file"]|"",id);
  else if(cmd=="abort_download"&&downloading&&(!id.length()||id==downloadId))abortDownload=true;else if(cmd=="start_record")startRecording();else if(cmd=="stop_record")stopRecording();else if(cmd=="delete"){String p=safePath(doc["file"]|"");if(p.length()&&!SD_MMC.remove(p.c_str()))errorMessage("delete_failed");sendFileList();}else errorMessage("unknown_command",id);
}

void onMessage(WebsocketsMessage m){if(m.isText())command(m.data());}
void onEvent(WebsocketsEvent e,String data){if(e==WebsocketsEvent::ConnectionOpened)Serial.println("WS conectado");if(e==WebsocketsEvent::ConnectionClosed)Serial.println("WS cerrado");(void)data;}

void setup(){Serial.begin(115200);delay(500);sdReady=sdInit();cameraReady=cameraInit();ws.onMessage(onMessage);ws.onEvent(onEvent);ws.enableHeartbeat(20000);wifiConnect();}
void loop(){if(WiFi.status()!=WL_CONNECTED&&millis()-lastWifi>10000){lastWifi=millis();WiFi.reconnect();}if(!ws.available()){socketConnect();delay(10);return;}ws.poll();if(downloading){for(uint8_t i=0;i<4&&downloading;i++){downloadChunk();ws.poll();}return;}if((streaming||recording)&&cameraReady&&millis()-lastFrame>=100){lastFrame=millis();capture();}delay(1);}
