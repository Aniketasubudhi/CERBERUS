#include "cerberus_common.h"

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_err.h>
#include <Wire.h>
#include <math.h>
#include <DHT.h>

// ======================================================
// CERBERUS NODE A — SENSOR-PRIMARY
// MPU6500 direct I2C version, no Adafruit MPU library.
// Hardware:
// MPU6500: SDA=21, SCL=22
// LDR midpoint: GPIO 34
// Green LED: GPIO 2
// ======================================================

const char MY_ID = 'A';
const uint8_t MY_PRIO = PRIO_A;

const int LDR_PIN = 34;
const int GREEN_LED = 2;

// ---- MPU6500 I2C addresses ----
const uint8_t MPU_ADDR_1 = 0x68;
const uint8_t MPU_ADDR_2 = 0x69;
uint8_t mpuAddr = MPU_ADDR_1;

// ---- MPU6500 registers ----
const uint8_t REG_SMPLRT_DIV    = 0x19;
const uint8_t REG_CONFIG        = 0x1A;
const uint8_t REG_GYRO_CONFIG   = 0x1B;
const uint8_t REG_ACCEL_CONFIG  = 0x1C;
const uint8_t REG_ACCEL_CONFIG2 = 0x1D;
const uint8_t REG_ACCEL_XOUT_H  = 0x3B;
const uint8_t REG_PWR_MGMT_1    = 0x6B;
const uint8_t REG_PWR_MGMT_2    = 0x6C;
const uint8_t REG_WHO_AM_I      = 0x75;

// We configure accel full-scale to ±8g.
// For ±8g, sensitivity is 4096 LSB/g.
const float ACCEL_SCALE_8G = 4096.0;
const float G_TO_MS2 = 9.80665;

// ---- Detection thresholds ----
const float TILT_TH = 30.0;     // tilt alert threshold in degrees
const int DARK_TH = 1200;       // adjust after testing
const int BRIGHT_TH = 3000;     // adjust after testing
const float Z_K = 3.0;          // anomaly sensitivity

bool mpuOK = false;

// ---- DHT11 (Phase 2) ----
#define DHT_PIN 4
#define DHT_TYPE DHT11
DHT dht(DHT_PIN, DHT_TYPE);

MeshPacket me;

struct Peer {
  char id;
  uint32_t last;
  uint8_t prio;
  bool seen;
  bool alive;
};

Peer peers[2] = {
  {'B', 0, PRIO_B, false, false},
  {'C', 0, PRIO_C, false, false}
};

uint32_t tHB = 0;
uint32_t tLoop = 0;
uint32_t bootMs = 0;
uint32_t seq = 0;

float aPrev = 0.0;
bool firstAccel = true;

// Rolling anomaly stats
float rMean = 0.3;
float rVar = 0.04;

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

// ======================================================
// MPU6500 LOW-LEVEL I2C FUNCTIONS
// ======================================================

bool i2cPing(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

bool i2cWrite8(uint8_t addr, uint8_t reg, uint8_t value) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool i2cReadBytes(uint8_t addr, uint8_t reg, uint8_t* buffer, uint8_t len) {
  Wire.beginTransmission(addr);
  Wire.write(reg);

  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  uint8_t got = Wire.requestFrom((int)addr, (int)len, (int)true);

  if (got != len) {
    return false;
  }

  for (uint8_t i = 0; i < len; i++) {
    buffer[i] = Wire.read();
  }

  return true;
}

uint8_t i2cRead8(uint8_t addr, uint8_t reg) {
  uint8_t value = 0xFF;
  i2cReadBytes(addr, reg, &value, 1);
  return value;
}

bool beginMPU6500() {
  if (i2cPing(MPU_ADDR_1)) {
    mpuAddr = MPU_ADDR_1;
  } else if (i2cPing(MPU_ADDR_2)) {
    mpuAddr = MPU_ADDR_2;
  } else {
    Serial.println("ERROR: MPU6500 not found at 0x68 or 0x69.");
    return false;
  }

  uint8_t who = i2cRead8(mpuAddr, REG_WHO_AM_I);

  Serial.print("MPU6500 found at I2C address 0x");
  Serial.println(mpuAddr, HEX);

  Serial.print("WHO_AM_I = 0x");
  Serial.println(who, HEX);

  if (who != 0x70) {
    Serial.println("WARNING: WHO_AM_I is not 0x70.");
    Serial.println("Some GY-6500/GY-9250 boards may still work, so continuing...");
  }

  // Reset device
  i2cWrite8(mpuAddr, REG_PWR_MGMT_1, 0x80);
  delay(100);

  // Wake up device, auto clock source
  if (!i2cWrite8(mpuAddr, REG_PWR_MGMT_1, 0x01)) {
    Serial.println("ERROR: Failed to wake MPU6500.");
    return false;
  }

  delay(50);

  // Enable all accel and gyro axes
  i2cWrite8(mpuAddr, REG_PWR_MGMT_2, 0x00);

  // Sample rate divider
  i2cWrite8(mpuAddr, REG_SMPLRT_DIV, 0x07);

  // DLPF setting
  i2cWrite8(mpuAddr, REG_CONFIG, 0x03);

  // Gyro ±500 dps, not heavily used here but configured
  i2cWrite8(mpuAddr, REG_GYRO_CONFIG, 0x08);

  // Accel ±8g: ACCEL_FS_SEL bits = 10, so register value = 0x10
  i2cWrite8(mpuAddr, REG_ACCEL_CONFIG, 0x10);

  // Accelerometer low-pass filter
  i2cWrite8(mpuAddr, REG_ACCEL_CONFIG2, 0x03);

  Serial.println("MPU6500 initialized.");
  return true;
}

bool readMPU6500Accel(float &ax, float &ay, float &az) {
  uint8_t raw[6];

  if (!i2cReadBytes(mpuAddr, REG_ACCEL_XOUT_H, raw, 6)) {
    return false;
  }

  int16_t rx = (int16_t)((raw[0] << 8) | raw[1]);
  int16_t ry = (int16_t)((raw[2] << 8) | raw[3]);
  int16_t rz = (int16_t)((raw[4] << 8) | raw[5]);

  ax = ((float)rx / ACCEL_SCALE_8G) * G_TO_MS2;
  ay = ((float)ry / ACCEL_SCALE_8G) * G_TO_MS2;
  az = ((float)rz / ACCEL_SCALE_8G) * G_TO_MS2;

  return true;
}

// ======================================================
// ESP-NOW RECEIVE
// ======================================================

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
}

// ======================================================
// SENSOR READING
// ======================================================

void readSensors() {
  if (mpuOK) {
    float ax, ay, az;

    if (readMPU6500Accel(ax, ay, az)) {
      float totalAccel = sqrt(ax * ax + ay * ay + az * az);

      float vibration = 0.0;

      if (firstAccel) {
        aPrev = totalAccel;
        firstAccel = false;
      } else {
        vibration = fabs(totalAccel - aPrev);
        aPrev = totalAccel;
      }

      me.vibration = vibration;
      me.tilt = atan2(ax, sqrt(ay * ay + az * az)) * 180.0 / PI;

      // Rolling z-score anomaly detection
      rMean += (vibration - rMean) / 20.0;
      rVar += (((vibration - rMean) * (vibration - rMean)) - rVar) / 20.0;

      if (rVar < 0.0) {
        rVar = 0.0;
      }

      float sd = sqrt(rVar > 0.000001 ? rVar : 0.000001);
      float z = (vibration - rMean) / sd;

      me.motionAlert = (fabs(z) > Z_K || fabs(me.tilt) > TILT_TH) ? 1 : 0;
    } else {
      Serial.println("WARNING: MPU6500 read failed.");

      me.vibration = 0.0;
      me.tilt = 0.0;
      me.motionAlert = 0;
    }
  } else {
    me.vibration = 0.0;
    me.tilt = 0.0;
    me.motionAlert = 0;
  }

  me.ldrRaw = analogRead(LDR_PIN);

  if (me.ldrRaw < DARK_TH) {
    me.light = 0;       // DARK
  } else if (me.ldrRaw > BRIGHT_TH) {
    me.light = 2;       // BRIGHT
  } else {
    me.light = 1;       // NORMAL
  }

  // ---- DHT11 environment (Phase 2) ----
  float h = dht.readHumidity();
  float t = dht.readTemperature();
  if (!isnan(h) && !isnan(t)) {
    me.temperature = t;
    me.humidity = h;
    me.hasEnv = 1;
    me.envAlert = (t > TEMP_ALERT_C || h > HUM_ALERT_PCT) ? 1 : 0;
  } else {
    me.hasEnv = 0;
    me.envAlert = 0;
  }
}

// ======================================================
// LEADER LOGIC
// ======================================================

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

// ======================================================
// ESP-NOW SETUP
// ======================================================

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

// ======================================================
// SETUP
// ======================================================

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("=================================");
  Serial.println("CERBERUS NODE A — SENSOR-PRIMARY");
  Serial.println("MPU6500 DIRECT I2C VERSION");
  Serial.println("=================================");

  pinMode(GREEN_LED, OUTPUT);
  digitalWrite(GREEN_LED, LOW);

  analogReadResolution(12);

  dht.begin();

  Wire.begin(21, 22);
  Wire.setClock(400000);

  mpuOK = beginMPU6500();

  if (!mpuOK) {
    Serial.println("WARNING: MPU6500 missing or not detected.");
    Serial.println("Node A will still send LDR + heartbeat data.");
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
  me.nodeId = MY_ID;
  me.priority = MY_PRIO;
  me.state = S_BOOT;
  me.leaderView = MY_ID;
  me.sensorRole = SROLE_PRIMARY;

  bootMs = millis();

  Serial.print("Node A MAC: ");
  Serial.println(WiFi.macAddress());

  Serial.print("ESP-NOW channel: ");
  Serial.println(WIFI_CHANNEL);
}

// ======================================================
// LOOP
// ======================================================

void loop() {
  uint32_t now = millis();

  if (now - tLoop >= LOOP_TICK) {
    tLoop = now;

    char leader = computeLeader();

    me.leaderView = leader;
    me.state = (leader == MY_ID) ? S_ACTIVE : S_STANDBY;

    digitalWrite(GREEN_LED, me.state == S_ACTIVE ? HIGH : LOW);
  }

  if (now - tHB >= HB_INTERVAL) {
    tHB = now;

    readSensors();

    me.seq = ++seq;
    me.uptimeS = (now - bootMs) / 1000;

    esp_err_t result = esp_now_send(BCAST, (uint8_t*)&me, sizeof(me));

    Serial.printf(
      "A HB #%lu | %s | Ldr=%d Vib=%.2f Tilt=%.1f T=%.1fC H=%.0f%% Alert=%d | Env=%d | Send=%s | B=%s C=%s\n",
      (unsigned long)me.seq,
      stateName(me.state),
      me.ldrRaw,
      me.vibration,
      me.tilt,
      me.temperature,
      me.humidity,
      me.motionAlert,
      me.envAlert,
      esp_err_to_name(result),
      peers[0].alive ? "alive" : "dead",
      peers[1].alive ? "alive" : "dead"
    );
  }
}