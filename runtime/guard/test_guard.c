// SPDX-License-Identifier: MIT
#include "guard.h"

#include <stdio.h>
#include <string.h>

static int checks, failed;
#define CHECK(x) do { checks++; if (!(x)) { failed++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)

static br_thermal_raw raw(int32_t soc, int32_t gpu, int32_t board, int32_t battery)
{
  br_thermal_raw out;
  memset(&out, 0, sizeof out);
  out.cpu = out.ddr = out.xo = out.lrp = GUARD_UNKNOWN;
  out.soc = soc;
  out.gpu = gpu;
  out.skin = board;
  out.ap = board;
  out.battery = battery;
  return out;
}

static void defaults_and_parse(void)
{
  guard_limits l, before;
  guard_limits_default(&l);
  CHECK(l.soc_stop == 1000 && l.gpu_stop == 850 && l.board_stop == 550 && l.battery_stop == 445);
  CHECK(l.samples == 3 && l.sensor_loss_samples == 10);
  CHECK(guard_limits_parse(&l, "battery", "43.5") == 0 && l.battery_stop == 435);
  CHECK(guard_limits_parse(&l, "soc", "102") == 0 && l.soc_stop == 1020);
  CHECK(guard_limits_parse(&l, "samples", "4") == 0 && l.samples == 4);
  before = l;
  CHECK(guard_limits_parse(&l, "samples", "0") == -1 && !memcmp(&l, &before, sizeof l));
  CHECK(guard_limits_parse(&l, "gpu", "wat") == -1 && !memcmp(&l, &before, sizeof l));
  CHECK(guard_limits_parse(&l, "gpu", "nan") == -1 && !memcmp(&l, &before, sizeof l));
  CHECK(guard_limits_parse(&l, "gpu", "inf") == -1 && !memcmp(&l, &before, sizeof l));
  CHECK(guard_limits_parse(&l, "unknown", "1") == -1 && !memcmp(&l, &before, sizeof l));
}

static void sustained_heat(void)
{
  guard_limits l;
  guard_limits_default(&l);
  guard_policy p = { 0 };
  br_thermal_raw r = raw(1000, 600, 400, 300);
  CHECK(guard_policy_step(&p, &l, &r) == GUARD_CONTINUE);
  CHECK(guard_policy_step(&p, &l, &r) == GUARD_CONTINUE);
  CHECK(guard_policy_step(&p, &l, &r) == GUARD_STOP_SOC);
  r.soc = 900;
  CHECK(guard_policy_step(&p, &l, &r) == GUARD_CONTINUE && p.soc_hot == 0);

  memset(&p, 0, sizeof p);
  r = raw(600, 900, 400, 300);
  guard_policy_step(&p, &l, &r);
  guard_policy_step(&p, &l, &r);
  CHECK(guard_policy_step(&p, &l, &r) == GUARD_STOP_GPU);

  memset(&p, 0, sizeof p);
  r = raw(600, 600, 560, 300);
  guard_policy_step(&p, &l, &r);
  guard_policy_step(&p, &l, &r);
  CHECK(guard_policy_step(&p, &l, &r) == GUARD_STOP_BOARD);

  memset(&p, 0, sizeof p);
  r = raw(600, 600, 400, 450);
  guard_policy_step(&p, &l, &r);
  guard_policy_step(&p, &l, &r);
  CHECK(guard_policy_step(&p, &l, &r) == GUARD_STOP_BATTERY);
}

static void spike_and_priority(void)
{
  guard_limits l;
  guard_limits_default(&l);
  guard_policy p = { 0 };
  br_thermal_raw hot = raw(1100, 950, 600, 500);
  br_thermal_raw cool = raw(600, 600, 350, 300);
  CHECK(guard_policy_step(&p, &l, &hot) == GUARD_CONTINUE);
  CHECK(guard_policy_step(&p, &l, &cool) == GUARD_CONTINUE);
  CHECK(p.soc_hot == 0 && p.gpu_hot == 0 && p.board_hot == 0 && p.battery_hot == 0);
  guard_policy_step(&p, &l, &hot);
  guard_policy_step(&p, &l, &hot);
  CHECK(guard_policy_step(&p, &l, &hot) == GUARD_STOP_BATTERY); // protect the cell first
}

static void missing_sensors(void)
{
  guard_limits l;
  guard_limits_default(&l);
  l.sensor_loss_samples = 3;
  guard_policy p = { 0 };
  br_thermal_raw missing = raw(GUARD_UNKNOWN, GUARD_UNKNOWN, GUARD_UNKNOWN, GUARD_UNKNOWN);
  CHECK(guard_policy_step(&p, &l, &missing) == GUARD_CONTINUE);
  CHECK(guard_policy_step(&p, &l, &missing) == GUARD_CONTINUE);
  CHECK(guard_policy_step(&p, &l, &missing) == GUARD_STOP_SENSOR_LOSS);
  br_thermal_raw partial = raw(GUARD_UNKNOWN, 500, GUARD_UNKNOWN, GUARD_UNKNOWN);
  memset(&p, 0, sizeof p);
  CHECK(guard_policy_step(&p, &l, &partial) == GUARD_CONTINUE);
  CHECK(guard_policy_step(&p, &l, &partial) == GUARD_CONTINUE);
  CHECK(guard_policy_step(&p, &l, &partial) == GUARD_STOP_SENSOR_LOSS);
  CHECK(p.soc_missing == 3 && p.gpu_missing == 0 && p.board_missing == 3 && p.battery_missing == 3);
}

int main(void)
{
  defaults_and_parse();
  sustained_heat();
  spike_and_priority();
  missing_sensors();
  CHECK(!strcmp(guard_decision_name(GUARD_STOP_SOC), "thermal-soc"));
  CHECK(!strcmp(guard_decision_name(GUARD_STOP_SENSOR_LOSS), "sensor-loss"));
  CHECK(!strcmp(guard_decision_name(GUARD_CONTINUE), "none"));
  printf("guard: %d checks, %d failed\n", checks, failed);
  return failed ? 1 : 0;
}
