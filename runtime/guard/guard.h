// SPDX-License-Identifier: MIT
#ifndef ONEIOS_GUARD_H
#define ONEIOS_GUARD_H

#include <stdbool.h>
#include <stdint.h>

#include "../bridges/thermal/thermal.h"

#define GUARD_UNKNOWN BR_THERMAL_UNKNOWN
#define GUARD_DEFAULT_SAMPLES 3u

typedef enum {
  GUARD_CONTINUE = 0,
  GUARD_STOP_SOC,
  GUARD_STOP_GPU,
  GUARD_STOP_BOARD,
  GUARD_STOP_BATTERY,
  GUARD_STOP_SENSOR_LOSS,
} guard_decision;

typedef struct {
  int32_t soc_stop;
  int32_t gpu_stop;
  int32_t board_stop;
  int32_t battery_stop;
  unsigned samples;
  unsigned sensor_loss_samples;
} guard_limits;

typedef struct {
  unsigned soc_hot;
  unsigned gpu_hot;
  unsigned board_hot;
  unsigned battery_hot;
  unsigned soc_missing;
  unsigned gpu_missing;
  unsigned board_missing;
  unsigned battery_missing;
} guard_policy;

void guard_limits_default(guard_limits *out);
int guard_limits_parse(guard_limits *limits, const char *name, const char *value);
guard_decision guard_policy_step(guard_policy *policy, const guard_limits *limits, const br_thermal_raw *raw);
const char *guard_decision_name(guard_decision decision);

#endif
