#pragma once
#include <Arduino.h>

#define PRIO_A 3
#define PRIO_B 2
#define PRIO_C 1

const uint32_t HB_INTERVAL = 2000;
const uint32_t TIMEOUT     = 7000;
const uint32_t LOOP_TICK   = 250;

static uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

enum {
  S_BOOT = 0,
  S_JOIN = 1,
  S_ACTIVE = 2,
  S_STANDBY = 3,
  S_FAILOVER = 4,
  S_DEGRADED = 5
};

// ---- CERBERUS 2.0 : sensor roles ----
#define SROLE_NONE    0
#define SROLE_PRIMARY 1
#define SROLE_BACKUP  2

// ---- CERBERUS 2.0 : server-room environment thresholds ----
const float TEMP_ALERT_C  = 35.0;   // alert if temperature above this (deg C)
const float HUM_ALERT_PCT = 70.0;   // alert if humidity above this (%)

// ---- Shared mesh packet (identical on A, B, C) ----
typedef struct {
  char nodeId;
  uint8_t priority;
  uint8_t state;
  uint32_t seq;
  uint32_t uptimeS;

  int ldrRaw;
  uint8_t light;
  float vibration;
  float tilt;
  uint8_t motionAlert;

  uint8_t leaderView;

  // ---- CERBERUS 2.0 Phase 2: environment sensing ----
  float temperature;   // deg C (DHT11); valid only if hasEnv == 1
  float humidity;      // % RH  (DHT11); valid only if hasEnv == 1
  uint8_t hasEnv;      // 1 = this packet carries valid temp/humidity
  uint8_t sensorRole;  // SROLE_PRIMARY / SROLE_BACKUP / SROLE_NONE
  uint8_t envAlert;    // 1 = temp/humidity outside safe server-room range
} MeshPacket;

#define WIFI_CHANNEL 6
