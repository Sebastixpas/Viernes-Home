/*
  ============================================================
  VIERNES - Firmware v0.5  (ESP32-S3)
  ============================================================
  - Estados: Configuracion / Completo / Local.
  - Panel web con LOGIN. Todo se guarda en la memoria del ESP32.
  - WiFi con diagnostico y soporte de REDES OCULTAS.
  - Dispositivos: los de GitHub + los que agregas desde el panel.
  - IA: cualquier servicio compatible con OpenAI (OmniRouter).
    Chat real que puede encender/apagar aparatos.

  SEGURIDAD: aqui NO hay claves. La clave inicial del panel esta en
  secrets.h (no se sube a GitHub). La API key y el token se escriben
  en el panel y se guardan en la memoria NVS del ESP32.

  Libreria: ArduinoJson v7.
  Placa: "ESP32S3 Dev Module", Flash 16MB, Particion "Huge APP".
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
const int   MAX_LOCALES = 12;

// ---------------- ESTADO GENERAL ----------------
enum Modo { MODO_CONFIG, MODO_COMPLETO, MODO_LOCAL };
Modo modo = MODO_CONFIG;

Preferences prefs;
WebServer server(80);
DNSServer dns;

struct Disp {
  String id, nombre, tipo, desc, alias;
  int pin = -1;
  bool activoAlto = true;
  bool habilitado = true;
  bool prox = false;
  bool estado = false;
  bool propio = false;        // lo agregaste tu desde el panel
  bool sobrescrito = false;   // viene de GitHub pero lo editaste
};
std::vector<Disp> disps;      // lista final en uso
std::vector<Disp> remotos;    // de GitHub
std::vector<Disp> locales;    // del panel
std::vector<int> pinesPermitidos;

String ajIdioma = "es-419", ajProveedor = "openai-compatible";
String ajIaUrl = "https://sebastixpas.prod.page/v1", ajIaModelo = "Viernes Model";
String ajAudio = "PC", ajIndicador = "luz_cuarto";
int ajVolumen = 70, ajSyncMin = 30, versionDisp = 0, versionAj = 0, ajMaxTokens = 400;
String personalidad = "";

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
bool probeOculta = false;
unsigned long probeIni = 0, probeOkMs = 0;
volatile int ultimaRazon = 0;

// ----- Chat -----
struct Msg { String rol; String txt; String acc; };
std::vector<Msg> chatUI;                 // lo que ve el panel
std::vector<String> ctxRol, ctxTxt;      // contexto para la IA

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
  if (pin < 0) return false;
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

String iaUrl() {
  String u = prefs.getString("ia_url", "");
  if (u == "") u = ajIaUrl;
  while (u.endsWith("/")) u.remove(u.length() - 1);
  return u;
}
String iaModelo() {
  String m = prefs.getString("ia_modelo", "");
  return m != "" ? m : ajIaModelo;
}

String slug(const String& s) {
  String r = "";
  for (unsigned i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) r += c;
    else if (c == ' ' || c == '-' || c == '_') { if (r.length() && r[r.length() - 1] != '_') r += '_'; }
  }
  while (r.endsWith("_")) r.remove(r.length() - 1);
  if (r == "") r = "aparato";
  return r;
}

// ---------------- DISPOSITIVOS ----------------
Disp parseDisp(JsonObject o) {
  Disp d;
  d.id = (const char*)(o["id"] | "");
  d.nombre = (const char*)(o["nombre"] | d.id.c_str());
  d.tipo = (const char*)(o["tipo"] | "led");
  d.desc = (const char*)(o["descripcion"] | "");
  d.pin = o["pin"] | -1;
  d.activoAlto = String((const char*)(o["activo_en"] | "alto")) == "alto";
  d.habilitado = o["habilitado"] | true;
  d.prox = o["proximamente"] | false;
  if (o["alias"].is<JsonArray>()) {
    for (JsonVariant a : o["alias"].as<JsonArray>()) {
      if (d.alias != "") d.alias += ", ";
      d.alias += a.as<String>();
    }
  } else if (o["alias"].is<const char*>()) {
    d.alias = (const char*)o["alias"];
  }
  if (d.desc == "" && d.pin >= 0) d.desc = "GPIO" + String(d.pin) + " · " + (d.tipo == "rele" ? "módulo de relé" : "LED");
  return d;
}

bool cargarRemotos(const String& json) {
  JsonDocument doc;
  if (deserializeJson(doc, json)) return false;
  if (!doc["dispositivos"].is<JsonArray>()) return false;
  std::vector<int> permitidos;
  for (JsonVariant p : doc["pines_permitidos"].as<JsonArray>()) permitidos.push_back(p.as<int>());
  std::vector<Disp> nuevos;
  for (JsonObject o : doc["dispositivos"].as<JsonArray>()) {
    Disp d = parseDisp(o);
    if (d.id != "") nuevos.push_back(d);
  }
  if (nuevos.empty()) return false;
  pinesPermitidos = permitidos;
  remotos = nuevos;
  versionDisp = doc["version"] | 0;
  return true;
}

void cargarLocales() {
  locales.clear();
  String s = "";
  File f = LittleFS.open("/locales.json", "r");
  if (f) { s = f.readString(); f.close(); }
  if (s == "") return;
  JsonDocument doc;
  if (deserializeJson(doc, s)) return;
  for (JsonObject o : doc.as<JsonArray>()) {
    Disp d = parseDisp(o);
    if (d.id != "") locales.push_back(d);
  }
}

void guardarLocales() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (auto& d : locales) {
    JsonObject o = arr.add<JsonObject>();
    o["id"] = d.id; o["nombre"] = d.nombre; o["tipo"] = d.tipo;
    o["pin"] = d.pin; o["activo_en"] = d.activoAlto ? "alto" : "bajo";
    o["habilitado"] = d.habilitado;
    JsonArray al = o["alias"].to<JsonArray>();
    String a = d.alias;
    while (a.length()) {
      int i = a.indexOf(',');
      String p = (i < 0) ? a : a.substring(0, i);
      p.trim();
      if (p != "") al.add(p);
      if (i < 0) break;
      a = a.substring(i + 1);
    }
  }
  File f = LittleFS.open("/locales.json", "w");
  if (f) { serializeJson(doc, f); f.close(); }
}

// Junta GitHub + panel, valida pines y configura los pines fisicos
void reconstruir() {
  std::vector<Disp> viejos = disps;
  for (auto& d : viejos) {
    if (d.pin >= 0 && d.tipo != "boton" && d.habilitado) {
      digitalWrite(d.pin, d.activoAlto ? LOW : HIGH);
      pinMode(d.pin, INPUT);
    }
  }

  std::vector<Disp> nuevos = remotos;
  for (auto& l : locales) {
    bool hallado = false;
    for (auto& n : nuevos) {
      if (n.id == l.id) { Disp m = l; m.sobrescrito = true; n = m; hallado = true; break; }
    }
    if (!hallado) { Disp m = l; m.propio = true; nuevos.push_back(m); }
  }

  std::vector<int> usados;
  for (auto& d : nuevos) {
    if (!d.habilitado || d.prox) continue;
    if (!pinPermitido(d.pin)) {
      logear("Pin " + String(d.pin) + " no permitido para " + d.nombre + ": desactivado");
      d.habilitado = false; continue;
    }
    bool dup = false;
    for (int u : usados) if (u == d.pin) dup = true;
    if (dup) { logear("Pin " + String(d.pin) + " repetido en " + d.nombre + ": desactivado"); d.habilitado = false; continue; }
    usados.push_back(d.pin);
  }

  pinBoton = PIN_BOTON_DEFECTO;
  botonActivoAlto = false;
  bool hayBoton = false;
  for (auto& d : nuevos) {
    if (!d.habilitado || d.pin < 0) continue;
    for (auto& v : viejos) if (v.id == d.id) d.estado = v.estado;
    if (d.tipo == "boton") {
      pinBoton = d.pin; botonActivoAlto = d.activoAlto; hayBoton = true;
      pinMode(d.pin, botonActivoAlto ? INPUT_PULLDOWN : INPUT_PULLUP);
    } else {
      pinMode(d.pin, OUTPUT);
      escribirDisp(d, d.estado);
    }
  }
  if (!hayBoton) pinMode(pinBoton, INPUT_PULLUP);
  disps = nuevos;
}

bool aplicarDispositivos(const String& json) {
  if (!cargarRemotos(json)) return false;
  reconstruir();
  return true;
}

const char* DISP_POR_DEFECTO =
  "{\"version\":0,\"pines_permitidos\":[1,2,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,21,38,39,40,41,42],\"dispositivos\":["
  "{\"id\":\"luz_cuarto\",\"nombre\":\"Luz del cuarto\",\"pin\":4,\"tipo\":\"led\",\"activo_en\":\"alto\",\"habilitado\":true},"
  "{\"id\":\"boton_escucha\",\"nombre\":\"Boton de escucha\",\"pin\":5,\"tipo\":\"boton\",\"descripcion\":\"GPIO5 · interruptor\",\"activo_en\":\"bajo\",\"habilitado\":true}]}";

bool aplicarAjustes(const String& json) {
  JsonDocument doc;
  if (deserializeJson(doc, json)) return false;
  versionAj = doc["version"] | 0;
  ajIdioma = (const char*)(doc["asistente"]["idioma"] | "es-419");
  ajVolumen = doc["asistente"]["volumen"] | 70;
  ajAudio = (const char*)(doc["asistente"]["audio"] | "PC");
  ajIndicador = (const char*)(doc["asistente"]["indicador_escucha"] | "");
  ajProveedor = (const char*)(doc["ia"]["proveedor"] | "openai-compatible");
  ajIaUrl = (const char*)(doc["ia"]["url_base"] | ajIaUrl.c_str());
  ajIaModelo = (const char*)(doc["ia"]["modelo"] | ajIaModelo.c_str());
  ajMaxTokens = doc["ia"]["max_tokens_respuesta"] | 400;
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
  cargarLocales();
  String d = leerArchivo("/dispositivos.json");
  if (d == "" || !aplicarDispositivos(d)) {
    aplicarDispositivos(DISP_POR_DEFECTO);
    logear("Usando dispositivos por defecto");
  } else {
    logear("Dispositivos cargados de la copia local (v" + String(versionDisp) + ")");
  }
  String a = leerArchivo("/ajustes.json");
  if (a != "") aplicarAjustes(a);
  personalidad = leerArchivo("/personalidad.md");
}

void actualizarIndicador() {
  Disp* ind = ajIndicador != "" ? buscarDisp(ajIndicador) : nullptr;
  if (ind && ind->habilitado) escribirDisp(*ind, escuchando && !silenciado);
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

bool sincronizar() {
  if (WiFi.status() != WL_CONNECTED) { hayInternet = false; return false; }
  String d, a, p;
  if (!descargar(urlRaw("config/dispositivos.json"), d)) { hayInternet = false; return false; }
  hayInternet = true;
  bool okA = descargar(urlRaw("config/ajustes.json"), a);
  bool okP = descargar(urlRaw("config/personalidad.md"), p);

  if (aplicarDispositivos(d)) guardarArchivo("/dispositivos.json", d);
  else logear("dispositivos.json descargado no es valido");
  if (okA && aplicarAjustes(a)) guardarArchivo("/ajustes.json", a);
  if (okP && p.length() > 5) { personalidad = p; guardarArchivo("/personalidad.md", p); }
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

// ---------------- IA (compatible con OpenAI) ----------------
String promptSistema() {
  String s = personalidad.length() > 5 ? personalidad : String("Eres Viernes, asistente de voz de la casa. Hablas en español latino con respuestas cortas.");
  if (s.length() > 1800) s = s.substring(0, 1800);
  s += "\n\nAparatos que puedes controlar (usa el campo id):\n";
  int n = 0;
  for (auto& d : disps) {
    if (!d.habilitado || d.prox || d.tipo == "boton") continue;
    s += "- id=" + d.id + " nombre=" + d.nombre;
    if (d.alias != "") s += " otros_nombres=" + d.alias;
    s += String(" estado=") + (d.estado ? "encendido" : "apagado") + "\n";
    n++;
  }
  if (n == 0) s += "(ninguno por ahora)\n";
  s += "\nFORMATO OBLIGATORIO: responde SIEMPRE y SOLO con un objeto JSON, sin markdown ni texto extra: "
       "{\"respuesta\":\"frase corta para decir en voz alta\",\"acciones\":[{\"id\":\"id_del_aparato\",\"estado\":\"encendido\"}]}. "
       "estado solo puede ser encendido o apagado. Si no hay que controlar nada, acciones es [].";
  return s;
}

String recorte(const String& s, unsigned n) {
  String r = s;
  r.replace("\n", " ");
  if (r.length() > n) r = r.substring(0, n) + "...";
  return r;
}

bool pedirIA(const String& sistema, bool conContexto, const String& usuario, String& contenido, String& error) {
  String key = prefs.getString("apikey", "");
  if (key == "") { error = "Falta la API key (Configuracion)"; return false; }
  if (WiFi.status() != WL_CONNECTED || !hayInternet) { error = "Sin internet"; return false; }

  JsonDocument req;
  req["model"] = iaModelo();
  req["max_tokens"] = ajMaxTokens;
  req["temperature"] = 0.4;
  JsonArray m = req["messages"].to<JsonArray>();
  { JsonObject o = m.add<JsonObject>(); o["role"] = "system"; o["content"] = sistema; }
  if (conContexto) {
    for (size_t i = 0; i < ctxRol.size(); i++) {
      JsonObject o = m.add<JsonObject>(); o["role"] = ctxRol[i]; o["content"] = ctxTxt[i];
    }
  }
  { JsonObject o = m.add<JsonObject>(); o["role"] = "user"; o["content"] = usuario; }
  String body;
  serializeJson(req, body);

  WiFiClientSecure cliente;
  cliente.setInsecure();
  HTTPClient http;
  http.setTimeout(30000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(cliente, iaUrl() + "/chat/completions")) { error = "URL de la IA no valida"; return false; }
  http.addHeader("Authorization", "Bearer " + key);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "application/json");
  int codigo = http.POST(body);
  String resp = (codigo > 0) ? http.getString() : "";
  http.end();

  if (codigo <= 0) { error = "No pude conectar con la IA: " + HTTPClient::errorToString(codigo); return false; }
  if (codigo != 200) {
    error = "La IA respondio " + String(codigo);
    JsonDocument e;
    if (!deserializeJson(e, resp)) {
      String msg = (const char*)(e["error"]["message"] | "");
      if (msg == "") msg = (const char*)(e["error"] | "");
      if (msg == "") msg = (const char*)(e["message"] | "");
      if (msg != "") error += ": " + recorte(msg, 140);
    } else if (resp != "") error += ": " + recorte(resp, 120);
    return false;
  }

  JsonDocument filtro;
  filtro["choices"][0]["message"]["content"] = true;
  JsonDocument r;
  if (deserializeJson(r, resp, DeserializationOption::Filter(filtro))) { error = "No entendi la respuesta de la IA"; return false; }
  contenido = (const char*)(r["choices"][0]["message"]["content"] | "");
  contenido.trim();
  if (contenido == "") { error = "La IA respondio vacio"; return false; }
  return true;
}

void agregarContexto(const String& rol, const String& txt) {
  ctxRol.push_back(rol);
  ctxTxt.push_back(txt.length() > 600 ? txt.substring(0, 600) : txt);
  while (ctxRol.size() > 8) { ctxRol.erase(ctxRol.begin()); ctxTxt.erase(ctxTxt.begin()); }
}

void agregarChat(const String& rol, const String& txt, const String& acc) {
  chatUI.push_back({rol, txt, acc});
  while (chatUI.size() > 14) chatUI.erase(chatUI.begin());
}

// ---------------- WIFI ----------------
String motivoTexto(int r, bool oculta) {
  switch (r) {
    case 201:
      return oculta ? "No encuentro la red oculta. Revisa que el nombre este EXACTO (mayusculas y espacios) y que sea de 2.4 GHz."
                    : "No encuentro esa red. Revisa el nombre. Si es OCULTA, marca la casilla 'Red oculta'. Recuerda: solo WiFi de 2.4 GHz.";
    case 202: case 15: case 204: case 2: return "La clave del WiFi es incorrecta.";
    case 203: case 205: return "El router rechazo la conexion (codigo " + String(r) + ").";
    default: return "No se pudo conectar (codigo " + String(r) + ").";
  }
}

bool conectarWiFi() {
  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");
  bool oculta = prefs.getBool("oculta", false);
  if (ssid == "") return false;
  WiFi.mode(WIFI_STA);
  if (oculta) WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.begin(ssid.c_str(), pass.c_str());
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < TIEMPO_WIFI_MS) delay(250);
  if (WiFi.status() != WL_CONNECTED && ultimaRazon != 0) logear("WiFi fallo: " + motivoTexto(ultimaRazon, oculta));
  return WiFi.status() == WL_CONNECTED;
}

void iniciarAP() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_NOMBRE);
  dns.start(53, "*", WiFi.softAPIP());
  logear("Modo configuracion: red " + String(AP_NOMBRE) + " -> http://192.168.4.1");
}

void iniciarProbe(const String& ssid, const String& pass, bool oculta) {
  probeSsid = ssid;
  probePass = pass;
  probeOculta = oculta;
  probeMotivo = "";
  probeIp = "";
  ultimaRazon = 0;
  probeIni = millis();
  probe = P_PROBANDO;
  WiFi.disconnect(false, true);
  delay(100);
  if (oculta) WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.begin(ssid.c_str(), pass.c_str());
  logear(String("Probando WiFi") + (oculta ? " (oculta): " : ": ") + ssid);
}

void vigilarProbe() {
  if (probe == P_PROBANDO) {
    if (WiFi.status() == WL_CONNECTED) {
      prefs.putString("ssid", probeSsid);
      prefs.putString("pass", probePass);
      prefs.putBool("oculta", probeOculta);
      probeIp = WiFi.localIP().toString();
      probe = P_OK;
      probeOkMs = millis();
      logear("WiFi OK (" + probeSsid + "): guardado. IP " + probeIp);
    } else if ((ultimaRazon != 0 && millis() - probeIni > (probeOculta ? 12000UL : 6000UL)) || millis() - probeIni > 30000UL) {
      probeMotivo = ultimaRazon != 0 ? motivoTexto(ultimaRazon, probeOculta) : "Tiempo agotado: no pude conectarme.";
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
.btn.peq{padding:7px 12px;font-size:13px}
.btn.mal{background:#3a1f26;color:#ff9b9b;border:1px solid #5a2a35}
.btn:disabled{opacity:.5}
.chatbox{display:flex;flex-direction:column;gap:10px;min-height:230px;max-height:340px;overflow:auto;padding:4px 2px}
.m{max-width:85%;padding:12px 16px;border-radius:14px;font-size:15px;line-height:1.4;white-space:pre-wrap}
.m.u{align-self:flex-end;background:var(--card2)}
.m.b{align-self:flex-start;background:#123a33;border:1px solid #1d5a4e}
.m.err{background:#3a1f26;border-color:#5a2a35}
.acc{align-self:flex-start;font-size:12px;color:var(--mut);margin-top:-6px;margin-left:4px}
.vacio{color:var(--mut);font-size:14px;text-align:center;margin:auto;padding:20px}
.chatin{display:flex;gap:10px;margin-top:16px}
.chatin input{flex:1}
.devs{display:grid;grid-template-columns:repeat(auto-fit,minmax(270px,1fr));gap:14px}
.dev{background:var(--card2);border-radius:16px;padding:16px}
.dev .top{display:flex;justify-content:space-between;align-items:center;gap:10px}
.dev b{display:block;font-size:16px;margin-bottom:3px}.dev small{display:block;color:var(--mut);font-size:13px;margin-bottom:6px}
.dev em{font-style:normal;font-size:14px}
.dev .ac2{display:flex;gap:8px;margin-top:12px;flex-wrap:wrap}
.tag{font-size:11px;border:1px solid #2f7f72;color:var(--ac);border-radius:999px;padding:2px 8px;margin-left:6px;font-weight:400}
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
input[type=checkbox]{width:auto}
.chk{display:flex;gap:8px;align-items:center;color:var(--txt);font-size:14px;margin-top:14px}
.pw{position:relative}.pw input{padding-right:78px}
.ver{position:absolute;right:6px;top:6px;bottom:6px;background:var(--card2);color:#c9cfdd;border:0;border-radius:8px;padding:0 12px;cursor:pointer;font-size:13px}
.msg{font-size:14px;margin-top:12px;min-height:20px;color:var(--warn);white-space:pre-wrap}.msg.ok{color:var(--ac)}.msg.mal{color:var(--bad)}
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
  <div class="card"><h2>Mis dispositivos</h2><div class="devs listaAdm"></div>
   <div class="nota">Los marcados "GitHub" vienen de config/dispositivos.json. Los que agregues aquí se guardan en el ESP32.</div></div>
  <div class="card"><h2 id="fTit">Agregar dispositivo</h2>
   <input type="hidden" id="dId">
   <label>Nombre</label><input id="dNombre" placeholder="Ej: Luz de la sala" maxlength="30">
   <label>Tipo</label><select id="dTipo"><option value="led">LED / salida directa</option><option value="rele">Módulo de relé</option></select>
   <label>Pin del ESP32</label><select id="dPin"></select>
   <label>Se activa en</label><select id="dAct"><option value="alto">ALTO (el pin da 3.3 V para encender)</option><option value="bajo">BAJO (el pin va a GND para encender; típico de relés)</option></select>
   <label>Otros nombres para la voz (separados por coma)</label><input id="dAlias" placeholder="foco, lámpara">
   <label class="chk"><input type="checkbox" id="dHab" checked> Activo (usar el pin ahora)</label>
   <div class="row2"><button class="btn" onclick="guardarDisp()">Guardar dispositivo</button><button class="btn sec" onclick="nuevoDisp()">Limpiar</button></div>
   <div class="msg" id="msgDisp"></div>
   <div class="nota">Pines libres ahora: <span id="pinesLibres">-</span>. No uses un pin que ya tenga algo conectado.</div>
  </div>
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
   <label class="chk"><input type="checkbox" id="oculta"> Es una red OCULTA (no aparece en la lista; escribe el nombre exacto)</label>
   <div class="row2"><button class="btn sec" onclick="buscar()">Buscar redes</button><button class="btn" onclick="conectar()">Conectar y guardar</button></div>
   <div class="msg" id="msgWifi"></div>
   <div class="nota">El ESP32 solo funciona con WiFi de 2.4 GHz.</div>
  </div>
  <div class="card"><h2>Inteligencia artificial</h2>
   <div class="nota" style="margin:0 0 6px" id="iaAct"></div>
   <label>URL del servicio (OmniRouter u otro compatible con OpenAI)</label>
   <input id="iaUrl" placeholder="https://.../v1" autocomplete="off">
   <label>Modelo</label>
   <input id="iaModelo" list="modelos" placeholder="Viernes Model" autocomplete="off"><datalist id="modelos"></datalist>
   <label>API key <span id="kApi"></span></label>
   <div class="pw"><input id="apikey" type="password" placeholder="Vacío = no cambiar" autocomplete="off"><button class="ver" type="button" onclick="ver('apikey',this)">Ver</button></div>
   <div class="row2"><button class="btn sec" onclick="verModelos()">Ver modelos</button><button class="btn" onclick="guardarIa()">Guardar IA</button><button class="btn sec" onclick="probarIa()">Probar IA</button></div>
   <div class="msg" id="msgIa"></div>
   <div class="nota">La API key se guarda solo en el ESP32. Si dejas URL y modelo vacíos se usan los de GitHub.</div>
  </div>
  <div class="card"><h2>Otras claves</h2>
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
)rawliteral"
R"rawliteral(
<script>
const $=id=>document.getElementById(id);
let S=null,primera=true,chat=[];
const CHAT='<div class="chatbox"></div><div class="chatin"><input class="chatinp" placeholder="Escribe un mensaje..." onkeydown="if(event.key===\'Enter\')enviar(this)"><button class="btn" onclick="enviar(this.previousElementSibling)">Enviar</button></div>';
document.querySelectorAll('.chatSlot').forEach(e=>e.innerHTML=CHAT);
function esc(t){return String(t).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));}
function renderChat(){const h=chat.length?chat.map(m=>'<div class="m '+(m.r==='u'?'u':'b')+(m.err?' err':'')+'">'+esc(m.t)+'</div>'+(m.acc?'<div class="acc">Acción: '+esc(m.acc)+'</div>':'')).join(''):'<div class="vacio">Escríbele algo a Viernes.<br>Ej: «enciende la luz del cuarto»</div>';
 document.querySelectorAll('.chatbox').forEach(b=>{b.innerHTML=h;b.scrollTop=b.scrollHeight;});}
function ir(n,b){document.querySelectorAll('.vista').forEach(v=>v.classList.remove('show'));$('v-'+n).classList.add('show');
 document.querySelectorAll('nav button').forEach(x=>x.classList.remove('sel'));if(b)b.classList.add('sel');else document.querySelectorAll('nav button')[['inicio','disp','conv','conf'].indexOf(n)].classList.add('sel');
 if(n==='conf')buscar();if(n==='disp')fillPins();}
function ver(id,b){const i=$(id);const h=i.type==='password';i.type=h?'text':'password';b.textContent=h?'Ocultar':'Ver';}
async function api(url,opt){const r=await fetch(url,opt);if(r.status===401){location.reload();throw 0}return r.json();}
function post(url,data){return api(url,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(data||{})});}
function msg(id,t,c){const e=$(id);e.textContent=t;e.className='msg '+(c||'');}
function devHtml(x,adm){
 const sub=x.desc||('GPIO'+x.pin+' · '+x.tipo);
 let der;
 if(x.prox)der='<span class="badge">Próximamente</span>';
 else if(x.tipo==='boton')der='<span class="badge">'+(x.estado?'Hundido':'Suelto')+'</span>';
 else der='<div class="sw '+(x.estado?'act':'')+' '+(x.habilitado?'':'dis')+'" onclick="tog(\''+x.id+'\','+(x.estado?0:1)+')"></div>';
 const est=(x.prox||x.tipo==='boton')?'':(x.habilitado?(x.estado?'Encendido':'Apagado'):'Sin conectar');
 const tag=x.propio?'<span class="tag">propio</span>':(x.sobrescrito?'<span class="tag">editado</span>':(adm?'<span class="tag" style="border-color:#4a5268;color:#8b93a7">GitHub</span>':''));
 let btn='';
 if(adm&&x.editable){btn='<div class="ac2"><button class="btn sec peq" onclick="editar(\''+x.id+'\')">Editar</button>'+
  (x.propio?'<button class="btn mal peq" onclick="borrar(\''+x.id+'\',\'¿Eliminar este dispositivo?\')">Eliminar</button>':'')+
  (x.sobrescrito?'<button class="btn sec peq" onclick="borrar(\''+x.id+'\',\'¿Volver al original de GitHub?\')">Restaurar original</button>':'')+'</div>';}
 return '<div class="dev"><div class="top"><div><b>'+esc(x.nombre)+tag+'</b><small>'+esc(sub)+'</small><em>'+est+'</em></div>'+der+'</div>'+btn+'</div>';}
async function est(){try{S=await api('/api/estado');
 const m={config:'Modo configuración',completo:'Completo',local:'Local (sin internet)'}[S.modo];
 $('chips').innerHTML='<span class="chip">IA: '+esc(S.ia.modelo||S.ia.proveedor)+'</span><span class="chip">Audio: '+esc(S.audio)+'</span><span class="chip">Configuración v'+S.versionDisp+'</span>';
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
 const h=S.dispositivos.filter(x=>!x.prox||true).map(x=>devHtml(x,false)).join('')||'<div class="nota">Sin aparatos</div>';
 document.querySelectorAll('.lista').forEach(e=>e.innerHTML=h);
 document.querySelectorAll('.listaAdm').forEach(e=>e.innerHTML=S.dispositivos.map(x=>devHtml(x,true)).join(''));
 $('pinesLibres').textContent=S.pinesLibres.join(', ')||'ninguno';
 $('gRepo').textContent=S.repo;$('gVer').textContent=S.versionDisp;
 const sinc=S.hace>=0&&S.internet;
 $('gDot').className='dot '+(sinc?'ok':(S.hace>=0?'mid':''));
 $('gEst').textContent=sinc?'Sincronizada (hace '+S.hace+' s)':(S.hace>=0?'Sin internet (copia local)':'Sin sincronizar');
 $('kApi').textContent=S.apiKey?'· guardada ✓':'· sin guardar';$('kGh').textContent=S.ghToken?'· guardado ✓':'· sin guardar';
 $('wifiAct').textContent=S.wifi?('Conectado a: '+S.ssid):'Sin WiFi conectado';
 $('iaAct').textContent='En uso: '+S.ia.modelo+' · '+S.ia.url;
 if(primera){$('iaUrl').value=S.ia.urlPropia?S.ia.url:'';$('iaModelo').value=S.ia.modeloPropio?S.ia.modelo:'';}
 $('log').textContent=S.log.join('\n');
 if(primera){primera=false;if(S.modo==='config')ir('conf');
  try{const h=await api('/api/chat/hist');chat=h.map(x=>({r:x.rol==='user'?'u':'b',t:x.txt,acc:x.acc}));}catch(e){}renderChat();}
}catch(e){}}
async function tog(id,on){await post('/api/toggle?id='+id+'&on='+on);est();}
async function silencio(){await post('/api/silencio',{on:S.silenciado?0:1});est();}
async function sync(){msg('msgSync','Sincronizando...');const j=await post('/api/sync');msg('msgSync',j.ok?'Listo: configuración actualizada':'No se pudo: '+(j.error||'sin internet'),j.ok?'ok':'mal');est();}
async function enviar(inp){const t=inp.value.trim();if(!t)return;inp.value='';
 document.querySelectorAll('.chatinp').forEach(i=>i.value='');
 chat.push({r:'u',t});chat.push({r:'b',t:'Pensando...'});renderChat();
 const j=await post('/api/chat',{mensaje:t});chat.pop();
 if(j.ok)chat.push({r:'b',t:j.respuesta,acc:j.acciones});else chat.push({r:'b',t:j.error||'Error',err:1});
 renderChat();est();}
async function buscar(){try{const l=await api('/api/redes');const seen={};$('redes').innerHTML=l.filter(n=>n.ssid&&!seen[n.ssid]&&(seen[n.ssid]=1)).map(n=>'<option value="'+esc(n.ssid)+'">'+n.rssi+' dBm</option>').join('');}catch(e){}}
async function conectar(){const s=$('ssid').value.trim();if(!s){msg('msgWifi','Escribe o elige el nombre de la red','mal');return;}
 msg('msgWifi','Conectando...');const j=await post('/api/wifi',{ssid:s,pass:$('pass').value,oculta:$('oculta').checked?1:0});if(!j.ok){msg('msgWifi',j.error,'mal');return;}
 for(let i=0;i<40;i++){await new Promise(r=>setTimeout(r,1500));
  try{const e=await api('/api/wifi/estado');
   if(e.estado==='ok'){msg('msgWifi','¡Conectado! Guardado. IP: '+e.ip+'. El ESP32 se reiniciará en unos segundos: conecta tu celular o PC a tu WiFi normal y abre http://viernes.local o http://'+e.ip,'ok');return;}
   if(e.estado==='fallo'){msg('msgWifi',e.motivo,'mal');return;}
   msg('msgWifi','Conectando... '+Math.round(i*1.5)+' s (si tu celular se desconecta de Viernes-Setup, vuelve a conectarte y espera)');
  }catch(x){msg('msgWifi','Esperando respuesta... (reconecta tu celular a Viernes-Setup si se cayó)');}}
 msg('msgWifi','Sin respuesta. Revisa si el ESP32 ya se conectó, o intenta de nuevo.','mal');}
async function guardarIa(){const j=await post('/api/ia',{url:$('iaUrl').value,modelo:$('iaModelo').value,apikey:$('apikey').value});
 msg('msgIa',j.ok?'Guardado en el ESP32':(j.error||'Error'),j.ok?'ok':'mal');if(j.ok)$('apikey').value='';est();}
async function verModelos(){msg('msgIa','Consultando modelos...');const j=await api('/api/ia/modelos');
 if(!j.ok){msg('msgIa',j.error,'mal');return;}
 $('modelos').innerHTML=j.modelos.map(m=>'<option value="'+esc(m)+'">').join('');
 msg('msgIa','Modelos disponibles ('+j.modelos.length+'):\n'+j.modelos.join('\n')+'\n\nToca el campo Modelo para elegir uno.','ok');}
async function probarIa(){msg('msgIa','Probando... (puede tardar unos segundos)');const j=await post('/api/ia/probar');
 msg('msgIa',j.ok?'IA funcionando ✓\nRespuesta: '+j.respuesta:(j.error||'Error'),j.ok?'ok':'mal');}
async function claves(){const j=await post('/api/claves',{ghtoken:$('ghtoken').value,newclave:$('newclave').value});
 msg('msgCl',j.ok?'Guardado en el ESP32':(j.error||'Error'),j.ok?'ok':'mal');if(j.ok){['ghtoken','newclave'].forEach(i=>$(i).value='');}est();}
async function guardarGh(){msg('msgGh','Guardando...');const j=await post('/api/guardar');msg('msgGh',j.ok?'Guardado en GitHub':(j.error||'Error'),j.ok?'ok':'mal');}
function fillPins(cur){if(!S)return;const l=S.pinesLibres.slice();if(cur!==undefined&&cur>=0&&l.indexOf(cur)<0)l.push(cur);l.sort((a,b)=>a-b);
 $('dPin').innerHTML=l.map(p=>'<option value="'+p+'">GPIO '+p+'</option>').join('');if(cur!==undefined)$('dPin').value=cur;}
function nuevoDisp(){$('fTit').textContent='Agregar dispositivo';$('dId').value='';$('dNombre').value='';$('dTipo').value='led';$('dAct').value='alto';$('dAlias').value='';$('dHab').checked=true;fillPins();msg('msgDisp','');}
function editar(id){const x=S.dispositivos.find(d=>d.id===id);if(!x)return;$('fTit').textContent='Editar: '+x.nombre;$('dId').value=x.id;$('dNombre').value=x.nombre;
 $('dTipo').value=x.tipo==='rele'?'rele':'led';$('dAct').value=x.activoAlto?'alto':'bajo';$('dAlias').value=x.alias||'';$('dHab').checked=x.habilitado;fillPins(x.pin);
 $('dNombre').scrollIntoView({behavior:'smooth',block:'center'});}
async function guardarDisp(){const j=await post('/api/disp/guardar',{id:$('dId').value,nombre:$('dNombre').value,tipo:$('dTipo').value,pin:$('dPin').value,activo:$('dAct').value,alias:$('dAlias').value,habilitado:$('dHab').checked?1:0});
 msg('msgDisp',j.ok?'Guardado ✓':(j.error||'Error'),j.ok?'ok':'mal');if(j.ok){await est();nuevoDisp();msg('msgDisp','Guardado ✓','ok');}}
async function borrar(id,q){if(!confirm(q))return;const j=await post('/api/disp/borrar',{id});if(!j.ok)alert(j.error);est();}
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
  ia["url"] = iaUrl();
  ia["modelo"] = iaModelo();
  ia["urlPropia"] = prefs.getString("ia_url", "") != "";
  ia["modeloPropio"] = prefs.getString("ia_modelo", "") != "";
  JsonArray arr = d["dispositivos"].to<JsonArray>();
  for (auto& x : disps) {
    JsonObject o = arr.add<JsonObject>();
    o["id"] = x.id; o["nombre"] = x.nombre; o["tipo"] = x.tipo; o["desc"] = x.desc; o["alias"] = x.alias;
    o["pin"] = x.pin; o["estado"] = x.estado; o["habilitado"] = x.habilitado; o["prox"] = x.prox;
    o["propio"] = x.propio; o["sobrescrito"] = x.sobrescrito; o["activoAlto"] = x.activoAlto;
    o["editable"] = (x.tipo == "led" || x.tipo == "rele") && !x.prox;
  }
  JsonArray libres = d["pinesLibres"].to<JsonArray>();
  for (int p : pinesPermitidos) {
    bool usado = false;
    for (auto& x : disps) if (x.habilitado && x.pin == p) usado = true;
    if (!usado) libres.add(p);
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
  iniciarProbe(ssid, server.arg("pass"), server.arg("oculta") == "1");
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
  if (server.arg("ghtoken") != "") { prefs.putString("ghtoken", server.arg("ghtoken")); logear("Token de GitHub guardado"); }
  if (nueva != "")                 { prefs.putString("clave", nueva); logear("Clave del panel cambiada"); }
  responderSimple(true);
}

void rutaIa() {
  REQUIERE_LOGIN();
  String url = server.arg("url");
  url.trim();
  if (url != "" && !url.startsWith("https://") && !url.startsWith("http://")) { responderSimple(false, "La URL debe empezar con https://"); return; }
  String modelo = server.arg("modelo");
  modelo.trim();
  prefs.putString("ia_url", url);
  prefs.putString("ia_modelo", modelo);
  if (server.arg("apikey") != "") { prefs.putString("apikey", server.arg("apikey")); logear("API key guardada"); }
  logear("IA: " + iaModelo() + " @ " + iaUrl());
  responderSimple(true);
}

void rutaIaModelos() {
  REQUIERE_LOGIN();
  String key = prefs.getString("apikey", "");
  if (key == "") { responderSimple(false, "Falta la API key"); return; }
  if (WiFi.status() != WL_CONNECTED || !hayInternet) { responderSimple(false, "Sin internet"); return; }
  WiFiClientSecure c;
  c.setInsecure();
  HTTPClient http;
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(c, iaUrl() + "/models")) { responderSimple(false, "URL no valida"); return; }
  http.addHeader("Authorization", "Bearer " + key);
  int code = http.GET();
  String resp = (code > 0) ? http.getString() : "";
  http.end();
  if (code != 200) { responderSimple(false, code > 0 ? "El servicio respondio " + String(code) + ": " + recorte(resp, 120) : "No pude conectar: " + HTTPClient::errorToString(code)); return; }
  JsonDocument filtro;
  filtro["data"][0]["id"] = true;
  JsonDocument r;
  if (deserializeJson(r, resp, DeserializationOption::Filter(filtro))) { responderSimple(false, "No entendi la lista de modelos"); return; }
  JsonDocument d;
  d["ok"] = true;
  JsonArray arr = d["modelos"].to<JsonArray>();
  int n = 0;
  for (JsonObject o : r["data"].as<JsonArray>()) { arr.add((const char*)(o["id"] | "")); if (++n >= 40) break; }
  enviarJson(d);
}

void rutaIaProbar() {
  REQUIERE_LOGIN();
  String out, err;
  if (pedirIA("Eres un asistente. Responde en español con una frase muy corta.", false, "Di: listo, te escucho.", out, err)) {
    JsonDocument d;
    d["ok"] = true;
    d["respuesta"] = recorte(out, 200);
    enviarJson(d);
    logear("Prueba de IA correcta");
  } else {
    logear("Prueba de IA fallo: " + err);
    responderSimple(false, err);
  }
}

void rutaChat() {
  REQUIERE_LOGIN();
  String texto = server.arg("mensaje");
  texto.trim();
  if (texto == "") { responderSimple(false, "Escribe un mensaje"); return; }
  if (texto.length() > 300) texto = texto.substring(0, 300);

  String contenido, err;
  if (!pedirIA(promptSistema(), true, texto, contenido, err)) { responderSimple(false, err); logear("Chat fallo: " + err); return; }

  // Extraer el JSON de la respuesta (por si viene con texto o ```json)
  String respuesta = contenido;
  String accTxt = "";
  int a = contenido.indexOf('{'), b = contenido.lastIndexOf('}');
  if (a >= 0 && b > a) {
    JsonDocument r;
    if (!deserializeJson(r, contenido.substring(a, b + 1))) {
      String rr = (const char*)(r["respuesta"] | "");
      if (rr != "") respuesta = rr;
      if (r["acciones"].is<JsonArray>()) {
        for (JsonObject ac : r["acciones"].as<JsonArray>()) {
          Disp* d = buscarDisp((const char*)(ac["id"] | ""));
          String est = (const char*)(ac["estado"] | "");
          if (d && d->habilitado && !d->prox && d->tipo != "boton" && (est == "encendido" || est == "apagado")) {
            escribirDisp(*d, est == "encendido");
            if (accTxt != "") accTxt += " · ";
            accTxt += d->nombre + " → " + est;
            logear("IA: " + d->nombre + " " + est);
          }
        }
      }
    }
  }
  if (accTxt != "") accTxt += " · enviada al ESP32-S3";

  agregarContexto("user", texto);
  agregarContexto("assistant", contenido);
  agregarChat("user", texto, "");
  agregarChat("bot", respuesta, accTxt);

  JsonDocument d;
  d["ok"] = true;
  d["respuesta"] = respuesta;
  d["acciones"] = accTxt;
  enviarJson(d);
}

void rutaChatHist() {
  REQUIERE_LOGIN();
  JsonDocument d;
  JsonArray arr = d.to<JsonArray>();
  for (auto& m : chatUI) {
    JsonObject o = arr.add<JsonObject>();
    o["rol"] = m.rol; o["txt"] = m.txt; o["acc"] = m.acc;
  }
  enviarJson(d);
}

void rutaDispGuardar() {
  REQUIERE_LOGIN();
  String nombre = server.arg("nombre");
  nombre.trim();
  String tipo = server.arg("tipo");
  int pin = server.arg("pin").toInt();
  bool alto = server.arg("activo") != "bajo";
  bool hab = server.arg("habilitado") == "1";
  String id = server.arg("id");

  if (nombre == "" || nombre.length() > 30) { responderSimple(false, "Escribe un nombre (maximo 30 letras)"); return; }
  if (tipo != "led" && tipo != "rele") { responderSimple(false, "Tipo no valido"); return; }
  if (!pinPermitido(pin)) { responderSimple(false, "El GPIO " + String(pin) + " no esta en la lista de pines permitidos"); return; }
  for (auto& d : disps) {
    if (d.id != id && d.habilitado && d.pin == pin && hab) { responderSimple(false, "El GPIO " + String(pin) + " ya lo usa: " + d.nombre); return; }
  }
  if (id != "") {
    Disp* ex = buscarDisp(id);
    if (!ex) { responderSimple(false, "Ese aparato no existe"); return; }
    if (ex->tipo == "boton" || ex->prox) { responderSimple(false, "Ese aparato no se puede editar aqui"); return; }
  } else {
    if ((int)locales.size() >= MAX_LOCALES) { responderSimple(false, "Maximo " + String(MAX_LOCALES) + " dispositivos propios"); return; }
    id = slug(nombre);
    String base = id;
    int n = 2;
    while (buscarDisp(id)) { id = base + "_" + String(n++); }
  }

  Disp nuevo;
  nuevo.id = id; nuevo.nombre = nombre; nuevo.tipo = tipo; nuevo.pin = pin;
  nuevo.activoAlto = alto; nuevo.habilitado = hab; nuevo.alias = server.arg("alias");
  bool reemplazado = false;
  for (auto& l : locales) if (l.id == id) { l = nuevo; reemplazado = true; }
  if (!reemplazado) locales.push_back(nuevo);
  guardarLocales();
  reconstruir();
  logear("Dispositivo guardado: " + nombre + " (GPIO " + String(pin) + ")");
  responderSimple(true);
}

void rutaDispBorrar() {
  REQUIERE_LOGIN();
  String id = server.arg("id");
  bool quitado = false;
  for (size_t i = 0; i < locales.size(); i++) {
    if (locales[i].id == id) { locales.erase(locales.begin() + i); quitado = true; break; }
  }
  if (!quitado) { responderSimple(false, "Ese aparato viene de GitHub: se borra editando config/dispositivos.json"); return; }
  guardarLocales();
  reconstruir();
  logear("Dispositivo eliminado: " + id);
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
  server.on("/api/ia", HTTP_POST, rutaIa);
  server.on("/api/ia/modelos", HTTP_GET, rutaIaModelos);
  server.on("/api/ia/probar", HTTP_POST, rutaIaProbar);
  server.on("/api/chat", HTTP_POST, rutaChat);
  server.on("/api/chat/hist", HTTP_GET, rutaChatHist);
  server.on("/api/disp/guardar", HTTP_POST, rutaDispGuardar);
  server.on("/api/disp/borrar", HTTP_POST, rutaDispBorrar);
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
  Serial.println("\n=== Viernes v0.5 ===");

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
