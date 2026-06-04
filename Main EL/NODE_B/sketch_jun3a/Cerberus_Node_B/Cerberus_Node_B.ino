#include "cerberus_common.h"

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ======================================================
// CERBERUS NODE B — HMI-STANDBY
// Hardware:
// OLED SSD1306: SDA=21, SCL=22, address usually 0x3C
// Buzzer: GPIO 5
// Amber LED: GPIO 4
// ======================================================

const char MY_ID = 'B';
const uint8_t MY_PRIO = PRIO_B;

const int BUZZER = 5;
const int AMBER_LED = 4;

const uint32_t STARTUP_GRACE_MS = 5000;
const uint32_t OLED_INTERVAL = 500;

Adafruit_SSD1306 oled(128, 64, &Wire, -1);
bool oledOK = false;

MeshPacket me;
MeshPacket lastA;
bool haveAData = false;

struct Peer {
  char id;
  uint32_t last;
  uint8_t prio;
  bool seen;
  bool alive;
};

Peer peers[2] = {
  {'A', 0, PRIO_A, false, false},
  {'C', 0, PRIO_C, false, false}
};

uint32_t tHB = 0;
uint32_t tLoop = 0;
uint32_t tOLED = 0;
uint32_t bootMs = 0;
uint32_t seq = 0;

bool wasLeader = false;
uint32_t buzzerOffAt = 0;

const char* stateName(uint8_t s) {
  switch (s) {
    case S_BOOT: return "BOOT";
    case S_JOIN: return "JOIN";
    case S_ACTIVE: return "ACTIVE";
    case S_STANDBY: return "STANDBY";
    case S_FAILOVER: return "FAILOVER";
    case S_DEGRADED: return "DEGRADED";
    default: return "?";
  }
}

void startBeep(uint32_t durationMs) {
  digitalWrite(BUZZER, HIGH);
  buzzerOffAt = millis() + durationMs;
}

void updateBuzzer() {
  if (buzzerOffAt != 0 && (int32_t)(millis() - buzzerOffAt) >= 0) {
    digitalWrite(BUZZER, LOW);
    buzzerOffAt = 0;
  }
}

void onRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (len != sizeof(MeshPacket)) {
    Serial.printf("Bad packet size: %d, expected: %u\n", len, (unsigned)sizeof(MeshPacket));
    return;
  }

  MeshPacket p;
  memcpy(&p, data, sizeof(p));

  for (auto &pe : peers) {
    if (pe.id == p.nodeId) {
      pe.last = millis();
      pe.seen = true;
    }
  }

  if (p.nodeId == 'A') {
    lastA = p;
    haveAData = true;
  }
}

bool isPeerAlive(char id) {
  for (auto &pe : peers) {
    if (pe.id == id) {
      return pe.alive;
    }
  }
  return false;
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

void drawOLED(char leader, bool amLeader) {
  if (!oledOK) return;

  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(0, 0);

  if (amLeader) {
    oled.setTextSize(2);
    oled.println("FAILOVER");

    oled.setTextSize(1);
    oled.println();
    oled.print("Leader: ");
    oled.println(leader);
    oled.println("A is DOWN");
    oled.println("B taking over");
  } else {
    oled.setTextSize(1);
    oled.println("CERBERUS NODE B");
    oled.println("----------------");

    oled.print("Leader: ");
    oled.println(leader);

    oled.print("B State: ");
    oled.println(stateName(me.state));

    oled.print("A Link: ");
    oled.println(isPeerAlive('A') ? "alive" : "dead");

    if (haveAData && isPeerAlive('A')) {
      oled.print("LDR: ");
      oled.println(lastA.ldrRaw);

      oled.print("Vib: ");
      oled.println(lastA.vibration, 2);

      oled.print("Tilt: ");
      oled.print(lastA.tilt, 1);
      oled.println(" deg");

      oled.print("Alert: ");
      oled.println(lastA.motionAlert ? "YES" : "NO");
    } else {
      oled.println("Waiting for A data...");
    }
  }

  oled.display();
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
    Serial.printf("Failed to add broadcast peer: %s\n", esp_err_to_name(result));
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("=============================");
  Serial.println("CERBERUS NODE B — HMI-STANDBY");
  Serial.println("=============================");

  pinMode(BUZZER, OUTPUT);
  pinMode(AMBER_LED, OUTPUT);

  digitalWrite(BUZZER, LOW);
  digitalWrite(AMBER_LED, LOW);

  Wire.begin(21, 22);

  oledOK = oled.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  if (oledOK) {
    oled.clearDisplay();
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(0, 0);
    oled.setTextSize(1);
    oled.println("CERBERUS NODE B");
    oled.println("Booting...");
    oled.display();
  } else {
    Serial.println("WARNING: OLED not detected at 0x3C.");
  }

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ERROR: ESP-NOW init failed.");
    while (true) {
      delay(1000);
    }
  }

  esp_now_register_recv_cb(onRecv);
  addBroadcastPeer();

  memset(&me, 0, sizeof(me));
  memset(&lastA, 0, sizeof(lastA));

  me.nodeId = MY_ID;
  me.priority = MY_PRIO;
  me.state = S_BOOT;
  me.leaderView = MY_ID;

  bootMs = millis();

  Serial.print("Node B MAC: ");
  Serial.println(WiFi.macAddress());
  Serial.print("ESP-NOW channel: ");
  Serial.println(WIFI_CHANNEL);

  startBeep(150);  // short boot beep
}

void loop() {
  uint32_t now = millis();

  updateBuzzer();

  if (now - tLoop >= LOOP_TICK) {
    tLoop = now;

    char leader = computeLeader();

    bool startupGrace = ((now - bootMs) < STARTUP_GRACE_MS) && !isPeerAlive('A');

    if (startupGrace) {
      me.state = S_JOIN;
      me.leaderView = '-';
      digitalWrite(AMBER_LED, LOW);
    } else {
      bool amLeader = (leader == MY_ID);

      me.leaderView = leader;
      me.state = amLeader ? S_FAILOVER : S_STANDBY;

      digitalWrite(AMBER_LED, amLeader ? HIGH : LOW);

      if (amLeader && !wasLeader) {
        startBeep(1500);  // long takeover beep
      }

      wasLeader = amLeader;
    }
  }

  if (now - tOLED >= OLED_INTERVAL) {
    tOLED = now;

    char leader = (char)me.leaderView;
    bool amLeader = (me.state == S_FAILOVER);

    drawOLED(leader, amLeader);
  }

  if (now - tHB >= HB_INTERVAL) {
    tHB = now;

    me.seq = ++seq;
    me.uptimeS = (now - bootMs) / 1000;

    // Forward last-known A sensor values in B's packet
    if (haveAData) {
      me.ldrRaw = lastA.ldrRaw;
      me.light = lastA.light;
      me.vibration = lastA.vibration;
      me.tilt = lastA.tilt;
      me.motionAlert = lastA.motionAlert;
    } else {
      me.ldrRaw = 0;
      me.light = 1;
      me.vibration = 0.0;
      me.tilt = 0.0;
      me.motionAlert = 0;
    }

    esp_err_t result = esp_now_send(BCAST, (uint8_t*)&me, sizeof(me));

    Serial.printf(
      "B HB #%lu | State=%s | Leader=%c | A=%s C=%s | LastA LDR=%d Vib=%.2f Tilt=%.1f | Send=%s\n",
      (unsigned long)me.seq,
      stateName(me.state),
      (char)me.leaderView,
      peers[0].alive ? "alive" : "dead",
      peers[1].alive ? "alive" : "dead",
      me.ldrRaw,
      me.vibration,
      me.tilt,
      esp_err_to_name(result)
    );
  }
}