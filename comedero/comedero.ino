// ===== Cat Feeder Automatico - ESP32 =====
#include <WiFi.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <ESP32Servo.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include "time.h"

#define RESET_KNOP   5
#define LED_R 10
#define LED_G 12
#define LED_B 11
#define SERVO_PIN 18

#define MAX_MOMENTEN 5
#define SERVO_CENTER 90

WebServer server(80);
WiFiManager wm;                 // GLOBAL: si es local, ~WiFiManager() destruye el portal al salir de setup()
Servo mijnServo;
Preferences prefs;

const char* ntpServer = "pool.ntp.org";
const char* apName    = "CatFeeder";

int snelheid = 30;
int duur = 2;
int voerUur[MAX_MOMENTEN];
int voerMin[MAX_MOMENTEN];
int aantalMomenten = 2;

bool serverActivo = false;
bool mdnsActivo  = false;
int  laatsteMinuut = -1;
uint32_t wifiCaidoTijd = 0;

// --- Maquina de estados del servo (NO bloqueante) ---
enum FeedState { FEED_IDLE, FEED_VOORUIT, FEED_TERUG };
FeedState feedState = FEED_IDLE;
uint32_t   feedTijd = 0;
int        feedStappen = 0;
int        feedStap = 0;
int        servoVooruit = SERVO_CENTER;
int        servoAchteruit = SERVO_CENTER;

// --- Boton de reset (NO bloqueante) ---
bool     knopIngedrukt = false;
uint32_t knopTijd = 0;
bool     resetGedaan = false;

void setKleur(bool r, bool g, bool b) {
  digitalWrite(LED_R, r ? LOW : HIGH);
  digitalWrite(LED_G, g ? LOW : HIGH);
  digitalWrite(LED_B, b ? LOW : HIGH);
}

// ---------- ALIMENTACION (no bloqueante) ----------

void startVoeren() {
  if (feedState != FEED_IDLE) return;          // ya esta alimentando

  snelheid = constrain(snelheid, 0, 100);
  duur    = constrain(duur, 1, 10);

  servoVooruit  = SERVO_CENTER - (int)(snelheid * 0.3);
  servoAchteruit = SERVO_CENTER + (int)(snelheid * 0.3);

  feedStappen = duur * 2;
  feedStap = 0;
  feedTijd = millis();
  feedState = FEED_VOORUIT;
  mijnServo.write(servoVooruit);
  setKleur(0, 0, 1);
}

void updateVoeren() {
  if (feedState == FEED_IDLE) return;

  uint32_t nu = millis();

  if (feedState == FEED_VOORUIT) {
    if (nu - feedTijd < 400) return;
    mijnServo.write(servoAchteruit);
    feedState = FEED_TERUG;
    feedTijd = nu;
  } else if (feedState == FEED_TERUG) {
    if (nu - feedTijd < 150) return;
    feedStap++;
    if (feedStap >= feedStappen) {
      mijnServo.write(SERVO_CENTER);
      feedState = FEED_IDLE;
      setKleur(1, 0, 1);
    } else {
      mijnServo.write(servoVooruit);
      feedState = FEED_VOORUIT;
      feedTijd = nu;
    }
  }
}

// ---------- BOTON DE RESET (no bloqueante) ----------

void checkResetKnop() {
  bool ingedrukt = (digitalRead(RESET_KNOP) == LOW);
  uint32_t nu = millis();

  if (!ingedrukt) {
    knopIngedrukt = false;
    return;
  }

  if (!knopIngedrukt) {          // flanco de pulsacion
    knopIngedrukt = true;
    knopTijd = nu;
    setKleur(1, 1, 0);           // avisar: manteniendo pulsado
    return;
  }

  if (!resetGedaan && (nu - knopTijd) >= 5000) {
    resetGedaan = true;
    setKleur(0, 1, 1);
    Serial.println("RESET: borrando configuracion y credenciales WiFi...");
    delay(50);

    prefs.begin("settings", false);
    prefs.clear();
    prefs.end();

    wm.resetSettings();          // borra las credenciales WiFi de la NVS
    delay(100);
    Serial.println("Reiniciando...");
    ESP.restart();
  }
}

// ---------- HORARIO ----------

void checkVoerTijd() {
  static uint32_t laatsteCheck = 0;
  if (millis() - laatsteCheck < 1000) return;   // comprobar 1 vez por segundo
  laatsteCheck = millis();

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 100)) return;    // aun sin hora NTP (sin spam)

  int huidig = timeinfo.tm_hour * 60 + timeinfo.tm_min;
  if (huidig == laatsteMinuut) return;          // ya evaluado este minuto
  laatsteMinuut = huidig;

  for (int i = 0; i < aantalMomenten; i++) {
    if ((voerUur[i] * 60 + voerMin[i]) == huidig) {
      Serial.println("Hora de alimentar!");
      startVoeren();
      break;
    }
  }
}

// ---------- PAGINA WEB ----------

String pagina() {
  struct tm timeinfo;
  bool conHora = getLocalTime(&timeinfo, 50);
  String tijd = "--:--";
  if (conHora) {
    String h = "", m = "";
    if (timeinfo.tm_hour < 10) h += "0";
    h += String(timeinfo.tm_hour);
    if (timeinfo.tm_min < 10) m += "0";
    m += String(timeinfo.tm_min);
    tijd = h + ":" + m;
  }

  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
body {font-family:sans-serif;background:#0f172a;color:white;text-align:center;}
.card {background:#1e293b;padding:20px;margin:10px;border-radius:15px;}
button {padding:15px;border-radius:10px;border:none;background:#38bdf8;}
.row {display:flex;gap:5px;margin:5px;}
input {flex:1;padding:10px;border-radius:10px;border:none;}
</style>
</head>
<body>

<h2>SmartCat Feeder</h2>
<p>ESP32: <b id="espClock">)rawliteral" + tijd + R"rawliteral(</b> &nbsp; Movil: <span id="clock">--:--</span></p>
<p>IP: <b id="ip">--</b> &nbsp; <span id="status">conectando...</span></p>

<div class="card">
speed : <span id="speedVal">0</span>%<br>
<input type="range" id="speed" min="0" max="100">

<br><br>

duration : <span id="timeVal">0</span> sec<br>
<input type="range" id="time" min="1" max="10">
</div>

<div class="card">
feed moments
<div id="momenten"></div>
<button onclick="addMoment()"> + </button>
</div>

<div class="card">
<button onclick="feedNow()"> feed </button>
</div>

<script>

let count = )rawliteral" + String(aantalMomenten) + R"rawliteral(;

function saveAll(){
for(let i=0;i<count;i++){
  let u=document.getElementById("u"+i);
  let m=document.getElementById("m"+i);
  if(u&&m){
    window['u'+i]=u.value;
    window['m'+i]=m.value;
  }
}
}

function renderMomenten(){
let c=document.getElementById("momenten");
c.innerHTML="";
for(let i=0;i<count;i++){
let d=document.createElement("div");
d.className="row";
d.innerHTML=`
<input type="number" id="u${i}" min="0" max="23" value="${window['u'+i] !== undefined ? window['u'+i] : 0}" onchange="saveMoment(${i})">
<input type="number" id="m${i}" min="0" max="59" value="${window['m'+i] !== undefined ? window['m'+i] : 0}" onchange="saveMoment(${i})">
<button onclick="removeMoment(${i})"> X </button>
`;
c.appendChild(d);
}
}

function addMoment(){
saveAll();
if(count>=5)return;
window['u'+count]=12;
window['m'+count]=0;
count++;
renderMomenten();
sendData();
}

function removeMoment(i){
saveAll();
for(let x=i;x<count-1;x++){
window['u'+x]=window['u'+(x+1)];
window['m'+x]=window['m'+(x+1)];
}
count--;
renderMomenten();
setTimeout(sendData,50);
}

function saveMoment(i){
let u=document.getElementById("u"+i);
let m=document.getElementById("m"+i);
if(u&&m){
window['u'+i]=u.value;
window['m'+i]=m.value;
sendData();
}
}

function sendData(){
let url="/update?";
url+="speed="+speed.value;
url+="&time="+time.value;
url+="&count="+count;
for(let i=0;i<count;i++){
url+="&u"+i+"="+window['u'+i];
url+="&m"+i+"="+window['m'+i];
}
fetch(url);
}

function feedNow(){fetch("/api/feed").then(()=>{document.getElementById("status").innerText="Alimentando...";});}

let speed=document.getElementById("speed");
let time=document.getElementById("time");

let speedVal=document.getElementById("speedVal");
let timeVal=document.getElementById("timeVal");

function updateLabels(){
  speedVal.innerText = speed.value;
  timeVal.innerText = time.value;
}

// oninput solo actualiza la etiqueta; onchange (al soltar) guarda -> sin tormenta de escrituras en NVS
speed.oninput = function(){ updateLabels(); }
speed.onchange = function(){ updateLabels(); sendData(); }

time.oninput = function(){ updateLabels(); }
time.onchange = function(){ updateLabels(); sendData(); }

window.onload=function(){
speed.value=)rawliteral" + String(snelheid) + R"rawliteral(;
time.value=)rawliteral" + String(duur) + R"rawliteral(;

updateLabels();

)rawliteral";

  for (int i = 0; i < aantalMomenten; i++) {
    html += "window['u" + String(i) + "']=" + String(voerUur[i]) + ";";
    html += "window['m" + String(i) + "']=" + String(voerMin[i]) + ";";
  }

  html += R"rawliteral(

updateClock();
renderMomenten();
pollState();
}

function updateClock(){
  let now = new Date();
  let h = now.getHours().toString().padStart(2,'0');
  let m = now.getMinutes().toString().padStart(2,'0');
  document.getElementById("clock").innerText = h + ":" + m;
}

// Estado real del ESP32 (su hora es la que manda en el horario)
async function pollState(){
  try{
    const r = await fetch("/api/state",{cache:"no-store"});
    const s = await r.json();
    document.getElementById("espClock").innerText = s.hora;
    document.getElementById("ip").innerText = s.ip;
    document.getElementById("status").innerText = s.alimentando ? "Alimentando..." : ("Proximo: " + s.proximo);
  }catch(e){
    document.getElementById("status").innerText = "sin conexion con el ESP32";
  }
}

setInterval(updateClock,1000);
setInterval(pollState,2000);
</script>

</body>
</html>
)rawliteral";

  return html;
}

void handleRoot() { server.send(200, "text/html", pagina()); }

void handleUpdate() {
  bool gewijzigd = false;

  if (server.hasArg("speed")) {
    int v = constrain(server.arg("speed").toInt(), 0, 100);
    if (v != snelheid) { snelheid = v; gewijzigd = true; }
  }
  if (server.hasArg("time")) {
    int v = constrain(server.arg("time").toInt(), 1, 10);
    if (v != duur) { duur = v; gewijzigd = true; }
  }
  if (server.hasArg("count")) {
    int v = constrain(server.arg("count").toInt(), 1, MAX_MOMENTEN);
    if (v != aantalMomenten) { aantalMomenten = v; gewijzigd = true; }
  }

  int nieuweUur[MAX_MOMENTEN];
  int nieuweMin[MAX_MOMENTEN];
  for (int i = 0; i < MAX_MOMENTEN; i++) { nieuweUur[i] = voerUur[i]; nieuweMin[i] = voerMin[i]; }

  for (int i = 0; i < aantalMomenten; i++) {
    if (server.hasArg("u" + String(i))) {
      int v = constrain(server.arg("u" + String(i)).toInt(), 0, 23);
      if (v != nieuweUur[i]) { nieuweUur[i] = v; gewijzigd = true; }
    }
    if (server.hasArg("m" + String(i))) {
      int v = constrain(server.arg("m" + String(i)).toInt(), 0, 59);
      if (v != nieuweMin[i]) { nieuweMin[i] = v; gewijzigd = true; }
    }
  }

  if (gewijzigd) {
    for (int i = 0; i < aantalMomenten; i++) { voerUur[i] = nieuweUur[i]; voerMin[i] = nieuweMin[i]; }

    prefs.begin("settings", false);
    prefs.putInt("speed", snelheid);
    prefs.putInt("duur", duur);
    prefs.putInt("aantal", aantalMomenten);
    for (int i = 0; i < aantalMomenten; i++) {
      prefs.putInt(("u" + String(i)).c_str(), voerUur[i]);
      prefs.putInt(("m" + String(i)).c_str(), voerMin[i]);
    }
    prefs.end();

    Serial.printf("Guardado -> velocidad=%d duracion=%d horarios=", snelheid, duur);
    for (int i = 0; i < aantalMomenten; i++) Serial.printf("%02d:%02d ", voerUur[i], voerMin[i]);
    Serial.println();
  }

  server.send(200, "text/plain", "OK");
}

void handleFeed() {
  startVoeren();
  server.send(200, "text/plain", "OK");
}

// ---------- API JSON (contrato estable para cuando elijas la app) ----------

String dosDigitos(int v) {
  String s = "";
  if (v < 10) s += "0";
  s += String(v);
  return s;
}

String estadoJson() {
  struct tm timeinfo;
  bool conHora = getLocalTime(&timeinfo, 50);

  String hora = "--:--";
  int ahora = -1;
  if (conHora) {
    hora = dosDigitos(timeinfo.tm_hour) + ":" + dosDigitos(timeinfo.tm_min);
    ahora = timeinfo.tm_hour * 60 + timeinfo.tm_min;
  }

  // proximo horario programado
  String proximo = "--";
  if (ahora >= 0) {
    int mejorDiff = 24 * 60 + 1;
    for (int i = 0; i < aantalMomenten; i++) {
      int t = voerUur[i] * 60 + voerMin[i];
      int diff = t - ahora;
      if (diff < 0) diff += 24 * 60;
      if (diff < mejorDiff) { mejorDiff = diff; proximo = dosDigitos(voerUur[i]) + ":" + dosDigitos(voerMin[i]); }
    }
  }

  JsonDocument doc;
  doc["hora"]        = hora;
  doc["ip"]          = WiFi.localIP().toString();
  doc["host"]        = String("kattenvoer.local");
  doc["alimentando"] = (feedState != FEED_IDLE);
  doc["snelheid"]    = snelheid;
  doc["duur"]        = duur;
  doc["momentos"]    = aantalMomenten;
  doc["proximo"]     = proximo;
  doc["rssi"]        = (int)WiFi.RSSI();
  doc["uptime"]      = (int)(millis() / 1000);

  JsonArray horarios = doc["horarios"].to<JsonArray>();
  for (int i = 0; i < aantalMomenten; i++) {
    JsonObject h = horarios.add<JsonObject>();
    h["h"] = voerUur[i];
    h["m"] = voerMin[i];
  }

  String out;
  serializeJson(doc, out);
  return out;
}

void handleApiState() {
  server.send(200, "application/json", estadoJson());
}

void handleApiFeed() {
  startVoeren();
  server.send(200, "application/json", estadoJson());
}

// ---------- SETUP / LOOP ----------

void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(RESET_KNOP, INPUT_PULLUP);
  pinMode(LED_R, OUTPUT);
  pinMode(LED_G, OUTPUT);
  pinMode(LED_B, OUTPUT);
  setKleur(1, 1, 0);

  // Servo: en ESP32 clasico ESP32Servo usa LEDC con frecuencia fija, no hace falta allocateTimer
  mijnServo.setPeriodHertz(50);
  mijnServo.attach(SERVO_PIN);

  prefs.begin("settings", true);
  snelheid = constrain(prefs.getInt("speed", 30), 0, 100);
  duur    = constrain(prefs.getInt("duur", 2), 1, 10);
  aantalMomenten = constrain(prefs.getInt("aantal", 2), 1, MAX_MOMENTEN);
  for (int i = 0; i < aantalMomenten; i++) {
    voerUur[i] = constrain(prefs.getInt(("u" + String(i)).c_str(), 8), 0, 23);
    voerMin[i] = constrain(prefs.getInt(("m" + String(i)).c_str(), 0), 0, 59);
  }
  prefs.end();

  // Zona horaria ANTES de configTime (si no, el SNTP puede aplicar la zona equivocada)
  setenv("TZ", "CET-1CEST,M3.5.0/02,M10.5.0/03", 1);
  tzset();
  configTime(0, 0, ntpServer);

  // Portal NO bloqueante: el bucle sigue vivo -> el watchdog del task WiFi no salta
  wm.setConfigPortalBlocking(false);
  wm.setConnectTimeout(20);

  if (wm.autoConnect(apName)) {
    Serial.println();
    Serial.println("==========================================");
    Serial.println("  CAT FEEDER - listo");
    Serial.print("  Panel (esta misma red): http://");
    Serial.println(WiFi.localIP().toString());
    Serial.println("  Panel (por nombre)   : http://kattenvoer.local");
    Serial.println("  -> En el movil: que este en ESTA misma WiFi");
    Serial.println("==========================================");
    Serial.println();
  } else {
    Serial.println();
    Serial.println("==========================================");
    Serial.println("  CAT FEEDER - sin WiFi configurado");
    Serial.println("  1) Conecta el movil a la AP 'CatFeeder'");
    Serial.println("  2) Se abrira el portal: elige tu red y contrasena");
    Serial.println("  3) Reinicia el ESP32");
    Serial.println("==========================================");
    Serial.println();
  }

  setKleur(1, 0, 1);

  server.on("/", handleRoot);
  server.on("/update", handleUpdate);
  server.on("/feed", handleFeed);
  server.on("/api/state", handleApiState);
  server.on("/api/feed", handleApiFeed);
}

void loop() {
  wm.process();                 // atiende el portal si esta activo

  // El servidor propio se arranca solo cuando hay WiFi, para no pelear el puerto 80 con el portal
  if (!serverActivo && WiFi.status() == WL_CONNECTED) {
    server.begin();
    serverActivo = true;
    Serial.println("Servidor web iniciado");
    Serial.println("Abre en el movil: http://" + WiFi.localIP().toString());
    if (!mdnsActivo) {
      MDNS.begin("kattenvoer");
      MDNS.addService("http", "tcp", 80);
      mdnsActivo = true;
    }
  }

  // Vigilancia del WiFi: si se cae, reconectar; si no vuelve, reinicio limpio
  // (reboot en vez de arrancar el portal a mitad de sesion, para no pelear el puerto 80)
  if (serverActivo && WiFi.status() != WL_CONNECTED) {
    if (wifiCaidoTijd == 0) {
      wifiCaidoTijd = millis();
      Serial.println("WiFi caido, reintentando...");
      WiFi.reconnect();
    } else if (millis() - wifiCaidoTijd > 60000) {
      Serial.println("WiFi no vuelve -> reiniciando");
      ESP.restart();
    }
  } else {
    wifiCaidoTijd = 0;
  }

  if (serverActivo) server.handleClient();

  updateVoeren();
  checkVoerTijd();
  checkResetKnop();
}
