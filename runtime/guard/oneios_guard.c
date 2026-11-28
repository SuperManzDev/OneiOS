// SPDX-License-Identifier: MIT
// Independent phone-side safety guard for native OneiOS takeover sessions.
#define _GNU_SOURCE
#include "guard.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static volatile sig_atomic_t ending;

static void on_signal(int sig) { (void)sig; ending = 1; }

static uint64_t monotonic_ms(void)
{
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts)) return 0;
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static int path_join(char *out, size_t size, const char *a, const char *b)
{
  int n = snprintf(out, size, "%s/%s", a, b);
  return n > 0 && (size_t)n < size ? 0 : -1;
}

static int write_atomic(const char *dir, const char *name, const char *text)
{
  char path[PATH_MAX], tmp[PATH_MAX];
  if (path_join(path, sizeof path, dir, name) || snprintf(tmp, sizeof tmp, "%s.tmp.%ld", path, (long)getpid()) >= (int)sizeof tmp) return -1;
  int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) return -1;
  size_t len = strlen(text), off = 0;
  while (off < len) {
    ssize_t n = write(fd, text + off, len - off);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) { close(fd); unlink(tmp); return -1; }
    off += (size_t)n;
  }
  int rc = fsync(fd);
  if (close(fd)) rc = -1;
  if (!rc && rename(tmp, path)) rc = -1;
  if (rc) unlink(tmp);
  return rc ? -1 : 0;
}

static bool bit(const unsigned long *bits, unsigned n)
{
  return !!(bits[n / (8u * sizeof *bits)] & (1ul << (n % (8u * sizeof *bits))));
}

static int open_volume_down(char *chosen, size_t chosen_size)
{
  int fallback = -1;
  char fallback_path[64] = "";
  for (unsigned i = 0; i < 128; i++) {
    char path[64], name[128] = "";
    snprintf(path, sizeof path, "/dev/input/event%u", i);
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) continue;
    unsigned long props[(INPUT_PROP_MAX + 8u * sizeof(unsigned long)) / (8u * sizeof(unsigned long))] = { 0 };
    unsigned long keys[(KEY_MAX + 8u * sizeof(unsigned long)) / (8u * sizeof(unsigned long))] = { 0 };
    ioctl(fd, EVIOCGNAME(sizeof name), name);
    if (ioctl(fd, EVIOCGPROP(sizeof props), props) < 0 || ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys) < 0 ||
        bit(props, INPUT_PROP_DIRECT) || !bit(keys, KEY_VOLUMEDOWN)) {
      close(fd);
      continue;
    }
    if (strstr(name, "pmic_resin")) {
      if (fallback >= 0) close(fallback);
      snprintf(chosen, chosen_size, "%s", path);
      return fd;
    }
    if (fallback < 0) {
      fallback = fd;
      snprintf(fallback_path, sizeof fallback_path, "%s", path);
    } else close(fd);
  }
  if (fallback >= 0) snprintf(chosen, chosen_size, "%s", fallback_path);
  return fallback;
}

static int read_text(const char *path, char *out, size_t size)
{
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return -1;
  ssize_t n;
  do n = read(fd, out, size - 1); while (n < 0 && errno == EINTR);
  close(fd);
  if (n <= 0) return -1;
  out[n] = 0;
  return 0;
}

static int proc_start(pid_t pid, unsigned long long *start)
{
  char path[64], text[4096];
  snprintf(path, sizeof path, "/proc/%ld/stat", (long)pid);
  if (read_text(path, text, sizeof text)) return -1;
  char *p = strrchr(text, ')');
  if (!p || p[1] != ' ') return -1;
  p += 2; // field 3
  for (unsigned field = 3; field <= 22; field++) {
    char *end = strchr(p, field == 22 ? '\n' : ' ');
    if (field == 22) {
      errno = 0;
      char *number_end = NULL;
      unsigned long long value = strtoull(p, &number_end, 10);
      if (errno || number_end == p || (*number_end && *number_end != ' ' && *number_end != '\n')) return -1;
      *start = value;
      return 0;
    }
    if (!end) return -1;
    p = end + 1;
  }
  return -1;
}

static bool process_matches(pid_t pid, unsigned long long expected, const char *session_dir)
{
  unsigned long long actual = 0;
  if (pid <= 1 || proc_start(pid, &actual) || actual != expected) return false;
  char path[64], cmd[4096];
  snprintf(path, sizeof path, "/proc/%ld/cmdline", (long)pid);
  if (read_text(path, cmd, sizeof cmd)) return false;
  for (char *p = cmd; p < cmd + sizeof cmd && *p; p += strlen(p) + 1) {
    if (strstr(p, "crosvm")) {
      // The staged kernel path or socket ties this crosvm to this session.
      for (char *q = cmd; q < cmd + sizeof cmd && *q; q += strlen(q) + 1)
        if (strstr(q, session_dir)) return true;
    }
  }
  return false;
}

static int read_identity(const char *guard_dir, pid_t *pid, unsigned long long *start)
{
  char path[PATH_MAX], text[128], extra;
  if (path_join(path, sizeof path, guard_dir, "vm.identity") || read_text(path, text, sizeof text)) return -1;
  long value = 0;
  if (sscanf(text, "%ld %llu %c", &value, start, &extra) != 2 || value <= 1 || value > INT32_MAX) return -1;
  *pid = (pid_t)value;
  return 0;
}

static int wait_process(pid_t pid, unsigned long long start, const char *session_dir, unsigned seconds)
{
  uint64_t until = monotonic_ms() + (uint64_t)seconds * 1000u;
  while (monotonic_ms() < until) {
    if (!process_matches(pid, start, session_dir)) return 0;
    usleep(100000);
  }
  return process_matches(pid, start, session_dir) ? -1 : 0;
}

static int run_wait(char *const argv[], unsigned seconds)
{
  pid_t child = fork();
  if (child < 0) return -1;
  if (child == 0) {
    execv(argv[0], argv);
    _exit(127);
  }
  uint64_t until = monotonic_ms() + (uint64_t)seconds * 1000u;
  int status = 0;
  while (monotonic_ms() < until) {
    pid_t rc = waitpid(child, &status, WNOHANG);
    if (rc == child) return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (rc < 0 && errno != EINTR) return -1;
    usleep(100000);
  }
  kill(child, SIGKILL);
  waitpid(child, &status, 0);
  return -1;
}

static void stop_vm(const char *session_dir, const char *guard_dir, const char *crosvm)
{
  char stop[PATH_MAX], socket[PATH_MAX];
  if (!path_join(stop, sizeof stop, session_dir, "stop.request")) {
    int fd = open(stop, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
    if (fd >= 0) close(fd);
  }
  pid_t pid = -1;
  unsigned long long start = 0;
  if (read_identity(guard_dir, &pid, &start) || !process_matches(pid, start, session_dir)) return;
  if (!path_join(socket, sizeof socket, session_dir, "crosvm.sock")) {
    char *const argv[] = { (char *)crosvm, "stop", socket, NULL };
    run_wait(argv, 5);
  }
  if (!wait_process(pid, start, session_dir, 10)) return;
  if (process_matches(pid, start, session_dir)) kill(pid, SIGTERM);
  if (!wait_process(pid, start, session_dir, 5)) return;
  if (process_matches(pid, start, session_dir)) kill(pid, SIGKILL);
  wait_process(pid, start, session_dir, 5);
}

static bool marker(const char *dir, const char *name)
{
  char path[PATH_MAX];
  return !path_join(path, sizeof path, dir, name) && !access(path, F_OK);
}

static int recover_ui(const char *session_dir, const char *reason, bool cleanup_done)
{
  char ui[PATH_MAX], why[96];
  if (path_join(ui, sizeof ui, session_dir, "ui.sh")) return -1;
  if (snprintf(why, sizeof why, "guard:%s", reason) >= (int)sizeof why) return -1;
  char *const argv[] = { "/system/bin/sh", ui, cleanup_done ? "--recover" : "--guard-failed", why, NULL };
  return run_wait(argv, cleanup_done ? 300 : 30);
}

static void usage(const char *argv0)
{
  fprintf(stderr, "usage: %s --dir SESSION --crosvm FILE [--soc C] [--gpu C] [--board C] [--battery C] [--samples N]\n", argv0);
}

int main(int argc, char **argv)
{
  const char *session_dir = NULL, *crosvm = NULL;
  guard_limits limits;
  guard_limits_default(&limits);
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--dir") && i + 1 < argc) session_dir = argv[++i];
    else if (!strcmp(argv[i], "--crosvm") && i + 1 < argc) crosvm = argv[++i];
    else if (!strncmp(argv[i], "--", 2) && i + 1 < argc && !guard_limits_parse(&limits, argv[i] + 2, argv[i + 1])) i++;
    else { usage(argv[0]); return 2; }
  }
  if (!session_dir || session_dir[0] != '/' || !crosvm || crosvm[0] != '/') { usage(argv[0]); return 2; }

  char guard_dir[PATH_MAX], active[PATH_MAX], key_path[64];
  if (path_join(guard_dir, sizeof guard_dir, session_dir, "guard") ||
      (mkdir(guard_dir, 0700) && errno != EEXIST) || path_join(active, sizeof active, guard_dir, "active")) return 3;
  int lock = open(active, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (lock < 0) { fprintf(stderr, "oneios-guard: another guard is active\n"); return 3; }
  dprintf(lock, "%ld\n", (long)getpid());
  close(lock);

  signal(SIGTERM, on_signal);
  signal(SIGINT, on_signal);
  signal(SIGHUP, on_signal);

  int key_fd = open_volume_down(key_path, sizeof key_path);
  br_thermal_sensors sensors;
  if (key_fd < 0 || br_thermal_find(NULL, &sensors)) {
    fprintf(stderr, "oneios-guard: refused to arm: %s%s\n", key_fd < 0 ? "no safe Volume Down device" : "", key_fd < 0 ? "" : "no thermal sensors");
    if (key_fd >= 0) close(key_fd);
    unlink(active);
    return 4;
  }

  // Arming is an approval to stop One UI, not merely proof that files exist. Require every advertised safety channel
  // and enough cool samples to reject an already-hot phone before takeover or huge-page acquisition begins.
  guard_policy preflight = { 0 };
  for (unsigned i = 0; i < limits.samples; i++) {
    br_thermal_raw raw;
    br_thermal_read(&sensors, &raw);
    int32_t board = raw.skin != GUARD_UNKNOWN ? raw.skin : raw.ap;
    if (raw.soc == GUARD_UNKNOWN || raw.gpu == GUARD_UNKNOWN || board == GUARD_UNKNOWN || raw.battery == GUARD_UNKNOWN ||
        guard_policy_step(&preflight, &limits, &raw) != GUARD_CONTINUE) {
      fprintf(stderr, "oneios-guard: refused to arm: hot, missing or unstable thermal channel\n");
      close(key_fd); unlink(active); return 4;
    }
    if (i + 1 < limits.samples) sleep(1);
  }

  char armed[256];
  snprintf(armed, sizeof armed, "pid=%ld key=%s soc=%d gpu=%d board=%d battery=%d samples=%u\n", (long)getpid(), key_path,
           limits.soc_stop, limits.gpu_stop, limits.board_stop, limits.battery_stop, limits.samples);
  if (write_atomic(guard_dir, "armed", armed)) { close(key_fd); unlink(active); return 4; }
  fprintf(stderr, "oneios-guard: armed; Volume Down 10 s; stop tenths C soc=%d gpu=%d board=%d battery=%d x%u\n",
          limits.soc_stop, limits.gpu_stop, limits.board_stop, limits.battery_stop, limits.samples);

  guard_policy policy = { 0 };
  uint64_t next_thermal = 0, down_since = 0;
  const char *reason = NULL;
  bool session_started = false, normal_done = false;
  unsigned long key_state[(KEY_MAX + 8u * sizeof(unsigned long)) / (8u * sizeof(unsigned long))] = { 0 };
  if (ioctl(key_fd, EVIOCGKEY(sizeof key_state), key_state) >= 0 && bit(key_state, KEY_VOLUMEDOWN)) down_since = monotonic_ms();
  while (!ending) {
    if (marker(guard_dir, "session.done")) { normal_done = true; break; }
    session_started = marker(guard_dir, "session.started");
    struct pollfd pfd = { .fd = key_fd, .events = POLLIN };
    int rc = poll(&pfd, 1, 250);
    if (rc > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
      if (session_started) { reason = "input-loss"; break; }
      fprintf(stderr, "oneios-guard: input device failed before takeover\n");
      break;
    }
    if (rc > 0 && (pfd.revents & POLLIN)) {
      struct input_event events[16];
      ssize_t n = read(key_fd, events, sizeof events);
      if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
        if (session_started) { reason = "input-loss"; break; }
        ending = 1;
      }
      if (n > 0) for (size_t i = 0; i < (size_t)n / sizeof events[0]; i++) {
        if (events[i].type == EV_KEY && events[i].code == KEY_VOLUMEDOWN) {
          if (events[i].value == 1 && !down_since) down_since = monotonic_ms();
          else if (events[i].value == 0) down_since = 0;
        } else if (events[i].type == EV_SYN && events[i].code == SYN_DROPPED) down_since = 0;
      }
    }
    uint64_t now = monotonic_ms();
    if (session_started && down_since && now - down_since >= 10000) { reason = "key"; break; }
    if (now >= next_thermal) {
      br_thermal_raw raw;
      br_thermal_read(&sensors, &raw);
      guard_decision decision = guard_policy_step(&policy, &limits, &raw);
      fprintf(stderr, "oneios-guard: thermal soc=%d gpu=%d skin=%d ap=%d battery=%d\n", raw.soc, raw.gpu, raw.skin, raw.ap, raw.battery);
      if (session_started && decision != GUARD_CONTINUE) { reason = guard_decision_name(decision); break; }
      next_thermal = now + 1000;
    }
  }

  if (!reason && ending && session_started && !normal_done) reason = "guard-signal";
  if (reason) {
    write_atomic(guard_dir, "reason", reason);
    fprintf(stderr, "oneios-guard: STOP %s\n", reason);
    stop_vm(session_dir, guard_dir, crosvm);
    uint64_t cleanup_until = monotonic_ms() + 30000;
    while (!marker(guard_dir, "cleanup.done") && monotonic_ms() < cleanup_until) usleep(100000);
    bool cleanup_done = marker(guard_dir, "cleanup.done");
    int recovered = recover_ui(session_dir, reason, cleanup_done);
    write_atomic(guard_dir, "outcome", recovered == 0 && cleanup_done ? "recovered\n" : "recovery-failed\n");
  } else write_atomic(guard_dir, "outcome", normal_done ? "normal-end\n" : "guard-ended-before-takeover\n");

  close(key_fd);
  unlink(active);
  return reason ? 10 : 0;
}
