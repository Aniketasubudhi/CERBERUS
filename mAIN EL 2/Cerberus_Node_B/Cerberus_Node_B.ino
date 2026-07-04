#include "cerberus_common.h"

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>

// ======================================================
// CERBERUS NODE B — HMI-STANDBY
//
// Hardware:
// OLED SSD1306: SDA=21, SCL=22, address usually 0x3C
// Buzzer: GPIO 5
// Amber LED: GPIO 4
// Backup DHT11: GPIO 18
// Backup LDR: GPIO 34
// ======================================================

const char MY_ID = 'B';
const uint8_t MY_PRIO = PRIO_B;

const int BUZZER = 5;
const int AMBER_LED = 4;

// ------------------------------------------------------
// Node B backup sensors
// ------------------------------------------------------

const int LDR_PIN = 34;
const int DARK_TH = 1200;
const int BRIGHT_TH = 3000;

#define DHT_PIN 18
#define DHT_TYPE DHT11

DHT dht(DHT_PIN, DHT_TYPE);

// ------------------------------------------------------
// Timing
// ------------------------------------------------------

const uint32_t STARTUP_GRACE_MS = 5000;
const uint32_t OLED_INTERVAL = 500;

// How long the takeover message remains on the OLED
const uint32_t FAILOVER_BANNER_MS = 4000;

// ------------------------------------------------------
// OLED
// ------------------------------------------------------

Adafruit_SSD1306 oled(128, 64, &Wire, -1);
bool oledOK = false;

// ------------------------------------------------------
// Mesh data
// ------------------------------------------------------

MeshPacket me;

// Stores the final complete packet received from Node A.
// When A fails, these MPU values remain frozen here.
MeshPacket lastA;
bool haveAData = false;

// ------------------------------------------------------
// Peer tracking
// ------------------------------------------------------

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

// ------------------------------------------------------
// Runtime variables
// ------------------------------------------------------

uint32_t tHB = 0;
uint32_t tLoop = 0;
uint32_t tOLED = 0;
uint32_t bootMs = 0;
uint32_t seq = 0;

bool wasLeader = false;

uint32_t buzzerOffAt = 0;
uint32_t failoverBannerUntil = 0;

// ------------------------------------------------------
// State name
// ------------------------------------------------------

const char* stateName(uint8_t s) {
  switch (s) {
    case S_BOOT:
      return "BOOT";

    case S_JOIN:
      return "JOIN";

    case S_ACTIVE:
      return "ACTIVE";

    case S_STANDBY:
      return "STANDBY";

    case S_FAILOVER:
      return "FAILOVER";

    case S_DEGRADED:
      return "DEGRADED";

    default:
      return "?";
  }
}

// ------------------------------------------------------
// Buzzer functions
// ------------------------------------------------------

void startBeep(uint32_t durationMs) {
  digitalWrite(BUZZER, HIGH);
  buzzerOffAt = millis() + durationMs;
}

void updateBuzzer() {
  if (buzzerOffAt != 0 &&
      (int32_t)(millis() - buzzerOffAt) >= 0) {

    digitalWrite(BUZZER, LOW);
    buzzerOffAt = 0;
  }
}

// ------------------------------------------------------
// Read Node B backup sensors
// ------------------------------------------------------

void readBSensors() {
  // Read B's own LDR
  me.ldrRaw = analogRead(LDR_PIN);

  if (me.ldrRaw < DARK_TH) {
    me.light = 0;  // DARK
  }
  else if (me.ldrRaw > BRIGHT_TH) {
    me.light = 2;  // BRIGHT
  }
  else {
    me.light = 1;  // NORMAL
  }

  // Read B's own DHT11
  float h = dht.readHumidity();
  float t = dht.readTemperature();

  if (!isnan(h) && !isnan(t)) {
    me.temperature = t;
    me.humidity = h;
    me.hasEnv = 1;

    me.envAlert =
      (t > TEMP_ALERT_C || h > HUM_ALERT_PCT) ? 1 : 0;
  }
  else {
    // Keep packet marked invalid if the DHT read fails
    me.hasEnv = 0;
    me.envAlert = 0;
  }

  // Node B has no MPU6500.
  // Its transmitted packet does not claim live MPU readings.
  me.vibration = 0.0;
  me.tilt = 0.0;
  me.motionAlert = 0;
}

// ------------------------------------------------------
// ESP-NOW receive callback
// ------------------------------------------------------

void onRecv(
  const esp_now_recv_info_t* info,
  const uint8_t* data,
  int len
) {
  if (len != sizeof(MeshPacket)) {
    Serial.printf(
      "Bad packet size: %d, expected: %u\n",
      len,
      (unsigned)sizeof(MeshPacket)
    );

    return;
  }

  MeshPacket p;
  memcpy(&p, data, sizeof(p));

  // Update peer heartbeat information
  for (auto &pe : peers) {
    if (pe.id == p.nodeId) {
      pe.last = millis();
      pe.seen = true;
    }
  }

  // Always preserve the newest full packet from A.
  // When A dies, this remains available as last-known data.
  if (p.nodeId == 'A') {
    lastA = p;
    haveAData = true;
  }
}

// ------------------------------------------------------
// Peer alive check
// ------------------------------------------------------

bool isPeerAlive(char id) {
  for (auto &pe : peers) {
    if (pe.id == id) {
      return pe.alive;
    }
  }

  return false;
}

// ------------------------------------------------------
// Leader election
// ------------------------------------------------------

char computeLeader() {
  char best = MY_ID;
  uint8_t bestPrio = MY_PRIO;

  uint32_t now = millis();

  for (auto &pe : peers) {
    pe.alive =
      pe.seen &&
      ((now - pe.last) < TIMEOUT);

    if (pe.alive && pe.prio > bestPrio) {
      bestPrio = pe.prio;
      best = pe.id;
    }
  }

  return best;
}

// ------------------------------------------------------
// Failover banner timer
// ------------------------------------------------------

bool isFailoverBannerActive() {
  if (failoverBannerUntil == 0) {
    return false;
  }

  return
    (int32_t)(millis() - failoverBannerUntil) < 0;
}

// ------------------------------------------------------
// OLED drawing
// ------------------------------------------------------

void drawOLED(char leader, bool amLeader) {
  if (!oledOK) {
    return;
  }

  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(0, 0);

  // ====================================================
  // SCREEN 1:
  // Temporary failover announcement
  // ====================================================

  if (amLeader && isFailoverBannerActive()) {
    oled.setTextSize(2);
    oled.println("FAILOVER");

    oled.setTextSize(1);
    oled.println();

    oled.print("Leader: ");
    oled.println(leader);

    oled.println("A is DOWN");
    oled.println("B taking over");

    oled.display();
    return;
  }

  // ====================================================
  // SCREEN 2:
  // Original display while Node A is alive and leader
  // This section remains in your original format.
  // ====================================================

  if (!amLeader) {
    oled.setTextSize(1);

    oled.println("CERBERUS NODE B");
    oled.println("----------------");

    oled.print("Leader: ");
    oled.println(leader);

    oled.print("B State: ");
    oled.println(stateName(me.state));

    // B's own backup temperature and humidity
    oled.print("B Env: ");

    if (me.hasEnv) {
      oled.print(me.temperature, 0);
      oled.print("C ");

      oled.print(me.humidity, 0);
      oled.println("%");
    }
    else {
      oled.println("--");
    }

    oled.print("A Link: ");
    oled.println(
      isPeerAlive('A') ? "alive" : "dead"
    );

    // Show Node A's complete live sensor packet
    if (haveAData && isPeerAlive('A')) {
      oled.print("LDR: ");
      oled.println(lastA.ldrRaw);

      oled.print("Vib: ");
      oled.println(lastA.vibration, 2);

      oled.print("Tilt: ");
      oled.print(lastA.tilt, 1);
      oled.println(" deg");

      oled.print("Alert: ");
      oled.println(
        lastA.motionAlert ? "YES" : "NO"
      );
    }
    else {
      oled.println("Waiting for A data...");
    }

    oled.display();
    return;
  }

  // ====================================================
  // SCREEN 3:
  // Node B leader after the failover message finishes
  //
  // Same display format:
  // - Environment comes from B's DHT11
  // - LDR comes from B's LDR
  // - Vibration and tilt are last-known from A
  // - MPU motion alert is last-known from A
  // ====================================================

  oled.setTextSize(1);

  oled.println("CERBERUS NODE B");
  oled.println("----------------");

  oled.print("Leader: ");
  oled.println(leader);

  oled.print("B State: ");
  oled.println(stateName(me.state));

  // Node B's live DHT11 values
  oled.print("B Env: ");

  if (me.hasEnv) {
    oled.print(me.temperature, 0);
    oled.print("C ");

    oled.print(me.humidity, 0);
    oled.println("%");
  }
  else {
    oled.println("--");
  }

  oled.print("A Link: ");
  oled.println(
    isPeerAlive('A') ? "alive" : "dead"
  );

  // Node B's own live backup LDR
  oled.print("LDR: ");
  oled.println(me.ldrRaw);

  // MPU values remain at the last packet received from A
  if (haveAData) {
    oled.print("Vib: ");
    oled.println(lastA.vibration, 2);

    oled.print("Tilt: ");
    oled.print(lastA.tilt, 1);
    oled.println(" deg");
  }
  else {
    oled.println("Vib: --");
    oled.println("Tilt: --");
  }

  // B environmental alert is live.
  // A motion alert is frozen at its last-known state.
  bool combinedAlert =
    me.envAlert ||
    (haveAData && lastA.motionAlert);

  oled.print("Alert: ");
  oled.println(combinedAlert ? "YES" : "NO");

  oled.display();
}

// ------------------------------------------------------
// Add ESP-NOW broadcast peer
// ------------------------------------------------------

void addBroadcastPeer() {
  esp_now_peer_info_t peerInfo = {};

  memcpy(peerInfo.peer_addr, BCAST, 6);

  peerInfo.channel = WIFI_CHANNEL;
  peerInfo.encrypt = false;
  peerInfo.ifidx = WIFI_IF_STA;

  esp_err_t result =
    esp_now_add_peer(&peerInfo);

  if (result == ESP_OK) {
    Serial.println("Broadcast peer added.");
  }
  else if (result == ESP_ERR_ESPNOW_EXIST) {
    Serial.println(
      "Broadcast peer already exists."
    );
  }
  else {
    Serial.printf(
      "Failed to add broadcast peer: %s\n",
      esp_err_to_name(result)
    );
  }
}

// ------------------------------------------------------
// Setup
// ------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("=============================");
  Serial.println("CERBERUS NODE B - HMI-STANDBY");
  Serial.println("=============================");

  pinMode(BUZZER, OUTPUT);
  pinMode(AMBER_LED, OUTPUT);

  digitalWrite(BUZZER, LOW);
  digitalWrite(AMBER_LED, LOW);

  // OLED I2C
  Wire.begin(21, 22);

  // Backup sensors
  analogReadResolution(12);
  dht.begin();

  // OLED startup
  oledOK =
    oled.begin(
      SSD1306_SWITCHCAPVCC,
      0x3C
    );

  if (oledOK) {
    oled.clearDisplay();
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(0, 0);
    oled.setTextSize(1);

    oled.println("CERBERUS NODE B");
    oled.println("Booting...");
    oled.display();
  }
  else {
    Serial.println(
      "WARNING: OLED not detected at 0x3C."
    );
  }

  // ESP-NOW Wi-Fi setup
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  esp_wifi_set_channel(
    WIFI_CHANNEL,
    WIFI_SECOND_CHAN_NONE
  );

  if (esp_now_init() != ESP_OK) {
    Serial.println(
      "ERROR: ESP-NOW init failed."
    );

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
  me.sensorRole = SROLE_BACKUP;

  bootMs = millis();

  Serial.print("Node B MAC: ");
  Serial.println(WiFi.macAddress());

  Serial.print("ESP-NOW channel: ");
  Serial.println(WIFI_CHANNEL);

  startBeep(150);
}

// ------------------------------------------------------
// Main loop
// ------------------------------------------------------

void loop() {
  uint32_t now = millis();

  updateBuzzer();

  // ----------------------------------------------------
  // Leader election and FSM
  // ----------------------------------------------------

  if (now - tLoop >= LOOP_TICK) {
    tLoop = now;

    char leader = computeLeader();

    bool startupGrace =
      ((now - bootMs) < STARTUP_GRACE_MS) &&
      !isPeerAlive('A');

    if (startupGrace) {
      me.state = S_JOIN;
      me.leaderView = '-';

      digitalWrite(
        AMBER_LED,
        LOW
      );
    }
    else {
      bool amLeader =
        (leader == MY_ID);

      me.leaderView = leader;

      me.state =
        amLeader
          ? S_FAILOVER
          : S_STANDBY;

      digitalWrite(
        AMBER_LED,
        amLeader ? HIGH : LOW
      );

      // This runs once when B first takes over.
      if (amLeader && !wasLeader) {
        startBeep(1500);

        failoverBannerUntil =
          now + FAILOVER_BANNER_MS;
      }

      // A returned, so clear any old banner timer.
      if (!amLeader && wasLeader) {
        failoverBannerUntil = 0;
      }

      wasLeader = amLeader;
    }
  }

  // ----------------------------------------------------
  // OLED refresh
  // ----------------------------------------------------

  if (now - tOLED >= OLED_INTERVAL) {
    tOLED = now;

    char leader =
      (char)me.leaderView;

    bool amLeader =
      (me.state == S_FAILOVER);

    drawOLED(
      leader,
      amLeader
    );
  }

  // ----------------------------------------------------
  // Heartbeat and backup sensor reading
  // ----------------------------------------------------

  if (now - tHB >= HB_INTERVAL) {
    tHB = now;

    me.seq = ++seq;
    me.uptimeS =
      (now - bootMs) / 1000;

    // Read B's own sensors.
    // DHT11 is intentionally not read every 500 ms.
    readBSensors();

    esp_err_t result =
      esp_now_send(
        BCAST,
        (uint8_t*)&me,
        sizeof(me)
      );

    Serial.printf(
      "B HB #%lu | %s | Leader=%c | "
      "A=%s C=%s | "
      "B(own) LDR=%d T=%.1fC H=%.0f%% "
      "Env=%d | Send=%s\n",

      (unsigned long)me.seq,
      stateName(me.state),
      (char)me.leaderView,

      peers[0].alive
        ? "alive"
        : "dead",

      peers[1].alive
        ? "alive"
        : "dead",

      me.ldrRaw,
      me.temperature,
      me.humidity,
      me.envAlert,

      esp_err_to_name(result)
    );
  }
}