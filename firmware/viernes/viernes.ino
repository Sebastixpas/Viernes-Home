/*
  ============================================================
  VIERNES - Firmware v0.4  (ESP32-S3)
  ============================================================
  - Arranque con 3 estados: Configuracion / Completo / Local.
  - Panel web con LOGIN (clave guardada en el ESP32).
  - Conexion al WiFi con diagnostico (dice si la clave esta mal,
    si no encuentra la red, etc.) y con boton "ver clave".
  - Configuracion descargada de GitHub y guardada como copia local.
  - Todo lo que configuras (WiFi, claves, silencio) queda guardado
    en la memoria NVS y NO se pierde al reiniciar.

  SEGURIDAD: aqui NO hay claves. La clave inicial del panel esta en
  secrets.h (ese archivo NO se sube a GitHub).

  Libreria necesaria: ArduinoJson v7. Placa: "ESP32S3 Dev Module".
  ============================================================
*/

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <vector>
#include "mbedtls/base64.h"

#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef CLAVE_PANEL
#define CLAVE_PANEL "cambiame"
#endif

// ---------------- AJUSTES DEL PROYECTO (publicos, sin claves) ----------------
const char* GH_USUARIO = "Sebastixpas";
const char* GH_REPO    = "Viernes-Home";
const char* GH_RAMA    = "main";
const char* AP_NOMBRE  = "Viernes-Setup";
const char* NOMBRE_MDNS = "viernes";
const int   PIN_BOTON_DEFECTO = 5;
const unsigned long TIEMPO_WIFI_MS = 15000;
const int   INTENTOS_WIFI = 3;

// ---------------- ESTADO GENERAL ----------------
enum Modo { MODO_CONFIG, MODO_COMPLETO, MODO_LOCAL };
Modo modo = MODO_CONFIG;

Preferences prefs;
WebServer server(80);
DNSServer dns;

struct Disp {
  String id, nombre, tipo, desc;
  int pin = -1;
  bool activoAlto = true;
  bool habilitado = true;
  bool prox = false;
  bool estado = false;
};
std::vector<Disp> disps;
std::vector<int> pinesPermitidos;

String ajIdioma = "es-419", ajProveedor = "anthropic", ajModelo = "claude-haiku-4-5";
String ajAudio = "PC", ajIndicador = "luz_cuarto";
int ajVolumen = 70, ajSyncMin = 30, versionDisp = 0, versionAj = 0;

bool hayInternet = false;
bool escuchando = false;
bool silenciado = false;
unsigned long ultimaSyncMs = 0;
unsigned long ultimoChequeoMs = 0;
unsigned long ultimoIntentoApMs = 0;
int pinBoton = PIN_BOTON_DEFECTO;
bool botonActivoAlto = false;
bool botLecturaPrev = HIGH, botEstable = HIGH;
unsigned long botCambioMs = 0;

// ----- Sesiones del panel -----
String sesiones[4];
int sesionSig = 0;
int fallosLogin = 0;
unsigned long bloqueoHasta = 0;

// ----- Prueba de conexion WiFi desde el panel -----
enum Probe { P_NINGUNO, P_PROBANDO, P_OK, P_FALLO };
Probe probe = P_NINGUNO;
String probeSsid, probePass, probeMotivo, probeIp;
unsigned long probeIni = 0, probeOkMs = 0;
volatile int ultimaRazon = 0;

// ----- Registro -----
std::vector<String> registro;
void logear(const String& t) {
  Serial.println(t);
  registro.push_back(t);
  if (registro.size() > 25) registro.erase(registro.begin());
}

// ---------------- UTILIDADES ----------------
String urlRaw(const char* ruta) {
  return String("https://raw.githubusercontent.com/") + GH_USUARIO + "/" + GH_REPO + "/" + GH_RAMA + "/" + ruta;
}

bool pinPermitido(int pin) {
  if (pinesPermitidos.empty()) return true;
  for (int p : pinesPermitidos) if (p == pin) return true;
  return false;
}

void escribirDisp(Disp& d, bool on) {
  d.estado = on;
  if (d.pin >= 0 && d.tipo != "boton" && d.habilitado) digitalWrite(d.pin, (on == d.activoAlto) ? HIGH : LOW);
}

Disp* buscarDisp(const String& id) {
  for (auto& d : disps) if (d.id == id) return &d;
  return nullptr;
}

String claveActual() { return prefs.getString("clave", CLAVE_PANEL); }

// ---------------- CONFIGURACION (JSON) ----------------
const char* DISP_POR_DEFECTO =
  "{\"version\":0,\"pines_permitidos\":[4,5],\"dispositivos\":["
  "{\"id\":\"luz_cuarto\",\"nombre\":\"Luz del cuarto\",\"pin\":4,\"tipo\":\"led\",\"descripcion\":\"GPIO4 · LED\",\"activo_en\":\"alto\",\"habilitado\":true},"
  "{\"id\":\"boton_escucha\",\"nombre\":\"Boton de escucha\",\"pin\":5,\"tipo\":\"boton\",\"descripcion\":\"GPIO5 · interruptor\",\"activo_en\":\"bajo\",\"habilitado\":true}]}";

bool aplicarDispositivos(const String& json) {
  JsonDocument doc;
  if (deserializeJson(doc, json)) return false;
  if (!doc["dispositivos"].is<JsonArray>()) return false;

  std::vector<int> permitidos;
  for (JsonVariant p : doc["pines_permitidos"].as<JsonArray>()) permitidos.push_back(p.as<int>());
  pinesPermitidos = permitidos;

  std::vector<Disp> nuevos;
  for (JsonObject o : doc["dispositivos"].as<JsonArray>()) {
    Disp d;
    d.id = (const char*)(o["id"] | "");
    d.nombre = (const char*)(o["nombre"] | d.id.c_str());
    d.tipo = (const char*)(o["tipo"] | "led");
    d.desc = (const char*)(o["descripcion"] | "");
    d.pin = o["pin"] | -1;
    d.activoAlto = String((const char*)(o["activo_en"] | "alto")) == "alto";
    d.habilitado = o["habilitado"] | true;
    d.prox = o["proximamente"] | false;
    if (d.id == "") continue;
    if (d.habilitado && !pinPermitido(d.pin)) {
      logear("Pin " + String(d.pin) + " no permitido para " + d.id + ": desactivado");
      d.habilitado = false;
    }
    nuevos.push_back(d);
  }
  if (nuevos.empty()) return false;

  disps = nuevos;
  versionDisp = doc["version"] | 0;

  pinBoton = PIN_BOTON_DEFECTO;
  for (auto& d : disps) {
    if (!d.habilitado || d.pin < 0) continue;
    if (d.tipo == "boton") {
      pinBoton = d.pin;
      botonActivoAlto = d.activoAlto;
      pinMode(d.pin, botonActivoAlto ? INPUT_PULLDOWN : INPUT_PULLUP);
    } else {
      pinMode(d.pin, OUTPUT);
      escribirDisp(d, false);
    }
  }
  return true;
}

bool aplicarAjustes(const String& json) {
  JsonDocument doc;
  if (deserializeJson(doc, json)) return false;
  versionAj = doc["version"] | 0;
  ajIdioma = (const char*)(doc["asistente"]["idioma"] | "es-419");
  ajVolumen = doc["asistente"]["volumen"] | 70;
  ajAudio = (const char*)(doc["asistente"]["audio"] | "PC");
  ajIndicador = (const char*)(doc["asistente"]["indicador_escucha"] | "");
  ajProveedor = (const char*)(doc["ia"]["proveedor"] | "anthropic");
  ajModelo = (const char*)(doc["ia"]["modelo"] | "");
  ajSyncMin = doc["red"]["sincronizar_cada_minutos"] | 30;
  if (ajSyncMin < 5) ajSyncMin = 5;
  return true;
}

String leerArchivo(const char* ruta) {
  File f = LittleFS.open(ruta, "r");
  if (!f) return "";
  String s = f.readString();
  f.close();
  return s;
}

void guardarArchivo(const char* ruta, const String& txt) {
  File f = LittleFS.open(ruta, "w");
  if (!f) return;
  f.print(txt);
  f.close();
}

void cargarCopiaLocal() {
  String d = leerArchivo("/dispositivos.json");
  if (d == "" || !aplicarDispositivos(d)) {
    aplicarDispositivos(DISP_POR_DEFECTO);
    logear("Usando dispositivos por defecto");
  } else {
    logear("Dispositivos cargados de la copia local (v" + String(versionDisp) + ")");
  }
  String a = leerArchivo("/ajustes.json");
  if (a != "") aplicarAjustes(a);
}

// ---------------- INTERNET / GITHUB ----------------
bool descargar(const String& url, String& salida) {
  WiFiClientSecure cliente;
  cliente.setInsecure();
  HTTPClient http;
  http.setTimeout(8000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(cliente, url)) return false;
  int codigo = http.GET();
  bool ok = (codigo == 200);
  if (ok) salida = http.getString();
  else logear("Descarga fallo (" + String(codigo) + ")");
  http.end();
  return ok;
}

void actualizarIndicador() {
  Disp* ind = ajIndicador != "" ? buscarDisp(ajIndicador) : nullptr;
  if (ind && ind->habilitado) escribirDisp(*ind, escuchando && !silenciado);
}

bool sincronizar() {
  if (WiFi.status() != WL_CONNECTED) { hayInternet = false; return false; }
  String d, a;
  if (!descargar(urlRaw("config/dispositivos.json"), d)) { hayInternet = false; return false; }
  hayInternet = true;
  bool okA = descargar(urlRaw("config/ajustes.json"), a);

  if (aplicarDispositivos(d)) guardarArchivo("/dispositivos.json", d);
  else logear("dispositivos.json descargado no es valido");
  if (okA && aplicarAjustes(a)) guardarArchivo("/ajustes.json", a);
  actualizarIndicador();

  ultimaSyncMs = millis();
  logear("Sincronizado con GitHub (config v" + String(versionDisp) + ")");
  return true;
}

String base64(const String& in) {
  size_t outLen = 0;
  size_t cap = ((in.length() + 2) / 3) * 4 + 4;
  unsigned char* buf = (unsigned char*)malloc(cap);
  if (!buf) return "";
  mbedtls_base64_encode(buf, cap, &outLen, (const unsigned char*)in.c_str(), in.length());
  String s((char*)buf, outLen);
  free(buf);
  return s;
}

String guardarEnGitHub() {
  String token = prefs.getString("ghtoken", "");
  if (token == "") return "Falta el token de GitHub (Configuracion)";
  if (WiFi.status() != WL_CONNECTED || !hayInternet) return "Sin internet";

  JsonDocument doc;
  doc["version"] = 1;
  doc["segundos_encendido"] = millis() / 1000;
  JsonArray arr = doc["dispositivos"].to<JsonArray>();
  for (auto& d : disps) {
    JsonObject o = arr.add<JsonObject>();
    o["id"] = d.id;
    o["estado"] = d.estado ? "encendido" : "apagado";
  }
  String contenido;
  serializeJsonPretty(doc, contenido);

  String url = String("https://api.github.com/repos/") + GH_USUARIO + "/" + GH_REPO + "/contents/datos/estado.json";
  String sha = "";
  {
    WiFiClientSecure cliente;
    cliente.setInsecure();
    HTTPClient http;
    http.setTimeout(10000);
    http.begin(cliente, url + "?ref=" + GH_RAMA);
    http.addHeader("Authorization", "Bearer " + token);
    http.addHeader("User-Agent", "Viernes-ESP32");
    http.addHeader("Accept", "application/vnd.github+json");
    int c = http.GET();
    if (c == 200) {
      JsonDocument r;
      if (!deserializeJson(r, http.getString())) sha = (const char*)(r["sha"] | "");
    } else if (c == 401 || c == 403) {
      http.end();
      return "GitHub rechazo el token (" + String(c) + ")";
    }
    http.end();
  }

  JsonDocument cuerpo;
  cuerpo["message"] = "Viernes: actualiza estado de aparatos";
  cuerpo["content"] = base64(contenido);
  cuerpo["branch"] = GH_RAMA;
  if (sha != "") cuerpo["sha"] = sha;
  String cuerpoTxt;
  serializeJson(cuerpo, cuerpoTxt);

  WiFiClientSecure cliente2;
  cliente2.setInsecure();
  HTTPClient http2;
  http2.setTimeout(10000);
  http2.begin(cliente2, url);
  http2.addHeader("Authorization", "Bearer " + token);
  http2.addHeader("User-Agent", "Viernes-ESP32");
  http2.addHeader("Accept", "application/vnd.github+json");
  http2.addHeader("Content-Type", "application/json");
  int c2 = http2.PUT(cuerpoTxt);
  http2.end();
  if (c2 == 200 || c2 == 201) { logear("Estado guardado en GitHub"); return "OK"; }
  return "GitHub respondio " + String(c2);
}

// ---------------- WIFI ----------------
String motivoTexto(int r) {
  switch (r) {
    case 201: return "No encuentro esa red. Revisa el nombre. Recuerda: el ESP32 solo usa WiFi de 2.4 GHz (no 5 GHz).";
    case 202: case 15: case 204: case 2: return "La clave del WiFi es incorrecta.";
    case 203: case 205: return "El router rechazo la conexion (codigo " + String(r) + ").";
    default: return "No se pudo conectar (codigo " + String(r) + ").";
  }
}

bool conectarWiFi() {
  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");
  if (ssid == "") return false;
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < TIEMPO_WIFI_MS) delay(250);
  if (WiFi.status() != WL_CONNECTED && ultimaRazon != 0) logear("WiFi fallo: " + motivoTexto(ultimaRazon));
  return WiFi.status() == WL_CONNECTED;
}

void iniciarAP() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_NOMBRE);
  dns.start(53, "*", WiFi.softAPIP());
  logear("Modo configuracion: red " + String(AP_NOMBRE) + " -> http://192.168.4.1");
}

void iniciarProbe(const String& ssid, const String& pass) {
  probeSsid = ssid;
  probePass = pass;
  probeMotivo = "";
  probeIp = "";
  ultimaRazon = 0;
  probeIni = millis();
  probe = P_PROBANDO;
  WiFi.disconnect(false, true);
  delay(100);
  WiFi.begin(ssid.c_str(), pass.c_str());
  logear("Probando WiFi: " + ssid);
}

void vigilarProbe() {
  if (probe == P_PROBANDO) {
    if (WiFi.status() == WL_CONNECTED) {
      prefs.putString("ssid", probeSsid);
      prefs.putString("pass", probePass);
      probeIp = WiFi.localIP().toString();
      probe = P_OK;
      probeOkMs = millis();
      logear("WiFi OK (" + probeSsid + "): guardado. IP " + probeIp);
    } else if ((ultimaRazon != 0 && millis() - probeIni > 6000) || millis() - probeIni > 25000) {
      probeMotivo = ultimaRazon != 0 ? motivoTexto(ultimaRazon) : "Tiempo agotado: no pude conectarme.";
      probe = P_FALLO;
      logear("WiFi fallo: " + probeMotivo);
      WiFi.disconnect(false, false);
      String viejo = prefs.getString("ssid", "");
      if (viejo != "" && modo != MODO_CONFIG) WiFi.begin(viejo.c_str(), prefs.getString("pass", "").c_str());
    }
  } else if (probe == P_OK && millis() - probeOkMs > 7000) {
    logear("Reiniciando para usar el nuevo WiFi");
    delay(300);
    ESP.restart();
  }
}

// ---------------- SESIONES / LOGIN ----------------
String nuevoToken() {
  char b[33];
  for (int i = 0; i < 4; i++) snprintf(b + i * 8, 9, "%08x", (unsigned int)esp_random());
  return String(b);
}

bool authOk() {
  if (!server.hasHeader("Cookie")) return false;
  String c = server.header("Cookie");
  int i = c.indexOf("vs=");
  if (i < 0) return false;
  String t = c.substring(i + 3, i + 3 + 32);
  if (t.length() != 32) return false;
  for (int k = 0; k < 4; k++) if (sesiones[k] == t) return true;
  return false;
}

#define REQUIERE_LOGIN() if (!authOk()) { server.send(401, "application/json", "{\"ok\":false,\"error\":\"auth\"}"); return; }

// ---------------- PAGINAS ----------------
const char PAGINA_LOGIN[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="es"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Viernes</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:system-ui,Segoe UI,Arial,sans-serif;background:#0d0e12;color:#e8eaf0;min-height:100vh;display:flex;align-items:center;justify-content:center;padding:20px}
.box{background:#161a24;border:1px solid #232938;border-radius:20px;padding:30px;width:100%;max-width:380px}
.logo{width:52px;height:52px;border-radius:50%;background:#3ddbc4;color:#0d0e12;font-weight:700;font-size:24px;display:flex;align-items:center;justify-content:center;margin-bottom:16px}
h1{font-size:24px}p{color:#8b93a7;font-size:14px;margin:6px 0 20px}
.pw{position:relative}
input{width:100%;background:#0d0e12;border:1px solid #2a3144;color:#e8eaf0;border-radius:12px;padding:14px 70px 14px 14px;font-size:16px}
.ver{position:absolute;right:6px;top:6px;bottom:6px;background:#232938;color:#c9cfdd;border:0;border-radius:8px;padding:0 12px;cursor:pointer;font-size:13px}
.btn{width:100%;margin-top:14px;background:#3ddbc4;color:#0d0e12;border:0;border-radius:12px;padding:14px;font-size:16px;font-weight:600;cursor:pointer}
.msg{color:#ff8a8a;font-size:14px;margin-top:12px;min-height:20px}
</style></head><body>
<div class="box"><div class="logo">V</div><h1>Viernes</h1><p>Escribe la clave para entrar al panel</p>
<div class="pw"><input id="c" type="password" placeholder="Clave" autofocus onkeydown="if(event.key==='Enter')entrar()">
<button class="ver" type="button" onclick="v()">Ver</button></div>
<button class="btn" onclick="entrar()">Entrar</button><div class="msg" id="m"></div></div>
<script>
function v(){const i=document.getElementById('c'),b=document.querySelector('.ver');const h=i.type==='password';i.type=h?'text':'password';b.textContent=h?'Ocultar':'Ver';}
async function entrar(){const m=document.getElementById('m');m.textContent='Entrando...';
try{const r=await fetch('/api/login',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams({clave:document.getElementById('c').value})});
const j=await r.json();if(j.ok)location.reload();else m.textContent=j.error||'Error';}catch(e){m.textContent='No hay conexion con el ESP32'}}
</script></body></html>
)rawliteral";

const char PAGINA[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="es"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Panel de Viernes</title>
<style>
:root{--bg:#0d0e12;--card:#161a24;--card2:#1c2230;--line:#232938;--txt:#e8eaf0;--mut:#8b93a7;--ac:#3ddbc4;--bad:#ff7b7b;--warn:#f5c451}
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:system-ui,Segoe UI,Arial,sans-serif;background:var(--bg);color:var(--txt)}
.app{display:grid;grid-template-columns:230px 1fr;gap:24px;padding:24px;max-width:1440px;margin:auto}
aside{background:var(--card);border:1px solid var(--line);border-radius:20px;padding:20px;height:fit-content;position:sticky;top:24px}
.brand{display:flex;gap:12px;align-items:center;margin-bottom:22px}
.logo{width:40px;height:40px;border-radius:50%;background:var(--ac);color:#0d0e12;font-weight:700;display:flex;align-items:center;justify-content:center}
.brand b{display:block;font-size:17px}.brand span{font-size:13px;color:var(--mut)}
nav button{display:block;width:100%;text-align:left;background:none;border:0;color:var(--txt);font-size:15px;padding:12px;border-radius:12px;cursor:pointer;margin-bottom:4px}
nav button.sel{background:var(--card2)}
.estado{border-top:1px solid var(--line);margin-top:18px;padding-top:16px;font-size:13px}
.estado b{display:flex;align-items:center;gap:8px;margin-bottom:6px}.estado span{color:var(--mut)}
.dot{width:8px;height:8px;border-radius:50%;background:var(--bad);display:inline-block}.dot.ok{background:var(--ac)}.dot.mid{background:var(--warn)}
header{display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:12px;margin-bottom:20px}
h1{font-size:32px}
.chips{display:flex;gap:8px;flex-wrap:wrap}
.chip{border:1px solid #2a3144;border-radius:999px;padding:7px 14px;font-size:13px}
.grid2{display:grid;grid-template-columns:1fr 1.3fr;gap:20px;margin-bottom:20px}
.card{background:var(--card);border:1px solid var(--line);border-radius:20px;padding:24px;margin-bottom:20px}
.grid2 .card{margin-bottom:0}
.card h2{font-size:19px;margin-bottom:16px}
.mic{text-align:center}.mic .peq{font-size:13px;color:var(--mut);display:flex;gap:8px;justify-content:center;align-items:center}
.circ{width:200px;height:200px;border-radius:50%;border:1px solid var(--ac);margin:26px auto 22px;display:flex;align-items:center;justify-content:center;gap:6px;background:#1a2030}
.circ i{display:block;width:6px;border-radius:4px;background:var(--ac);height:18px}
.circ.on i{animation:b 1s ease-in-out infinite}
.circ.on i:nth-child(odd){animation-delay:.15s}.circ.on i:nth-child(3n){animation-delay:.3s}
@keyframes b{0%,100%{height:14px}50%{height:70px}}
.circ.off{border-color:#3a4256}.circ.off i{background:#4a5268}
.mic h3{font-size:24px;margin-bottom:6px}.mic p{color:var(--mut);font-size:14px;margin-bottom:16px}
.btn{background:var(--ac);color:#0d0e12;border:0;border-radius:12px;padding:12px 18px;font-size:15px;font-weight:600;cursor:pointer}
.btn.sec{background:var(--card2);color:var(--txt);border:1px solid var(--line);font-weight:500}
.btn.full{width:100%}
.chat{display:flex;flex-direction:column;gap:12px;min-height:230px;justify-content:center;align-items:center;text-align:center;color:var(--mut);font-size:14px;padding:10px}
.chatin{display:flex;gap:10px;margin-top:16px}
.chatin input{flex:1}
.devs{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:14px}
.dev{background:var(--card2);border-radius:16px;padding:16px;display:flex;justify-content:space-between;align-items:center;gap:10px}
.dev b{display:block;font-size:16px;margin-bottom:3px}.dev small{display:block;color:var(--mut);font-size:13px;margin-bottom:6px}
.dev em{font-style:normal;font-size:14px}
.sw{width:54px;height:30px;border-radius:999px;background:#303850;position:relative;cursor:pointer;flex:none;transition:.2s}
.sw::after{content:"";position:absolute;top:3px;left:3px;width:24px;height:24px;border-radius:50%;background:#fff;transition:.2s}
.sw.act{background:var(--ac)}.sw.act::after{left:27px}.sw.dis{opacity:.35;pointer-events:none}
.badge{border:1px dashed #4a5268;border-radius:999px;padding:6px 12px;font-size:12px;color:var(--mut)}
.gh{display:flex;justify-content:space-between;align-items:center;margin-bottom:14px;flex-wrap:wrap;gap:10px}.gh h2{margin:0}
.info{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:14px;font-size:14px}
.info small{display:block;color:var(--mut);margin-bottom:3px}
.nota{color:var(--mut);font-size:13px;margin-top:14px}
label{font-size:13px;color:var(--mut);display:block;margin:12px 0 6px}
input,select{width:100%;background:#0d0e12;border:1px solid #2a3144;color:var(--txt);border-radius:12px;padding:13px;font-size:15px}
.pw{position:relative}.pw input{padding-right:78px}
.ver{position:absolute;right:6px;top:6px;bottom:6px;background:var(--card2);color:#c9cfdd;border:0;border-radius:8px;padding:0 12px;cursor:pointer;font-size:13px}
.msg{font-size:14px;margin-top:12px;min-height:20px;color:var(--warn)}.msg.ok{color:var(--ac)}.msg.mal{color:var(--bad)}
.log{font-family:ui-monospace,Consolas,monospace;font-size:12px;color:#b8c2e0;line-height:1.7;white-space:pre-wrap}
.row2{display:flex;gap:10px;margin-top:12px;flex-wrap:wrap}.row2 .btn{flex:1}
.vista{display:none}.vista.show{display:block}
@media(max-width:900px){
 .app{grid-template-columns:1fr;padding:14px 14px 90px}
 aside{position:fixed;left:0;right:0;bottom:0;top:auto;border-radius:18px 18px 0 0;padding:8px;z-index:9}
 .brand,.estado{display:none}
 nav{display:flex;gap:4px}nav button{text-align:center;font-size:13px;padding:12px 4px;margin:0}
 .grid2{grid-template-columns:1fr}h1{font-size:26px}
}
</style></head><body>
<div class="app">
<aside>
 <div class="brand"><div class="logo">V</div><div><b>Viernes</b><span>asistente de voz</span></div></div>
 <nav>
  <button class="sel" onclick="ir('inicio',this)">Inicio</button>
  <button onclick="ir('disp',this)">Dispositivos</button>
  <button onclick="ir('conv',this)">Conversación</button>
  <button onclick="ir('conf',this)">Configuración</button>
 </nav>
 <div class="estado"><b><span class="dot" id="dEsp"></span><span id="tEsp" style="color:var(--txt)">ESP32-S3</span></b><span id="tEsp2"></span></div>
</aside>
<div>
 <header><h1>Panel de Viernes</h1><div class="chips" id="chips"></div></header>

 <section class="vista show" id="v-inicio">
  <div class="grid2">
   <div class="card mic">
    <div class="peq"><span class="dot ok" id="dMic"></span><span id="tMicSrc">Micrófono de la PC</span></div>
    <div class="circ off" id="circ"><i></i><i></i><i></i><i></i><i></i><i></i><i></i><i></i><i></i><i></i><i></i></div>
    <h3 id="mEst">-</h3><p id="mSub"></p>
    <button class="btn sec" id="btnSil" onclick="silencio()">Silenciar micrófono</button>
    <p style="margin:16px 0 0;font-size:12px" id="mPin"></p>
   </div>
   <div class="card"><h2>Conversación</h2><div class="chatSlot"></div></div>
  </div>
  <div class="card"><h2>Dispositivos</h2><div class="devs lista"></div></div>
  <div class="card">
   <div class="gh"><h2>Configuración en GitHub</h2><button class="btn" onclick="sync()">Sincronizar ahora</button></div>
   <div class="info">
    <div><small>Repositorio</small><span id="gRepo">-</span></div>
    <div><small>Archivos</small><span>dispositivos.json · ajustes.json · personalidad.md</span></div>
    <div><small>Versión</small><span id="gVer">-</span></div>
    <div><small>Estado</small><span><span class="dot" id="gDot"></span> <span id="gEst">-</span></span></div>
   </div>
   <div class="msg" id="msgSync"></div>
   <div class="nota">Sin claves ni contraseñas en el repositorio: la API key vive solo en el ESP32.</div>
  </div>
 </section>

 <section class="vista" id="v-disp">
  <div class="card"><h2>Dispositivos</h2><div class="devs lista"></div>
   <div class="nota">Los aparatos se definen en config/dispositivos.json en GitHub.</div></div>
 </section>

 <section class="vista" id="v-conv">
  <div class="card"><h2>Conversación</h2><div class="chatSlot"></div></div>
 </section>

 <section class="vista" id="v-conf">
  <div class="card"><h2>WiFi</h2>
   <div class="nota" id="wifiAct" style="margin:0 0 6px"></div>
   <label>Nombre de la red (elige o escribe)</label>
   <input id="ssid" list="redes" placeholder="Nombre de tu WiFi" autocomplete="off"><datalist id="redes"></datalist>
   <label>Contraseña del WiFi</label>
   <div class="pw"><input id="pass" type="password" placeholder="Contraseña" autocomplete="off"><button class="ver" type="button" onclick="ver('pass',this)">Ver</button></div>
   <div class="row2"><button class="btn sec" onclick="buscar()">Buscar redes</button><button class="btn" onclick="conectar()">Conectar y guardar</button></div>
   <div class="msg" id="msgWifi"></div>
   <div class="nota">El ESP32 solo funciona con WiFi de 2.4 GHz. Si tu router tiene 2 redes (2.4 y 5 GHz), elige la de 2.4.</div>
  </div>
  <div class="card"><h2>Claves (se guardan solo en el ESP32)</h2>
   <label>API key de la IA <span id="kApi"></span></label>
   <div class="pw"><input id="apikey" type="password" placeholder="Vacío = no cambiar" autocomplete="off"><button class="ver" type="button" onclick="ver('apikey',this)">Ver</button></div>
   <label>Token de GitHub (solo para guardar datos) <span id="kGh"></span></label>
   <div class="pw"><input id="ghtoken" type="password" placeholder="Vacío = no cambiar" autocomplete="off"><button class="ver" type="button" onclick="ver('ghtoken',this)">Ver</button></div>
   <label>Nueva clave para entrar al panel</label>
   <div class="pw"><input id="newclave" type="password" placeholder="Vacío = no cambiar (mínimo 4 letras)" autocomplete="off"><button class="ver" type="button" onclick="ver('newclave',this)">Ver</button></div>
   <div class="row2"><button class="btn" onclick="claves()">Guardar</button></div>
   <div class="msg" id="msgCl"></div>
  </div>
  <div class="card"><h2>Base de datos en GitHub</h2>
   <div class="nota" style="margin:0 0 12px">Guarda el estado de los aparatos en datos/estado.json (necesita el token).</div>
   <button class="btn" onclick="guardarGh()">Guardar estado en GitHub</button><div class="msg" id="msgGh"></div></div>
  <div class="card"><h2>Registro</h2><div class="log" id="log"></div>
   <div class="row2"><button class="btn sec" onclick="salir()">Cerrar sesión</button></div></div>
 </section>
</div></div>
<script>
const $=id=>document.getElementById(id);
let S=null,primera=true;
const CHAT='<div class="chat">Aún no hay conversación.<br>Se activará cuando conectemos el puente con la PC (micrófono y voz).</div><div class="chatin"><input disabled placeholder="Escribe un mensaje..."><button class="btn" disabled style="opacity:.5">Enviar</button></div>';
document.querySelectorAll('.chatSlot').forEach(e=>e.innerHTML=CHAT);
function ir(n,b){document.querySelectorAll('.vista').forEach(v=>v.classList.remove('show'));$('v-'+n).classList.add('show');
 document.querySelectorAll('nav button').forEach(x=>x.classList.remove('sel'));if(b)b.classList.add('sel');else document.querySelectorAll('nav button')[['inicio','disp','conv','conf'].indexOf(n)].classList.add('sel');
 if(n==='conf')buscar();}
function ver(id,b){const i=$(id);const h=i.type==='password';i.type=h?'text':'password';b.textContent=h?'Ocultar':'Ver';}
async function api(url,opt){const r=await fetch(url,opt);if(r.status===401){location.reload();throw 0}return r.json();}
function post(url,data){return api(url,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(data||{})});}
function msg(id,t,c){const e=$(id);e.textContent=t;e.className='msg '+(c||'');}
function devHtml(x){
 const sub=x.desc||('GPIO'+x.pin+' · '+x.tipo);
 let der;
 if(x.prox)der='<span class="badge">Próximamente</span>';
 else if(x.tipo==='boton')der='<span class="badge">'+(x.estado?'Hundido':'Suelto')+'</span>';
 else der='<div class="sw '+(x.estado?'act':'')+' '+(x.habilitado?'':'dis')+'" onclick="tog(\''+x.id+'\','+(x.estado?0:1)+')"></div>';
 const est=x.prox?'':(x.tipo==='boton'?'':(x.habilitado?(x.estado?'Encendido':'Apagado'):'Sin conectar'));
 return '<div class="dev"><div><b>'+x.nombre+'</b><small>'+sub+'</small><em>'+est+'</em></div>'+der+'</div>';}
async function est(){try{S=await api('/api/estado');
 const m={config:'Modo configuración',completo:'Completo',local:'Local (sin internet)'}[S.modo];
 $('chips').innerHTML='<span class="chip">IA: '+S.ia.proveedor+(S.ia.modelo?' · '+S.ia.modelo:'')+'</span><span class="chip">Audio: '+S.audio+'</span><span class="chip">Configuración v'+S.versionDisp+'</span>';
 $('dEsp').className='dot '+(S.wifi?'ok':(S.modo==='config'?'mid':''));
 $('tEsp').textContent=S.wifi?'ESP32-S3 conectado':'ESP32-S3 sin WiFi';
 $('tEsp2').textContent=S.wifi?(S.ssid+' · '+S.ip):(m+' · '+S.ip);
 const act=S.escuchando&&!S.silenciado;
 $('circ').className='circ '+(act?'on':'off');
 $('mEst').textContent=S.silenciado?'Silenciado':(S.escuchando?'Escuchando':'Dormido');
 $('mSub').textContent=S.silenciado?'El micrófono está silenciado':(S.escuchando?'Di «Viernes» para hablar':'Hunde el botón para activar la escucha');
 $('btnSil').textContent=S.silenciado?'Activar micrófono':'Silenciar micrófono';
 $('mPin').textContent='Interruptor físico de escucha: GPIO'+S.pinBoton;
 $('tMicSrc').textContent='Micrófono de la '+S.audio;
 const h=S.dispositivos.map(devHtml).join('')||'<div class="nota">Sin aparatos</div>';
 document.querySelectorAll('.lista').forEach(e=>e.innerHTML=h);
 $('gRepo').textContent=S.repo;$('gVer').textContent=S.versionDisp;
 const sinc=S.hace>=0&&S.internet;
 $('gDot').className='dot '+(sinc?'ok':(S.hace>=0?'mid':''));
 $('gEst').textContent=sinc?'Sincronizada (hace '+S.hace+' s)':(S.hace>=0?'Sin internet (copia local)':'Sin sincronizar');
 $('kApi').textContent=S.apiKey?'· guardada ✓':'· sin guardar';$('kGh').textContent=S.ghToken?'· guardado ✓':'· sin guardar';
 $('wifiAct').textContent=S.wifi?('Conectado a: '+S.ssid):'Sin WiFi conectado';
 $('log').textContent=S.log.join('\n');
 if(primera){primera=false;if(S.modo==='config')ir('conf');}
}catch(e){}}
async function tog(id,on){await post('/api/toggle?id='+id+'&on='+on);est();}
async function silencio(){await post('/api/silencio',{on:S.silenciado?0:1});est();}
async function sync(){msg('msgSync','Sincronizando...');const j=await post('/api/sync');msg('msgSync',j.ok?'Listo: configuración actualizada':'No se pudo: '+(j.error||'sin internet'),j.ok?'ok':'mal');est();}
async function buscar(){try{const l=await api('/api/redes');const seen={};$('redes').innerHTML=l.filter(n=>n.ssid&&!seen[n.ssid]&&(seen[n.ssid]=1)).map(n=>'<option value="'+n.ssid.replace(/"/g,'&quot;')+'">'+n.rssi+' dBm</option>').join('');}catch(e){}}
async function conectar(){const s=$('ssid').value.trim();if(!s){msg('msgWifi','Escribe o elige el nombre de la red','mal');return;}
 msg('msgWifi','Conectando...');const j=await post('/api/wifi',{ssid:s,pass:$('pass').value});if(!j.ok){msg('msgWifi',j.error,'mal');return;}
 for(let i=0;i<40;i++){await new Promise(r=>setTimeout(r,1500));
  try{const e=await api('/api/wifi/estado');
   if(e.estado==='ok'){msg('msgWifi','¡Conectado! Guardado. IP: '+e.ip+'. El ESP32 se reiniciará en unos segundos: conecta tu celular o PC a tu WiFi normal y abre http://viernes.local o http://'+e.ip,'ok');return;}
   if(e.estado==='fallo'){msg('msgWifi',e.motivo,'mal');return;}
   msg('msgWifi','Conectando... '+Math.round(i*1.5)+' s (si tu celular se desconecta de Viernes-Setup, vuelve a conectarte y espera)');
  }catch(x){msg('msgWifi','Esperando respuesta... (reconecta tu celular a Viernes-Setup si se cayó)');}}
 msg('msgWifi','Sin respuesta. Revisa si el ESP32 ya se conectó, o intenta de nuevo.','mal');}
async function claves(){const j=await post('/api/claves',{apikey:$('apikey').value,ghtoken:$('ghtoken').value,newclave:$('newclave').value});
 msg('msgCl',j.ok?'Guardado en el ESP32':(j.error||'Error'),j.ok?'ok':'mal');if(j.ok){['apikey','ghtoken','newclave'].forEach(i=>$(i).value='');}est();}
async function guardarGh(){msg('msgGh','Guardando...');const j=await post('/api/guardar');msg('msgGh',j.ok?'Guardado en GitHub':(j.error||'Error'),j.ok?'ok':'mal');}
async function salir(){await post('/api/logout');location.reload();}
est();setInterval(est,3000);
</script></body></html>
)rawliteral";

// ---------------- RUTAS ----------------
void enviarJson(JsonDocument& doc, int codigo = 200) {
  String s;
  serializeJson(doc, s);
  server.send(codigo, "application/json", s);
}

void responderSimple(bool ok, const String& error = "") {
  JsonDocument d;
  d["ok"] = ok;
  if (!ok) d["error"] = error;
  enviarJson(d);
}

void rutaRaiz() {
  if (authOk()) server.send_P(200, "text/html; charset=utf-8", PAGINA);
  else server.send_P(200, "text/html; charset=utf-8", PAGINA_LOGIN);
}

void rutaLogin() {
  if (millis() < bloqueoHasta) { responderSimple(false, "Demasiados intentos. Espera 1 minuto."); return; }
  if (server.arg("clave") == claveActual()) {
    fallosLogin = 0;
    String t = nuevoToken();
    sesiones[sesionSig] = t;
    sesionSig = (sesionSig + 1) % 4;
    server.sendHeader("Set-Cookie", "vs=" + t + "; Path=/; HttpOnly; SameSite=Strict; Max-Age=2592000");
    logear("Inicio de sesion en el panel");
    responderSimple(true);
  } else {
    fallosLogin++;
    if (fallosLogin >= 5) { bloqueoHasta = millis() + 60000UL; fallosLogin = 0; }
    delay(700);
    responderSimple(false, "Clave incorrecta");
  }
}

void rutaLogout() {
  REQUIERE_LOGIN();
  String c = server.header("Cookie");
  int i = c.indexOf("vs=");
  String t = c.substring(i + 3, i + 3 + 32);
  for (int k = 0; k < 4; k++) if (sesiones[k] == t) sesiones[k] = "";
  server.sendHeader("Set-Cookie", "vs=; Path=/; Max-Age=0");
  responderSimple(true);
}

void rutaEstado() {
  REQUIERE_LOGIN();
  JsonDocument d;
  d["modo"] = (modo == MODO_CONFIG) ? "config" : (modo == MODO_COMPLETO ? "completo" : "local");
  d["wifi"] = (WiFi.status() == WL_CONNECTED);
  d["ssid"] = WiFi.SSID();
  d["ip"] = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  d["internet"] = hayInternet;
  d["escuchando"] = escuchando;
  d["silenciado"] = silenciado;
  d["pinBoton"] = pinBoton;
  d["repo"] = GH_REPO;
  d["audio"] = ajAudio;
  d["hace"] = ultimaSyncMs == 0 ? -1 : (long)((millis() - ultimaSyncMs) / 1000);
  d["versionDisp"] = versionDisp;
  d["versionAj"] = versionAj;
  d["apiKey"] = prefs.getString("apikey", "") != "";
  d["ghToken"] = prefs.getString("ghtoken", "") != "";
  JsonObject ia = d["ia"].to<JsonObject>();
  ia["proveedor"] = ajProveedor;
  ia["modelo"] = ajModelo;
  ia["idioma"] = ajIdioma;
  ia["volumen"] = ajVolumen;
  JsonArray arr = d["dispositivos"].to<JsonArray>();
  for (auto& x : disps) {
    JsonObject o = arr.add<JsonObject>();
    o["id"] = x.id; o["nombre"] = x.nombre; o["tipo"] = x.tipo; o["desc"] = x.desc;
    o["pin"] = x.pin; o["estado"] = x.estado; o["habilitado"] = x.habilitado; o["prox"] = x.prox;
  }
  JsonArray lg = d["log"].to<JsonArray>();
  for (int i = (int)registro.size() - 1; i >= 0; i--) lg.add(registro[i]);
  enviarJson(d);
}

void rutaToggle() {
  REQUIERE_LOGIN();
  Disp* x = buscarDisp(server.arg("id"));
  if (!x || !x->habilitado || x->tipo == "boton") { responderSimple(false, "Aparato no valido"); return; }
  escribirDisp(*x, server.arg("on") == "1");
  logear(x->nombre + (x->estado ? " encendido" : " apagado"));
  responderSimple(true);
}

void rutaSilencio() {
  REQUIERE_LOGIN();
  silenciado = server.arg("on") == "1";
  prefs.putBool("silencio", silenciado);
  actualizarIndicador();
  logear(silenciado ? "Microfono silenciado" : "Microfono activado");
  responderSimple(true);
}

void rutaRedes() {
  REQUIERE_LOGIN();
  int n = WiFi.scanNetworks();
  JsonDocument d;
  JsonArray arr = d.to<JsonArray>();
  for (int i = 0; i < n && i < 20; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["ssid"] = WiFi.SSID(i);
    o["rssi"] = WiFi.RSSI(i);
  }
  WiFi.scanDelete();
  enviarJson(d);
}

void rutaWifi() {
  REQUIERE_LOGIN();
  String ssid = server.arg("ssid");
  if (ssid == "") { responderSimple(false, "Escribe el nombre de la red"); return; }
  if (probe == P_PROBANDO) { responderSimple(false, "Ya hay una prueba en curso"); return; }
  iniciarProbe(ssid, server.arg("pass"));
  responderSimple(true);
}

void rutaWifiEstado() {
  REQUIERE_LOGIN();
  JsonDocument d;
  d["estado"] = probe == P_PROBANDO ? "probando" : probe == P_OK ? "ok" : probe == P_FALLO ? "fallo" : "ninguno";
  d["motivo"] = probeMotivo;
  d["ip"] = probeIp;
  enviarJson(d);
}

void rutaClaves() {
  REQUIERE_LOGIN();
  String nueva = server.arg("newclave");
  if (nueva != "" && nueva.length() < 4) { responderSimple(false, "La clave del panel debe tener al menos 4 caracteres"); return; }
  if (server.arg("apikey") != "")  { prefs.putString("apikey", server.arg("apikey")); logear("API key guardada"); }
  if (server.arg("ghtoken") != "") { prefs.putString("ghtoken", server.arg("ghtoken")); logear("Token de GitHub guardado"); }
  if (nueva != "")                 { prefs.putString("clave", nueva); logear("Clave del panel cambiada"); }
  responderSimple(true);
}

void rutaSync() {
  REQUIERE_LOGIN();
  bool ok = sincronizar();
  if (ok) modo = MODO_COMPLETO;
  else if (WiFi.status() == WL_CONNECTED) modo = MODO_LOCAL;
  responderSimple(ok, WiFi.status() == WL_CONNECTED ? "No pude descargar de GitHub" : "Sin WiFi");
}

void rutaGuardar() {
  REQUIERE_LOGIN();
  String r = guardarEnGitHub();
  responderSimple(r == "OK", r);
}

void iniciarWeb() {
  const char* cab[] = {"Cookie"};
  server.collectHeaders(cab, 1);
  server.on("/", HTTP_GET, rutaRaiz);
  server.on("/api/login", HTTP_POST, rutaLogin);
  server.on("/api/logout", HTTP_POST, rutaLogout);
  server.on("/api/estado", HTTP_GET, rutaEstado);
  server.on("/api/toggle", HTTP_POST, rutaToggle);
  server.on("/api/silencio", HTTP_POST, rutaSilencio);
  server.on("/api/redes", HTTP_GET, rutaRedes);
  server.on("/api/wifi", HTTP_POST, rutaWifi);
  server.on("/api/wifi/estado", HTTP_GET, rutaWifiEstado);
  server.on("/api/claves", HTTP_POST, rutaClaves);
  server.on("/api/sync", HTTP_POST, rutaSync);
  server.on("/api/guardar", HTTP_POST, rutaGuardar);
  server.onNotFound([]() {
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "");
  });
  server.begin();
}

// ---------------- BOTON ----------------
void aplicarEscucha(bool nuevo) {
  escuchando = nuevo;
  actualizarIndicador();
  logear(escuchando ? "Boton hundido: ESCUCHANDO" : "Boton suelto: DORMIDO");
}

void leerBoton() {
  bool lectura = digitalRead(pinBoton);
  if (lectura != botLecturaPrev) { botCambioMs = millis(); botLecturaPrev = lectura; }
  if ((millis() - botCambioMs) > 40 && lectura != botEstable) {
    botEstable = lectura;
    bool presionado = botonActivoAlto ? (lectura == HIGH) : (lectura == LOW);
    aplicarEscucha(presionado);
  }
}

// ---------------- SETUP Y LOOP ----------------
void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("\n=== Viernes v0.4 ===");

  prefs.begin("viernes", false);
  silenciado = prefs.getBool("silencio", false);
  if (!LittleFS.begin(true)) logear("No se pudo iniciar LittleFS");

  WiFi.onEvent([](arduino_event_id_t ev, arduino_event_info_t info) {
    if (ev == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) ultimaRazon = info.wifi_sta_disconnected.reason;
  });

  cargarCopiaLocal();

  botEstable = botLecturaPrev = digitalRead(pinBoton);
  bool presionado = botonActivoAlto ? (botEstable == HIGH) : (botEstable == LOW);
  aplicarEscucha(presionado);

  bool conectado = false;
  if (prefs.getString("ssid", "") != "") {
    for (int i = 1; i <= INTENTOS_WIFI && !conectado; i++) {
      logear("Conectando al WiFi (intento " + String(i) + ")");
      conectado = conectarWiFi();
    }
  }

  if (conectado) {
    logear("WiFi conectado: " + WiFi.localIP().toString());
    if (MDNS.begin(NOMBRE_MDNS)) MDNS.addService("http", "tcp", 80);
    modo = sincronizar() ? MODO_COMPLETO : MODO_LOCAL;
    if (modo == MODO_LOCAL) logear("Sin internet: modo local con la ultima copia");
  } else {
    modo = MODO_CONFIG;
    iniciarAP();
  }
  iniciarWeb();
  logear("Panel listo");
}

void loop() {
  server.handleClient();
  if (modo == MODO_CONFIG) dns.processNextRequest();
  leerBoton();
  vigilarProbe();

  unsigned long ahora = millis();

  if (modo == MODO_CONFIG && probe == P_NINGUNO && prefs.getString("ssid", "") != "") {
    if (WiFi.status() == WL_CONNECTED) { logear("WiFi disponible: reiniciando"); delay(500); ESP.restart(); }
    if (ahora - ultimoIntentoApMs > 120000UL) {
      ultimoIntentoApMs = ahora;
      WiFi.begin(prefs.getString("ssid", "").c_str(), prefs.getString("pass", "").c_str());
    }
  }

  if (modo != MODO_CONFIG && ahora - ultimoChequeoMs > 60000UL) {
    ultimoChequeoMs = ahora;
    if (WiFi.status() != WL_CONNECTED) {
      hayInternet = false;
      modo = MODO_LOCAL;
      logear("WiFi caido: reconectando");
      WiFi.reconnect();
    } else {
      bool tocaSync = (ultimaSyncMs == 0) || (ahora - ultimaSyncMs > (unsigned long)ajSyncMin * 60000UL);
      if (modo == MODO_LOCAL || tocaSync) modo = sincronizar() ? MODO_COMPLETO : MODO_LOCAL;
    }
  }
}
