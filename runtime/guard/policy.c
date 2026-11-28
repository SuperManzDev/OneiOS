// SPDX-License-Identifier: MIT
#include "guard.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static bool known(int32_t value) { return value != GUARD_UNKNOWN; }

void guard_limits_default(guard_limits *out)
{
  *out = (guard_limits) {
    .soc_stop = 1000,       // 100.0 C: below the sustained 103-105 C diagnostic incident
    .gpu_stop = 850,        // 85.0 C: the observed diagnostic stop point
    .board_stop = 550,      // 55.0 C: conservative case/board bound
    .battery_stop = 445,    // 44.5 C: conservative cell bound
    .samples = GUARD_DEFAULT_SAMPLES,
    .sensor_loss_samples = 10,
  };
}

static int parse_tenths(const char *text, int32_t *out)
{
  if (!text || !*text) return -1;
  errno = 0;
  char *end = NULL;
  double value = strtod(text, &end);
  if (errno || !end || end == text || *end || !isfinite(value) || value < -40.0 || value > 200.0) return -1;
  long tenths = (long)(value * 10.0 + (value >= 0 ? 0.5 : -0.5));
  if (tenths < INT32_MIN || tenths > INT32_MAX) return -1;
  *out = (int32_t)tenths;
  return 0;
}

static int parse_unsigned(const char *text, unsigned min, unsigned max, unsigned *out)
{
  if (!text || !*text) return -1;
  errno = 0;
  char *end = NULL;
  unsigned long value = strtoul(text, &end, 10);
  if (errno || !end || *end || value < min || value > max) return -1;
  *out = (unsigned)value;
  return 0;
}

int guard_limits_parse(guard_limits *limits, const char *name, const char *value)
{
  guard_limits next = *limits;
  int rc = -1;
  if (!strcmp(name, "soc")) rc = parse_tenths(value, &next.soc_stop);
  else if (!strcmp(name, "gpu")) rc = parse_tenths(value, &next.gpu_stop);
  else if (!strcmp(name, "board")) rc = parse_tenths(value, &next.board_stop);
  else if (!strcmp(name, "battery")) rc = parse_tenths(value, &next.battery_stop);
  else if (!strcmp(name, "samples")) rc = parse_unsigned(value, 1, 60, &next.samples);
  else if (!strcmp(name, "sensor-loss-samples")) rc = parse_unsigned(value, 1, 600, &next.sensor_loss_samples);
  if (rc == 0) *limits = next;
  return rc;
}

static unsigned count(bool hot, unsigned was)
{
  return hot ? (was == UINT_MAX ? was : was + 1) : 0;
}

guard_decision guard_policy_step(guard_policy *policy, const guard_limits *limits, const br_thermal_raw *raw)
{
  int32_t board = known(raw->skin) ? raw->skin : raw->ap;
  policy->soc_hot = count(known(raw->soc) && raw->soc >= limits->soc_stop, policy->soc_hot);
  policy->gpu_hot = count(known(raw->gpu) && raw->gpu >= limits->gpu_stop, policy->gpu_hot);
  policy->board_hot = count(known(board) && board >= limits->board_stop, policy->board_hot);
  policy->battery_hot = count(known(raw->battery) && raw->battery >= limits->battery_stop, policy->battery_hot);

  policy->soc_missing = count(!known(raw->soc), policy->soc_missing);
  policy->gpu_missing = count(!known(raw->gpu), policy->gpu_missing);
  policy->board_missing = count(!known(board), policy->board_missing);
  policy->battery_missing = count(!known(raw->battery), policy->battery_missing);

  if (policy->battery_hot >= limits->samples) return GUARD_STOP_BATTERY;
  if (policy->board_hot >= limits->samples) return GUARD_STOP_BOARD;
  if (policy->gpu_hot >= limits->samples) return GUARD_STOP_GPU;
  if (policy->soc_hot >= limits->samples) return GUARD_STOP_SOC;
  if (policy->soc_missing >= limits->sensor_loss_samples || policy->gpu_missing >= limits->sensor_loss_samples ||
      policy->board_missing >= limits->sensor_loss_samples || policy->battery_missing >= limits->sensor_loss_samples)
    return GUARD_STOP_SENSOR_LOSS;
  return GUARD_CONTINUE;
}

const char *guard_decision_name(guard_decision decision)
{
  switch (decision) {
    case GUARD_STOP_SOC: return "thermal-soc";
    case GUARD_STOP_GPU: return "thermal-gpu";
    case GUARD_STOP_BOARD: return "thermal-board";
    case GUARD_STOP_BATTERY: return "thermal-battery";
    case GUARD_STOP_SENSOR_LOSS: return "sensor-loss";
    default: return "none";
  }
}
