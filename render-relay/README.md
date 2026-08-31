# Centinela Render Relay

En Render usa **Web Service**, con `render-relay/` como Root Directory, `npm install` como Build Command y `npm start` como Start Command.

Define estas variables de entorno en Render:

```text
ADMIN_USER=admin
ADMIN_PASSWORD=una-clave-larga
DEVICE_TOKEN=un-token-largo-para-la-placa
```

El usuario y la contraseña protegen el dashboard, las API, WebSocket de navegador y descargas. El `DEVICE_TOKEN` autentica la conexión de la ESP32.

Configura en el firmware `WIFI_SSID`, `WIFI_PASSWORD` y `WS_URL`:

```text
wss://TU-SERVICIO.onrender.com/ws?role=device&token=un-token-largo-para-la-placa
```

La placa usa SD_MMC en 1-bit y guarda grabaciones MJPEG en `/captures/*.mjpeg`. El protocolo binario usa `RCAM` + 12 bytes de cabecera: tipo `0` para JPEG live y tipo `1` para chunks; `flags & 1` indica el último chunk.
