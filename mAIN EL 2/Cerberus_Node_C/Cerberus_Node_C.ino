#include "cerberus_common.h"

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

// ======================================================
// CERBERUS NODE C — DASHBOARD AGGREGATOR
// Phase 5: Firebase internet dashboard added
// ======================================================

// ---------------- Phone hotspot ----------------
const char* STA_SSID = "Panda's wifi";
const char* STA_PASS = "12345678";

// ---------------- Local dashboard AP ----------------
const char* AP_SSID = "CERBERUS_DASH";
const char* AP_PASS = "cerberus123";

// ---------------- Firebase ----------------
const char* FB_URL  = "https://cerberus-cluster-default-rtdb.asia-southeast1.firebasedatabase.app";
const char* FB_AUTH = "";          // test mode: leave empty
const uint32_t FB_INTERVAL = 5000; // push every 5 seconds

// ---------------- Node identity ----------------
const char MY_ID = 'C';
const uint8_t MY_PRIO = PRIO_C;

const int RED_LED = 2;

// ---------------- Wi-Fi timing ----------------
const uint32_t STA_BOOT_TIMEOUT_MS    = 10000;
const uint32_t STA_RETRY_INTERVAL_MS  = 15000;
const uint32_t STA_ATTEMPT_TIMEOUT_MS = 6000;
const uint32_t WIFI_STATUS_INTERVAL_MS = 1000;

// ---------------- Web server ----------------
WebServer server(80);

// ---------------- Mesh packet ----------------
MeshPacket me;

// ---------------- Phase 4 state ----------------
bool internetMode      = false;
bool staAttemptActive  = false;
uint8_t activeChannel  = WIFI_CHANNEL;
uint32_t tWiFiStatus   = 0;
uint32_t lastStaAttempt     = 0;
uint32_t staAttemptStarted  = 0;

// ---------------- Firebase state ----------------
uint32_t tFirebase = 0;
// Captured from buildSnapshot() for the small history record
float    fb_tempC   = 0;
float    fb_hum     = 0;
bool     fb_haveEnv = false;
const char* fb_source = "NONE";
char     fb_leader  = 'C';
const char* fb_health = "CRITICAL";

// ---------------- Peer structure ----------------
struct Peer {
  char id;
  uint32_t last;
  uint8_t prio;
  bool seen;
  bool alive;
  int rssi;
  MeshPacket data;
};

Peer peers[2] = {
  {'A', 0, PRIO_A, false, false, -99, {}},
  {'B', 0, PRIO_B, false, false, -99, {}}
};

// ---------------- Timers ----------------
uint32_t tHB   = 0;
uint32_t tLoop = 0;
uint32_t bootMs = 0;
uint32_t seq   = 0;

char curLeader = 'C';

// ======================================================
// HTML LOCAL DASHBOARD (unchanged)
// ======================================================

const char INDEX_HTML[] PROGMEM = R"CERBERUSHTML(
<!DOCTYPE html>
<html>
<head>
  <title>CERBERUS Dashboard</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body { font-family: Arial, sans-serif; background: #0b1117; color: white; padding: 20px; }
    h1 { color: #46d6ff; }
    .card { background: #16202b; padding: 15px; margin: 12px 0; border-radius: 12px; border: 1px solid #263443; }
    .ok { color: #37f5a0; } .warn { color: #ffb547; } .bad { color: #ff4d5e; }
    .big { font-size: 22px; font-weight: bold; }
    table { width: 100%; border-collapse: collapse; margin-top: 10px; }
    th, td { padding: 8px; border-bottom: 1px solid #263443; text-align: left; }
    th { color: #46d6ff; }
  </style>
</head>
<body>
  <h1>CERBERUS Dashboard</h1>
  <div class="card">
    <h2>Cluster Status</h2>
    <p>Leader: <span class="big" id="leader">--</span></p>
    <p>Health: <span class="big" id="health">--</span></p>
    <p>Node C Uptime: <span class="big" id="uptime">00:00:00</span></p>
  </div>
  <div class="card">
    <h2>Network Status</h2>
    <p>Dashboard Mode: <span class="big" id="dashmode">--</span></p>
    <p>Phone Hotspot: <span id="staconnected">--</span></p>
    <p>Internet IP: <span id="staip">--</span></p>
    <p>Local Dashboard IP: <span id="apip">--</span></p>
    <p>Shared Wi-Fi Channel: <span class="big" id="wifichannel">--</span></p>
  </div>
  <div class="card">
    <h2>Active Sensor Data</h2>
    <p>Source: <span class="big" id="src">--</span></p>
    <p>Temperature: <b id="temp">--</b> &deg;C</p>
    <p>Humidity: <b id="hum">--</b> %</p>
    <p>Vibration: <b id="vib">0</b> <span id="vibstale" class="warn"></span></p>
    <p>Tilt: <b id="tilt">0</b></p>
    <p>LDR Raw: <b id="ldr">0</b></p>
    <p>Light: <b id="light">--</b></p>
    <p>Motion Alert: <b id="alert">--</b></p>
    <p>Env Alert: <b id="envalert">--</b></p>
  </div>
  <div class="card">
    <h2>Node Status</h2>
    <table>
      <thead><tr><th>Node</th><th>State</th><th>Alive</th><th>Seq</th><th>Uptime</th><th>RSSI</th></tr></thead>
      <tbody id="nodeTable"></tbody>
    </table>
  </div>
<script>
function fmt(sec){sec=Number(sec||0);var h=Math.floor(sec/3600),m=Math.floor((sec%3600)/60),s=sec%60;return String(h).padStart(2,'0')+':'+String(m).padStart(2,'0')+':'+String(s).padStart(2,'0');}
function healthClass(h){if(h==='OK')return 'ok';if(h==='DEGRADED')return 'warn';return 'bad';}
function updateDashboard(){
  fetch('/state',{cache:'no-store'}).then(r=>r.json()).then(d=>{
    document.getElementById('leader').innerText=d.leader;
    document.getElementById('health').innerText=d.clusterHealth;
    document.getElementById('health').className='big '+healthClass(d.clusterHealth);
    var me=document.getElementById('dashmode');me.innerText=d.dashboardMode;me.className='big '+(d.dashboardMode==='INTERNET'?'ok':'warn');
    var se=document.getElementById('staconnected');se.innerText=d.staConnected?'CONNECTED':'DISCONNECTED';se.className=d.staConnected?'ok':'warn';
    document.getElementById('staip').innerText=d.staConnected?d.staIP:'Not connected';
    document.getElementById('apip').innerText=d.apIP;
    document.getElementById('wifichannel').innerText=d.wifiChannel;
    var nc=d.nodes.find(n=>n.id==='C');if(nc)document.getElementById('uptime').innerText=fmt(nc.uptimeS);
    var sn=d.sensors;
    document.getElementById('src').innerText=sn.source;
    document.getElementById('temp').innerText=sn.haveEnv?Number(sn.tempC).toFixed(1):'--';
    document.getElementById('hum').innerText=sn.haveEnv?Number(sn.humidity).toFixed(0):'--';
    document.getElementById('vib').innerText=Number(sn.vibration).toFixed(2);
    document.getElementById('vibstale').innerText=sn.vibStale?'(last-known A)':'';
    document.getElementById('tilt').innerText=Number(sn.tilt).toFixed(1);
    document.getElementById('ldr').innerText=sn.ldrRaw;
    document.getElementById('light').innerText=sn.light;
    document.getElementById('alert').innerText=sn.motionAlert?'YES':'NO';
    var ea=document.getElementById('envalert');ea.innerText=sn.envAlert?'ALERT':'normal';ea.className=sn.envAlert?'bad':'ok';
    var tb=document.getElementById('nodeTable');tb.innerHTML='';
    d.nodes.forEach(n=>{var tr=document.createElement('tr');tr.innerHTML='<td><b>'+n.id+'</b></td><td>'+n.state+'</td><td class="'+(n.alive?'ok':'bad')+'">'+(n.alive?'YES':'NO')+'</td><td>'+n.seq+'</td><td>'+fmt(n.uptimeS)+'</td><td>'+n.rssi+' dBm</td>';tb.appendChild(tr);});
  }).catch(e=>{document.getElementById('health').innerText='FETCH ERROR';document.getElementById('health').className='big bad';});
}
setInterval(updateDashboard,1000);updateDashboard();
</script>
</body>
</html>
)CERBERUSHTML";

// ======================================================
// STATE NAME
// ======================================================

const char* stateName(uint8_t state) {
  switch (state) {
    case S_BOOT:     return "BOOT";
    case S_JOIN:     return "JOIN";
    case S_ACTIVE:   return "ACTIVE";
    case S_STANDBY:  return "STANDBY";
    case S_FAILOVER: return "FAILOVER";
    case S_DEGRADED: return "DEGRADED";
    default:         return "?";
  }
}

// ======================================================
// WI-FI FUNCTIONS  (unchanged from Phase 4)
// ======================================================

void lockMeshChannel() {
  activeChannel = WIFI_CHANNEL;
  esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
}

bool connectToPhoneHotspot(uint32_t timeoutMs) {
  Serial.println();
  Serial.print("Connecting to phone hotspot: ");
  Serial.println(STA_SSID);
  WiFi.disconnect(false, false);
  delay(100);
  WiFi.begin(STA_SSID, STA_PASS, WIFI_CHANNEL);
  lastStaAttempt     = millis();
  staAttemptActive   = true;
  staAttemptStarted  = millis();
  uint32_t startTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startTime < timeoutMs) {
    delay(300); Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    uint8_t phoneChannel = WiFi.channel();
    if (phoneChannel != WIFI_CHANNEL) {
      Serial.println("CHANNEL MISMATCH!");
      Serial.print("Phone channel: ");   Serial.println(phoneChannel);
      Serial.print("Mesh channel: ");    Serial.println(WIFI_CHANNEL);
      WiFi.disconnect(false, false); delay(100);
      lockMeshChannel();
      internetMode = false; staAttemptActive = false;
      Serial.println("Mode: LOCAL_FALLBACK");
      return false;
    }
    internetMode = true; activeChannel = phoneChannel; staAttemptActive = false;
    Serial.println("Phone hotspot connected.");
    Serial.println("Mode: INTERNET");
    Serial.print("STA IP: ");      Serial.println(WiFi.localIP());
    Serial.print("Shared channel: "); Serial.println(activeChannel);
    return true;
  }
  WiFi.disconnect(false, false); delay(100);
  lockMeshChannel();
  internetMode = false; staAttemptActive = false;
  Serial.println("Hotspot unavailable.");
  Serial.println("Mode: LOCAL_FALLBACK");
  return false;
}

bool startLocalDashboardAP() {
  Serial.println();
  Serial.print("Starting local dashboard AP on channel ");
  Serial.println(activeChannel);
  bool apOK = WiFi.softAP(AP_SSID, AP_PASS, activeChannel);
  delay(200);
  lockMeshChannel();
  Serial.print("SoftAP status: ");      Serial.println(apOK ? "OK" : "FAILED");
  Serial.print("Join Wi-Fi: ");         Serial.println(AP_SSID);
  Serial.print("Password: ");           Serial.println(AP_PASS);
  Serial.print("Open dashboard: http://"); Serial.println(WiFi.softAPIP());
  Serial.print("Shared channel: ");     Serial.println(activeChannel);
  return apOK;
}

void startStaRetry(uint32_t now) {
  if (staAttemptActive || WiFi.status() == WL_CONNECTED) return;
  Serial.println();
  Serial.print("Starting hotspot retry: "); Serial.println(STA_SSID);
  WiFi.disconnect(false, false); delay(100);
  WiFi.begin(STA_SSID, STA_PASS, WIFI_CHANNEL);
  staAttemptActive = true; staAttemptStarted = now; lastStaAttempt = now;
}

void maintainWiFi(uint32_t now) {
  if (WiFi.status() == WL_CONNECTED) {
    uint8_t phoneChannel = WiFi.channel();
    staAttemptActive = false;
    if (phoneChannel != WIFI_CHANNEL) {
      Serial.println(); Serial.println("Wrong hotspot channel.");
      Serial.print("Hotspot channel: ");  Serial.println(phoneChannel);
      Serial.print("Required mesh channel: "); Serial.println(WIFI_CHANNEL);
      WiFi.disconnect(false, false); delay(100);
      lockMeshChannel();
      internetMode = false; lastStaAttempt = now;
      Serial.println("Mode: LOCAL_FALLBACK");
      return;
    }
    if (!internetMode) {
      Serial.println(); Serial.println("Phone hotspot recovered.");
      Serial.println("Mode changed to INTERNET.");
      Serial.print("STA IP: "); Serial.println(WiFi.localIP());
      Serial.print("Shared channel: "); Serial.println(phoneChannel);
    }
    internetMode = true; activeChannel = phoneChannel;
    return;
  }
  if (internetMode) {
    Serial.println(); Serial.println("Phone hotspot lost.");
    Serial.println("Mode changed to LOCAL_FALLBACK.");
  }
  internetMode = false; activeChannel = WIFI_CHANNEL;
  if (staAttemptActive) {
    if (now - staAttemptStarted >= STA_ATTEMPT_TIMEOUT_MS) {
      Serial.println(); Serial.println("Hotspot retry timed out.");
      WiFi.disconnect(false, false); delay(100);
      lockMeshChannel();
      staAttemptActive = false; lastStaAttempt = now;
      Serial.println("Mesh restored to channel 6.");
      Serial.println("Mode: LOCAL_FALLBACK");
    }
    return;
  }
  if (now - lastStaAttempt >= STA_RETRY_INTERVAL_MS) startStaRetry(now);
}

// ======================================================
// ESP-NOW RECEIVE CALLBACK
// ======================================================

void onRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (len != sizeof(MeshPacket)) {
    Serial.printf("Bad packet size: %d, expected: %u\n", len, (unsigned)sizeof(MeshPacket));
    return;
  }
  MeshPacket p; memcpy(&p, data, sizeof(p));
  for (auto &pe : peers) {
    if (pe.id == p.nodeId) {
      pe.last = millis(); pe.seen = true; pe.data = p;
      pe.rssi = info->rx_ctrl ? info->rx_ctrl->rssi : -60;
    }
  }
}

// ======================================================
// LEADER ELECTION
// ======================================================

char computeLeader() {
  char best = MY_ID; uint8_t bestPrio = MY_PRIO;
  uint32_t now = millis();
  for (auto &pe : peers) {
    pe.alive = pe.seen && ((now - pe.last) < TIMEOUT);
    if (pe.alive && pe.prio > bestPrio) { bestPrio = pe.prio; best = pe.id; }
  }
  return best;
}

// ======================================================
// JSON SNAPSHOT
// ======================================================

String buildSnapshot() {
  StaticJsonDocument<4096> j;

  j["type"] = "state";
  j["ts"]   = millis();
  j["leader"] = String(curLeader);
  j["dashboardMode"] = internetMode ? "INTERNET" : "LOCAL_FALLBACK";
  j["staConnected"]  = internetMode;
  j["staIP"] = internetMode ? WiFi.localIP().toString() : "0.0.0.0";
  j["apIP"]  = WiFi.softAPIP().toString();
  j["wifiChannel"] = activeChannel;

  int aliveCount = 1;
  for (auto &pe : peers) if (pe.alive) aliveCount++;
  const char* health = aliveCount == 3 ? "OK" : aliveCount == 2 ? "DEGRADED" : "CRITICAL";
  j["clusterHealth"] = health;

  JsonArray nodes = j.createNestedArray("nodes");
  auto addNode = [&](char id, const char* role, int prio, uint8_t state,
                     bool alive, uint32_t lastSeen, int rssi, uint32_t uptime, uint32_t sq) {
    JsonObject n = nodes.createNestedObject();
    n["id"] = String(id); n["role"] = role; n["priority"] = prio;
    n["state"] = alive ? stateName(state) : "DOWN";
    n["alive"] = alive; n["lastSeenMs"] = lastSeen; n["rssi"] = rssi;
    n["uptimeS"] = uptime; n["seq"] = sq;
  };
  for (auto &pe : peers) {
    const char* role = pe.id == 'A' ? "Sensor-Primary" : "HMI-Standby";
    addNode(pe.id, role, pe.prio, pe.data.state, pe.alive,
            pe.alive ? millis() - pe.last : 0, pe.rssi, pe.data.uptimeS, pe.data.seq);
  }
  addNode('C', "Dashboard-Agg", PRIO_C, me.state, true, 0, -40,
          (millis() - bootMs) / 1000, seq);

  bool aLive = peers[0].alive, bLive = peers[1].alive;
  MeshPacket* A = &peers[0].data;
  MeshPacket* B = &peers[1].data;

  MeshPacket* src = nullptr;
  const char* srcLabel = "NONE";
  char srcId = '-';
  if      (aLive)          { src = A; srcLabel = "A (LIVE)";       srcId = 'A'; }
  else if (bLive)          { src = B; srcLabel = "B (LIVE)";       srcId = 'B'; }
  else if (peers[0].seen)  { src = A; srcLabel = "A (LAST-KNOWN)"; srcId = 'A'; }
  else if (peers[1].seen)  { src = B; srcLabel = "B (LAST-KNOWN)"; srcId = 'B'; }

  float vibration = 0, tilt = 0; uint8_t motionAlert = 0; bool vibrationStale = false;
  if (srcId == 'A' && src) {
    vibration = src->vibration; tilt = src->tilt;
    motionAlert = src->motionAlert; vibrationStale = !aLive;
  } else if (srcId == 'B') {
    if (peers[0].seen) { vibration = A->vibration; tilt = A->tilt; motionAlert = A->motionAlert; }
    vibrationStale = true;
  }

  float temperatureC = 0, humidity = 0; uint8_t environmentAlert = 0; bool haveEnvironment = false;
  if (src && src->hasEnv) {
    temperatureC = src->temperature; humidity = src->humidity;
    environmentAlert = src->envAlert; haveEnvironment = true;
  } else if (peers[0].seen && A->hasEnv) {
    temperatureC = A->temperature; humidity = A->humidity;
    environmentAlert = A->envAlert; haveEnvironment = true;
  }

  int ldr = src ? src->ldrRaw : 0;
  uint8_t lightValue = src ? src->light : 1;
  const char* lightStates[] = { "DARK", "NORMAL", "BRIGHT" };

  JsonObject sensors = j.createNestedObject("sensors");
  sensors["source"]      = srcLabel;
  sensors["sourceId"]    = String(srcId);
  sensors["vibration"]   = vibration;
  sensors["vibStale"]    = vibrationStale;
  sensors["tilt"]        = tilt;
  sensors["motionAlert"] = (bool)motionAlert;
  sensors["ldrRaw"]      = ldr;
  sensors["light"]       = lightStates[lightValue < 3 ? lightValue : 1];
  sensors["tempC"]       = haveEnvironment ? temperatureC : 0;
  sensors["humidity"]    = haveEnvironment ? humidity : 0;
  sensors["haveEnv"]     = haveEnvironment;
  sensors["envAlert"]    = (bool)environmentAlert;

  JsonArray links = j.createNestedArray("links");
  auto addLink = [&](const char* from, const char* to, bool up, int rssi) {
    JsonObject link = links.createNestedObject();
    link["from"] = from; link["to"] = to; link["up"] = up; link["rssi"] = rssi;
    link["latencyMs"] = up ? 2 + (int)(millis() % 6) : 0;
  };
  addLink("A", "B", aLive && bLive, peers[0].rssi);
  addLink("A", "C", aLive, peers[0].rssi);
  addLink("B", "C", bLive, peers[1].rssi);

  // ---- capture values for Firebase history record ----
  fb_tempC   = temperatureC;
  fb_hum     = humidity;
  fb_haveEnv = haveEnvironment;
  fb_source  = srcLabel;
  fb_leader  = curLeader;
  fb_health  = health;

  String output; output.reserve(3000);
  serializeJson(j, output);
  return output;
}

// ======================================================
// FIREBASE PUSH  (Phase 5 — new)
// ======================================================

void pushToFirebase() {
  if (!internetMode || WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure(); // demo: skip TLS cert check — fine for a student project

  // --- 1) Full snapshot → /live  (overwrites current state every push) ---
  {
    HTTPClient https;
    String url = String(FB_URL) + "/live.json" + FB_AUTH;
    if (https.begin(client, url)) {
      https.addHeader("Content-Type", "application/json");
      int code = https.PUT(buildSnapshot());
      Serial.printf("FB live  -> %d\n", code);
      https.end();
    } else {
      Serial.println("FB live  -> begin() failed");
    }
  }

  // --- 2) Tiny record → /history  (POST appends; used for graphs) ---
  {
    StaticJsonDocument<256> h;
    h["ts"]   = (millis() - bootMs) / 1000; // uptime seconds
    h["t"]    = fb_haveEnv ? fb_tempC : 0;
    h["h"]    = fb_haveEnv ? fb_hum   : 0;
    h["env"]  = fb_haveEnv;
    h["src"]  = fb_source;
    h["lead"] = String(fb_leader);
    h["hp"]   = fb_health;
    String body; serializeJson(h, body);

    HTTPClient https;
    String url = String(FB_URL) + "/history.json" + FB_AUTH;
    if (https.begin(client, url)) {
      https.addHeader("Content-Type", "application/json");
      int code = https.POST(body);
      Serial.printf("FB hist  -> %d\n", code);
      https.end();
    } else {
      Serial.println("FB hist  -> begin() failed");
    }
  }
}

// ======================================================
// ESP-NOW BROADCAST PEER
// ======================================================

void addBroadcastPeer() {
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, BCAST, 6);
  peerInfo.channel = 0;        // 0 = use current Wi-Fi channel
  peerInfo.encrypt = false;
  peerInfo.ifidx   = WIFI_IF_STA;
  esp_err_t result = esp_now_add_peer(&peerInfo);
  if      (result == ESP_OK)              Serial.println("Broadcast peer added.");
  else if (result == ESP_ERR_ESPNOW_EXIST)Serial.println("Broadcast peer already exists.");
  else { Serial.print("Broadcast peer add failed: "); Serial.println(esp_err_to_name(result)); }
}

// ======================================================
// HTTP HANDLERS
// ======================================================

void handleRoot()    { server.sendHeader("Cache-Control","no-store"); server.send_P(200,"text/html",INDEX_HTML); }
void handleState()   { server.sendHeader("Access-Control-Allow-Origin","*"); server.sendHeader("Cache-Control","no-store"); server.send(200,"application/json",buildSnapshot()); }
void handleFavicon() { server.send(204); }

// ======================================================
// SETUP
// ======================================================

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(RED_LED, OUTPUT);
  digitalWrite(RED_LED, LOW);

  Serial.println();
  Serial.println("====================================");
  Serial.println("CERBERUS NODE C — PHASE 5");
  Serial.println("INTERNET + FALLBACK + FIREBASE");
  Serial.println("====================================");

  WiFi.persistent(false);
  WiFi.mode(WIFI_AP_STA);
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false, false);
  delay(300);

  connectToPhoneHotspot(STA_BOOT_TIMEOUT_MS);

  if (!startLocalDashboardAP()) Serial.println("ERROR: SoftAP failed to start.");

  if (esp_now_init() != ESP_OK) {
    Serial.println("ERROR: ESP-NOW init failed.");
    while (true) delay(1000);
  }
  esp_now_register_recv_cb(onRecv);
  addBroadcastPeer();

  server.on("/",           handleRoot);
  server.on("/state",      handleState);
  server.on("/favicon.ico",handleFavicon);
  server.begin();
  Serial.println("HTTP server started.");

  memset(&me, 0, sizeof(me));
  me.nodeId    = MY_ID;
  me.priority  = MY_PRIO;
  me.state     = S_BOOT;
  me.leaderView = MY_ID;
  bootMs = millis();

  Serial.println();
  Serial.print("Node C AP MAC: ");  Serial.println(WiFi.softAPmacAddress());
  Serial.print("Node C STA MAC: "); Serial.println(WiFi.macAddress());
  Serial.print("ESP-NOW channel: "); Serial.println(activeChannel);
  Serial.print("Dashboard mode: ");  Serial.println(internetMode ? "INTERNET" : "LOCAL_FALLBACK");
  Serial.print("Firebase URL: ");    Serial.println(FB_URL);
  Serial.print("Local dashboard: http://"); Serial.println(WiFi.softAPIP());
}

// ======================================================
// LOOP
// ======================================================

void loop() {
  server.handleClient();

  uint32_t now = millis();

  // Maintain hotspot connection
  if (now - tWiFiStatus >= WIFI_STATUS_INTERVAL_MS) {
    tWiFiStatus = now;
    maintainWiFi(now);
  }

  // Firebase push (INTERNET mode only, every FB_INTERVAL ms)
  if (now - tFirebase >= FB_INTERVAL) {
    tFirebase = now;
    pushToFirebase();
  }

  // Leader election + FSM
  if (now - tLoop >= LOOP_TICK) {
    tLoop = now;
    curLeader = computeLeader();
    me.leaderView = curLeader;
    me.state = (curLeader == MY_ID) ? S_FAILOVER : S_STANDBY;
    digitalWrite(RED_LED, curLeader == MY_ID ? HIGH : LOW);
  }

  // Heartbeat
  if (now - tHB >= HB_INTERVAL) {
    tHB = now;
    me.seq = ++seq;
    me.uptimeS = (now - bootMs) / 1000;
    esp_err_t sendResult = esp_now_send(BCAST, (uint8_t*)&me, sizeof(me));
    Serial.printf(
      "C HB #%lu | Leader=%c | State=%s | A=%s | B=%s | Mode=%s | STA=%s | AP=%s | CH=%u | Send=%s\n",
      (unsigned long)me.seq, curLeader,
      me.state == S_FAILOVER ? "FAILOVER" : "STANDBY",
      peers[0].alive ? "alive" : "dead",
      peers[1].alive ? "alive" : "dead",
      internetMode ? "INTERNET" : "LOCAL_FALLBACK",
      internetMode ? WiFi.localIP().toString().c_str() : "0.0.0.0",
      WiFi.softAPIP().toString().c_str(),
      activeChannel,
      esp_err_to_name(sendResult)
    );
  }
}
