# Centinela Render Relay

En Render usa **Web Service**, con `render-relay/` como Root Directory, `npm install` como Build Command y `npm start` como Start Command.

Configura en el firmware `WIFI_SSID`, `WIFI_PASSWORD` y `WS_URL`:

```text
wss://TU-SERVICIO.onrender.com/ws?role=device
```

La placa usa SD_MMC en 1-bit y guarda grabaciones MJPEG en `/captures/*.mjpeg`. El protocolo binario usa `RCAM` + 12 bytes de cabecera: tipo `0` para JPEG live y tipo `1` para chunks; `flags & 1` indica el último chunk.
