// ===== Cat Feeder Automatico - ESP32 =====
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <ESP32Servo.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include "time.h"

#define RESET_KNOP   4
#define LED_R 13
#define LED_G 14
#define LED_B 27
#define SERVO_PIN 18

#define MAX_MOMENTEN 5
#define SERVO_FEED_US 400   // valor que lo hace girar (mas lejos de 1500 = mas rapido)

#define SIN_WIFI 0
#define DEBUG_CORE 0
#define MAX_ARRANQUES 5

RTC_NOINIT_ATTR int arranqueNum = 0;

WebServer server(80);
WiFiManager wm;
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
bool guardarYReiniciar = false;

enum FeedState { FEED_IDLE, FEED_VOORUIT, FEED_TERUG };
FeedState feedState = FEED_IDLE;
uint32_t   feedTijd = 0;
int        feedStappen = 0;
int        feedStap = 0;

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
  if (feedState != FEED_IDLE) return;
  duur = constrain(duur, 1, 10);

  mijnServo.attach(SERVO_PIN, 300, 2400);      // rango ampliado
  mijnServo.writeMicroseconds(SERVO_FEED_US);  // GIRA (al otro lado)
  feedTijd  = millis();
  feedState = FEED_VOORUIT;
  setKleur(0, 0, 1);
}

void updateVoeren() {
  if (feedState == FEED_IDLE) return;

  if (millis() - feedTijd >= (uint32_t)duur * 1000) {
    mijnServo.detach();   // corta señal -> PARA
    feedState = FEED_IDLE;
    setKleur(1, 0, 1);
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

  if (!knopIngedrukt) {
    knopIngedrukt = true;
    knopTijd = nu;
    setKleur(1, 1, 0);
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

    wm.resetSettings();
    delay(100);
    Serial.println("Reiniciando...");
    ESP.restart();
  }
}

// ---------- HORARIO ----------

void checkVoerTijd() {
  static uint32_t laatsteCheck = 0;
  if (millis() - laatsteCheck < 1000) return;
  laatsteCheck = millis();

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 100)) return;

  int huidig = timeinfo.tm_hour * 60 + timeinfo.tm_min;
  if (huidig == laatsteMinuut) return;
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

  String html = "";
  html += "<!DOCTYPE html>\n";
  html += "<html>\n";
  html += "<head>\n";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>\n";
  html += "<style>\n";
  html += "body {font-family:sans-serif;background:#0f172a;color:white;text-align:center;}\n";
  html += ".card {background:#1e293b;padding:20px;margin:10px;border-radius:15px;}\n";
  html += "button {padding:15px;border-radius:10px;border:none;background:#38bdf8;}\n";
  html += ".row {display:flex;gap:5px;margin:5px;}\n";
  html += "input {flex:1;padding:10px;border-radius:10px;border:none;}\n";
  html += "</style>\n";
  html += "</head>\n";
  html += "<body>\n";
  html += "<h2>SmartCat Feeder</h2>\n";
  html += "<p>ESP32: <b id='espClock'>" + tijd + "</b> &nbsp; Movil: <span id='clock'>--:--</span></p>\n";
  html += "<p>IP: <b id='ip'>--</b> &nbsp; <span id='status'>conectando...</span></p>\n";
  html += "<div class='card'>\n";
  html += "speed : <span id='speedVal'>0</span>%<br>\n";
  html += "<input type='range' id='speed' min='0' max='100'>\n";
  html += "<br><br>\n";
  html += "duration : <span id='timeVal'>0</span> sec<br>\n";
  html += "<input type='range' id='time' min='1' max='10'>\n";
  html += "</div>\n";
  html += "<div class='card'>\n";
  html += "feed moments\n";
  html += "<div id='momenten'></div>\n";
  html += "<button onclick='addMoment()'> + </button>\n";
  html += "</div>\n";
  html += "<div class='card'>\n";
  html += "<button onclick='feedNow()'> feed </button>\n";
  html += "</div>\n";
  html += "<script>\n";

  html += "let count = " + String(aantalMomenten) + ";\n";

  html += "function saveAll(){\n";
  html += "for(let i=0;i<count;i++){\n";
  html += "let u=document.getElementById('u'+i);\n";
  html += "let m=document.getElementById('m'+i);\n";
  html += "if(u&&m){window['u'+i]=u.value;window['m'+i]=m.value;}\n";
  html += "}\n";
  html += "}\n";

  html += "function renderMomenten(){\n";
  html += "let c=document.getElementById('momenten');\n";
  html += "c.innerHTML='';\n";
  html += "for(let i=0;i<count;i++){\n";
  html += "let d=document.createElement('div');\n";
  html += "d.className='row';\n";
  html += "d.innerHTML=`\n";
  html += "<input type='number' id='u${i}' min='0' max='23' value='${window['u'+i] !== undefined ? window['u'+i] : 0}' onchange='saveMoment(${i})'>\n";
  html += "<input type='number' id='m${i}' min='0' max='59' value='${window['m'+i] !== undefined ? window['m'+i] : 0}' onchange='saveMoment(${i})'>\n";
  html += "<button onclick='removeMoment(${i})'> X </button>\n";
  html += "`;\n";
  html += "c.appendChild(d);\n";
  html += "}\n";
  html += "}\n";

  html += "function addMoment(){\n";
  html += "saveAll();\n";
  html += "if(count>=5)return;\n";
  html += "window['u'+count]=12;\n";
  html += "window['m'+count]=0;\n";
  html += "count++;\n";
  html += "renderMomenten();\n";
  html += "sendData();\n";
  html += "}\n";

  html += "function removeMoment(i){\n";
  html += "saveAll();\n";
  html += "for(let x=i;x<count-1;x++){\n";
  html += "window['u'+x]=window['u'+(x+1)];\n";
  html += "window['m'+x]=window['m'+(x+1)];\n";
  html += "}\n";
  html += "count--;\n";
  html += "renderMomenten();\n";
  html += "setTimeout(sendData,50);\n";
  html += "}\n";

  html += "function saveMoment(i){\n";
  html += "let u=document.getElementById('u'+i);\n";
  html += "let m=document.getElementById('m'+i);\n";
  html += "if(u&&m){window['u'+i]=u.value;window['m'+i]=m.value;sendData();}\n";
  html += "}\n";

  html += "function sendData(){\n";
  html += "let url='/update?';\n";
  html += "url+='speed='+speed.value;\n";
  html += "url+='&time='+time.value;\n";
  html += "url+='&count='+count;\n";
  html += "for(let i=0;i<count;i++){\n";
  html += "url+='&u'+i+'='+window['u'+i];\n";
  html += "url+='&m'+i+'='+window['m'+i];\n";
  html += "}\n";
  html += "fetch(url);\n";
  html += "}\n";

  html += "function feedNow(){fetch('/api/feed').then(()=>{document.getElementById('status').innerText='Alimentando...';});}\n";

  html += "let speed=document.getElementById('speed');\n";
  html += "let time=document.getElementById('time');\n";
  html += "let speedVal=document.getElementById('speedVal');\n";
  html += "let timeVal=document.getElementById('timeVal');\n";
  html += "function updateLabels(){speedVal.innerText=speed.value;timeVal.innerText=time.value;}\n";
  html += "speed.oninput = function(){ updateLabels(); }\n";
  html += "speed.onchange = function(){ updateLabels(); sendData(); }\n";
  html += "time.oninput = function(){ updateLabels(); }\n";
  html += "time.onchange = function(){ updateLabels(); sendData(); }\n";

  html += "window.onload=function(){\n";
  html += "speed.value=" + String(snelheid) + ";\n";
  html += "time.value=" + String(duur) + ";\n";
  html += "updateLabels();\n";

  for (int i = 0; i < aantalMomenten; i++) {
    html += "window['u" + String(i) + "']=" + String(voerUur[i]) + ";";
    html += "window['m" + String(i) + "']=" + String(voerMin[i]) + ";";
  }

  html += "\n";
  html += "updateClock();\n";
  html += "renderMomenten();\n";
  html += "pollState();\n";
  html += "}\n";

  html += "function updateClock(){\n";
  html += "let now=new Date();\n";
  html += "let h=now.getHours().toString().padStart(2,'0');\n";
  html += "let m=now.getMinutes().toString().padStart(2,'0');\n";
  html += "document.getElementById('clock').innerText=h+':'+m;\n";
  html += "}\n";

  html += "async function pollState(){\n";
  html += "try{\n";
  html += "const r=await fetch('/api/state',{cache:'no-store'});\n";
  html += "const s=await r.json();\n";
  html += "document.getElementById('espClock').innerText=s.hora;\n";
  html += "document.getElementById('ip').innerText=s.ip;\n";
  html += "document.getElementById('status').innerText=s.alimentando ? 'Alimentando...' : ('Proximo: '+s.proximo);\n";
  html += "}catch(e){\n";
  html += "document.getElementById('status').innerText='sin conexion con el ESP32';\n";
  html += "}\n";
  html += "}\n";

  html += "setInterval(updateClock,1000);\n";
  html += "setInterval(pollState,2000);\n";
  html += "</script>\n";
  html += "</body>\n";
  html += "</html>\n";

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

// ---------- API JSON ----------

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

void cp(const String& msg) {
  Serial.println(msg);
  Serial.flush();
}

void setup() {
  Serial.begin(115200);
  delay(300);
#if DEBUG_CORE
  Serial.setDebugOutput(true);
#endif

  if (arranqueNum < 0 || arranqueNum > 1000) arranqueNum = 0;
  arranqueNum++;
  bool modoSeguro = (arranqueNum > MAX_ARRANQUES);

  Serial.println();
  cp("[1] setup - arranque");
  Serial.print("    arranque nro: "); Serial.println(arranqueNum); Serial.flush();
  Serial.print("    motivo del reinicio previo: ");
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   Serial.println("POWERON (encendido/reset de pin)"); break;
    case ESP_RST_EXT:       Serial.println("EXT (pin RESET)"); break;
    case ESP_RST_SW:        Serial.println("SW (ESP.restart)"); break;
    case ESP_RST_PANIC:     Serial.println("PANIC (error de codigo)"); break;
    case ESP_RST_INT_WDT:   Serial.println("INT_WDT (watchdog de un task: WiFi o flash)"); break;
    case ESP_RST_TASK_WDT:  Serial.println("TASK_WDT (watchdog de un task)"); break;
    case ESP_RST_WDT:       Serial.println("WDT"); break;
    case ESP_RST_BROWNOUT:  Serial.println("BROWNOUT (falta de alimentacion)"); break;
    default:                Serial.println("otro"); break;
  }
  Serial.flush();
  if (modoSeguro) cp("    !! MODO SEGURO: demasiados reinicios, se desactiva el WiFi");

  pinMode(RESET_KNOP, INPUT_PULLUP);
  cp("[2a] pin boton ok");
  pinMode(LED_R, OUTPUT);
  pinMode(LED_G, OUTPUT);
  pinMode(LED_B, OUTPUT);
  cp("[2b] pines led ok");
  setKleur(1, 1, 0);
  cp("[2c] leds ok");

  mijnServo.setPeriodHertz(50);
  mijnServo.attach(SERVO_PIN, 300, 2400);
  mijnServo.detach();                 // sin señal = servo quieto al arrancar
  cp("[3] servo ok (pin 18)");

  prefs.begin("settings", true);
  snelheid = constrain(prefs.getInt("speed", 30), 0, 100);
  duur    = constrain(prefs.getInt("duur", 2), 1, 10);
  aantalMomenten = constrain(prefs.getInt("aantal", 2), 1, MAX_MOMENTEN);
  for (int i = 0; i < aantalMomenten; i++) {
    voerUur[i] = constrain(prefs.getInt(("u" + String(i)).c_str(), 8), 0, 23);
    voerMin[i] = constrain(prefs.getInt(("m" + String(i)).c_str(), 0), 0, 59);
  }
  prefs.end();
  cp("[4] ajustes leidos de la NVS");

  setenv("TZ", "CET-1CEST,M3.5.0/02,M10.5.0/03", 1);
  tzset();
  cp("[5] zona horaria ok");

#if SIN_WIFI
  cp("[6] MODO DIAGNOSTICO: se salta el WiFi por completo");
#else
  bool conectado = false;

  if (modoSeguro) {
    cp("[6] MODO SEGURO: se salta el auto-connect, se levanta el portal");
  } else {
    cp("[6a] WiFi: modo STA, connectando con la red guardada...");
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);
    wm.setEnableConfigPortal(false);
    wm.setConnectTimeout(20);

    conectado = wm.autoConnect(apName);
    cp(conectado ? "[6b] WiFi CONECTADO" : "[6c] no hay ninguna red guardada");
  }

  if (!conectado) {
    cp("[6d] levantando el portal (AP CatFeeder)...");
    wm.setEnableConfigPortal(true);
    wm.setConfigPortalBlocking(false);

    wm.setSaveConfigCallback([]() {
      guardarYReiniciar = true;
    });

    wm.startConfigPortal(apName);
    cp("[6e] portal activo");
  }

  if (conectado) {
    Serial.print("    IP: "); Serial.println(WiFi.localIP().toString()); Serial.flush();
  }

  cp("[7] iniciando NTP...");
  configTime(0, 0, ntpServer);
  cp("[8] NTP configurado (sincroniza en segundo plano)");

  Serial.println();
  Serial.println("==========================================");
  Serial.println("  CAT FEEDER - listo");
  if (conectado) {
    Serial.print("  Panel (esta misma red): http://");
    Serial.println(WiFi.localIP().toString());
    Serial.println("  Panel (por nombre)   : http://kattenvoer.local");
    Serial.println("  -> En el movil: que este en ESTA misma WiFi");
  } else {
    Serial.println("  1) Conecta el movil a la AP 'CatFeeder'");
    Serial.println("  2) Se abrira el portal: elige tu red y contrasena");
    Serial.println("  3) El ESP32 se reinicia solo al guardar");
  }
  Serial.println("==========================================");
  Serial.println();
  Serial.flush();
#endif

  setKleur(1, 0, 1);

  server.on("/", handleRoot);
  server.on("/update", handleUpdate);
  server.on("/feed", handleFeed);
  server.on("/api/state", handleApiState);
  server.on("/api/feed", handleApiFeed);

  cp("[10] setup terminado");

  arranqueNum = 0;
}

void loop() {
  wm.process();

  if (guardarYReiniciar) {
    guardarYReiniciar = false;
    cp("[*] red guardada, reiniciando...");
    delay(1500);
    ESP.restart();
  }

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
