#include "cerberus_common.h"

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <WebServer.h>
#include <ArduinoJson.h>

// ======================================================
// CERBERUS NODE C — DASHBOARD AGGREGATOR
// Built-in WebServer version
// Join Wi-Fi: CERBERUS_DASH
// Open: http://192.168.4.1
// ======================================================

const char* AP_SSID = "CERBERUS_DASH";
const char* AP_PASS = "cerberus123";

const char MY_ID = 'C';
const uint8_t MY_PRIO = PRIO_C;

const int RED_LED = 2;

WebServer server(80);
MeshPacket me;

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
  {'A', 0, PRIO_A, false, false, -99},
  {'B', 0, PRIO_B, false, false, -99}
};

uint32_t tHB = 0;
uint32_t tLoop = 0;
uint32_t bootMs = 0;
uint32_t seq = 0;

char curLeader = 'C';

const char INDEX_HTML[] PROGMEM = R"CERBERUSHTML(
<!DOCTYPE html>
<html>
<head>
  <title>CERBERUS Dashboard</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body {
      font-family: Arial, sans-serif;
      background: #0b1117;
      color: white;
      padding: 20px;
    }
    h1 {
      color: #46d6ff;
    }
    .card {
      background: #16202b;
      padding: 15px;
      margin: 12px 0;
      border-radius: 12px;
      border: 1px solid #263443;
    }
    .ok { color: #37f5a0; }
    .warn { color: #ffb547; }
    .bad { color: #ff4d5e; }
    .big {
      font-size: 22px;
      font-weight: bold;
    }
    table {
      width: 100%;
      border-collapse: collapse;
      margin-top: 10px;
    }
    th, td {
      padding: 8px;
      border-bottom: 1px solid #263443;
      text-align: left;
    }
    th {
      color: #46d6ff;
    }
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
    <h2>Sensor Data from Node A</h2>
    <p>Vibration: <b id="vib">0</b></p>
    <p>Tilt: <b id="tilt">0</b></p>
    <p>LDR Raw: <b id="ldr">0</b></p>
    <p>Light: <b id="light">--</b></p>
    <p>Motion Alert: <b id="alert">--</b></p>
  </div>

  <div class="card">
    <h2>Node Status</h2>
    <table>
      <thead>
        <tr>
          <th>Node</th>
          <th>State</th>
          <th>Alive</th>
          <th>Seq</th>
          <th>Uptime</th>
          <th>RSSI</th>
        </tr>
      </thead>
      <tbody id="nodeTable"></tbody>
    </table>
  </div>

<script>
function fmt(sec) {
  sec = Number(sec || 0);
  var h = Math.floor(sec / 3600);
  var m = Math.floor((sec % 3600) / 60);
  var s = sec % 60;
  return String(h).padStart(2, '0') + ':' +
         String(m).padStart(2, '0') + ':' +
         String(s).padStart(2, '0');
}

function healthClass(h) {
  if (h === 'OK') return 'ok';
  if (h === 'DEGRADED') return 'warn';
  return 'bad';
}

function updateDashboard() {
  fetch('/state', { cache: 'no-store' })
    .then(function(response) {
      return response.json();
    })
    .then(function(d) {
      document.getElementById('leader').innerText = d.leader;
      document.getElementById('health').innerText = d.clusterHealth;
      document.getElementById('health').className = 'big ' + healthClass(d.clusterHealth);

      var nodeC = d.nodes.find(function(n) { return n.id === 'C'; });
      if (nodeC) {
        document.getElementById('uptime').innerText = fmt(nodeC.uptimeS);
      }

      document.getElementById('vib').innerText = d.sensors.vibration;
      document.getElementById('tilt').innerText = d.sensors.tilt;
      document.getElementById('ldr').innerText = d.sensors.ldrRaw;
      document.getElementById('light').innerText = d.sensors.light;
      document.getElementById('alert').innerText = d.sensors.motionAlert ? 'YES' : 'NO';

      var table = document.getElementById('nodeTable');
      table.innerHTML = '';

      d.nodes.forEach(function(n) {
        var tr = document.createElement('tr');

        tr.innerHTML =
          '<td><b>' + n.id + '</b></td>' +
          '<td>' + n.state + '</td>' +
          '<td class="' + (n.alive ? 'ok' : 'bad') + '">' + (n.alive ? 'YES' : 'NO') + '</td>' +
          '<td>' + n.seq + '</td>' +
          '<td>' + fmt(n.uptimeS) + '</td>' +
          '<td>' + n.rssi + ' dBm</td>';

        table.appendChild(tr);
      });
    })
    .catch(function(err) {
      document.getElementById('health').innerText = 'FETCH ERROR';
      document.getElementById('health').className = 'big bad';
    });
}

setInterval(updateDashboard, 1000);
updateDashboard();
</script>

</body>
</html>
)CERBERUSHTML";

void onRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (len != sizeof(MeshPacket)) {
    Serial.printf("Bad packet size: %d\n", len);
    return;
  }

  MeshPacket p;
  memcpy(&p, data, sizeof(p));

  for (auto &pe : peers) {
    if (pe.id == p.nodeId) {
      pe.last = millis();
      pe.seen = true;
      pe.data = p;
      pe.rssi = info->rx_ctrl ? info->rx_ctrl->rssi : -60;
    }
  }
}

char computeLeader() {
  char best = MY_ID;
  uint8_t bestPrio = MY_PRIO;
  uint32_t now = millis();

  for (auto &pe : peers) {
    pe.alive = pe.seen && ((now - pe.last) < TIMEOUT);

    if (pe.alive && pe.prio > bestPrio) {
      bestPrio = pe.prio;
      best = pe.id;
    }
  }

  return best;
}

String buildSnapshot() {
  StaticJsonDocument<2048> j;

  j["type"] = "state";
  j["ts"] = millis();
  j["leader"] = String(curLeader);

  int aliveCount = 1;
  for (auto &pe : peers) {
    if (pe.alive) aliveCount++;
  }

  if (aliveCount == 3) {
    j["clusterHealth"] = "OK";
  } else if (aliveCount == 2) {
    j["clusterHealth"] = "DEGRADED";
  } else {
    j["clusterHealth"] = "CRITICAL";
  }

  JsonArray nodes = j.createNestedArray("nodes");

  auto addNode = [&](char id, const char* role, int prio, uint8_t state,
                     bool alive, uint32_t lastSeen, int rssi,
                     uint32_t uptime, uint32_t sq) {
    JsonObject n = nodes.createNestedObject();

    const char* states[] = {
      "BOOT", "JOIN", "ACTIVE", "STANDBY", "FAILOVER", "DEGRADED"
    };

    n["id"] = String(id);
    n["role"] = role;
    n["priority"] = prio;
    n["state"] = alive ? states[state] : "DOWN";
    n["alive"] = alive;
    n["lastSeenMs"] = lastSeen;
    n["rssi"] = rssi;
    n["uptimeS"] = uptime;
    n["seq"] = sq;
  };

  for (auto &pe : peers) {
    const char* role = (pe.id == 'A') ? "Sensor-Primary" : "HMI-Standby";

    addNode(
      pe.id,
      role,
      pe.prio,
      pe.data.state,
      pe.alive,
      pe.alive ? millis() - pe.last : 0,
      pe.rssi,
      pe.data.uptimeS,
      pe.data.seq
    );
  }

  addNode(
    'C',
    "Dashboard-Agg",
    PRIO_C,
    me.state,
    true,
    0,
    -40,
    (millis() - bootMs) / 1000,
    seq
  );

  MeshPacket* a = nullptr;
  MeshPacket* b = nullptr;

  for (auto &pe : peers) {
    if (pe.id == 'A') {
      a = &pe.data;   // last known A data, even if A is down
    }

    if (pe.id == 'B' && pe.alive) {
      b = &pe.data;   // B may also carry last known A data
    }
  }

  // Priority:
  // 1. If A is alive, use A live data.
  // 2. If A is down but B is alive, use B's forwarded last-known A data.
  // 3. If both are unavailable, use Node C's stored last-known A packet.
  MeshPacket* sensorSource = nullptr;

  if (peers[0].alive && a != nullptr) {
    sensorSource = a;
  } else if (b != nullptr) {
    sensorSource = b;
  } else if (a != nullptr) {
    sensorSource = a;
  }

  JsonObject s = j.createNestedObject("sensors");

  s["vibration"] = sensorSource ? sensorSource->vibration : 0;
  s["tilt"] = sensorSource ? sensorSource->tilt : 0;
  s["ldrRaw"] = sensorSource ? sensorSource->ldrRaw : 0;

  const char* lightStates[] = {"DARK", "NORMAL", "BRIGHT"};
  s["light"] = sensorSource ? lightStates[sensorSource->light < 3 ? sensorSource->light : 1] : "N/A";
  s["motionAlert"] = sensorSource ? (bool)sensorSource->motionAlert : false;

  if (peers[0].alive) {
    s["source"] = "A LIVE";
  } else if (b != nullptr) {
    s["source"] = "B LAST-KNOWN";
  } else {
    s["source"] = "C LAST-KNOWN";
  }

  JsonArray links = j.createNestedArray("links");

  auto addLink = [&](const char* from, const char* to, bool up, int rssi) {
    JsonObject l = links.createNestedObject();

    l["from"] = from;
    l["to"] = to;
    l["up"] = up;
    l["rssi"] = rssi;
    l["latencyMs"] = up ? 2 + (int)(millis() % 6) : 0;
  };

  bool aAlive = peers[0].alive;
  bool bAlive = peers[1].alive;

  addLink("A", "B", aAlive && bAlive, peers[0].rssi);
  addLink("A", "C", aAlive, peers[0].rssi);
  addLink("B", "C", bAlive, peers[1].rssi);

  String out;
  serializeJson(j, out);
  return out;
}

void addBroadcastPeer() {
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, BCAST, 6);

  peerInfo.channel = WIFI_CHANNEL;
  peerInfo.encrypt = false;
  peerInfo.ifidx = WIFI_IF_STA;

  esp_err_t result = esp_now_add_peer(&peerInfo);

  if (result == ESP_OK) {
    Serial.println("Broadcast peer added.");
  } else if (result == ESP_ERR_ESPNOW_EXIST) {
    Serial.println("Broadcast peer already exists.");
  } else {
    Serial.printf("Broadcast peer add failed: %s\n", esp_err_to_name(result));
  }
}

void handleRoot() {
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleState() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", buildSnapshot());
}

void handleFavicon() {
  server.send(204);
}

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(RED_LED, OUTPUT);
  digitalWrite(RED_LED, LOW);

  Serial.println();
  Serial.println("====================================");
  Serial.println("CERBERUS NODE C — DASHBOARD NODE");
  Serial.println("Built-in WebServer version");
  Serial.println("====================================");

  WiFi.mode(WIFI_AP_STA);

  bool apOK = WiFi.softAP(AP_SSID, AP_PASS, WIFI_CHANNEL);
  delay(100);

  esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);

  Serial.print("SoftAP status: ");
  Serial.println(apOK ? "OK" : "FAILED");

  Serial.print("Join Wi-Fi: ");
  Serial.println(AP_SSID);

  Serial.print("Password: ");
  Serial.println(AP_PASS);

  Serial.print("Open dashboard: http://");
  Serial.println(WiFi.softAPIP());

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed.");
    while (true) {
      delay(1000);
    }
  }

  esp_now_register_recv_cb(onRecv);
  addBroadcastPeer();

  server.on("/", handleRoot);
  server.on("/state", handleState);
  server.on("/favicon.ico", handleFavicon);
  server.begin();

  Serial.println("HTTP server started.");

  memset(&me, 0, sizeof(me));

  me.nodeId = MY_ID;
  me.priority = MY_PRIO;
  me.state = S_BOOT;
  me.leaderView = MY_ID;

  bootMs = millis();

  Serial.print("Node C AP MAC: ");
  Serial.println(WiFi.softAPmacAddress());

  Serial.print("Node C STA MAC: ");
  Serial.println(WiFi.macAddress());

  Serial.print("ESP-NOW Channel: ");
  Serial.println(WIFI_CHANNEL);
}

void loop() {
  server.handleClient();

  uint32_t now = millis();

  if (now - tLoop >= LOOP_TICK) {
    tLoop = now;

    curLeader = computeLeader();

    me.leaderView = curLeader;
    me.state = (curLeader == MY_ID) ? S_FAILOVER : S_STANDBY;

    digitalWrite(RED_LED, curLeader == MY_ID ? HIGH : LOW);
  }

  if (now - tHB >= HB_INTERVAL) {
    tHB = now;

    me.seq = ++seq;
    me.uptimeS = (now - bootMs) / 1000;

    esp_now_send(BCAST, (uint8_t*)&me, sizeof(me));

    Serial.printf(
      "C HB #%lu | Leader=%c | State=%s | A=%s | B=%s | IP=%s\n",
      (unsigned long)me.seq,
      curLeader,
      me.state == S_FAILOVER ? "FAILOVER" : "STANDBY",
      peers[0].alive ? "alive" : "dead",
      peers[1].alive ? "alive" : "dead",
      WiFi.softAPIP().toString().c_str()
    );
  }
}