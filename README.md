# Viernes-Home

**Viernes** es un asistente de voz casero tipo JARVIS/FRIDAY, en español latino.
Corre sobre un **ESP32-S3** y controla aparatos de la casa (luces, ventilador) mediante relés.

> Proyecto de aprendizaje, hecho por etapas.

## ¿Qué es este repositorio?

Este repositorio guarda **solo la configuración** de Viernes. El ESP32 tiene poco espacio,
así que al arrancar (y cada cierto tiempo) descarga estos archivos por WiFi:

| Archivo | Para qué sirve |
|---|---|
| `config/dispositivos.json` | Lista de aparatos: nombre, alias, pin, tipo y si se activan en alto o bajo |
| `config/ajustes.json` | Idioma, proveedor de IA, modelo, volumen y versión de la configuración |
| `config/personalidad.md` | Cómo habla Viernes: tono, español latino, respuestas cortas |
| `docs/notas.md` | Apuntes del proyecto |

## Hardware

- ESP32-S3 (ESP32-S3-WROOM-1): el cerebro.
- ESP32-C3 Super Mini: nodo secundario para controlar aparatos.
- Relés para luces y ventilador.
- Pendiente: micrófono INMP441 y amplificador MAX98357A.

## Seguridad (importante)

- **Este repositorio es público: aquí NO va ninguna clave, contraseña ni token.**
- La API key de la IA y las credenciales WiFi se guardan en la memoria NVS del ESP32.
- La API key debe tener un límite de gasto configurado en el proveedor.

## Estado

- [x] LED de prueba en el GPIO 4
- [x] Portal web de configuración WiFi (punto de acceso `ESP32-Config`)
- [ ] Descarga de configuración desde GitHub
- [ ] Voz desde la PC con Windows
- [ ] Audio directo en el ESP32
