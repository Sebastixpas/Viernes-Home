# Notas del proyecto Viernes

## Decisiones
- Cerebro: ESP32-S3 (ESP32-S3-WROOM-1, placa con dos USB-C).
- Nodo secundario: ESP32-C3 Super Mini (más adelante).
- Voz por ahora: desde la PC con Windows. Sin micrófono en el ESP32 todavía.
- Entrada física: botón que se queda hundido (interruptor de escucha) en el GPIO 5.
- La configuración vive en este repositorio y el ESP32 la descarga por WiFi.
- La API key y el WiFi se guardan en la memoria NVS del ESP32, nunca aquí.

## Estados del ESP32 al arrancar
1. Configuración: sin WiFi guardado, crea la red `Viernes-Setup`.
2. Completo: WiFi + internet, descarga la config de GitHub.
3. Local: WiFi sin internet, usa la última copia guardada.
4. Reconexión: si se cae el WiFi, reintenta cada minuto.

## Pines del ESP32-S3 (WROOM-1)
**Permitidos:** 1, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 21, 38, 39, 40, 41, 42

**Evitar:**
- 0, 3, 45, 46: pines de arranque (strapping).
- 19, 20: USB.
- 26 a 32: memoria flash.
- 33 a 37: pueden usarse por la PSRAM.
- 43, 44: puerto serie (consola).
- 48: suele ser el LED RGB de la placa.

## Pendiente de comprar
- Micrófono INMP441.
- Amplificador MAX98357A.
- Módulo de relés.

## URLs raw
Base: `https://raw.githubusercontent.com/Sebastixpas/Viernes-Home/main/`
- `config/dispositivos.json`
- `config/ajustes.json`
- `config/personalidad.md`
