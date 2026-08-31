# Centinela Render Relay

Relay para una AI-Thinker ESP32-CAM detrás de CGNAT. La placa abre una conexión saliente al relay; el navegador nunca necesita conectarse directamente a la red local.

## Render

Configura un **Web Service** apuntando a `render-relay/`:

- Build command: `npm install`
- Start command: `npm start`
- Environment: Node 18 o superior

La URL que se usa en el firmware es:

```text
wss://TU-SERVICIO.onrender.com/ws?role=device
```

Abre `https://TU-SERVICIO.onrender.com` para el dashboard. El endpoint de descarga es `GET /api/download/<archivo>` y mantiene la respuesta HTTP abierta mientras recibe los chunks de la ESP32.

## ESP32-CAM

1. Instala `ArduinoJson` y `ArduinoWebsockets` de Gil Maimon.
2. Abre `firmware/esp32_cam/esp32_cam.ino`.
3. Cambia `WIFI_SSID`, `WIFI_PASSWORD` y `WS_URL`.
4. Selecciona **AI Thinker ESP32-CAM** y habilita PSRAM.
5. Carga el firmware con GPIO0 conectado a GND y después retíralo para arrancar.

La SD se monta obligatoriamente con `SD_MMC.begin("/sdcard", true)` y los pines 14/15/2 en 1-bit. Las grabaciones se guardan como MJPEG crudo en `/captures/*.mjpeg`; el visor web extrae los frames JPEG sin necesitar un códec externo.

El ejemplo usa `setInsecure()` para simplificar la validación TLS de Render. El tráfico sigue cifrado, pero una instalación expuesta a Internet debería reemplazarlo por `setCACert()` con la CA de confianza.

## Protocolo binario

Cada mensaje binario empieza por una cabecera de 12 bytes:

```text
RCAM | tipo:1 | secuencia:uint32 big-endian | flags:1 | reservado:uint16 | payload
```

- Tipo `0`: payload JPEG de streaming.
- Tipo `1`: payload de archivo; `flags & 1` indica el último chunk.

El servidor solo permite rutas de archivos sin `..` y extensiones de vídeo/imagen conocidas.
