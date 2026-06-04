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
} MeshPacket;

#define WIFI_CHANNEL 6