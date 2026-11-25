/*
  ============================================================
  VIERNES - Firmware v0.3  (ESP32-S3)
  ============================================================
  Que hace:
   1) Al arrancar intenta conectarse al WiFi guardado.
   2) Si NO hay WiFi guardado o falla -> crea la red "Viernes-Setup"
      (abre http://192.168.4.1) para que elijas tu WiFi.
   3) Si hay WiFi + internet -> descarga la configuracion de GitHub
      (dispositivos.json y ajustes.json) y la guarda como copia.
   4) Si hay WiFi pero NO internet -> usa la ultima copia guardada.
   5) Sirve un panel web moderno: http://viernes.local  (o la IP).
   6) Lee el boton (GPIO 5) y controla el LED (GPIO 4) y los aparatos.

  SEGURIDAD:
   - Aqui NO hay ninguna clave. El WiFi, el token de GitHub y la API key
     se escriben desde el panel y se guardan en la memoria NVS del ESP32.
   - El panel nunca muestra esas claves de vuelta.

  Librerias: ArduinoJson (v7). El resto viene con el paquete ESP32.
  Placa en Arduino IDE: "ESP32S3 Dev Module".
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

// ---------------- AJUSTES DEL PROYECTO (publicos, sin claves) ----------------
const char* GH_USUARIO = "Sebastixpas";
const char* GH_REPO    = "Viernes-Home";
const char* GH_RAMA    = "main";
const char* AP_NOMBRE  = "Viernes-Setup";    // red que crea si no hay WiFi
const char* NOMBRE_MDNS = "viernes";         // http://viernes.local
const char* PIN_POR_DEFECTO = "1234";        // PIN del panel (cambialo en Ajustes)
const int   PIN_LED_DEFECTO   = 4;
const int   PIN_BOTON_DEFECTO = 5;
const unsigned long TIEMPO_WIFI_MS = 15000;  // espera por intento
const int   INTENTOS_WIFI = 3;

// ---------------- ESTADO GENERAL ----------------
enum Modo { MODO_CONFIG, MODO_COMPLETO, MODO_LOCAL };
Modo modo = MODO_CONFIG;

Preferences prefs;
WebServer server(80);
DNSServer dns;

struct Disp {
  String id, nombre, tipo;
  int pin = -1;
  bool activoAlto = true;
  bool habilitado = true;
  bool estado = false;
};
std::vector<Disp> disps;
std::vector<int> pinesPermitidos;

String ajIdioma = "es-419", ajProveedor = "anthropic", ajModelo = "claude-haiku-4-5";
int ajVolumen = 70, ajSyncMin = 30, versionDisp = 0, versionAj = 0;

bool hayInternet = false;
bool escuchando = false;
unsigned long ultimaSyncMs = 0;     // 0 = nunca
unsigned long ultimoChequeoMs = 0;
unsigned long ultimoIntentoApMs = 0;
int pinBoton = PIN_BOTON_DEFECTO;
bool botonActivoAlto = false;

// Boton con filtro de rebotes
bool botLecturaPrev = HIGH, botEstable = HIGH;
unsigned long botCambioMs = 0;

// Registro en memoria (ultimos eventos)
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
  if (d.pin >= 0 && d.tipo != "boton") digitalWrite(d.pin, (on == d.activoAlto) ? HIGH : LOW);
}

Disp* buscarDisp(const String& id) {
  for (auto& d : disps) if (d.id == id) return &d;
  return nullptr;
}

Disp* primerLed() {
  for (auto& d : disps) if (d.tipo == "led" && d.habilitado) return &d;
  return nullptr;
}

// ---------------- CONFIGURACION (JSON) ----------------
const char* DISP_POR_DEFECTO =
  "{\"version\":0,\"pines_permitidos\":[4,5],\"dispositivos\":["
  "{\"id\":\"led_prueba\",\"nombre\":\"LED de prueba\",\"pin\":4,\"tipo\":\"led\",\"activo_en\":\"alto\",\"habilitado\":true},"
  "{\"id\":\"boton_escucha\",\"nombre\":\"Boton de escucha\",\"pin\":5,\"tipo\":\"boton\",\"activo_en\":\"bajo\",\"habilitado\":true}]}";

bool aplicarDispositivos(const String& json) {
  JsonDocument doc;
  if (deserializeJson(doc, json)) return false;
  if (!doc["dispositivos"].is<JsonArray>()) return false;

  std::vector<Disp> nuevos;
  std::vector<int> permitidos;
  for (JsonVariant p : doc["pines_permitidos"].as<JsonArray>()) permitidos.push_back(p.as<int>());
  pinesPermitidos = permitidos;

  for (JsonObject o : doc["dispositivos"].as<JsonArray>()) {
    Disp d;
    d.id = o["id"] | "";
    d.nombre = o["nombre"] | d.id;
    d.tipo = o["tipo"] | "led";
    d.pin = o["pin"] | -1;
    d.activoAlto = String((const char*)(o["activo_en"] | "alto")) == "alto";
    d.habilitado = o["habilitado"] | true;
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

  // Configurar pines
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
  cliente.setInsecure();   // Sin validar certificado (simple para aprender)
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
  String d, a;
  bool okD = descargar(urlRaw("config/dispositivos.json"), d);
  if (!okD) { hayInternet = false; return false; }
  hayInternet = true;
  bool okA = descargar(urlRaw("config/ajustes.json"), a);

  if (aplicarDispositivos(d)) { guardarArchivo("/dispositivos.json", d); }
  else logear("dispositivos.json descargado no es valido");
  if (okA && aplicarAjustes(a)) { guardarArchivo("/ajustes.json", a); }

  ultimaSyncMs = millis();
  logear("Sincronizado con GitHub (disp v" + String(versionDisp) + ", ajustes v" + String(versionAj) + ")");
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

// Guarda el estado de los aparatos en GitHub (datos/estado.json) con el token de NVS
String guardarEnGitHub() {
  String token = prefs.getString("ghtoken", "");
  if (token == "") return "Falta el token de GitHub (Ajustes)";
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
  WiFiClientSecure cliente;
  cliente.setInsecure();

  // 1) Buscar el sha del archivo si ya existe
  String sha = "";
  {
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

  // 2) Crear o actualizar
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
bool conectarWiFi() {
  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");
  if (ssid == "") return false;
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < TIEMPO_WIFI_MS) delay(250);
  return WiFi.status() == WL_CONNECTED;
}

void iniciarAP() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_NOMBRE);   // red abierta para configurar
  dns.start(53, "*", WiFi.softAPIP());
  logear("Modo configuracion: red " + String(AP_NOMBRE) + " -> http://192.168.4.1");
}

// ---------------- PANEL WEB ----------------
const char PAGINA[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="es"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Viernes</title>
<style>
:root{--bg:#0b0f1a;--card:#141b2d;--card2:#1b2540;--txt:#e8ecf8;--mut:#8b96b5;--ac:#6c8cff;--ok:#36d399;--bad:#ff6b6b;--warn:#fbbd23}
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:system-ui,Segoe UI,Arial,sans-serif;background:var(--bg);color:var(--txt);padding-bottom:84px}
header{padding:22px 18px 8px}
h1{font-size:26px;letter-spacing:.5px}
h1 span{color:var(--ac)}
.sub{color:var(--mut);font-size:14px;margin-top:4px}
.chips{display:flex;flex-wrap:wrap;gap:8px;padding:8px 18px}
.chip{background:var(--card);border-radius:999px;padding:6px 12px;font-size:13px;display:flex;align-items:center;gap:6px}
.dot{width:9px;height:9px;border-radius:50%;background:var(--mut)}
.on{background:var(--ok)}.off{background:var(--bad)}.mid{background:var(--warn)}
main{padding:8px 18px;max-width:760px;margin:auto}
.card{background:var(--card);border-radius:18px;padding:16px;margin:12px 0}
.card h2{font-size:15px;color:var(--mut);margin-bottom:10px;font-weight:600}
.row{display:flex;align-items:center;justify-content:space-between;padding:12px 0;border-top:1px solid #222c48}
.row:first-of-type{border-top:0}
.nom{font-size:16px}.pie{font-size:12px;color:var(--mut);margin-top:2px}
.sw{width:56px;height:30px;border-radius:999px;background:#2a3452;position:relative;cursor:pointer;flex:none;transition:.2s}
.sw::after{content:"";position:absolute;top:3px;left:3px;width:24px;height:24px;border-radius:50%;background:#fff;transition:.2s}
.sw.act{background:var(--ok)}.sw.act::after{left:29px}
.sw.dis{opacity:.4;pointer-events:none}
.big{font-size:34px;font-weight:700}
nav{position:fixed;bottom:0;left:0;right:0;background:var(--card);display:flex;justify-content:space-around;padding:10px 0 14px;border-top:1px solid #222c48}
nav button{background:none;border:0;color:var(--mut);font-size:13px;display:flex;flex-direction:column;align-items:center;gap:3px;padding:4px 14px;cursor:pointer}
nav button.sel{color:var(--ac)}
nav .ic{font-size:20px}
input,select{width:100%;background:var(--card2);border:1px solid #2a3452;color:var(--txt);border-radius:12px;padding:12px;font-size:15px;margin:6px 0 12px}
label{font-size:13px;color:var(--mut)}
.btn{background:var(--ac);color:#fff;border:0;border-radius:12px;padding:12px 16px;font-size:15px;cursor:pointer;width:100%;margin-top:4px}
.btn.sec{background:var(--card2)}
.msg{font-size:13px;margin-top:8px;color:var(--warn);min-height:18px}
.log{font-family:ui-monospace,Consolas,monospace;font-size:12px;color:#b8c2e0;line-height:1.6;white-space:pre-wrap}
.tab{display:none}.tab.show{display:block}
</style></head><body>
<header><h1>Viernes<span>.</span></h1><div class="sub" id="sub">Cargando...</div></header>
<div class="chips" id="chips"></div>
<main>
 <section class="tab show" id="t-inicio">
  <div class="card"><h2>MODO DEL ASISTENTE</h2><div class="big" id="esc">-</div>
   <div class="pie" id="escSub"></div></div>
  <div class="card"><h2>APARATOS</h2><div id="lista1"></div></div>
 </section>
 <section class="tab" id="t-disp">
  <div class="card"><h2>DISPOSITIVOS DE LA CONFIGURACION</h2><div id="lista2"></div></div>
  <div class="card"><h2>SINCRONIZACION</h2><div class="pie" id="syncInfo"></div>
   <button class="btn" onclick="sync()">Actualizar config desde GitHub</button><div class="msg" id="msgSync"></div></div>
 </section>
 <section class="tab" id="t-ia">
  <div class="card"><h2>ASISTENTE DE IA</h2>
   <div class="row"><div class="nom">Proveedor</div><div id="iaProv">-</div></div>
   <div class="row"><div class="nom">Modelo</div><div id="iaMod">-</div></div>
   <div class="row"><div class="nom">Idioma</div><div id="iaIdi">-</div></div>
   <div class="row"><div class="nom">Volumen</div><div id="iaVol">-</div></div>
   <div class="row"><div class="nom">API key</div><div id="iaKey">-</div></div>
   <div class="pie" style="margin-top:10px">Estos valores se cambian editando config/ajustes.json en GitHub. La API key se escribe en Ajustes.</div></div>
 </section>
 <section class="tab" id="t-aj">
  <div class="card"><h2>PIN DEL PANEL</h2>
   <label>Escribe tu PIN para guardar cambios (por defecto 1234)</label>
   <input id="pin" type="password" inputmode="numeric" placeholder="PIN"></div>
  <div class="card"><h2>WIFI</h2>
   <label>Red</label><select id="ssid"></select>
   <label>Contrasena</label><input id="pass" type="password" placeholder="Contrasena del WiFi">
   <button class="btn" onclick="guardarWifi()">Guardar WiFi y reiniciar</button>
   <button class="btn sec" onclick="buscar()" style="margin-top:8px">Buscar redes</button>
   <div class="msg" id="msgWifi"></div></div>
  <div class="card"><h2>CLAVES (se guardan solo en el ESP32)</h2>
   <label>API key de la IA</label><input id="apikey" type="password" placeholder="(vacio = no cambiar)">
   <label>Token de GitHub (solo para guardar datos)</label><input id="ghtoken" type="password" placeholder="(vacio = no cambiar)">
   <label>Nuevo PIN del panel</label><input id="newpin" type="password" placeholder="(vacio = no cambiar)">
   <button class="btn" onclick="guardarClaves()">Guardar</button><div class="msg" id="msgCl"></div></div>
  <div class="card"><h2>BASE DE DATOS EN GITHUB</h2>
   <div class="pie">Guarda el estado de los aparatos en datos/estado.json (necesita token).</div>
   <button class="btn" onclick="guardarGh()">Guardar estado en GitHub</button><div class="msg" id="msgGh"></div></div>
  <div class="card"><h2>REGISTRO</h2><div class="log" id="log"></div></div>
 </section>
</main>
<nav>
 <button class="sel" onclick="ir('inicio',this)"><span class="ic">&#127968;</span>Inicio</button>
 <button onclick="ir('disp',this)"><span class="ic">&#128268;</span>Aparatos</button>
 <button onclick="ir('ia',this)"><span class="ic">&#129302;</span>IA</button>
 <button onclick="ir('aj',this)"><span class="ic">&#9881;</span>Ajustes</button>
</nav>
<script>
const $=id=>document.getElementById(id);
function ir(n,b){document.querySelectorAll('.tab').forEach(t=>t.classList.remove('show'));$('t-'+n).classList.add('show');
 document.querySelectorAll('nav button').forEach(x=>x.classList.remove('sel'));b.classList.add('sel');if(n==='aj')buscar();}
function lista(e,d){return d.map(x=>`<div class="row"><div><div class="nom">${x.nombre}</div><div class="pie">${x.tipo} &middot; pin ${x.pin}${x.habilitado?'':' &middot; desactivado'}</div></div>
 ${x.tipo==='boton'?`<div class="pie">${x.estado?'hundido':'suelto'}</div>`:`<div class="sw ${x.estado?'act':''} ${x.habilitado?'':'dis'}" onclick="tog('${x.id}',${x.estado?0:1})"></div>`}</div>`).join('')||'<div class="pie">Sin aparatos</div>';}
async function est(){try{const r=await fetch('/api/estado');const s=await r.json();
 const m={config:'Modo configuracion',completo:'Completo (WiFi + internet)',local:'Local (sin internet)'}[s.modo];
 $('sub').textContent=m+' \u00b7 '+s.ip;
 $('chips').innerHTML=`<div class="chip"><span class="dot ${s.wifi?'on':'off'}"></span>WiFi ${s.wifi?s.ssid:'sin conexion'}</div>
 <div class="chip"><span class="dot ${s.internet?'on':'off'}"></span>Internet</div>
 <div class="chip"><span class="dot ${s.modo==='completo'?'on':s.modo==='local'?'mid':'off'}"></span>${s.modo}</div>`;
 $('esc').textContent=s.escuchando?'Escuchando':'Dormido';$('esc').style.color=s.escuchando?'#36d399':'#8b96b5';
 $('escSub').textContent='Controlado por el boton fisico (GPIO '+s.pinBoton+')';
 $('lista1').innerHTML=lista(0,s.dispositivos.filter(x=>x.habilitado));$('lista2').innerHTML=lista(0,s.dispositivos);
 $('syncInfo').textContent=s.hace<0?'Todavia no se sincronizo. Config v'+s.versionDisp:'Ultima sincronizacion hace '+s.hace+' s. Config v'+s.versionDisp+', ajustes v'+s.versionAj;
 $('iaProv').textContent=s.ia.proveedor;$('iaMod').textContent=s.ia.modelo||'-';$('iaIdi').textContent=s.ia.idioma;$('iaVol').textContent=s.ia.volumen+'%';
 $('iaKey').textContent=s.apiKey?'guardada \u2022\u2022\u2022\u2022\u2022':'sin guardar';
 $('log').textContent=s.log.join('\n');}catch(e){$('sub').textContent='Sin conexion con el ESP32'}}
async function tog(id,on){await fetch('/api/toggle?id='+id+'&on='+on,{method:'POST'});est();}
async function sync(){$('msgSync').textContent='Sincronizando...';const r=await fetch('/api/sync',{method:'POST'});const j=await r.json();$('msgSync').textContent=j.ok?'Listo, configuracion actualizada':'No se pudo (sin internet?)';est();}
async function buscar(){const s=$('ssid');s.innerHTML='<option>Buscando...</option>';try{const r=await fetch('/api/redes');const l=await r.json();
 s.innerHTML=l.map(n=>`<option value="${n.ssid}">${n.ssid} (${n.rssi} dBm)</option>`).join('')||'<option value="">No se encontraron redes</option>';}catch(e){s.innerHTML='<option value="">Error</option>'}}
function post(url,data){return fetch(url,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(data)}).then(r=>r.json());}
async function guardarWifi(){const j=await post('/api/wifi',{pin:$('pin').value,ssid:$('ssid').value,pass:$('pass').value});
 $('msgWifi').textContent=j.ok?'Guardado. Reiniciando... reconectate a tu WiFi y abre http://viernes.local':(j.error||'Error');}
async function guardarClaves(){const j=await post('/api/claves',{pin:$('pin').value,apikey:$('apikey').value,ghtoken:$('ghtoken').value,newpin:$('newpin').value});
 $('msgCl').textContent=j.ok?'Guardado':(j.error||'Error');if(j.ok){$('apikey').value='';$('ghtoken').value='';$('newpin').value='';}est();}
async function guardarGh(){$('msgGh').textContent='Guardando...';const j=await post('/api/guardar',{pin:$('pin').value});$('msgGh').textContent=j.ok?'Guardado en GitHub':(j.error||'Error');}
est();setInterval(est,3000);
</script></body></html>
)rawliteral";

bool pinOk() {
  String guardado = prefs.getString("pin", PIN_POR_DEFECTO);
  return server.arg("pin") == guardado;
}

void enviarJson(JsonDocument& doc, int codigo = 200) {
  String s;
  serializeJson(doc, s);
  server.send(codigo, "application/json", s);
}

void responderSimple(bool ok, const char* error = "") {
  JsonDocument d;
  d["ok"] = ok;
  if (!ok) d["error"] = error;
  enviarJson(d);
}

void rutaEstado() {
  JsonDocument d;
  d["modo"] = (modo == MODO_CONFIG) ? "config" : (modo == MODO_COMPLETO ? "completo" : "local");
  d["wifi"] = (WiFi.status() == WL_CONNECTED);
  d["ssid"] = WiFi.SSID();
  d["ip"] = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  d["internet"] = hayInternet;
  d["escuchando"] = escuchando;
  d["pinBoton"] = pinBoton;
  d["hace"] = ultimaSyncMs == 0 ? -1 : (long)((millis() - ultimaSyncMs) / 1000);
  d["versionDisp"] = versionDisp;
  d["versionAj"] = versionAj;
  d["apiKey"] = prefs.getString("apikey", "") != "";
  JsonObject ia = d["ia"].to<JsonObject>();
  ia["proveedor"] = ajProveedor;
  ia["modelo"] = ajModelo;
  ia["idioma"] = ajIdioma;
  ia["volumen"] = ajVolumen;
  JsonArray arr = d["dispositivos"].to<JsonArray>();
  for (auto& x : disps) {
    JsonObject o = arr.add<JsonObject>();
    o["id"] = x.id; o["nombre"] = x.nombre; o["tipo"] = x.tipo;
    o["pin"] = x.pin; o["estado"] = x.estado; o["habilitado"] = x.habilitado;
  }
  JsonArray lg = d["log"].to<JsonArray>();
  for (int i = (int)registro.size() - 1; i >= 0; i--) lg.add(registro[i]);
  enviarJson(d);
}

void rutaToggle() {
  Disp* x = buscarDisp(server.arg("id"));
  if (!x || !x->habilitado || x->tipo == "boton") { responderSimple(false, "Aparato no valido"); return; }
  escribirDisp(*x, server.arg("on") == "1");
  logear(x->nombre + (x->estado ? " encendido" : " apagado"));
  responderSimple(true);
}

void rutaRedes() {
  int n = WiFi.scanNetworks();
  JsonDocument d;
  JsonArray arr = d.to<JsonArray>();
  for (int i = 0; i < n && i < 15; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["ssid"] = WiFi.SSID(i);
    o["rssi"] = WiFi.RSSI(i);
  }
  WiFi.scanDelete();
  enviarJson(d);
}

void rutaWifi() {
  if (!pinOk()) { responderSimple(false, "PIN incorrecto"); return; }
  String ssid = server.arg("ssid");
  if (ssid == "") { responderSimple(false, "Elige una red"); return; }
  prefs.putString("ssid", ssid);
  prefs.putString("pass", server.arg("pass"));
  responderSimple(true);
  logear("WiFi guardado: reiniciando");
  delay(800);
  ESP.restart();
}

void rutaClaves() {
  if (!pinOk()) { responderSimple(false, "PIN incorrecto"); return; }
  if (server.arg("apikey") != "")  { prefs.putString("apikey", server.arg("apikey")); logear("API key guardada"); }
  if (server.arg("ghtoken") != "") { prefs.putString("ghtoken", server.arg("ghtoken")); logear("Token de GitHub guardado"); }
  if (server.arg("newpin") != "")  { prefs.putString("pin", server.arg("newpin")); logear("PIN cambiado"); }
  responderSimple(true);
}

void rutaSync() {
  bool ok = sincronizar();
  if (ok) modo = MODO_COMPLETO;
  else if (WiFi.status() == WL_CONNECTED) modo = MODO_LOCAL;
  responderSimple(ok, "Sin internet");
}

void rutaGuardar() {
  if (!pinOk()) { responderSimple(false, "PIN incorrecto"); return; }
  String r = guardarEnGitHub();
  responderSimple(r == "OK", r.c_str());
}

void iniciarWeb() {
  server.on("/", HTTP_GET, []() { server.send_P(200, "text/html; charset=utf-8", PAGINA); });
  server.on("/api/estado", HTTP_GET, rutaEstado);
  server.on("/api/toggle", HTTP_POST, rutaToggle);
  server.on("/api/redes", HTTP_GET, rutaRedes);
  server.on("/api/wifi", HTTP_POST, rutaWifi);
  server.on("/api/claves", HTTP_POST, rutaClaves);
  server.on("/api/sync", HTTP_POST, rutaSync);
  server.on("/api/guardar", HTTP_POST, rutaGuardar);
  server.onNotFound([]() {   // portal cautivo: cualquier direccion lleva al panel
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "");
  });
  server.begin();
}

// ---------------- BOTON ----------------
void aplicarEscucha(bool nuevo) {
  escuchando = nuevo;
  Disp* led = primerLed();
  if (led) escribirDisp(*led, escuchando);
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
  Serial.println("\n=== Viernes v0.3 ===");

  prefs.begin("viernes", false);
  if (!LittleFS.begin(true)) logear("No se pudo iniciar LittleFS");

  cargarCopiaLocal();

  // Estado inicial del boton
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

  unsigned long ahora = millis();

  // Si estamos en configuracion pero hay WiFi guardado, reintenta cada 2 minutos
  if (modo == MODO_CONFIG && prefs.getString("ssid", "") != "") {
    if (WiFi.status() == WL_CONNECTED) { logear("WiFi disponible: reiniciando"); delay(500); ESP.restart(); }
    if (ahora - ultimoIntentoApMs > 120000UL) {
      ultimoIntentoApMs = ahora;
      WiFi.begin(prefs.getString("ssid", "").c_str(), prefs.getString("pass", "").c_str());
    }
  }

  // Cada minuto: revisar conexion y sincronizar si toca
  if (modo != MODO_CONFIG && ahora - ultimoChequeoMs > 60000UL) {
    ultimoChequeoMs = ahora;
    if (WiFi.status() != WL_CONNECTED) {
      hayInternet = false;
      modo = MODO_LOCAL;
      logear("WiFi caido: reconectando");
      WiFi.reconnect();
    } else {
      bool tocaSync = (ultimaSyncMs == 0) || (ahora - ultimaSyncMs > (unsigned long)ajSyncMin * 60000UL);
      if (modo == MODO_LOCAL || tocaSync) {
        modo = sincronizar() ? MODO_COMPLETO : MODO_LOCAL;
      }
    }
  }
}
